#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S4b end-to-end: validate UVERBS_METHOD_RESTORE_MR + mlx5_ib_restore_mr
# against a real save/load cycle.
#
# This is the driver-end-to-end follow-on to test_mr_adopt.sh, mirroring
# what test_pd_restore_mlx5_vfmig.sh does for §S3b:
#
#   test_mr_adopt.sh    Drives MLX5_VFMIG_IOC_PROBE_MKEY against the PF
#                       cdev to confirm the source's mkey_index is alive
#                       on the destination after LOAD_VHCA_STATE and
#                       carries an mkc context (pd, len, start_addr)
#                       byte-equal to the source's pre-SAVE view.
#                       Validates Model A's *underlying firmware
#                       behaviour*. Does NOT exercise the new kernel
#                       verb path.
#
#   this script         Validates the actual landed verb path:
#                       opens a destination ucontext with
#                       MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE, issues
#                       UVERBS_METHOD_RESTORE_PD (prerequisite) +
#                       UVERBS_METHOD_RESTORE_MR with a UHW payload
#                       carrying the source mkey_index, and confirms
#                       via INFO_HANDLES + RESP_LKEY/RESP_RKEY echo
#                       that the adopted MR landed at the caller-
#                       chosen ufile handle with the wire-visible
#                       identity preserved. Folds in the FW-level
#                       PROBE_MKEY check from test_mr_adopt.sh while
#                       the adopted MR is alive.
#
# Flow
# ----
#
#   Phase A  Provision source VF, set_tracked + enable_migratable, bind.
#   Phase B  Fork fw_id_continuity_probe on the source ibdev to allocate
#            one of each {PD, CQ, QP, MR, SRQ}. Capture src_pdn,
#            src_lkey, src_mkey_index, src_mr_addr, src_mr_length from
#            its READY-line dictionary. The probe stays alive holding
#            the MR (+ everything else) across SAVE.
#   Phase C  SAVE_VHCA_STATE on the source.
#   Phase D  Quit the source probe; tear down the source VF.
#   Phase E  Provision a fresh destination VF: set_tracked,
#            enable_migratable, LOAD_VHCA_STATE, mark_restored, bind.
#   Phase F  ===== THE TEST =====
#            Fork mr_restore_probe_mlx5_vfmig in the background against
#            the destination ibdev with the captured src_*. Wait for
#            "READY" -- at that point subtests 1-7 (gate, UAPI rejects
#            x4, happy path, collision) have all run and the adopted MR
#            is alive at MR_TARGET_HANDLE on the probe's ucontext, with
#            the parent PD adopted at PD_TARGET_HANDLE.
#   Phase G  While the MR + PD are alive, run MLX5_VFMIG_IOC_PROBE_MKEY
#            against the adopted mkey_index to confirm FW still sees
#            the mkc context (pd, start_addr, length) as the source
#            captured pre-SAVE. This is the same check test_mr_adopt.sh
#            runs; folding it in here gives a single integrated test
#            covering both the ABI + FW dimensions.
#   Phase H  Tell the probe to "quit", which triggers subtest 8 (v0
#            dealloc semantics: DEREG_MR on the adopted mkey must
#            -EINVAL because LOAD_VHCA_STATE also carried over the
#            source's mkey-referencing dependents and v0 has not yet
#            restored kernel uobjects for those). Wait for the probe
#            to exit 0.
#   Phase I  Verdict + manifest.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_mr_restore_mlx5_vfmig.sh
#
# Optional knobs:
#   BLOB                    Path for the SAVE blob.
#   TOOL                    mlx5_vfmig CLI.
#   SRC_PROBE               fw_id_continuity_probe binary.
#   DST_PROBE               mr_restore_probe_mlx5_vfmig binary.
#   MR_TARGET_HANDLE        ufile handle for the adopted MR (default 0x4242).
#   PD_TARGET_HANDLE        ufile handle for the adopted PD (default 0x4241).
#   ACCESS_FLAGS            Source MR access flags. fw_id_continuity_probe
#                           hard-codes LOCAL_WRITE|REMOTE_WRITE|REMOTE_READ
#                           = 0x7; this knob is exposed only to keep the
#                           contract explicit.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
SRC_PROBE=${SRC_PROBE:-$ROOT_DIR/uobject_restore/fw_id_continuity/fw_id_continuity_probe}
DST_PROBE=${DST_PROBE:-$ROOT_DIR/uobject_restore/mr_restore/mr_restore_probe_mlx5_vfmig}
BLOB=${BLOB:-/tmp/vf_mr_restore.blob}
META=${META:-${BLOB}.meta}
MR_TARGET_HANDLE=${MR_TARGET_HANDLE:-0x4242}
PD_TARGET_HANDLE=${PD_TARGET_HANDLE:-0x4241}
ACCESS_FLAGS=${ACCESS_FLAGS:-0x7}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]       || { echo "build $TOOL first";       exit 1; }
[ -x "$SRC_PROBE" ]  || { echo "build $SRC_PROBE first";  exit 1; }
[ -x "$DST_PROBE" ]  || { echo "build $DST_PROBE first";  exit 1; }
[ -e "$CDEV" ]       || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

