#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S3b "Generalising to QP/CQ" empirical probe (CQ branch).
#
# Why we care
# -----------
# The PD-gate analysis (design/pd_registration_wipe.md) showed
# DEALLOC_PD on a vfmig-restored PD fails with status=0x9
# syndrome=0xef0c8a regardless of the asserting uid -- the per-VHCA
# (pdn -> owner_uid) registration table is wiped by LOAD_VHCA_STATE.
# The QP destroy matrix (qp_destroy_matrix/) showed DESTROY_QP and
# 2RST_QP work cross-uid against vfmig-restored QPCs. Open question
# for the v0 QP-restore data path: does DESTROY_CQ behave like
# DESTROY_QP (cross-uid honored, no kernel gate needed) or like
# DEALLOC_PD (cross-uid honored opcode-wise but the resource is
# unknown to the dest VHCA's allocator, kernel gate needed)?
#
# Outcome determines whether mlx5_ib_destroy_cq needs a parallel
# vfmig_restored gate. The §6 "Generalising to QP/CQ" recipe in
# pd_registration_wipe.md spells out the gate pattern; this
# harness establishes whether to instantiate it.
#
# Test matrix
# -----------
# 3 source CQs (one per uid lane) -> SAVE/LOAD -> 3-cell matrix:
#
#     uid=0           DESTROY_CQ(cqn0, uid=0)
#     uid=src_devx    DESTROY_CQ(cqn1, uid=src_devx_uid)
#     uid=hi_unalloc  DESTROY_CQ(cqn2, uid=0xfffd)
#
# Per cell we capture (via PROBE_CQ_DESTROY ioctl):
#   pre_query_status      QUERY_CQ pre-op (sanity: CQC must exist)
#   pre_cqc_status        cqc.status pre-op
#   op_status             FW return on DESTROY_CQ (0=ack, !=0=reject)
#   op_syndrome           FW syndrome on DESTROY_CQ
#   post_query_status     QUERY_CQ post-op (the smoking gun:
#                          0 -> CQC still alive (silent no-op or
#                               op_status was non-zero too)
#                          !=0 -> CQC gone)
#   post_cqc_status       cqc.status post-op (only meaningful if alive)
#   destroyed             1 iff op_status==0 && post_query_status!=0
#   op_accept             1 iff op_status==0 && op_syndrome==0
#
# Verdict per cell:
#   destroyed=1 op_accept=1                    -> cross-uid worked
#   destroyed=0 op_accept=1                    -> SILENT NO-OP
#   destroyed=0 op_accept=0 syndrome=0xef0c8a  -> registration wipe
#   destroyed=0 op_accept=0 syndrome=other     -> other rejection
#
# Source flow mirrors qp_destroy_matrix:
#
#   Phase A  Provision source VF.
#   Phase B  3 fw_id_continuity_probe instances. Each opens its
#            own libmlx5 ucontext (auto-DEVX) and allocates a
#            full PD+CQ+QP+MR shape; we capture src{N}_cqn from
#            each. Probes hold their resources through SAVE.
#   Phase C  SAVE_VHCA_STATE.
#   Phase D  Quit probes; tear down source VF.
#   Phase E  Fresh DEST VF + LOAD + bind.
#   Phase F  Run the 3-cell PROBE_CQ_DESTROY matrix.
#   Phase G  Print verdict.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_cq_destroy_matrix.sh
#
# Optional knobs:
#   BLOB        SAVE blob path (default /tmp/vf_cq_destroy_matrix.blob)
#   TOOL        mlx5_vfmig CLI
#   PROBE       fw_id_continuity_probe binary
#   HI_UNALLOC  uid value for the high-unalloc lane (default 0xfffd)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe}
BLOB=${BLOB:-/tmp/vf_cq_destroy_matrix.blob}
META=${META:-${BLOB}.meta}
HI_UNALLOC=${HI_UNALLOC:-$((0xfffd))}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]  || { echo "build $TOOL first: make -C $ROOT_DIR";  exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first: make -C $ROOT_DIR"; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

WORKDIR=$(mktemp -d -t cqdmtx.XXXXXX)
declare -a SRC_PROBE_PIDS=()
cleanup() {
    local i pid
    for i in "${!SRC_PROBE_PIDS[@]}"; do
        pid=${SRC_PROBE_PIDS[$i]}
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            local fifo_in="$WORKDIR/src_$i.in"
            echo "quit" > "$fifo_in" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

vf_path()       { echo "/sys/bus/pci/devices/$1"; }
wait_for_path() { for i in $(seq 1 50); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }

bind_vf_safe() {
    local vf=$1
    local timeout=${2:-60}
    local label=${3:-bind}
    local started bg
    started=$(date +%s)
    ( echo "$vf" | sudo tee /sys/bus/pci/drivers/mlx5_core/bind ) >/dev/null 2>&1 &
    bg=$!
    while :; do
        if [ -e "$(vf_path $vf)/driver" ]; then
            wait "$bg" 2>/dev/null || true
            return 0
        fi
        if ! kill -0 "$bg" 2>/dev/null; then
            sleep 0.2
            [ -e "$(vf_path $vf)/driver" ] && return 0
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

start_indexed_src_probe() {
    local idx=$1
    local ibdev=$2
    local prefix=$3
    local fifo_in="$WORKDIR/src_$idx.in"
    local fifo_out="$WORKDIR/src_$idx.out"

    mkfifo "$fifo_in" "$fifo_out"
    echo "=== source probe #$idx: $PROBE $ibdev ==="
    sudo "$PROBE" "$ibdev" < "$fifo_in" > "$fifo_out" 2>&1 &
    local pid=$!
    SRC_PROBE_PIDS+=("$pid")
    eval "exec $((10 + idx))> \"\$fifo_in\""

    local line
    while IFS= read -r line < "$fifo_out"; do
        echo "  [src#$idx] $line"
        case "$line" in
            READY) return 0 ;;
            *=*)
                local k="${line%%=*}"
                local v="${line#*=}"
                eval "${prefix}_${k}=\"\$v\""
                ;;
        esac
    done
    echo "  [src#$idx] EOF before READY -- probe failed"
    return 1
}

quit_indexed_src_probe() {
    local idx=$1
    local fd=$((10 + idx))
    local pid=${SRC_PROBE_PIDS[$idx]}
    eval "echo quit >&${fd}" 2>/dev/null || true
    eval "exec ${fd}>&-" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    SRC_PROBE_PIDS[$idx]=""
}

# `mlx5_vfmig $PF probe_cq_destroy <vf> <cqn> <uid_hint>`
probe_cq_destroy_into() {
    local label=$1 vf=$2 cqn=$3 uid_hint=$4 prefix=$5
    local out line k v
    echo "=== PROBE_CQ_DESTROY ($label): vf=$vf cqn=$cqn uid_hint=$uid_hint ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_cq_destroy "$vf" "$cqn" "$uid_hint" 2>&1); then
        echo "$out" | sed 's/^/  /'
        echo "  PROBE_CQ_DESTROY ioctl failed"
        return 1
    fi
    while IFS= read -r line; do
        echo "  [$label] $line"
        case "$line" in
            *=*) k="${line%%=*}"; v="${line#*=}";
                 eval "${prefix}_${k}=\"\$v\"" ;;
        esac
    done <<<"$out"
    return 0
}

