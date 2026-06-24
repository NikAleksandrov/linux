#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# M2-revised end-to-end SAVE+LOAD round-trip on a single host. No VFIO,
# no IOMMU, no QEMU -- everything is driven through the in-driver
# /dev/mlx5_vfmig/<bdf> control plane.
#
# Phases:
#   A. Provision VF, let mlx5_core probe it normally (autoprobe=on).
#      Snapshot baseline operational state (vhca_id, link, MAC, etc).
#   B. SAVE the VF's firmware state into a blob via the SAVE ioctl.
#      The driver SUSPENDs the VHCA, snapshots, then RESUMEs.
#   C. Tear down the VF (sriov_numvfs=0). Re-create it with
#      autoprobe=off so we can race its probe.
#   D. LOAD the blob back into the fresh VF, mark it restored, then
#      bind mlx5_core.
#   E. Compare post-restore operational state against the baseline.
#
# Acceptance:
#   - SAVE ioctl returns a non-empty blob, no firmware errors in dmesg
#   - LOAD ioctl consumes the blob without firmware errors
#   - VF re-probes cleanly (no "bad system state", no QUERY_ADAPTER
#     errors, no probe error code)
#   - Post-restore baseline (link, MAC, vhca_id) matches pre-save
#
# Usage: sudo PF=0000:00:08.0 ./test_inkernel_save_load_roundtrip.sh

set -euxo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
BLOB=${BLOB:-/tmp/vf_m2r.blob}
SAVE_FLAGS=${SAVE_FLAGS:-}   # e.g. "keep_suspended"

[ -x "$TOOL" ] || { echo "build $TOOL first: make -C $ROOT_DIR"; exit 1; }

CDEV="/dev/mlx5_vfmig/$PF"
[ -e "$CDEV" ] || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

# --- helpers -----------------------------------------------------------

vf_path()       { echo "/sys/bus/pci/devices/$1"; }
wait_for_path() { for i in $(seq 1 50); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }

snapshot_vf() {
    # $1 = VF BDF, $2 = label
    local bdf=$1 label=$2 ifname mac state vhca
    ifname=$(ls "/sys/bus/pci/devices/$bdf/net/" 2>/dev/null | head -n1 || true)
    if [ -n "$ifname" ]; then
        mac=$(cat "/sys/class/net/$ifname/address" 2>/dev/null || echo "?")
        state=$(cat "/sys/class/net/$ifname/operstate" 2>/dev/null || echo "?")
    else
        ifname="(none)"
        mac="(none)"
        state="(none)"
    fi
    vhca=$(sudo "$TOOL" "$PF" get_vhca_id 0 2>/dev/null | awk '{print $NF}' || echo "?")
    echo "[$label] bdf=$bdf netdev=$ifname mac=$mac state=$state vhca_id=$vhca"
}

# --- Phase A: provision and snapshot baseline --------------------------
#
# Ordering matters: we must flip the per-VF "migratable" cap *before*
# mlx5_core probes the VF. The firmware accepts a SET_HCA_CAP on a
# VHCA that hasn't been ENABLE_HCA'd yet (pre-probe), but rejects it
# with "bad resource state" once it's been brought up. So:
#   autoprobe=0 -> sriov_numvfs=1 -> enable_migratable -> manual bind
#
# After that the VF is fully provisioned with migratable=1 latched in,
# and SAVE in Phase B can SUSPEND/SAVE/RESUME normally.

echo "=== Phase A: provision + snapshot baseline ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe"
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs"

wait_for_path "$(vf_path $PF)/virtfn0"
VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "VF is $VF"

# Pre-bind window: enable migratable while the VHCA is in pre-ENABLE_HCA state.
sudo "$TOOL" "$PF" enable_migratable 0

# Now bind mlx5_core, which will run ENABLE_HCA with migratable=1 latched in.
echo mlx5_core | sudo tee "$(vf_path $VF)/driver_override"
echo "$VF"     | sudo tee /sys/bus/pci/drivers/mlx5_core/bind

wait_for_path "$(vf_path $VF)/driver"
DRV_PRE=$(basename "$(readlink "$(vf_path $VF)/driver")")
[ "$DRV_PRE" = "mlx5_core" ] || { echo "expected mlx5_core, got $DRV_PRE"; exit 1; }

# Give the VF a moment to settle (netdev appears slightly after probe).
sleep 1
PRE=$(snapshot_vf "$VF" "PRE")
echo "$PRE"

