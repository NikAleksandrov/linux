#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S3b PDN allocator high-water probe (v3 of the dealloc-PD investigation).
#
# Question:
#   After LOAD_VHCA_STATE of a "PD + CQ + QP" source on the dest VF
#   -- the shape where DEALLOC_PD(src_pdn, uid=any) returns
#   syndrome 0xef0c8a ("PDN unknown to allocator") -- what PDNs does
#   the dest's PDN allocator hand out for FRESH ALLOC_PD calls?
#
#   * If FW returns pdns STRICTLY GREATER than the source's max_pdn,
#     the high-water mark IS advanced; the source's PDN slots are
#     reserved (just not deallocable), and the leak from a "skip
#     DEALLOC_PD on syndrome 0xef0c8a" workaround is bounded by the
#     count of source-restored PDs that get dealloc'd. Tolerable.
#
#   * If FW returns the SAME pdn as the source's max_pdn (or any
#     pdn the source already used), the high-water is NOT advanced;
#     the source PDN is fully unknown to the dest allocator and
#     would be a fresh allocation slot. That would invalidate the
#     "PDN slots are reserved on the destination" premise that the
#     mlx5_ib_restore_pd Model A comment is built on, and would
#     mean v1 (PD-only) DEALLOC success was just coincidence.
#
# This is option-3 in the user's reply ("verify_leak_shape").
#
# Method:
#   - Source provisions 3 full-shape probes -> source pdns 20,21,22
#     (deterministic given a fresh VF). Hold them.
#   - SAVE_VHCA_STATE on source.
#   - LOAD on fresh dest VF, bind, mark_restored.
#   - On the dest, run a SHORT-LIVED probe that allocates 8 fresh
#     PDs back-to-back via ibv_alloc_pd and prints each pdn. The
#     probe doesn't go through any RESTORE_PD path -- it's the
#     vanilla mlx5_ib_alloc_pd code path issuing ALLOC_PD against
#     the dest VF's cmdif.
#   - Decode: if the first dest pdn is > 22, high-water advanced;
#     if first dest pdn <= 22 and overlaps {20,21,22}, NOT advanced.
#
# Usage: sudo PF=0000:08:00.0 ./test_pdn_highwater.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe}
BLOB=${BLOB:-/tmp/vf_pdn_highwater.blob}

