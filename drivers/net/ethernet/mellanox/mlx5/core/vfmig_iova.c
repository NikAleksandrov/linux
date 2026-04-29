// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
// Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

/*
 * vfmig_iova: per-VF deterministic IOVA allocator + page registry.
 *
 * See vfmig_iova.h for the high-level rationale. This file is the
 * implementation. All entry points lock dom->lock; the iommu_domain
 * itself is reentrant under iommu_map / iommu_unmap, so we don't need
 * to serialize the underlying iommu API calls beyond what dom->lock
 * gives us.
 */

#include <linux/device.h>
#include <linux/gfp.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "mlx5_core.h"
#include "vfmig_iova.h"

/*
 * One backing page (or higher-order compound page) registered in a
 * domain. iova/len identify the IOMMU mapping; page/vaddr are the
 * host-side handles. Length is always a multiple of PAGE_SIZE.
 *
 * @slot tags which IOVA sub-window the entry belongs to. On the SAVE
 * side it's set by alloc_slot() at create time. On the LOAD side
 * vfmig_iova_replay_page() takes the slot directly as a parameter
 * (sourced from the HOST_PAGE wire record) and the IOVA is
 * cross-checked against the destination's slot partitioning so any
 * source/destination disagreement about NR_SLOTS / SLOT_BYTES /
 * PER_VF surfaces as -ERANGE before we install anything.
 *
 * @instance_key is the per-slot identifier described in the
 * vfmig_iova.h header doc. SAVE emits the source's value on the
 * wire and replay_page restores it verbatim; the destination's
 * subsequent alloc_slot() call must pass the same key (or 0 to
 * accept the per-slot auto-numbering, which lines up with the
 * source's replayed sequence by construction).
 */
struct vfmig_iova_page {
	struct list_head node;	/* dom->pages, sorted by iova ascending */
	u64		 iova;
	size_t		 len;
	struct page	*page;
	void		*vaddr;
	enum vfmig_iova_slot slot;
	u64		 instance_key;
};

/*
 * Maximum number of pages the per-VF transient arena can grow to.
 * Sized from VFMIG_IOVA_TRANSIENT_BYTES; fixed at compile time so the
 * arena's by-index slot table can be a flat array (32 KiB at 4096
 * entries x 8 B / pointer).
 */
#define VFMIG_IOVA_TRANSIENT_MAX_PAGES \
	(VFMIG_IOVA_TRANSIENT_BYTES / PAGE_SIZE)

/*
 * One backing page in the transient arena. Lives in one of two
 * states: on dom->transient.free (free list, available for the next
 * vfmig_iova_transient_get) or off the list with arena->slots[idx]
 * still pointing at it (currently handed out to a caller).
 *
 * The IOMMU mapping is set up exactly once when the page is first
 * grown into the arena; vfmig_iova_transient_get/put never call
 * iommu_map / iommu_unmap on the hot path. Pages are only unmapped
 * at vfmig_iova_domain_destroy() time.
 */
struct vfmig_transient_page {
	struct list_head free_node;	/* on arena->free when free */
	u64		 iova;
	void		*vaddr;
	struct page	*page;
};

/*
 * Per-domain transient arena. Lives in the top sub-window of the
 * per-VF IOVA range, [arena_base, arena_end), where
 * arena_end == dom->base + VFMIG_IOVA_PER_VF.
 *
 * Lazily populated: pages are mapped from the cursor on the first
 * _get() that finds the freelist empty, up to a hard ceiling of
 * VFMIG_IOVA_TRANSIENT_MAX_PAGES. Once mapped, pages stay mapped for
 * the lifetime of the domain and are recycled via the freelist.
 *
 * Protected by dom->lock; no separate lock for the arena since the
 * hot path is short and serialized contention is rare in practice.
 */
struct vfmig_transient_arena {
	u64		 base;
	u64		 end;
	u64		 cursor;	/* next IOVA to map on grow */
	struct list_head free;		/* of vfmig_transient_page */
	struct vfmig_transient_page **slots;	/* by-index lookup */
	unsigned int	 n_mapped;	/* total pages currently mapped */
	unsigned int	 n_free;	/* len of @free, for diagnostics */
	unsigned int	 max_pages;	/* arena ceiling, in pages */
};

struct vfmig_iova_domain {
	struct iommu_domain *iommu_dom;
	struct pci_dev	    *vf_pdev;	/* held via pci_dev_get() */
	u32		     vf_id;

