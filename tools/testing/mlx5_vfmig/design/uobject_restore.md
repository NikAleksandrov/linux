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

Concise, ordered list. Each item links to its detailed §; together they are
the full driver-side surface this doc requires. CRIU-side work has zero
upstream-kernel dependency and lands in parallel; the kernel asks gate
end-to-end correctness, not initial scaffolding.

| # | ask | where | priority | size |
|---|---|---|---|---|
| K6 | **v0 gate.** FW-identity-continuity experiment: does `LOAD_VHCA_STATE` preserve PD/CQ/QP/SRQ/MKEY id reservations the way it provably does for UARs? Mirrors `uar_restore.md` §3. Outcome decides whether K3/K4 mlx5 handlers are a small alloc-with-hint extension (best case) or require new "pre-reserve id N" FW commands (worst case, possibly FW patch). Run this first. | §8.2, §10 | **very high** | empirical experiment + small probe ioctl |
| K2 | **Already exists upstream as `UVERBS_METHOD_INFO_HANDLES` on `UVERBS_OBJECT_DEVICE`** (drivers/infiniband/core/uverbs_std_types_device.c). Takes a `UVERBS_ATTR_INFO_OBJECT_ID` (u16 -- accepts ANY core or driver-namespace object id via `uapi_key_obj()`), walks `ufile->uobjects` under `uobjects_lock` filtered by `obj->uapi_object`, returns `UVERBS_ATTR_INFO_HANDLES_LIST` (u32[]) and `UVERBS_ATTR_INFO_TOTAL_HANDLES` (filled count). Covers AH and every other non-restracked uobject. Drives the pre-suspend coverage check (DEVX/MW/FLOW/XRCD rejection) by enumerating those types and failing the dump if any are present. Validated end-to-end by `info_handles_probe` -- see §7.2 | §6.2 | done | zero kernel work |
| K2.5 | Wire up the **existing** `rdma_alloc_begin_uobject_at_handle()` helper (already in `drivers/infiniband/core/rdma_core.c`, added by the UAR restore work) into every K3 `RESTORE_<TYPE>` method. The primitive — XA-insert at caller-specified handle, return `-EBUSY` if taken — is already proven by the UAR restore path; this is plumbing, not new core | §7.3 | medium | reuse existing helper |
| K3 | New generic uverbs method namespace `UVERBS_OBJECT_RESTORE` with one method per uobject class: `RESTORE_PD`, `RESTORE_CQ`, `RESTORE_COMP_CHANNEL`, `RESTORE_SRQ`, `RESTORE_QP`, `RESTORE_MR`, `RESTORE_AH`, `RESTORE_ASYNC_EVENT`. Each takes (target user_handle, hw-agnostic attrs, opaque blob, parent_handle xrefs). Dispatches through new `ib_device_ops.restore_<type>` callbacks. Gated by a new hw-agnostic `IB_UCONTEXT_RESTORE_MODE` ucontext flag, which mlx5_vfmig sets when the ucontext was opened with `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE` and rxe sets when opened with the corresponding rxe restore-mode flag. Generic verb checks the hw-agnostic flag only | §7.1, §7.2 | high | medium per type |
| K4 | `ib_device_ops` extended with `restore_pd`, `restore_cq`, `restore_qp`, `restore_mr`, `restore_srq`, `restore_ah`, `restore_comp_channel`, `restore_async_event`. Each driver installs its restore-mode ops vector once, at VF/device **probe** time, when the device is entering VFMIG_RESTORE state (i.e. before any uverbs cdev opens against it). No mid-life ops swapping | §7.3, §7.4 | high | one ops vector + per-driver impl |
| K8 | **v0 required.** Expose the per-uobject `ufile_handle` (= `obj->id` from `ufile->uobjects`) at dump time, paired with `restrack_id`. K3's `target_handle` install semantic requires CRIU to know `ufile_handle` per uobject so the destination kernel re-installs at exactly the value user code's restored memory still references. Two viable shapes -- (K8a) one `nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_HANDLE, <type>->uobject->id)` per `fill_res_<type>_entry` (PD/CQ/QP/MR/SRQ -- five lines); (K8b) extend `UVERBS_METHOD_INFO_HANDLES` with an optional `UVERBS_ATTR_INFO_RESTRACK_LIST` u32[] paired with `INFO_HANDLES_LIST`. Without K8 there is no way to cross-join NLDEV's `restrack_id` view (used for parent-edge encoding) with `INFO_HANDLES`'s `ufile_handle` view (used for `target_handle` install). K8a preferred -- symmetric with existing PDN/CQN/MRN/SRQN emission, smaller patch | §7.5 | high | one nla_put per fn |
| K5 | (already landed) `show_fdinfo` for cdev (`52721d09a`), async event fd (`551a1355f`), comp event fd (`a753315597`). No further fdinfo work | §6.3 | done | -- |
| K7 | (optional, stretch) Add restrack entries for AH (`RDMA_RESTRACK_AH`). If we land K2, this is unnecessary -- but adding it later is cheap if K2 ends up not landing | §6.2 | low | optional |
| K1 | (deprioritized, optional cleanup) Add `RDMA_NLDEV_ATTR_RES_CTXN` emission in `fill_res_qp_entry`, `fill_res_mr_entry`, `fill_res_srq_entry`, `fill_res_cm_id_entry`. Not v0-blocking: CRIU joins QP/MR/SRQ to ctxn through PDN against the PD inventory (PD entries already emit CTXN). Land only if a follow-on need surfaces | §6.1 | very low | one line per fn |

**v0 ordering**: K6 first (gates K3/K4 mlx5 design) -- **done, PARTIAL
PASS, see §10**. K2 in parallel -- **discovered already implemented as
`UVERBS_METHOD_INFO_HANDLES`, no kernel work; see §7.2**. K2.5 / K3 / K4
/ K8 implement the restore path, callback-by-callback across rxe +
mlx5_vfmig per class (PD -> MR -> CQ -> QP; see §9.1). K8 unblocks
correct `target_handle` propagation and is required before any K3
`RESTORE_<TYPE>` handler can be exercised end-to-end; K8a is a trivial
patch and lands ahead of (or alongside) the first K3 method. K5 is
already done. K1 and K7 are deferred / optional.

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
  reservations across `LOAD_VHCA_STATE` (see §10/K6 for the empirical question).

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
* **Multi-process / cross-tree**: out of scope for v0 in the sense that
  cross-tree exclusivity check (already in CRIU) refuses dump if the same
  ibdev is shared across the dump tree boundary by an EXCLUSIVE plugin.
  SHAREABLE (rxe) plugins permit it.

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
* MW/FLOW/XRCD/DM/DEVX (deferred per §1.3).
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
  §3): cross-host empirically confirmed. Source's FW UAR ids re-installable
  on destination via the new RESTORE_UCONTEXT/RESTORE_DYN_UARS verbs.
  R3 leans on the same property for the rest of the uobject ids if K6 holds.
* **User-MR DMA on tracked VFs** (`user_mr_dma.md` stage 1): landed
  in kernel HEAD. `ibv_reg_mr` succeeds on a tracked VF without the
  `IB_WC_MW_BIND_ERR` failure mode. Means MR creation in the destination
  (during R3 restore) won't itself fail at the IOMMU layer; only the
  identity continuity (rkey preservation) is open.
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
  LOAD_VHCA_STATE.** Mirror the `uar_restore.md` §3 experiment shape:
  on host A, allocate uobjects of each kind, log their FW ids, SAVE; on
  host B, LOAD, then probe "what's the next id FW would hand out?" via a
  PF-cdev ioctl. If next-id starts above the SAVE-time peak for a class,
  reservations are preserved (good); if it starts at zero, they aren't
  (bad, need pre-reserve verbs).
* Until K6 is run, the design assumes preservation by analogy to UARs and
  flags the failure mode in §10.

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
  discontinuity stage 3 of `user_mr_dma.md` solves for MRs).
  No mitigation in v0; lifted only if a real consumer needs it.
* `xref` edges always reference `restrack_id` (the cross-resource key).
  Restore translates `restrack_id` -> `ufile_handle` via the per-ufile
  handle map.
* Identity hints (qp_num, sq_psn, rq_psn, q_key, lkey, rkey, ...) are
  always populated on dump and always passed to RESTORE on restore. The
  per-uobject-class restore handler decides whether failure to honour is
  fail-loud or silent-fallback (§4.4).
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

For software providers (rxe), identity hints are honoured by extending the
allocator with "prefer this id" semantics. Trivial change: rxe already uses
`xa_alloc` for QPN/MKEY; we add an `xa_insert(prefer_id)` fallback path
behind a flag.

## 5. Per-uobject-class details

For each class: discovery source, kernel verb args, kernel handler shape,
plugin contribution.

### 5.1 PD

* **Discovery**: NLDEV `RES_PD_GET` (CTXN already emitted today). Yields
  hw-agnostic attrs (none beyond access flags, which aren't currently in
  NLDEV but are recoverable from `ib_pd->flags`); plugin may add FW pdn
  via `MLX5_IB_METHOD_VFMIG_QUERY_PD(handle)` returning `{fw_pdn}`.
* **Kernel verb**: `UVERBS_METHOD_RESTORE_PD(target_handle, alloc_flags,
  blob)`. Calls `ib_dev->ops.restore_pd()`.
* **Driver-side (mlx5_vfmig)**: allocates a `mlx5_ib_pd`, calls FW
  `ALLOC_PD` with a hint for the source's pdn (or relies on
  LOAD_VHCA_STATE having reserved it; K6 decides). Installs at
  `target_handle`.
* **Driver-side (rxe)**: standard `rxe_alloc_pd()` plus install-at-handle.
  No FW state.

### 5.2 CQ + comp channel fd

* **Discovery (CQ)**: NLDEV `RES_CQ_GET` (CTXN already emitted). Yields
  cqe count, dim flag, comp_vector (if exposed), poll_ctx for kernel CQs.
  Plugin adds FW cqn + per-cqe-buffer iova map via
  `MLX5_IB_METHOD_VFMIG_QUERY_CQ(handle)`.
* **Discovery (CC)**: `LIST_UOBJS(UVERBS_OBJECT_COMP_CHANNEL)` (K2).
  Returns handles only.
* **Xref**: CQ -> CC via `XR_COMP_CHANNEL` if the CQ was created with a
  comp channel.
* **Restore order**: CC before CQ.
* **Kernel verb (CC)**: `UVERBS_METHOD_RESTORE_COMP_CHANNEL(target_handle)`
  -> `ib_dev->ops.restore_comp_channel()`. Allocates the
  `ib_uverbs_completion_event_file` uobject and its associated fd; CRIU's
  fd-table machinery installs the fd at the user's saved fd number.
* **Kernel verb (CQ)**: `UVERBS_METHOD_RESTORE_CQ(target_handle, attrs,
  blob, comp_channel_handle?)`. The PD reference doesn't apply at CQ-create
  time in modern uverbs; comp_channel_handle is the optional xref.

### 5.3 QP

* **Discovery**: NLDEV `RES_QP_GET`, ctxn derived via PDN-join against
  the PD inventory (one-hop after K1). Yields type, port, qp_state,
  src/dst PSN, src/dst QPN. Hw-agnostic `ib_qp_attr`
  fields not in NLDEV are recoverable via `IB_USER_VERBS_CMD_QUERY_QP`
  (existing uverbs command, no kernel change needed). Plugin adds FW qpn
  context via `MLX5_IB_METHOD_VFMIG_QUERY_QP(handle)` returning
  `{fw_qpn, fw_uar_idx, fw_resp_buf_iova_map, ...}`.
* **Xrefs**: PD (XR_PARENT_PD), send_cq (XR_SEND_CQ), recv_cq (XR_RECV_CQ),
  optionally SRQ (XR_SRQ).
* **Restore order**: after PD, CQ, SRQ.
* **Kernel verb**: `UVERBS_METHOD_RESTORE_QP(target_handle, init_attr,
  attr, attr_mask, blob, pd_handle, send_cq_handle, recv_cq_handle,
  srq_handle?)`. Honours qp_num as identity hint.
* **Driver-side (mlx5_vfmig)**: allocates `mlx5_ib_qp`, calls FW
  `CREATE_QP` with qpn hint, applies `MODIFY_QP` chain through INIT->RTR->RTS
  to land in the saved state with all PSN/QKEY/AV fields. Activation pass
  (§4.3 hook 3, or restore-fini per §6 below) re-posts any RQ WRs.
* **Driver-side (rxe)**: standard `rxe_create_qp` + identity-hint
  extension on QPN allocation, plus QP state machine replay.

### 5.4 MR

* **Discovery**: NLDEV `RES_MR_GET`, ctxn derived via PDN-join (one-hop
  after K1). Yields MRLEN, RKEY/LKEY (with CAP_NET_ADMIN). Other fields
  (virt_addr,
  access_flags, iova) recoverable via uverbs query path. Plugin adds FW
  mkey context via `MLX5_IB_METHOD_VFMIG_QUERY_MR(handle)`.
* **Xref**: PD (XR_PARENT_PD).
* **Restore order**: after PD.
* **Kernel verb**: `UVERBS_METHOD_RESTORE_MR(target_handle, virt_addr,
  length, access_flags, lkey_hint, rkey_hint, blob, pd_handle)`. Honours
  rkey/lkey as identity hints.
* **Driver-side (mlx5_vfmig)**: see also `user_mr_dma.md` stage 3,
  which is the user-MR-DMA-side counterpart to this verb. R3 owns the
  uobject identity; user_mr_dma stage 3 owns the IOMMU-side mkey-keyed
  page binding. The two land together as a coherent per-MR restore.
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

Already covered in §5.2 alongside CQ.

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
        if uobj.type == MR:
            plugin.post_restore_mr(uobj, new_handle)         # see below
```

v0 ships exactly **one** plugin hook: `post_restore_mr`. It exists
because MR restore couples with `user_mr_dma.md` stage 3 (rkey
continuity at the IOMMU layer) -- the post-MR hook is where the plugin
re-binds the per-MR IOVA map after the kernel verb has allocated the
mkey. Every other class restores cleanly from the kernel verb alone in
v0; we don't pre-declare empty hooks for them. If a future per-class
quirk surfaces (DM/DEVX, or a mlx5e-style netdev coupling), we add the
hook at that point. CRIU core handles fd-table install for CC/AEF
directly; no plugin hook needed.

### 6.3 Restore-fini activation pass

After all per-ufile restores are complete (and all peer state is in place,
implicit by the dump tree being one connected unit):

```
For each ufile in image:
    For each QP in ufile (in QP creation order):
        # Already restored to its saved qp_state via the per-uobj restore
        # chain (INIT->RTR->RTS). Final transition + CQ re-arm only:
        rearm_cqs(qp.recv_cq, qp.send_cq)
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
piggyback driven by `K6_POST_RECV_WRS=N`):

```
On host A (source):
  (1) fw_id_continuity_probe alloc PD/CQ/QP(RC)/MR; modify_qp(INIT);
      post N receive WRs.
  (2) PF cdev QUERY_QP @ qpn -> record QPC subset.
  (3) SAVE_VHCA_STATE while the probe holds the QP alive.

On host B (dest, same host in our loopback test):
  (4) tear down source VF, fresh dest VF, LOAD_VHCA_STATE,
      MARK_RESTORED, bind.
  (5) PF cdev QUERY_QP @ same qpn -> record QPC subset.
  (6) Byte-equal compare.
```

Result (2026-05-13, ConnectX-6 Dx, 5.6MB blob):

```
state, pd, q_key, cqn_snd, cqn_rcv, srqn_rmpn_xrqn,
next_send_psn, next_rcv_psn, last_acked_psn,
hw/sw sq_wqebb_counter, hw/sw rq_counter

-> all 13 fields PASS (src == dst, byte-equal).
```

Conclusion: the FW QPC round-trips losslessly across
`SAVE_VHCA_STATE` / `LOAD_VHCA_STATE`. No driver-side
`QUERY_QP_PENDING_WRS` + replay path is needed for v0.

Caveat on what the experiment does and does not prove. With the QP
held in `INIT` the FW-side `sw_rq_counter` is 0 on both sides,
because FW does not snapshot the user-space DB-page producer index
into the QPC until the QP transitions through RTR (where FW first
registers the DB MKEY). The experiment therefore shows:

* the FW-tracked **QPC** survives byte-equal, which is the part FW
  alone carries across `LOAD_VHCA_STATE`; and
* by independent argument (K6's user-page result), the **user
  RQ buffer** and the **user DB page** -- which carry the actual
  WQE entries and the SW producer index -- survive via the
  `vfmig_iova` `HOST_PAGE`-replay machinery, since both live in
  the QP's user-mode umem which `user_mr_dma` already preserves.

Together these are sufficient. We could not drive a live RTR/RTS
round-trip in the same experiment without GIDs + active port, which
on a tracked VF is intentionally blocked by the netdev TX-dropper.
Re-running the piggyback in RTR/RTS once a tracked VF has a
functioning (or shim-functioning) netdev would tighten the proof; for
v0 the structural argument is sufficient and §6.3 is **closed**.

Open question (separate from WR replay): exact relationship between
the per-uobj `RESTORE_QP` handler's `modify_qp` chain and the fini
pass. Two options:

* (a) Per-uobj `RESTORE_QP` lands the QP in `IB_QPS_RTS` immediately.
  Fini pass handles only CQ re-arm.
* (b) Per-uobj `RESTORE_QP` lands the QP in `IB_QPS_RTR` only. Fini pass
  does the final `modify_qp(RTS)` once both ends are ready.

(b) is more conservative for cross-host where peer state may not yet
be in place at per-uobj restore time. Doc commits to (b) as the default;
revisit if a concrete problem surfaces.

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

EXCLUSIVE plugins (mlx5_vfmig) take ownership of port-level state when
the flag is supplied -- e.g. install GIDs at the requested indices via
the netlink RDMA_NLDEV_CMD_SYS_SET command path. SHAREABLE plugins (rxe)
trust the orchestrator and only validate.

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
…) and driver-namespace object ids (`MLX5_IB_OBJECT_UAR`,
`MLX5_IB_OBJECT_DEVX_*`, …) uniformly. That's exactly the surface CRIU
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
`drivers/infiniband/core/uverbs_std_types_restore.c` (new). Per-method
shape:

```c
DECLARE_UVERBS_NAMED_METHOD(
    UVERBS_METHOD_RESTORE_PD,
    UVERBS_ATTR_IDR(UVERBS_ATTR_RESTORE_PD_HANDLE,
                    UVERBS_OBJECT_PD,
                    UVERBS_ACCESS_NEW,
                    UA_MANDATORY,
                    UA_TARGET_HANDLE),       /* new attr semantic:
                                              * NEW + caller-specified id */
    UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_PD_ATTRS,
                       UVERBS_ATTR_TYPE(struct ib_uverbs_restore_pd_attrs),
                       UA_MANDATORY),
    UVERBS_ATTR_PTR_IN(UVERBS_ATTR_RESTORE_PD_BLOB,
                       UVERBS_ATTR_MIN_SIZE(0),
                       UA_OPTIONAL),
    UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_RESTORE_PD_USER_HANDLE,
                        UVERBS_ATTR_TYPE(__u32),
                        UA_MANDATORY));
```

Handler:

```c
static int UVERBS_HANDLER(UVERBS_METHOD_RESTORE_PD)(
    struct uverbs_attr_bundle *attrs)
{
    struct ib_uverbs_file *ufile = attrs->ufile;
    struct ib_device *dev;
    u32 target_handle;
    int err;

    /* Gating: parent ucontext must be in restore mode. The generic
     * `IB_UCONTEXT_RESTORE_MODE` flag is hw-agnostic; each driver sets
     * it from its own restore-mode entry path:
     *   - mlx5_vfmig: set when ucontext opened with
     *     MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE.
     *   - rxe: set when opened with the rxe restore-mode flag (small
     *     additive change to rxe's GET_CONTEXT path; rxe doesn't have
     *     a pre-existing restore concept).
     * Generic verb tests only the hw-agnostic flag, so adding new
     * providers later doesn't grow the gating check. */
    if (!(ufile->ucontext->flags & IB_UCONTEXT_RESTORE_MODE))
        return -EPERM;

    dev = ufile->device->ib_dev;
    if (!dev->ops.restore_pd)
        return -EOPNOTSUPP;

    target_handle = uobj_get_target_handle(attrs);
    /* unpack attrs blob, parent xrefs */

    err = dev->ops.restore_pd(ufile, target_handle, &init_attr,
                              blob, blob_len);
    if (err)
        return err;

    /* Emit new ib_uobject; install at target_handle. */
    return uverbs_install_uobj_at(attrs, target_handle);
}
```

