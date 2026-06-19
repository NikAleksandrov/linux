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
# Empirical findings
# ------------------
#
# 2026-05-14, uid=0 lane (mlx5 FW 28.48.1000): both the positive
# case (pd=src_pdn, uid=0) and the negative control
# (pd=BOGUS_PDN, uid=0) returned fw_syndrome=0. That is: mlx5
# firmware does NOT gate CREATE_MKEY's mkc.pd field under uid=0
# -- kernel-uid is privileged and mlx5 treats the pd index as
# metadata rather than an existence check. This is not a failure
# of Model A; it is the absence of a gate to fail. Combined with
# K6's PARTIAL PASS (src_pdn's slot reserved in FW post-LOAD) +
# mlx5_ib's non-DEVX code path always running under uid=0, Model A
# is trivially sound for the v0 critical path (non-DEVX
# libibverbs applications -- the common CRIU restore target).
#
# 2026-05-15, DEVX-source lane: extended to also probe the
# DEVX-adoption case where the source PD was allocated under
# devx_uid != 0 (libmlx5 default with MLX5_LIB_CAP_DYN_UAR).
# This is the path the CRIU full-integration flow exercises and
# the path mlx5_ib_restore_pd's CREATE_MKEY defense-in-depth
# probe (kernel d4acb54ebd3d) gates on. The new probes are:
#
#   * (pdn=src_pdn, uid=src_devx_uid): does the source DEVX
#     ucontext's (pdn, uid) ownership survive LOAD_VHCA_STATE?
#     Answers the design-doc §S3b DEVX gap.
#   * (pdn=src_pdn, uid=BOGUS_UID): sanity-check that FW DOES
#     gate non-zero uids on CREATE_MKEY. If this passes, FW
#     ungating under uid=0 also extends to non-zero uids and the
#     probe is meaningless.
#
# Verdict matrix
# --------------
# Notation: P_zero = PROBE_PD(src_pdn, uid=0); N_zero = PROBE_PD
# (BOGUS_PDN, uid=0); P_devx = PROBE_PD(src_pdn, uid=src_devx_uid);
# N_devx = PROBE_PD(src_pdn, uid=BOGUS_UID). Each cell is
# fw_accept (1=accept, 0=reject).
#
#   P_zero | N_zero | P_devx | N_devx | meaning
#   -------+--------+--------+--------+--------------------------
#      1   |   0    |   x    |   x    | STRONG PASS (uid=0 lane):
#                                       FW gates uid=0 on pdn validity
#                                       AND src_pdn survived LOAD.
#                                       Model A confirmed.
#      1   |   1    |  N/A   |  N/A   | WEAK PASS (uid=0 lane,
#                                       src_devx_uid==0): FW doesn't
#                                       gate uid=0; Model A trivially
#                                       sound for non-DEVX v0.
#      1   |   1    |   1    |   0    | STRONG PASS (DEVX lane):
#                                       FW preserves (pdn, devx_uid)
#                                       across LOAD; current
#                                       mlx5_ib_restore_pd works
#                                       directly. DEVX adoption is a
#                                       free path forward.
#      1   |   1    |   0    |   0    | DEVX-ADOPT BLIND SPOT:
#                                       FW lost the (pdn, devx_uid)
#                                       ownership across LOAD (or
#                                       never preserved it). uid=0
#                                       path still works; DEVX needs
#                                       a Path-B fix (PROBE_UID-
#                                       delta validation chain +
#                                       fresh CREATE_UCTX with
#                                       rebind, or REGISTER_UID).
#      1   |   1    |   x    |   1    | FW UNGATED EVERYWHERE: even
#                                       non-zero uids don't gate
#                                       CREATE_MKEY; the probe is
#                                       meaningless. Investigate
#                                       whether a different verb is
#                                       the right gate (CREATE_QP,
#                                       ALLOC_TD, etc.).
#      0   |   *    |   x    |   x    | HARD FAIL: even uid=0 can't
#                                       use src_pdn. Implies K6 lied
#                                       or LOAD didn't preserve pdn
#                                       allocator. Inspect.
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
#   Phase F  ===== THE TEST (uid=0 lane) =====
#            Issue PROBE_PD(dst_vf, src_pdn, uid=0) and read
#            fw_accept=. Expected (FW 28.48.1000): fw_accept=1
#            with fw_syndrome=0 (uid=0 ungated).
#   Phase G  Negative controls (uid=0 lane). PROBE_PD with bogus
#            pdn must return fw_accept=0 (FW rejects with
#            "invalid PD" syndrome) IF uid=0 is gated. On current
#            FW it accepts (WEAK PASS).
#   Phase H  ===== DEVX-source variant =====
#            If src_devx_uid != 0 (libmlx5 opened DEVX), run two
#            additional probes:
#              * PROBE_PD(src_pdn, uid=src_devx_uid): is the
#                source DEVX ucontext's (pdn, uid) ownership
#                preserved across LOAD?
#              * PROBE_PD(src_pdn, uid=BOGUS_UID): does FW gate
#                CREATE_MKEY on non-zero uids at all?
#            Decodes the design-doc §S3b DEVX gap that bites the
#            CRIU full integration when libmlx5 opens DEVX.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_pd_adopt.sh
#
# Optional knobs (mirroring test_fw_id_continuity.sh):
#   BLOB                Path for the SAVE blob (default /tmp/vf_pd_adopt.blob).
#   TOOL                mlx5_vfmig CLI (default $ROOT_DIR/tools/mlx5_vfmig).
#   PROBE               fw_id_continuity_probe binary
#                       (default $SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe).
#   BOGUS_PDN           pdn value to use for the uid=0 negative control.
#                       Default 0xffff00 -- well above anything a
#                       fresh VHCA's allocator will reach during
#                       this short-lived test.
#   BOGUS_UID           uid value to use for the DEVX-lane negative
#                       control. Default 0xfffd -- in the legal
#                       16-bit FW uid range but well above any
#                       plausible per-VHCA uctx allocation.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe}
BLOB=${BLOB:-/tmp/vf_pd_adopt.blob}
META=${META:-${BLOB}.meta}
BOGUS_PDN=${BOGUS_PDN:-$((0xffff00))}
# Plausible-but-unallocated uid for the DEVX-lane negative control.
# 16-bit FW field; pick a value well above the per-VHCA uctx pool
# but inside the legal range. If this collides with a real uid
# on your hardware (it should not on a fresh VF), override.
BOGUS_UID=${BOGUS_UID:-$((0xfffd))}

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
sudo dmesg -C
# Enable the mlx5_ib_dbg trace in mlx5_ib_alloc_ucontext that emits the
# freshly-allocated devx_uid (vfmig_uctx_dbg). Replaces the older
# vfmig_pd_dbg pr_info that was retired when the PD gate landed.
echo 'func mlx5_ib_alloc_ucontext +p' | \
    sudo tee /sys/kernel/debug/dynamic_debug/control >/dev/null 2>&1 || true
