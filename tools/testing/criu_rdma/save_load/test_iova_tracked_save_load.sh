#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# End-to-end SAVE + LOAD round-trip exercising the vfmig
# deterministic-IOVA path (layers L0-L3). Now supports both
# single-host (default) and cross-host operation, plus an optional
# post-restore ping smoke test.
#
# Three behaviors vs. test_inkernel_save_load_roundtrip.sh:
#
#   1. After each sriov_numvfs=1 (source provisioning in Phase A,
#      destination provisioning in Phase C), it issues
#      MLX5_VFMIG_IOC_SET_TRACKED { vf_id=0, enable=1 } so cmd ring,
#      mailboxes, MANAGE_PAGES pages, EQ buffers, and DB pages are
#      allocated through the deterministic IOVA allocator. SAVE then
#      snapshots the IOVA registry into HOST_PAGE wire records;
#      LOAD replays them on the destination before applying
#      LOAD_VHCA_STATE.
#
#      mlx5_sriov_disable() drops the per-VF IOVA domains *after*
#      pci_disable_sriov() has finished VF teardown, so the
#      destination set_tracked is always a fresh attach.
#
#   2. The L0-L3 keystone result is that the post-LOAD bind reaches
#      a fully functional mlx5_core/mlx5e: same vhca_id, same MAC,
#      same netdev. mlx5_ib still fails at CREATE_SRQ (L4 territory)
#      and that failure is informational, not fatal.
#
#   3. ROLE=source | destination | both (default both) splits the
#      flow. A small manifest file ($BLOB.meta) is written by the
#      source role and read by the destination role for sanity
#      comparison across hosts.
#
# Usage:
#
#   Single-host round-trip (default):
#     sudo PF=0000:08:00.0 ./test_iova_tracked_save_load.sh
#
#   Cross-host:
#     # on source host:
#     sudo PF=0000:08:00.0 ROLE=source ./test_iova_tracked_save_load.sh
#     scp /tmp/vf_m2r_iova.blob /tmp/vf_m2r_iova.blob.meta \
#         user@dest:/tmp/
#     # on destination host:
#     sudo PF=0000:08:00.0 ROLE=destination ./test_iova_tracked_save_load.sh
#
#   Optional post-restore ping (any ROLE that runs Phase D-E):
#     sudo PF=... PING_LOCAL_CIDR=10.0.0.2/24 PING_TARGET=10.0.0.1 \
#         ROLE=destination ./test_iova_tracked_save_load.sh

set -euxo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
BLOB=${BLOB:-/tmp/vf_m2r_iova.blob}
META=${META:-${BLOB}.meta}
SAVE_FLAGS=${SAVE_FLAGS:-}   # e.g. "keep_suspended"
ROLE=${ROLE:-both}

# Optional post-restore ping (no-op if either is unset).
# PING_LOCAL_CIDR is assigned to the restored VF's netdev for the
# duration of the ping, then removed. PING_TARGET is the IP we ping.
PING_LOCAL_CIDR=${PING_LOCAL_CIDR:-}
PING_TARGET=${PING_TARGET:-}
PING_COUNT=${PING_COUNT:-5}

# Optional RDMA pingpong smoke test. PINGPONG=1 enables an
# ibv_rc_pingpong self-loopback in Phase A (source bind, treated
# as must-pass: catches the R1.5 'FW ignores qpc.xrcd/qpc.srqn for
# non-XRC-non-SRQ QP' assumption regressing) and Phase D
# (destination bind, treated as best-effort: until L4 R2/R3 lands
# the destination's mlx5_ib port 1 doesn't open and pingpong is
# expected to fail at modify-QP-to-RTR -- we run it anyway for
# diagnostic value and to show the failure mode shifts as R2/R3
# are implemented).
PINGPONG=${PINGPONG:-0}