# Drop dependent QP first (DESTROY_QP works cross-uid per
# qp_destroy_matrix), then exercise DESTROY_CQ on the same slot.
# Equivalent to the DEALLOC_PD chain harness's pattern: we want
# the matrix to mirror the actual mlx5_ib teardown order, so that
# any cross-uid DESTROY_CQ rejection is attributable to the
# CQN-registration table (the question we're asking) rather than
# to dependent-QP enforcement (the case fully explored elsewhere).
destroy_qp_uid0() {
    local label=$1 vf=$2 qpn=$3
    local out
    echo "=== chain step: DESTROY_QP($label, qpn=$qpn, uid=0) ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_qp_teardown "$vf" "$qpn" 0 0 2>&1); then
        echo "$out" | sed 's/^/  /'
        return 1
    fi
    echo "$out" | sed 's/^/  /'
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
SRC_IBDEV=$(find_ib_dev_for_pci "$SRC_VF") || { echo "FAIL: no ibdev"; exit 1; }
echo "source ibdev: $SRC_IBDEV"

# --- Phase B ---------------------------------------------------------

echo "=== Phase B: 3 source probes, capturing 3 cqn ==="
sudo dmesg -C
echo 'func mlx5_ib_alloc_ucontext +p' | \
    sudo tee /sys/kernel/debug/dynamic_debug/control >/dev/null 2>&1 || true
for i in 0 1 2; do
    start_indexed_src_probe "$i" "$SRC_IBDEV" "src${i}"
done

src_devx_uid=$(sudo dmesg | \
    grep -E "vfmig_uctx_dbg: alloc_ucontext ibdev=$SRC_IBDEV devx_uid=[0-9]+ adopted=0" | \
    tail -1 | sed -nE 's/.* devx_uid=([0-9]+) .*/\1/p')
src_devx_uid=${src_devx_uid:-0}
echo "captured: src_devx_uid=$src_devx_uid"
if [ "$src_devx_uid" = 0 ]; then
    echo "  WARNING: src_devx_uid == 0 -- libmlx5 did NOT auto-allocate a"
    echo "           DEVX uid for these probes. The 'uid=src_devx' lane is"
    echo "           degenerate (== uid=0)."
fi

# --- Phase C ---------------------------------------------------------

echo "=== Phase C: SAVE_VHCA_STATE ==="
# snapshot-ordering: pause datapath (CRIU CHECKPOINT_DEVICES), then
# capture (SAVE is suspend-aware and skips its own suspend), then resume.
sudo "$TOOL" "$PF" suspend_vhca 0
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" resume_vhca 0
sudo chmod 0644 "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
[ "$SAVE_BYTES" -gt 16 ] || { echo "FAIL: blob suspiciously small: $SAVE_BYTES"; exit 1; }
echo "  save bytes: $SAVE_BYTES"

# --- Phase D ---------------------------------------------------------

echo "=== Phase D: quit src probes + tear down source VF ==="
for i in 0 1 2; do
    quit_indexed_src_probe "$i"
done
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
sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" mark_restored 0

echo mlx5_core | sudo tee "$(vf_path $DST_VF)/driver_override" >/dev/null
bind_vf_safe "$DST_VF" 60 "Phase E: dest VF bind"sleep 1
DST_IBDEV=$(find_ib_dev_for_pci "$DST_VF") || { echo "FAIL: no ibdev"; exit 1; }
echo "dest ibdev: $DST_IBDEV"

# --- Phase F: 3-cell matrix ------------------------------------------

echo
echo "================ Phase F: 3-cell PROBE_CQ_DESTROY matrix ================"
echo "src_devx_uid=$src_devx_uid hi_unalloc=$HI_UNALLOC"
echo "CQNs per probe slot:"
for i in 0 1 2; do
    eval "echo \"  src${i}: pdn=\$src${i}_pdn cqn=\$src${i}_cqn qpn=\$src${i}_qpn\""
done

# For each slot: drop the dependent QP first (cross-uid DESTROY_QP
# is known to work per qp_destroy_matrix), then exercise the CQ
# destroy on the same slot under the lane's uid_hint.
destroy_qp_uid0 "slot0" 0 "$src0_qpn"
probe_cq_destroy_into "destroy_zero" 0 "$src0_cqn" 0 c_zero

destroy_qp_uid0 "slot1" 0 "$src1_qpn"
probe_cq_destroy_into "destroy_src"  0 "$src1_cqn" "$src_devx_uid" c_src

destroy_qp_uid0 "slot2" 0 "$src2_qpn"
probe_cq_destroy_into "destroy_hi"   0 "$src2_cqn" "$HI_UNALLOC" c_hi

# --- Phase G: verdict ------------------------------------------------

echo
echo "================ Phase G: §S3b CQ destroy matrix ================"
printf "%-26s %-22s %-22s\n" "uid lane" "verdict" "op_status/syndrome"
printf "%-26s %-22s %-22s\n" "--------------------------" "----------------------" "----------------------"

decode_cell() {
    local prefix=$1
    local destroyed op_acc op_status op_syndrome
    eval "destroyed=\${${prefix}_destroyed:-?}"
    eval "op_acc=\${${prefix}_op_accept:-?}"
    eval "op_status=\${${prefix}_op_status:-?}"
    eval "op_syndrome=\${${prefix}_op_syndrome:-?}"
    if [ "$destroyed" = 1 ]; then
        echo "destroyed"
    elif [ "$op_acc" = 1 ]; then
        echo "SILENT NO-OP"
    elif [ "$op_status" != 0 ] && [ "$op_syndrome" = "0x00ef0c8a" ]; then
        echo "registration wipe"
    elif [ "$op_status" != 0 ]; then
        echo "rejected (other)"
    else
        echo "?"
    fi
}

verdict_zero=$(decode_cell c_zero)
verdict_src=$(decode_cell c_src)
verdict_hi=$(decode_cell c_hi)

eval "row_zero=\"\${c_zero_op_status:-?}/\${c_zero_op_syndrome:-?}\""
eval "row_src=\"\${c_src_op_status:-?}/\${c_src_op_syndrome:-?}\""
eval "row_hi=\"\${c_hi_op_status:-?}/\${c_hi_op_syndrome:-?}\""

printf "%-26s %-22s %-22s\n" "uid=0 (host-priv)" "$verdict_zero" "$row_zero"
printf "%-26s %-22s %-22s\n" "uid=src_devx=$src_devx_uid" "$verdict_src" "$row_src"
printf "%-26s %-22s %-22s\n" "uid=hi_unalloc=$HI_UNALLOC" "$verdict_hi" "$row_hi"

echo
echo "Detailed output (forensic decode):"
for cell in c_zero c_src c_hi; do
    eval "echo \"  $cell: pre_query_status=\${${cell}_pre_query_status:-?} pre_cqc_status=\${${cell}_pre_cqc_status:-?} op_status=\${${cell}_op_status:-?} op_syndrome=\${${cell}_op_syndrome:-?} post_query_status=\${${cell}_post_query_status:-?} post_cqc_status=\${${cell}_post_cqc_status:-?} destroyed=\${${cell}_destroyed:-?}\""
done

echo
overall_verdict="UNKNOWN"
if [ "$verdict_src" = "destroyed" ] && [ "$verdict_zero" = "destroyed" ]; then
    overall_verdict="CROSS-UID DESTROY_CQ WORKS -- no kernel gate needed for vfmig-restored CQs"
elif [ "$verdict_src" = "registration wipe" ]; then
    overall_verdict="CQN REGISTRATION WIPED -- need parallel mlx5_ib_destroy_cq gate (mirror PD pattern)"
elif [ "$verdict_src" = "SILENT NO-OP" ]; then
    overall_verdict="SILENT NO-OP CONFIRMED on cross-uid DESTROY_CQ -- alarming, see qp_destroy_matrix for reference"
fi
echo "Overall: $overall_verdict"

# --- manifest --------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# cq_destroy_matrix manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_devx_uid=$src_devx_uid
hi_unalloc=$HI_UNALLOC
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV

src0_pdn=$src0_pdn src0_cqn=$src0_cqn src0_qpn=$src0_qpn
src1_pdn=$src1_pdn src1_cqn=$src1_cqn src1_qpn=$src1_qpn
src2_pdn=$src2_pdn src2_cqn=$src2_cqn src2_qpn=$src2_qpn

verdict_zero=$verdict_zero
verdict_src=$verdict_src
verdict_hi=$verdict_hi
overall_verdict=$overall_verdict
EOF
echo
echo "manifest: $META"
echo "blob: $BLOB ($SAVE_BYTES bytes)"