	/*
	 * Per-VF IOVA window. The full hardware-visible range is
	 * [base, base + VFMIG_IOVA_PER_VF). The first
	 *   VFMIG_IOVA_NR_SLOTS * VFMIG_IOVA_SLOT_BYTES
	 * == VFMIG_IOVA_PER_VF - VFMIG_IOVA_TRANSIENT_BYTES bytes are the
	 * deterministic range, partitioned across the slot windows; the
	 * remaining top TRANSIENT_BYTES belong to the transient arena.
	 *
	 * Slot N's window is
	 *   [base + N * SLOT_BYTES, base + (N+1) * SLOT_BYTES).
	 * Slot 0 (VFMIG_SLOT_INVALID) is reserved and never allocated
	 * from. alloc_slot honours each slot's window and returns
	 * -ENOSPC at the slot boundary.
	 */
	u64		     base;

	struct mutex	     lock;
	/*
	 * Per-slot bump cursor. cursor[N] is the next fresh IOVA in
	 * slot N's window, valid in [slot_base(N), slot_base(N+1)).
	 * Initialized to slot_base(N) at domain create. Reset back to
	 * slot_base(N) by vfmig_iova_reset_cursor() after replay.
	 */
	u64		     cursor[VFMIG_IOVA_NR_SLOTS];
	/*
	 * Per-slot auto-key counter. Incremented on each alloc_slot
	 * call that passes instance_key == 0. Reset to 0 by
	 * vfmig_iova_reset_cursor() so the destination's claim sequence
	 * matches the source's. Skipped when the caller passes a non-
	 * zero (caller-pinned) key.
	 */
	u64		     next_auto_key[VFMIG_IOVA_NR_SLOTS];

	struct list_head     pages;	/* of vfmig_iova_page, sorted */
	unsigned int	     n_pages;

	struct vfmig_transient_arena transient;

	/*
	 * At-probe drift detection.
	 *
	 * vfmig_iova_arm_drift_detection() flips @drift_armed once
	 * the LOAD path has finished replaying every promised
	 * HOST_PAGE record AND verified the wire manifest CRC32. From
	 * that point onward, vfmig_iova_alloc_slot enforces:
	 *
	 *   - HIT (cursor lookup found a replayed entry): the
	 *     caller's instance_key (or the auto-bumped key, if the
	 *     caller passed 0) must equal the replayed entry's key.
	 *     A mismatch means the destination's pinned-vs-auto
	 *     allocation sequence in this slot has drifted from the
	 *     source's.
	 *
	 *   - MISS (cursor walked past every replayed entry in this
	 *     slot, would normally fall through to a fresh alloc): if
	 *     the slot ever had any replays (expected_count > 0), the
	 *     destination is asking for an alloc the source didn't
	 *     have at SAVE time -- a kernel-side change has added an
	 *     allocation in this slot.
	 *
	 * Both fail fast with -EPROTO. dev_err() prints the offending
	 * (slot, key, IOVA, size) tuple. dump_stack() is fired once
	 * per domain via @drift_reported so the first offending call
	 * site is identifiable in dmesg without a stack-trace flood
	 * if probe-time alloc_slot churn keeps tripping the same
	 * mismatch.
	 *
	 * @expected_count[s] is the number of HOST_PAGE records we
	 * replayed into slot s; bumped by vfmig_iova_replay_page.
	 * @alloc_count[s] is the number of alloc_slot HITs (and, for
	 * accounting only, MISSes) processed since the last
	 * vfmig_iova_reset_cursor.
	 *
	 * If @drift_armed is false (legacy / fresh-VF / SET_TRACKED-
	 * but-no-LOAD path), no checks fire and alloc_slot has the
	 * same behaviour as before this patch.
	 */
	bool		     drift_armed;
	bool		     drift_reported;
	u32		     expected_count[VFMIG_IOVA_NR_SLOTS];
	u32		     alloc_count[VFMIG_IOVA_NR_SLOTS];
};

static inline u64
vfmig_iova_slot_base(const struct vfmig_iova_domain *dom,
		     enum vfmig_iova_slot slot)
{
	return dom->base + (u64)slot * VFMIG_IOVA_SLOT_BYTES;
}

static inline u64
vfmig_iova_slot_end(const struct vfmig_iova_domain *dom,
		    enum vfmig_iova_slot slot)
{
	return dom->base + (u64)(slot + 1) * VFMIG_IOVA_SLOT_BYTES;
}

/*
 * Inverse of vfmig_iova_slot_base(): which slot does @iova fall into,
 * or VFMIG_SLOT_INVALID if it's outside any deterministic slot
 * window. The deterministic range is exactly
 * [base, base + NR_SLOTS * SLOT_BYTES); anything above belongs to the
 * transient arena (or is out of range entirely).
 */
