#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# test_teardown_resume_timing.sh -- reproduce and gate the slow SR-IOV
# teardown of a *bound* VF parked in the vfmig STOP state.
#
# The bug: mlx5_sriov_disable() used to run pci_disable_sriov() (the full
# per-VF teardown: mlx5_ib_remove -> DESTROY_QP/CQ/MKEY, DEALLOC_PD/UAR,
# DESTROY_UCTX, ... on the *VF's own* command ring) BEFORE the force-resume
# in mlx5_device_disable_sriov(). A VF left in STOP has a dead command ring
# (it needs >= RUNNING_P2P), so each of those ~15-20 commands blocked a full
# MLX5_CMD_TIMEOUT before failing -- 15-25 minutes per parked VF.
#
# The fix (drivers/.../mlx5/core/sriov.c) hoists mlx5_vfmig_pf_drop_suspends()
# to *before* pci_disable_sriov(), so each VF's ring is live (>= RUNNING_P2P)
# when its DESTROY commands run, and teardown completes in seconds.
#
# Unlike test_suspend_resume_split.sh (which keeps the VF UNBOUND and so
# never reproduces the stall), this harness BINDS the VF to mlx5_core so the
# per-VF DESTROY chain actually runs on the VF ring -- the only configuration
# in which the bug manifests. Every step that can hang on a wedged
# bind/unbind or a dead ring is wrapped in `timeout` so a regression FAILS
# fast instead of hanging this script for 20+ minutes.
#
# What it asserts:
#   1. The bound, migratable VF can be parked with SUSPEND_VHCA.
#   2. sriov_numvfs=0 teardown of that parked+bound VF completes well under
#      the regression threshold (TEARDOWN_MAX_S, default 90s; the unfixed
#      path took many minutes).
#   3. The teardown logged the force-resume warn for the parked VF.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_teardown_resume_timing.sh
#   sudo ./test_teardown_resume_timing.sh                  # PF auto-detect

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."

PF=${PF:-}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
# Generous: a fixed kernel tears down in a few seconds; the unfixed path was
# minutes. Anything under this clearly distinguishes fixed from regressed.
TEARDOWN_MAX_S=${TEARDOWN_MAX_S:-90}
# Hard ceiling so a true regression (multi-minute stall) can't hang the run.
TEARDOWN_TIMEOUT_S=${TEARDOWN_TIMEOUT_S:-300}

if [ -z "$PF" ]; then
    PF=$(ls /dev/mlx5_vfmig/ 2>/dev/null | head -n1)
fi
[ -n "$PF" ] || { echo "no PF cdev found under /dev/mlx5_vfmig/ (set PF=)"; exit 1; }

CDEV="/dev/mlx5_vfmig/$PF"
[ -e "$CDEV" ] || { echo "missing $CDEV (mlx5_core with CONFIG_MLX5_VFMIG loaded?)"; exit 1; }
[ -x "$TOOL" ] || { echo "build $TOOL first: make -C $ROOT_DIR"; exit 1; }

vf_path()       { echo "/sys/bus/pci/devices/$1"; }
wait_for_path() { for i in $(seq 1 50); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }

PASS=0
FAIL=0
pass() { echo "  PASS: $*"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }

run_tool() {
    set +e
    TOOL_OUT=$(sudo "$TOOL" "$PF" "$@" 2>&1)
    TOOL_RC=$?
    set -e
    echo "    \$ $TOOL $PF $* -> rc=$TOOL_RC"
    [ -n "$TOOL_OUT" ] && echo "$TOOL_OUT" | sed 's/^/      /'
    return 0
}

VF=""
cleanup() {
    # Best-effort: unbind, restore autoprobe, drop VFs. Guard the unbind
    # (it can stall on a wedged VF ring) so cleanup itself never hangs.
    if [ -n "$VF" ] && [ -e "/sys/bus/pci/drivers/mlx5_core/$VF" ]; then
        timeout 60 bash -c "echo '$VF' | sudo tee /sys/bus/pci/drivers/mlx5_core/unbind >/dev/null" 2>/dev/null || true
    fi
    [ -n "$VF" ] && echo "" | sudo tee "$(vf_path $VF)/driver_override" >/dev/null 2>&1 || true
    echo 1 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null 2>&1 || true
    timeout 300 bash -c "echo 0 | sudo tee '$(vf_path $PF)/sriov_numvfs' >/dev/null" 2>/dev/null || true
}
trap cleanup EXIT

