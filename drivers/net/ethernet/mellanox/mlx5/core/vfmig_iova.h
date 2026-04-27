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
 *   3. Provide a bump allocator (vfmig_iova_alloc_coherent) that the
 *      mlx5_core probe path uses *instead of* dma_alloc_coherent for
 *      every DMA buffer the firmware will record an IOVA for. Same
 *      sequence of calls on source and destination -> same IOVAs.
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
 * vfmig_iova_alloc_coherent will fail loudly rather than silently
 * stash the wrong IOVA in the firmware's view of the world. The
 * conversion lands incrementally per the layered restore plan
 * (cursor_plans/mlx5_vfmig_iova_layered_restore_*.plan.md):
 *   Layer 1: cmd ring;
 *   Layer 2: MANAGE_PAGES (boot pages, FW-driven page-give);
 *   Layer 3: EQs + UARs;
 *   Layer 4: user resources.
 *
 * Determinism contract (v1 shortcut -- READ THIS)
 * -----------------------------------------------
 * vfmig_iova_alloc_coherent() does NOT change allocation semantics
 * at the call site. It looks just like dma_alloc_coherent: caller
 * passes a size, gets back (iova, vaddr), and passes no identifier
 * for *which* allocation this is. The IOVA we hand back is whatever
 * the per-VF bump cursor currently points at.
 *
 * That means source/destination IOVA equivalence rests entirely on
 * caller discipline: the same converted call sites must execute in
 * the same order with the same sizes on both sides of the migration.
 * Any reorder, addition, removal, or size change between source and
 * destination probe paths drifts the cursor and silently misroutes
 * IOVAs -- the destination FW then dereferences a buffer at the
 * wrong address with no immediately-visible error.
 *
 * Why we accept this in v1: Layer 1 has exactly one converted call
 * site (the cmd ring); Layer 2 adds MANAGE_PAGES which are also
 * order-stable in practice. Building the proper structured store
 * (below) before validating the IOVA-preservation hypothesis on
 * Layer 1 risks scaffolding for an approach that may not work.
 *
 * The intended v2: every converted site gets a stable slot identity
 * (enum vfmig_iova_slot + an instance index). Allocator becomes
 * vfmig_iova_alloc_slot(dom, slot, instance, size, ...). IOVA is
 * derived from (slot, instance), and registry / wire records are
 * keyed by the same. Order, missing-on-one-side, and size mismatches
 * all become loud, specific errors. The bump cursor and
 * lookup-at-cursor logic in alloc_coherent go away.
 *
 * v2 is a hard-required follow-up before Layer 3, not a "nice to
 * have". See the "Architectural shortcut: order-based determinism"
 * section of the plan document for the full rationale and the
 * proposed slot enum sketch.
 *
 * The one defensive cross-check we keep at this site: a registry hit
 * at the cursor whose recorded length differs from the requested
 * size returns -EINVAL. That catches size drift but not order drift.
 */

#ifndef __MLX5_CORE_VFMIG_IOVA_H__
#define __MLX5_CORE_VFMIG_IOVA_H__

#include <linux/types.h>

struct mlx5_core_dev;
struct pci_dev;
struct vfmig_iova_domain;

#if IS_ENABLED(CONFIG_MLX5_VFMIG)

/*
 * IOVA window layout (host-virtual addresses the hardware sees).
 *
 *   VFMIG_IOVA_BASE        -- per-domain base. Picked so that
 *                             [BASE, BASE + N * PER_VF) sits
 *                             comfortably above the dma-iommu
 *                             allocator's reach (which on x86 tops
 *                             out around dma_get_required_mask(),
 *                             typically 32- or 39-bit) but well
 *                             *below* every IOMMU geometry aperture
 *                             we expect to encounter. The latter is
 *                             the load-bearing constraint:
 *                             iommu_paging_domain_alloc() returns a
 *                             domain whose geometry.aperture_end is
 *                             determined by the underlying IOMMU's
 *                             address-width (typically 48 bits on
 *                             Intel VT-d, 48 or 52 on AMD-Vi /
 *                             newer Intel with 5-level IOMMU paging),
 *                             and iommu_map() returns -ERANGE for
 *                             any IOVA outside it.
 *                             1 TiB (2^40) is a sweet spot: ~1000x
 *                             above any plausible required_mask and
 *                             ~256x below the smallest plausible
 *                             aperture. Validated at create time
 *                             against the actual geometry; if a
 *                             future platform reports a smaller
 *                             aperture, vfmig_iova_domain_create()
 *                             will fail cleanly with -EOPNOTSUPP.
 *                             VFs are individually addressed, so the
 *                             base is identical across VFs of the
 *                             same PF.
 *   VFMIG_IOVA_PER_VF      -- 4 GB of IOVA space per VF. Plenty of
 *                             room for cmd ring + MANAGE_PAGES + EQs
 *                             + UARs at typical sizes; we'll add
 *                             accounting if a real workload pushes
 *                             past this.
 *   VFMIG_IOVA_GRANULE     -- minimum allocation alignment. Matches
 *                             PAGE_SIZE; mlx5 hardware page size is
 *                             also 4 KB.
 */
#define VFMIG_IOVA_BASE		0x10000000000ULL	/* 1 TiB */
#define VFMIG_IOVA_PER_VF	0x100000000ULL		/* 4 GiB */
#define VFMIG_IOVA_GRANULE	PAGE_SIZE

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
 * Lookup-or-allocate a coherent DMA region in @dom at the next
 * deterministic IOVA position.
 *
 *   Source flow (no replay):  every call creates a fresh page,
 *                             registers it, iommu_maps it, returns
 *                             (iova, vaddr).
 *   Destination flow (post-replay): @dom's bump cursor has been
 *                             reset to BASE by replay; this call
 *                             finds the already-replayed entry at
 *                             that IOVA and returns its (iova,
 *                             vaddr) without allocating a new page.
 *
 * Either way the cursor advances by ALIGN(size, VFMIG_IOVA_GRANULE).
 *
 * v1 CALLER CONTRACT (see file-top "Determinism contract" block):
 * probe-time allocators MUST call this in the SAME ORDER and with
 * the SAME SIZES on source and destination, or IOVAs drift silently.
 * The API has no slot-id parameter on purpose: v1 ships the bump
 * cursor as-is so we can validate the IOVA-preservation hypothesis
 * (Layer 1 keystone) before investing in the structured slot store
 * v2 will introduce (vfmig_iova_alloc_slot). Treat the "no slot id"
 * as a temporary state, not a permanent design.
 *
 * Size is rounded up to PAGE_SIZE. @gfp is honoured for the page
 * allocation in the fresh-alloc case; on the replay-hit path no
 * allocation happens and @gfp is ignored.
 */
int  vfmig_iova_alloc_coherent(struct vfmig_iova_domain *dom,
			       size_t size, gfp_t gfp,
			       dma_addr_t *iova_out, void **vaddr_out);

/*
 * Free a previously-allocated coherent region. Unmaps from the
 * iommu_domain, frees the backing pages, removes from the registry.
 * The IOVA range is NOT reclaimed for re-use -- the bump cursor
 * never goes backwards. For the Layer 0/1 caller set (cmd ring,
 * MANAGE_PAGES) total volume is bounded; long-running migration
 * workloads that thrash allocations would leak fragmentation, which
 * we'll address if a real workload demands it.
 */
void vfmig_iova_free_coherent(struct vfmig_iova_domain *dom,
			      dma_addr_t iova, size_t size);

/*
 * Pre-populate a registry entry at @iova with @len bytes of
 * @contents. Allocates a backing page, copies @contents in,
 * iommu_maps the page at @iova in @dom, and inserts it into the
 * registry.
 *
 * Used by the LOAD path to rehydrate the destination's IOVA space
 * from HOST_PAGE wire records *before* LOAD_VHCA_STATE runs. The
 * subsequent vfmig_iova_alloc_coherent calls during VF probe will
 * find these entries via the cursor lookup and reuse them.
 *
 * @iova must be in this domain's window and PAGE_SIZE-aligned. @len
 * must be a multiple of PAGE_SIZE. Replaying twice at the same IOVA
 * is an error (returns -EEXIST).
 */
int  vfmig_iova_replay_page(struct vfmig_iova_domain *dom,
			    dma_addr_t iova, const void *contents,
			    size_t len);

/*
 * Reset the bump cursor to the domain's base IOVA. Called once on
 * the destination after all HOST_PAGE records have been replayed
 * but before VF probe starts: subsequent vfmig_iova_alloc_coherent
 * calls will then walk the registry from the bottom and find the
 * replayed entries.
 *
 * Idempotent. Safe to call on a domain that's never been allocated
 * from.
 */
void vfmig_iova_reset_cursor(struct vfmig_iova_domain *dom);

/*
 * Iterate the registry in IOVA-ascending order. @cb is invoked once
 * per entry with (iova, vaddr, len, ctx). Iteration order is the
 * order SAVE will emit HOST_PAGE records, which (for v1) is the
 * same order entries were created -- meaning the destination's
 * replay observes the source's allocation order, which is what we
 * need for cursor-lookup correctness.
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
static inline int vfmig_iova_alloc_coherent(struct vfmig_iova_domain *dom,
					    size_t size, gfp_t gfp,
					    dma_addr_t *iova_out,
					    void **vaddr_out)
{
	return -EOPNOTSUPP;
}
static inline void vfmig_iova_free_coherent(struct vfmig_iova_domain *dom,
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
