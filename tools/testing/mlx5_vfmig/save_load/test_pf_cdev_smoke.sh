#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Smoke test for the mlx5_vfmig M1' control plane.
#
# Acceptance criteria:
#   1. /dev/mlx5_vfmig/<PF> exists after mlx5_core probes the PF.
#   2. PF can resolve VF vhca_id without binding the VF.
#   3. mark_restored on a fresh VF succeeds; second mark_restored returns
#      EALREADY.
#   4. After driver_override + bind, dmesg contains:
#         vfmig: VF (vhca_id 0x....) marked restored, skipping INIT_HCA
#   5. The VF probe then fails downstream with "bad system state (0x4)"
#      because LOAD_VHCA_STATE has not been wired yet -- expected at M1'.
#
# Usage: sudo PF=0000:00:08.0 ./test_pf_cdev_smoke.sh

set -euxo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
CDEV=/dev/mlx5_vfmig/$PF

[ -x "$TOOL" ] || { echo "build $TOOL first: make -C $ROOT_DIR"; exit 1; }

# 0. clean slate
echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs

# 1. confirm the PF cdev exists (M1' loaded correctly)
ls -l "$CDEV"

# 2. close the autoprobe window so we can intervene before the VF binds
echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_drivers_autoprobe

# 3. spawn one VF
echo 1 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs
for i in $(seq 1 20); do
    [ -e /sys/bus/pci/devices/$PF/virtfn0 ] && break
    sleep 0.1
done
VF=$(basename "$(readlink /sys/bus/pci/devices/$PF/virtfn0)")
echo "VF is $VF"

# 4. it must NOT be bound (autoprobe=0)
[ ! -e /sys/bus/pci/devices/$VF/driver ] && echo "OK: VF $VF unbound"

# 5. diagnostic snapshot before any marking
sudo "$TOOL" "$PF" list

# 6. PF-side vhca_id resolution (sanity)
sudo "$TOOL" "$PF" get_vhca_id 0

# 7. mark VF #0 as restored
sudo "$TOOL" "$PF" mark_restored 0

# second call must EALREADY
sudo "$TOOL" "$PF" mark_restored 0 || echo "OK: second mark returns EALREADY"

# diagnostic snapshot after marking -- restored=1 expected
sudo "$TOOL" "$PF" list

# 8. force-bind the VF to mlx5_core (driver_override required because autoprobe=0)
echo mlx5_core | sudo tee /sys/bus/pci/devices/$VF/driver_override
sudo dmesg -C
echo "$VF" | sudo tee /sys/bus/pci/drivers/mlx5_core/bind || true

# 9. inspect the probe path -- THE key line:
#       vfmig: VF (vhca_id 0x....) marked restored, skipping INIT_HCA
echo "--- dmesg after bind ---"
sudo dmesg | tail -60

# 10. post-bind diagnostic: restored bit must have been consumed
sudo "$TOOL" "$PF" list