# --- provision 1 VF, migratable, then bind it to mlx5_core ------------

echo "=== provision 1 migratable VF on $PF and bind it to mlx5_core ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"             >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe"  >/dev/null
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs"             >/dev/null
wait_for_path "$(vf_path $PF)/virtfn0" || { echo "FAIL: VF did not appear"; exit 1; }
VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "VF is $VF"

# migratable cap must be enabled before the VF binds.
run_tool enable_migratable 0
[ "$TOOL_RC" -eq 0 ] || { echo "FATAL: enable_migratable failed"; exit 1; }

# Bind while the VF is RUNNING (not parked): post-bind-park is the supported
# order; binding a paused VF is the QUERY_HCA_CAP-timeout path we avoid.
#
# Retry once: if a prior test (e.g. suspend_resume_split's mark_restored)
# left a stale "restored" marker on this vf index, the first probe takes the
# post-LOAD restore fast-path and fails QUERY_HCA_CAP with bad-system-state.
# That failed probe *consumes* the marker, so the second bind takes the
# normal probe path and succeeds. (numvfs recreate does not clear it.)
echo mlx5_core | sudo tee "$(vf_path $VF)/driver_override" >/dev/null
bound=0
for attempt in 1 2; do
    if timeout 120 bash -c "echo '$VF' | sudo tee /sys/bus/pci/drivers/mlx5_core/bind >/dev/null" 2>/dev/null \
        && [ -e "/sys/bus/pci/drivers/mlx5_core/$VF" ]; then
        bound=1
        break
    fi
    echo "  bind attempt $attempt failed (likely stale restored marker, now consumed); retrying"
    sleep 1
done
[ "$bound" -eq 1 ] || { echo "FATAL: bind of $VF to mlx5_core failed"; exit 1; }
echo "VF $VF bound to mlx5_core (datapath up)"

# --- subtest 1: park the bound VF -------------------------------------

echo "=== subtest 1: SUSPEND_VHCA parks the bound VF in STOP ==="
run_tool suspend_vhca 0
if [ "$TOOL_RC" -eq 0 ]; then
    pass "SUSPEND_VHCA on bound VF returned 0"
else
    fail "SUSPEND_VHCA on bound VF returned $TOOL_RC"
fi

# --- subtest 2: timed teardown of the parked, bound VF ----------------

echo "=== subtest 2: sriov_numvfs=0 teardown of a parked, bound VF is fast ==="
sudo dmesg -C
echo 1 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null 2>&1 || true

start=$(date +%s)
set +e
timeout "$TEARDOWN_TIMEOUT_S" bash -c "echo 0 | sudo tee '$(vf_path $PF)/sriov_numvfs' >/dev/null"
td_rc=$?
set -e
end=$(date +%s)
elapsed=$((end - start))
echo "    teardown took ${elapsed}s (rc=$td_rc, max=${TEARDOWN_MAX_S}s, hard timeout=${TEARDOWN_TIMEOUT_S}s)"

if [ "$td_rc" -eq 124 ]; then
    fail "teardown hit the ${TEARDOWN_TIMEOUT_S}s hard timeout (regression: stalled on dead VF ring)"
elif [ "$elapsed" -le "$TEARDOWN_MAX_S" ]; then
    pass "teardown completed in ${elapsed}s (<= ${TEARDOWN_MAX_S}s)"
else
    fail "teardown took ${elapsed}s (> ${TEARDOWN_MAX_S}s) -- force-resume likely not hoisted before pci_disable_sriov()"
fi
VF=""  # VFs gone; skip unbind in cleanup

# --- subtest 3: teardown warned about the parked VF -------------------

echo "=== subtest 3: teardown logged the force-resume warn ==="
if sudo dmesg | grep -q "vfmig: tearing down vf 0 while datapath-parked"; then
    pass "teardown warned + force-resumed parked vf 0"
else
    fail "teardown did not warn 'tearing down vf 0 while datapath-parked'"
fi

# --- summary -----------------------------------------------------------

echo
echo "==== teardown resume timing summary ===="
echo "  PASS: $PASS"
echo "  FAIL: $FAIL"
if [ "$FAIL" -ne 0 ]; then
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: PASS"
exit 0
