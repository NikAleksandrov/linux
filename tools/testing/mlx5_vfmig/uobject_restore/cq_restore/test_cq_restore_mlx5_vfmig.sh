#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S5b end-to-end: validate UVERBS_METHOD_RESTORE_CQ + mlx5_ib_restore_cq
# against a real save/load cycle.
#
# This is the driver-end-to-end follow-on to test_cq_adopt.sh, mirroring
# what test_mr_restore_mlx5_vfmig.sh does for §S4b:
#
#   test_cq_adopt.sh    Drives MLX5_VFMIG_IOC_PROBE_CQN against the PF
#                       cdev to confirm the source's cqn is alive on
#                       the destination after LOAD_VHCA_STATE and
#                       carries a cqc context (eqn, log_cq_size,
#                       log_page_size, page_offset, status, oi)
#                       byte-equal to the source's pre-SAVE view.
#                       Validates Model A's *underlying firmware
#                       behaviour*. Does NOT exercise the new kernel
#                       verb path.
#
#   this script         Validates the actual landed verb path:
#                       opens a destination ucontext with
#                       MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE, issues
#                       UVERBS_METHOD_RESTORE_CQ with a UHW payload
#                       carrying the source (cqn, cqe_size, buf_addr,
#                       db_addr), and confirms via INFO_HANDLES +
#                       RESP_CQE echo that the adopted CQ landed at
#                       the caller-chosen ufile handle with the wire-
#                       visible identity preserved. Folds in the FW-
#                       level PROBE_CQN check from test_cq_adopt.sh
#                       while the adopted CQ is alive on the destination
#                       ucontext, so we get a single integrated test
#                       covering both the ABI + FW dimensions for §S5b.
#
# Why the "PROBE_CQN while alive" check matters here
# --------------------------------------------------
# test_cq_adopt.sh (B0) runs PROBE_CQN against a destination VF that is
# bound to mlx5_core but has NO ucontext or kernel-side mlx5_ib_cq for
# the adopted cqn. The check there is purely "FW-side liveness across
# LOAD_VHCA_STATE" -- the kernel ib_device hasn't been told the cqn
# exists. Folding the same PROBE_CQN call into Phase G of this harness
# upgrades it to "FW-side liveness with the kernel ib_device actively
# wrapping that cqn in a kernel-side mlx5_ib_cq via mlx5_core_adopt_cq"
# -- i.e. the union of every state machine that has to agree about a
# restored CQ. If Phase G's snapshot still byte-matches the source's
# pre-SAVE snapshot, mlx5_core_adopt_cq's EQ-tree register has not
# corrupted any cqc bits FW reads back through QUERY_CQ.
#
# Flow
# ----
#
#   Phase A  Provision source VF, set_tracked + enable_migratable, bind.
#   Phase B  Fork fw_id_continuity_probe on the source ibdev to allocate
#            one of each {PD, CQ, QP, MR, SRQ}. Capture src_cqn,
#            src_cqe, src_cqe_size, src_cq_buf_addr, src_cq_db_addr
#            from its READY-line dictionary. Also run PROBE_CQN on the
#            source VF to capture the cqc shape parameters (eqn,
#            log_cq_size, log_page_size, page_offset, status, oi) the
#            cqc.byte-equality assertion needs.
#   Phase C  SAVE_VHCA_STATE on the source.
#   Phase D  Quit the source probe; tear down the source VF.
#   Phase E  Provision a fresh destination VF: set_tracked,
#            enable_migratable, LOAD_VHCA_STATE, mark_restored, bind.
#   Phase F  ===== THE TEST =====
#            Fork cq_restore_probe_mlx5_vfmig in the background against
#            the destination ibdev with the captured src_*. Wait for
#            "READY" -- at that point subtests 1-9 (gate, UAPI rejects
#            x4, bad comp_vector, COMP_CHANNEL rejection, happy path,
#            collision) have all run and the adopted CQ is alive at
#            CQ_TARGET_HANDLE on the probe's ucontext.
#   Phase G  While the CQ is alive, run MLX5_VFMIG_IOC_PROBE_CQN against
#            the adopted cqn and byte-compare the cqc snapshot to the
#            source's pre-SAVE snapshot from Phase B. fw_accept=1 +
#            content match means the FW side of Model A is sound under
#            the live verb path.
#   Phase H  Tell the probe to "quit", which triggers subtest 10 (v0
#            dealloc semantics: DESTROY_CQ on the adopted cqn must
#            -EINVAL because LOAD_VHCA_STATE also carried over the
#            source's cqn-referencing QPCs/SRQCs and v0 has not yet
#            restored kernel uobjects for those). Wait for the probe
#            to exit 0.
#   Phase I  Verdict + manifest.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_cq_restore_mlx5_vfmig.sh
#
# Optional knobs:
#   BLOB                    Path for the SAVE blob.
#   TOOL                    mlx5_vfmig CLI.
#   SRC_PROBE               fw_id_continuity_probe binary.
#   DST_PROBE               cq_restore_probe_mlx5_vfmig binary.
#   CQ_TARGET_HANDLE        ufile handle for the adopted CQ (default 0x4244).
#   COMP_VECTOR             comp_vector to pass to RESTORE_CQ (default 0).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
SRC_PROBE=${SRC_PROBE:-$ROOT_DIR/uobject_restore/fw_id_continuity/fw_id_continuity_probe}
DST_PROBE=${DST_PROBE:-$ROOT_DIR/uobject_restore/cq_restore/cq_restore_probe_mlx5_vfmig}
BLOB=${BLOB:-/tmp/vf_cq_restore.blob}
META=${META:-${BLOB}.meta}
CQ_TARGET_HANDLE=${CQ_TARGET_HANDLE:-0x4244}
COMP_VECTOR=${COMP_VECTOR:-0}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]       || { echo "build $TOOL first";       exit 1; }
[ -x "$SRC_PROBE" ]  || { echo "build $SRC_PROBE first";  exit 1; }
[ -x "$DST_PROBE" ]  || { echo "build $DST_PROBE first";  exit 1; }
[ -e "$CDEV" ]       || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

