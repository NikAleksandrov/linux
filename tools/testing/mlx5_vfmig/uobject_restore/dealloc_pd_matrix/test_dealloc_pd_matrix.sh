#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S3b "DEALLOC_PD uid-gating" empirical probe -- follow-up to
# test_qp_destroy_matrix.sh (2026-06-03), which refuted the CRIU
# agent's "FW silently no-ops cross-uid DESTROY_QP" hypothesis on
# RESET-state QPs. With QP teardown ruled out, the next-most-
# likely cause of the agent's pd_cq_qp DEALLOC_PD failure is
# DEALLOC_PD itself having uid-scoped semantics: FW gates the
# dealloc on the asserting uid matching the PDC's owning uid,
# even on a dependent-free PDC.
#
# This harness tests that hypothesis directly:
#   - 3 source probes, each running fw_id_continuity_probe with
#     --pd-only so the PDC has ZERO dependents (no CQ/QP/MR/SRQ
#     to confound the dealloc result).
#   - SAVE_VHCA_STATE on the source.
#   - LOAD_VHCA_STATE on a fresh dest VF. Per §S3b "PROBE_PD"
#     work, the source's PDCs land on the dest with their owner-
#     uid intact (we confirmed CREATE_MKEY-acceptance under those
#     uids in test_pd_adopt.sh).
#   - 3-cell matrix:
#       Cell 0: PROBE_DEALLOC_PD(pdn=src0_pdn, uid=0)
#       Cell 1: PROBE_DEALLOC_PD(pdn=src1_pdn, uid=src_devx_uid)
#       Cell 2: PROBE_DEALLOC_PD(pdn=src2_pdn, uid=hi_unalloc=0xfffd)
#     Each cell brackets the dealloc with PROBE_PD before/after
#     (CREATE_MKEY-acceptance test) so we can tell:
#       * pre fw_accept=1               PDC is alive on the dest
#       * dealloc op_accept=?           FW result of the dealloc
#       * post fw_accept=0 with PDN-class syndrome
#                                       PDC was actually deallocated
#       * post fw_accept=1              PDC is still alive (silent
#                                       no-op or dealloc rejected)
#
# Decoding the verdict per cell
# -----------------------------
#   pre=1, op_accept=1, post=0  -> dealloc worked. uid lane is
#                                   host-priv for DEALLOC_PD.
#   pre=1, op_accept=1, post=1  -> SILENT NO-OP confirmed. FW
#                                   ack'd the dealloc but the PDC
#                                   is still allocated.
#   pre=1, op_accept=0          -> loud reject. FW returned a
#                                   syndrome on the dealloc; uid
#                                   lane is gated.
#   pre=0                       -> PDC didn't survive LOAD or
#                                   CREATE_MKEY-acceptance under
#                                   uid=0 changed; investigate.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_dealloc_pd_matrix.sh
#
# Optional knobs (mirror test_qp_destroy_matrix.sh):
#   BLOB        Path for the SAVE blob (default /tmp/vf_dealloc_pd_matrix.blob).
#   TOOL        mlx5_vfmig CLI (default $ROOT_DIR/tools/mlx5_vfmig).
#   PROBE       fw_id_continuity_probe binary
#               (default $SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe).
#   HI_UNALLOC  uid value for the high-unalloc lane (default 0xfffd).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe}
BLOB=${BLOB:-/tmp/vf_dealloc_pd_matrix.blob}
META=${META:-${BLOB}.meta}
HI_UNALLOC=${HI_UNALLOC:-$((0xfffd))}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]  || { echo "build $TOOL first: make -C $ROOT_DIR";  exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first: make -C $ROOT_DIR"; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

WORKDIR=$(mktemp -d -t depdmtx.XXXXXX)
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

# --- helpers (copy-paste from test_qp_destroy_matrix.sh) ------------

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

start_indexed_pd_only_probe() {
    local idx=$1
    local ibdev=$2
    local prefix=$3
    local fifo_in="$WORKDIR/src_$idx.in"
    local fifo_out="$WORKDIR/src_$idx.out"

    mkfifo "$fifo_in" "$fifo_out"
    echo "=== src probe #$idx (--pd-only): $PROBE $ibdev ==="
    sudo "$PROBE" "$ibdev" --pd-only < "$fifo_in" > "$fifo_out" 2>&1 &
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

    eval "echo quit >&${fd}"
    eval "exec ${fd}>&-"
    wait "$pid" 2>/dev/null || true
    SRC_PROBE_PIDS[$idx]=""
}

# Run PROBE_PD(pdn, uid_hint=0) and capture each key=value into
# "${prefix}_<key>". Pure existence check; non-destructive.
probe_pd_existence_into() {
    local label=$1 vf=$2 pdn=$3 prefix=$4
    local out line k v
    echo "=== PROBE_PD existence ($label): vf=$vf pdn=$pdn uid_hint=0 ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_pd "$vf" "$pdn" 0 2>&1); then
        echo "$out" | sed 's/^/  /'
        echo "  PROBE_PD ioctl failed (transport/arg error)"
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

# Run PROBE_DEALLOC_PD(pdn, uid_hint) and capture each key=value
# into "${prefix}_<key>". DESTRUCTIVE on FW accept.
probe_dealloc_pd_into() {
    local label=$1 vf=$2 pdn=$3 uid_hint=$4 prefix=$5
    local out line k v
    echo "=== PROBE_DEALLOC_PD ($label): vf=$vf pdn=$pdn uid_hint=$uid_hint ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_dealloc_pd "$vf" "$pdn" "$uid_hint" 2>&1); then
        echo "$out" | sed 's/^/  /'
        echo "  PROBE_DEALLOC_PD ioctl failed (transport/arg error)"
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

echo "=== Phase B: 3 PD-only source probes ==="
sudo dmesg -C
# Enable the mlx5_ib_dbg trace in mlx5_ib_alloc_ucontext that emits the
# freshly-allocated devx_uid (vfmig_uctx_dbg). Replaces the older
# vfmig_pd_dbg pr_info that was retired when the PD gate landed.
echo 'func mlx5_ib_alloc_ucontext +p' | \
    sudo tee /sys/kernel/debug/dynamic_debug/control >/dev/null 2>&1 || true
for i in 0 1 2; do
    start_indexed_pd_only_probe "$i" "$SRC_IBDEV" "src${i}"
done

# Capture src_devx_uid from the vfmig_uctx_dbg trace. libmlx5's default
# ibv_open_device auto-allocates a DEVX uid; read its value directly from
# mlx5_ib_alloc_ucontext's debug print. Format of the dmesg line:
# "vfmig_uctx_dbg: alloc_ucontext ibdev=mlx5_X devx_uid=N adopted=0".
src_devx_uid=$(sudo dmesg | \
    grep -E "vfmig_uctx_dbg: alloc_ucontext ibdev=$SRC_IBDEV devx_uid=[0-9]+ adopted=0" | \
    tail -1 | sed -nE 's/.* devx_uid=([0-9]+) .*/\1/p')
src_devx_uid=${src_devx_uid:-0}
echo "captured: src_devx_uid=$src_devx_uid"

# --- Phase C ---------------------------------------------------------

echo "=== Phase C: SAVE_VHCA_STATE ==="
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
sudo chmod 0644 "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
[ "$SAVE_BYTES" -gt 16 ] || { echo "FAIL: blob suspiciously small: $SAVE_BYTES"; exit 1; }
echo "  save bytes: $SAVE_BYTES"

# --- Phase D ---------------------------------------------------------

echo "=== Phase D: quit 3 src probes + tear down source VF ==="
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
bind_vf_safe "$DST_VF" 60 "Phase E: dest VF bind"
sleep 1
DST_IBDEV=$(find_ib_dev_for_pci "$DST_VF") || { echo "FAIL: no ibdev for dest $DST_VF"; exit 1; }
echo "dest ibdev: $DST_IBDEV"

# --- Phase F: the 3-cell matrix --------------------------------------

echo
echo "================ Phase F: 3-cell PROBE_DEALLOC_PD matrix ================"
echo "src_devx_uid=$src_devx_uid hi_unalloc=$HI_UNALLOC"
echo "PDNs per probe slot:"
for i in 0 1 2; do
    eval "echo \"  src${i}: pdn=\$src${i}_pdn\""
done

run_dealloc_cell() {
    local cell=$1 pdn=$2 uid=$3
    local pre_prefix="${cell}_pre" mid_prefix="${cell}" post_prefix="${cell}_post"

    echo
    echo "----- Cell $cell: pdn=$pdn uid_hint=$uid -----"
    probe_pd_existence_into "${cell}_pre"  0 "$pdn" "$pre_prefix"
    probe_dealloc_pd_into   "${cell}"      0 "$pdn" "$uid"  "$mid_prefix"
    probe_pd_existence_into "${cell}_post" 0 "$pdn" "$post_prefix"
}

run_dealloc_cell "c_zero" "$src0_pdn" 0
run_dealloc_cell "c_src"  "$src1_pdn" "$src_devx_uid"
run_dealloc_cell "c_hi"   "$src2_pdn" "$HI_UNALLOC"

# --- Phase G: verdict ------------------------------------------------

echo
echo "================ Phase G: §S3b DEALLOC_PD uid matrix verdict ================"
printf "%-30s %-12s %-12s %-12s %-22s\n" \
    "uid lane" "pre_alive" "op_accept" "post_alive" "verdict"
printf "%-30s %-12s %-12s %-12s %-22s\n" \
    "------------------------------" "------------" "------------" \
    "------------" "----------------------"

decode_dealloc_cell() {
    local prefix=$1
    local pre_alive op_acc post_alive
    eval "pre_alive=\${${prefix}_pre_fw_accept:-?}"
    eval "op_acc=\${${prefix}_op_accept:-?}"
    eval "post_alive=\${${prefix}_post_fw_accept:-?}"

    if [ "$pre_alive" = 0 ]; then
        echo "?:?:?:PDC_NOT_ALIVE_PRE"
        return
    fi
    if [ "$op_acc" = 1 ] && [ "$post_alive" = 0 ]; then
        echo "$pre_alive:$op_acc:$post_alive:DEALLOCATED"
    elif [ "$op_acc" = 1 ] && [ "$post_alive" = 1 ]; then
        echo "$pre_alive:$op_acc:$post_alive:SILENT NO-OP"
    elif [ "$op_acc" = 0 ]; then
        echo "$pre_alive:$op_acc:$post_alive:LOUD REJECT"
    else
        echo "$pre_alive:$op_acc:$post_alive:?"
    fi
}

emit_row() {
    local label=$1 prefix=$2
    local r pre op post v
    r=$(decode_dealloc_cell "$prefix")
    pre=$(echo "$r" | cut -d: -f1)
    op=$(echo "$r" | cut -d: -f2)
    post=$(echo "$r" | cut -d: -f3)
    v=$(echo "$r" | cut -d: -f4)
    printf "%-30s %-12s %-12s %-12s %-22s\n" \
        "$label" "$pre" "$op" "$post" "$v"
}

emit_row "uid=0 (host-priv)"      c_zero
emit_row "uid=src_devx=$src_devx_uid" c_src
emit_row "uid=hi_unalloc=$HI_UNALLOC"  c_hi

echo
echo "Detailed forensic output:"
for cell in c_zero c_src c_hi; do
    eval "echo \"  ${cell}: op_status=\${${cell}_op_status:-?} op_syndrome=\${${cell}_op_syndrome:-?} pre_syn=\${${cell}_pre_fw_syndrome:-?} post_syn=\${${cell}_post_fw_syndrome:-?}\""
done

echo
overall_verdict="UNKNOWN"
zero_v=$(decode_dealloc_cell c_zero | cut -d: -f4)
src_v=$(decode_dealloc_cell c_src   | cut -d: -f4)
hi_v=$(decode_dealloc_cell c_hi    | cut -d: -f4)

if [ "$zero_v" = "DEALLOCATED" ]; then
    overall_verdict="uid=0 IS host-priv for DEALLOC_PD; agent's failure must come from PD dependents"
elif [ "$zero_v" = "LOUD REJECT" ] && [ "$src_v" = "DEALLOCATED" ]; then
    overall_verdict="DEALLOC_PD uid-gated; kernel-only fix: assert uid=src_devx on dealloc"
elif [ "$zero_v" = "LOUD REJECT" ] && [ "$src_v" = "LOUD REJECT" ] && [ "$hi_v" = "DEALLOCATED" ]; then
    overall_verdict="hi_unalloc is the only host-priv lane that ungates DEALLOC_PD"
elif [ "$zero_v" = "SILENT NO-OP" ]; then
    overall_verdict="DEALLOC_PD silent-no-op confirmed under uid=0 (FW ack but PDC alive)"
elif [ "$zero_v" = "LOUD REJECT" ] && [ "$src_v" = "LOUD REJECT" ] && [ "$hi_v" = "LOUD REJECT" ]; then
    overall_verdict="every uid lane rejects DEALLOC_PD on the source PDC -- LOAD did not transfer ownership"
fi
echo "Overall: $overall_verdict"

# --- manifest --------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# dealloc_pd_matrix manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_devx_uid=$src_devx_uid
hi_unalloc=$HI_UNALLOC
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV

src0_pdn=$src0_pdn
src1_pdn=$src1_pdn
src2_pdn=$src2_pdn

c_zero=$(decode_dealloc_cell c_zero)
c_src=$(decode_dealloc_cell c_src)
c_hi=$(decode_dealloc_cell c_hi)

overall_verdict=$overall_verdict
EOF
echo
echo "manifest: $META"
echo "blob: $BLOB ($SAVE_BYTES bytes)"