start_src_probe "$SRC_IBDEV"
echo "captured: src_pdn=$src_pdn (FW pdn from mlx5dv_pd)"

# Capture the source ucontext's devx_uid from the kernel's
# vfmig_uctx_dbg trace that fires during ibv_open_device inside the
# probe. Whether libibverbs opens DEVX or not depends on libmlx5's
# defaults + MLX5_LIB_CAP_DYN_UAR negotiation; we read the truth
# off the kernel side rather than guessing. Format of the dmesg line:
# "vfmig_uctx_dbg: alloc_ucontext ibdev=mlx5_X devx_uid=N adopted=0".
src_devx_uid=$(sudo dmesg | \
    grep -E "vfmig_uctx_dbg: alloc_ucontext ibdev=$SRC_IBDEV devx_uid=[0-9]+ adopted=0" | \
    tail -1 | sed -nE 's/.* devx_uid=([0-9]+) .*/\1/p')
src_devx_uid=${src_devx_uid:-0}
echo "captured: src_devx_uid=$src_devx_uid (0 == non-DEVX; non-zero == DEVX adoption lane)"

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
bind_vf_safe "$DST_VF" 60 "Phase E: dest VF bind"sleep 1
DST_IBDEV=$(find_ib_dev_for_pci "$DST_VF") || { echo "FAIL: no ibdev for dest $DST_VF"; exit 1; }
echo "dest ibdev: $DST_IBDEV"

# --- Phase F: THE TEST ----------------------------------------------

