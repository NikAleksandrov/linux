#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Wrapper around multi_load_stage_gate_probe that provisions SR-IOV
# in the autoprobe=off + enable_migratable shape the LOAD ioctl
# requires, runs the kernel-matrix probe, and tears the rig back down.
# The probe itself does NOT cycle sriov_numvfs and does NOT issue
# enable_migratable -- both are scripted here so the same probe binary
# can be reused under different orchestration shells (CI / interactive
# / cross-host) without ioctl-vs-sysfs ordering ambiguity.
#
# What this validates
# -------------------
#   - vfmig_vf_id_busy_locked (drivers/net/ethernet/mellanox/mlx5/
#     core/vfmig/vfmig.c line 4199): the per-vf_id staging gate fires on
#     concurrent LOAD fds and clears on close, and is keyed on
#     vf_id (not on the PF or on whether a pending_load is
#     installed).
#
# What this does NOT validate
# ---------------------------
#   - vfmig_install_pending_load_locked (the second staging gate, in
#     the LOAD fd's release path). In practice the IOVA replay
#     drift_armed gate fires *first* on the second write of HOST_PAGE
#     records and aborts the second LOAD before its release path
#     runs; the install gate is a safety net for the small window
#     between "apply path takes the slot" and "userspace closes the
#     load_fd", which is hard to manufacture cleanly without a second
#     IOVA domain. See test_multi_load_drift_gate.sh in this
#     directory for the empirical drift_armed-gate probe, and
#     design/vf_prerestore_split.md §3.5.5.1 for the gate-by-gate
#     table.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_multi_load_stage_gate.sh [N]
#
#   PF      PF BDF, defaults to 0000:00:08.0.
#   N       VF count to provision; defaults to 2 so the
#           different_vf cell can run. Must be >= 1; the probe will
#           SKIP different_vf if num_vfs < 2.
#
# Exit codes:
#   0  all subtests PASSed
#   1  setup error (missing tool, missing cdev, sysfs write rejected)
#   2  at least one probe subtest FAILed
set -euo pipefail

PF=${PF:-0000:00:08.0}
N=${1:-2}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$SCRIPT_DIR/multi_load_stage_gate_probe}
CDEV="/dev/mlx5_vfmig/$PF"
SYSFS_NUMVFS="/sys/bus/pci/devices/$PF/sriov_numvfs"
SYSFS_AUTOPROBE="/sys/bus/pci/devices/$PF/sriov_drivers_autoprobe"

[ -x "$TOOL" ]  || { echo "build $TOOL first: make -C $ROOT_DIR" >&2; exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first: make -C $ROOT_DIR" >&2; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV (mlx5_core not loaded?)" >&2; exit 1; }
[ -w "$SYSFS_NUMVFS" ] || { echo "ERROR: $SYSFS_NUMVFS not writable; need root" >&2; exit 1; }

ORIG_NUMVFS=$(cat "$SYSFS_NUMVFS")
ORIG_AUTOPROBE=$(cat "$SYSFS_AUTOPROBE")
trap 'echo "[cleanup] sriov_numvfs=$ORIG_NUMVFS autoprobe=$ORIG_AUTOPROBE";
      echo 0 > "$SYSFS_NUMVFS";
      echo "$ORIG_AUTOPROBE" > "$SYSFS_AUTOPROBE";
      [ "$ORIG_NUMVFS" != 0 ] && echo "$ORIG_NUMVFS" > "$SYSFS_NUMVFS";
      exit' EXIT

# Always go through 0 to force a clean teardown of any prior state
# (other tests may have left autoprobe at 1, etc.). Then provision
# with autoprobe=off so VFs come up unbound -- the LOAD ioctl
# requires the migratable cap to be set in the pre-ENABLE_HCA
# window, which only exists while the VF is unbound.
echo 0 > "$SYSFS_NUMVFS"
echo 0 > "$SYSFS_AUTOPROBE"
echo "$N" > "$SYSFS_NUMVFS"

# Wait for the kernel to populate the virtfn symlinks (sriov_numvfs
# write returns before the VF objects materialize).
for vf_idx in $(seq 0 $((N - 1))); do
	for _ in $(seq 1 50); do
		[ -e "/sys/bus/pci/devices/$PF/virtfn$vf_idx" ] && break
		sleep 0.1
	done
	[ -e "/sys/bus/pci/devices/$PF/virtfn$vf_idx" ] || {
		echo "ERROR: virtfn$vf_idx never appeared on $PF" >&2
		exit 1
	}
done

# enable_migratable on every vf_id we plan to touch. The LOAD ioctl
# checks this cap before the busy gate, so a missing migratable
# would surface as -EOPNOTSUPP and mask the gate cells.
for vf_idx in $(seq 0 $((N - 1))); do
	"$TOOL" "$PF" enable_migratable "$vf_idx" >/dev/null
done

echo "--- multi_load_stage_gate_probe ---"
"$PROBE" "$PF" 0
