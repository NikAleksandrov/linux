/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * Per-VF deterministic IOVA allocator + page registry for mlx5_vfmig.
 *
 * Why
 * ---
 * SAVE_VHCA_STATE captures, in the firmware-side blob, every IOVA the
 * source VHCA was using -- cmd ring, host pages, EQ buffers, UAR
 * pages. On native (non-VFIO, non-VM) mlx5_core, the destination's
 * dma-iommu layer hands out fresh IOVAs that have nothing to do with
 * the source's, so post-LOAD the FW dereferences IOVAs that no longer
 * point to anything meaningful and silently times out every command.
 *
 * The fix that this module implements:
 *
 *   1. Per VF, give the kernel its own unmanaged paging iommu_domain
 *      (one we fully own; not the dma-iommu-managed one).
 *   2. Lay out a 4 GB IOVA window per VF at a high, well-known base
 *      (VFMIG_IOVA_BASE) so the source's IOVAs are reproducible.
 *   3. Provide a slot-tagged allocator (vfmig_iova_alloc_slot) that
 *      the mlx5_core probe path uses *instead of* dma_alloc_coherent
 *      for every DMA buffer the firmware will record an IOVA for. Each
 *      caller declares which kind of allocation it is (CMD_RING,
 *      FW_PAGE, DMA_COHERENT, ...) and gets a deterministic IOVA from
 *      that slot's dedicated sub-window.
 *   4. SAVE walks this domain's page registry, emits one HOST_PAGE
 *      wire record per entry. LOAD parses them and replays each entry
 *      via vfmig_iova_replay_page() *before* LOAD_VHCA_STATE runs;
 *      destination's probe then sees an already-mapped, already-filled
 *      page at the right IOVA when it goes to allocate the cmd ring,
 *      etc.
 *
 * Lifetime
 * --------
 * Domains are owned by struct mlx5_vfmig_pf via
 * sriov->vfs_ctx[vf_id].vfmig_iova_dom. Created lazily when userspace
 * issues SET_TRACKED { enable=1 } and the VF is currently unbound.
 * Destroyed by:
 *
 *   - SET_TRACKED { enable=0 } (also requires VF unbound),
 *   - mlx5_vfmig_pf_cleanup() on the owning PF unbinding,
 *   - mlx5_device_disable_sriov() when sriov_numvfs goes to 0.
 *
 * The domain explicitly *survives* a VF unbind+rebind cycle, and
 * survives a SAVE -> destroy-VF -> recreate-VF -> LOAD round trip on
 * the same PF: that's the modal use case.
 *
 * Coexistence with dma-iommu
 * --------------------------
 * Attaching an IOMMU_DOMAIN_UNMANAGED to a device displaces its
 * dma-iommu-managed default DMA domain. While our domain is attached,
 * dma_alloc_coherent on this device WILL FAIL -- the dma-iommu
 * fast-path expects an IOMMU_DOMAIN_DMA. That's intentional: any
 * mlx5_core probe-time allocation that hasn't been converted to
 * vfmig_iova_alloc_slot will fail loudly rather than silently stash
 * the wrong IOVA in the firmware's view of the world. The conversion
 * lands incrementally per the layered restore plan
 * (cursor_plans/mlx5_vfmig_iova_layered_restore_*.plan.md):
 *   Layer 1: cmd ring;
 *   Layer 2: MANAGE_PAGES (boot pages, FW-driven page-give);
 *   Layer 3: EQs + UARs;
 *   Layer 4: user resources.
 *
 * Determinism contract
 * --------------------
 * Every converted call site declares which slot its allocation
 * belongs to (enum vfmig_iova_slot). Each slot owns its own
 * VFMIG_IOVA_SLOT_BYTES sub-window of the per-VF IOVA range and its
 * own bump cursor. Adding, removing, or reordering allocations in one
 * slot CANNOT shift IOVAs in another slot -- the per-slot windows are
 * a fixed partition.
 *
 * Within a single slot, source/destination IOVA equivalence still
 * rests on call-site discipline: same set of allocations of the same
 * sizes in the same order. The instance_key argument lets the caller
 * pin a specific (slot, key) -> IOVA mapping when it has a stable
 * identifier (e.g. a firmware-assigned handle); passing 0 falls back
 * to per-slot auto-numbering, which is the order-based shortcut
 * scoped to a single slot.
 *
 * What this v2 still doesn't do: it doesn't notice when src and dst
 * disagree about the *number* of allocations in a slot. The per-slot
 * cursor on the destination just keeps bumping into fresh IOVAs that
 * have no SAVE-side counterpart, and the FW dereferences something
 * that was never set up. The next patch in this series adds an
 * on-wire manifest of (slot, instance_key, len) tuples and a LOAD-
 * time cross-check that surfaces those drifts as a hard, named error
 * before any FW dereference happens.
 */

#ifndef __MLX5_CORE_VFMIG_IOVA_H__
#define __MLX5_CORE_VFMIG_IOVA_H__

#include <linux/types.h>

struct mlx5_core_dev;
struct pci_dev;
struct vfmig_iova_domain;

/*
 * Slot identity. Each value names one *category* of converted
 * allocation; the (slot, instance_key) pair plus size identifies a
 * specific allocation within that category. Defined here (outside the
 * CONFIG_MLX5_VFMIG block below) so both the real declarations and
 * the !CONFIG inline stubs can reference the type.
 *
 * Adding new values:
 *   - Always append before VFMIG_SLOT_NR. NEVER renumber existing
 *     enumerators -- the numeric value is the slot's IOVA base
 *     offset within the per-VF window, and stable IOVAs are the
 *     point of this whole subsystem.
 *   - Stay below VFMIG_IOVA_NR_SLOTS. Bumping NR_SLOTS itself
 *     repartitions every existing slot's window and is a wire-
 *     incompatible change.
 *
 * Slot semantics:
 *   VFMIG_SLOT_INVALID    -- sentinel. No call site should ever pass
 *                            this; alloc_slot rejects it with
 *                            -EINVAL.
 *   VFMIG_SLOT_CMD_RING   -- cmd ring DMA buffer. Singleton per VF
 *                            (one ring), allocated by mlx5_cmd_enable
 *                            during probe.
 *   VFMIG_SLOT_FW_PAGE    -- firmware-owned page backing for
 *                            MANAGE_PAGES. Many allocations per VF
 *                            (one per page the FW asks for); the
 *                            sequence is deterministic per FW
 *                            version + capability set.
 *   VFMIG_SLOT_DMA_COHERENT -- catch-all for the legacy
 *                              mlx5_dma_zalloc_coherent_node call
 *                              site (EQ buffers, UAR/DB pages, etc.)
 *                              The "DMA_COHERENT" name reflects what
 *                              this slot used to be in v1 -- a
 *                              renamed alloc_coherent. Future
 *                              revisions may split it into per-
 *                              consumer slots (EQ_BUF, UAR_PAGE,
 *                              ...); each split is an additive enum
 *                              change above (new tail enumerators).
 */
enum vfmig_iova_slot {
	VFMIG_SLOT_INVALID	= 0,
	VFMIG_SLOT_CMD_RING	= 1,
	VFMIG_SLOT_FW_PAGE	= 2,
	VFMIG_SLOT_DMA_COHERENT	= 3,
	VFMIG_SLOT_NR,	/* count, must stay <= VFMIG_IOVA_NR_SLOTS */
};

#if IS_ENABLED(CONFIG_MLX5_VFMIG)

/*
 * IOVA window layout (host-virtual addresses the hardware sees).
 *
 *   VFMIG_IOVA_BASE        -- per-PF base. The window
 *                             [BASE, BASE + N * PER_VF) must fit
 *                             entirely inside the IOMMU's geometry
 *                             aperture: iommu_paging_domain_alloc()
 *                             returns a domain whose
 *                             geometry.aperture_end is set by the
 *                             underlying hardware's address width,
 *                             and iommu_map() returns -ERANGE for
 *                             any IOVA outside that range. Real-
 *                             world apertures observed:
 *                                Intel VT-d agaw=2  -> 39-bit
 *                                                     ([0, 0x7fffffffff])
 *                                Intel VT-d agaw=3  -> 48-bit
 *                                AMD-Vi             -> 48 or 52-bit
 *                             We pick the floor of those (39 bits)
 *                             as the binding constraint.
 *
 *                             4 GiB (2^32) is the chosen base:
 *                                - safely above any 32-bit-only
 *                                  device's dma_mask range, which
 *                                  doesn't actually matter because
 *                                  our unmanaged domain *replaces*
 *                                  the default DMA domain on attach
 *                                  (there is no co-tenancy), but
 *                                  keeps IOVAs visually distinct
 *                                  from anything the default
 *                                  allocator would have produced;
 *                                - leaves IOVA 0..4 GiB free for any
 *                                  "sentinel zero" or low-address
 *                                  semantics future code might want;
 *                                - lets us pack ~126 VFs of 4 GiB
 *                                  each before brushing the 39-bit
 *                                  aperture ceiling, comfortably
 *                                  more than any single-PF VF count
 *                                  we plan to test.
 *
 *                             VFs of the same PF get distinct sub-
 *                             windows (BASE + vf_id * PER_VF) so
 *                             that an IOVA value alone identifies
 *                             which VF it belongs to in dmesg.
 *
 *                             vfmig_iova_domain_create() validates
 *                             the chosen window against the live
 *                             aperture and fails the SET_TRACKED
 *                             ioctl with -EOPNOTSUPP (and a printed
 *                             diagnostic) if a future platform
 *                             reports something even tighter than
 *                             39 bits.
 *   VFMIG_IOVA_PER_VF      -- 4 GB of IOVA space per VF. Plenty of
 *                             room for cmd ring + MANAGE_PAGES + EQs
 *                             + UARs at typical sizes; we'll add
 *                             accounting if a real workload pushes
 *                             past this.
 *   VFMIG_IOVA_GRANULE     -- minimum allocation alignment. Matches
 *                             PAGE_SIZE; mlx5 hardware page size is
 *                             also 4 KB.
 */
#define VFMIG_IOVA_BASE		0x100000000ULL		/* 4 GiB */
#define VFMIG_IOVA_PER_VF	0x100000000ULL		/* 4 GiB */
#define VFMIG_IOVA_GRANULE	PAGE_SIZE

/*
 * Transient sub-window: the topmost slice of each VF's IOVA window,
 * reserved for vfmig_iova_transient_get/put (see below). Sized to hold
 * worst-case in-flight transient allocations -- today only the cmd
 * mailbox cache, whose ceiling is ~3900 pages. 16 MiB = 4096 pages
 * leaves headroom for additional future transient call sites.
 *
 * The deterministic part of the per-VF window (everything below this
 * sub-window) is partitioned across VFMIG_IOVA_NR_SLOTS slot windows;
 * see VFMIG_IOVA_SLOT_BYTES below. An alloc_slot attempt that would
 * grow into the transient range (i.e. past slot N-1's end) returns
 * -ENOSPC.
 */
#define VFMIG_IOVA_TRANSIENT_BYTES	(16ULL << 20)	/* 16 MiB */

/*
 * Per-VF slot fan-out for the deterministic allocator.
 *
 * The per-VF deterministic range (VFMIG_IOVA_PER_VF -
 * VFMIG_IOVA_TRANSIENT_BYTES bytes, == 4080 MiB) is split into
 * VFMIG_IOVA_NR_SLOTS equal sub-windows of VFMIG_IOVA_SLOT_BYTES
 * (== 510 MiB) each. Slot N occupies
 *   [base + N * SLOT_BYTES, base + (N+1) * SLOT_BYTES).
 * Slot 0 is reserved for VFMIG_SLOT_INVALID -- its IOVA range is
 * never allocated from. Real allocations come from slots 1..NR-1.
 *
 * Sized for headroom rather than measured worst case: 510 MiB / slot
 * is wildly more than any current call site needs (FW_PAGE peaks at
 * ~32 MiB on a fully-used VF). The headroom is cheap because slot
 * sub-windows consume IOVA space, not physical memory; a slot with
 * one allocation costs one mapped page just like before.
 *
 * The 8-slot fan-out is a deliberate over-provision: it lets us add
 * up to 4 more named slots in future revisions without renumbering
 * existing slots. Renumbering would change the IOVA bases of
 * already-deployed slots -- breaking SAVE/LOAD compatibility. New
 * slots get appended at the next free index; existing slots' bases
 * stay put.
 */
#define VFMIG_IOVA_NR_SLOTS		8U
#define VFMIG_IOVA_SLOT_BYTES \
	((VFMIG_IOVA_PER_VF - VFMIG_IOVA_TRANSIENT_BYTES) / VFMIG_IOVA_NR_SLOTS)

/*
 * (Slot identity is enum vfmig_iova_slot, defined outside the
 *  #if IS_ENABLED(CONFIG_MLX5_VFMIG) block above so the !CONFIG
 *  inline stubs can reference it in their function signatures.)
 */

/*
 * Allocate + attach a per-VF unmanaged paging domain.
 *
 * @vf_pdev:	the VF's pci_dev. Must currently be unbound; caller
 *		holds device_lock(&vf_pdev->dev).
 * @vf_id:	0-based vf index, used to derive the IOVA base for
 *		this VF (BASE + vf_id * PER_VF).
 * @out:	on success, *out is the new domain handle. Caller
 *		stashes it in vfs_ctx[vf_id].vfmig_iova_dom.
 *
 * Returns 0 on success. On failure no domain is created and the
 * device is left attached to whatever default DMA domain it had.
 *
 * Typical errors:
 *   -ENOMEM      kmalloc / iommu_paging_domain_alloc / page tables
 *   -EOPNOTSUPP  IOMMU not present for this device, or the IOMMU
 *                driver doesn't support unmanaged paging domains
 *   -EBUSY       device already has an unmanaged domain attached
 *                (someone else is squatting on the address space)
 */
int  vfmig_iova_domain_create(struct pci_dev *vf_pdev, u32 vf_id,
			      struct vfmig_iova_domain **out);

/*
 * Detach + free a domain previously created with
 * vfmig_iova_domain_create(). Unmaps and frees every page in the
 * registry, detaches from the VF, releases the iommu_domain, and
 * drops the pci_dev reference taken at create time.
 *
 * Caller must guarantee the VF is currently unbound (driver=NULL).
 * Safe with @dom == NULL.
 */
void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom);

/*
 * Lookup-or-allocate a deterministic DMA-coherent region in @dom from
 * the IOVA sub-window owned by @slot.
 *
 *   Source flow (no replay):    creates a fresh page, registers it
 *                               with (slot, instance_key) tagging,
 *                               iommu_maps it, returns (iova, vaddr).
 *   Destination flow (post-replay): @dom's per-slot cursor has been
 *                               reset to that slot's base by
 *                               vfmig_iova_reset_cursor(); this call
 *                               finds the already-replayed entry at
 *                               that IOVA and returns its (iova,
 *                               vaddr) without allocating a new page.
 *
 * Either way @slot's per-slot cursor advances by ALIGN(size,
 * VFMIG_IOVA_GRANULE).
 *
 * Determinism contract:
 *   - @slot identifies the IOVA sub-window. Different slots have
 *     disjoint IOVA ranges; growth in one slot can never shift
 *     another slot's IOVAs.
 *   - @instance_key disambiguates multiple allocations within the
 *     same slot. Two modes:
 *       instance_key == 0  -> per-slot auto-numbering. The allocator
 *                             assigns the next sequence number from a
 *                             per-slot counter. Determinism within a
 *                             slot then rests on call-site discipline:
 *                             same set of allocations of the same
 *                             sizes in the same order. Suitable for
 *                             singletons (CMD_RING) and for stable-
 *                             order multi-instance call sites
 *                             (FW_PAGE, today's DMA_COHERENT pool).
 *       instance_key != 0  -> caller-pinned. Reserved for future
 *                             call sites (mlx5_ib resources) where
 *                             firmware/orchestrator hands the kernel
 *                             a stable identifier. The allocator
 *                             records the key on the registry entry
 *                             but does NOT yet enforce uniqueness or
 *                             use it for IOVA derivation -- the
 *                             upcoming on-wire manifest patch is the
 *                             first consumer that will use the key
 *                             for correctness, and that patch will
 *                             also add the collision check.
 *   - @size is rounded up to PAGE_SIZE.
 *
 * Slot validation:
 *   - @slot in (VFMIG_SLOT_INVALID, VFMIG_SLOT_NR), else -EINVAL.
 *
 * @gfp MUST NOT include __GFP_HIGHMEM/COMP/DMA/DMA32. The first
 * because page_address() must be valid on the backing page (used by
 * the SAVE/replay paths); the others because iommu_map() rejects
 * them outright. Pass GFP_KERNEL or GFP_ATOMIC. Violations return
 * -EINVAL with a ratelimited dev_warn.
 *
 * Other returns:
 *   -ENOSPC   per-slot window exhausted
 *   -ERANGE   computed IOVA outside the slot's window (kernel bug)
 */
