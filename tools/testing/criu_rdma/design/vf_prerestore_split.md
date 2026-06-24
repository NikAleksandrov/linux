# DESIGN: vf_prerestore -- split host-level VF restore from process-level
# uobject restore so mid-restore host config (ARP pin, IP setup, route)
# can land between LOAD_VHCA_STATE and RESTORE_QP

> Single source of truth for the prerestore-split architecture.
> Covers the **CRIU-side** plugin work (new standalone binary,
> plugin `init()` branch logic, soft-fallback contract, multi-VF
> roadmap, alternates considered) **and** the **kernel-side**
> surfaces that work depends on:
>
> * KS7.1 -- the existing `MLX5_VFMIG_IOC_QUERY_VF.restored`
>   indicator. **LANDED**, no new kernel work.
> * KS7.2 -- the `MLX5_VFMIG_IOC_REFRESH_AV_DMAC` ioctl.
>   **REVERTED** as architecturally infeasible AND
>   no longer needed; the ?S6b stale-dmac problem it was
>   trying to address turned out to be a missing
>   orchestrator-side step, not a kernel gap. See
>   [`qp_av_dmac_swap.md` STATUS banner](qp_av_dmac_swap.md)
>   for the resolution and Appendix A ?12 of that doc for
>   the FW post-mortem we generated along the way (preserved
>   so the next person who's tempted by post-LOAD primary-AV
>   refresh sees why it doesn't work on mlx5 + CX-7 28.x).
> * KS7.3 -- an **orchestrator-owned** per-VF UUID
>   (`SET_VF_UUID` write ioctl, called by the orchestrator
>   when provisioning a VF; `vf_uuid` extension on
>   `QUERY_VF`, read by CRIU at dump and restore time).
>   **NEW**, small. CRIU never writes a UUID -- the
>   orchestrator owns the workload ��� VF binding and is the
>   only component that can assign UUIDs consistently
>   across save/restore. Required because `vhca_id` is not
>   stable across SAVE/LOAD (see ?3.5); without it the
>   prerestore model has no safe way to bind a CRIU dump
>   to a specific prerestored VF.
> * KS7.4 -- **orchestrator-side per-VF identity migration
>   on the destination, pre-LOAD_VHCA_STATE** (?S6b
>   resolution). The orchestrator (or whatever provisioning
>   tool drives the VF on the destination) sets the
>   destination VF's MAC, IP, and pinned ARP to mirror the
>   source-time peer-VF's identity, the same way SR-IOV VM
>   live-migration sets the destination VF's admin MAC to
>   the migrating VM's vNIC MAC. With those steps in place
>   the source-baked `path.rmac_*` in the saved QPC is
>   correct at t=0; LOAD_VHCA_STATE installs the QPC
>   verbatim and RESTORE_QP works on it without
>   modification. **No new kernel surface required** -- this
>   ask is documentation + harness wiring, not a kernel
>   patch. Spec lives in ?4.6. Empirically confirmed
>   2026-06-08; see `qp_av_dmac_swap.md` STATUS banner +
>   ?1 (Resolution) + ?2 (Validation).
>
> **Kernel agent: ALL v0 kernel work is now landed.** KS7.1
> already in place (existing `MLX5_VFMIG_IOC_QUERY_VF`); KS7.3
> landed 2026-06-08 (new ioctl 0x12 `MLX5_VFMIG_IOC_SET_VF_UUID`
> + 16-byte `vf_uuid` field on the QUERY_VF return struct);
> KS7.2 reverted; KS7.4 resolved orchestrator-side with no
> kernel surface required. Next handoff is the CRIU agent
> picking up KS7.3's UAPI in the dump/restore plugin (see
> ?3.5.3 for the contract). Start at
> [?2](#2-kernel-agent-asks-at-a-glance) for the at-a-glance
> table; the KS7.3 specification + lifecycle lives in ?3.5.
> The CRIU-side sections (?6-?10) are the next agent's
> focus.
>
> Filed by the CRIU agent. Companion to
> [`uobject_restore.md`](uobject_restore.md) and
> [`qp_av_dmac_swap.md`](qp_av_dmac_swap.md). Numbering follows the
> existing `pd_registration_wipe.md` / `qp_av_dmac_swap.md` chain;
> the kernel agent should pick a ?S identifier that fits the wider
> scheme (probably ?S6c since the dmac-refresh enablement is the
> immediate motivator).

## TL;DR

> **Updated 2026-06-08.** The ?S6b dmac-stale problem that
> originally motivated this doc has been **resolved
> orchestrator-side, with no kernel work**. The fix is the
> same per-VF identity migration SR-IOV VM live-migration
> already does for free: set the destination VF's MAC, IP,
> and pinned ARP to mirror the source-time peer-VF identity
> via `ip link set vf mac` + `ip addr add` + `ip neigh
> replace`, all before `LOAD_VHCA_STATE`. With those steps
> the source-baked QPC is correct at t=0 and `LOAD_VHCA_STATE
> ��� RESTORE_QP` works on the unmodified blob. See
> [`qp_av_dmac_swap.md`](qp_av_dmac_swap.md) STATUS banner +
> ?0 / ?1 / ?2 for the field-by-field analysis and
> validation; [Appendix A ?12](qp_av_dmac_swap.md#12-post-mortem-post-rtr-primary-av-refresh-is-not-supported-on-mlx5--cx-7-28x)
> of that doc preserves the FW-rejection post-mortem we
> generated while still chasing a kernel-side fix. KS7.4 in
> this document collapses from "pre-RESTORE_QP blob fixup
> with a kernel patch surface" to "**orchestrator-side VF
> identity migration; no new kernel surface**" -- spec at
> ?4.6.

The ?S6b dmac-refresh problem is that `LOAD_VHCA_STATE`
faithfully preserves the source's QPC verbatim, including the
source-resolved `av.dmac` baked into `path.rmac_*` at
`MODIFY_QP_TO_RTR` time. On the destination physical host, the
*peer* of the restored process is now whichever process was
swapped onto the *other* physical host -- and that peer process
is using a different VF MAC than its source-time predecessor.
Outgoing RoCEv2 frames carry the source-baked `path.rmac` as L2
destination; if that MAC doesn't match the actual peer's
current VF MAC, frames are silently dropped by the fabric and
surface as `IBV_WC_RETRY_EXC_ERR` (status 12,
`vendor_err 0x81`) on first post-restore `post_send`.

The fix is to make the destination-side VF *take on* the
source-time peer-VF MAC -- the same way SR-IOV VM-LM sets the
destination VF's admin MAC to the migrating VM's vNIC MAC.
Combined with the existing IP-swap step (which already
auto-populates the GID table at the same `src_addr_index`
slot), that's enough: `path.rmac_*` matches the actual peer
post-swap, and `path.src_addr_index` resolves correctly.
Concretely, on each destination host before `LOAD_VHCA_STATE`:

```bash
ip link set <PF> vf <VF_ID> mac <source-time-peer-vf-mac>
ip addr add <source-time-peer-vf-ip>/<prefix> dev <vf_netdev>
ip neigh replace <source-time-local-ip> \
                 lladdr <source-time-local-vf-mac> \
                 dev <vf_netdev> nud permanent
```

(MAC first, then IP -- GID-table entries bind `(IP, MAC)` at
`ip addr add` time, and changing MAC after leaves the GID
entries with stale bindings.)

This split-restore architecture -- two operator-orderable
phases instead of CRIU's monolithic flow -- is still the right
shape for *making the orchestrator's reconfiguration step
explicit and operator-controllable*. It just acquires KS7.4's
identity-migration as one of its prerestore steps rather than
relying on a post-LOAD kernel helper that doesn't actually
exist (per the FW post-mortem in `qp_av_dmac_swap.md`
Appendix A ?12).

The two phases:

1. **VF prerestore** -- a standalone CRIU-side binary
   (`mlx5_vfmig_restore_vf`, dlopens `rdma_mlx5_vfmig_plugin.so`)
   that consumes the dump's plugin blob, hands operator-visible
   control to the orchestrator long enough to perform the
   per-VF identity migration above (KS7.4), then drives the
   kernel to apply `LOAD_VHCA_STATE`. The destination QPC
   therefore loads with the correct DMAC at t=0; no
   post-restore FW commands are needed.

2. **Process restore** -- a subsequent `criu restore` invocation.
   The plugin's `init()` checks a kernel-exposed VF state flag;
   if the VF was already loaded by step 1 the plugin **skips**
   `LOAD_VHCA_STATE` and proceeds straight to PD/CQ/MR/QP
   restore. Because step 1 already mirrored the source's per-VF
   identity onto the destination and `LOAD_VHCA_STATE` installed
   the QPC verbatim, the restored QP is datapath-ready as soon
   as `RESTORE_QP` completes.

This split is entirely a CRIU-side change. The kernel-side
surfaces it depends on:

1. **A per-VF loaded/unloaded indicator.** The existing
   `MLX5_VFMIG_IOC_QUERY_VF.restored` field on the PF cdev
   (`/dev/mlx5_vfmig/<pf_bdf>`, ioctl `0x03`) already exposes
   exactly this signal: it goes 0 -> 1 when `MARK_RESTORED`
   is issued during the destination LOAD lifecycle, and stays
   1 until VF teardown. The CRIU plugin's `init()` queries it
   per VF in the dump blob to decide between the prerestore
   path (skip LOAD) and the monolithic path (run LOAD). See ?3.

2. **An orchestrator-owned per-VF UUID.** New
   `MLX5_VFMIG_IOC_SET_VF_UUID` write ioctl (called by the
   orchestrator when provisioning a VF; CRIU never calls it)
   plus a 16-byte `vf_uuid` field added to the existing
   `MLX5_VFMIG_IOC_QUERY_VF` return struct. CRIU's dump path
   reads the source VF's UUID and stores it in the plugin
   image; CRIU's restore path iterates eligible PFs/VFs to
   find the destination VF carrying the matching UUID (which
   the orchestrator stamped before kicking off restore).
   Required because `vhca_id` is not stable across SAVE/LOAD
   -- the source's `vhca_id` is not in the SAVE blob, the
   destination's `vhca_id` is allocated by the dest PF at
   LOAD time, and the orchestrator's only collision detection
   today is a 60-second IOMMU-cmd-ring timeout. See ?3.5.

Surface (1) is LANDED. Surface (2) -- KS7.3 -- is the only
outstanding kernel ask. KS7.4 (per-VF identity migration) is
not a kernel ask at all; the existing `ip link set vf mac` /
`ip addr add` / `ip neigh replace` UAPIs are sufficient. None
of these require invasive changes to existing fast paths; all
are additive.

## Status

Pre-implementation on the CRIU side. KS7.1 is landed; **KS7.2
was reverted 2026-06-06** (see Appendix A ?4 banner /
`qp_av_dmac_swap.md` Appendix A ?12 for the FW post-mortem);
**KS7.4 needs no kernel surface** (resolved orchestrator-side;
empirically validated 2026-06-08 -- see `qp_av_dmac_swap.md`
STATUS banner). KS7.3 (orchestrator UUID) is the only
outstanding kernel-side ask (?2 / ?3.5). This doc is the
contract for the CRIU-side prerestore-binary work and the
KS7.3 kernel work that follows.

Soft-fallback default: if the prerestore binary was not run,
the plugin falls back to today's monolithic flow
(`LOAD_VHCA_STATE` inside `criu restore`'s `init()`). Behavior
on that path is **broken on the v0 swap workload** -- the QP
comes up with the wrong DMAC and first `post_send` fails with
`IBV_WC_RETRY_EXC_ERR`, exactly the ?S6b symptom. There is no
post-restore recovery: per the post-mortem above, FW does not
support post-LOAD primary-AV refresh. The v0 swap workload
therefore requires the prerestore binary (or equivalent
orchestrator-driven path that performs the KS7.4 identity
migration before `LOAD_VHCA_STATE`); a soft-fallback that
runs `LOAD_VHCA_STATE` without it surfaces as "QP not
operational" with no clean recovery path. The plugin still
logs whether the VF was prerestored or restored in-line so
post-hoc analysis can tell the two paths apart (see ?6.4).

## 1. Architecture: the two phases, end-to-end

### 1.1 Phase 1 -- VF prerestore (new)

Operator workflow:

```
$ mlx5_vfmig_restore_vf -D /var/lib/criu/img/<dumpid>/
prerestore: read 1 VF entry from image: vf_uuid=8f3a-...
prerestore: scanning vfmig-eligible PFs for matching vf_uuid...
prerestore: matched vf_uuid=8f3a-... at PF 0000:08:00.0 vf_id 0
prerestore: VF 0 (PF 0000:08:00.0): applying LOAD_VHCA_STATE...
prerestore: VF 0: restored=1 (eth4 up, ibdev mlx5_2)
prerestore: VF 0: ready for host-level setup; run 'criu restore' next
$ ip neigh add 192.168.100.4 lladdr 02:00:f0:04:00:01 dev eth4 nud permanent
$ criu restore -D /var/lib/criu/img/<dumpid>/ ...
```

Inside `mlx5_vfmig_restore_vf`:

* `dlopen("rdma_mlx5_vfmig_plugin.so")`.
* Resolve a new exported symbol `mlx5_vfmig_plugin_restore_vf_only`.
* That symbol reuses the existing `vfmig_blob` reader path the
  CRIU plugin already runs in `init()`, drives the same kernel
  uverbs / netlink calls that apply `LOAD_VHCA_STATE`, and
  returns once the netdev / ibdev have surfaced. It does **not**
  open a ucontext, allocate a PD, or restore any uobject.
* Before returning, the symbol issues `MLX5_VFMIG_IOC_MARK_RESTORED`
  for the VF. After that point, `MLX5_VFMIG_IOC_QUERY_VF` returns
  `restored=1` to any subsequent caller (in particular the CRIU
  plugin's `init()` on the next `criu restore` invocation).
* The symbol does NOT call `SET_VF_UUID` -- the orchestrator
  has already stamped the matching UUID on this VF as part of
  provisioning. The binary's job is to FIND the VF whose
  `vf_uuid` matches the dump entry's UUID, then drive LOAD on
  that VF. See ?3.5 for ownership rationale.

Detailed CRIU-side shape: ?6.1, ?6.2.

### 1.2 Phase 2 -- process restore (existing `criu restore`)

For each VF entry in the dump's plugin blob, the CRIU plugin's
`init()` runs a single QUERY_VF-driven step that produces both
the *identity* answer (which dest VF carries the matching
UUID?) and the *state* answer (did prerestore already run
against that VF?), then branches on the latter:

```
1. Identity match (KS7.3, mandatory):
     scan eligible PFs in /dev/mlx5_vfmig/* for a VF whose
     QUERY_VF.vf_uuid == image.vf_uuid;
     no match -> hard refuse (orchestrator must provision
                 destination VF with matching UUID first).

2. State branch on the matched VF's QUERY_VF.restored bit
   (KS7.1, returned by the same QUERY_VF call as step 1):

     restored == 1  -> prerestore binary already drove LOAD;
                       skip LOAD_VHCA_STATE, go to PD/CQ/MR/QP.
     restored == 0  -> soft-fallback: orchestrator stamped
                       the UUID but nobody ran prerestore.
                       Drive LOAD_VHCA_STATE inline against
                       the matched (PF, vf_id), then proceed
                       to PD/CQ/MR/QP. (This is today's
                       monolithic flow against a UUID-matched
                       slot.)
```

`restored` is **only** consulted *after* the UUID match has
already pinned down a specific (PF, vf_id) on the destination.
It is **not** a primary identity tag (a stale `restored=1` on
some unrelated VF would be ignored, because that VF's UUID
won't match this dump). It is the "did the prerestore binary
already do its thing on the matched VF" disambiguator that
gates the LOAD-or-skip-LOAD decision.

Identity-matching detail (orchestrator-owned `vf_uuid`):
the orchestrator stamps a 16-byte UUID into the kernel's
per-VF slot via `MLX5_VFMIG_IOC_SET_VF_UUID` when provisioning
the VF (on both source and destination, matching across
hosts). CRIU reads the source's UUID at dump time and stores
it in the plugin image; at restore time CRIU iterates
eligible PFs/VFs and matches by UUID. CRIU never writes a
UUID itself. Detailed in ?3.5 and ?6.3.

The binary `restored` state is intentional. A finer "loaded
but no process has restored its uobjects yet" state is
**not** modelled in v0 -- in a multi-process / multi-image
workload (multiple CRIU images sharing a single VF, see ?6.5)
the kernel would need per-image / per-uobject tracking to
know whether a VF is "fully loaded" vs "partially loaded
with some images still pending restore". That tracking is
out of scope for v0; we treat `restored=1` (LOAD applied /
staged for application) as the only state signal `init()`
needs. Future work could add per-uobject restore-completeness
tracking if a workload needs it; the kernel-side surface is
already designed to extend forward (the existing `restored`
field is binary, additional fields can be added to
`struct mlx5_vfmig_query_vf` without ABI break for in-tree
consumers, and out-of-tree consumers will recompile against
the new header anyway).

Detailed CRIU-side shape: ?6.3, ?6.4.

## 2. Kernel-agent asks at a glance

All kernel-side surfaces are now in place (KS7.1, KS7.3); KS7.2
was landed and then reverted (see row below + ?4 banner); KS7.4
needs no kernel surface at all (resolved orchestrator-side
2026-06-08). **One new kernel ask is open: KS7.6** (split
SUSPEND/RESUME out of SAVE/LOAD so DMA can be quiesced before CRIU
copies the dumpee's memory -- the stop-and-copy ordering fix; the
"KS7.5" label was already spent on the rejected cross-slot LOAD ask in
?3.5.3, hence KS7.6). Next
work item is the CRIU agent picking KS7.3's `vf_uuid` UAPI up
in the dump/restore plugin (see ?3.5.3).

| # | ask | where | status | size |
|---|---|---|---|---|
| KS7.1 | **VF loaded/unloaded indicator surfaced via `MLX5_VFMIG_IOC_QUERY_VF`** (ioctl 0x03 on the PF cdev). The existing `restored` field is set by `MLX5_VFMIG_IOC_MARK_RESTORED` during the destination LOAD lifecycle. **2026-06-10 update:** the bit is **transient** -- it's consumed by `mlx5_vfmig_vf_consume_restored()` at VF probe time (i.e. inside the `bind` write), so a post-bind QUERY_VF reads `restored=0` regardless of whether `MARK_RESTORED` was just issued. The CRIU plugin's `init()` always runs post-bind, so it cannot use this bit; it instead checks `/sys/bus/pci/devices/<vf_bdf>/driver` symlink existence as the soft-fallback signal under the orchestrator contract "destination VF is bound only by prerestore (or by the plugin itself)". The prerestore binary -- when it lands -- can still consume `restored` legitimately because it queries QUERY_VF **before** driving the bind. PF cdev is still the right identity surface: KS7.3's `vf_uuid` lives there, so identity matching is a single-ioctl roundtrip per VF iterated. The bind-state check is a separate sysfs `lstat()` per matched VF. | ?3 (implementation note) + ?6.3 | LANDED (existing field; no new kernel work). Plugin uses sysfs bind-check, not the bit. | -- |
| KS7.2 | ~~**`MLX5_VFMIG_IOC_REFRESH_AV_DMAC` ioctl** (?S6b "Alternative C" backstop). Landed in `b70b6624084a` with CLI wrapper in `1bbe576bc7c5`.~~ **REVERTED 2026-06-06** in `0a05d29edb2f` / `6055711aae44` / `5786cb303142`: post-RTR primary-AV refresh is not supported on mlx5 + CX-7 28.x. The ?S6b problem this ioctl was meant to address turned out to be addressable orchestrator-side (KS7.4); see `qp_av_dmac_swap.md` STATUS banner for the resolution and Appendix A ?12 of that doc for the FW post-mortem. | ?4 (historical) | REVERTED | -- |
| KS7.3 | **Orchestrator-owned per-VF UUID.** New `MLX5_VFMIG_IOC_SET_VF_UUID` write ioctl 0x12 on the PF cdev (called by the **orchestrator** when provisioning the VF; CRIU never calls it), plus a 16-byte `vf_uuid` field appended to the existing `MLX5_VFMIG_IOC_QUERY_VF` return struct (struct grows; ioctl number bumps via the `_IOWR` `sizeof` encoding; same ABI pattern as the earlier QUERY_QP grow). Storage is `uuid_t vf_uuid` on the per-VF context (`mlx5_vf_context.vf_uuid`), using the kernel's standard `<linux/uuid.h>` helpers (`uuid_is_null` / `uuid_equal` / `uuid_copy` / `import_uuid` / `export_uuid`); the value is cleared on SR-IOV teardown so a recycled slot starts fresh. CRIU dump reads `vf_uuid` via QUERY_VF and stores it in the plugin image; CRIU restore iterates VFs across eligible PFs to find the match. Required because `vhca_id` is not stable across SAVE/LOAD (?3.5) and the orchestrator's only collision detection today is a 60-second IOMMU-cmd-ring timeout. The orchestrator-side contract on top of this surface (?3.5.3) requires that the destination VF be stamped on the **same `vf_id` slot** as the source -- this is hard-enforced by CRIU restore via a paired `(vf_uuid, vf_id)` match because the kernel's per-VF IOVA window is `vf_id`-keyed and the FW E-Switch `vport_num` is `vf_id+1`-keyed (?3.5.3.1). **LANDED 2026-06-08** with kernel-matrix C probe (`uobject_restore/vf_uuid/vf_uuid_probe_mlx5_vfmig`) and lifecycle / multi-VF shell harness (`uobject_restore/vf_uuid/test_vf_uuid_lifecycle.sh`). | ?3.5 | LANDED | tiny |
| KS7.6 | **Split SUSPEND/RESUME out of SAVE/LOAD (stop-and-copy ordering fix).** CRIU copies the dumpee's memory before the RDMA plugin quiesces the datapath, so a peer's RDMA WRITE/SEND (or the mlx5 VF's own DMA) can land in pinned MR pages mid-snapshot. Fix: add `MLX5_VFMIG_IOC_SUSPEND_VHCA` (0x13) + `MLX5_VFMIG_IOC_RESUME_VHCA` (0x14) so CRIU can SUSPEND at the early `CHECKPOINT_DEVICES` hook (pre-memory-dump) and RESUME at `RESUME_DEVICES_LATE` (post-VMA-restore); `SAVE_VHCA_STATE` becomes suspend-aware (skips the in-SAVE suspend if already parked) and the restore bind path gains a `defer_resume` mode. Back-compatible: legacy harnesses never call the new ioctls and keep self-suspend + resume-on-close. Full spec: **`design/snapshot_ordering_pause_capture.md`** Part A. | new doc, Part A | OPEN (priority) | medium |
| KS7.4 | **Per-VF identity migration on the destination, pre-LOAD_VHCA_STATE** (?S6b resolution). Orchestrator (or whatever provisioning tool drives the VF on the destination) sets `ip link set <PF> vf <VF_ID> mac <source-time-peer-vf-mac>`, `ip addr add <source-time-peer-vf-ip>`, and `ip neigh replace <source-time-local-ip> lladdr <source-time-local-vf-mac>` to mirror the source's per-VF identity. With those steps in place, the source-baked `path.rmac_*` in the saved QPC matches the actual peer's VF MAC at t=0 and `LOAD_VHCA_STATE` installs a QPC that's already correct. Empirically confirmed 2026-06-08 on `rdma_test_agent_vfmig_criu_swap_after_qp.yaml`. **No kernel surface required** -- existing `ip link set vf mac` / `ip addr` / `ip neigh` UAPIs are sufficient. Spec lives in ?4.6. | ?4.6 | RESOLVED orchestrator-side; no kernel work | -- |

The earlier draft of this doc proposed a new sysfs node at
`/sys/bus/pci/devices/<pf>/vfmig/vf%u/state` for KS7.1. That
node would have been fragile -- the per-VF subdirectory's
existence pre-LOAD is not guaranteed (the VF kobject hierarchy
isn't always populated before the VF probes), and pre-VF
visibility is exactly what the prerestore detection needs.
The PF cdev surface used by the existing
`MLX5_VFMIG_IOC_QUERY_VF` ioctl avoids that problem cleanly:
the PF cdev is per-PF, lives at `/dev/mlx5_vfmig/<pf_bdf>`,
and the iteration model `0..num_vfs-1` works regardless of
which VFs are bound or have been touched.

The earlier draft also proposed using `vhca_id` to detect
"prerestored with the wrong dump". That was wrong: `vhca_id`
is allocated per-PF on the destination at LOAD time and is
not encoded in the SAVE blob (see ?3.5 for full analysis).
KS7.3's per-VF UUID replaces it with a stable identifier
**owned by the orchestrator**, not CRIU. CRIU's role is
purely passive: read at dump, store in the image, read +
match at restore. The orchestrator is the only component
that has all the workload ��� VF binding information needed
to assign UUIDs consistently across save/restore.

All v0 kernel work is now landed (KS7.1 already in place,
KS7.3 added 2026-06-08, KS7.2 reverted, KS7.4 no kernel
surface required). The remainder is CRIU-side (?6) plus
harness changes (?10).

## 3. KS7.1 -- VF loaded/unloaded indicator (already in place)

No new kernel work. The CRIU plugin and the prerestore binary
both consume the existing per-VF `restored` bit from
`MLX5_VFMIG_IOC_QUERY_VF` (ioctl `0x03` on the PF cdev).

> **Implementation note (2026-06-10): `restored` is transient,
> consumed at probe time.** The kernel's per-VF
> `sriov->vfs_ctx[vf_id].restored` is cleared by
> `mlx5_vfmig_vf_consume_restored()` as soon as `mlx5_load_one()`
> runs against the bound VF -- which happens immediately after the
> prerestore (or plugin-monolithic) `driver_override + bind` step
> in ?3.1 step 7. Any `MLX5_VFMIG_IOC_QUERY_VF` issued **after**
> the VF has been bound therefore reads `restored == 0`,
> regardless of whether `MARK_RESTORED` was just issued moments
> earlier. The bit's job is to drive the kernel-side post-LOAD
> probe quirks (FW page recovery, replay accounting); it is not a
> stable post-bind userspace signal.
>
> The CRIU plugin's `init()` always runs **after** the destination
> VF is bound (either prerestore drove the bind, or the plugin's
> own monolithic path drove it earlier in the same `init()`
> invocation), so `QUERY_VF.restored` cannot serve as the
> soft-fallback signal there. The plugin instead uses a
> **`/sys/bus/pci/devices/<vf_bdf>/driver` symlink check** to
> distinguish prerestore-bound from unbound. The orchestrator
> contract "destination VF is bound **only** via prerestore (or
> via the plugin itself)" is what makes the bind-state signal a
> sufficient discriminator -- a destination VF that's bound when
> the plugin's `init()` runs ��� prerestore drove the bind. See
> `vfmig_is_vf_bound()` in `criu/plugins/rdma/mlx5_sriov_vfmig/
> vfmig_restore.c` for the implementation.
>
> The prerestore binary -- when it lands -- still has a legitimate
> use for the `restored` bit: it queries QUERY_VF **before**
> driving step 7 (the bind), so it sees `restored == 0` if no LOAD
> has been staged yet, or `restored == 1` if a previous prerestore
> attempt staged a LOAD but the bind never ran. That's outside the
> scope of the post-bind soft-fallback decision.
>
> Read: ?3.1 step-7 documents `restored=1 ... persisting through
> step 7 and beyond` -- that text is correct **on the kernel cdev
> level until** the probe path runs `consume_restored`. In
> practice the probe runs synchronously inside the `bind` write,
> so for any post-bind userspace reader the bit is gone. The
> `loaded/unloaded` model in ?3 is still the correct mental model
> -- we just learn the binary "is this VF loaded?" answer from
> sysfs rather than from QUERY_VF.

### 3.1 The existing surface

`include/uapi/linux/mlx5_vfmig.h`:

```c
struct mlx5_vfmig_query_vf {
    __u32 vf_id;     /* in  */
    __u32 num_vfs;   /* out: total VFs provisioned on this PF */
    __u16 vhca_id;   /* out: live VHCA identifier */
    __u8  restored;  /* out: 1 if MARK_RESTORED was issued */
    __u8  tracked;   /* out: 1 if SET_TRACKED { enable=1 } in effect */
};
#define MLX5_VFMIG_IOC_QUERY_VF \
    _IOWR(MLX5_VFMIG_IOC_MAGIC, 0x03, struct mlx5_vfmig_query_vf)
```

Per the destination-side LOAD lifecycle in the same header:

```
   1. sriov_drivers_autoprobe = 0 on the PF
   2. sriov_numvfs = N on the PF                  (VFs created, unbound)
   3. ioctl(MLX5_VFMIG_IOC_ENABLE_MIGRATABLE, vf_id)
   4. open /dev/mlx5_vfmig/<pf_bdf>
   5. ioctl(MLX5_VFMIG_IOC_LOAD_VHCA_STATE, vf_id) (stage blob)
   6. ioctl(MLX5_VFMIG_IOC_MARK_RESTORED, vf_id)   (sets `restored`=1)
   7. driver_override + bind the VF                (probe applies LOAD)
```

After step 6 -- and persisting through step 7 and beyond, until
explicit VF teardown -- `MLX5_VFMIG_IOC_QUERY_VF.restored == 1`.
That is exactly the binary "this VF has had its FW state
applied (or is about to, on next probe) by someone other than
me" indicator the CRIU plugin's `init()` needs to branch on.

### 3.2 How the prerestore binary and CRIU restore use it

The orchestrator's flow (independent of CRIU):

```
1. provision SR-IOV: sriov_drivers_autoprobe=0, sriov_numvfs=N
2. ioctl(MLX5_VFMIG_IOC_ENABLE_MIGRATABLE, vf_id)
3. ioctl(MLX5_VFMIG_IOC_SET_TRACKED, { vf_id, enable=1 })
4. ioctl(MLX5_VFMIG_IOC_SET_VF_UUID, { vf_id, uuid=U })   <--- KS7.3 ask
5. driver_override + bind the VF -> ENABLE_HCA latches the
   migratable bit
6. workload runs, eventually CRIU dumps it (capturing U into
   the plugin image as the VF's identity)
```

(Step 4 is the only piece that's new; steps 1-3 + 5 are the
existing destination-side lifecycle from `mlx5_vfmig.h`.)

The prerestore binary's flow on the destination host
(after the orchestrator has already done steps 1-4 there):

```
1. open dump image, parse plugin blob, build list of
   (vf_uuid, vf_id, blob_path, blob_size) entries.
2. for each entry:
     scan /dev/mlx5_vfmig/* for a VF whose
     QUERY_VF.vf_uuid == entry.vf_uuid;
     if no match           -> refuse with clear error
                              ("no VF with uuid X found ...");
     if match.vf_id != entry.vf_id
                            -> refuse with distinct error
                              ("found UUID X on vf_id=Z but
                                image dumped from vf_id=Y;
                                see ?3.5.3.1");
3. for each matched (PF, vf_id):
     run the destination LOAD lifecycle steps 5-7:
       ioctl(MLX5_VFMIG_IOC_LOAD_VHCA_STATE, vf_id)
         -> write blob, close fd
       ioctl(MLX5_VFMIG_IOC_MARK_RESTORED, vf_id)
       driver_override + bind the VF
4. exit; netdev / ibdev are up, operator runs ARP-pin,
   then `criu restore`.
```

The CRIU plugin's `init()` flow at restore time:

```
1. for each entry in dump's vfmig list:
     scan /dev/mlx5_vfmig/* PFs for a VF whose
     QUERY_VF.vf_uuid == entry.vf_uuid;
     if no match           -> refuse with clear error
                              ("orchestrator must provision
                               destination VF with matching
                               uuid before running restore");
     if match.vf_id != entry.vf_id
                            -> refuse with distinct error
                              ("orchestrator stamped UUID on
                               wrong slot; see ?3.5.3.1");
     (matched_pf, matched_vf_id) = match.

2. for each matched (matched_pf, matched_vf_id):
     vf_bdf = readlink("/sys/bus/pci/devices/<matched_pf>/virtfn<matched_vf_id>");
     /*
      * Soft-fallback signal: is the destination VF bound to a
      * driver? See ?3 implementation note -- QUERY_VF.restored
      * is transient (consumed at probe), so we use the sysfs
      * driver-symlink check instead.
      */
     if exists("/sys/bus/pci/devices/<vf_bdf>/driver"):
         /* prerestore binary already drove LOAD + bind; skip. */
         log_prerestore_detected(entry, matched_pf, matched_vf_id);
     else:
         /* monolithic fallback: orchestrator stamped UUID but
          * nobody ran prerestore. Drive LOAD + bind inline against
          * the matched VF. */
         log_prerestore_not_run(entry, matched_pf, matched_vf_id);
         apply_load_vhca_state(matched_pf, matched_vf_id, entry);
         driver_override_and_bind(vf_bdf);

3. proceed to PD/CQ/MR/QP restore against the matched VF tuple.
```

Both flows do **only reads** of `vf_uuid`. Neither calls
`SET_VF_UUID`. The orchestrator owns the UUID; CRIU finds the
VF by `(vf_uuid, vf_id)` and refuses if either half of the
pair disagrees with the image.

The `vf_uuid` and `vhca_id` are returned in the same QUERY_VF
roundtrip (KS7.3 lives on the surface), so the match step is a
single ioctl per VF iterated; the prerestore-vs-monolithic
decision is then a sysfs `lstat()` per matched VF (see ?3
implementation note for why we don't use QUERY_VF.restored).

The cross-PF iteration is the user's "check all UUIDs on all
relevant PFs for a match" pattern -- it lets the orchestrator
park the destination VF on any PF, and CRIU still finds it at
restore time without needing the dump to record the
destination's PF BDF (which it can't know at dump time
anyway).

### 3.3 PF cdev surface vs the originally-proposed sysfs node

An earlier draft of this doc proposed a new sysfs hierarchy
under `/sys/bus/pci/devices/<pf>/vfmig/vf%u/state`. Two
problems with that surface:

* **Per-VF subdirectory existence is fragile pre-LOAD.** The
  VF kobject under the PF's pci_dev isn't always populated
  before the VF has been touched by mlx5_core. A consumer that
  needs to read state PRE-bind (which is exactly what the CRIU
  plugin does to decide whether to bind the VF itself) can't
  rely on the per-VF subdir being there yet.
* **The discoverable per-VF attributes don't help on their
  own.** `QUERY_VF` already exposes `vhca_id` and the binary
  `restored` flag. Neither is sufficient as a save-restore
  identity: `vhca_id` is destination-allocated and not stable
  across SAVE/LOAD (?3.5); `restored=1` is monotonic but says
  nothing about *which dump* the LOAD applied. Adding a
  sysfs path that exposes the same fields through a less-
  flexible discovery model would just duplicate surface
  without solving the identity problem.

The PF cdev path resolves the surface question (the cdev
exists per-PF, iteration via `0..num_vfs-1` covers any VF
count without relying on per-VF kobject hierarchies). The
*identity* question is solved separately by KS7.3's
orchestrator-owned `vf_uuid` extension to QUERY_VF,
described in ?3.5.

### 3.4 Future relaxation: per-image / per-uobject tracking

The two-state `unloaded` / `loaded` model intentionally does
not distinguish "loaded but no process has restored its
uobjects yet" from "loaded and fully in use". For the v0
single-process workload that's enough; the prerestore binary
is the one that drives the LOAD, and the CRIU plugin's
init() is the one that drives uobject restore.

For a future multi-process / multi-image workload (where one
VF is referenced by multiple CRIU images and only some images
have been restored at any given moment), the kernel-side flag
alone is insufficient: you'd need the CRIU images themselves
to track which uobjects have been restored, and you'd need
the orchestrator to enforce a "restore-in-reverse-saved" order
(see ?6.5).

The kernel surface is forward-compatible with that:

* `struct mlx5_vfmig_query_vf` is a fixed-size struct that
  ships under a single ioctl number. Adding a new field
  (e.g. `__u8 uobjects_restored_count` or similar) bumps the
  struct size, which bumps the encoded ioctl number, which
  cleanly fails old userspace with `-ENOTTY` rather than
  reading misaligned bytes (this is the same ABI pattern the
  existing `MLX5_VFMIG_IOC_QUERY_QP` already uses; see the
  "ABI note" doc-comment in mlx5_vfmig.h).
* Adding new state bits doesn't require a new ioctl number,
  just a recompile of in-tree consumers (CRIU's plugin, the
  test tooling) against the updated header.

So nothing about the v0 contract closes the door on
later-added per-uobject restore-completeness tracking. The
v0 contract just doesn't need it.

### 3.5 KS7.3 -- orchestrator-owned per-VF UUID (LANDED)

> **Status (2026-06-08): LANDED.** Kernel-side surface frozen
> as described in ?3.5.4. Storage is `uuid_t vf_uuid` on
> `struct mlx5_vf_context` (using the kernel's
> `<linux/uuid.h>` helpers: `uuid_is_null`, `uuid_equal`,
> `uuid_copy`, `import_uuid`, `export_uuid`); write site is the new
> `MLX5_VFMIG_IOC_SET_VF_UUID` ioctl 0x12 on the PF cdev;
> read site is the existing `MLX5_VFMIG_IOC_QUERY_VF`
> (now extended with a 16-byte `vf_uuid` + 8-byte
> `reserved_out` tail; size grows so the encoded ioctl
> number bumps and old userspace gets `-ENOTTY` instead
> of a misaligned read, same ABI pattern as the earlier
> QUERY_QP grow). Cleared on SR-IOV teardown
> (sriov_numvfs=0) so a recycled slot starts fresh. The
> kernel-matrix C probe lives at
> `tools/testing/criu_rdma/uobject_restore/vf_uuid/vf_uuid_probe_mlx5_vfmig`
> and the lifecycle / multi-VF shell harness at
> `tools/testing/criu_rdma/uobject_restore/vf_uuid/test_vf_uuid_lifecycle.sh`.
> Both build via the standard
> `make -C tools/testing/criu_rdma`. CRIU plugin work
> picks up at ?3.5.3 (the contract).

#### 3.5.1 Why `vhca_id` is the wrong identifier

The earlier draft of this design used `vhca_id` to detect
"prerestored with the wrong dump". That doesn't work:

* `vhca_id` is allocated per-PF on whichever host the VHCA
  currently lives on. SAVE on host A captures the source's
  blob (an opaque firmware-state byte stream); LOAD on host B
  applies the blob into a destination VHCA whose `vhca_id`
  came from host B's PF allocator. The two `vhca_id`s have
  no relationship to each other.

* The SAVE blob does **not** encode the source's `vhca_id`.
  The 16-byte FW_DATA record header `(record_size, flags,
  tag)` is identical across all blobs; the rest is opaque
  firmware state with no source-identity tag.

* Both the existing vfio mlx5 LM driver
  (`drivers/vfio/pci/mlx5/cmd.c`, `mlx5vf_cmd_get_vhca_id`)
  and our PF-driven `mlx5_core/vfmig/vfmig.c`
  (`vfmig_query_vhca_id`) query `vhca_id` from the **local**
  device on bind. SAVE_VHCA_STATE / LOAD_VHCA_STATE are
  parameterized by that local `vhca_id` -- both source and
  destination kernels use *their own* `vhca_id` to address
  *their own* VHCA. There's no source-vs-destination compare
  anywhere.

* Even on a SAME-host dump-restore, tearing down SR-IOV
  (`sriov_numvfs=0`) and re-creating it (`sriov_numvfs=N`)
  yields different `vhca_id`s for the same `vf_id` slot --
  the FW reuses the slot but assigns a fresh `vhca_id` from
  its allocator. So even "VHCA on the same physical PCI
  function" doesn't preserve `vhca_id`.

So a `vhca_id` mismatch check would either:

* Always fire (different host -> different `vhca_id`,
  refuse), making the prerestore-binary path unusable for
  the actual cross-host migration use case.
* Match by coincidence (allocator state happens to align
  on dump-restore-on-same-host), making the check unsafe.

Neither outcome is acceptable.

#### 3.5.2 Why the orchestrator must own the identity tag

SR-IOV LM today (vfio mlx5 + libvirt / kubevirt) trusts the
**orchestrator** to pick the right `(target_pf, target_vf_id)`
and apply the right blob. There's no firmware-side identity
check on LOAD: the FW will accept any blob into any VHCA if
the IOMMU/IOVA layout matches. Mismatched IOVAs surface as
a 60-second cmd-ring timeout (per the
`MLX5_VFMIG_IOC_LOAD_VHCA_STATE` doc-comment about "post-LOAD
the destination VHCA's cmd ring is dead"). That's not a
check; it's a cliff.

CRIU needs an in-band identity check, but **CRIU is the wrong
component to own the identity tag.** The orchestrator already
knows end-to-end which workload runs on which VF: it
provisioned the VF, bound the workload to it, picked a target
VF on the destination, and migrated the VF state. A v0 design
that has CRIU mint UUIDs at dump time forces the orchestrator
to "discover" the UUID by reading the dump back out, then
re-stamp it on the destination VF -- needlessly indirect, and
breaks the model where the orchestrator can pre-arrange the
identity tag before any CRIU dump even runs.

The right shape:

* The **orchestrator** assigns a UUID to each migration-eligible
  VF when it provisions the VF (the natural pairing is with
  `MLX5_VFMIG_IOC_SET_TRACKED { enable=1 }`, which the
  orchestrator already calls before binding the workload).
* The orchestrator stamps the **same** UUID on the matching
  destination VF before kicking off restore. The orchestrator
  is the only component that has all the information to do
  that mapping.
* CRIU's role is purely passive: at dump time it reads the
  source VF's UUID via `MLX5_VFMIG_IOC_QUERY_VF` and stores
  it in the plugin image; at restore time it iterates VFs on
  candidate PFs, finds the one whose UUID matches the
  dump's, and validates that everything else lines up.

This inverts the responsibility from "CRIU mints, orchestrator
follows" to "orchestrator mints, CRIU follows", which is the
right division of labour: the orchestrator owns the workload
��� VF binding, CRIU owns the dump artifact.

#### 3.5.3 The contract

* **Orchestrator** (provisioning):
  - On both source and destination hosts, before any workload
    binds the VF: `SET_VF_UUID(vf_id, uuid)`.
  - The orchestrator picks the UUID. RFC 4122 v4 random is
    suggested but not enforced; any non-zero 16-byte value
    that the orchestrator commits to as a stable identity
    works.
  - The same logical workload gets the same UUID stamped on
    both the source VF and the destination VF.
  - **The destination VF's `vf_id` MUST equal the source
    VF's `vf_id`** (see ?3.5.3.1 for the rationale). The
    orchestrator may pick any PF on the destination host;
    cross-PF migration is fine. But the per-PF `vf_id` slot
    must match end-to-end. So the contract is "same
    `vf_id`, any PF, any host", not the looser "same UUID,
    any slot, any PF, any host".

* **CRIU dump path** (passive read):
  - At dump time, `vfmig_capture_one_vf` calls
    `MLX5_VFMIG_IOC_QUERY_VF` and reads `query.vf_uuid` along
    with the existing `vhca_id` / `tracked` / etc.
  - **Refuse to dump if `vf_uuid` is all-zeros.** That means
    the orchestrator hasn't tagged this VF and CRIU has no
    safe way to bind the resulting dump to a specific VF on
    restore.
  - The plugin image stores `vf_uuid` per-VF entry alongside
    the SAVE blob path, blob size, and (diagnostic) source
    `vhca_id`. The image also records `vf_id` (it always
    has, as a diagnostic field); under this contract that
    `vf_id` is also part of the identity check on restore.

* **CRIU restore path** (passive read + paired match):
  - Plugin `init()` (and the prerestore binary) iterate
    eligible PFs (anything under `/dev/mlx5_vfmig/`),
    iterate `0..num_vfs-1` on each, call QUERY_VF, look for
    `query.vf_uuid == image.vf_uuid`. First match wins.
  - **After a UUID match, also verify
    `query.vf_id == image.vf_id`.** If they disagree --
    UUID hit but on a different slot than the image was
    dumped from -- refuse with a *distinct* error pointing
    at ?3.5.3.1: "found UUID X on (pf=..., vf_id=Z) but
    image was dumped from vf_id=Y; orchestrator must
    provision matching slot on destination". This is a
    different failure mode from "no UUID match at all":
    the operator has stamped the right workload identity,
    just on the wrong slot, and needs to fix the
    destination `sriov_numvfs` / `SET_VF_UUID` call. CRIU
    does NOT silently coerce the LOAD onto the slot the
    orchestrator picked, because cross-slot LOAD is not
    supported (see ?3.5.3.1).
  - **No write surface from CRIU.** Neither `init()` nor
    the prerestore binary calls `SET_VF_UUID`. If no UUID
    match is found at all, refuse with a clear error: "no
    VF with uuid X found on any vfmig-eligible PF; ensure
    the orchestrator has provisioned the destination VF
    with the matching UUID before running restore".
  - The restore-side identity check is the
    `(vf_uuid, vf_id)` pair-match described above. There
    is no fallback to UUID-only matching, no "if image.
    vf_id != dest.vf_id, coerce" path. That fallback would
    re-introduce the cross-slot LOAD failure mode ?3.5.3.1
    explicitly avoids.

The match is by exact 16-byte UUID compare plus exact
`vf_id` agreement. With the orchestrator controlling
allocation, exotic cases like "two VFs with the same UUID
on the same host" are an orchestrator bug, not a CRIU
concern. The kernel-side `SET_VF_UUID` does NOT enforce
host-wide uniqueness for the same reason -- enforcement
would require cross-PF coordination the kernel doesn't have
a good place for. Documented as the orchestrator's invariant.

##### 3.5.3.1 Why same-`vf_id` is required

The kernel computes each VF's per-VF IOVA window
deterministically from `vf_id` (see
`drivers/.../mlx5/core/vfmig/vfmig_iova.c:vfmig_iova_domain_create`):

```c
base = VFMIG_IOVA_BASE + (u64)vf_id * VFMIG_IOVA_PER_VF;
```

`VFMIG_IOVA_BASE` and `VFMIG_IOVA_PER_VF` are kernel
compile-time constants; `vf_id` is the only variable. The
slot grid (8 deterministic slots, plus the kcoherent and
transient sub-arenas) is byte-identical across hosts for a
given `vf_id`, which is what makes the saved IOVA replay
log portable: the destination computes the same `iova_slot`
from the same IOVA, and the cross-check at
`vfmig_iova_install_replay` (`vfmig_iova.c:1378` ish, the
"wire claims slot N for IOVA X but destination partitioning
maps it to slot M" warn) just passes. If the destination's
`vf_id` differs, every replayed record's claimed slot
disagrees with the destination's slot grid and `LOAD` aborts
with the slot-mismatch warn. We confirmed this empirically
on 2026-06-10: a deliberate cross-slot smoke (source
`vf_id=0`, destination `vf_id=1`, same UUID) fails at
`write(load_fd) -> -EINVAL` with the expected dmesg.

The IOVA layer is not the only `vf_id`-keyed dependency.
The FW E-Switch maps each VF to a `vport_num` (typically
`vf_id + 1`), and the FW's saved QPC / FDB / etc. references
that `vport_num` internally. Same-`vf_id` ��� same-`vport_num`
��� FW state references stay valid post-LOAD without rewrites.
A general cross-slot LOAD would also need vport rewriting,
which is materially harder and deeper than just relaxing
the IOVA cross-check; we deliberately don't pursue it.

The same-`vf_id` constraint is therefore not a workaround
for a kernel bug; it's the natural shape of the kernel/FW
model. The orchestrator already owns where each workload's
VF lives, so "pick a destination slot that matches the
source slot" is within its existing responsibility scope.

Forward-look: if a future kernel + FW pair ever grows
cross-slot LOAD support (relaxed IOVA-replay slot grid +
vport rewriting + post-LOAD audit), this constraint can
be relaxed CRIU-side by dropping the `vf_id` equality
check. As of v0 it's a hard contract, hard-refuse. We
considered an explicit kernel ask for cross-slot LOAD
(provisional name "KS7.5") and decided against it: the
orchestrator-side same-slot constraint is cheap and
clean, the kernel work is large, and we have no
production motivation to spend that complexity yet.

#### 3.5.4 Kernel-side surface (the actual ask)

Two changes to `include/uapi/linux/mlx5_vfmig.h`:

```c
/*
 * Extend mlx5_vfmig_query_vf with vf_uuid. The struct grows;
 * the encoded ioctl number bumps with the new sizeof, so old
 * userspace built against the smaller struct gets -ENOTTY rather
 * than a misaligned read. (Same ABI pattern as MLX5_VFMIG_IOC_QUERY_QP.)
 */
struct mlx5_vfmig_query_vf {
    __u32 vf_id;       /* in  */
    __u32 num_vfs;     /* out */
    __u16 vhca_id;     /* out (RUNTIME-ONLY; not stable across SAVE/LOAD) */
    __u8  restored;    /* out */
    __u8  tracked;     /* out */
    __u8  vf_uuid[16]; /* out: zero if SET_VF_UUID has not been
                        *      issued for this VF since teardown;
                        *      otherwise the 16-byte UUID the
                        *      orchestrator stamped on this VF.
                        */
    __u8  reserved_out[8];
};

/*
 * MLX5_VFMIG_IOC_SET_VF_UUID:
 *   Stamp the orchestrator's 16-byte UUID into the VF's per-VF
 *   context slot. Intended to be called by the orchestrator
 *   exactly once per VF, between provisioning the VF
 *   (sriov_numvfs=N + ENABLE_MIGRATABLE + SET_TRACKED) and
 *   binding any workload to it. Idempotent if the same UUID is
 *   set twice. Returns -EBUSY if a different UUID is already
 *   set (defends against accidental cross-workload reuse of a
 *   VF slot). The UUID is cleared with the rest of the per-VF
 *   context on VF teardown (sriov_numvfs=0, or PF unload).
 *
 *   CRIU NEVER calls this ioctl. Both the dump path and the
 *   restore path (including the prerestore binary) only READ
 *   vf_uuid via MLX5_VFMIG_IOC_QUERY_VF. The kernel does not
 *   enforce that contract -- root-owned userspace can call
 *   either ioctl from anywhere -- but documenting the split
 *   keeps the ownership model clear.
 *
 *   Returns 0 on success, -EINVAL if vf_id is out of range or
 *   uuid is all zeros (we treat all-zeros as "unset" and reject
 *   it as a write so userspace can't accidentally stamp a no-op
 *   UUID), -EBUSY per above, -ENODEV if the PF is gone.
 */
struct mlx5_vfmig_set_vf_uuid {
    __u32 vf_id;
    __u32 reserved;
    __u8  vf_uuid[16];
};
#define MLX5_VFMIG_IOC_SET_VF_UUID \
    _IOW(MLX5_VFMIG_IOC_MAGIC, 0x13, struct mlx5_vfmig_set_vf_uuid)
```

Storage is a single `uuid_t vf_uuid` field on the existing
per-VF context (`mlx5_core_sriov.vfs_ctx[vf_id]`, alongside
`restored` / `restored_vhca_id`). The driver reaches the field
through the standard `<linux/uuid.h>` helpers
(`uuid_is_null` / `uuid_equal` / `uuid_copy` / `import_uuid`
/ `export_uuid`); the UAPI struct stays as `__u8 vf_uuid[16]`
because `uuid_t` is a kernel-internal typedef that is not
exposed via `<uapi/linux/uuid.h>`. One write site
(`vfmig_ioc_set_vf_uuid`), one read site
(`vfmig_ioc_query_vf`'s extended return), zero impact on any
fast path.

#### 3.5.5 Lifecycle

| event                                            | vf_uuid value                            |
|---|---|
| Initial boot / SR-IOV enable (`sriov_numvfs=N`)  | all-zeros (unset)                        |
| `SET_VF_UUID(vf_id, U)` first call (orchestrator)| `U`                                      |
| `SET_VF_UUID(vf_id, U)` same UUID (orchestrator) | `U` (idempotent, returns 0)              |
| `SET_VF_UUID(vf_id, V)` other UUID               | unchanged; returns -EBUSY                |
| `sriov_numvfs=0` / PF unload                     | all-zeros (slot torn down)               |

The "set once until teardown" semantics defend against the
orchestrator accidentally re-tagging a VF that's still bound
to a workload. A legitimate workflow that wants to repurpose
a `vf_id` slot for a *different* workload identity must go
through `sriov_numvfs=0 -> sriov_numvfs=N` first.

The clear-on-`sriov_numvfs=0` step in the lifecycle table is
implemented by `mlx5_vfmig_pf_drop_vf_uuids()` (called from
`mlx5_device_disable_sriov`), because `sriov->vfs_ctx[]`
itself survives the cycle -- the array is allocated at PF
bind and freed at PF unbind, not on each `sriov_numvfs`
write. Without the explicit memset hook, a stale UUID would
survive into the next generation and the orchestrator's
fresh `SET_VF_UUID` would `-EBUSY` with no in-kernel clear
path short of PF unload/reload.

##### 3.5.5.1 Empirical multi-LOAD validation

The hook coexists with three independent gates that
together pin down what "multiple `LOAD_VHCA_STATE`
invocations on the same VHCA" means in practice. All four
layers were exercised on this rig (single-host, native
mlx5_core, IOMMU + deterministic-IOVA tracking) on
2026-06-09:

| layer                                                                                                              | what the kernel does                                                                                                                          | empirical status |
|---|---|---|
| **Stage gate (per-fd)** -- `vfmig_vf_id_busy_locked` (`drivers/.../vfmig.c` line 4199)                             | A second `MLX5_VFMIG_IOC_LOAD_VHCA_STATE` ioctl on the same `vf_id` while the first `load_fd` is still open returns `-EBUSY`.                 | **Tested.** `save_load/multi_load_gates/multi_load_stage_gate_probe` (4 cells: open/close, concurrent-fd `-EBUSY`, gate-clears-on-close, per-vf_id isolation). Pure ioctl path; no FW interaction. |
| **Install gate (per-pending_load)** -- `vfmig_install_pending_load_locked` (`drivers/.../vfmig.c` line 3789)        | After a successful first stage installs `vfs_ctx[vf_id].vfmig_pending_load`, a second stage's release path emits `-EBUSY` to the dmesg warning channel and discards the second blob. | **Code-verified.** Not separately probed because in practice the IOVA replay gate (next row) fires *first* on the second `write()` of HOST_PAGE records and aborts the second LOAD before its release path runs. The install gate is the safety net for the small window between "apply path takes the slot" and "userspace closes the load_fd"; that race is hard to manufacture cleanly without a second IOVA domain, which a single VHCA does not have. |
| **IOVA replay gate (`drift_armed`)** -- `vfmig_iova_replay_page` (`drivers/.../vfmig_iova.c` line 1397)              | The first LOAD's parser arms `dom->drift_armed=1` after parsing all HOST_PAGE records. Any subsequent `replay_page` call (i.e. a second LOAD on the same domain) hits `WARN_ON_ONCE(dom->drift_armed)` and returns `-EBUSY`, which the parser surfaces as `-EINVAL`. | **Tested.** Reproduced on this rig: full SAVE ��� LOAD ��� bind ��� unbind ��� second LOAD on the same `vf_id` aborts at the first HOST_PAGE record with `replay_page(slot=1 ...) failed: -16` followed by `-22` in dmesg, plus the `WARN: dom->drift_armed` taint at `vfmig_iova_replay_page+0x1cc`. The first LOAD's IOVA installations stay live; the kernel refuses to grow `expected_count[slot]` after the source's footprint has been declared. |
| **Apply path (FW)** -- `mlx5_vfmig_vf_apply_pending_load` (`drivers/.../vfmig.c` line 5822)                          | Pops `vfmig_pending_load`, walks SUSPEND_INITIATOR ��� SUSPEND_RESPONDER ��� `LOAD_VHCA_STATE` ��� RESUME_RESPONDER ��� RESUME_INITIATOR on the FW.    | **Tested for the cycle path.** A SAVE ��� LOAD ��� bind sequence, followed by `sriov_numvfs=0` / `sriov_numvfs=1` / SAVE ��� LOAD ��� bind on the same PF, completes end-to-end with `mlx5_core/mlx5e` reaching the same `vhca_id` and MAC both times. The test driver is `save_load/test_iova_tracked_save_load.sh`. Whether FW would accept a *non-cycle* second LOAD is moot: the IOVA replay gate above blocks it before any FW command issues. |

The architectural takeaway:

* **Slot repurposing across `sriov_numvfs` cycles is the
  validated path.** End-to-end on this rig, twice in a
  row, vhca_id and MAC preserved both rounds.
  `mlx5_sriov_disable()` drops the per-VF IOVA domain, so
  the next `sriov_numvfs=1` allocates a fresh
  `drift_armed=0` domain. This is exactly the workflow
  the `mlx5_vfmig_pf_drop_vf_uuids` hook supports, and it
  is what KS7.3 was designed for.
* **Multi-LOAD on the same VHCA without a cycle is
  blocked by the kernel's IOVA replay gate, not by FW.**
  The first LOAD arms `dom->drift_armed`; any later
  HOST_PAGE replay on that domain trips the WARN and
  returns `-EBUSY`. `LOAD_VHCA_STATE` is never issued, so
  the firmware's behaviour on a "DISABLE_HCA ���
  ENABLE_HCA ��� second LOAD" sequence remains unknown
  *and irrelevant* to the in-tree contract: the kernel's
  position is that re-loading without a cycle would
  break the deterministic-IOVA invariant (allocations
  added between SAVE and second LOAD would map to slots
  the source never declared, and those would alias the
  replayed source ranges if the replay were allowed to
  grow `expected_count`), so the gate is correct as
  written.

If a future consumer needs to LOAD twice without a
cycle, the kernel-side change required is *not* lifting
the install gate or the stage gate -- it is a redesign of
the IOVA registry to support a "discard-and-reseed"
operation that retires the first LOAD's drift expectations
before accepting the second. Today's invariant is
intentional and the gate's WARN is the right shape: it
fires once in dmesg per offending sequence, fails the
ioctl cleanly, and leaves the first-LOAD'd VHCA
intact (validated above by clean recovery via
`sriov_numvfs=0` cycle).

Cross-host migration is unaffected: source host's slot
keeps its UUID until source teardown; destination host's
slot gets the UUID stamped fresh by the orchestrator
before workload bind on the dest. The same orchestrator
that performs the migration also issues the matching
`SET_VF_UUID` on the dest, so identity-tag continuity is
trivially preserved.

#### 3.5.6 Authorization

Same as the rest of `MLX5_VFMIG_IOC_*`: the cdev is root-only
via udev rules; the cdev FD is the authorization gate. No
new authorization model.

#### 3.5.7 Why not store the UUID purely userspace-side

A purely-userspace alternative would be a sidecar file like
`/run/orchestrator/vfmig/<pf_bdf>/<vf_id>.uuid` that the
orchestrator writes and CRIU reads. Considered and rejected
because:

* Cleanup correctness depends on tmpfs lifecycle / explicit
  unlink. A reboot between provisioning and `criu restore`
  loses the file (probably acceptable since SR-IOV state
  also doesn't survive reboot, but creates an extra
  invariant to maintain).
* No cross-process serialization. Two orchestrator helpers
  racing to stamp the same `vf_id` would race on the file;
  the kernel's per-VF mutex on `SET_VF_UUID` gives us
  natural serialization for free.
* The UUID is conceptually as kernel-bound as `tracked` --
  it represents an attribute the kernel has stamped on the
  VHCA -- so keeping the two together avoids consistency
  questions ("what does it mean for tracked=1 but uuid is
  missing?").

The kernel side is so small (one `u8[16]` field, one
write-only ioctl, one read in QUERY_VF) that the ergonomic
win of in-kernel storage is worth it.

## 4. KS7.2 -- `MLX5_VFMIG_IOC_REFRESH_AV_DMAC` (~~LANDED~~ -- **REVERTED**, historical)

> **Updated 2026-06-06: REVERTED.** The ioctl this section
> documents has been removed from the tree. End-to-end testing
> with the ioctl in place AND neighbor entries pinned BEFORE
> the ioctl was fired reproduced the FW reject documented in
> [`qp_av_dmac_swap.md` ?12](qp_av_dmac_swap.md#12-post-mortem-post-rtr-primary-av-refresh-is-not-supported-on-mlx5--cx-7-28x):
> `op_status=0xffffffea` (`-EINVAL`),
> `op_syndrome=0x00498c8b`. The follow-up Patch B
> (`66edef3f790f` / `fe188a601af9`) that mirrored a fully-formed
> `primary_address_path` from a pre-op `QUERY_QP` did not
> change the verdict, ruling out modify-blob internal
> consistency as the rejection cause.
>
> Structural confirmation: mlx5_ib's own `opt_mask[cur][new][qp_type]`
> allowlist (the table `__mlx5_ib_modify_qp` AND-masks the
> user-requested optpar with) excludes
> `MLX5_QP_OPTPAR_PRIMARY_ADDR_PATH` from EVERY state-pair
> cell. The verbs layer's `ib_modify_qp_is_ok` independently
> rejects `IB_QP_AV` at RTS for RC. Both gates agree with the
> firmware: post-RTR primary-AV refresh is not a supported
> operation on this driver / FW combination.
>
> The ?S6b dmac-correction work has moved to **KS7.4** below
> (per-VF identity migration via `ip link set vf mac` + `ip
> addr` + `ip neigh`, run by the orchestrator before
> `LOAD_VHCA_STATE`; empirically validated 2026-06-08 -- see
> `qp_av_dmac_swap.md` STATUS banner / ?0 / ?1 / ?2). The
> remainder of this ?4 is preserved as a historical record of
> the as-tried ioctl shape; the surfaces it documents (ioctl
> `0x12`, `struct mlx5_vfmig_refresh_av_dmac`,
> `mlx5_vfmig refresh_av_dmac` CLI verb, `refresh_av_dmac.sh`
> wrapper) **are no longer in the tree**.

~~Landed in:~~ (reverted)

* ~~`b70b6624084a` -- `mlx5_vfmig: ?S6b add userspace-triggered av.dmac refresh ioctl`~~
* ~~`1bbe576bc7c5` -- `mlx5_vfmig: ?S6b CLI verb + harness wrapper for av.dmac refresh ioctl`~~
* ~~`09d7fd51dc99` -- design notes in `qp_av_dmac_swap.md` ?11~~ (also retracted; see qp_av_dmac_swap.md ?11 banner + ?12)

This section captures the as-built shape **as it was before
revert**, for the historical record. The current tree has none
of these surfaces; KS7.4 (?4.6 below) is the canonical owner
of the ?S6b dmac-correction problem going forward, and is now
purely orchestrator-side (no kernel surface).

### 4.1 Use cases

1. **End-to-end datapath verification while CRIU-side
   prerestore-binary work is in progress.** The harness can
   `criu restore` -> pin ARP -> ioctl-refresh -> resume
   traffic. This validates the ?S6b in-restore helper's logic
   with the current ARP table independent of any CRIU-side
   plumbing, unblocking the dmac-stale fix verification on the
   primary v0 swap workload.

2. **Recovery path** when the operator's orchestration drifts.
   If the prerestore binary's ARP-pin step raced and the
   in-restore helper hit `-ENOENT` even on the prerestore
   path, the operator pins and refreshes manually -- no need
   to redo the whole CRIU restore.

3. **Future** non-vfmig dmac-refresh consumers (e.g. graceful
   handover after netdev MAC change without a full QP destroy).
   Out of scope for v0 but the ioctl shape leaves room.

### 4.2 As-built UAPI

`include/uapi/linux/mlx5_vfmig.h`:

```c
struct mlx5_vfmig_refresh_av_dmac {
    __u32 vf_id;                 /* in:  target VF on this PF */
    __u32 qpn;                   /* in:  FW QP number to refresh
                                  *      (24 bits significant) */
    __u8  reserved_in[16];       /* in:  must be 0 */

    /* pre-op QUERY_QP */
    __u32 pre_query_status;      /* out: 0 = QPC found, !=0 = FW reject */
    __u32 pre_query_syndrome;
    __u8  pre_qpc_state;         /* RST=0/INIT=1/RTR=2/RTS=3/SQEr=4/SQD=5/ERR=6 */
    __u8  pre_vhca_port_num;
    __u8  reserved_pre0[2];
    __u32 pre_sgid_index;
    __u8  pre_dmac[6];           /* qpc.path.rmac_47_32 || rmac_31_0 */
    __u8  reserved_pre1[2];
    __u8  pre_dgid[16];          /* qpc.path.rgid_rip raw GID */

    /* neighbor lookup against the VF's destination netdev */
    __s32 lookup_status;         /* 0 / -ENOENT / -EAGAIN / -ENODEV */
    __u8  resolved_dmac[6];      /* peer MAC from neigh table */
    __u8  dmac_changed;          /* 0 = exact match, no MODIFY_QP issued */
    __u8  reserved_lookup;

    /* op MODIFY_QP(RTS2RTS_QP, opt_param_mask=PRIMARY_ADDR_PATH);
     * only meaningful when @dmac_changed == 1 */
    __u32 op_status;
    __u32 op_syndrome;

    /* post-op QUERY_QP; only meaningful when @dmac_changed == 1 */
    __u32 post_query_status;
    __u32 post_query_syndrome;
    __u8  post_qpc_state;        /* should equal @pre_qpc_state */
    __u8  reserved_post0;
    __u8  post_dmac[6];          /* should equal @resolved_dmac */

    __u8  reserved_out[16];
};

#define MLX5_VFMIG_IOC_REFRESH_AV_DMAC \
    _IOWR(MLX5_VFMIG_IOC_MAGIC, 0x12, struct mlx5_vfmig_refresh_av_dmac)
```

### 4.3 Behavior (as shipped)

* Pre-op QUERY_QP snapshots `qpc.state`, `qpc.path.{vhca_port_num,
  src_addr_index, rgid_rip, rmac_*}` into the pre_* output
  fields.
* Resolves the destination netdev for the VF via
  `mlx5_uplink_netdev_get` on the bound VF mdev, then issues
  `neigh_lookup` against the IPv4 / IPv6 GID. Cache-only, no
  ARP/NS solicit. Missing entry -> `lookup_status = -ENOENT`;
  not-NUD_VALID -> `lookup_status = -EAGAIN`.
* If `resolved_dmac == pre_dmac`, sets `dmac_changed = 0` and
  returns success without touching the QP. (VM live-migration
  case where the source-preserved dmac is still valid.)
* Otherwise issues `MODIFY_QP(RTS2RTS_QP,
  opt_param_mask=PRIMARY_ADDR_PATH)` with a qpc blob carrying
  only the new `path.rmac_*`. opt_param_mask gates which
  subfields FW consumes; PSN / dest_qpn / sgid_index / etc.
  stay untouched. `op_status` / `op_syndrome` capture the FW
  result.
* Post-op QUERY_QP snapshots the resulting QPC; caller can
  verify `post_dmac == resolved_dmac` on success.

The op step uses `uid=0` (host-privileged); the QPC's
owning-uid was wiped by LOAD_VHCA_STATE per
`pd_registration_wipe.md`, so a uid=0 MODIFY_QP against a
restored QPC's destination cmdif is the same path the always-on
`mlx5_ib_restore_qp_refresh_av_dmac` helper takes.

### 4.4 Errors

* `-EFAULT` on copy_{from,to}_user
* `-EINVAL` if `@vf_id` out of range, `@qpn` exceeds 24 bits,
  or any reserved field is non-zero
* `-ENODEV` if the VF is unbound, its mdev interface is down,
  or the VF has no associated netdev
* Any negative kernel/FW err code on cmdif transport failure
  for the bracketing QUERY_QPs

The op-under-test (MODIFY_QP) does NOT propagate its negative
errno: a FW reject is recorded in `op_status` / `op_syndrome`
and the call returns 0 so the caller can see the bracketing
post-query result.

### 4.5 Userspace consumer

The CRIU plugin won't routinely call this ioctl on the
prerestore-binary path -- the in-restore
`mlx5_ib_restore_qp_refresh_av_dmac` helper covers that case
once ARP is pinned ahead of CRIU restore. The ioctl is for
recovery + verification.

The immediate consumer is the `rdma_test_agent` framework's
post-restore step: after `pin_static_neighbor_*`, it calls the
ioctl per migrated QP via the CLI verb wrapper added in
`1bbe576bc7c5`. This validates the ?S6b helper logic
end-to-end on the v0 process-swap workload without depending
on the CRIU-side prerestore-binary plumbing. See ?9.1 for the
test path.

A future `--refresh-dmac` subcommand on the prerestore binary
itself would also be reasonable for operator convenience, but
is plugin-side work and not gated on the kernel side.

### 4.6 KS7.4 -- per-VF identity migration on the destination, pre-LOAD_VHCA_STATE

This subsection is the canonical specification for the ?S6b
dmac-correction work, replacing the reverted KS7.2 ioctl. It
lives in ?4.6 (rather than as its own top-level ?5) so the
surrounding section numbering and inbound cross-references in
?6-?10 remain stable.

KS7.4 is **not a kernel ask**. It is documentation + harness
wiring around the existing per-VF identity-migration UAPIs (`ip
link set vf mac`, `ip addr`, `ip neigh`) so that CRIU
process-swap mirrors the source-time per-VF identity onto the
destination the same way SR-IOV VM live-migration does for the
migrating VM's vNIC. With those steps in place, the
source-baked QPC is correct at t=0 and `LOAD_VHCA_STATE` /
`RESTORE_QP` work on the unmodified blob.

#### 4.6.1 Why per-VF identity migration suffices

The companion document
[`qp_av_dmac_swap.md`](qp_av_dmac_swap.md) ?0 walks the
restored QPC's `primary_address_path` field by field; the
short version is:

* **`path.rmac_*`** is the only field that goes stale on a
  symmetric process-swap. It's baked verbatim into the QPC
  at source-time `MODIFY_QP_TO_RTR` and points at the *peer's
  source-time VF MAC*. Mirror the source's per-VF MAC on the
  destination via `ip link set vf mac` and the actual peer
  on the *other* destination host now matches what
  `path.rmac_*` already says.
* **`path.rgid_rip`** (peer GID) is correct because the
  existing IP-swap step puts the peer's source-time IP on
  the destination's VF.
* **`path.src_addr_index`** is an *index* into the local
  GID table; the entry at that index matches because the
  kernel auto-populates GID entries from netdev IPs at
  `ip addr add` time, and the IPs we add mirror the
  source's.
* **`smac` is not in the QPC**; the FW resolves it on every
  send via `src_addr_index ��� GID ��� netdev ��� netdev MAC`. So
  the local NIC's *current* MAC always wins, automatically.
* All other identity-bearing fields (PSNs, dest_qpn,
  timeouts, RoCE knobs) are host-agnostic and travel
  cleanly through `LOAD_VHCA_STATE`.

The companion doc's [Appendix A ?12](qp_av_dmac_swap.md#12-post-mortem-post-rtr-primary-av-refresh-is-not-supported-on-mlx5--cx-7-28x)
captures the post-LOAD-refresh investigation in detail (FW
reject syndrome `0x00498c8b`, mlx5_ib `opt_mask` excludes
`PRIMARY_ADDR_PATH` from every state-pair cell). It is
preserved so the next person tempted by a kernel-side fix
sees the road that has been ruled out.

#### 4.6.2 Phase ordering

On each destination host, ordered:

1. **(Orchestrator)** Stamp `vf_uuid` via `MLX5_VFMIG_IOC_SET_VF_UUID`
   (KS7.3) so the prerestore binary can match the dump to a
   destination VF.
2. **(Orchestrator, pre-LOAD_VHCA_STATE)** Mirror the source's
   per-VF identity onto the destination VF. **MAC first,
   then IP, then ARP** -- GID-table entries bind `(IP, MAC)`
   at IP-add time, so changing MAC after `ip addr add`
   leaves the GID with a stale binding.

   ```bash
   ip link set <PF> vf <VF_ID> mac <source-time-peer-vf-mac>
   ip addr add <source-time-peer-vf-ip>/<prefix> dev <vf_netdev>
   ip neigh replace <source-time-local-ip> \
                    lladdr <source-time-local-vf-mac> \
                    dev <vf_netdev> nud permanent
   ```

3. **(Prerestore binary)** Read the dump's saved QP blobs
   and issue `LOAD_VHCA_STATE`. The blob is consumed
   verbatim; no patching, no FW commands, no neighbor
   lookups inside the binary's path.
4. **(CRIU plugin's `init()`, later)** Sees `restored=1`
   from QUERY_VF (KS7.1), skips its own LOAD_VHCA_STATE
   call, and proceeds straight to RESTORE_QP. RESTORE_QP
   attaches uobjects to the already-correct QPC; first
   post-restore `post_send` succeeds.

The orchestrator owns step (2) because it's a privileged
host-side operation (`ip link set vf mac` requires
CAP_NET_ADMIN on the PF host) that orchestration tools
already drive routinely as part of SR-IOV provisioning.
Folding it into the prerestore binary would push that
privilege requirement onto the binary; the orchestrator-side
split keeps the prerestore binary unprivileged-by-default.

The prerestore binary *can* sanity-check the per-VF identity
before issuing LOAD_VHCA_STATE (e.g. confirm `ip link show vf
<VF_ID>` reports the expected MAC) and refuse to load if the
orchestrator hasn't done its job. That's an operational
guardrail, not a correctness requirement; recommended but
optional for v0.

#### 4.6.3 Kernel-side surface

**None.** Every UAPI needed already exists:

* `ip link set <PF> vf <VF_ID> mac <MAC>` -- existing SR-IOV
  admin-MAC primitive; FW-enforced; no mlx5-specific hook
  needed.
* `ip addr add` / `ip addr del` -- existing netlink IFADDR;
  drives the kernel's auto-population of the RoCE GID table.
* `ip neigh replace ... nud permanent` -- existing netlink
  neigh; pins the L3->L2 mapping in the destination's ARP
  cache.

All three are routine, well-supported on mlx5, and
documented. There is **no new mlx5 ioctl, no new sysfs node,
no `MLX5_VFMIG_IOC_PATCH_SAVE_BLOB` ask**, and no extension
of `MLX5_VFMIG_IOC_LOAD_VF`. Earlier drafts of this section
sketched a "saved-blob patch list" surface; that surface is
no longer needed because the underlying problem turned out
to be host-side identity drift (which existing UAPIs
already address) rather than blob-content drift.

#### 4.6.4 Userspace consumer

* **Orchestrator / harness**: drives the three `ip` commands
  in step (2) above. In `rdma_test_agent_vfmig_criu_swap_after_qp.yaml`,
  this slots into the existing per-host prerestore phase,
  reusing the same step shape as the IP-add and ARP-pin
  steps that are already there.
* **Prerestore binary**: unchanged path -- read dump,
  verify `vf_uuid` match, optionally sanity-check that
  per-VF MAC has been set, then `LOAD_VHCA_STATE`.
* **CRIU plugin proper**: unchanged. By the time `init()`
  runs, the QPC is already loaded with the right DMAC.

Diagnostic tooling stays as-is:
[`check_qp_av_dmac.sh`](../uobject_restore/qp_av_dmac/check_qp_av_dmac.sh)
remains the canonical post-restore confirmation that
`path.rmac_*` matches the destination's resolved peer MAC;
the harness runs it after RESTORE_QP completes and expects
`VERDICT=DMAC_IS_PEER`. (See `qp_av_dmac_swap.md` ?2 for
empirical confirmation that this is exactly what the
post-fix harness reports.)

#### 4.6.5 Failure modes

* **Orchestrator forgot step (2).** The QP comes up with
  the wrong DMAC and first post_send fails with
  `IBV_WC_RETRY_EXC_ERR` (status 12). `check_qp_av_dmac.sh`
  reports `VERDICT=DMAC_IS_LOCAL`. Recovery: tear down,
  re-run with the orchestrator step in place. There is no
  post-LOAD recovery (per `qp_av_dmac_swap.md` Appendix A
  ?12).
* **Orchestrator applied step (2) in the wrong order.** If
  `ip addr add` runs before `ip link set vf mac`, the GID
  table populates with the old VF MAC, and outgoing sends
  carry `src_mac = old_vf_mac` even though the netdev's
  current MAC is correct. Symptoms identical to "forgot
  step (2)". Recovery: redo step (2) in MAC-then-IP order
  (the kernel's GID-table refresh on netdev MAC change is
  not always immediate; the safe path is to delete and
  re-add the IP after the MAC change).
* **Orchestrator stamped the wrong MAC.** Symmetric flip
  is the most common bug -- e.g. on host A you set the VF
  MAC to host A's *own* original VF MAC instead of host B's
  source-time peer-VF MAC. `check_qp_av_dmac.sh` will
  report `VERDICT=DMAC_AMBIGUOUS` if the captured MAC
  doesn't match the expected pattern; recovery is to
  consult the source-time topology dump and rerun.
* **`LOAD_VHCA_STATE` fails for an unrelated reason.** Same
  recovery as today; identity migration doesn't change LOAD
  failure semantics.

#### 4.6.6 Out of scope (forward look)

* **Source-side dmac fixup before SAVE_VHCA_STATE.** Would
  require the source kernel to know about destination
  topology, which violates the "save is host-state-blind"
  contract. Pre-LOAD identity migration on the destination
  is the right side of the wire to put this work.
* **A `MLX5_VFMIG_IOC_PATCH_SAVE_BLOB` ioctl that lets
  userspace rewrite QPC subfields.** Earlier drafts of this
  section proposed it as a fallback if identity migration
  proved insufficient. Empirically (see `qp_av_dmac_swap.md`
  ?2 validation) it isn't needed for ?S6b, and we don't
  want to ship a "rewrite arbitrary QPC fields" UAPI
  speculatively. Any future drift category that the existing
  `ip link` / `ip addr` / `ip neigh` triple can't address
  should re-open this design space rather than reaching
  for a blob-patch ioctl on day one.
* **Source-side identity capture in the dump blob.** The
  v0 harness uses a deterministic MAC scheme so the
  orchestrator can reconstruct source-time MACs without
  reading the dump. Production deployments may want the
  CRIU plugin to record the source-time per-VF MAC in a
  dump-side sidecar so the orchestrator on the destination
  can read it back at restore time without out-of-band
  state. This is plugin-side work (a few bytes per VF in
  the plugin image) and not a kernel ask; tracked
  separately in ?6 if needed.

## 5. What the kernel agent does NOT need to do

Out of scope for the kernel-side asks:

* **Multi-VF batching.** The prerestore binary handles one VF
  per invocation (or a `--vfs 0,1,2` list, but each VF gets its
  own kernel `LOAD_VHCA_STATE` call). The kernel does not need
  to add a batched primitive.
* **Cross-process sharing of a single VF's restore image.**
  The CRIU side asserts exclusivity today (one CRIU image per
  VF). A future relaxation that lets multiple processes share
  a single VF post-restore is on the CRIU-side roadmap (see
  ?6.5); it does not change the kernel-side flag semantics.
* **A separate "prerestore mode" `alloc_ucontext` flag.** The
  existing `MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID` adoption signal
  is sufficient -- it already means "this ucontext is the
  resume side of a vfmig session", and the state-flag transition
  hangs off that existing detection point.
* **Action-script / mid-restore hook in CRIU core.** Considered
  and rejected on the CRIU side (see ?8.1); the operator-orderable
  phases live entirely inside the CRIU plugin, with no
  CRIU-core surface change.

## 6. CRIU-side architecture

This section is the contract for the CRIU plugin work. Kernel
agent: this is FYI; you do not need to act on anything here.

### 6.0 VF UUID read at dump time (passive)

The plugin's existing dump path
(CR_PLUGIN_HOOK__RDMA_DUMP_UVERBS_CONTEXT in `vfmig_dump.c`,
`vfmig_capture_one_vf`) gains a passive read of the
orchestrator-assigned `vf_uuid`, performed once per
`(pf_bdf, vf_id)` capture using the extended `QUERY_VF`:

```c
struct mlx5_vfmig_query_vf q = { .vf_id = vf_id };
if (ioctl(cdev_fd, MLX5_VFMIG_IOC_QUERY_VF, &q) < 0)
    return fail("QUERY_VF on (pf=%s, vf_id=%u) failed", pf_bdf, vf_id);

if (uuid_is_null(q.vf_uuid)) {
    /* Hard refuse: orchestrator hasn't tagged this VF, so
     * CRIU cannot safely save its state -- there's no way
     * to bind the resulting dump to a specific VF on
     * restore. Return a clear error pointing the operator
     * at the orchestrator's provisioning step. */
    return fail("vfmig: VF (pf=%s, vf_id=%u) has no vf_uuid set; "
                "orchestrator must call SET_VF_UUID before dump",
                pf_bdf, vf_id);
}
memcpy(out->vf_uuid, q.vf_uuid, 16);
```

The UUID is written into the per-VF entry of the plugin image
(alongside the existing `vhca_id` / `blob_path` / `blob_size`
fields in `struct vfmig_saved_vf` and its protobuf-serialized
counterpart in `vf_image.c`'s `VfmigImage` message). The
existing `vhca_id` field stays for diagnostic purposes (it
tells you the source's runtime FW handle, useful for
`pr_info` output and offline dump analysis), but `vf_uuid`
is the canonical identity tag from this point forward.

The plugin **does not generate** UUIDs. If the dump path ever
finds a VF with `vf_uuid == 0` it refuses to dump rather than
filling the slot itself, because that would override the
orchestrator's contract that the orchestrator is the single
authoritative source of VF identity.

### 6.1 The new binary: `mlx5_vfmig_restore_vf`

Lives alongside the existing `rdma_mlx5_vfmig_plugin.so` in the
plugin's install tree:

```
/usr/lib/criu/plugins/rdma/mlx5_sriov_vfmig/
    rdma_mlx5_vfmig_plugin.so       # existing
    mlx5_vfmig_restore_vf            # new, this work
```

It is a thin wrapper that:

1. Parses CLI: `-D <image_dir>` (CRIU-style), `--vfs <id[,id...]>`
   (one or many), `--dry-run` (print what would be done, no
   kernel calls), `-v / -vv` (verbose).
2. `dlopen()`s the plugin `.so` from a known search path
   (`/usr/lib/criu/plugins/rdma/mlx5_sriov_vfmig/` first, then
   the path overridable via `$CRIU_RDMA_VFMIG_PLUGIN_PATH`).
3. Resolves the new exported symbol
   `mlx5_vfmig_plugin_restore_vf_only` (signature in ?6.2).
4. For each requested vf_id, reads the dump's plugin blob to
   find that VF's image, then invokes the symbol.
5. Emits one operator-readable status line per VF (success or
   error), exits with non-zero on any failure (no
   "partial success" exit code -- if the operator asked for
   N VFs and gets fewer, the exit code says so).

The binary itself does not link against any libmlx5 / libibverbs;
all RDMA-touching code lives in the `.so`, which already pulls
those in for the existing plugin path. Keeping the binary
minimal lets it be invoked standalone without any RDMA-specific
runtime config.

`--dry-run` mode reads the dump but issues no kernel calls; it
emits the planned `LOAD_VHCA_STATE` parameters per VF, which is
useful for orchestrators that want to validate dump-image
integrity before scheduling restores.

### 6.2 The plugin `.so`: new exported symbol

```c
/* In rdma_mlx5_vfmig_plugin.c (exported via the plugin's
 * existing visibility-default mechanism). */

/**
 * mlx5_vfmig_plugin_restore_vf_only - apply LOAD_VHCA_STATE for one
 *                                     VF; do nothing process-side.
 * @image_dir: CRIU image directory (where the dump's plugin blob
 *             lives; same path criu restore would consume).
 * @vf_id:     the SR-IOV vf_id whose state to apply.
 *
 * Reuses the same vfmig_blob reader path as the plugin's
 * regular init() routine. Drives the kernel to apply
 * LOAD_VHCA_STATE on (PF, vf_id), waits for the netdev / ibdev
 * to surface, then returns. Does NOT open a ucontext, allocate
 * a PD, or restore any uobject -- those steps stay in the
 * regular init() path on the next criu restore.
 *
 * The symbol does NOT generate or write any UUID -- the
 * orchestrator has already stamped vf_uuid on the destination
 * VF as part of provisioning. The symbol scans
 * /dev/mlx5_vfmig/* for a VF whose QUERY_VF.vf_uuid matches
 * the dump entry's vf_uuid; if no match is found it returns
 * -ENOENT and the caller should error out telling the operator
 * to ensure orchestrator-side SET_VF_UUID has been issued.
 *
 * On success the VF's MLX5_VFMIG_IOC_QUERY_VF.restored bit
 * transitions 0 -> 1 (set by the MARK_RESTORED ioctl issued
 * after staging the LOAD blob).
 *
 * Returns 0 on success, negative errno on failure. Idempotent:
 * a second call against an already-prerestored VF (matched by
 * vf_uuid; restored == 1) logs a no-op line and returns 0.
 */
int mlx5_vfmig_plugin_restore_vf_only(const char *image_dir,
                                      unsigned int vf_id);
```

The symbol is intentionally narrow -- one VF, one image_dir,
one return code. Multi-VF orchestration lives in the binary.
The symbol is also reusable: future tooling (e.g. orchestrator
sidecars, debug utilities) can `dlopen()` the plugin and call
this symbol directly without going through the binary.

### 6.3 Plugin `init()` branch logic

The plugin's existing `init()` already locates the per-VF
plugin blob and prepares to invoke `LOAD_VHCA_STATE`. The
branch logic added by this work:

```c
/* pseudocode -- real code lives in vfmig_restore.c */

for each vf_image in dump.vfs:
    /* Phase A: locate the destination VF by matching vf_uuid.
     * The orchestrator stamped vf_image.uuid on exactly one
     * destination VF as part of provisioning (KS7.3 ?3.5.3).
     * The orchestrator may have picked a different PF on the
     * destination, so we scan all eligible PFs; the dump's
     * recorded source PF BDF is just a hint we try first to
     * keep the common case fast. The destination's vf_id
     * MUST equal vf_image.vf_id (?3.5.3.1); we check that
     * after the UUID hit and refuse with a distinct error
     * if it doesn't agree. */
    matched_pf = NULL; matched_vf_id = -1; matched_q = {};
    for pf_bdf in [vf_image.source_pf_bdf] + other_eligible_pfs:
        int fd = open("/dev/mlx5_vfmig/" + pf_bdf, O_RDWR);
        if fd < 0: continue;
        for vf_id in 0..query_num_vfs(fd) - 1:
            struct mlx5_vfmig_query_vf q = { .vf_id = vf_id };
            if ioctl(fd, MLX5_VFMIG_IOC_QUERY_VF, &q) < 0: continue;
            if memcmp(q.vf_uuid, vf_image.uuid, 16) == 0:
                matched_pf = pf_bdf;
                matched_vf_id = vf_id;
                matched_q = q;
                break;
        if matched_pf: break;
        close(fd);

    if not matched_pf:
        /* Hard refuse: orchestrator must provision the destination
         * VF with the matching uuid before restore. CRIU does NOT
         * pick a vf_id slot or stamp a UUID on its own -- that
         * would re-introduce the orchestrator-skipping-the-stamp
         * bug we're trying to prevent. */
        return error("vfmig: no VF with uuid %s found on any "
                     "vfmig-eligible PF; orchestrator must "
                     "SET_VF_UUID on destination VF before restore",
                     uuid_str(vf_image.uuid));

    if matched_vf_id != vf_image.vf_id:
        /* Hard refuse, distinct error: orchestrator stamped the
         * right UUID but on the wrong slot. Cross-slot LOAD is
         * not supported (?3.5.3.1: vfmig_iova partitions per
         * vf_id; vport_num is vf_id+1; LOAD's IOVA replay
         * cross-check fails on slot mismatch). The operator
         * needs to fix sriov_numvfs / SET_VF_UUID on the
         * destination so the matching slot carries the UUID. */
        return error("vfmig: found UUID %s on (pf=%s, vf_id=%u) "
                     "but image was dumped from vf_id=%u; "
                     "orchestrator must provision matching slot "
                     "on destination (see ?3.5.3.1)",
                     uuid_str(vf_image.uuid), matched_pf,
                     matched_vf_id, vf_image.vf_id);

    /* Phase B: was the prerestore binary run for this VF?
     *
     * Signal: /sys/bus/pci/devices/<vf_bdf>/driver symlink
     * existence (i.e. is the VF bound to a kernel driver?).
     * NOT QUERY_VF.restored -- the kernel clears that bit at
     * VF probe time (mlx5_vfmig_vf_consume_restored), so any
     * post-bind QUERY_VF reads it as 0 regardless of whether
     * MARK_RESTORED was just issued. Plugin's init() always
     * runs post-bind (either prerestore drove the bind, or
     * the plugin itself drove it earlier in this same init()
     * invocation -- the monolithic path), so the restored bit
     * is unusable here. Sysfs bind-state under the
     * orchestrator contract "destination VF is bound only
     * via prerestore (or via the plugin itself)" is the
     * correct discriminator. See ?3 implementation note. */
    char vf_bdf[64];
    readlink("/sys/bus/pci/devices/" + matched_pf +
             "/virtfn" + matched_vf_id, vf_bdf);
    if exists("/sys/bus/pci/devices/" + vf_bdf + "/driver"):
        /* Yes -- prerestore drove LOAD + bind; plugin proceeds
         * to uobject restore against the already-bound VF. */
        log_prerestore_detected(vf_image, matched_pf, matched_vf_id);
    else:
        /* No -- monolithic fallback: orchestrator stamped UUID but
         * nobody ran prerestore. Drive LOAD + bind inline against
         * the matched VF. apply_load_vhca_state() also issues
         * MARK_RESTORED but does NOT touch vf_uuid -- that's
         * already set by the orchestrator. */
        log_prerestore_not_run(vf_image, matched_pf, matched_vf_id);
        apply_load_vhca_state(matched_pf, matched_vf_id, vf_image);
        driver_override_and_bind(vf_bdf);

    /* PD/CQ/MR/QP restore proceeds against
     * (matched_pf, matched_vf_id). */
```

The flow has one paired identity check
(`(vf_uuid, vf_id)` match) pulled from a QUERY_VF roundtrip,
plus one bind-state check pulled from sysfs:

* `q.vf_uuid == vf_image.uuid` AND `q.vf_id ==
  vf_image.vf_id` -- this is the right VF on the right
  slot (the one the orchestrator earmarked for this dump,
  on the slot the kernel/FW model requires; see
  ?3.5.3.1).
* `lstat("/sys/.../<vf_bdf>/driver")` -- has the prerestore
  binary already driven LOAD + bind on this VF? (See ?3
  implementation note: we use sysfs bind-state, not
  QUERY_VF.restored, because the latter is consumed at
  probe time.)

If no PF has the matching UUID at all, or the matching UUID
landed on the wrong slot, restore refuses with separate,
distinct error messages so the operator can tell which half
of the contract was violated. There is no fallback to
UUID-only matching nor to dump-recorded `vf_id` coercion:
the orchestrator owns the destination slot decision via
both `sriov_numvfs` provisioning and the `SET_VF_UUID`
call, and CRIU never picks a destination VF on its own.

A VF whose UUID doesn't match any dump in the current
restore session is left alone -- it belongs to some other
dump or some other workload.

### 6.4 Soft-fallback contract + log lines

Both code paths produce a single operator-readable log line
identifying which path ran. These are the operator's primary
diagnostic for "did I get the workflow right":

If the prerestore binary was run and the plugin detected it:

```
rdma_mlx5_vfmig: VF 0: prerestore detected; skipping LOAD_VHCA_STATE
                       (av.dmac refresh will run with current ARP cache)
```

If the prerestore binary was NOT run (monolithic fallback):

```
rdma_mlx5_vfmig: VF 0: prerestore was NOT run; applying LOAD_VHCA_STATE
                       in-line (av.dmac refresh will rely on operator
                       having pinned ARP before traffic resumes)
```

The plugin emits these via `pr_info` (CRIU's standard logging
macro, ends up in the CRIU restore log). They are **always
emitted**, regardless of verbosity level, because they affect
the post-restore datapath behavior and the operator needs to
correlate them with their orchestration flow.

The fallback path itself is unchanged from current main minus
the ?S6b helper, which has been removed (see ?4 banner /
`qp_av_dmac_swap.md` ?12). The fallback path therefore no longer
includes any post-restore dmac correction; on the v0 process-swap
workload it surfaces the ?S6b symptom (first `post_send` returns
`IBV_WC_RETRY_EXC_ERR` on RoCE v2 RC QPs whose source-resolved
dmac no longer matches the destination peer's MAC). No regression
for VM-LM-style workloads where the source-resolved dmac is
already valid on the destination -- those still complete RESTORE
cleanly and run traffic without intervention.

### 6.5 Multi-VF / multi-process: assert-exclusivity now, skip-SAVE later

**v0 behavior: assert exclusivity.**

Today the plugin's dump path asserts that exactly one CRIU image
captures any given VF's state. If two processes share access to
the same VF, the dump refuses with a clear error. Restore
inherits the same invariant: exactly one CRIU image's plugin
blob describes any given VF, and that image's restore is what
drives `LOAD_VHCA_STATE` for that VF.

The prerestore binary inherits the same single-VF invariant: it
takes one image directory's blob and applies it to one VF. If
two restored processes want to share a VF, they share the same
`vf_uuid` (the orchestrator stamped one UUID on that one VF;
both dumps captured that UUID); the kernel-side `vf_uuid` slot
is single-valued, which works fine for that case because the
shared workloads have the same identity. A future
many-different-UUIDs-per-VF relaxation would need either a set
of slots or a separate identity scheme; v0 doesn't need it.

**Future relaxation: dump-side skip-SAVE hook.**

For workloads with multiple processes that share a VF,
capturing the same VF state in N CRIU images is wasteful and
creates consistency problems on restore. The right shape (worth
noting for the future, NOT in v0):

* The plugin gains a dump-side hook that runs before
  `SAVE_VHCA_STATE` and lets external policy decide
  whether to actually save the VF blob into THIS image
  (versus declaring "image X already has the canonical
  state for vf_uuid Y"). The orchestrator owns the
  policy: it knows which image is the canonical owner
  for any given VF.

* At restore time, the orchestrator restores the canonical
  image first; dependent images' `init()` calls find the
  VF already at `restored=1` (matched by `vf_uuid`) and
  skip LOAD as today, then proceed to their own uobject
  restore.

This creates a "restore-in-reverse-saved" ordering
requirement on the orchestrator, which seems reasonable:
the orchestrator's workload graph already implies the
partial order. CRIU itself doesn't need to track the
canonical-vs-dependent distinction beyond the dump-side
hook saying "skip SAVE here".

This is **not** scoped for v0. The v0 contract is one-VF
per-image with the orchestrator handling identity end-to-end
via UUIDs. Multi-image relaxation builds on the same UUID
identity but adds the dump-side policy hook and (eventually)
per-uobject restore-completeness tracking on the kernel side
(?3.4 covers ABI compatibility for that growth).

### 6.6 Failure modes + idempotency

Idempotency: running `mlx5_vfmig_restore_vf` twice against the
same image is a no-op for the second call. The binary scans
PFs by `vf_uuid` from the image; if it finds a match with
`q.restored == 1` it logs an idempotent-no-op line and returns
0 without re-running LOAD. The orchestrator-owned UUID stays
put across re-runs (the SET_VF_UUID call was made once at
provisioning; CRIU never touches it).

Failure mid-LOAD_VHCA_STATE: leaves the VF in a kernel-side
indeterminate state. Today's monolithic plugin already has this
edge; the prerestore binary does not introduce a new failure
mode. The operator's recovery is the same: tear down the VF
(SR-IOV disable + re-enable on the PF) and try again.

Operator forgets prerestore: covered by soft-fallback, see ?6.4.

Operator runs prerestore but never runs criu restore: VF is
left at `restored=1` with `vf_uuid` set, indefinitely (LOAD
applied, no ucontext ever opened against it). Eventually torn
down by VF removal / SR-IOV disable; the UUID slot is cleared
as part of teardown. No kernel resource leak; FW cleanup
happens at VHCA close.

**Orchestrator forgets to stamp UUID on destination VF:**
Plugin's `init()` walk finds no PF/vf_id with the expected
UUID. Hard refuse with "no VF with uuid X found on any
vfmig-eligible PF; orchestrator must SET_VF_UUID on
destination VF before restore". This is the signal that
the orchestrator-side provisioning is incomplete; CRIU does
NOT autoprovision.

**Orchestrator stamps the wrong UUID on destination VF:**
Same outcome as above -- plugin's UUID-match scan finds no
hit, refuses. Orchestrator-side mistake; orchestrator-side
fix.

**Orchestrator stamps the right UUID on the wrong `vf_id`
slot:** Plugin's `init()` walk finds the UUID hit but on a
slot that disagrees with `image.vf_id`. Hard refuse with a
*distinct* error: "found UUID X on (pf=..., vf_id=Z) but
image was dumped from vf_id=Y; orchestrator must provision
matching slot on destination (see ?3.5.3.1)". The
distinct-error rule matters because the operator's recovery
is different: they have stamped the right workload identity
but on the wrong slot, so the fix is "tear down the
destination's `sriov_numvfs`, re-provision so the matching
`vf_id` slot exists, re-stamp the UUID there", not "find
which workload is missing its UUID stamp". CRIU does not
silently coerce LOAD onto the orchestrator's chosen slot
because cross-slot LOAD isn't supported (?3.5.3.1).

**Orchestrator stamps the same UUID on two different VFs
(orchestrator bug):** Plugin's `init()` walk finds the first
match and uses it; the duplicate is silently ignored. This is
an orchestrator-side invariant violation -- the kernel's
`SET_VF_UUID` doesn't enforce host-wide uniqueness because
that requires cross-PF coordination the kernel has no good
hook for. Documented as the orchestrator's responsibility.
Note that under the `(vf_uuid, vf_id)` paired-match
contract, "two VFs with the same UUID on the same host"
becomes meaningfully harder to hit by accident -- the
orchestrator would have to stamp the UUID *twice on
matching `vf_id` slots across two different PFs* for the
first-match-wins behaviour to be observable; stamping
twice on different slots is detected as a slot-mismatch
refuse on whichever PF the resolver hits first.

**Operator runs criu restore expecting prerestore but it
didn't run:** UUID match found (orchestrator did its
provisioning) but `restored == 0`. Monolithic fallback fires
against the matched VF; log line in ?6.4 surfaces the
condition. Operator sees "prerestore was NOT run" and either
accepts the in-line LOAD or aborts and runs prerestore
properly.

## 7. Generalization to other transports (forward look)

The prerestore-vs-monolithic split is not specific to mlx5
SR-IOV. Any future CRIU plugin for a transport that requires
mid-restore host-level configuration (vDPA, virtio, future
generations of the same SR-IOV story, etc.) faces the same
basic problem: CRIU's `init()` is a single shot, the operator
needs a control point inside it, and threading that control
point through CRIU core means inventing a generic phase
mechanism (which the wider CRIU community will not accept on
behalf of one transport).

The pattern this doc lands -- "ship a small standalone binary
that handles the host-level part, leave the process-level part
in the plugin's `init()`, gate the two via a kernel-exposed
state flag" -- generalizes cleanly:

* The binary lives in the plugin's install tree, not in CRIU
  core.
* The state flag lives in the transport's kernel driver, not
  in CRIU core.
* The `init()` branch logic lives in the plugin, not in CRIU
  core.

CRIU core sees only "a plugin's `init()` runs and either does
or does not apply transport state". No new core surface.

We **cannot enforce** this convention on future transport
plugins -- that's the cost of keeping CRIU core uninvolved.
But we can document the pattern. The home for that
documentation, when a second transport actually wants it, is a
new doc under `criu/Documentation/plugins/` describing
"transport plugins with mid-restore operator control points".
For now, this section serves as that documentation; if and
when a second transport adopts the pattern we factor it out.

The kernel-side surface pattern KS7.1 used (a binary
loaded/unloaded indicator queryable from a per-PF cdev or
similar pre-bind-visible surface) is reusable: any transport
driver that wants the same split can expose an equivalent
indicator. The exact mechanism (per-PF cdev ioctl vs sysfs vs
netlink) should follow whatever existing diagnostic surface
that driver already maintains; cross-driver consistency on
naming or location is a soft suggestion, not a requirement.

## 8. Alternates considered + rejected

These were evaluated and rejected during design discussion. We
record them here so future reviewers don't re-walk the same
ground.

### 8.1 CRIU action-script hook at a new phase

Add a new action-script phase to CRIU itself
(`post-rdma-vf-restore`) that fires after the RDMA plugin's
`LOAD_VHCA_STATE` and before `restore_rdma_qp`. Operator passes
`--action-script /path/to/pin_arp.sh` and CRIU runs the script
at the new hook point.

**Rejected** because:

* Requires a CRIU-core change to introduce a new phase, which
  sets a precedent ("plugins can request named phases") that
  CRIU maintainers will push back on.
* The action script runs in CRIU's restore process context,
  which complicates orchestration if the operator wants to
  call out to an external orchestrator (would need IPC).
* The existing test framework (which lives outside CRIU and
  orchestrates across hosts) doesn't naturally map onto an
  embedded action script -- it'd have to ship orchestration
  logic into a script CRIU shells out to.
* The hook gives the orchestrator no way to abort if the pin
  fails -- CRIU would proceed regardless.

### 8.2 Lazy dmac refresh on first post_send

Helper records "needs refresh" on the QPC. First
`mlx5_ib_post_send` after restore runs the refresh inline.
Operator pins ARP between restore and first traffic.

**Rejected** because:

* Adds branch + neigh_lookup to the post_send fast path, which
  is exactly where mlx5 maintainers will push back hardest.
* Doesn't actually solve the problem: if the operator hasn't
  pinned ARP yet when first traffic happens, the lazy refresh
  fails the same way Policy A does today, just later.
* Hard to surface "pin happened, retry the refresh" to the
  kernel without polling.

### 8.3 Refresh ioctl as primary path (not backstop)

Skip kernel-side in-restore refresh entirely. Operator workflow:
`criu restore` -> pin ARP -> userspace ioctl-refresh -> resume
traffic. Process is held frozen by the plugin until refresh
completes.

**Rejected** as the primary path because:

* Asymmetric workflow: setup happens AFTER restore instead of
  before, which is unusual and harder to document.
* Doesn't compose well with `--leave-stopped` semantics
  (CRIU's normal flow is "restore and immediately resume";
  freezing and waiting for an external signal needs careful
  integration).
* Operator has to enumerate QPs (we'd need a "list restored
  QPs needing refresh" sysfs/netlink interface).

~~**Accepted as a backstop** -- KS7.2 (?4) lands the ioctl as a
recovery + verification path while the primary in-restore
helper remains the canonical mechanism.~~

**(Updated 2026-06-08)** -- Both branches of this approach
are obsolete. KS7.2 was reverted (FW post-mortem in
`qp_av_dmac_swap.md` Appendix A ?12) AND the ?S6b problem
itself turned out to be addressable orchestrator-side (KS7.4
per-VF identity migration, ?4.6) without needing any
post-LOAD recovery surface. The rejection of "refresh ioctl
as primary path" stands -- the right primary path doesn't
need a refresh ioctl at all; it sets the destination VF's
identity to mirror the source-time peer's identity *before*
LOAD_VHCA_STATE, the same way SR-IOV VM-LM has always done.

### 8.4 Capture-and-replay ARP from dump-side

At dump, plugin captures the current ARP table for relevant
peers (filtered by what's referenced by any QP's
`av.dgid_ipv4`). At restore, plugin installs those entries on
the new netdev BEFORE calling RESTORE_QP.

**Rejected** because the v0 process-swap workload reassigns
peer IPs to the OPPOSITE physical host. The dump-time ARP entry
maps `192.168.100.4 -> MAC_of_old_host` but at restore time
`192.168.100.4` actually lives on `MAC_of_new_host`. Replaying
the dump-time ARP would re-create the dmac-stale problem in a
slightly different shape.

The "new ARP mapping" is, by construction, information that
doesn't exist at dump time and can only be supplied externally
at restore time. So this approach can't work for the
process-swap workload.

(It WOULD work for VM-LM or for failover scenarios where ARP
is preserved. But that's not the case we're solving.)

### 8.5 CRIU's existing networking restore handles static neighbors

If we could route the "pin static neighbor on new netdev"
through CRIU's existing net restore mechanism, the pin would
happen as part of CRIU's restore, before the RDMA plugin's QP
phase.

**Rejected** because the static neighbor mapping is host-side
configuration, not process state. CRIU's net restore is for
state captured at dump time. The new ARP mapping is a property
of the destination host, not the source process. So this is a
layering violation, and the captured-vs-supplied conflict in
?8.4 reappears.

## 9. Testing

### 9.1 KS7.2-only path (~~immediate verification, no CRIU-side change~~) -- **REMOVED**

> **Removed 2026-06-06; obsolete 2026-06-08.** This test
> path drove the reverted `MLX5_VFMIG_IOC_REFRESH_AV_DMAC`
> ioctl (KS7.2), which the firmware rejected
> reproducibly -- see `qp_av_dmac_swap.md` Appendix A ?12
> for the FW post-mortem. Both the ioctl and the test
> path are gone.
>
> The ?S6b problem the path was trying to validate is now
> **resolved orchestrator-side** (KS7.4: per-VF identity
> migration via `ip link set vf mac` + `ip addr` + `ip
> neigh`, executed before LOAD_VHCA_STATE). The forward
> equivalent of "immediate verification, no CRIU-side
> change" is to add KS7.4's three `ip` commands directly
> to the existing harness's per-host prerestore phase --
> validated 2026-06-08; see `qp_av_dmac_swap.md` STATUS
> banner. No additional test path is needed; ?9.2 / ?9.3
> already cover the full and soft-fallback flows.

### 9.2 Full prerestore-binary path (CRIU-side work landed)

Updated 2026-06-08 to reflect the resolved KS7.4: per-VF
identity migration is an *orchestrator-side step* using
existing `ip` UAPIs, not a kernel-side blob rewrite. The
prerestore binary's job is just `LOAD_VHCA_STATE` on the
unmodified blob; the orchestrator's job is to make sure the
VF identity matches the source-time peer's identity before
that LOAD happens.

```
provision_vf_host1:      orchestrator stamps SET_VF_UUID on
                         destination VF (KS7.3) -- must run BEFORE
                         workload bind on source AND BEFORE prerestore
                         on dest
provision_vf_host2:      same on host2
identity_migrate_host1:  KS7.4. Orchestrator runs, in order:
                           ip link set <PF> vf <ID> mac <SRC_PEER_VF_MAC>
                           ip addr  add  <SRC_PEER_VF_IP>/<prefix> dev <vf_netdev>
                           ip neigh replace <SRC_LOCAL_IP> \
                                            lladdr <SRC_LOCAL_VF_MAC> \
                                            dev <vf_netdev> nud permanent
                         The destination VF now presents the source-time
                         peer-VF identity at L2/L3.
identity_migrate_host2:  same on host2 with the symmetric flip
                         (host2 mirrors what host1 had source-time;
                         host1 mirrors what host2 had source-time)
restore_vf_host1:        runs mlx5_vfmig_restore_vf on host1
                         (binary scans /dev/mlx5_vfmig/* for matching
                          vf_uuid, then issues LOAD_VHCA_STATE on the
                          unmodified saved blob; QUERY_VF.restored
                          goes 0 -> 1)
restore_vf_host2:        same on host2
criu_restore_host1:      runs criu restore on host1
criu_restore_host2:      runs criu restore on host2
                         (init() looks up VF by vf_uuid, sees
                          restored=1, skips LOAD; QPs were loaded with
                          a QPC whose path.rmac_* already matches the
                          actual peer's VF MAC because the orchestrator
                          symmetrically reconfigured both hosts; so
                          RESTORE_QP just attaches uobjects)
ping_pong_after_restore: traffic should succeed
```

Pass criterion (empirically validated 2026-06-08):
`ping_pong_after_restore` returns OK MATCH on both
directions; `local_ack_timeout_err` does not tick;
`check_qp_av_dmac.sh` returns `VERDICT=DMAC_IS_PEER` on both
hosts; CRIU restore log on each host emits the "prerestore
detected; skipping LOAD_VHCA_STATE" line from ?6.4. The
prerestore binary log notes successful LOAD_VHCA_STATE on
the unmodified blob; no "blob rewrote N QPs" line because
no rewriting happens.

### 9.3 Soft-fallback path (no prerestore binary)

Validates that the monolithic flow still works for VM-LM
style workloads where the source-baked `av.dmac` is genuinely
valid on the destination -- typically because the migrating
VM brought its vNIC MAC with it and the peer didn't move.
The orchestrator-side UUID stamp is still required;
monolithic fallback uses the matched VF, it just does LOAD
inline.

The KS7.4 identity-migration step is **also** required for
the process-swap workload here -- monolithic vs prerestored
is orthogonal to whether the orchestrator did the per-VF
identity mirror. If the orchestrator did the migration,
monolithic LOAD_VHCA_STATE inside criu init() works the same
way prerestore would. If the orchestrator didn't, neither
flow saves you (per `qp_av_dmac_swap.md` Appendix A ?12, no
post-LOAD recovery exists).

```
provision_vf_host1 / host2:    orchestrator stamps SET_VF_UUID
                               on destination VFs (KS7.3)
identity_migrate_host1 / host2: orchestrator runs the three-step
                               KS7.4 mirror, same as ?9.2
criu_restore_host1 / host2:    runs criu restore (monolithic flow:
                               init() finds VF by vf_uuid, restored=0,
                               runs LOAD inline, sets restored=1)
ping_pong_after_restore:       succeeds when the orchestrator's KS7.4
                               step matches the workload
```

Pass criterion: log line "prerestore was NOT run" appears;
all PD/CQ/MR/QP restore steps succeed; ping/pong succeeds.
Difference from ?9.2 is purely operational: when there's no
mid-restore window the operator wants for other host
config, the prerestore binary is a no-op savings and the
monolithic flow is simpler. The KS7.4 identity-migration
step is identical and required in both shapes.

### 9.4 Idempotency

After `provision_vf_host1` (orchestrator stamps a UUID on
some VF that matches the dump), run
`mlx5_vfmig_restore_vf -D <image_dir>` twice. The second
invocation scans PFs by `vf_uuid`, finds the same VF with
`q.restored == 1`, logs an idempotent-no-op line, and returns
0. A subsequent `criu restore` proceeds normally.

### 9.5 Mismatch detection -- orchestrator forgot SET_VF_UUID

Provision SR-IOV but skip the `SET_VF_UUID` step. Run
`mlx5_vfmig_restore_vf -D <image_dir>`. The binary scans
all eligible PFs, finds no VF with `vf_uuid == dump.uuid`,
and refuses with "no VF with uuid X found on any
vfmig-eligible PF; orchestrator must SET_VF_UUID on
destination VF before restore". A subsequent `criu restore`
without a fix would emit the same error. The remediation
is operator-side: stamp the UUID, re-run.

### 9.6 Mismatch detection -- orchestrator stamped wrong UUID

Provision two SR-IOV VFs, stamp a UUID matching dump A on
vf0, then try restoring dump B (different uuid). Plugin's
init() walk finds no match for B's uuid, refuses with the
same hard error. No fallback to vf0's slot just because it's
"available" -- CRIU does not pick destination VFs on its
own.

### 9.7 Mismatch detection -- orchestrator stamped UUID on wrong slot

Provision two SR-IOV VFs on the destination (sriov_numvfs=2),
both `set_tracked=1`. The source dump was captured from
`vf_id=0`. The orchestrator (incorrectly) stamps the matching
`vf_uuid` onto `vf_id=1` instead, leaving `vf_id=0` with
`vf_uuid=all-zeros`. Plugin's `init()` walk finds the UUID
match on `vf_id=1` but `image.vf_id == 0`, so it refuses with
a *distinct* error (separate from ?9.5 / ?9.6): "found UUID
X on (pf=..., vf_id=1) but image was dumped from vf_id=0;
orchestrator must provision matching slot on destination
(see ?3.5.3.1)". No fallback: cross-slot LOAD is not
supported and CRIU does not silently coerce LOAD onto the
orchestrator's chosen slot. The remediation is operator-side:
re-provision so `vf_id=0` exists on the destination and stamp
the UUID there.

This scenario is also useful as a positive test for the
"more than coincidence" UUID resolver -- swap the two
stamps, so `vf_id=0` carries the matching UUID and `vf_id=1`
carries a different (or zero) UUID. The resolver must skip
past `vf_id=1` and bind to `vf_id=0`. With sriov_numvfs=1
on both ends, the trivial degenerate case can't distinguish
"resolver matched by UUID" from "resolver coincidentally
picked vf_id=0 because it's the only slot".

### 9.8 Same UUID stamped twice (orchestrator bug)

Provision two SR-IOV VFs and stamp the SAME `vf_uuid` on
both via `SET_VF_UUID` (orchestrator-side mistake). The
kernel-side ioctl does NOT enforce host-wide uniqueness so
both stamps succeed. Plugin's `init()` finds the first match
and uses it; the duplicate is silently ignored.

Note that under the `(vf_uuid, vf_id)` paired-match
contract this case is meaningfully harder to hit by accident
than the pure UUID-only design implied: the orchestrator
would have to stamp the UUID twice on slots that *both*
match the image's `vf_id` (i.e. across two PFs, on the
same `vf_id` slot of each). Stamping twice on different
slots within the same PF surfaces as ?9.7 (slot mismatch)
on whichever PF the resolver hits first, not as silent
duplication.

Failure mode: harmless but wasteful; documented as the
orchestrator's invariant.

## 10. YAML harness changes

The existing `rdma_test_agent_vfmig_criu_swap_after_qp.yaml`
gets three new step types and one reordered step. The harness
plays the role of orchestrator -- it stamps `vf_uuid` on both
source and destination VFs as part of provisioning, dumps the
source, then drives prerestore / criu-restore on the
destination.

* **`set_vf_uuid` (new)**: invokes `MLX5_VFMIG_IOC_SET_VF_UUID`
  via a CLI wrapper (likely added to
  `tools/testing/criu_rdma/tools/mlx5_vfmig.c` alongside
  the existing `mlx5_vfmig` verbs). Takes `host`, `pf_bdf`,
  `vf_id`, `vf_uuid`. The harness generates the UUID once at
  test setup and uses the same value on both source and
  destination so the dump captures it and the restore looks
  it up. Per ?3.5.3.1 the same `vf_id` value must be used on
  both sides; the YAML keeps them as separate keys (rather
  than collapsing to one shared field) so future tests can
  *deliberately* mismatch them to exercise the slot-mismatch
  refuse path (?9.7) without hand-rolling the YAML.
* **`restore_vf` (new)**: invokes `mlx5_vfmig_restore_vf`.
  Takes `host`, `image_dir`. Asserts exit code 0 and a status
  line that includes the matched `(pf_bdf, vf_id)` tuple.
  No `vf_id` parameter: the binary discovers the matching VF
  by `vf_uuid`, then verifies its slot agrees with the
  image's `vf_id` and refuses if not.
* ~~**`refresh_av_dmac` (new)**: invokes the
  `MLX5_VFMIG_IOC_REFRESH_AV_DMAC` ioctl via the CLI verb
  added to `tools/testing/criu_rdma/tools/mlx5_vfmig.c` in
  `1bbe576bc7c5`.~~ **Removed 2026-06-06**: the ioctl, CLI
  verb, and wrapper script were reverted along with KS7.2.
  See ?4 banner / `qp_av_dmac_swap.md` Appendix A ?12. The
  ?9.2 path no longer needs a post-restore refresh step:
  KS7.4 (orchestrator-side per-VF identity migration via
  `ip link set vf mac` / `ip addr` / `ip neigh`) makes the
  source-loaded QPC correct at t=0.
* **`identity_migrate_host*` (new, KS7.4)**: orchestrator
  step that runs the three `ip` commands to mirror the
  source-time peer-VF identity onto the destination VF.
  Slots in *between* `restore_vf_host*` provisioning (or
  the `sriov_numvfs` cycle, whichever is later) and the
  `criu_restore_host*` step. **MAC must be set before the
  IP** -- GID-table entries bind `(IP, MAC)` at IP-add
  time. Inputs are deterministic from the harness's
  source-time MAC scheme (the symmetric flip described in
  ?4.6.2).
* **`pin_static_neighbor_*` (still in YAML)**: now folds
  into `identity_migrate_host*` as the third `ip neigh
  replace` command. With the corrected lladdr (source-time
  peer-VF MAC, *not* destination NIC MAC), this step
  becomes a routine ARP pin rather than the workaround it
  was when the kernel-side fix was assumed.

YAML diff sketch (full prerestore-binary path, ?9.2):

```yaml
# Source-side provisioning (before workload runs / dump captures):
- name: set_vf_uuid_host1_src
  type: set_vf_uuid
  host: host1
  pf_bdf: 0000:08:00.0
  vf_id: 0
  vf_uuid: ${TEST_VF_UUID}
- name: set_vf_uuid_host2_src
  type: set_vf_uuid
  host: host2
  pf_bdf: 0000:08:00.0
  vf_id: 0
  vf_uuid: ${TEST_VF_UUID}

# ... existing workload + dump steps (dump captures vf_uuid) ...

# Destination-side provisioning (after sriov_numvfs cycle, before restore):
- name: set_vf_uuid_host1_dst
  type: set_vf_uuid
  host: host1
  pf_bdf: 0000:08:00.0
  vf_id: 0
  vf_uuid: ${TEST_VF_UUID}
- name: set_vf_uuid_host2_dst
  type: set_vf_uuid
  host: host2
  pf_bdf: 0000:08:00.0
  vf_id: 0
  vf_uuid: ${TEST_VF_UUID}

# Identity migration (KS7.4) + prerestore + criu restore:
- name: identity_migrate_host1
  type: identity_migrate          # new step type
  host: host1
  pf_bdf: 0000:08:00.0
  vf_id: 0
  # MAC is the source-time peer-VF MAC -- on host1 post-swap,
  # the local VF takes on what was host2's source-time VF MAC.
  vf_mac: ${SOURCE_TIME_HOST2_VF_MAC}
  vf_ip: ${SOURCE_TIME_HOST2_VF_IP}/24
  peer_ip: ${SOURCE_TIME_HOST1_VF_IP}
  peer_lladdr: ${SOURCE_TIME_HOST1_VF_MAC}
- name: identity_migrate_host2
  type: identity_migrate
  host: host2
  pf_bdf: 0000:08:00.0
  vf_id: 0
  # Symmetric flip: host2's local VF takes on host1's
  # source-time VF MAC, etc.
  vf_mac: ${SOURCE_TIME_HOST1_VF_MAC}
  vf_ip: ${SOURCE_TIME_HOST1_VF_IP}/24
  peer_ip: ${SOURCE_TIME_HOST2_VF_IP}
  peer_lladdr: ${SOURCE_TIME_HOST2_VF_MAC}
- name: restore_vf_host1
  type: restore_vf
  host: host1
  image_dir: /var/lib/criu/img/host1/
- name: restore_vf_host2
  type: restore_vf
  host: host2
  image_dir: /var/lib/criu/img/host2/
- name: criu_restore_host1
  # ... existing definition (no change) ...
- name: criu_restore_host2
  # ... existing definition (no change) ...
- name: ping_after_restore
  # ... existing definition (no change) ...
```

The `identity_migrate` step type encapsulates the three
`ip` commands from ?4.6.2 (MAC -> IP -> ARP, in order) and
returns success only if all three settle cleanly. On
failure, the step type emits the offending command and
its stderr so the operator can debug without having to
re-derive what went wrong.

The `pin_static_neighbor_*` steps from earlier drafts are
absorbed into `identity_migrate_host*` as the third `ip
neigh replace` command -- the lladdr it carries is the
source-time peer-VF MAC, *not* the destination host's
local NIC MAC, which was the bug the v0 harness shipped
with.

## 11. Open questions

Most of the original ?11 questions for the kernel agent are
resolved by the as-shipped surface; the remainder are KS7.3
review items.

1. **?S identifier.** Should this work get ?S6c (since it
   directly enables the ?S6b dmac refresh on the actual
   end-to-end workload) or ?S7 (because it's a broader
   architectural split)? The CRIU side has a slight preference
   for ?S6c -- the dmac-refresh helper is the only hard
   downstream dependency -- but defer to the kernel agent's
   judgment on the wider numbering. **Open.**

2. ~~**Sysfs path naming.**~~ **Resolved.** Dropped the sysfs
   surface in favor of the existing `MLX5_VFMIG_IOC_QUERY_VF`
   ioctl on the PF cdev. See ?3.3 for the rationale (per-VF
   sysfs subdir is fragile pre-LOAD; PF cdev with `0..num_vfs-1`
   iteration is the right shape). No new kernel work.

3. ~~**`refresh_qp_av` location.**~~ **Resolved, then
   REVERTED, then obsoleted.** Originally landed as
   `MLX5_VFMIG_IOC_REFRESH_AV_DMAC` (ioctl `0x12`) on the
   `/dev/mlx5_vfmig/<pf_bdf>` cdev in `b70b6624084a` with
   CLI wrapper `1bbe576bc7c5`; reverted 2026-06-06 because
   FW rejected `MODIFY_QP(RTS2RTS_QP, PRIMARY_ADDR_PATH)`
   with syndrome `0x00498c8b`. See ?4 banner /
   `qp_av_dmac_swap.md` Appendix A ?12. The replacement
   KS7.4 (?4.6) is now per-VF identity migration via
   existing `ip` UAPIs, with no runtime refresh ioctl
   needed -- the question is moot in both shapes (no
   post-LOAD ioctl; no pre-LOAD blob-patch ioctl either).

4. ~~**State on a cold boot with no SR-IOV.**~~ **Resolved.**
   With the sysfs proposal dropped, this becomes "what does
   `MLX5_VFMIG_IOC_QUERY_VF` return when SR-IOV is disabled?"
   Per the ioctl's existing semantics (see header doc-comment),
   out-of-range vf_id returns `-ERANGE` with `restored=0` /
   `tracked=0`, which is safe for the plugin to read in the
   error path.

5. **CRIU-side timing for issuing `MARK_RESTORED`.** This
   ioctl needs to fire between staging the LOAD blob (step 5
   of the lifecycle) and binding the VF (step 7). The
   prerestore binary owns this on the prerestore path; the
   plugin's `apply_load_vhca_state()` owns it on the
   monolithic fallback. Question for plugin authors, not
   kernel: should it be a separate plugin entry point, or
   internal to `apply_load_vhca_state()`? CRIU-side call this
   when the plugin work begins.

5a. ~~**Cross-slot LOAD support
   ("KS7.5").**~~ **Resolved 2026-06-10: not pursued.**
   2026-06-10 cross-slot smoke (source `vf_id=0`, destination
   `vf_id=1`, matching UUID) failed at `LOAD_VHCA_STATE` with
   the kernel's `vfmig_iova` slot-grid cross-check
   ("`vfmig_iova: vf 1 replay: wire claims slot N for IOVA X
   but destination partitioning maps it to slot M`"). The
   underlying cause is two `vf_id`-keyed FW invariants: the
   per-VF IOVA window (`base = VFMIG_IOVA_BASE + vf_id *
   VFMIG_IOVA_PER_VF`) and the FW E-Switch `vport_num`
   (typically `vf_id + 1`, embedded in saved QPCs / FDB).
   Relaxing the IOVA cross-check alone wouldn't be enough --
   the FW state would still reference the old `vport_num`.
   We considered an explicit kernel ask for cross-slot LOAD
   (relaxed IOVA replay grid + vport rewriting + LOAD-time
   audit) and rejected it: the orchestrator-side same-`vf_id`
   constraint is cheap (the orchestrator already owns
   provisioning), the kernel work would be large, and we
   have no production motivation today. Recorded as a hard
   contract in ?3.5.3 + ?3.5.3.1 + ?9.7. Reopen this when a
   real workload needs cross-slot.

6. **KS7.3: relationship to `SET_TRACKED`.** Should
   `SET_VF_UUID` be a separate ioctl (as proposed in ?3.5.4)
   or fold into `SET_TRACKED { enable=1 }` as a new field?
   Separate-ioctl pros: orthogonal lifecycle, doesn't grow an
   existing ABI. Folded-into-SET_TRACKED pros: single ioctl
   for "the orchestrator's whole VF provisioning ritual",
   avoids a class of "tracked but uuid not yet set" race.
   ?3.5.4 leans separate; defer to the kernel agent. Either
   way, CRIU and the prerestore binary read it via QUERY_VF.

7. **KS7.3: should `SET_VF_UUID` accept a "force" flag for
   testing?** ?3.5.5 says `-EBUSY` if a different UUID is
   already set; the orchestrator should never need to bypass
   that. But for the test harness it might be useful to have
   a force path that overwrites without teardown. Open
   question for the kernel agent: lean towards "no force
   flag, force a teardown for tests too" unless the harness
   writers push back.

8. **KS7.3: where should the `vf_uuid` field live in the
   per-VF context struct?** ?3.5.4 sketches it next to
   `restored` / `restored_vhca_id` on
   `mlx5_core_sriov.vfs_ctx[vf_id]`. That seems right but
   mlx5_core internal layout may have a preferred home
   (e.g. behind a `vfmig_state_lock`). Defer to the kernel
   agent.

9. **KS7.3: do we want to log the UUID on `MARK_RESTORED` /
   `SET_VF_UUID` per-VF mlx5_core_info lines for forensic
   tracing?** The existing `MARK_RESTORED` line includes
   `vhca_id`; adding the UUID hex would make post-mortem
   dump correlation trivial. Suggest yes; no ABI implication,
   just log content.

## 12. Cross-references

* ?S6b dmac refresh design + landed fix:
  [`qp_av_dmac_swap.md`](qp_av_dmac_swap.md) -- the helper this
  work is enabling.

* ?S3b PDN-registration-table wipe pattern:
  [`pd_registration_wipe.md`](pd_registration_wipe.md) -- the
  gated-restore precedent the state-flag approach borrows from
  (kernel tolerates a CRIU-orchestrated transition that vanilla
  code paths would refuse).

* Top-level uobject restore design:
  [`uobject_restore.md`](uobject_restore.md) -- where the
  prerestore vs in-line split fits into the wider ?S6 ucontext /
  uobject restore picture.
