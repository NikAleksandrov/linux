#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# test_directional_suspend_resume.sh -- exercise the DIRECTIONAL
# SUSPEND_VHCA / RESUME_VHCA ioctls (the MLX5_VFMIG_DIR_FLAG_* flags)
# that drive a VF's VHCA one step at a time along the firmware migration
# FSM ladder:
#
#     RUNNING(0) <--INITIATOR--> RUNNING_P2P(1) <--RESPONDER--> STOP(2)
#
# This is the kernel half of the cross-host directional suspend/resume
# (H1 / Part C) in
# tools/testing/criu_rdma/design/datapath_pause_resume.md. The legacy
# fused pair (flags == 0) is covered by test_suspend_resume_split.sh;
# here we validate the single-step semantics, the reached-state logging,
# and the FW-order rejections.
#
# Why an unbound VF: SUSPEND_VHCA / RESUME_VHCA are PF-issued
# other_function commands, so the ladder can be walked without ever
# binding the VF to mlx5_core -- the harness only drives the PF cdev
# ioctls and observes the kernel's info-level "datapath A->B" log lines.
#
# What it asserts (each is a numbered subtest; the run fails if any
# fails):
#   1. suspend initiator:  RUNNING -> P2P   (logs "datapath 0->1").
#   2. suspend initiator again is idempotent (rc 0, no new pause line).
#   3. suspend responder:  P2P -> STOP      (logs "datapath 1->2").
#   4. resume initiator while STOP is rejected (EINVAL, FW order).
#   5. resume responder:   STOP -> P2P      (logs "datapath 2->1").
#   6. resume initiator:   P2P -> RUNNING   (logs "datapath 1->0").
#   7. suspend responder while RUNNING is rejected (EINVAL, FW order).
#   8. an unknown direction flag bit is rejected (EINVAL).
#   9. legacy fused pair still works: suspend both (0->2) then resume
#      both (2->0), each logging the full-ladder transition.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_directional_suspend_resume.sh
#   sudo ./test_directional_suspend_resume.sh                # auto-detect

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."

PF=${PF:-}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}

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

# Assert the kernel logged a suspend/resume with a given "A->B" transition
# since the last "dmesg -C". $1 = suspend|resume, $2 = "0->1" etc.
saw_transition() {
    local kind="$1" arrow="$2" re
    if [ "$kind" = suspend ]; then
        re="vfmig: suspended VF 0 .*datapath ${arrow}"
    else
        re="vfmig: resumed VF 0 .*datapath ${arrow}"
    fi
    sudo dmesg | grep -Eq "$re"
}

cleanup() {
    # Best-effort return to RUNNING so teardown never stalls, then drop VFs.
    sudo "$TOOL" "$PF" resume_vhca 0 >/dev/null 2>&1 || true
    echo 1 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null 2>&1 || true
    echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"            >/dev/null 2>&1 || true
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

# --- subtest 1: suspend initiator RUNNING -> P2P ----------------------

echo "=== subtest 1: suspend initiator (RUNNING -> P2P) ==="
sudo dmesg -C
run_tool suspend_vhca 0 initiator
if [ "$TOOL_RC" -eq 0 ]; then
    pass "suspend initiator returned 0"
else
    fail "suspend initiator returned $TOOL_RC"
fi
if saw_transition suspend "0->1"; then
    pass "kernel logged datapath 0->1 (RUNNING -> P2P)"
else
    fail "missing 'datapath 0->1' pause line"
fi

# --- subtest 2: suspend initiator again is idempotent -----------------

echo "=== subtest 2: suspend initiator is idempotent at P2P ==="
sudo dmesg -C
run_tool suspend_vhca 0 initiator
if [ "$TOOL_RC" -eq 0 ]; then
    pass "second suspend initiator returned 0"
else
    fail "second suspend initiator returned $TOOL_RC"
fi
if sudo dmesg | grep -q "vfmig: suspended VF 0"; then
    fail "idempotent suspend issued FW traffic (unexpected pause line)"
else
    pass "idempotent suspend issued no new pause line"
fi

# --- subtest 3: suspend responder P2P -> STOP -------------------------

echo "=== subtest 3: suspend responder (P2P -> STOP) ==="
sudo dmesg -C
run_tool suspend_vhca 0 responder
if [ "$TOOL_RC" -eq 0 ]; then
    pass "suspend responder returned 0"
else
    fail "suspend responder returned $TOOL_RC"
fi
if saw_transition suspend "1->2"; then
    pass "kernel logged datapath 1->2 (P2P -> STOP)"
else
    fail "missing 'datapath 1->2' pause line"
fi

# --- subtest 4: resume initiator while STOP is rejected ---------------

echo "=== subtest 4: resume initiator while STOP is out of order (EINVAL) ==="
run_tool resume_vhca 0 initiator
if [ "$TOOL_RC" -ne 0 ]; then
    pass "resume initiator from STOP rejected (rc=$TOOL_RC)"
else
    fail "resume initiator from STOP unexpectedly succeeded"
fi

# --- subtest 5: resume responder STOP -> P2P --------------------------

echo "=== subtest 5: resume responder (STOP -> P2P) ==="
sudo dmesg -C
run_tool resume_vhca 0 responder
if [ "$TOOL_RC" -eq 0 ]; then
    pass "resume responder returned 0"
else
    fail "resume responder returned $TOOL_RC"
fi
if saw_transition resume "2->1"; then
    pass "kernel logged datapath 2->1 (STOP -> P2P)"
else
    fail "missing 'datapath 2->1' resume line"
fi

# --- subtest 6: resume initiator P2P -> RUNNING -----------------------

echo "=== subtest 6: resume initiator (P2P -> RUNNING) ==="
sudo dmesg -C
run_tool resume_vhca 0 initiator
if [ "$TOOL_RC" -eq 0 ]; then
    pass "resume initiator returned 0"
else
    fail "resume initiator returned $TOOL_RC"
fi
if saw_transition resume "1->0"; then
    pass "kernel logged datapath 1->0 (P2P -> RUNNING)"
else
    fail "missing 'datapath 1->0' resume line"
fi

# --- subtest 7: suspend responder while RUNNING is rejected -----------

echo "=== subtest 7: suspend responder while RUNNING is out of order (EINVAL) ==="
run_tool suspend_vhca 0 responder
if [ "$TOOL_RC" -ne 0 ]; then
    pass "suspend responder from RUNNING rejected (rc=$TOOL_RC)"
else
    fail "suspend responder from RUNNING unexpectedly succeeded"
fi

# --- subtest 8: unknown direction flag bit is rejected ----------------

echo "=== subtest 8: unknown direction flag bit is rejected (EINVAL) ==="
run_tool suspend_vhca 0 4
if [ "$TOOL_RC" -ne 0 ]; then
    pass "unknown flag bit 0x4 rejected (rc=$TOOL_RC)"
else
    fail "unknown flag bit 0x4 unexpectedly accepted"
fi

# --- subtest 9: legacy fused pair still works -------------------------

echo "=== subtest 9: fused suspend (0->2) then fused resume (2->0) ==="
sudo dmesg -C
run_tool suspend_vhca 0
if [ "$TOOL_RC" -eq 0 ] && saw_transition suspend "0->2"; then
    pass "fused suspend drove RUNNING -> STOP (datapath 0->2)"
else
    fail "fused suspend did not log 'datapath 0->2' (rc=$TOOL_RC)"
fi
sudo dmesg -C
run_tool resume_vhca 0
if [ "$TOOL_RC" -eq 0 ] && saw_transition resume "2->0"; then
    pass "fused resume drove STOP -> RUNNING (datapath 2->0)"
else
    fail "fused resume did not log 'datapath 2->0' (rc=$TOOL_RC)"
fi

# --- summary -----------------------------------------------------------

echo
echo "==== directional suspend/resume summary ===="
echo "  PASS: $PASS"
echo "  FAIL: $FAIL"
if [ "$FAIL" -ne 0 ]; then
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: PASS"
exit 0
