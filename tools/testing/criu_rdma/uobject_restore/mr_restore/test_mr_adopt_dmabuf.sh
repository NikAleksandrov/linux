#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# gpu-dmabuf-criu-plan.md §1A/§3c/§3d empirical: SAVE_VHCA_STATE +
# LOAD_VHCA_STATE + RESTORE_MR_DMABUF end-to-end, for a GPU dma-buf
# MR. Adapted from uobject_restore/mr_adopt/test_mr_adopt.sh (the
# plain-MR analogue) -- same phase structure, single PF, sequential
# source/dest VFs, no CRIU dependency.
#
# Differences from test_mr_adopt.sh:
#   - Phase B uses gpu_dmabuf_mr_source_probe (registers a GPU dma-buf
#     MR via CUDA VMM + ibv_reg_dmabuf_mr) instead of
#     fw_id_continuity_probe (which registers a plain ib_umem-pinned
#     MR among other resources).
#   - Phase F runs mr_restore_probe_mlx5_vfmig_dmabuf against the dest
#     ibdev instead of a bare PROBE_MKEY query. That probe itself
#     performs RESTORE_PD + RESTORE_MR_DMABUF (with its OWN fresh CUDA
#     dma-buf allocation as the destination-side backing memory) plus
#     UAPI-reject/collision/dealloc subtests -- a much more complete
#     exercise of the actual restore code path (§3c relocate + §3d
#     verb) than a raw FW-state PROBE_MKEY comparison would be, since
#     it validates the full mlx5_ib_restore_mr_dmabuf flow end-to-end,
#     not just FW mkey survival.
#
# Usage:
#   sudo PF=0000:4f:00.0 ./test_mr_adopt_dmabuf.sh
#
# Optional knobs (mirror test_mr_adopt.sh):
#   BLOB      Path for the SAVE blob.
#   TOOL      mlx5_vfmig CLI (built by `make` in this directory's parent).
#   SRC_PROBE gpu_dmabuf_mr_source_probe binary.
#   DST_PROBE mr_restore_probe_mlx5_vfmig_dmabuf binary.
#   GPU_ORD   CUDA device ordinal for the source-side allocation
#             (default 0).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
SRC_PROBE=${SRC_PROBE:-$SCRIPT_DIR/gpu_dmabuf_mr_source_probe}
DST_PROBE=${DST_PROBE:-$SCRIPT_DIR/mr_restore_probe_mlx5_vfmig_dmabuf}
BLOB=${BLOB:-/tmp/vf_mr_adopt_dmabuf.blob}
META=${META:-${BLOB}.meta}
GPU_ORD=${GPU_ORD:-0}
# gpu-dmabuf-criu-3f-restore-injection-design.md Phase 3: when set,
# route Phase F through DST_PROBE's HIJACK_INJECT_TEST=1 mode (prints
# inject_* values + blocks) and drive HIJACKER against it externally,
# instead of DST_PROBE performing RESTORE_MR_DMABUF in-process.
# Default (unset) behavior is completely unchanged.
HIJACK_INJECT_TEST=${HIJACK_INJECT_TEST:-}
# Lives in a completely separate tree (~/scripts, built ad hoc on
# /opt/builds) from this file's own kernel tree -- not a relative
# path, override via env if built somewhere else.
HIJACKER=${HIJACKER:-/opt/builds/hijack_rdma_restore_mr_dmabuf}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]      || { echo "build $TOOL first: make -C $ROOT_DIR";      exit 1; }
[ -x "$SRC_PROBE" ] || { echo "build $SRC_PROBE first: make -C $ROOT_DIR"; exit 1; }
[ -x "$DST_PROBE" ] || { echo "build $DST_PROBE first: make -C $ROOT_DIR"; exit 1; }
[ -e "$CDEV" ]      || { echo "missing $CDEV (mlx5_core not loaded?)";     exit 1; }

WORKDIR=$(mktemp -d -t mradoptdb.XXXXXX)
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

# --- helpers (copy-paste from test_mr_adopt.sh) ----------------------

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

# Start gpu_dmabuf_mr_source_probe in the background hooked to FIFOs.
# Drains READY then captures key=value lines into "src_<key>".
start_src_probe() {
    local ibdev=$1
    local fifo_in="$WORKDIR/src.in"
    local fifo_out="$WORKDIR/src.out"

    mkfifo "$fifo_in" "$fifo_out"
    echo "=== source probe: $SRC_PROBE $ibdev --gpu $GPU_ORD ==="
    "$SRC_PROBE" "$ibdev" --gpu "$GPU_ORD" < "$fifo_in" > "$fifo_out" 2>&1 &
    SRC_PROBE_PID=$!
    exec 7> "$fifo_in"

    local line
    while IFS= read -r line < "$fifo_out"; do
        echo "  [src probe] $line"
        case "$line" in
            READY) return 0 ;;
            *=*)
                local k="${line%%=*}"
                local v="${line#*=}"
                eval "src_${k}=\"\$v\""
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

# --- Phase A -----------------------------------------------------------

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

# --- Phase B -------------------------------------------------------------

echo "=== Phase B: source probe (registers GPU dma-buf MR, holds it across SAVE) ==="
sudo dmesg -C 2>/dev/null || true
start_src_probe "$SRC_IBDEV"
echo "captured: src_pdn=$src_pdn"
echo "captured: src_mkey_index=$src_mkey_index"
echo "captured: src_lkey=$src_lkey src_rkey=$src_rkey"
echo "captured: src_mr_length=$src_mr_length"
echo "captured: src_access_flags=$src_access_flags"

# --- Phase C -------------------------------------------------------------