int  vfmig_iova_alloc_slot(struct vfmig_iova_domain *dom,
			   enum vfmig_iova_slot slot, u64 instance_key,
			   size_t size, gfp_t gfp,
			   dma_addr_t *iova_out, void **vaddr_out);

/*
 * Free a previously-allocated slot region. Unmaps from the
 * iommu_domain, frees the backing pages, removes from the registry.
 * The IOVA range is NOT reclaimed for re-use -- per-slot bump cursors
 * never go backwards. For the current caller set total volume is
 * bounded; long-running migration workloads that thrash allocations
 * would leak fragmentation, which we'll address if a real workload
 * demands it.
 *
 * @slot is what the caller passed to vfmig_iova_alloc_slot(); we
 * cross-check it against the recorded slot tag and WARN_ON_ONCE on
 * mismatch (caller bug; the free still proceeds).
 */
void vfmig_iova_free_slot(struct vfmig_iova_domain *dom,
			  enum vfmig_iova_slot slot,
			  dma_addr_t iova, size_t size);

/*
 * Pre-populate a registry entry at @iova with @len bytes of
 * @contents. Allocates a backing page, copies @contents in,
 * iommu_maps the page at @iova in @dom, and inserts it into the
 * registry.
 *
 * Used by the LOAD path to rehydrate the destination's IOVA space
 * from HOST_PAGE wire records *before* LOAD_VHCA_STATE runs. The
 * subsequent vfmig_iova_alloc_slot calls during VF probe will find
 * these entries via the per-slot cursor lookup and reuse them.
 *
 * Slot derivation: the v1-compatible HOST_PAGE wire record carries
 * (iova, len, contents) with no slot tag, so we infer the slot from
 * @iova by which sub-window it falls into. instance_key on the
 * replayed entry is set to 0 (placeholder); the destination's
 * subsequent alloc_slot call re-tags it with the real key when it
 * claims the entry. The next patch in the series adds an explicit
 * slot+instance_key on the wire and removes the inference.
 *
 * @iova must be in this domain's deterministic window and
 * PAGE_SIZE-aligned. @len must be a multiple of PAGE_SIZE. Replaying
 * twice at the same IOVA is an error (returns -EEXIST).
 */
int  vfmig_iova_replay_page(struct vfmig_iova_domain *dom,
			    dma_addr_t iova, const void *contents,
			    size_t len);

/*
 * Acquire one DMA-coherent region from the per-VF domain's pre-mapped
 * transient arena.
 *
 * "Transient" means: lives at most across one firmware command, never
 * recorded in the SAVE manifest, IOVA NOT stable across migration.
 * Use only for buffers the firmware dereferences in-flight and never
 * retains a reference to past command completion (today: cmd.c
 * mailbox indirection pages).
 *
 * @size is rounded up to PAGE_SIZE. Sizes above PAGE_SIZE are not
 * supported in this revision and return -EINVAL with a ratelimited
 * dev_warn -- the only current caller is cmd mailboxes which are
 * exactly PAGE_SIZE. Multi-size-class support can be added later
 * without changing the public API.
 *
 * Behaviour:
 *   - Hot path:   pop a page off the arena's freelist; no iommu_map,
 *                 no alloc_pages.
 *   - Slow path:  the freelist is empty AND the arena hasn't reached
 *                 its ceiling (VFMIG_IOVA_TRANSIENT_BYTES). Allocate
 *                 a fresh page, iommu_map it at the next arena IOVA,
 *                 hand it back. @gfp is honoured for the page
 *                 allocation here.
 *   - Failure:    arena at its ceiling AND freelist empty -> -ENOMEM.
 *
 * @gfp constraints match vfmig_iova_alloc_slot: must NOT include
 * __GFP_HIGHMEM/COMP/DMA/DMA32. Pass GFP_KERNEL or GFP_ATOMIC.
 *
 * Page contents are NOT zeroed; callers that need zeroing do it
 * themselves.
 */
int  vfmig_iova_transient_get(struct vfmig_iova_domain *dom,
			      size_t size, gfp_t gfp,
			      void **vaddr_out, dma_addr_t *iova_out);

/*
 * Return a region previously obtained from vfmig_iova_transient_get()
 * to the freelist. @size MUST match the size passed to _get().
 *
 * Safe with @dom == NULL (no-op). An @iova outside the arena's
 * sub-window is treated as a caller bug: WARN and ignore (so the
 * arena's accounting can't be corrupted by a stray free).
 */
void vfmig_iova_transient_put(struct vfmig_iova_domain *dom,
			      dma_addr_t iova, size_t size);

/*
 * Reset every per-slot bump cursor to its slot's base IOVA, and
 * reset every per-slot auto-key counter to 0. Called once on the
 * destination after all HOST_PAGE records have been replayed but
 * before VF probe starts: subsequent vfmig_iova_alloc_slot calls
 * will then walk each slot from its bottom and find the replayed
 * entries.
 *
 * Idempotent. Safe to call on a domain that's never been allocated
 * from.
 */
void vfmig_iova_reset_cursor(struct vfmig_iova_domain *dom);

/*
 * Iterate the registry in IOVA-ascending order. @cb is invoked once
 * per entry with (iova, vaddr, len, ctx). Iteration order is
 * IOVA-sorted across all slots; SAVE uses this order to emit
 * HOST_PAGE records and the destination replays them in the same
 * order, which is what the per-slot cursor-lookup correctness rests
 * on (replayed entries land at exactly the IOVAs the destination's
 * subsequent alloc_slot calls will request).
 *
 * @cb may not modify the registry (no alloc/free/replay calls).
 * Returning a non-zero value from @cb stops iteration and is
 * propagated as the return value.
 */
typedef int (*vfmig_iova_for_each_fn)(dma_addr_t iova, const void *vaddr,
				      size_t len, void *ctx);
int  vfmig_iova_for_each(struct vfmig_iova_domain *dom,
			 vfmig_iova_for_each_fn cb, void *ctx);

#else /* !CONFIG_MLX5_VFMIG */

/*
 * Stubs so callers (vfmig.c, future cmd.c hook) keep compiling
 * cleanly with vfmig disabled. Semantically: no domain ever exists,
 * every API call fails fast with -EOPNOTSUPP. The on-the-wire
 * SET_TRACKED ioctl is itself compiled out, so no caller can
 * actually reach these stubs in a CONFIG_MLX5_VFMIG=n build.
 */
static inline int vfmig_iova_domain_create(struct pci_dev *vf_pdev, u32 vf_id,
					   struct vfmig_iova_domain **out)
{
	return -EOPNOTSUPP;
}
static inline void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom) { }
static inline int vfmig_iova_alloc_slot(struct vfmig_iova_domain *dom,
					enum vfmig_iova_slot slot,
					u64 instance_key,
					size_t size, gfp_t gfp,
					dma_addr_t *iova_out,
					void **vaddr_out)
{
	return -EOPNOTSUPP;
}
static inline void vfmig_iova_free_slot(struct vfmig_iova_domain *dom,
					enum vfmig_iova_slot slot,
					dma_addr_t iova, size_t size) { }
static inline int vfmig_iova_transient_get(struct vfmig_iova_domain *dom,
					   size_t size, gfp_t gfp,
					   void **vaddr_out,
					   dma_addr_t *iova_out)
{
	return -EOPNOTSUPP;
}
static inline void vfmig_iova_transient_put(struct vfmig_iova_domain *dom,
					    dma_addr_t iova, size_t size) { }
static inline int vfmig_iova_replay_page(struct vfmig_iova_domain *dom,
					 dma_addr_t iova, const void *contents,
					 size_t len)
{
	return -EOPNOTSUPP;
}
static inline void vfmig_iova_reset_cursor(struct vfmig_iova_domain *dom) { }

#endif /* CONFIG_MLX5_VFMIG */

#endif /* __MLX5_CORE_VFMIG_IOVA_H__ */