WORKDIR=$(mktemp -d -t mrrestore.XXXXXX)
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

# --- helpers (copy-paste from test_pd_restore_mlx5_vfmig.sh) ---

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

# Fork the dst MR probe in the background. Mirrors the PD-restore
# harness with the larger arg list specific to RESTORE_MR.
start_dst_probe() {
    local ibdev=$1 pdn=$2 mkey_index=$3 lkey=$4 addr=$5 length=$6 access=$7
    local mr_target=$8 pd_target=$9
    local fifo_in="$WORKDIR/dst.in"
    local fifo_out="$WORKDIR/dst.out"

    mkfifo "$fifo_in" "$fifo_out"
    echo "=== dst probe: $DST_PROBE $ibdev $pdn $mkey_index $lkey $addr $length $access $mr_target $pd_target ==="
    sudo "$DST_PROBE" "$ibdev" "$pdn" "$mkey_index" "$lkey" \
        "$addr" "$length" "$access" "$mr_target" "$pd_target" \
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
    echo "  [dst probe] EOF before READY -- probe failed (subtests 1-7)"
    return 1
}

quit_dst_probe() {
    echo "quit" >&8; exec 8>&-
    while IFS= read -r line < "$WORKDIR/dst.out"; do
        echo "  [dst probe] $line"
        case "$line" in
            "  FAIL"*) dst_post_quit_fail=1 ;;
            "mr_restore_probe_mlx5_vfmig: PASS"*) dst_overall_pass=1 ;;
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