static enum vfmig_iova_slot
vfmig_iova_slot_from_iova(const struct vfmig_iova_domain *dom, u64 iova)
{
	u64 off, idx;

	if (iova < dom->base)
		return VFMIG_SLOT_INVALID;
	off = iova - dom->base;
	idx = off / VFMIG_IOVA_SLOT_BYTES;
	if (idx >= VFMIG_IOVA_NR_SLOTS)
		return VFMIG_SLOT_INVALID;
	return (enum vfmig_iova_slot)idx;
}

/* dom->lock held. Returns the entry mapped at exactly @iova, or NULL. */
static struct vfmig_iova_page *
vfmig_iova_find_locked(struct vfmig_iova_domain *dom, u64 iova)
{
	struct vfmig_iova_page *p;

	list_for_each_entry(p, &dom->pages, node) {
		if (p->iova == iova)
			return p;
		if (p->iova > iova)
			return NULL;	/* sorted: gone past it */
	}
	return NULL;
}

/* dom->lock held. Inserts @new keyed by iova; sorted ascending. */
static void
vfmig_iova_insert_locked(struct vfmig_iova_domain *dom,
			 struct vfmig_iova_page *new)
{
	struct vfmig_iova_page *p;

	list_for_each_entry(p, &dom->pages, node) {
		if (p->iova > new->iova) {
			list_add_tail(&new->node, &p->node);
			dom->n_pages++;
			return;
		}
	}
	list_add_tail(&new->node, &dom->pages);
	dom->n_pages++;
}

/*
 * dom->lock held. Allocate a backing page or higher-order compound,
 * iommu_map it at @iova for @len bytes, and append the registry
 * entry. Does NOT advance the cursor; callers do that themselves
 * because the meaning of "advance" differs between alloc_slot and
 * replay.
 *
 * @slot/@instance_key are stamped on the new entry. @slot is also
 * used to validate that @iova falls inside that slot's window
 * ([slot_base, slot_end)); a callsite passing the wrong slot for an
 * IOVA returns -ERANGE.
 *
 * Returns 0 with *out_p set on success, negative errno otherwise.
 */
static int
vfmig_iova_install_page_locked(struct vfmig_iova_domain *dom,
			       enum vfmig_iova_slot slot, u64 instance_key,
			       u64 iova, size_t len, gfp_t gfp,
			       struct vfmig_iova_page **out_p)
{
	struct vfmig_iova_page *p;
	unsigned int order;
	int err;

	if (!IS_ALIGNED(iova, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(len, VFMIG_IOVA_GRANULE) ||
	    len == 0)
		return -EINVAL;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR)
		return -EINVAL;
	if (iova < vfmig_iova_slot_base(dom, slot) ||
	    iova + len > vfmig_iova_slot_end(dom, slot))
		return -ERANGE;
	if (vfmig_iova_find_locked(dom, iova))
		return -EEXIST;

	/*
	 * iommu_map() rejects __GFP_HIGHMEM/COMP/DMA/DMA32 with WARN +
	 * -EINVAL, and we additionally need page_address() to work on
	 * the backing page (used on the SAVE/replay paths for memcpy).
	 * Reject the offending flags here with a clear errno so callers
	 * don't get a stack-trace-shaped surprise from the iommu layer.
	 */
	if (gfp & (__GFP_COMP | __GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM)) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: install_page: rejected gfp 0x%x (must not include __GFP_HIGHMEM/COMP/DMA/DMA32)\n",
				     gfp);
		return -EINVAL;
	}

	p = kzalloc(sizeof(*p), gfp);
	if (!p)
		return -ENOMEM;

	order = get_order(len);
	p->page = alloc_pages(gfp | __GFP_ZERO, order);
	if (!p->page) {
		err = -ENOMEM;
		goto err_free_p;
	}
	p->vaddr	= page_address(p->page);
	p->iova		= iova;
	p->len		= len;
	p->slot		= slot;
	p->instance_key	= instance_key;

	err = iommu_map(dom->iommu_dom, iova, page_to_phys(p->page), len,
			IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, gfp);
	if (err)
		goto err_free_page;

	vfmig_iova_insert_locked(dom, p);
	*out_p = p;
	return 0;

err_free_page:
	__free_pages(p->page, order);
err_free_p:
	kfree(p);
	return err;
}

/*
 * dom->lock held. Tear down a single registry entry: iommu_unmap,
 * release backing pages, free the bookkeeping struct. List unlink is
 * caller's responsibility (so we can be called from list iteration).
 */
static void
vfmig_iova_destroy_page_locked(struct vfmig_iova_domain *dom,
			       struct vfmig_iova_page *p)
{
	(void)iommu_unmap(dom->iommu_dom, p->iova, p->len);
	__free_pages(p->page, get_order(p->len));
	kfree(p);
}

/* -------- exported API -------------------------------------------------- */

