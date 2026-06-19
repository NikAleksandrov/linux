#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S3b "Generalising to QP/CQ" empirical probe (MR/mkey branch).
#
# Companion to qp_destroy_matrix.sh and cq_destroy_matrix.sh:
# determines whether DESTROY_MKEY honors caller-supplied uid_hint
# against vfmig-restored mkeys, or whether it triggers a
# registration-wipe rejection (status 0x9 syndrome 0xef0c8a) that
# would require a parallel gate in mlx5_ib_dereg_mr.
#
# Test matrix
# -----------
# 3 source MRs (one per uid lane) -> SAVE/LOAD -> 3-cell matrix:
#
#     uid=0           DESTROY_MKEY(mkey_index0, uid=0)
#     uid=src_devx    DESTROY_MKEY(mkey_index1, uid=src_devx_uid)
#     uid=hi_unalloc  DESTROY_MKEY(mkey_index2, uid=0xfffd)
#
# Per cell we capture (via PROBE_MR_DESTROY ioctl):
#   pre_query_status      QUERY_MKEY pre-op (sanity: mkey must exist)
#   pre_mkc_free          mkc.free pre-op (should be 0 for an active MR)
#   op_status             FW return on DESTROY_MKEY
#   op_syndrome           FW syndrome on DESTROY_MKEY
#   post_query_status     QUERY_MKEY post-op (smoking gun)
#   post_mkc_free         mkc.free post-op (only if alive)
#   destroyed             1 iff op_status==0 && post_query_status!=0
#   op_accept             1 iff op_status==0 && op_syndrome==0
#
# Verdict per cell (mirrors cq_destroy_matrix decode):
#   destroyed=1 op_accept=1                    -> cross-uid worked
#   destroyed=0 op_accept=1                    -> SILENT NO-OP
#   destroyed=0 op_accept=0 syndrome=0xef0c8a  -> registration wipe
#   destroyed=0 op_accept=0 syndrome=other     -> other rejection
#
# Source flow mirrors qp_destroy_matrix / cq_destroy_matrix.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_mr_destroy_matrix.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe}
BLOB=${BLOB:-/tmp/vf_mr_destroy_matrix.blob}
META=${META:-${BLOB}.meta}
HI_UNALLOC=${HI_UNALLOC:-$((0xfffd))}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]  || { echo "build $TOOL first"; exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first"; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

WORKDIR=$(mktemp -d -t mrdmtx.XXXXXX)
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
    local started
    local bg
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
    local d
    local pci
    for d in /sys/class/infiniband/*; do
        [ -e "$d/device" ] || continue
        pci=$(basename "$(readlink "$d/device")")
        [ "$pci" = "$bdf" ] && { basename "$d"; return 0; }
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
                eval "${prefix}_${k}=\"\$v\"" ;;
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

probe_mr_destroy_into() {
    local label=$1
    local vf=$2
    local mkey_index=$3
    local uid_hint=$4
    local prefix=$5
    local out
    local line
    local k
    local v
    echo "=== PROBE_MR_DESTROY ($label): vf=$vf mkey_index=$mkey_index uid_hint=$uid_hint ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_mr_destroy "$vf" "$mkey_index" "$uid_hint" 2>&1); then
        echo "$out" | sed 's/^/  /'
        echo "  PROBE_MR_DESTROY ioctl failed"
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

# --- Phase A --------------------------------------------------------

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
bind_vf_safe "$SRC_VF" 60 "Phase A"
sleep 1
SRC_IBDEV=$(find_ib_dev_for_pci "$SRC_VF") || { echo "FAIL: no ibdev"; exit 1; }
echo "source ibdev: $SRC_IBDEV"

# --- Phase B --------------------------------------------------------

echo "=== Phase B: 3 source probes, capturing 3 mkey_index ==="
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
[ "$src_devx_uid" = 0 ] && echo "  WARNING: src_devx_uid lane is degenerate (== 0)"

# --- Phase C --------------------------------------------------------

echo "=== Phase C: SAVE_VHCA_STATE ==="
# snapshot-ordering: pause datapath (CRIU CHECKPOINT_DEVICES), then
# capture (SAVE is suspend-aware and skips its own suspend), then resume.
sudo "$TOOL" "$PF" suspend_vhca 0
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" resume_vhca 0
sudo chmod 0644 "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
echo "  save bytes: $SAVE_BYTES"

# --- Phase D --------------------------------------------------------

echo "=== Phase D: quit src probes + tear down source VF ==="
for i in 0 1 2; do
    quit_indexed_src_probe "$i"
done
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null

# --- Phase E --------------------------------------------------------

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
bind_vf_safe "$DST_VF" 60 "Phase E"
sleep 1
DST_IBDEV=$(find_ib_dev_for_pci "$DST_VF") || { echo "FAIL: no ibdev"; exit 1; }
echo "dest ibdev: $DST_IBDEV"

# --- Phase F: 3-cell matrix -----------------------------------------

echo
echo "================ Phase F: 3-cell PROBE_MR_DESTROY matrix ================"
echo "src_devx_uid=$src_devx_uid hi_unalloc=$HI_UNALLOC"
echo "MKEY indices per probe slot:"
for i in 0 1 2; do
    eval "echo \"  src${i}: pdn=\$src${i}_pdn cqn=\$src${i}_cqn qpn=\$src${i}_qpn mkey_index=\$src${i}_mkey_index\""
done

probe_mr_destroy_into "destroy_zero" 0 "$src0_mkey_index" 0 c_zero
probe_mr_destroy_into "destroy_src"  0 "$src1_mkey_index" "$src_devx_uid" c_src
probe_mr_destroy_into "destroy_hi"   0 "$src2_mkey_index" "$HI_UNALLOC" c_hi

# --- Phase G: verdict -----------------------------------------------

echo
echo "================ Phase G: §S3b MR destroy matrix ================"
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
    eval "echo \"  $cell: pre_query_status=\${${cell}_pre_query_status:-?} pre_mkc_free=\${${cell}_pre_mkc_free:-?} op_status=\${${cell}_op_status:-?} op_syndrome=\${${cell}_op_syndrome:-?} post_query_status=\${${cell}_post_query_status:-?} post_mkc_free=\${${cell}_post_mkc_free:-?} destroyed=\${${cell}_destroyed:-?}\""
done

echo
overall_verdict="UNKNOWN"
if [ "$verdict_src" = "destroyed" ] && [ "$verdict_zero" = "destroyed" ]; then
    overall_verdict="CROSS-UID DESTROY_MKEY WORKS -- no kernel gate needed for vfmig-restored mkeys"
elif [ "$verdict_src" = "registration wipe" ]; then
    overall_verdict="MKEY REGISTRATION WIPED -- need parallel mlx5_ib_dereg_mr gate (mirror PD pattern)"
elif [ "$verdict_src" = "SILENT NO-OP" ]; then
    overall_verdict="SILENT NO-OP CONFIRMED on cross-uid DESTROY_MKEY"
fi
echo "Overall: $overall_verdict"

# --- manifest -------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# mr_destroy_matrix manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_devx_uid=$src_devx_uid
hi_unalloc=$HI_UNALLOC
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV

src0_pdn=$src0_pdn src0_cqn=$src0_cqn src0_qpn=$src0_qpn src0_mkey_index=$src0_mkey_index
src1_pdn=$src1_pdn src1_cqn=$src1_cqn src1_qpn=$src1_qpn src1_mkey_index=$src1_mkey_index
src2_pdn=$src2_pdn src2_cqn=$src2_cqn src2_qpn=$src2_qpn src2_mkey_index=$src2_mkey_index

verdict_zero=$verdict_zero
verdict_src=$verdict_src
verdict_hi=$verdict_hi
overall_verdict=$overall_verdict
EOF
echo
echo "manifest: $META"
echo "blob: $BLOB ($SAVE_BYTES bytes)"