# Optional UAR/ucontext save+restore smoke test (step 4 of the
# UAR-restore plan). UCTX=1 enables:
#   Phase A.5  emit a vfmig uctx snapshot to $UCTX_BLOB by opening
#              a normal mlx5_ib ucontext on the source VF, two-pass
#              QUERY_UCONTEXT'ing it, and writing
#              meta+uar_table+bfreg_count to disk. Must-pass (it's
#              just a query, no FW state change risk).
#   Phase D.5  consume that snapshot on the destination by opening
#              a ucontext with MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
#              calling RESTORE_UCONTEXT, re-QUERYing to confirm
#              bitwise identity, and mmap()ing one static NC UAR
#              page as a sanity probe. Must-pass: the cross-host
#              UAR persistence experiment (PASS-B + PASS-A) proved
#              the seeded FW UAR ids are valid post-LOAD_VHCA_STATE.
# Stand-alone (no CRIU dependency); validates the wire claim
# end-to-end at user space.
UCTX=${UCTX:-0}
UCTX_TOOL=${UCTX_TOOL:-$ROOT_DIR/tools/ucontext_vendor_verbs}
UCTX_BLOB=${UCTX_BLOB:-${BLOB}.uctx}

case "$ROLE" in
    source|destination|both) ;;
    *) echo "ROLE must be one of: source, destination, both"; exit 2 ;;
esac

[ -x "$TOOL" ] || { echo "build $TOOL first: make -C $ROOT_DIR"; exit 1; }

if [ "$UCTX" = "1" ]; then
    [ -x "$UCTX_TOOL" ] || { echo "build $UCTX_TOOL first (UCTX=1): make -C $ROOT_DIR"; exit 1; }
fi

CDEV="/dev/mlx5_vfmig/$PF"
[ -e "$CDEV" ] || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

# --- helpers -----------------------------------------------------------

vf_path()       { echo "/sys/bus/pci/devices/$1"; }
wait_for_path() { for i in $(seq 1 50); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }

# Safely bind a PCI device to mlx5_core with a bounded wait.
#
# The synchronous "echo BDF | tee /sys/bus/pci/drivers/mlx5_core/bind"
# holds &dev->mutex for the entire probe path. On a tracked VF in our
# layered build-out, any not-yet-hooked DMA path that FW dereferences
# (cmd ring, MANAGE_PAGES pages, EQ buffers, ...) silently faults; FW
# never completes the cmd; the driver waits 60s then leaks the cmd
# resource and continues to the next failing op; eventually the probe
# wedges entirely with the device_lock + devlink_lock + dev_lock chain
# held. Once that happens, NO subsequent operation involving any mlx5
# device can complete (rmmod, sriov_numvfs=0, even other VF binds), and
# the only way out is a reboot.
#
# Backgrounding the bind doesn't unblock the kernel-side probe -- the
# wedged task still holds those mutexes -- but it does let THIS script
# exit cleanly so the rest of your shell stays usable for diagnosis.
# We poll for either driver-attached success, a known-failure dmesg
# sentinel, or the timeout.
#
# Args:
#   $1 = VF BDF
#   $2 = timeout seconds (default 60)
#   $3 = label printed on hang (e.g. "Phase A bind")
# Returns:
#   0  bound successfully to mlx5_core
#   1  bind returned (background tee exited) but VF is not attached:
#      probe ran to completion and reported failure -- see dmesg
#   2  timeout: kernel-side probe is wedged; reboot required before
#      any further mlx5 work can succeed
bind_vf_safe() {
    local vf=$1
    local timeout=${2:-60}
    local label=${3:-bind}
    local started bg
    started=$(date +%s)

    ( echo "$vf" | sudo tee /sys/bus/pci/drivers/mlx5_core/bind ) \
        >/dev/null 2>&1 &
    bg=$!

    while :; do
        if [ -e "$(vf_path $vf)/driver" ]; then
            wait "$bg" 2>/dev/null || true
            return 0
        fi
        if ! kill -0 "$bg" 2>/dev/null; then
            # tee returned. Either probe succeeded (caught above next
            # iter) or returned an error -- give it a tick to settle.
            sleep 0.2
            if [ -e "$(vf_path $vf)/driver" ]; then
                return 0
            fi
            return 1
        fi
        if [ $(( $(date +%s) - started )) -ge "$timeout" ]; then
            echo "" >&2
            echo "ERROR: $label has been pending for ${timeout}s; kernel probe wedged." >&2
            echo "       Background pid $bg is in uninterruptible sleep and cannot be killed." >&2
            echo "       Reboot is required before the next mlx5 operation can complete." >&2
            return 2
        fi
        sleep 0.5
    done
}

