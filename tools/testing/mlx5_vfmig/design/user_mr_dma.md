# DESIGN: user-MR DMA coverage in the v1 deterministic IOVA allocator

> **Status (2026-05-19):**
>
> * **Stage 1 — landed.** `vfmig_dma_ops` shim, `VFMIG_SLOT_USER_PAGE`,
>   per-VF `dev->dma_iommu` override, registry external-page entries
>   with the `awaiting_bind` field wired-but-unused. Source-side data
>   path works on tracked VFs.
> * **Stage 2 — landed.** Identity infrastructure (kind/fw_id keys,
>   secondary rb_tree, observer ioctl, wire-format extension,
>   LOAD-side replay-as-placeholder, source-side retag at every user
>   uobject creation site). Empirical chain in §6.5 — eleven commits
>   `504602987fb5` ⇒ `3513b4e28a0e`, validated end-to-end against the
>   running kernel via
>   `tools/testing/mlx5_vfmig/save_load/user_object_replay/test_user_object_replay.sh`
>   FULL PASS verdict (`MR=4 CQ=1 QP=1 SRQ=1 DBR=1 total=8`).
> * **Stage 3 — in this design revision; not yet implemented.**
>   Reshaped twice. First reshape (2026-05-18, preserved in §A.G)
>   replaced the original cursor-based sketch with a per-task hint
>   trigger after `uobject_restore.md` landed `UVERBS_OBJECT_RESTORE`
>   / `mlx5_ib_restore_mr` (S4b). Second reshape (this revision,
>   2026-05-19) dropped the per-task hint in favour of an
>   `ib_umem_pin` IB-core primitive + a driver-side
>   `mlx5_ib_umem_restore` wrapper. No `task_struct` extension, no
>   side-channel between the verb handler and `vfmig_dma_ops.map_sg`;
>   the bind is an explicit, type-safe call from
>   `mlx5_ib_restore_X` to the vfmig binding primitive. See §7 for
>   the current design; §A.C for the implementation sketch; §A.G for
>   both superseded approaches.
> * **Stage 4 — forward-compat sketch only (§8).** IOVA recycling for
>   `VFMIG_SLOT_USER_PAGE`; not on the critical path for end-to-end
>   data-path continuity.

This doc covers the design we agreed on before any code lands. Lives
alongside `uar_restore.md` and `uobject_restore.md`; the verb-level
identity model documented in `uobject_restore.md` is the trigger that
stages 2 and 3 hang off.

## 1. Goal and scope

Close the `v1_allocator_user_mr_dma_gap`: today the v1 deterministic IOVA
allocator hooks a curated set of **kernel-side** mlx5_core allocation
sites (cmd ring, MANAGE_PAGES, EQ buffers, frag bufs, DB pages,
`DMA_COHERENT` slot). User-side allocations -- the umem buffers backing
user MRs, user CQs, user QP/SRQ work-queue buffers, user doorbell records
-- all funnel through `ib_umem_get -> dma_map_sgtable`, which is **not**
covered. On a tracked VF those calls go through the kernel's default
DMA-IOMMU path against our unmanaged domain and produce IOVAs that are
never installed in our domain's page tables. FW dereferences them during
the UMR PAS update, returns `IB_WC_MW_BIND_ERR` (vendor syndrome 0x25,
"memory bind error"), and surfaces in user space as `ibv_reg_mr` failing
with "Couldn't register MR".

Stage 1 closed this on the source-side (and on a fresh-registration
destination). Stages 2 and 3 close it across SAVE/LOAD for restored
uobjects, ending the data-path gap that `uobject_restore.md` §10.9
records. They land together as a unit; landing stage 2 alone produces
dead state on the destination because nothing consumes the
pre-installed `awaiting_bind` entries.

### Stage 1 — landed

* `drivers/net/ethernet/mellanox/mlx5/core/vfmig/vfmig_dma_ops.c` exposing
  `vfmig_dma_ops_attach(vf_pdev, dom)` /
  `vfmig_dma_ops_detach(vf_pdev, dom)`. Internally:
  * `.map_sg` / `.unmap_sg` / `.map_phys` / `.unmap_phys` route through
    `vfmig_iova_install_external_phys_locked()` /
    `vfmig_iova_destroy_page_locked()` helpers that install/remove
    externally-owned phys at deterministic IOVAs (no `alloc_pages`).
  * `.alloc` / `.free` deliberately **left unimplemented**. Unconverted
    in-tree `dma_alloc_coherent` callers continue to fail loudly.
  * `.map_phys` rejects `DMA_ATTR_MMIO` for v1 with a ratelimited warn.
  * `.sync_*` no-ops on x86 cache-coherent.
* Per-VF override of `dev->dma_iommu` to false during the lifetime of
  our unmanaged domain (see §3.3).
* `VFMIG_SLOT_USER_PAGE` (asymmetric expand-to-fill layout, §4.1.1).
* `struct vfmig_iova_page` flags `external` (in use by stage 1) and
  `awaiting_bind` (wired in stage 1, populated by stage 2).
* Hook attach into `vfmig_iova_domain_create()` after
  `iommu_attach_device`; detach inverse before `iommu_detach_device`.
* Validated by `save_load/test_iova_tracked_save_load.sh` (`PINGPONG=1`)
  and by the rkey diagnostic in §5.2.

### Stage 1 — explicitly out of scope (delivered by later stages)

* Wire format extension. (Stage 2 adds `HOST_USER_PAGE` records.)
* Destination-side replay of user-side mappings. (Stages 2 + 3.)
* User-uobject identity preservation across SAVE/LOAD. The destination's
  `ibv_reg_mr` on a CRIU-restored process pre-stage-3 produces a fresh
  MKEY because no verb adopts the LOAD-replayed FW one; rkey continuity
  on a *restored* MR is delivered by `mlx5_ib_restore_mr` (kernel verb,
  landed in S4b — see `uobject_restore.md` §9.1) which is gated on
  stages 2+3 making the data path actually work end-to-end. The §5.2
  rkey diagnostic is the in-place readout for the fresh-`ibv_reg_mr`
  case (which never gains rkey continuity at any stage of this work).
* DM (on-chip device memory). Bypasses host DMA entirely.
* DMA-buf user MRs (`reg_user_mr_dmabuf`). Compatible with the design
  in principle (§9) but deferred — sg-granularity stability across
  SAVE/LOAD on the destination's dma-buf attachment is a known unknown.
* ODP (on-demand paging). Calls `hmm_dma_map_pfn` not
  `dma_map_sgtable`; needs a separate hook.

### Stage 2 + Stage 3 (this design)

These two PRs jointly close the end-to-end data path on a
CRIU-restored process. Stage 2 is identity infrastructure (wire
format + LOAD-side placeholder installation + source-side retag).
Stage 3 is the kernel-verb-driven binder that consumes stage 2's
placeholders. They must land together because stage 2's
`awaiting_bind` entries are write-only state until stage 3's
binder reads them.

**What stage 2 delivers (§6).** A new `HOST_USER_PAGE` wire record
carries `(kind, fw_id, iova, length)` per source-side user uobject.
SAVE iterates external registry entries and emits one record each.
LOAD pre-installs `awaiting_bind=true` registry entries indexed by
`(kind, fw_id)`. **No `iommu_map` at LOAD** — the destination's phys
pages don't exist yet (CRIU restores the user process *after* LOAD).
Source-side retag at every user-uobject creation site rewrites the
auto-numbered `instance_key` with `VFMIG_HUOBJ_KEY(kind, fw_id)`.

**What stage 3 delivers (§7).** Each `RESTORE_X` verb constructs
its umem via a new `mlx5_ib_umem_restore(dev, kind, fw_id, addr,
size, access)` driver helper instead of `ib_umem_get`. The helper
composes two pieces: (i) a new IB-core primitive `ib_umem_pin` that
pins user pages and builds the sgtable without calling
`dma_map_sgtable`; (ii) a new vfmig primitive
`vfmig_iova_bind_user_object(dom, kind, fw_id, sgt)` that looks up
the matching `awaiting_bind` placeholder by `(kind, fw_id)`,
`iommu_map`s the freshly-pinned phys pages PAGE_SIZE-at-a-time at
the source IOVAs, populates `sg_dma_address(sg)`, clears
`awaiting_bind`. `vfmig_dma_ops.map_sg` is untouched — the bind
path never goes through the DMA shim. Dereg flows symmetrically
through the existing `vfmig_dma_ops.unmap_sg` path — no special
cases.

**Generality.** The same mechanism covers MR, CQ, QP, SRQ, and
shared DBR (doorbell record) pages. DBRs are sub-keyed under their
parent uobject; `mlx5_ib_db_map_user`'s existing dedup covers the
multi-uobject-per-DBR case naturally (§A.E).

**Validation.** Stage 2: `MLX5_VFMIG_IOC_QUERY_AWAITING_BIND` ioctl
+ sidecar `test_user_object_replay.sh` asserting count match
(source-emitted records == destination's `awaiting_bind` entries
post-LOAD). Stage 3: Phase J data-path subtest in
`test_mr_restore_mlx5_vfmig.sh` (1-WR RDMA write through the
restored MR; `IBV_WC_SUCCESS` is the load-bearing pass criterion).
The same Phase J extends to S5/S6/S7 harnesses as
`RESTORE_{CQ,QP,SRQ}` land.

### Stage 4 — future, post-MVP

Per-slot recycling allocator for `VFMIG_SLOT_USER_PAGE`. Stage 1's
bump cursor exhausts under registration churn. Free bitmap is the
likely first cut; buddy allocator and interval-tree-of-free are
alternatives. Doc-only; defer until empirical exhaustion is observed
on real workloads. See §8.

## 2. Background: today's failure mode

A bound, tracked VF currently has:

* Default DMA domain replaced by our unmanaged paging domain (via
  `iommu_attach_device(dom->iommu_dom, &vf_pdev->dev)` in
  `vfmig_iova_domain_create`).
