#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S6b stale-dmac fix harness -- userspace re-trigger of the kernel
# refresh path.
#
# Companion to check_qp_av_dmac.sh (the same-directory diagnostic
# that emits the VERDICT=DMAC_IS_LOCAL pre-fix signal). This script
# drives the dev-branch backup ioctl
# MLX5_VFMIG_IOC_REFRESH_AV_DMAC, which mirrors the always-on
# mlx5_ib_restore_qp_refresh_av_dmac path but is triggered from
# userspace AFTER pin_static_neighbor_* has populated the
# destination ARP/NDISC table.
#
# Why we need this script and not just the always-on kernel path:
# the v0 mlx5_sriov_vfmig CRIU plugin invokes RESTORE_QP atomically
# with the rest of VF restore -- BEFORE the test harness's
# pin_static_neighbor_* step runs. The always-on
# mlx5_ib_restore_qp_refresh_av_dmac call inside RESTORE_QP
# therefore hits Policy A (log + skip) on every restored RC/UC QP
# and the QPC is left with the stale source-resolved dmac. This
# script is the manual escape hatch: the harness pins neighbors
# AFTER restore, then invokes this per restored QP, and the dmac
# is re-resolved against the now-populated neighbor table.
#
# Mechanism:
#   1. Resolve PF -> VF_BDF -> IBDEV / IFACE the same way
#      check_qp_av_dmac.sh does (sysfs walk).
#   2. Print check_qp_av_dmac.sh's pre-state line so the harness log
#      shows the (LOCAL_MAC, NEIGH_MAC, QPC.dmac) triple BEFORE
#      we touch the QP.
#   3. Invoke mlx5_vfmig refresh_av_dmac <vf_id> <qpn>. The kernel
#      handler does QUERY_QP -> neigh_lookup ->
#      MODIFY_QP(RTS2RTS, PRIMARY_ADDR_PATH) -> QUERY_QP and emits
#      a verdict line.
#   4. Re-print the verdict and surface key fields.
#
# Usage:
#   sudo PF=0000:08:00.0 [IBDEV=mlx5_2] [IFACE=eth4] \
#       ./refresh_av_dmac.sh <vf_id> <qpn>
#
# Exit codes:
#    0  ran successfully and emitted a verdict (read verdict= line)
#    1  argv / environment problem
#    2  refresh_av_dmac ioctl returned non-zero (see stderr)
#    3  could not resolve IBDEV/IFACE from PF + vf_id

set -euo pipefail

PF=${PF:-0000:00:08.0}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
CDEV="/dev/mlx5_vfmig/$PF"