echo "=== Phase C: SAVE_VHCA_STATE ==="
sudo dmesg -C 2>/dev/null || true
sudo "$TOOL" "$PF" suspend_vhca 0 initiator
sudo "$TOOL" "$PF" suspend_vhca 0 responder
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" resume_vhca 0 responder
sudo "$TOOL" "$PF" resume_vhca 0 initiator
sudo chmod 0644 "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
[ "$SAVE_BYTES" -gt 16 ] || { echo "FAIL: blob suspiciously small: $SAVE_BYTES"; exit 1; }
echo "blob size: $SAVE_BYTES bytes"

# --- Phase D -------------------------------------------------------------

echo "=== Phase D: quit source probe + tear down source VF ==="
quit_src_probe
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null

# --- Phase E -------------------------------------------------------------

echo "=== Phase E: fresh DEST VF + LOAD + bind ==="
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
wait_for_path "$(vf_path $PF)/virtfn0"
DST_VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "dest VF: $DST_VF"

sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0

sudo dmesg -C 2>/dev/null || true
sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" mark_restored 0

echo mlx5_core | sudo tee "$(vf_path $DST_VF)/driver_override" >/dev/null
bind_vf_safe "$DST_VF" 60 "Phase E: dest VF bind"
sleep 1
DST_IBDEV=$(find_ib_dev_for_pci "$DST_VF") || { echo "FAIL: no ibdev for dest $DST_VF"; exit 1; }
echo "dest ibdev: $DST_IBDEV"

# --- Phase F: THE TEST -----------------------------------------------

echo
echo "================ Phase F: RESTORE_MR_DMABUF end-to-end ============"
echo "Running mr_restore_probe_mlx5_vfmig_dmabuf against $DST_IBDEV with"
echo "the captured source identity. This exercises: RESTORE_PD adoption,"
echo "gate/UAPI-reject subtests, the happy-path RESTORE_MR_DMABUF (fresh"
echo "CUDA dma-buf import -> mlx5_vfmig_relocate_dmabuf_mr -> mkey"
echo "adoption), collision detection, and (after quit) v0 dealloc"
echo "semantics."
echo

sudo dmesg -C 2>/dev/null || true
set +e
if [ -n "$HIJACK_INJECT_TEST" ]; then
    [ -x "$HIJACKER" ] || { echo "FAIL: HIJACKER not found/executable: $HIJACKER"; exit 1; }
    echo "=== Phase F (injection mode): DST_PROBE will block for external RESTORE_MR_DMABUF ==="

    dfifo_in="$WORKDIR/dst.in"
    dfifo_out="$WORKDIR/dst.out"
    mkfifo "$dfifo_in" "$dfifo_out"
    sudo env HIJACK_INJECT_TEST=1 "$DST_PROBE" "$DST_IBDEV" "$src_pdn" "$src_mkey_index" \
        "$src_lkey" "$src_mr_length" "$src_access_flags" 0x4242 0x4241 "$GPU_ORD" \
        < "$dfifo_in" > "$dfifo_out" 2>&1 &
    DST_PROBE_PID=$!
    exec 8> "$dfifo_in"

    line=""
    while IFS= read -r line < "$dfifo_out"; do
        echo "  [dst probe] $line"
        case "$line" in
            INJECT_READY) break ;;
            inject_*=*)
                k="${line%%=*}"
                v="${line#*=}"
                eval "dst_${k}=\"\$v\""
                ;;
        esac
    done
    if [ "$line" != "INJECT_READY" ]; then
        echo "FAIL: DST_PROBE never printed INJECT_READY"
        exec 8>&-
        wait "$DST_PROBE_PID" 2>/dev/null || true
        exit 1
    fi

    echo "=== running external hijacker against pid $DST_PROBE_PID ==="
    sudo "$HIJACKER" "$DST_PROBE_PID" "$dst_inject_fd" "$dst_inject_mr_target_handle" \
        "$dst_inject_pd_target_handle" "$dst_inject_dmabuf_fd" "$dst_inject_offset" \
        "$dst_inject_length" "$dst_inject_iova" "$dst_inject_access_flags" \
        "$dst_inject_lkey_hint" "$dst_inject_rkey_hint" "$dst_inject_mkey_index"
    HIJACKER_RC=$?
    echo "hijacker rc=$HIJACKER_RC"

    echo "go" >&8

    while IFS= read -r line < "$dfifo_out"; do
        echo "  [dst probe] $line"
        [ "$line" = "READY" ] && break
    done

    echo "quit" >&8
    exec 8>&-
    wait "$DST_PROBE_PID"
    DST_RC=$?
    if [ "$HIJACKER_RC" -ne 0 ]; then
        DST_RC=1
    fi
else
    sudo "$DST_PROBE" "$DST_IBDEV" "$src_pdn" "$src_mkey_index" "$src_lkey" \
        "$src_mr_length" "$src_access_flags" 0x4242 0x4241 "$GPU_ORD" \
        <<< "quit"
    DST_RC=$?
fi
set -e

overall_rc=$DST_RC

# --- manifest ----------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# mr_adopt_dmabuf manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_pdn=$src_pdn
src_mkey_index=$src_mkey_index
src_lkey=$src_lkey
src_rkey=$src_rkey
src_mr_length=$src_mr_length
src_access_flags=$src_access_flags
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV
dst_probe_rc=$DST_RC
EOF

echo
echo "manifest: $META"
echo "blob:     $BLOB"
if [ "$overall_rc" -eq 0 ]; then
    echo "================ MR ADOPT DMABUF VERDICT: PASS ==================="
else
    echo "================ MR ADOPT DMABUF VERDICT: FAIL (rc=$overall_rc) ==="
fi
exit "$overall_rc"
