#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Regression probe for the IOVA-replay drift_armed gate, which is
# the kernel's structural answer to "what happens if userspace
# tries a second LOAD_VHCA_STATE on the same VHCA without an
# sriov_numvfs cycle". The first LOAD's parser arms
# dom->drift_armed=1 after every HOST_PAGE record has been
# parsed; vfmig_iova_replay_page() then refuses any further
# replay with WARN_ON_ONCE(dom->drift_armed) -> -EBUSY, which the
# parser surfaces as -EINVAL on the second write().
#
# What this validates
# -------------------
#   - vfmig_iova_replay_page (drivers/.../vfmig_iova.c line 1397):
#     a second LOAD on the same VHCA, with no sriov_numvfs cycle
#     in between, fails at the first HOST_PAGE record and never
#     issues LOAD_VHCA_STATE to firmware.
#   - mlx5_sriov_disable() recovers cleanly: sriov_numvfs=0
#     after the failed second LOAD drops the per-VF IOVA domain
#     (drift_armed goes with it), and a subsequent fresh
#     SAVE -> LOAD -> bind cycle works end-to-end. (The cycle
#     half is covered by the wrapped invocation of
#     test_iova_tracked_save_load.sh; this script asserts it
#     stays clean *after* the failed Scenario-B path.)
#
# What this does NOT validate
# ---------------------------
#   - Firmware behaviour on a hypothetical "DISABLE_HCA ->
#     ENABLE_HCA -> second LOAD" sequence. The drift_armed
#     gate fires before any second LOAD_VHCA_STATE issues, so
#     the FW question is moot for in-tree consumers. See
#     design/vf_prerestore_split.md §3.5.5.1 for the full table.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_multi_load_drift_gate.sh
#
# Exit codes:
#   0  drift gate fired as expected; recovery cycle clean
#   1  setup error
#   2  drift gate did NOT fire (regression: second LOAD succeeded
#      or failed with the wrong error)
#  77  the restored-VF unbind in cell 1 wedged the kernel (a firmware
#      command in the half-restored VHCA's mlx5e/flow-steering teardown
#      never completed). This is the documented restored-VF teardown
#      gap, not a drift-gate regression. The box must be rebooted before
#      further mlx5 work can succeed; run_all_harnesses.sh treats this
#      as a non-fatal WEDGE/SKIP and stops the sweep.

set -euo pipefail

PF=${PF:-0000:00:08.0}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
IOVA_TEST=${IOVA_TEST:-$SCRIPT_DIR/../test_iova_tracked_save_load.sh}
BLOB=${BLOB:-/tmp/vf_m2r_iova.blob}

[ -x "$TOOL" ]      || { echo "build $TOOL first: make -C $ROOT_DIR" >&2; exit 1; }
[ -x "$IOVA_TEST" ] || { echo "missing $IOVA_TEST" >&2; exit 1; }

PASS_COUNT=0
FAIL_COUNT=0
WEDGED=0

emit_pass() { echo "PASS: $*"; PASS_COUNT=$((PASS_COUNT + 1)); }
emit_fail() { echo "FAIL: $*"; FAIL_COUNT=$((FAIL_COUNT + 1)); }

cleanup() {
	# If cell 1 wedged the kernel, ANY sysfs write that touches the
	# stuck device (sriov_numvfs -> pci_disable_sriov walks the wedged
	# VF; even autoprobe is on the same PF) would itself block in
	# uninterruptible D state on the held device_lock chain. Leave the
	# rig exactly as-is and bail -- the operator must reboot.
	[ "$WEDGED" = 1 ] && return 0
	# Best-effort: leave the rig in numvfs=0 / autoprobe=1 so the
	# next test is set up to provision freshly. Ignore failures.
	echo 0 > "/sys/bus/pci/devices/$PF/sriov_numvfs" 2>/dev/null || true
	echo 1 > "/sys/bus/pci/devices/$PF/sriov_drivers_autoprobe" 2>/dev/null || true
}
trap cleanup EXIT

# Unbind a VF from its driver with a bounded wait -- the unbind-direction
# twin of bind_vf_safe() in test_iova_tracked_save_load.sh.
#
# A *restored* (LOAD_VHCA_STATE'd) VF is only functional for the L0-L3
# happy path; its mlx5e / flow-steering teardown issues firmware commands
# (mlx5_cmd_update_root_ft, *_VHCA, MANAGE_PAGES reclaim, ...) that the
# half-restored VHCA may be unable to service. A bare foreground
#
#     echo $VF > /sys/bus/pci/devices/$VF/driver/unbind
#
# then blocks in write() holding the device_lock chain while each stuck
# teardown command burns the full command timeout (MLX5_TO_CMD_MS = 60s)
# and "leaks a command resource" -- serially, once per command. Minutes
# of held locks back up the health poller and the console into a
# soft-lockup / RCU-stall storm, and the only recovery is a reboot.
#
# Backgrounding the write does NOT unwedge the kernel (the stuck task
# keeps the mutexes), but it lets THIS script bail cleanly so the suite
# records a result and the operator gets a "reboot required" signal
# instead of a silent wedge.
#
# Args:  $1 = VF BDF   $2 = timeout seconds (default 75, > 60s FW timeout)
# Returns: 0 unbound within the bound; 1 wedged (reboot required).
unbind_vf_safe() {
	local vf=$1 timeout=${2:-75} started bg
	started=$(date +%s)
	( echo "$vf" > "/sys/bus/pci/devices/$vf/driver/unbind" ) >/dev/null 2>&1 &
	bg=$!
	while :; do
		if [ ! -e "/sys/bus/pci/devices/$vf/driver" ]; then
			wait "$bg" 2>/dev/null || true
			return 0
		fi
		if ! kill -0 "$bg" 2>/dev/null; then
			# write() returned; give the symlink a tick to drop.
			sleep 0.2
			[ ! -e "/sys/bus/pci/devices/$vf/driver" ] && return 0
			return 1
		fi
		if [ $(( $(date +%s) - started )) -ge "$timeout" ]; then
			return 1
		fi
		sleep 0.5
	done
}

