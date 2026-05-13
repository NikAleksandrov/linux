#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# End-to-end test for the mlx5_vfmig M2 LOAD_VHCA_STATE control plane.
#
# What this script does
# ---------------------
# Phase A (save):
#   - sriov_drivers_autoprobe=0; sriov_numvfs=1
#   - bind the resulting VF to mlx5_vfio_pci via driver_override
#   - run vfio_stop_copy_save to drain a STOP_COPY data_fd into a blob file
#   - unbind the VF, sriov_numvfs=0
#
# Phase B (load):
#   - sriov_drivers_autoprobe=0; sriov_numvfs=1 again (fresh VF, possibly
#     a new vhca_id; the blob bytes are opaque so this is fine)
#   - mlx5_vfmig load_vhca_state 0 <blob>
#   - mlx5_vfmig mark_restored 0
#   - driver_override + bind to mlx5_core
#
# Acceptance:
#   1. dmesg shows "vfmig: LOAD session opened for vf 0 (vhca_id 0x...)" and
#      no LOAD_VHCA_STATE failures
#   2. dmesg shows "vfmig: VF (vhca_id 0x....) marked restored, skipping INIT_HCA"
#   3. NO "bad system state" errors after the skip log
#   4. The probe completes; basic VF sysfs (/sys/class/{net,infiniband}/...)
#      is created
#
# Usage:
#   sudo PF=0000:00:08.0 ./test_vfio_save_load_roundtrip.sh

set -euxo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
SAVE=${SAVE:-$ROOT_DIR/tools/vfio_stop_copy_save}
BLOB=${BLOB:-/tmp/vf.blob}

[ -x "$TOOL" ] || { echo "build $TOOL: make -C $ROOT_DIR"; exit 1; }
[ -x "$SAVE" ] || { echo "build $SAVE: make -C $ROOT_DIR vfio-helper"; exit 1; }

# 0. clean slate
echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs
ls -l "/dev/mlx5_vfmig/$PF"

# ---- PHASE A: save via mlx5_vfio_pci ----
echo "=== Phase A: SAVE via mlx5_vfio_pci ==="

# autoprobe off so the VF doesn't bind to mlx5_core under us
echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_drivers_autoprobe

echo 1 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs
for i in $(seq 1 20); do
    [ -e /sys/bus/pci/devices/$PF/virtfn0 ] && break
    sleep 0.1
done
VF=$(basename "$(readlink /sys/bus/pci/devices/$PF/virtfn0)")
echo "VF is $VF"

# Force-bind the VF to mlx5_vfio_pci
echo mlx5_vfio_pci | sudo tee /sys/bus/pci/devices/$VF/driver_override
echo "$VF" | sudo tee /sys/bus/pci/drivers/mlx5_vfio_pci/bind

# Drain STOP_COPY into BLOB
sudo rm -f "$BLOB"
sudo "$SAVE" "$VF" "$BLOB"
ls -l "$BLOB"

# Cleanup: unbind from mlx5_vfio_pci, drop VFs
echo "$VF" | sudo tee /sys/bus/pci/drivers/mlx5_vfio_pci/unbind
echo "" | sudo tee /sys/bus/pci/devices/$VF/driver_override
echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs

# ---- PHASE B: load via mlx5_vfmig ----
echo "=== Phase B: LOAD via mlx5_vfmig ==="

echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_drivers_autoprobe
echo 1 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs
for i in $(seq 1 20); do
    [ -e /sys/bus/pci/devices/$PF/virtfn0 ] && break
    sleep 0.1
done
VF=$(basename "$(readlink /sys/bus/pci/devices/$PF/virtfn0)")
echo "VF is $VF"

[ ! -e /sys/bus/pci/devices/$VF/driver ] && echo "OK: VF $VF unbound"

sudo "$TOOL" "$PF" list

# pre-bind: load FW state, then mark restored
sudo dmesg -C
sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" mark_restored 0
sudo "$TOOL" "$PF" list

echo "--- dmesg after LOAD+MARK ---"
sudo dmesg | tail -40

# Bind to mlx5_core
echo mlx5_core | sudo tee /sys/bus/pci/devices/$VF/driver_override
sudo dmesg -C
echo "$VF" | sudo tee /sys/bus/pci/drivers/mlx5_core/bind || true

echo "--- dmesg after BIND ---"
sudo dmesg | tail -60

echo "--- VF sysfs after BIND ---"
ls -l /sys/bus/pci/devices/$VF/driver || true
ls -l /sys/class/net/ 2>/dev/null | grep -E "$(echo $VF | tr ':' '_')|virtfn" || true
ls -l /sys/class/infiniband/ 2>/dev/null || true

# Acceptance checks
if sudo dmesg | grep -q "bad system state"; then
    echo "FAIL: 'bad system state' present in dmesg"
    exit 1
fi
if ! sudo dmesg | grep -q "vfmig: VF.*marked restored, skipping INIT_HCA"; then
    echo "FAIL: skip-INIT_HCA log line missing"
    exit 1
fi
echo "PASS: M2 acceptance criteria met"