**K2.5: caller-specified handle plumbing.** The "install at exactly the
caller's user_handle, fail with `-EBUSY` if taken" semantic is **not**
new core work; the helper already exists in
`drivers/infiniband/core/rdma_core.c`:

```c
struct ib_uobject *rdma_alloc_begin_uobject_at_handle(
    struct uverbs_attr_bundle *attrs,
    const struct uverbs_api_object *obj,
    u32 target_handle);
```

It was added by the UAR restore work (see
`drivers/infiniband/hw/mlx5/vfmig_uctx.c:556` for the existing user) and
implements the XA-insert-at-handle behaviour with the right errno
contract. Each `RESTORE_<TYPE>` handler reuses this helper. The
`UA_TARGET_HANDLE` attr semantic is just the uapi shape that drives it
-- the attr passes the caller-specified handle through to the helper.

Repeat per-class with the appropriate attr blob shape. Total: 8 new
methods (PD, CQ, COMP_CHANNEL, SRQ, MR, AH, QP, ASYNC_EVENT).

### 7.4 K4: ib_device_ops.restore_<type> callbacks

```diff
 struct ib_device_ops {
     ...
+    int (*restore_pd)(struct ib_uverbs_file *ufile, u32 target_handle,
+                      const struct ib_pd_init_attr *attr,
+                      const void *blob, size_t blob_len);
+    int (*restore_cq)(struct ib_uverbs_file *ufile, u32 target_handle,
+                      const struct ib_cq_init_attr *attr,
+                      const void *blob, size_t blob_len,
+                      u32 comp_channel_handle);
+    int (*restore_qp)(struct ib_uverbs_file *ufile, u32 target_handle,
+                      const struct ib_qp_init_attr *init_attr,
+                      const struct ib_qp_attr *attr, int attr_mask,
+                      const void *blob, size_t blob_len,
+                      u32 pd_handle, u32 send_cq, u32 recv_cq,
+                      u32 srq_handle);
+    int (*restore_mr)(...);
+    int (*restore_srq)(...);
+    int (*restore_ah)(...);
+    int (*restore_comp_channel)(...);
+    int (*restore_async_event_file)(...);
};
```

Driver-side population:

* **rxe**: populated statically at module load via `rxe_set_device_ops`.
  Each handler is the standard alloc+modify path with identity-hint
  extension on the id allocators. Roughly `rxe_alloc_pd_with_id_hint(...)`
  etc.
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

### 7.5 K8: per-uobject ufile_handle exposure (v0 required)

**Why required.** K3's `RESTORE_<TYPE>` methods install at a
caller-specified `target_handle` (= the per-ufile `obj->id` user code
holds in restored memory). To produce that value at dump, CRIU has to
read `ufile_handle` per uobject from the kernel and pair it with the
`restrack_id`-keyed view it already has from NLDEV (which encodes
parent-edges -- e.g. QP's `parent_pdn`). Today the two views can't be
cross-joined:

* **NLDEV** emits `restrack_id` (`PDN`/`CQN`/`MRN`/`SRQN`) per
  resource via `fill_res_<type>_entry`, but not `obj->id`.
* **`UVERBS_METHOD_INFO_HANDLES`** (K2) returns a flat `u32[]` of
  `obj->id` values per type per ufile, but no `restrack_id`
  alongside.

So a QP entry can record `parent_pdn=5` (NLDEV) without any way to
translate "PDN 5 lives at ufile_handle 2" -- which is what
`target_handle` needs.

**Proposed shapes** (kernel agent picks):

#### 7.5.1 K8a -- NLDEV emit `RES_HANDLE` (preferred)

```diff
 static int fill_res_pd_entry(struct sk_buff *msg, struct rdma_restrack_entry *res) {
     ...
     if (!rdma_is_kernel_res(res) &&
         nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_PDN, pd->res.id))
         return -EMSGSIZE;
+    if (!rdma_is_kernel_res(res) &&
+        nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_HANDLE, pd->uobject->id))
+        return -EMSGSIZE;
     ...
 }
```

Same shape repeated in `fill_res_cq_entry`, `fill_res_qp_entry`,
`fill_res_mr_entry`, `fill_res_srq_entry`. Five fns, one `nla_put_u32`
each. Mirrors the existing per-class id emission. New attr
`RDMA_NLDEV_ATTR_RES_HANDLE` (one line in `rdma_netlink.h`).

* Pros: symmetric with existing NLDEV layout, reuses CRIU's existing
  per-type NLDEV walk, no second ioctl per ufile per type.
* Cons: only covers types NLDEV knows about (PD/CQ/QP/MR/SRQ). AH,
  COMP_CHANNEL, ASYNC_EVENT still need INFO_HANDLES anyway, but for
  those we don't need the pairing -- they have no parent edges that
  reference them via restrack.

#### 7.5.2 K8b -- extend `INFO_HANDLES` to pair restrack ids

```c
DECLARE_UVERBS_NAMED_METHOD(
    UVERBS_METHOD_INFO_HANDLES,
    UVERBS_ATTR_CONST_IN(UVERBS_ATTR_INFO_OBJECT_ID, ...),
    UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_INFO_TOTAL_HANDLES, ...),
    UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_INFO_HANDLES_LIST, ...),
+   UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_INFO_RESTRACK_LIST,
+                       UVERBS_ATTR_MIN_SIZE(sizeof(u32)),
+                       UA_OPTIONAL));
```

Handler walks `ufile->uobjects` once, emits both `obj->id` and
`obj->object`'s underlying `restrack_entry.id` (zero when the type has
no restrack). One file touched, one fn.

* Pros: more general -- extends naturally to any future restracked
  type without per-type fill changes.
* Cons: slightly larger surface (new attr to `UVERBS_OBJECT_DEVICE`),
  requires CRIU to issue `INFO_HANDLES` per (ufile, type) which is the
  shape it'd use for AH/COMP_CHANNEL/ASYNC_EVENT anyway -- but adds
  per-ufile-per-type calls for PD/CQ/QP/MR/SRQ on top of the existing
  per-device NLDEV walk. Net more ioctls, less attractive at scale.

#### 7.5.3 Recommendation

**K8a.** Smaller patch, matches the existing NLDEV per-resource layout,
no churn to the device ioctl surface. CRIU consumes `RES_HANDLE`
inline during the existing per-type NLDEV walks (`rdma_nl_for_each_resource`
in `criu/rdma_netlink.c`); zero new ioctl plumbing on the CRIU side.

**Without K8** the design has no path to preserve user-visible handle
identity across restore. The fallback ("don't preserve handles, mutate
user memory") was rejected in §10.5 as it violates the libibverbs ABI
boundary. Surface this clearly: K8 is a hard prerequisite for the
first K3 method to work end-to-end.

### 7.6 One-plugin-per-port invariant

`ib_device_ops` is a per-device singleton. The current
`CR_PLUGIN_DECLARE_RDMA_PROVIDED_DRIVER` enum (RCD_RXE, RCD_MLX5_SRIOV_VFMIG)
maps 1:1 to per-device-ops sets. A future world with multiple plugin-types
per ibdev would need a per-context (not per-device) ops dispatcher;
explicitly out of scope. Doc records the invariant so future work doesn't
silently break it.

## 8. Cross-host caveats

### 8.1 GID / PKey port-level state

mlx5_vfmig is EXCLUSIVE on its port; the destination VF's port GID/PKey
table is whatever the FW + netdev configure at bind time. R3:

* **Symmetric setup** (test rigs, normal LM): destination netdev has the
  same IPs as source -> RoCE GIDs at the same indices. No-op match.
* **Asymmetric setup**: orchestrator hint file (§6.5) declares the
  destination's GID-index -> GID-value mapping. mlx5_vfmig plugin reads
  the hint at init(RESTORE) and installs GIDs via netlink before any
  uobject restore touches the port. Mismatch with the dump image's
  saved GIDs => fail loud with clear diagnostic.

rxe is SHAREABLE; orchestrator owns rxe link setup (`rdma link add rxe7
type rxe netdev <X>`); CRIU just validates the resulting GID table
matches the dump.

### 8.2 FW identity continuity (K6)

For each of PD, CQ, QP, SRQ, MR (and any other VHCA-scoped FW id
class), the unknown is whether `LOAD_VHCA_STATE` preserves FW id
reservations on the destination VHCA the way it provably does for
UARs.

#### 8.2.1 Acceptance criteria

For UARs the criterion was simply "indexes increase across restore"
because user-allocated UARs were directly enumerable on the restored
VHCA via the dynamic UAR query verb (`uar_restore.md` §3).
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
and is integrated as the validation step of S3 (PD restore) per §9.1.

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
  K3/K4 mlx5 handler shape. Piggyback the §6.3 `QUERY_QP` RQ-head/tail
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
* **S3: PD restore (rxe + mlx5_vfmig together).** Implement K3
  `RESTORE_PD` method + both drivers' `restore_pd` callbacks. mlx5
  shape determined by S0. Both `run_uverbs_cr.sh` and
  `run_vfmig_cr.sh` flip to PASS for the PD round-trip.
* **S4: MR restore (rxe + mlx5_vfmig together).** Implement
  `RESTORE_MR` + both drivers. Couples with `user_mr_dma.md`
  stage 3 (rkey continuity at the IOMMU layer); the kernel verb and
  the user_mr_dma IOMMU-layer binding land together as a coherent
  per-MR restore. With PD + MR working, the `rdma_test_agent` send
  buffer is restorable end-to-end.
* **S5: CQ restore + comp channel (rxe + mlx5_vfmig together).** With
  PD + MR + CQ working, the send/recv completion path is back.
* **S6: QP restore (rxe + mlx5_vfmig together).** State-machine replay
  to RTR per §6.3 option (b). Fini-pass transitions to RTS. **First
  passing `rdma_test_agent` round-trip on a restored ucontext --
  R3 v0 minimum bar.**
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

Mirror `user_mr_dma.md` §5.2's rkey-logging idiom. CRIU emits
prefixed log lines per uobject restored:

```
rdma_r3: pid=<P> ufile=<U> type=PD source_handle=N -> dest_handle=M
         source_pdn=X dest_pdn=Y identity=ok
rdma_r3: pid=<P> ufile=<U> type=QP source_handle=N -> dest_handle=M
         source_qpn=0xN dest_qpn=0xM identity={preserved|fresh}
```

Easy `grep rdma_r3` to confirm what survived round-trip; no hard
assertions in CRIU itself, since FW behaviour is the empirical variable.

## 10. Open questions

1. **K6 outcome**: does `LOAD_VHCA_STATE` preserve PD/CQ/QP/SRQ/MKEY id
   reservations? **Answered (2026-05-13)**: PARTIAL PASS -- preserved
   for PD/CQ/QP/MKEY (SRQ skipped pending the known restored-VF SRQ
   gate, deferred to S7). K3/K4 mlx5 handlers can use the
   alloc-with-id-hint pattern; no FW patch required for v0.
   See `test_fw_id_continuity.sh`.
2. **Pending RQ/SQ WR preservation across `LOAD_VHCA_STATE`**:
   **Answered (2026-05-13)**: structural PASS -- the FW QPC
   round-trips byte-equal across LOAD_VHCA_STATE for all 13 fields we
   sampled (state, pd, q_key, cqn_snd/rcv, srqn_rmpn_xrqn, all PSN
   fields, both sq/rq counter pairs). User-mode RQ WQE buffer + DB
   page are independently preserved via `user_mr_dma` HOST_PAGE
   replay. No driver-side `QUERY_QP_PENDING_WRS` ioctl needed.
   Caveat: live RTR/RTS confirmation is gated on the netdev TX-dropper
   on tracked VFs and was not run; structural argument is sufficient
   for v0. See `test_fw_id_continuity.sh K6_POST_RECV_WRS=N` and §6.3.
3. **modify_qp split**: per-uobj restore lands QP in RTR or RTS? §6.3
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
   §10.6 (partial-restore atomicity) -- both share the "what userspace
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

## 11. Sequencing relative to other work

* **Builds on** `uar_restore.md`: the ucontext + UAR restore is
  the substrate R3 lives on top of. R3 cannot start until ucontext
  restore works (it does, end-to-end as of `87e9813c6` in CRIU and
  matching kernel commits).
* **Parallel to** `user_mr_dma.md`: the user-MR DMA work
  preserves IOMMU-level page binding identity; R3 preserves uobject-level
  user-handle and rkey identity. The two converge at MR restore (S7) and
  must land together to deliver end-to-end MR continuity. Stages 1-6 of
  R3 are independent of `user_mr_dma` and can land first.
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
