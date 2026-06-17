# DESIGN: §S6b -- stale `av.dmac` in migrated QPC after CRIU process-swap (RESOLVED)

> ## STATUS (2026-06-08): RESOLVED via orchestrator-side VF identity migration
>
> The §S6b stale-dmac problem -- post-restore RC traffic hangs
> and the first `post_send` surfaces `IBV_WC_RETRY_EXC_ERR`
> (status 12) -- was a **missing orchestrator-side
> reconfiguration step on the destination, not a kernel bug**.
> A complete CRIU process-swap requires the same per-VF
> identity migration that SR-IOV VM live-migration performs
> for free: each destination VF needs to take on the
> *source-time peer-VF MAC and IP*, and pinned ARP needs to
> map the source-time peer IP to the source-time peer-VF MAC
> (which the *other* destination host has been similarly
> reconfigured to expose). With those steps in place, the
> source-baked `path.rmac_*` in each restored QPC is correct
> at t=0 and `LOAD_VHCA_STATE → RESTORE_QP` works on the
> unmodified blob. **No kernel patches required.**
>
> Empirically confirmed 2026-06-08 on
> `rdma_test_agent_vfmig_criu_swap_after_qp.yaml`: adding
> `ip link set <PF> vf <VF_ID> mac <source-time-peer-vf-mac>`
> to the destination-side prerestore steps, and updating the
> existing `pin_static_neighbor_*` step to use source-time
> peer-VF MAC values (instead of the destination host's local
> NIC MAC), makes the end-to-end test pass on the unmodified
> source-loaded QPC. `check_qp_av_dmac.sh` returns
> `VERDICT=DMAC_IS_PEER` on both hosts; `ping_pong_after_restore`
> returns OK MATCH.
>
> ### What stays in tree
> * `tools/mlx5_vfmig query_qp` ioctl + CLI verb (commits
>   `c659ab66483d`, `e4b0b417a97d`, `a7a17b9bb670`) -- the
>   diagnostic surface that backs `check_qp_av_dmac.sh`. Useful
>   as forward post-restore verification ("did the orchestrator
>   actually mirror the VF identity correctly?").
> * `check_qp_av_dmac.sh` -- harness wrapper that drives the
>   diagnostic ioctl and emits `DMAC_IS_LOCAL` /
>   `DMAC_IS_PEER` / `DMAC_AMBIGUOUS`. Pass criterion for the
>   v0 swap workload is `DMAC_IS_PEER` on both hosts.
>
> ### What was reverted
> * `mlx5_ib_restore_qp_refresh_av_dmac()` always-on helper
>   (`5166e228c223`, Patch B `66edef3f790f`) -- reverted in
>   `0a05d29edb2f`.
> * `MLX5_VFMIG_IOC_REFRESH_AV_DMAC` ioctl + `vfmig_*` helpers
>   (`b70b6624084a`, Patch B `fe188a601af9`) -- reverted in
>   `6055711aae44`.
> * `mlx5_vfmig refresh_av_dmac` CLI verb +
>   `refresh_av_dmac.sh` harness wrapper (`1bbe576bc7c5`) --
>   reverted in `5786cb303142`.
>
> All three drove the same `MODIFY_QP(RTS2RTS_QP,
> opt_param_mask=PRIMARY_ADDR_PATH)` sequence, which CX-7 28.x
> firmware rejects with syndrome `0x00498c8b`. The investigation
> chain that established this -- including the audit of mlx5_ib's
> own `opt_mask` allowlist, which excludes `PRIMARY_ADDR_PATH`
> from every state-pair cell -- is preserved in
> [Appendix A](#appendix-a-investigation-history-preserved).
> With the orchestrator-side fix in place, none of that matters
> at runtime: the QPC is correct from t=0 and no post-LOAD
> modification is ever attempted.

## 0. What was actually wrong

The harness was reassigning per-VF *IPs* between hosts on each
swap but leaving per-VF *MACs* untouched. Walking the restored
QPC's `primary_address_path` field by field on the v0 swap
workload (single-port RoCE v2 IPv4 RC):

| Field | Stored in QPC verbatim? | What it represents | Stale post-swap on v0 harness? | Mitigated by |
|---|---|---|---|---|
| `rmac_47_32` / `rmac_31_0` | **Yes** | Peer's MAC at MODIFY_QP_TO_RTR time | **Yes** -- peer moved to other host; its source-time MAC is now the *destination host's local NIC MAC* | VF-MAC swap (the missing step) |
| `rgid_rip` | Yes | Peer's GID (IPv4-mapped IPv6 for RoCEv2 IPv4) | No -- harness IP swap puts peer's source-time IP on the destination's VF | Existing IP-swap step |
| `src_addr_index` | Yes (an *index*) | Index into local GID table | No -- the index is stable; the GID-table contents at that index match because the destination's VF has the source-time IP | Existing IP-swap step (kernel auto-populates GIDs from netdev IPs) |
| `vhca_port_num` | Yes | Local physical port | No -- single-port NIC; same on both hosts | n/a |
| `tclass`, `flow_label`, `udp_sport`, `dscp`, `eth_prio`, `sl`, `hop_limit`, `mtu` | Yes | RoCE/IP knobs | No -- host-agnostic | n/a |
| **`smac` (local source MAC)** | **No** -- not in QPC | Local VF's MAC | n/a -- FW resolves on every send via `src_addr_index → GID → netdev → netdev MAC` | n/a (the local NIC's *current* MAC always wins, automatically) |
| `dest_qpn`, `rq_psn`, `sq_psn`, timeouts | Yes | Connection state | No -- both peers' QPCs are symmetrically restored, so all numbering matches | n/a |

So **`path.rmac_*` was the only field whose value disagreed
between the source's saved QPC and the destination's post-swap
network reality**. Every other identity-bearing field either
moves with the IP-swap step the harness already did, or is a
purely host-agnostic value, or is resolved at runtime from
state we control.

Why VM-LM doesn't hit this: in SR-IOV VM live-migration, the
hypervisor sets the destination VF's admin MAC to the VM's
vNIC MAC as part of the migration setup -- vNIC MAC is portable
identity that travels with the VM. The peer is unchanged (the
peer didn't move), so `path.rmac` is correct on the destination
as soon as `LOAD_VHCA_STATE` installs the QPC. Zero patching,
zero kernel commands, zero MODIFY_QP attempts.

For symmetric CRIU process-swap (where *both* peers move to
opposite hosts simultaneously), both VFs need to take on their
respective source-time peer-VF MACs. That's what was missing.

## 1. The resolution: orchestrator-side VF identity migration

On each destination host, between the SR-IOV VF cycle and
`LOAD_VHCA_STATE`, the orchestrator mirrors the source's
per-VF identity:

```bash
# (1) Local VF takes on the source-time peer-VF MAC, since the
# peer is now running on this host post-swap.  This was the
# missing step.
ip link set <PF> vf <VF_ID> mac <source-time-peer-vf-mac>

# (2) IPs are reassigned to mirror source-time peer-VF IPs.
# (Existing harness step; unchanged.)
ip addr add <source-time-peer-vf-ip>/<prefix> dev <vf_netdev>

# (3) Static ARP for the actual peer (now on the OTHER host,
# which has been similarly reconfigured) maps to that host's
# source-time-peer-VF MAC -- not to the local NIC MAC, which
# was the v0 harness's bug.
ip neigh replace <source-time-local-ip> \
                 lladdr <source-time-local-vf-mac> \
                 dev <vf_netdev> nud permanent
```

The four MAC values are deterministic from the harness's own
MAC scheme; the symmetric flip is exactly the mirror image of
the source-time configuration. (See
[Appendix A §3.1](#31-pre-checkpoint-macip-topology-from-the-test-log)
for the source-time topology.)

This works because:

* `path.rmac_*` (peer MAC, baked into the QPC at source-time
  RTR) matches the actual peer's VF MAC -- which the *other*
  destination host has been told to take on via the same
  `ip link set vf mac` step. Both hosts run step (1) with
  source-time *peer*-VF MACs as inputs, which is the
  symmetric flip that the source-time topology already had.
* `path.src_addr_index` resolves to a GID-table slot whose
  contents (source-time local IP) match what the source
  put there. This was already working pre-fix because the
  harness's `ip addr add` already drove the auto-population.
  (See `Appendix A §10.1 / §10.2` for why we sweat over
  this; for the v0 single-IP single-port case there's no
  slot-allocation drift.)
* `smac` is *not* in the QPC; the FW resolves it on every
  send via `src_addr_index → GID → netdev → netdev MAC`. So
  the local NIC's *current* (post-`ip link set vf mac`) MAC
  always wins, automatically.

Step (1) must run *before* step (2). GID-table entries bind
`(IP, MAC)` at IP-add time, and changing the netdev's MAC
afterwards leaves entries with stale `(IP, old-MAC)` bindings.
"MAC first, then IP" is the safe ordering.

## 2. Validation

Post-restore confirmation lives in
`tools/testing/mlx5_vfmig/uobject_restore/qp_av_dmac/check_qp_av_dmac.sh`,
which uses the diagnostic `MLX5_VFMIG_IOC_QUERY_QP` ioctl to
dump `path.rmac_*` from the FW QPC and compare it to the
destination's `ip neigh` view of the actual peer MAC:

* **Pre-fix** harness: `VERDICT=DMAC_IS_LOCAL` on both hosts
  -- the QPC's stored peer MAC matched the *destination
  host's local NIC MAC*, confirming the swap left the
  source-baked dmac stale.
* **Post-fix** harness (with `ip link set vf mac` added):
  `VERDICT=DMAC_IS_PEER` on both hosts; first
  `ping_pong_after_restore` returns OK MATCH on both
  directions; `local_ack_timeout_err` does not tick.

The diagnostic surface stays in tree as the canonical
post-restore confirmation that the orchestrator did its job:

* `tools/mlx5_vfmig query_qp` ioctl + CLI verb (`c659ab66483d`,
  `e4b0b417a97d`, `a7a17b9bb670`).
* `check_qp_av_dmac.sh` wrapper (in `a7a17b9bb670`).

Future workloads adding new restore-time identity drift
(VLAN, MTU, etc.) should follow the same pattern: extend the
orchestrator's per-VF mirror step, *not* the kernel's
post-LOAD reconciliation surface (there isn't one;
[Appendix A §12](#12-post-mortem-post-rtr-primary-av-refresh-is-not-supported-on-mlx5--cx-7-28x)
explains why).

## Appendix A: investigation history (preserved)

The remainder of this document is the original investigation
chain that led to the resolution above. It is **preserved
verbatim** for the record -- so future readers can see the
full diagnosis lock, the alternatives considered, the reverted
as-tried kernel mitigations, and the FW post-mortem that
established why post-LOAD primary-AV refresh isn't a viable
mechanism on this driver/FW combination (syndrome
`0x00498c8b`; mlx5_ib's `opt_mask` excludes `PRIMARY_ADDR_PATH`
from every state-pair cell).

None of the kernel asks named in this appendix are still open;
all of them have been reverted or superseded by the
orchestrator-side fix above. Section numbers and intra-doc
references are kept as-is for git-blame and inbound-link
friendliness.

The appendix entry points worth pulling forward:

* **§3.1** -- the pre-checkpoint MAC/IP topology that the
  resolution at §1 mirrors. Useful when working out which
  source-time MAC each destination host should take on.
* **§3.4** -- the architectural reason this is unique to
  CRIU-on-host swap: VFs *don't* move with the process the
  way vNICs move with a VM, so per-VF identity has to be
  reassigned post-hoc by the orchestrator.
* **§6.0** -- the cheap diagnostic (`check_qp_av_dmac.sh`)
  that landed and stays in tree.
* **§12** -- the FW-rejection post-mortem; documents the
  `opt_mask` audit and the verbs `qp_state_table` agreement.
  Worth preserving so future work doesn't try to revive
  the post-LOAD `MODIFY_QP(RTS2RTS_QP, PRIMARY_ADDR_PATH)`
  approach without first reading why it's not viable.

The TL;DR section that follows was the original framing
("the architectural fix this doc proposes" -- a kernel-side
helper). That framing was wrong; read it as a record of how
the team got from initial diagnosis to the shipped resolution.

## A.TL;DR (original; superseded by §1)

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

The architectural fix originally proposed (superseded by the
orchestrator-side fix at §1):

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

This was rejected in §12 (post-LOAD primary-AV refresh isn't a
supported operation on mlx5 + CX-7 28.x).

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

> **Update (post-initial-draft):** the cheaper diagnostic shortcut
> -- "just dump `path.rmac_*` from the FW QPC and compare to
> `ip neigh`" -- is now wired up and ready to run. See
> [§6.0 Cheapest diagnostic: cdev `query_qp` + `check_qp_av_dmac.sh`](#60-cheapest-diagnostic-cdev-query_qp--check_qp_av_dmacsh)
> below. Run that on each physical host post-restore (and before
> any post-restore traffic) to lock or refute the dmac theory in
> seconds, *without* needing the full §6 tcpdump/pause-step
> capture flow.
>
> The Section-2 RETRY_EXC CQE itself already eliminates one alt
> hypothesis: if a CQE was generated, FW DID receive the WR. So
> the bug is downstream of FW receiving the SQ doorbell. The
> cdev `query_qp` lookup distinguishes the two remaining
> alternatives (stale dmac in QPC vs FW egressed correctly but
> peer didn't process) in one ioctl roundtrip.

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

> **Erratum -- this section's claim is wrong as written, and the
> correction below changes the §5.1 sketch.** The IB spec table
> for QP state transitions in `drivers/infiniband/core/verbs.c`
> (`qp_state_table[IB_QPS_RTS][IB_QPS_RTS]`) permits `IB_QP_ALT_PATH`
> (alternate path) but not primary `IB_QP_AV` for any QP type
> including RC/UC. mlx5 enforces this gate directly via
> `ib_modify_qp_is_ok` in `__mlx5_ib_modify_qp`, before any FW
> command issues. So `mlx5_ib_modify_qp(qp, &attr, IB_QP_AV)`
> against an RTS QP returns `-EINVAL` from the verbs layer
> without reaching `mlx5_set_path`. The same gate applies to
> RTR->RTR.
>
> The FW itself **does** support primary-AV update at RTS: the
> `RTS2RTS_QP` opcode accepts an `opt_param_mask` with
> `MLX5_QP_OPTPAR_PRIMARY_ADDR_PATH` set, and mlx5 already wires
> up the IB_QP_AV -> PRIMARY_ADDR_PATH optpar mapping
> (`drivers/infiniband/hw/mlx5/qp.c`, the `case IB_QP_AV` in
> `set_qp_state_optpar`'s caller table). Only the spec-strict
> verbs validation gate is in the way.
>
> The corrected fix uses a **direct `mlx5_cmd_exec(MODIFY_QP)`**
> issued from inside `mlx5_ib_restore_qp`, bypassing the verbs
> validation. This matches the gated-restore pattern we already
> have for the PD-wipe workaround
> (`pd_registration_wipe.md`'s `mlx5_ib_dealloc_pd` gate): the
> primitive is mlx5-private, gated on the restore-only path,
> with all consistency checks done locally (we still call
> `mlx5_set_path` to fill in the synthesized qpc blob from a
> rebuilt `rdma_ah_attr`, so sgid_index validity / port_num /
> RoCEv2 traffic-class preservation are all re-validated --
> we just don't go through the IB-spec verbs gate).
>
> The original §5.2 argument "writing path.rmac_* fields
> directly via a raw FW command bypasses mlx5_set_path -- strictly
> worse" was based on the false premise that the verbs path was
> open. With the verbs path closed, the FW-direct route is the
> *only* path; we still call `mlx5_set_path` into the synthesized
> qpc, so the consistency checks are preserved.

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

### 6.0 Cheapest diagnostic: cdev `query_qp` + `check_qp_av_dmac.sh`

Before any of the heavyweight §6.1-§6.3 captures (YAML pause,
tcpdump, ethtool, hw_counters), the dmac theory can be locked
or refuted in **one ioctl roundtrip per host** using the cdev's
existing `MLX5_VFMIG_IOC_QUERY_QP`. The kernel side already pulls
the entire 64-byte `primary_address_path` out of the FW QPC; the
userspace tool now decodes the AV subfields locally:

```text
$ sudo mlx5_vfmig <pf-bdf> query_qp <vf_id> <qpn>
...
qpc_primary_address_path=00000000c80fffff...   # 64-byte hex blob
av_dmac=02:00:f0:04:00:01                       # path.rmac_47_32 || rmac_31_0
av_dgid=fe80:0000:0000:0000:0000:00ff:fe04:0001 # rgid_rip[16]
av_dgid_ipv4=192.168.100.5                      # if RoCEv2-IPv4
av_sgid_index=3
av_hop_limit=64
av_vhca_port_num=1
av_grh=1
```

The decode is done in userspace from the existing blob (no kernel
rebuild needed). A runtime self-check cross-decodes pkey_index from
both the IFC accessor and the raw blob; mismatch aborts the run
loud rather than silently emitting a wrong dmac.

The harness `tools/testing/mlx5_vfmig/uobject_restore/qp_av_dmac/check_qp_av_dmac.sh`
wraps the call. It auto-derives IBDEV / IFACE from PF + vf_id,
queries the QPC, reads the local NIC MAC from sysfs, looks up
`ip neigh show dev <iface> <av_dgid_ipv4>` for the peer's expected
MAC, and emits a verdict:

| Verdict | Meaning |
|---|---|
| `DMAC_IS_LOCAL` | `av.dmac == local_NIC_mac`. Frames are self-addressed at L2 → fabric drops → no ACK → RETRY_EXC. **Stale-dmac theory CONFIRMED.** Kernel-side restore-time refresh fix is justified. |
| `DMAC_IS_PEER`  | `av.dmac == peer_mac` from `ip neigh`. L2 destination is correct. **Stale-dmac theory REFUTED.** Look elsewhere (peer RQ state, FW egress, SQ-buf umem mapping). |
| `DMAC_AMBIGUOUS` | Neither match. Likely `NUD_INCOMPLETE` neighbor, IPv6 dgid (no v4 decode path), or third bug. Inspect manually. |

Operator runbook (run on each physical host, post-restore,
before any post-restore traffic):

```text
# Discover the restored QPN (or read it from the test framework's
# pre-checkpoint state dump):
$ rdma resource show qp link mlx5_2 -dd | grep "$peer_qpn_hint"

# Run the diagnostic:
$ sudo PF=0000:08:00.0 ./check_qp_av_dmac.sh <vf_id> <qpn>
```

This bypasses the §6.1-§6.3 capture flow entirely for the
specific question "is `av.dmac` stale?". The full capture is still
useful if §6.0 returns `DMAC_IS_PEER` (we then need pcap/PHY
counters to localize the alternative bug), but most-likely-case
the §6.0 verdict is enough to direct the next action.

> **Caveat that comes out of the kernel walk-through:** if §6.0
> returns `DMAC_IS_LOCAL`, the proposed §5.1 fix shape needs one
> correction. `mlx5_ib_modify_qp(qp, &attr, IB_QP_AV)` against
> an RTS QP is **rejected by the verbs layer** before reaching
> `mlx5_set_path` -- `ib_modify_qp_is_ok`'s `qp_state_table[RTS][RTS]`
> permits `IB_QP_ALT_PATH` (alternate path) but not primary
> `IB_QP_AV`, for any QP type. The fix needs a direct
> `mlx5_cmd_exec(MODIFY_QP, opcode=RTS2RTS,
> opt_param_mask=PRIMARY_ADDR_PATH)` issued from inside
> `mlx5_ib_restore_qp`, paralleling the gated-restore pattern we
> already use for the PD-wipe workaround. The FW *does* support
> primary-AV update at RTS via the `RTS2RTS_QP` opcode +
> `MLX5_QP_OPTPAR_PRIMARY_ADDR_PATH` optpar bit (mlx5 already
> wires up that optpar mapping at `qp.c:set_qp_state_optpar` for
> `IB_QP_AV`); only the spec-strict verbs gate stops it.

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

| # | ask | size | priority | status |
|---|---|---|---|---|
| KS6b.0 | **(prerequisite)** Run `check_qp_av_dmac.sh` on each physical host post-restore (see §6.0). Verdict locks or refutes the dmac theory in seconds. KS6b.1+ only proceed if verdict is `DMAC_IS_LOCAL`. | trivial | **highest** | **DONE** -- verdict was `DMAC_IS_LOCAL` on both hosts. |
| KS6b.1 | Implement `mlx5_ib_restore_qp_refresh_av_dmac` per §5.1 + §5.2 erratum with Policy A (§5.3): direct `mlx5_core_qp_modify(MLX5_CMD_OP_RTS2RTS_QP, MLX5_QP_OPTPAR_PRIMARY_ADDR_PATH, ...)` from inside `mlx5_ib_restore_qp`, NOT `mlx5_ib_modify_qp(IB_QP_AV)`. The verbs path is closed for primary-AV at RTS (see §5.2 erratum). Skip non-RC/UC, non-RoCEv2, non-userspace QPs. | small | high | **DONE** -- ~115 LOC across `qp.c` + `qp.h` + `main.c`. As-built shape in §10. |
| KS6b.2 | Verify FW accepts `MODIFY_QP(RTS2RTS_QP, opt_param_mask=PRIMARY_ADDR_PATH)` against a real RTS RC QP, with only `path.rmac_47_32 / rmac_31_0` changed and PSN / dest_qpn / sgid_index untouched. | small | high | pending end-to-end run (post-fix `mlx5_vfmig query_qp` + harness rerun -- verdict should flip to `DMAC_IS_PEER`, with `qpc_last_acked_psn` advancing past the stuck WR). |
| KS6b.3 | Decide on the `rdma_addr_find_l2_eth_by_grh_cached` non-blocking variant. | trivial | medium | **N/A** -- as-built helper uses `neigh_lookup` directly against the port's netdev (synchronous, cache-only, no ARP/NS solicit). See §10. v1's Policy B may revisit. |
| KS6b.4 | Add the `local_ack_timeout_err` regression check to the `qp_restore_probe` harness: assert delta == 0 across one successful migrated-QP roundtrip after the fix lands. | small | medium | pending. |
| KS6b.5 | (Optional) Policy B deferred-refresh on first post_send (§5.3). v1 follow-up. | medium | low | **N/A for the v0 harness** -- `mlx5_ib_post_send` is the kverbs `ib_device_ops.post_send` callback and is NOT reached for uverbs-created QPs (the data path goes directly to userspace SQ + UAR doorbell). For our use case Policy B would never fire. Replaced by KS6b.6 below. |
| KS6b.6 | Dev-branch backup: userspace-triggered `MLX5_VFMIG_IOC_REFRESH_AV_DMAC` ioctl driven from the harness AFTER `pin_static_neighbor_*` (see §11). The always-on path inside RESTORE_QP fires too early for the v0 harness ordering and hits Policy A; this ioctl is the manual escape hatch. | small | high | **DONE** -- handler in `drivers/net/ethernet/mellanox/mlx5/core/vfmig/vfmig.c`, CLI verb `mlx5_vfmig refresh_av_dmac <vf_id> <qpn>`, wrapper script `tools/testing/mlx5_vfmig/uobject_restore/qp_av_dmac/refresh_av_dmac.sh`. Pending end-to-end run. |

KS6b.0 + KS6b.1 unblocked the v0 RC datapath end-to-end (subject to
KS6b.2 confirmation). KS6b.4 is the post-fix regression guard.
KS6b.6 backstops KS6b.1 for harness orderings that pin neighbors
*after* RESTORE_QP returns -- which is the actual shape of the v0
mlx5_sriov_vfmig criu plugin. See §11.

## 9. Cross-references

- `uobject_restore.md` ?S6b -- per-uobject QP restore (the work
  that exposed this datapath surface).
- `pd_registration_wipe.md` -- prior §S3b mitigation pattern; this
  doc follows the same shape (TL;DR -> evidence -> architectural
  model -> mitigation -> alternatives).
- `uar_restore.md` -- baseline for the LOAD_VHCA_STATE
  preservation model and the "what FW preserves vs what mlx5_ib
  needs to refresh" decomposition.

## 10. Implementation summary (as-built)

The fix landed as **~115 LOC across three files**:

| file | what changed | LOC |
|---|---|---|
| `drivers/infiniband/hw/mlx5/qp.c` | new `mlx5_ib_lookup_l2_dmac()` static helper + new `mlx5_ib_restore_qp_refresh_av_dmac()` exported helper; 4 net header includes (`net/arp.h`, `net/ipv6.h`, `net/ipv6_stubs.h`, `net/neighbour.h`) | ~95 (incl. comments) |
| `drivers/infiniband/hw/mlx5/qp.h` | prototype + kerneldoc for the new helper. **No `struct mlx5_ib_qp` exposure** -- the helper takes `struct mlx5_ib_dev *` + `struct mlx5_core_qp *`, mirroring the rest of qp.h's FW-command-wrapper pattern (`mlx5_qpc_create_qp`, `mlx5_qpc_adopt_qp`, `mlx5_core_qp_modify`, `mlx5_core_qp_query`, etc.). | ~20 |
| `drivers/infiniband/hw/mlx5/main.c` | call site in `mlx5_ib_restore_qp` after `mlx5_ib_register_user_qp_in_dev_lists`, gated on `qp->ibqp.uobject && (qp->type == IB_QPT_RC \|\| qp->type == IB_QPT_UC)`. Policy-A non-fatal error treatment. | ~25 |

The implementation differs from the §5.1 sketch in three deliberate places:

### 10.1 `neigh_lookup` instead of `rdma_addr_find_l2_eth_by_grh`

§5.1 / §5.3 pinned hopes on a `rdma_addr_find_l2_eth_by_grh_cached`
non-blocking variant of ib_core's address resolver. **As built we
sidestep the ib_core resolver entirely** in favor of a direct
`neigh_lookup(&arp_tbl, ...)` (RoCEv2-IPv4) or
`neigh_lookup(ipv6_stub->nd_tbl, ...)` (RoCEv2-IPv6) against the
port's netdev (`ib_device_get_netdev(ib_dev, port_num)`).

Why:

- `rdma_addr_find_l2_eth_by_grh` is **declared only in
  `drivers/infiniband/core/core_priv.h`**, not in any public header
  (`<rdma/ib_addr.h>` exposes `rdma_resolve_ip` and the gid<->ip
  inlines, but not the higher-level resolver). It IS `EXPORT_SYMBOL`'d,
  but mlx5_ib (in-tree) shouldn't reach into core_priv.h. No other
  in-tree driver does.
- The resolver also drives `rdma_resolve_ip` -> `addr_wq` ->
  potential ARP/NS solicit with a 1s `wait_for_completion`. For our
  v0 flow (operator pre-pins `NUD_PERMANENT` entries via
  `pin_static_neighbor_*`) we explicitly want a **non-blocking
  cache-only probe**: if the entry is missing, the right answer is
  Policy A "log + skip", not "wait 1s for an ARP that's never going
  to come because the peer is on the other physical host."
- `neigh_lookup` returns `NULL` if the entry is missing (we map to
  `-ENOENT`) and a non-`NUD_VALID` entry is mapped to `-EAGAIN`.
  Both are folded into Policy A by the caller.

The trade-off: no automatic VLAN sub-iface special handling. The v0
test setup doesn't VLAN-trunk VFs, so this is not a blocker. A v1
that needs VLAN should walk `sgid_attr->ndev` (which the ib_core
resolver does internally) instead of the port's base netdev.

### 10.2 No call into `mlx5_set_path` for the synthesized qpc blob -- *RETRACTED*

> **RETRACTED** (2026-06-06): the *premise* of this entire section --
> "the optpar mask `MLX5_QP_OPTPAR_PRIMARY_ADDR_PATH` tells FW which
> path subfields to consume" -- is correct as a description of the
> optpar bit's *intent*, but presupposes that FW will accept this
> opcode + optpar combination at all. As §12 establishes, it does
> not, on either side of the gate (mlx5_ib's own `opt_mask` table
> excludes `PRIMARY_ADDR_PATH` from every state-pair cell, and CX-7
> 28.x firmware independently rejects with syndrome `0x00498c8b`).
> The follow-up Patch B (`66edef3f790f` / `fe188a601af9`) that tried
> the *opposite* approach -- mirroring the entire pre-queried
> `primary_address_path` into the modify blob and overriding only
> `rmac_*` -- ALSO got the same syndrome, ruling out "blob internal
> consistency" as the rejection cause. So this design choice
> was moot in both directions. Preserved verbatim below for the
> record; the helper itself is gone.

§5.2's erratum suggested still calling `mlx5_set_path` into the
synthesized qpc blob to preserve consistency checks. **As built we
write only the `path.rmac_*` field** via
`ether_addr_copy(MLX5_ADDR_OF(ads, path_in, rmac_47_32), new_dmac)`.

Why: the optpar mask `MLX5_QP_OPTPAR_PRIMARY_ADDR_PATH` tells FW
which path subfields to consume from the qpc; FW only consumes
those gated by the optpar bit. We're touching exactly one logical
field (the dmac), so we set exactly that field. All other QPC
fields (PSN, dest_qpn, sgid_index, traffic_class, flow_label, etc.)
came from `LOAD_VHCA_STATE` and are still trusted verbatim. No
consistency checks are skipped because no consistency checks were
needed -- we're not refreshing those fields.

The §5.2 erratum's "still call mlx5_set_path" was conservative
caution from a place where we hadn't yet articulated that "MLX5_SET
on rmac_*" is the entire payload. It is.

### 10.3 Non-fatal error policy at the call site

§5.1 gated `goto err_destroy_qp` on real (non-`-EAGAIN`) errors. **As
built `mlx5_ib_restore_qp` logs the error loudly via `mlx5_ib_warn`
but treats it as non-fatal** -- the QP is already adopted +
registered in dev lists, and a stale-dmac QP is no worse off than
the pre-fix behavior (it was the entire bug we're fixing!). Failing
the restore would tear down a QP whose dmac just happens to be the
old one; the right answer is to surface the QP and let the operator
rerun `check_qp_av_dmac.sh` for localization.

The v1 / Policy B follow-up may upgrade this to a sticky per-QP
flag that retries on first `post_send`; for v0 this is sufficient.

### 10.4 Helper signature: `mlx5_core_qp` not `mlx5_ib_qp`

The kerneldoc-and-prototype pair in `qp.h` deliberately mirrors the
rest of that header's `mlx5_qpc_*` / `mlx5_core_qp_*` family:

```c
int mlx5_ib_restore_qp_refresh_av_dmac(struct mlx5_ib_dev *dev,
                                       struct mlx5_core_qp *qp);
```

The helper itself only consumes `qp->qpn` (passed verbatim into
`MLX5_SET(query_qp_in, in, qpn, ...)`) and `qp->uid` (consumed
implicitly by `mlx5_core_qp_modify`). The ibverbs-level gates --
"skip kernel-mode QPs", "skip non-RC/UC QP types" -- are NOT inside
the helper. They live at the call site:

```c
if (qp->ibqp.uobject &&
    (qp->type == IB_QPT_RC || qp->type == IB_QPT_UC)) {
        err = mlx5_ib_restore_qp_refresh_av_dmac(dev, &base->mqp);
        ...
}
```

This keeps `qp.h` free of `struct mlx5_ib_qp` exposure (the only
forward decl needed in qp.h is the long-standing `struct mlx5_ib_dev`),
and makes the call site read like the natural English statement of
the semantic ("if the QP is a userspace RC/UC QP, refresh its
dmac"). The helper itself remains a focused FW-command sequence
(QUERY_QP -> neigh_lookup -> conditional MODIFY_QP) that can in
principle be reused by any future code path that needs the same
operation against a `mlx5_core_qp` -- e.g. a probe ioctl for
KS6b.2-style validation.

## 11. Dev-branch backup: userspace-triggered refresh ioctl (KS6b.6) -- *REVERTED*

> **REVERTED** (2026-06-06): the ioctl described in this section
> drove the same `MODIFY_QP(RTS2RTS_QP, PRIMARY_ADDR_PATH)`
> sequence as the always-on `mlx5_ib_restore_qp_refresh_av_dmac`
> helper, just from a userspace trigger point AFTER the harness's
> `pin_static_neighbor_*` step. End-to-end run with the ioctl in
> place AND neighbor entries pinned hit the FW reject documented
> in §12: `op_status=0xffffffea` (`-EINVAL`), `op_syndrome=0x00498c8b`,
> `dmac_changed=1` but `post_dmac == pre_dmac` (the modify did not
> land). Patch B (`fe188a601af9`) that mirrored a fully-formed
> primary path into the modify blob did not change the verdict.
>
> Surfaces removed in the §S6b revert series:
> * `MLX5_VFMIG_IOC_REFRESH_AV_DMAC` (cmd `0x12`) and
>   `struct mlx5_vfmig_refresh_av_dmac` from `include/uapi/linux/mlx5_vfmig.h`
> * `vfmig_ioc_refresh_av_dmac()` handler + dispatch in `vfmig.c`
> * `vfmig_lookup_l2_dmac()` / `vfmig_path_extract_dmac()` static helpers
> * `mlx5_vfmig refresh_av_dmac <vf_id> <qpn>` CLI verb in
>   `tools/testing/mlx5_vfmig/tools/mlx5_vfmig.c`
> * `tools/testing/mlx5_vfmig/uobject_restore/qp_av_dmac/refresh_av_dmac.sh`
>   wrapper script
>
> The §11.5 `lookup_l2_dmac` deduplication TODO is moot now that
> both call sites are gone. Preserved verbatim below for the
> record; KS7.1 takes over the av.dmac problem.


The first end-to-end run with KS6b.1 in place surfaced a harness
ordering issue rather than a kernel bug: the always-on
`mlx5_ib_restore_qp_refresh_av_dmac` was being called inside the
`RESTORE_QP` uobject path **before** the harness's
`pin_static_neighbor_*` step had populated the destination's ARP /
NDISC table. `vfmig_lookup_l2_dmac` therefore returned `-ENOENT`
on every restored RC/UC QP and Policy A (log + skip) silently
deferred the refresh -- leaving the QPC with the stale
source-resolved dmac and the data path back at `RETRY_EXC`.

The dmesg signature on both physical hosts after that run:

```
vfmig: refresh_av_dmac qpn=0xff: deferring -- destination neighbor
for dgid=0000:0000:0000:0000:0000:ffff:c0a8:6404 not resolved
(err=-2). First post_send will RETRY_EXC until the operator
populates the neighbor entry...
```

(Notice `err=-2` == `-ENOENT` from `neigh_lookup`. The dgid is the
v4-mapped form of the peer's IPv4. `qpn=0xff` is the migrated QP.)

### 11.1 Why we need a userspace surface and not a kernel-only fix

The natural in-kernel alternative is "Policy B: refresh on first
post_send". It does not work for our case:

- `mlx5_ib_post_send` is the kverbs `ib_device_ops.post_send`
  callback, reached only via `ib_post_send()` from kernel-mode
  RDMA consumers (NVMe-RDMA, IPoIB, RDS).
- For uverbs-created QPs (i.e. anything our CRIU harness
  restores), the WQE write + UAR doorbell happen entirely in
  userspace via the libibverbs-mapped SQ buffer. The kernel sees
  nothing on the data path.

So the kernel has no synchronous hook between `RESTORE_QP`
returning and the application's first `ibv_post_send`. The only
hooks left are control-plane ioctls (modify_qp, query_qp, destroy
via uverbs) and async events. Neither fires "automatically before
the first WQE."

A workqueue-driven background retry (the kernel polls
`neigh_lookup` every N seconds until it succeeds) is a possible
v1 follow-up but adds a per-QP timer and a periodic poll; the
explicit ioctl is simpler, exposes structured pre/post output for
the harness log, and lets the harness drive ordering precisely.

### 11.2 As-built shape

| component | what changed | LOC |
|---|---|---|
| `include/uapi/linux/mlx5_vfmig.h` | new `MLX5_VFMIG_IOC_REFRESH_AV_DMAC` (cmd 0x12) + `struct mlx5_vfmig_refresh_av_dmac` carrying `vf_id` / `qpn` inputs and pre-op / lookup / op / post-op output fields. | ~180 (incl. doc comment) |
| `drivers/net/ethernet/mellanox/mlx5/core/vfmig/vfmig.c` | `vfmig_lookup_l2_dmac()` static helper (mirror of `mlx5_ib_lookup_l2_dmac` but uses `mlx5_uplink_netdev_get`), `vfmig_path_extract_dmac()` static helper, `vfmig_ioc_refresh_av_dmac()` handler, dispatch wiring. | ~230 |
| `tools/testing/mlx5_vfmig/tools/mlx5_vfmig.c` | new `do_refresh_av_dmac()` verb-handler with key=value output (including a derived `verdict=` line) and updated usage. | ~110 |
| `tools/testing/mlx5_vfmig/uobject_restore/qp_av_dmac/refresh_av_dmac.sh` | new wrapper mirroring `check_qp_av_dmac.sh`'s PF/VF derivation; drives the ioctl and decodes the verdict. | ~140 |

**Note: there is intentional code duplication** between
`mlx5_ib_lookup_l2_dmac` (in `drivers/infiniband/hw/mlx5/qp.c`)
and `vfmig_lookup_l2_dmac` (in
`drivers/net/ethernet/mellanox/mlx5/core/vfmig/vfmig.c`). The bodies are
~50 LOC each and identical apart from how each side acquires the
netdev (mlx5_ib uses `ib_device_get_netdev`; mlx5_core uses
`mlx5_uplink_netdev_get`). A follow-up commit can pull the
neigh-lookup core down into a `mlx5_core` lib helper exported via
`EXPORT_SYMBOL_GPL` and have mlx5_ib call it; deferred until the
runtime fix is verified end-to-end (so the refactor doesn't gate
the fix). The orchestrators above the lookup don't unify well --
mlx5_ib's helper goes through `mlx5_core_qp_modify` (which uses
`qp->uid`), the ioctl runs MODIFY_QP host-priv with explicit
`uid=0` -- so the ~80 LOC orchestrator duplication is not
recoverable by a shared helper.

The orchestrator follows the same QUERY_QP -> action -> QUERY_QP
bracketing pattern as `vfmig_ioc_probe_qp_teardown`. The op-under-
test errno is reported in `op_status` / `op_syndrome` rather than
propagated as the ioctl return, so the caller still sees the
post-op snapshot even on FW reject. Pre-op and lookup failures
short-circuit with the rest of the output zeroed.

### 11.3 No module-parameter gate

An earlier draft of this section gated the ioctl on a default-off
`vfmig_av_dmac_refresh_enabled` module parameter. Dropped: the
ioctl is unconditionally available, on the principle that if we
don't want the surface we drop the patch entirely rather than ship
a feature behind a knob nobody flips. The "EXPERIMENTAL DEV-BACKUP"
disclaimer in the UAPI doc-comment carries the same warning that
the other PROBE_* / QUERY_* ioctls already use ("can be removed
without breaking any in-tree consumer").

### 11.4 Harness recipe

On each physical host, after CRIU restore returns and AFTER
`pin_static_neighbor_*` runs:

```sh
sudo PF=$PF_BDF tools/testing/mlx5_vfmig/uobject_restore/qp_av_dmac/\
refresh_av_dmac.sh <vf_id> <qpn>
```

Expected post-fix verdict on both hosts:

```
*** VERDICT=REFRESHED_OK ***
```

Cross-check by re-running `check_qp_av_dmac.sh` -- it should
flip from `DMAC_IS_LOCAL` to `DMAC_IS_PEER` for the same
(vf_id, qpn). Then the application data path should make
forward progress.

If a host reports `VERDICT=NEIGH_UNRESOLVED` after the
`pin_static_neighbor_*` step, the static neighbor wasn't pinned
on the right interface -- inspect `ip neigh show dev $IFACE` and
re-run the pin step against the VF's RoCE netdev (typically
visible at `/sys/bus/pci/devices/$VF_BDF/net/<iface>`).

### 11.5 Follow-up: `lookup_l2_dmac` deduplication

Tracked TODO -- pull the neigh-lookup core (which takes an
already-acquired `net_device *` plus the dgid bytes) down into a
new exported helper in
`drivers/net/ethernet/mellanox/mlx5/core/lib/`, and have both
`mlx5_ib_lookup_l2_dmac` and `vfmig_lookup_l2_dmac` reduce to a
~15-LOC wrapper that does its idiomatic netdev acquisition and
delegates. Defer until the v0 runtime fix is verified end-to-end.

## 12. Post-mortem: post-RTR primary-AV refresh is not supported on mlx5 + CX-7 28.x

The §5 / §10 / §11 mitigation strategy rested on a single
load-bearing premise (carried verbatim from §5.2 erratum and §10.2):

> FW *does* support primary-AV update at RTS via the `RTS2RTS_QP`
> opcode + `MLX5_QP_OPTPAR_PRIMARY_ADDR_PATH` bit (see the optpar
> mapping in `set_qp_state_optpar`'s `IB_QP_AV` case). This helper
> bypasses the verbs gate by calling `mlx5_core_qp_modify`
> directly...

That premise was **never verified end-to-end**, and the end-to-end
runs that finally exercised it have **falsified it**, in three
mutually corroborating ways.

### 12.1 Empirical: firmware rejects the opcode + optpar combination

With the always-on path (KS6b.1) AND the userspace ioctl (KS6b.6)
in tree, AND the harness ordered to pin static neighbors before
firing the ioctl (so `neigh_lookup` actually returned the right
peer MAC), the `MODIFY_QP(RTS2RTS_QP, opt_param_mask=PRIMARY_ADDR_PATH)`
sent through `mlx5_core_qp_modify` reproducibly returned:

```
op_status=0xffffffea (-EINVAL)
op_syndrome=0x00498c8b
```

against an otherwise-healthy restored RTS RC QPC:

```
pre_qpc_state=3                          (RTS, as required by RTS2RTS)
pre_vhca_port_num=1                      (Ethernet, RoCE v2)
pre_sgid_index=3                         (valid GID slot, RoCE v2 IPv4 mapped)
pre_dmac=02:00:ef:04:00:01               (stale == local NIC MAC)
pre_dgid=...c0a8:6404                    (peer IP, valid IPv4-mapped IPv6)
lookup_status=0
resolved_dmac=02:00:f0:04:00:01          (peer NIC MAC, neigh_lookup OK)
dmac_changed=1
post_dmac=02:00:ef:04:00:01              (modify did not land)
```

A subsequent Patch B (`66edef3f790f` / `fe188a601af9`) that built
the modify blob's `primary_address_path` by mirroring the *entire*
pre-op `QUERY_QP` path and overriding only `rmac_*` produced
identical results -- same syndrome, same `post_dmac == pre_dmac`.
This rules out "modify-blob internal consistency" as the rejection
cause: FW rejects the opcode + optpar bit combination itself, not
the payload shape.

(For completeness: the diagnostic
[`check_qp_av_dmac.sh`](#60-cheapest-diagnostic-cdev-query_qp--check_qp_av_dmacsh)
captured the *symptom* correctly -- `VERDICT=DMAC_IS_LOCAL`. The
bug it identified is real. What §5 / §10 / §11 got wrong was the
*remediation path*.)

### 12.2 Structural: mlx5_ib's own opt_mask allowlist excludes PRIMARY_ADDR_PATH everywhere

`drivers/infiniband/hw/mlx5/qp.c` carries the kernel's
authoritative FW-accept allowlist as a 3-D table:

```c
static enum mlx5_qp_optpar opt_mask
        [MLX5_QP_NUM_STATE]   /* current state */
        [MLX5_QP_NUM_STATE]   /* target  state */
        [MLX5_QP_ST_MAX];     /* qp type */
```

`__mlx5_ib_modify_qp` AND-masks the user-requested optpar with
`opt_mask[mlx5_cur][mlx5_new][mlx5_st]` before issuing the FW
command (`optpar &= opt_mask[...]`). Auditing the table:

| state pair      | RC opt_mask bits                                                      | `PRIMARY_ADDR_PATH` allowed? |
|-----------------|------------------------------------------------------------------------|------------------------------|
| INIT -> INIT    | `RRE \| RAE \| RWE \| PKEY_INDEX \| PRI_PORT \| LAG_TX_AFF`                | no                           |
| INIT -> RTR     | `ALT_ADDR_PATH \| RRE \| RAE \| RWE \| PKEY_INDEX \| LAG_TX_AFF`           | no                           |
| RTR  -> RTS     | `ALT_ADDR_PATH \| RRE \| RAE \| RWE \| PM_STATE \| RNR_TIMEOUT`            | no                           |
| RTS  -> RTS     | `RRE \| RAE \| RWE \| RNR_TIMEOUT \| PM_STATE \| ALT_ADDR_PATH`            | **no**                        |
| SQER -> RTS     | `RNR_TIMEOUT \| RWE \| RAE \| RRE`                                     | no                           |

`MLX5_QP_OPTPAR_PRIMARY_ADDR_PATH` (bit `1 << 7`) is **never** in
any cell of `opt_mask` for any `(cur, new, qp_type)` triple. The
verbs path's `ib_mask_to_mlx5_opt(IB_QP_AV)` does map to
`PRIMARY_ADDR_PATH | PRI_PORT`, but both bits get AND-masked to
zero before reaching the FW. mlx5_ib relies on the primary AV being
set as a *mandatory* QPC field on the INIT -> RTR transition (where
the path is required, not optional, so it bypasses the optpar gate
entirely) and offers **no path through the optpar-gated route to
update the primary AV at any state**.

Our reverted helpers bypassed `__mlx5_ib_modify_qp` and called
`mlx5_core_qp_modify` directly, which skips the AND-mask. That's
why the helper *built and ran* but FW rejected: we were sending an
optpar bit the kernel itself never expects to see in any
`MODIFY_QP` FW command, against a state-pair whose accept list
doesn't include it.

### 12.3 Structural: the verbs layer agrees

`ib_modify_qp_is_ok(IB_QPS_RTS, IB_QPS_RTS, IB_QPT_RC, IB_QP_AV)`
returns `false` -- the IB verbs layer's per-state-pair allowed
attribute mask (`qp_state_table[][]` in `drivers/infiniband/core/verbs.c`)
disallows `IB_QP_AV` for RTS -> RTS on RC. So an application
calling `ibv_modify_qp(qp, &attr, IBV_QP_AV)` against an RTS QP
fails before any kernel command is built. The §5 / §10 sketches
acknowledged this gate ("`__mlx5_ib_modify_qp` rejects with
`-EINVAL`") and chose to bypass it. We now know the bypass was
trying to drive the FW into a corner the FW also doesn't support.

### 12.4 Conclusion and forward path

Post-RTR primary-AV refresh isn't a thing on mlx5 + CX-7 28.x.
There is no opcode + optpar pairing that lands a corrected DMAC
into a live RTS QPC. Every mechanism we have for installing
primary AV runs at INIT -> RTR (mandatory, fixed), or via
`LOAD_VHCA_STATE` (the entire QPC verbatim). Once the QP is in
RTR or beyond, the path is immutable from the host side.

Two corollaries:

1. **The architecturally clean fix is to land the corrected DMAC
   *before* `LOAD_VHCA_STATE`**, by rewriting `path.rmac_*` in
   the saved QPC blob. This is exactly what
   [`vf_prerestore_split.md`](vf_prerestore_split.md) **KS7.1**
   describes (pre-RESTORE_QP DMAC fixup): the orchestrator
   resolves the destination's peer MAC via `neigh_lookup` /
   pinned static neighbor, edits the QPC payload in flight, and
   `LOAD_VHCA_STATE` then installs a QPC that's already correct
   at t=0. No state transitions, no FW commands beyond the
   normal restore path, no `MODIFY_QP` syndromes.

2. **A destructive rebuild path** -- driving the QP through
   `2RST_QP` and back through `INIT2INIT_QP` / `INIT2RTR_QP`
   with a fresh primary AV containing the corrected DMAC -- is
   theoretically possible (the primary AV bit at INIT -> RTR is
   mandatory, so it lives outside the optpar gate) but resets
   PSN and breaks the connection's invariants. It defeats the
   point of CRIU restore (which exists to preserve those
   invariants verbatim) and is therefore not viable for the
   workload.

KS7.1 is the canonical owner of §S6b's stale-dmac problem going
forward. KS6b.1 / KS6b.6 in this document are reverted as
unrealizable on the targeted FW; the diagnostic tooling
(`check_qp_av_dmac.sh`, `tools/mlx5_vfmig query_qp`) stays in
tree as the canonical way to *confirm* a DMAC is correct on a
freshly restored QP, which is the verification surface KS7.1
will key off of.

## Changelog

- 2026-06-04: initial draft, filed by the CRIU agent after the
  first end-to-end run with QP restore. Diagnosis tentative;
  awaiting mid-failure capture per §6.
- 2026-06-04 (later): added §6.0 cheap diagnostic
  (`tools/mlx5_vfmig query_qp` + `check_qp_av_dmac.sh`) that
  decodes `path.rmac_*` from the existing `MLX5_VFMIG_IOC_QUERY_QP`
  ioctl and emits a `DMAC_IS_LOCAL` / `DMAC_IS_PEER` /
  `DMAC_AMBIGUOUS` verdict in seconds. Added §5.2 erratum: the
  proposed `mlx5_ib_modify_qp(IB_QP_AV)` primitive is closed by
  the verbs `qp_state_table` for RTS->RTS primary AV; the fix
  needs a direct `mlx5_cmd_exec(MODIFY_QP, RTS2RTS,
  PRIMARY_ADDR_PATH)` from inside `mlx5_ib_restore_qp` instead.
  Reordered Kernel Asks: KS6b.0 (run the diagnostic) is a
  prerequisite before any kernel code is written.
- 2026-06-04 (evening): diagnosis **LOCKED** --
  `check_qp_av_dmac.sh` returned `VERDICT=DMAC_IS_LOCAL` on both
  physical hosts post-restore. Kernel-side fix
  `mlx5_ib_restore_qp_refresh_av_dmac()` **landed** in
  `drivers/infiniband/hw/mlx5/{qp.c,qp.h,main.c}` (~115 LOC, builds
  clean). Implementation diverges from §5.1 in three places --
  using `neigh_lookup` directly instead of ib_core's
  `rdma_addr_find_l2_eth_by_grh` (which is private to ib_core),
  not calling `mlx5_set_path` for the rmac-only payload, and
  non-fatal error treatment at the call site. See §10 for the
  full as-built shape and the rationale for each divergence.
  KS6b.1 marked DONE; KS6b.2 (FW acceptance via end-to-end harness
  rerun + verdict flip to `DMAC_IS_PEER`) and KS6b.4 (regression
  guard) remain.
- 2026-06-04 (later evening): first end-to-end run with KS6b.1 in
  place hit the harness-ordering issue documented in §11 -- the
  always-on path runs inside `RESTORE_QP` BEFORE the harness pins
  static neighbors, so `vfmig_lookup_l2_dmac` returns `-ENOENT`
  and Policy A defers on every restored QP. KS6b.5 (Policy B on
  first `mlx5_ib_post_send`) ruled out as architecturally
  unreachable for uverbs QPs. New KS6b.6 added: dev-branch backup
  ioctl `MLX5_VFMIG_IOC_REFRESH_AV_DMAC` (cmd 0x12) that re-drives
  the same QUERY_QP -> neigh_lookup -> MODIFY_QP sequence from
  userspace, callable AFTER `pin_static_neighbor_*` runs.
  Implementation landed in `drivers/net/ethernet/mellanox/mlx5/
  core/vfmig/vfmig.c` (~230 LOC handler), `tools/testing/mlx5_vfmig/
  tools/mlx5_vfmig.c` (`refresh_av_dmac` verb), and
  `tools/testing/mlx5_vfmig/uobject_restore/qp_av_dmac/
  refresh_av_dmac.sh` wrapper. Module-parameter gate considered
  and rejected (see §11.3). KS6b.6 marked DONE; KS6b.2 / KS6b.4
  pending end-to-end harness rerun.
- 2026-06-06: end-to-end run with the ioctl in tree AND neighbor
  entries pinned reproduced the FW reject documented in **§12**:
  `op_status=0xffffffea` (`-EINVAL`),
  `op_syndrome=0x00498c8b`, `dmac_changed=1` /
  `post_dmac == pre_dmac`. Patch B (commits `66edef3f790f` /
  `fe188a601af9`) that mirrored a fully-formed
  `primary_address_path` from a pre-op `QUERY_QP` into the
  modify blob did not change the verdict, ruling out modify-blob
  internal consistency as the rejection cause. Audit of
  mlx5_ib's `opt_mask[cur][new][qp_type]` allowlist established
  that `MLX5_QP_OPTPAR_PRIMARY_ADDR_PATH` is absent from every
  cell, mirroring the FW gate -- post-RTR primary-AV refresh is
  not a supported operation on this driver / FW combination.
  KS6b.1 + KS6b.6 + Patch B + design-doc updates around them
  **REVERTED** as architecturally infeasible; ownership of §S6b
  moved (transiently) to `vf_prerestore_split.md` KS7.4
  (pre-RESTORE_QP blob fixup). Top-of-file STATUS banner, §10.2
  RETRACTED note, §11 REVERTED note, and a then-new §12
  post-mortem all added in this pass. Diagnostic tooling
  (`check_qp_av_dmac.sh`, `tools/mlx5_vfmig query_qp`) stays in
  tree.
- 2026-06-08: **RESOLVED.** Empirical orchestrator-side test on
  `rdma_test_agent_vfmig_criu_swap_after_qp.yaml` confirmed
  that the §S6b stale-dmac problem is fully addressable by
  adding `ip link set <PF> vf <VF_ID> mac
  <source-time-peer-vf-mac>` to the destination-side
  prerestore steps and pointing the existing
  `pin_static_neighbor_*` step at source-time peer-VF MACs
  instead of the destination host's local NIC MACs.
  `check_qp_av_dmac.sh` returns `VERDICT=DMAC_IS_PEER` on
  both hosts and `ping_pong_after_restore` returns OK MATCH
  on the unmodified, source-loaded QPC. **No kernel work
  needed**: the missing piece was the same per-VF identity
  migration that SR-IOV VM live-migration performs for free
  (vNIC MAC moves with the VM); CRIU process-swap requires
  the same step explicitly because VFs don't move with the
  process. Doc reframed: top of file rewritten as
  §0 (what was actually wrong) / §1 (resolution) / §2
  (validation), with the original investigation chain
  (§§1-12 plus the `Outstanding evidence` and original
  `TL;DR` sections) preserved verbatim under
  **Appendix A: investigation history**. KS7.4 in
  `vf_prerestore_split.md` collapses from "pre-RESTORE_QP
  blob fixup" to "orchestrator-side VF identity migration;
  no kernel surface required".