# Wrap the SET_TRACKED ioctl so a stale-kernel-module mismatch
# (ENOTTY because the loaded mlx5_core predates the SET_TRACKED ioctl
# number) prints an actionable recovery hint instead of just
# "Inappropriate ioctl for device". This is the most common
# post-reboot footgun: initramfs autoloads an older mlx5_core, and a
# subsequent `modprobe mlx5_core` is a no-op because the module is
# already resident.
vfmig_set_tracked() {
    local pf=$1 vf_id=$2 enable=$3
    local out rc
    set +e
    out=$(sudo "$TOOL" "$pf" set_tracked "$vf_id" "$enable" 2>&1)
    rc=$?
    set -e
    echo "$out"
    if [ "$rc" -ne 0 ] && echo "$out" | grep -q 'Inappropriate ioctl'; then
        cat >&2 <<EOF

ERROR: SET_TRACKED ioctl rejected (ENOTTY).
The loaded mlx5_core does not recognize the SET_TRACKED ioctl number.
Most likely the running kernel module predates the userspace tool --
e.g. initramfs autoloaded a stale copy at boot and modprobe was a
no-op because mlx5_core was already resident.

Recovery (run from /opt/builds/linux):
  echo 0 | sudo tee /sys/bus/pci/devices/$pf/sriov_numvfs
  sudo rmmod mlx5_ib mlx5_fwctl mlx5_core 2>/dev/null
  sudo cp drivers/net/ethernet/mellanox/mlx5/core/mlx5_core.ko \\
     /lib/modules/\$(uname -r)/kernel/drivers/net/ethernet/mellanox/mlx5/core/
  sudo depmod -a
  sudo modprobe mlx5_core

To prevent recurrence after the next reboot, regenerate the initramfs
once you've installed a fresh module:
  sudo update-initramfs -u   # Debian/Ubuntu
  sudo dracut --force        # RHEL/Fedora
EOF
    fi
    return "$rc"
}

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