if [ $# -ne 2 ]; then
    echo "usage: PF=<pf_bdf> [IBDEV=<ibdev>] [IFACE=<iface>] $0 <vf_id> <qpn>" >&2
    echo "  PF defaults to 0000:00:08.0; IBDEV / IFACE are auto-derived if unset." >&2
    exit 1
fi

VF_ID=$1
QPN=$2

[ -x "$TOOL" ] || { echo "build $TOOL first (make -C tools/testing/mlx5_vfmig)" >&2; exit 1; }
[ -e "$CDEV" ] || { echo "missing $CDEV" >&2; exit 1; }

# Resolve VF bdf from (PF, vf_id) via the standard sysfs symlink.
VF_BDF=$(basename "$(readlink -f "/sys/bus/pci/devices/$PF/virtfn$VF_ID" 2>/dev/null)" 2>/dev/null || true)
if [ -z "$VF_BDF" ] || [ "$VF_BDF" = "virtfn$VF_ID" ]; then
    echo "ERROR: could not resolve PF=$PF vf_id=$VF_ID via /sys/bus/pci/devices/$PF/virtfn$VF_ID" >&2
    exit 3
fi

# IBDEV: walk /sys/class/infiniband and match device symlink against VF_BDF.
if [ -z "${IBDEV:-}" ]; then
    for d in /sys/class/infiniband/*; do
        [ -e "$d/device" ] || continue
        if [ "$(basename "$(readlink -f "$d/device")")" = "$VF_BDF" ]; then
            IBDEV=$(basename "$d")
            break
        fi
    done
    if [ -z "${IBDEV:-}" ]; then
        echo "ERROR: could not resolve IBDEV for VF_BDF=$VF_BDF; pass IBDEV=<name> explicitly" >&2
        exit 3
    fi
fi

# IFACE: the VF's netdev under /sys/bus/pci/devices/<vf>/net/<iface>.
if [ -z "${IFACE:-}" ]; then
    IFACE=$(ls "/sys/bus/pci/devices/$VF_BDF/net/" 2>/dev/null | head -n1 || true)
    if [ -z "$IFACE" ]; then
        echo "ERROR: VF_BDF=$VF_BDF has no netdev (is it bound to mlx5_core?); pass IFACE=<iface>" >&2
        exit 3
    fi
fi

LOCAL_MAC=$(cat "/sys/class/net/$IFACE/address" 2>/dev/null || true)

echo "refresh_av_dmac: PF=$PF VF_ID=$VF_ID VF_BDF=$VF_BDF IBDEV=$IBDEV IFACE=$IFACE QPN=$QPN"
echo "refresh_av_dmac: local NIC MAC ($IFACE) = ${LOCAL_MAC:-<unread>}"

# Drive the ioctl. Capture stderr too: a failed neigh lookup or
# MODIFY_QP propagates as kernel info-level dmesg, but the tool's
# stdout is what we parse for the verdict.
set +e
OUT=$(sudo "$TOOL" "$PF" refresh_av_dmac "$VF_ID" "$QPN" 2>&1)
RC=$?
set -e

echo "--- refresh_av_dmac output (rc=$RC) ---"
echo "$OUT" | sed 's/^/  /'
echo "----------------------------------------"

if [ "$RC" -ne 0 ]; then
    cat >&2 <<EOF
ERROR: refresh_av_dmac ioctl returned rc=$RC. Possible causes:
  * QPN $QPN does not exist on VF $VF_ID (run \`rdma resource show qp link $IBDEV\`).
  * VF $VF_ID is not bound to mlx5_core.
  * Kernel built without the MLX5_VFMIG_IOC_REFRESH_AV_DMAC ioctl
    (you are running on a kernel that predates the dev-branch
    backup; the always-on mlx5_ib_restore_qp_refresh_av_dmac path
    runs unconditionally on every kernel that has the kernel-side
    fix).
EOF
    exit 2
fi

VERDICT=$(echo "$OUT" | awk -F= '/^verdict=/{print $2}')
PRE_DMAC=$(echo "$OUT"      | awk -F= '/^pre_dmac=/{print $2}')
RESOLVED_DMAC=$(echo "$OUT" | awk -F= '/^resolved_dmac=/{print $2}')
POST_DMAC=$(echo "$OUT"     | awk -F= '/^post_dmac=/{print $2}')
DMAC_CHANGED=$(echo "$OUT"  | awk -F= '/^dmac_changed=/{print $2}')

echo
echo "refresh_av_dmac: pre_dmac      = ${PRE_DMAC:-<unread>}"
echo "refresh_av_dmac: resolved_dmac = ${RESOLVED_DMAC:-<unread>}"
echo "refresh_av_dmac: post_dmac     = ${POST_DMAC:-<unread>}"
echo "refresh_av_dmac: dmac_changed  = ${DMAC_CHANGED:-<unread>}"
echo
case "${VERDICT:-MISSING}" in
REFRESHED_OK)
    cat <<EOF
*** VERDICT=REFRESHED_OK ***
The QPC's stale source-resolved dmac was rewritten to match the
destination's neighbor table entry. The data path should now make
forward progress against this peer; verify by re-running
check_qp_av_dmac.sh (it should report VERDICT=DMAC_IS_PEER) and by
issuing application traffic on this QP.
EOF
    ;;
ALREADY_OK)
    cat <<EOF
*** VERDICT=ALREADY_OK ***
The QPC's preserved dmac already matched the destination's neighbor
table entry. No MODIFY_QP was issued. This is the expected steady
state on true VM live-migration (where the peer's MAC is stable
across hosts) and on a re-run of this script after a previous
REFRESHED_OK.
EOF
    ;;
NEIGH_UNRESOLVED)
    cat <<EOF
*** VERDICT=NEIGH_UNRESOLVED ***
The destination netdev ($IFACE) does not have a NUD_VALID neighbor
entry for the QP's dgid. The harness must pin the neighbor before
this script can refresh the QPC. Typical recipe:

    ip neigh add <peer-ipv4> lladdr <peer-mac> dev $IFACE \\
        nud permanent

Then re-run this script with the same <vf_id> <qpn>.
EOF
    exit 2
    ;;
MODIFY_FAILED)
    cat <<EOF
*** VERDICT=MODIFY_FAILED ***
neigh_lookup succeeded and the harness identified a new dmac, but
firmware MODIFY_QP(RTS2RTS, PRIMARY_ADDR_PATH) was rejected.
Inspect op_status / op_syndrome above. Possible causes:
  * QP not in RTS state (the refresh path requires RTS for the
    RTS2RTS opcode; check pre_qpc_state == 3).
  * FW gating on uid for the modify command (the ioctl uses uid=0
    host-priv; if the QPC's owning uid restricts modifies, fall
    back to the userspace ibv_modify_qp(IBV_QP_AV) escape hatch
    via libibverbs).
EOF
    exit 2
    ;;
PRE_QUERY_FAILED|POST_QUERY_FAILED|MISSING|*)
    cat <<EOF
*** VERDICT=${VERDICT:-MISSING} ***
Inspect the full ioctl output above. PRE_QUERY_FAILED typically
means qpn $QPN doesn't exist on VF $VF_ID. POST_QUERY_FAILED is
unexpected -- the modify may have transitioned the QP into ERR.
EOF
    exit 2
    ;;
esac
