# DESIGN: user-MR DMA coverage in the v1 deterministic IOVA allocator

Status: **proposed**, pre-implementation. This doc covers the design we
agreed on before any code lands. Lives alongside `uar_restore.md`;
the verb-level cross-driver identity model documented there carries over
verbatim into stage 3 of this work.

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

This work is staged across three PRs. Stage 1 is the focus of this doc;
stages 2 and 3 are sketched here so stage 1's choices remain
forward-compatible.

**Stage 1 (this PR) -- in scope**

* New file `drivers/net/ethernet/mellanox/mlx5/core/vfmig_dma_ops.c`
  exposing `vfmig_dma_ops_attach(vf_pdev, dom)` /
  `vfmig_dma_ops_detach(vf_pdev, dom)`. Internally:
  * `.map_sg` / `.unmap_sg` / `.map_phys` / `.unmap_phys` route through
    a new internal `vfmig_iova_install_external_phys_locked()` helper
    that installs an externally-owned phys at a deterministic IOVA
    (no `alloc_pages` -- the page is owned by the umem and unpinned
    by `__ib_umem_release`).
  * `.alloc` / `.free` deliberately **left unimplemented**. Unconverted
    in-tree `dma_alloc_coherent` callers continue to fail loudly. (Same
    property as today; the diagnostic value of "every unconverted site
    fails at first DMA" is preserved.) MR pages are *host-side*
    allocations already pinned by `pin_user_pages_fast`, so the lack of
    `.alloc` is by design -- our ops never invent new device-side
    coherent buffers; the only thing they do is install pre-existing
    host phys at deterministic IOVAs.
  * `.map_phys` rejects `DMA_ATTR_MMIO` for v1 with a ratelimited warn.
    Peer-to-peer DMA from an MMIO source (e.g. GPU BAR -> NIC IOVA)
    would otherwise conflate system memory and MMIO source phys in the
    same slot, which is wrong. Future work.
  * `.sync_*` no-ops on x86 cache-coherent. (Documented as such; future
    arches with non-coherent DMA on this device will need adjustment.)
* Per-VF override of `dev->dma_iommu` to false during the lifetime of
  our unmanaged domain, so the DMA core dispatches `dma_map_sgtable`
  to our `.map_sg` rather than to `iommu_dma_map_sg` (see section 3.3).
* New slot `VFMIG_SLOT_USER_PAGE` (appended at the next free index;
  existing slots' IOVA bases unchanged -- no wire-incompat).
* `struct vfmig_iova_page` gains a `bool external` flag.
  * `vfmig_iova_destroy_page_locked` skips `__free_pages` on external
    entries.
  * `vfmig_iova_for_each` iterator skips external entries today
    (stage 1 doesn't emit them on the wire). Stage 2 lifts this skip
    and emits identity-only records.
* Hook attach into `vfmig_iova_domain_create()` immediately after
  `iommu_attach_device`; detach into `vfmig_iova_domain_destroy()`
  immediately before `iommu_detach_device`. dma_ops lifetime is then
  exactly equal to unmanaged-domain lifetime.
* Test plan: `PINGPONG=1 ./save_load/test_iova_tracked_save_load.sh` Phase A and Phase D both
  flip from FAIL to PASS. **Plus** rkey logging (see section 5.2):
  the test prints `vfmig_user_mr: source_rkey=...` and
  `vfmig_user_mr: dest_rkey=...` so we can read off, by inspection,
  which stage's invariant the running kernel matches. No hard
  assertion at any stage; the diagnostic value is in the printout.

**Stage 1 -- explicitly out of scope**

* Wire format extension (no new record type yet).
* Destination-side replay of user-MR mappings. The destination's user
  MRs go through the same dma_ops path freshly; LOAD-replayed FW MKEY
  entries from the source are orphan in stage 1 and harmless as long as
  nobody dereferences them.
* User-MR identity preservation across SAVE/LOAD. Pre-SAVE rkey is
  not guaranteed to match post-LOAD rkey on the same userspace
  re-register call (the rkey logging in 5.2 is the diagnostic).
  Stages 2/3 deliver this.
* DM (on-chip device memory). DM uses on-chip SRAM, not host DMA, and
  bypasses this whole path -- handled separately as part of R3 per
  `uar_restore.md` section 1 ("Out of scope").
* DMA-buf user MRs (`reg_user_mr_dmabuf`). Handled via
  `ib_umem_dmabuf_get` / `dma_buf_map_attachment`. Out of scope for
  stage 1, but **the path is compatible by design**: see section 8
  for the dma-buf compatibility analysis. No fundamental design
  conflict.
* ODP (on-demand paging). Calls `hmm_dma_map_pfn` not
  `dma_map_sgtable`; out of scope for stage 1.

**Stage 2 (next PR) -- mlx5_core MR mapping continuity (order-discipline)**

The cross-host vs same-PF axis is **orthogonal** to the mlx5_core vs
uverbs axis. SAVE always requires the VF unbound, which requires no
live ucontexts, which means user uobjects are torn down before SAVE
in *all* topologies (same-PF or cross-host). So uverbs identity is
never preserved without explicit verb-driven reconstruction (stage
3). Stage 2 is about getting the mlx5_core layer consistent across
SAVE/LOAD; topology is irrelevant.

* Extend the SAVE wire format with an *identity-only* record type
  for user-MR mappings: `(slot, instance_key, iova, len)`, no
  contents. `HOST_PAGE` continues emitting kernel-slot entries with
  contents. (Identity-only because user-page contents are CRIU's
  responsibility, restored at the user-VA layer.)
* LOAD parses identity-only records and pre-installs registry
  entries with `external = true, page = NULL, awaiting_bind = true`.
  `vfmig_iova_reset_cursor()` puts the destination's `USER_PAGE`
  cursor back at slot base (same as for kernel slots).
* Destination `vfmig_dma_ops.map_sg` reuses the existing
  kernel-slot replay machinery: cursor-walk hits an `awaiting_bind`
  entry, binds a freshly-pinned page to the pre-replayed IOVA via
  `iommu_map`, clears `awaiting_bind`. **No hint mechanism
  required** -- the cursor position *is* the identity, and CRIU
  preserves intra-process call ordering for the single-migrating-
  process case stage 2 targets.
* Stage 2 also lands the per-slot **free bitmap** for `USER_PAGE`
  recycling on `.unmap_sg` (see section 4.3).
* What stage 2 delivers at the mlx5_core layer: the destination's
  IOMMU is programmed to back the FW MKEY's PAS entries with the
  destination process's freshly-pinned pages. Replayed FW MKEYs in
  the destination's MKEY table are *live*: a remote peer using the
  source's old `rkey` would dereference the MKEY's PAS, the IOMMU
  would resolve to destination pages, the RDMA op would complete
  data-correctly. The user's uobject is fresh (with a new `rkey`),
  so the user can't yet discover what arrived -- that's stage 3.
* Validation: tracepoint or counter for "user-MR registrations
  bound to pre-replayed `awaiting_bind` entry" vs "fresh
  allocation". Stage 2 success == nonzero awaiting-bind hits on the
  destination after a SAVE/LOAD cycle. The rkey diagnostic from
  5.2 is **expected to still show** `dest_rkey != source_rkey` at
  this stage; rkey continuity is stage 3.

**Stage 3 (later PR) -- uverbs identity continuity**

* `MLX5_IB_OBJECT_VFMIG_MR` namespace with `QUERY_MR` and
  `RESTORE_MR` methods, parallel to
  `MLX5_IB_OBJECT_VFMIG_UCONTEXT/{QUERY,RESTORE}`.
* Per-task hint mechanism: `RESTORE_MR` sets
  `current->vfmig_next_mkey = blob.mkey` before triggering the user
  re-registration; our `.map_sg` reads the hint and looks up the
  pre-replayed entry by `(slot, mkey, sg_idx)` instead of by cursor
  position. This breaks out of the order-discipline contract --
  CRIU may restore MRs in any order.
* Source-side companion: stage 3 also adds the post-UMR retag step
  that overwrites the auto-numbered `instance_key` with mkey-based
  identity, so the wire records become mkey-keyed.
* `RESTORE_MR` binds the fresh `ib_uobject` to the existing FW
  MKEY rather than allocating a new one, so `dest_rkey ==
  source_rkey`.
* Validation: rkey continuity end-to-end (the diagnostic from 5.2
  flips to "expected equal").

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
  (cannot be derived from anything stable inside `.map_sg`; see 6.2
  for why). Stage 2 introduces a post-UMR retag step that overwrites
  the auto-numbered keys with `((u64)mlx5_mkey.key << 32) | sg_idx`,
  i.e. tags the registry entries with the FW MKEY *after* the FW has
  assigned it. Wire records emitted in stage 2 use those FW-mkey-based
  keys.

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
* When `external == true && awaiting_bind == true` (stage 2 only,
  pre-bind):
  * Created by `vfmig_iova_replay_external_locked(dom, slot, key,
    iova, len, awaiting=true)` from a HOST_USER_PAGE record.
  * Entry has no `iommu_map` installed yet (`page` is NULL, no phys
    to map). The IOVA reservation exists in the registry only as
    metadata for subsequent `.map_sg` to consume.
  * Destination `.map_sg` cursor walk lands on the entry, calls
    `iommu_map(dom, iova, sg_phys, sg->length, prot)` and clears
    `awaiting_bind`.
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
-- this is the contract dma-buf re-binding requires (see section 8).
**Stage 1 limitation**: per-slot bump cursors don't go backwards, so
the IOVA range itself isn't recycled. Bounded for typical workloads
(registrations are process-lifetime; user-MR IOVA budget at the
proposed default `PER_VF_GIB=16` is ~12.5 GiB per VF, ~3.2M 4 KiB
entries).

**Stage 2 deliverable**: a per-slot free bitmap or freelist for
`VFMIG_SLOT_USER_PAGE` so unmap returns the IOVA range to the slot's
allocatable pool. Lands as part of stage 2 (alongside the wire
format extension), not later -- without it, long-running workloads
that thrash MR registrations would eventually exhaust the slot.
Stage 1 ships without recycling because the validation workloads
(pingpong, bounded test programs) don't trigger exhaustion; stage 2
moves to a real workload model where it does.

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
fresh MR was allocated; the LOAD-replayed FW MKEY is orphan and
its IOVAs are not in the destination's IOMMU.

Stage 2 documented expectation: `dest_rkey != source_rkey` *(still
not equal at this stage!)*. The mlx5_core layer is consistent --
the IOMMU is programmed at `(source_rkey, sg_idx) -> destination
page` and the replayed FW MKEY is *live* -- but the user's
`ibv_reg_mr` produces a fresh MKEY because no verb tells FW to bind
to the replayed one. Stage 2's observable signal is **not** rkey
continuity; it's the awaiting-bind tracepoint/counter (see 5.4).

Stage 3 documented expectation: `dest_rkey == source_rkey` -- the
`RESTORE_MR` verb binds the fresh `ib_uobject` to the existing FW
MKEY.

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

### 5.4 awaiting-bind counter (stage 2 signal, foreshadowed for stage 1)

Stage 1 doesn't produce or consume `awaiting_bind` entries (no wire
emission, no LOAD-side replay), so the counter is always zero at
stage 1. The counter exists in stage 1 as a hook for stage 2 to
populate; the test reads it and prints
`vfmig_user_mr: awaiting_bind_hits=%llu`. Stage 1 expected value:
0. Stage 2 expected value after a SAVE/LOAD cycle: > 0 (specifically,
equal to the number of pre-replayed user-MR entries the destination
process re-registered against). The same hook serves stage 3 as a
sanity check (mkey-hint-based lookups should also populate the
counter when they hit a pre-replayed entry).

## 6. Stage 2 forward-compat sketch

Just enough detail to confirm stage 1's choices are forward-compatible.

### 6.1 Wire format

New record type alongside `HOST_PAGE`:

```
HOST_USER_PAGE {
    slot:           u8     (always VFMIG_SLOT_USER_PAGE in v0)
    flags:          u8     (bit 0: external; bit 1: awaiting_bind on LOAD)
    reserved:       u16
    iova:           u64
    len:            u64    (PAGE_SIZE-multiple)
    instance_key:   u64
}
```

No contents field. The destination's umem-side pages will be pinned
by the user's re-registration and bound to this `iova` at
`vfmig_dma_ops.map_sg` time.

### 6.2 Order-discipline replay (no hint mechanism needed in stage 2)

Stage 2's lookup model is **cursor-based**, identical to how the
existing kernel slots are replayed. No hint mechanism, no uverbs
verb, no source-side retag step.

**Why cursor-based works.** At `dma_map_sgtable` time, our `.map_sg`
gets only `(dev, sg_table, dir, attrs)`. We don't know which user
uobject, rkey, lkey, or FW mkey -- so we can't look up by any
of those. But we *do* know "this is the next IOVA the slot's bump
cursor would hand out", because that's the contract `.map_sg` has
with the caller. As long as the destination's `.map_sg` call sequence
matches the source's, the cursor walks land on the source's IOVAs in
the same order, and our pre-replayed `awaiting_bind` entries get
consumed in lockstep.

**The order-discipline contract.** CRIU preserves intra-process call
ordering: a CRIU-restored process re-issues syscalls in the same
order as the original process did. So for the single-migrating-
process case, the destination's `ibv_reg_mr` calls happen in the
same order as the source's, so `.map_sg` is called in the same
order, and the cursor walks match up.

What this contract does NOT cover: multi-process workloads where
two processes register MRs in different relative orders on source
vs destination. Stage 2 is explicitly scoped to single-process
migration (CRIU's primary use case anyway); multi-process is stage
3 territory because it requires the mkey-hint mechanism to break
out of order-discipline.

**Concrete `.map_sg` flow at stage 2.**

```
for each sg in sg_table:
    iova = USER_PAGE_cursor;
    entry = find_entry(dom, iova);          /* O(log N) tree lookup */
    if (entry && entry->awaiting_bind) {
        /* stage-2 cursor hit -- pre-replayed entry */
        iommu_map(dom, iova, sg_phys, sg->length, prot);
        entry->awaiting_bind = false;
        atomic_inc(&dom->stats.awaiting_bind_hits);
    } else if (!entry) {
        /* fresh allocation (stage-1 path; or stage-2 cursor miss
         * because source had fewer registrations in this slot) */
        install_external_phys_locked(dom, USER_PAGE,
                                     auto_key++, iova,
                                     sg_phys, sg->length);
    } else {
        /* entry exists but already bound -- caller bug or a stale
         * registry entry. WARN and return error. */
        return -EEXIST;
    }
    sg_dma_address(sg) = iova;
    sg_dma_len(sg)     = sg->length;
    USER_PAGE_cursor  += sg->length;
```

Note: `instance_key` for stage 2's wire records is auto-numbered
(per-slot counter). The wire records carry `instance_key` as a tag
that lets the destination's drift detection cross-check the call
sequence (e.g. "destination's 5th alloc in this slot has
instance_key 5; source's recorded 5th alloc had instance_key 5;
match"). It is *not* used as a primary lookup key in stage 2; that's
strictly cursor-based.

### 6.3 LOAD-side replay (concrete)

LOAD parses HOST_USER_PAGE records and calls a new
`vfmig_iova_replay_external_locked(dom, slot, key, iova, len, awaiting=true)`
that creates an entry with `external=true, page=NULL,
awaiting_bind=true`. Then `vfmig_iova_reset_cursor()` rewinds the
USER_PAGE cursor back to slot base (same machinery the kernel slots
already use), so the destination's first `.map_sg` call lands at the
source's first user-MR IOVA.

### 6.4 What stage 1 ships toward stage 2 without doing it yet

Stage 1 doesn't emit wire records, doesn't replay them, and doesn't
populate `awaiting_bind`. But the registry data structure already has
the `external` flag (4.2), the `awaiting_bind` flag (4.2 new), and the
`awaiting_bind_hits` counter (5.4) wired in stage 1, all reading
zero. Stage 2 is then purely additive: turn on the iterator emission
for external entries, add the new wire record type and the LOAD
parser, and let the existing cursor-based `.map_sg` flow pick up
`awaiting_bind` entries naturally.

## 7. Stage 3 forward-compat sketch

Stage 3 adds two things on top of stage 2's mlx5_core consistency:
the uverbs vendor verb pair, and the per-task hint mechanism that
breaks `.map_sg` out of cursor-based order-discipline.

### 7.1 Uverbs vendor verbs

`MLX5_IB_OBJECT_VFMIG_MR` namespace, parallel to
`MLX5_IB_OBJECT_VFMIG_UCONTEXT`:

```
MLX5_IB_METHOD_VFMIG_QUERY_MR(uobject_handle)
  -> { rkey, lkey, mkey, virt_addr, length, access_flags,
       umem.address, umem.iova, num_sgs, ... }

MLX5_IB_METHOD_VFMIG_RESTORE_MR(uobject_handle, blob_from_query)
  -> validates blob; binds the freshly-allocated ib_uobject MR + umem
     to the FW MKEY (preserved by LOAD_VHCA_STATE); preconditions
     mirror the UCONTEXT shape (ucontext opened with VFMIG_RESTORE
     flag, lib_uar_dyn=false, strict META cross-check, no
     re-allocation of the rkey).
```

CRIU drives these. Userspace contract: the restored process's
`ibv_reg_mr` returns a buffer registered with the same rkey as
pre-checkpoint, so peers still using the old rkey on the wire
continue working.

### 7.2 Per-task mkey hint mechanism

Order-discipline (stage 2) breaks down when CRIU restores MRs in a
different order than the source registered them, or when multiple
processes share a VF. Stage 3 introduces an explicit lookup-by-mkey
path:

```
mlx5_ib_vfmig_restore_mr(uobject, blob_from_query) {
    /* per-task hint, cleared after one umem registration */
    current->vfmig_next_mkey = blob.mkey;
    /* trigger user-driven re-registration ... */
}
```

In `.map_sg`, before falling through to the cursor-based path, check
`current->vfmig_next_mkey`. If set:

* Look up registry entries by `(VFMIG_SLOT_USER_PAGE,
  ((u64)hint_mkey << 32) | sg_idx)` instead of by cursor IOVA.
* Hit -> bind freshly-pinned page to the pre-replayed IOVA, clear
  `awaiting_bind`, increment `awaiting_bind_hits`.
* Miss -> error (CRIU asked us to bind to a mkey we don't have a
  record for; fail loudly, the bug is upstream).

The hint is consumed by the first `.map_sg` of the registration
and then cleared, so subsequent registrations from the same task
fall back to cursor-based unless another `RESTORE_MR` re-arms it.

### 7.3 Source-side mkey retag (stage 3 concomitant)

For the destination's hint-based lookup to find the right registry
entries, stage 3 needs the *source* to have tagged its registry
entries with mkey-based instance_keys. So stage 3 adds, on top of
stage 2's source-side machinery:

* A new helper `vfmig_iova_tag_user_mr(dom, sgt, mkey)` called by
  mlx5_ib's `mlx5_ib_reg_user_mr` post-UMR completion: walks the
  sgt, looks up each registry entry by `sg_dma_address(sg)`, and
  overwrites `instance_key = ((u64)mkey << 32) | sg_idx`.
* Wire records emitted in stage 3 carry these mkey-based keys.

> **Note on the mkey choice.** Picking `mlx5_ib_mkey.key` as the
> wire-level identity for stage 3 is deliberate-but-revisable. It
> works because the FW preserves it across `SAVE/LOAD_VHCA_STATE`
> and `RESTORE_MR` naturally carries it. If a future generation of
> the wire protocol wants different identity (e.g. an explicit
> source-assigned u64 distinct from any FW-visible value), the
> stage 3 retag step is the single point of change -- both source
> and destination read whatever instance_key the source wrote.
> The `instance_key` field width (u64) is intentionally generous
> so alternative encodings fit. Stage 1 doesn't commit to anything
> (auto-numbered, never on the wire); stage 2 commits only to
> "auto-numbered cursor-position-as-identity" which doesn't
> conflict with stage 3 changing the encoding.

### 7.4 What if a single sgt-segment maps multiple pages and FW re-coalesces the PAS?

Won't matter at this level. We tag entries by `sg_dma_address`;
each `iommu_map` we did was per-sg-segment keyed by the segment's
chosen IOVA; FW's PAS coalescing happens above us at MKEY-write
time. Both source and destination see the same FW-mkey ->
per-segment-iova mapping, so `(mkey, sg_idx)` is unambiguous on
both sides.

## 8. DMA-buf compatibility (forward-looking)

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

## 9. Open questions

* **Stage 2 mkey retag timing**: validate the exact mlx5_ib hook
  point (post-UMR, in reg_user_mr's success path) and confirm
  ordering against any concurrent SAVE iterator. Stage 1 doesn't
  emit external entries on the wire so the ordering is moot for
  stage 1; stage 2 needs to nail this down before turning the
  iterator on.
* **`dev->dma_iommu` override visibility**: are there subsystems
  that cache `dev->dma_iommu` somewhere else? `iommu_dma_init_domain`
  initializes the iova_cookie; `iommu_setup_dma_ops` is the only
  writer. We're confident the override is safe but
  `attach -> dma_map -> detach -> dma_map against the default
  domain after detach` needs a concrete smoke cycle. Plan: a small
  test in stage 1 that toggles SET_TRACKED on/off across a single
  uverbs MR registration to verify both states behave correctly.
* **ODP** (`hmm_dma_map_pfn` not `dma_map_sgtable`): needs a
  separate hook; out of scope for stage 1, tracked as a follow-up.

(Stress test for "concurrent dma_map and domain destroy" -- deferred
to post-v2; nothing about user MRs changes the existing lifetime
invariant that requires the VF unbound at destroy.)

## 10. Source references

* `kernel/dma/mapping.c:230` -- `__dma_map_sg_attrs` dispatch order.
* `kernel/dma/mapping.c:120` -- `dma_go_direct` (use_dma_iommu check).
* `include/linux/dma-map-ops.h:77` -- `set_dma_ops`.
* `include/linux/iommu-dma.h:13` -- `use_dma_iommu` -> `dev->dma_iommu`.
* `drivers/iommu/dma-iommu.c:2107` -- `iommu_setup_dma_ops` is the
  only writer of `dev->dma_iommu`.
* `drivers/infiniband/core/umem.c:164` -- `ib_umem_get`.
* `drivers/infiniband/core/umem.c:260` -- `ib_dma_map_sgtable_attrs`
  call site inside `ib_umem_get`.
* `drivers/infiniband/hw/mlx5/mr.c:1578` -- `mlx5_ib_reg_user_mr`.
* `drivers/net/ethernet/mellanox/mlx5/core/vfmig_iova.c` -- the
  existing v1 allocator we're extending.
* `tools/testing/mlx5_vfmig/design/uar_restore.md` -- the verb-pattern
  template stage 3 follows.