probe_mkey_into() {
    local label=$1 vf=$2 mkey_index=$3 prefix=$4
    local out line k v
    echo "=== PROBE_MKEY ($label): vf=$vf mkey_index=$mkey_index ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_mkey "$vf" "$mkey_index" 2>&1); then
        echo "$out" | sed 's/^/  /'
        echo "  PROBE_MKEY ioctl failed (transport / arg error)"
        return 1
    fi
    while IFS= read -r line; do
        echo "  [$label probe_mkey] $line"
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
echo "captured: src_pdn=$src_pdn src_lkey=$src_lkey src_mkey_index=$src_mkey_index"
echo "          src_mr_addr=$src_mr_addr src_mr_length=$src_mr_length"

# Sanity: lkey upper-24 must equal mkey_index (mlx5 invariant).
src_lkey_upper=$(printf "%u" $(( src_lkey >> 8 )))
if [ "$src_lkey_upper" != "$src_mkey_index" ]; then
    echo "FAIL: src_lkey=$src_lkey does not encode src_mkey_index=$src_mkey_index"
    exit 1
fi

# --- Phase C ---------------------------------------------------------

echo "=== Phase C: SAVE_VHCA_STATE ==="
sudo dmesg -C
# snapshot-ordering: two-phase dump-side quiesce (design C.7). Pause the
# initiator (RUNNING -> P2P), then -- after the cross-host barrier, a
# no-op on this single host -- the responder (P2P -> STOP), at CRIU
# CHECKPOINT_DEVICES. SAVE is suspend-aware (sees STOP, skips its own
# suspend); resume walks the ladder back up.
sudo "$TOOL" "$PF" suspend_vhca 0 initiator
sudo "$TOOL" "$PF" suspend_vhca 0 responder
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" resume_vhca 0 responder
sudo "$TOOL" "$PF" resume_vhca 0 initiator
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
bind_vf_safe "$DST_VF" 60 "Phase E: dest VF bind"
sleep 1
DST_IBDEV=$(find_ib_dev_for_pci "$DST_VF") || { echo "FAIL: no ibdev for dest $DST_VF"; exit 1; }
echo "dest ibdev: $DST_IBDEV"

# --- Phase F: THE TEST ----------------------------------------------

echo
echo "================ Phase F: RESTORE_MR ================="
echo "Drive UVERBS_METHOD_RESTORE_MR on $DST_IBDEV with the src mkey."
echo "The probe runs subtests 1-7 (gate, UAPI rejects x4, happy path,"
echo "collision) and then parks at READY while the adopted MR is alive"
echo "at handle $MR_TARGET_HANDLE (with parent PD at $PD_TARGET_HANDLE)."
if ! start_dst_probe "$DST_IBDEV" "$src_pdn" "$src_mkey_index" \
                     "$src_lkey" "$src_mr_addr" "$src_mr_length" \
                     "$ACCESS_FLAGS" "$MR_TARGET_HANDLE" \
                     "$PD_TARGET_HANDLE"; then
    echo "FAIL: dst probe did not reach READY"
    exit 1
fi

# --- Phase G: cross-check FW liveness of the adopted mkey -----------

echo
echo "================ Phase G: FW liveness check =========="
echo "While the adopted MR is alive, run MLX5_VFMIG_IOC_PROBE_MKEY against"
echo "the same mkey_index and confirm fw_pd / fw_start_addr / fw_length"
echo "still match the source's pre-SAVE view. fw_accept=1 + content match"
echo "means the FW side of Model A is sound under the live verb path."
probe_mkey_into adopted 0 "$src_mkey_index" pos

# --- Phase H: destroy round-trip ------------------------------------

echo
echo "================ Phase H: v0 dealloc semantics ======="
echo "Tell the probe to quit; it will run subtest 8."
echo
echo "Asymmetric with PD's subtest 7. In the FW resource graph, mkey is a"
echo "leaf under PD; QPs reference an mkey by (lkey/rkey) wire value rather"
echo "than as a tracked FW resource dep. So DESTROY_MKEY on the orphan"
echo "adopted mkey SUCCEEDS even with the source's mkey-using QPs still"
echo "alive in destination FW. __mlx5_ib_dereg_mr returns 0 and the kernel"
echo "uobj is freed. Subtest 8 PASSes when the dereg succeeds AND"
echo "INFO_HANDLES(MR) drops the handle."
echo
echo "v0 implication for CRIU: unlike PDs (where FW automatically refuses"
echo "premature dealloc), MR teardown ordering is plugin-policy only --"
echo "the kernel will accept any DEREG_MR. This is documented in"
echo "design/uobject_restore.md S4b."
quit_dst_probe

# --- verdict --------------------------------------------------------

overall_rc=0
fw_ok=0
content_ok=0

if [ "${pos_fw_accept:-0}" = 1 ]; then
    fw_ok=1
fi

# Compare PROBE_MKEY's mkc snapshot against what the source captured
# pre-SAVE. This is the same byte-equality check test_mr_adopt.sh
# runs; see test_mr_adopt.sh's content-match block for the rationale.
if [ "$fw_ok" = 1 ]; then
    src_pdn_hex=$(printf '0x%06x' "$src_pdn")
    if [ "${pos_fw_pd:-}" = "$src_pdn_hex" ] \
       && [ "${pos_fw_start_addr:-}" = "$src_mr_addr" ] \
       && [ "${pos_fw_length:-}" = "$src_mr_length" ]; then
        content_ok=1
    fi
fi

echo
echo "================ MR RESTORE VERDICT ================="
echo "  src_pdn               = $src_pdn"
echo "  src_mkey_index        = $src_mkey_index"
echo "  src_lkey              = $src_lkey"
echo "  src_mr_addr           = $src_mr_addr"
echo "  src_mr_length         = $src_mr_length"
echo "  mr_target_handle      = $MR_TARGET_HANDLE"
echo "  pd_target_handle      = $PD_TARGET_HANDLE"
echo
echo "  dst probe subtests    = ${DST_PROBE_RC:-?}  (0 means all PASS)"
if [ "${dst_overall_pass:-0}" = 1 ]; then
    echo "  dst probe overall     = PASS"
else
    echo "  dst probe overall     = FAIL/UNKNOWN"
fi
echo "  dst adopted_pdn       = ${dst_adopted_pdn:-?}"
echo "  dst adopted_mkey_idx  = ${dst_adopted_mkey_index:-?}"
echo "  FW liveness fw_accept = ${pos_fw_accept:-?}  syndrome=${pos_fw_syndrome:-?}"
echo "  FW mkc.pd             = ${pos_fw_pd:-?}    expected=$(printf '0x%06x' "$src_pdn")"
echo "  FW mkc.start_addr     = ${pos_fw_start_addr:-?}    expected=$src_mr_addr"
echo "  FW mkc.length         = ${pos_fw_length:-?}    expected=$src_mr_length"
echo

if [ "${DST_PROBE_RC:-1}" = 0 ] \
   && [ "${dst_overall_pass:-0}" = 1 ] \
   && [ "$fw_ok" = 1 ] \
   && [ "$content_ok" = 1 ]; then
    echo "  PASS  RESTORE_MR landed on $DST_IBDEV @ handle $MR_TARGET_HANDLE,"
    echo "        adopted mkey_index $src_mkey_index, FW still sees the mkc"
    echo "        context (pd, start_addr, length) byte-equal to source pre-SAVE,"
    echo "        and DEREG_MR cleanly tears down the orphan mkey (mkey is a"
    echo "        leaf in the FW resource graph -- subtest 8's v0 expectation,"
    echo "        asymmetric with PD's parent role). MR teardown ordering is"
    echo "        plugin-policy only at the v0 layer; the kernel will not"
    echo "        refuse a premature DEREG_MR. mlx5_ib_restore_mr is functional"
    echo "        for the v0 critical path."
elif [ "${DST_PROBE_RC:-1}" = 0 ] \
     && [ "${dst_overall_pass:-0}" = 1 ] \
     && [ "$fw_ok" = 1 ]; then
    echo "  WEAK PASS  Verb path landed and FW accepted PROBE_MKEY, but the"
    echo "             mkc snapshot did not match the source pre-SAVE values."
    echo "             Possible causes:"
    echo "               - subtle PRM packing change (re-check probe_mkey_into"
    echo "                 hex formatting vs. fw_id_continuity_probe's emit)"
    echo "               - LOAD_VHCA_STATE didn't fully restore mkc"
    echo "             Treat as a yellow flag and investigate before claiming"
    echo "             Model A v0-correct."
    overall_rc=1
else
    echo "  FAIL  see the per-subtest log above. Common causes:"
    echo "        - kernel not running B1+B2 (rebuild + reboot needed)"
    echo "        - DEVX-aware ucontext on src (uid != 0): test_mr_adopt.sh"
    echo "          only validated the uid=0 case"
    echo "        - LOAD_VHCA_STATE didn't preserve the source mkey"
    echo "          (re-run test_mr_adopt.sh in isolation to confirm)"
    overall_rc=1
fi
echo "====================================================="

# --- manifest -------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# mr_restore_mlx5_vfmig manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_pdn=$src_pdn
src_mkey_index=$src_mkey_index
src_lkey=$src_lkey
src_mr_addr=$src_mr_addr
src_mr_length=$src_mr_length
src_access_flags=$ACCESS_FLAGS
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV
mr_target_handle=$MR_TARGET_HANDLE
pd_target_handle=$PD_TARGET_HANDLE
dst_probe_rc=${DST_PROBE_RC:-?}
dst_overall_pass=${dst_overall_pass:-0}
dst_adopted_pdn=${dst_adopted_pdn:-?}
dst_adopted_mkey_index=${dst_adopted_mkey_index:-?}
fw_accept=${pos_fw_accept:-?}
fw_syndrome=${pos_fw_syndrome:-?}
fw_pd=${pos_fw_pd:-?}
fw_start_addr=${pos_fw_start_addr:-?}
fw_length=${pos_fw_length:-?}
fw_content_match=$content_ok
overall_rc=$overall_rc
EOF
sudo chmod 0644 "$META"
echo "wrote $META"

exit "$overall_rc"
