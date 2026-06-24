#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# vf_uuid lifecycle + multi-VF orchestration harness for KS7.3
# (design/vf_prerestore_split.md §3.5).
#
# Covers the cells the C probe (vf_uuid_probe_mlx5_vfmig) cannot
# reach in-process because they require an SR-IOV teardown / re-enable
# cycle or a multi-VF batch:
#
#   clear_on_teardown    Stamp UUID U on a slot; sriov_numvfs=0 +
#                        sriov_numvfs=N; QUERY shows the slot's
#                        vf_uuid back to all-zeros.
#
#   stamp_after_recycle  After a teardown that cleared U, stamp a
#                        *different* UUID V on the same slot;
#                        succeeds (no stale-stamp -EBUSY). This
#                        cell validates the kernel's clear-on-
#                        sriov_numvfs=0 hook, NOT a claim that
#                        multiple LOAD_VHCA_STATE invocations are
#                        supported on the same VHCA without a
#                        cycle (that path is structurally blocked
#                        by the IOVA replay drift_armed gate; see
#                        design/vf_prerestore_split.md §3.5.5.1).
#                        The cell only proves that the cycle the
#                        orchestrator already has to perform for
#                        any slot repurposing also resets the
#                        identity tag.
#
#   multi_vf_symmetric   Stamp distinct UUIDs on N VFs in a batch,
#                        then iterate QUERY_VF over the same range
#                        and verify each slot reports its assigned
#                        UUID -- catches "all slots see the same
#                        UUID" / "off-by-one slot" bugs in the read
#                        site.
#
#   persist_without_bind A stamp survives close()-and-reopen of the
#                        cdev without binding the VF -- proves the
#                        UUID lives on the per-PF vfs_ctx[] and not
#                        on any per-fd state.
#
# This script DOES drive sriov_numvfs writes (root-only, sysfs).
# It assumes the PF is in the test-harness configuration where
# bouncing SR-IOV is safe (no live workload on the VFs). It backs
# up + restores the original sriov_numvfs as a courtesy.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_vf_uuid_lifecycle.sh [N]
#
#   PF      PF BDF, defaults to 0000:00:08.0.
#   N       VF count for the multi-VF test, defaults to 4. The
#           harness will set sriov_numvfs=N if it differs from the
#           current value.
#
# Exit codes:
#   0  all subtests PASSed
#   1  setup error (missing tool, missing cdev, sriov_numvfs write
#      rejected, etc.)
#   2  at least one subtest FAILed; details on stderr

set -euo pipefail

PF=${PF:-0000:00:08.0}
N=${1:-4}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
CDEV="/dev/mlx5_vfmig/$PF"
SYSFS_NUMVFS="/sys/bus/pci/devices/$PF/sriov_numvfs"

[ -x "$TOOL" ] || { echo "build $TOOL first (make -C tools/testing/criu_rdma)" >&2; exit 1; }
[ -e "$CDEV" ] || { echo "missing $CDEV (vfmig cdev not present)" >&2; exit 1; }
[ -w "$SYSFS_NUMVFS" ] || { echo "ERROR: $SYSFS_NUMVFS not writable; need root + a vfmig-eligible PF" >&2; exit 1; }

#
# UUID generation: prefer libuuid's `uuidgen`; fall back to /proc/sys
# kernel uuid generator. Both yield canonical 8-4-4-4-12 hex.
#
gen_uuid() {
	if command -v uuidgen >/dev/null; then
		uuidgen | tr '[:upper:]' '[:lower:]'
	elif [ -r /proc/sys/kernel/random/uuid ]; then
		cat /proc/sys/kernel/random/uuid
	else
		echo "no UUID generator available (install util-linux or kernel /proc/sys/kernel/random/uuid)" >&2
		exit 1
	fi
}

#
# Read one slot's vf_uuid via the CLI's `query_vf <vf_id>` output,
# which prints "vf_uuid=<uuid>" or "vf_uuid=<unset>" at end-of-line.
# Returns the literal "<unset>" if the slot is all-zeros.
#
read_uuid() {
	local vf_id=$1
	local out
	out=$("$TOOL" "$PF" query_vf "$vf_id")
	# Output shape:
	#   vf 0 vhca_id 0x0099 restored=0 tracked=1 (num_vfs=4) vf_uuid=<uuid|<unset>>
	echo "$out" | sed -n 's/.*vf_uuid=\([^ ]*\).*/\1/p'
}

#
# Restamp helpers. SET_VF_UUID failures are surfaced (we want the
# kernel's verdict on each cell), but a script-level `set -e` would
# abort on any non-zero exit, so the helper opts out and propagates
# the rc.
#
set_uuid() {
	local vf_id=$1 uuid=$2 rc
	set +e
	"$TOOL" "$PF" set_vf_uuid "$vf_id" "$uuid" >/dev/null 2>&1
	rc=$?
	set -e
	return $rc
}

