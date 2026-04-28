#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# L1b end-to-end SAVE+LOAD round-trip on a single host, with the
# vfmig deterministic-IOVA path engaged. Differs from test_m2r.sh in
# exactly two pieces of behavior:
#
#   1. After each sriov_numvfs=1 (source provisioning in Phase A,
#      destination provisioning in Phase C), it issues
#      MLX5_VFMIG_IOC_SET_TRACKED { vf_id=0, enable=1 } via the
#      `set_tracked` verb, so that:
#        - on the source, alloc_cmd_page() routes through
#          vfmig_iova_alloc_coherent() and the cmd ring's IOVA is
#          deterministic; SAVE then snapshots the IOVA registry
#          into HOST_PAGE wire records (vfmig_save_build_host_pages_buf);
#        - on the destination, LOAD_VHCA_STATE's parser replays each
#          HOST_PAGE record into the destination's
#          vfmig_iova_domain via vfmig_iova_replay_page() before the
#          FW_DATA record is staged; the destination's
#          vfmig_iova_alloc_coherent() then finds the replayed page
#          via lookup-at-cursor instead of allocating a fresh empty
#          page.
#
#      set_tracked is destroyed implicitly by sriov_numvfs=0 between
#      Phase B and Phase C (mlx5_vfmig_pf_drop_iova_domains runs
#      from mlx5_sriov_disable before pci_disable_sriov), so the
#      Phase-C set_tracked call is a fresh attach against a fresh VF.
#
#   2. The acceptance criterion for the post-LOAD bind is RELAXED to
#      "failure mode shifts away from the baseline ENABLE_HCA(0x104)
#      timeout" rather than "VF probes cleanly". Even with HOST_PAGE
#      replay of the cmd ring page, Layer 1 still doesn't preserve
#      MANAGE_PAGES / EQ buffers / UAR pages, so the probe will fail
#      somewhere downstream of mlx5_cmd_enable -- but the failure
#      should be a different, later, FW-syndrome-tagged error rather
#      than a 60-second cmd-ring timeout. That shift IS the L1b
#      keystone result.
#
# This script does NOT try to make the post-restore VF fully
# functional. That's Layer 3's job. Our pass signal is just that the
# command interface itself works after LOAD.
#
# Usage: sudo PF=0000:00:08.0 ./test_m2r_iova.sh

set -euxo pipefail

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-./mlx5_vfmig}
BLOB=${BLOB:-/tmp/vf_m2r_iova.blob}
SAVE_FLAGS=${SAVE_FLAGS:-}   # e.g. "keep_suspended"

[ -x "$TOOL" ] || { echo "build $TOOL first"; exit 1; }

CDEV="/dev/mlx5_vfmig/$PF"
[ -e "$CDEV" ] || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

# --- helpers -----------------------------------------------------------

vf_path()       { echo "/sys/bus/pci/devices/$1"; }
wait_for_path() { for i in $(seq 1 50); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }

snapshot_vf() {
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

# --- Phase A: provision SOURCE with set_tracked + migratable ----------
#
# Ordering: autoprobe=0 -> sriov_numvfs=1 -> set_tracked -> enable_migratable -> bind.
# set_tracked must precede bind (set_tracked rejects -EBUSY on a bound VF) and
# must precede the cmd-ring allocation that the bind triggers (otherwise
# alloc_cmd_page falls through to dma_alloc_coherent and the source's IOVA
# layout is lost forever for this generation of the VF).

echo "=== Phase A: provision SOURCE (set_tracked + migratable) + snapshot baseline ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe"
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs"

wait_for_path "$(vf_path $PF)/virtfn0"
VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "VF is $VF"

# Engage IOVA tracking BEFORE the bind that triggers cmd ring allocation.
sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0

echo mlx5_core | sudo tee "$(vf_path $VF)/driver_override"
echo "$VF"     | sudo tee /sys/bus/pci/drivers/mlx5_core/bind

wait_for_path "$(vf_path $VF)/driver"
DRV_PRE=$(basename "$(readlink "$(vf_path $VF)/driver")")
[ "$DRV_PRE" = "mlx5_core" ] || { echo "expected mlx5_core, got $DRV_PRE"; exit 1; }

sleep 1
PRE=$(snapshot_vf "$VF" "PRE")
echo "$PRE"

# Sanity log: confirm the IOVA hook actually fired for the cmd ring.
echo "--- cmd ring IOVA registry sanity (expect at least one HOST_PAGE entry) ---"
sudo dmesg | grep -E 'vfmig_iova: vf 0 domain attached|vfmig: cmd ring at iova' | tail -10 || true

# --- Phase B: SAVE -----------------------------------------------------

echo "=== Phase B: SAVE (now emits HOST_PAGE records before FW_DATA) ==="
sudo dmesg -C
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB" $SAVE_FLAGS
ls -l "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
[ "$SAVE_BYTES" -gt 16 ] || { echo "blob suspiciously small: $SAVE_BYTES"; exit 1; }

echo "--- dmesg after SAVE ---"
sudo dmesg | tail -20
if sudo dmesg | grep -E 'SUSPEND_VHCA|SAVE_VHCA_STATE|QUERY_VHCA_MIGRATION' | grep -i 'failed'; then
    echo "FAIL: firmware error during SAVE"
    exit 1
fi
if sudo dmesg | grep -E 'build_host_pages_buf failed'; then
    echo "FAIL: HOST_PAGE prefix build failed"
    exit 1
fi

# --- Phase C: tear down + re-provision DESTINATION (no autoprobe) -----

echo "=== Phase C: tear down + re-provision DESTINATION ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe"
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs"

wait_for_path "$(vf_path $PF)/virtfn0"
VF2=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "fresh VF is $VF2"
[ ! -e "$(vf_path $VF2)/driver" ] && echo "OK: VF $VF2 unbound"

# --- Phase D: set_tracked + enable_migratable + LOAD + bind -----------

echo "=== Phase D: set_tracked + LOAD (replays HOST_PAGE) + bind ==="
sudo dmesg -C

# CRITICAL: set_tracked must precede load_vhca_state. The LOAD ioctl
# captures vfs_ctx[0].vfmig_iova_dom into its load_ctx at open time;
# without an IOVA domain attached, HOST_PAGE records in the blob are
# rejected with -EINVAL ("destination not SET_TRACKED'd").
sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0

sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" mark_restored 0

# L1b expectation: probe should NOT hit ENABLE_HCA(0x104) timeout
# anymore. It may still fail somewhere later (QUERY_HCA_CAP /
# MANAGE_PAGES / etc.) because we don't yet preserve those pages --
# that's L2/L3. The keystone here is the failure mode shift.
echo mlx5_core | sudo tee "$(vf_path $VF2)/driver_override"
echo "$VF2"     | sudo tee /sys/bus/pci/drivers/mlx5_core/bind 2>&1 || true

echo "--- dmesg after LOAD+BIND ---"
sudo dmesg | tail -60

# Hard-fail on the legacy baseline failure: 60s ENABLE_HCA timeout.
# If this still appears, L1b's HOST_PAGE replay isn't actually being
# fed the cmd ring (most likely missing set_tracked on src or dst).
if sudo dmesg | grep -E 'ENABLE_HCA\(0x104\) timeout'; then
    echo "FAIL: still hitting baseline ENABLE_HCA(0x104) timeout -- L1b regression"
    exit 1
fi
if sudo dmesg | grep -E 'replay_page.*failed'; then
    echo "FAIL: HOST_PAGE replay rejected by destination"
    exit 1
fi

# --- Phase E: post-restore snapshot (best-effort) ---------------------

echo "=== Phase E: snapshot post-restore (best-effort) ==="
if [ -e "$(vf_path $VF2)/driver" ]; then
    DRV_POST=$(basename "$(readlink "$(vf_path $VF2)/driver")")
    sleep 1
    POST=$(snapshot_vf "$VF2" "POST")
    echo "$POST"
else
    DRV_POST="(unbound)"
    POST="[POST] bdf=$VF2 netdev=(none) mac=(none) state=(none) vhca_id=?"
    echo "$POST"
fi

echo
echo "==== L1b summary ===="
echo "$PRE"
echo "$POST"
echo
echo "Acceptance signals:"
echo "  - SAVE produced $SAVE_BYTES bytes (now includes HOST_PAGE prefix records)"
echo "  - SAVE/LOAD reported no firmware errors"
echo "  - HOST_PAGE replay reported no errors on the destination"
echo "  - Post-LOAD bind did NOT hit ENABLE_HCA(0x104) timeout (if any failure,"
echo "    look for it later in mlx5_function_open / QUERY_HCA_CAP / INIT_HCA)"
echo
echo "L1b pass condition: failure mode shifted away from ENABLE_HCA timeout."
echo "Whether the bind ultimately succeeds depends on Layer 2/3 work (MANAGE_PAGES,"
echo "EQ/UAR preservation), which this script does not yet exercise."

# Cleanup leaves VF in restored state for inspection. To reset:
#   sudo $TOOL $PF set_tracked 0 0
#   echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs
