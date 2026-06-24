#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S3b end-to-end: validate UVERBS_METHOD_RESTORE_PD + mlx5_ib_restore_pd
# against a real save/load cycle.
#
# This is the driver-end-to-end follow-on to test_pd_adopt.sh:
#
#   test_pd_adopt.sh    Drives the empirical "can a destination FW
#                       command reference the source's pdn?" question
#                       via the PF cdev's MLX5_VFMIG_IOC_PROBE_PD
#                       ioctl. Validates Model A's *underlying
#                       firmware behaviour*. Does NOT exercise the
#                       new kernel verb path.
#
#   this script         Validates the actual landed verb path:
#                       opens a destination ucontext with
#                       MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE, issues
#                       UVERBS_METHOD_RESTORE_PD with a UHW payload
#                       carrying the source pdn, and confirms via
#                       INFO_HANDLES that the adopted PD landed at
#                       the caller-chosen ufile handle. Also folds
#                       in the test_pd_adopt.sh FW-level check by
#                       running MLX5_VFMIG_IOC_PROBE_PD against the
#                       adopted pdn while the PD is still alive.
#
# Flow
# ----
#
#   Phase A  Provision source VF, set_tracked + enable_migratable, bind.
#   Phase B  Fork fw_id_continuity_probe on the source ibdev to
#            allocate one of each {PD, CQ, QP, MR, SRQ}. Capture
#            src_pdn from its READY-line dictionary. Probe stays
#            alive, holding the resources across SAVE.
#   Phase C  SAVE_VHCA_STATE on the source.
#   Phase D  Quit the source probe; tear down the source VF.
#   Phase E  Provision a fresh destination VF: set_tracked,
#            enable_migratable, LOAD_VHCA_STATE, mark_restored, bind.
#   Phase F  ===== THE TEST =====
#            Fork pd_restore_probe_mlx5_vfmig in the background
#            against the destination ibdev with the captured
#            src_pdn. Wait for "READY" -- at that point subtests
#            1-5 (gate, UAPI rejection x2, happy path, collision)
#            have all run and the adopted PD is alive at
#            target_handle on the probe's ucontext.
#   Phase G  While the PD is alive, run MLX5_VFMIG_IOC_PROBE_PD
#            (uid=0) against the adopted pdn to confirm FW
#            actually accepts CREATE_MKEY referencing it. This is
#            the same check test_pd_adopt.sh runs; folding it in
#            here means a single integrated test covers both the
#            ABI + FW dimensions.
#   Phase H  Tell the probe to "quit", which triggers subtest 7
#            (v0 dealloc semantics, post-gate: DEALLOC_PD on the
#            adopted PD must succeed (return 0) because the
#            mlx5_ib_dealloc_pd gate suppresses the expected FW
#            failure -- status=0x9 syndrome=0xef0c8a, "PDN unknown
#            to allocator", caused by LOAD_VHCA_STATE not preserving
#            the (pdn -> owner_uid) registration table. INFO_HANDLES
#            must NOT report the handle afterwards (uobj freed). See
#            tools/testing/criu_rdma/design/pd_registration_wipe.md
#            for the full empirical investigation that revised this
#            subtest's expected behaviour.). Wait for the probe to
#            exit 0.
#   Phase I  Verdict + manifest.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_pd_restore_mlx5_vfmig.sh
#
# Optional knobs:
#   BLOB                Path for the SAVE blob.
#   TOOL                mlx5_vfmig CLI.
#   SRC_PROBE           fw_id_continuity_probe binary.
#   DST_PROBE           pd_restore_probe_mlx5_vfmig binary.
#   TARGET_HANDLE       ufile handle to install the adopted PD at on
#                       the destination ucontext (default 0x4242).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
SRC_PROBE=${SRC_PROBE:-$ROOT_DIR/uobject_restore/fw_id_continuity/fw_id_continuity_probe}
DST_PROBE=${DST_PROBE:-$ROOT_DIR/uobject_restore/pd_restore/pd_restore_probe_mlx5_vfmig}
BLOB=${BLOB:-/tmp/vf_pd_restore.blob}
META=${META:-${BLOB}.meta}
TARGET_HANDLE=${TARGET_HANDLE:-0x4242}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]       || { echo "build $TOOL first";       exit 1; }
[ -x "$SRC_PROBE" ]  || { echo "build $SRC_PROBE first";  exit 1; }
[ -x "$DST_PROBE" ]  || { echo "build $DST_PROBE first";  exit 1; }
[ -e "$CDEV" ]       || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

WORKDIR=$(mktemp -d -t pdrestore.XXXXXX)
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

# --- helpers (copy-paste from test_pd_adopt.sh -- no common.sh today) ---

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

# Fork the dst probe in the background; capture both the verdict
# stream (so the user sees the subtest log) and any key=value lines
# emitted between subtests + READY.
start_dst_probe() {
    local ibdev=$1 pdn=$2 target=$3
    local fifo_in="$WORKDIR/dst.in"
    local fifo_out="$WORKDIR/dst.out"

    mkfifo "$fifo_in" "$fifo_out"
    echo "=== dst probe: $DST_PROBE $ibdev $pdn $target ==="
    sudo "$DST_PROBE" "$ibdev" "$pdn" "$target" \
        < "$fifo_in" > "$fifo_out" 2>&1 &
    DST_PROBE_PID=$!
    exec 8> "$fifo_in"

    local line k v
    while IFS= read -r line < "$fifo_out"; do
        echo "  [dst probe] $line"
        case "$line" in
            READY) return 0 ;;
            "  FAIL"*|" FAIL"*|"FAIL"*)
                # Surface a pre-READY failure so the verdict
                # knows to fail. Must match BEFORE the key=value
                # pattern so we don't eval "FAIL RESTORE_PD(target=..."
                # as a shell assignment.
                dst_pre_ready_fail=1
                ;;
            *)
                # Strict "ident=value" shape only: eval anything
                # else is a footgun (subtest log lines like
                # "FAIL RESTORE_PD(target=0x4242)" contain '=').
                if [[ "$line" =~ ^[a-zA-Z_][a-zA-Z0-9_]*= ]]; then
                    k="${line%%=*}"
                    v="${line#*=}"
                    eval "dst_${k}=\"\$v\""
                fi
                ;;
        esac
    done
    echo "  [dst probe] EOF before READY -- probe failed (subtests 1-5)"
    return 1
}

quit_dst_probe() {
    echo "quit" >&8; exec 8>&-
    # Drain the rest of the probe's output so subtest 7 lines reach the
    # user. The probe will eventually close stdout when it exits.
    while IFS= read -r line < "$WORKDIR/dst.out"; do
        echo "  [dst probe] $line"
        case "$line" in
            "  FAIL"*) dst_post_quit_fail=1 ;;
            "pd_restore_probe_mlx5_vfmig: PASS"*) dst_overall_pass=1 ;;
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
echo "captured: src_pdn=$src_pdn (FW pdn from mlx5dv_pd; uid scope = 0)"

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

# --- Phase F: THE TEST ----------------------------------------------

echo
echo "================ Phase F: RESTORE_PD ================="
echo "Drive UVERBS_METHOD_RESTORE_PD on $DST_IBDEV with the src pdn."
echo "The probe runs subtests 1-5 (gate, UAPI rejection x2, happy"
echo "path, collision) and then parks at READY while the adopted"
echo "PD is alive at handle $TARGET_HANDLE."
if ! start_dst_probe "$DST_IBDEV" "$src_pdn" "$TARGET_HANDLE"; then
    echo "FAIL: dst probe did not reach READY"
    exit 1
fi

# --- Phase G: cross-check FW liveness of the adopted pdn ------------

echo
echo "================ Phase G: FW liveness check =========="
echo "While the adopted PD is alive, run MLX5_VFMIG_IOC_PROBE_PD(uid=0)"
echo "against the same pdn. fw_accept=1 means CREATE_MKEY referencing"
echo "the adopted pdn succeeds on the destination VF -- the FW side"
echo "of Model A is sound."
probe_pd_into adopted 0 "$src_pdn" 0 pos

# --- Phase H: destroy round-trip ------------------------------------

echo
echo "================ Phase H: v0 dealloc semantics ======="
echo "Tell the probe to quit; it will run subtest 7."
echo
echo "DEALLOC_PD on a vfmig_restored PD will fail at FW with the"
echo "expected status=0x9 syndrome=0xef0c8a (PDN unknown to allocator,"
echo "because LOAD_VHCA_STATE preserves the per-VHCA PDN-allocator"
echo "high-water mark but not the (pdn -> owner_uid) registration"
echo "table). The mlx5_ib_dealloc_pd gate recognises this exact"
echo "(status, syndrome, mpd->vfmig_restored) tuple and suppresses"
echo "the failure as benign: kernel returns 0, ib_dealloc_pd_user"
echo "frees the kernel mpd, INFO_HANDLES no longer reports it. FW"
echo "state for the source's PDN slot is reclaimed at VHCA close"
echo "(VF unbind), bounded leak documented in"
echo "  tools/testing/criu_rdma/design/pd_registration_wipe.md."
echo
echo "Subtest 7 PASSes when DEALLOC_PD returns 0 AND INFO_HANDLES"
echo "no longer reports the handle."
quit_dst_probe

# --- verdict --------------------------------------------------------

overall_rc=0
fw_ok=0
if [ "${pos_fw_accept:-0}" = 1 ]; then
    fw_ok=1
fi

echo
echo "================ PD RESTORE VERDICT ================="
echo "  src_pdn               = $src_pdn"
echo "  target_handle         = $TARGET_HANDLE"
echo
echo "  dst probe subtests    = ${DST_PROBE_RC:-?}  (0 means all PASS)"
if [ "${dst_overall_pass:-0}" = 1 ]; then
    echo "  dst probe overall     = PASS"
else
    echo "  dst probe overall     = FAIL/UNKNOWN"
fi
echo "  dst probe adopted_pdn = ${dst_adopted_pdn:-?}"
echo "  FW liveness fw_accept = ${pos_fw_accept:-?}  syndrome=${pos_fw_syndrome:-?}"
echo

if [ "${DST_PROBE_RC:-1}" = 0 ] \
   && [ "${dst_overall_pass:-0}" = 1 ] \
   && [ "$fw_ok" = 1 ]; then
    echo "  PASS  RESTORE_PD landed on $DST_IBDEV @ handle $TARGET_HANDLE,"
    echo "        adopted pdn $src_pdn, FW accepts CREATE_MKEY referencing"
    echo "        it, and DEALLOC_PD round-trips cleanly via the"
    echo "        mlx5_ib_dealloc_pd vfmig_restored+syndrome gate"
    echo "        (subtest 7's post-gate expectation). The kernel-side"
    echo "        teardown completes regardless of S4..S7 landing because"
    echo "        the gate is independent of dependent FW resources --"
    echo "        FW state is reclaimed at VHCA close (VF unbind)."
    echo "        mlx5_ib_restore_pd + the gate are functional for the v0"
    echo "        critical path. See design/pd_registration_wipe.md for the"
    echo "        leak-budget analysis."
else
    echo "  FAIL  see the per-subtest log above. Common causes:"
    echo "        - kernel not running C3+C4 (rebuild + reboot needed)"
    echo "        - DEVX-aware ucontext on src (uid != 0): test_pd_adopt.sh"
    echo "          only validated the uid=0 case"
    echo "        - LOAD_VHCA_STATE didn't actually preserve src_pdn"
    echo "          (re-run uobject_restore/fw_id_continuity to check)"
    overall_rc=1
fi
echo "====================================================="

# --- manifest -------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# pd_restore_mlx5_vfmig manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_pdn=$src_pdn
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV
target_handle=$TARGET_HANDLE
dst_probe_rc=${DST_PROBE_RC:-?}
dst_overall_pass=${dst_overall_pass:-0}
dst_adopted_pdn=${dst_adopted_pdn:-?}
fw_accept=${pos_fw_accept:-?}
fw_syndrome=${pos_fw_syndrome:-?}
overall_rc=$overall_rc
EOF
sudo chmod 0644 "$META"
echo "wrote $META"

exit "$overall_rc"
