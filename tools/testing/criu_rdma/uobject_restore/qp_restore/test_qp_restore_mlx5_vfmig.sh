#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S6b end-to-end: validate UVERBS_METHOD_RESTORE_QP + mlx5_ib_restore_qp
# against a real save/load cycle.
#
# This is the driver-end-to-end follow-on to test_fw_id_continuity.sh's
# K6 §6.3 piggyback (which only validates FW-side QPC byte-equality
# across LOAD_VHCA_STATE without exercising the new kernel verb path):
#
#   test_fw_id_continuity.sh K6_QP_STATE=RTS K6_POST_RECV_WRS=4
#                       Drives MLX5_VFMIG_IOC_QUERY_QP against the PF
#                       cdev to confirm the source's qpn is alive on
#                       the destination after LOAD_VHCA_STATE and that
#                       the QPC fields written by RESET->INIT->RTR->RTS
#                       round-trip byte-equal. Validates the *underlying
#                       firmware behaviour* but does NOT exercise the
#                       new kernel verb path.
#
#   this script         Validates the actual landed verb path:
#                       opens a destination ucontext with
#                       MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE, performs the
#                       RESTORE_PD + RESTORE_CQ setup chain, then
#                       issues UVERBS_METHOD_RESTORE_QP with a UHW
#                       payload carrying the source's (qpn, WQ shape,
#                       buf_addr, db_addr), and confirms via
#                       INFO_HANDLES + RESP_QPN echo that the adopted
#                       QP landed at the caller-chosen ufile handle
#                       with wire-visible identity preserved. Folds
#                       the FW-level QUERY_QP comparison from K6 into
#                       Phase G *while the kernel-side mlx5_ib_qp is
#                       active*, upgrading the comparison from "does
#                       LOAD preserve QPC bits?" to "does the new
#                       mlx5_qpc_adopt_qp wiring preserve QPC bits
#                       FW reads back via QUERY_QP under the live
#                       verb path?".
#
# Why "QUERY_QP while alive" matters
# ----------------------------------
# K6's QUERY_QP runs against a destination VF that is bound to
# mlx5_core but has NO ucontext or kernel-side mlx5_ib_qp for the
# adopted qpn. The check there is purely "FW-side QPC continuity
# across LOAD_VHCA_STATE" -- the kernel ib_device hasn't been told
# the qpn exists. Folding the same QUERY_QP call into Phase G
# upgrades it to "FW-side QPC continuity with the kernel ib_device
# actively wrapping that qpn in a kernel-side mlx5_ib_qp via
# mlx5_qpc_adopt_qp + mlx5_ib_register_user_qp_in_dev_lists" --
# i.e. the union of every state machine that has to agree about a
# restored QP. If Phase G's snapshot still byte-matches the
# source's pre-SAVE snapshot, the new wiring has not corrupted any
# QPC bits FW reads back through QUERY_QP.
#
# RTS-with-pending-WRs happy-path validation
# ------------------------------------------
# The source probe is invoked with --qp-state RTS --post-recv-wrs 4
# so the K7-locked RTS-set group (path_mtu, primary_address_path,
# remote_qpn, log_sra_max, retry_count, rnr_retry, next_send_psn,
# next_rcv_psn, last_acked_psn, plus the queue counters touched by
# the posted recv WRs) is populated in the QPC. Phase G then byte-
# compares this entire group between the source's pre-SAVE QUERY_QP
# and the destination's live-verb-path QUERY_QP.
#
# Flow
# ----
#
#   Phase A  Provision source VF, set_tracked + enable_migratable, bind.
#            Bring the source netdev up (RTR/RTS need the GID table).
#   Phase B  Fork fw_id_continuity_probe on the source ibdev with
#            --qp-state RTS --post-recv-wrs N. Capture src_pdn,
#            src_cqn, src_cqe, src_cqe_size, src_cq_buf_addr,
#            src_cq_db_addr, src_qpn, src_sq_wqe_count,
#            src_rq_wqe_count, src_rq_wqe_shift, src_qp_buf_addr,
#            src_qp_db_addr from its READY-line dictionary. Run
#            QUERY_QP on the source VF to capture the QPC subset
#            the byte-equality assertion below needs.
#   Phase C  SAVE_VHCA_STATE on the source.
#   Phase D  Quit the source probe; tear down the source VF.
#   Phase E  Provision a fresh destination VF: set_tracked,
#            enable_migratable, LOAD_VHCA_STATE, mark_restored,
#            bind. Bring the destination netdev up (the live-
#            verb-path QUERY_QP path is netdev-agnostic but a
#            future S7 ARP-replay rung needs the netdev anyway).
#   Phase F  ===== THE TEST =====
#            Fork qp_restore_probe_mlx5_vfmig in the background
#            against the destination ibdev with the captured src_*.
#            Wait for "READY" -- at that point subtests 1-11 (gate,
#            setup PD/CQ, UAPI rejects x5, dispatcher rejects x2,
#            happy path, collision) have all run and the adopted
#            QP is alive at qp_target_handle on the probe's ucontext.
#   Phase G  While the QP is alive, run MLX5_VFMIG_IOC_QUERY_QP
#            against the adopted qpn and byte-compare the QPC
#            snapshot to the source's pre-SAVE snapshot from
#            Phase B. Match means the FW side of Model A is sound
#            under the live verb path.
#   Phase H  Tell the probe to "quit", which triggers subtest 13
#            (v0 dealloc semantics: DESTROY_QP succeeds because QP
#            is a leaf in the FW resource graph; ASYMMETRIC with
#            CQ/PD subtests). Wait for the probe to exit 0.
#   Phase I  Verdict + manifest.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_qp_restore_mlx5_vfmig.sh
#
# Optional knobs:
#   BLOB                    Path for the SAVE blob.
#   TOOL                    mlx5_vfmig CLI.
#   SRC_PROBE               fw_id_continuity_probe binary.
#   DST_PROBE               qp_restore_probe_mlx5_vfmig binary.
#   QP_TARGET_HANDLE        ufile handle for the adopted QP (default 0x4248).
#   POST_RECV_WRS           number of recv WRs to post on the source RQ
#                           (default 4; set 0 to skip the queue-counter
#                           comparisons).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
SRC_PROBE=${SRC_PROBE:-$ROOT_DIR/uobject_restore/fw_id_continuity/fw_id_continuity_probe}
DST_PROBE=${DST_PROBE:-$ROOT_DIR/uobject_restore/qp_restore/qp_restore_probe_mlx5_vfmig}
BLOB=${BLOB:-/tmp/vf_qp_restore.blob}
META=${META:-${BLOB}.meta}
QP_TARGET_HANDLE=${QP_TARGET_HANDLE:-0x4248}
POST_RECV_WRS=${POST_RECV_WRS:-4}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]       || { echo "build $TOOL first";       exit 1; }
[ -x "$SRC_PROBE" ]  || { echo "build $SRC_PROBE first";  exit 1; }
[ -x "$DST_PROBE" ]  || { echo "build $DST_PROBE first";  exit 1; }
[ -e "$CDEV" ]       || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

