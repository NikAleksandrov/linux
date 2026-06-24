#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S5b empirical: validate that Model A for mlx5_ib_restore_cq is
# sound, i.e. the source's user-mode CQ at cqn=N survives
# LOAD_VHCA_STATE on the destination intact, with byte-identical
# cqc context (eqn, log_cq_size, log_page_size, page_offset,
# status, oi).
#
# This is the CQ analogue of test_mr_adopt.sh / test_pd_adopt.sh:
#
#   test_pd_adopt.sh   -- given a fresh ucontext on the destination,
#                         can FW accept a CREATE_MKEY against the
#                         source's pdn?
#   test_mr_adopt.sh   -- given the source's mkey_index, does the
#                         destination FW still have that mkey alive
#                         post-LOAD with byte-identical mkc context?
#   test_cq_adopt.sh   -- given the source's cqn, does the
#                         destination FW still have that CQ alive
#                         post-LOAD with byte-identical cqc context,
#                         and -- the load-bearing question CQ adds
#                         over MR -- does its EQ binding (cqc.eqn)
#                         survive intact?
#
# Why "eqn-survives" matters
# --------------------------
# A CQ delivers completions through an event queue identified by
# cqc.c_eqn_or_apu_element. If LOAD_VHCA_STATE preserves the cqn
# but loses (or remaps) the eqn binding, the adopted CQ is alive
# but mute -- no ARM_CQ event would ever fire. Model A for
# mlx5_ib_restore_cq is only sound if eqn is preserved verbatim
# alongside cqn. PROBE_CQN reads the eqn back out of cqc on both
# the source pre-SAVE and the destination post-LOAD; this script
# byte-compares the two.
#
# Symmetric source/destination snapshot
# -------------------------------------
# Unlike test_mr_adopt.sh -- which knows src_mr_addr / src_mr_length
# from the userspace allocation and only PROBE_MKEY's the destination
# -- the cqc shape parameters (eqn, log_cq_size, log_page_size,
# page_offset, status, oi) are NOT exposed by libibverbs/libmlx5.
# The cleanest fix is to PROBE_CQN the source VF pre-SAVE too: the
# ioctl works on any bound VF mdev that is INTERFACE_STATE_UP, so we
# get a FW-of-truth source snapshot at no extra cost. The byte-
# equality assertion then becomes "src PROBE_CQN output == dst
# PROBE_CQN output" across the cqc fields the design doc calls out.
#
# Verdict matrix
# --------------
# Notation: P = PROBE_CQN(dst_vf, src_cqn);
#           N = PROBE_CQN(dst_vf, BOGUS_CQN);
#           S = PROBE_CQN(src_vf, src_cqn) captured pre-SAVE.
# Comparison is byte-equal across the 6 cqc fields the design doc
# locks in as the §S5b assertion: (eqn, log_cq_size, log_page_size,
# page_offset, status, oi).
#
#   P_accept | N_accept | S vs P match | meaning
#   ---------+----------+--------------+------------------------
#       1    |    0     | yes          | STRONG PASS: CQ alive,
#                                        FW gates QUERY_CQ on
#                                        existence, all 6 cqc
#                                        fields preserved.
#                                        Model A confirmed.
#       1    |    1     | yes          | WEAK PASS: CQ alive,
#                                        FW does NOT gate
#                                        QUERY_CQ (host-priv
#                                        path). Negative control
#                                        meaningless on this FW
#                                        but Model A still sound
#                                        for v0.
#       1    |    *     | no           | CONTENT MISMATCH:
#                                        cqn survived but cqc
#                                        was not byte-preserved
#                                        across LOAD. Kernel
#                                        SAVE/LOAD bug, not
#                                        Model A unsound.
#                                        eqn drift = adopted CQ
#                                        delivers completions
#                                        on the wrong EQ; other
#                                        drifts = ring shape /
#                                        flags wrong.
#       0    |    *     | n/a          | HARD FAIL: source cqn
#                                        was wiped post-LOAD.
#                                        Model A unworkable;
#                                        §S5b plan must be
#                                        revisited (rebuild
#                                        via fresh CREATE_CQ
#                                        at the same index).
#
# Flow
# ----
#   Phase A  Provision source VF, set_tracked + enable_migratable,
#            bind.
#   Phase B  Fork fw_id_continuity_probe on the source ibdev; it
#            allocates {PD, CQ, QP, MR, SRQ}. Capture src_cqn from
#            its READY-line dictionary. Probe stays alive holding
#            the resources through SAVE.
#   Phase C  PROBE_CQN(src_vf, src_cqn) -- capture the source-side
#            cqc snapshot from FW. This is the truth we'll byte-
#            compare against post-LOAD.
#   Phase D  SAVE_VHCA_STATE on the source.
#   Phase E  Quit the source probe; tear down the source VF.
#   Phase F  Provision a fresh destination VF: set_tracked,
#            enable_migratable, LOAD_VHCA_STATE, mark_restored,
#            bind.
#   Phase G  ===== THE TEST =====
#            PROBE_CQN(dst_vf, src_cqn) -- expect fw_accept=1 with
#            all 6 cqc fields byte-equal to the source snapshot
#            captured in Phase C.
#   Phase H  Negative control. PROBE_CQN(dst_vf, BOGUS_CQN).
#            Expected: fw_accept=0 with FW's "BAD_RES_STATE"
#            syndrome IF FW gates QUERY_CQ on existence; on a
#            permissive FW, fw_accept=1 with zeroed-out content
#            (== WEAK PASS overall).
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_cq_adopt.sh
#
# Optional knobs (mirror test_mr_adopt.sh):
#   BLOB           Path for the SAVE blob.
#   TOOL           mlx5_vfmig CLI.
#   PROBE          fw_id_continuity_probe binary.
#   BOGUS_CQN      cqn for the negative control (default 0xffff00 --
#                  well above any plausible per-VHCA cqn allocation
#                  in this short-lived test).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe}
BLOB=${BLOB:-/tmp/vf_cq_adopt.blob}
META=${META:-${BLOB}.meta}
BOGUS_CQN=${BOGUS_CQN:-$((0xffff00))}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]  || { echo "build $TOOL first: make -C $ROOT_DIR";  exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first: make -C $ROOT_DIR"; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

# fw_id_continuity_probe needs ibv_open_device(); a VF that's bound
# to mlx5_core but with mlx5_ib unloaded would silently produce "no
# ibdev" later. Catch it here with a clear message. /sys/module/X
# is the canonical "is module X loaded?" check and -- unlike lsmod
# -- does not depend on /sbin being on PATH (which it often isn't
# under `sudo SCRIPT.sh`).
if [ ! -d /sys/module/mlx5_ib ]; then
    echo "FAIL: mlx5_ib is not loaded -- the source/dest VFs would bind"
    echo "      to mlx5_core but no IB device would appear, and the"
    echo "      probe would have nothing to ibv_open_device() on."
    echo "      Run: sudo modprobe mlx5_ib"
    exit 1
fi

WORKDIR=$(mktemp -d -t cqadopt.XXXXXX)
cleanup() {
    if [ -n "${SRC_PROBE_PID:-}" ] && kill -0 "$SRC_PROBE_PID" 2>/dev/null; then
        echo "quit" > "$WORKDIR/src.in" 2>/dev/null || true
        wait "$SRC_PROBE_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

# --- helpers (copy-paste from test_mr_adopt.sh; intentional since
#     there's no common.sh today) ------------------------------------

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

# Poll /sys/class/infiniband/* until an ibdev whose ->device symlink
# points at $bdf appears, or $timeout seconds elapse. The mlx5_core
# probe registers the IB device asynchronously after the PCI driver
# attaches, so just doing `find_ib_dev_for_pci` immediately after
# bind_vf_safe is racy (bind sets ->driver; ibdev registration
# happens later in the probe sequence). Print the ibdev name on
# success.
wait_for_ib_dev() {
    local bdf=$1
    local timeout=${2:-30}
    local ib started
    started=$(date +%s)
    while :; do
        if ib=$(find_ib_dev_for_pci "$bdf"); then
            echo "$ib"
            return 0
        fi
        if [ $(( $(date +%s) - started )) -ge "$timeout" ]; then
            echo "ERROR: no ibdev for $bdf after ${timeout}s." >&2
            echo "       Is mlx5_ib loaded? ([ -d /sys/module/mlx5_ib ])" >&2
            return 1
        fi
        sleep 0.2
    done
}

# Start fw_id_continuity_probe in the background hooked to FIFOs.
# Drains READY then captures key=value lines into "src_<key>".
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

# Run `mlx5_vfmig $PF probe_cqn $vf $cqn`, capture each key=value
# into "$prefix_<key>" variables in the caller's scope.
probe_cqn_into() {
    local label=$1 vf=$2 cqn=$3 prefix=$4
    local out line k v
    echo "=== PROBE_CQN ($label): vf=$vf cqn=$cqn ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_cqn "$vf" "$cqn" 2>&1); then
        echo "$out" | sed 's/^/  /'
        echo "  PROBE_CQN ioctl failed (transport / arg error)"
        return 1
    fi
    while IFS= read -r line; do
        echo "  [$label probe_cqn] $line"
        case "$line" in
            *=*) k="${line%%=*}"; v="${line#*=}";
                 eval "${prefix}_${k}=\"\$v\"" ;;
        esac
    done <<<"$out"
    return 0
}

# Numeric-equality on values captured by probe_cqn_into. Both args
# are interpreted as integers (printf %d eats 0x prefixes).
val_eq() {
    [ "$(printf '%d' "$1" 2>/dev/null)" = \
      "$(printf '%d' "$2" 2>/dev/null)" ]
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
SRC_IBDEV=$(wait_for_ib_dev "$SRC_VF" 30) || {
    echo "FAIL: no ibdev for source $SRC_VF"
    echo "  Diagnostics:"
    echo "    /sys/module/mlx5_ib exists: $( [ -d /sys/module/mlx5_ib ] && echo yes || echo no )"
    echo "    /sys/class/infiniband/ contains:"
    ls /sys/class/infiniband/ 2>/dev/null | sed 's/^/      /' || echo "      (empty)"
    echo "    /sys/bus/pci/devices/$SRC_VF/driver -> $(readlink /sys/bus/pci/devices/$SRC_VF/driver 2>/dev/null || echo 'unbound')"
    exit 1
}
echo "source ibdev: $SRC_IBDEV"

# --- Phase B ---------------------------------------------------------

echo "=== Phase B: source probe (registers CQ, holds it across SAVE) ==="
sudo dmesg -C
start_src_probe "$SRC_IBDEV"
echo "captured: src_cqn=$src_cqn (will byte-compare cqc fields below)"

# --- Phase C: source-side cqc snapshot (FW-of-truth) ---------------

echo
echo "=== Phase C: PROBE_CQN against the SOURCE VF (pre-SAVE) ==="
echo "Capture the FW's view of the source CQ context. This is the"
echo "truth that LOAD_VHCA_STATE on the destination must reproduce"
echo "byte-for-byte."
probe_cqn_into source 0 "$src_cqn" src_pre

if [ "${src_pre_fw_accept:-0}" != 1 ]; then
    echo "FAIL: source-side PROBE_CQN rejected cqn=$src_cqn"
    echo "      fw_syndrome=${src_pre_fw_syndrome:-?}"
    echo "      The probe just allocated this CQ; FW should accept it."
    exit 1
fi
if [ "${src_pre_fw_apu_cq:-0}" != 0 ]; then
    echo "FAIL: source CQ has cqc.apu_cq=${src_pre_fw_apu_cq}"
    echo "      v0 of S5b assumes apu_cq=0 (c_eqn_or_apu_element"
    echo "      interpreted as eqn). APU CQs are unsupported."
    exit 1
fi

# --- Phase D ---------------------------------------------------------

echo "=== Phase D: SAVE_VHCA_STATE ==="
sudo dmesg -C
# snapshot-ordering: pause datapath (CRIU CHECKPOINT_DEVICES), then
# capture (SAVE is suspend-aware and skips its own suspend), then resume.
sudo "$TOOL" "$PF" suspend_vhca 0
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" resume_vhca 0
sudo chmod 0644 "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
[ "$SAVE_BYTES" -gt 16 ] || { echo "FAIL: blob suspiciously small: $SAVE_BYTES"; exit 1; }

# --- Phase E ---------------------------------------------------------

echo "=== Phase E: quit source probe + tear down source VF ==="
quit_src_probe
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null

# --- Phase F ---------------------------------------------------------

echo "=== Phase F: fresh DEST VF + LOAD + bind ==="
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
bind_vf_safe "$DST_VF" 60 "Phase F: dest VF bind"
DST_IBDEV=$(wait_for_ib_dev "$DST_VF" 30) || {
    echo "FAIL: no ibdev for dest $DST_VF"
    exit 1
}
echo "dest ibdev: $DST_IBDEV"

# --- Phase G: THE TEST ----------------------------------------------

echo
echo "================ Phase G: PROBE_CQN (existence + content) ==="
echo "Question: does the FW still have the source CQ at cqn=$src_cqn"
echo "alive on the destination, with cqc fields byte-equal to the"
echo "source snapshot captured in Phase C?"
probe_cqn_into positive 0 "$src_cqn" pos

# --- Phase H: negative control --------------------------------------

echo
echo "================ Phase H: NEGATIVE CONTROL =================="
echo "Question: does PROBE_CQN correctly REJECT a bogus cqn?"
echo "(If yes, the positive result above wasn't just FW returning"
echo "success for everything host-priv asks about.)"
probe_cqn_into bogus 0 "$BOGUS_CQN" neg

# --- verdict --------------------------------------------------------

overall_rc=0

echo
echo "================ CQ ADOPT VERDICT ==================="
echo "  src_cqn          = $src_cqn"
echo "  bogus_cqn        = $BOGUS_CQN"
echo
echo "  Source pre-SAVE snapshot (Phase C, FW-of-truth):"
echo "    fw_accept       = ${src_pre_fw_accept:-?}"
echo "    fw_eqn          = ${src_pre_fw_eqn:-?}"
echo "    fw_status       = ${src_pre_fw_status:-?}"
echo "    fw_log_cq_size  = ${src_pre_fw_log_cq_size:-?}"
echo "    fw_log_page_size= ${src_pre_fw_log_page_size:-?}"
echo "    fw_page_offset  = ${src_pre_fw_page_offset:-?}"
echo "    fw_oi           = ${src_pre_fw_oi:-?}"
echo "    fw_cqe_sz       = ${src_pre_fw_cqe_sz:-?}    (forensic)"
echo "    fw_apu_cq       = ${src_pre_fw_apu_cq:-?}    (forensic; v0 expects 0)"
echo "    fw_uar_page     = ${src_pre_fw_uar_page:-?}    (forensic; B3)"
echo "    fw_dbr_addr     = ${src_pre_fw_dbr_addr:-?}    (forensic; B3)"
echo
echo "  Positive (cqn=$src_cqn on dst, post-LOAD):"
echo "    fw_accept       = ${pos_fw_accept:-?}    (1 = cqn present in dest FW)"
echo "    fw_syndrome     = ${pos_fw_syndrome:-?}"
echo "    fw_eqn          = ${pos_fw_eqn:-?}    (expect ${src_pre_fw_eqn:-?})"
echo "    fw_status       = ${pos_fw_status:-?}    (expect ${src_pre_fw_status:-?})"
echo "    fw_log_cq_size  = ${pos_fw_log_cq_size:-?}    (expect ${src_pre_fw_log_cq_size:-?})"
echo "    fw_log_page_size= ${pos_fw_log_page_size:-?}    (expect ${src_pre_fw_log_page_size:-?})"
echo "    fw_page_offset  = ${pos_fw_page_offset:-?}    (expect ${src_pre_fw_page_offset:-?})"
echo "    fw_oi           = ${pos_fw_oi:-?}    (expect ${src_pre_fw_oi:-?})"
echo "  Negative (cqn=$BOGUS_CQN on dst):"
echo "    fw_accept       = ${neg_fw_accept:-?}    (0 = FW gates QUERY_CQ)"
echo "    fw_syndrome     = ${neg_fw_syndrome:-?}"
echo

P=${pos_fw_accept:-?}
N=${neg_fw_accept:-?}

if [ "$P" = 0 ]; then
    echo "  HARD FAIL    P REJECTED -- the source CQ at cqn=$src_cqn"
    echo "               is gone post-LOAD on the destination."
    echo "               Model A for mlx5_ib_restore_cq is unworkable"
    echo "               on this FW; the §S5b plan must be revisited"
    echo "               (rebuild via fresh CREATE_CQ at the same"
    echo "               index instead of adopting)."
    echo "               P syndrome: ${pos_fw_syndrome:-?}"
    overall_rc=1
else
    eqn_ok=0; status_ok=0; lcs_ok=0; lps_ok=0; po_ok=0; oi_ok=0
    val_eq "${pos_fw_eqn:-0}"           "${src_pre_fw_eqn:-0}"           && eqn_ok=1
    val_eq "${pos_fw_status:-0}"        "${src_pre_fw_status:-0}"        && status_ok=1
    val_eq "${pos_fw_log_cq_size:-0}"   "${src_pre_fw_log_cq_size:-0}"   && lcs_ok=1
    val_eq "${pos_fw_log_page_size:-0}" "${src_pre_fw_log_page_size:-0}" && lps_ok=1
    val_eq "${pos_fw_page_offset:-0}"   "${src_pre_fw_page_offset:-0}"   && po_ok=1
    val_eq "${pos_fw_oi:-0}"            "${src_pre_fw_oi:-0}"            && oi_ok=1

    if [ "$eqn_ok"    = 1 ] && [ "$status_ok" = 1 ] && \
       [ "$lcs_ok"    = 1 ] && [ "$lps_ok"    = 1 ] && \
       [ "$po_ok"     = 1 ] && [ "$oi_ok"     = 1 ]; then
        if [ "$N" = 0 ]; then
            echo "  STRONG PASS  Source CQ alive on destination."
            echo "               (eqn, status, log_cq_size,"
            echo "               log_page_size, page_offset, oi)"
            echo "               byte-identical to source pre-SAVE"
            echo "               view. FW gates QUERY_CQ on existence"
            echo "               (negative control rejected)."
            echo "               Model A for mlx5_ib_restore_cq is sound."
        else
            echo "  WEAK PASS    Source CQ alive on destination."
            echo "               (eqn, status, log_cq_size,"
            echo "               log_page_size, page_offset, oi)"
            echo "               byte-identical to source pre-SAVE"
            echo "               view. FW does NOT gate QUERY_CQ"
            echo "               (negative control accepted);"
            echo "               negative control is meaningless on"
            echo "               this FW. Model A is still sound for"
            echo "               v0 -- positive content match is what"
            echo "               we need."
        fi
    else
        echo "  CONTENT MISMATCH  Source cqn survived post-LOAD (P=1)"
        echo "                    but its cqc context differs from the"
        echo "                    source's pre-SAVE view:"
        [ "$eqn_ok"    = 0 ] && echo "                    fw_eqn:           ${pos_fw_eqn:-?} != ${src_pre_fw_eqn:-?}    *** load-bearing ***"
        [ "$status_ok" = 0 ] && echo "                    fw_status:        ${pos_fw_status:-?} != ${src_pre_fw_status:-?}"
        [ "$lcs_ok"    = 0 ] && echo "                    fw_log_cq_size:   ${pos_fw_log_cq_size:-?} != ${src_pre_fw_log_cq_size:-?}"
        [ "$lps_ok"    = 0 ] && echo "                    fw_log_page_size: ${pos_fw_log_page_size:-?} != ${src_pre_fw_log_page_size:-?}"
        [ "$po_ok"     = 0 ] && echo "                    fw_page_offset:   ${pos_fw_page_offset:-?} != ${src_pre_fw_page_offset:-?}"
        [ "$oi_ok"     = 0 ] && echo "                    fw_oi:            ${pos_fw_oi:-?} != ${src_pre_fw_oi:-?}"
        echo "                    eqn drift = adopted CQ delivers on"
        echo "                    the wrong EQ (mute or wrong-vector)."
        echo "                    Other drifts = ring shape / flag bug."
        echo "                    This is a kernel SAVE/LOAD bug, not"
        echo "                    Model A unsoundness. Inspect the"
        echo "                    cq-state replay path in vfmig.c"
        echo "                    around LOAD_VHCA_STATE."
        overall_rc=1
    fi
fi
echo "====================================================="

# --- manifest -------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# cq_adopt manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_cqn=$src_cqn
S_fw_accept=${src_pre_fw_accept:-?}
S_fw_eqn=${src_pre_fw_eqn:-?}
S_fw_status=${src_pre_fw_status:-?}
S_fw_log_cq_size=${src_pre_fw_log_cq_size:-?}
S_fw_log_page_size=${src_pre_fw_log_page_size:-?}
S_fw_page_offset=${src_pre_fw_page_offset:-?}
S_fw_oi=${src_pre_fw_oi:-?}
S_fw_cqe_sz=${src_pre_fw_cqe_sz:-?}
S_fw_apu_cq=${src_pre_fw_apu_cq:-?}
S_fw_uar_page=${src_pre_fw_uar_page:-?}
S_fw_dbr_addr=${src_pre_fw_dbr_addr:-?}
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV
P_fw_accept=${pos_fw_accept:-?}
P_fw_syndrome=${pos_fw_syndrome:-?}
P_fw_eqn=${pos_fw_eqn:-?}
P_fw_status=${pos_fw_status:-?}
P_fw_log_cq_size=${pos_fw_log_cq_size:-?}
P_fw_log_page_size=${pos_fw_log_page_size:-?}
P_fw_page_offset=${pos_fw_page_offset:-?}
P_fw_oi=${pos_fw_oi:-?}
N_fw_accept=${neg_fw_accept:-?}
N_fw_syndrome=${neg_fw_syndrome:-?}
EOF

echo
echo "manifest: $META"
echo "blob:     $BLOB"
exit "$overall_rc"