int vfmig_iova_domain_create(struct pci_dev *vf_pdev, u32 vf_id,
			     struct vfmig_iova_domain **out)
{
	struct vfmig_iova_domain *dom;
	struct iommu_domain *idom;
	u64 base;
	int err;

	if (!vf_pdev || !out)
		return -EINVAL;
	if ((u64)vf_id >= U32_MAX / 2)	/* defensive: catch overflow */
		return -EINVAL;

	base = VFMIG_IOVA_BASE + (u64)vf_id * VFMIG_IOVA_PER_VF;
	if (base < VFMIG_IOVA_BASE)	/* wrapped */
		return -ERANGE;

	dom = kzalloc(sizeof(*dom), GFP_KERNEL);
	if (!dom)
		return -ENOMEM;

	mutex_init(&dom->lock);
	INIT_LIST_HEAD(&dom->pages);
	INIT_LIST_HEAD(&dom->transient.free);
	dom->vf_id  = vf_id;
	dom->base   = base;
	/*
	 * Per-slot bump cursors: each starts at its slot's window base.
	 * Slot 0 (VFMIG_SLOT_INVALID) gets a cursor too, to keep the
	 * indexing trivial -- alloc_slot rejects SLOT_INVALID before it
	 * ever touches cursor[0]. next_auto_key[] is zero-initialized
	 * by kzalloc above; first auto-assignment yields key 1.
	 */
	{
		unsigned int s;

		for (s = 0; s < VFMIG_IOVA_NR_SLOTS; s++)
			dom->cursor[s] = vfmig_iova_slot_base(dom,
					(enum vfmig_iova_slot)s);
	}

	/*
	 * Transient arena owns [det_end, det_end + TRANSIENT_BYTES),
	 * where det_end is the upper bound of the deterministic range.
	 * det_end equals base + NR_SLOTS * SLOT_BYTES by construction
	 * (see SLOT_BYTES definition); using the slot helper keeps the
	 * arithmetic in one place.
	 */
	dom->transient.base      = vfmig_iova_slot_end(dom,
					(enum vfmig_iova_slot)
					(VFMIG_IOVA_NR_SLOTS - 1));
	dom->transient.end       = base + VFMIG_IOVA_PER_VF;
	dom->transient.cursor    = dom->transient.base;
	dom->transient.max_pages = VFMIG_IOVA_TRANSIENT_MAX_PAGES;
	dom->transient.slots = kcalloc(dom->transient.max_pages,
				       sizeof(*dom->transient.slots),
				       GFP_KERNEL);
	if (!dom->transient.slots) {
		err = -ENOMEM;
		goto err_free_dom;
	}

	idom = iommu_paging_domain_alloc(&vf_pdev->dev);
	if (IS_ERR(idom)) {
		err = PTR_ERR(idom);
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: paging_domain_alloc failed: %d\n", err);
		goto err_free_dom;
	}
	dom->iommu_dom = idom;

	err = iommu_attach_device(dom->iommu_dom, &vf_pdev->dev);
	if (err) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: attach failed: %d\n", err);
		goto err_free_idom;
	}

	/*
	 * Validate that our deterministic IOVA window fits inside the
	 * IOMMU's geometry aperture. The underlying iommu driver picks
	 * aperture_end based on the hardware's address-width (e.g. 48 bits
	 * on most Intel VT-d, 48 or 52 on AMD-Vi / 5-level Intel), and
	 * iommu_map() returns -ERANGE for any IOVA outside it. Catch the
	 * mismatch here so the failure surfaces at "set_tracked enable=1"
	 * with a printed reason, rather than at "first cmd ring DMA"
	 * deep inside mlx5_cmd_enable.
	 */
	/*
	 * Validate the FULL window (deterministic + transient) against
	 * the IOMMU aperture. The deterministic upper bound is the end
	 * of the last slot, which is also the transient arena's base.
	 */
	if (dom->base < dom->iommu_dom->geometry.aperture_start ||
	    dom->transient.end - 1 > dom->iommu_dom->geometry.aperture_end) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u IOVA window [0x%llx, 0x%llx) does not fit IOMMU aperture [0x%llx, 0x%llx]\n",
			 vf_id, dom->base, dom->transient.end,
			 dom->iommu_dom->geometry.aperture_start,
			 dom->iommu_dom->geometry.aperture_end);
		err = -EOPNOTSUPP;
		goto err_detach;
	}

	dom->vf_pdev = pci_dev_get(vf_pdev);

	dev_info(&vf_pdev->dev,
		 "vfmig_iova: vf %u domain attached, IOVA window [0x%llx, 0x%llx) (deterministic, %u slots x 0x%llx) + [0x%llx, 0x%llx) (transient) within IOMMU aperture [0x%llx, 0x%llx]\n",
		 vf_id, dom->base, dom->transient.base,
		 VFMIG_IOVA_NR_SLOTS, (u64)VFMIG_IOVA_SLOT_BYTES,
		 dom->transient.base, dom->transient.end,
		 dom->iommu_dom->geometry.aperture_start,
		 dom->iommu_dom->geometry.aperture_end);

	*out = dom;
	return 0;

