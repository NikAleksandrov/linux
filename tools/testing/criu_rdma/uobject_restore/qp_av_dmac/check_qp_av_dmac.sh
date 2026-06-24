#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S6b stale-dmac diagnostic harness.
#
# Asks the question that
# tools/testing/criu_rdma/design/qp_av_dmac_swap.md is debating:
# "After CRIU restore on a VF whose IP was reassigned, does the
# preserved QPC's resolved av.dmac point at the LOCAL NIC's own MAC
# (== self-addressed L2, the dmac-stale theory) or at the actual
# peer NIC's MAC (== bug is elsewhere, look at SQ/RQ state)?"
#
# Mechanism:
#   1. Run the existing MLX5_VFMIG_IOC_QUERY_QP via the CLI tool to
#      pull primary_address_path out of the FW QPC.
#   2. The tool now decodes av_dmac / av_dgid_ipv4 / av_sgid_index
#      / av_vhca_port_num from the blob (single line each).
#   3. Look up the local NIC's MAC from sysfs.
#   4. Look up `ip neigh show dev <iface>` for the dgid's IPv4.
#   5. Print all four side-by-side and emit a verdict line:
#        VERDICT=DMAC_IS_LOCAL  -- self-addressed, dmac-stale theory
#                                 confirmed; kernel-side
#                                 mlx5_ib_restore_qp av-refresh fix
#                                 is justified.
#        VERDICT=DMAC_IS_PEER   -- dmac points at the right MAC; the
#                                 RC RETRY_EXC bug is NOT a stale
#                                 dmac. Look at SQ doorbell / UAR
#                                 mapping / RQ state.
#        VERDICT=DMAC_AMBIGUOUS -- dmac matches neither the local NIC
#                                 nor any neighbor entry; could be
#                                 caching state or a third bug.
#                                 Inspect manually.
#
# Input requirements: this is a one-shot manual diagnostic. The
# operator runs it on EACH physical host post-restore (after CRIU
# has re-bound the VF and before any post-restore traffic), passing
# the (vf_id, qpn) for the migrated QP they are debugging. The qpn
# can be discovered with `rdma resource show qp link <ibdev>` or
# pulled from the test framework's pre-checkpoint state dump.
#
# Usage:
#   sudo PF=0000:08:00.0 IBDEV=mlx5_2 IFACE=eth4 \
#       ./check_qp_av_dmac.sh <vf_id> <qpn>
#
# IBDEV / IFACE auto-discovery: if either is left unset, the harness
# tries to derive them from PF + vf_id via sysfs. PF defaults to
# 0000:00:08.0 (the test rig default).
#
# Exit codes:
#    0  ran successfully and emitted a verdict (read VERDICT= line)
#    1  argv / environment problem
#    2  query_qp returned no av_dmac (kernel/UAPI/tool mismatch?)
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

