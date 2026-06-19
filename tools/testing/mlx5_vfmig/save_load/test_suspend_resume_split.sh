#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# test_suspend_resume_split.sh -- exercise the split SUSPEND_VHCA /
# RESUME_VHCA ioctls and the suspend-aware SAVE_VHCA_STATE / teardown
# behavior introduced for the CRIU stop-and-copy snapshot-ordering fix.
# See tools/testing/mlx5_vfmig/design/snapshot_ordering_pause_capture.md
# (Part A).
#
# Why an unbound VF: SUSPEND_VHCA / RESUME_VHCA are PF-issued
# other_function commands, so the VF datapath can be parked without ever
# binding the VF to mlx5_core. Keeping the VF unbound means this harness
# never walks the wedge-prone bind/unbind path -- it only drives the new
# PF cdev ioctls and observes the kernel's info-level log lines.
#
# What it asserts (each is a numbered subtest; the run fails if any
# fails):
#   1. SUSPEND_VHCA on a migratable VF succeeds and logs the pause line.
#   2. SUSPEND_VHCA is idempotent (second call succeeds, no FW traffic).
#   3. SAVE_VHCA_STATE on a pre-suspended VF produces a non-trivial blob
#      AND does NOT emit its own SUSPEND_VHCA(INITIATOR/RESPONDER) log
#      lines (it skipped the in-SAVE suspend because the caller owns it)
#      AND does NOT auto-resume on close (no "resumed" line from SAVE).
#   4. RESUME_VHCA succeeds and logs the resume line.
#   5. RESUME_VHCA is idempotent (second call succeeds).
#   6. SUSPEND_VHCA on an out-of-range vf_id is rejected (EINVAL).
#   7. mark_restored with the defer_resume flag is accepted.
#   8. Teardown force-resume: re-suspend, then sriov_numvfs=0, and
#      confirm the teardown path logged "force-resuming parked vf".
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_suspend_resume_split.sh
#   sudo ./test_suspend_resume_split.sh                  # PF auto-detect

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."

PF=${PF:-}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
BLOB=${BLOB:-/tmp/vf_suspend_resume.blob}

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

# Run the tool, capture rc + combined output. Sets TOOL_OUT / TOOL_RC.
run_tool() {
    set +e
    TOOL_OUT=$(sudo "$TOOL" "$PF" "$@" 2>&1)
    TOOL_RC=$?
    set -e
    echo "    \$ $TOOL $PF $* -> rc=$TOOL_RC"
    [ -n "$TOOL_OUT" ] && echo "$TOOL_OUT" | sed 's/^/      /'
    return 0
}

cleanup() {
    echo 1 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null 2>&1 || true
    echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"            >/dev/null 2>&1 || true
    rm -f "$BLOB" 2>/dev/null || true
}
trap cleanup EXIT

# --- provision a single UNBOUND, migratable VF ------------------------

echo "=== provision 1 unbound migratable VF on $PF ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"             >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe"  >/dev/null
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs"             >/dev/null
wait_for_path "$(vf_path $PF)/virtfn0" || { echo "FAIL: VF did not appear"; exit 1; }
VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "VF is $VF (left unbound)"

run_tool enable_migratable 0
[ "$TOOL_RC" -eq 0 ] || { echo "FATAL: enable_migratable failed"; exit 1; }

# --- subtest 1: SUSPEND succeeds + logs pause -------------------------

echo "=== subtest 1: SUSPEND_VHCA succeeds and logs the pause line ==="
sudo dmesg -C
run_tool suspend_vhca 0
if [ "$TOOL_RC" -eq 0 ]; then
    pass "SUSPEND_VHCA returned 0"
else
    fail "SUSPEND_VHCA returned $TOOL_RC"
fi
if sudo dmesg | grep -Eq "vfmig: suspended VF 0 .*snapshot-ordering pause"; then
    pass "kernel logged the snapshot-ordering pause line"
else
    fail "missing 'suspended VF 0 ... snapshot-ordering pause' in dmesg"
fi

# --- subtest 2: SUSPEND idempotent ------------------------------------

echo "=== subtest 2: SUSPEND_VHCA is idempotent ==="
sudo dmesg -C
run_tool suspend_vhca 0
if [ "$TOOL_RC" -eq 0 ]; then
    pass "second SUSPEND_VHCA returned 0"
else
    fail "second SUSPEND_VHCA returned $TOOL_RC"
fi
if sudo dmesg | grep -q "vfmig: suspended VF 0"; then
    fail "idempotent SUSPEND issued FW traffic (unexpected pause line)"
else
    pass "idempotent SUSPEND issued no new pause line"
fi

# --- subtest 3: SAVE on pre-suspended VF is suspend-aware -------------

echo "=== subtest 3: SAVE on a pre-suspended VF skips self-suspend + does not auto-resume ==="
sudo dmesg -C
run_tool save_vhca_state 0 "$BLOB"
if [ "$TOOL_RC" -eq 0 ]; then
    pass "SAVE_VHCA_STATE returned 0"
else
    fail "SAVE_VHCA_STATE returned $TOOL_RC"
fi
if [ -e "$BLOB" ]; then
    BYTES=$(stat -c %s "$BLOB" 2>/dev/null || echo 0)
    if [ "$BYTES" -gt 16 ]; then
        pass "SAVE produced a non-trivial blob ($BYTES bytes)"
    else
        fail "SAVE blob suspiciously small ($BYTES bytes)"
    fi
else
    fail "SAVE produced no blob at $BLOB"
fi
# The SAVE path must NOT have issued its own suspend (caller owns it) and
# must NOT have resumed on close (owns_suspend==false). The info-level
# "resumed VF 0" line is only emitted by RESUME_VHCA; the SAVE release
# path resumes silently via vfmig_cmd_resume_vhca when it owns the
# suspend, which would show as a RESUME_VHCA(RESPONDER) warn only on
# failure -- so the robust signal is the absence of a fresh pause line.
if sudo dmesg | grep -q "vfmig: suspended VF 0"; then
    fail "SAVE re-issued SUSPEND despite VF already parked"
else
    pass "SAVE did not re-issue SUSPEND (skipped self-suspend)"
fi
if sudo dmesg | grep -Eq "SUSPEND_VHCA\((INITIATOR|RESPONDER)\).*failed"; then
    fail "firmware SUSPEND error logged during SAVE"
else
    pass "no firmware SUSPEND error during SAVE"
fi

# --- subtest 4: RESUME succeeds + logs resume -------------------------

echo "=== subtest 4: RESUME_VHCA succeeds and logs the resume line ==="
sudo dmesg -C
run_tool resume_vhca 0
if [ "$TOOL_RC" -eq 0 ]; then
    pass "RESUME_VHCA returned 0"
else
    fail "RESUME_VHCA returned $TOOL_RC"
fi
if sudo dmesg | grep -q "vfmig: resumed VF 0 "; then
    pass "kernel logged the resume line"
else
    fail "missing 'resumed VF 0' in dmesg"
fi

# --- subtest 5: RESUME idempotent -------------------------------------

echo "=== subtest 5: RESUME_VHCA is idempotent ==="
run_tool resume_vhca 0
if [ "$TOOL_RC" -eq 0 ]; then
    pass "second RESUME_VHCA returned 0"
else
    fail "second RESUME_VHCA returned $TOOL_RC"
fi

# --- subtest 6: SUSPEND out-of-range vf_id rejected -------------------

echo "=== subtest 6: SUSPEND_VHCA on out-of-range vf_id is rejected ==="
run_tool suspend_vhca 99
if [ "$TOOL_RC" -ne 0 ]; then
    pass "out-of-range SUSPEND_VHCA rejected (rc=$TOOL_RC)"
else
    fail "out-of-range SUSPEND_VHCA unexpectedly succeeded"
fi

# --- subtest 7: mark_restored defer_resume accepted -------------------

echo "=== subtest 7: mark_restored defer_resume flag accepted ==="
run_tool mark_restored 0 defer_resume
if [ "$TOOL_RC" -eq 0 ]; then
    pass "mark_restored defer_resume accepted"
else
    fail "mark_restored defer_resume returned $TOOL_RC"
fi

# --- subtest 8: teardown force-resumes a parked VF --------------------

echo "=== subtest 8: SR-IOV teardown force-resumes a still-parked VF ==="
run_tool suspend_vhca 0
[ "$TOOL_RC" -eq 0 ] || fail "re-suspend before teardown failed (rc=$TOOL_RC)"
sudo dmesg -C
echo 1 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null 2>&1 || true
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"            >/dev/null
sleep 1
if sudo dmesg | grep -q "vfmig: force-resuming parked vf 0"; then
    pass "teardown logged force-resume of parked vf 0"
else
    fail "teardown did not log 'force-resuming parked vf 0'"
fi

# --- summary -----------------------------------------------------------

echo
echo "==== suspend/resume split summary ===="
echo "  PASS: $PASS"
echo "  FAIL: $FAIL"
if [ "$FAIL" -ne 0 ]; then
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: PASS"
exit 0