err_detach:
	iommu_detach_device(dom->iommu_dom, &vf_pdev->dev);

err_free_idom:
	iommu_domain_free(dom->iommu_dom);
err_free_dom:
	kfree(dom->transient.slots);
	mutex_destroy(&dom->lock);
	kfree(dom);
	return err;
}

/*
 * dom->lock held. Tear down every page in the transient arena: walk
 * arena->slots[], iommu_unmap each mapped page, free the backing
 * struct page, and free the bookkeeping. Doesn't touch the slots[]
 * array itself (the caller frees that).
 *
 * It is intentional that we drain via slots[] rather than the
 * freelist: if a caller leaked a transient_get() (didn't pair it with
 * _put()), the page is OFF the freelist but still on slots[], and we
 * would leak its iommu_map and the backing page if we walked the free
 * list only.
 */
static void vfmig_transient_drain_locked(struct vfmig_iova_domain *dom)
{
	struct vfmig_transient_arena *a = &dom->transient;
	unsigned int i;

	if (!a->slots)
		return;
	for (i = 0; i < a->max_pages; i++) {
		struct vfmig_transient_page *tp = a->slots[i];

		if (!tp)
			continue;
		(void)iommu_unmap(dom->iommu_dom, tp->iova, PAGE_SIZE);
		__free_pages(tp->page, 0);
		kfree(tp);
		a->slots[i] = NULL;
	}
	INIT_LIST_HEAD(&a->free);
	a->n_mapped = 0;
	a->n_free   = 0;
}

void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom)
{
	struct vfmig_iova_page *p, *tmp;
	struct pci_dev *vf_pdev;

	if (!dom)
		return;
	vf_pdev = dom->vf_pdev;

	mutex_lock(&dom->lock);
	vfmig_transient_drain_locked(dom);
	list_for_each_entry_safe(p, tmp, &dom->pages, node) {
		list_del(&p->node);
		vfmig_iova_destroy_page_locked(dom, p);
	}
	dom->n_pages = 0;
	mutex_unlock(&dom->lock);

	if (vf_pdev) {
		iommu_detach_device(dom->iommu_dom, &vf_pdev->dev);
		dev_info(&vf_pdev->dev,
			 "vfmig_iova: vf %u domain detached and freed\n",
			 dom->vf_id);
	}
	iommu_domain_free(dom->iommu_dom);
	if (vf_pdev)
		pci_dev_put(vf_pdev);

	kfree(dom->transient.slots);
	mutex_destroy(&dom->lock);
	kfree(dom);
}

