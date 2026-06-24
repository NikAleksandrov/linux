#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# M2 plumbing smoke-test using a synthesized blob (no VFIO save side).
#
# Validates that the kernel LOAD_VHCA_STATE control plane works end-to-
# end: ioctl returns an fd, parser consumes a real header, PD/MKEY/DMA
# get set up, the LOAD_VHCA_STATE firmware command actually fires, the
# fd cleanly tears down, and a subsequent MARK_RESTORED + bind reaches
# the same probe-time skip as M1'.
#
# What is NOT validated here:
#   * Whether the firmware accepts the bytes -- we feed garbage, so it
#     will reject them ("LOAD_VHCA_STATE ... failed" expected). That's
#     fine: we are testing the *kernel-side* plumbing in isolation.
#
# Usage: sudo PF=0000:00:08.0 ./test_synthetic_load_plumbing.sh

set -euxo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
SYNTH=${SYNTH:-$ROOT_DIR/tools/synthetic_blob_emit}
BLOB=${BLOB:-/tmp/vf_synth.blob}
SIZE=${SIZE:-4096}

[ -x "$TOOL" ]  || { echo "build $TOOL first: make -C $ROOT_DIR"; exit 1; }
[ -x "$SYNTH" ] || { echo "build $SYNTH first: make -C $ROOT_DIR"; exit 1; }

# 0. clean slate
echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs
ls -l "/dev/mlx5_vfmig/$PF"

# 1. produce the synthetic blob
$SYNTH "$BLOB" "$SIZE"
ls -l "$BLOB"

# 2. spawn one VF, autoprobe off
echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_drivers_autoprobe
echo 1 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs
for i in $(seq 1 20); do
    [ -e /sys/bus/pci/devices/$PF/virtfn0 ] && break
    sleep 0.1
done
VF=$(basename "$(readlink /sys/bus/pci/devices/$PF/virtfn0)")
echo "VF is $VF"
[ ! -e /sys/bus/pci/devices/$VF/driver ] && echo "OK: VF $VF unbound"

# 3. clear dmesg so the LOAD/MARK/probe trace is easy to read
sudo dmesg -C

# 4. issue the LOAD ioctl. Expect:
#       vfmig: LOAD session opened for vf 0 (vhca_id 0x....)
#       vfmig: LOAD_VHCA_STATE vf 0 ... failed: -EREMOTEIO   <-- expected
#    AND the userspace tool's write() fails with EREMOTEIO.
echo "--- LOAD (expect firmware reject of synth bytes) ---"
sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB" || \
    echo "OK: LOAD failed at firmware as expected"

echo "--- dmesg after LOAD ---"
sudo dmesg | tail -20

# 5. confirm subsequent ioctls still work (no UAF / no left-over state)
sudo "$TOOL" "$PF" list

# 6. MARK_RESTORED (with defer_resume) still works after a failed LOAD
sudo "$TOOL" "$PF" mark_restored 0 defer_resume
sudo "$TOOL" "$PF" list

# 7. driver_override + bind. We've not actually loaded valid state, so
#    the same "bad system state" we saw in M1' will reappear -- that's
#    the expected outcome of feeding firmware garbage. The point of
#    this test is purely: the kernel LOAD plumbing didn't crash, leak,
#    or wedge anything.
echo mlx5_core | sudo tee /sys/bus/pci/devices/$VF/driver_override
sudo dmesg -C
echo "$VF" | sudo tee /sys/bus/pci/drivers/mlx5_core/bind || true

# restore mirror: idempotent resume (no-op here since the synthetic
# LOAD was rejected, so the VHCA was never parked); exercises the
# RESUME_VHCA plumbing after a failed LOAD.
sudo "$TOOL" "$PF" resume_vhca 0

echo "--- dmesg after BIND ---"
sudo dmesg | tail -30

echo "--- final list ---"
sudo "$TOOL" "$PF" list

# 8. cleanup
echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs

echo
echo "Plumbing acceptance:"
echo "  - 'LOAD session opened' line present in dmesg"
echo "  - LOAD_VHCA_STATE failed with firmware error (synth bytes rejected)"
echo "  - subsequent ioctls still work"
echo "  - VF probe still reaches the M1' INIT_HCA-skip path"
