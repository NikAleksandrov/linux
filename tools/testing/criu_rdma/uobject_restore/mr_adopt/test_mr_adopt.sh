#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S4b empirical: validate that Model A for mlx5_ib_restore_mr is
# sound, i.e. the source's user-mode MKEY at index N survives
# LOAD_VHCA_STATE on the destination intact (same pd, length,
# start_addr).
#
# This is the MR analogue of test_pd_adopt.sh. The two answer
# different but related questions:
#
#   test_pd_adopt.sh   -- given a fresh ucontext on the destination,
#                         can FW accept a CREATE_MKEY against the
#                         source's pdn? Tests the (pd, uid) FW
#                         binding under a chosen uid_hint (uid=0
#                         for the v0 critical path).
#
#   test_mr_adopt.sh   -- given the source's mkey_index, does the
#                         destination FW still have that mkey alive
#                         post-LOAD, and is its mkc context
#                         (pd, length, start_addr) byte-identical
#                         to what the source wrote pre-SAVE?
#
# These are independent gates; both must hold for Model A
# mlx5_ib_restore_mr to be sound. test_pd_adopt.sh established the
# PD half (WEAK PASS on uid=0 lane, validated by S3b's empirical
# chain). This script establishes the MR half.
#
# Why we care
# -----------
# mlx5_ib_restore_mr in Model A wraps a kernel-side mlx5_ib_mr
# around the source's already-existing mkey_index without issuing
# a fresh CREATE_MKEY against the destination VHCA. For that to
# work:
#
#   (i)  The mkey at index=src_mkey_index must still exist in
#        the destination FW post-LOAD (FW table not wiped).
#   (ii) Its mkc.pd must equal src_pdn (PD reference preserved).
#   (iii) Its mkc.start_addr must equal src_iova (IOVA preserved).
#   (iv)  Its mkc.len must equal src_length (length preserved).
#
# (i) is the critical gate. (ii)-(iv) are byte-equality checks
# that catch a different class of bug: SAVE/LOAD partial restore.
# A failure of (ii)-(iv) with (i) holding means the kernel-side
# SAVE_VHCA_STATE / LOAD_VHCA_STATE flow corrupted the mkey
# context, not that Model A is unsound.
#
# Verdict matrix
# --------------
# Notation: P = PROBE_MKEY(src_mkey_index); N = PROBE_MKEY(BOGUS).
# Each cell is fw_accept (1=accept, 0=reject) plus a comparison
# of P's (fw_pd, fw_start_addr, fw_length) against the source's
# (src_pdn, src_mr_addr, src_mr_length).
#
#   P_accept | N_accept | content match | meaning
#   ---------+----------+---------------+-------------------------
#       1    |    0     | yes           | STRONG PASS: mkey alive,
#                                         FW gates QUERY_MKEY on
#                                         existence, all fields
#                                         preserved. Model A
#                                         confirmed.
#       1    |    1     | yes           | WEAK PASS: mkey alive,
#                                         FW does NOT gate
#                                         QUERY_MKEY (host-priv);
#                                         all fields preserved.
#                                         The negative control is
#                                         meaningless on this FW
#                                         but Model A is still
#                                         sound for v0 (correct
#                                         content is what we need).
#       1    |    *     | no            | CONTENT MISMATCH:
#                                         mkey survived but its
#                                         mkc was not byte-
#                                         preserved across LOAD.
#                                         Kernel SAVE/LOAD bug,
#                                         not Model A unsound.
#                                         Inspect fw_pd,
#                                         fw_start_addr, fw_length
#                                         vs source values.
#       0    |    *     | n/a           | HARD FAIL: source mkey
#                                         was wiped post-LOAD.
#                                         Model A unworkable; need
#                                         a different design (e.g.
#                                         rebuild via fresh
#                                         CREATE_MKEY at the same
#                                         index).
#
# Flow
# ----
#   Phase A  Provision source VF, set_tracked + enable_migratable,
#            bind.
#   Phase B  Fork fw_id_continuity_probe on the source ibdev; it
#            allocates {PD, CQ, QP, MR, SRQ}. Capture src_pdn,
#            src_mkey_index, src_mr_addr, src_mr_length from its
#            READY-line dictionary. Probe stays alive holding the
#            resources through SAVE.
#   Phase C  SAVE_VHCA_STATE on the source.
#   Phase D  Quit the source probe; tear down the source VF.
#   Phase E  Provision a fresh destination VF: set_tracked,
#            enable_migratable, LOAD_VHCA_STATE, mark_restored,
#            bind.
#   Phase F  ===== THE TEST =====
#            Issue PROBE_MKEY(dst_vf, src_mkey_index) and read
#            fw_accept= + fw_pd= + fw_start_addr= + fw_length=.
#            Expected: fw_accept=1 with all three byte-equal to
#            the source values captured in Phase B.
#   Phase G  Negative control. PROBE_MKEY with a bogus mkey_index
#            (default 0xffff00). Expected: fw_accept=0 with FW's
#            "invalid mkey" syndrome IF FW gates QUERY_MKEY on
#            existence; on a permissive FW, fw_accept=1 with
#            zeroed-out content (== WEAK PASS overall).
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_mr_adopt.sh
#
# Optional knobs (mirror test_pd_adopt.sh):
#   BLOB                Path for the SAVE blob.
#   TOOL                mlx5_vfmig CLI.
#   PROBE               fw_id_continuity_probe binary.
#   BOGUS_MKEY_INDEX    Index for the negative control (default
#                       0xffff00 -- well above any plausible
#                       per-VHCA mkey allocation in this short-
#                       lived test).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe}
BLOB=${BLOB:-/tmp/vf_mr_adopt.blob}
META=${META:-${BLOB}.meta}
BOGUS_MKEY_INDEX=${BOGUS_MKEY_INDEX:-$((0xffff00))}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]  || { echo "build $TOOL first: make -C $ROOT_DIR";  exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first: make -C $ROOT_DIR"; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

WORKDIR=$(mktemp -d -t mradopt.XXXXXX)
cleanup() {
    if [ -n "${SRC_PROBE_PID:-}" ] && kill -0 "$SRC_PROBE_PID" 2>/dev/null; then
        echo "quit" > "$WORKDIR/src.in" 2>/dev/null || true
        wait "$SRC_PROBE_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

# --- helpers (copy-paste from test_pd_adopt.sh; intentional since
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

# Run `mlx5_vfmig $PF probe_mkey $vf $mkey_index`, capture each
# key=value into "$prefix_<key>" variables in the caller's scope.
probe_mkey_into() {
    local label=$1 vf=$2 mkey_index=$3 prefix=$4
    local out line k v
    echo "=== PROBE_MKEY ($label): vf=$vf mkey_index=$mkey_index ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_mkey "$vf" "$mkey_index" 2>&1); then
        echo "$out" | sed 's/^/  /'
        echo "  PROBE_MKEY ioctl failed (transport / arg error)"
        return 1
    fi
    while IFS= read -r line; do
        echo "  [$label probe_mkey] $line"
        case "$line" in
            *=*) k="${line%%=*}"; v="${line#*=}";
                 eval "${prefix}_${k}=\"\$v\"" ;;
        esac
    done <<<"$out"
    return 0
}

# Numeric-equality on values captured by probe_mkey_into. Both args
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
sleep 1
SRC_IBDEV=$(find_ib_dev_for_pci "$SRC_VF") || { echo "FAIL: no ibdev for source $SRC_VF"; exit 1; }
echo "source ibdev: $SRC_IBDEV"

# --- Phase B ---------------------------------------------------------

echo "=== Phase B: source probe (registers MR, holds it across SAVE) ==="
sudo dmesg -C
start_src_probe "$SRC_IBDEV"
echo "captured: src_pdn=$src_pdn"
echo "captured: src_mkey_index=$src_mkey_index"
echo "captured: src_mr_addr=$src_mr_addr"
echo "captured: src_mr_length=$src_mr_length"
echo "captured: src_lkey=$src_lkey src_rkey=$src_rkey"

# --- Phase C ---------------------------------------------------------

echo "=== Phase C: SAVE_VHCA_STATE ==="
sudo dmesg -C
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
echo "================ Phase F: PROBE_MKEY (existence + content) =="
echo "Question: does the FW still have the source mkey at index"
echo "$src_mkey_index alive on the destination, and does its mkc"
echo "context match the source's pre-SAVE view?"
probe_mkey_into positive 0 "$src_mkey_index" pos

# --- Phase G: negative control --------------------------------------

echo
echo "================ Phase G: NEGATIVE CONTROL =================="
echo "Question: does PROBE_MKEY correctly REJECT a bogus mkey_index?"
echo "(If yes, the positive result above wasn't just FW returning"
echo "success for everything host-priv asks about.)"
probe_mkey_into bogus 0 "$BOGUS_MKEY_INDEX" neg

# --- verdict --------------------------------------------------------

overall_rc=0

echo
echo "================ MR ADOPT VERDICT ==================="
echo "  src_pdn          = $src_pdn"
echo "  src_mkey_index   = $src_mkey_index"
echo "  src_mr_addr      = $src_mr_addr"
echo "  src_mr_length    = $src_mr_length"
echo "  bogus_mkey_index = $BOGUS_MKEY_INDEX"
echo
echo "  Positive (mkey_index=$src_mkey_index):"
echo "    fw_accept    = ${pos_fw_accept:-?}    (1 = mkey present in dest FW)"
echo "    fw_syndrome  = ${pos_fw_syndrome:-?}"
echo "    fw_pd        = ${pos_fw_pd:-?}    (expect $src_pdn)"
echo "    fw_start_addr= ${pos_fw_start_addr:-?}"
echo "    (expect          $src_mr_addr)"
echo "    fw_length    = ${pos_fw_length:-?}"
echo "    (expect          $src_mr_length)"
echo "  Negative (mkey_index=$BOGUS_MKEY_INDEX):"
echo "    fw_accept    = ${neg_fw_accept:-?}    (0 = FW gates QUERY_MKEY)"
echo "    fw_syndrome  = ${neg_fw_syndrome:-?}"
echo

P=${pos_fw_accept:-?}
N=${neg_fw_accept:-?}

if [ "$P" = 0 ]; then
    echo "  HARD FAIL    P REJECTED -- the source mkey at index"
    echo "               $src_mkey_index is gone post-LOAD on the"
    echo "               destination. Model A for mlx5_ib_restore_mr"
    echo "               is unworkable on this FW; the §S4b plan must"
    echo "               be revisited (rebuild via fresh CREATE_MKEY"
    echo "               at the same index instead of adopting)."
    echo "               P syndrome: ${pos_fw_syndrome:-?}"
    overall_rc=1
else
    pd_ok=0; addr_ok=0; len_ok=0
    val_eq "${pos_fw_pd:-0}"         "${src_pdn:-0}"          && pd_ok=1
    val_eq "${pos_fw_start_addr:-0}" "${src_mr_addr:-0}"      && addr_ok=1
    val_eq "${pos_fw_length:-0}"     "${src_mr_length:-0}"    && len_ok=1

    if [ "$pd_ok" = 1 ] && [ "$addr_ok" = 1 ] && [ "$len_ok" = 1 ]; then
        if [ "$N" = 0 ]; then
            echo "  STRONG PASS  Source mkey alive on destination,"
            echo "               (pd, start_addr, length) byte-identical"
            echo "               to source view. FW gates QUERY_MKEY on"
            echo "               existence (negative control rejected)."
            echo "               Model A for mlx5_ib_restore_mr is sound."
        else
            echo "  WEAK PASS    Source mkey alive on destination,"
            echo "               (pd, start_addr, length) byte-identical"
            echo "               to source view. FW does NOT gate"
            echo "               QUERY_MKEY (negative control accepted);"
            echo "               negative control is meaningless on this"
            echo "               FW. Model A is still sound for v0 --"
            echo "               positive content match is what we need."
        fi
    else
        echo "  CONTENT MISMATCH  Source mkey survived post-LOAD (P=1)"
        echo "                    but its mkc context differs from the"
        echo "                    source's pre-SAVE view:"
        [ "$pd_ok"   = 0 ] && echo "                    fw_pd:        ${pos_fw_pd:-?} != $src_pdn"
        [ "$addr_ok" = 0 ] && echo "                    fw_start_addr: ${pos_fw_start_addr:-?} != $src_mr_addr"
        [ "$len_ok"  = 0 ] && echo "                    fw_length:    ${pos_fw_length:-?} != $src_mr_length"
        echo "                    This is a kernel SAVE/LOAD bug, not"
        echo "                    Model A unsoundness. Inspect the"
        echo "                    mkey-state replay path in vfmig.c"
        echo "                    around LOAD_VHCA_STATE."
        overall_rc=1
    fi
fi
echo "====================================================="

# --- manifest -------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# mr_adopt manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_pdn=$src_pdn
src_mkey_index=$src_mkey_index
src_mr_addr=$src_mr_addr
src_mr_length=$src_mr_length
src_lkey=$src_lkey
src_rkey=$src_rkey
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV
P_fw_accept=${pos_fw_accept:-?}
P_fw_syndrome=${pos_fw_syndrome:-?}
P_fw_pd=${pos_fw_pd:-?}
P_fw_start_addr=${pos_fw_start_addr:-?}
P_fw_length=${pos_fw_length:-?}
N_fw_accept=${neg_fw_accept:-?}
N_fw_syndrome=${neg_fw_syndrome:-?}
EOF

echo
echo "manifest: $META"
echo "blob:     $BLOB"
exit "$overall_rc"