int vfmig_iova_alloc_slot(struct vfmig_iova_domain *dom,
			  enum vfmig_iova_slot slot, u64 instance_key,
			  size_t size, gfp_t gfp,
			  dma_addr_t *iova_out, void **vaddr_out)
{
	struct vfmig_iova_page *p;
	size_t aligned;
	u64 iova, slot_end;
	u64 caller_key;
	int err;

	if (!dom || !iova_out || !vaddr_out || size == 0)
		return -EINVAL;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR)
		return -EINVAL;

	aligned = ALIGN(size, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);

	/*
	 * Auto-assign instance_key if the caller passed 0. Per-slot
	 * counter, so adding allocations in another slot doesn't
	 * perturb this slot's keys. Caller-pinned (non-zero) keys are
	 * recorded as-is and don't bump the counter.
	 *
	 * caller_key remembers the pre-resolution value so the drift
	 * diagnostic on HIT mismatch can distinguish a 0/auto caller
	 * from a pinned caller.
	 */
	caller_key = instance_key;
	if (instance_key == 0)
		instance_key = ++dom->next_auto_key[slot];

	iova = dom->cursor[slot];
	slot_end = vfmig_iova_slot_end(dom, slot);
	if (iova + aligned > slot_end) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u slot %u exhausted at cursor 0x%llx (slot_end 0x%llx, asked %zu)\n",
				     dom->vf_id, slot, iova, slot_end,
				     aligned);
		err = -ENOSPC;
		goto out_unlock;
	}

	/*
	 * Lookup-or-alloc at the per-slot cursor. If an entry already
	 * exists at @iova it must have come from a prior replay; we
	 * expect the size to match what the source had at this slot
	 * position, but if it doesn't we error out rather than silently
	 * hand back a too-small or too-big mapping.
	 *
	 * The order-based determinism is scoped to a single slot:
	 * a missing or extra alloc in slot X drifts only slot X's
	 * cursor; slots Y, Z stay put. When @drift_armed is set
	 * (i.e. the destination is restoring from a SAVE blob), HIT
	 * additionally checks that the caller's resolved instance_key
	 * matches the replayed entry's, and MISS in a slot that
	 * carried any replays is rejected outright as kernel-side
	 * drift.
	 */
	p = vfmig_iova_find_locked(dom, iova);
	if (p) {
		if (p->len != aligned) {
			dev_warn(&dom->vf_pdev->dev,
				 "vfmig_iova: vf %u slot %u replay/alloc size mismatch at IOVA 0x%llx: replayed %zu, requested %zu\n",
				 dom->vf_id, slot, iova, p->len, aligned);
			err = -EINVAL;
			goto out_unlock;
		}

		if (dom->drift_armed && p->instance_key != instance_key) {
			dev_err(&dom->vf_pdev->dev,
				"vfmig_iova: vf %u slot %u DRIFT: caller passed key %s (resolved 0x%llx) but replayed entry at IOVA 0x%llx carries key 0x%llx; pinned/auto sequence diverged from source\n",
				dom->vf_id, slot,
				caller_key == 0 ? "0/auto" : "pinned",
				(unsigned long long)instance_key,
				(unsigned long long)iova,
				(unsigned long long)p->instance_key);
			if (!dom->drift_reported) {
				dom->drift_reported = true;
				dump_stack();
			}
			err = -EPROTO;
			goto out_unlock;
		}

		/*
		 * Re-tag is now expected to be a no-op (slot is fixed
		 * by cursor position; instance_key matches per the
		 * armed check above). Kept unconditional for the
		 * non-armed case where the source's wire records still
		 * supplied the canonical key on replay; we just echo it
		 * back to make it explicit that the entry is now
		 * "owned" by this caller.
		 */
		p->slot		= slot;
		p->instance_key	= instance_key;
		*iova_out  = p->iova;
		*vaddr_out = p->vaddr;
		dom->cursor[slot] = iova + aligned;
		dom->alloc_count[slot]++;
		err = 0;
		goto out_unlock;
	}

	if (dom->drift_armed && dom->expected_count[slot] > 0) {
		dev_err(&dom->vf_pdev->dev,
			"vfmig_iova: vf %u slot %u DRIFT: alloc beyond source's footprint (replays=%u, claimed=%u, requested key 0x%llx size %zu at cursor 0x%llx); kernel-side added an alloc in this slot\n",
			dom->vf_id, slot,
			dom->expected_count[slot],
			dom->alloc_count[slot],
			(unsigned long long)instance_key,
			aligned,
			(unsigned long long)iova);
		if (!dom->drift_reported) {
			dom->drift_reported = true;
			dump_stack();
		}
		err = -EPROTO;
		goto out_unlock;
	}

	err = vfmig_iova_install_page_locked(dom, slot, instance_key,
					     iova, aligned, gfp, &p);
	if (err)
		goto out_unlock;

	dom->cursor[slot] = iova + aligned;
	dom->alloc_count[slot]++;
	*iova_out  = p->iova;
	*vaddr_out = p->vaddr;
	err = 0;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

void vfmig_iova_arm_drift_detection(struct vfmig_iova_domain *dom)
{
	unsigned int s;
	u32 total = 0;

	if (!dom)
		return;
	mutex_lock(&dom->lock);
	if (!dom->drift_armed) {
		dom->drift_armed = true;
		for (s = 0; s < VFMIG_IOVA_NR_SLOTS; s++)
			total += dom->expected_count[s];
		dev_info(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u drift detection armed (replays: cmd_ring=%u fw_page=%u dma_coherent=%u, total=%u)\n",
			 dom->vf_id,
			 dom->expected_count[VFMIG_SLOT_CMD_RING],
			 dom->expected_count[VFMIG_SLOT_FW_PAGE],
			 dom->expected_count[VFMIG_SLOT_DMA_COHERENT],
			 total);
	}
	mutex_unlock(&dom->lock);
}

void vfmig_iova_free_slot(struct vfmig_iova_domain *dom,
			  enum vfmig_iova_slot slot,
			  dma_addr_t iova, size_t size)
{
	struct vfmig_iova_page *p;
	size_t aligned;

	if (!dom)
		return;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: free_slot: bad slot %u for IOVA 0x%llx\n",
			 slot, (u64)iova);
		return;
	}
	aligned = ALIGN(size, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);
	p = vfmig_iova_find_locked(dom, iova);
	if (!p) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u free of unknown IOVA 0x%llx (slot %u)\n",
			 dom->vf_id, (u64)iova, slot);
		goto out_unlock;
	}
	WARN_ON_ONCE(p->slot != slot);
	if (p->len != aligned) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u free size mismatch at IOVA 0x%llx (slot %u): have %zu, asked %zu\n",
			 dom->vf_id, (u64)iova, slot, p->len, aligned);
		/* still proceed: the entry is what it is */
	}
	list_del(&p->node);
	dom->n_pages--;
	vfmig_iova_destroy_page_locked(dom, p);