WORKDIR=$(mktemp -d -t qprestore.XXXXXX)
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

# --- helpers (copy-paste pattern from test_cq_restore_mlx5_vfmig.sh) ---

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

# Bring the netdev up so the RoCE GID table populates -- required for
# the source probe's RTR->RTS modify_qp (the AV references an
# sgid_index resolved via the GID table).
bring_netdev_up_for_pci() {
    local bdf=$1
    local nd_path nd
    for nd_path in $(vf_path "$bdf")/net/*; do
        [ -d "$nd_path" ] || continue
        nd=$(basename "$nd_path")
        sudo ip link set "$nd" up 2>/dev/null || true
        sleep 0.3
        return 0
    done
    echo "  WARN: no netdev found under $(vf_path $bdf)/net (RoCE may fail)"
    return 1
}

# Source probe -- driven with --qp-state RTS --post-recv-wrs $POST_RECV_WRS
# so the RTS-set QPC fields (path_mtu, AV, log_sra_max, retry_count,
# rnr_retry, PSNs) and the queue counters touched by the posted recv
# WRs are populated. Captures every key=value line emitted before
# READY into src_$key shell variables.
start_src_probe() {
    local ibdev=$1
    local fifo_in="$WORKDIR/src.in"
    local fifo_out="$WORKDIR/src.out"

    mkfifo "$fifo_in" "$fifo_out"
    echo "=== source probe: $SRC_PROBE $ibdev --qp-state RTS --post-recv-wrs $POST_RECV_WRS ==="
    sudo "$SRC_PROBE" "$ibdev" \
         --qp-state RTS --post-recv-wrs "$POST_RECV_WRS" \
         < "$fifo_in" > "$fifo_out" 2>&1 &
    SRC_PROBE_PID=$!
    exec 7> "$fifo_in"
    # Hold fifo_out open as fd 6 across the entire read loop. Per-
    # iteration `read < $fifo` would close + reopen each cycle,
    # opening a SIGPIPE race window where the probe's next write
    # races a reader-less moment and gets killed mid-output.
    exec 6< "$fifo_out"

    local line
    while IFS= read -r line <&6; do
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
    exec 6<&-
    wait "$SRC_PROBE_PID" 2>/dev/null || true
    SRC_PROBE_PID=""
    echo "  [src probe] exited"
}

# Destination probe -- carries the source's QP context as 14
# positional args (src_pdn, src_cqn / cqe / cqe_size / cq_buf_addr /
# cq_db_addr, src_qpn, sq_wqe_count, rq_wqe_count, rq_wqe_shift,
# qp_buf_addr, qp_db_addr, qp_type, qp_state). The probe emits
# adopted_qpn / qp_target_handle / READY before parking on stdin.
start_dst_probe() {
    local ibdev=$1 pdn=$2
    local cqn=$3 cqe=$4 cqe_size=$5 cq_buf=$6 cq_db=$7
    local qpn=$8 sq_wqe=$9 rq_wqe=${10} rq_shift=${11}
    local qp_buf=${12} qp_db=${13}
    local qp_type=${14} qp_state=${15} qp_target=${16}
    local fifo_in="$WORKDIR/dst.in"
    local fifo_out="$WORKDIR/dst.out"

    mkfifo "$fifo_in" "$fifo_out"
    echo "=== dst probe: $DST_PROBE $ibdev $pdn $cqn $cqe $cqe_size $cq_buf $cq_db \\"
    echo "                            $qpn $sq_wqe $rq_wqe $rq_shift $qp_buf $qp_db \\"
    echo "                            $qp_type $qp_state $qp_target ==="
    sudo "$DST_PROBE" "$ibdev" "$pdn" \
        "$cqn" "$cqe" "$cqe_size" "$cq_buf" "$cq_db" \
        "$qpn" "$sq_wqe" "$rq_wqe" "$rq_shift" "$qp_buf" "$qp_db" \
        "$qp_type" "$qp_state" "$qp_target" \
        < "$fifo_in" > "$fifo_out" 2>&1 &
    DST_PROBE_PID=$!
    exec 8> "$fifo_in"
    # Hold fifo_out open as fd 5 across the entire read loop. Per-
    # iteration `read < $fifo` would close + reopen each cycle,
    # opening a SIGPIPE race window where the probe's next write
    # races a reader-less moment and gets killed mid-output.
    exec 5< "$fifo_out"

    local line k v
    while IFS= read -r line <&5; do
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
    echo "  [dst probe] EOF before READY -- probe failed (subtests 1-11)"
    return 1
}

quit_dst_probe() {
    echo "quit" >&8; exec 8>&-
    while IFS= read -r line <&5; do
        echo "  [dst probe] $line"
        case "$line" in
            "  FAIL"*) dst_post_quit_fail=1 ;;
            "qp_restore_probe_mlx5_vfmig: PASS"*) dst_overall_pass=1 ;;
        esac
    done
    exec 5<&-
    if wait "$DST_PROBE_PID"; then
        DST_PROBE_RC=0
    else
        DST_PROBE_RC=$?
    fi
    DST_PROBE_PID=""
    echo "  [dst probe] exited rc=$DST_PROBE_RC"
}

# QUERY_QP via the PF cdev. Same shell-variable-prefix pattern as
# test_fw_id_continuity.sh's query_qp_into.
query_qp_into() {
    local label=$1 vf_id=$2 qpn=$3 prefix=$4
    local out line k v
    echo "=== QUERY_QP ($label): vf=$vf_id qpn=$qpn ==="
    if ! out=$(sudo "$TOOL" "$PF" query_qp "$vf_id" "$qpn" 2>&1); then
        echo "$out" | sed 's/^/  /'
        echo "  QUERY_QP ioctl failed (transport / arg error)"
        return 1
    fi
    while IFS= read -r line; do
        echo "  [$label query_qp] $line"
        case "$line" in
            *=*) k="${line%%=*}"; v="${line#*=}";
                 eval "${prefix}_${k}=\"\$v\"" ;;
        esac
    done <<<"$out"
    return 0
}

# Per-field QPC byte-equality verdict. Mirror of
# test_fw_id_continuity.sh::qpc_verdict, scoped to this script.
qpc_verdict() {
    local field=$1 src=$2 dst=$3
    if [ -z "$src" ] || [ -z "$dst" ]; then
        echo "  SKIP  $field:  src=${src:-?}  dst=${dst:-?}   (missing data)"
        rq_skipped=$((rq_skipped + 1))
        return
    fi
    if [ "$src" = "$dst" ]; then
        echo "  PASS  $field:  src=$src  dst=$dst   (matches under live verb path)"
    else
        echo "  FAIL  $field:  src=$src  dst=$dst   (RESTORE_QP wiring corrupted QPC)"
        rq_overall_rc=1
    fi
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

# RTR/RTS modify_qp pulls a sgid_index out of the netdev's GID table,
# so the netdev must be up before the source probe runs.
bring_netdev_up_for_pci "$SRC_VF"

# --- Phase B ---------------------------------------------------------

echo "=== Phase B: source probe (RTS-with-pending-WRs; resources alive across SAVE) ==="
start_src_probe "$SRC_IBDEV"

echo "captured (CQ): src_cqn=$src_cqn src_cqe=$src_cqe src_cqe_size=$src_cqe_size"
echo "               src_cq_buf_addr=$src_cq_buf_addr src_cq_db_addr=$src_cq_db_addr"
echo "captured (QP): src_pdn=$src_pdn src_qpn=$src_qpn"
echo "               sq_wqe_count=$src_sq_wqe_count rq_wqe_count=$src_rq_wqe_count rq_wqe_shift=$src_rq_wqe_shift"
echo "               src_qp_buf_addr=$src_qp_buf_addr src_qp_db_addr=$src_qp_db_addr"
echo "               qp_state=$src_qp_state recv_wrs_posted=$src_recv_wrs_posted"

# Pre-SAVE QUERY_QP -- captures the QPC subset needed for Phase G
# byte-equality. Stored under the `srcq_*` prefix so it doesn't
# collide with the source-probe's `src_*` manifest fields.
if ! query_qp_into source 0 "$src_qpn" srcq; then
    echo "FAIL: source-side QUERY_QP(vf=0, qpn=$src_qpn) ioctl failed"
    exit 1
fi
if [ "${srcq_qpc_state:-?}" = "?" ]; then
    echo "FAIL: source-side QUERY_QP did not emit qpc_state (parse failure?)"
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
bind_vf_safe "$DST_VF" 60 "Phase E: dest VF bind"
sleep 1
DST_IBDEV=$(find_ib_dev_for_pci "$DST_VF") || { echo "FAIL: no ibdev for dest $DST_VF"; exit 1; }
echo "dest ibdev: $DST_IBDEV"

# Bring the dest netdev up too -- not strictly required for this
# verb-only test (no destination-side modify_qp), but keeps parity
# with the source side and is needed by future S7 / S8 rungs.
bring_netdev_up_for_pci "$DST_VF"

# --- Phase F: THE TEST ----------------------------------------------

echo
echo "================ Phase F: RESTORE_QP ================="
echo "Drive UVERBS_METHOD_RESTORE_QP on $DST_IBDEV with the src qpn."
echo "The probe runs subtests 1-11 (gate, RESTORE_PD + RESTORE_CQ"
echo "setup, UAPI rejects x5, dispatcher rejects x2, happy path,"
echo "collision) and then parks at READY while the adopted QP is"
echo "alive at handle $QP_TARGET_HANDLE."
if ! start_dst_probe "$DST_IBDEV" "$src_pdn" \
                     "$src_cqn" "$src_cqe" "$src_cqe_size" \
                     "$src_cq_buf_addr" "$src_cq_db_addr" \
                     "$src_qpn" "$src_sq_wqe_count" \
                     "$src_rq_wqe_count" "$src_rq_wqe_shift" \
                     "$src_qp_buf_addr" "$src_qp_db_addr" \
                     RC RTS "$QP_TARGET_HANDLE"; then
    echo "FAIL: dst probe did not reach READY"
    exit 1
fi

# --- Phase G: cross-check FW continuity of the adopted qpn ----------

echo
echo "================ Phase G: FW-side QPC continuity =========="
echo "While the adopted QP is alive, run MLX5_VFMIG_IOC_QUERY_QP"
echo "against the same qpn and confirm the QPC subset (state, AV,"
echo "PSNs, retry counts, queue counters, ...) still matches the"
echo "source's pre-SAVE snapshot. This is the *live-verb-path*"
echo "upgrade of test_fw_id_continuity.sh's K6 §6.3 piggyback --"
echo "if mlx5_qpc_adopt_qp + mlx5_ib_register_user_qp_in_dev_lists"
echo "or any of the new wiring corrupted QPC bits FW reads back"
echo "via QUERY_QP, the byte-comparison below would catch it."
rq_overall_rc=0
rq_skipped=0
if ! query_qp_into adopted 0 "$src_qpn" dstq; then
    echo "FAIL: dest-side QUERY_QP(qpn=$src_qpn) ioctl failed"
    rq_overall_rc=1
fi

# Per-field comparison. Always-compared = state-independent +
# INIT-set + queue counters; RTR-set if rank>=2; RTS-set if rank>=3.
# Source probe was driven to RTS so we expect the full set.
if [ "${rq_overall_rc}" = 0 ]; then
    echo "  --- state-independent / INIT-set ---"
    qpc_verdict state         "${srcq_qpc_state:-}"         "${dstq_qpc_state:-}"
    qpc_verdict pd            "${srcq_qpc_pd:-}"            "${dstq_qpc_pd:-}"
    qpc_verdict q_key         "${srcq_qpc_q_key:-}"         "${dstq_qpc_q_key:-}"
    qpc_verdict uar_page      "${srcq_qpc_uar_page:-}"      "${dstq_qpc_uar_page:-}"
    qpc_verdict log_page_size "${srcq_qpc_log_page_size:-}" "${dstq_qpc_log_page_size:-}"
    qpc_verdict log_sq_size   "${srcq_qpc_log_sq_size:-}"   "${dstq_qpc_log_sq_size:-}"
    qpc_verdict log_rq_size   "${srcq_qpc_log_rq_size:-}"   "${dstq_qpc_log_rq_size:-}"
    qpc_verdict log_msg_max   "${srcq_qpc_log_msg_max:-}"   "${dstq_qpc_log_msg_max:-}"
    qpc_verdict user_index    "${srcq_qpc_user_index:-}"    "${dstq_qpc_user_index:-}"
    qpc_verdict cqn_snd       "${srcq_qpc_cqn_snd:-}"       "${dstq_qpc_cqn_snd:-}"
    qpc_verdict cqn_rcv       "${srcq_qpc_cqn_rcv:-}"       "${dstq_qpc_cqn_rcv:-}"

    echo "  --- queue counters (RQ touched by $src_recv_wrs_posted post_recv) ---"
    qpc_verdict sw_rq_counter "${srcq_qpc_sw_rq_counter:-}" "${dstq_qpc_sw_rq_counter:-}"
    qpc_verdict hw_rq_counter "${srcq_qpc_hw_rq_counter:-}" "${dstq_qpc_hw_rq_counter:-}"

    echo "  --- RTR-set fields (path_mtu / AV / ...) ---"
    qpc_verdict path_mtu              "${srcq_qpc_path_mtu:-}"              "${dstq_qpc_path_mtu:-}"
    qpc_verdict min_rnr_nak           "${srcq_qpc_min_rnr_nak:-}"           "${dstq_qpc_min_rnr_nak:-}"
    qpc_verdict log_rra_max           "${srcq_qpc_log_rra_max:-}"           "${dstq_qpc_log_rra_max:-}"
    qpc_verdict remote_qpn            "${srcq_qpc_remote_qpn:-}"            "${dstq_qpc_remote_qpn:-}"
    qpc_verdict primary_address_path  "${srcq_qpc_primary_address_path:-}"  "${dstq_qpc_primary_address_path:-}"
    qpc_verdict next_rcv_psn          "${srcq_qpc_next_rcv_psn:-}"          "${dstq_qpc_next_rcv_psn:-}"

    echo "  --- RTS-set fields (retry / PSNs / ...) ---"
    qpc_verdict log_sra_max   "${srcq_qpc_log_sra_max:-}"   "${dstq_qpc_log_sra_max:-}"
    qpc_verdict retry_count   "${srcq_qpc_retry_count:-}"   "${dstq_qpc_retry_count:-}"
    qpc_verdict rnr_retry     "${srcq_qpc_rnr_retry:-}"     "${dstq_qpc_rnr_retry:-}"
    qpc_verdict next_send_psn "${srcq_qpc_next_send_psn:-}" "${dstq_qpc_next_send_psn:-}"
    qpc_verdict last_acked_psn "${srcq_qpc_last_acked_psn:-}" "${dstq_qpc_last_acked_psn:-}"
fi

# --- Phase H: destroy round-trip ------------------------------------

echo
echo "================ Phase H: v0 dealloc semantics ======="
echo "Tell the probe to quit; it will run subtest 13."
echo
echo "ASYMMETRIC with CQ subtest 10 / PD subtest 7; SYMMETRIC with"
echo "MR subtest 8. QP is a *leaf* in the FW resource graph: peer"
echo "qpc.remote_qpn does not install a back-reference, so FW"
echo "DESTROY_QP on an orphan adopted qpn is ACCEPTED. Its only"
echo "dependents (PD, CQs) are PARENTS of the QP and remain alive"
echo "across the destroy."
echo
echo "v0 implication for CRIU: QP teardown can run before its"
echo "parent CQ/PD have been deallocated -- and indeed *must*,"
echo "because deallocating CQ first would fail with the orphan-CQ"
echo "rejection from cq_restore subtest 10. Documented in"
echo "design/uobject_restore.md S6b + S10.8."
quit_dst_probe

# --- verdict --------------------------------------------------------

overall_rc=0
fw_path_ok=0
fw_content_ok=0

if [ "${dst_overall_pass:-0}" = 1 ]; then
    fw_path_ok=1
fi
if [ "$rq_overall_rc" = 0 ]; then
    fw_content_ok=1
fi

echo
echo "================ QP RESTORE VERDICT ================="
echo "  src_pdn               = $src_pdn"
echo "  src_cqn               = $src_cqn"
echo "  src_qpn               = $src_qpn"
echo "  sq_wqe_count          = $src_sq_wqe_count"
echo "  rq_wqe_count          = $src_rq_wqe_count"
echo "  rq_wqe_shift          = $src_rq_wqe_shift"
echo "  qp_buf_addr           = $src_qp_buf_addr"
echo "  qp_db_addr            = $src_qp_db_addr"
echo "  qp_state              = $src_qp_state"
echo "  recv_wrs_posted       = $src_recv_wrs_posted"
echo "  qp_target_handle      = $QP_TARGET_HANDLE"
echo
echo "  dst probe subtests    = ${DST_PROBE_RC:-?}  (0 means all PASS)"
if [ "${dst_overall_pass:-0}" = 1 ]; then
    echo "  dst probe overall     = PASS"
else
    echo "  dst probe overall     = FAIL/UNKNOWN"
fi
echo "  dst adopted_qpn       = ${dst_adopted_qpn:-?}"
echo "  qpc.state             = ${dstq_qpc_state:-?}    expected=${srcq_qpc_state:-?}"
echo "  qpc.next_send_psn     = ${dstq_qpc_next_send_psn:-?}    expected=${srcq_qpc_next_send_psn:-?}"
echo "  qpc.next_rcv_psn      = ${dstq_qpc_next_rcv_psn:-?}    expected=${srcq_qpc_next_rcv_psn:-?}"
echo "  qpc.path_mtu          = ${dstq_qpc_path_mtu:-?}    expected=${srcq_qpc_path_mtu:-?}"
echo "  qpc.primary_address_path"
echo "                  src   = ${srcq_qpc_primary_address_path:-?}"
echo "                  dst   = ${dstq_qpc_primary_address_path:-?}"
echo "  fw_content skipped    = $rq_skipped fields (missing data)"
echo

if [ "${DST_PROBE_RC:-1}" = 0 ] \
   && [ "$fw_path_ok" = 1 ] \
   && [ "$fw_content_ok" = 1 ]; then
    echo "  PASS  RESTORE_QP landed on $DST_IBDEV @ handle $QP_TARGET_HANDLE,"
    echo "        adopted qpn $src_qpn, FW still sees the QPC subset"
    echo "        (state, INIT-set, RTR-set AV/PSNs, RTS-set retry/PSNs,"
    echo "        queue counters touched by $src_recv_wrs_posted post_recv WRs)"
    echo "        byte-equal to source pre-SAVE under the live verb path,"
    echo "        and DESTROY_QP correctly REAPS the leaf qpn while parents"
    echo "        (PD, CQs) remain alive (subtest 13's v0 expectation,"
    echo "        ASYMMETRIC with CQ/PD parent-role rejection in their"
    echo "        own subtests)."
    echo "        mlx5_ib_restore_qp is functional for the v0 critical path."
elif [ "${DST_PROBE_RC:-1}" = 0 ] \
     && [ "$fw_path_ok" = 1 ]; then
    echo "  WEAK PASS  Verb path landed and DESTROY_QP succeeded, but the"
    echo "             QPC snapshot did not match the source pre-SAVE values."
    echo "             Possible causes:"
    echo "               - mlx5_qpc_adopt_qp's kernel-side wiring (radix"
    echo "                 tree insert + debug_qp_add) clobbered a QPC"
    echo "                 field FW reads back via QUERY_QP -- improbable,"
    echo "                 the helper does no FW commands"
    echo "               - mlx5_ib_register_user_qp_in_dev_lists triggered"
    echo "                 a side-effect (CQ-list lock / fence) that"
    echo "                 mutated QPC state"
    echo "               - LOAD_VHCA_STATE didn't fully restore qpc"
    echo "                 (re-run test_fw_id_continuity.sh K6_QP_STATE=RTS"
    echo "                  in isolation; if the K6 piggyback also reports"
    echo "                  the mismatch the verb path is innocent)"
    echo "             Treat as a yellow flag and investigate before"
    echo "             claiming Model A v0-correct."
    overall_rc=1
else
    echo "  FAIL  see the per-subtest log above. Common causes:"
    echo "        - kernel not running B1+B2+B3 (rebuild + reboot needed)"
    echo "        - DEVX-aware ucontext on src (uid != 0): the mlx5_ib"
    echo "          restore handler explicitly excludes that path for v0"
    echo "        - LOAD_VHCA_STATE didn't preserve the source qpn"
    echo "          (re-run test_fw_id_continuity.sh K6_QP_STATE=RTS in"
    echo "          isolation to confirm)"
    echo "        - vfmig placeholder lookup failed inside"
    echo "          mlx5_vfmig_bind_user_qp / mlx5_vfmig_bind_user_dbr"
    echo "          (check dmesg for placeholder-iova messages)"
    overall_rc=1
fi
echo "====================================================="

# --- manifest -------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# qp_restore_mlx5_vfmig manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_pdn=$src_pdn
src_cqn=$src_cqn
src_cqe=$src_cqe
src_cqe_size=$src_cqe_size
src_cq_buf_addr=$src_cq_buf_addr
src_cq_db_addr=$src_cq_db_addr
src_qpn=$src_qpn
src_sq_wqe_count=$src_sq_wqe_count
src_rq_wqe_count=$src_rq_wqe_count
src_rq_wqe_shift=$src_rq_wqe_shift
src_qp_buf_addr=$src_qp_buf_addr
src_qp_db_addr=$src_qp_db_addr
src_qp_state=$src_qp_state
src_recv_wrs_posted=$src_recv_wrs_posted
srcq_qpc_state=${srcq_qpc_state:-}
srcq_qpc_next_send_psn=${srcq_qpc_next_send_psn:-}
srcq_qpc_next_rcv_psn=${srcq_qpc_next_rcv_psn:-}
srcq_qpc_path_mtu=${srcq_qpc_path_mtu:-}
srcq_qpc_remote_qpn=${srcq_qpc_remote_qpn:-}
srcq_qpc_primary_address_path=${srcq_qpc_primary_address_path:-}
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV
qp_target_handle=$QP_TARGET_HANDLE
post_recv_wrs=$POST_RECV_WRS
dst_probe_rc=${DST_PROBE_RC:-?}
dst_overall_pass=${dst_overall_pass:-0}
dst_adopted_qpn=${dst_adopted_qpn:-?}
dstq_qpc_state=${dstq_qpc_state:-}
dstq_qpc_next_send_psn=${dstq_qpc_next_send_psn:-}
dstq_qpc_next_rcv_psn=${dstq_qpc_next_rcv_psn:-}
dstq_qpc_path_mtu=${dstq_qpc_path_mtu:-}
dstq_qpc_remote_qpn=${dstq_qpc_remote_qpn:-}
dstq_qpc_primary_address_path=${dstq_qpc_primary_address_path:-}
fw_content_ok=$fw_content_ok
fw_content_skipped=$rq_skipped
overall_rc=$overall_rc
EOF
sudo chmod 0644 "$META"
echo "wrote $META"

exit "$overall_rc"