#
# sriov_numvfs management. Writing 0 to sriov_numvfs requires every
# VF to be unbound; the test rig configures driver_override='' for
# vfmig-eligible PFs so this is the default state. write_numvfs
# returns the previous value so we can restore at exit.
#
read_numvfs() {
	cat "$SYSFS_NUMVFS"
}

write_numvfs() {
	local v=$1
	echo "$v" > "$SYSFS_NUMVFS"
}

ensure_numvfs() {
	local desired=$1 cur
	cur=$(read_numvfs)
	if [ "$cur" = "$desired" ]; then
		return 0
	fi
	# Always go through 0 to force a clean teardown. Writing
	# directly from N1 -> N2 is allowed only if N2 < N1 in some
	# kernels and rejects EBUSY in others.
	if [ "$cur" != "0" ]; then
		write_numvfs 0
	fi
	if [ "$desired" != "0" ]; then
		write_numvfs "$desired"
	fi
}

ORIG_NUMVFS=$(read_numvfs)
trap 'echo "[cleanup] restoring sriov_numvfs=$ORIG_NUMVFS"; ensure_numvfs "$ORIG_NUMVFS" || true' EXIT

#
# Make sure we're at the configured N for the multi-VF test. If the
# system was already at N, this is a no-op; otherwise we write 0
# then N which clears any pre-existing UUIDs as a side effect (which
# is what the lifecycle tests want anyway).
#
ensure_numvfs "$N"

PASS=0
FAIL=0

pass() { echo "PASS $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL $1: $2" >&2; FAIL=$((FAIL + 1)); }

#
# Test 1: clear_on_teardown
#
echo "--- clear_on_teardown ---"
U=$(gen_uuid)
if set_uuid 0 "$U"; then
	got=$(read_uuid 0)
	if [ "$got" = "$U" ]; then
		# Cycle.
		ensure_numvfs 0
		ensure_numvfs "$N"
		got=$(read_uuid 0)
		if [ "$got" = "<unset>" ]; then
			pass clear_on_teardown
		else
			fail clear_on_teardown "after sriov cycle vf 0 vf_uuid=$got (expected <unset>)"
		fi
	else
		fail clear_on_teardown "after stamp vf 0 vf_uuid=$got (expected $U)"
	fi
else
	fail clear_on_teardown "SET vf 0 U failed unexpectedly"
fi

#
# Test 2: stamp_after_recycle
#  Slot 0 was just cleared by clear_on_teardown's cycle. A fresh
#  V should land cleanly with no stale -EBUSY.
#
echo "--- stamp_after_recycle ---"
V=$(gen_uuid)
if set_uuid 0 "$V"; then
	got=$(read_uuid 0)
	if [ "$got" = "$V" ]; then
		pass stamp_after_recycle
	else
		fail stamp_after_recycle "vf 0 vf_uuid=$got (expected $V)"
	fi
else
	fail stamp_after_recycle "SET vf 0 V after recycle failed"
fi

#
# Test 3: multi_vf_symmetric
#  Cycle to a clean state, stamp distinct UUIDs on every VF, then
#  read back. The point of the cycle is to start from a known-zero
#  baseline; stamp_after_recycle already left vf 0 with V on it.
#
echo "--- multi_vf_symmetric ---"
ensure_numvfs 0
ensure_numvfs "$N"
declare -a EXPECTED
for ((i = 0; i < N; i++)); do
	EXPECTED[i]=$(gen_uuid)
	if ! set_uuid "$i" "${EXPECTED[i]}"; then
		fail multi_vf_symmetric "SET vf $i ${EXPECTED[i]} failed"
		EXPECTED[i]=""
		continue
	fi
done
ok=1
for ((i = 0; i < N; i++)); do
	if [ -z "${EXPECTED[i]}" ]; then
		ok=0
		continue
	fi
	got=$(read_uuid "$i")
	if [ "$got" != "${EXPECTED[i]}" ]; then
		fail multi_vf_symmetric "vf $i vf_uuid=$got (expected ${EXPECTED[i]})"
		ok=0
	fi
done
if [ "$ok" = 1 ]; then
	pass multi_vf_symmetric
fi

#
# Test 4: persist_without_bind
#  Each call to the CLI tool opens-and-closes the cdev fresh.
#  read_uuid above already exercises that. As an explicit cell, do
#  10 quick consecutive reads of vf 0's stamp and confirm the value
#  stays stable -- proves the UUID is on the per-PF vfs_ctx[] and
#  not on any per-fd state.
#
echo "--- persist_without_bind ---"
W=${EXPECTED[0]}
ok=1
for _ in 1 2 3 4 5 6 7 8 9 10; do
	got=$(read_uuid 0)
	if [ "$got" != "$W" ]; then
		fail persist_without_bind "open-cycle reread vf 0 vf_uuid=$got (expected $W)"
		ok=0
		break
	fi
done
if [ "$ok" = 1 ]; then
	pass persist_without_bind
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
