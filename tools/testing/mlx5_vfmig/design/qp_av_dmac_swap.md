# DESIGN: §S6b -- stale `av.dmac` in migrated QPC after CRIU process-swap with IP reassignment

> Companion to [`uobject_restore.md`](uobject_restore.md). Documents the
> empirical evidence, architectural reasoning, and proposed kernel-side
> mitigation for an RC datapath failure observed end-to-end with
> `mlx5_sriov_vfmig` + CRIU process-swap, where post-restore RC traffic
> hangs and eventually surfaces `IBV_WC_RETRY_EXC_ERR` (status 12) on
> the first post-restore `post_send`.
>
> Filed by the CRIU agent after end-to-end testing with the
> `rdma_test_agent_vfmig_criu_swap_after_qp.yaml` harness. Diagnosis is
> tentative pending mid-failure capture (the original counter snapshots
> were taken post-cleanup and are stale). The "Outstanding evidence"
> section below lists the captures we still need before acting.

## TL;DR

`LOAD_VHCA_STATE` faithfully preserves the FW QPC, including the
**resolved `av.dmac`** that was written into the QPC at source-side
`MODIFY_QP_TO_RTR` time via the source kernel's neighbor lookup against
the source netdev's ARP table.

For a **CRIU-on-host process-swap** workload (where the *process* moves
to the opposite physical host but the *VFs stay put*, with peer IPs
reassigned to simulate VM-style mobility -- exactly what
`rdma_test_agent_vfmig_criu_swap_after_qp.yaml` exercises), the
preserved `dmac` no longer points at the peer NIC -- it points at the
**local NIC's own MAC**, because IP reassignment puts the peer's IP on
what is now the local NIC's MAC. Outgoing RoCEv2 frames are
self-addressed at L2 (`src_mac == dst_mac`) and are silently dropped
by the fabric.

The architectural fix this doc proposes:

```c
/* mlx5_ib_restore_qp -- after LOAD_VHCA_STATE has installed the FW
 * QPC verbatim, but BEFORE marking the uobject ready for userspace,
 * re-resolve av.dmac for RoCEv2 RC/UD QPs from the destination
 * netdev's neighbor table using the QPC's own dgid as the key.
 *
 * Implementation outline (Section 5 below has the full sketch):
 *   1. QUERY_QP to extract (dgid, sgid_index, port_num) from FW QPC.
 *   2. Confirm port link_layer == ETH (RoCE). InfiniBand is a no-op.
 *   3. neigh_lookup(dgid_to_ipv4(dgid), netdev) -> dmac.
 *   4. MODIFY_QP(IBV_QP_AV, av={..., dmac=new_dmac}) on the existing
 *      RTS state -- mlx5 supports IBV_QP_AV-only modify in RTS for
 *      RC. Returns -EAGAIN if neighbor entry is incomplete; the
 *      restore-mode caller can retry/poll or surface to userspace.
 */
```

This puts the dmac-refresh policy in the same place where every other
restore-time FW reconciliation lives, makes process-swap migration
**transparent to userspace** (no
`ibv_modify_qp(qp, &attr, IBV_QP_AV)` cooperation required from
applications), and matches the semantic the rest of the existing
RoCEv2 modify path already implements at `MODIFY_QP_TO_RTR` time.

The leak budget is **zero** -- this is a state-refresh, not a
resource allocation.

## Outstanding evidence (captures we still need)

The original tcpdump / ethtool / hw_counters snapshots were taken
**after** the YAML's `cleanup_mlx5_vf_dev_*` and `cleanup_vf_*` steps
ran, so they reflect a freshly-recreated VF instance with zero
history. The "0 packets" reading is consistent with both the dmac
hypothesis (fabric drops, but PHY-tx still ticks on the source) **and**
with a different bug entirely (SQ doorbell not reaching FW post-
restore, which would point at UAR-mapping or SQ-state breakage in
`mlx5_ib_restore_qp` itself).

Until we have mid-failure data, the "stale dmac" framing is
**tentative**. The harness change to capture it is documented in
Section 6.

The minimal set we need, in priority order:

1. `port_xmit_packets` and `tx_packets_phy` deltas across one failed
   `post_send` on the sender -- distinguishes "WR didn't reach FW"
   from "WR reached fabric, fabric dropped it".