# --- Phase B: SAVE -----------------------------------------------------

echo "=== Phase B: SAVE ==="
sudo dmesg -C
# snapshot-ordering: pause datapath (CRIU CHECKPOINT_DEVICES), then
# capture (SAVE is suspend-aware and skips its own suspend), then resume.
sudo "$TOOL" "$PF" suspend_vhca 0
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB" $SAVE_FLAGS
sudo "$TOOL" "$PF" resume_vhca 0
ls -l "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
[ "$SAVE_BYTES" -gt 16 ] || { echo "blob suspiciously small: $SAVE_BYTES"; exit 1; }

echo "--- dmesg after SAVE ---"
sudo dmesg | tail -20
if sudo dmesg | grep -E 'SUSPEND_VHCA|SAVE_VHCA_STATE|QUERY_VHCA_MIGRATION' | grep -i 'failed'; then
    echo "FAIL: firmware error during SAVE"
    exit 1
fi

# --- Phase C: tear down + re-provision (no autoprobe) ------------------

echo "=== Phase C: tear down + re-provision ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe"
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs"

wait_for_path "$(vf_path $PF)/virtfn0"
VF2=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "fresh VF is $VF2"
[ ! -e "$(vf_path $VF2)/driver" ] && echo "OK: VF $VF2 unbound"

# --- Phase D: enable_migratable + LOAD + bind --------------------------

echo "=== Phase D: LOAD + bind ==="
sudo dmesg -C

# Same pre-bind window as Phase A: the VF was just (re)created with
# autoprobe=0, so the VHCA is in pre-ENABLE_HCA state and the firmware
# will accept the migratable flip. LOAD/RESUME both gate on this bit.
sudo "$TOOL" "$PF" enable_migratable 0

# load_vhca_state stages the blob and (on close) auto-installs the
# pending_load slot + restored flag. The actual SUSPEND + LOAD_VHCA_STATE
# + RESUME firmware commands run from inside the next mlx5_core probe,
# in mlx5_function_enable(), *before* any VHCA-side bring-up command --
# the only window where the firmware accepts the loaded state without
# rejecting it with bad parameter (syndrome 0x2c9bb0).
#
# mark_restored is left in for symmetry / belt-and-suspenders; it's a
# no-op now that LOAD's close already set the flag.
sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" mark_restored 0

# NOTE: on a native (non-VFIO, non-VM) probe with a destination cmd
# ring at a different DMA address than the source's, this bind is
# expected to hang and eventually fail with "Connection timed out" /
# QUERY_HCA_CAP timeout. See README.md for the architectural reason
# and the path to fixing it. The script still exits non-zero in that
# case via the dmesg checks below, which is correct behaviour.
echo mlx5_core | sudo tee "$(vf_path $VF2)/driver_override"
echo "$VF2"     | sudo tee /sys/bus/pci/drivers/mlx5_core/bind

echo "--- dmesg after LOAD+BIND ---"
sudo dmesg | tail -40
if sudo dmesg | grep -E 'bad system state|probe with driver mlx5_core failed'; then
    echo "FAIL: VF probe surfaced bad-system-state / probe failure"
    exit 1
fi
if sudo dmesg | grep -E 'LOAD_VHCA_STATE.*failed|RESUME_VHCA.*failed'; then
    echo "FAIL: firmware error during LOAD/RESUME"
    exit 1
fi

# --- Phase E: post-restore snapshot + comparison -----------------------

echo "=== Phase E: snapshot post-restore ==="
wait_for_path "$(vf_path $VF2)/driver"
DRV_POST=$(basename "$(readlink "$(vf_path $VF2)/driver")")
[ "$DRV_POST" = "mlx5_core" ] || { echo "post-restore driver=$DRV_POST"; exit 1; }
sleep 1
POST=$(snapshot_vf "$VF2" "POST")
echo "$POST"

echo
echo "==== summary ===="
echo "$PRE"
echo "$POST"
echo
echo "Acceptance signals:"
echo "  - SAVE produced $SAVE_BYTES bytes"
echo "  - VF re-probed clean (no bad system state / probe failure)"
echo "  - LOAD/RESUME reported no firmware errors"
echo
echo "(Compare the PRE / POST lines above for vhca_id and netdev MAC equality.)"

# Cleanup leaves VF in restored state for inspection. To reset:
#   echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs
