# DESIGN: R3 -- per-uobject restore for CRIU-managed RDMA contexts

Status: **proposed**, pre-implementation. This doc covers the per-`ib_uobject`
restore layer that brings end-to-end RDMA workload checkpoint/restore to the
point where `ib_write_bw` (default polling mode) round-trips intact across a
CRIU dump+restore on a tracked + migrated VF.

Builds on `uar_restore.md` (which delivered ucontext + UAR restore).
Independent of, but compatible with, `user_mr_dma.md` (which delivers
user-MR DMA continuity at the IOMMU layer; R3 carries identity, that doc
carries page-backing).

The driving observation: today's `run_uverbs_cr.sh` (rxe) and
`run_vfmig_cr.sh` (mlx5) end-to-end tests both pass through CRIU's existing
ucontext / UAR / cdev plumbing cleanly, then both fail at the same line --

```
FAIL: ibv_dealloc_pd of pre-dump PD returned 2 (No such file or directory)
      -- pre-dump uobject did not survive restore
```

\-- because *no* uobject (PD/CQ/QP/MR/SRQ/AH) under the ucontext is preserved.
R3 closes that gap.

## 0. Kernel asks at a glance (handoff to kernel agent)

Concise, ordered list. Each item links to its detailed ?; together they are
the full driver-side surface this doc requires. CRIU-side work has zero
upstream-kernel dependency and lands in parallel; the kernel asks gate
end-to-end correctness, not initial scaffolding.

| # | ask | where | priority | size |
|---|---|---|---|---|
| K6 | **v0 gate.** FW-identity-continuity experiment: does `LOAD_VHCA_STATE` preserve PD/CQ/QP/SRQ/MKEY id reservations the way it provably does for UARs? Mirrors `uar_restore.md` ?3. Outcome decides whether K3/K4 mlx5 handlers are a small alloc-with-hint extension (best case) or require new "pre-reserve id N" FW commands (worst case, possibly FW patch). Run this first. | ?8.2, ?10 | **very high** | empirical experiment + small probe ioctl |
| K2 | **Already exists upstream as `UVERBS_METHOD_INFO_HANDLES` on `UVERBS_OBJECT_DEVICE`** (drivers/infiniband/core/uverbs_std_types_device.c). Takes a `UVERBS_ATTR_INFO_OBJECT_ID` (u16 -- accepts ANY core or driver-namespace object id via `uapi_key_obj()`), walks `ufile->uobjects` under `uobjects_lock` filtered by `obj->uapi_object`, returns `UVERBS_ATTR_INFO_HANDLES_LIST` (u32[]) and `UVERBS_ATTR_INFO_TOTAL_HANDLES` (filled count). Covers AH and every other non-restracked uobject. Drives the pre-suspend coverage check (DEVX/MW/FLOW/XRCD rejection) by enumerating those types and failing the dump if any are present. Validated end-to-end by `info_handles_probe` -- see ?7.2 | ?6.2 | done | zero kernel work |
| K2.5 | Wire up the **existing** `rdma_alloc_begin_uobject_at_handle()` helper (already in `drivers/infiniband/core/rdma_core.c`, added by the UAR restore work) into every K3 `RESTORE_<TYPE>` method. The primitive ??? XA-insert at caller-specified handle, return `-EBUSY` if taken ??? is already proven by the UAR restore path; this is plumbing, not new core | ?7.3 | medium | reuse existing helper |
| K3 | New generic uverbs method namespace `UVERBS_OBJECT_RESTORE` with one method per uobject class: `RESTORE_PD`, `RESTORE_CQ`, `RESTORE_COMP_CHANNEL`, `RESTORE_SRQ`, `RESTORE_QP`, `RESTORE_MR`, `RESTORE_AH`, `RESTORE_ASYNC_EVENT`. Each takes (target user_handle, hw-agnostic attrs, opaque blob, parent_handle xrefs). Dispatches through new `ib_device_ops.restore_<type>` callbacks. Gated by a new opt-in `ib_device_ops.ucontext_is_restore_mode` predicate that each driver implements over its own per-ucontext sticky bool (mlx5: `mlx5_ib_ucontext.vfmig_restore_mode`, set when the ucontext was opened with `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE`; rxe: `rxe_ucontext.restore_mode`, set when opened with `RXE_ALLOC_UCTX_RESTORE_MODE`). Generic dispatch treats missing callback as "no ucontext on this device may restore", so adding RESTORE_* support is strictly opt-in and the `ib_ucontext` core struct stays lean | ?7.1, ?7.2 | high | medium per type |
| K4 | `ib_device_ops` extended with `restore_pd`, `restore_cq`, `restore_qp`, `restore_mr`, `restore_srq`, `restore_ah`, `restore_comp_channel`, `restore_async_event`. Each driver installs its restore-mode ops vector once, at VF/device **probe** time, when the device is entering VFMIG_RESTORE state (i.e. before any uverbs cdev opens against it). No mid-life ops swapping | ?7.3, ?7.4 | high | one ops vector + per-driver impl |
| K8 | **Landed as `0601c496b413` (K8a NLDEV emit).** Per-uobject `ufile_handle` (== `obj->id` from `ufile->uobjects`) now emitted alongside the existing restrack-id attr from every `fill_res_<type>_entry` whose resource is user-created (PD/CQ/QP/MR/SRQ), gated by `!rdma_is_kernel_res(res)`. New UAPI attr `RDMA_NLDEV_ATTR_RES_HANDLE`. Validated end-to-end by `nldev_res_handle_probe` (asserts both presence and exact `obj->handle` equality, plus the kernel-only "MUST NOT carry" contract). Lets a CRIU dump plugin join NLDEV's restrack-id view (parent-edge encoding) with the uverbs `INFO_HANDLES` ufile-handle view (`target_handle` install) without an extra cross-reference dispatch. K8b alternative (extend `INFO_HANDLES` with a paired restrack list) recorded in ?7.5 as the rejected-but-considered shape | ?7.5 | done | -- |
| K5 | (already landed) `show_fdinfo` for cdev (`52721d09a`), async event fd (`551a1355f`), comp event fd (`a753315597`). No further fdinfo work | ?6.3 | done | -- |
| K7 | (optional, stretch) Add restrack entries for AH (`RDMA_RESTRACK_AH`). If we land K2, this is unnecessary -- but adding it later is cheap if K2 ends up not landing | ?6.2 | low | optional |
| K1 | (deprioritized, optional cleanup) Add `RDMA_NLDEV_ATTR_RES_CTXN` emission in `fill_res_qp_entry`, `fill_res_mr_entry`, `fill_res_srq_entry`, `fill_res_cm_id_entry`. Not v0-blocking: CRIU joins QP/MR/SRQ to ctxn through PDN against the PD inventory (PD entries already emit CTXN). Land only if a follow-on need surfaces | ?6.1 | very low | one line per fn |

**v0 ordering**: K6 first (gates K3/K4 mlx5 design) -- **done, PARTIAL
PASS, see ?10**. K2 in parallel -- **discovered already implemented as
`UVERBS_METHOD_INFO_HANDLES`, no kernel work; see ?7.2**. K8 landed as
**`0601c496b413` (K8a NLDEV emit)**, unblocking `target_handle`
propagation; CRIU now consumes `RDMA_NLDEV_ATTR_RES_HANDLE` directly
from its existing per-type NLDEV walk. K2.5 / K3 / K4 implement the
restore path, callback-by-callback across rxe + mlx5_vfmig per class
(PD -> MR -> CQ -> QP; see ?9.1). K5 is already done. K1 and K7 are
deferred / optional.

## 1. Goal and scope

### 1.1 In scope (R3 v0)

The eight uobject types under each `ib_uverbs_file` that an `ib_write_bw`
workload exercises:

* **PD** -- protection domain
* **CQ** -- completion queue
* **QP** -- queue pair (RC, UC, UD)
* **MR** -- memory region
* **SRQ** -- shared receive queue
* **AH** -- address handle
* **COMP_CHANNEL_FILE** -- completion event channel fd
* **ASYNC_EVENT_FILE** -- async event channel fd

For each: (a) hw-agnostic image schema captured at dump from NLDEV +
`LIST_UOBJS` + uverbs queries, (b) restore via a generic uverbs verb
dispatching through `ib_device_ops.restore_<type>` per driver, (c) topological
restore order across the per-ucontext DAG, (d) old-handle/restrack-id ->
new-handle map maintained in CRIU userspace.

### 1.2 Cross-driver coverage

* **rxe** (software): full coverage. Empty plugin blobs, fresh kernel state,
  no FW gymnastics. R3 makes rxe end-to-end work; rxe is the early-validation
  driver for the whole DAG/discovery/restore machinery.
* **mlx5_sriov_vfmig**: full coverage. Plugin blobs carry FW identity
  (pdn / cqn / qpn / mkey / srqn). Restore depends on FW preserving id
  reservations across `LOAD_VHCA_STATE` (see ?10/K6 for the empirical question).

### 1.3 Out of scope (deferred, with rationale)

* **DM** (`UVERBS_OBJECT_DM`) -- on-chip device memory, mlx5-only meaningful
  implementation. Not on `ib_write_bw` path. Defer; doc commits to placement
  inside the DAG (parallel to PD; MR-on-DM is a typed xref) so adding it
  later is structural extension, not refactor.
* **DEVX** (`MLX5_IB_OBJECT_DEVX_*`) -- mlx5-private raw FW commands. Not on
  `ib_write_bw` path. Pre-suspend coverage check rejects any process holding
  DEVX uobjects in v0; modeled as plugin-private subgraph for v0+1.
* **MW**, **FLOW**, **XRCD** -- generic API, not in NLDEV today, not on
  `ib_write_bw` path. Deferred to follow-on; `LIST_UOBJS` (K2) makes them
  trivially addable.
* **XRC** (XRC_INI/XRC_TGT QP variants and XRCD-keyed wire coupling) --
  XRC's uobjects are still rooted under one ucontext (XRCD/XRC SRQ/XRC TGT
  all live under their creator's ufile). What XRC adds is **wire-level**
  identity coupling between QPs that don't share a kernel uobject edge
  (INI <-> TGT via SRQN). That's the same shape as `dest_qp_num` continuity
  for plain RC -- pure schema/identity-hint problem, not a DAG-structure
  problem. Deferred for v0 because (a) `LIST_UOBJS` is needed for XRCD
  enumeration and (b) `ib_write_bw` doesn't use XRC.
* **CM_ID full state** (RDMA-CM state machine, listen state, IP <-> GID
  resolution cache) -- NLDEV gives us identity but not the full CMA state.
  v0 preserves `RES_CM_ID` enough for the application to re-establish on top;
  full CMA-layer continuity is a follow-on.
* **MAD agent registrations** -- mostly IB SM; not RoCE-relevant for v0.
* **Backward compatibility** of the image format -- still in-flux while the
  R3 schema settles. Same posture as `uar_restore.md`.

### 1.4 Topology coverage

* **Same-host** (same PF, same VF freshly re-bound or different VF on same
  PF): covered. FW state preservation is the easier case (id reservations
  unambiguously preserved across SAVE/LOAD on the same FW instance, modulo
  K6).
* **Cross-host** (different PF, possibly different generation of HCA):
  primary target. mlx5_vfmig's destination-is-fresh-VF model is identical
  to same-host from the R3 layer's perspective; the destination VHCA boots
  from `LOAD_VHCA_STATE` and CRIU/plugin restores uobjects on top. K6 is
  the cross-host empirical question.
* **Multi-VF** (process holds N uverbs fds across M ibdevs, where M > 1
  in the SR-IOV case): covered by construction. Discovery is per-ufile,
  restore is per-ufile in dependency order, plugin dispatch picks the right
  ops by parent ucontext's `criu_driver`.
* **Multi-process / cross-tree**: out of scope for v0. CRIU classifies
  RDMA plugins (and therefore the ibdevs they own) as **EXCLUSIVE** or
  **SHAREABLE** for cross-tree purposes:

  * **EXCLUSIVE** -- the ibdev's FW/kernel state is captured as a
    single VHCA-scoped blob at dump time and cannot be split per
    process. mlx5_vfmig is EXCLUSIVE because `SAVE_VHCA_STATE` is a
    per-VHCA atomic snapshot, and CRIU's freeze (SIGSTOP of dumpee
    threads) only quiesces the dump tree's userspace -- a process
    *outside* the tree holding open ufiles on the same VF would
    continue posting WRs / mutating FW state, racing the SAVE.  CRIU
    refuses to dump if any such cross-tree ufile holder exists on
    the same ibdev.  The orchestrator's job is to ensure each VF
    is bound 1:1 to a workload (typically via the KS7.3
    per-VF UUID, see [`vf_prerestore_split.md`](vf_prerestore_split.md)
    §3.5) so the cross-tree check is satisfied by construction.
  * **SHAREABLE** -- the ibdev has no atomic per-device snapshot
    operation; what dump captures is whatever uobjects the dump
    tree holds, and other trees can hold their own uobjects on the
    same ibdev independently.  rxe is SHAREABLE because it has no
    SAVE_VHCA_STATE equivalent and its kernel state is naturally
    per-process (queues live in vmalloc'd buffers per-ucontext;
    there is no shared FW image to capture).

  The check that enforces this is CRIU's existing per-ibdev
  cross-tree dump admission; the EXCLUSIVE / SHAREABLE bit just
  decides whether a sharing collision is a hard error (EXCLUSIVE)
  or permitted (SHAREABLE).

  > **Future work.** The EXCLUSIVE-on-mlx5_vfmig restriction comes
  > from `SAVE_VHCA_STATE`'s VHCA-wide scope, not from any
  > per-process state we couldn't otherwise capture.  Lifting it
  > -- so that a CRIU dump can capture process A's uobjects on a
  > shared VF without stalling process B's traffic on the same
  > VF -- would require either (a) a per-process FW-state save
  > (no current FW command), (b) a way for CRIU to skip
  > `SAVE_VHCA_STATE` and rely on the orchestrator owning
  > VF-level snapshots out-of-band, or (c) a quiesce primitive
  > that holds traffic only briefly enough to read identity
  > continuity and lets the VF resume immediately.  Out of scope
  > for v0; tracked as a follow-on so the orchestrator-side
  > workload-per-VF binding (KS7.3) doesn't become a permanent
  > deployment constraint.  The dump-side
  > "freeze the whole VF for the entire dump window" model is
  > acceptable for the v0 single-workload-per-VF case.

## 2. Background: state model and where each piece lives

```
                 process
                   |
                   |  (fd table)
                   v
            uverbs cdev fd  -- /dev/infiniband/uverbsN
                   |
                   |  (private_data)
                   v
            ib_uverbs_file (ufile)
              |
              | ufile->uobjects (per-ufile linked list of all uobjects)
              v
   +----+----+----+----+----+----+----+----+----+
   |    |    |    |    |    |    |    |    |    |
   PD   CQ   QP   MR   SRQ  AH   CC   AEF  XRCD  ...
                                  |    |
                                  fd   fd
                                  (visible to user via fd table)
```

Where:

* **uverbs cdev fd**: the user's "handle" to a ucontext. CRIU restores this
  via the existing `RDMA_OPEN_UVERBS_CDEV` plugin hook (already shipped in
  `uar_restore.md` work). The plugin returns a fd that already has
  `GET_CONTEXT` issued and (for vfmig) `RESTORE_UCONTEXT`/`RESTORE_DYN_UARS`
  applied. CRIU installs that fd at the user's saved fd number.

* **ib_uverbs_file**: kernel-side ucontext ownership root. Has a per-ufile
  uobject id space; user code holds `pd->handle`, `cq->handle`, etc.

* **ib_uobject**: the per-resource kernel object. Has a `ufile_handle`
  (small int, user-visible) and a `restrack_id` (u32, kernel-internal,
  ibdev-scoped). NLDEV emits `restrack_id` for cross-resource reference.

* **comp channel fd / async event fd** (CC, AEF): also uobjects (allocated
  via `UVERBS_TYPE_ALLOC_FD`), additionally exposed to userspace as fd
  numbers in the process fd table. Restore needs both: install the kernel
  uobject at the right ufile_handle, then install the fd at the right user
  fd number.

### 2.1 Per-ucontext DAG

Within one `ib_uverbs_file`, uobjects form a DAG:

* **PD** has no incoming xrefs from other uobjects (it's a root).
* **CQ** has no incoming xrefs (root). May reference a comp channel.
* **MR** xrefs PD. (Stage 2 of `user_mr_dma.md` adds an MR-on-DM
  variant that xrefs DM; out of scope here.)
* **SRQ** xrefs PD, optionally CQ.
* **AH** xrefs PD.
* **QP** xrefs PD, send_cq (CQ), recv_cq (CQ), optionally SRQ.
* **COMP_CHANNEL_FILE** has no incoming xrefs from other uobjects (it's a
  root); referenced by CQ at create time.
* **ASYNC_EVENT_FILE** has no incoming xrefs; intrinsic to the ucontext.

Topological order for restore: roots (PD, COMP_CHANNEL_FILE,
ASYNC_EVENT_FILE) first, then CQ, then SRQ + MR + AH, then QP. Generic CRIU
code derives this from the explicit xref edges in the image.

### 2.2 What NLDEV gives us today

NLDEV's `RDMA_NLDEV_CMD_RES_*_GET` covers PD/CQ/QP/MR/SRQ/CM_ID/CTX. It
emits per-resource ids (PDN/CQN/LQPN/MRN/SRQN), per-resource attrs
(qp_state, qp_type, src/dst PSN, src/dst QPN, MRLEN, RKEY/LKEY with
CAP_NET_ADMIN, CQE count, etc.), parent xrefs (PDN on QP/MR/SRQ,
CTXN on PD/CQ/CTX), and pid + kernel-name.

What NLDEV doesn't cover today, that R3 needs:

* CTXN on QP/MR/SRQ/CM_ID (currently only on PD/CQ/CTX). K1 -- not
  v0-blocking; PDN-join is the v0 workaround for QP/MR/SRQ. CM_ID has
  no PDN and is out of v0 scope.
* AH at all (no `RDMA_RESTRACK_AH`). Solved by K2 instead.
* MW/FLOW/XRCD/DM/DEVX (deferred per ?1.3).
* Driver-private FW state per uobject (FW pdn, mkey, etc.). Solved by
  per-driver QUERY ioctls (K3 mirror), not by NLDEV.

### 2.3 Userspace -> ufile -> ucontext mapping

CRIU walks `/proc/<pid>/fd/*`. For each uverbs-class fd, fdinfo emits
`ctxn: <restrack_id>` (already landed: K5). That maps fd -> ufile. The
ufile's ucontext is then the key for NLDEV resource queries on the
backing ibdev.

For comp channel fds and async event fds, the same ctxn is exposed via
the same fdinfo mechanism (already landed). So discovery is purely:
walk fdinfo -> get ctxn per fd -> NLDEV per-resource walk (filter by
ctxn directly for PD/CQ; join via PDN for QP/MR/SRQ; K1 makes this
uniform) + LIST_UOBJS query (for AH and other non-restracked types) ->
compose DAG.

## 3. Empirical foundation

### 3.1 What's known to work

* **UAR table preservation across LOAD_VHCA_STATE** (`uar_restore.md`
  ?3): cross-host empirically confirmed. Source's FW UAR ids re-installable
  on destination via the new RESTORE_UCONTEXT/RESTORE_DYN_UARS verbs.
  R3 leans on the same property for the rest of the uobject ids if K6 holds.
* **User-MR DMA on tracked VFs** (`user_mr_dma.md` stages 1-3
  landed; stage 1 = transparent IOMMU shim, stage 2 = source-side
  `(KIND_X, fw_id)` retag + `HOST_USER_PAGE` SAVE/LOAD wire path,
  stage 3 = `ib_umem_pin` + `vfmig_iova_bind_user_object`
  destination-bind primitive driven from inside the per-class
  RESTORE_X verb body). `ibv_reg_mr` succeeds on a tracked VF
  without the `IB_WC_MW_BIND_ERR` failure mode (stage 1), and
  rkey continuity across SAVE/LOAD is delivered by the kernel
  inside RESTORE_MR (stages 2+3) so the plugin path stays a
  single ioctl. The same pattern extends to CQ/QP/SRQ via S5/S6/S7.
* **RC ping pong on tracked VF** (kernel HEAD `52021ccf9ab3 ... Hacks
  which enable RC ping pong`): hacks landed enabling RC QP traffic on
  tracked VFs. Direct evidence that an RC QP on a tracked VF is wire-functional
  -- so R3's QP restore has a working baseline to validate against.
  Caveats (not fundamental IB-path issues, rather artefacts of the
  netdev<->ib-stack coupling that R3 inherits):
  - The mlx5e netdev on a restored VF runs with a TX-dropper /
    `netif_carrier_off` shim (FIXME tracked separately) -- so ARP doesn't
    egress and cross-host setups require pre-pinned neighbour entries
    (`ip neigh add ... lladdr ...`) on both sides.
  - `mlx5_ib_dev_res_init` runs unconditionally on restored VFs to
    materialise fresh PD/CQ/XRCDs (workaround documented as FIXME); SRQ
    init stays gated, which produces the expected "Couldn't create
    ib_mad QP1" log.
* **QP state continuity in production SR-IOV LM**: NVIDIA's existing
  SR-IOV live migration pipeline preserves QP state (PSN, dest_qp_num,
  q_key, etc.) across migration. Strong indirect evidence that
  `LOAD_VHCA_STATE` preserves the FW-side QP context. Doesn't tell us
  about *user-visible identity* (qpn/mkey/etc.) cross-host -- that's K6.

### 3.2 What's not yet known

* **K6: cross-host preservation of PD/CQ/QP/SRQ/MR id reservations across
  LOAD_VHCA_STATE.** Mirror the `uar_restore.md` ?3 experiment shape:
  on host A, allocate uobjects of each kind, log their FW ids, SAVE; on
  host B, LOAD, then probe "what's the next id FW would hand out?" via a
  PF-cdev ioctl. If next-id starts above the SAVE-time peak for a class,
  reservations are preserved (good); if it starts at zero, they aren't
  (bad, need pre-reserve verbs).
* Until K6 is run, the design assumes preservation by analogy to UARs and
  flags the failure mode in ?10.

## 4. The model: discovery + image + restore

### 4.1 Discovery

Three surfaces, used additively per uobject type:

1. **NLDEV per-resource walk + ctxn join** for PD/CQ/QP/MR/SRQ. CRIU
   userspace iterates `RDMA_NLDEV_CMD_RES_<TYPE>_GET` per ibdev. PD and
   CQ entries already emit CTXN; QP/MR/SRQ entries emit PDN, so CRIU
   joins through the PD inventory (PD restrack id -> ctxn) to recover
   the owning ucontext. K1 (when it lands) collapses this to a one-hop
   filter; until then the join works for v0 because every user-mode
   QP/MR/SRQ has a parent PD that's also in the inventory.
2. **Generic uverbs `LIST_UOBJS(type)`** (K2) for AH and any other type
   not in NLDEV. Walks `ufile->uobjects` filtered by type, returns
   `[{handle, ...}]`.
3. **Per-driver QUERY ioctls** (driver-specific, e.g.
   `MLX5_IB_METHOD_VFMIG_QUERY_PD`) for the plugin blob portion. Each
   plugin defines its own QUERY shape; CRIU shuttles the resulting bytes.

CRIU exposes a helper:

```c
int criu_rdma_discover(struct ibv_context *cdev, uint32_t ctxn,
                       struct rdma_uobj_inv **out, size_t *out_n);
```

Default per-plugin behaviour: call this inline. Plugins that need different
discovery override via `CR_PLUGIN_HOOK__RDMA_DISCOVER_UOBJS`. The two plugins
we ship (rxe, mlx5_vfmig) use the inline default; the override is offered
for future plugins.

### 4.2 Image format

```protobuf
message rdma_uobj_entry {
    /* Identity */
    required uint32 ufile_id      = 1;  /* xref UverbsFileEntry.id          */
    required uint32 hw_driver_id  = 2;  /* mirror UverbsFileEntry.criu_driver
                                         * for image-inspection convenience */
    required UobjType type        = 3;  /* PD/CQ/QP/MR/SRQ/AH/CC/AEF        */
    required uint32 ufile_handle  = 4;  /* user-visible kernel handle on
                                         * source; install at this value    */
    required uint32 restrack_id   = 5;  /* per-ibdev restrack id on source;
                                         * used as xref edge target         */

    /* HW-agnostic per-class attrs. EXACTLY ONE matches `type`. */
    optional rdma_pd_attrs   pd  = 10;
    optional rdma_cq_attrs   cq  = 11;
    optional rdma_qp_attrs   qp  = 12;
    optional rdma_mr_attrs   mr  = 13;
    optional rdma_srq_attrs  srq = 14;
    optional rdma_ah_attrs   ah  = 15;
    optional rdma_cc_attrs   cc  = 16;  /* comp channel */
    optional rdma_aef_attrs  aef = 17;  /* async event file */

    /* Xref edges into the same image. type+restrack_id keyed.
     * Resolved through CRIU's old->new handle map at restore. */
    repeated rdma_uobj_xref  xref = 30;

    /* Plugin-specific opaque blob. Recommended: a per-plugin protobuf
     * serialised into bytes. Empty for software providers (rxe). */
    optional bytes plugin_blob = 40;
}

message rdma_uobj_xref {
    required UobjType target_type      = 1;  /* PD/CQ/SRQ/...                 */
    required uint32   target_restrack  = 2;  /* source-side restrack id       */
    required XrefRole role             = 3;  /* PARENT_PD, SEND_CQ, RECV_CQ,
                                              * SRQ, COMP_CHANNEL, ...        */
}

message rdma_qp_attrs {
    /* From ib_qp_init_attr */
    required QpType  type           = 1;  /* RC/UC/UD                         */
    required QpCap   cap            = 2;  /* max_send_wr, max_recv_wr,
                                           * max_send_sge, max_recv_sge,
                                           * max_inline_data                  */
    /* From ib_qp_attr -- the full transition state */
    required QpState state          = 10;
    required uint32  qp_num         = 11;  /* identity hint at restore        */
    optional uint32  dest_qp_num    = 12;
    optional uint32  sq_psn         = 13;
    optional uint32  rq_psn         = 14;
    optional uint32  q_key          = 15;
    optional rdma_ah_attrs av       = 16;  /* primary path                    */
    optional rdma_ah_attrs alt_av   = 17;
    optional uint32  path_mtu       = 18;
    optional uint32  retry_cnt      = 19;
    optional uint32  rnr_retry      = 20;
    optional uint32  timeout        = 21;
    optional uint32  rnr_timer      = 22;
    optional uint32  max_rd_atomic  = 23;
    optional uint32  max_dest_rd_atomic = 24;
    optional uint32  min_rnr_timer  = 25;
    optional uint32  pkey_index     = 26;
    optional uint32  port_num       = 27;
    optional uint32  alt_pkey_index = 28;
    optional uint32  alt_port_num   = 29;
    required uint32  access_flags   = 30;
    required uint32  qp_access_flags= 31;
    optional uint32  sq_draining    = 32;
}

message rdma_mr_attrs {
    required uint64 virt_addr   = 1;  /* user-side VA the MR was registered at */
    required uint64 length      = 2;
    required uint32 access_flags= 3;
    required uint32 lkey        = 10; /* identity hint                          */
    required uint32 rkey        = 11; /* identity hint -- on the wire           */
    optional uint64 iova        = 12;
}

message rdma_pd_attrs {
    /* PDs carry only access semantics; FW pdn lives in plugin_blob. */
    optional uint32 alloc_flags = 1;
}

message rdma_cq_attrs {
    required uint32 cqe_count   = 1;
    optional uint32 comp_vector = 2;
    optional uint32 flags       = 3;
    /* If CQ was created with a comp channel, xref carries it (role=COMP_CHANNEL) */
}

message rdma_srq_attrs {
    required SrqType type           = 1;  /* BASIC / XRC */
    required uint32  max_wr         = 2;
    required uint32  max_sge        = 3;
    optional uint32  srq_limit      = 4;
    /* xref carries PD; XRC SRQ adds XRCD xref (deferred) */
}

message rdma_ah_attrs {
    /* From struct rdma_ah_attr */
    required uint32 ah_attr_type   = 1;   /* IB / ROCE / OPA */
    required uint32 port_num       = 2;
    required uint32 sl             = 3;
    required uint32 src_path_bits  = 4;
    required uint32 static_rate    = 5;
    optional rdma_grh_attrs grh    = 6;   /* always set on ROCE */
    /* ROCE-specific */
    optional bytes  dmac           = 10;  /* 6 bytes */
}

message rdma_grh_attrs {
    required bytes  dgid            = 1;  /* 16 bytes */
    required uint32 sgid_index      = 2;
    required uint32 hop_limit       = 3;
    required uint32 traffic_class   = 4;
    required uint32 flow_label      = 5;
}

message rdma_cc_attrs {
    /* Comp channels carry no per-class hw_agnostic state beyond their handle.
     * The fd-number install happens via CRIU's existing fd-table machinery. */
    required uint32 user_fd_no = 1;       /* fd number in source's fd table   */
}

message rdma_aef_attrs {
    /* Async event file: same shape as comp channel. */
    required uint32 user_fd_no = 1;
}

enum UobjType {
    UOBJ_PD                  = 0;
    UOBJ_CQ                  = 1;
    UOBJ_QP                  = 2;
    UOBJ_MR                  = 3;
    UOBJ_SRQ                 = 4;
    UOBJ_AH                  = 5;
    UOBJ_COMP_CHANNEL_FILE   = 6;
    UOBJ_ASYNC_EVENT_FILE    = 7;
    /* Reserved for follow-on:
     * UOBJ_DM, UOBJ_DEVX_OBJ, UOBJ_DEVX_UMEM, UOBJ_MW, UOBJ_FLOW,
     * UOBJ_XRCD, UOBJ_CM_ID  */
}

enum XrefRole {
    XR_PARENT_PD     = 0;
    XR_SEND_CQ       = 1;
    XR_RECV_CQ       = 2;
    XR_SRQ           = 3;
    XR_COMP_CHANNEL  = 4;
    /* Reserved: XR_DM, XR_XRCD, XR_DEVX_UAR, ... */
}
```

Image-format notes:

* `ufile_handle` and `restrack_id` are deliberately distinct. `ufile_handle`
  is small, per-ufile, install-target; `restrack_id` is per-ibdev,
  cross-resource-key, **and is user-visible via NLDEV** (`rdma resource
  show` exposes it as the per-resource `id`). R3 carries it for two
  reasons: as the xref edge-target inside the image, and -- relevantly --
  in case any consumer outside the dump tree caches it. The honest
  caveat: if a process or external tool relies on `restrack_id` being
  stable across restore (e.g. a sidecar metrics daemon scraping
  `rdma resource show` and joining on resource id), R3 will break it
  silently. Restore allocates fresh restrack ids on the destination
  ibdev; the source's are not preserved. Documented as a known
  externally-visible identity discontinuity (parallel to the wire-rkey
  discontinuity that stage 3 of `user_mr_dma.md` solved for MRs;
  S5/S6/S7 will extend the same pattern to CQ/QP/SRQ rkey-equivalents).
  No mitigation in v0; lifted only if a real consumer needs it.
* `xref` edges always reference `restrack_id` (the cross-resource key).
  Restore translates `restrack_id` -> `ufile_handle` via the per-ufile
  handle map.
* Identity hints (qp_num, sq_psn, rq_psn, q_key, lkey, rkey, ...) are
  always populated on dump and always passed to RESTORE on restore. The
  per-uobject-class restore handler decides whether failure to honour is
  fail-loud or silent-fallback (?4.4).
* Plugin blob is opaque; recommended encoding is a per-plugin protobuf
  (e.g. `mlx5_vfmig.proto` extended with per-uobject sub-messages). CRIU
  core never parses it.

### 4.3 Restore

Three surfaces, used in order per uobject:

1. **(optional) CRIU plugin pre-restore hook** -- per uobject type, e.g.
   `CR_PLUGIN_HOOK__RDMA_PRE_RESTORE_QP`. Default: no-op. Used for
   process-side state (memory mappings, file installs) that has to exist
   before the kernel verb fires.
2. **Generic uverbs `RESTORE_<TYPE>` ioctl** (K3) on the cdev fd. Args:
   target ufile_handle, hw-agnostic attrs, opaque blob, parent_handle
   xrefs (already resolved by CRIU). Kernel handler dispatches through
   `ib_device_ops.restore_<type>` (K4). Returns the new ib_uobject; CRIU
   records `(restrack_id_on_source -> ufile_handle_on_destination)` in
   the per-ufile handle map.
3. **(optional) CRIU plugin post-restore hook** -- per uobject type,
   default no-op. Used for post-create state that doesn't fit kernel
   semantics (mostly VMA replay, already covered by the existing
   `HANDLE_DEVICE_VMA` / `UPDATE_VMA_MAP` plumbing for vfmig UAR pages;
   could also handle fd-table install for CC/AEF).

Generic CRIU code (criu/rdma.c) drives the iteration order from the DAG
topological sort.

### 4.4 Identity continuity policy

Schema always carries every relevant identity hint; honour-or-fail policy
is per-uobject-class, encoded in the kernel handler:

| class | wire-visible identity | policy on hint mismatch |
|---|---|---|
| PD  | (none)                                          | always succeed |
| CQ  | (none)                                          | always succeed |
| AH  | (none)                                          | always succeed |
| SRQ | (XRC SRQN -- deferred; no v0 identity)          | always succeed |
| CC  | (none)                                          | always succeed |
| AEF | (none)                                          | always succeed |
| MR  | rkey, lkey                                      | **fail loud** if FW can't honour |
| QP  | qpn (BTH.DestQP), sq_psn/rq_psn, q_key (UD)     | **fail loud** if FW can't honour |

For the fail-loud cases, the hint travels into the kernel restore handler;
the handler attempts to bind to that identity (e.g. via vfmig's
"pre-reserved by LOAD_VHCA_STATE" path). Errno contract:

* `-EADDRINUSE` -- FW returned a *different* id than the caller's hint
  (identity not preserved on the destination VHCA). CRIU surfaces this
  upstream as "FW didn't preserve identity X for uobject Y of class C".
* `-EBUSY` -- the destination ufile already has a uobject installed at
  the requested user_handle. Should not happen on a freshly-opened
  ucontext but is the contract from `rdma_alloc_begin_uobject_at_handle`.
* `-EOPNOTSUPP` -- driver doesn't implement `restore_<type>` (no
  restore-mode support). CRIU surfaces as "this driver doesn't support
  R3 restore yet".

For software providers (rxe), identity hints are honoured by extending
the allocator with "install at this index" semantics. Landed for MR via
`__rxe_add_to_pool_at_index` in `drivers/infiniband/sw/rxe/rxe_pool.c`:
the new pool primitive does an `xa_insert(index)` instead of the cyclic
`xa_alloc_cyclic`, returns `-EBUSY` on collision (the per-verb shape
CRIU expects), and co-exists transparently with fresh-alloc callers
because `xa_alloc_cyclic` skips taken slots. The 8-bit per-MR nonce
inside the lkey/rkey is overwritten from the hint's low byte after
`rxe_mr_init` runs. Net result: `rxe_restore_mr` lands an MR whose
wire-visible `lkey`/`rkey` is byte-identical to the source's, so
post-restore work requests that embed the source `lkey` keep
functioning. The same pattern (`__rxe_add_to_pool_at_index`) extends
trivially to QP / CQ / SRQ when those land in S5..S7.

## 5. Per-uobject-class details

For each class: discovery source, kernel verb args, kernel handler shape,
plugin contribution.

### 5.1 PD

* **Discovery**: NLDEV `RES_PD_GET` (CTXN already emitted today). Yields
  hw-agnostic attrs (none beyond access flags, which aren't currently in
  NLDEV but are recoverable from `ib_pd->flags`); the plugin adds the FW
  pdn via the **landed** `MLX5_IB_METHOD_VFMIG_QUERY_PD(handle)`, which
  returns a `struct mlx5_ib_restore_pd_req` blob (byte-equal to what
  `RESTORE_PD` consumes) plus the source PD's `uid` as a dump-side
  cross-check out. See ?5.1.4 for the full rationale, including why this
  superseded the earlier NLDEV `fw_pdn`/`fw_uid` driver-TLV path.
* **Kernel verb**: `UVERBS_METHOD_RESTORE_PD(target_handle, alloc_flags,
  blob)`. Calls `ib_dev->ops.restore_pd()`.
* **Driver-side (mlx5_vfmig)**: allocates a `mlx5_ib_pd`, calls FW
  `ALLOC_PD` with a hint for the source's pdn (or relies on
  LOAD_VHCA_STATE having reserved it; K6 decides). Installs at
  `target_handle`.
* **Driver-side (rxe)**: standard `rxe_alloc_pd()` plus install-at-handle.
  No FW state.

#### 5.1.4 PD dump-side discovery: `MLX5_IB_METHOD_VFMIG_QUERY_PD`

CRIU's PD dump phase needs exactly one field the destination's
`RESTORE_PD` will consume: the FW `pdn` (`mpd->pdn`), so the
destination ucontext adopts the same FW pdn that LOAD_VHCA_STATE
preserved. PD is the simplest of the adopted-resource family --
there is no umem and no source userspace VA, just the FW
resource id and the owning `uid`.

**Why this is the QUERY family's last member, not an NLDEV TLV**

The first cut of PD discovery emitted `fw_pdn` and `fw_uid` as
driver-private TLVs nested under `RDMA_NLDEV_ATTR_DRIVER` from a
`fill_res_pd_entry` hook in `mlx5/restrack.c`. That worked, but
it made PD the odd one out: every other adopted FW resource id
in this effort (`cqn` via `MLX5_IB_METHOD_VFMIG_QUERY_CQ`, `qpn`
via `_QUERY_QP`) is discovered through a per-handle driver-private
QUERY method that returns a byte-equal `RESP_BLOB`. Two specific
problems with leaving PD on NLDEV:

* **Layering asymmetry.** A CRIU plugin would have to special-case
  PD: scrape a hex TLV string (`"fw_pdn"`) out of an
  `RDMA_NLDEV_ATTR_DRIVER` nest for PD, but `memcpy` a fixed-layout
  blob off a uverbs ioctl for CQ and QP. Same FW-resource-id
  problem, two unrelated discovery shapes.
* **Wider security scope than needed.** NLDEV is
  `CAP_NET_ADMIN`-gated and visible across every netns. CRIU
  already holds the dumpee's `uverbsfd` (the same fd it uses for
  `INFO_HANDLES` / K8a `RES_HANDLE` joins and for QUERY_CQ/_QP),
  so routing PD discovery through a uverbs ioctl gives the right
  boundary -- "if you can see the ucontext, you can read its
  metadata" -- without the `CAP_NET_ADMIN` / cross-netns surface.
  Same argument ?5.2.4 made for not extending NLDEV with the CQ
  user-VA fields, and ?7.7 for the MR fields.

Note the core NLDEV `RES_PDN` attribute (`res->id`) stays as-is
and is **not** the FW pdn -- it is the per-`ib_device` restrack
`xa_alloc_cyclic` id used as the `rdma res show pd pdn=N` handle.
The two coincide on freshly-booted devices but diverge once the
restrack IDR has wrapped past the FW pdn allocator's high-water
mark, which is exactly why CRIU needs the FW `pdn` explicitly
rather than reusing `RES_PDN`.

**Landed shape**

`include/uapi/rdma/mlx5_user_ioctl_cmds.h`:

```c
enum mlx5_ib_vfmig_methods {
    /* ... QUERY_UCONTEXT ... QUERY_CQ, QUERY_QP ... */
    MLX5_IB_METHOD_VFMIG_QUERY_PD,    /* new -- appended last */
};

enum mlx5_ib_vfmig_query_pd_attrs {
    MLX5_IB_ATTR_VFMIG_QUERY_PD_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
    MLX5_IB_ATTR_VFMIG_QUERY_PD_RESP_BLOB,   /* mlx5_ib_restore_pd_req, 16B */
    MLX5_IB_ATTR_VFMIG_QUERY_PD_RESP_UID,    /* u32, mpd->uid */
};
```

The HANDLE is `UVERBS_ATTR_IDR(UVERBS_OBJECT_PD,
UVERBS_ACCESS_READ)` -- the calling fd's ufile-idr must own this
PD, the IDR pins the uobject for the duration of the call. Same
security boundary as `INFO_HANDLES(UVERBS_OBJECT_PD)` and as the
QUERY_CQ / QUERY_QP handle attrs.

**Byte-equal payload contract.** `RESP_BLOB` is byte-equal to
`struct mlx5_ib_restore_pd_req` (16 bytes). The handler zeroes
`reserved`/`reserved2` so the round-trip into RESTORE_PD's
"must be 0" guards passes verbatim. `RESP_UID` is a dump-side
cross-check **only** -- `RESTORE_PD` sets `mpd->uid` from the
adopted ucontext's `devx_uid`, not from this value; the plugin
uses it to fail the dump early if the source PD is not under the
v0 host-privileged lane (`uid != 0`) rather than producing an
unrestorable image. CRIU plugin code at the seam reduces to:

```c
/* dump phase */
ioctl(uverbsfd, RDMA_VERBS_IOCTL, &query_pd_cmd);
if (resp_uid != 0)
    bail("source PD not on the uid==0 host lane");
img.pd[i].blob = blob;             /* 16B verbatim */

/* restore phase */
restore_pd_cmd.uhw_in.data = (uintptr_t)&img.pd[i].blob;
restore_pd_cmd.uhw_in.len  = sizeof(img.pd[i].blob);
ioctl(dst_uverbsfd, RDMA_VERBS_IOCTL, &restore_pd_cmd);
```

No field-level marshaling, no NLDEV TLV scrape.

**No kernel-mode rejection needed.** Unlike QUERY_CQ / QUERY_QP
there is nothing to reject defensively: a PD has no umem and no
source userspace VA, the IDR lookup came through a user ufile by
construction (kernel PDs are not in any user idr), and `mpd->pdn`
is the 24-bit FW resource id, non-zero for any live PD.

**Validator.** `tools/testing/mlx5_vfmig/uobject_restore/pd_query/
pd_query_probe_mlx5_vfmig.c` is the single-process byte-equality
probe. It allocs real PDs via `ibv_alloc_pd`, reads view (A) via
`mlx5dv_init_obj(MLX5DV_OBJ_PD)` (`dvpd.pdn`), reads view (B) via
`MLX5_IB_METHOD_VFMIG_QUERY_PD`, and asserts the verb-mechanics
contract -- `blob.pdn == dvpd.pdn` plus the two reserved-zero
fields -- unconditionally. `resp_uid` is *reported as a lane*, not
asserted to a fixed value: every PD inherits its ucontext's
`devx_uid`, so the probe annotates `uid==0` as the v0-supported
(non-DEVX) lane and `uid!=0` as the libmlx5 auto-DEVX lane (which
the CRIU dump policy refuses at the `QUERY_UCONTEXT`
`meta.devx_uid` gate, not inside QUERY_PD). The multi-PD subtest
additionally asserts both PDs report the *same* uid (shared
`context->devx_uid` invariant). Subtests cover happy path,
invalid-handle (-ENOENT), and multi-PD disambiguation.

Empirical note: on rdma-core 39 (Ubuntu) running the probe under
`sudo`, libmlx5 auto-allocates a DEVX uid for the default context,
so the probe reports the auto-DEVX lane (`uid != 0`, e.g. `uid=2`)
and the `pdn` byte-equal contract still passes. To exercise the
`uid==0` lane explicitly, run a context that does not obtain a
DEVX uid (no DEVX privilege); the verb itself behaves identically
either way -- only the lane annotation changes.

### 5.2 CQ + comp channel fd

* **Discovery (CQ)**: NLDEV `RES_CQ_GET` (CTXN already emitted). Yields
  cqe count, dim flag, poll_ctx for kernel CQs. Plugin adds the
  source-side userspace VAs of the CQE-ring buffer + DBR page, plus
  the FW cqn and `cqe_size` and the user's `comp_vector`/`flags`,
  via the **landed** `MLX5_IB_METHOD_VFMIG_QUERY_CQ(handle)` (see
  ?5.2.4 for full rationale). The QUERY emits a 32-byte payload
  byte-equal to `struct mlx5_ib_restore_cq_req` so the CRIU plugin
  can `memcpy` it into protobuf at dump and back into the
  `RESTORE_CQ` UHW tail at restore with no field-level marshaling.
* **Discovery (CC)**: `LIST_UOBJS(UVERBS_OBJECT_COMP_CHANNEL)` (K2).
  Returns handles only.
* **Xref**: CQ -> CC via `XR_COMP_CHANNEL` if the CQ was created with a
  comp channel.
* **Restore order**: CC before CQ.
* **Kernel verb (CC)**: `UVERBS_METHOD_RESTORE_COMP_CHANNEL(target_handle)`
  -> `ib_dev->ops.restore_comp_channel()`. **Deferred -- not part of v0
  S5; lives in the future fd-bearing-uobjects micro-stage (S5c) alongside
  `RESTORE_ASYNC_EVENT` (S8).** Both share the per-fd allocation +
  CRIU-fd-table-reinstall plumbing, so they land together once the
  `ib_write_bw -e` event-mode milestone (S10) becomes the active goal. v0
  CQs that were bound to a comp channel on the source side cannot be
  restored end-to-end until S5c lands; CRIU plugin policy at v0 is
  "checkpoint only CQs created without a comp channel."
* **Kernel verb (CQ)**: `UVERBS_METHOD_RESTORE_CQ(target_handle, cqe,
  user_handle, comp_vector, flags?, comp_channel?, event_fd?, blob)`,
  with `RESP_CQE` returned out. **Landed at S5 A1 (`a77cc4d8e8b9`).**
  No core `cqn_hint` attr: the CQ identifier is not part of any
  RDMA-spec wire packet, so drivers with FW-side cqn (mlx5_vfmig, S5
  B-series) carry the source's cqn through their UHW payload (`struct
  mlx5_ib_restore_cq_req.cqn`) and the driver enforces continuity
  internally. Drivers with no cqn concept (rxe) allocate a fresh pool
  slot. K8a's NLDEV `RES_HANDLE` for the restored CQ matches
  `RESTORE_CQ_HANDLE` (the dispatcher-reserved ufile handle), so
  user-visible identity continuity is preserved out of band by the
  same mechanism PD/MR use. `comp_channel?` is declared `UA_OPTIONAL`
  for forward compat with S5c, but the v0 dispatcher hard-rejects with
  `-EOPNOTSUPP` if any caller passes one (so future plugins don't have
  to dance around stale plugin policy when S5c lands). `event_fd?`
  resolves via `ib_uverbs_get_async_event()` -> ufile default
  `async_file` when absent (the v0 path; S8 will let it resolve to an
  explicit restored async-event uobject without a UAPI bump).

#### 5.2.4 CQ dump-side discovery: `MLX5_IB_METHOD_VFMIG_QUERY_CQ`

CRIU's CQ dump phase needs five fields the destination's
`RESTORE_CQ` will consume (`buf_addr`, `db_addr`, `cqn`,
`cqe_size`, plus the per-CQ core inputs `cqe`/`comp_vector`/
`flags`). Three are FW properties (`cqn`, `cqe_size`,
`cqe`/`cqe_cnt`) and are process-independent. Two -- `buf_addr`
and `db_addr` -- are the **source userspace VAs** that the
destination kernel uses to look up the LOAD_VHCA_STATE-installed
KIND_CQ / KIND_DBR placeholders.

**Why neither library introspection nor netlink works**

* `mlx5dv_init_obj(MLX5DV_OBJ_CQ)` reads `dvcq.{buf, dbrec}` out of
  libmlx5's struct `mlx5_cq` -- but those are the **calling
  process's** userspace VAs. CRIU runs in its own address space;
  the libmlx5 struct it could reach via dlsym holds CRIU's VAs,
  not the dumpee's. The fields are useful only when the caller
  *is* the dumpee (as in `fw_id_continuity_probe`,
  `cq_query_probe_mlx5_vfmig` -- single-process probes).
* `ibv_import_cq` does not exist in upstream rdma-core. There is
  no public verb to manufacture an `ibv_cq*` in CRIU's address
  space against the dumpee's underlying kernel CQ, so we cannot
  even feed `mlx5dv_init_obj` a handle resolved from CRIU.
  Compare to `ibv_import_pd` / `ibv_import_mr`, which exist but
  whose man pages explicitly call out that imported objects carry
  no userspace mapping (`addr` field is documented as "NULL value
  is expected").
* Extending NLDEV with `user_addr` TLVs would scope the leak too
  widely: NLDEV is `CAP_NET_ADMIN`-gated and visible across every
  netns. CRIU is the only known consumer and it already holds the
  dumpee's `uverbsfd` (the same fd it uses for `INFO_HANDLES` /
  K8a `RES_HANDLE` joins), so a uverbs ioctl gives the right
  security boundary: "if you can see the ucontext, you can read
  its metadata." Same argument ?7.7 made for not extending NLDEV
  with the analogous MR fields.
* Parasite-injected `mlx5dv_init_obj` inside the dumpee
  (compel-style) is technically possible but brittle (libmlx5
  internal struct ABI is not stable) and adds parasite-side
  library plumbing CRIU does not currently carry; rejected as a
  v0 path. `process_vm_readv` against libmlx5's `struct mlx5_cq`
  is even more brittle (libmlx5 internal, not part of rdma-core
  ABI) and is rejected outright.

**Why driver-private under VFMIG, not core `QUERY_CQ`**

?7.7 extended core `UVERBS_METHOD_QUERY_MR` with two new
`UA_OPTIONAL` outs (`USER_ADDR`, `ACCESS_FLAGS`) backed by two
new fields on `struct ib_mr` itself. That worked because
`user_addr` and `access_flags` are *core* properties of every
user MR, present on every provider. CQ is structurally
different: of the five fields we need to round-trip,

| field | kernel storage | scope |
|-------|----------------|-------|
| `cqe` (entries-1) | `struct ib_cq.cqe` | core |
| `cqe_size` | `mlx5_ib_cq.cqe_size` | mlx5-only |
| FW `cqn` | `mcq->mcq.cqn` | mlx5-only |
| `buf_addr` | `mcq->buf.umem->address` | mlx5-only (rxe has no user umem) |
| `db_addr` | `mcq->db.u.user_page->user_virt` | mlx5-only |

four of five live on `mlx5_ib_cq`, and rxe's CQ has no source
userspace VA at all (rxe queues are kernel-allocated and exposed
via `vm_ops`; the rxe `RESTORE_CQ` UHW carries `vm_pgoff` not
`buf_addr` -- see `drivers/infiniband/sw/rxe/rxe_mmap.c:118`).
Promoting `buf_addr`/`db_addr`/`cqe_size` to `struct ib_cq` would
either misuse the abstraction (rxe writes 0s) or require a
per-driver hook anyway. The clean shape is therefore a
**driver-private** method under `MLX5_IB_OBJECT_VFMIG`, with rxe
landing its own `RXE_METHOD_VFMIG_QUERY_CQ` (out of v0 scope --
rxe restore doesn't consume source VAs the same way) when the
CRIU plugin grows rxe support.

**Landed shape**

`include/uapi/rdma/mlx5_user_ioctl_cmds.h`:

```c
enum mlx5_ib_vfmig_methods {
    MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT = (1U << UVERBS_ID_NS_SHIFT),
    MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT,
    MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS,
    MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS,
    MLX5_IB_METHOD_VFMIG_QUERY_CQ,    /* new */
};

enum mlx5_ib_vfmig_query_cq_attrs {
    MLX5_IB_ATTR_VFMIG_QUERY_CQ_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
    MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_BLOB,        /* mlx5_ib_restore_cq_req, 32B */
    MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_CQE,         /* u32, ibcq->cqe */
    MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_COMP_VECTOR, /* u32, mcq->mcq.vector */
    MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_FLAGS,       /* u32, cq->create_flags */
};
```

The HANDLE is `UVERBS_ATTR_IDR(UVERBS_OBJECT_CQ,
UVERBS_ACCESS_READ)` -- the calling fd's ufile-idr must own this
CQ, the IDR pins the uobject for the duration of the call. Same
security boundary as `INFO_HANDLES(UVERBS_OBJECT_CQ)`.

**Byte-equal payload contract.** `RESP_BLOB` is byte-equal to
`struct mlx5_ib_restore_cq_req` (32 bytes). The kernel handler
zeroes `reserved`/`reserved2` so the round-trip into RESTORE_CQ's
"must be 0" guards passes verbatim. CRIU plugin code at the
seam reduces to:

```c
/* dump phase */
ioctl(uverbsfd, RDMA_VERBS_IOCTL, &query_cq_cmd);
img.cq[i].blob        = blob;          /* 32B verbatim */
img.cq[i].cqe         = resp_cqe;
img.cq[i].comp_vector = resp_comp_vector;
img.cq[i].flags       = resp_flags;

/* restore phase */
restore_cq_cmd.uhw_in.data = (uintptr_t)&img.cq[i].blob;
restore_cq_cmd.uhw_in.len  = sizeof(img.cq[i].blob);
restore_cq_cmd.cqe         = img.cq[i].cqe;
restore_cq_cmd.comp_vector = img.cq[i].comp_vector;
restore_cq_cmd.flags       = img.cq[i].flags;
ioctl(dst_uverbsfd, RDMA_VERBS_IOCTL, &restore_cq_cmd);
```

No field-level marshaling, no `mlx5dv` anywhere.

**Kernel-mode CQ rejection.** The handler rejects kernel-mode
CQs with `-ENXIO` (`mcq->buf.umem == NULL` or
`mlx5_ib_db_user_virt(&mcq->db) == 0`). Kernel-mode CQs have no
source userspace state to emit; CRIU does not own them and
should not see them in `INFO_HANDLES(CQ)` anyway (kernel CQs are
not in any user ufile's idr). The check is defensive belt &
suspenders.

**`mlx5_ib_db_user_virt` accessor.** `struct mlx5_ib_user_db_page`
is private to `drivers/infiniband/hw/mlx5/doorbell.c`. The
QUERY_CQ handler reaches the doorbell user-virt via a small
read-only accessor (`u64 mlx5_ib_db_user_virt(const struct
mlx5_db *db)`) declared in `mlx5_ib.h` and implemented in
`doorbell.c`. The accessor returns 0 for kernel-mode db slots
(union's `pgdir` branch). Returning `u64` (not `unsigned long`)
matches the UAPI seam (`db_addr` is `__aligned_u64`); the
kernel-internal `page->user_virt` stays `unsigned long` to match
the rest of the doorbell pipeline (`mlx5_ib_db_map_user`'s
`virt`, `ib_umem.address`, the doorbell-page-list dedup key).
Designed to also serve future `MLX5_IB_METHOD_VFMIG_QUERY_QP`
and `_QUERY_SRQ`, which both have a DBR-page round-trip.

**`mcq.vector` symmetry fix in create path.** Pre-existing
asymmetry: `mlx5_ib_create_cq` did not set `cq->mcq.vector` after
calling `mlx5_comp_eqn_get(vector, &eqn)` -- the FW got the
right `cqc.c_eqn_or_apu_element` but the kernel-side bookkeeping
field stayed zero. `mlx5_ib_restore_cq` already set it on the
adoption path, and a comment in
`drivers/net/ethernet/mellanox/mlx5/core/cq.c::mlx5_core_adopt_cq`
explicitly called the create-side gap "we mirror" (i.e. neither
path sets it). QUERY_CQ needs it set to faithfully echo the
user's source-side `comp_vector`, so we close the gap with a
one-line `cq->mcq.vector = vector;` after the eqn lookup. Side
effect: any other introspection path that reads `mcq.vector`
post-create now sees the user's hint instead of 0; the fix is
purely additive and aligns the create path with the restore
path's existing semantics.

**Validator.** `tools/testing/mlx5_vfmig/uobject_restore/cq_query/
cq_query_probe_mlx5_vfmig.c` is the single-process byte-equality
probe. It creates real CQs via `ibv_create_cq`, reads view (A)
via `mlx5dv_init_obj(MLX5DV_OBJ_CQ)`, reads view (B) via
`MLX5_IB_METHOD_VFMIG_QUERY_CQ`, and asserts strict equality
across `cqn`, `cqe_size`, `buf_addr`, `(db_addr & PAGE_MASK)`,
`cqe`, `comp_vector`, `flags`, plus the two reserved-zero
contracts. Subtests cover happy path, invalid-handle (-ENOENT),
multi-CQ disambiguation, and `comp_vector=1` echo (gates the
create-path symmetry fix above). Empirically **STRONG PASS** on
ConnectX (mlx5_2, FW 28.48.1000) -- once the byte-equal contract
holds, a CRIU dumper that cannot run `mlx5dv_init_obj` from its
own address space (because `dvcq.{buf, dbrec}` would be CRIU's
VAs) substitutes the QUERY_CQ ioctl on the dumpee's `uverbsfd`
and gets the same bytes.

### 5.3 QP

#### 5.3.1 v0 state coverage

Each IBTA-defined QP state is handled, deferred, or refused at v0:

| state | v0 status | rationale |
|------|-----------|-----------|
| RESET | landed | trivial: create QP at qpn, no attrs |
| INIT  | landed | create + stamp INIT-set attrs |
| RTR   | landed | create + stamp RTR-set attrs (subset of RTS path) |
| **RTS-with-content** | **landed** | the R3 v0 milestone; queue contents preserved, PSNs preserved or captured per-driver |
| ERR   | refused | dispatcher returns -EOPNOTSUPP (`uverbs_std_types_restore.c` qp_state switch). A QP that the source captured in ERR has flushed its queues already; a v0 RESTORE_QP that adopts the FW QPC verbatim would land in ERR with no recovery lever (the landed handler has no `modify_qp` chain -- see ?6.3). CRIU pre-suspend coverage refuses processes holding ERR-state QPs at v0 by querying `qp_state` via NLDEV `RES_QP_GET` (or libibverbs `ibv_query_qp`); cheap because the same precheck already filters SQD/SQE. Land as RESET-then-modify post-v0 if a real workload needs it. |
| SQD   | refused | transient drain state; CRIU plugin must hold a barrier and wait for SQD->RTS or SQD->ERR before checkpoint |
| SQE   | refused | UD-only send-error (UC is parked at v0 type-gate). Same argument as ERR: dispatcher returns -EOPNOTSUPP; CRIU pre-suspend filters via `ibv_query_qp`. Can land post-v0 as RESET+modify(SQE-equivalent) if needed. |
| unknown | refused | mid-transition or driver-internal-only state; plugin rejects |

QP type coverage at v0 is **RC + UD only**; UC is parked at the
dispatcher gate (`uverbs_std_types_restore.c` qp_type switch
returns -EOPNOTSUPP) until a `qp_restore` probe subtest exercises
it. The FW QPC is preserved byte-equal across LOAD regardless of
QP type (see K7 STRONG PASS), so re-enabling UC is a one-line
revert plus a probe subtest that verifies the UC-specific subset
(no ack timer, no retry counters) round-trips. CRIU pre-suspend
coverage at v0 must therefore filter UC processes alongside ERR /
SQD / SQE.

Multicast attachments (UD), RD_ATOMIC slot table state, alt-path
attrs, and AH lifecycle for UD WRs are deferred (see ?5.6 for AH
which lands at S7 alongside SRQ).

#### 5.3.2 Discovery

* **NLDEV** `RES_QP_GET` + K1 PDN-join: type, port, qp_state,
  src/dst PSN, src/dst QPN, ctxn. Hw-agnostic `ib_qp_attr` fields
  not on NLDEV come from the existing `IB_USER_VERBS_CMD_QUERY_QP`
  (no kernel change).
* **Plugin** adds the per-driver dump-side blob via
  `*_METHOD_VFMIG_QUERY_QP(handle)` (?5.3.4). The blob is byte-
  equal to the matching driver's `*_ib_restore_qp_req` UHW
  payload, mirroring the QUERY_CQ/RESTORE_CQ contract from
  ?5.2.4.

* **Xrefs**: PD (XR_PARENT_PD), send_cq (XR_SEND_CQ), recv_cq
  (XR_RECV_CQ), optionally SRQ (XR_SRQ -- deferred until
  QP-without-SRQ + `ib_write_bw` is empirically passing).
* **Restore order**: after PD, CQ, MR. SRQ + SRQ-bound QPs land
  as a follow-up after the unsharded QP path is proven (?9.1 S6
  vs S7 sequencing).

#### 5.3.3 Single-shot restore: full attr capture, no `ib_modify_qp` chain

The kernel restore verb does not climb the IBTA state machine.
The driver handler stamps every captured attr field directly
onto a fresh `*_ib_qp` and sets `qp->state` to the captured
final state, exactly like `mlx5_ib_restore_cq` adopts a CQ
context without ever calling `ib_modify_cq`. Two consequences:

1. SAVE captures the **full current attr set** (`ib_qp_attr`-
   shaped) plus `qp_init_attr.create_flags` (immutable post-
   create), plus the captured final `qp_state`. No per-
   transition mask history is needed -- the static IBTA
   transition table never enters the picture.
2. RESTORE applies all captured attrs in one driver call. No
   `ib_modify_qp(INIT)` -> `(RTR)` -> `(RTS)` chain. The
   driver bypasses the state-table validation in
   `core/verbs.c::qp_state_table` because the verb sits below
   that layer (same liberty `RESTORE_CQ` already takes).

```
UVERBS_METHOD_RESTORE_QP(target_handle, init_attr,
                         qp_attr_full, captured_qp_state,
                         pd_handle, send_cq_handle,
                         recv_cq_handle, srq_handle?,
                         uhw_blob /* per-driver */)
```

The `uhw_blob` carries the wire-format-relevant data the core
attrs cannot express: source FW qpn, queue umem source VAs
(mlx5) / queue mmap vm_pgoff (rxe), PSNs / wqe_index (rxe), ECE
(mlx5).

`qp_init_attr.create_flags` is captured verbatim and replayed
through `init_attr.create_flags`. v0 policy: if the dst kernel
does not recognise a flag bit (newer-source-than-dst), reject
loud rather than silently zero.

#### 5.3.4 CRIU dump-side discovery: `*_METHOD_VFMIG_QUERY_QP`

Mirrors ?5.2.4's QUERY_CQ rationale verbatim, with QP-shaped
fields. Closes the same CRIU dump-side gap: CRIU runs in its
own address space, so `mlx5dv_init_obj(MLX5DV_OBJ_QP)` returns
CRIU's VAs not the dumpee's; `ibv_import_qp` does not exist in
upstream rdma-core (and the adjacent `ibv_import_dm` /
`ibv_import_mr` / `ibv_import_pd` verbs that DO exist all
document `addr` as "NULL value is expected" -- imported objects
explicitly carry no userspace mapping, so they would not solve
the address-space gap even if a hypothetical `ibv_import_qp`
existed: the dumpee's `dvqp.{rq.buf, dbrec, sq_buf}` VAs are
exactly what we need, and an imported QP would not expose
them); NLDEV is the wrong scope (CAP_NET_ADMIN-gated,
cross-netns). The verb is driver-private under
`MLX5_IB_OBJECT_VFMIG` (and a future `RXE_OBJECT_VFMIG` for
rxe) because the byte-exact wire format is driver-specific:

* **mlx5** emits `mlx5_ib_restore_qp_req` (FW qpn, send/recv
  buf source VAs, DBR source VA, source UAR index for the SQ
  doorbell, ECE bits). Kernel reads `mqp->qpn`,
  `mqp->buf.umem->address` (or per-queue umem if the driver
  splits SQ/RQ buffers), `mlx5_ib_db_user_virt(&mqp->db)` (the
  accessor lifted at S5b B5 already serves QP), `mqp->ece`. The
  S6 B5 step adds whatever per-mlx5 fields are needed beyond
  the CQ-extracted set.
* **rxe** emits `rxe_restore_qp_req` (sq_vm_pgoff, rq_vm_pgoff,
  req.psn, resp.psn, req.wqe_index, optional pending-completion
  state -- TBD at A1). All fields are kernel-internal to rxe;
  there is no userspace parallel of `mlx5dv_init_obj` for rxe,
  so the verb is the only reasonable path.

Same security boundary as QUERY_CQ -- HANDLE attr is
`UVERBS_ATTR_IDR(UVERBS_OBJECT_QP, UVERBS_ACCESS_READ)`. Caller
must own the QP via the ucontext's ufile-idr; dispatcher pins
the uobject for the call.

**CRIU plugin v0 contract.** The plugin must enforce the
following at pre-suspend (via `ibv_query_qp` + a single
`mlx5dv_init_obj(MLX5DV_OBJ_QP)` per QP, both already in the
plugin's dump path) and at restore time:

* **SRQ-bound QPs are refused.** The dispatcher accepts
  `RESTORE_QP_SRQ_HANDLE` as `UA_OPTIONAL` for forward
  compatibility but the v0 mlx5 handler returns -EOPNOTSUPP
  if the attr is present (`mlx5_ib_restore_qp` early-return
  on `uverbs_attr_is_valid(SRQ_HANDLE)`). Discovering this
  at the kernel ioctl is too late -- by then the dumpee's PD
  / CQs / send_cq / recv_cq are all restored. CRIU pre-suspend
  must filter via `ibv_query_qp` (the returned `ibv_qp_attr`
  has no SRQ slot but `ibv_qp::srq` is non-NULL on SRQ-bound
  QPs); reject the dump with a clear message.
* **QP type filter: RC + UD.** UC, XRC_INI, XRC_TGT,
  RAW_PACKET, GSI, DRIVER (DCT/DCI) all return -EOPNOTSUPP
  from the dispatcher. UC is parked specifically (see ?5.3.1);
  the others are not on the v0 roadmap.
* **QP state filter: RESET / INIT / RTR / RTS.** ERR / SQD /
  SQE / unknown all return -EOPNOTSUPP from the dispatcher.
  See ?5.3.1 for the per-state rationale.
* **`resp_qpn` identity assert.** RESTORE_QP echoes the
  adopted qpn as `RESP_QPN`. For mlx5_vfmig the FW resource-id
  continuity is the whole point of the SAVE/LOAD path; the
  plugin must hard-assert `resp_qpn == requested_qpn` (where
  requested_qpn is the source's `dvqp.qp_id` captured at
  dump). For rxe (post-v0) the kernel allocates from rxe's
  own qpn pool and the assert relaxes to "any qpn the rxe
  driver accepted". Same shape as the cqn / mkey identity
  asserts the CQ/MR plugin paths already do.
* **Source `devx_uid` must be 0.** Pre-suspend the plugin
  reads `meta.devx_uid` from `MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT`
  (kernel commit "RDMA/mlx5: Expose source devx_uid via
  VFMIG_QUERY_UCONTEXT"); a non-zero value indicates a
  DEVX-enabled source ucontext (e.g. libmlx5 auto-DEVX). v0
  cannot restore those: `LOAD_VHCA_STATE` does NOT preserve the
  FW uctx-registration table (§S3b "DEVX-adoption blind spot"),
  so neither the destination's ADOPT_DEVX_UID path (alloc
  ucontext fails at ALLOC_TRANSPORT_DOMAIN with "unknown uid")
  nor the uid=0 host-priv lane (which empirically only covers
  CREATE_*, DESTROY_PD, DESTROY_CQ for cross-uid resources --
  DESTROY_QP / 2RST_QP modify against a uid != 0-owned QPC
  silently fail at FW with destroy_qp_common warn-only-logging
  to dmesg) can complete the destroy chain on uid != 0 FW
  resources -- the user-visible failure surfaces only at the
  first errno-propagating downstream opcode, typically
  `DEALLOC_PD bad_resource_state` (syndrome 0xef0c8a-class). The
  plugin must refuse the dump with a clear "VFMIG v0 does not
  support DEVX-enabled source ucontexts; await future-FW
  uctx-registry preservation or a per-resource uid rebind path"
  message. As defense-in-depth the kernel includes
  `meta.devx_uid` in the `RESTORE_UCONTEXT` strict-equality
  check, so even a buggy plugin pair that gets to RESTORE_UCONTEXT
  on a `devx_uid != 0` source fails -EINVAL there (early, before
  any `restore_pd/cq/qp` runs) instead of obscurely at teardown.

  > **Update (2026-06):** parts of the analysis in this paragraph
  > were superseded by the empirical work in
  > [`pd_registration_wipe.md`](pd_registration_wipe.md). In
  > particular: (a) `DESTROY_QP` and `2RST_QP` empirically DO
  > honor cross-uid lanes (the silent-fail framing was wrong --
  > see `qp_destroy_matrix/`); (b) `DESTROY_CQ` (after dropping
  > dependent QPs) and `DESTROY_MKEY` also honor cross-uid lanes
  > (`cq_destroy_matrix/`, `mr_destroy_matrix/`); (c) the
  > `DEALLOC_PD bad_resource_state syndrome 0xef0c8a` failure is
  > independent of the DEVX/non-DEVX source-uid story -- it's the
  > (pdn -> owner_uid) registration table that gets wiped by
  > `LOAD_VHCA_STATE`, not just the (uid -> uctx_attrs) one, and
  > the same syndrome is returned for definitely-bogus pdns. The
  > kernel-side mitigation landed in `mlx5_ib_dealloc_pd` (gated
  > on `mpd->vfmig_restored` plus the exact "PDN unknown"
  > status/syndrome tuple) closes the failure path for v0.
  >
  > **Update (2026-06, follow-up):** the strict-equality
  > `meta.devx_uid` check in `RESTORE_UCONTEXT` (commit
  > `c659ab66483d`) was relaxed to log-and-continue. With the
  > `mlx5_ib_dealloc_pd` gate in place and the empirically-proven
  > cross-uid behaviour of `DESTROY_QP`/`DESTROY_CQ`/`DESTROY_MKEY`,
  > the strict check is no longer load-bearing AND it actively
  > rejects the common case of a default `libmlx5` ucontext, where
  > `source.devx_uid != dest.devx_uid` by construction (each
  > `ibv_open_device` auto-allocates a fresh DEVX uid). The
  > standard-verbs data path through such ucontexts works fine
  > post-restore -- doorbells and completions are HW-only and do
  > not consult the FW registration tables. Mismatch is now logged
  > via `mlx5_ib_dbg` ("VFMIG_RESTORE_UCONTEXT: devx_uid mismatch
  > tolerated"). DEVX-direct manipulation of restored objects
  > remains out of scope for v0.

**Landed shape (mlx5)**

`include/uapi/rdma/mlx5_user_ioctl_cmds.h`:

```c
enum mlx5_ib_vfmig_methods {
    MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT = (1U << UVERBS_ID_NS_SHIFT),
    MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT,
    MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS,
    MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS,
    MLX5_IB_METHOD_VFMIG_QUERY_CQ,
    MLX5_IB_METHOD_VFMIG_QUERY_QP,    /* new */
};

enum mlx5_ib_vfmig_query_qp_attrs {
    MLX5_IB_ATTR_VFMIG_QUERY_QP_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
    MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_BLOB,         /* mlx5_ib_restore_qp_req, 64B */
    MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_TYPE,         /* u32, mqp->type */
    MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_STATE,        /* u32, mqp->state */
    MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_USER_HANDLE,  /* u64, ib_qp_user_handle */
    MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_CAP,          /* struct ib_uverbs_qp_cap */
    MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_CREATE_FLAGS, /* u32, mqp->flags */
};
```

The HANDLE is `UVERBS_ATTR_IDR(UVERBS_OBJECT_QP,
UVERBS_ACCESS_READ)` -- the calling fd's ufile-idr must own this
QP, the IDR pins the uobject for the duration of the call. Same
security boundary as `INFO_HANDLES(UVERBS_OBJECT_QP)`.

**Byte-equal payload contract.** `RESP_BLOB` is byte-equal to
`struct mlx5_ib_restore_qp_req` (64 bytes). The kernel handler
zeroes `reserved`/`reserved2` so the round-trip into RESTORE_QP's
"must be 0" guards passes verbatim, and emits sentinels for the
three FW-side fields LOAD_VHCA_STATE preserves (`uidx = 0`,
`bfreg_index = MLX5_IB_INVALID_BFREG`, `ece_options = 0`) -- all
three are validated-but-discarded by `mlx5_ib_restore_qp` since
the QPC's `user_index` / `uar_page` / `ece_options` round-trip
intact across LOAD per the K7 byte-equal proof. CRIU plugin code
at the seam reduces to:

```c
/* dump phase */
ioctl(uverbsfd, RDMA_VERBS_IOCTL, &query_qp_cmd);
img.qp[i].blob          = blob;          /* 64B verbatim */
img.qp[i].type          = resp_type;
img.qp[i].state         = resp_state;
img.qp[i].user_handle   = resp_user_handle;
img.qp[i].cap           = resp_cap;
img.qp[i].create_flags  = resp_create_flags;

/* restore phase */
restore_qp_cmd.uhw_in.data    = (uintptr_t)&img.qp[i].blob;
restore_qp_cmd.uhw_in.len     = sizeof(img.qp[i].blob);
restore_qp_cmd.type           = img.qp[i].type;
restore_qp_cmd.state          = img.qp[i].state;
restore_qp_cmd.user_handle    = img.qp[i].user_handle;
restore_qp_cmd.cap            = img.qp[i].cap;
restore_qp_cmd.create_flags   = img.qp[i].create_flags;
ioctl(dst_uverbsfd, RDMA_VERBS_IOCTL, &restore_qp_cmd);
```

No field-level marshaling, no `mlx5dv` anywhere.

**Type gate.** v0 accepts the IBTA QP types whose mlx5_ib
representation lives in `mlx5_ib_qp.trans_qp` *and* whose
restore path is exercised by the qp_restore probe. v0 set is
RC + UD; UC is parked at the dispatcher gate (-EOPNOTSUPP)
until a UC subtest lands -- see ?5.3.1 for the reasoning.
Other types (raw_packet uses `raw_packet_qp`; XRC INI/TGT, GSI,
DCT, DCI use other union arms) reject with `-EOPNOTSUPP` --
mirror of the v0 type set `mlx5_ib_restore_qp` accepts and
matches the same v0-scope reasoning that motivated S6b B2's
dispatcher rejects.

**Kernel-mode QP rejection.** The handler rejects kernel-mode
QPs with `-ENXIO` (`trans_qp.base.ubuffer.umem == NULL` or
`mlx5_ib_db_user_virt(&mqp->db) == 0`). Kernel-mode QPs have no
source userspace state to emit; CRIU does not own them and
should not see them in `INFO_HANDLES(QP)` anyway (kernel QPs are
not in any user ufile's idr). The check is defensive belt &
suspenders, mirror of the QUERY_CQ kernel-mode reject.

**Cap is best-effort.** mlx5 doesn't track every cap field on
user-mode QPs (see `mlx5_ib_query_qp` in `qp.c`: for user QPs
`max_send_wr / max_send_sge` are 0 because those values are
libmlx5-internal post-rounding). The handler emits a useful
echo (`max_send_wr = sq.wqe_cnt`, `max_recv_wr = rq.wqe_cnt`,
`max_recv_sge = rq.max_gs`, `max_inline_data = max_inline_data`,
`max_send_sge = 1`) -- the mlx5 RESTORE_QP handler doesn't
validate cap content (the actual WQ shape comes from the UHW's
`{sq,rq}_wqe_count`); the field is forward-compat surface for a
future driver that may consult it.

**`ib_qp_user_handle` accessor.** `struct ib_qp.uobject` is
opaquely typed `struct ib_uqp_object *` (XRC bookkeeping requires
the embedded `ib_uevent_object` rather than a bare `ib_uobject`);
the struct definitions live in `drivers/infiniband/core/uverbs.h`
and are not exported via `<rdma/*>`. The QUERY_QP handler reaches
the userspace tag via a small accessor (`u64
ib_qp_user_handle(const struct ib_qp *qp)`) declared in
`<rdma/ib_verbs.h>` and implemented in `core/verbs.c`,
EXPORT_SYMBOL'd for module use. Returns 0 for kernel-mode QPs
(`qp->uobject == NULL`). CQ has the same dump-side verb shape
but does not need a parallel accessor: `ib_cq.uobject` is
`struct ib_uobject *` directly (no XRC wrapper), so QUERY_CQ
already reads `mcq->ibcq.uobject->user_handle` via core's
existing layout.

**Validator.** `tools/testing/mlx5_vfmig/uobject_restore/qp_query/
qp_query_probe_mlx5_vfmig.c` is the single-process byte-equality
probe. It creates real QPs via `ibv_create_qp`, reads view (A)
via `mlx5dv_init_obj(MLX5DV_OBJ_QP)`, reads view (B) via
`MLX5_IB_METHOD_VFMIG_QUERY_QP`, and asserts strict equality
across `qpn`, `sq_wqe_count`, `rq_wqe_count`, `rq_wqe_shift`,
`buf_addr`, `(db_addr & PAGE_MASK)`, `type`, `state`,
`create_flags` plus the sentinel-zero contracts on
`sq_buf_addr`, `uidx`, `ece_options`, `reserved`, `reserved2`.
Subtests cover happy path (RESET -> INIT for non-trivial
state), invalid-handle (-ENOENT), multi-QP disambiguation,
type echo (RC vs UD distinct resp_type), and state echo
(RESET -> INIT round-trip via `ibv_modify_qp`). Build-clean
against current libibverbs + libmlx5; full byte-equal
contract validation against ConnectX hardware deferred to the
next hardware run (compiles + lint-clean on this branch).

#### 5.3.5 PSN preservation: per-driver split

* **mlx5_vfmig**: trust FW. The QPC `next_send_psn`,
  `next_rcv_psn`, `last_acked_psn`, sq/rq head/tail pointers,
  WQE buffer MKEY references, AV (primary_address_path),
  retry/timeout counters, MTU, and pending-WR state all
  round-trip losslessly across `SAVE_VHCA_STATE` /
  `LOAD_VHCA_STATE` -- empirically validated 2026-06-01 by
  `MLX5_VFMIG_IOC_QUERY_QP` + `test_fw_id_continuity.sh` with
  `K6_QP_STATE={INIT,RTR,RTS}` self-loopback, byte-equal across
  the full §6.3 / S6 QPC subset (state-independent + INIT-set +
  RTR-set + RTS-set, including the 44-byte
  `primary_address_path`). No driver-side PSN, AV, or attr-byte
  bookkeeping in the RESTORE_QP handler. The user-mode WQE
  buffer is preserved by `user_mr_dma` at the IOMMU layer
  (Stage-2 source-side retag was wired at `c0183184ad82` for
  the QP umem callsite).

* **rxe**: explicit kernel-stored PSNs go through the UHW.
  `rxe_qp.req.psn` (next-PSN-to-send), `rxe_qp.resp.psn`
  (next-PSN-to-receive-and-ACK), and `rxe_qp.req.wqe_index`
  (the requester's cursor into the SQ ring) are captured at
  SAVE via the per-driver QUERY_QP verb (?5.3.4) and replayed
  at RESTORE by directly assigning into the fresh `rxe_qp`
  before kicking the worker tasks. On restore, the rxe arm of
  the plugin sends a synthetic ACK to the peer at the saved
  `resp.psn` so the peer retransmits everything past that PSN
  -- the same behaviour the peer already exhibits for routine
  packet loss. To the peer, our restore looks like a momentary
  network blip. No co-checkpoint requirement on the peer side.
  In-flight outgoing sends with no captured ACK are
  re-transmitted by the requester from `req.psn` on its next
  task tick; the peer dedups via its PSN-window logic.

  Single timer kick (`rxe_run_task(&qp->req.task)` after the
  synthetic-ACK send is queued) is enough -- no captured
  retransmit-timer phase.

#### 5.3.6 Queue contents preservation: per-driver split

Both drivers preserve byte-equal queue contents (WQEs, CQEs,
inline data) across SAVE/RESTORE without a memcpy hook -- but
through different mechanisms:

* **mlx5_vfmig**: queues are user-allocated umems mapped via
  the existing user_mr_dma stage-2 retag (KIND_QP for the
  per-QP umem buffer) + KIND_DBR (for the doorbell page). LOAD-
  side replay installs placeholders at the source VAs;
  `mlx5_ib_umem_restore_qp` (analog of `mlx5_ib_umem_restore_cq`
  from S5b B3) pins user pages and binds them at the
  placeholder IOVA range. Same shape as MR/CQ.

* **rxe**: queues are kernel-allocated `vmalloc_user` buffers
  remapped into user via `remap_vmalloc_range`
  (`rxe_queue.c` + `rxe_mmap.c`). Userspace and kernel read/
  write the same `rxe_queue_buf` pages, so CRIU's user-VMA
  snapshot already includes the queue contents and the on-buffer
  `producer_index`/`consumer_index`. Restore lifts the CQ
  vm_pgoff round-trip pattern (`forced_offset` parameter on
  `rxe_create_mmap_info`, landed for CQ at `e48f4e378a55`):

  ```
  struct rxe_restore_qp_req {
      u64 sq_vm_pgoff;    /* source's mminfo.offset for SQ */
      u64 rq_vm_pgoff;    /* source's mminfo.offset for RQ */
      u32 req_psn;        /* qp->req.psn at SAVE */
      u32 resp_psn;       /* qp->resp.psn at SAVE */
      u32 req_wqe_index;  /* qp->req.wqe_index at SAVE */
      /* ... per-state attrs not on ib_qp_attr ... */
  };
  ```

  `rxe_restore_qp` calls `rxe_create_mmap_info(forced_offset=
  req.sq_vm_pgoff)` for SQ + similar for RQ. CRIU's pie restorer
  mmaps the user VMA at the original VA -> maps to the fresh
  vmalloc pages. CRIU's page-restore writes the snapshot bytes
  -> lands in those pages. Result: the new
  `rxe_queue_buf.{producer,consumer}_index` and `data[]` are
  byte-equal to source with no kernel memcpy.

  One small additional sync vs. the CQ pattern: `rxe_queue` has
  a kernel-private `q->index` (the rxe-owned copy of whichever
  index the driver advances; see `rxe_queue.h:71` comment). The
  restore handler propagates `buf->consumer_index` -> `q->index`
  for `QUEUE_TYPE_FROM_CLIENT` (SQ) and `buf->producer_index`
  -> `q->index` for `QUEUE_TYPE_TO_CLIENT` (CQ-direction-on-
  consumer-side; not relevant for QP). Plus
  `qp->req.wqe_index` from the captured PSN block.

#### 5.3.7 RXE `FREEZE_DATAPATH` verb (rxe-only, ucontext-scope)

Between CRIU's `SIGSTOP` of the dumpee's user threads and the
user-VMA dump, the rxe kernel worker kthreads (`req`, `resp`,
`comp` -- see `rxe_qp.c::*->task`) are not paused. They can
advance `consumer_index`, dispatch retransmits, and update
kernel-private indices -- racing against the plugin's per-
uobject QUERY_* calls and against CRIU's VMA snapshot. The fix
is an explicit, non-destructive pause primitive at the
ucontext layer:

```
RXE_METHOD_VFMIG_FREEZE_DATAPATH
    in:  (none -- ucontext implicit via fd)
    out: (none)
```

Kernel impl in `rxe_qp.c`:

```c
int rxe_qp_pause(struct rxe_qp *qp);     /* sets qp->paused, drains workers */
int rxe_qp_resume(struct rxe_qp *qp);    /* clears qp->paused, kicks workers */
```

`qp->paused` is a new flag that the three task workers check
at the top of their per-iteration loop and, if set, return
without running. The verb iterates the ucontext's QPs and
calls `rxe_qp_pause` on each, waiting for in-progress
iterations to complete via the existing `rxe_task` synch.
**No state-machine transition** (does not touch
`qp->attr.qp_state` or `qp->valid`); does not interfere with
the existing destructive `qp->valid = false` shutdown path or
the IBTA-state-machine `rxe_qp_drain` path.

Lifecycle in the source-side dump flow:

```
1. orchestrator: prepare migration
2. CRIU: SIGSTOP all dumpee threads
3. CRIU: invoke RDMA plugin per uverbs fd
4. plugin (rxe arm): RXE_METHOD_VFMIG_FREEZE_DATAPATH    <-- here
   plugin (mlx5 arm): no-op (FW SAVE_VHCA_STATE freezes)
5. plugin: INFO_HANDLES + per-uobj QUERY_*
6. orchestrator: SAVE_VHCA_STATE (mlx5 only)
7. CRIU: dump user VMAs (now stable)
8. CRIU: kill source process
```

There is no thaw verb. The dumpee is killed at step 8; the
fresh `rxe_qp` allocated at restore time on the destination
starts unpaused by construction. A standalone `rxe_qp_resume`
helper exists for testing / future symmetry.

**No symmetric mlx5 verb.** SAVE_VHCA_STATE freezes the FW
datapath as part of its own contract; mlx5 has no equivalent
of rxe's worker kthreads racing the user. The CRIU plugin's
mlx5 arm skips the freeze step entirely (different driver
namespace -> different ioctl dispatch -> driver-shaped
plugin). A no-op verb under `MLX5_IB_OBJECT_VFMIG` would be
API-surface tax for nothing.

#### 5.3.8 Path validity at restore

Assuming the orchestrator preserves IP/GID/MAC continuity per
?8.1 (symmetric setup or hint-file-driven for mlx5_vfmig;
`rdma link add ... type rxe netdev <X>` against the original
IP for rxe), the QP restore handler validates:

* **path_mtu**: AV-stamped `path_mtu` must be <= dst port's
  `active_mtu`. Reject loud at `RESTORE_QP` if not.
* **GID type per index**: GID at `sgid_index` must have the
  same type (RoCE-v1 vs RoCE-v2 vs IB) on src and dst. Mismatch
  changes UDP sport hashing for RoCE-v2 -> peer drops.
  Orchestrator hint-file extension; `RESTORE_QP` re-reads the
  port's GID table type for the captured `sgid_index` and
  rejects on mismatch.
* **Resolved peer dmac in AV** (`wr.av.dmac` for rxe; mlx5's
  AV cache): v0 simplification assumes peer has not moved. If
  peer also migrated, ARP/neighbor cache will re-resolve on
  first send. Multi-VM coordinated migration is a follow-up.

Items already covered upstream: AV-stamped dlid/dgid pointing
at the (unmoved) peer is still valid; src MAC of outgoing
packet is determined by the dst NIC's HW MAC and is not
validated by peer for ACK matching (peer matches by QPN+PSN);
UDP sport hash is determined by QP attrs which are preserved.

### 5.4 MR

* **Discovery**: NLDEV `RES_MR_GET` gives the per-MR ufile handle
  (K8a `RDMA_NLDEV_ATTR_RES_HANDLE`) plus mrlen and (with
  CAP_NET_ADMIN) rkey/lkey/iova; ctxn derived via PDN-join (one-hop
  after K1). The wire-restore-relevant attrs not on NLDEV --
  `user_addr` (the user VA the MR was registered against) and
  `access_flags` -- come from the extended
  `UVERBS_METHOD_QUERY_MR` on `UVERBS_OBJECT_MR` (see ?7.7 for
  the rationale). Plugin adds FW mkey context via
  `MLX5_IB_METHOD_VFMIG_QUERY_MR(handle)`.
* **Xref**: PD (XR_PARENT_PD).
* **Restore order**: after PD.
* **Kernel verb**: `UVERBS_METHOD_RESTORE_MR(target_handle, virt_addr,
  length, access_flags, lkey_hint, rkey_hint, blob, pd_handle)`. Honours
  rkey/lkey as identity hints.
* **Driver-side (mlx5_vfmig)**: see also `user_mr_dma.md` stage 3
  (landed), which is the user-MR-DMA-side counterpart to this
  verb. R3 owns the uobject identity; user_mr_dma stage 3 owns
  the IOMMU-side mkey-keyed page binding. The two landed
  together as a coherent per-MR restore: `mlx5_ib_restore_mr`
  (S4b B2) calls `mlx5_ib_umem_restore_mr` (Stage-3 D3), which
  composes `ib_umem_pin` (D1) + `mlx5_vfmig_bind_user_mr` (D3)
  + `vfmig_iova_bind_user_object` (D2) inside the RESTORE_MR
  verb body -- the destination's `mr->umem` is real and bound
  to the source-emitted IOVA before the verb returns. Plugin
  side: zero changes from the S4b shape; no separate
  post-restore IOMMU-bind hook needed (see ?6.2).
* **Driver-side (rxe)**: standard `rxe_reg_user_mr` + identity-hint on
  mkey allocation. rxe is a software provider, so the user pages re-pin
  via `pin_user_pages_fast` against the destination process's mm; no
  IOMMU work needed.

### 5.5 SRQ

* **Discovery**: NLDEV `RES_SRQ_GET`, ctxn derived via PDN-join (one-hop
  after K1). Yields type (BASIC/XRC), max_wr/max_sge. Plugin adds FW
  srqn + buffer map.
* **Xref**: PD (XR_PARENT_PD), optionally CQ (XR_SRQ has a CQ in some
  paths; check at impl time).
* **Restore order**: after PD, CQ.
* **Kernel verb**: `UVERBS_METHOD_RESTORE_SRQ(target_handle, init_attr,
  blob, pd_handle, cq_handle?)`.

### 5.6 AH

* **Discovery**: `LIST_UOBJS(UVERBS_OBJECT_AH)` (K2). Returns handles plus
  `struct rdma_ah_attr` per handle (cheap to extract from the AH uobject's
  cached attrs without touching the data path).
* **Xref**: PD (XR_PARENT_PD).
* **Restore order**: after PD.
* **Kernel verb**: `UVERBS_METHOD_RESTORE_AH(target_handle, ah_attr,
  blob, pd_handle)`. AH has no wire-visible identity to honour; always
  succeed.
* **Driver-side**: trivially calls `rdma_create_ah_user` against the
  resolved PD with the saved ah_attr.

### 5.7 Async event fd

* **Discovery**: `LIST_UOBJS(UVERBS_OBJECT_ASYNC_EVENT)` (K2).
  Important: a ucontext is **not** limited to a single async event file
  -- there's the implicit default from `GET_CONTEXT` plus zero-or-more
  explicit ones allocated via `UVERBS_METHOD_ASYNC_EVENT_ALLOC`
  (`drivers/infiniband/core/uverbs_std_types_async_fd.c`). K2-driven
  enumeration walks `ufile->uobjects` filtered by type and returns
  every AEF handle, so we capture all of them. The "derive from
  ucontext" shortcut is incorrect for any process that uses the
  explicit allocator.
* **Xref**: implicit parent ucontext.
* **Restore order**: after the ucontext is up. Independent of all other
  uobjects.
* **Kernel verb**: `UVERBS_METHOD_RESTORE_ASYNC_EVENT(target_handle)`
  -> `ib_dev->ops.restore_async_event()`. Allocates the
  `ib_uverbs_async_event_file` uobject + fd. CRIU installs the fd at
  the user's saved number.

### 5.8 Comp channel fd

Already covered in ?5.2 alongside CQ.

## 6. CRIU plugin integration

### 6.1 Per-ucontext discovery flow

```
For each pid in dump tree:
    For each uverbs-class fd:
        ctxn = parse_fdinfo(/proc/<pid>/fdinfo/<fd>)         # K5
        plugin = claim_uverbs_context(ibdev, kernel_driver_id)
        ufile_state = plugin.dump_uverbs_context(ctxn)        # existing
        inv = plugin.discover_uobjs(ctxn) or
              criu_rdma_discover(ctxn)                        # default
        For each uobj in inv:
            blob = plugin.query_uobj(ctxn, uobj.type, uobj.handle)
            emit_image(rdma_uobj_entry(uobj, blob))
```

`discover_uobjs` is the optional plugin override; default is the inline
`criu_rdma_discover()` helper which does the NLDEV+LIST_UOBJS walk.

### 6.2 Topological sort + handle map

CRIU userspace, at restore time:

```
For each ufile in image (in dependency-free order across ufiles):
    plugin.open_uverbs_cdev(ufile)                           # existing
    handle_map = {}                                          # restrack_id -> ufile_handle
    sorted_uobjs = topo_sort(ufile.uobjs by xrefs)
    For each uobj in sorted_uobjs:
        resolved_xrefs = {role: handle_map[ref.target_restrack]
                          for ref in uobj.xref}
        new_handle = ioctl(cdev_fd, RESTORE_<TYPE>,
                           target=uobj.ufile_handle,
                           attrs=uobj.<type>,
                           blob=uobj.plugin_blob,
                           **resolved_xrefs)
        handle_map[uobj.restrack_id] = new_handle
```

v0 ships **zero** plugin post-restore hooks. The original sketch
needed a `post_restore_mr` hook because MR restore couples with
`user_mr_dma.md` stage 3 (rkey continuity at the IOMMU layer)
and the kernel verb couldn't itself re-bind the per-MR IOVA map.
Stage-3 D3+D4 fold the bind into the verb body
(`mlx5_ib_restore_mr` -> `mlx5_ib_umem_restore_mr` ->
`ib_umem_pin` + `mlx5_vfmig_bind_user_mr`), so the plugin path
collapses to a single ioctl per uobject for every v0 class.
CRIU core handles fd-table install for CC/AEF directly; no
plugin hook needed there either. If a future per-class quirk
surfaces (DM/DEVX, dma-buf, or a mlx5e-style netdev coupling),
we add a hook at that point.

### 6.3 Restore-fini activation pass

After all per-ufile restores are complete (and all peer state is in place,
implicit by the dump tree being one connected unit):

```
For each ufile in image:
    For each QP in ufile (in QP creation order):
        # Already restored to its saved qp_state via the per-uobj
        # single-shot restore (?5.3.3 -- no INIT->RTR->RTS chain;
        # the driver handler stamps every captured attr field and
        # sets qp->state to the captured final state in one call).
        # Fini pass:
        rearm_cqs(qp.recv_cq, qp.send_cq)
        if device.driver == rxe and qp.qp_state == IB_QPS_RTS:
            # rxe-only: send synthetic ACK at saved resp.psn so peer
            # retransmits everything past that PSN. See ?5.3.5.
            rxe_qp_resume(qp)         # clears qp->paused, kicks workers
            rxe_send_synthetic_ack(qp, qp.resp_psn)
```

Working assumption (now empirically confirmed; see below):
**pending RQ/SQ WRs survive `LOAD_VHCA_STATE` intrinsically.** The
QP's WQE buffers and FW-side head/tail pointers are part of the VHCA
state the FW saves and restores; nothing user-visible needs to re-post
them. Indirect evidence:

* Production SR-IOV LM (the `drivers/vfio/pci/mlx5/` path) preserves QP
  state end-to-end without any user-visible WR-replay machinery. If FW
  didn't carry RQ/SQ pending WRs across `LOAD_VHCA_STATE`, the vfio LM
  path would have had to either query and re-post WRs, or the migration
  would lose them silently -- neither is observed.
* The QP context FW save format is documented as carrying head/tail
  pointers and the WQE buffer's MKEY; the user-space WQE buffer itself
  is in user memory, which `user_mr_dma` preserves at the IOMMU layer.

Direct empirical evidence (committed alongside this design --
`MLX5_VFMIG_IOC_QUERY_QP` PF cdev ioctl + `test_fw_id_continuity.sh`
piggyback driven by `K6_QP_STATE={INIT,RTR,RTS}` self-loopback):

```
On host A (source):
  (1) fw_id_continuity_probe alloc PD/CQ/QP(RC)/MR.
  (2) Bring the VF netdev up so the RoCE GID table populates with
      the link-local IPv6 GID auto-derived from the netdev MAC.
  (3) Drive QP through RESET -> INIT -> [RTR -> [RTS]] via SELF-
      LOOPBACK: dest_qpn = own qpn, dgid = first non-zero local
      GID, sgid_index = its slot. We never post send WRs; the
      RTR/RTS transition alone is enough to populate the AV /
      retry / PSN / MTU fields in the QPC.
  (4) Optionally post N receive WRs (post_recv_wrs).
  (5) PF cdev QUERY_QP @ qpn -> record wider QPC subset (32+
      individual fields plus the 44-byte primary_address_path).
  (6) SAVE_VHCA_STATE while the probe holds the QP alive.

On host B (dest, same host in our loopback test):
  (7) tear down source VF, fresh dest VF, LOAD_VHCA_STATE,
      MARK_RESTORED, bind.
  (8) PF cdev QUERY_QP @ same qpn -> record QPC subset.
  (9) Byte-equal compare across the whole subset relevant to the
      achieved qp_state.
```

Result (2026-06-01, ConnectX-7, single PF self-loopback):

```
K6_QP_STATE=INIT  ->  PASS on the state-independent + INIT-set core
                     (state, pd, q_key, uar_page,
                      log_{page,sq,rq}_size, log_msg_max,
                      user_index, cqn_snd, cqn_rcv,
                      srqn_rmpn_xrqn, hw/sw sq_wqebb_counter,
                      hw/sw rq_counter, next_rcv_psn, pkey_index)

K6_QP_STATE=RTR   ->  PASS on the above + RTR-set
                     (path_mtu, min_rnr_nak, log_rra_max,
                      remote_qpn, primary_address_path:
                      44 bytes byte-equal -- dgid, sgid_index,
                      dlid/mlid, sl, port, dmac, hop_limit,
                      tclass, flow_label, udp_sport, ack_timeout,
                      eth_prio)

K6_QP_STATE=RTS   ->  PASS on the above + RTS-set
                     (log_sra_max, retry_count, rnr_retry,
                      next_send_psn, last_acked_psn)
```

The `primary_address_path` byte-blob includes the link-local IPv6
GID (`fe80...`) the FW derived from the netdev's MAC; src and dst
share a MAC because both VFs are vf0 of the same PF. Cross-host
migration moves this proof onto the orchestrator's "consistent VF
MAC" contract which is independent of S6 (?5.3.8).

Conclusion: the **entire** FW QPC round-trips losslessly across
`SAVE_VHCA_STATE` / `LOAD_VHCA_STATE`, including every state-
dependent field S6b RESTORE_QP would otherwise have had to replay.
No driver-side `QUERY_QP_PENDING_WRS` + replay path is needed for v0.

What this means for the S6b mlx5_vfmig RESTORE_QP handler design:

* The handler does NOT need to issue `MODIFY_QP(RST -> INIT -> RTR
  -> RTS)` on the destination. The QPC is already at the captured
  qp_state when the dest VF binds.
* The handler does NOT need to carry attribute bytes (path_mtu,
  retry_count, AV, PSNs, ...) through user-visible UAPI. The FW
  preserves them. RESTORE_QP only needs (a) the source qpn, (b)
  per-queue umem source VAs for adoption, (c) the doorbell-page
  source VA, and -- for the userspace mlx5dv-internal path --
  the ECE word.
* The single-shot restore model in ?5.3.3 is the simpler of the
  two design options, and is empirically justified by the RTS
  byte-equal result above.

The SQ/RQ user buffers and the DB page are an independent question
(they live in user memory, not the QPC). They survive via the
`vfmig_iova` `HOST_USER_PAGE`-replay machinery (Stage-2 C4/C5 emit +
LOAD-time placeholder install) bound on RESTORE_QP via the
`mlx5_ib_umem_restore_<class>` wrapper pattern S4b uses for the MR
umem. ?6.3 is **closed**.

Relationship between the per-uobj `RESTORE_QP` handler's
`modify_qp` chain and the fini pass: there is no `modify_qp`
chain. RESTORE_QP adopts the FW QPC verbatim at the state the
source captured. K7 (STRONG PASS at INIT/RTR/RTS) proves
LOAD_VHCA_STATE preserves the entire QPC -- state, PSNs, AV,
retry counters, MTU -- byte-equal across SAVE/LOAD. The mlx5
handler stamps `qp->state = qp_state` from the UAPI, the FW
already has `qpc.state = qp_state` from the inherited state
blob, and there is nothing for a kernel-side state-machine
chain to do.

This commits to **policy (a)**: the captured state IS the
restored state. No fini-pass `modify_qp(RTS)` is needed for QP
state. The fini pass's job for QP is limited to CQ re-arm and
plugin-private bookkeeping (e.g. WR-replay, if a future stage
adopts that policy).

Why not (b) (land in RTR, modify(RTS) at fini): the landed
mlx5_ib_restore_qp handler has no `modify_qp` lever -- it does
not call into the IBTA state machine at all. Implementing (b)
would require either:

* lying to the handler (pass `qp_state = IB_QPS_RTR` while
  the FW QPC says RTS) -- creates a kernel/FW state mismatch
  the next legitimate `ibv_modify_qp(RTS)` would FW-reject
  with bad-state-transition; or
* a follow-on commit that lets RESTORE_QP optionally drive
  `modify_qp(INIT) -> modify_qp(RTR) -> modify_qp(RTS)` while
  the FW is already past RTR -- material work that contradicts
  the K7 byte-equal preservation invariant we already lean on.

The "peer state may not yet be in place" worry that originally
motivated (b) does not actually argue for (b) in a CRIU snapshot:

* In single-host snapshot/restore (the v0 use case), the peer
  QP's QPC is also being restored from the same dump tree, so
  by the time fini runs the peer is in the same state the
  source captured -- no peer-readiness delay vs. (a).
* In cross-host migration (post-v0), peer-readiness is
  asynchronous regardless of our local sub-state. Whether we
  hold the local QP at RTR or let it go to RTS, the peer
  doesn't know we've moved until network sync; a misaddressed
  packet hitting an RTR peer is just as wasteful as one hitting
  an RTS peer that's mid-recv-WR-replay. Holding RTR locally
  saves nothing.

Implication for CRIU: the plugin's RESTORE_QP must pass the
captured `qp_state` verbatim. No "land at RTR then bump to RTS
in fini" half-measure.

### 6.4 Plugin-private subgraph

For uobjects CRIU's generic discovery doesn't model (DM/DEVX in v0+1):

```
inv += plugin.list_private_uobjs(ctxn)        # plugin-defined enumeration
For each priv_uobj in inv (from plugin):
    emit_image(rdma_uobj_entry(type=PRIVATE, plugin_blob=...))
```

The `priv_uobj` entries land in the same image stream with `type=PRIVATE`
and a plugin-defined sub-blob. At restore, after all generic uobjects
are up, CRIU calls `plugin.restore_private_uobj()` per entry. The
plugin owns the entire path; CRIU is a transport.

For v0 this code path is dormant; the pre-suspend coverage check rejects
processes holding any private uobj type a plugin doesn't claim.

### 6.5 Orchestrator-hint file

CRIU adds an optional CLI flag:

```
--rdma-orchestrator-hints=<path>
```

Where `<path>` is a directory or a single file containing operator-asserted
destination state. The file is a small JSON or key-value blob CRIU's RDMA
core (and plugins) read at restore-init. Schema sketch:

```json
{
    "ibdev_remap": {
        "rxe1": "rxe7"
    },
    "port_state": {
        "mlx5_2": {
            "port": 1,
            "gid_table": [
                {"index": 0, "value": "fe80::1234:5678:9abc:def0",
                 "type": "RoCE_v2"}
            ],
            "pkey_table": [
                {"index": 0, "value": "0xffff"}
            ]
        }
    }
}
```

If the flag is absent, CRIU asserts strict symmetry between dump-image
port state and destination port state. Any mismatch fails loudly with
a clear diagnostic (which GID/PKey/ibdev didn't match, what hint would
satisfy it).

Port-state ownership is **not** keyed on the EXCLUSIVE / SHAREABLE
plugin classification (which is a dump-side cross-tree property,
see §1.4).  Under the KS7.4 / §S6b resolution model
([`vf_prerestore_split.md`](vf_prerestore_split.md) §4.6), the
**orchestrator** owns destination-side L2/L3 setup uniformly: it
runs `ip link set <PF> vf <VF_ID> mac <…>`, `ip addr add <…>`, and
`ip neigh replace <…>` to mirror the source's per-VF identity onto
the destination *before* `LOAD_VHCA_STATE`.  The kernel
auto-populates the RoCE GID table from netdev IPs as a side effect
of `ip addr add`, so by the time the CRIU plugin's `init(RESTORE)`
runs, the GID table already reflects the orchestrator's intent.

The plugin's job, regardless of EXCLUSIVE / SHAREABLE
classification, is therefore to **validate** that the populated
GID/PKey state matches the dump image (or the hint file's
overrides), and fail loud if it doesn't.  Both mlx5_vfmig and rxe
use the same code path here; what differs is *who set up the port
on the destination* (orchestrator's `ip` commands for mlx5_vfmig;
orchestrator's `rdma link add rxe…` for rxe), not whether CRIU's
plugin layer is doing the install.

(An earlier draft of this section sketched the plugin calling
`RDMA_NLDEV_CMD_SYS_SET` to install GIDs at requested indices.
That sketch was based on a model where a kernel-side post-LOAD
helper would also be needed to refresh `path.rmac_*` -- a model
the §S6b post-mortem (`qp_av_dmac_swap.md` Appendix A §12) ruled
out as architecturally infeasible, after which the entire port
setup migrated orchestrator-side.  The plugin retains no
GID-install path on either driver.)

## 7. Driver-side changes (kernel asks K2-K4 for v0 + K1 cleanup)

### 7.1 K1: NLDEV CTXN emission completeness (cleanup, not v0-blocking)

```diff
 static int fill_res_qp_entry(...) {
     ...
     if (!rdma_is_kernel_res(res) &&
         nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_PDN, qp->pd->res.id))
         return -EMSGSIZE;
+    if (!rdma_is_kernel_res(res) &&
+        nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_CTXN,
+                    qp->uobject->uevent.uobject.context->res.id))
+        return -EMSGSIZE;
     ...
 }
```

Without K1, CRIU joins QP/MR/SRQ to ctxn through their PDN against the
PD inventory it already gets with CTXN. The join is correct for v0 (every
user-mode QP/MR/SRQ has a parent PD that's in the same inventory), so
this is a cleanup not a blocker. K1 land order is independent of the
rest of R3.

Same shape for `fill_res_mr_entry`, `fill_res_srq_entry`,
`fill_res_cm_id_entry`. Mirrors the existing PD / CQ emitters at
nldev.c:743 and :656.

No new ABI; `RDMA_NLDEV_ATTR_RES_CTXN` already exists (line 103). Adds
~4 LOC per fill function.

### 7.2 K2: generic LIST_UOBJS uverbs method (already upstream)

**Status (2026-05-13): no new kernel work required.** The primitive
this section proposed already exists upstream as
`UVERBS_METHOD_INFO_HANDLES` on `UVERBS_OBJECT_DEVICE`. From
`drivers/infiniband/core/uverbs_std_types_device.c`:

```c
DECLARE_UVERBS_NAMED_METHOD(
    UVERBS_METHOD_INFO_HANDLES,
    UVERBS_ATTR_CONST_IN(UVERBS_ATTR_INFO_OBJECT_ID,
                         enum uverbs_default_objects, UA_MANDATORY),
    UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_INFO_TOTAL_HANDLES,
                        UVERBS_ATTR_TYPE(u32), UA_OPTIONAL),
    UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_INFO_HANDLES_LIST,
                        UVERBS_ATTR_MIN_SIZE(sizeof(u32)),
                        UA_OPTIONAL));
```

Handler walks `ufile->uobjects` under `uobjects_lock`, filtered by
`obj->uapi_object == uapi_get_object(uapi, hdr.object_id)`. The
`uapi_get_object()` lookup goes through `uapi_key_obj()` which already
encodes the namespace bit, so the same ioctl path accepts both core
(`UVERBS_OBJECT_AH`, `UVERBS_OBJECT_ASYNC_EVENT`, `UVERBS_OBJECT_XRCD`,
???) and driver-namespace object ids (`MLX5_IB_OBJECT_UAR`,
`MLX5_IB_OBJECT_DEVX_*`, ???) uniformly. That's exactly the surface CRIU
needs for both the per-type enumeration and the DEVX/MW/FLOW/XRCD
coverage check.

Empirically validated end-to-end by
`tools/testing/mlx5_vfmig/uobject_restore/info_handles/info_handles_probe.c`. Run output on
ConnectX-6 Dx, `mlx5_0`, after allocating 3 PDs + 2 CQs + 2 MRs +
2 QPs via libibverbs:

```
PASS PD:   3 handle(s) match libibverbs view
PASS CQ:   2 handle(s) match libibverbs view
PASS MR:   2 handle(s) match libibverbs view
PASS QP:   2 handle(s) match libibverbs view
PASS AH-empty: 0 handles
PASS SRQ-empty: 0 handles
PASS MLX5_IB_OBJECT_UAR: 2 handle(s)  -- driver namespace accepted
PASS PD-saturation: TOTAL=1 with cap=1
```

Caveats to record for the CRIU consumer:

* The kernel returns the **filled** count (= `min(real_total, capacity)`),
  not the true total. Callers detect saturation by `total ==
  capacity` and retry with a bigger buffer; otherwise the
  enumeration is complete. There is no "sizing-only" mode (the
  kernel rejects `LIST` len <= 0 with `-EINVAL`).
* `hdr->driver_id` must match the bound device's `uapi->driver_id`
  even though INFO_HANDLES is a core-namespace method
  (`ib_uverbs_cmd_verbs()` enforces this unconditionally at
  uverbs_ioctl.c:570). Trivial, but easy to miss.
* `obj->uapi_object` is set at uobject commit time and never mutated;
  the filter is therefore stable for the duration of the spinlocked
  walk. No need to revalidate identity post-walk.

The K6 sibling finding -- that current libmlx5 dispenses dynamic
`MLX5_IB_OBJECT_UAR` uobjects on a per-QP basis (the probe observed
N_QP=2 produced N_UAR=2) -- means S3/S4/S5/S6 will see UAR uobjects in
the ucontext alongside the user-visible PD/CQ/MR/QP. The restore path
already covers UAR via the existing `MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS`
plumbing, but the enumeration order must restore UARs **before** any
QP that references them. The R3 DAG already encodes this because the
QP create ABI carries `bfreg.uar` as an input, so the dependency edge
is implicit in the per-uobj blob.

For v0, `INFO_HANDLES` is consumed by CRIU for:
* every uobject class enumerated at dump (NLDEV restrack covers
  PD/CQ/QP/MR/SRQ/CM_ID but **not** AH, ASYNC_EVENT, COMP_CHANNEL,
  XRCD, DM, COUNTERS, DMAH; for those `INFO_HANDLES` is the primary
  enumeration source); and
* the pre-suspend coverage check (enumerate DEVX/MW/FLOW/XRCD; if
  any non-empty, refuse the dump with a clear "v0 doesn't cover
  type X" message and the offending handles).

### 7.3 K3: UVERBS_OBJECT_RESTORE namespace + per-class methods

New uverbs object namespace, one method per uobject class. Lives in
`drivers/infiniband/core/uverbs_std_types_restore.c` (new).

**Restore-mode gate (design rationale).** The handler must reject
calls from ucontexts that were not opened in CRIU-restore mode --
otherwise a non-restore application could mint uobjects at
caller-chosen ufile handles, which is at minimum surprising and at
worst lets a malicious caller deny-of-service-collide future
`ALLOC_*` handles.

There are two natural designs for the gate:

1. **Sticky bit on `struct ib_ucontext`** -- e.g.
   `ucontext->flags & IB_UCONTEXT_RESTORE_MODE`. Cheap to test, but
   adds driver-flavoured state to a core struct that intentionally
   stays lean, and the driver still owns the truth (only the
   driver's `alloc_ucontext` knows whether the caller asked for
   restore mode).

2. **Per-driver predicate `ib_device_ops.ucontext_is_restore_mode`**
   -- a `bool (*)(struct ib_ucontext *)` callback the generic
   handler consults. Default `NULL` = "this driver does not
   implement CRIU restore, no ucontext on it may restore". Drivers
   that implement restore latch a sticky bool in their own
   per-ucontext storage at `alloc_ucontext` time and report it.

We chose (2). Reasons:

* The state is intrinsically driver-owned; (1) would just shadow
  the driver's truth into the core struct.
* The gate runs once per RESTORE_* method invocation (~6 calls per
  CRIU-restored ucontext); a virtual call is free at that rate.
* `ib_ucontext` stays lean. No new field on a struct shared by
  every RDMA driver in the tree.
* Opt-in default. A driver that hasn't been audited for the
  restore semantics cannot accidentally accept RESTORE_* calls
  just because someone passes a flag at GET_CONTEXT.

Concretely the per-driver predicate is wired as follows:

* **mlx5_ib**: `mlx5_ib_alloc_ucontext()` sets
  `mlx5_ib_ucontext.vfmig_restore_mode = true` when the caller
  passes `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE`. This bit is
  intentionally separate from the pre-existing
  `vfmig_restore_pending` bool: the latter is single-shot and
  cleared by `MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT` (the UAR
  snapshot consume), whereas `vfmig_restore_mode` is sticky for
  the ucontext's lifetime. The sticky bit MUST outlive
  `vfmig_restore_pending` because RESTORE_PD/CQ/QP/... run AFTER
  RESTORE_UCONTEXT (those resources need a usable UAR at
  hw-create time, so the UAR snapshot must already have seeded
  `bfregi->sys_pages[]`). `mlx5_ib_ucontext_is_restore_mode()`
  returns `vfmig_restore_mode`.
* **rxe**: new uapi `struct rxe_alloc_ucontext_req { __u32 flags;
  __u32 reserved; }` carried in `udata` to GET_CONTEXT, plus
  `RXE_ALLOC_UCTX_RESTORE_MODE = 1u << 0`. `rxe_alloc_ucontext()`
  parses the req when `udata->inlen > 0` (older librxe userspace
  passes `inlen=0` and is unaffected), validates unknown flags,
  and sets `rxe_ucontext.restore_mode = true` when present.
  `rxe_ucontext_is_restore_mode()` returns it.

**Status (2026-05-20): RESTORE_PD (`e06868342fce`), RESTORE_MR
(`57cf75f25a48` core + `f422e6ba6bdc` rxe-hint primitive), RESTORE_CQ
(`a77cc4d8e8b9`) landed.** The namespace and dispatcher infrastructure
described below is in place; RESTORE_QP / RESTORE_SRQ / RESTORE_AH /
RESTORE_COMP_CHANNEL / RESTORE_ASYNC_EVENT will reuse the exact same
pattern (UVERBS_ATTR_PTR_IN target_handle + per-class init params +
UHW driver blob).

Per-method shape -- shown for PD, the first landed concrete example.
The attr key is a plain `UVERBS_ATTR_PTR_IN` u32 carrying the target
ufile handle. We intentionally do NOT use `UVERBS_ATTR_IDR(...,
UVERBS_ACCESS_NEW)`: that mode lets the framework choose the next
free id, which is the opposite of what we need. Instead the handler
calls `rdma_alloc_begin_uobject_at_handle()` directly on the
caller-supplied u32, and does the commit/abort itself (the standard
ioctl dispatch only auto-finalises declared IDR attrs).

```c
DECLARE_UVERBS_NAMED_METHOD(
    UVERBS_METHOD_RESTORE_PD,
    UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_PD_HANDLE,
                       UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
    UVERBS_ATTR_UHW());