2. `mlx5dv_query_qp` output for the migrated QP showing the resolved
   `av.dmac` field -- direct confirmation of the stale-vs-fresh
   prediction.
3. `tcpdump -e -n -i eth4 udp port 4791` pcap captured during the
   failed send -- if frames are visible with `src_mac == dst_mac`,
   diagnosis is locked.
4. `ip neigh show dev eth4` on both physical hosts during the failure
   window -- confirms the correct neighbor entry is in place
   pre-MODIFY_QP, so a future MODIFY_QP(IBV_QP_AV) would resolve to
   the correct dmac.

## 1. The failure

End-to-end run of
`rdma_test_agent_vfmig_criu_swap_after_qp.yaml`. Pre-checkpoint RC
ping/pong succeeds. Checkpoint, transfer, swap-restore (host1's
checkpoint restored on host2 and vice versa), VF IPs swapped, static
neighbors pinned. Post-restore the YAML attempts a fresh ping/pong on
the migrated QPs. The first sender's `post_send` blocks for ~4 s and
then surfaces:

```
2026-06-04 23:38:24 - framework.steps - INFO -
[host1:rdma_agent_host2:stdout] ERR VERBS_FAIL send completion status 12
```

`status 12` is `IBV_WC_RETRY_EXC_ERR` -- the IB transport's "no ACK
within `timeout * 2^retry_cnt`, giving up" terminal status. Crucially
this is **not** `IBV_WC_RNR_RETRY_EXC_ERR` (status 13): RNR means the
peer is alive but had no posted recv WQE; RETRY_EXC means the peer
**never responded at the wire level**.

QP attrs in this harness: `attr.timeout = 14` (4.096 us *
`2^14` = ~67 ms first-attempt RTO), `attr.retry_cnt = 7`, so the
total budget before declaring RETRY_EXC is up to ~17 s with
exponential backoff -- the observed ~4 s is mid-retries.

## 2. The smoking gun in the test logs

Two facts from `qp_supp_restore_after_qp_0.txt` together pin the
direction of failure:

(a) The sender printed exactly one `OK` post-restore, followed by
the status-12 line ~4 s later:

```
5874  [host1:rdma_agent_host2:stderr] [cmd] post_send qp1 m1 0 4
5877  [host1:rdma_agent_host2:stdout] OK            <-- write_mem's OK
                                                        (the stdin was
                                                        "write_mem r1
                                                        0 ping &&
                                                        post_send qp1
                                                        m1 0 4")
                                                        cmd_post_send
                                                        is now busy-
                                                        polling cq1.
5919  [host1:rdma_agent_host2:stdout]
        ERR VERBS_FAIL send completion status 12     <-- ~4s later
5920+ pipe readers exit                              <-- agent abort()
```

Only `cmd_write_mem` printed its `OK`; `cmd_post_send` never
got a SUCCESS WC -- it was still busy-polling when its WR finally
flushed back through the CQ as `RETRY_EXC_ERR`.

(b) The receiver was simultaneously stuck waiting for a "ping"
recv-WC that never arrived:

```
5882  [host2:rdma_agent_host1:stderr] [cmd] recv_poll_cq qp0 ping 4
        (NO subsequent stdout from rdma_agent_host1 -- the agent
         is busy-polling its recv CQ, ping never arrived at qp0)
```

Together: the sender's WR never made it to the peer's NIC (or made
it but the peer's NIC dropped it before generating an ACK). The IB
transport gave up after ~4s of retransmits.

The framework's `PASS` for the post-restore steps in the executor
log is **misleading**: it confirms the stdin-fifo write succeeded,
not that the agent-side command completed. The `recv_ping_after_
restore`, `send_pong_after_restore`, and `recv_pong_after_restore`
steps all "passed" by this measure even though the agents were
silently busy-polling. This is a separate, low-priority harness fix
(make `send_process_stdin` block on a matching `OK`/`ERR` line),
out of scope for this design doc.

## 3. The architectural model

### 3.1 Pre-checkpoint MAC/IP topology (from the test log)

| | physical host1 VF eth4 | physical host2 VF eth4 |
|---|---|---|
| MAC (always) | `02:00:ef:04:00:01` | `02:00:f0:04:00:01` |
| IP pre-checkpoint | 192.168.100.4 | 192.168.100.5 |
| IP post-restore (after `configure_restored_vf_*`) | 192.168.100.5 | 192.168.100.4 |

(The MACs are recoverable from `dump_ctx`'s `gid[0]` link-local
field: `fe80::0000_effffe04_0001` -> EUI-64-from-MAC
`02:00:ef:04:00:01`, and analogously for the f0 MAC.)

### 3.2 What MODIFY_QP_TO_RTR baked into each QPC pre-checkpoint

`rdma_test_agent`'s `qp_to_rts` runs the standard
RTR-then-RTS modify with `attr.ah_attr.grh.dgid` set from the peer's
RoCEv2 GID. mlx5's RoCEv2 modify-QP path does
`rdma_addr_find_l2_eth_by_grh(...)` against the **source netdev's**
neighbor table, resolves dgid -> peer-IP -> peer-MAC, and writes the
resolved MAC into the QPC's `path.rmac_47_32 / rmac_31_0` (the FW's
storage of `av.dmac`).