WORKDIR=$(mktemp -d -t cqrestore.XXXXXX)
cleanup() {
    if [ -n "${SRC_PROBE_PID:-}" ] && kill -0 "$SRC_PROBE_PID" 2>/dev/null; then
        echo "quit" > "$WORKDIR/src.in" 2>/dev/null || true
        wait "$SRC_PROBE_PID" 2>/dev/null || true
    fi
    if [ -n "${DST_PROBE_PID:-}" ] && kill -0 "$DST_PROBE_PID" 2>/dev/null; then
        echo "quit" > "$WORKDIR/dst.in" 2>/dev/null || true
        wait "$DST_PROBE_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

# --- helpers (copy-paste from test_mr_restore_mlx5_vfmig.sh) ---

vf_path()       { echo "/sys/bus/pci/devices/$1"; }
wait_for_path() { for i in $(seq 1 50); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }

bind_vf_safe() {
    local vf=$1
    local timeout=${2:-60}
    local label=${3:-bind}
    local started bg
    started=$(date +%s)

    ( echo "$vf" | sudo tee /sys/bus/pci/drivers/mlx5_core/bind ) \
        >/dev/null 2>&1 &
    bg=$!

    while :; do
        if [ -e "$(vf_path $vf)/driver" ]; then
            wait "$bg" 2>/dev/null || true
            return 0
        fi
        if ! kill -0 "$bg" 2>/dev/null; then
            sleep 0.2
            if [ -e "$(vf_path $vf)/driver" ]; then
                return 0
            fi
            return 1
        fi
        if [ $(( $(date +%s) - started )) -ge "$timeout" ]; then
            echo "ERROR: $label has been pending for ${timeout}s; probe wedged." >&2
            return 2
        fi
        sleep 0.5
    done
}

find_ib_dev_for_pci() {
    local bdf=$1
    local d pci
    for d in /sys/class/infiniband/*; do
        [ -e "$d/device" ] || continue
        pci=$(basename "$(readlink "$d/device")")
        if [ "$pci" = "$bdf" ]; then
            basename "$d"
            return 0
        fi
    done
    return 1
}

start_src_probe() {
    local ibdev=$1
    local fifo_in="$WORKDIR/src.in"
    local fifo_out="$WORKDIR/src.out"

    mkfifo "$fifo_in" "$fifo_out"
    echo "=== source probe: $SRC_PROBE $ibdev ==="
    sudo "$SRC_PROBE" "$ibdev" < "$fifo_in" > "$fifo_out" 2>&1 &
    SRC_PROBE_PID=$!
    exec 7> "$fifo_in"

    local line
    while IFS= read -r line < "$fifo_out"; do
        echo "  [src probe] $line"
        case "$line" in
            READY) return 0 ;;
            *=*)
                if [[ "$line" =~ ^[a-zA-Z_][a-zA-Z0-9_]*= ]]; then
                    local k="${line%%=*}"
                    local v="${line#*=}"
                    eval "src_${k}=\"\$v\""
                fi
                ;;
        esac
    done
    echo "  [src probe] EOF before READY -- probe failed"
    return 1
}

quit_src_probe() {
    echo "quit" >&7; exec 7>&-
    wait "$SRC_PROBE_PID" 2>/dev/null || true
    SRC_PROBE_PID=""
    echo "  [src probe] exited"
}

# Fork the dst CQ probe in the background.
start_dst_probe() {
    local ibdev=$1 cqn=$2 cqe=$3 cqe_size=$4 buf_addr=$5 db_addr=$6
    local comp_vector=$7 cq_target=$8
    local fifo_in="$WORKDIR/dst.in"
    local fifo_out="$WORKDIR/dst.out"

    mkfifo "$fifo_in" "$fifo_out"
    echo "=== dst probe: $DST_PROBE $ibdev $cqn $cqe $cqe_size $buf_addr $db_addr $comp_vector $cq_target ==="
    sudo "$DST_PROBE" "$ibdev" "$cqn" "$cqe" "$cqe_size" \
        "$buf_addr" "$db_addr" "$comp_vector" "$cq_target" \
        < "$fifo_in" > "$fifo_out" 2>&1 &
    DST_PROBE_PID=$!
    exec 8> "$fifo_in"

    local line k v
    while IFS= read -r line < "$fifo_out"; do
        echo "  [dst probe] $line"
        case "$line" in
            READY) return 0 ;;
            "  FAIL"*|" FAIL"*|"FAIL"*)
                dst_pre_ready_fail=1
                ;;
            *)
                if [[ "$line" =~ ^[a-zA-Z_][a-zA-Z0-9_]*= ]]; then
                    k="${line%%=*}"
                    v="${line#*=}"
                    eval "dst_${k}=\"\$v\""
                fi
                ;;
        esac
    done
    echo "  [dst probe] EOF before READY -- probe failed (subtests 1-9)"
    return 1
}

quit_dst_probe() {
    echo "quit" >&8; exec 8>&-
    while IFS= read -r line < "$WORKDIR/dst.out"; do
        echo "  [dst probe] $line"
        case "$line" in
            "  FAIL"*) dst_post_quit_fail=1 ;;
            "cq_restore_probe_mlx5_vfmig: PASS"*) dst_overall_pass=1 ;;
        esac
    done
    if wait "$DST_PROBE_PID"; then
        DST_PROBE_RC=0
    else
        DST_PROBE_RC=$?
    fi
    DST_PROBE_PID=""
    echo "  [dst probe] exited rc=$DST_PROBE_RC"
}

probe_cqn_into() {
    local label=$1 vf=$2 cqn=$3 prefix=$4
    local out line k v
    echo "=== PROBE_CQN ($label): vf=$vf cqn=$cqn ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_cqn "$vf" "$cqn" 2>&1); then
        echo "$out" | sed 's/^/  /'
        echo "  PROBE_CQN ioctl failed (transport / arg error)"
        return 1
    fi
    while IFS= read -r line; do
        echo "  [$label probe_cqn] $line"
        case "$line" in
            *=*) k="${line%%=*}"; v="${line#*=}";
                 eval "${prefix}_${k}=\"\$v\"" ;;
        esac
    done <<<"$out"
    return 0
}

# --- Phase A ---------------------------------------------------------

echo "=== Phase A: provision SOURCE VF ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null

wait_for_path "$(vf_path $PF)/virtfn0"
SRC_VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "source VF: $SRC_VF"

sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0

echo mlx5_core | sudo tee "$(vf_path $SRC_VF)/driver_override" >/dev/null
bind_vf_safe "$SRC_VF" 60 "Phase A: source VF bind"
sleep 1
SRC_IBDEV=$(find_ib_dev_for_pci "$SRC_VF") || { echo "FAIL: no ibdev for source $SRC_VF"; exit 1; }
echo "source ibdev: $SRC_IBDEV"

# --- Phase B ---------------------------------------------------------

echo "=== Phase B: source probe (allocates {PD,CQ,QP,MR,SRQ}, holds them across SAVE) ==="
start_src_probe "$SRC_IBDEV"
echo "captured: src_cqn=$src_cqn src_cqe=$src_cqe src_cqe_size=$src_cqe_size"
echo "          src_cq_buf_addr=$src_cq_buf_addr src_cq_db_addr=$src_cq_db_addr"

# PROBE_CQN the source VF pre-SAVE to capture the cqc shape parameters
# the byte-equality assertion below needs (eqn, log_cq_size,
# log_page_size, page_offset, status, oi). Same trick test_cq_adopt.sh
# uses; see its docstring for the why. Stored under the `src_pos_*`
# prefix to avoid clobbering the manifest-style src_* names from the
# source probe.
probe_cqn_into source 0 "$src_cqn" src_pos
if [ "${src_pos_fw_accept:-0}" != 1 ]; then
    echo "FAIL: source-side PROBE_CQN(vf=0, cqn=$src_cqn) rejected fw_syndrome=${src_pos_fw_syndrome:-?}"
    exit 1
fi

# --- Phase C ---------------------------------------------------------

echo "=== Phase C: SAVE_VHCA_STATE ==="
sudo dmesg -C
# snapshot-ordering: pause datapath (CRIU CHECKPOINT_DEVICES), then
# capture (SAVE is suspend-aware and skips its own suspend), then resume.
sudo "$TOOL" "$PF" suspend_vhca 0
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" resume_vhca 0
sudo chmod 0644 "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
[ "$SAVE_BYTES" -gt 16 ] || { echo "FAIL: blob suspiciously small: $SAVE_BYTES"; exit 1; }

# --- Phase D ---------------------------------------------------------

echo "=== Phase D: quit source probe + tear down source VF ==="
quit_src_probe
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null

# --- Phase E ---------------------------------------------------------

echo "=== Phase E: fresh DEST VF + LOAD + bind ==="
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
wait_for_path "$(vf_path $PF)/virtfn0"
DST_VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "dest VF: $DST_VF"

sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0

sudo dmesg -C
sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" mark_restored 0

echo mlx5_core | sudo tee "$(vf_path $DST_VF)/driver_override" >/dev/null
bind_vf_safe "$DST_VF" 60 "Phase E: dest VF bind"sleep 1
DST_IBDEV=$(find_ib_dev_for_pci "$DST_VF") || { echo "FAIL: no ibdev for dest $DST_VF"; exit 1; }
echo "dest ibdev: $DST_IBDEV"

# --- Phase F: THE TEST ----------------------------------------------

echo
echo "================ Phase F: RESTORE_CQ ================="
echo "Drive UVERBS_METHOD_RESTORE_CQ on $DST_IBDEV with the src cqn."
echo "The probe runs subtests 1-9 (gate, UAPI rejects x4, bad"
echo "comp_vector, COMP_CHANNEL rejection, happy path, collision)"
echo "and then parks at READY while the adopted CQ is alive at"
echo "handle $CQ_TARGET_HANDLE."
if ! start_dst_probe "$DST_IBDEV" "$src_cqn" "$src_cqe" "$src_cqe_size" \
                     "$src_cq_buf_addr" "$src_cq_db_addr" \
                     "$COMP_VECTOR" "$CQ_TARGET_HANDLE"; then
    echo "FAIL: dst probe did not reach READY"
    exit 1
fi

# --- Phase G: cross-check FW liveness of the adopted cqn ------------

echo
echo "================ Phase G: FW liveness check =========="
echo "While the adopted CQ is alive, run MLX5_VFMIG_IOC_PROBE_CQN against"
echo "the same cqn and confirm fw_eqn / fw_log_cq_size / fw_log_page_size"
echo "/ fw_page_offset / fw_oi still match the source's pre-SAVE snapshot."
echo "This is the *live-verb-path* upgrade of test_cq_adopt.sh's check"
echo "-- if mlx5_core_adopt_cq's EQ-tree register or any of the kernel-"
echo "side wiring corrupted cqc bits FW reads back via QUERY_CQ, the"
echo "byte-comparison below would catch it."
probe_cqn_into adopted 0 "$src_cqn" pos

# --- Phase H: destroy round-trip ------------------------------------

echo
echo "================ Phase H: v0 dealloc semantics ======="
echo "Tell the probe to quit; it will run subtest 10."
echo
echo "Symmetric with PD's subtest 7 (asymmetric with MR's subtest 8)."
echo "In the FW resource graph, CQ is a *parent*: QPCs/SRQCs reference"
echo "cqn as a tracked FW resource dep. So DESTROY_CQ on the orphan"
echo "adopted cqn FAILS with FW BAD_RES_STATE (-> -EINVAL via"
echo "cmd_status_to_err) because LOAD_VHCA_STATE also carried over the"
echo "source's cqn-referencing QPs/SRQs. v0 has not restored kernel"
echo "uobjects for those, so the kernel cannot dealloc them first; FW"
echo "correctly rejects an orphan DESTROY_CQ. INFO_HANDLES must still"
echo "report cq_target_handle (uobj parked in the ufile so a later"
echo "cascading teardown can retry once S6+S7 land)."
echo
echo "v0 implication for CRIU: CQ teardown ordering is FW-enforced --"
echo "userspace will see -EINVAL until the dependent QP/SRQ uobjects"
echo "have been restored and torn down. Documented in"
echo "design/uobject_restore.md S5b + S10.8."
quit_dst_probe

# --- verdict --------------------------------------------------------

overall_rc=0
fw_ok=0
content_ok=0

if [ "${pos_fw_accept:-0}" = 1 ]; then
    fw_ok=1
fi

# Compare PROBE_CQN's cqc snapshot against what the source captured
# pre-SAVE. Same byte-equality check as test_cq_adopt.sh; see its
# verdict-matrix block for the rationale. Six fields per the §S5b
# locked-in assertion: eqn, log_cq_size, log_page_size, page_offset,
# status, oi.
if [ "$fw_ok" = 1 ]; then
    if [ "${pos_fw_eqn:-}" = "${src_pos_fw_eqn:-}" ] \
       && [ "${pos_fw_log_cq_size:-}" = "${src_pos_fw_log_cq_size:-}" ] \
       && [ "${pos_fw_log_page_size:-}" = "${src_pos_fw_log_page_size:-}" ] \
       && [ "${pos_fw_page_offset:-}" = "${src_pos_fw_page_offset:-}" ] \
       && [ "${pos_fw_status:-}" = "${src_pos_fw_status:-}" ] \
       && [ "${pos_fw_oi:-}" = "${src_pos_fw_oi:-}" ]; then
        content_ok=1
    fi
fi

echo
echo "================ CQ RESTORE VERDICT ================="
echo "  src_cqn               = $src_cqn"
echo "  src_cqe               = $src_cqe"
echo "  src_cqe_size          = $src_cqe_size"
echo "  src_cq_buf_addr       = $src_cq_buf_addr"
echo "  src_cq_db_addr        = $src_cq_db_addr"
echo "  cq_target_handle      = $CQ_TARGET_HANDLE"
echo "  comp_vector           = $COMP_VECTOR"
echo
echo "  dst probe subtests    = ${DST_PROBE_RC:-?}  (0 means all PASS)"
if [ "${dst_overall_pass:-0}" = 1 ]; then
    echo "  dst probe overall     = PASS"
else
    echo "  dst probe overall     = FAIL/UNKNOWN"
fi
echo "  dst adopted_cqn       = ${dst_adopted_cqn:-?}"
echo "  dst adopted_cqe       = ${dst_adopted_cqe:-?}"
echo "  FW liveness fw_accept = ${pos_fw_accept:-?}  syndrome=${pos_fw_syndrome:-?}"
echo "  FW cqc.eqn            = ${pos_fw_eqn:-?}    expected=${src_pos_fw_eqn:-?}"
echo "  FW cqc.log_cq_size    = ${pos_fw_log_cq_size:-?}    expected=${src_pos_fw_log_cq_size:-?}"
echo "  FW cqc.log_page_size  = ${pos_fw_log_page_size:-?}    expected=${src_pos_fw_log_page_size:-?}"
echo "  FW cqc.page_offset    = ${pos_fw_page_offset:-?}    expected=${src_pos_fw_page_offset:-?}"
echo "  FW cqc.status         = ${pos_fw_status:-?}    expected=${src_pos_fw_status:-?}"
echo "  FW cqc.oi             = ${pos_fw_oi:-?}    expected=${src_pos_fw_oi:-?}"
echo

if [ "${DST_PROBE_RC:-1}" = 0 ] \
   && [ "${dst_overall_pass:-0}" = 1 ] \
   && [ "$fw_ok" = 1 ] \
   && [ "$content_ok" = 1 ]; then
    echo "  PASS  RESTORE_CQ landed on $DST_IBDEV @ handle $CQ_TARGET_HANDLE,"
    echo "        adopted cqn $src_cqn, FW still sees the cqc context"
    echo "        (eqn, log_cq_size, log_page_size, page_offset, status, oi)"
    echo "        byte-equal to source pre-SAVE under the live verb path,"
    echo "        and DESTROY_CQ correctly REJECTS the orphan cqn while"
    echo "        QPC/SRQC dependents remain (cqn is a parent in the FW"
    echo "        resource graph -- subtest 10's v0 expectation, symmetric"
    echo "        with PD's parent role and asymmetric with MR's leaf role)."
    echo "        CQ teardown ordering is FW-enforced at the v0 layer."
    echo "        mlx5_ib_restore_cq is functional for the v0 critical path."
elif [ "${DST_PROBE_RC:-1}" = 0 ] \
     && [ "${dst_overall_pass:-0}" = 1 ] \
     && [ "$fw_ok" = 1 ]; then
    echo "  WEAK PASS  Verb path landed and FW accepted PROBE_CQN, but the"
    echo "             cqc snapshot did not match the source pre-SAVE values."
    echo "             Possible causes:"
    echo "               - mlx5_core_adopt_cq's EQ-tree register clobbered"
    echo "                 a cqc field FW reads back via QUERY_CQ"
    echo "               - LOAD_VHCA_STATE didn't fully restore cqc"
    echo "                 (re-run test_cq_adopt.sh in isolation to"
    echo "                  separate the FW-only check from the verb-path"
    echo "                  check; if test_cq_adopt.sh also reports the"
    echo "                  mismatch the verb path is innocent)"
    echo "             Treat as a yellow flag and investigate before"
    echo "             claiming Model A v0-correct."
    overall_rc=1
else
    echo "  FAIL  see the per-subtest log above. Common causes:"
    echo "        - kernel not running B1+B2+B3 (rebuild + reboot needed)"
    echo "        - DEVX-aware ucontext on src (uid != 0): test_cq_adopt.sh"
    echo "          only validated the uid=0 case"
    echo "        - LOAD_VHCA_STATE didn't preserve the source cqn"
    echo "          (re-run test_cq_adopt.sh in isolation to confirm)"
    echo "        - vfmig placeholder lookup failed inside"
    echo "          mlx5_vfmig_bind_user_cq / mlx5_vfmig_bind_user_dbr"
    echo "          (check dmesg for placeholder-iova messages)"
    overall_rc=1
fi
echo "====================================================="

# --- manifest -------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# cq_restore_mlx5_vfmig manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_cqn=$src_cqn
src_cqe=$src_cqe
src_cqe_size=$src_cqe_size
src_cq_buf_addr=$src_cq_buf_addr
src_cq_db_addr=$src_cq_db_addr
src_pos_fw_eqn=${src_pos_fw_eqn:-?}
src_pos_fw_log_cq_size=${src_pos_fw_log_cq_size:-?}
src_pos_fw_log_page_size=${src_pos_fw_log_page_size:-?}
src_pos_fw_page_offset=${src_pos_fw_page_offset:-?}
src_pos_fw_status=${src_pos_fw_status:-?}
src_pos_fw_oi=${src_pos_fw_oi:-?}
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV
cq_target_handle=$CQ_TARGET_HANDLE
comp_vector=$COMP_VECTOR
dst_probe_rc=${DST_PROBE_RC:-?}
dst_overall_pass=${dst_overall_pass:-0}
dst_adopted_cqn=${dst_adopted_cqn:-?}
dst_adopted_cqe=${dst_adopted_cqe:-?}
fw_accept=${pos_fw_accept:-?}
fw_syndrome=${pos_fw_syndrome:-?}
fw_eqn=${pos_fw_eqn:-?}
fw_log_cq_size=${pos_fw_log_cq_size:-?}
fw_log_page_size=${pos_fw_log_page_size:-?}
fw_page_offset=${pos_fw_page_offset:-?}
fw_status=${pos_fw_status:-?}
fw_oi=${pos_fw_oi:-?}
fw_content_match=$content_ok
overall_rc=$overall_rc
EOF
sudo chmod 0644 "$META"
echo "wrote $META"

exit "$overall_rc"