out_unlock:
	mutex_unlock(&dom->lock);
}

int vfmig_iova_replay_page(struct vfmig_iova_domain *dom,
			   enum vfmig_iova_slot slot, u64 instance_key,
			   dma_addr_t iova, const void *contents,
			   size_t len)
{
	struct vfmig_iova_page *p;
	enum vfmig_iova_slot iova_slot;
	int err;

	if (!dom || !contents)
		return -EINVAL;

	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_IOVA_NR_SLOTS) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u replay: slot %u out of range\n",
			 dom->vf_id, slot);
		return -EINVAL;
	}

	/*
	 * Cross-check that the wire-claimed slot agrees with the slot
	 * the destination's IOVA partitioning would assign to this
	 * @iova. A mismatch means the source and destination disagree
	 * about VFMIG_IOVA_NR_SLOTS / VFMIG_IOVA_SLOT_BYTES /
	 * VFMIG_IOVA_PER_VF (i.e. wire-incompatible kernel build
	 * options); the deterministic guarantee is broken and we must
	 * not silently install at an unexpected slot.
	 */
	iova_slot = vfmig_iova_slot_from_iova(dom, (u64)iova);
	if (iova_slot != slot) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u replay: wire claims slot %u for IOVA 0x%llx but destination partitioning maps it to slot %u\n",
			 dom->vf_id, slot, (u64)iova, iova_slot);
		return -ERANGE;
	}

	mutex_lock(&dom->lock);

	/*
	 * Replay after the domain has been armed for drift detection
	 * is anomalous: the LOAD path arms exactly once, after every
	 * HOST_PAGE record has been parsed and the wire CRC has
	 * verified. A late replay would mean expected_count[slot]
	 * grows after we've already declared the source's footprint,
	 * which would let a later (kernel-added) alloc HIT this entry
	 * and mask the drift. Refuse it.
	 */
	if (WARN_ON_ONCE(dom->drift_armed)) {
		err = -EBUSY;
		goto out_unlock;
	}

	err = vfmig_iova_install_page_locked(dom, slot, instance_key,
					     iova, len, GFP_KERNEL, &p);
	if (err)
		goto out_unlock;

	memcpy(p->vaddr, contents, len);
	dom->expected_count[slot]++;

	/*
	 * Push the slot's cursor past the highest-replayed IOVA in
	 * THAT slot so a later vfmig_iova_reset_cursor() resets to the
	 * slot's base, not "ahead of everything in the slot"; and so
	 * that if no reset is issued, fresh allocs in this slot still
	 * don't collide with replayed ranges.
	 */
	if (iova + len > dom->cursor[slot])
		dom->cursor[slot] = iova + len;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

void vfmig_iova_reset_cursor(struct vfmig_iova_domain *dom)
{
	unsigned int s;

	if (!dom)
		return;
	mutex_lock(&dom->lock);
	for (s = 0; s < VFMIG_IOVA_NR_SLOTS; s++) {
		dom->cursor[s] = vfmig_iova_slot_base(dom,
				(enum vfmig_iova_slot)s);
		dom->next_auto_key[s] = 0;
		/*
		 * Per-slot alloc accounting is reset alongside the
		 * cursor so the destination's claim sequence starts
		 * from zero on each VF probe arc. expected_count is
		 * NOT reset: it's the source's recorded footprint, set
		 * once at LOAD time and frozen by drift_armed.
		 */
		dom->alloc_count[s] = 0;
	}
	mutex_unlock(&dom->lock);
}

int vfmig_iova_for_each(struct vfmig_iova_domain *dom,
			vfmig_iova_for_each_fn cb, void *ctx)
{
	struct vfmig_iova_page *p;
	int ret = 0;

	if (!dom || !cb)
		return -EINVAL;

	mutex_lock(&dom->lock);
	list_for_each_entry(p, &dom->pages, node) {
		ret = cb(p->slot, p->instance_key,
			 p->iova, p->vaddr, p->len, ctx);
		if (ret)
			break;
	}
	mutex_unlock(&dom->lock);
	return ret;
}

/* -------- transient arena ----------------------------------------------- */

