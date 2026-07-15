#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S3b "DEALLOC_PD-after-DESTROY_QP" empirical -- v2 of the dealloc
# PD probe. Differences from test_dealloc_pd_matrix.sh (v1):
#   * Source probes are FULL shape (PD + CQ + QP), matching the
#     CRIU agent's pd_cq_qp repro instead of PD-only. The PDC
#     therefore has a real QP dependent at SAVE time.
#   * The matrix per lane now runs:
#         DESTROY_QP(qpn, uid=0)              -- always uid=0 here,
#                                                proven to honor
#                                                cross-uid in
#                                                test_qp_destroy_matrix.
#         DEALLOC_PD(pdn, uid=<lane>)         -- the cell under test
#         DEALLOC_PD(pdn, uid=<lane>)         -- second call: tight
#                                                existence check
#                                                ("PDC unknown" syn
#                                                 0xef0c8a-class).
#   * No PROBE_PD bracketing -- v1 showed CREATE_MKEY is lenient
#     about pdn validity and therefore not a tight existence test.
#     The second-dealloc syndrome IS the existence test:
#         op_status=0  -> first call worked, PDC was alive
#         op_status=-EINVAL syn=0xef0c8a -> first call already
#           freed it (or PDC was never there)
#
# This is the harness designed to actually reproduce (or refute)
# the agent's failure: their dmesg shows
#   DEALLOC_PD failed status bad_resource_state syndrome 0xef0c8a
#   under uid=0 against a PDC that was alive at the source (uid=2).
# v1 showed: on a fresh LOAD with PD-only, DEALLOC_PD(uid=0)
# succeeds. So if the failure is reproducible, it must come from
# something the QP/CQ contributed -- this harness adds them back.
#
# Per-cell decoder:
#   first_op  second_op  -> verdict
#   accept    reject     -> PDC alive after DESTROY_QP, dealloc OK
#                           (uid lane is host-priv for DEALLOC_PD,
#                            assuming the post-QP-destroy PDC was
#                            still alive; this is the GOOD case)
#   accept    accept     -> SILENT NO-OP confirmed: FW ack'd the
#                           dealloc but the PDC is still allocated
#                           (we'd see TWO consecutive accepts).
#   reject    *          -> PDC was already gone before our dealloc
#                           ran. THIS reproduces the agent's
#                           syndrome 0xef0c8a -- meaning DESTROY_QP
#                           (or some other side effect) implicitly
#                           freed the PDC. Look there for the bug.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_dealloc_pd_chain.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe}
BLOB=${BLOB:-/tmp/vf_dealloc_pd_chain.blob}
META=${META:-${BLOB}.meta}
HI_UNALLOC=${HI_UNALLOC:-$((0xfffd))}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]  || { echo "build $TOOL first";  exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first"; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV";       exit 1; }

WORKDIR=$(mktemp -d -t depdc.XXXXXX)
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
            wait "$bg" 2>/dev/null || true; return 0
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
        [ "$pci" = "$bdf" ] && { basename "$d"; return 0; }
    done
    return 1
}

start_indexed_probe() {
    local idx=$1
    local ibdev=$2
    local prefix=$3
    local fifo_in="$WORKDIR/src_$idx.in"
    local fifo_out="$WORKDIR/src_$idx.out"
    mkfifo "$fifo_in" "$fifo_out"
    echo "=== src probe #$idx (full PD+CQ+QP): $PROBE $ibdev ==="
    sudo "$PROBE" "$ibdev" < "$fifo_in" > "$fifo_out" 2>&1 &
    local pid=$!
    SRC_PROBE_PIDS+=("$pid")
    eval "exec $((10 + idx))> \"\$fifo_in\""
    local line
    while IFS= read -r line < "$fifo_out"; do
        echo "  [src#$idx] $line"
        case "$line" in
            READY) return 0 ;;
            *=*) eval "${prefix}_${line%%=*}=\"${line#*=}\"" ;;
        esac
    done
    echo "  [src#$idx] EOF before READY"
    return 1
}

quit_indexed_probe() {
    local idx=$1
    local fd=$((10 + idx))
    local pid=${SRC_PROBE_PIDS[$idx]}
    eval "echo quit >&${fd}"
    eval "exec ${fd}>&-"
    wait "$pid" 2>/dev/null || true
    SRC_PROBE_PIDS[$idx]=""
}