[ -x "$TOOL" ]  || { echo "build $TOOL first (make -C tools/testing/criu_rdma)" >&2; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV"                                       >&2; exit 1; }

#
# Resolve VF bdf from (PF, vf_id) via the standard sysfs symlink
# /sys/bus/pci/devices/<pf>/virtfn<N>.
#
VF_BDF=$(basename "$(readlink -f "/sys/bus/pci/devices/$PF/virtfn$VF_ID" 2>/dev/null)" 2>/dev/null || true)
if [ -z "$VF_BDF" ] || [ "$VF_BDF" = "virtfn$VF_ID" ]; then
    echo "ERROR: could not resolve PF=$PF vf_id=$VF_ID via /sys/bus/pci/devices/$PF/virtfn$VF_ID" >&2
    exit 3
fi

#
# IBDEV: walk /sys/class/infiniband and match device symlink against VF_BDF.
#
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

#
# IFACE: the VF's netdev under /sys/bus/pci/devices/<vf>/net/<iface>.
# A bound mlx5_core VF has exactly one netdev there.
#
if [ -z "${IFACE:-}" ]; then
    IFACE=$(ls "/sys/bus/pci/devices/$VF_BDF/net/" 2>/dev/null | head -n1 || true)
    if [ -z "$IFACE" ]; then
        echo "ERROR: VF_BDF=$VF_BDF has no netdev (is it bound to mlx5_core?); pass IFACE=<iface>" >&2
        exit 3
    fi
fi

#
# Local NIC MAC of the VF, from sysfs.
#
LOCAL_MAC=$(cat "/sys/class/net/$IFACE/address" 2>/dev/null || true)
if [ -z "$LOCAL_MAC" ]; then
    echo "ERROR: cannot read /sys/class/net/$IFACE/address" >&2
    exit 3
fi

echo "check_qp_av_dmac: PF=$PF VF_ID=$VF_ID VF_BDF=$VF_BDF IBDEV=$IBDEV IFACE=$IFACE QPN=$QPN"
echo "check_qp_av_dmac: local NIC MAC ($IFACE)         = $LOCAL_MAC"

#
# Pull the FW QPC's primary_address_path via the cdev ioctl. We
# capture stderr too because the tool emits its EINVAL/ENODEV
# diagnostic on stderr -- we want it visible in the harness log
# regardless of pass/fail. `set -e` would otherwise terminate the
# whole script silently on a non-zero query_qp exit, swallowing
# the diagnostic.
#
set +e
OUT=$(sudo "$TOOL" "$PF" query_qp "$VF_ID" "$QPN" 2>&1)
QUERY_RC=$?
set -e
echo "--- query_qp output (rc=$QUERY_RC) ---"
echo "$OUT" | sed 's/^/  /'
echo "--------------------------------------"
if [ "$QUERY_RC" -ne 0 ]; then
    cat >&2 <<EOF
ERROR: query_qp returned rc=$QUERY_RC. Possible causes:
  * QPN $QPN does not exist on VF $VF_ID (run \`rdma resource show qp link $IBDEV\`
    to enumerate live QPs).
  * VF $VF_ID is not bound to mlx5_core (the cdev ioctl needs a bound VF).
  * vf_id $VF_ID is out of range for PF $PF (numvfs is too small).
EOF
    exit 2
fi

AV_DMAC=$(echo "$OUT" | awk -F= '/^av_dmac=/{print $2}')
AV_DGID_IPV4=$(echo "$OUT" | awk -F= '/^av_dgid_ipv4=/{print $2}')
AV_SGID_INDEX=$(echo "$OUT" | awk -F= '/^av_sgid_index=/{print $2}')
AV_PORT=$(echo "$OUT" | awk -F= '/^av_vhca_port_num=/{print $2}')
AV_GRH=$(echo "$OUT" | awk -F= '/^av_grh=/{print $2}')
QPC_STATE=$(echo "$OUT" | awk -F= '/^qpc_state=/{print $2}')

if [ -z "$AV_DMAC" ]; then
    echo "ERROR: query_qp produced no av_dmac line. Did you rebuild the tool?" >&2
    exit 2
fi

#
# Cross-check against ip neigh.
#
NEIGH_LINE=""
NEIGH_MAC=""
if [ -n "$AV_DGID_IPV4" ]; then
    NEIGH_LINE=$(ip neigh show dev "$IFACE" "$AV_DGID_IPV4" 2>/dev/null || true)
    NEIGH_MAC=$(echo "$NEIGH_LINE" | awk '/lladdr/{for (i=1; i<=NF; i++) if ($i == "lladdr") {print $(i+1); exit}}')
fi

#
# Lower-case both for compare.
#
norm() { echo "$1" | tr 'A-Z' 'a-z'; }
LOCAL_MAC_LC=$(norm "$LOCAL_MAC")
AV_DMAC_LC=$(norm "$AV_DMAC")
NEIGH_MAC_LC=$(norm "${NEIGH_MAC:-}")

echo
echo "check_qp_av_dmac: qpc_state                         = $QPC_STATE  (0=RST 1=INIT 2=RTR 3=RTS 4=SQEr 5=SQD 6=ERR)"
echo "check_qp_av_dmac: av_grh                            = ${AV_GRH:-<unset>}"
echo "check_qp_av_dmac: av_sgid_index                     = ${AV_SGID_INDEX:-<unset>}"
echo "check_qp_av_dmac: av_vhca_port_num                  = ${AV_PORT:-<unset>}"
echo "check_qp_av_dmac: av_dgid_ipv4                      = ${AV_DGID_IPV4:-<not v4>}"
echo "check_qp_av_dmac: ip neigh ($IFACE, $AV_DGID_IPV4)  = ${NEIGH_LINE:-<no entry>}"
echo "check_qp_av_dmac: peer MAC from neighbor table      = ${NEIGH_MAC:-<unresolved>}"
echo "check_qp_av_dmac: QPC.av.dmac (preserved from src)  = $AV_DMAC"
echo

if [ "$AV_DMAC_LC" = "$LOCAL_MAC_LC" ]; then
    cat <<EOF
*** VERDICT=DMAC_IS_LOCAL ***
The QPC's preserved av.dmac equals the LOCAL NIC's MAC.
Outgoing RoCEv2 frames from this QP have src_mac == dst_mac and
will be silently dropped by the fabric -- this confirms the
dmac-stale theory in design/qp_av_dmac_swap.md. The kernel-side
av-refresh fix in mlx5_ib_restore_qp is justified, with the
caveat (see analysis): the proposed mlx5_ib_modify_qp(IB_QP_AV)
primitive is rejected by ib_modify_qp_is_ok in RTS->RTS, so the
fix needs a direct mlx5_cmd_exec(MODIFY_QP, RTS2RTS,
opt_param_mask=PRIMARY_ADDR_PATH) instead.
EOF
    echo "VERDICT=DMAC_IS_LOCAL"
elif [ -n "$NEIGH_MAC_LC" ] && [ "$AV_DMAC_LC" = "$NEIGH_MAC_LC" ]; then
    cat <<EOF
*** VERDICT=DMAC_IS_PEER ***
The QPC's preserved av.dmac equals the neighbor table's MAC for
the dgid. The L2 destination is correct, so the post-restore
RETRY_EXC failure is NOT explained by stale dmac. Look elsewhere:
  * Is the SQ doorbell reaching FW? (RETRY_EXC implies yes -- a
    CQE was generated -- so doorbell is fine.)
  * Is the FW egressing the frame? Check tx_packets_phy delta.
  * Is the peer-side RQ corrupted? Check the peer's
    port_rcv_packets / hw_counters/rx_write_requests deltas.
  * Is the source/dest IP routing correct? (Different bug class
    -- IP-layer rather than L2-layer.)
The kernel-side av-refresh fix is NOT justified by this run.
EOF
    echo "VERDICT=DMAC_IS_PEER"
else
    cat <<EOF
*** VERDICT=DMAC_AMBIGUOUS ***
QPC.av.dmac matches neither the local NIC's MAC nor the neighbor
table entry for the dgid. Possible explanations:
  * The neighbor entry isn't installed yet (NUD_INCOMPLETE / no
    `pin_static_neighbor`).
  * dgid is IPv6 (no av_dgid_ipv4 decode path); compare manually
    against `ip -6 neigh show`.
  * Multiple MACs aliasing the same dgid (e.g. bonding).
Inspect the values above and decide manually.
EOF
    echo "VERDICT=DMAC_AMBIGUOUS"
fi