[ -x "$TOOL" ]  || { echo "build $TOOL first"; exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first"; exit 1; }
[ -e "/dev/mlx5_vfmig/$PF" ] || { echo "missing /dev/mlx5_vfmig/$PF"; exit 1; }

WORKDIR=$(mktemp -d -t pdnhw.XXXXXX)
declare -a PIDS=()
cleanup() {
    local i pid
    for i in "${!PIDS[@]}"; do
        pid=${PIDS[$i]}
        [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && echo "quit" >&"$((10+i))" 2>/dev/null
        wait "$pid" 2>/dev/null || true
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
        if [ -e "$(vf_path $vf)/driver" ]; then wait "$bg" 2>/dev/null || true; return 0; fi
        if ! kill -0 "$bg" 2>/dev/null; then sleep 0.2; [ -e "$(vf_path $vf)/driver" ] && return 0; return 1; fi
        if [ $(( $(date +%s) - started )) -ge "$timeout" ]; then return 2; fi
        sleep 0.5
    done
}

find_ib() {
    local bdf=$1 d
    for d in /sys/class/infiniband/*; do
        [ "$(basename "$(readlink $d/device)")" = "$bdf" ] && basename "$d" && return 0
    done
    return 1
}

start_probe() {
    local idx=$1
    local ibdev=$2
    local prefix=$3
    local extra="${4:-}"
    local fifo_in="$WORKDIR/$idx.in"
    local fifo_out="$WORKDIR/$idx.out"
    mkfifo "$fifo_in" "$fifo_out"
    sudo "$PROBE" "$ibdev" $extra < "$fifo_in" > "$fifo_out" 2>&1 &
    local pid=$!
    PIDS[$idx]="$pid"
    eval "exec $((10 + idx))> \"\$fifo_in\""
    local line
    while IFS= read -r line < "$fifo_out"; do
        echo "  [$prefix] $line"
        case "$line" in
            READY) return 0 ;;
            *=*) eval "${prefix}_${line%%=*}=\"${line#*=}\"" ;;
        esac
    done
    return 1
}

quit_probe() {
    local idx=$1
    local fd=$((10 + idx))
    local pid=${PIDS[$idx]}
    eval "echo quit >&${fd}"
    eval "exec ${fd}>&-"
    wait "$pid" 2>/dev/null || true
    PIDS[$idx]=""
}

# ---------- Phase A: source VF + 3 source probes ----------

echo "=== Phase A: source VF + 3 source probes (full PD+CQ+QP) ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
wait_for_path "$(vf_path $PF)/virtfn0"
SRC_VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
sudo "$TOOL" "$PF" set_tracked 0 1 >/dev/null
sudo "$TOOL" "$PF" enable_migratable 0 >/dev/null
echo mlx5_core | sudo tee "$(vf_path $SRC_VF)/driver_override" >/dev/null
bind_vf_safe "$SRC_VF" 60 srcbind
sleep 1
SRC_IB=$(find_ib "$SRC_VF") || { echo "no ibdev"; exit 1; }
echo "src ibdev: $SRC_IB"

sudo dmesg -C
for i in 0 1 2; do
    start_probe "$i" "$SRC_IB" "src${i}"
done

src_max_pdn=0
for i in 0 1 2; do
    eval "p=\$src${i}_pdn"
    if [ "$p" -gt "$src_max_pdn" ]; then src_max_pdn=$p; fi
done
echo "src pdns: src0=$src0_pdn src1=$src1_pdn src2=$src2_pdn (max=$src_max_pdn)"

# ---------- Phase B: SAVE + tear down source ----------

echo "=== Phase B: SAVE + tear down source ==="
# snapshot-ordering: pause datapath (CRIU CHECKPOINT_DEVICES), then
# capture (SAVE is suspend-aware and skips its own suspend), then resume.
sudo "$TOOL" "$PF" suspend_vhca 0
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" resume_vhca 0
sudo chmod 0644 "$BLOB"
echo "  save bytes: $(stat -c %s "$BLOB")"
for i in 0 1 2; do quit_probe "$i"; done
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null

# ---------- Phase C: dest VF + LOAD + bind ----------

echo "=== Phase C: dest VF + LOAD + bind ==="
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
wait_for_path "$(vf_path $PF)/virtfn0"
DST_VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
sudo "$TOOL" "$PF" set_tracked 0 1 >/dev/null
sudo "$TOOL" "$PF" enable_migratable 0 >/dev/null
sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" mark_restored 0
echo mlx5_core | sudo tee "$(vf_path $DST_VF)/driver_override" >/dev/null
bind_vf_safe "$DST_VF" 60 dstbind
sleep 1
DST_IB=$(find_ib "$DST_VF") || { echo "no ibdev"; exit 1; }
echo "dst ibdev: $DST_IB"

# ---------- Phase D: spin up 8 short-lived probes on dest, capture each new pdn ----------

echo
echo "=== Phase D: 8 fresh ALLOC_PD calls on dest (vanilla path, no RESTORE_PD) ==="
echo "src pdn range: {$src0_pdn, $src1_pdn, $src2_pdn} max=$src_max_pdn"
echo
declare -a DST_PDNS=()
for i in 0 1 2 3 4 5 6 7; do
    PROBE_IDX=$((10 + i))
    fifo_in="$WORKDIR/d$i.in"
    fifo_out="$WORKDIR/d$i.out"
    mkfifo "$fifo_in" "$fifo_out"
    sudo "$PROBE" "$DST_IB" --pd-only < "$fifo_in" > "$fifo_out" 2>&1 &
    pid=$!
    eval "exec ${PROBE_IDX}> \"\$fifo_in\""
    pdn_seen=""
    while IFS= read -r line < "$fifo_out"; do
        case "$line" in
            pdn=*) pdn_seen="${line#pdn=}" ;;
            READY) break ;;
        esac
    done
    DST_PDNS+=("$pdn_seen")
    echo "  dst alloc #$i: pdn=$pdn_seen"
    eval "echo quit >&${PROBE_IDX}"
    eval "exec ${PROBE_IDX}>&-"
    wait "$pid" 2>/dev/null || true
done

# ---------- Phase E: verdict ----------

echo
echo "=== Verdict ==="
echo "src pdns:   {$src0_pdn, $src1_pdn, $src2_pdn} (max=$src_max_pdn)"
echo "dst pdns:   ${DST_PDNS[*]}"

dst_min_pdn=${DST_PDNS[0]}
for p in "${DST_PDNS[@]}"; do
    if [ "$p" -lt "$dst_min_pdn" ]; then dst_min_pdn=$p; fi
done
echo "dst min:    $dst_min_pdn"

reused=0
for p in "${DST_PDNS[@]}"; do
    case "$p" in
        $src0_pdn|$src1_pdn|$src2_pdn) reused=1 ;;
    esac
done

if [ "$reused" = "1" ]; then
    echo
    echo "VERDICT: ** dest allocator REUSED a source pdn **"
    echo "  -> high-water NOT advanced; PDN registry not preserved through SAVE/LOAD."
    echo "  -> mlx5_ib_restore_pd's 'PDN reserved on dest' premise is FALSE for this shape."
    echo "  -> Heads-up: any active QPC.pd referencing a reused pdn would now collide"
    echo "     with the freshly-allocated PD's owner -- traffic-affecting if so."
elif [ "$dst_min_pdn" -gt "$src_max_pdn" ]; then
    echo
    echo "VERDICT: ** dest allocator advanced past source max **"
    echo "  -> high-water IS advanced; source pdns reserved (no collision risk)."
    echo "  -> 'skip DEALLOC_PD on syndrome 0xef0c8a' leak is bounded per-restore,"
    echo "     not per-allocate-cycle."
else
    echo
    echo "VERDICT: ** mixed / inconclusive **"
    echo "  dst pdns are below src_max_pdn=$src_max_pdn but did not collide with src set."
fi