# Run probe_qp_teardown(DESTROY_QP, uid=0) and capture op_accept,
# qpc_alive_after_op into "${prefix}_*". Caller checks op_accept=1
# and qpc_alive_after_op=0 for "QP destroyed".
probe_destroy_qp_into() {
    local label=$1
    local vf=$2
    local qpn=$3
    local prefix=$4
    local out line
    echo "=== DESTROY_QP via PROBE_QP_TEARDOWN ($label): vf=$vf qpn=$qpn uid=0 ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_qp_teardown "$vf" "$qpn" 0 0 2>&1); then
        echo "$out" | sed 's/^/  /'
        return 1
    fi
    while IFS= read -r line; do
        echo "  [$label] $line"
        case "$line" in
            *=*) eval "${prefix}_${line%%=*}=\"${line#*=}\"" ;;
        esac
    done <<<"$out"
    return 0
}

probe_dealloc_pd_into() {
    local label=$1
    local vf=$2
    local pdn=$3
    local uid_hint=$4
    local prefix=$5
    local out line
    echo "=== PROBE_DEALLOC_PD ($label): vf=$vf pdn=$pdn uid_hint=$uid_hint ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_dealloc_pd "$vf" "$pdn" "$uid_hint" 2>&1); then
        echo "$out" | sed 's/^/  /'
        return 1
    fi
    while IFS= read -r line; do
        echo "  [$label] $line"
        case "$line" in
            *=*) eval "${prefix}_${line%%=*}=\"${line#*=}\"" ;;
        esac
    done <<<"$out"
    return 0
}

# --- Phase A: provision SOURCE VF -------------------------------------

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

# --- Phase B: 3 source probes (FULL shape) ----------------------------

echo "=== Phase B: 3 full-shape source probes ==="
sudo dmesg -C
# Enable mlx5_ib_dbg trace in mlx5_ib_alloc_ucontext to surface
# the ucontext's freshly-allocated devx_uid. Replaces the older
# vfmig_pd_dbg pr_info that was retired when the gate landed.
echo 'func mlx5_ib_alloc_ucontext +p' | \
    sudo tee /sys/kernel/debug/dynamic_debug/control >/dev/null 2>&1 || true
for i in 0 1 2; do
    start_indexed_probe "$i" "$SRC_IBDEV" "src${i}"
done

src_devx_uid=$(sudo dmesg | \
    grep -E "vfmig_uctx_dbg: alloc_ucontext ibdev=$SRC_IBDEV devx_uid=[0-9]+ adopted=0" | \
    tail -1 | sed -nE 's/.* devx_uid=([0-9]+) .*/\1/p')
src_devx_uid=${src_devx_uid:-0}
echo "captured: src_devx_uid=$src_devx_uid"

# --- Phase C/D --------------------------------------------------------

echo "=== Phase C: SAVE_VHCA_STATE ==="
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
echo "  save bytes: $SAVE_BYTES"

echo "=== Phase D: quit src probes + tear down source VF ==="
for i in 0 1 2; do quit_indexed_probe "$i"; done
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
bind_vf_safe "$DST_VF" 60 "Phase E: dest VF bind"
sleep 1
DST_IBDEV=$(find_ib_dev_for_pci "$DST_VF") || { echo "FAIL"; exit 1; }
echo "dest ibdev: $DST_IBDEV"

# --- Phase F: chain matrix --------------------------------------------

echo
echo "================ Phase F: 3-cell DESTROY_QP -> DEALLOC_PD chain ================"
echo "src_devx_uid=$src_devx_uid hi_unalloc=$HI_UNALLOC"
echo "Per-slot PDN/QPN:"
for i in 0 1 2; do
    eval "echo \"  src${i}: pdn=\$src${i}_pdn qpn=\$src${i}_qpn cqn=\$src${i}_cqn\""
done

run_chain_cell() {
    local cell=$1
    local pdn=$2
    local qpn=$3
    local lane_uid=$4
    echo
    echo "----- Cell $cell: pdn=$pdn qpn=$qpn lane_uid=$lane_uid -----"
    probe_destroy_qp_into  "${cell}_qp"     0 "$qpn" "${cell}_qp"
    probe_dealloc_pd_into  "${cell}_pd1"    0 "$pdn" "$lane_uid" "${cell}_pd1"
    probe_dealloc_pd_into  "${cell}_pd2"    0 "$pdn" "$lane_uid" "${cell}_pd2"
}

run_chain_cell "c_zero" "$src0_pdn" "$src0_qpn" 0
run_chain_cell "c_src"  "$src1_pdn" "$src1_qpn" "$src_devx_uid"
run_chain_cell "c_hi"   "$src2_pdn" "$src2_qpn" "$HI_UNALLOC"

# --- Phase G: verdict -------------------------------------------------

echo
echo "================ Phase G: DESTROY_QP -> DEALLOC_PD chain verdict ================"
printf "%-30s %-12s %-12s %-12s %-22s\n" \
    "uid lane" "qp_destroyed" "pd1_op" "pd2_op" "verdict"
printf "%-30s %-12s %-12s %-12s %-22s\n" \
    "------------------------------" "------------" "------------" \
    "------------" "----------------------"

decode_chain_cell() {
    local prefix=$1
    local qp_alive_after_op qp_op_acc pd1 pd2
    eval "qp_alive_after_op=\${${prefix}_qp_qpc_alive_after_op:-?}"
    eval "qp_op_acc=\${${prefix}_qp_op_accept:-?}"
    eval "pd1=\${${prefix}_pd1_op_accept:-?}"
    eval "pd2=\${${prefix}_pd2_op_accept:-?}"

    local qp_destroyed=0
    if [ "$qp_op_acc" = 1 ] && [ "$qp_alive_after_op" = 0 ]; then
        qp_destroyed=1
    fi

    local v
    if [ "$qp_destroyed" = 0 ]; then
        v="QP_DESTROY_FAILED"
    elif [ "$pd1" = 1 ] && [ "$pd2" = 0 ]; then
        v="PD_DEALLOC_OK"
    elif [ "$pd1" = 1 ] && [ "$pd2" = 1 ]; then
        v="SILENT_NOOP"
    elif [ "$pd1" = 0 ]; then
        v="PD_ALREADY_GONE_AT_DEALLOC1"
    else
        v="?"
    fi
    echo "$qp_destroyed:$pd1:$pd2:$v"
}

emit_row() {
    local label=$1
    local prefix=$2
    local r qp pd1 pd2 v
    r=$(decode_chain_cell "$prefix")
    qp=$(echo "$r" | cut -d: -f1)
    pd1=$(echo "$r" | cut -d: -f2)
    pd2=$(echo "$r" | cut -d: -f3)
    v=$(echo "$r" | cut -d: -f4)
    printf "%-30s %-12s %-12s %-12s %-22s\n" \
        "$label" "$qp" "$pd1" "$pd2" "$v"
}

emit_row "uid=0 (host-priv)"            c_zero
emit_row "uid=src_devx=$src_devx_uid"   c_src
emit_row "uid=hi_unalloc=$HI_UNALLOC"   c_hi

echo
echo "Detailed forensic output:"
for cell in c_zero c_src c_hi; do
    eval "echo \"  ${cell}: qp_op=\${${cell}_qp_op_status:-?}/\${${cell}_qp_op_syndrome:-?} qp_alive=\${${cell}_qp_qpc_alive_after_op:-?} pd1=\${${cell}_pd1_op_status:-?}/\${${cell}_pd1_op_syndrome:-?} pd2=\${${cell}_pd2_op_status:-?}/\${${cell}_pd2_op_syndrome:-?}\""
done

echo
zero_v=$(decode_chain_cell c_zero | cut -d: -f4)
src_v=$(decode_chain_cell c_src   | cut -d: -f4)
hi_v=$(decode_chain_cell c_hi    | cut -d: -f4)

if [ "$zero_v" = "PD_DEALLOC_OK" ]; then
    overall="DESTROY_QP -> DEALLOC_PD(uid=0) chain works fine; agent's failure NOT reproduced"
elif [ "$zero_v" = "PD_ALREADY_GONE_AT_DEALLOC1" ]; then
    overall="*** SMOKING GUN ***: PDC vanishes during DESTROY_QP. Agent's syndrome 0xef0c8a reproduced."
elif [ "$zero_v" = "SILENT_NOOP" ]; then
    overall="DEALLOC_PD silent-no-op confirmed across DESTROY_QP boundary"
else
    overall="UNKNOWN ($zero_v)"
fi

echo "Overall: $overall"

sudo tee "$META" >/dev/null <<EOF
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_devx_uid=$src_devx_uid
hi_unalloc=$HI_UNALLOC
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV
src0_pdn=$src0_pdn src0_qpn=$src0_qpn
src1_pdn=$src1_pdn src1_qpn=$src1_qpn
src2_pdn=$src2_pdn src2_qpn=$src2_qpn
c_zero=$(decode_chain_cell c_zero)
c_src=$(decode_chain_cell c_src)
c_hi=$(decode_chain_cell c_hi)
overall_verdict=$overall
EOF
echo
echo "manifest: $META"
echo "blob: $BLOB ($SAVE_BYTES bytes)"
