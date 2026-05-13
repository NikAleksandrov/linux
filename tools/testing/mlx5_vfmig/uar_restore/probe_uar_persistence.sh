#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Empirical probe: does LOAD_VHCA_STATE preserve the source's UAR id
# space, or does FW reallocate UARs from scratch on the destination?
#
# This is a single-host SAVE+LOAD round-trip on a tracked VF (so the
# vfmig deterministic IOVA + identity-on-the-wire path is exercised),
# with one extra mechanism: we enable dynamic_debug for the
# "post-alloc kernel bfreg" line in mlx5_load() and compare the
# kernel UAR id (bfreg.up->index) and bfreg slot (bfreg.index) the
# source got at first probe vs. what the destination got after LOAD.
#
# Three possible outcomes, all instructive:
#
#   - dst.uar.index == src.uar.index
#       FW preserved (or deterministically re-allocated to the same
#       id). R3's UAR-identity-preservation assumption is on solid
#       ground for the kernel UAR. We still need to probe the
#       user-facing bfreg/UAR pool (DEVX-allocated UARs in the user
#       process's libmlx5 context), but kernel UAR persistence is a
#       strong positive signal.
#
#   - dst.uar.index >  src.uar.index
#       FW kept the source's UARs reserved (the destination's
#       ALLOC_UAR landed above the source's high-water-mark). Same
#       practical implication as the equal case: source UARs still
#       exist on the destination and could be re-mapped at
#       RESTORE_CONTEXT time. Worth a follow-up: can we explicitly
#       request a specific UAR id at ALLOC_UAR time?
#
#   - dst.uar.index <= src.uar.index, dst != src
#       FW threw the source's UARs away on LOAD. R3's UAR-identity
#       design has to change; either we ask FW for an explicit
#       UAR-id-pin at allocation time, or we accept that user
#       doorbell instructions need rewriting at restore.
#
# Usage:
#   sudo PF=0000:08:00.0 ./probe_uar_persistence.sh
#
# Assumes the existing save_load/test_iova_tracked_save_load.sh has
# been built+installed with the matching mlx5_core/mlx5_ib that prints
# the bfreg log line. If you don't see "post-alloc kernel bfreg" in
# the output, dynamic_debug wasn't enabled or the modules in
# /lib/modules are stale.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."

