#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# test_qp_restore_rxe.sh -- bring up a Soft-RoCE (rxe) device and drive
# the S6a QP save/restore probes against it (design/uobject_restore.md
# §9.1 S6a).
#
# Unlike the mlx5 VF harnesses (test_cq_adopt.sh et al.) there is no
# SR-IOV / SAVE_VHCA_STATE plumbing here: rxe is a software RDMA device
# with no firmware, so the whole save/restore round-trip happens inside
# a single host. This script's only job is environment setup:
#
#   1. modprobe rdma_rxe.
#   2. If no rxe link exists, create one over a usable netdev (loopback
#      works fine for self-loopback QPs) via `rdma link add`.
#   3. Run the dump-side probe (qp_query_probe_rxe): QUERY_QP field
#      fidelity + FREEZE_DATAPATH lifecycle on a live RC QP.
#   4. Run the end-to-end probe (qp_restore_probe_rxe): snapshot a live
#      RC QP, destroy it, RESTORE_QP at the source qpn on a restore-mode
#      ucontext, and assert the restored wire state is byte-identical.
#
# Any rxe link this script creates is torn down on exit; a pre-existing
# link supplied via $RXE_DEV is left alone.
#
# Usage:
#   sudo ./test_qp_restore_rxe.sh
#
# Knobs:
#   RXE_DEV    Use this existing rxe ibdev instead of creating one.
#   NETDEV     netdev to attach a new rxe link to (default: auto-pick).
#   QUERY_BIN  Path to qp_query_probe_rxe.
#   RESTORE_BIN Path to qp_restore_probe_rxe.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

QUERY_BIN=${QUERY_BIN:-$SCRIPT_DIR/../qp_query/qp_query_probe_rxe}
RESTORE_BIN=${RESTORE_BIN:-$SCRIPT_DIR/qp_restore_probe_rxe}

[ -x "$QUERY_BIN" ]   || { echo "build $QUERY_BIN first (make -C tools/testing/mlx5_vfmig)"; exit 1; }
[ -x "$RESTORE_BIN" ] || { echo "build $RESTORE_BIN first (make -C tools/testing/mlx5_vfmig)"; exit 1; }

if [ "$(id -u)" -ne 0 ]; then
    echo "NOTE: not root; modprobe / rdma link add may fail. Re-run with sudo if so." >&2
fi

CREATED_LINK=""
cleanup() {
    if [ -n "$CREATED_LINK" ]; then
        echo "=== tearing down rxe link $CREATED_LINK ==="
        rdma link delete "$CREATED_LINK" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# --- locate or create an rxe device ----------------------------------

rxe_ibdevs() {
    # An rxe ibdev exposes /sys/class/infiniband/<dev>/parent (the
    # backing netdev); real HCAs (mlx5 etc.) do not. This is a far more
    # reliable discriminator than parsing `rdma link show`, which lists
    # every link regardless of driver and would happily match mlx5_0.
    local d
    for d in /sys/class/infiniband/*; do
        [ -e "$d/parent" ] || continue
        basename "$d"
    done
}

pick_netdev() {
    # Prefer an UP non-loopback ethernet netdev; fall back to lo, which
    # is sufficient for the self-loopback RC QPs the probes build.
    local nd
    for nd in $(ls /sys/class/net 2>/dev/null); do
        [ "$nd" = "lo" ] && continue
        if [ "$(cat /sys/class/net/$nd/operstate 2>/dev/null)" = "up" ]; then
            echo "$nd"; return 0
        fi
    done
    echo "lo"
}

echo "=== loading rdma_rxe ==="
modprobe rdma_rxe 2>/dev/null || {
    [ -d /sys/module/rdma_rxe ] || { echo "FAIL: rdma_rxe not available (CONFIG_RDMA_RXE=m?)"; exit 1; }
}

RXE=""
if [ -n "${RXE_DEV:-}" ]; then
    RXE="$RXE_DEV"
    echo "using caller-supplied rxe device: $RXE"
else
    RXE="$(rxe_ibdevs | head -n1 || true)"
    if [ -z "$RXE" ]; then
        NETDEV=${NETDEV:-$(pick_netdev)}
        RXE="rxe0"
        echo "=== creating rxe link $RXE over netdev $NETDEV ==="
        rdma link add "$RXE" type rxe netdev "$NETDEV"
        CREATED_LINK="$RXE"
    else
        echo "using existing rxe device: $RXE"
    fi
fi

# Confirm the ibdev is visible before launching probes.
if [ ! -e "/sys/class/infiniband/$RXE" ]; then
    echo "FAIL: rxe ibdev $RXE not present after setup"
    exit 1
fi
echo "rxe device ready: $RXE"

# --- run the probes ---------------------------------------------------

rc=0

echo
echo "############ qp_query_probe_rxe ($RXE) ############"
if "$QUERY_BIN" "$RXE"; then
    echo "qp_query_probe_rxe: OK"
else
    echo "qp_query_probe_rxe: FAILED"
    rc=1
fi

echo
echo "############ qp_restore_probe_rxe ($RXE) ############"
if "$RESTORE_BIN" "$RXE"; then
    echo "qp_restore_probe_rxe: OK"
else
    echo "qp_restore_probe_rxe: FAILED"
    rc=1
fi

echo
if [ "$rc" -eq 0 ]; then
    echo "=== test_qp_restore_rxe: PASS ==="
else
    echo "=== test_qp_restore_rxe: FAIL ==="
fi
exit "$rc"