echo
echo "================ Phase F: PROBE_PD (uid=0 lane) ====="
echo "Question: does the FW accept CREATE_MKEY(uid=0, pd=src_pdn) on"
echo "the destination, indicating the source PD survives LOAD?"
probe_pd_into positive_uid0 0 "$src_pdn" 0 pos_zero

# --- Phase G: negative controls -------------------------------------

echo
echo "================ Phase G: NEGATIVE CONTROL (uid=0) =="
echo "Question: does PROBE_PD correctly REJECT a bogus pdn? (If yes,"
echo "the positive result above wasn't just FW accepting everything.)"
probe_pd_into bogus_uid0 0 "$BOGUS_PDN" 0 neg_zero

# --- Phase H: DEVX-source variant -----------------------------------
#
# Six probes form a 3x2 matrix:
#
#                 |  src_pdn (=20)   |  BOGUS_PDN (=0xffff00)
#   --------------+------------------+------------------------
#   uid=src_devx  | P_devx           | X_devx
#   uid=lo_unalloc| P_lo             | X_lo
#   uid=hi_unalloc| P_hi (== N_devx) | X_hi
#
# Cross-correlations between cells decode the FW gating model:
#
#   - If uid=src_devx fails for both src_pdn AND BOGUS_PDN
#     (P_devx=0, X_devx=0), the rejection is uid-scoped (uid=2 is
#     "live but broken" post-LOAD, regardless of pdn).
#   - If uid=src_devx fails only for src_pdn (P_devx=0, X_devx=1),
#     the rejection is (pdn, uid) ownership-scoped (FW remembers
#     uid=2 owned a specific pdn=20 binding which is now corrupt).
#   - If uid=lo_unalloc and uid=hi_unalloc behave identically,
#     FW gates "known" uids only and treats all unknown uids as
#     host-priv (the most likely hypothesis given run-1 data).
#   - If uid=lo_unalloc rejects and uid=hi_unalloc accepts, FW has
#     a uid range cutoff (less likely).
#
# probe_uid (run twice, before and after the PROBE_PD sweep) lets us
# also observe the dest's uctx allocator state. Adjacent return
# values mean the allocator survived LOAD; resetting to 1 means it
# was wiped.

# Pick uids that the source-side allocator definitely did NOT
# allocate. We default to src_devx_uid + 7 for the "low unalloc"
# slot -- past the source's known uids but still in the low range
# FW would normally hand out, and BOGUS_UID (default 0xfffd) for
# the "high unalloc" slot.
LO_UNALLOC=${LO_UNALLOC:-$((src_devx_uid + 7))}
HI_UNALLOC=${HI_UNALLOC:-$BOGUS_UID}

if [ "$src_devx_uid" != 0 ]; then
    echo
    echo "================ Phase H: DEVX-source PROBE_PD ======"
    echo "Goal: decode FW's uid-gating semantics post-LOAD by"
    echo "running a 3x2 matrix of PROBE_PD calls. lo_unalloc=$LO_UNALLOC,"
    echo "hi_unalloc=$HI_UNALLOC."
    echo
    echo "--- pre-sweep: dest uctx allocator state (probe_uid) ---"
    if probe_pre1_out=$(sudo "$TOOL" "$PF" probe_uid 0 2>&1); then
        echo "  [pre-sweep] $probe_pre1_out"
        # "vf 0: probe_uid -> uid=N" -- pluck the N.
        probe_pre1_uid=$(echo "$probe_pre1_out" | \
            sed -nE 's/.*probe_uid -> uid=([0-9]+).*/\1/p')
    else
        echo "  [pre-sweep] probe_uid FAILED:"; echo "$probe_pre1_out" | sed 's/^/    /'
    fi
    if probe_pre2_out=$(sudo "$TOOL" "$PF" probe_uid 0 2>&1); then
        echo "  [pre-sweep] $probe_pre2_out"
        probe_pre2_uid=$(echo "$probe_pre2_out" | \
            sed -nE 's/.*probe_uid -> uid=([0-9]+).*/\1/p')
    else
        echo "  [pre-sweep] probe_uid (2nd) FAILED:"; echo "$probe_pre2_out" | sed 's/^/    /'
    fi
    echo "  ==> dest_fresh_uid_1=${probe_pre1_uid:-?} dest_fresh_uid_2=${probe_pre2_uid:-?}"
    echo

    echo "--- Row 1: uid=src_devx_uid ($src_devx_uid) ---"
    probe_pd_into "uid_devx, src_pdn"   "0" "$src_pdn"   "$src_devx_uid" pos_devx
    probe_pd_into "uid_devx, BOGUS_PDN" "0" "$BOGUS_PDN" "$src_devx_uid" xpd_devx

    echo
    echo "--- Row 2: uid=lo_unalloc ($LO_UNALLOC) ---"
    probe_pd_into "uid_lo, src_pdn"   "0" "$src_pdn"   "$LO_UNALLOC" pos_lo
    probe_pd_into "uid_lo, BOGUS_PDN" "0" "$BOGUS_PDN" "$LO_UNALLOC" xpd_lo

    echo
    echo "--- Row 3: uid=hi_unalloc ($HI_UNALLOC) (== N_devx alias) ---"
    probe_pd_into "uid_hi, src_pdn"   "0" "$src_pdn"   "$HI_UNALLOC" pos_hi
    probe_pd_into "uid_hi, BOGUS_PDN" "0" "$BOGUS_PDN" "$HI_UNALLOC" xpd_hi

    # back-compat: the existing matrix used neg_devx for the
    # (src_pdn, BOGUS_UID) cell; keep its name in scope.
    neg_devx_fw_accept=${pos_hi_fw_accept:-?}
    neg_devx_fw_syndrome=${pos_hi_fw_syndrome:-?}
fi

# --- verdict --------------------------------------------------------

overall_rc=0

echo
echo "================ PD ADOPT VERDICT ==================="
echo "  src_pdn          = $src_pdn"
echo "  src_devx_uid     = $src_devx_uid"
echo "  bogus_pdn        = $BOGUS_PDN"
echo "  bogus_uid        = $BOGUS_UID"
echo
echo "  ----------------------------------------------------------"
echo "  uid=0 lane (sanity baseline):"
echo "    (src_pdn=$src_pdn,  uid=0)            fw_accept=${pos_zero_fw_accept:-?} syndrome=${pos_zero_fw_syndrome:-?}"
echo "    (BOGUS_PDN=$BOGUS_PDN, uid=0)         fw_accept=${neg_zero_fw_accept:-?} syndrome=${neg_zero_fw_syndrome:-?}"
if [ "$src_devx_uid" != 0 ]; then
    echo "  ----------------------------------------------------------"
    echo "  DEVX lane probe matrix:"
    echo "  uctx-alloc characterisation:"
    echo "    dest_fresh_uid_1 = ${probe_pre1_uid:-?}"
    echo "    dest_fresh_uid_2 = ${probe_pre2_uid:-?} (adjacent => monotonic allocator survived LOAD)"
    echo
    printf "  %-32s %-22s %-22s\n" "uid" "src_pdn=$src_pdn" "BOGUS_PDN=$BOGUS_PDN"
    printf "  %-32s %-22s %-22s\n" "--------------------------------" "----------------------" "----------------------"
    printf "  %-32s accept=%-2s syn=%-10s  accept=%-2s syn=%-10s\n" \
        "src_devx_uid=$src_devx_uid (registered)" \
        "${pos_devx_fw_accept:-?}" "${pos_devx_fw_syndrome:-?}" \
        "${xpd_devx_fw_accept:-?}" "${xpd_devx_fw_syndrome:-?}"
    printf "  %-32s accept=%-2s syn=%-10s  accept=%-2s syn=%-10s\n" \
        "lo_unalloc=$LO_UNALLOC (low unallocated)" \
        "${pos_lo_fw_accept:-?}" "${pos_lo_fw_syndrome:-?}" \
        "${xpd_lo_fw_accept:-?}" "${xpd_lo_fw_syndrome:-?}"
    printf "  %-32s accept=%-2s syn=%-10s  accept=%-2s syn=%-10s\n" \
        "hi_unalloc=$HI_UNALLOC (high unallocated)" \
        "${pos_hi_fw_accept:-?}" "${pos_hi_fw_syndrome:-?}" \
        "${xpd_hi_fw_accept:-?}" "${xpd_hi_fw_syndrome:-?}"
fi
echo

# Bind the cells into a verdict.
P_zero=${pos_zero_fw_accept:-?}
N_zero=${neg_zero_fw_accept:-?}

if [ "$P_zero" = 0 ]; then
    echo "  HARD FAIL    P_zero REJECTED -- the source pdn is not even"
    echo "               usable on the destination under host-privileged"
    echo "               uid=0. This contradicts K6's PARTIAL PASS"
    echo "               (LOAD preserved the FW pdn allocator). Either"
    echo "               K6 needs re-validation against this FW or"
    echo "               LOAD_VHCA_STATE on this VHCA failed to apply"
    echo "               source pdn state. Inspect P_zero syndrome"
    echo "               (${pos_zero_fw_syndrome:-?}) and dmesg around"
    echo "               LOAD; the §S3b plan must be revisited."
    overall_rc=1
elif [ "$src_devx_uid" = 0 ]; then
    # Non-DEVX lane only -- back-compat with the original C2 verdict.
    if [ "$N_zero" = 0 ]; then
        echo "  STRONG PASS  (uid=0 lane) FW gates kernel-uid CREATE_MKEY"
        echo "               on pdn validity AND src_pdn survived LOAD."
        echo "               Model A confirmed for the non-DEVX critical"
        echo "               path. DEVX lane not exercised (libibverbs"
        echo "               opened without DEVX -- src_devx_uid=0)."
    elif [ "$N_zero" = 1 ]; then
        echo "  WEAK PASS    (uid=0 lane) FW does NOT gate kernel-uid"
        echo "               CREATE_MKEY on pdn validity. Combined with"
        echo "               K6 (LOAD preserves pdn allocator) +"
        echo "               mlx5_ib non-DEVX always running under uid=0,"
        echo "               Model A is trivially sound for the v0"
        echo "               critical path (non-DEVX libibverbs apps)."
        echo "               DEVX lane not exercised."
    else
        echo "  UNEXPECTED   N_zero=$N_zero with src_devx_uid=0; investigate."
        overall_rc=1
    fi
else
    # DEVX lane: decode the 3x2 matrix.
    PD=${pos_devx_fw_accept:-?}    # (src_pdn, src_devx_uid)
    XD=${xpd_devx_fw_accept:-?}    # (BOGUS_PDN, src_devx_uid)
    PL=${pos_lo_fw_accept:-?}      # (src_pdn, lo_unalloc)
    XL=${xpd_lo_fw_accept:-?}      # (BOGUS_PDN, lo_unalloc)
    PH=${pos_hi_fw_accept:-?}      # (src_pdn, hi_unalloc)
    XH=${xpd_hi_fw_accept:-?}      # (BOGUS_PDN, hi_unalloc)

    if [ "$PD" = 1 ] && [ "$XD" = 0 ]; then
        echo "  DEVX OWNERSHIP   (pdn=src, uid=src_devx) ACCEPTS while"
        echo "  PRESERVED        (pdn=BOGUS, uid=src_devx) REJECTS. FW"
        echo "                   preserves the (pdn, devx_uid) binding"
        echo "                   across LOAD. Current mlx5_ib_restore_pd"
        echo "                   logic is correct; the integration"
        echo "                   failure must come from somewhere else"
        echo "                   (port-init teardown, mad-QP1 cleanup"
        echo "                   races). Reinspect dmesg between LOAD"
        echo "                   and restore_pd."
    elif [ "$PD" = 0 ] && [ "$XD" = 0 ]; then
        if [ "$PL" = 0 ] && [ "$XL" = 0 ] && [ "$PH" = 0 ] && [ "$XH" = 0 ]; then
            echo "  FW WEDGED        Every non-zero uid is rejected. FW"
            echo "                   may be in a degraded state post-LOAD."
            echo "                   Inspect syndromes + dmesg."
            overall_rc=1
        else
            echo "  UID=SRCDEVX      uid=$src_devx_uid is broken post-LOAD"
            echo "  BLANKET-BROKEN   regardless of pdn. Other uids (low/"
            echo "                   high unallocated) behave normally."
            echo "                   FW preserved the uctx registration"
            echo "                   for uid=$src_devx_uid but in a degraded"
            echo "                   state where CREATE_MKEY can't bind"
            echo "                   under it. Path B work: rebuild the"
            echo "                   uctx state via CREATE_UCTX or find"
            echo "                   a FW \"resume\" op."
            overall_rc=1
        fi
    elif [ "$PD" = 0 ] && [ "$XD" = 1 ]; then
        echo "  (PDN, DEVX_UID)  uid=$src_devx_uid is alive (accepts pdn=BOGUS)"
        echo "  OWNERSHIP CORRUPT but rejects pdn=$src_pdn specifically. FW"
        echo "                   preserves uctx registration AND has a"
        echo "                   per-(pdn, uid) ownership check. LOAD"
        echo "                   broke the (pdn=$src_pdn, uid=$src_devx_uid)"
        echo "                   binding. Path B work: either avoid the"
        echo "                   corrupt binding (allocate a fresh pdn"
        echo "                   on dest under uid=$src_devx_uid) or find"
        echo "                   a FW REBIND/RECLAIM op."
        overall_rc=1
    elif [ "$PL" = 1 ] && [ "$PH" = 1 ] && [ "$XL" = 1 ] && [ "$XH" = 1 ]; then
        echo "  UNREGISTERED     uid=$src_devx_uid is gated (registered with"
        echo "  UIDS ARE         FW post-LOAD) but every UNREGISTERED uid"
        echo "  HOST-PRIV        (lo=$LO_UNALLOC, hi=$HI_UNALLOC) is accepted"
        echo "                   under host-privileged fallback. PROBE_PD"
        echo "                   with an unregistered uid is therefore"
        echo "                   meaningless as a negative control."
        if [ "$PD" = 0 ]; then
            echo "                   uid=$src_devx_uid rejects at src_pdn ="
            echo "                   the actual DEVX adoption failure shape."
            echo "                   Path B work needed to make CREATE_MKEY"
            echo "                   work under (pdn, src_devx_uid) post-LOAD."
            overall_rc=1
        else
            echo "                   But uid=$src_devx_uid still accepts (src_pdn)"
            echo "                   -- combine with other rows to interpret."
        fi
    else
        echo "  COMPLEX PATTERN  matrix doesn't fit a simple model;"
        echo "                   inspect each cell's syndrome by hand."
        echo "                   Likely needs additional uid probes."
        overall_rc=1
    fi
fi
echo "====================================================="

# --- manifest -------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# pd_adopt manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_pdn=$src_pdn
src_devx_uid=$src_devx_uid
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV
dest_fresh_uid_1=${probe_pre1_uid:-NA}
dest_fresh_uid_2=${probe_pre2_uid:-NA}
P_zero_fw_accept=${pos_zero_fw_accept:-?}
P_zero_fw_syndrome=${pos_zero_fw_syndrome:-?}
N_zero_fw_accept=${neg_zero_fw_accept:-?}
N_zero_fw_syndrome=${neg_zero_fw_syndrome:-?}
# DEVX 3x2 matrix (only populated when src_devx_uid != 0)
pos_devx_fw_accept=${pos_devx_fw_accept:-NA}
pos_devx_fw_syndrome=${pos_devx_fw_syndrome:-NA}
xpd_devx_fw_accept=${xpd_devx_fw_accept:-NA}
xpd_devx_fw_syndrome=${xpd_devx_fw_syndrome:-NA}
pos_lo_fw_accept=${pos_lo_fw_accept:-NA}
pos_lo_fw_syndrome=${pos_lo_fw_syndrome:-NA}
xpd_lo_fw_accept=${xpd_lo_fw_accept:-NA}
xpd_lo_fw_syndrome=${xpd_lo_fw_syndrome:-NA}
pos_hi_fw_accept=${pos_hi_fw_accept:-NA}
pos_hi_fw_syndrome=${pos_hi_fw_syndrome:-NA}
xpd_hi_fw_accept=${xpd_hi_fw_accept:-NA}
xpd_hi_fw_syndrome=${xpd_hi_fw_syndrome:-NA}
bogus_pdn=$BOGUS_PDN
bogus_uid=$BOGUS_UID
lo_unalloc=${LO_UNALLOC:-NA}
hi_unalloc=${HI_UNALLOC:-NA}
overall_rc=$overall_rc
EOF
sudo chmod 0644 "$META"
echo "wrote $META"

exit "$overall_rc"