/*
 * dom->lock held. Grow the arena by one page: alloc_pages, iommu_map
 * at the next cursor IOVA, install in slots[], return the new
 * descriptor (NOT on the freelist; caller will hand it to its
 * requester directly).
 */
static struct vfmig_transient_page *
vfmig_transient_grow_locked(struct vfmig_iova_domain *dom, gfp_t gfp)
{
	struct vfmig_transient_arena *a = &dom->transient;
	struct vfmig_transient_page *tp;
	unsigned int idx;
	int err;

	if (a->n_mapped >= a->max_pages)
		return ERR_PTR(-ENOMEM);

	tp = kzalloc(sizeof(*tp), gfp);
	if (!tp)
		return ERR_PTR(-ENOMEM);
	INIT_LIST_HEAD(&tp->free_node);	/* enables list_empty()
					 * double-free detection in
					 * vfmig_iova_transient_put() */

	tp->page = alloc_pages(gfp | __GFP_ZERO, 0);
	if (!tp->page) {
		kfree(tp);
		return ERR_PTR(-ENOMEM);
	}
	tp->vaddr = page_address(tp->page);
	tp->iova  = a->cursor;

	err = iommu_map(dom->iommu_dom, tp->iova, page_to_phys(tp->page),
			PAGE_SIZE, IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE,
			gfp);
	if (err) {
		__free_pages(tp->page, 0);
		kfree(tp);
		return ERR_PTR(err);
	}

	idx = (tp->iova - a->base) >> PAGE_SHIFT;
	a->slots[idx] = tp;
	a->cursor    += PAGE_SIZE;
	a->n_mapped++;

	return tp;
}

int vfmig_iova_transient_get(struct vfmig_iova_domain *dom,
			     size_t size, gfp_t gfp,
			     void **vaddr_out, dma_addr_t *iova_out)
{
	struct vfmig_transient_arena *a;
	struct vfmig_transient_page *tp;
	int err;

	if (!dom || !vaddr_out || !iova_out || size == 0)
		return -EINVAL;

	if (size > PAGE_SIZE) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: transient_get(size=%zu) > PAGE_SIZE not supported\n",
				     size);
		return -EINVAL;
	}

	if (gfp & (__GFP_COMP | __GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM)) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: transient_get: rejected gfp 0x%x (must not include __GFP_HIGHMEM/COMP/DMA/DMA32)\n",
				     gfp);
		return -EINVAL;
	}

	a = &dom->transient;
	mutex_lock(&dom->lock);

	tp = list_first_entry_or_null(&a->free,
				      struct vfmig_transient_page, free_node);
	if (tp) {
		/*
		 * list_del_init() instead of list_del() so that
		 * list_empty(&tp->free_node) is true while @tp is
		 * out-with-the-caller. _put() uses that for double-free
		 * detection.
		 */
		list_del_init(&tp->free_node);
		a->n_free--;
	} else {
		tp = vfmig_transient_grow_locked(dom, gfp);
		if (IS_ERR(tp)) {
			err = PTR_ERR(tp);
			mutex_unlock(&dom->lock);
			return err;
		}
	}

	*iova_out  = tp->iova;
	*vaddr_out = tp->vaddr;
	mutex_unlock(&dom->lock);
	return 0;
}

void vfmig_iova_transient_put(struct vfmig_iova_domain *dom,
			      dma_addr_t iova, size_t size)
{
	struct vfmig_transient_arena *a;
	struct vfmig_transient_page *tp;
	unsigned int idx;

	if (!dom)
		return;

	a = &dom->transient;
	if ((u64)iova < a->base || (u64)iova >= a->end ||
	    !IS_ALIGNED((u64)iova, PAGE_SIZE)) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: transient_put: IOVA 0x%llx outside arena [0x%llx, 0x%llx) or unaligned\n",
			 (u64)iova, a->base, a->end);
		return;
	}
	if (size > PAGE_SIZE) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: transient_put: size=%zu > PAGE_SIZE\n",
			 size);
		return;
	}

	idx = ((u64)iova - a->base) >> PAGE_SHIFT;

	mutex_lock(&dom->lock);
	tp = a->slots[idx];
	if (!tp) {
		mutex_unlock(&dom->lock);
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: transient_put: IOVA 0x%llx never allocated\n",
			 (u64)iova);
		return;
	}
	if (WARN_ON_ONCE(tp->iova != (u64)iova)) {
		mutex_unlock(&dom->lock);
		return;
	}
	if (!list_empty(&tp->free_node)) {
		mutex_unlock(&dom->lock);
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: transient_put: double-free of IOVA 0x%llx\n",
			 (u64)iova);
		return;
	}
	list_add(&tp->free_node, &a->free);
	a->n_free++;
	mutex_unlock(&dom->lock);
}