# Walk /sys/class/infiniband/ and find the ib_device whose
# `device` symlink points at $bdf. Empty stdout + non-zero return on
# miss. The mlx5_ib auxiliary driver creates these as soon as the VF
# binds (see L4 R1 -- the ib_device registers even on a restored VF
# even though port 1 won't open).
find_ib_dev_for_pci() {
    local bdf=$1
    local d pci
    for d in /sys/class/infiniband/*; do
        [ -e "$d/device" ] || continue
        pci=$(basename "$(readlink "$d/device")")
        if [ "$pci" = "$bdf" ]; then
            basename "$d"
            return 0
        fi
    done
    return 1
}

# Optional ibv_rc_pingpong self-loopback against $ibdev.
#
# This drives an actual RC QP through INIT->RTR->RTS->RTS data exchange
# -- the smallest end-to-end test of the RDMA datapath that doesn't
# require XRC/SRQ. The command requires rdma-core's perftest helper
# (`apt install perftest libibverbs-utils ibverbs-utils`).
#
# We run server and client in two background processes targeting the
# same ib_device on TCP loopback -- this is what `man ibv_rc_pingpong`
# documents as the self-test pattern.
#
# Args:
#   $1 = ib_device name (e.g. "mlx5_2")
#   $2 = label printed on success/failure
#   $3 = expectation: "must-pass" (return 1 on failure) or "best-effort"
#        (return 0 even on failure, just log).
# Returns:
#   0  pingpong exchange completed cleanly OR best-effort failure
#   1  must-pass failure
#   2  ibv_rc_pingpong not installed (always non-fatal; logs and returns 0)
pingpong_self_loopback() {
    local ibdev=$1 label=$2 expect=${3:-best-effort}
    local logdir port server_pid client_rc

    if ! command -v ibv_rc_pingpong >/dev/null; then
        echo "  WARN: ibv_rc_pingpong not installed; skipping $label"
        echo "        (apt install ibverbs-utils  /  dnf install libibverbs-utils)"
        return 0
    fi

    logdir=$(mktemp -d)
    # Random-ish port to avoid clashing with a previous run that
    # didn't clean up. ibv_rc_pingpong defaults to 18515.
    port=$(( 18000 + RANDOM % 1000 ))

    echo "  $label: ibv_rc_pingpong -d $ibdev -p $port self-loopback"

    # Server: backgrounds, listens on $port, handles one client and exits.
    sudo ibv_rc_pingpong -d "$ibdev" -p "$port" -i 1 -n 100 \
        > "$logdir/server.log" 2>&1 &
    server_pid=$!

    # Give the server a moment to bind its listen socket.
    sleep 1

    # Client: connects to 127.0.0.1:$port. We bound the timeout
    # generously: a healthy pingpong completes in well under 5s.
    set +e
    sudo timeout 30 ibv_rc_pingpong -d "$ibdev" -p "$port" -i 1 -n 100 \
        127.0.0.1 > "$logdir/client.log" 2>&1
    client_rc=$?
    set -e

    # Reap the server. If it's still running it's blocked accepting --
    # client died before connecting -- tear it down explicitly.
    if kill -0 "$server_pid" 2>/dev/null; then
        sudo kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    else
        wait "$server_pid" 2>/dev/null || true
    fi

    if [ "$client_rc" -eq 0 ]; then
        echo "  PASS: $label completed (n=100 RC pingpongs on $ibdev)"
        rm -rf "$logdir"
        return 0
    fi

    echo "  $expect FAIL: $label rc=$client_rc"
    echo "  -- client log ($logdir/client.log) --"
    sed 's/^/    /' "$logdir/client.log"
    echo "  -- server log ($logdir/server.log) --"
    sed 's/^/    /' "$logdir/server.log"
    rm -rf "$logdir"

    if [ "$expect" = "must-pass" ]; then
        return 1
    fi
    return 0
}

# --- Phase A: provision SOURCE with set_tracked + migratable ----------
#
# Ordering: autoprobe=0 -> sriov_numvfs=1 -> set_tracked -> enable_migratable -> bind.
# set_tracked must precede bind (set_tracked rejects -EBUSY on a bound VF) and
# must precede the cmd-ring allocation that the bind triggers (otherwise
# alloc_cmd_page falls through to dma_alloc_coherent and the source's IOVA
# layout is lost forever for this generation of the VF).

PRE=""
SAVE_BYTES=0

if [ "$ROLE" = "source" ] || [ "$ROLE" = "both" ]; then

echo "=== Phase A: provision SOURCE (set_tracked + migratable) + snapshot baseline ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe"
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs"

wait_for_path "$(vf_path $PF)/virtfn0"
VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "VF is $VF"

# Engage IOVA tracking BEFORE the bind that triggers cmd ring allocation.
vfmig_set_tracked "$PF" 0 1
sudo "$TOOL" "$PF" enable_migratable 0

echo mlx5_core | sudo tee "$(vf_path $VF)/driver_override"
bind_vf_safe "$VF" 60 "Phase A: source VF bind" || rc=$?
case "${rc:-0}" in
    0) ;;
    1) echo "FAIL: Phase A source bind failed -- inspect dmesg, no reboot needed"; exit 1 ;;
    2) exit 1 ;;
esac
unset rc

DRV_PRE=$(basename "$(readlink "$(vf_path $VF)/driver")")
[ "$DRV_PRE" = "mlx5_core" ] || { echo "expected mlx5_core, got $DRV_PRE"; exit 1; }

sleep 1
PRE=$(snapshot_vf "$VF" "PRE")
echo "$PRE"

# Optional UID probe hook (PROBE_UID=1). Issues CREATE_UCTX +
# DESTROY_UCTX against the just-bound source VF and emits the
# allocated uid as a sentinel-tagged line we can grep for from
# probe_uar_persistence.sh and friends. Cheap, no firmware state
# left behind.
if [ "${PROBE_UID:-0}" = "1" ]; then
    SRC_UID_OUT=$(sudo "$TOOL" "$PF" probe_uid 0 || true)
    echo "[probe_uid src] $SRC_UID_OUT"
    SRC_UID_OUT2=$(sudo "$TOOL" "$PF" probe_uid 0 || true)
    echo "[probe_uid src] $SRC_UID_OUT2"
fi

# Optional RDMA datapath check on the source-side (non-restored,
# but tracked) VF.
#
# Treated as best-effort, NOT must-pass. The reason is subtle and
# worth documenting in-line so a future reader doesn't try to "fix"
# the test by re-promoting it:
#
#   The vfmig v1 deterministic IOVA allocator hooks a curated set of
#   mlx5_core allocation sites (cmd ring, MANAGE_PAGES, EQ buffers,
#   frag bufs, DB pages, DMA_COHERENT slots). Other DMA paths --
#   crucially mlx5_ib_reg_user_mr's ib_umem_get -> dma_map_sgtable
#   path for user-MR backing pages -- are NOT hooked. On a tracked
#   VF those calls go through the kernel's default DMA-IOMMU path
#   against our unmanaged domain and produce IOVAs that are never
#   installed in our domain's page tables. FW dereferences them
#   during the UMR PAS update and returns IB_WC_MW_BIND_ERR (vendor
#   syndrome 0x25, "memory bind error"), which surfaces in user
#   space as ibv_reg_mr failing with "Couldn't register MR".
#
#   This is independent of L4 R1 / R1.5 / the netdev gate -- it's a
#   pre-existing v1 allocator coverage gap that the pingpong test
#   simply happens to be the first thing to exercise. The fix lives
#   in v2 (extend allocator to cover user-MR DMA-map; same shape of
#   work as R3 MR rebinding). Tracked under the
#   v1_allocator_user_mr_dma_gap todo.
#
#   When the v2 allocator lands, this expectation can be flipped to
#   "must-pass" again. Until then, treat the actual rc as a
#   diagnostic data point.
if [ "$PINGPONG" = "1" ]; then
    src_ibdev=$(find_ib_dev_for_pci "$VF") || src_ibdev=""
    if [ -n "$src_ibdev" ]; then
        echo "=== Phase A.1: ibv_rc_pingpong self-loopback (source, best-effort) ==="
        echo "  NOTE: failure here is the v1 allocator's user-MR DMA gap, not"
        echo "        an R1.5 regression. See in-line comment in this script."
        pingpong_self_loopback "$src_ibdev" \
            "Phase A.1 (tracked VF $VF / $src_ibdev)" "best-effort"
    else
        echo "WARN: no ib_device for $VF; skipping Phase A.1 pingpong"
    fi
fi

# Sanity log: confirm the IOVA hook actually fired for the cmd ring.
echo "--- cmd ring IOVA registry sanity (expect at least one HOST_PAGE entry) ---"
sudo dmesg | grep -E 'vfmig_iova: vf 0 domain attached|vfmig: cmd ring at iova' | tail -10 || true

# --- Phase A.5: emit UCTX snapshot to disk (UCTX=1) -------------------

if [ "$UCTX" = "1" ]; then
    src_ibdev_uctx=$(find_ib_dev_for_pci "$VF") || src_ibdev_uctx=""
    if [ -z "$src_ibdev_uctx" ]; then
        echo "FAIL: UCTX=1 but no ib_device for source VF $VF"
        exit 1
    fi
    echo "=== Phase A.5: UCTX snapshot ($src_ibdev_uctx -> $UCTX_BLOB) ==="
    sudo "$UCTX_TOOL" save_uctx_snapshot "$src_ibdev_uctx" "$UCTX_BLOB"
    sudo chmod 0644 "$UCTX_BLOB"
    UCTX_SHA=$(sha256sum "$UCTX_BLOB" | awk '{print $1}')
    UCTX_BYTES=$(stat -c %s "$UCTX_BLOB")
    echo "uctx snapshot sha256: $UCTX_SHA ($UCTX_BYTES bytes)"
fi

# --- Phase B: SAVE -----------------------------------------------------

echo "=== Phase B: SAVE (now emits HOST_PAGE records before FW_DATA) ==="
sudo dmesg -C
# This harness is deliberately the LEGACY-standalone regression: no
# explicit SUSPEND/RESUME_VHCA bracket, so SAVE exercises its own
# self-suspend-on-entry + resume-on-close path (design A.4, owns_suspend
# = true from RUNNING). This is byte-identical to the pre-split behavior
# and is the one full save/load roundtrip that guards it -- the
# directional two-phase dump bracket (Appendix D.3) is covered by the
# other save_load / uobject_restore harnesses and by
# test_directional_suspend_resume.sh.
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB" $SAVE_FLAGS
# The save tool creates the blob 0600 root:root by default. For
# cross-host scp-as-user to succeed without sudo on both ends, drop
# it to world-readable. The blob holds firmware migration state, no
# host secrets.
sudo chmod 0644 "$BLOB"
ls -l "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
[ "$SAVE_BYTES" -gt 16 ] || { echo "blob suspiciously small: $SAVE_BYTES"; exit 1; }
SAVE_SHA=$(sha256sum "$BLOB" | awk '{print $1}')
echo "blob sha256: $SAVE_SHA"

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

# Manifest sibling for cross-host: lets the destination role compare
# its POST snapshot against the source's PRE without an out-of-band
# channel. Single-host runs use it too; harmless.
sudo tee "$META" >/dev/null <<EOF
# vfmig SAVE manifest (sibling to $BLOB)
pre=$PRE
save_bytes=$SAVE_BYTES
save_sha256=$SAVE_SHA
src_kernel=$(uname -r)
src_host=$(hostname)
src_pf=$PF
src_vf=$VF
uctx_enabled=$UCTX
uctx_sha256=${UCTX_SHA:-}
uctx_bytes=${UCTX_BYTES:-0}
EOF
sudo chmod 0644 "$META"
echo "wrote manifest $META"
cat "$META"

if [ "$ROLE" = "source" ]; then
    echo
    echo "==== Source role done ===="
    if [ "$UCTX" = "1" ]; then
        echo "Copy these to the destination host, e.g.:"
        echo "  scp $BLOB $META $UCTX_BLOB user@DEST_HOST:/tmp/"
        echo "Then on the destination:"
        echo "  sudo PF=$PF ROLE=destination UCTX=1 $0"
    else
        echo "Copy these to the destination host, e.g.:"
        echo "  scp $BLOB $META user@DEST_HOST:/tmp/"
        echo "Then on the destination:"
        echo "  sudo PF=$PF ROLE=destination $0"
    fi
    exit 0
fi

fi  # ROLE = source | both

# --- Phase C: tear down + re-provision DESTINATION (no autoprobe) -----

if [ "$ROLE" = "destination" ]; then
    [ -e "$BLOB" ] || { echo "missing blob $BLOB (copy it from source host)"; exit 1; }
    SAVE_BYTES=$(stat -c %s "$BLOB")
    if [ -e "$META" ]; then
        echo "--- source manifest ($META) ---"
        cat "$META"
        PRE=$(grep '^pre=' "$META" | sed 's/^pre=//')
        # Cross-host integrity check: refuse to LOAD a blob that does
        # not match the manifest's recorded sha256. This catches the
        # very-easy-to-make mistake of scp'ing the manifest but not
        # the blob (or only one of them updating), which would
        # otherwise silently restore from a stale local blob and look
        # like a successful cross-host run.
        EXPECT_SHA=$(grep '^save_sha256=' "$META" | sed 's/^save_sha256=//')
        if [ -n "$EXPECT_SHA" ]; then
            ACTUAL_SHA=$(sha256sum "$BLOB" | awk '{print $1}')
            if [ "$EXPECT_SHA" != "$ACTUAL_SHA" ]; then
                echo "FAIL: blob sha256 mismatch -- manifest expects $EXPECT_SHA"
                echo "      actual blob is             $ACTUAL_SHA"
                echo "      did you scp $BLOB across?"
                exit 1
            fi
            echo "blob sha256 matches manifest: $ACTUAL_SHA"
        else
            echo "WARN: manifest has no save_sha256; cannot verify blob integrity"
        fi
        EXPECT_BYTES=$(grep '^save_bytes=' "$META" | sed 's/^save_bytes=//')
        if [ -n "$EXPECT_BYTES" ] && [ "$EXPECT_BYTES" != "$SAVE_BYTES" ]; then
            echo "FAIL: blob size mismatch -- manifest expects $EXPECT_BYTES, actual $SAVE_BYTES"
            exit 1
        fi
        # Cross-host integrity for the UCTX sidecar (UCTX=1 only).
        # Same shape of check as the main blob: refuse to run Phase
        # D.5 if the sidecar doesn't match the manifest.
        if [ "$UCTX" = "1" ]; then
            MAN_UCTX_ENABLED=$(grep '^uctx_enabled=' "$META" | sed 's/^uctx_enabled=//')
            if [ "$MAN_UCTX_ENABLED" != "1" ]; then
                echo "FAIL: UCTX=1 but source manifest reports uctx_enabled=$MAN_UCTX_ENABLED"
                exit 1
            fi
            [ -e "$UCTX_BLOB" ] || { echo "FAIL: UCTX=1 but missing sidecar $UCTX_BLOB"; exit 1; }
            EXPECT_UCTX_SHA=$(grep '^uctx_sha256=' "$META" | sed 's/^uctx_sha256=//')
            ACTUAL_UCTX_SHA=$(sha256sum "$UCTX_BLOB" | awk '{print $1}')
            if [ -n "$EXPECT_UCTX_SHA" ] && [ "$EXPECT_UCTX_SHA" != "$ACTUAL_UCTX_SHA" ]; then
                echo "FAIL: uctx blob sha256 mismatch -- manifest expects $EXPECT_UCTX_SHA"
                echo "      actual sidecar is              $ACTUAL_UCTX_SHA"
                echo "      did you scp $UCTX_BLOB across?"
                exit 1
            fi
            echo "uctx blob sha256 matches manifest: $ACTUAL_UCTX_SHA"
        fi
    else
        echo "WARN: no source manifest at $META; POST won't be compared to PRE"
        PRE="(no source manifest)"
    fi
fi

if [ "$ROLE" = "destination" ] || [ "$ROLE" = "both" ]; then

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
vfmig_set_tracked "$PF" 0 1
sudo "$TOOL" "$PF" enable_migratable 0

sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" mark_restored 0

# L1b expectation: probe should NOT hit ENABLE_HCA(0x104) timeout
# anymore. It may still fail somewhere later (QUERY_HCA_CAP /
# MANAGE_PAGES / etc.) because we don't yet preserve those pages --
# that's L2/L3. The keystone here is the failure mode shift.
echo mlx5_core | sudo tee "$(vf_path $VF2)/driver_override"
bind_vf_safe "$VF2" 60 "Phase D: post-LOAD destination VF bind" || rc=$?
case "${rc:-0}" in
    0) echo "OK: destination VF bound (this is genuinely surprising at the L1b layer)" ;;
    1) echo "INFO: destination probe failed cleanly (expected at L1b); inspect dmesg" ;;
    2) exit 1 ;;
esac
unset rc

echo "--- dmesg after LOAD+BIND ---"
sudo dmesg | tail -60

# Optional UID probe hook on the destination side (paired with the
# source-side hook in Phase A). If the source uid space is preserved
# across LOAD_VHCA_STATE, the destination's first probe should land
# strictly above the source's high-water-mark; if FW reset the table
# the destination's first probe should match the source's first
# probe.
if [ "${PROBE_UID:-0}" = "1" ] && [ -e "$(vf_path $VF2)/driver" ]; then
    DST_UID_OUT=$(sudo "$TOOL" "$PF" probe_uid 0 || true)
    echo "[probe_uid dst] $DST_UID_OUT"
fi

# Optional RDMA datapath check on the restored VF. Treated as
# best-effort: TWO independent reasons it's expected to fail today:
#
#   1) L4 R1 mlx5_ib port-1 gating: Couldn't create ib_mad QP1 ->
#      Couldn't open port 1, so ibv_query_port returns IB_PORT_DOWN
#      (or fails outright) and pingpong can't even modify a QP
#      to RTR. Failure mode shifts to "connect timeout" once R2
#      imports the dev_res FW objects and port 1 opens.
#   2) v1 allocator user-MR DMA gap (same as Phase A.1): even if
#      port 1 were open, ibv_reg_mr would still return ENOMEM/EIO
#      with the "memory bind error" UMR completion before any QP
#      transition happens. Resolved by v2 allocator's user-MR
#      coverage.
#
# We still run it because the failure mode is a useful regression
# signal: a *crash* here on a tracked-and-restored VF (rather than
# a clean error return) would be genuinely interesting. The
# original mlx5e_poll_tx_cq FIFO-underflow crash that motivated the
# netdev gate was exactly such a failure.
if [ "$PINGPONG" = "1" ] && [ -e "$(vf_path $VF2)/driver" ]; then
    dst_ibdev=$(find_ib_dev_for_pci "$VF2") || dst_ibdev=""
    if [ -n "$dst_ibdev" ]; then
        echo "=== Phase D.1: ibv_rc_pingpong self-loopback (restored, best-effort) ==="
        pingpong_self_loopback "$dst_ibdev" \
            "Phase D.1 (restored VF $VF2 / $dst_ibdev)" "best-effort"
    else
        echo "WARN: no ib_device for $VF2; skipping Phase D.1 pingpong"
    fi
fi

# --- Phase D.5: consume UCTX snapshot on destination (UCTX=1) ---------
#
# Run AFTER the destination bind has succeeded so the ib_device exists.
# Treated as must-pass: the cross-host UAR persistence experiment
# (PASS-A + PASS-B in this session) demonstrated that the FW UAR ids
# captured on the source remain valid after LOAD_VHCA_STATE, and the
# in-handler strict META + static-slot validity checks at RESTORE time
# would catch any silent drift before mmap. A failure here therefore
# means an honest regression of one of: (a) the alloc-flag short-
# circuit (step 1), (b) the QUERY snapshot fidelity (step 2), (c) the
# RESTORE preconditions or apply (step 3), or (d) FW's preservation of
# per-VHCA UAR allocator state across LOAD_VHCA_STATE.
if [ "$UCTX" = "1" ] && [ -e "$(vf_path $VF2)/driver" ]; then
    dst_ibdev_uctx=$(find_ib_dev_for_pci "$VF2") || dst_ibdev_uctx=""
    if [ -z "$dst_ibdev_uctx" ]; then
        echo "FAIL: UCTX=1 but no ib_device for restored VF $VF2"
        exit 1
    fi
    echo "=== Phase D.5: UCTX restore ($UCTX_BLOB -> $dst_ibdev_uctx) ==="
    sudo "$UCTX_TOOL" restore_uctx_snapshot "$dst_ibdev_uctx" "$UCTX_BLOB"
elif [ "$UCTX" = "1" ]; then
    echo "FAIL: UCTX=1 but VF $VF2 not bound -- can't run Phase D.5"
    exit 1
fi

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
echo "Pass condition (L0-L3): post-LOAD bind reaches mlx5_core/mlx5e cleanly with"
echo "vhca_id and MAC preserved. mlx5_ib SRQ/QP1 init failure is L4 territory and"
echo "does not invalidate the L0-L3 result; it just means RDMA verbs aren't usable yet."

# --- Optional ping smoke test ----------------------------------------
# If both PING_LOCAL_CIDR and PING_TARGET are set, assign the CIDR
# to the restored VF's netdev, ping the target a few times, then
# remove the address. This is the simplest possible "the VF is
# really alive" check at the L2/L3 layer.

if [ -n "$PING_LOCAL_CIDR" ] && [ -n "$PING_TARGET" ]; then
    echo
    echo "=== Phase F: post-restore ping smoke test ==="
    if [ ! -e "$(vf_path $VF2)/driver" ]; then
        echo "FAIL: VF $VF2 not bound; cannot ping"
        exit 1
    fi
    iface=$(ls "$(vf_path $VF2)/net/" 2>/dev/null | head -n1 || true)
    [ -n "$iface" ] || { echo "FAIL: no netdev under $(vf_path $VF2)/net/"; exit 1; }
    echo "ping setup: iface=$iface local=$PING_LOCAL_CIDR target=$PING_TARGET"
    sudo ip link set "$iface" up
    sudo ip addr add "$PING_LOCAL_CIDR" dev "$iface" || true
    sleep 1
    set +e
    sudo ping -c "$PING_COUNT" -W 2 -I "$iface" "$PING_TARGET"
    ping_rc=$?
    set -e
    sudo ip addr del "$PING_LOCAL_CIDR" dev "$iface" 2>/dev/null || true
    if [ "$ping_rc" -eq 0 ]; then
        echo "OK: restored VF pings $PING_TARGET successfully"
    else
        echo "FAIL: ping $PING_TARGET returned rc=$ping_rc"
        exit 1
    fi
fi

fi  # ROLE = destination | both

# Cleanup leaves VF in restored state for inspection. To reset:
#   sudo $TOOL $PF set_tracked 0 0
#   echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs
