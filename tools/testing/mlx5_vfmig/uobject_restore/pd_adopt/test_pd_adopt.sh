#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S3b empirical: validate that Model A for mlx5_ib_restore_pd is
# sound for the v0 critical path (non-DEVX libibverbs ucontexts).
#
# Why we care
# -----------
# mlx5_ib_restore_pd has two architectural shapes:
#
#   Model A: just build a kernel-side mlx5_ib_pd with
#            (pdn=src_pdn, uid=src_uid) on the destination; no FW
#            round-trip, no FW patch.
#
#   Model B: add a new FW command "alloc PD with id hint" and call
#            it from mlx5_ib_restore_pd. Robust but requires FW work.
#
# For Model A to be sound we need two things to be true after
# LOAD_VHCA_STATE on the destination:
#
#   (i)  The FW's pdn allocator high-water mark survived LOAD --
#        i.e. a fresh ALLOC_PD on the destination returns a value
#        STRICTLY GREATER than the source's pdn, so it cannot
#        collide with the pdn we're trying to adopt.
#        K6 already established this for {PD, CQ, QP, MKEY} via
#        uobject_restore/fw_id_continuity/. PARTIAL PASS, recorded.
#
#   (ii) Subsequent FW ops referencing the adopted pdn under the
#        same uid succeed. mlx5_ib_alloc_pd() sets
#        pd->uid = context->devx_uid, and every uid-scoped op
#        (CREATE_MKEY/QP/TIS/TIR/RQ/SQ/DCT/SRQ) reads its uid
#        from to_mpd(pd)->uid -- so for the v0 critical path
#        (non-DEVX libibverbs apps -- mlx5_ib_devx_create only
#        fires when MLX5_IB_ALLOC_UCTX_DEVX is requested,
#        otherwise devx_uid stays 0), ALL of mlx5's downstream
#        FW commands run under uid=0.
#
# This script tests (ii) directly via the new MLX5_VFMIG_IOC_PROBE_PD
# ioctl: a one-shot CREATE_MKEY(uid=0, pd=src_pdn) on the
# destination VF's mdev, followed by DESTROY_MKEY.
#
# Empirical finding (2026-05-14, mlx5 FW 28.48.1000)
# --------------------------------------------------
# Both the positive case (pd=src_pdn) and the negative control
# (pd=BOGUS_PDN well outside any plausible allocator high-water
# mark) returned fw_syndrome=0. That is: mlx5 firmware does NOT
# gate CREATE_MKEY's mkc.pd field under uid=0. Kernel-uid is
# privileged and mlx5 treats the pd index as metadata rather
# than an existence check.
#
# This is not a failure of Model A -- it is the absence of a gate
# to fail. Combined with (i) K6's PARTIAL PASS we have:
#
#   * K6:  src_pdn's slot is reserved in FW post-LOAD (otherwise
#          fresh ALLOC_PD on dst would have handed it back).
#   * This test:  uid=0 ops accept src_pdn (and any other pdn)
#          freely; no FW gate to satisfy.
#   * mlx5_ib code: non-DEVX => devx_uid=0 => every downstream
#          op uses to_mpd(pd)->uid = 0 = the case proven above.
#
# Therefore Model A is sound for the v0 critical path
# (non-DEVX libibverbs apps -- the common CRIU restore target).
# DEVX-aware applications (mlx5dv_*, devx_obj_create, etc.) are
# orthogonal and remain unprobed; they will need a separate
# DEVX-aware test that opens a ucontext with
# MLX5_IB_ALLOC_UCTX_DEVX, captures the resulting devx_uid via
# PROBE_UID delta, and re-issues PROBE_PD with that uid.
#
# Verdict matrix
# --------------
#
#    positive | negative | meaning
#    ---------+----------+--------------------------------------------
#       1     |    0     | STRONG PASS: FW gates kernel-uid on pdn
#                          validity AND src_pdn survived LOAD. Would
#                          surprise me -- uid=0 is usually ungated.
#                          Model A confirmed.
#       1     |    1     | WEAK PASS: FW doesn't gate kernel-uid on
#                          pdn validity at all (this is what we see
#                          today on mlx5 FW 28.48.1000). Model A is
#                          trivially sound for the v0 critical path;
#                          DEVX needs separate validation.
#       0     |    *     | HARD FAIL: positive rejected -- src_pdn is
#                          not usable under uid=0 on the destination.
#                          Inspect fw_syndrome + dmesg; Model A is
#                          blocked, revisit §S3b plan.
#
# Flow
# ----
#
#   Phase A  Provision source VF, set_tracked + enable_migratable,
#            bind.
#   Phase B  Fork fw_id_continuity_probe on the source ibdev; it
#            allocates one of {PD, CQ, QP, MR, SRQ}. Capture
#            src_pdn from its READY-line dictionary. Probe stays
#            alive, holding the resources through SAVE.
#   Phase C  SAVE_VHCA_STATE on the source.
#   Phase D  Quit the source probe; tear down the source VF.
#   Phase E  Provision a fresh destination VF: set_tracked,
#            enable_migratable, LOAD_VHCA_STATE, mark_restored,
#            bind.
#   Phase F  ===== THE TEST =====
#            Issue PROBE_PD(dst_vf, src_pdn, uid=0) and read
#            fw_accept=. Expected: fw_accept=1 with fw_syndrome=0.
#   Phase G  Negative controls. PROBE_PD with bogus pdn must
#            return fw_accept=0 (FW rejects with "invalid PD"
#            syndrome). Establishes that PROBE_PD is actually
#            doing the gating we think it is, rather than
#            unconditionally accepting.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_pd_adopt.sh
#
# Optional knobs (mirroring test_fw_id_continuity.sh):
#   BLOB                Path for the SAVE blob (default /tmp/vf_pd_adopt.blob).
#   TOOL                mlx5_vfmig CLI (default $ROOT_DIR/tools/mlx5_vfmig).
#   PROBE               fw_id_continuity_probe binary
#                       (default $SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe).
#   BOGUS_PDN           pdn value to use for the negative control.
#                       Default 0xffff00 -- well above anything a
#                       fresh VHCA's allocator will reach during
#                       this short-lived test.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe}
BLOB=${BLOB:-/tmp/vf_pd_adopt.blob}
META=${META:-${BLOB}.meta}
BOGUS_PDN=${BOGUS_PDN:-$((0xffff00))}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]  || { echo "build $TOOL first: make -C $ROOT_DIR";  exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first: make -C $ROOT_DIR"; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

WORKDIR=$(mktemp -d -t pdadopt.XXXXXX)
cleanup() {
    if [ -n "${SRC_PROBE_PID:-}" ] && kill -0 "$SRC_PROBE_PID" 2>/dev/null; then
        echo "quit" > "$WORKDIR/src.in" 2>/dev/null || true
        wait "$SRC_PROBE_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

# --- helpers (copy-paste from test_fw_id_continuity.sh; intentional
#     since there's no common.sh today) ------------------------------

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

# Start fw_id_continuity_probe in the background hooked to FIFOs.
# Drains READY then captures key=value lines into "${prefix}_<key>".
start_src_probe() {
    local ibdev=$1
    local fifo_in="$WORKDIR/src.in"
    local fifo_out="$WORKDIR/src.out"

    mkfifo "$fifo_in" "$fifo_out"
    echo "=== source probe: $PROBE $ibdev ==="
    sudo "$PROBE" "$ibdev" < "$fifo_in" > "$fifo_out" 2>&1 &
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

# Run `mlx5_vfmig $PF probe_pd $vf $pdn $uid_hint`, capture each
# key=value into "$prefix_<key>" variables in the caller's scope.
probe_pd_into() {
    local label=$1 vf=$2 pdn=$3 uid_hint=$4 prefix=$5
    local out line k v
    echo "=== PROBE_PD ($label): vf=$vf pdn=$pdn uid_hint=$uid_hint ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_pd "$vf" "$pdn" "$uid_hint" 2>&1); then
        echo "$out" | sed 's/^/  /'
        echo "  PROBE_PD ioctl failed (transport / arg error)"
        return 1
    fi
    while IFS= read -r line; do
        echo "  [$label probe_pd] $line"
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

echo "=== Phase B: source probe (allocates PD, holds it across SAVE) ==="
start_src_probe "$SRC_IBDEV"
echo "captured: src_pdn=$src_pdn (FW pdn from mlx5dv_pd; uid scope = 0 since libibverbs default ucontext has no DEVX)"

# --- Phase C ---------------------------------------------------------

echo "=== Phase C: SAVE_VHCA_STATE ==="
sudo dmesg -C
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
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
echo "================ Phase F: PROBE_PD =================="
echo "Question: does the FW accept CREATE_MKEY(uid=0, pd=src_pdn) on"
echo "the destination, indicating the source PD survives LOAD?"
probe_pd_into positive 0 "$src_pdn" 0 pos

# --- Phase G: negative controls -------------------------------------

echo
echo "================ Phase G: NEGATIVE CONTROL =========="
echo "Question: does PROBE_PD correctly REJECT a bogus pdn? (If yes,"
echo "the positive result above wasn't just FW accepting everything.)"
probe_pd_into bogus 0 "$BOGUS_PDN" 0 neg

# --- verdict --------------------------------------------------------

overall_rc=0

echo
echo "================ PD ADOPT VERDICT ==================="
echo "  src_pdn          = $src_pdn"
echo "  bogus_pdn        = $BOGUS_PDN"
echo
echo "  positive fw_accept   = ${pos_fw_accept:-?}  syndrome=${pos_fw_syndrome:-?}"
echo "  negative fw_accept   = ${neg_fw_accept:-?}  syndrome=${neg_fw_syndrome:-?}"
echo

if [ "${pos_fw_accept:-0}" = 1 ] && [ "${neg_fw_accept:-1}" = 0 ]; then
    echo "  STRONG PASS  FW gates kernel-uid CREATE_MKEY on pdn validity AND"
    echo "               the source pdn survived LOAD_VHCA_STATE. Model A is"
    echo "               directly confirmed: mlx5_ib_restore_pd can adopt"
    echo "               (src_pdn, src_uid) into a fresh mlx5_ib_pd wrapper"
    echo "               without any FW round-trip."
elif [ "${pos_fw_accept:-0}" = 1 ] && [ "${neg_fw_accept:-1}" = 1 ]; then
    echo "  WEAK PASS    FW does NOT gate kernel-uid CREATE_MKEY on pdn"
    echo "               validity -- mkc.pd is just metadata under uid=0,"
    echo "               not an existence-checked reference. This is the"
    echo "               absence of a gate to fail, not a failure of Model A:"
    echo
    echo "                 - K6 (uobject_restore/fw_id_continuity/) already"
    echo "                   established that LOAD preserves the FW pdn"
    echo "                   allocator (fresh ALLOC_PD on dst > src_pdn), so"
    echo "                   src_pdn is reserved in FW post-LOAD."
    echo "                 - mlx5_ib non-DEVX path always runs under uid=0,"
    echo "                   so every downstream FW op (CREATE_MKEY / QP /"
    echo "                   TIS / TIR / RQ / SQ / DCT / SRQ) is ungated."
    echo
    echo "               Therefore Model A is trivially sound for the v0"
    echo "               critical path (non-DEVX libibverbs applications)."
    echo "               DEVX-aware apps remain unprobed; revisit with a"
    echo "               PROBE_UID-delta variant before any DEVX restore"
    echo "               work."
elif [ "${pos_fw_accept:-0}" = 0 ]; then
    echo "  HARD FAIL    positive REJECTED -- the source pdn is not usable"
    echo "               on the destination under uid=0. Model A would not"
    echo "               work for the v0 critical path. Inspect"
    echo "               fw_syndrome=${pos_fw_syndrome:-?} and dmesg; the"
    echo "               §S3b plan must be revisited."
    overall_rc=1
else
    echo "  UNEXPECTED   positive=${pos_fw_accept:-?}, negative=${neg_fw_accept:-?} --"
    echo "               this combination shouldn't be reachable. Inspect."
    overall_rc=1
fi
echo "====================================================="

# --- manifest -------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# pd_adopt manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_pdn=$src_pdn
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV
positive_fw_accept=${pos_fw_accept:-?}
positive_fw_syndrome=${pos_fw_syndrome:-?}
negative_fw_accept=${neg_fw_accept:-?}
negative_fw_syndrome=${neg_fw_syndrome:-?}
bogus_pdn=$BOGUS_PDN
overall_rc=$overall_rc
EOF
sudo chmod 0644 "$META"
echo "wrote $META"

exit "$overall_rc"