| QP | runs on | peer dgid encodes | source ARP resolved peer MAC to | QPC `av.dmac` baked |
|---|---|---|---|---|
| qp0 (host1's qp) | physical host1 | 192.168.100.5 | `02:00:f0:04:00:01` (host2's MAC) | `02:00:f0:04:00:01` |
| qp1 (host2's qp) | physical host2 | 192.168.100.4 | `02:00:ef:04:00:01` (host1's MAC) | `02:00:ef:04:00:01` |

These are correct for the pre-checkpoint topology -- traffic flows.

### 3.3 What LOAD_VHCA_STATE preserves vs the post-swap reality

`LOAD_VHCA_STATE` preserves the FW QPC byte-for-byte, including the
`av.dmac` field. CRIU then `RESTORE_QP`s the kernel uobject onto the
*destination* VHCA -- but the *destination* VHCA is now the OPPOSITE
physical host's VF, and the IP-swap step put the peer's IP on what
is now the local NIC's MAC.

| QP | now runs on | local NIC MAC | preserved QPC `av.dmac` | result |
|---|---|---|---|---|
| qp0 | physical **host2**'s VF | `02:00:f0:04:00:01` | `02:00:f0:04:00:01` (host2's MAC) | **dmac == src_mac** -- self-addressed at L2 |
| qp1 | physical **host1**'s VF | `02:00:ef:04:00:01` | `02:00:ef:04:00:01` (host1's MAC) | **dmac == src_mac** -- self-addressed at L2 |

The post-restore `pin_static_neighbor_*` steps install the *correct*
neighbor entries on the destination netdev (192.168.100.4 ->
`02:00:f0:04:00:01` on host1, etc.) -- but those entries are only
consulted by `MODIFY_QP_TO_RTR`-style paths that re-resolve. The QPC's
already-resolved `av.dmac` field is not refreshed by anything in the
v0 restore pipeline.

### 3.4 Why this is unique to the CRIU-on-host swap workload

In a real SR-IOV live-migration (VF migrates with the guest, GUEST
kernel is the consumer), the *VF's MAC* is preserved across the
move and the GUEST's view of MAC topology is unchanged. The QPC's
preserved `dmac` is still correct on the destination host because
the peer's MAC didn't move -- the peer's *IP* is still on the same
*MAC* the QPC remembers.

In CRIU-on-host process-swap, the VF stays at the destination
physical host (it's a different MAC than the source) and the IP is
reassigned to simulate mobility. The QPC's preserved `dmac` is the
*source* peer's MAC, which is the *destination's local* MAC after
the swap. That collision is fundamental to the workload: any test
that swaps process+IP without swapping MACs (i.e. without true VF
migration) hits this.

This means the fix lands in `mlx5_ib_restore_qp` rather than in the
FW or in `LOAD_VHCA_STATE`'s preservation contract -- the restore
path is the first place that has *both* the QPC's old dgid and the
*destination* netdev's neighbor table. FW LOAD honoring source dmac
is correct (and necessary for VM LM); restore-time refresh is the
mlx5_ib responsibility.

### 3.5 The local_ack_timeout_err counter remains the canonical signal

`/sys/class/infiniband/<dev>/ports/<port>/hw_counters/local_ack_timeout_err`
ticks once per RC ACK timeout that consumed the retry budget. With
the proposed fix, a successful migration leaves this counter
unchanged (no retries, ACK arrives on the first attempt against the
correct dmac). Without the fix, this counter ticks
`(1 + retry_cnt) == 8` times per failed `post_send` before the
WR is flushed. That gives us a clean post-fix regression check.

## 4. Why the existing §S3b mitigations don't help here

`pd_registration_wipe.md` documents the existing `mlx5_ib_dealloc_pd`
gate (the PDN-registration wipe) and the relaxed `RESTORE_UCONTEXT`
`devx_uid` check from `54a7a1386d1b`. Both target *teardown* and
*adoption* paths -- they let restored resources be cleaned up and
let restored ucontexts open against a different `devx_uid`. Neither
touches the resolved-AV stored inside QPCs.

The dmac issue is orthogonal: even with both §S3b mitigations in
place, the *datapath* fails on the first RC send because the L2
destination is wrong. We could not have hit this earlier in the
testing matrix because pre-`uobject_restore.md` work didn't restore
QPs at all -- the harness only exercised PD/CQ/MR teardown after
restore. With QP restore landed (the work this doc anchors), the
datapath becomes the next surface.

## 5. Proposed kernel mitigation

Add a post-LOAD_VHCA_STATE refresh hook to `mlx5_ib_restore_qp` that
re-resolves `av.dmac` for RoCEv2 RC/UD QPs from the destination
netdev's neighbor table, using the QPC's own dgid as the lookup key.
Out-of-tree placement: `drivers/infiniband/hw/mlx5/qp.c`, called
from `mlx5_ib_restore_qp` after the existing `LOAD_VHCA_STATE` /
`mlx5_ib_qpc_adopt_qp` block and before the uobject is published to
userspace.

### 5.1 Sketch

```c
/* Refresh av.dmac on a RoCEv2 RC/UD QP after LOAD_VHCA_STATE
 * preserved the source's resolved dmac into the FW QPC.
 *
 * Caller holds the qp->mutex and the QP is in the state restored
 * from the source -- typically RTS but may be RTR (for the recv-
 * only side of an asymmetric setup).
 *
 * Returns 0 on success or refresh-skipped (IB link layer, UD AH-
 * key, kernel-mode QP, etc.); negative errno on real failure
 * (NUD_INCOMPLETE neighbor, MODIFY_QP rejected, etc.).
 */
static int mlx5_ib_restore_qp_refresh_dmac(struct mlx5_ib_qp *qp,
                                           struct ib_pd *pd)
{
        struct mlx5_ib_dev *dev = to_mdev(qp->ibqp.device);
        struct ib_qp_attr attr = {};
        struct ib_qp_init_attr init_attr = {};
        struct net_device *netdev;
        union ib_gid dgid;
        u8 dmac[ETH_ALEN];
        int port_num, attr_mask;
        int err;

        /* Skip non-connected types. UD AHs already carry their dmac
         * out-of-band; XRC and DC are out of scope for v0. */
        if (qp->type != IB_QPT_RC && qp->type != IB_QPT_UC)
                return 0;

        /* Skip kernel-mode QPs (RDS/iSER/SRP). v0 only restores
         * userspace QPs created via uverbs. */
        if (!qp->ibqp.uobject)
                return 0;

        /* Pull current QPC into ib_qp_attr to learn dgid +
         * sgid_index + port_num. ib_query_qp wraps the ucmd_QUERY
         * path; mlx5 has its own already, but the generic call
         * gives us the resolved av cleanly. */
        err = ib_query_qp(&qp->ibqp, &attr, IB_QP_AV | IB_QP_PORT,
                          &init_attr);
        if (err)
                return err;

        port_num = attr.port_num;
        if (rdma_port_get_link_layer(&dev->ib_dev, port_num) !=
            IB_LINK_LAYER_ETHERNET)
                return 0;  /* InfiniBand: no L2 dmac to refresh */

        dgid = attr.ah_attr.grh.dgid;
        netdev = ib_device_get_netdev(&dev->ib_dev, port_num);
        if (!netdev)
                return -ENODEV;

        /* RoCEv2-IPv4 GID: ::ffff:<a.b.c.d>.  RoCEv2-IPv6 GID:
         * the GID *is* the IPv6. Both go through neigh_lookup
         * via rdma_addr_find_l2_eth_by_grh, which is the same
         * helper the existing MODIFY_QP_TO_RTR path uses.
         *
         * NB: rdma_addr_find_l2_eth_by_grh blocks on
         * NUD_INCOMPLETE up to its internal timeout. For
         * restore-mode we want a non-blocking variant -- if no
         * cached neighbor is present, return -EAGAIN and let the
         * caller decide (typically: succeed the restore, mark the
         * QP as needing-dmac-refresh, and have the first
         * post_send trigger a retry).  See ?5.3 below. */
        err = rdma_addr_find_l2_eth_by_grh_cached(&dgid, netdev,
                                                  attr.ah_attr.grh.sgid_index,
                                                  dmac);
        dev_put(netdev);
        if (err == -EAGAIN || err == -ENOENT)
                return -EAGAIN;
        if (err)
                return err;

        /* If the resolved dmac matches what's already in the QPC,
         * skip the MODIFY_QP roundtrip. This is the common case
         * for VM-LM (preserved dmac is correct). */
        if (ether_addr_equal(attr.ah_attr.roce.dmac, dmac))
                return 0;

        memcpy(attr.ah_attr.roce.dmac, dmac, ETH_ALEN);

        /* IBV_QP_AV-only modify is allowed in RTR/RTS for RC/UC.
         * This re-runs mlx5_set_path which writes path.rmac_47_32 /
         * rmac_31_0 in the QPC. */
        attr_mask = IB_QP_AV;
        err = mlx5_ib_modify_qp(&qp->ibqp, &attr, attr_mask, NULL);
        if (err)
                return err;

        mlx5_ib_dbg(dev,
                    "vfmig: refreshed av.dmac on restored qpn 0x%x: "
                    "%pM (was preserved from source)\n",
                    qp->ibqp.qp_num, dmac);
        return 0;
}
```

The call site in `mlx5_ib_restore_qp` is a one-liner inserted after
the existing FW-state-adopt block, before
`rdma_alloc_commit_uobject` (or whatever publishes the uobject):

```c
        err = mlx5_ib_restore_qp_refresh_dmac(qp, pd);
        if (err == -EAGAIN) {
                /* Neighbor not resolved yet; let userspace's
                 * first post_send trigger a retry via the
                 * existing modify-qp path, OR mark the qp for
                 * deferred refresh. v0: log + continue. */
                mlx5_ib_dbg(dev,
                            "vfmig: deferring av.dmac refresh on "
                            "qpn 0x%x: NUD incomplete, neighbor "
                            "must resolve before first post_send\n",
                            qp->ibqp.qp_num);
                err = 0;
        }
        if (err)
                goto err_destroy_qp;
```

### 5.2 Why `IBV_QP_AV` modify in RTS is the right primitive

The IB spec table for QP state transitions (Table 3-2 in IBA 1.4)
permits `QP_AV` modify in RTR->RTR and RTS->RTS for RC and UC. mlx5
honors this: `mlx5_ib_modify_qp` -> `__mlx5_ib_modify_qp` ->
`modify_raw_packet_qp` / `mlx5_modify_qp` accepts `IB_QP_AV` with
no state change, and the underlying `MODIFY_QP(opcode=2RTR2RTR or
2RTS2RTS)` FW command re-runs `mlx5_set_path` -- which is the same
helper that resolved the dmac at original `MODIFY_QP_TO_RTR` time.
By going through this primitive we re-use every consistency check
that the existing path enforces (sgid_index validity, port_num
match, RoCEv2 traffic-class preservation, etc.).

The alternative -- writing the QPC's `path.rmac_*` fields directly
via a raw FW command -- bypasses `mlx5_set_path` and would re-
implement (or skip) those checks. Strictly worse.

### 5.3 What to do when the neighbor entry isn't resolved yet

`rdma_addr_find_l2_eth_by_grh_cached` (or whichever non-blocking
variant we choose; the existing
`rdma_addr_find_l2_eth_by_grh` blocks on `addr_resolve` for up to
~5 s) returns `-EAGAIN` when the destination netdev's neighbor
entry for the dgid's IP is `NUD_INCOMPLETE` or absent.

Two policies, in increasing complexity:

**Policy A (v0): log + skip.** Restore proceeds without dmac
refresh; userspace's first `post_send` will fail with
`RETRY_EXC_ERR` exactly as today, but the failure mode now
includes a kernel `mlx5_ib_dbg` line that explicitly tells the
operator to populate the neighbor entry (the test harness already
does this via `pin_static_neighbor_*`). Simple, doesn't regress
the existing failure mode, lets the harness work today.

**Policy B (follow-on): deferred refresh on first post_send.**
Add a per-QP `vfmig_dmac_pending` bit set when refresh returns
`-EAGAIN`. In the post_send fast path, if the bit is set, attempt
a refresh once before doorbell-ring; on failure return `-EAGAIN`
to userspace. This is invasive on the post_send hot path -- I'd
defer it to a v1 unless the test harness needs it.

The test harness pins static neighbors *before* the post-restore
traffic, so for our v0 work Policy A + correctly-ordered YAML
steps is sufficient. The reordering: ensure
`pin_restored_neighbor_*` runs **before** `restore_agents_swapped`
... which it doesn't currently. See Section 6.

### 5.4 Locking, concurrency, and userspace racing

`mlx5_ib_restore_qp` runs in the CRIU-restore process's task
context, holding the uobject creation chain locks. The QP is not
yet visible to userspace at this point (RESTORE_QP returns the
handle only after the uobject is committed), so there's no
userspace `post_send` racing with the refresh. The refresh's
internal `mlx5_ib_modify_qp(IB_QP_AV)` takes the same `qp->mutex`
as a userspace modify would; that mutex isn't yet contended
because userspace can't reach the QP. Lock-ordering identical to
the existing `MODIFY_QP_TO_RTR` path.

The neighbor-table read is RCU-safe (`neigh_lookup` uses RCU
inside). The netdev pointer is taken under `ib_device_get_netdev`
(returns a refcounted reference); we drop it via `dev_put` before
returning.

## 6. Test plan / capture script for the harness

The original capture failed because diagnostics ran *after*
`cleanup_vf_*` destroyed the failing VFs. The harness needs a
manual pause window between `discover_restored_vf_*` and
`post_recv_*_after_restore`, during which a one-shot snapshot
script collects everything we need on both physical hosts.

### 6.1 YAML pause step

Insert immediately after `verify_restored_ctx_host*_process` and
before `post_recv_host1_after_restore`:

```yaml
  - name: "DEBUG_pause_before_post_restore_traffic_host1"
    step: "start_process"
    host: "host1"
    process_name: "debug_pause_host1"
    command: "sleep 600"
    wait_time: 1
    allow_exit: true

  - name: "DEBUG_pause_before_post_restore_traffic_host2"
    step: "start_process"
    host: "host2"
    process_name: "debug_pause_host2"
    command: "sleep 600"
    wait_time: 1
    allow_exit: true
```

For users running with the framework's `--pause-at-step` flag,
pause at `recv_ping_after_restore` instead (this captures the
state with one post_send already in flight; the dmac mismatch is
already locked into the SQ at this point and the diagnostic delta
is more interesting than pausing pre-traffic).

### 6.2 Per-host snapshot script

`tools/testing/mlx5_vfmig/qp_av_dmac_capture.sh`:

```bash
#!/bin/bash
# qp_av_dmac_capture.sh -- one-shot diagnostic snapshot for the
# §S6b stale-dmac investigation. Run on each physical host during
# the YAML's DEBUG_pause window.
#
# Usage:  sudo qp_av_dmac_capture.sh <ifname> <ibdev> <bdf>
# Example: sudo qp_av_dmac_capture.sh eth4 mlx5_2 0000:08:00.2
set -eu
IF=$1
IBDEV=$2
BDF=$3
TS=$(date +%s)
OUT=/tmp/qp_av_dmac_capture_${HOSTNAME}_${TS}
mkdir -p "$OUT"
exec > >(tee -a "$OUT/log") 2>&1

echo "=== topology ==="
ip -d link show dev "$IF"
ip addr show dev "$IF"
ip neigh show dev "$IF"

echo "=== rdma resource snapshot (pre-traffic) ==="
rdma resource show qp link "$IBDEV" -dd > "$OUT/rdma_qp_pre"
cat "$OUT/rdma_qp_pre"

echo "=== mlx5 debugfs QPs (pre-traffic) ==="
for qp in /sys/kernel/debug/mlx5/$BDF/QPs/*; do
    [ -d "$qp" ] || continue
    qpn=$(basename "$qp")
    echo "--- QP $qpn ---"
    for f in state remote_qpn mtu transport num_send num_recv pid; do
        printf "%-14s %s\n" "$f:" "$(cat "$qp/$f" 2>/dev/null)"
    done
done > "$OUT/mlx5_qps_pre"
cat "$OUT/mlx5_qps_pre"

echo "=== ibv counters (pre-traffic baseline) ==="
for c in port_xmit_packets port_rcv_packets; do
    echo "$c=$(cat /sys/class/infiniband/$IBDEV/ports/1/counters/$c)"
done | tee "$OUT/counters_pre"
for c in local_ack_timeout_err out_of_sequence rx_read_requests \
         rx_write_requests; do
    p=/sys/class/infiniband/$IBDEV/ports/1/hw_counters/$c
    [ -f "$p" ] && echo "$c=$(cat $p)" || echo "$c=<absent>"
done | tee -a "$OUT/counters_pre"

echo "=== ethtool PHY counters (pre-traffic baseline) ==="
ethtool -S "$IF" | grep -E \
    'tx_packets_phy|rx_packets_phy|tx_dropped|rx_discards_phy' \
    > "$OUT/ethtool_pre"
cat "$OUT/ethtool_pre"

echo "=== starting tcpdump (background, kill via $OUT/tcpdump.pid) ==="
tcpdump -e -n -i "$IF" -w "$OUT/post_restore.pcap" \
        udp port 4791 &
echo $! > "$OUT/tcpdump.pid"

echo "=== capture dir: $OUT ==="
echo "After the post_send fires, run:"
echo "  kill \$(cat $OUT/tcpdump.pid)"
echo "  $0 --post $IF $IBDEV $BDF $OUT"
```

(Add a corresponding `--post` mode that re-runs the `rdma_qp_pre /
mlx5_qps_pre / counters_pre / ethtool_pre` collection writing to
`*_post` files, then prints diffs. Omitted here for brevity.)

### 6.3 What "the diagnosis is locked" looks like

After running the harness with the pause step and the capture
script on both hosts, the dmac hypothesis is confirmed iff:

1. `rdma_qp_pre` shows both restored QPs in state `RTS` with
   correct `dst-qpn` and matching `src-/dst-psn`.
2. `mlx5_qps_pre` shows `state: 3` (RTS), correct `remote_qpn`,
   nonzero `num_send` and `num_recv`.
3. `ip neigh show dev eth4` lists the peer's IP as `PERMANENT`
   pointing to the *peer's* MAC (the static neighbor pin worked).
4. After the post_send phase: `port_xmit_packets` ticked by `>=1`,
   AND `tx_packets_phy` ticked by `>=1` (frame DID hit the wire),
   AND the captured pcap shows frames with `src_mac == dst_mac`
   and `dst_mac == <local NIC MAC>`, AND
   `local_ack_timeout_err` ticked by `>=1` per failed WR.

If 4 fails on `tx_packets_phy` (no PHY tx) but `port_xmit_packets`
ticks, the WR reached FW but FW didn't egress -- a DIFFERENT bug
(likely SQ-state corruption from the restore path). That outcome
invalidates this design doc and points at `mlx5_ib_restore_qp`'s
SQ-state handling instead.

If 4 fails on `port_xmit_packets` (no IB tx), the SQ doorbell
isn't reaching FW -- yet another different bug (UAR mapping or
SQ-state breakage), again invalidating this doc.

This is why we capture before acting on the proposed fix.

## 7. Alternatives considered

### 7.1 Application cooperation: userspace `ibv_modify_qp(IBV_QP_AV)` post-restore

The application (or a CRIU restore-time helper) re-modifies each
RC QP with `IBV_QP_AV` after restore, refreshing the dmac via the
existing kernel modify path. Trivial to implement: ~20 lines in
`rdma_test_agent.c`, and any real-world RDMA application doing
its own connection re-establishment after migration would do this
naturally.

**Rejected as the canonical fix** because it's not transparent --
every application has to participate. It's still useful as a
short-term diagnostic workaround (proves the dmac theory by
fixing the symptom), and it's the right escape hatch for
applications that already have their own post-migration handshake
(e.g. NCCL's bootstrap reconnect). But the design intent for
mlx5_sriov_vfmig is "drop in CRIU + plugin, no app changes" --
the kernel must handle this.

We will still want to add a `qp_modify_av` command to
`rdma_test_agent` for diagnostic use; that's tracked separately
in the harness changelog.

### 7.2 CRIU-side post-restore step

CRIU plugin issues an `ibv_modify_qp(IBV_QP_AV)` immediately after
`RESTORE_QP` returns, with the AV reconstructed from the QPC
query.

**Rejected**: CRIU shouldn't know peer-network details. The
plugin doesn't have a stable way to extract dgid + sgid_index
from the QPC except by calling QUERY_QP and re-issuing
modify-QP. That's exactly what the kernel-side fix does, but
done in CRIU it duplicates state and pushes neighbor-resolution
policy into userspace.

### 7.3 Modify the LOAD_VHCA_STATE preservation contract

Have FW skip preserving `path.rmac_*` and require the destination
to re-resolve at LOAD time using a destination-supplied netdev
pointer.

**Rejected**: this would regress real VM live-migration (where
the preserved dmac is the right answer). The CRIU-on-host swap
is the unusual workload, not LOAD_VHCA_STATE's contract.

### 7.4 Patch source-side dmac before SAVE_VHCA_STATE

Have CRIU dump-side issue an `ibv_modify_qp(IBV_QP_AV)` with a
synthetic "broadcast" or "loopback" dmac before SAVE_VHCA_STATE,
so the saved QPC has a known sentinel dmac that the destination
recognizes and refreshes.

**Rejected**: any synthetic dmac corrupts the running QP between
the dump-time modify and the actual checkpoint -- the source
loses connectivity briefly, which would race with any in-flight
WRs and risks RETRY_EXC on the *source* before checkpoint
completes. Dump-side state mutation also violates the
non-interference principle for restore-mode-only changes.

## 8. Kernel asks (handoff)

| # | ask | size | priority |
|---|---|---|---|
| KS6b.1 | Implement `mlx5_ib_restore_qp_refresh_dmac` per ?5.1 with Policy A (?5.3). Call from `mlx5_ib_restore_qp` after the existing `LOAD_VHCA_STATE` adopt block. Skip non-RC/UC, non-RoCEv2, non-userspace QPs. ~80 lines including helper. | small | high (gates v0 RC datapath end-to-end) |
| KS6b.2 | Verify `mlx5_ib_modify_qp(qp, &attr, IB_QP_AV)` against an RTS RC QP works as expected (re-runs `mlx5_set_path`, writes `path.rmac_47_32 / rmac_31_0` only, leaves PSN / dest_qpn untouched). Quick FW trace or empirical probe; this is the assumption the whole fix rests on. | trivial | high |
| KS6b.3 | Decide on the `rdma_addr_find_l2_eth_by_grh_cached` non-blocking variant: either add it (small core/addr.c addition) or accept the existing blocking helper's worst-case ~5s pause inside RESTORE_QP. Blocking is fine for the harness; a non-blocking variant matters once we wire up Policy B (?5.3). | trivial | medium |
| KS6b.4 | Add the `local_ack_timeout_err` regression check to the `qp_restore_probe` harness: assert delta == 0 across one successful migrated-QP roundtrip after the fix lands. | small | medium |
| KS6b.5 | (Optional) Policy B deferred-refresh on first post_send (?5.3). v1 follow-up. | medium | low |

KS6b.1 + KS6b.2 unblock the v0 RC datapath end-to-end. KS6b.3 is a
core-side addition that can land either before or after KS6b.1
depending on the policy choice.

## 9. Cross-references

- `uobject_restore.md` ?S6b -- per-uobject QP restore (the work
  that exposed this datapath surface).
- `pd_registration_wipe.md` -- prior §S3b mitigation pattern; this
  doc follows the same shape (TL;DR -> evidence -> architectural
  model -> mitigation -> alternatives).
- `uar_restore.md` -- baseline for the LOAD_VHCA_STATE
  preservation model and the "what FW preserves vs what mlx5_ib
  needs to refresh" decomposition.

## Changelog

- 2026-06-04: initial draft, filed by the CRIU agent after the
  first end-to-end run with QP restore. Diagnosis tentative;
  awaiting mid-failure capture per ?6.