* `dev->dma_iommu == true`, set at IOMMU device-probe time by
  `iommu_setup_dma_ops` and **not cleared** by `iommu_attach_device`.
  (`iommu_setup_dma_ops` is the only code path that writes to
  `dev->dma_iommu`; `iommu_attach_device` doesn't touch it.)

When userspace calls `ibv_reg_mr`, the path is:

```
ibv_reg_mr (libibverbs)
  -> uverbs IB_USER_VERBS_CMD_REG_MR
    -> mlx5_ib_reg_user_mr           (drivers/infiniband/hw/mlx5/mr.c:1578)
      -> ib_umem_get                 (drivers/infiniband/core/umem.c:164)
        -> pin_user_pages_fast       (pages held by current->mm)
        -> sg_alloc_append_table_from_pages
        -> ib_dma_map_sgtable_attrs  (include/rdma/ib_verbs.h:4281)
          -> dma_map_sgtable         (kernel/dma/mapping.c:318)
            -> __dma_map_sg_attrs    (kernel/dma/mapping.c:230)
              -> use_dma_iommu(dev)? YES (dev->dma_iommu == true)
                -> iommu_dma_map_sg  <-- WRONG DOMAIN
```

`iommu_dma_map_sg` allocates IOVAs from a per-device dma-iommu cookie
that was set up against the **default DMA domain**, not against our
unmanaged domain. The `iommu_map` call inside `iommu_dma_map_sg` may or
may not succeed depending on subtle interactions, but in either case the
hardware (which sees our currently-attached unmanaged domain) does not
have a valid translation for the resulting IOVAs.

mlx5_ib then issues an UMR PAS update against the FW with those
addresses, FW dereferences the IOVAs through the IOMMU, and the page
walk faults. The MR registration completes from the verbs API's
perspective but subsequent posts return `IB_WC_MW_BIND_ERR` (vendor
syndrome 0x25). libibverbs converts this to `ibv_reg_mr` failure with
the generic "Couldn't register MR" string.

## 3. Hook choice

### 3.1 Three options considered

| | (a) `ib_core` / `ib_umem_get` | (b) `mlx5_ib` post-`ib_umem_get` | (c) `vfmig_dma_ops` per VF |
|---|---|---|---|
| Coverage | every umem in every driver | only the call sites we wrap | every DMA op on the VF (umem from MR, CQ, QP, SRQ, doorbell, devx, plus future sites) |
| `ib_core` changes | yes (intrusive) | yes (need a "skip dma_map" entry point) | none |
| `mlx5_ib` changes | none | yes (one wrapper per call site, replicates pin + sg) | none |
| `mlx5_core/vfmig_iova` changes | small new API: install external phys at deterministic IOVA | same | + new `vfmig_dma_ops.c` (set/clear `dev->dma_ops` and `dev->dma_iommu`) |
| Catches missed call sites? | yes | only what we wrap | yes |
| `dma_alloc_coherent` failure-loud preserved? | yes | yes | yes (`.alloc` left unimplemented) |

(c) is the only option that doesn't sprawl into ib_core or every umem
call site, and it gets uniform coverage of every umem call site, every
ODP call site (after stage 1+ extends `.map_phys`), every devx call site,
and any future-driver-internal dma site we missed.

The historical concern about (c) -- "hardest to reason about" -- comes
down to interactions between our installed dma_ops and the dma-iommu
fast path. Section 3.3 dissects exactly how those interact and shows
the precedence order is, in fact, well-defined.

### 3.2 Why (c) is transparent to `ib_umem_get`

`ib_umem_get` doesn't know about (a)/(b)/(c). It calls
`ib_dma_map_sgtable_attrs(device, sgt, DMA_BIDIRECTIONAL, attrs)`,
which forwards to `dma_map_sgtable(dev->dma_device, sgt, ...)`. That
device is the underlying mdev->device (the VF's PCI device). The DMA
core's first action is `get_dma_ops(dev)`:

```c
static inline const struct dma_map_ops *get_dma_ops(struct device *dev)
{
	if (dev->dma_ops)
		return dev->dma_ops;
	return get_arch_dma_ops();
}
```

If we `set_dma_ops(&vf_pdev->dev, &vfmig_dma_ops)` while our unmanaged
domain is attached, every umem caller -- MR, CQ, QP buffer, SRQ,
doorbell record, devx -- short-circuits the dma-iommu path and lands
in our `.map_sg`, which uses `iommu_map` against our unmanaged domain
at deterministic IOVAs. The umem caller still sees populated
`sg_dma_address` / `sg_dma_len` and proceeds normally.

### 3.3 Critical detail: `dev->dma_iommu` precedence

`__dma_map_sg_attrs` checks dispatch order:

```c
if (dma_map_direct(dev, ops) || arch_dma_map_sg_direct(dev, sg, nents))
	ents = dma_direct_map_sg(...);
else if (use_dma_iommu(dev))         /* (*) */
	ents = iommu_dma_map_sg(...);
else
	ents = ops->map_sg(...);          /* our hook */
```

`(*)` short-circuits to `iommu_dma_map_sg` when `dev->dma_iommu == true`,
**before** falling through to our installed `ops->map_sg`. To make our
ops actually take effect, we must also override `dev->dma_iommu` to
`false` while our unmanaged domain is attached. Concretely, on attach:

```
dom->saved_dma_iommu = dev->dma_iommu;
dev->dma_iommu = false;
set_dma_ops(dev, &vfmig_dma_ops);
```

and on detach, the inverse. Both happen under the device lock the
SET_TRACKED ioctl already holds, atomic with respect to any concurrent
DMA from the device (the device is unbound at attach/detach time per
the `vfmig_iova_domain_create` contract -- a tracked VF is only
attached when unbound).

The same precedence consideration applies to `dma_alloc_attrs`:

```c
if (dma_alloc_direct(dev, ops))
	... = dma_direct_alloc(...);
else if (use_dma_iommu(dev))
	... = iommu_dma_alloc(...);
else if (ops->alloc)
	... = ops->alloc(...);
else
	return NULL;
```

With `dma_iommu == false` and `ops->alloc == NULL`, `dma_alloc_attrs`
returns NULL. That's the failure mode we want to preserve for
unconverted `dma_alloc_coherent` callers (rather than have them
silently succeed against our slot allocator).

The same applies to `dma_map_phys` for which we provide `.map_phys`,
and to `dma_unmap_*` which we provide for symmetry.

## 4. Stage 1 design

### 4.1 New slot `VFMIG_SLOT_USER_PAGE`

Appended at the next free index in `enum vfmig_iova_slot` (slot 7,
preserving 0..6). Slot semantics:

* Each user-MR umem registration consumes one IOVA per scatter-gather
  entry (one per page in the simple case; coalesced contig runs use
  one IOVA per coalesced segment). Coalescing reduces the *number* of
  registry entries but not the *total IOVA range* consumed by a given
  MR.
* `instance_key` defaults to per-slot auto-numbering in stage 1
  (cannot be derived from anything stable inside `.map_sg`: that
  callsite has no caller identity, only `(dev, sgt, dir, attrs)`).
  Stage 2 introduces a post-FW-create retag step that overwrites
  the auto-numbered keys with `VFMIG_HUOBJ_KEY(kind, fw_id)`
  (8-bit kind, 56-bit fw_id; encoding in §A.A), i.e. tags the
  registry entries with the FW resource id (`mkey_index`, `cqn`,
  …) *after* the FW has assigned it. Wire records emitted in
  stage 2 carry these per-uobject identity keys (§A.B for the
  callsite map, §A.G.1 for the prior per-sg mkey sketch and why
  it was superseded).

#### 4.1.1 Per-VF user-MR IOVA budget

The current slot model partitions the per-VF IOVA window into 8
**equal** sub-windows of size `(PER_VF - transient) / 8`. That makes
`USER_PAGE`'s share scale with `PER_VF` only at 1/8 -- and grows the
already-wildly-overprovisioned kernel slots (CMD_RING peaks at 1 page,
FW_PAGE peaks at ~32 MiB) at the same rate. At Kconfig max
`PER_VF=256 GiB`, `USER_PAGE` is only ~32 GiB. Not enough for
single-VF-per-PF use cases that want the full IOMMU aperture.

Stage 1 changes the layout to **asymmetric**: kernel slots are
**fixed at 510 MiB each** regardless of `PER_VF`, and `USER_PAGE`
absorbs the entire remainder of the deterministic range:

```
slot_base(s) = base + s * 510 MiB           for s in 0..6  (kernel)
slot_end(s)  = base + (s+1) * 510 MiB       for s in 0..6
slot_base(USER_PAGE = 7) = base + 7 * 510 MiB        (FIXED)
slot_end(USER_PAGE)      = transient.base = base + PER_VF - 16 MiB
```

Per-VF user-MR budget = `transient.base - slot_base(USER_PAGE)` =
`PER_VF - 7 * 510 MiB - 16 MiB transient`:

| `CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB` | budget for `USER_PAGE` |
|---|---|
| 4 (Kconfig minimum, post-stage-1) | ~510 MiB |
| 16 | ~12.5 GiB |
| 128 (current default, unchanged) | ~124.5 GiB |
| 65536 (Kconfig maximum) | ~63.5 TiB |

This is the upper limit on **total** registered user-MR memory per VF,
across all processes that have a ucontext on that VF. A single MR
registration > budget returns `-ENOSPC` from our `.map_sg`. The
realistic ceiling is the live IOMMU aperture, not the Kconfig range:
on a 39-bit aperture (Intel VT-d agaw=2, ~508 GiB usable) a single
VF can use most of it; on 48-bit apertures (agaw=3 / AMD-Vi) the
SR-IOV hardware cap is binding well before the aperture.

Stage 1 leaves the default `CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB`
unchanged at 128 (already gives ~124.5 GiB user-MR budget). Stage 1
**raises** the Kconfig minimum from 1 to 4 because the new fixed
510-MiB-per-kernel-slot layout needs at least
`7 * 510 MiB + 16 MiB ~= 3.6 GiB` of address space before
`USER_PAGE` even exists; below that the static_asserts fire at
compile time.

**Wire compatibility properties** (improved over my earlier "going up
is fine" claim):

* Kernel-slot IOVAs are now `PER_VF`-**independent**. SAVE blobs from
  a `PER_VF=4` source replay cleanly on a `PER_VF=64` destination
  *for kernel slots*, because the IOVA bases are the same constants.
  This is wire-compat with everything we've shipped so far (where
  the only used PER_VF was 4 GiB).
* `USER_PAGE` records are wire-compat-with-source -- the destination
  must have `PER_VF` >= source's `PER_VF` to accept all the
  source's `USER_PAGE` IOVAs. Same `replay_page` cross-check as
  before; just operates on a different upper bound now.
* The new `slot_end(USER_PAGE) = transient.base` rule is a one-line
  special-case in `vfmig_iova_slot_end()`. No enum renumbering, no
  record format change, no kernel-slot offset shift.

### 4.2 Registry: external-page entries

`struct vfmig_iova_page` gains two flags wired in stage 1:

```c
struct vfmig_iova_page {
	...
	enum vfmig_iova_slot slot;
	u64		     instance_key;
	bool		     external;	   /* page is owned by caller (umem),
					    * not by our allocator */
	bool		     awaiting_bind;/* stage 2: pre-replayed entry
					    * waiting for a destination-side
					    * .map_sg to bind a freshly-pinned
					    * page at this IOVA. Always false
					    * in stage 1. */
};
```

The two flags are orthogonal:

* `external`: set on entries whose backing page is owned by the
  caller (the umem). Affects ownership during destroy.
* `awaiting_bind`: set on entries pre-installed by a stage-2
  LOAD-side replay; cleared once a destination-side `.map_sg` call
  binds a real page at the IOVA. Always false in stage 1 (no LOAD
  replay produces awaiting-bind entries).

Lifecycle:

* When `external == true && awaiting_bind == false` (stage 1
  steady state):
  * `vfmig_iova_install_external_phys_locked(dom, slot, key, iova, phys, len, prot)`
    creates the entry. No `alloc_pages` / no `__free_pages`. Caller
    provides the phys address. `page` field set to `NULL` (we don't
    need `page_address()` because we never memcpy these for SAVE).
  * `vfmig_iova_destroy_page_locked` skips `__free_pages`.
  * `vfmig_iova_for_each` skips emission in stage 1 (will start
    emitting identity-only records in stage 2).
* When `external == true && awaiting_bind == true` (stage 2,
  pre-bind):
  * Created by `vfmig_iova_replay_external_locked(dom, slot,
    instance_key, iova, len, awaiting=true)` from a `HOST_USER_PAGE`
    record. `instance_key` carries `VFMIG_HUOBJ_KEY(kind, fw_id)`
    (§A.A); the entry is also indexed in a secondary
    `(kind, fw_id)` table for stage 3 lookup.
  * Entry has no `iommu_map` installed yet (`page` is NULL, no phys
    to map). The IOVA reservation exists in the registry only as
    metadata for stage 3's binder to consume.
  * Destination consumption: stage 3's `RESTORE_X` verb constructs
    its umem via `mlx5_ib_umem_restore(dev, kind, fw_id, ...)`,
    which calls `ib_umem_pin` to pin user pages + build the sgtable
    (no `dma_map_sgtable`), then calls
    `vfmig_iova_bind_user_object(dom, kind, fw_id, sgt)`, which
    looks the entry up by `(kind, fw_id)` in the C1 rb_tree,
    iterates PAGE_SIZE-at-a-time over the freshly-pinned phys pages
    calling `iommu_map(dom, source_iova_offset, dst_phys, PAGE_SIZE,
    prot)`, populates `sg_dma_address` per sg, clears
    `awaiting_bind`. Full implementation sketch in §A.C.
* When `external == false` (kernel slots):
  * Existing `vfmig_iova_install_page_locked` continues to be the
    creator. Behaviour unchanged from before this work.

### 4.3 `vfmig_dma_ops`

```c
static const struct dma_map_ops vfmig_dma_ops = {
	.map_sg     = vfmig_dma_map_sg,
	.unmap_sg   = vfmig_dma_unmap_sg,
	.map_phys   = vfmig_dma_map_phys,
	.unmap_phys = vfmig_dma_unmap_phys,

	/* No coherent allocator: keeps unconverted dma_alloc_coherent
	 * callers failing-loud, same as today (with our unmanaged domain
	 * attached, dma_alloc_coherent fails). */

	/* x86 cache-coherent: explicit no-ops keep the trace_dma helpers
	 * happy without forcing a fallback. */
	.sync_single_for_cpu     = vfmig_dma_sync_noop_single,
	.sync_single_for_device  = vfmig_dma_sync_noop_single,
	.sync_sg_for_cpu         = vfmig_dma_sync_noop_sg,
	.sync_sg_for_device      = vfmig_dma_sync_noop_sg,

	.dma_supported       = vfmig_dma_supported,    /* always true */
	.get_required_mask   = vfmig_dma_get_required_mask, /* DMA_BIT_MASK(64) */
};
```

`.map_sg` walks the sgt, picks one IOVA per scatter-gather entry from
`VFMIG_SLOT_USER_PAGE` (auto-numbered in stage 1), calls
`vfmig_iova_install_external_phys_locked` for each, and writes the
resulting IOVA into `sg_dma_address(sg)` / `sg_dma_len(sg)`. Returns
the number of mapped entries.

`.unmap_sg` walks the sgt, looks up each `sg_dma_address(sg)` in the
registry, calls `vfmig_iova_destroy_page_locked` on each. The
registry entry is fully torn down (iommu_unmap called, entry freed)
-- this is the contract dma-buf re-binding requires (see §9).
**Stage 1 limitation**: per-slot bump cursors don't go backwards, so
the IOVA range itself isn't recycled. Bounded for typical workloads
(registrations are process-lifetime; user-MR IOVA budget at the
proposed default `PER_VF_GIB=16` is ~12.5 GiB per VF, ~3.2M 4 KiB
entries).

**Stage 4 deliverable** (post-MVP, §8): a per-slot free bitmap or
freelist for `VFMIG_SLOT_USER_PAGE` so unmap returns the IOVA range
to the slot's allocatable pool. Originally folded into stage 2 in
the pre-redesign sketch; split out because it's orthogonal to
data-path correctness and isn't on the critical path for end-to-end
SAVE/LOAD continuity. Stage 1 ships without recycling because the
validation workloads (pingpong, bounded test programs) don't trigger
exhaustion; deferred until empirical exhaustion is observed on a
real workload.

`.map_phys` / `.unmap_phys` analogous, single-page path.

`.dma_supported` returns true; `.get_required_mask` returns
`DMA_BIT_MASK(64)`. mlx5_core/mlx5_ib already set their dma_mask at
probe time.

### 4.4 Attach / detach

`vfmig_iova_domain_create()`:

```c
err = iommu_attach_device(dom->iommu_dom, &vf_pdev->dev);
...

/* dev->dma_ops takes precedence over dma-iommu only when
 * dev->dma_iommu is false. Override here, restore on detach. */
dom->saved_dma_iommu = vf_pdev->dev.dma_iommu;
vf_pdev->dev.dma_iommu = false;
set_dma_ops(&vf_pdev->dev, &vfmig_dma_ops);
```

`vfmig_iova_domain_destroy()` does the inverse before
`iommu_detach_device`. Both happen under the SET_TRACKED ioctl's
device lock and require the VF to be unbound.

### 4.5 Implicit coverage: QP / CQ / SRQ / WQ / doorbell records

User MRs are the headline use case but stage 1's `.map_sg` covers
every other user-side mlx5_ib uobject buffer too, by construction.
Listing them out so we don't have to re-derive coverage later:

**User QPs** (`mlx5_ib_create_qp` -> `create_user_qp` -> `_create_user_qp`):

* bfreg allocation: bookkeeping only; picks an *index* into the
  ucontext's pre-allocated UAR table. No DMA path.
* QP buffer (SQ + RQ + control region): `ib_umem_get(buf_addr,
  buf_size, ...)`. Hits our `.map_sg`. Covered.
* Doorbell record: `mlx5_ib_db_map_user` -> `ib_umem_get` on the
  user's doorbell page. Hits our `.map_sg`. Covered.
* UAR for doorbells / blueflame regs: pre-existing `mmap()` of BAR
  space, no DMA path.
* `CREATE_QP` FW command itself: travels on the cmd ring (already
  covered by `VFMIG_SLOT_CMD_RING`).

**Kernel QPs** (`mlx5_ib`-internal, e.g. GSI QP1):

* QP buffer: `mlx5_frag_buf_alloc_node` -> `VFMIG_SLOT_FRAG_BUF`
  (existing kernel-slot allocator). Covered.
* Doorbell record: `mlx5_db_alloc_node` -> `VFMIG_SLOT_DB_PAGE`
  (existing kernel-slot allocator). Covered.

**User CQs / SRQs / WQs**: all use the same shape -- `ib_umem_get`
for user-pinned backing memory, `ib_db_map_user` for the doorbell
record. Hit `.map_sg`. Covered.

**Kernel CQs / SRQs**: `mlx5_frag_buf_alloc_node` and
`mlx5_db_alloc_node`. Existing slot allocators. Covered.

**Result: `.alloc()` is never dispatched on the user-uobject path.**
Every user-side allocation is either pinned-and-mapped via
`ib_umem_get` (covered by `.map_sg`) or carved out of pre-mapped UAR
space. The decision to leave `.alloc` unimplemented therefore costs
us no coverage; it only catches *unconverted* in-tree
`dma_alloc_coherent` callers, which is precisely the fail-loud
diagnostic property we want to keep.

## 5. Stage 1 test plan

### 5.1 Positive: pingpong PASS on tracked VF

Already wired in `save_load/test_iova_tracked_save_load.sh` via `PINGPONG=1`. Pre-stage 1:

* Phase A pingpong on source: FAIL ("Couldn't register MR")
* Phase D pingpong on destination: FAIL ("Couldn't register MR")

Post-stage 1:

* Phase A pingpong on source: **PASS** (fresh registration via
  `vfmig_dma_ops.map_sg` -> `iommu_map` against unmanaged domain).
* Phase D pingpong on destination: **PASS** (same path, fresh
  domain, fresh `VFMIG_SLOT_USER_PAGE` cursor; LOAD-replayed FW
  MKEY entries are orphan and harmless because pingpong tore down
  its MRs before the source was checkpointed, so the FW MKEY table
  is empty at SAVE time).

### 5.2 rkey logging (diagnostic, not assertion)

To track where in the staging we are without writing brittle
assertions, the test logs the rkey at two points and prints both with
a stable prefix:

1. On source: register MR via `ibv_reg_mr`, log
   `vfmig_user_mr: source_rkey=0x%x mr_addr=0x%llx mr_len=0x%llx`.
   SAVE while MR is still registered (so the FW MKEY ends up in the
   blob).
2. On destination: LOAD; re-register the same buffer/HVA on a
   CRIU-restored process; log
   `vfmig_user_mr: dest_rkey=0x%x ...`.

Stage 1 documented expectation: `dest_rkey != source_rkey` -- a
fresh MR was allocated; the LOAD-replayed FW MKEY is orphan and its
IOVAs are not in the destination's IOMMU.

Stage 2 documented expectation: `dest_rkey != source_rkey` *(still
not equal)*. Stage 2 ships only identity infrastructure --
`HOST_USER_PAGE` records on the wire and pre-installed
`awaiting_bind` entries on the destination -- but no consumer of
those entries without stage 3's binder. Stage 2's observable
signal is **count match** between source emissions and destination
installations (the `MLX5_VFMIG_IOC_QUERY_AWAITING_BIND` ioctl, see
§6.4), not rkey continuity.

Stage 3 documented expectation under fresh `ibv_reg_mr` (this test):
`dest_rkey != source_rkey` *(still not equal -- stage 3 doesn't
change this test)*. Fresh user-driven `ibv_reg_mr` always allocates
a new MKEY; rkey continuity in v0 is delivered exclusively by the
`mlx5_ib_restore_mr` kernel verb, validated separately in
`mr_restore_probe_mlx5_vfmig` (`uobject_restore.md` §9.6). What
stage 3 *does* deliver, observable here: the awaiting-bind counter
of §5.4 increments after a SAVE/LOAD/`RESTORE_X` cycle.

No hard assertion in any stage; the diagnostic value is in the
printout. Easy to inspect by `grep vfmig_user_mr` in the test
output, easy to compare across stages, no flake risk from a flipped
inequality. (And the cross-host vs same-PF distinction has no
bearing on any stage's rkey expectation -- the user uobject is
always reconstructed fresh because SAVE requires the VF unbound,
which requires no live ucontexts.)

### 5.3 `dma_alloc_coherent` still fails-loud

Already covered by existing test infrastructure: kernel-side
`dma_alloc_coherent` on a tracked VF fails at first DMA (today
because of dma-iommu rejecting our domain; post-stage 1 because our
ops have no `.alloc` and `dev->dma_iommu == false`). Confirms the
"unconverted call site detector" is preserved.

### 5.4 awaiting-bind counter (stage 3 signal, foreshadowed for stages 1 and 2)

Stage 1 doesn't produce or consume `awaiting_bind` entries, so the
counter is always zero at stage 1. Stage 2 ships LOAD-side replay
that pre-installs `awaiting_bind=true` entries; stage 2's signal is
**count-of-entries match** (validated via
`MLX5_VFMIG_IOC_QUERY_AWAITING_BIND`, §6.4), not the counter -- no
consumer of `awaiting_bind` entries exists at stage 2.

Stage 3 lands the consumer (the new `vfmig_iova_bind_user_object`
primitive, invoked from `mlx5_ib_umem_restore` -- see §7.2 / §A.C).
Per `RESTORE_X` verb invocation, the counter increments by
`ib_umem_num_dma_blocks(umem)`. Post-SAVE/LOAD/`RESTORE_X` cycle, the
diagnostic `vfmig_user_mr: awaiting_bind_hits=K` reports K equal to
the total DMA-block count across all restored uobjects. Used as a
soft signal during development; the load-bearing data-path pass
criterion is Phase J in §7.5.

## 6. Stage 2 design: identity infrastructure (landed)

Stage 2 reshaped the original cursor-based sketch (preserved in §A.G)
to fit the kernel-verb-driven trigger model. Three additive pieces;
no behaviour change for fresh-registration code paths. Landed across
the eleven-commit series enumerated in §6.5.

### 6.1 What stage 2 delivers

After SAVE/LOAD with stages 1+2, the destination's registry has
`awaiting_bind=true` entries at every source IOVA the FW resource
table references (MKEY PAS for MRs; CQC `dbr_addr` and CQE-buffer
PAs for CQs; analogous for QP/SRQ; one DBR-page entry shared across
the parents that mapped it). No `iommu_map` is issued at LOAD time.
The entries are write-only metadata; consumers come in stage 3.

Wire records are **identity-only**: per-uobject `(kind, fw_id, iova,
length)`, no contents. User-page contents are CRIU's responsibility,
restored at the user-VA layer; we only track the IOVA → identity
mapping the FW will dereference.

### 6.2 The three pieces

**(a) Wire format extension** (commit `e088700f16db`). New tag
`VFMIG_WIRE_TAG_HOST_USER_PAGE` (byte layout in §A.A). Carried
alongside existing `HOST_PAGE` records. The SAVE iterator
(`vfmig_iova_for_each_external`) walks every external registry
entry and emits one record per uobject -- not per sg, since the
source-side IOVA range per uobject is contiguous (each umem's
`.map_sg` call bumps the cursor monotonically per sg, so all IOVAs
for the umem live in `[iova_base, iova_base + total_length)`). SAVE
blob structure becomes `HOST_PAGE × N (kernel slots) +
HOST_USER_PAGE × M (user-side) + opaque FW VHCA blob`. The
`vfmig_stream_header.reserved` slot was repurposed to carry
`num_user_pages`; `manifest_crc32` folds over the HUP tuples too.

**(b) Source-side retag** (commits `b2ff659fa032` MR, `f6416c8b14a1`
DBR, `fd81527efd4f` CQ, `c0183184ad82` QP, `4c522ea91f2e` SRQ). Stage
1 auto-numbers `instance_key` at `vfmig_dma_ops.map_sg` time because
the FW resource id (`mkey_index`, `cqn`, …) isn't yet known there.
Stage 2 adds a post-creation retag at every user-uobject creation
site that overwrites the auto-numbered key with
`VFMIG_HUOBJ_KEY(kind, fw_id)`. Idempotent (re-tagging an
already-tagged entry is a no-op). Callsites listed in §A.B. The
public helper signature is
`mlx5_vfmig_retag_user_<kind>(vf_dev, fw_id, iova_base, length)`;
each callsite gates on `dev->mdev->cmd.vfmig_iova_dom` (O(1),
lock-free, NULL on PF and non-vfmig VFs) to avoid touching the PF
`intf_state_mutex` on the steady-state non-vfmig path. The DBR site
retags only in the miss branch of `mlx5_ib_db_map_user` (hit branch
is already retagged from first install).

**(c) LOAD-side replay-as-placeholder** (commits `504602987fb5`
foundation + `c44a482a91ce` consumption). New state-machine arc
`VFMIG_LS_HUP_READ_SUBHDR → VFMIG_LS_HUP_REPLAY` in `vfmig.c`'s LOAD
parser (alongside the existing `VFMIG_LS_HP_*` arc for `HOST_PAGE`).
Calls `vfmig_iova_replay_external(dom, slot, instance_key, iova,
length, awaiting=true)` (the function name referenced since §4.2,
implemented in `504602987fb5`) that allocates a `vfmig_iova_page`
with `external=true, page=NULL, awaiting_bind=true`, inserts it
into the primary list and a new secondary index keyed by `(kind,
fw_id)`, and bumps the slot cursor past `iova + length`. **No
`alloc_pages`. No `iommu_map`.** The secondary index (a per-domain
`rb_root` keyed by the 64-bit `instance_key`) is the lookup point
for stage 3's binder.

### 6.3 Generality across MR / CQ / QP / SRQ / DBR

Stage 1 §4.5 already establishes that every user-side uobject's
backing memory flows through `vfmig_dma_ops.map_sg`. Stage 2's
retag-at-create + emit-per-entry / replay-at-load machinery applies
uniformly: each user-uobject creation site gets one retag callsite
in §A.B, each kind gets one entry in the kind enum (§A.A). DBR
pages are the only complication -- they're shared across multiple
parent uobjects via `mlx5_ib_db_map_user`'s dedup. Sub-keyed under
parent uobject by user VA; the dedup makes the second/third
`RESTORE_X` that lands on a previously-bound DBR a refcount-up
no-op. Full walkthrough in §A.E.

### 6.4 Stage 2 success criterion (met)

The PF cdev ioctl `MLX5_VFMIG_IOC_QUERY_AWAITING_BIND` (commit
`a0bb5ec10ed1`) returns the count of `awaiting_bind=true` entries
from the destination's registry, filterable by kind. The sidecar
test
`tools/testing/mlx5_vfmig/save_load/user_object_replay/test_user_object_replay.sh`
(commit `685842d9211d`, harness self-calibration `3513b4e28a0e`)
drives it:

* Phase A: bind a tracked source VF.
* Phase B: source probe (`user_object_replay_probe`) registers
  `N` MRs + CQ + QP + best-effort SRQ; emits an `expected_*`
  manifest the harness uses to self-calibrate the assertion
  thresholds.
* Phase C: SAVE.
* Phase D: quit probe, tear down source VF.
* Phase E: bind destination VF, LOAD.
* Phase F: `QUERY_AWAITING_BIND` on the destination, compare against
  the probe's emitted manifest.

Pass criterion: per-kind count on the destination matches what the
probe registered on the source. Stage 2 PASS = "infrastructure
works"; data path is *not* yet exercised (that's stage 3's Phase J).
The count-match property is invariant under the choice of stage-3
binder consumption order and under the choice of sg shape on
destination, which makes it a good fault-isolation signal: a
stage-3 regression that breaks data path doesn't break stage-2
count match, so this test keeps catching infrastructure regressions
independently.

### 6.5 Empirical chain (landed)

Eleven-commit Stage 2 series, in landing order:

| #  | Commit          | What it adds |
|----|-----------------|--------------|
| C1 | `504602987fb5`  | Foundation: `enum vfmig_huobj_kind`, `VFMIG_HUOBJ_KEY/KIND/FWID`, per-domain `rb_root user_index`, `vfmig_iova_install_external_placeholder_locked`, `vfmig_iova_replay_external`, `vfmig_iova_retag_external_range`, `vfmig_iova_for_each_external`, `vfmig_iova_count_awaiting_bind`. `reset_cursor` preserves the `VFMIG_SLOT_USER_PAGE` high-water mark. Buildable NOP (no callers in this commit). |
| C2 | `a0bb5ec10ed1`  | `MLX5_VFMIG_IOC_QUERY_AWAITING_BIND` PF cdev ioctl + `query_awaiting_bind` CLI verb. Mirrors `PROBE_MKEY` / `PROBE_PD` shape. Observer ships before the observed. |
| C3 | `685842d9211d`  | `user_object_replay_probe` (PD + N MRs + CQ + QP + best-effort SRQ; manifest + READY/quit handshake) + `test_user_object_replay.sh` harness (Phases A–F). C3-era baseline: ioctl callable, all counts zero (no source retag wired). |
| C4 | `e088700f16db`  | SAVE-side emit: `VFMIG_WIRE_TAG_HOST_USER_PAGE` (0x4855) + 32-byte `vfmig_host_user_page_record`; `vfmig_stream_header.reserved` → `num_user_pages`; four-pass save walker (HP-size, HUP-size, HP-emit, HUP-emit); `manifest_crc32` covers HUP tuples; `KIND_NONE` entries skipped (no retag, no emit). |
| C5 | `c44a482a91ce`  | LOAD-side replay: `VFMIG_LS_HUP_READ_SUBHDR` + `VFMIG_LS_HUP_REPLAY` states; `STREAM_HDR_READ` consumes `num_user_pages`; shared `vfmig_load_maybe_finalize_records` helper latches CRC verify + drift-detection arm to `(hp+hup)_seen == expected`; HUP subhdr validation + replay folds CRC and calls `vfmig_iova_replay_external`. |
| C6 | `b2ff659fa032`  | Source-side retag — MR (`create_real_mr`, post-FW-wired). Public helper `mlx5_vfmig_retag_user_mr` uses `vf_dev->cmd.vfmig_iova_dom` for O(1) lock-free gate; callsite gates on `dev->mdev->cmd.vfmig_iova_dom && !umem->is_dmabuf`. Non-fatal warning on retag failure. |
| C7 | `f6416c8b14a1`  | Source-side retag — DBR (`mlx5_ib_db_map_user`, miss branch only). `KIND_DBR`'s `fw_id` is `user_virt & PAGE_MASK` — only kind whose `fw_id` is a userspace VA (§6.3, §A.E). Hit branch deliberately not retagged. |
| C8 | `fd81527efd4f`  | Source-side retag — CQ (`mlx5_ib_create_cq`, post-`mlx5_core_create_cq`). Gates: `udata && cq->buf.umem && vfmig_iova_dom && !is_dmabuf`. |
| C9 | `c0183184ad82`  | Source-side retag — QP (`create_user_qp`, post-`mlx5_qpc_create_qp`). v0 skips `RAW_PACKET` / `IB_QP_CREATE_SOURCE_QPN` (split SQ/RQ umems out of scope) — see §10. |
| C10| `4c522ea91f2e`  | Source-side retag — SRQ (`mlx5_ib_create_srq`, post-`mlx5_cmd_create_srq`). Covers BASIC / XRC / TM SRQ types. Completes the five-kind retag set. |
| H  | `3513b4e28a0e`  | Harness self-calibration: `EXPECT_*_COUNT` default to the probe's emitted `src_expected_*` manifest values, so the harness automatically expects what was actually created (including `srqn=0`, which is a legitimate FW assignment). Removes the "PASS-in-disguise" failure mode. |

Validation (running kernel
`6.19.0-raphael-criu-dev-criu-c92f63055698+ #3`, 2026-05-19):

```
================ STAGE-2 USER OBJECT REPLAY VERDICT =====
source (probe-emitted):
  pdn = 20  num_mrs = 4  cqn = 2597  qpn = 200  srqn = 0
destination (QUERY_AWAITING_BIND on dst vf 0):
  kind  observed  expected
  ----  --------  --------
  MR           4         4
  CQ           1         1
  QP           1         1
  SRQ          1         1
  DBR          1         1
  TOTAL        8         8
FULL PASS (C6..C10): every kind round-trips identity infrastructure
across SAVE/LOAD on a tracked VF.
```

A teardown-time `__iommu_group_free_device` WARN is emitted on
`sriov_numvfs=0` — pre-existing detach-ordering issue, orthogonal
to Stage 2. Diagnosis + suppression is deferred to a follow-on
diagnostic-only commit (a `dev_info` trace at each early-return in
`mlx5_vfmig_vf_detach_iova_domain` to identify which gate misfires
under `sriov_numvfs=0`), then suppression once the branch is known.

## 7. Stage 3 design: kernel-verb-driven IOMMU binding

Stage 3 introduces the consumer of stage 2's pre-installed
`awaiting_bind` entries. The consumer is an **explicit kernel-side
call chain** from `mlx5_ib_restore_X` (the restore verb body)
through a new driver helper `mlx5_ib_umem_restore` to a new vfmig
primitive `vfmig_iova_bind_user_object`. No side-channel storage,
no DMA-shim conditional branch, no `task_struct` extension. The
fresh-registration path is untouched.

### 7.1 What stage 3 delivers

After SAVE/LOAD/`RESTORE_X` cycles, the destination's IOMMU has
entries at every source IOVA the LOAD-replayed FW resource state
references, mapping to dst-side phys pages of the CRIU-restored
user process. Data path works end-to-end: a remote peer's RDMA op
→ FW dereferences mkey/cqc/qpc/srqc → DMA at source IOVA → dst
IOMMU resolves to dst phys → DMA succeeds.

This closes the data-path gap recorded as `uobject_restore.md` §10.9.

### 7.2 Binding mechanism: `ib_umem_pin` + driver wrapper

The mechanism has three layers; each is a normal function call with
typed arguments. Nothing is set on `current`. The new pieces are:

**(a) IB-core primitive `ib_umem_pin`** -- a refactor of the existing
`ib_umem_get` in `drivers/infiniband/core/umem.c`. `ib_umem_pin`
does steps 1-5 of today's `ib_umem_get` (validate args, allocate
`ib_umem`, pin user pages via `pin_user_pages_fast`, build the
sgtable via `sg_alloc_append_table_from_pages`) and **stops before
`dma_map_sgtable`**. The returned umem has `sg_page(sg)` populated
per sg, but `sg_dma_address(sg)` / `sg_dma_len(sg)` are zero. The
caller is responsible for either populating them via a
driver-specific binder before first DMA, or calling `ib_umem_release`
to roll the pins back. `ib_umem_get` itself becomes a thin wrapper
around `ib_umem_pin` + `ib_dma_map_sgtable_attrs`; no behaviour
change for existing callers.

**(b) vfmig primitive `vfmig_iova_bind_user_object`** -- new
`drivers/net/ethernet/mellanox/mlx5/core/vfmig/vfmig_iova.c` entry point:

```c
int vfmig_iova_bind_user_object(struct vfmig_iova_domain *dom,
                                u8 kind, u64 fw_id,
                                struct sg_table *sgt);
```

Under `dom->lock`, looks up the placeholder by `instance_key =
VFMIG_HUOBJ_KEY(kind, fw_id)` via the C1 rb_tree. If the entry is
missing → `-ENOENT` (hard fail). If the entry has `awaiting_bind ==
false` → `-EBUSY` (hard fail: double-bind). Otherwise: walks the
sgtable at PAGE_SIZE granularity, `iommu_map`s each phys chunk at
the corresponding source-IOVA offset within the placeholder's range,
writes `sg_dma_address(sg)` / `sg_dma_len(sg)` per sg as it goes,
clears `awaiting_bind`. On any partial-bind failure, rolls back the
`iommu_map`s already issued and leaves `awaiting_bind = true` for
retry/diagnosis.

PAGE_SIZE granularity matters because src and dst phys layouts can
differ -- src might have one 1 MiB hugepage compounded into one sg
segment, dst might have 256 × 4 KiB pages with 256 sg segments. The
IOMMU just needs `iommu_map(iova, phys, PAGE_SIZE)` calls covering
the right total range; per-uobject granularity at the wire level +
PAGE_SIZE iteration at the bind site is the right combination.

**(c) Driver wrapper `mlx5_ib_umem_restore`** -- new helper in
`drivers/infiniband/hw/mlx5/`:

```c
struct ib_umem *mlx5_ib_umem_restore(struct mlx5_ib_dev *dev,
                                     enum vfmig_huobj_kind kind,
                                     u64 fw_id,
                                     unsigned long addr,
                                     size_t size, int access);
```

Composes (a) and (b): `ib_umem_pin` → on success,
`vfmig_iova_bind_user_object` → on success returns the populated
umem; on either failure, releases the umem and propagates the
errno. Each kind has its own typed callsite (no opaque blob in core);
the wrapper itself is the type-safe container of `(struct
mlx5_ib_dev *, enum vfmig_huobj_kind, u64 fw_id)`. Full
implementation sketch in §A.C.

Rationale for choosing this shape over a per-task hint (which was
the prior revision, preserved in §A.G): see §A.H risks 1, 3, 4 in
the prior revision -- all three (hint cross-talk between
`vfmig_set_bind_hint` and the intended `ib_umem_get`, verb-error
leaves hint set, kernel-thread-deferred `dma_map_sgtable`) are
structurally impossible with `ib_umem_pin` because the binding is an
explicit synchronous call from the verb body, not a side channel.

### 7.3 Per-uobject application

Each `RESTORE_X` verb calls `mlx5_ib_umem_restore` directly, passing
the kind and FW id from its UAPI input. No "set hint" / "clear hint"
dance:

* **`mlx5_ib_restore_mr`**: replaces today's `mr->umem = NULL` stub
  with
  `mr->umem = mlx5_ib_umem_restore(dev, KIND_MR, mkey_index, addr,
  length, access)`. Returns the umem error directly on failure.
* **`mlx5_ib_restore_cq`** (S5, future):
  `cq->buf.umem = mlx5_ib_umem_restore(dev, KIND_CQ, cqn, cqe_buf_addr,
  cqe_buf_len, access)`; DBR handled via a new
  `mlx5_ib_db_map_user_restore` wrapper, see §A.E.
* **`mlx5_ib_restore_qp`** (S6, future): analogous with `qpn`.
* **`mlx5_ib_restore_srq`** (S7, future): analogous with `srqn`.
* **DBR pages**: `mlx5_ib_db_map_user`'s existing dedup is preserved
  by a parallel restore-time entrypoint that, on cache miss, calls
  `mlx5_ib_umem_restore(dev, KIND_DBR, user_va & PAGE_MASK, ...)`
  for the single-page DBR umem; on cache hit, refcount++ as today.
  No hint dance, no clear-on-error. Full walkthrough in §A.E.

### 7.4 Symmetric dereg

`__mlx5_ib_dereg_mr` (and analogous for CQ/QP/SRQ) calls
`ib_umem_release(umem)` which calls `dma_unmap_sgtable` which
dispatches to `vfmig_dma_ops.unmap_sg`, which walks
`sg_dma_address` per sg, looks each up in the registry, and calls
`vfmig_iova_user_page_unmap_phys` (existing stage-1 helper) to
`iommu_unmap` and remove the entry. **Same code path as
fresh-registration's unmap.** This works because
`vfmig_iova_bind_user_object` populates `sg_dma_address` to
USER_PAGE-slot IOVAs that the registry recognizes; the
`is_external`-entry invariant in `vfmig_iova_user_page_unmap_phys`
is satisfied by both fresh and restored entries (both have
`external = true`). The asymmetry between bind-via-explicit-call and
unbind-via-iova is deliberate: dereg has the IOVA in
`sg_dma_address` and doesn't need any out-of-band info to find the
registry entry.

### 7.5 Stage 3 success criterion: Phase J data-path subtest

`test_mr_restore_mlx5_vfmig.sh` gains Phase J after the existing
Phase H (READY checkpoint + `PROBE_MKEY` external check):

* Set up a partner peer: easiest is a paired VF on the same host
  with the test process attached to both, used as RDMA loopback.
  CRIU is not involved on the partner side (it's just a verb-level
  partner, not a restored process).
* On the restored side: post a 1-WR RDMA Write of a 64-byte
  payload to the partner's MR (a fresh registration, not restored).
* On the partner side: poll for `IBV_WC_SUCCESS` with deadline.
* PASS: completion arrives, payload bytes match.
* FAIL: completion timeout or
  `IBV_WC_REM_ACCESS_ERR` / `IBV_WC_LOC_PROT_ERR` → IOMMU faulted
  → binder bug or stage-2 placeholder wrong.

Equivalent Phase J in `test_cq_restore_*.sh` /
`test_qp_restore_*.sh` / `test_srq_restore_*.sh` once those land
under S5/S6/S7. The MR-only Phase J alone is sufficient to lock
in stage-2+3 v0; the rest extend the assertion as the verbs land.

The Phase J PASS is the load-bearing data-path criterion for v0:
without it, S4b's "STRONG PASS" framing in `uobject_restore.md`
§9.1 / §9.6 is misleading. Stage 3 lands the test machinery and the
binder simultaneously; once Phase J PASSes, that doc's framing
tightens from "identity-only" to "end-to-end".

## 8. Stage 4: IOVA recycling for VFMIG_SLOT_USER_PAGE (forward-compat sketch)

Stage 1's bump cursor never recycles IOVAs on `.unmap_sg`. The slot's
allocatable space is finite (~125 GiB at the default `PER_VF_GIB=128`,
see §4.1.1). A long-running workload that thrashes MR registrations
will eventually `-ENOSPC` from `vfmig_iova_user_page_map_phys`. The
original `user_mr_dma.md` folded this into stage 2's deliverable
list; we split it out because it's orthogonal to data-path
correctness.

Three options:

* **Free bitmap (recommended first cut).** One bit per PAGE_SIZE-aligned
  IOVA in the slot. `.map_sg` searches for a PAGE_SIZE-multiple run of
  zeros; `.unmap_sg` clears bits. Simple, cache-friendly, ~125 GiB /
  4 KiB / 8 = ~4 MiB of bitmap per VF at default config -- fits
  comfortably.
* **Buddy allocator.** Power-of-two ranges; better for highly
  fragmented churn but heavier code.
* **Interval tree of free ranges.** Coalesces adjacent frees naturally;
  O(log n) per op; rb-tree machinery already in tree.

Bitmap is sufficient because (a) PAGE_SIZE granularity is the natural
quantum for our allocations and (b) we don't need power-of-two
semantics. Defer until empirical exhaustion is observed on a real
workload.

**Trigger heuristic.** Track the slot's high-water cursor position;
emit a tracepoint when it crosses 50% of slot size. Reopen stage 4
when any deployed VF emits this tracepoint on a real workload.

Stage 4 is out of scope for the data-path-continuity work. Doc-only.

## 9. DMA-buf compatibility (forward-looking)

Walking the dma-buf paths with our design in mind, to confirm
nothing in stage 1 prevents future support:

* **System-memory dma-bufs** (e.g. `system_heap`): producer's
  `map_dma_buf` callback ultimately calls
  `dma_map_sgtable(attach->dev, sgt, ...)` against the importer's
  device. With our ops on the VF, that lands in our `.map_sg` --
  same path as user MRs. **Compatible by design.**
* **Pinned dma-bufs** via `ib_umem_dmabuf_get_pinned_with_dma_device`:
  the dma_device is `pd->device->dma_device` -- the same VF PCI
  device our ops are installed on. Same path. **Compatible by
  design.**
* **Peer-to-peer dma-bufs** (e.g. GPU exporting BAR memory to NIC):
  producer calls `dma_map_resource` / `dma_map_phys(DMA_ATTR_MMIO)`.
  Stage 1 rejects `DMA_ATTR_MMIO` in `.map_phys` to avoid conflating
  system memory and MMIO source phys in the same slot. **Future
  work** to add: a separate `VFMIG_SLOT_USER_MMIO` (or
  attribute-based dispatch) that handles MMIO source phys with the
  appropriate iommu_map flags. Not a fundamental design conflict.
* **Importer-side `move_notify`** (producer relocates backing
  store; importer must re-map): drives `unmap_attachment` ->
  `dma_unmap_sgtable` -> our `.unmap_sg` -> `iommu_unmap`, then
  `map_attachment` -> `dma_map_sgtable` -> our `.map_sg`. Works
  transparently as long as our `.unmap_sg` is full teardown
  (registry entry removed, iommu_unmap called, IOVA range available
  for reuse from the slot's free pool). **Stage 1 must implement
  full teardown** -- a refcount-only unmap would break dma-buf
  re-binding, and is out of spec for the dma_map_ops contract
  anyway.

## 10. Open questions

* **Stage 2 retag timing across kinds — resolved.** The five
  per-kind callsites in §A.B (commits C6..C10 in §6.5) all sit
  post-FW-create and pre-uobject-finalize, so the retag observes the
  authoritative `fw_id` and races no SAVE iterator (SAVE runs only
  against a quiesced unbound VF). End-to-end validation: PASS
  verdict in §6.5.
* **`dev->dma_iommu` override visibility.** Are there subsystems that
  cache `dev->dma_iommu` somewhere else? `iommu_dma_init_domain`
  initializes the iova_cookie; `iommu_setup_dma_ops` is the only
  writer. Plan: a small test in stage 1 that toggles SET_TRACKED on/off
  across a single uverbs MR registration to verify both states behave
  correctly.
* **ODP** (`hmm_dma_map_pfn` not `dma_map_sgtable`): out of scope;
  needs a separate hook (sub-page granularity).
* **DEVX user-side allocations** (`MLX5_IB_OBJECT_DEVX_OBJ` raw FW
  command issuance, including `CREATE_MKEY` from devx). Probably
  fits the design via a new kind enum entry; gate-question is whether
  devx-allocated user pages flow through `ib_umem_get` /
  `dma_map_sgtable` (quick read: yes via `mlx5_ib_devx_create_dct`
  and friends). Confirm before committing devx to v0 scope.
* **RAW_PACKET / IB_QP_CREATE_SOURCE_QPN QPs (deferred from stage 2 C9).**
  These QPs have split SQ/RQ umems plumbed through alternate paths
  (`create_raw_packet_qp` rather than `create_user_qp`). The C9
  callsite explicitly skips them (`init_attr->qp_type !=
  IB_QPT_RAW_PACKET && !(qp->flags & IB_QP_CREATE_SOURCE_QPN)`). v0
  scope leaves RAW_PACKET QPs unrestored at the umem layer; revisit
  when a workload needs them.
* **DMA-buf MRs (deferred from stage 1).** DMA-buf umems don't go
  through `pin_user_pages_fast`; they attach via
  `dma_buf_map_attachment`. The C6/C8/C9/C10 callsites gate on
  `!umem->is_dmabuf`, so dma-buf umems are deliberately skipped on
  the source — they would not retag, would emit `KIND_NONE` (skipped
  by the SAVE walker), and so would not appear as `awaiting_bind`
  entries on the destination. Validating dma-buf MR continuity is a
  follow-on once a workload needs it.
* **Stage 3 mechanism choice.** The prior revision used a per-task
  `vfmig_bind_hint` as the verb-handler→`.map_sg` side channel; the
  current revision uses an explicit `ib_umem_pin` IB-core primitive
  + a driver-side `mlx5_ib_umem_restore` wrapper that calls
  `vfmig_iova_bind_user_object` directly. The hint cross-talk
  hazard (5 risks enumerated in §A.H of the prior revision) is
  structurally impossible in the current shape -- there's no
  side-channel. Open: validate at D2 time that the
  `vfmig_iova_user_page_unmap_phys` `is_external`-entry invariant
  is satisfied by placeholders bound through
  `vfmig_iova_bind_user_object` exactly as for stage-1
  cursor-allocated entries (it should be -- both set `external =
  true` -- but worth confirming).
* **Stage 4 trigger.** When do we land it? Exhaustion has to be
  observed once on a real workload; until then, it's tempting to
  defer indefinitely. Suggest tracking max watermark of cursor
  position in stats (§8) and reopen when it crosses a threshold (50%
  of slot size?) on any deployed VF.

(Stress test for "concurrent dma_map and domain destroy" -- deferred
to post-v2; nothing about user MRs changes the existing lifetime
invariant that requires the VF unbound at destroy.)

## 11. Source references

* `kernel/dma/mapping.c:230` -- `__dma_map_sg_attrs` dispatch order.
* `kernel/dma/mapping.c:120` -- `dma_go_direct` (use_dma_iommu check).
* `include/linux/dma-map-ops.h:77` -- `set_dma_ops`.
* `include/linux/iommu-dma.h:13` -- `use_dma_iommu` -> `dev->dma_iommu`.
* `drivers/iommu/dma-iommu.c:2107` -- `iommu_setup_dma_ops` is the
  only writer of `dev->dma_iommu`.
* `drivers/infiniband/core/umem.c:164` -- `ib_umem_get`. Stage 3 D1
  factors lines 178-255 (pin + sgtable build) into a new
  `ib_umem_pin` helper; `ib_umem_get` then becomes a thin wrapper
  around `ib_umem_pin` + `ib_dma_map_sgtable_attrs` (see §A.C).
* `drivers/infiniband/core/umem.c:260` -- `ib_dma_map_sgtable_attrs`
  call site inside `ib_umem_get`. Stage 3 D1 moves this to the
  `ib_umem_get` wrapper, after `ib_umem_pin` returns.
* `drivers/infiniband/hw/mlx5/mr.c:1578` -- `mlx5_ib_reg_user_mr`.
* `drivers/infiniband/hw/mlx5/main.c` -- `mlx5_ib_restore_mr` (S4b),
  the kernel verb stages 2+3 hang off.
* `drivers/net/ethernet/mellanox/mlx5/core/vfmig/vfmig_iova.c` -- the v1
  allocator. `vfmig_iova_replay_external` (commit `504602987fb5`)
  is the stage-2 LOAD-side primitive; `vfmig_iova_replay_page` now
  routes `VFMIG_SLOT_USER_PAGE` to it instead of returning
  `-EOPNOTSUPP`.
* `drivers/net/ethernet/mellanox/mlx5/core/vfmig/vfmig.c` -- LOAD-side
  state machine. Stage 2 added the parallel `VFMIG_LS_HUP_*` arc
  alongside the existing `VFMIG_LS_HP_*` (commit `c44a482a91ce`).
* `tools/testing/mlx5_vfmig/save_load/user_object_replay/` --
  stage-2 empirical harness (probe + shell script).
* `tools/testing/mlx5_vfmig/design/uar_restore.md` -- the verb-pattern
  template stages 2+3 follow.
* `tools/testing/mlx5_vfmig/design/uobject_restore.md` -- §9.1 (S4b
  landed Model A) + §9.6 (`mr_restore_probe_mlx5_vfmig` writeup) +
  §10.9 (the data-path gap stages 2+3 close).

## A. Appendix

### A.A Wire format

Byte-level layout of `vfmig_host_user_page_record`, kind enum,
`instance_key` encoding, state-machine arc.

```c
/* drivers/net/ethernet/mellanox/mlx5/core/vfmig/vfmig.c */
#define VFMIG_WIRE_TAG_HOST_PAGE       0x4842   /* existing, kernel slots */
#define VFMIG_WIRE_TAG_HOST_USER_PAGE  0x4855   /* NEW, user-side uobjects */

/* Kind enum: 8-bit space; reserves room for future shapes. */
enum vfmig_huobj_kind {
    VFMIG_HUOBJ_KIND_MR  = 1,
    VFMIG_HUOBJ_KIND_CQ  = 2,
    VFMIG_HUOBJ_KIND_QP  = 3,
    VFMIG_HUOBJ_KIND_SRQ = 4,
    VFMIG_HUOBJ_KIND_DBR = 5,
    /* expansion: ODP, DEVX, dma-buf, ... */
};

/* Instance-key encoding: 56-bit fw_id, 8-bit kind. */
#define VFMIG_HUOBJ_KEY(kind, fw_id)   (((u64)(kind) << 56) | \
                                        ((fw_id) & 0xffffffffffffffULL))
#define VFMIG_HUOBJ_KIND(k)            (((k) >> 56) & 0xff)
#define VFMIG_HUOBJ_FWID(k)            ((k) & 0xffffffffffffffULL)

/* Wire record (identity-only; no contents). */
struct vfmig_host_user_page_record {
    __le32 slot_id;          /* always VFMIG_SLOT_USER_PAGE for v0 */
    __le32 reserved;
    __le64 instance_key;     /* VFMIG_HUOBJ_KEY(kind, fw_id) */
    __le64 iova;
    __le64 length;
};
```

For multi-sg umems, the source-side IOVA range is contiguous (each
umem's `.map_sg` call bumps the cursor monotonically per sg, so all
N IOVAs for the umem live in `[iova_base, iova_base + total_length)`).
We emit one record covering the whole range, not N records -- see
§A.D for the per-uobject vs per-sg trade-off analysis.

DBR pages are PAGE_SIZE; one record each; `fw_id` encodes the
page-aligned user VA so the page-VA cross-check at restore time
(matched against `mlx5_ib_db_map_user`'s dedup keys) is direct.

LOAD-side state-machine extension:

```
VFMIG_LS_HUP_READ_HEADER  -> reads vfmig_host_user_page_record (sizeof)
VFMIG_LS_HUP_REPLAY       -> calls vfmig_iova_replay_external_locked()
                             (no _READ_DATA arc; identity-only, no payload)
```

The new replay function:

```c
int vfmig_iova_replay_external_locked(struct vfmig_iova_domain *dom,
                                      enum vfmig_iova_slot slot,
                                      u64 instance_key,
                                      dma_addr_t iova,
                                      size_t length,
                                      bool awaiting);
```

Pre-conditions: `slot == VFMIG_SLOT_USER_PAGE`; caller holds
`dom->lock`. Post-conditions: registry entry at `(slot, iova,
length)` with `external=true, page=NULL, awaiting_bind=awaiting`,
indexed in primary list and in secondary index by
`(VFMIG_HUOBJ_KIND(instance_key), VFMIG_HUOBJ_FWID(instance_key))`.
Slot cursor bumped past `iova + length`. `+0` on success, `-EINVAL`
on bad slot, `-EBUSY` if `(kind, fw_id)` already in secondary index,
`-ENOMEM` on alloc failure.

Wire compat: the new tag `0x4855` is distinct from `0x4842`. Old
LOAD parsers don't understand it and warn-skip (forward compat).
New LOAD parsers handle both. Old SAVE blobs (no `HOST_USER_PAGE`
records) replay cleanly on new LOAD code (zero awaiting_bind
entries, same as stage 1 behaviour).

### A.B Source-side retag callsite map

| Site | When called | Pre-condition | Retag with |
|---|---|---|---|
| `mlx5_ib_reg_user_mr` (post-`create_real_mr`) | after `mr->mmkey.key` assigned | `umem.sgt` populated; mlx5_core is vfmig-tracked | `VFMIG_HUOBJ_KEY(MR, mr->mmkey.key >> 8)` (mkey_index) |
| `mlx5_ib_create_cq` (post-create) | after CQC FW write returns cqn | `umem.sgt` populated for CQE buf | `VFMIG_HUOBJ_KEY(CQ, cqn)` |
| `mlx5_ib_create_qp` (post-create) | after QPC FW write returns qpn | `umem.sgt` populated for QP buf | `VFMIG_HUOBJ_KEY(QP, qpn)` |
| `mlx5_ib_create_srq` (post-create) | after SRQC FW write returns srqn | `umem.sgt` populated for SRQ buf | `VFMIG_HUOBJ_KEY(SRQ, srqn)` |
| `mlx5_ib_db_map_user` (miss branch) | new DBR page allocation | `umem.sgt` populated for the (single-page) DBR | `VFMIG_HUOBJ_KEY(DBR, virt & PAGE_MASK)` |

All five callsites use the same helper:

```c
int vfmig_iova_retag_external_range(struct vfmig_iova_domain *dom,
                                    dma_addr_t iova_base,
                                    size_t length,
                                    u64 new_instance_key);
/* Walks registry entries in [iova_base, iova_base + length). For each
 * entry: if external && current instance_key is auto-numbered (high
 * bit clear or kind-byte zero), overwrite with new_instance_key.
 * If already retagged with the same key, no-op (idempotent). If
 * retagged with a different key, returns -EEXIST and rolls back. */
```

Idempotency makes the retag safe across REREG_MR (which today calls
`mlx5_ib_reg_user_mr` semantics with the existing umem) and across
shutdown races.

DBR special handling: `mlx5_ib_db_map_user` retags only in the miss
branch (the new-allocation path). Hit branch (refcount-up on existing
`mlx5_ib_user_db_page`) does nothing -- entry is already retagged
from the first miss.

### A.C Binding mechanism implementation

**Step 1: IB-core refactor.** `drivers/infiniband/core/umem.c`
factors `ib_umem_get` into a head + tail:

```c
/**
 * ib_umem_pin - Pin userspace memory and build sgtable; no DMA mapping.
 *
 * Like ib_umem_get(), but skips ib_dma_map_sgtable_attrs(). The
 * returned umem has sg_page() populated for each sgl entry, but
 * sg_dma_address() and sg_dma_len() are unpopulated. The caller MUST
 * populate them (via a driver-specific bind helper) before the umem
 * can serve DMA, or call ib_umem_release() to roll back the pins.
 */
struct ib_umem *ib_umem_pin(struct ib_device *device, unsigned long addr,
                            size_t size, int access);
EXPORT_SYMBOL(ib_umem_pin);

struct ib_umem *ib_umem_get(struct ib_device *device, unsigned long addr,
                            size_t size, int access)
{
    struct ib_umem *umem;
    unsigned long dma_attr = (access & IB_ACCESS_RELAXED_ORDERING)
                             ? DMA_ATTR_WEAK_ORDERING : 0;
    int ret;

    umem = ib_umem_pin(device, addr, size, access);
    if (IS_ERR(umem))
        return umem;

    ret = ib_dma_map_sgtable_attrs(device, &umem->sgt_append.sgt,
                                   DMA_BIDIRECTIONAL, dma_attr);
    if (ret) {
        ib_umem_release(umem);
        return ERR_PTR(ret);
    }
    return umem;
}
EXPORT_SYMBOL(ib_umem_get);
```

No behaviour change for existing `ib_umem_get` callers. The
extracted head is otherwise identical to today's lines 178-255 of
`drivers/infiniband/core/umem.c`.

**Step 2: vfmig binding primitive.** In
`drivers/net/ethernet/mellanox/mlx5/core/vfmig/vfmig_iova.c`:

```c
int vfmig_iova_bind_user_object(struct vfmig_iova_domain *dom,
                                u8 kind, u64 fw_id,
                                struct sg_table *sgt)
{
    u64 instance_key = VFMIG_HUOBJ_KEY(kind, fw_id);
    struct vfmig_iova_page *entry;
    struct scatterlist *sg;
    dma_addr_t iova_cur;
    unsigned int i;
    size_t total = 0;
    int err = 0;

    mutex_lock(&dom->lock);

    entry = vfmig_iova_user_index_lookup_locked(dom, instance_key);
    if (!entry) {
        err = -ENOENT;        /* hard fail: no placeholder */
        goto out_unlock;
    }
    if (!entry->awaiting_bind) {
        err = -EBUSY;         /* hard fail: double bind */
        goto out_unlock;
    }

    /* sgtable total bytes must match the placeholder's recorded length. */
    for_each_sgtable_sg(sgt, sg, i)
        total += sg->length;
    if (total != entry->len) {
        err = -EINVAL;
        goto out_unlock;
    }

    iova_cur = entry->iova;
    for_each_sgtable_sg(sgt, sg, i) {
        phys_addr_t phys = page_to_phys(sg_page(sg)) + sg->offset;
        size_t left = sg->length;
        size_t step;

        sg_dma_address(sg) = iova_cur;
        sg_dma_len(sg)     = sg->length;

        while (left) {
            step = min_t(size_t, left, PAGE_SIZE);
            err = iommu_map(dom->iommu_domain, iova_cur, phys, step,
                            entry->prot, GFP_KERNEL);
            if (err)
                goto out_rollback;
            iova_cur += step;
            phys     += step;
            left     -= step;
        }
    }

    entry->awaiting_bind = false;
    atomic_long_add(total >> PAGE_SHIFT, &dom->awaiting_bind_hits);

out_unlock:
    mutex_unlock(&dom->lock);
    return err;

out_rollback:
    /* Unmap whatever we mapped so far; sg_dma_address values are
     * partially populated -- callers MUST not consume sgt after a
     * failing bind. */
    iommu_unmap(dom->iommu_domain, entry->iova, iova_cur - entry->iova);
    goto out_unlock;
}
EXPORT_SYMBOL(vfmig_iova_bind_user_object);
```

Hard-fail policy per user direction (May 2026):

* `-ENOENT` on placeholder miss surfaces CRIU plugin bugs that try
  to restore a uobject the source never SAVE'd, or whose `fw_id`
  doesn't match what stage 2 emitted.
* `-EBUSY` on `awaiting_bind == false` surfaces double-bind --
  either a CRIU plugin issuing two `RESTORE_X` for the same FW id,
  or a stage-3 internal bug.

Both errno values propagate up through `mlx5_ib_umem_restore` and
the parent `RESTORE_X` verb's UAPI rc; no silent fallback.

**Step 3: driver wrapper.** In
`drivers/infiniband/hw/mlx5/restore_umem.c` (new file, or inline
into `mr.c`/`cq.c`/`qp.c` per-kind):

```c
struct ib_umem *mlx5_ib_umem_restore(struct mlx5_ib_dev *dev,
                                     enum vfmig_huobj_kind kind,
                                     u64 fw_id,
                                     unsigned long addr, size_t size,
                                     int access)
{
    struct vfmig_iova_domain *dom = dev->mdev->cmd.vfmig_iova_dom;
    struct ib_umem *umem;
    int err;

    if (!dom)
        return ERR_PTR(-ENODEV);   /* non-vfmig path; gate at caller */

    umem = ib_umem_pin(&dev->ib_dev, addr, size, access);
    if (IS_ERR(umem))
        return umem;

    err = vfmig_iova_bind_user_object(dom, kind, fw_id,
                                       &umem->sgt_append.sgt);
    if (err) {
        ib_umem_release(umem);
        return ERR_PTR(err);
    }
    return umem;
}
```

Caller pattern (using MR as the canonical example):

```c
/* drivers/infiniband/hw/mlx5/mr.c, in mlx5_ib_restore_mr */
mr->umem = mlx5_ib_umem_restore(dev, VFMIG_HUOBJ_KIND_MR,
                                req->mkey_index,
                                cmd->addr, cmd->length, access_flags);
if (IS_ERR(mr->umem))
    return PTR_ERR(mr->umem);
```

No state on `current`. No DMA-shim conditional branch. The bind is
synchronous and explicit; if anything fails between `ib_umem_pin`
and the `mr->umem` assignment, the verb returns the errno and the
adopted-mkey kuobject is rolled back by the caller.

### A.D reg_create refactor (optional code-quality polish)

Once stage 3 lands and `mr->umem` is non-NULL on restored MRs, the
kernel-side state setup in `mlx5_ib_restore_mr` becomes nearly
identical to `reg_create`'s -- only deviations are "`mr->mmkey.key`
from hint instead of `mlx5_ib_create_mkey` return" and "no
`mlx5_ib_populate_pas` PAS write." Splitting `reg_create` into:

```c
static struct mlx5_ib_mr *reg_create_alloc_state(
    struct ib_pd *pd, struct ib_umem *umem, u64 iova,
    int access_flags, unsigned long page_size,
    int access_mode, u16 st_index, u8 ph);

static int reg_create_emit_fw(struct mlx5_ib_mr *mr,
                              struct ib_umem *umem,
                              u64 iova, int access_flags,
                              unsigned long page_size, bool populate,
                              int access_mode, u16 st_index, u8 ph);
```

…lets `mlx5_ib_restore_mr` reuse `_alloc_state` directly:

```c
mr = reg_create_alloc_state(pd, umem, iova, access, page_size,
                            MLX5_MKC_ACCESS_MODE_MTT, ...);
mr->mmkey.key = lkey_hint;     /* adopt instead of CREATE_MKEY */
set_mr_fields(dev, mr, length, access, iova);
atomic_add(ib_umem_num_pages(umem), &dev->mdev->priv.reg_pages);
return &mr->ibmr;
```

Net effect: every kernel-side field on a restored MR mirrors a
freshly-registered MR exactly, except `mr->mmkey.key` (adopted from
FW state) and the absence of a CREATE_MKEY round-trip. This is the
"make restore look like create" follow-through. Optional because
stage 3's binder makes `mr->umem` real and the existing
`mlx5_ib_restore_mr` body (after stage 3 wiring) populates fields
correctly; the refactor is clarity polish, not load-bearing.

### A.E DBR special handling

`mlx5_ib_db_map_user(context, virt, db)` semantics:

* Walks `context->db_page_list` for an existing
  `mlx5_ib_user_db_page` whose umem covers `virt & PAGE_MASK`.
* **Hit:** `refcount++`, return cached `db->dma`.
* **Miss:** allocate `mlx5_ib_user_db_page`, `ib_umem_get` the page,
  record in `db_page_list`. `db->dma = umem->sgt.dma_address[0]`.

Source-side retag inside the miss branch:

```c
/* mlx5_ib_db_map_user, miss branch, after ib_umem_get returns */
if (mlx5_core_is_vfmig_tracked(dev->mdev)) {
    vfmig_iova_retag_external_range(
        vfmig_dom(dev->mdev),
        page->umem->sgt.dma_address[0],
        PAGE_SIZE,
        VFMIG_HUOBJ_KEY(VFMIG_HUOBJ_KIND_DBR, virt & PAGE_MASK));
}
```

User VA is stable across SAVE/LOAD by CRIU's `MAP_FIXED` contract.
PAGE_SIZE granularity (DBR is always single-page).

Source-side SAVE: emits one `HOST_USER_PAGE` record per DBR umem,
keyed by `(KIND_DBR, page_va)`.

Destination-side at restore time, two-call sequence inside
RESTORE_CQ (analogously RESTORE_QP / RESTORE_SRQ):

```c
/* mlx5_ib_restore_cq pseudo-code, S5 */
cq->buf.umem = mlx5_ib_umem_restore(dev, VFMIG_HUOBJ_KIND_CQ, cqn,
                                    cqe_buf_addr, cqe_buf_len, access);
if (IS_ERR(cq->buf.umem))
    return PTR_ERR(cq->buf.umem);

/* DBR variant of mlx5_ib_db_map_user that, on cache miss, uses
 * mlx5_ib_umem_restore(KIND_DBR, user_va & PAGE_MASK) for the
 * single-page DBR umem; on cache hit (a previously-restored parent
 * already bound this DBR page), refcount++ and reuse existing
 * db->dma -- no umem_restore needed. */
err = mlx5_ib_db_map_user_restore(context, dbr_user_va, &cq->db);
if (err)
    goto err_release_buf;
```

The dedup machinery thus naturally handles the "multiple uobjects
share one DBR" case: first restored uobject does the bind (cache
miss → `mlx5_ib_umem_restore(KIND_DBR, ...)` → `iommu_map` at the
DBR placeholder's IOVA), subsequent ones are refcount-ups on the
existing `mlx5_ib_user_db_page`. No "clear on cache hit" cleanup
step is required because nothing was set in the hit path. The new
restore-variant of `mlx5_ib_db_map_user` is a parallel entry point
that takes `dev` + `(KIND_DBR, virt & PAGE_MASK)` and calls
`mlx5_ib_umem_restore` directly on cache miss instead of
`ib_umem_get`.

### A.F Cross-reference matrix

| Uobject | Buffer kind | Buffer fw_id | DBR? | Retag callsite | Restore verb |
|---|---|---|---|---|---|
| MR | `KIND_MR` | `mkey_index` | n/a | `mlx5_ib_reg_user_mr` post-`create_real_mr` | `mlx5_ib_restore_mr` (S4b, landed) |
| CQ | `KIND_CQ` | `cqn` | yes | `mlx5_ib_create_cq` post-FW-create | `mlx5_ib_restore_cq` (S5) |
| QP | `KIND_QP` | `qpn` | yes | `mlx5_ib_create_qp` post-FW-create | `mlx5_ib_restore_qp` (S6) |
| SRQ | `KIND_SRQ` | `srqn` | yes | `mlx5_ib_create_srq` post-FW-create | `mlx5_ib_restore_srq` (S7) |
| DBR (shared) | `KIND_DBR` | `user_va & PAGE_MASK` | self | `mlx5_ib_db_map_user` miss branch | n/a (sub-keyed under parent) |
| AH | none | none | none | n/a (no umem; pure FW-state) | `mlx5_ib_restore_ah` (S7) |
| PD | none | none | none | n/a (no umem; pure FW-state) | `mlx5_ib_restore_pd` (S3b, landed) |

PD and AH have no umem at all (pure FW-state-only resources) and
don't intersect with this design. Async-event uobjects (S8) are
also pure FW-state.

### A.G Earlier sketches (superseded)

This appendix preserves earlier design revisions for history. The
revision-1 cursor-based prose (superseded 2026-05-18) is in
§A.G.1-§A.G.2; the revision-2 per-task `vfmig_bind_hint` mechanism
(superseded 2026-05-19) is in §A.G.3. The current design is in §7
and §A.C.

#### A.G.0 Why each prior revision was superseded

* **Revision 1 → 2 (2026-05-18).** `uobject_restore.md` landed
  `UVERBS_OBJECT_RESTORE` / `mlx5_ib_restore_mr` (S4b) as a
  kernel-only verb that adopts the FW mkey without userspace
  re-registration. That landing changed the *trigger* model
  (kernel verb instead of userspace `ibv_reg_mr`) and made the
  cursor-based stage-2 binding and the per-task `vfmig_next_mkey`
  stage-3 hint described in §A.G.1-§A.G.2 obsolete.
* **Revision 2 → 3 (2026-05-19).** The per-task `vfmig_bind_hint`
  shape (§A.G.3) required either a `task_struct` extension or a
  per-domain task-keyed map; both add cost or core-kernel surface
  area. User direction was to avoid `task_struct` and avoid lookup
  cost on the `vfmig_dma_ops.map_sg` hot path (which fires on
  every kernel cmd-ring / EQ / frag-buf DMA, not just restore).
  The current `ib_umem_pin` + `mlx5_ib_umem_restore` shape
  achieves the same outcome with an explicit synchronous call
  from the verb body to the vfmig binding primitive, no
  side-channel storage, and zero changes to `.map_sg`.

The infrastructure pieces foreshadowed in stage 1 (the
`awaiting_bind` flag, the `awaiting_bind_hits` counter, the
`external` flag on registry entries) are preserved across the
redesign -- what changed is *how* `awaiting_bind` entries get
pre-installed (per-uobject identity-keyed vs per-sg
cursor-positioned) and *how* they get consumed (per-task hint from
kernel verb vs userspace-re-registration cursor walk).

#### A.G.1 Original §6 (Stage 2 forward-compat sketch, superseded)

> Just enough detail to confirm stage 1's choices are forward-compatible.
>
> **6.1 Wire format.** New record type alongside `HOST_PAGE`:
>
> ```
> HOST_USER_PAGE {
>     slot:           u8     (always VFMIG_SLOT_USER_PAGE in v0)
>     flags:          u8     (bit 0: external; bit 1: awaiting_bind on LOAD)
>     reserved:       u16
>     iova:           u64
>     len:            u64    (PAGE_SIZE-multiple)
>     instance_key:   u64
> }
> ```
>
> No contents field. The destination's umem-side pages will be pinned
> by the user's re-registration and bound to this `iova` at
> `vfmig_dma_ops.map_sg` time.
>
> **6.2 Order-discipline replay (no hint mechanism needed in stage 2).**
> Stage 2's lookup model is **cursor-based**, identical to how the
> existing kernel slots are replayed. No hint mechanism, no uverbs
> verb, no source-side retag step.
>
> **Why cursor-based works.** At `dma_map_sgtable` time, our `.map_sg`
> gets only `(dev, sg_table, dir, attrs)`. We don't know which user
> uobject, rkey, lkey, or FW mkey -- so we can't look up by any of
> those. But we *do* know "this is the next IOVA the slot's bump
> cursor would hand out", because that's the contract `.map_sg` has
> with the caller. As long as the destination's `.map_sg` call
> sequence matches the source's, the cursor walks land on the
> source's IOVAs in the same order, and our pre-replayed
> `awaiting_bind` entries get consumed in lockstep.
>
> **The order-discipline contract.** CRIU preserves intra-process
> call ordering: a CRIU-restored process re-issues syscalls in the
> same order as the original process did. So for the
> single-migrating-process case, the destination's `ibv_reg_mr`
> calls happen in the same order as the source's, so `.map_sg` is
> called in the same order, and the cursor walks match up.
>
> What this contract does NOT cover: multi-process workloads where
> two processes register MRs in different relative orders on source
> vs destination. Stage 2 is explicitly scoped to single-process
> migration (CRIU's primary use case anyway); multi-process is
> stage 3 territory because it requires the mkey-hint mechanism to
> break out of order-discipline.
>
> **Concrete `.map_sg` flow at stage 2.**
>
> ```
> for each sg in sg_table:
>     iova = USER_PAGE_cursor;
>     entry = find_entry(dom, iova);          /* O(log N) tree lookup */
>     if (entry && entry->awaiting_bind) {
>         /* stage-2 cursor hit -- pre-replayed entry */
>         iommu_map(dom, iova, sg_phys, sg->length, prot);
>         entry->awaiting_bind = false;
>         atomic_inc(&dom->stats.awaiting_bind_hits);
>     } else if (!entry) {
>         /* fresh allocation (stage-1 path; or stage-2 cursor miss
>          * because source had fewer registrations in this slot) */
>         install_external_phys_locked(dom, USER_PAGE,
>                                      auto_key++, iova,
>                                      sg_phys, sg->length);
>     } else {
>         /* entry exists but already bound -- caller bug or a stale
>          * registry entry. WARN and return error. */
>         return -EEXIST;
>     }
>     sg_dma_address(sg) = iova;
>     sg_dma_len(sg)     = sg->length;
>     USER_PAGE_cursor  += sg->length;
> ```
>
> Note: `instance_key` for stage 2's wire records is auto-numbered
> (per-slot counter). The wire records carry `instance_key` as a tag
> that lets the destination's drift detection cross-check the call
> sequence (e.g. "destination's 5th alloc in this slot has
> instance_key 5; source's recorded 5th alloc had instance_key 5;
> match"). It is *not* used as a primary lookup key in stage 2;
> that's strictly cursor-based.
>
> **6.3 LOAD-side replay (concrete).** LOAD parses HOST_USER_PAGE
> records and calls a new
> `vfmig_iova_replay_external_locked(dom, slot, key, iova, len, awaiting=true)`
> that creates an entry with `external=true, page=NULL,
> awaiting_bind=true`. Then `vfmig_iova_reset_cursor()` rewinds the
> USER_PAGE cursor back to slot base (same machinery the kernel
> slots already use), so the destination's first `.map_sg` call
> lands at the source's first user-MR IOVA.
>
> **6.4 What stage 1 ships toward stage 2 without doing it yet.**
> Stage 1 doesn't emit wire records, doesn't replay them, and
> doesn't populate `awaiting_bind`. But the registry data structure
> already has the `external` flag (4.2), the `awaiting_bind` flag
> (4.2 new), and the `awaiting_bind_hits` counter (5.4) wired in
> stage 1, all reading zero. Stage 2 is then purely additive: turn
> on the iterator emission for external entries, add the new wire
> record type and the LOAD parser, and let the existing
> cursor-based `.map_sg` flow pick up `awaiting_bind` entries
> naturally.

**Why superseded.** S4b landed `mlx5_ib_restore_mr` as a kernel
verb. The CRIU-restored userspace does not call `ibv_reg_mr` for
restored MRs at all -- the CRIU plugin issues the kernel verb
instead. This breaks the order-discipline contract end-to-end:
without a userspace re-registration, no `.map_sg` is ever called
from the user side for that MR, and the cursor-based approach
never hits the `awaiting_bind` entries. The trigger now is the
kernel verb itself; cursor-position-as-identity is replaced by
explicit `(kind, fw_id)` lookup keyed off a per-task hint set by
the verb.

#### A.G.2 Original §7 (Stage 3 forward-compat sketch, superseded)

> Stage 3 adds two things on top of stage 2's mlx5_core consistency:
> the uverbs vendor verb pair, and the per-task hint mechanism that
> breaks `.map_sg` out of cursor-based order-discipline.
>
> **7.1 Uverbs vendor verbs.** `MLX5_IB_OBJECT_VFMIG_MR` namespace,
> parallel to `MLX5_IB_OBJECT_VFMIG_UCONTEXT`:
>
> ```
> MLX5_IB_METHOD_VFMIG_QUERY_MR(uobject_handle)
>   -> { rkey, lkey, mkey, virt_addr, length, access_flags,
>        umem.address, umem.iova, num_sgs, ... }
>
> MLX5_IB_METHOD_VFMIG_RESTORE_MR(uobject_handle, blob_from_query)
>   -> validates blob; binds the freshly-allocated ib_uobject MR + umem
>      to the FW MKEY (preserved by LOAD_VHCA_STATE); preconditions
>      mirror the UCONTEXT shape (ucontext opened with VFMIG_RESTORE
>      flag, lib_uar_dyn=false, strict META cross-check, no
>      re-allocation of the rkey).
> ```
>
> CRIU drives these. Userspace contract: the restored process's
> `ibv_reg_mr` returns a buffer registered with the same rkey as
> pre-checkpoint, so peers still using the old rkey on the wire
> continue working.
>
> **7.2 Per-task mkey hint mechanism.** Order-discipline (stage 2)
> breaks down when CRIU restores MRs in a different order than the
> source registered them, or when multiple processes share a VF.
> Stage 3 introduces an explicit lookup-by-mkey path:
>
> ```
> mlx5_ib_vfmig_restore_mr(uobject, blob_from_query) {
>     /* per-task hint, cleared after one umem registration */
>     current->vfmig_next_mkey = blob.mkey;
>     /* trigger user-driven re-registration ... */
> }
> ```
>
> In `.map_sg`, before falling through to the cursor-based path,
> check `current->vfmig_next_mkey`. If set:
>
> * Look up registry entries by `(VFMIG_SLOT_USER_PAGE,
>   ((u64)hint_mkey << 32) | sg_idx)` instead of by cursor IOVA.
> * Hit -> bind freshly-pinned page to the pre-replayed IOVA, clear
>   `awaiting_bind`, increment `awaiting_bind_hits`.
> * Miss -> error (CRIU asked us to bind to a mkey we don't have a
>   record for; fail loudly, the bug is upstream).
>
> The hint is consumed by the first `.map_sg` of the registration
> and then cleared, so subsequent registrations from the same task
> fall back to cursor-based unless another `RESTORE_MR` re-arms it.
>
> **7.3 Source-side mkey retag (stage 3 concomitant).** For the
> destination's hint-based lookup to find the right registry
> entries, stage 3 needs the *source* to have tagged its registry
> entries with mkey-based instance_keys. So stage 3 adds, on top of
> stage 2's source-side machinery:
>
> * A new helper `vfmig_iova_tag_user_mr(dom, sgt, mkey)` called by
>   mlx5_ib's `mlx5_ib_reg_user_mr` post-UMR completion: walks the
>   sgt, looks up each registry entry by `sg_dma_address(sg)`, and
>   overwrites `instance_key = ((u64)mkey << 32) | sg_idx`.
> * Wire records emitted in stage 3 carry these mkey-based keys.
>
> **7.4 What if a single sgt-segment maps multiple pages and FW
> re-coalesces the PAS?** Won't matter at this level. We tag entries
> by `sg_dma_address`; each `iommu_map` we did was per-sg-segment
> keyed by the segment's chosen IOVA; FW's PAS coalescing happens
> above us at MKEY-write time. Both source and destination see the
> same FW-mkey -> per-segment-iova mapping, so `(mkey, sg_idx)` is
> unambiguous on both sides.

**Why superseded.** Same reason as §A.G.1: the trigger model
flipped from userspace re-registration to kernel verb. The
specific `(mkey, sg_idx)` lookup key is also replaced -- the new
design uses `(kind, fw_id)` *per uobject* (one wire record per
umem, not per sg) and iterates PAGE_SIZE chunks at bind time
inside the binding primitive, decoupling source vs destination sg
shapes. Consequently the source-side retag also reshapes: now it
tags entries with `(kind, fw_id)` post-FW-create rather than
`(mkey, sg_idx)` post-UMR, and applies uniformly across MR / CQ /
QP / SRQ / DBR rather than just MR (§A.B).

#### A.G.3 Revision-2 per-task `vfmig_bind_hint` mechanism (superseded 2026-05-19)

This revision (the prose previously at §7.2 and §A.C between
2026-05-18 and 2026-05-19) routed `(kind, fw_id)` from the verb
handler to `vfmig_dma_ops.map_sg` via a side channel on
`task_struct`:

> ```c
> struct vfmig_bind_hint {
>     u8  kind;       /* VFMIG_HUOBJ_KIND_{MR,CQ,QP,SRQ,DBR} */
>     u8  reserved[7];
>     u64 fw_id;      /* mkey_index | cqn | qpn | srqn | dbr_user_va */
> };
> /* Stored in task_struct. Default-zero; nonzero kind means "binder
>  * mode". Cleared by vfmig_dma_ops.map_sg on first consumption. */
> ```
>
> The hint was a **trigger** for `.map_sg`, not a payload. Each
> `RESTORE_X` verb set it immediately before calling `ib_umem_get`:
>
> ```c
> vfmig_set_bind_hint(VFMIG_HUOBJ_KIND_MR, mkey_index);
> mr->umem = ib_umem_get(ibdev, addr, length, access);
> /* hint consumed (cleared) by .map_sg inside ib_umem_get */
> vfmig_clear_bind_hint();  /* defensive on error paths */
> ```
>
> Inside `.map_sg`, when the hint was set, the shim entered "binder
> mode": looked up the placeholder by `(kind, fw_id)` in the C1
> rb_tree, `iommu_map`d freshly-pinned pages at the source IOVAs,
> populated `sg_dma_address`, cleared `awaiting_bind`. When the hint
> was unset, the existing stage-1 cursor-allocate path ran. DBR
> pages got a second hint set/clear cycle before
> `mlx5_ib_db_map_user`'s dedup'd umem_get.
>
> The hint was **one-shot** (cleared on consumption) to prevent a
> single verb's hint from incorrectly binding a subsequent
> unrelated `dma_map_sgtable` call on the same task. The known risk
> enumeration was Risks 1-5 documented in the
> 2026-05-18-to-2026-05-19 §A.H ("Hint cross-talk hazard, full
> enumeration").

**Why superseded.** Per-task storage required either extending
`task_struct` directly (touching core kernel surface area + a
Kconfig gate) or maintaining a per-domain task-keyed map (RCU or
mutex lookup on every `.map_sg` -- not just restore traffic, but
every cmd-ring / EQ / frag-buf DMA on every vfmig-tracked VF; for
the steady-state RDMA workload that's the wrong tradeoff). The
`ib_umem_pin` shape achieves the same outcome with no `task_struct`
extension, no `.map_sg` hot-path lookup, and zero side-channel
risk (Risks 1-5 of the prior revision are all structurally
impossible -- see §A.H bullet 1).

The deeper architectural shift: in revision 2, the bind was driven
by `dma_map_sgtable` (via the standard
`ib_umem_get → dma_map_sgtable → vfmig_dma_ops.map_sg` chain) with
the hint as the disambiguator. In the current revision, the bind
is driven directly by the verb body, and `dma_map_sgtable` is not
called at all on the restore path -- a clean separation between
"steady-state DMA mapping" and "CRIU restore-time binding".
Symmetric dereg is preserved because populating `sg_dma_address`
during the explicit bind makes the existing
`vfmig_dma_ops.unmap_sg` path work unmodified.

### A.H Implementation notes / extended open questions

* **Hint cross-talk hazard -- no longer applies.** The 5-risk
  enumeration from the prior revision (Risk 1: intermediate
  `dma_map_sgtable` between hint-set and intended `ib_umem_get`;
  Risk 3: error path leaves hint set; Risk 4: preemption; Risk 5:
  kernel-thread workqueue defers `dma_map_sgtable` and misses the
  per-task hint) is structurally impossible in the `ib_umem_pin`
  approach because there is no out-of-band channel: the bind is a
  direct synchronous call from `mlx5_ib_umem_restore` (the verb
  body's helper) to `vfmig_iova_bind_user_object`, with all args
  passed by value. The vfmig `.map_sg` shim is untouched by stage 3.
  Risk 2 (single-vs-multiple `dma_map_sgtable` per umem) is also
  moot because `ib_umem_pin` doesn't call `dma_map_sgtable` at all.
  Original enumeration preserved in §A.G for design history.
* **`ib_umem_pin` lifecycle invariants** (replaces the hint
  enumeration).
  * **L1 Caller obligation.** After `ib_umem_pin` returns success,
    the caller MUST either populate `sg_dma_address` for every sg
    (via `vfmig_iova_bind_user_object` or equivalent) before any
    DMA touches the umem, OR call `ib_umem_release` to roll back
    the pins. There's no third option; the umem is not consumable
    in the half-pinned state.
  * **L2 Release-before-bind ordering on error.** If
    `vfmig_iova_bind_user_object` fails *after* partially
    populating `sg_dma_address`, the rollback path inside the
    primitive does `iommu_unmap` for the partially-bound range but
    leaves `sg_dma_address` half-written. The caller (the
    `mlx5_ib_umem_restore` wrapper) MUST treat the umem as opaque
    on the error path and only call `ib_umem_release` -- it must
    not consume `sg_dma_address` itself. `ib_umem_release` is
    safe in this state because `vfmig_dma_ops.unmap_sg` walks
    `sg_dma_address` per sg and skips zero values.
  * **L3 No partial-success returns.** `vfmig_iova_bind_user_object`
    is all-or-nothing at the placeholder level: on any failure
    after the lookup, it rolls back all `iommu_map`s and returns
    the umem to "awaiting_bind = true" state for retry/diagnosis
    by userspace. This trades a sliver of internal complexity for
    a clean retry semantics.
* **Hard-fail policy on placeholder mismatch / double-bind.**
  Both `-ENOENT` (no placeholder for the requested `(kind, fw_id)`)
  and `-EBUSY` (placeholder already bound) are returned directly to
  the caller without silent fallback. Per user direction (May
  2026): silent fallback would mask CRIU plugin bugs (e.g.
  restoring an MR the source never SAVE'd, or issuing two
  `RESTORE_X` for the same FW id). Userspace observes the failure
  via the verb's UAPI rc and can debug deterministically.
* **Source-side retag concurrency.** Retag walks registry entries
  in a range with `dom->lock`. The range is freshly populated by
  the just-completed `dma_map_sgtable` from the same task; no
  other task observes those entries until retag returns. Race-free.
* **Stage 2 alone runtime.** Stage 2 has landed; stage 3 has not.
  Current behaviour: source emits `HOST_USER_PAGE` records;
  destination LOAD pre-installs `awaiting_bind` entries;
  `mlx5_ib_restore_mr` continues to set `mr->umem = NULL` (S4b
  identity-only). Result: `awaiting_bind` entries remain
  `awaiting_bind = true` forever, the verb returns success but
  data path faults at first DMA. Failure mode is identical to
  today's "S4b identity-only PASS" -- no worse, no better. Stage 3
  D4 (the `mlx5_ib_restore_mr` rewire) closes this gap; until
  then, the `test_user_object_replay.sh` harness PASS continues to
  assert stage-2 infrastructure independently of data path.
* **`MLX5_VFMIG_IOC_QUERY_AWAITING_BIND` ioctl shape.** Returns:

  ```c
  struct mlx5_vfmig_query_awaiting_bind {
      __u32 vf_id;
      __u32 reserved;
      __u64 total_count;          /* sum across all kinds */
      __u64 count_by_kind[8];     /* index by kind enum */
      __u64 reserved2[8];
  };
  ```

  Used by `test_user_object_replay.sh` to assert installation
  count matches source-side emission count, broken down by kind.
* **awaiting_bind_hits counter, scope.** Incremented on every
  PAGE_SIZE chunk bound by `vfmig_iova_bind_user_object`. Surfaced
  via the existing `vfmig_iova_awaiting_bind_hits` accessor + a
  debugfs entry. Not used as a hard pass criterion (Phase J
  data-path test is); used as a soft signal during development.
* **Multi-process VF sharing under v0.** v0 is single-process per
  VF (CRIU's primary use case). The binding mechanism is also
  multi-process-safe by construction: each `mlx5_ib_umem_restore`
  call is independently authenticated by `(kind, fw_id)` against
  the per-VF rb_tree, with no per-task state involved.
  Multi-process is therefore not a future redesign -- just a future
  enabler in the CRIU plugin layer.
* **Stage 4 trigger heuristic.** Track the slot's high-water
  cursor position; emit a tracepoint when it crosses 50% of slot
  size. Reopen stage 4 when any deployed VF emits this tracepoint
  on a real workload.