```

Handler (abbreviated -- see
`drivers/infiniband/core/uverbs_std_types_restore.c`):

```c
static int UVERBS_HANDLER(UVERBS_METHOD_RESTORE_PD)(
    struct uverbs_attr_bundle *attrs)
{
    struct ib_ucontext *ctx;
    struct ib_device   *ib_dev;
    struct ib_uobject  *uobj;
    struct ib_pd       *pd;
    u32 target_handle;
    int ret;

    /* Per-driver predicate gate; opt-in default. */
    ret = restore_check_ucontext(attrs, &ctx);
    if (ret)
        return ret;
    ib_dev = ctx->device;
    if (!ib_dev->ops.restore_pd)
        return -EOPNOTSUPP;

    ret = uverbs_copy_from(&target_handle, attrs,
                           UVERBS_ATTR_RESTORE_PD_HANDLE);
    if (ret)
        return ret;

    /* Atomic xa_insert; -EBUSY if handle taken. */
    uobj = rdma_alloc_begin_uobject_at_handle(attrs, UVERBS_OBJECT_PD,
                                              target_handle);
    if (IS_ERR(uobj))
        return PTR_ERR(uobj);

    pd = rdma_zalloc_drv_obj(ib_dev, ib_pd);
    /* ... pd->device / pd->uobject / restrack_new / restrack_set_name ... */

    ret = ib_dev->ops.restore_pd(pd, target_handle, &attrs->driver_udata);
    if (ret)
        goto err_restrack;
    rdma_restrack_add(&pd->res);

    uobj->object = pd;
    rdma_alloc_commit_uobject(uobj, attrs);
    return 0;

    /* error paths: rdma_restrack_put + kfree(pd);
     *               rdma_alloc_abort_uobject(uobj, attrs, false). */
}
```

**Caller-specified handle plumbing (K2.5).** The "install at exactly
the caller's user_handle, fail with `-EBUSY` if taken" primitive
already exists in `drivers/infiniband/core/rdma_core.c`:

```c
struct ib_uobject *rdma_alloc_begin_uobject_at_handle(
    struct uverbs_attr_bundle *attrs,
    u16 object_id, u32 target_handle);
```

It was added by the UAR restore work and implements the
XA-insert-at-handle behaviour with the right errno contract. Each
`RESTORE_<TYPE>` handler reuses this helper.

Future per-class methods (QP/SRQ/AH/COMP_CHANNEL/ASYNC_EVENT) will
mirror the shape above: one `UVERBS_ATTR_PTR_IN` u32 target handle,
plus whatever per-class init params are needed (e.g. QP will add
`pd_handle`, `send_cq_handle`, `recv_cq_handle`, init/modify attrs,
state). Total: 8 new methods (PD/MR/CQ landed, 5 remaining).

**RESTORE_CQ shape (S5 A1 landed concrete example).** CQ has no
parent-IDR xref (no PD on the modern CREATE_CQ path), so the dispatcher
takes only `UVERBS_ATTR_PTR_IN` u32s plus the optional FD-class attrs:

```c
DECLARE_UVERBS_NAMED_METHOD(
    UVERBS_METHOD_RESTORE_CQ,
    UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_CQ_HANDLE,
                       UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
    UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_CQ_CQE,
                       UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
    UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_CQ_USER_HANDLE,
                       UVERBS_ATTR_TYPE(__aligned_u64), UA_MANDATORY),
    UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR,
                       UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
    UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_CQ_FLAGS,
                       UVERBS_ATTR_TYPE(__u32), UA_OPTIONAL),
    UVERBS_ATTR_FD(UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL,
                   UVERBS_OBJECT_COMP_CHANNEL,
                   UVERBS_ACCESS_READ, UA_OPTIONAL),
    UVERBS_ATTR_FD(UVERBS_ATTR_RESTORE_CQ_EVENT_FD,
                   UVERBS_OBJECT_ASYNC_EVENT,
                   UVERBS_ACCESS_READ, UA_OPTIONAL),
    UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_RESTORE_CQ_RESP_CQE,
                        UVERBS_ATTR_TYPE(__u32), UA_MANDATORY),
    UVERBS_ATTR_UHW());
```

Three notable design choices, each load-bearing:

1. **No `cqn_hint`.** Unlike RESTORE_MR (which has `lkey_hint` /
   `rkey_hint` carrying wire-visible identity), the CQ identifier
   does not appear in any RDMA-spec wire packet -- receive
   completions are local to the receiver's HCA, and no SEND/RECV/RDMA
   header references cqn. So a "wire-identity" attr would be
   meaningless at the core level. Drivers that do have an FW-side
   cqn (mlx5_vfmig: `qpc.cqn_snd`/`cqn_rcv` reference cqn as a
   tracked FW resource) carry the source's cqn through their UHW
   payload (`struct mlx5_ib_restore_cq_req.cqn`, S5 B1 pending), and
   the driver's restore_cq enforces continuity internally. Drivers
   with no cqn concept (rxe) ignore the question entirely.
2. **`COMP_CHANNEL` declared `UA_OPTIONAL` but rejected at v0.** Comp
   channel restore (RESTORE_COMP_CHANNEL) is deferred to S5c
   alongside RESTORE_ASYNC_EVENT (S8); both share fd-table reinstall
   plumbing. Declaring the attr now and rejecting it with
   `-EOPNOTSUPP` keeps the UAPI shape stable -- when S5c lands no
   plugin needs to bump its caller-side ABI.
3. **`EVENT_FD` declared `UA_OPTIONAL`, default-fallback in
   dispatcher.** The dispatcher calls
   `ib_uverbs_get_async_event(attrs, UVERBS_ATTR_RESTORE_CQ_EVENT_FD)`
   exactly the way `UVERBS_METHOD_CQ_CREATE` does. Absent attr ->
   `ufile->default_async_file` (auto-allocated), which is the v0
   path. Present attr -> the explicit `UVERBS_OBJECT_ASYNC_EVENT`
   uobject the caller passed (S8 plugin path). Same helper, same
   ufile-state machinery, no driver-side awareness. Net effect at
   v0: the restored CQ's async events flow through the ufile default
   regardless of how the source had them routed; S8 lifts that
   limitation.

The handler dispatches through `ib_dev->ops.restore_cq(cq,
target_handle, &init_attr, &attrs->driver_udata)` after copying the
attrs into a local `struct ib_cq_init_attr` -- driver shape is the
existing `create_cq` signature plus the `target_handle` hint.

### 7.4 K4: ib_device_ops.restore_<type> callbacks

**Status (2026-05-20): restore_pd (`e06868342fce`), restore_mr
(`57cf75f25a48`), restore_cq (`a77cc4d8e8b9`) landed.** Shape
deliberately mirrors each driver's existing `alloc_pd` / `reg_user_mr`
/ `create_cq` plus an extra `u32 target_handle` hint and the standard
`ib_udata` for driver vendor-private bytes. The generic dispatcher has
already reserved the requested ufile handle via
`rdma_alloc_begin_uobject_at_handle()` by the time this callback is
invoked; the driver's job is just to make the `ib_<class>` hw-usable.
Drivers that ignore the hint behave identically to their alloc/create
counterpart. Drivers that consume it use it as the FW
allocate-with-id input.

```c
struct ib_device_ops {
    ...
    int (*restore_pd)(struct ib_pd *pd, u32 target_handle,
                      struct ib_udata *udata);
    int (*restore_mr)(struct ib_mr *mr, u32 target_handle,
                      u64 user_addr, u64 length, u64 iova,
                      int access_flags, u32 lkey_hint, u32 rkey_hint,
                      struct ib_udata *udata);
    int (*restore_cq)(struct ib_cq *cq, u32 target_handle,
                      const struct ib_cq_init_attr *attr,
                      struct ib_udata *udata);
    /* future: restore_qp, restore_srq, restore_ah,
     * restore_comp_channel, restore_async_event_file. Each mirrors
     * the shape of its existing alloc/create counterpart plus a
     * u32 target_handle hint. */
};
```

**Per-driver `restore_cq` behaviour (S5 A1):**

* **rxe**: `rxe_restore_cq` is intentionally near-identical to
  `rxe_create_cq` -- there is no rxe-side cqn that userspace can
  observe, so the `target_handle` hint is unused for hw-id purposes
  (it's already been honoured by the dispatcher's ufile-handle
  reservation; there's nothing further for the driver to do). The
  rxe pool slot for the restored CQ may differ from the source's,
  but pool slots are not exposed beyond restrack and are not part of
  any wire / userspace-API contract (no rxe-DV cqn equivalent).
* **mlx5_vfmig** (S5 B-series, pending): `mlx5_ib_restore_cq` will
  adopt the source's cqn from `struct mlx5_ib_restore_cq_req.cqn`
  carried through `udata` (Model A, mirroring `mlx5_ib_restore_pd` /
  `mlx5_ib_restore_mr`). cqn continuity is FW-state-coherence-load-
  bearing: `qpc.cqn_snd`/`cqn_rcv` of the source's preserved QPCs
  point at source-side cqn values, and S6 (RESTORE_QP) will need
  those CQs reachable at the same cqn on the destination.

Driver-side population:

* **rxe**: populated statically at module load via `rxe_set_device_ops`.
  Landed example: `rxe_restore_pd` is just `return rxe_alloc_pd(...)`
  -- rxe has no hw-side id whose value must round-trip, so the
  `target_handle` hint is intentionally unused. Future per-class
  callbacks will have the same shape (delegate to the existing
  alloc path).
* **mlx5_vfmig**: populated **once at VF probe time**, when
  `mlx5_vfmig_vf_consume_restored()` flips the device into
  `VFMIG_RESTORE` state, **before** any uverbs cdev is opened against
  the new `ib_device`. The restore-mode ops vector is installed via
  `ib_set_device_ops()` at that point and is read-only for the lifetime
  of the restored VHCA. **No mid-life ops swapping.** Concretely: the
  call lives in mlx5_ib's probe path (e.g. inside or just after
  `mlx5_ib_dev_res_init` on a restored VF), guarded by
  `mlx5_vfmig_is_restored(dev)`. Non-restored VFs and PFs are
  unaffected; mlx5's normal alloc paths remain in place for them.

Default if unset: kernel returns `-EOPNOTSUPP` from the generic verb,
which CRIU surfaces as "this driver doesn't support R3 restore yet".

### 7.5 K8: per-uobject ufile_handle exposure (landed)

**Status (2026-05-13): landed as `0601c496b413` via K8a (NLDEV emit).**
The K-ask was driven by the join problem: K3's `RESTORE_<TYPE>` methods
install at a caller-specified `target_handle` (= the per-ufile `obj->id`
user code holds in restored memory), so at dump time CRIU has to pair
`ufile_handle` with `restrack_id` (which encodes the parent-edge graph,
e.g. QP's `parent_pdn`). Before K8, the two kernel views couldn't be
joined:

* **NLDEV** emits `restrack_id` (`PDN`/`CQN`/`LQPN`/`MRN`/`SRQN`) per
  resource via `fill_res_<type>_entry`, but not `obj->id`.
* **`UVERBS_METHOD_INFO_HANDLES`** (K2) returns a flat `u32[]` of
  `obj->id` values per type per ufile, but no `restrack_id` alongside.

A QP entry could record `parent_pdn=5` (NLDEV) without any way to
translate "PDN 5 lives at ufile_handle 2" -- which is what
`target_handle` needs.

#### 7.5.1 K8a -- NLDEV emit `RES_HANDLE` (the path that landed)

Patch shape (`drivers/infiniband/core/nldev.c`):

```diff
 static int fill_res_pd_entry(struct sk_buff *msg, struct rdma_restrack_entry *res) {
     ...
     if (!rdma_is_kernel_res(res) &&
         nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_PDN, pd->res.id))
         goto err;
+    if (!rdma_is_kernel_res(res) &&
+        nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_HANDLE, pd->uobject->id))
+        goto err;
     ...
 }
```

Same shape repeated in `fill_res_cq_entry`, `fill_res_qp_entry`,
`fill_res_mr_entry`, `fill_res_srq_entry`. New UAPI attr
`RDMA_NLDEV_ATTR_RES_HANDLE` registered in `nldev_policy[]`. Each
emission gated by `!rdma_is_kernel_res(res)` so kernel-internal
restrack entries (no backing `ib_uobject`) keep their attribute set
unchanged.

Classes intentionally not touched (per the commit message): `ucontext`
(RES_CTXN already identifies it; "no handle within itself"), `cm_id`
(lives in the ucma fd namespace; no `ib_uobject` to take an id from),
`counter` (kernel-only stat counter state).

Validated end-to-end by
`tools/testing/mlx5_vfmig/uobject_restore/nldev_res_handle/nldev_res_handle_probe.c`,
which:

* allocates one of each `{PD, CQ, QP, MR, SRQ}` via libibverbs;
* runs `RDMA_NLDEV_CMD_RES_*_GET` dumps over a raw `NETLINK_RDMA`
  socket;
* finds each entry by `pid + ibdev`;
* asserts (a) `RDMA_NLDEV_ATTR_RES_HANDLE` is present, (b) its value
  exactly equals libibverbs's `obj->handle`, (c) kernel-internal
  entries (matched by `RES_KERN_NAME`) MUST NOT carry the new attr.

CRIU consumer side: `rdma_nl_for_each_resource()` in
`criu/rdma_netlink.c` parses each NLDEV per-resource entry attr-by-attr
in `parse_res_entry()`; populating `rdma_nl_res_entry.ufile_handle` from
`RDMA_NLDEV_ATTR_RES_HANDLE` is a few-line addition per type, with no
new ioctl plumbing.

#### 7.5.2 K8b -- extend `INFO_HANDLES` to pair restrack ids (rejected)

Recorded for posterity. The alternative was to extend
`UVERBS_METHOD_INFO_HANDLES` with an optional `UVERBS_ATTR_INFO_RESTRACK_LIST`
u32[] paired with `INFO_HANDLES_LIST`, so the same call returned both
keys in one go.

* Pros: more general -- extends naturally to any future restracked
  type without per-type fill changes.
* Cons: required CRIU to issue `INFO_HANDLES` per (ufile, type) for
  PD/CQ/QP/MR/SRQ on top of its existing per-device NLDEV walk. Net
  more ioctls per dump, larger uapi surface.

K8a was preferred and landed: smaller patch, symmetric with the
existing NLDEV per-resource layout, zero new CRIU dispatch.

### 7.6 One-plugin-per-port invariant

`ib_device_ops` is a per-device singleton. The current
`CR_PLUGIN_DECLARE_RDMA_PROVIDED_DRIVER` enum (RCD_RXE, RCD_MLX5_SRIOV_VFMIG)
maps 1:1 to per-device-ops sets. A future world with multiple plugin-types
per ibdev would need a per-context (not per-device) ops dispatcher;
explicitly out of scope. Doc records the invariant so future work doesn't
silently break it.

### 7.7 Extending QUERY_MR for CRIU dump-side discovery

CRIU's dump phase needs the MR's `user_addr` (the user VA the MR was
registered against) and `access_flags` so the restore-side can
re-feed them to `UVERBS_METHOD_RESTORE_MR`. Neither field is on the
core `struct ib_mr` historically, and neither is on NLDEV today.

**Why uverbs ioctl, not NLDEV.** NLDEV currently emits MR metadata
(lkey/rkey/iova/mrlen) behind a `CAP_NET_ADMIN` gate. We considered
extending NLDEV with `user_addr` + `access_flags` TLVs but that
scopes the leak too widely: any process with `CAP_NET_ADMIN` would
see every MR's user VA across every other process and netns, which
is more attack surface than CRIU needs. CRIU is the only known
consumer and CRIU already has the holder's `uverbsfd` open at dump
time (it `dup`s it from the seized victim, same path as the
`INFO_HANDLES`-based K2 discovery). The natural security boundary
is therefore "if you can see the MR's ucontext, you can read its
metadata" -- which is exactly what a uverbs ioctl gives us through
the standard `UVERBS_IDR_ANY_OBJECT` lookup.

**Landed shape.** The pre-existing `UVERBS_METHOD_QUERY_MR` (on
`UVERBS_OBJECT_MR`) is extended with two new UA_OPTIONAL out
attrs in `include/uapi/rdma/ib_user_ioctl_cmds.h`:

```c
enum uverbs_attrs_query_mr_cmd_attr_ids {
    UVERBS_ATTR_QUERY_MR_HANDLE,
    UVERBS_ATTR_QUERY_MR_RESP_LKEY,
    UVERBS_ATTR_QUERY_MR_RESP_RKEY,
    UVERBS_ATTR_QUERY_MR_RESP_LENGTH,
    UVERBS_ATTR_QUERY_MR_RESP_IOVA,
    UVERBS_ATTR_QUERY_MR_RESP_USER_ADDR,    /* new, UA_OPTIONAL */
    UVERBS_ATTR_QUERY_MR_RESP_ACCESS_FLAGS, /* new, UA_OPTIONAL */
};
```

The matching kernel storage lives on `struct ib_mr` itself (two
new fields, `user_addr` and `access_flags`) so the handler stays
core-only and no driver hook is needed. The fields are set by the
existing core MR-creating paths (`REG_MR`, `REG_DMABUF_MR`, legacy
write `IB_USER_VERBS_CMD_REG_MR`, `REREG_MR`) and the
`RESTORE_MR` dispatcher. For non-user MRs (DMABUF, DM, FR) where
no user VA exists, `user_addr` stays 0; userspace MUST read
`RESP_USER_ADDR == 0` as "MR has no user VA" rather than
"registered at NULL".

**Caller compat.** The two new attrs are `UA_OPTIONAL` so existing
QUERY_MR callers that only ask for the four legacy outs stay
byte-compatible.

**CRIU dump-side use.** Per MR found via `INFO_HANDLES(MR)`:
issue `QUERY_MR(handle)` on the same `uverbsfd`, capture the six
outs, store them in `rdma-uobj.img`. Restore-side feeds the same
six fields back into `UVERBS_METHOD_RESTORE_MR`'s IN attrs.

## 8. Cross-host caveats

### 8.1 GID / PKey port-level state

mlx5_vfmig is EXCLUSIVE on its VF (dump-side cross-tree exclusivity,
per §1.4 -- one workload per VF for v0).  Port GID/PKey state
ownership is a separate axis: under the §S6b resolution
([`vf_prerestore_split.md`](vf_prerestore_split.md) §4.6, KS7.4),
**the orchestrator** owns destination-side port setup uniformly,
running `ip link set <PF> vf <VF_ID> mac <…>` + `ip addr add <…>` +
`ip neigh replace <…>` *before* `LOAD_VHCA_STATE`.  The kernel
auto-populates the RoCE GID table from the IPs the orchestrator
adds; by the time the CRIU plugin's `init(RESTORE)` runs, the GID
table already reflects the orchestrator's intent.  R3:

* **Symmetric setup** (test rigs, normal LM): orchestrator runs the
  three `ip` commands to mirror source-time peer-VF identity onto
  the destination, the destination netdev gets the source-time
  IPs, and the kernel auto-populates RoCE GIDs at the same indices
  the source had.  CRIU plugin validates -- no-op match.
* **Asymmetric setup**: orchestrator hint file (§6.5) declares the
  destination's GID-index -> GID-value mapping that the plugin
  should expect (rather than the source's verbatim).  Plugin
  validates the populated GID table against the hint at
  `init(RESTORE)`.  Mismatch with the dump image's saved GIDs (or
  the hint's overrides) => fail loud with clear diagnostic.

rxe is SHAREABLE on its ibdev (multiple unrelated processes can
hold ufiles on the same rxe link); the same validate-only port
setup applies, just driven by the orchestrator's
`rdma link add rxe7 type rxe netdev <X>` rather than `ip link set
vf mac`.

The earlier draft of this section had the mlx5_vfmig plugin
*installing* GIDs via netlink at `init(RESTORE)`.  That path no
longer exists; see §6.5 for the rationale (KS7.4 moved L2/L3 setup
fully orchestrator-side).

### 8.2 FW identity continuity (K6)

For each of PD, CQ, QP, SRQ, MR (and any other VHCA-scoped FW id
class), the unknown is whether `LOAD_VHCA_STATE` preserves FW id
reservations on the destination VHCA the way it provably does for
UARs.

#### 8.2.1 Acceptance criteria

For UARs the criterion was simply "indexes increase across restore"
because user-allocated UARs were directly enumerable on the restored
VHCA via the dynamic UAR query verb (`uar_restore.md` ?3).
For the other classes the analogous shape is **"the FW's next-allocation
cursor for class C on the restored VHCA sits above the source's
SAVE-time peak for that class."** Per-class pass criterion:

| class | source-side observable | dest-side probe         | pass condition |
|-------|------------------------|-------------------------|----------------|
| PD    | `mlx5_ib_pd.pdn` via NLDEV `RES_PD_GET` | `mlx5_alloc_pd` then read `pdn` | `dest_pdn > max(source_pdns)` |
| CQ    | `cqn` via NLDEV `RES_CQ_GET` | `mlx5_create_cq` then read `cqn` | `dest_cqn > max(source_cqns)` |
| QP    | `qpn` via NLDEV `RES_QP_GET` (`RES_LQPN`) | `mlx5_create_qp` then read `qpn` | `dest_qpn > max(source_qpns)` |
| MKEY  | `mr->key` via NLDEV `RES_MR_GET` (with CAP_NET_ADMIN) | `mlx5_alloc_mkey` then read mkey | `dest_mkey_index > max(source_mkey_indices)` |
| SRQ   | `srqn` via NLDEV `RES_SRQ_GET` | `mlx5_create_srq` then read `srqn` | `dest_srqn > max(source_srqns)` |

Note we don't need to dig new probe ioctls beyond what NLDEV already
emits -- the FW ids are user-visible via `rdma resource show`. The
"probe" is just `alloc one fresh resource of class C; record its id`.

#### 8.2.2 How to run without R3 yet implemented

The experiment is **kernel-side standalone** and doesn't depend on the
generic CRIU restore path. We can run it today using the existing
`ucontext_vendor_verbs` test harness + a small driver extension:

```
On host A (source):
  (1) Open ucontext, SET_TRACKED, allocate one of each class via
      standard uverbs (ibv_alloc_pd / ibv_create_cq / ibv_create_qp /
      ibv_reg_mr / ibv_create_srq).
  (2) Record FW ids via NLDEV (rdma resource show per class).
  (3) SAVE_VHCA_STATE.

On host A or B (destination):
  (4) Fresh VF, LOAD_VHCA_STATE with the host A blob.
  (5) MARK_RESTORED.
  (6) Open fresh ucontext (in restore mode -- the existing
      MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE path).
  (7) Allocate one fresh of each class. Record FW ids.
  (8) Apply the per-class pass condition table above.
```

What this *doesn't* tell us: whether the source's specific ids can be
re-claimed by a future R3 `RESTORE_<TYPE>` ioctl -- only whether
they're protected (above the cursor). The stronger probe ("can we
actually install at exactly id=N") requires the K3/K4 plumbing itself
and is integrated as the validation step of S3 (PD restore) per ?9.1.

#### 8.2.3 Outcome -> design impact

* **All pass conditions met**: FW honours source ids by treating them
  as reserved on the loaded VHCA. K3/K4 mlx5 handlers are then a small
  extension: each `restore_<type>` calls the existing FW alloc command
  with an id hint, FW honours the hint, kernel-side state matches
  source. Small per-class diff.
* **Any pass condition fails for class C**: FW does NOT honour
  reservations for class C. Two implementation paths:
  - **(a)** Issue a new FW command "pre-reserve id N for class C on
    VHCA V" before each `restore_<type>` (likely requires FW patch).
  - **(b)** Extend the `LOAD_VHCA_STATE` blob format to carry explicit
    per-class id tables that FW honours on load (almost certainly
    requires FW patch).
  Either case is a much larger scope than the doc currently absorbs;
  surface it loudly and revisit the v0 commitment.

CRIU userspace is unaffected by either outcome -- the difference is
internal to the driver-side `restore_<type>` handler.

### 8.3 PF / VF asymmetry

The destination-is-fresh-VF model (mlx5_vfmig) sidesteps PF-level
asymmetry concerns: the destination VF's parent PF is whatever the
orchestrator chose, the VHCA contents come from LOAD, and the source's
PF-side state never crosses. PF-level state (firmware version, link
config, ...) is the orchestrator's responsibility.

## 9. Test plan

### 9.1 Stage-by-stage validation

Strategy: drive toward a passing `rdma_test_agent` (and then
`ib_write_bw`) end-to-end as quickly as possible by landing the
**most-traffic-relevant uobject classes first**, callback-by-callback
across rxe + mlx5_vfmig per class. The minimum set for an RC send/recv
round-trip is PD + MR + CQ + QP; SRQ/AH/CC/AEF land after.

* **S0: K6 empirical experiment.** Land the PF cdev probe ioctl
  (next-id-per-class). Run on a single host via `ucontext_vendor_verbs`
  fork-save-load; then cross-host. Output classifies each of
  `{PD, CQ, QP, MKEY, SRQ}` as "preserved" or "not preserved". Drives
  K3/K4 mlx5 handler shape. Piggyback the ?6.3 `QUERY_QP` RQ-head/tail
  experiment on the same harness. **Blocks all mlx5 work below.**
* **S1: DAG discovery on rxe**, no restore verbs. Image carries the full
  per-ucontext DAG; restore is no-op (existing behaviour). Validates
  schema, topo-sort, handle-map machinery in isolation. CRIU-only;
  zero kernel dependency. `run_uverbs_cr.sh` passes through the
  existing "PD-uobject preservation gap" failure unchanged; image
  inspection via `crit show` shows the DAG correctly captured.
  **First landable CRIU-side commit set.** Lands in parallel with S0.
* **S2: K2 (LIST_UOBJS).** **Done -- already upstream as
  `UVERBS_METHOD_INFO_HANDLES`.** Validated by
  `info_handles_probe`. CRIU plugin will call it for AH discovery
  and the DEVX/MW/FLOW/XRCD pre-suspend coverage check; no kernel
  patch needed.
* **S3a: PD restore on rxe (landed).** Generic
  `UVERBS_METHOD_RESTORE_PD` dispatcher + `rxe_restore_pd` landed as
  `e06868342fce`. Validated empirically by `pd_restore_probe_rxe`
  (see ?9.4). rxe-side is intentionally simpler than mlx5: no FW id
  to preserve, so the `target_handle` hint is unused and
  `rxe_restore_pd` is a pass-through to `rxe_alloc_pd`. Validates
  the generic dispatcher's choreography end-to-end on a software
  device before mlx5 layers FW-id-hint complexity on top.
* **S3b: PD restore on mlx5_vfmig (landed: C3+C4).** Implement
  `mlx5_ib_restore_pd` via **Model A** -- adopt the source's FW
  `pdn` into a fresh kernel-side `mlx5_ib_pd` with no destination
  FW round-trip. CRIU passes the source `pdn` in the
  driver-private UHW payload (`struct mlx5_ib_restore_pd_req`,
  `include/uapi/rdma/mlx5-abi.h`); the handler sets
  `mpd->pdn = req.pdn`, `mpd->uid = context->devx_uid` and
  returns. Model A is sound for the v0 critical path (non-DEVX
  `libibverbs`) because of two empirical results:

  * **K6 PARTIAL PASS**
    (`tools/testing/mlx5_vfmig/uobject_restore/fw_id_continuity/`):
    after `LOAD_VHCA_STATE` the FW `pdn` allocator's high-water
    mark is preserved, so source `pdn` slots are reserved on the
    destination VF.
  * **`pd_adopt` WEAK PASS**
    (`tools/testing/mlx5_vfmig/uobject_restore/pd_adopt/`,
    driven by the new `MLX5_VFMIG_IOC_PROBE_PD` PF-cdev ioctl):
    `CREATE_MKEY` under `uid=0` is ungated by firmware on
    `mkc.pd` validity, so downstream FW ops referencing the
    adopted `pdn` succeed without any FW gate to satisfy.
    Combined: no FW round-trip is required.

  **DEVX-adoption blind spot (2026-05-15).** The initial S3b
  plan also covered DEVX-aware ucontexts via a
  `MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID` flag plumbed through
  `mlx5_ib_alloc_ucontext_req_v2::adopt_devx_uid` (kernel commit
  `afb3b5614af3`). The premise was that `LOAD_VHCA_STATE`
  preserves the source ucontext's FW registration so the
  destination could re-claim the same `devx_uid` and run all
  source-allocated resources under it. Empirical falsification
  via the DEVX-source variant of `test_pd_adopt.sh` (matrix
  data captured on FW 28.48.1000):

  | uid lane                       | `src_pdn`            | `BOGUS_PDN`          |
  |--------------------------------|----------------------|----------------------|
  | `uid=0` (host-priv, ungated)   | accept `syn=0`       | accept `syn=0`       |
  | `uid=src_devx_uid` (registered)| reject `syn=0x76555f`| reject `syn=0x76555f`|
  | `uid=lo_unalloc` (never reg'd) | reject `syn=0x76555f`| reject `syn=0x76555f`|
  | `uid=hi_unalloc` (host range)  | accept `syn=0`       | accept `syn=0`       |

  Combined with `probe_uid` returning the *next* free uid past
  the source's high-water mark on the destination, the model is:
  **LOAD_VHCA_STATE preserves the FW `next_free_uctx` counter
  but does NOT preserve the uctx-registration table** (the
  `uid -> uctx_attrs` map). Every uid in the low/"user" range is
  unregistered post-LOAD; FW rejects `CREATE_MKEY(pdn, uid)`
  with a consistent "unknown uid" syndrome regardless of pdn.
  The `0xfff_` high range is a separate host-privileged fallback
  bucket that bypasses the registry check entirely, which is
  why `uid=hi_unalloc` accepts -- it is not a usable mitigation
  for adoption since it routes around `(pdn, uid)` ownership
  too.

  **v0 mitigation: don't propagate `devx_uid`.** The CRIU
  plugin opens the destination ucontext WITHOUT
  `MLX5_IB_ALLOC_UCTX_DEVX` /
  `MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID`, leaving
  `context->devx_uid = 0`. All adopted resources land in the
  `uid=0` host-priv lane, which is the ungated row proven by
  the matrix's `P_zero/N_zero` cells. The kernel-side
  `MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID` handler and the
  `adopt_devx_uid` UAPI field stay in tree as forward-compat
  but are marked `VESTIGIAL FOR v0 -- DO NOT SET FROM USERSPACE`
  in `include/uapi/rdma/mlx5-abi.h`. The
  `mlx5_ib_restore_pd_fw_probe` defense-in-depth check stays
  too: it now reads as "fail loudly the moment a future caller
  flips the flag on without an FW capability that preserves the
  uctx registry."

  **Critical caveat (2026-06-02): the v0 mitigation only works
  when source `devx_uid == 0` too.** Empirical re-evaluation
  via the CRIU pd_cq_qp end-to-end test exposed that the matrix
  above only covers `CREATE_MKEY` semantics; the host-priv
  uid=0 lane is asymmetric across opcodes:

    * `CREATE_*` / `DEALLOC_PD` / `DESTROY_CQ` under uid=0 act
      on FW resources owned by source.devx_uid != 0 cleanly
      (these rows of the matrix). The pd_cq scenario's teardown
      (no QP) succeeds end-to-end on a DEVX-enabled source.
    * `DESTROY_QP` under uid=0 against a QPC owned by
      source.devx_uid != 0 silently fails at FW. The kernel
      path `mlx5_ib_destroy_qp -> destroy_qp_common` is `void`
      and only `mlx5_ib_warn`s the FW errno; `mlx5_ib_destroy_qp`
      unconditionally returns 0 to userspace. The orphan QPC
      then surfaces at the next opcode that propagates its
      errno verbatim -- `DEALLOC_PD` -- as `bad_resource_state`
      (syndrome 0xef0c8a-class). Same hazard for the prerequisite
      `2RST_QP` modify if the QPC was non-RESET on the source.

  This means a pure "uid=0 lane" v0 mitigation is unsafe for any
  source that opened its ucontext through libmlx5 (or anything
  else that triggers auto-DEVX). To make the failure mode
  detectable instead of obscure, the kernel exposes the source's
  `devx_uid` across the seam:

    * `MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT` populates
      `meta.devx_uid = c->devx_uid` (occupies bytes formerly in
      `reserved1[0..1]`; older userspaces see 0, which is the
      correct non-DEVX behavior).
    * `MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT` includes
      `meta.devx_uid == c->devx_uid` in its strict-equality
      precondition #3. A snapshot taken from a `devx_uid != 0`
      source against a destination ucontext opened without
      `ADOPT_DEVX_UID` (or with the wrong value) fails -EINVAL
      at `RESTORE_UCONTEXT`, before any resource adoption runs.
    * `destroy_qp_common`'s warn-only paths upgrade from
      `mlx5_ib_warn` to `mlx5_ib_err` and now carry qpn / uid /
      state / errno so a uid mismatch is obvious in dmesg.

  The CRIU plugin's pre-suspend filter must therefore reject
  any source ucontext with `meta.devx_uid != 0` (see ?5.3.4
  "CRIU plugin v0 contract"). When one of the §S3b future-FW
  options 1-3 below lands, the filter relaxes to "match dump and
  restore through ADOPT_DEVX_UID".

  **Restored-process limitation.** Without DEVX on the dest,
  `mlx5dv_*` / `devx_obj_create` / DEVX-rooted UAR allocations
  are unavailable post-restore. Standard verbs (PD/MR/CQ/QP/SRQ
  for RC/UD/UC) work. The vast majority of CRIU's actual
  restore targets (HPC pingpong, `ib_write_bw`-class workloads,
  storage front-ends) live in the standard-verbs lane and are
  unaffected. DEVX-using workloads (Spectrum-X-controller-class,
  some collectives backends) are explicitly out of scope for v0
  and need future-FW work; see "future-FW options" below.

  **Future-FW options** for true DEVX adoption:

  1. **FW preserves the uctx registry across `LOAD_VHCA_STATE`**
     -- the right answer, gated on a new FW capability bit + a
     `LOAD_VHCA_STATE` variant that includes the uctx table.
     Multi-release-cycle item.
  2. **Per-resource uid rebind**. On the destination, allocate
     a fresh `uid_dst` via `CREATE_UCTX`, then `MODIFY_*_UID`
     every adopted FW resource from `owning_uid_src` to
     `uid_dst`. Requires a FW op family (`MODIFY_PD_UID`,
     `MODIFY_MKEY_UID`, `MODIFY_QP_UID`, ...) that does not
     currently exist on this FW.
  3. **Source-side `uid=0` negotiation**. CRIU + the application
     cooperate so the source process never opens a DEVX
     ucontext for resources we want to restore. Less invasive,
     pushes work onto every app.

  Until one of these lands, the v0 mitigation is the path.

  **v0 dealloc-ordering invariant.** Because v0 only restores
  PDs into the kernel ufile (S3b), the source's pdn-rooted
  CQ/QP/MR/SRQ remain alive on the destination *firmware* after
  `LOAD_VHCA_STATE` with no corresponding kernel uobjects. The
  kernel therefore cannot dealloc those FW dependents before
  attempting `MLX5_CMD_OP_DEALLOC_PD`, so firmware *must* reject
  an orphan DEALLOC_PD with status `BAD_RES_STATE` (0x9), which
  `cmd_status_to_err` maps to `-EINVAL`. Dmesg shows this as
  `mlx5_core ... DEALLOC_PD(...) op_mod(0x0) failed, status bad
  resource state(0x9), syndrome (0x...), err(-22)`. This is the
  *correct* CRIU restore-ordering invariant -- the proper
  cascade is `RESTORE_PD -> RESTORE_{CQ,QP,MR,SRQ} -> user
  destroys QP/MR/CQ/SRQ -> user destroys PD -> FW DEALLOC_PD
  succeeds`. `uverbs_destroy_uobject` propagates the FW error
  out of `destroy_hw` without clearing `uobj->object` or
  removing the idr handle, so the adopted PD's uobj stays
  parked in the ufile, waiting for the future
  RESTORE_CQ/MR/QP/SRQ teardown cascade (S4 landed; S5..S7
  pending) to drain it.
  `pd_restore_probe_mlx5_vfmig`'s subtest 7 (`v0 dealloc
  semantics`) locks this invariant in: DEALLOC_PD on the
  orphan adopted PD MUST fail with `-EINVAL`/`-EBUSY`/`-EREMOTEIO`
  and INFO_HANDLES MUST still report the handle.

  **Empirical validation harness.** The
  `pd_restore_probe_mlx5_vfmig` driver-end-to-end probe at
  `tools/testing/mlx5_vfmig/uobject_restore/pd_restore/pd_restore_probe_mlx5_vfmig.c`
  opens a `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE` ucontext on a
  bound, post-LOAD VF, walks subtests 1-5 (gate, UAPI rejection
  x2, happy path, collision), parks at READY, lets the harness
  invoke `MLX5_VFMIG_IOC_PROBE_PD` (Phase G FW-liveness check),
  and on `quit` runs subtest 7 (v0 dealloc semantics).
  The shell wrapper
  `test_pd_restore_mlx5_vfmig.sh` mirrors Phases A-E of
  `test_pd_adopt.sh` and adds Phases F-I. Source FW state
  preservation is validated independently by
  `test_fw_id_continuity.sh K6_POST_RECV_WRS=N`.
* **S4a: MR restore on rxe (landed).** Generic
  `UVERBS_METHOD_RESTORE_MR` dispatcher + `rxe_restore_mr` landed
  in the kernel core + rxe driver. rxe **honours** the
  `lkey_hint`/`rkey_hint` identity hint via a new
  `__rxe_add_to_pool_at_index` primitive (xa_insert at a specific
  pool index, returns -EBUSY on collision) plus a post-init
  overwrite of the 8-bit per-MR nonce from the hint's low byte.
  Net result: a CRIU-restored rxe MR is wire-compatible with the
  source -- WRs embedding the source's `lkey`/`rkey` keep
  functioning post-restore, which makes rxe a real validation
  surface for the verb contract (not just plumbing). Empirically
  validated by `mr_restore_probe_rxe` (see ?9.5).
* **S4b: MR restore on mlx5_vfmig (landed).** `mlx5_ib_restore_mr`
  adopts the source's FW mkey into a fresh kernel-side
  `mlx5_ib_mr` wrapper without re-issuing `CREATE_MKEY`. The
  kernel-side handler runs no FW commands; the destination FW
  state was already established by `LOAD_VHCA_STATE` and is
  preserved across the SAVE/LOAD boundary (the empirical chain
  below). Couples with `user_mr_dma.md` stage 3 (rkey continuity
  at the IOMMU layer); the kernel verb and the user_mr_dma
  IOMMU-layer binding land together as a coherent per-MR
  restore. With PD + MR working, the `rdma_test_agent` send
  buffer is restorable end-to-end.

  **Model A (no FW round-trip on mkey adoption).** The
  `mlx5_ib_mr` wrapper is `kzalloc`'d, its `mmkey.key` is set
  to the source's full FW key (`(mkey_index << 8) |
  variant_byte`), and its `cache_ent` is left NULL (the cache
  path doesn't apply to adopted mkeys). The wrapper is
  wire-visible at the source's `lkey`/`rkey`; mlx5 always
  honours the identity hint (`mlx5_ib_dispatcher` echoes back
  `mr->lkey`/`mr->rkey` byte-identical to the caller's hint --
  contrast with rxe, where `__rxe_add_to_pool_at_index` decides
  whether to honour it; see S4a for the rxe path).

  **`mr->umem` is real (Stage-3 D3+D4 landed).** `mlx5_ib_restore_mr`
  calls `mlx5_ib_umem_restore_mr` before mmkey-state population;
  that helper composes `ib_umem_pin` (D1, IB-core refactor that
  pins user pages without DMA-mapping) + `mlx5_vfmig_bind_user_mr`
  (D3 driver wrapper) + `vfmig_iova_bind_user_object` (D2 vfmig
  primitive, looks up the `(KIND_MR, mkey_index)` placeholder
  Stage-2 C5 installed during LOAD and `iommu_map`s each sg at
  the source-emitted IOVA). On verb success `mr->umem` carries
  the destination's pinned pages; on dereg `__mlx5_ib_dereg_mr`
  follows the `if (mr->umem)` arm (FW `DESTROY_MKEY` -> 0,
  `ib_umem_release(mr->umem)` releases the pins, registry
  placeholder is dropped). Page accounting is symmetric with
  `create_real_mr` via `atomic_add(ib_umem_num_pages, &reg_pages)`.

  **Empirical chain anchoring Model A** (each ran on
  destination FW 28.48.1000, mkey class only, `uid=0`):

  1. `K6 / test_fw_id_continuity.sh` -- the source's FW mkey
     index is preserved across `LOAD_VHCA_STATE` with the same
     reservation (no PARTIAL: PD/CQ/QP/MKEY all observed +3..+5
     deltas, fully explained by destination-side internal
     `mlx5_ib_dev_res` allocations).
  2. `B3 / MLX5_VFMIG_IOC_PROBE_MKEY` + `test_mr_adopt.sh` --
     issuing FW `QUERY_MKEY` against the source's mkey index on
     the destination VF post-LOAD-VHCA-STATE returns the full
     `mkc` context (`pd`, `start_addr`, `len`) byte-equal to
     the source's pre-SAVE view, with `fw_syndrome=0`. Negative
     control (bogus mkey index) is rejected with a syndrome.
     STRONG PASS, recorded in the test_mr_adopt log.
  3. `B4 / mr_restore_probe_mlx5_vfmig` +
     `test_mr_restore_mlx5_vfmig.sh` -- the live verb path
     repeats the chain end-to-end: source allocates an MR via
     `fw_id_continuity_probe`, SAVE, fresh dst VF, LOAD,
     destination `RESTORE_MR` adopts the source's mkey, and
     while the adopted MR is alive `PROBE_MKEY` on the dst VF
     still reports `(pd, start_addr, length)` byte-identical to
     source pre-SAVE. The verb path doesn't perturb FW state
     after adoption.

  **UAPI shape** (`include/uapi/rdma/mlx5-abi.h`):

  ```c
  struct mlx5_ib_restore_mr_req {
      __u32  mkey_index;     /* 24 bits significant */
      __u32  reserved;       /* must be 0 */
      __aligned_u64 reserved2; /* must be 0 */
  };
  ```

  Carried as the UHW payload to `UVERBS_METHOD_RESTORE_MR`. The
  16-byte size is deliberate (> 8 bytes pushes the kernel's
  uverbs UHW dispatch onto the ptr path, identical to the
  `mlx5_ib_restore_pd_req` analysis in S3b's UHW commentary).
  The handler enforces three identity invariants on top of the
  generic dispatcher's attr validation:

  1. `req.reserved == 0 && req.reserved2 == 0` (forward-compat
     reservation) -> else `-EINVAL`.
  2. `req.mkey_index != 0 && (req.mkey_index & ~0xffffff) == 0`
     (24-bit FW mkey index, 0 reserved as sentinel) -> else
     `-EINVAL`.
  3. `lkey_hint == rkey_hint` (mlx5 user-MR invariant) AND
     `(lkey_hint >> 8) == req.mkey_index` (the wire-visible
     identity must encode the FW key) -> else `-EINVAL`. This
     is the load-bearing check: it catches the class of CRIU
     bugs that ship a restrack id where a FW key was expected.

  **v0 dealloc semantics -- asymmetric with PD's invariant.**
  Unlike PDs (where FW BAD_RES_STATE rejects an orphan
  DEALLOC_PD because PD is a parent in the FW resource graph
  and its CQ/QP/MR/SRQ children are still alive), mkey is a
  *leaf* under PD in the FW resource graph. QPs reference an
  mkey by its (lkey/rkey) wire value rather than as a tracked
  FW resource dependency, so `MLX5_CMD_OP_DESTROY_MKEY` on the
  orphan adopted mkey *succeeds* even with the source's
  mkey-using QPs still alive in destination FW post-LOAD.
  `__mlx5_ib_dereg_mr` follows the `if (mr->umem)` arm:
  FW DESTROY_MKEY -> 0, `ib_umem_release(mr->umem)` drops the
  destination-side pins (and Stage-3 D2's
  `vfmig_dma_ops.unmap_sg` runs against the bound sg-table so
  the registry placeholder is freed too); then
  `uverbs_destroy_uobject` removes the uobj from the ufile.
  Empirically validated by `mr_restore_probe_mlx5_vfmig`'s
  subtest 8: `DEREG_MR(adopted_handle) -> 0` and
  `INFO_HANDLES(MR)` drops the handle, with no kernel WARNs and
  no umem leak (mkey is a leaf in the FW resource graph).

  v0 implication for CRIU: where PDs get restore-ordering
  enforcement *for free* via FW (the kernel parks the orphan
  uobj until the children are torn down), MR teardown ordering
  is plugin-policy only -- the kernel will not refuse a
  premature `DEREG_MR`. The CRIU plugin must not issue
  `DEREG_MR` on adopted MRs ahead of the user's intent. The
  inverse: nothing on the kernel side blocks the plugin from
  driving an explicit MR teardown if it ever needs to. This
  asymmetry is recorded as an open finding in ?10.8.

  **Empirical validation harness.** The
  `mr_restore_probe_mlx5_vfmig` driver-end-to-end probe at
  `tools/testing/mlx5_vfmig/uobject_restore/mr_restore/mr_restore_probe_mlx5_vfmig.c`
  opens a `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE` ucontext on a
  bound, post-LOAD VF, walks subtests 1-7 (gate, UAPI rejects
  x4 covering the three identity invariants above, happy path
  with byte-identical RESP_LKEY/RKEY echo, EBUSY collision),
  parks at READY, lets the harness invoke
  `MLX5_VFMIG_IOC_PROBE_MKEY` (Phase G FW-liveness check), and
  on `quit` runs subtest 8 (the asymmetric v0 dealloc
  semantics). Full sub-shape in ?9.6. Validated PASS
  end-to-end: 8/8 subtests + FW liveness fw_accept=1 + mkc
  content match. The shell wrapper
  `test_mr_restore_mlx5_vfmig.sh` mirrors Phases A-E of
  `test_mr_adopt.sh` and adds Phases F-I.

  **v0 limitations** (deliberate; not blocking):

  * IOMMU-bind happens via `user_mr_dma`'s `HOST_USER_PAGE`
    replay path (Stage-2 C4/C5 + Stage-3 D2). The destination
    pins its own copy of the user pages via `ib_umem_pin`
    inside the verb body and `iommu_map`s them at the
    source-emitted IOVA, so the wire-visible iova survives
    SAVE/LOAD; the source's pages themselves do not migrate
    (deliberate -- only identity does).
  * No kernel-side mkey cache participation. Adopted mkeys
    bypass the destination's mkey cache; on dereg they go
    direct to FW DESTROY_MKEY rather than back to a cache
    entry. Wastes one cache slot's worth of optimisation per
    restored MR; not a correctness issue.
  * No `REREG_MR` on adopted MRs. Out of scope for v0; if it
    becomes needed, the plugin can dereg + re-restore.
  * dma-buf MRs and ODP MRs are out of scope for v0. Stage-2
    retag (`create_real_mr`) gates on `!umem->is_dmabuf`, and
    `mlx5_ib_restore_mr`'s `ib_umem_pin` path doesn't speak
    ODP (rejects `IB_ACCESS_ON_DEMAND` with `-EOPNOTSUPP`).
* **S5a: CQ restore on rxe (landed).** Generic
  `UVERBS_METHOD_RESTORE_CQ` dispatcher + `rxe_restore_cq` landed as
  `a77cc4d8e8b9`. Shape mirrors S3a's PD landing: dispatcher gates,
  `rdma_alloc_begin_uobject_at_handle()` for the ufile-handle
  reservation, attribute parsing into `struct ib_cq_init_attr`,
  driver callback. Two CQ-specific dispatcher decisions (no analogue
  for PD/MR -- see ?7.3): (a) `UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL`
  declared `UA_OPTIONAL` for forward-compat with S5c but rejected
  with `-EOPNOTSUPP` at v0; (b) `UVERBS_ATTR_RESTORE_CQ_EVENT_FD`
  routed through the existing `ib_uverbs_get_async_event()` helper,
  which falls back to `ufile->default_async_file` when the attr is
  absent (the v0 path -- restored CQ async events flow through the
  ufile default; S8 will let plugins pin them to an explicit
  restored async-event uobject without a UAPI bump).
  rxe-side is intentionally near-identical to `rxe_create_cq` since
  rxe has no userspace-visible cqn (no rxe-DV equivalent for
  `mlx5dv_cq->cqn`); the `target_handle` hint is already honoured by
  the dispatcher's ufile-handle reservation, so the driver has
  nothing further to do for hw-id purposes. Empirically validated by
  `cq_restore_probe_rxe` (see ?9.7).

  **v0 dealloc semantics on rxe.** `rxe_destroy_cq` returns
  `-EINVAL` only when `atomic_read(&cq->num_wq) != 0` (the standard
  uverbs invariant: a CQ with attached WQs cannot be destroyed). At
  v0 -- with no RESTORE_QP yet -- no kernel-side QP is attached to
  the restored CQ, so the orphan adopted CQ destroys cleanly via
  `IB_USER_VERBS_CMD_DESTROY_CQ`. (Different from mlx5's S5b case
  below; the asymmetry mirrors the PD-vs-MR axis of ?10.8 and is
  recorded explicitly there now that CQ has landed.)

* **S5b: CQ restore on mlx5_vfmig (B1+B2+B3 LANDED, B4 pending).**
  Implements `mlx5_ib_restore_cq` via Model A -- adopt the source's
  FW `cqn` into a fresh kernel-side `mlx5_ib_cq` with no destination
  FW round-trip. Empirical chain anchoring the handler:

  1. **K6 / `test_fw_id_continuity.sh`** -- already PASS for cqn
     (PARTIAL PASS, +3..+5 deltas explained by destination internal
     allocations, no missing high-water mark).
  2. **B0 / `MLX5_VFMIG_IOC_PROBE_CQN`** -- LANDED, **STRONG PASS**
     on first run. Raw `QUERY_CQ` against the source's cqn on the
     destination VF post-LOAD, asserting `(cqn, eqn, log_cq_size,
     log_page_size, page_offset, status, oi)` byte-equal to the
     source's pre-SAVE view. The `eqn` field is the load-bearing
     one CQ adds over mkey: a CQ's events flow through an event
     queue (EQ), and the EQ binding must survive `LOAD_VHCA_STATE`
     for adopted CQs to deliver completions. Mirrors
     `MLX5_VFMIG_IOC_PROBE_MKEY`'s shape (B3 in S4b); the Phase G
     out-of-band check inside `cq_restore_probe_mlx5_vfmig`.

     **B0 ioctl shape, opcode 0x0d in `MLX5_VFMIG_IOC_MAGIC` space.**
     `struct mlx5_vfmig_probe_cqn { vf_id; cqn; reserved_in[8];
     fw_syndrome; fw_eqn; fw_status; fw_log_cq_size;
     fw_log_page_size; fw_page_offset; fw_oi; fw_cqe_sz; fw_apu_cq;
     fw_uar_page; fw_dbr_addr; reserved_out[..]; }`. Locking shape
     copied from PROBE_MKEY: resolve VF pci_dev from PF + vf_id,
     `device_lock` to pin `->driver` / drvdata, match driver by
     `KBUILD_MODNAME`, require `MLX5_INTERFACE_STATE_UP`. The cmd
     runs on the VF mdev's cmdif (cmdif-uid=0); like PROBE_MKEY,
     this ioctl tests FW-side existence with zero side effects --
     destination-ucontext usability is left to B4. Symmetric
     usage: the `test_cq_adopt.sh` harness calls it on the source
     VF pre-SAVE *and* the destination VF post-LOAD, then byte-
     compares the two FW snapshots. Stronger assertion than
     `test_mr_adopt.sh` does (which only knows source-side
     `mr_addr` / `mr_length` from userspace), made possible
     because the cqc fields aren't exposed by libibverbs/libmlx5
     so the FW-of-truth path is the only path.

     **B0 first-run evidence (single-host, same-PF SAVE/LOAD,
     `test_cq_adopt.sh`):** src cqn=2596 (=0xa24); pre-SAVE source
     PROBE_CQN: eqn=6, status=0, log_cq_size=5, log_page_size=0,
     page_offset=0, oi=0, cqe_sz=0, apu_cq=0, uar_page=0x16,
     dbr_addr=0x21f201000; post-LOAD destination PROBE_CQN: every
     field byte-equal. Negative control with cqn=0xffff00:
     `fw_accept=0, fw_syndrome=0x001fb6ec` (FW gates QUERY_CQ on
     existence -> the positive accept wasn't a host-priv
     permissiveness artifact). Verdict: STRONG PASS for Model A
     `mlx5_ib_restore_cq` -- adopting the source's `(cqn, eqn)`
     pair into a destination kernel-side `mlx5_ib_cq` is sound,
     no destination FW round-trip needed, the EQ binding survives
     verbatim. Forensic fields (`uar_page`, `dbr_addr`) also
     byte-equal -- pre-evidence that B3's KIND_DBR doorbell-page
     bind helper will land cleanly because the source's user-VA
     for the dbr ring survives in cqc unchanged.
  3. **B4 / `cq_restore_probe_mlx5_vfmig`** (pending) -- end-to-end
     verb-path probe with the live adopted CQ, gated by
     `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE` and walking subtests 1-7
     (gate, UAPI rejects, happy path with byte-identical RESP_CQE
     echo, EBUSY collision, COMP_CHANNEL-rejection, EVENT_FD
     default-fallback success), parking at READY for the harness to
     run B0's PROBE_CQN, and on `quit` running subtest 8 (the
     PD-side-of-the-asymmetry v0 dealloc semantics: orphan adopted
     CQ DESTROY_CQ -> `-EINVAL` via FW BAD_RES_STATE because the
     source's cqn-using QPCs are still alive in destination FW until
     S6 lands).

  **UAPI shape** (`include/uapi/rdma/mlx5-abi.h`, B1, LANDED):
  ```c
  struct mlx5_ib_restore_cq_req {
      __aligned_u64 buf_addr;  /* user VA of CQE ring buffer */
      __aligned_u64 db_addr;   /* user VA of doorbell page */
      __u32  cqn;              /* 24 bits significant */
      __u32  cqe_size;         /* 64 or 128 */
      __u32  reserved;         /* must be 0 */
      __u32  reserved2;        /* must be 0 */
  };
  ```
  32-byte UHW payload (above the 8-byte inline-UHW threshold, so
  `ib_copy_from_udata()` takes the userspace-pointer path the same
  way `mlx5_ib_restore_pd_req` / `mlx5_ib_restore_mr_req` already
  do). Handler enforces: `req.reserved == 0 && req.reserved2 == 0`
  (forward-compat), `req.cqn != 0 && (req.cqn & ~0xffffff) == 0`
  (24-bit FW cqn, 0 reserved sentinel), `req.cqe_size in {64, 128}`,
  `attr->cqe >= 0`, `attr->flags` in
  `{TIMESTAMP_COMPLETION, IGNORE_OVERRUN}`. No `lkey/rkey`-style
  cross-check against a core hint because there is no core
  `cqn_hint` (see ?7.3) -- cqn identity travels only through this
  UHW payload.

  Note vs original sketch (revision 1, 16-byte cqn-only): the
  smaller shape was insufficient because `RESTORE_CQ` does NOT
  carry `buf_addr` / `db_addr` / `cqe_size` in its core attrs
  (which only carry `cqe` / `comp_vector` / `flags`), and the
  destination kernel needs all three: `buf_addr` / `cqe_size` to
  bind the CQE ring umem and parse CQEs at poll time, `db_addr`
  to bind the doorbell-page umem. The 32-byte shape mirrors
  `mlx5_ib_create_cq` UHW exactly modulo the absence of
  `cqe_comp_*` and `flags` (deferred to a future restore-time
  layout-adoption knob; FW already has the source's
  `cqe_comp_en` / pad / RTS bits set in the adopted cqc and the
  data path honours them).

  **Bind helpers (S5 B3, LANDED).** Mirrors S4b's
  `mlx5_ib_umem_restore_mr` / `mlx5_vfmig_bind_user_mr`. Handler
  composes `mlx5_ib_umem_restore_cq` (KIND_CQ buffer) and
  `mlx5_ib_db_map_user_restore` (KIND_DBR doorbell page) before
  registering with the comp + async EQ trees via
  `mlx5_core_adopt_cq()` (a sibling of `mlx5_core_create_cq` that
  skips FW CREATE_CQ and only does the kernel-side mlx5_core_cq
  registration). Both umem wrappers compose `ib_umem_pin()` with a
  thin mlx5_core entrypoint -- `mlx5_vfmig_bind_user_cq()` for
  KIND_CQ keyed by `cqn`, `mlx5_vfmig_bind_user_dbr()` for
  KIND_DBR keyed by `user_virt & PAGE_MASK` -- which call
  `vfmig_iova_bind_user_object()` (Stage-3 D2) against the
  placeholder records emitted by Stage-2 C8/C7 source retags.

  **Handler kernel-side init (S5 B2, LANDED).** Mirrors
  `mlx5_ib_create_cq`'s post-FW-cmd tail except no FW round-trip
  is issued. Sets `cq->ibcq.cqe = attr->cqe`, `cq->cqe_size =
  req.cqe_size`, the standard list-head + spin/mutex + private-
  flags init, the user-mode CQ callback trio (via
  `mlx5_ib_set_user_cq_callbacks()`, a new exported helper that
  decouples main.c from cq.c's file-static comp/event functions),
  and `mlx5_core_adopt_cq()` for the EQ-tree registration. The
  destination-side eqn is resolved via
  `mlx5_comp_eqn_get(comp_vector)`; we trust the K6 + B0 chain
  that this is the same eqn the source's cqc encodes (validated
  byte-equal at B0). No redundant QUERY_CQ at restore time.

* **S5c: COMP_CHANNEL restore (deferred to event-mode milestone).**
  Lives with S8 (RESTORE_ASYNC_EVENT) -- both share fd-table
  reinstall plumbing. Lands when `ib_write_bw -e` becomes the active
  goal (S10). The S5 dispatcher's hard-reject of the COMP_CHANNEL
  attr means CRIU plugin policy at v0 can be "checkpoint only CQs
  created without a comp channel" without ABI churn when S5c finally
  lands.

  Combined empirical chain table (S5b only; S5a uses
  `cq_restore_probe_rxe` directly):

  | step | what | landed | output |
  |------|------|--------|--------|
  | K6 | LOAD_VHCA_STATE preserves cqn high-water mark | yes (?10 #1) | PARTIAL PASS |
  | B0 | PROBE_CQN raw QUERY_CQ post-LOAD | yes (`test_cq_adopt.sh`) | **STRONG PASS** -- src/dst byte-equal across (cqn, eqn=6, log_cq_size=5, log_page_size=0, page_offset=0, status=0, oi=0); negative control rejected with FW syndrome 0x001fb6ec |
  | B1 | UAPI `mlx5_ib_restore_cq_req` (32B; `buf_addr` / `db_addr` / `cqn` / `cqe_size` / reserved) | yes | compiles, no abi churn |
  | B2 | `mlx5_ib_restore_cq` handler + `dev_ops.restore_cq` slot, `mlx5_core_adopt_cq` EQ-tree register helper, `mlx5_ib_set_user_cq_callbacks` cq.c export | yes | compiles, no regressions in mlx5_core / mlx5_ib build |
  | B3 | bind helpers (`mlx5_vfmig_bind_user_cq` / `_user_dbr` mlx5_core, `mlx5_ib_umem_restore_cq` mlx5_ib mem.c, `mlx5_ib_db_map_user_restore` mlx5_ib doorbell.c) | yes | compiles |
  | B4 | live verb path adopts cqn cleanly (`cq_restore_probe_mlx5_vfmig` + `test_cq_restore_mlx5_vfmig.sh`: 10 subtests -- gate, 4 UAPI rejects, bad comp_vector, COMP_CHANNEL rejection, happy path, EBUSY collision, Phase-G PROBE_CQN byte-equal vs. source pre-SAVE, post-quit v0 dealloc rejection) | yes (2026-05-29) | **STRONG PASS** -- 10/10 subtests + Phase G byte-equal across (eqn=6, log_cq_size=5, log_page_size=0, page_offset=0, status=0, oi=0); subtest 10 confirmed orphan `DESTROY_CQ` -> `-EINVAL` via FW BAD_RES_STATE while QPC dependents still alive; LOAD-side dmesg shows 5 user_page placeholders replayed (CQE-ring + DBR + MR + QP + SRQ), residue of `total - (cmd_ring+fw_page+dma_coherent+eq_buf+frag_buf+db_page) = 218 - 213 = 5`. DBR-VA-keyed lookup invariant locked in by `MAP_FIXED_NOREPLACE` in the probe's `cq_args_mmap_local` (commit `da4dd37ac8bc`) -- without it the fresh-anon-mmap shim would mismatch the (KIND_DBR, src_db_addr & PAGE_MASK) placeholder. |
  | B5 | dump-side verb (`MLX5_IB_METHOD_VFMIG_QUERY_CQ` + `cq_query_probe_mlx5_vfmig`): kernel reads `mcq->mcq.cqn` / `ibcq->cqe` / `mcq->cqe_size` / `mcq->buf.umem->address` / `mlx5_ib_db_user_virt(&mcq->db)` / `mcq->mcq.vector` / `mcq->create_flags` and emits a 32-byte payload byte-equal to `mlx5_ib_restore_cq_req` plus three scalar outs (cqe / comp_vector / flags). Closes the cross-process gap that motivated this verb: CRIU runs in its own address space and `mlx5dv_init_obj` from CRIU returns CRIU's VAs, not the dumpee's; `ibv_import_cq` does not exist in upstream rdma-core (see ?5.2.4 for the full design-space rejection of mlx5dv-import / NLDEV-extension / parasite-injection alternatives). Includes an `mlx5_ib_create_cq` symmetry fix (`cq->mcq.vector = vector;` after `mlx5_comp_eqn_get`) so QUERY_CQ echoes the user's source-side comp_vector instead of zalloc-zero -- pre-existing asymmetry called out in `mlx5_core_adopt_cq`'s docstring as "we mirror"; QUERY_CQ flips it. | yes (2026-05-30) | **STRONG PASS** -- `cq_query_probe_mlx5_vfmig mlx5_2 16 0`: subtests 1 (happy: cqn=0xa27 / buf_addr=0x55cb2a0e0000 / db_addr=0x55cb2a0e4000 / cqe=31 / comp_vector=0 / flags=0x0), 2 (invalid handle -> -ENOENT), 3 (multi-CQ disambiguation across two distinct cqn / buf_addr), 4 (`comp_vector=1` echoes correctly post-symmetry-fix). Round-trip pin len = cqe(31) * cqe_size(64) = 1984 bytes, sane vs. the 2048-byte ring-with-padding. byte-equal contract holds across all three ring-buffer / DBR-page / FW-id fields against `mlx5dv_init_obj` view (A). |
* **S6: QP restore (rxe + mlx5_vfmig together).** Single-shot
  `RESTORE_QP` (no `ib_modify_qp` chain) lands the QP at its captured
  final state (RESET / INIT / RTR / RTS-with-content). v0 type
  scope: RC + UD (UC parked; see ?5.3.1). v0 state scope: as
  above; ERR / SQD / SQE / multicast / RD_ATOMIC all deferred or
  refused per ?5.3.1. **First passing `rdma_test_agent` round-trip
  on a restored ucontext -- R3 v0 minimum bar.**

  Sequencing guidance: land **mlx5_vfmig first** (the umem-adoption
  pattern is mostly mechanical to lift from S5b CQ; FW handles PSN
  preservation per ?6.3 empirical proof). Land **rxe second** (the
  vm_pgoff queue pattern is also a CQ-restore lift but the explicit
  PSN capture + synthetic-ACK kick + `FREEZE_DATAPATH` verb are net-
  new). Then both together for **`ib_write_bw`** at S9.

  Combined empirical chain table (mirrors S5b's structure; S6a =
  rxe arm, S6b = mlx5_vfmig arm):

  | step | what | landed | output |
  |------|------|--------|--------|
  | K7 | LOAD_VHCA_STATE preserves QPC end-to-end (`MLX5_VFMIG_IOC_QUERY_QP` PF cdev + `test_fw_id_continuity.sh` piggyback driven by `K6_QP_STATE={INIT,RTR,RTS}` with self-loopback) | yes (2026-06-01, INIT/RTR/RTS) | **STRONG PASS** at all three states. Subset is the wider §6.3 / S6 set the harness now enforces: state-independent (state, pd, q_key, uar_page, log_page_size, log_sq_size, log_rq_size, log_msg_max, user_index), cross-references (cqn_snd, cqn_rcv, srqn_rmpn_xrqn), queue counters (hw/sw sq_wqebb_counter, hw/sw rq_counter), PSNs (next_send_psn, next_rcv_psn, last_acked_psn), pkey_index, and -- once the source has driven `ibv_modify_qp(RTR)` -- the entire RTR-set: path_mtu, min_rnr_nak, log_rra_max, remote_qpn, primary_address_path (44 bytes byte-equal: dgid, sgid_index, dlid/mlid, sl, port, dmac, hop_limit, tclass, flow_label, udp_sport, ack_timeout, eth_prio). RTS additionally byte-equal on log_sra_max, retry_count, rnr_retry. **Implication:** S6b mlx5_vfmig RESTORE_QP needs no per-field FW replay -- LOAD_VHCA_STATE preserves the entire QPC. The handler only needs to (a) adopt umem (S6b B3) and (b) re-bind userspace VA mappings to the FW qpn (S6b B2). PSNs, AV, retry counts, MTU all ride along on the FW-side QPC blob. |
  | S6b B0 | PROBE_QPN raw QUERY_QP post-LOAD on adopted qpn (mirrors S5b B0 for cqn) | yes (covered by K7 wider sweep, 2026-06-01) | **STRONG PASS** -- `test_fw_id_continuity.sh K6_QP_STATE={INIT,RTR,RTS}` self-loopback proves the source's qpn stays alive in destination FW post-LOAD with byte-equal QPC bytes (state, pd, q_key, uar_page, log_{page,sq,rq}_size, log_msg_max, user_index, cqn_snd, cqn_rcv, srqn_rmpn_xrqn, hw/sw {sq_wqebb,rq}_counter, PSNs, RTR-set incl. 44-byte primary_address_path, RTS-set). The same harness also doubles as a B0 negative cross-check by comparing src and dst at the *exact same* qpn. |
  | S6b B1 | UAPI `mlx5_ib_restore_qp_req` (64B; `buf_addr` / `db_addr` / `sq_buf_addr` / `qpn` / `sq_wqe_count` / `rq_wqe_count` / `rq_wqe_shift` / `flags` / `uidx` / `bfreg_index` / `ece_options` / 2 reserved). UHW omits PSNs / AV / retry / MTU because B0/K7 has shown the FW QPC preserves them byte-equal across LOAD; the UHW only carries the userspace-side identity FW can't re-derive (queue VAs, DBR VA, qpn, BFREG slot, ECE, create-time flags). Mirror of S5b B1 mlx5_ib_restore_cq_req. | yes (2026-06-01) | compiles, no abi churn |
  | S6b B2 | `mlx5_ib_restore_qp` handler + `dev_ops.restore_qp` slot + `UVERBS_METHOD_RESTORE_QP` core dispatch + FW-qpn-adopt helper (`mlx5_qpc_adopt_qp` -- mirrors S5b B2/B3 `mlx5_core_adopt_cq`). Core attr set (mandatory: HANDLE / PD\_HANDLE / SEND\_CQ\_HANDLE / RECV\_CQ\_HANDLE / TYPE / STATE / USER\_HANDLE / CAP (re-uses `struct ib_uverbs_qp_cap`); optional: SRQ\_HANDLE, CREATE\_FLAGS, EVENT\_FD; UHW; output: RESP\_QPN). Stub scope: kernel-side `mlx5_ib_qp` + `mlx5_core_qp` registration only (mutex / state / type / flags / port=1 / bfregn / has\_rq / cq\_{recv,send}\_list / qps\_list / `trans_qp.base.{container_mibqp, mqp.{qpn, uid=context->devx_uid, pid}}`). WQ-ring umem and DBR umem binding deferred to B3. | yes (2026-06-01) | **STRONG PASS** (hardware-validated 2026-06-02 via B4 harness end-to-end run after the `ib_set_device_ops` `restore_qp` slot was plumbed at commit 307787af956c "RDMA/core: Plumb restore_qp through ib_set_device_ops (S6b B2 fix)" -- without that follow-on, every RESTORE_QP short-circuited at the dispatcher's `if (!ib_dev->ops.restore_qp \|\| !ib_dev->ops.destroy_qp)` gate with -EOPNOTSUPP, masking the slot wiring with the same errno as the type-/state-switch reject paths). |
  | S6b B3 | bind helpers: `mlx5_vfmig_bind_user_qp` (mlx5_core, mirror of `mlx5_vfmig_bind_user_cq` keyed on `(KIND_QP, qpn)`); `mlx5_ib_umem_restore_qp` (mlx5_ib, single bind covering RQ + SQ in one contiguous mapping with access=0 since FW only reads WQE descriptors out of the WQ-ring umem); reuses `mlx5_ib_db_map_user_restore` from S5b B3 for the doorbell page. `mlx5_ib_restore_qp` extended to bind both umems, stamp `qp->{rq,sq}.{wqe_cnt, wqe_shift, offset}` + `base->ubuffer.{buf_addr, buf_size, umem}` from `set_user_buf_size`-equivalent arithmetic on UHW `(rq_wqe_count, rq_wqe_shift, sq_wqe_count)`, force `bfregn = MLX5_IB_INVALID_BFREG` (UHW carries source's UAR index, not a kernel-allocator slot), wire `mqp.event = mlx5_ib_qp_event` via `mlx5_ib_set_user_qp_event_callback`, and register into `dev->qp_list` / `send_cq->list_send_qp` / `recv_cq->list_recv_qp` via `mlx5_ib_register_user_qp_in_dev_lists` so destroy_qp_common's unconditional `list_del` chain matches. | yes (2026-06-01) | **STRONG PASS** (hardware-validated 2026-06-02 via B4 harness happy-path subtest 10 + Phase G QPC byte-equality cross-check + subtest 13 leaf-DESTROY_QP -- the bind helpers correctly wire the source's WQ-ring umem and doorbell-page umem onto the destination's adopted qpn, the kernel registers the QP into the same per-CQ / per-PD lists `destroy_qp_common` walks, and DESTROY_QP unwinds without a stuck `list_del` or umem leak). |
  | S6b B4 | live verb path adopts qpn cleanly (`qp_restore_probe_mlx5_vfmig` + `test_qp_restore_mlx5_vfmig.sh` -- gate, UAPI rejects, RTS-with-pending-WRs happy path, byte-equal QPC subset vs. source pre-SAVE). Probe subtest battery: gate (no VFMIG_RESTORE → -EPERM, exercised via RESTORE_PD because RESTORE_QP's mandatory PD/SEND_CQ/RECV_CQ IDR refs short-circuit at -ENOENT before the gate fires; the gate predicate is shared across all four RESTORE_* dispatchers via `restore_check_ucontext`), RESTORE_PD + RESTORE_CQ setup chain (positive), 5 UAPI rejects (qpn=0 / qpn high bits / reserved!=0 / uidx high bits / rq_wqe_shift out of [4,16]), 2 dispatcher rejects (qp_type=XRC_INI / qp_state=SQE), happy path (RESP_QPN echoes req.qpn; INFO_HANDLES contains qp_target_handle), collision (-EBUSY), READY checkpoint (harness drives `MLX5_VFMIG_IOC_QUERY_QP` while QP is alive), v0 dealloc (DESTROY_QP succeeds + INFO_HANDLES drops the handle -- ASYMMETRIC with CQ subtest 10 / PD subtest 7 because QP is a leaf in the FW resource graph; SYMMETRIC with MR subtest 8). Harness Phases A-I: source VF + RoCE netdev up → fork `fw_id_continuity_probe` with `--qp-state RTS --post-recv-wrs N` (N=4 default; populates RTS-set + RQ counters) → pre-SAVE QUERY_QP snapshot → SAVE → dest VF + LOAD + bind → fork dst probe with all 14 source-side fields (pdn, cqn/cqe/cqe_size/cq_buf_addr/cq_db_addr, qpn/sq_wqe_count/rq_wqe_count/rq_wqe_shift/qp_buf_addr/qp_db_addr, qp_type=RC, qp_state=RTS) → live-verb-path QUERY_QP byte-comparison (state-independent + INIT-set + RTR-set AV/PSNs + RTS-set retry/PSNs + queue counters touched by post_recv) → quit dst probe → DESTROY_QP success → manifest. Verdict matrix: PASS / WEAK PASS (verb path landed but QPC snapshot diverged → adopt-QP wiring or LOAD preservation suspect; cross-check K6 §6.3 piggyback in isolation) / FAIL (verb path failed; bullets common causes). Prerequisite: `fw_id_continuity_probe` extended to emit QP context (`sq_wqe_count`, `rq_wqe_count`, `rq_wqe_shift`, `qp_buf_addr` = `dvqp.rq.buf` since libmlx5 lays out RQ at offset 0 within the shared WQ buffer, `qp_db_addr` = `dvqp.dbrec`) via `mlx5dv_init_obj(MLX5DV_OBJ_QP)`. | yes (2026-06-01) | **STRONG PASS** (hardware-validated 2026-06-02). Full PASS battery: subtests 1, setup A/B, 3-9 (UAPI/dispatcher rejects), happy path (subtest 10), collision (subtest 11), Phase G QPC byte-equality vs. source pre-SAVE under live verb path (state, pd, q_key, uar_page, log_{page,sq,rq}_size, log_msg_max, user_index, cqn_snd, cqn_rcv, sw/hw rq counters, path_mtu, min_rnr_nak, log_rra_max, remote_qpn, primary_address_path 44-byte AV, next_rcv_psn, log_sra_max, retry_count, rnr_retry, next_send_psn, last_acked_psn -- 24 fields), subtest 13 leaf-DESTROY_QP semantics. Two harness-side bugs were found and fixed during validation: (a) FIFO SIGPIPE race in `start_{src,dst}_probe`'s `while IFS= read -r line < $fifo`-per-iteration pattern, fixed by holding fifo_out on a dedicated fd across the loop (commit cf8251f89e48); (b) subtest 1 anchored on RESTORE_QP whose mandatory IDR attrs short-circuit at -ENOENT before the gate fires, re-anchored on RESTORE_PD which has no IDR pre-conditions and reaches the shared `restore_check_ucontext` gate immediately (commit 8470fea1f23b). |
  | S6b B5 | dump-side verb (`MLX5_IB_METHOD_VFMIG_QUERY_QP` + `qp_query_probe_mlx5_vfmig` + `test_qp_query_mlx5_vfmig.sh` runner): kernel reads `base->mqp.qpn` / `trans_qp.base.ubuffer.umem->address` / `mlx5_ib_db_user_virt(&mqp->db)` / WQ-ring shape (`{sq,rq}.wqe_cnt`, `rq.wqe_shift`) / `mqp->flags_en` and emits a 64-byte payload byte-equal to `mlx5_ib_restore_qp_req` plus five scalar outs (type / state / user_handle / cap / create_flags). FW-side fields (uidx / bfreg_index / ece_options) are emitted as sentinels (0 / `MLX5_IB_INVALID_BFREG` / 0) since the QPC's user_index / uar_page / ece_options round-trip intact across LOAD per the K7 byte-equal proof and `mlx5_ib_restore_qp` validates-and-discards them. Mirror of S5b B5 for QP. Re-uses the `mlx5_ib_db_user_virt` accessor lifted at S5b B5 (designed to also serve QP per §5.2.4 lift-time docstring). Adds an `ib_qp_user_handle` accessor in `core/verbs.c` (declared in `<rdma/ib_verbs.h>`) because `struct ib_qp.uobject` is opaquely typed `struct ib_uqp_object *` (XRC bookkeeping requires the embedded `ib_uevent_object`), so driver-side dump verbs cannot reach `qp->uobject->uevent.uobject.user_handle` directly -- minimum API surface, EXPORT_SYMBOL'd for module use, returns 0 for kernel-mode QPs. CQ has no parallel accessor because `ib_cq.uobject` is `struct ib_uobject *` directly. Type gate (-EOPNOTSUPP for non-{RC,UC,UD}) mirrors `mlx5_ib_restore_qp`'s v0 type set; kernel-mode QP gate (-ENXIO when `ubuffer.umem == NULL` or `db_user_virt == 0`) mirrors QUERY_CQ. Cap is best-effort (mlx5 doesn't track max_send_wr / max_send_sge for user QPs per `mlx5_ib_query_qp`); the dispatcher seam doesn't validate cap content -- forward-compat surface only. | yes (2026-06-01) | **STRONG PASS** (hardware-validated 2026-06-02 via `test_qp_query_mlx5_vfmig.sh` on `mlx5_2`, phys_port_cnt=1). All 5 single-process subtests byte-equal across (A) `mlx5dv_init_obj(MLX5DV_OBJ_QP)` view and (B) kernel ioctl view: happy path (qpn / sq_wqe_count / rq_wqe_count / rq_wqe_shift / buf_addr / db_addr / type / state / user_handle / cap / create_flags), invalid-handle gate (-ENOENT), multi-QP disambiguation (two distinct QPs with distinct qpn / user_handle / buf_addr), type echo (RC vs UD distinct enum values), state echo (RESET -> INIT round-trip via `ibv_modify_qp`). Cross-process B6-style harness rejected as redundant: QUERY_QP is pure introspection, both processes would read from the same kernel state, and a wire-format misalignment would diverge identically across processes. The cross-process round-trip contract is already locked down at S6b B4 by the qp_restore harness's Phase G (which runs `MLX5_VFMIG_IOC_QUERY_QP` on the destination's adopted qpn and asserts byte-equality against the source pre-SAVE QPC snapshot). |
  | S6a A1 | UAPI `rxe_restore_qp_req` (sq_vm_pgoff / rq_vm_pgoff / req.psn / resp.psn / req.wqe_index / per-state attrs not on `ib_qp_attr`) | pending | -- |
  | S6a A2 | `rxe_restore_qp` handler: alloc fresh `rxe_qp` at source qpn, stamp every attr, set `qp->state` to captured final state, propagate `q->index` from buf indices, set `qp->req.psn` / `qp->resp.psn` / `qp->req.wqe_index`. Single-shot, no `ib_modify_qp` | pending | -- |
  | S6a A3 | `RXE_METHOD_VFMIG_FREEZE_DATAPATH` ucontext-scope verb + `rxe_qp_pause` / `rxe_qp_resume` helpers (?5.3.7). Non-destructive, no IBTA state-machine touch. mlx5_vfmig has no symmetric verb (FW SAVE_VHCA_STATE is the freeze) | pending | -- |
  | S6a A4 | dump-side verb (`RXE_METHOD_VFMIG_QUERY_QP` + `qp_query_probe_rxe`): kernel reads `qp->sq.queue->ip->info.offset` / `qp->rq.queue->ip->info.offset` / `qp->req.psn` / `qp->resp.psn` / `qp->req.wqe_index`; emits payload byte-equal to `rxe_restore_qp_req` | pending | -- |
  | S6a A5 | live verb path (`qp_restore_probe_rxe` + `test_qp_restore_rxe.sh`): RTS-with-pending-WRs happy path, FREEZE_DATAPATH lifecycle, synthetic-ACK kick, peer-driven retransmit observation | pending | -- |
* **S7: SRQ + AH (rxe + mlx5_vfmig together).** AH discovery lands on
  K2 from S2.
* **S8: Async event (rxe + mlx5_vfmig together).** Covers explicit AEF
  allocator path; default-AEF case already wired by ucontext restore.
* **S9: `ib_write_bw` end-to-end** on both drivers. Default polling
  mode (no comp channel involvement). **R3 v0 milestone.**
* **S10: `ib_write_bw` with `-e`** (event mode, exercises comp
  channels). Final v0 deliverable.

### 9.2 Test agent integration

The `/opt/builds/criu/scratch/rdma_test_agent_*.yaml` orchestrator
already exists; the rollout pattern from your message --

> Step by step I'd like to move rdma test agent steps from after
> criu/restore to before until we get past QP

\-- maps cleanly onto S2 through S6. Each S<N> moves the next test agent
step from "after-restore" to "before-restore" (i.e. covered by R3
restore rather than by post-restore re-creation).

### 9.3 Diagnostics

Mirror `user_mr_dma.md` ?5.2's rkey-logging idiom. CRIU emits
prefixed log lines per uobject restored:

```
rdma_r3: pid=<P> ufile=<U> type=PD source_handle=N -> dest_handle=M
         source_pdn=X dest_pdn=Y identity=ok
rdma_r3: pid=<P> ufile=<U> type=QP source_handle=N -> dest_handle=M
         source_qpn=0xN dest_qpn=0xM identity={preserved|fresh}
```

Easy `grep rdma_r3` to confirm what survived round-trip; no hard
assertions in CRIU itself, since FW behaviour is the empirical variable.

### 9.4 pd_restore_probe_rxe -- empirical S3a validation

Lives at
`tools/testing/mlx5_vfmig/uobject_restore/pd_restore/pd_restore_probe_rxe.c`.
Runs on any host with `CONFIG_RDMA_RXE=m`, no privileged access
required. The probe walks the full RESTORE_PD contract on rxe so we
can be sure the generic dispatcher (`uverbs_std_types_restore.c`)
is correct before mlx5 piles FW-id-hint complexity on top.

The probe exercises five subtests against `rxe0`:

1. **Gate (negative).** Open a ucontext WITHOUT
   `RXE_ALLOC_UCTX_RESTORE_MODE`. Invoke `UVERBS_METHOD_RESTORE_PD`
   with `target_handle = 0x4242`. Expect `-EPERM` from the
   per-driver `ucontext_is_restore_mode` predicate; the call must
   not touch the ufile's idr.

2. **Happy path.** Open a second ucontext WITH
   `RXE_ALLOC_UCTX_RESTORE_MODE`. Invoke `RESTORE_PD` with
   `target_handle = 0x4242`. Expect success. Verify via
   `UVERBS_METHOD_INFO_HANDLES(UVERBS_OBJECT_PD)` that the
   returned handle list contains `0x4242`. Optionally verify via
   NLDEV `RES_PD_GET` that the per-PD entry carries
   `RDMA_NLDEV_ATTR_RES_HANDLE = 0x4242` (cross-check against the
   K8a path already validated by `nldev_res_handle_probe`).

3. **Collision.** Invoke `RESTORE_PD(0x4242)` again on the
   restore-mode ucontext. Expect `-EBUSY` from the
   `xa_insert()` inside `rdma_alloc_begin_uobject_at_handle`.

4. **No interference with normal alloc.** Invoke the legacy
   write-path `IB_USER_VERBS_CMD_ALLOC_PD` (via libibverbs's
   `ibv_alloc_pd`). Expect a fresh `pd->handle` that is NOT
   `0x4242`; the restored PD remains addressable at `0x4242`.

5. **Destroy round-trip.** Invoke
   `IB_USER_VERBS_CMD_DEALLOC_PD(handle = 0x4242)`. Expect
   success and that `INFO_HANDLES` no longer returns `0x4242`.

Combined with `nldev_res_handle_probe` (already PASS on the same
running kernel) this gives us full coverage of the
PD-restore-via-handle flow before any mlx5 work is touched.

Failure modes the probe explicitly distinguishes:
* `-EOPNOTSUPP` from RESTORE_PD on a restore-mode ucontext =>
  generic dispatcher couldn't find `dev->ops.restore_pd` (rxe ops
  registration regression).
* `-EPERM` on a restore-mode ucontext => `rxe_ucontext_is_restore_mode`
  is misreading the bit or the rxe alloc-ucontext udata parse is
  wrong.
* Success on a NON-restore-mode ucontext => predicate isn't being
  consulted (security regression).

### 9.5 mr_restore_probe_rxe -- empirical S4a validation

Lives at
`tools/testing/mlx5_vfmig/uobject_restore/mr_restore/mr_restore_probe_rxe.c`.
Mirrors `pd_restore_probe_rxe`'s shape but exercises the full
`RESTORE_MR` contract: the dispatcher choreography (per-driver
gate, atomic ufile-handle reservation, IDR PD lookup, attribute
parsing), the rxe identity-hint honouring landed in S4a, the
cross-uobj refcount edge (`atomic_inc(&pd->usecnt)`), and the
post-restore destroy path.

Eight subtests against `rxe0`:

1. **Gate (negative).** Ucontext WITHOUT
   `RXE_ALLOC_UCTX_RESTORE_MODE` cannot invoke `RESTORE_MR`;
   expect `-EPERM`.
2. **lkey != rkey rejection.** Hint pair with `lkey != rkey`
   must `-EINVAL` before any pool install. Mirrors rxe's
   invariant that `ibmr.lkey == ibmr.rkey` on first install.
3. **Happy path.** `RESTORE_MR(target=0x4242, hint key=0x00424200)`
   under a fresh PD. Assert response `lkey`/`rkey` are
   byte-identical to the hint (S4a identity contract) and that
   the MR handle shows up in `INFO_HANDLES(MR)`.
4. **Collision (ufile handle).** Second
   `RESTORE_MR(target=0x4242)` returns `-EBUSY` from the
   dispatcher's `rdma_alloc_begin_uobject_at_handle`.
5. **Collision (pool index).** `RESTORE_MR` with a fresh target
   handle but the same key hint returns `-EBUSY` from
   `__rxe_add_to_pool_at_index`. Distinct collision shape from
   (4); CRIU surfaces both as the same errno.
6. **Bogus parent PD.** `RESTORE_MR` with an unknown `pd_handle`
   returns `-ENOENT` from the IDR attr machinery.
7. **Cross-uobj refcount.** While the restored MR is alive,
   `DEALLOC_PD` on its parent returns `-EBUSY` -- proves the
   `atomic_inc(&pd->usecnt)` installed by the dispatcher is the
   right edge.
8. **Dereg round-trip.** `DEREG_MR` clears the MR; `INFO_HANDLES`
   no longer reports it; the parent PD's `DEALLOC_PD` now
   succeeds.

The S4a identity check in subtest 3 is the load-bearing new
property over the ?9.4 PD shape: any future rxe-side change that
silently dropped the hint-honouring (e.g. a refactor of
`__rxe_add_to_pool_at_index` or the post-init key overwrite in
`rxe_restore_mr`) would surface as a `lkey != hint` mismatch
that this subtest catches.

### 9.6 mr_restore_probe_mlx5_vfmig -- empirical S4b validation

Lives at
`tools/testing/mlx5_vfmig/uobject_restore/mr_restore/mr_restore_probe_mlx5_vfmig.c`.
Mirrors `pd_restore_probe_mlx5_vfmig`'s shape -- driver-end-to-end
probe driven by an out-of-process shell harness across SAVE/LOAD,
with a READY checkpoint that lets the harness run an
out-of-process FW-side verifier (`MLX5_VFMIG_IOC_PROBE_MKEY`) while
the adopted MR + its parent adopted PD are alive on the
destination ucontext. Eight subtests against a fresh dst VF after
LOAD_VHCA_STATE:

1. **Gate (negative).** A ucontext WITHOUT
   `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE` cannot invoke
   `RESTORE_MR`; expect `-EPERM` from the dispatcher's
   `restore_check_ucontext` predicate. The probe ALLOC_PDs a
   throwaway PD on the non-restore-mode ucontext first, since
   the dispatcher resolves `UVERBS_ATTR_RESTORE_MR_PD_HANDLE`
   (an `UVERBS_ATTR_TYPE_IDR`) before the gate predicate runs;
   without a valid PD handle we'd get `-ENOENT` from the IDR
   resolver and never observe `-EPERM`.
2. **UAPI reject: `mkey_index = 0`.** `mlx5_ib_restore_mr_req
   .mkey_index = 0` -> `-EINVAL` from the handler's
   `mkey_index == 0` sentinel guard.
3. **UAPI reject: `reserved != 0`.** `req.reserved = 0xDEAD`
   -> `-EINVAL` from the forward-compat reservation check.
4. **UAPI reject: `lkey != rkey`.** Hint pair with
   `lkey_hint != rkey_hint` -> `-EINVAL` from the mlx5
   user-MR invariant check.
5. **UAPI reject: `(lkey >> 8) != mkey_index`.** Hint encodes
   a different mkey index than the UHW payload -> `-EINVAL`
   from the wire-visible identity cross-check. The
   load-bearing UAPI invariant; catches CRIU bugs that ship
   a restrack id where a FW key was expected.
6. **Happy path.** Prereq `RESTORE_PD` lands the parent PD,
   then `RESTORE_MR(target=mr_target_handle, lkey=rkey=src_lkey,
   mkey_index=src_mkey_index, addr/length/iova/access_flags=...
   from source pre-SAVE)`. Assert `RESP_LKEY` / `RESP_RKEY` are
   byte-identical to the hint (mlx5 always honours -- this is
   the wire-visible identity contract distinct from rxe's S4a)
   and that the MR handle shows up in `INFO_HANDLES(MR)`.
7. **Collision (ufile handle).** Second
   `RESTORE_MR(target=mr_target_handle)` on the same ucontext
   -> `-EBUSY` from `xa_insert` inside
   `rdma_alloc_begin_uobject_at_handle`.
8. **v0 dealloc semantics (asymmetric with PD).** After the
   READY checkpoint and the harness's FW-liveness check
   (`PROBE_MKEY`), `DEREG_MR(mr_target_handle)` -> 0 (mkey is
   a leaf in the FW resource graph; FW DESTROY_MKEY accepts
   even with the source's mkey-using QPs still alive in dst
   FW). Assert `INFO_HANDLES(MR)` no longer reports the
   handle (uobj freed). Compare with
   `pd_restore_probe_mlx5_vfmig`'s subtest 7 where the
   inverse holds for PD -- the test catches any future
   regression that silently flipped the asymmetry. See ?10.8.

The Phase G external check (`MLX5_VFMIG_IOC_PROBE_MKEY` against
the live adopted mkey) runs out-of-band of the verb path: the
probe parks at READY with the adopted MR alive, and the harness
issues raw FW `QUERY_MKEY` via the PF cdev. fw_accept=1 +
byte-equal mkc context (`fw_pd`, `fw_start_addr`, `fw_length`)
against the source's pre-SAVE values is the strongest empirical
evidence that Model A's no-FW-round-trip adoption keeps the
destination FW state intact under the live verb path. On a
disagreement the harness emits a "WEAK PASS" verdict, treating
that as a yellow flag rather than an automatic FAIL since a
benign cause (FW field-packing change between releases) is
plausible; investigate before claiming Model A correctness on
that FW.

Failure modes the probe distinguishes (mirrors ?9.4's PD list):

* `-EOPNOTSUPP` from RESTORE_MR on a restore-mode ucontext =>
  `mlx5_ib_restore_mr` not registered in `mlx5_ib_dev_ops`.
* `-EPERM` on a restore-mode ucontext =>
  `mlx5_ib_ucontext_is_restore_mode` not reporting true,
  typically because the ucontext alloc didn't see the
  `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE` flag.
* Uniform `-EINVAL` across all subtests => attr-bundle decode
  failure before the dispatcher runs. The two known shapes of
  this footgun: (a) `RESP_LKEY`/`RESP_RKEY` are `UA_MANDATORY`
  in the UAPI declaration, so they must be wired up on every
  call (negative subtests too); (b) `PD_HANDLE` is
  `UVERBS_ATTR_IDR` and uses `len = 0 + data = ufile handle`
  on the wire, not `len = 4`. Both are documented inline in
  the probe source.
* mkc-content mismatch under PROBE_MKEY (Phase G WEAK PASS) =>
  either `LOAD_VHCA_STATE` lost FW mkey state, or a FW-PRM
  packing change. Investigate against
  `test_fw_id_continuity.sh K6` and `test_mr_adopt.sh` in
  isolation to pin the source.

### 9.7 cq_restore_probe_rxe -- empirical S5a validation

Lives at
`tools/testing/mlx5_vfmig/uobject_restore/cq_restore/cq_restore_probe_rxe.c`.
Runs on any host with `CONFIG_RDMA_RXE=m`, no privileged access
required. Mirrors `pd_restore_probe_rxe`'s shape (no SAVE/LOAD
round-trip, no FW-side verifier -- rxe has no FW; the probe's job is
to lock in the dispatcher contract before mlx5 layers cqn-adoption
on top in S5b).

Six subtests against `rxe0`:

1. **Gate (negative).** Open a ucontext WITHOUT
   `RXE_ALLOC_UCTX_RESTORE_MODE`. Invoke `RESTORE_CQ` with
   `target_handle = 0x4242, cqe = 64, comp_vector = 0,
   user_handle = 0xDEADBEEF`. Expect `-EPERM` from the per-driver
   `ucontext_is_restore_mode` predicate; the ufile idr must not be
   touched.
2. **Bad comp_vector.** On a restore-mode ucontext, invoke
   `RESTORE_CQ` with `comp_vector = UINT_MAX` (out of range).
   Expect `-EINVAL` from the dispatcher's `comp_vector >=
   ib_dev->num_comp_vectors` guard. Validates the dispatcher
   pre-checks before reaching the driver callback.
3. **Happy path.** Invoke `RESTORE_CQ(target = 0x4242, cqe = 64,
   comp_vector = 0, user_handle = 0xDEADBEEF, no comp_channel,
   no event_fd)`. Expect success. Assert (a) `RESP_CQE` is
   non-zero (rxe-internal cqe count after `rxe_cq_chk_attr`
   normalization), (b) `INFO_HANDLES(UVERBS_OBJECT_CQ)` includes
   `0x4242`, (c) NLDEV `RES_CQ_GET` reports
   `RDMA_NLDEV_ATTR_RES_HANDLE = 0x4242` (cross-check with K8a
   path; user-visible identity continuity).
4. **Collision.** Re-invoke `RESTORE_CQ(target = 0x4242, ...)` on
   the same restore-mode ucontext. Expect `-EBUSY` from
   `xa_insert()` inside `rdma_alloc_begin_uobject_at_handle`.
5. **COMP_CHANNEL rejected.** On the restore-mode ucontext,
   first allocate a comp channel via the legacy
   `IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL` write-cmd
   (`ibv_create_comp_channel()` from libibverbs); pass the cc fd
   as `UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL` on a
   `RESTORE_CQ(target = 0x4243, ...)` call. Expect `-EOPNOTSUPP`
   from the dispatcher's v0 forward-compat-reject guard. Locks in
   the design choice that the comp channel attr is declared but
   refused at v0; any future regression that silently accepts it
   would surface here.
6. **Destroy round-trip.** Invoke
   `IB_USER_VERBS_CMD_DESTROY_CQ(handle = 0x4242)`. Expect
   success (rxe `cq->num_wq` is 0 with no QPs attached) and that
   `INFO_HANDLES` no longer returns `0x4242`. The S5a-on-rxe
   half of the v0 dealloc-ordering invariant: rxe restored CQs
   destroy cleanly because rxe has no FW graph and no kernel-side
   children at v0. (mlx5 S5b's analogous subtest 8 will assert
   the inverse -- BAD_RES_STATE -- because mlx5 FW does carry
   cqn in QPCs as a tracked dep; ?10.8.)

Failure modes the probe explicitly distinguishes (mirrors ?9.4 and
?9.5):

* `-EOPNOTSUPP` on a restore-mode ucontext for subtests 3 / 4 / 6
  => `rxe_restore_cq` not registered in `rxe_dev_ops` (rxe ops
  registration regression).
* `-EPERM` on a restore-mode ucontext => `rxe_ucontext_is_restore_mode`
  is misreading the bit or the rxe alloc-ucontext udata parse is
  wrong.
* Success on subtest 5 => the dispatcher's COMP_CHANNEL reject
  guard is disabled -- security-adjacent (forward-compat policy
  drift is the kind of bug that surfaces years later and is hard
  to back out cleanly).
* RESP_CQE absent on subtest 3 => the dispatcher is not setting
  the OUT attr after the driver callback (UA_MANDATORY violation
  would already have been caught by attr-bundle decode, so this
  is the post-callback wire-up); see ?9.6's comparable note for
  RESP_LKEY/RESP_RKEY on RESTORE_MR.

## 10. Open questions

1. **K6 outcome**: does `LOAD_VHCA_STATE` preserve PD/CQ/QP/SRQ/MKEY id
   reservations? **Answered (2026-05-13)**: PARTIAL PASS -- preserved
   for PD/CQ/QP/MKEY (SRQ skipped pending the known restored-VF SRQ
   gate, deferred to S7). Combined with the `pd_adopt` follow-up
   (`uobject_restore/pd_adopt/test_pd_adopt.sh`, WEAK PASS), which
   established that `CREATE_MKEY` under `uid=0` is FW-ungated on
   `mkc.pd` validity, the v0 mlx5 restore handlers can adopt
   source FW ids directly into fresh kernel-side wrappers (Model
   A, "no destination FW round-trip"); no FW patch required.
   `mlx5_ib_restore_pd` landed in S3b (C3+C4). See
   `test_fw_id_continuity.sh` and `test_pd_adopt.sh`.

   **Addendum (2026-05-15) -- DEVX adoption empirically
   ruled out for v0.** The DEVX-source variant of `test_pd_adopt.sh`
   shows that `LOAD_VHCA_STATE` preserves the FW `next_free_uctx`
   *counter* but not the uctx-registration *table*; every
   user-range uid (registered on source or not) rejects
   `CREATE_MKEY` post-LOAD with a consistent "unknown uid"
   syndrome (`0x76555f` on FW 28.48.1000). The
   `MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID` UAPI flag and the
   `adopt_devx_uid` field stay in tree but are vestigial; the
   v0 plugin mitigation is to NOT propagate the source's
   `devx_uid` and run every adopted resource under `uid=0`.
   Full empirical chain + future-FW options live in ?9.1 S3b
   "DEVX-adoption blind spot".
2. **Pending RQ/SQ WR preservation across `LOAD_VHCA_STATE`**:
   **Answered (2026-05-13)**: structural PASS -- the FW QPC
   round-trips byte-equal across LOAD_VHCA_STATE for all 13 fields we
   sampled (state, pd, q_key, cqn_snd/rcv, srqn_rmpn_xrqn, all PSN
   fields, both sq/rq counter pairs). User-mode RQ WQE buffer + DB
   page are independently preserved via `user_mr_dma` Stage-2
   `HOST_USER_PAGE` SAVE/LOAD wire path + Stage-3
   `vfmig_iova_bind_user_object` (call chain to be wired in by S6
   via `mlx5_ib_umem_restore_qp`/`mlx5_ib_db_map_user_restore`).
   No driver-side `QUERY_QP_PENDING_WRS` ioctl needed.
   Caveat: live RTR/RTS confirmation is gated on the netdev TX-dropper
   on tracked VFs and was not run; structural argument is sufficient
   for v0. See `test_fw_id_continuity.sh K6_POST_RECV_WRS=N` and ?6.3.
3. **modify_qp split**: per-uobj restore lands QP in RTR or RTS? ?6.3
   commits to RTR + fini-pass RTS as default; revisit if concrete
   problems arise.
4. **Plugin-private xref encoding**: when DM/DEVX land, plugin-internal
   xrefs (e.g. DEVX QP -> DEVX UAR) need a representation. v0 doesn't
   exercise; doc commits to "plugin-defined sub-blob; generic CRIU
   doesn't see internal edges".
5. **AH cache identity / libibverbs in-process state**: libibverbs keeps
   per-context caches and bookkeeping (AH cache, QP table, MR registration
   index, etc.) in the user process's address space. By construction CRIU
   restores process memory verbatim, so any pure-userspace bookkeeping
   round-trips for free. The remaining question is whether libibverbs's
   userspace state contains stale references to *kernel* identifiers (AH
   handles, restrack ids, anything we re-allocate at restore time).
   Introspecting libibverbs's own state structures from CRIU would mean
   reading user-VA contents and parsing them as libibverbs internals,
   which is an unstable user ABI. Plan: where feasible, re-derive what
   userspace would have cached by querying the kernel after restore (the
   per-class restore handlers already produce the authoritative state).
   Concrete ask falls out per-case as we hit it; v0 records the shape and
   defers actual integration until a concrete failure surfaces. See also
   ?10.6 (partial-restore atomicity) -- both share the "what userspace
   cached vs what the kernel knows" axis.
6. **Restore vs SOCK_SEQPACKET-style atomicity**: each per-uobj RESTORE_*
   ioctl is atomic in itself, but a multi-uobj restore is not atomic as
   a whole. If the restore fails partway, the partially-constructed
   ucontext may be in a weird state. Mitigation: treat partial-restore
   failure the same as restore failure (CRIU bails the whole process).
   Defer "incremental restore" to follow-on if ever needed.
7. **CM_ID full state continuity**: the RDMA-CM state machine + listen
   state + IP/GID resolution cache are above the uobject layer. v0
   preserves CM_ID identity but not the full CMA state. Application
   re-establishes on top. Follow-on if needed.
8. **Per-class dealloc semantics asymmetry under v0 -- the FW
   resource-graph axis**:
   **Discovered (2026-05-17) for PD/MR; CQ row validated
   (2026-05-29) by `cq_restore_probe_mlx5_vfmig` subtest 10 on
   FW 28.48.1000 -- orphan `DESTROY_CQ` rejected with
   BAD_RES_STATE -> `-EINVAL` while LOAD_VHCA_STATE'd QPC/SRQC
   dependents remain. Recorded, not blocking.**

   The axis is "is this resource a *parent* in the FW resource
   graph (i.e. does FW track other resources as children that
   reference it)?" Resources on the parent side block orphan
   dealloc with BAD_RES_STATE until their children are gone;
   resources on the leaf side dealloc cleanly even with
   referencing siblings still alive.

   | class | FW-graph role at v0 | orphan dealloc | how validated |
   |-------|---------------------|----------------|---------------|
   | PD | parent (CQ/QP/MR/SRQ children reference pdn) | rejected `BAD_RES_STATE` -> `-EINVAL` | `pd_restore_probe_mlx5_vfmig` subtest 7 |
   | MR | leaf (QPs reference by wire (l/r)key, not tracked) | accepts `DESTROY_MKEY` -> 0 | `mr_restore_probe_mlx5_vfmig` subtest 8 |
   | CQ | parent (QPCs reference cqn_snd/cqn_rcv as tracked dep; SRQ context too) | rejects `BAD_RES_STATE` -> `-EINVAL` until S6 drains the source's cqn-using QPs | `cq_restore_probe_mlx5_vfmig` subtest 10 (validated 2026-05-29) |

   **MR side** (the original surprise): in the FW resource graph
   mkey is a leaf under PD; QPs reference an mkey by its
   (lkey/rkey) wire value rather than as a tracked FW resource
   dependency, so FW has nothing to refuse against. Empirically
   established by `mr_restore_probe_mlx5_vfmig`'s subtest 8.

   **CQ side** (validated 2026-05-29 with B4 STRONG PASS): cqn
   IS a tracked dep -- `qpc.cqn_snd`/`cqn_rcv` and SRQ context
   all carry cqn as a FW reference. So the orphan adopted CQ
   behaves like an orphan adopted PD: FW rejects with
   BAD_RES_STATE -> `-EINVAL` until the source's cqn-using
   QPs/SRQs are themselves drained, which doesn't happen until
   S6/S7. v0 dealloc-ordering invariant for CQ inherits the PD
   shape exactly, as locked in by `cq_restore_probe_mlx5_vfmig`'s
   subtest 10 against FW 28.48.1000.

   **rxe is uniformly on the leaf side** (no FW graph at all):
   rxe `destroy_<class>` returns `-EINVAL` only when kernel-side
   refcounts (e.g. `cq->num_wq`, `pd->usecnt`) are non-zero, and
   at v0 those are zero for all restored objects since no QPs
   exist yet. So rxe restored PDs/MRs/CQs all dealloc cleanly at
   v0 -- the asymmetry is mlx5-specific.

   Implication for CRIU's plugin policy: the plugin destroys in
   reverse-creation order (AH -> QP -> SRQ -> CQ -> MR -> PD).
   On mlx5 the FW enforces the parent classes (PD, CQ) for free;
   the plugin must enforce ordering for the leaf classes (MR) on
   its own since the kernel will accept any DEREG_MR. The
   `beea656e494d` rdma_core gate for restore-mode ufiles
   suppresses the cleanup-loop WARN that would otherwise fire on
   end-of-process closure with parent classes still parked.

   This is *correct behaviour* on FW's part -- the asymmetry
   reflects the actual FW resource model. Anything that reverses
   the MR row (e.g. a hardening change that adds mkey-as-
   tracked-dep to QP) would surface as a subtest 8 regression;
   anything that reverses the CQ row (FW dropping cqn from QPC
   tracking) would surface as a subtest 8 regression at S5b.
   QP/SRQ rows fill in with S6/S7 -- both expected on the parent
   side (QPs are referenced by the source's CM_ID / QPC graph;
   SRQs are referenced by QPCs).

9. **TODO: drop unused `MLX5_IB_ALLOC_UCTX_DEVX` symbol from the
   pd_restore + mr_restore probes' inlined enums**:
   **Recorded (2026-05-29). Pure documentation hygiene; deferred
   intentionally to keep B4 series focused on CQ.**

   `cq_restore_probe_mlx5_vfmig.c` deliberately does NOT inline
   `MLX5_IB_ALLOC_UCTX_DEVX`/`ADOPT_DEVX_UID` next to
   `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE` (commit `43cb650ab55c`):
   the v0 RESTORE path lives exclusively on the `devx_uid = 0`
   lane per ?9.1 S3b "DEVX-adoption blind spot", and leaking the
   symbol into a probe's enum invites accidental
   `DEVX | VFMIG_RESTORE` usage that would silently undermine
   Model A by allocating a fresh devx_uid for the destination
   ucontext.

   The pre-existing `pd_restore_probe_mlx5_vfmig.c` (line 119)
   and `mr_restore_probe_mlx5_vfmig.c` (line 168) still inline
   `MLX5_IB_ALLOC_UCTX_DEVX = 1u << 0` next to the VFMIG flag.
   The symbol is defined but never referenced anywhere in those
   files; the only `do_get_context()` flags arguments are `0`
   (gate-negative subtest) or `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE`
   (happy path). So dropping the enum entry is functionally a
   pure no-op -- verified by `grep MLX5_IB_ALLOC_UCTX_DEVX` in
   each probe.

   When picked up, mirror the multi-paragraph comment from
   `cq_restore_probe_mlx5_vfmig.c`'s enum that explains:
     - why DEVX (bit 0) is omitted (would allocate fresh uid)
     - why ADOPT_DEVX_UID (bit 2) is omitted (uctx-registry
       wipe blind spot)
     - the v0 mitigation (`devx_uid = 0` lane proven by the
       P_zero / N_zero cells of the pd_adopt matrix)
     - how to re-introduce the bits if a future test needs the
       DEVX-aware lane

   Two atomic commits (one per probe) keep the diff trivially
   reviewable.

## 11. Sequencing relative to other work

* **Builds on** `uar_restore.md`: the ucontext + UAR restore is
  the substrate R3 lives on top of. R3 cannot start until ucontext
  restore works (it does, end-to-end as of `87e9813c6` in CRIU and
  matching kernel commits).
* **Coupled to** `user_mr_dma.md`: the user-MR DMA work
  preserves IOMMU-level page binding identity; R3 preserves
  uobject-level user-handle and rkey identity. The two
  converged at S4 (MR restore): S4b B2 (`mlx5_ib_restore_mr`
  Model A) + Stage-3 D1-D4 landed together as a coherent
  per-MR restore, with the kernel's IOMMU bind folded into the
  RESTORE_MR verb body so the plugin path stays a single ioctl
  per uobject. S5/S6/S7 (CQ/QP/SRQ restore) extend the same
  pattern: their kernel handlers will compose
  `mlx5_ib_umem_restore_<class>` (parallel to
  `mlx5_ib_umem_restore_mr`) onto the corresponding
  `KIND_<CLASS>` placeholder, with the destination's user
  buffers + DBR page rebound from `vfmig_iova_replay_external`
  records emitted by Stage-2 C7-C10 source retags. Stages 1-3
  (UAR / ucontext / PD) are independent of `user_mr_dma` and
  landed first.
* **Precedes** XRC / DM / DEVX uobject continuity work. The `LIST_UOBJS`
  (K2) and per-class restore (K3/K4) machinery extends naturally to those
  types when needed.
* **Precedes** any "live RDMA migration" (SAVE without quiescence)
  effort. R3 assumes a quiesced source per the existing CRIU dump model.

## 12. References

* `uar_restore.md` -- ucontext + UAR restore (R3's substrate).
* `user_mr_dma.md` -- user-MR DMA continuity (R3's MR-restore
  partner).
* `criu/rdma.c` -- existing CRIU RDMA core (claim arbitration,
  pre-suspend coverage check, NLDEV scaffolding).
* `criu/plugins/rdma/` -- existing rxe + mlx5_vfmig plugins (will host
  R3 plugin contributions).
* `drivers/infiniband/core/nldev.c` -- where K1 lands.
* `drivers/infiniband/core/uverbs_std_types*.c` -- where K2 + K3 land.
* `include/rdma/ib_verbs.h` -- where K4 lands (`ib_device_ops`).
* `include/uapi/rdma/ib_user_ioctl_cmds.h` -- where K3's UAPI lands.
* `tools/testing/mlx5_vfmig/save_load/test_iova_tracked_save_load.sh` -- existing kernel-side
  end-to-end test harness; R3's S6/S8 will extend it.
* `criu/test/rdma/run_uverbs_cr.sh`, `criu/test/rdma/run_vfmig_cr.sh` --
  existing CRIU-side end-to-end tests; today fail at the PD-uobject gap;
  R3 v0 closes them.