PF=${PF:-0000:08:00.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
INNER_TEST=${INNER_TEST:-$ROOT_DIR/save_load/test_iova_tracked_save_load.sh}

if [ ! -x "$TOOL" ]; then
    echo "build $TOOL first: make -C $ROOT_DIR"; exit 1
fi
if [ ! -x "$INNER_TEST" ]; then
    echo "missing $INNER_TEST"; exit 1
fi

# --- Step 1: enable dbg for the bfreg line -----------------------------
#
# The probe line lives in mlx5_load() in main.c. Match by file+function
# rather than by message text so kernel updates that re-word the log
# don't silently break the experiment.

DD_FILE=/proc/dynamic_debug/control
[ -w "$DD_FILE" ] || { echo "$DD_FILE not writable; CONFIG_DYNAMIC_DEBUG?"; exit 1; }

echo "+++ enabling mlx5_load dbg lines +++"
# dynamic_debug filter keyword is `func`, not `function`; passing the
# wrong keyword is rejected by the kernel as -EINVAL with a useless
# "Invalid argument" tee error. Match by message-format prefix so a
# mlx5_load() refactor doesn't silently disable the probe.
echo 'format "vfmig: post-alloc kernel bfreg" +p' \
    | sudo tee "$DD_FILE" >/dev/null

# Belt-and-braces: also enable the L4-R1 dev_res restored skip line in
# mlx5_ib so we can confirm the restored gating fires too. Cheap and
# adjacent.
echo 'format "vfmig: restored VF -- skipping dev_res" +p' \
    | sudo tee "$DD_FILE" >/dev/null

# --- Step 2: drive the round-trip --------------------------------------
#
# We read the kernel log via `journalctl --dmesg --since=$START` rather
# than plain `dmesg`. This is deliberate: test_iova_tracked_save_load.sh issues
# `dmesg -C` between Phase A and Phase D to keep its own per-phase
# tails legible, which would otherwise wipe the source's bfreg log
# before this script has a chance to read it. journald's kmsg journal
# is *not* affected by `dmesg -C` and keeps the full kernel-log
# history regardless. If journald is not running on this box
# (`systemctl is-active systemd-journald` says inactive) the script
# falls back to plain dmesg with a loud warning -- that case is the
# only one in which a single-host run can lose the source reading.

START_TS=$(date '+%Y-%m-%d %H:%M:%S')
USE_JOURNAL=0
if systemctl is-active --quiet systemd-journald 2>/dev/null; then
    USE_JOURNAL=1
fi

echo "+++ running $(basename $INNER_TEST) ROLE=both PROBE_UID=1 +++"
sudo dmesg -C
# PROBE_UID=1 makes the harness call `mlx5_vfmig probe_uid 0` after
# both the source bind (Phase A) and the destination bind (Phase D),
# emitting "[probe_uid src]" / "[probe_uid dst]" lines we can parse
# alongside the bfreg log to answer both the UAR and UID persistence
# questions in a single round-trip.
sudo PF="$PF" ROLE=both PROBE_UID=1 "$INNER_TEST" | tee /tmp/probe_uar_test.log

read_kmsg() {
    if [ "$USE_JOURNAL" = "1" ]; then
        sudo journalctl --dmesg --since="$START_TS" --no-pager 2>/dev/null
    else
        sudo dmesg
    fi
}

# --- Step 3: extract the bfreg lines ----------------------------------
#
# Two "post-alloc kernel bfreg" lines are expected on a single-host
# round-trip:
#   - one from Phase A's source bind (no "(restored VF)" suffix)
#   - one from Phase D's destination bind (with "(restored VF)" suffix)

echo
echo "==== bfreg observations (raw kernel log) ===="
if ! read_kmsg | grep -E 'post-alloc kernel bfreg'; then
    echo "FAIL: no bfreg log lines in kernel log"
    echo "Likely causes:"
    echo "  - /lib/modules mlx5_core is stale (predates this experiment)"
    echo "  - dynamic_debug write was rejected (printed an error above)"
    echo "  - mlx5_load() ran before dynamic_debug took effect"
    if [ "$USE_JOURNAL" = "0" ]; then
        echo "  - systemd-journald is not running, so the source bfreg log"
        echo "    was almost certainly wiped by test_iova_tracked_save_load.sh's"
        echo "    in-Phase-D 'dmesg -C'. Enable journald or hand-instrument"
        echo "    test_iova_tracked_save_load.sh to avoid that."
    fi
    exit 1
fi

echo
echo "==== UAR persistence verdict ===="

# Pull the two readings. The destination one carries "(restored VF)";
# the source one does not.
SRC_LINE=$(read_kmsg | grep 'post-alloc kernel bfreg' | grep -v '(restored VF)' | tail -n1 || true)
DST_LINE=$(read_kmsg | grep 'post-alloc kernel bfreg' | grep    '(restored VF)' | tail -n1 || true)

if [ -z "$SRC_LINE" ] || [ -z "$DST_LINE" ]; then
    echo "WARN: missing one or both readings:"
    echo "  src: ${SRC_LINE:-<none>}"
    echo "  dst: ${DST_LINE:-<none>}"
    echo "Cannot render verdict."
    exit 1
fi

extract() { echo "$1" | sed -nE "s/.*$2=([0-9]+).*/\1/p"; }

SRC_UAR=$(extract "$SRC_LINE" "uar.index")
SRC_BFR=$(extract "$SRC_LINE" "bfreg.index")
DST_UAR=$(extract "$DST_LINE" "uar.index")
DST_BFR=$(extract "$DST_LINE" "bfreg.index")

echo "  source     : uar.index=$SRC_UAR bfreg.index=$SRC_BFR"
echo "  destination: uar.index=$DST_UAR bfreg.index=$DST_BFR"

if [ "$SRC_UAR" = "$DST_UAR" ]; then
    echo
    echo "PASS-A: kernel UAR id is identical pre-SAVE and post-LOAD."
    echo "        FW either preserved the source's UAR allocation or"
    echo "        deterministically re-allocated to the same id. Either"
    echo "        way the R3 UAR-identity assumption holds for the kernel"
    echo "        UAR. Worth a follow-up DEVX-side probe (ALLOC_UAR from"
    echo "        user space) to confirm the user-pool behaviour matches."
elif [ "$DST_UAR" -gt "$SRC_UAR" ]; then
    echo
    echo "PASS-B: destination UAR id is higher than source's. FW kept"
    echo "        the source's UARs reserved post-LOAD and handed us a"
    echo "        fresh id above the high-water-mark. Same practical"
    echo "        implication as PASS-A for R3 (the source's user UARs"
    echo "        could be re-mapped at RESTORE_CONTEXT time), with the"
    echo "        added requirement that mlx5_alloc_bfreg() needs a way"
    echo "        to pin a specific id rather than take what FW hands"
    echo "        out. Open a follow-up to investigate ALLOC_UAR's id"
    echo "        argument."
else
    echo
    echo "FAIL  : destination UAR id ($DST_UAR) is lower than source's"
    echo "        ($SRC_UAR). FW most likely tore down the source's UARs"
    echo "        on LOAD_VHCA_STATE. R3's UAR-identity-preservation"
    echo "        story has to change. Likely options:"
    echo "          (a) ask FW team for explicit UAR-id-pin at"
    echo "              ALLOC_UAR (private-driver request)"
    echo "          (b) restore-time UAR remap with doorbell rewriting"
    echo "              in libmlx5 (unpalatable -- libibverbs ABI)"
    echo "          (c) capture-and-replay UAR ids inside the vfmig blob"
    echo "              (extends LOAD_VHCA_STATE semantics)"
fi

# --- Step 4: UID persistence verdict (PROBE_UID=1 hook) ---------------
#
# test_iova_tracked_save_load.sh emitted three `[probe_uid src|dst] vf 0: probe_uid -> uid=N`
# lines on stdout (which we tee'd to /tmp/probe_uar_test.log):
#   - two on the source side (back-to-back to read the allocator's
#     monotonic step / reuse behaviour),
#   - one on the destination side post-LOAD.
#
# Comparison rubric is the same shape as the UAR one: dst > src_max
# means FW preserves the uctx table across LOAD; dst <= src_min means
# FW reset it.

echo
echo "==== uctx (uid) persistence verdict ===="
if [ ! -s /tmp/probe_uar_test.log ]; then
    echo "WARN: /tmp/probe_uar_test.log empty -- can't read probe_uid lines"
else
    SRC_UIDS=$(grep -E '\[probe_uid src\]' /tmp/probe_uar_test.log | \
               sed -nE 's/.*uid=([0-9]+).*/\1/p')
    DST_UID=$(grep -E '\[probe_uid dst\]' /tmp/probe_uar_test.log  | \
              sed -nE 's/.*uid=([0-9]+).*/\1/p' | tail -n1)

    if [ -z "$SRC_UIDS" ] || [ -z "$DST_UID" ]; then
        echo "WARN: missing one or both readings:"
        echo "  src: ${SRC_UIDS:-<none>}"
        echo "  dst: ${DST_UID:-<none>}"
    else
        SRC_MIN=$(echo "$SRC_UIDS" | sort -n | head -n1)
        SRC_MAX=$(echo "$SRC_UIDS" | sort -n | tail -n1)
        echo "  source     : uids=$(echo $SRC_UIDS | tr '\n' ' ')"
        echo "  destination: uid=$DST_UID"

        if   [ "$DST_UID" -gt "$SRC_MAX" ]; then
            echo
            echo "PASS-A: destination uid ($DST_UID) is strictly above the"
            echo "        source's high-water-mark ($SRC_MAX). FW kept the"
            echo "        source's uctx table reserved across LOAD; uid"
            echo "        identity could be preserved at R3."
        elif [ "$DST_UID" = "$SRC_MAX" ] || [ "$DST_UID" = "$SRC_MIN" ]; then
            echo
            echo "AMBIG : destination uid ($DST_UID) lies inside the source's"
            echo "        observed range [$SRC_MIN..$SRC_MAX]. On a same-VHCA"
            echo "        single-host round-trip (this is one) FW could have"
            echo "        either (i) preserved + reused our just-released"
            echo "        slot, or (ii) reset the table and started fresh"
            echo "        from the same low number we got the first time."
            echo "        SINGLE-HOST CAN'T DISTINGUISH THESE. Re-run the"
            echo "        experiment cross-host (provision a fresh VF on a"
            echo "        different machine, LOAD into it) and compare:"
            echo "          - dst > src_max  -> FW preserves uctx table"
            echo "          - dst == 0/1      -> FW resets uctx table"
        else
            echo
            echo "FAIL  : destination uid ($DST_UID) is below the source's"
            echo "        minimum ($SRC_MIN). FW most likely reset the uctx"
            echo "        table on LOAD_VHCA_STATE. R3 cannot use FW uid"
            echo "        as the cross-migration identity."
        fi
    fi
fi