echo "==== drift_gate setup: SAVE + LOAD + bind via iova-tracked test ===="
PF="$PF" BLOB="$BLOB" ROLE=both "$IOVA_TEST" >/dev/null

VF=$(basename "$(readlink "/sys/bus/pci/devices/$PF/virtfn0")")
[ -e "/sys/bus/pci/devices/$VF/driver" ] || {
	echo "ERROR: post-setup VF $VF is not bound; cannot probe drift gate" >&2
	exit 1
}

echo "==== drift_gate cell 1: unbind, then second LOAD must fail ===="
sudo dmesg -C
if ! unbind_vf_safe "$VF" 75; then
	WEDGED=1
	cat >&2 <<EOF

WEDGE: unbind of restored VF $VF did not complete within 75s.

The half-restored VHCA's mlx5e / flow-steering teardown is stuck on a
firmware command that never completes (look for a
'mlx5_cmd_update_root_ft', '*_VHCA', or 'MANAGE_PAGES ... timeout' leak
in dmesg). The unbind holds the device_lock chain, so every subsequent
mlx5 operation -- sriov_numvfs=0, rmmod, other VF binds -- will also
block. A reboot is required before further mlx5 work can succeed.

This is the documented restored-VF teardown gap, not a drift-gate
regression. See bind_vf_safe() in test_iova_tracked_save_load.sh and
design/vf_prerestore_split.md. Skipping the rest of the probe.
EOF
	exit 77
fi
[ ! -e "/sys/bus/pci/devices/$VF/driver" ] || {
	emit_fail "VF $VF still bound after unbind"
	exit 2
}

# enable_migratable is idempotent and required by the LOAD ioctl gate.
"$TOOL" "$PF" enable_migratable 0 >/dev/null

set +e
SECOND_LOAD_OUT=$("$TOOL" "$PF" load_vhca_state 0 "$BLOB" 2>&1)
SECOND_LOAD_RC=$?
set -e

if [ "$SECOND_LOAD_RC" -eq 0 ]; then
	emit_fail "second LOAD on same VHCA succeeded; drift_armed gate did NOT fire"
	echo "  output: $SECOND_LOAD_OUT"
	exit 2
fi

# Userspace tool surfaces the kernel's -EINVAL as "Invalid argument".
if echo "$SECOND_LOAD_OUT" | grep -q 'Invalid argument'; then
	emit_pass "second LOAD rejected with -EINVAL (rc=$SECOND_LOAD_RC)"
else
	emit_fail "second LOAD rejected with unexpected error (rc=$SECOND_LOAD_RC)"
	echo "  output: $SECOND_LOAD_OUT"
fi

# Kernel logs the offending replay slot via dev_warn at vfmig_load_step
# line 3469 -- this fires every time, unlike the WARN_ON_ONCE in
# vfmig_iova_replay_page which is one-shot per kernel boot.
if sudo dmesg | grep -q 'replay_page.*failed: -16'; then
	emit_pass "kernel emitted replay_page -EBUSY (drift_armed) on second LOAD"
else
	emit_fail "expected 'replay_page ... failed: -16' in dmesg, not seen"
	echo "  recent dmesg:"
	sudo dmesg | tail -5 | sed 's/^/    /'
fi

echo "==== drift_gate cell 2: sriov cycle drops domain, fresh round-trip works ===="
echo 0 > "/sys/bus/pci/devices/$PF/sriov_numvfs"
sleep 0.5

set +e
PF="$PF" BLOB="$BLOB" ROLE=both "$IOVA_TEST" >/dev/null
RECOVERY_RC=$?
set -e

if [ "$RECOVERY_RC" -eq 0 ]; then
	emit_pass "post-failure sriov cycle -> fresh round-trip succeeded"
else
	emit_fail "post-failure recovery cycle failed (rc=$RECOVERY_RC)"
fi

echo
echo "==== drift_gate summary: $PASS_COUNT pass, $FAIL_COUNT fail ===="
[ "$FAIL_COUNT" -eq 0 ] || exit 2
exit 0
