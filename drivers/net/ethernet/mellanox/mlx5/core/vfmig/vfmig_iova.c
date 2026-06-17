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

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/gfp.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "mlx5_core.h"
#include "vfmig_dma_ops.h"
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
	/*
	 * Stage-2 secondary index node, keyed by @instance_key. Only
	 * populated when @external == true AND @instance_key has been
	 * retagged with VFMIG_HUOBJ_KEY(kind, fw_id) (kind byte != 0)
	 * either by source-side vfmig_iova_retag_external_range() or
	 * by LOAD-side vfmig_iova_replay_external(). RB_CLEAR_NODE'd
	 * by every install path; rb_erase() short-circuited via
	 * RB_EMPTY_NODE() check in destroy.
	 */
	struct rb_node	 user_index_node;
	u64		 iova;
	size_t		 len;
	struct page	*page;
	void		*vaddr;
	enum vfmig_iova_slot slot;
	u64		 instance_key;

	/*
	 * @external: the backing page is owned by the caller, not by
	 * the registry. Set on entries created by the
	 * vfmig_iova_user_page_map_phys() path (umem-pinned MR / CQ /
	 * QP / SRQ buffers + doorbell records flowing through
	 * vfmig_dma_ops). For these entries the registry tracks the
	 * (iova, len, phys) bookkeeping and owns the iommu_map slot,
	 * but skips alloc_pages at install and __free_pages at
	 * destroy.
	 *
	 * @page is NULL and @vaddr is meaningless on external entries.
	 * @len + @iova are the live IOMMU mapping. @phys (recorded in
	 * the IOMMU page tables only -- not stored separately on the
	 * entry) is the umem-pinned physical address; we recover it
	 * via iommu_iova_to_phys() if SAVE-time iteration ever needs
	 * to read the umem contents back (stage 2 wire format will
	 * reach for it that way rather than carrying phys in the
	 * registry struct).
	 *
	 * @awaiting_bind: stage-2 LOAD-side flag. Set on entries
	 * pre-installed by HOST_USER_PAGE replay before any
	 * destination-side map_sg has run. The first map_sg call that
	 * reaches the matching cursor position consumes the entry,
	 * iommu_maps the freshly-pinned umem page at that IOVA, clears
	 * @awaiting_bind, increments @awaiting_bind_hits, and proceeds
	 * normally.
	 *
	 * Stage 1 wires both flags and the counter, but never sets
	 * @awaiting_bind: there is no LOAD-side USER_PAGE replay yet.
	 * Counter reads as zero, .map_sg always falls into the "fresh
	 * IOVA + iommu_map + install external" branch.
	 */
	bool		 external;
	bool		 awaiting_bind;
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

/*
 * One outstanding kcoherent allocation. Lives on
 * dom->kcoherent.pages until vfmig_iova_kcoherent_free unlinks and
 * destroys it. We keep enough state to unmap + free in domain
 * destroy if the caller leaks (drivers should call .free for every
 * .alloc, but the destroy path is defensively complete).
 *
 * @vaddr is the kernel-virtual base; the backing memory is a
 * physically-contiguous run of pages obtained via alloc_pages_exact,
 * so virt_to_phys(vaddr) is the IOMMU-mapped phys base. @len is the
 * (PAGE_SIZE-aligned) byte length passed at alloc time, also handed
 * to free_pages_exact at teardown.
 */
struct vfmig_kcoherent_page {
	struct list_head node;
	u64		 iova;
	size_t		 len;
	void		*vaddr;
};

/*
 * Per-domain kcoherent arena: bottom VFMIG_IOVA_KCOHERENT_BYTES of
 * slot 7 (USER_PAGE)'s window. Backs vfmig_dma_ops's .alloc / .free
 * and .map_phys / .unmap_phys callbacks (see vfmig_dma_ops.c
 * header comment for the .map_sg vs .map_phys routing split).
 *
 *   [base, end) == [slot_base(USER_PAGE),
 *                   slot_base(USER_PAGE) + KCOHERENT_BYTES)
 *
 * Allocations bump @cursor monotonically from @base toward @end.
 * Frees do NOT reclaim IOVA range to @cursor: kcoherent's caller
 * set is dominated by mlx5e ring/buffer setup at probe + ndo_open
 * (driver-lifetime, low churn) and per-WQE / per-skb streaming
 * maps that cycle through page_pool's stable mapping (also low
 * churn under steady-state). If a real workload reveals
 * fragmentation pressure (e.g. many netdev up/down cycles), switch
 * to gen_pool / iova allocator and add a free bitmap.
 *
 * @pages is the list of currently-outstanding *coherent*
 * allocations (vfmig_iova_kcoherent_alloc); it is used exclusively
 * for find-by-iova in _free and for the destroy-time drain. The
 * streaming-map path (vfmig_iova_kcoherent_map_phys) does NOT
 * insert into this list -- it relies on the caller passing back
 * the (iova, len) pair at unmap time, so the per-call cost stays
 * O(1). Neither path is exposed via vfmig_iova_for_each(); kcoherent
 * entries are NEVER part of the SAVE manifest.
 *
 * Locking: protected by @lock, a dedicated spinlock NOT shared with
 * dom->lock. This is mandatory because mlx5e's RX path calls
 * dma_map_page from NAPI poll context (softirq), which cannot take
 * mutexes. @lock is held only for cursor reservation and (for the
 * coherent-alloc path) list manipulation; iommu_map / iommu_unmap
 * happen OUTSIDE the lock with GFP_ATOMIC, relying on the IOMMU
 * subsystem's own internal locking. The critical section is
 * therefore O(1) integer arithmetic plus an O(1) list_add_tail,
 * keeping softirq stalls negligible.
 */
struct vfmig_kcoherent_arena {
	u64		 base;
	u64		 end;
	u64		 cursor;
	spinlock_t	 lock;
	struct list_head pages;
	unsigned int	 n_pages;	/* len of @pages, for diagnostics */
};

struct vfmig_iova_domain {
	struct iommu_domain *iommu_dom;
	struct pci_dev	    *vf_pdev;	/* held via pci_dev_get() */
	u32		     vf_id;

	/*
	 * Set by vfmig_iova_domain_detach_dev() once the iommu_dom has
	 * been detached from @vf_pdev and the dma_ops shim removed.
	 * Guards vfmig_iova_domain_destroy() from doing the detach a
	 * second time. The split exists because the iommu_dom attach
	 * MUST be torn down before pci_disable_sriov() fires device_del
	 * on the VF (otherwise the iommu core's BUS_NOTIFY_REMOVED_DEVICE
	 * notifier WARNs at drivers/iommu/iommu.c:715 -- the per-VF group
	 * goes empty while still holding our unmanaged domain instead of
	 * the default), but the rest of the domain teardown (page-list
	 * drain, kcoherent drain, kfree) must run AFTER pci_disable_sriov
	 * returns because per-VF mlx5_core remove_one paths free DMA
	 * mappings through vfmig_iova_free_slot() which still derefs
	 * @dom. Splitting "detach iommu/dma_ops" from "destroy domain"
	 * lets both invariants hold.
	 */
	bool		     dev_detached;

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

	/*
	 * Stage-2 secondary index over external entries, keyed by
	 * @instance_key (which encodes (kind, fw_id) per
	 * VFMIG_HUOBJ_KEY()). Populated by:
	 *   - vfmig_iova_retag_external_range() on the source side,
	 *     after a creation callsite knows its FW-assigned id;
	 *   - vfmig_iova_replay_external() on the destination side,
	 *     replaying a HOST_USER_PAGE record.
	 *
	 * Used by Stage 3's hint-aware vfmig_dma_ops.map_sg lookup to
	 * find the matching awaiting_bind entry at restore time.
	 *
	 * Protected by @lock (the same mutex that guards @pages).
	 * Inserts happen only via the retag / replay paths; the
	 * @awaiting_bind transition (true -> false) does NOT remove
	 * the entry from this tree (Stage 3 still needs the lookup
	 * for symmetric dereg).
	 */
	struct rb_root	     user_index;

	struct vfmig_transient_arena transient;
	struct vfmig_kcoherent_arena kcoherent;

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

	/*
	 * Cross-stage diagnostic: count of successful placeholder bind
	 * events on this domain. Bumped exactly once per call to
	 * vfmig_iova_bind_user_object() that consumes an awaiting_bind
	 * placeholder (Stage 3 D2). The Stage-1 dma_ops shim's .map_sg
	 * path was originally documented as the only writer; Stage 3
	 * shifted bind responsibility to the explicit verb-driven
	 * vfmig_iova_bind_user_object() primitive instead, so .map_sg
	 * no longer increments this counter (it always installs fresh
	 * external entries) and this field counts bind-from-placeholder
	 * events strictly. Read with vfmig_iova_awaiting_bind_hits().
	 */
	atomic_long_t	     awaiting_bind_hits;

	/*
	 * Diagnostic for the post-armed runtime-growth fallback.
	 * vfmig_iova_alloc_slot() routes any MISS-after-armed allocation
	 * to the kcoherent sub-arena instead of returning -EPROTO, so the
	 * destination can serve NEW kernel allocations (e.g. a fresh DB
	 * pgdir page for a userspace QP/CQ post-restore) without breaking
	 * deterministic IOVAs for *migrated* allocations. Each fallback
	 * bumps this counter; a non-zero value tells you the destination
	 * grew past the source's recorded footprint in some migration-
	 * tracked slot.
	 *
	 * FIXME(stage2): a cleaner long-term answer is per-component
	 * "transient" sub-arenas (e.g. mlx5e gets its own slot/window)
	 * rather than this catch-all auto-fallback. For Stage 1 the
	 * fallback unblocks restored-VF userspace verbs while preserving
	 * determinism for everything that is migrated.
	 */
	atomic_long_t	     kcoherent_fallback_hits;
};

static inline u64
vfmig_iova_slot_base(const struct vfmig_iova_domain *dom,
		     enum vfmig_iova_slot slot)
{
	return dom->base + (u64)slot * VFMIG_IOVA_SLOT_BYTES;
}

/*
 * USER_PAGE's effective starting IOVA after the kcoherent carve.
 *
 * The bottom VFMIG_IOVA_KCOHERENT_BYTES of slot 7's window are
 * reserved for the non-migrated kcoherent sub-arena (see
 * struct vfmig_kcoherent_arena above), so the user-MR IOVA range
 * actually available via vfmig_iova_user_page_map_phys() begins
 * at this offset. Code that needs the lower bound for USER_PAGE
 * range checks or cursor initialization MUST use this helper
 * rather than vfmig_iova_slot_base(dom, USER_PAGE) directly.
 */
static inline u64
vfmig_iova_user_page_start(const struct vfmig_iova_domain *dom)
{
	return vfmig_iova_slot_base(dom, VFMIG_SLOT_USER_PAGE) +
	       VFMIG_IOVA_KCOHERENT_BYTES;
}

/*
 * The deterministic IOVA range has an asymmetric layout: kernel slots
 * 0..6 are each VFMIG_IOVA_SLOT_BYTES (510 MiB) wide, and slot 7
 * (VFMIG_SLOT_USER_PAGE) absorbs everything between
 * slot_base(USER_PAGE) and the transient arena's base. That makes
 * "slot end" trivial for kernel slots and a separate lookup for
 * USER_PAGE: dom->transient.base is the inclusive upper bound.
 *
 * Note: vfmig_iova_domain_create() computes dom->transient.base
 * directly (PER_VF - TRANSIENT_BYTES from base), not via this helper,
 * so the function is safe to call any time after domain_create has
 * returned.
 */
static inline u64
vfmig_iova_slot_end(const struct vfmig_iova_domain *dom,
		    enum vfmig_iova_slot slot)
{
	if (slot == VFMIG_SLOT_USER_PAGE)
		return dom->transient.base;
	return dom->base + (u64)(slot + 1) * VFMIG_IOVA_SLOT_BYTES;
}

/*
 * Inverse of vfmig_iova_slot_base(): which slot does @iova fall into,
 * or VFMIG_SLOT_INVALID if it's outside any deterministic slot
 * window.
 *
 * Asymmetric layout (see vfmig_iova_slot_end above):
 *   - Anything below slot_base(USER_PAGE) maps by uniform division:
 *     (iova - base) / SLOT_BYTES gives the slot index in 0..6.
 *   - Anything in [slot_base(USER_PAGE),
 *                  slot_base(USER_PAGE) + KCOHERENT_BYTES) belongs
 *     to the kcoherent sub-arena. kcoherent is NOT a deterministic
 *     slot (allocations there are non-migrated and have no replay
 *     records on the wire), so it returns VFMIG_SLOT_INVALID. A
 *     replay record claiming USER_PAGE for an IOVA in this range
 *     fails the slot cross-check in vfmig_iova_replay_page() and
 *     is rejected with -ERANGE -- which is exactly the right
 *     behaviour: kcoherent IOVAs must never be re-installed via
 *     replay.
 *   - Anything in [user_page_start, transient.base) is USER_PAGE.
 *   - Anything >= transient.base belongs to the transient arena (or
 *     is out of range entirely) -- not a deterministic slot.
 */
static enum vfmig_iova_slot
vfmig_iova_slot_from_iova(const struct vfmig_iova_domain *dom, u64 iova)
{
	u64 off, idx;

	if (iova < dom->base)
		return VFMIG_SLOT_INVALID;
	if (iova >= dom->transient.base)
		return VFMIG_SLOT_INVALID;
	off = iova - dom->base;
	idx = off / VFMIG_IOVA_SLOT_BYTES;
	if (idx >= VFMIG_SLOT_USER_PAGE) {
		if (iova < vfmig_iova_user_page_start(dom))
			return VFMIG_SLOT_INVALID;	/* kcoherent range */
		return VFMIG_SLOT_USER_PAGE;
	}
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
	RB_CLEAR_NODE(&p->user_index_node);

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
 * dom->lock held. Install one external (caller-owned-page) registry
 * entry at @iova, mapping @phys for @len bytes. Used by the
 * vfmig_dma_ops shim to plumb umem-pinned pages through the per-VF
 * iommu_domain.
 *
 * Differs from vfmig_iova_install_page_locked in three ways:
 *   - No alloc_pages / page_address: the caller supplies @phys
 *     directly (typically derived from sg_phys(sg_entry)).
 *   - The new entry is flagged @external = true so
 *     destroy_page_locked skips __free_pages on tear-down.
 *   - p->page = NULL, p->vaddr = NULL: the registry doesn't own a
 *     kernel-virtual handle to the underlying page; SAVE-time read-
 *     back goes through iommu_iova_to_phys + kmap_local_page on the
 *     resolved struct page (stage 2 wire format will cover that).
 *
 * Same window/alignment validation as install_page_locked, same
 * -EEXIST on duplicate IOVA, same -ERANGE on slot/IOVA mismatch.
 */
static int
vfmig_iova_install_external_phys_locked(struct vfmig_iova_domain *dom,
					enum vfmig_iova_slot slot,
					u64 instance_key, u64 iova,
					phys_addr_t phys, size_t len,
					gfp_t gfp,
					struct vfmig_iova_page **out_p)
{
	struct vfmig_iova_page *p;
	int err;

	if (!IS_ALIGNED(iova, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(len, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(phys, VFMIG_IOVA_GRANULE) ||
	    len == 0)
		return -EINVAL;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR)
		return -EINVAL;
	{
		u64 lo = (slot == VFMIG_SLOT_USER_PAGE)
			? vfmig_iova_user_page_start(dom)
			: vfmig_iova_slot_base(dom, slot);
		if (iova < lo ||
		    iova + len > vfmig_iova_slot_end(dom, slot))
			return -ERANGE;
	}
	if (vfmig_iova_find_locked(dom, iova))
		return -EEXIST;

	p = kzalloc(sizeof(*p), gfp);
	if (!p)
		return -ENOMEM;

	p->page		= NULL;
	p->vaddr	= NULL;
	p->iova		= iova;
	p->len		= len;
	p->slot		= slot;
	p->instance_key	= instance_key;
	p->external	= true;
	p->awaiting_bind = false;
	RB_CLEAR_NODE(&p->user_index_node);

	err = iommu_map(dom->iommu_dom, iova, phys, len,
			IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, gfp);
	if (err) {
		kfree(p);
		return err;
	}

	vfmig_iova_insert_locked(dom, p);
	*out_p = p;
	return 0;
}

/*
 * dom->lock held. Tear down a single registry entry: iommu_unmap,
 * release backing pages, free the bookkeeping struct. List unlink is
 * caller's responsibility (so we can be called from list iteration).
 *
 * External entries (p->external == true) skip __free_pages because
 * the backing page is owned by the umem / dma-buf caller, not by
 * the registry. The IOMMU mapping is still ours to tear down.
 *
 * Awaiting-bind entries (p->awaiting_bind == true) also skip the
 * iommu_unmap: stage-2 LOAD pre-installs them before any actual
 * physical page exists for the IOVA, so iommu_unmap would walk
 * empty page tables. (Stage 1 never sets awaiting_bind; the check
 * is here so the path is consistent when stage 2 lands.)
 */
static void
vfmig_iova_destroy_page_locked(struct vfmig_iova_domain *dom,
			       struct vfmig_iova_page *p)
{
	/*
	 * Drop the entry from the stage-2 secondary index. Untagged
	 * (auto-numbered, kind == 0) external entries never get
	 * inserted, hence the RB_EMPTY_NODE short-circuit. Internal
	 * (non-external) entries always have RB_EMPTY_NODE because the
	 * insert paths (retag_external_range / replay_external) only
	 * touch external entries.
	 */
	if (!RB_EMPTY_NODE(&p->user_index_node))
		rb_erase(&p->user_index_node, &dom->user_index);
	if (!p->awaiting_bind)
		(void)iommu_unmap(dom->iommu_dom, p->iova, p->len);
	if (!p->external && p->page)
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
	dom->user_index = RB_ROOT;
	INIT_LIST_HEAD(&dom->transient.free);
	INIT_LIST_HEAD(&dom->kcoherent.pages);
	dom->vf_id  = vf_id;
	dom->base   = base;
	/*
	 * Per-slot bump cursors: each starts at its slot's window base.
	 * Slot 0 (VFMIG_SLOT_INVALID) gets a cursor too, to keep the
	 * indexing trivial -- alloc_slot rejects SLOT_INVALID before it
	 * ever touches cursor[0]. next_auto_key[] is zero-initialized
	 * by kzalloc above; first auto-assignment yields key 1.
	 *
	 * USER_PAGE is special-cased: its cursor starts at the
	 * effective user-MR base, i.e. past the kcoherent sub-arena
	 * (which lives in the bottom KCOHERENT_BYTES of slot 7's
	 * window). The kcoherent arena maintains its own independent
	 * cursor in dom->kcoherent.cursor.
	 */
	{
		unsigned int s;

		for (s = 0; s < VFMIG_IOVA_NR_SLOTS; s++)
			dom->cursor[s] = vfmig_iova_slot_base(dom,
					(enum vfmig_iova_slot)s);
		dom->cursor[VFMIG_SLOT_USER_PAGE] =
			vfmig_iova_user_page_start(dom);
	}

	/*
	 * Kcoherent sub-arena: bottom KCOHERENT_BYTES of slot 7's
	 * window. Lives between the deterministic kernel slots and the
	 * user-MR (USER_PAGE) sub-window. Allocations are not in the
	 * SAVE manifest; the cursor restarts at base on the destination
	 * naturally because each side's domain_create runs fresh.
	 */
	dom->kcoherent.base   = vfmig_iova_slot_base(dom,
						     VFMIG_SLOT_USER_PAGE);
	dom->kcoherent.end    = dom->kcoherent.base +
				VFMIG_IOVA_KCOHERENT_BYTES;
	dom->kcoherent.cursor = dom->kcoherent.base;
	spin_lock_init(&dom->kcoherent.lock);

	/*
	 * Transient arena owns the topmost VFMIG_IOVA_TRANSIENT_BYTES
	 * of each VF's IOVA window, [PER_VF - TRANSIENT_BYTES, PER_VF).
	 * Computed directly from the per-VF window because the
	 * vfmig_iova_slot_end() helper for VFMIG_SLOT_USER_PAGE reads
	 * dom->transient.base back, so we can't use it here without a
	 * chicken-and-egg.
	 *
	 * Layout invariant (see VFMIG_IOVA_NR_SLOTS / SLOT_BYTES doc in
	 * vfmig_iova.h): transient.base sits at exactly
	 *   base + KERNEL_NR_SLOTS * SLOT_BYTES + USER_PAGE_size,
	 * with USER_PAGE absorbing the remainder of the deterministic
	 * range.
	 */
	dom->transient.base      = base + VFMIG_IOVA_PER_VF -
				   VFMIG_IOVA_TRANSIENT_BYTES;
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

	/*
	 * Install the per-VF dma_map_ops shim that intercepts
	 * dma_map_sgtable / dma_map_phys for user-side ib_umem_get
	 * registrations. Done after iommu_attach_device so the
	 * (custom dma_ops, custom iommu_domain) pair is atomic from
	 * the DMA layer's point of view: while we're attached, every
	 * DMA-API entry point routes through vfmig_iova; while we're
	 * not, the default dma-iommu path owns the device.
	 *
	 * If this fails -- e.g. CONFIG_ARCH_HAS_DMA_OPS=n on this
	 * arch -- the kernel-slot allocator alone doesn't degrade
	 * gracefully (user-side umem mappings would silently bypass
	 * us), so we tear the whole domain down rather than ship a
	 * partially-functional one.
	 */
	err = vfmig_dma_ops_attach(vf_pdev, dom);
	if (err) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vfmig_dma_ops_attach failed: %d\n",
			 err);
		goto err_pci_put;
	}

	dev_info(&vf_pdev->dev,
		 "vfmig_iova: vf %u domain attached, IOVA window [0x%llx, 0x%llx) (deterministic, %u slots x 0x%llx) + [0x%llx, 0x%llx) (kcoherent) + [0x%llx, 0x%llx) (transient) within IOMMU aperture [0x%llx, 0x%llx]\n",
		 vf_id, dom->base, dom->kcoherent.base,
		 VFMIG_IOVA_NR_SLOTS, (u64)VFMIG_IOVA_SLOT_BYTES,
		 dom->kcoherent.base, dom->kcoherent.end,
		 dom->transient.base, dom->transient.end,
		 dom->iommu_dom->geometry.aperture_start,
		 dom->iommu_dom->geometry.aperture_end);

	*out = dom;
	return 0;

err_pci_put:
	pci_dev_put(dom->vf_pdev);
	dom->vf_pdev = NULL;
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

/*
 * Tear down every outstanding kcoherent allocation. Called from
 * vfmig_iova_domain_destroy() with the VF unbound (caller contract);
 * no concurrent kcoherent traffic is possible, so we don't need to
 * take @kcoherent.lock here. Tear down each entry on the list in
 * order: iommu_unmap, free_pages_exact, kfree the bookkeeping. The
 * iommu mapping cleanup for any _map_phys leaks (which carry no
 * bookkeeping) is left to iommu_domain_free() in the caller.
 *
 * A non-empty list at destroy time prints a once-per-domain warning
 * to surface the leak.
 */
static void vfmig_kcoherent_drain(struct vfmig_iova_domain *dom)
{
	struct vfmig_kcoherent_arena *a = &dom->kcoherent;
	struct vfmig_kcoherent_page *kp, *tmp;

	if (a->n_pages > 0)
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u kcoherent: %u outstanding allocations at domain destroy (driver leak); cleaning up\n",
				     dom->vf_id, a->n_pages);

	list_for_each_entry_safe(kp, tmp, &a->pages, node) {
		(void)iommu_unmap(dom->iommu_dom, kp->iova, kp->len);
		free_pages_exact(kp->vaddr, kp->len);
		list_del(&kp->node);
		kfree(kp);
	}
	a->n_pages = 0;
}

/*
 * Detach the per-VF iommu_dom and dma_ops shim from the VF's PCI device.
 * Leaves the domain struct (page list, kcoherent arena, transient slots,
 * iommu_dom pointer) intact so vfmig_iova_free_slot() / kcoherent_free()
 * still work for any teardown DMA calls that race after the iommu detach.
 * Idempotent: a second call is a no-op.
 *
 * Required call ordering for the SR-IOV teardown path:
 *
 *   pci_disable_sriov(pf_pdev)             // tears down each VF:
 *     for each vf:
 *       device_release_driver(vf)
 *         mlx5_core remove_one(vf)
 *           ... FW commands, EQ drain, DMA frees ...
 *           [HOOK] vfmig_iova_domain_detach_dev(dom_for_this_vf)
 *       pci_remove_bus_device(vf)
 *         device_del(vf)
 *           iommu core BUS_NOTIFY_REMOVED_DEVICE notifier      <-- no WARN
 *   mlx5_vfmig_pf_drop_iova_domains(pf)    // frees the domain structs
 *     for each dom:
 *       vfmig_iova_domain_destroy(dom)     // drain pages, kcoherent,
 *                                          //   skip detach (already done)
 */
void vfmig_iova_domain_detach_dev(struct vfmig_iova_domain *dom)
{
	struct pci_dev *vf_pdev;

	if (!dom || dom->dev_detached)
		return;

	vf_pdev = dom->vf_pdev;
	if (!vf_pdev)
		return;

	/*
	 * Reverse of domain_create: undo dma_ops first so any racing
	 * DMA path routes through the default dma-iommu shim before we
	 * tear down the iommu_domain it depends on. Caller contract is
	 * that no in-flight FW DMA is outstanding by the time we get
	 * here -- on the SR-IOV teardown path this is guaranteed because
	 * we're called from mlx5_core remove_one's tail, after
	 * mlx5_pci_close() has drained the cmd ring + EQs.
	 */
	vfmig_dma_ops_detach(vf_pdev);
	iommu_detach_device(dom->iommu_dom, &vf_pdev->dev);
	dom->dev_detached = true;

	dev_info(&vf_pdev->dev,
		 "vfmig_iova: vf %u domain detached from device (struct kept for cleanup)\n",
		 dom->vf_id);
}

void vfmig_iova_domain_detach_dev_if_unbound(struct vfmig_iova_domain *dom)
{
	if (!dom || dom->dev_detached || !dom->vf_pdev)
		return;
	if (dom->vf_pdev->driver)
		return;
	vfmig_iova_domain_detach_dev(dom);
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

	/*
	 * kcoherent drain is outside dom->lock: the arena has its own
	 * spinlock for runtime synchronization, and at destroy time
	 * the VF is unbound (caller contract) so no concurrent
	 * kcoherent traffic is possible -- drain runs lockless.
	 */
	vfmig_kcoherent_drain(dom);

	if (vf_pdev) {
		/*
		 * Reverse of domain_create: undo dma_ops first so any
		 * subsequent DMA path (none expected -- the VF is
		 * unbound by caller contract -- but be safe) routes
		 * through the default dma-iommu shim before we tear
		 * down the iommu_domain it depends on.
		 *
		 * If the caller already invoked vfmig_iova_domain_detach_dev()
		 * during the VF's mlx5_core remove_one (the recommended
		 * teardown ordering, see comment on @dev_detached) then
		 * this is a no-op and we go straight to iommu_domain_free()
		 * + kfree.
		 */
		if (!dom->dev_detached) {
			vfmig_dma_ops_detach(vf_pdev);
			iommu_detach_device(dom->iommu_dom, &vf_pdev->dev);
			dom->dev_detached = true;
		}
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
	/*
	 * USER_PAGE has its own dispatch path (vfmig_iova_user_page_map_phys)
	 * that installs caller-owned external pages. Kernel callers must
	 * not reach this slot through alloc_slot, which would alloc_pages
	 * and clobber the cursor that the dma_ops shim is bumping.
	 */
	if (slot == VFMIG_SLOT_USER_PAGE)
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
		/*
		 * Post-replay runtime growth on a restored VF.
		 *
		 * The source's footprint in this slot is exhausted (we
		 * walked through every replayed entry and the cursor is
		 * past the highest-replayed IOVA). New kernel allocations
		 * here -- e.g. a fresh DB pgdir page for a userspace QP/CQ
		 * created post-restore, the kernel WC-CQ probe inside
		 * mlx5_ib_alloc_ucontext, an EQ buffer for a new MSI-X
		 * vector -- represent legitimate destination-side state
		 * that the source never had. Returning -EPROTO would block
		 * Stage 1 functional gating (userspace verbs on the
		 * restored VF can't proceed past the very first
		 * post-restore alloc).
		 *
		 * Route to the kcoherent sub-arena instead: it has its
		 * own bump cursor in a dedicated IOVA window, is NOT in
		 * the SAVE manifest, and is invisible to drift detection.
		 * Determinism for migrated allocations is preserved (they
		 * already landed in this slot at their replayed IOVAs and
		 * the cursor walked past them); only post-replay growth
		 * is diverted.
		 *
		 * Logged + counted so the diagnostic value of drift
		 * detection is preserved: a non-zero
		 * @kcoherent_fallback_hits tells the operator the
		 * destination grew past the source's footprint in some
		 * migration-tracked slot.
		 *
		 * FIXME(stage2): replace this catch-all auto-fallback
		 * with per-component transient sub-arenas (e.g. give
		 * mlx5e a private slot/window so its post-probe
		 * allocations are partitioned from migration-tracked
		 * state at allocation time, not at fault time). For
		 * Stage 1 this fallback is the simplest unblock that
		 * keeps deterministic IOVAs intact for everything that
		 * IS migrated.
		 */
		dev_warn_ratelimited(&dom->vf_pdev->dev,
			"vfmig_iova: vf %u slot %u: routing alloc beyond source footprint to kcoherent (replays=%u, claimed=%u, key 0x%llx size %zu at cursor 0x%llx)\n",
			dom->vf_id, slot,
			dom->expected_count[slot],
			dom->alloc_count[slot],
			(unsigned long long)instance_key,
			aligned,
			(unsigned long long)iova);
		atomic_long_inc(&dom->kcoherent_fallback_hits);
		mutex_unlock(&dom->lock);
		return vfmig_iova_kcoherent_alloc(dom, size, gfp,
						  iova_out, vaddr_out);
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
			 "vfmig_iova: vf %u drift detection armed (replays: cmd_ring=%u fw_page=%u dma_coherent=%u eq_buf=%u frag_buf=%u db_page=%u, total=%u)\n",
			 dom->vf_id,
			 dom->expected_count[VFMIG_SLOT_CMD_RING],
			 dom->expected_count[VFMIG_SLOT_FW_PAGE],
			 dom->expected_count[VFMIG_SLOT_DMA_COHERENT],
			 dom->expected_count[VFMIG_SLOT_EQ_BUF],
			 dom->expected_count[VFMIG_SLOT_FRAG_BUF],
			 dom->expected_count[VFMIG_SLOT_DB_PAGE],
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
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR ||
	    slot == VFMIG_SLOT_USER_PAGE) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: free_slot: bad slot %u for IOVA 0x%llx\n",
			 slot, (u64)iova);
		return;
	}

	/*
	 * Mirror the alloc-time auto-fallback: if the IOVA lives in the
	 * kcoherent sub-arena, the alloc was diverted there because the
	 * caller's slot was past the source's footprint. Free via the
	 * kcoherent path; the caller's @slot tag is irrelevant for the
	 * actual unmap.
	 */
	if ((u64)iova >= dom->kcoherent.base &&
	    (u64)iova <  dom->kcoherent.end) {
		vfmig_iova_kcoherent_free(dom, iova, size, NULL);
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
	if (slot == VFMIG_SLOT_USER_PAGE) {
		/*
		 * USER_PAGE records take a different replay path:
		 * HOST_USER_PAGE wire records carry only identity
		 * (kind, fw_id, iova, len), not contents, and replay
		 * as awaiting_bind=true placeholders that stage 3
		 * binds when the user-mode RESTORE_x verb fires. The
		 * vfmig.c LOAD parser routes those records through
		 * vfmig_iova_replay_external() instead of this
		 * function; HOST_PAGE records (which DO carry
		 * contents) must never target the USER_PAGE slot.
		 */
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u replay_page: USER_PAGE slot is HOST_USER_PAGE-only (use replay_external)\n",
			 dom->vf_id);
		return -EOPNOTSUPP;
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
	u64 user_page_hwm;

	if (!dom)
		return;
	mutex_lock(&dom->lock);
	/*
	 * Snapshot the USER_PAGE cursor BEFORE the per-slot reset.
	 * Stage-2 replay (vfmig_iova_replay_external) bumps this
	 * cursor monotonically to (highest_replayed_iova + length) so
	 * fresh post-restore user_page_map_phys() calls land above
	 * source-side replayed placeholders. The for-loop below would
	 * otherwise clobber that high-water mark with slot_base, and
	 * the post-loop adjustment only lifts up to user_page_start
	 * -- which is exactly the bottom of the replay region, where
	 * the lowest-IOVA placeholder lives. The first fresh
	 * post-restore user_page allocation would then collide at
	 * user_page_start with the replayed placeholder there and
	 * return -EEXIST. Snapshot + max_t() with user_page_start
	 * preserves the replayed high-water mark on LOAD probe arcs
	 * AND still ratchets up to user_page_start on first-time /
	 * non-replay probe arcs (where the pre-loop value is
	 * slot_base, below user_page_start).
	 */
	user_page_hwm = dom->cursor[VFMIG_SLOT_USER_PAGE];

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
	/*
	 * USER_PAGE: skip the kcoherent carve at the bottom of slot 7's
	 * window, but ALSO preserve the post-replay high-water mark
	 * captured above. Without the max_t(), the for-loop reset
	 * collapses cursor[USER_PAGE] to slot_base and a fresh
	 * post-restore user_page_map_phys() collides with the
	 * lowest-IOVA replayed placeholder at user_page_start.
	 *
	 * Discovered by the CRIU runner's post-restore acid test
	 * (ibv_create_cq on the restored ucontext) which triggers
	 * mlx5_ib_db_map_user -> vfmig_dma_ops.map_sg -> a fresh
	 * user_page allocation at cursor[USER_PAGE]; the bug was
	 * silent before that path was exercised because no prior
	 * post-LOAD path allocates fresh user pages in slot 7.
	 */
	dom->cursor[VFMIG_SLOT_USER_PAGE] =
		max_t(u64, user_page_hwm,
		      vfmig_iova_user_page_start(dom));
	/*
	 * The kcoherent arena is NOT reset on replay: it has no
	 * SAVE-side records so there's nothing for replay to land in,
	 * and any allocations live for the lifetime of the bound
	 * driver, not across a probe arc.
	 */
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
		/*
		 * External entries (USER_PAGE, vfmig_dma_ops-backed)
		 * are skipped here: their backing pages are umem-pinned
		 * with @vaddr == NULL, so the SAVE-side callbacks that
		 * memcpy from @vaddr (HOST_PAGE record emission) would
		 * dereference NULL. Stage 2 surfaces externals through
		 * a sibling iterator (vfmig_iova_for_each_external)
		 * that emits identity-only HOST_USER_PAGE records, no
		 * contents.
		 */
		if (p->external)
			continue;
		ret = cb(p->slot, p->instance_key,
			 p->iova, p->vaddr, p->len, ctx);
		if (ret)
			break;
	}
	mutex_unlock(&dom->lock);
	return ret;
}

/*
 * dom->lock held. Insert @new into the (instance_key, iova) secondary
 * rb-tree index. The composite ordering matters: a single user
 * uobject (MR/CQ/QP/SRQ) whose umem spans more than one page produces
 * one registry entry per source-side sg (vfmig_dma_ops_map_sg iterates
 * the umem's sg_table and bumps the cursor per sg). All those
 * registry entries share @instance_key (one (kind, fw_id) pair per
 * uobject) but have distinct iovas, so without iova in the secondary
 * key the second sibling's insert would falsely collide with the
 * first.
 *
 * Returns 0 on success, -EEXIST only on the truly-impossible
 * same-(instance_key, iova) duplicate, which would mean the caller
 * inserted the same entry twice or the dom->pages primary index has
 * gone out of sync with itself (kernel bug, not a caller bug).
 *
 * Pre-condition: VFMIG_HUOBJ_KIND(@new->instance_key) != KIND_NONE
 * (auto-numbered entries don't go in the tree). Caller is
 * responsible for ensuring @new is also linked in dom->pages.
 */
static int
vfmig_iova_user_index_insert_locked(struct vfmig_iova_domain *dom,
				    struct vfmig_iova_page *new)
{
	struct rb_node **link = &dom->user_index.rb_node;
	struct rb_node *parent = NULL;
	struct vfmig_iova_page *p;

	while (*link) {
		parent = *link;
		p = rb_entry(parent, struct vfmig_iova_page,
			     user_index_node);
		if (new->instance_key < p->instance_key)
			link = &parent->rb_left;
		else if (new->instance_key > p->instance_key)
			link = &parent->rb_right;
		else if (new->iova < p->iova)
			link = &parent->rb_left;
		else if (new->iova > p->iova)
			link = &parent->rb_right;
		else
			return -EEXIST;
	}
	rb_link_node(&new->user_index_node, parent, link);
	rb_insert_color(&new->user_index_node, &dom->user_index);
	return 0;
}

/*
 * dom->lock held. Look up the LEFTMOST (lowest-iova) external
 * registry entry whose @instance_key equals @key in the
 * (instance_key, iova) secondary index, or return NULL if no entry
 * exists at that key.
 *
 * For a single-page uobject the leftmost match is the only match.
 * For a multi-page uobject (umem spans >1 page producing N source-
 * side sgs and N registry sibling entries), the leftmost match is
 * the head of the sibling chain; the caller iterates with
 * vfmig_iova_user_index_next_sibling_locked() to walk the rest.
 *
 * The C1 foundation commit deferred this helper because no in-tree
 * caller existed yet; Stage 3 D2's vfmig_iova_bind_user_object() is
 * the first consumer. Stage 3 wires the verb-driven bind path
 * (mlx5_ib_restore_X -> mlx5_ib_umem_restore -> vfmig_iova_bind_user_object)
 * and uses this lookup to translate a verb-supplied (kind, fw_id)
 * pair into the awaiting_bind=true placeholder(s) installed earlier
 * by vfmig_iova_replay_external().
 */
static struct vfmig_iova_page *
vfmig_iova_user_index_lookup_locked(struct vfmig_iova_domain *dom, u64 key)
{
	struct rb_node *n = dom->user_index.rb_node;
	struct vfmig_iova_page *p;
	struct vfmig_iova_page *found = NULL;

	while (n) {
		p = rb_entry(n, struct vfmig_iova_page, user_index_node);
		if (key < p->instance_key) {
			n = n->rb_left;
		} else if (key > p->instance_key) {
			n = n->rb_right;
		} else {
			/*
			 * Match: any leftmost-with-the-same-key candidate
			 * lives in n->rb_left (composite ordering: (key,
			 * x < n->iova) is in the left subtree). Walking
			 * left from a non-matching node still terminates
			 * at the correct leaf because the BST invariant
			 * guarantees go-left/go-right decisions explore
			 * the right subtrees.
			 */
			found = p;
			n = n->rb_left;
		}
	}
	return found;
}

/*
 * dom->lock held. Return the next sibling of @p in the
 * (instance_key, iova) secondary index, or NULL if @p is the
 * trailing sibling. Sibling ordering is iova-ascending, mirroring
 * the dom->pages primary index for the iova range a single uobject
 * occupies.
 */
static struct vfmig_iova_page *
vfmig_iova_user_index_next_sibling_locked(struct vfmig_iova_page *p)
{
	struct rb_node *next = rb_next(&p->user_index_node);
	struct vfmig_iova_page *q;

	if (!next)
		return NULL;
	q = rb_entry(next, struct vfmig_iova_page, user_index_node);
	if (q->instance_key != p->instance_key)
		return NULL;
	return q;
}

/*
 * dom->lock held. Install a LOAD-side placeholder external entry:
 * external = true, awaiting_bind = true, page = NULL, vaddr = NULL.
 * No iommu_map happens: the destination's physical page doesn't
 * exist yet (CRIU restores the user process after LOAD). The
 * iommu_map will happen later in Stage 3 when the hint-aware
 * map_sg consumes the awaiting_bind flag and binds a freshly-pinned
 * phys against the placeholder.
 *
 * If @instance_key has a non-zero kind byte (i.e. was source-side
 * retagged before SAVE), the entry is also inserted into the
 * (kind, fw_id) secondary index. A KIND_NONE instance_key is
 * rejected with -EINVAL: replay records only get emitted for
 * retagged entries.
 *
 * Same window/alignment validation as install_external_phys_locked:
 *   -EINVAL  bad alignment / wrong slot / KIND_NONE key
 *   -ERANGE  IOVA outside USER_PAGE sub-window
 *   -EEXIST  duplicate IOVA (the secondary index uses composite
 *            (instance_key, iova) ordering, so multi-page replays
 *            sharing one instance_key across N distinct iovas no
 *            longer collide on the second sibling)
 *   -ENOMEM  kzalloc failure
 */
static int
vfmig_iova_install_external_placeholder_locked(struct vfmig_iova_domain *dom,
					       enum vfmig_iova_slot slot,
					       u64 instance_key, u64 iova,
					       size_t len, gfp_t gfp,
					       struct vfmig_iova_page **out_p)
{
	struct vfmig_iova_page *p;
	int err;

	if (!IS_ALIGNED(iova, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(len, VFMIG_IOVA_GRANULE) ||
	    len == 0)
		return -EINVAL;
	if (slot != VFMIG_SLOT_USER_PAGE)
		return -EINVAL;
	if (VFMIG_HUOBJ_KIND(instance_key) == VFMIG_HUOBJ_KIND_NONE)
		return -EINVAL;
	{
		u64 lo = vfmig_iova_user_page_start(dom);

		if (iova < lo ||
		    iova + len > vfmig_iova_slot_end(dom, slot))
			return -ERANGE;
	}
	if (vfmig_iova_find_locked(dom, iova))
		return -EEXIST;

	p = kzalloc(sizeof(*p), gfp);
	if (!p)
		return -ENOMEM;

	p->page		= NULL;
	p->vaddr	= NULL;
	p->iova		= iova;
	p->len		= len;
	p->slot		= slot;
	p->instance_key	= instance_key;
	p->external	= true;
	p->awaiting_bind = true;
	RB_CLEAR_NODE(&p->user_index_node);

	err = vfmig_iova_user_index_insert_locked(dom, p);
	if (err) {
		kfree(p);
		return err;
	}

	vfmig_iova_insert_locked(dom, p);
	*out_p = p;
	return 0;
}

int vfmig_iova_replay_external(struct vfmig_iova_domain *dom,
			       enum vfmig_iova_slot slot,
			       u64 instance_key, dma_addr_t iova,
			       size_t length, gfp_t gfp)
{
	struct vfmig_iova_page *p;
	u64 above;
	int err;

	if (!dom)
		return -EINVAL;

	mutex_lock(&dom->lock);

	err = vfmig_iova_install_external_placeholder_locked(dom, slot,
							     instance_key,
							     (u64)iova,
							     length, gfp, &p);
	if (err)
		goto out_unlock;

	/*
	 * Push the per-slot bump cursor above this placeholder so any
	 * later post-restore user_page_map_phys() lands above the
	 * source's high-water IOVA. See the docstring on reset_cursor
	 * for why USER_PAGE's cursor is preserved across reset_cursor.
	 */
	above = (u64)iova + length;
	if (above > dom->cursor[VFMIG_SLOT_USER_PAGE])
		dom->cursor[VFMIG_SLOT_USER_PAGE] = above;

	/*
	 * Charge the replay to the slot's expected_count so the
	 * drift-detection arming pass at the end of LOAD doesn't
	 * conclude USER_PAGE saw zero replays and skip the
	 * drift-armed code path. Mirrors what vfmig_iova_replay_page()
	 * does for kernel slots.
	 */
	dom->expected_count[VFMIG_SLOT_USER_PAGE]++;
	err = 0;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

int vfmig_iova_retag_external_range(struct vfmig_iova_domain *dom,
				    dma_addr_t iova_base, size_t length,
				    u64 new_instance_key)
{
	struct vfmig_iova_page *p;
	u64 base = (u64)iova_base;
	u64 limit;
	u8 new_kind = VFMIG_HUOBJ_KIND(new_instance_key);
	unsigned int retagged = 0;
	int err = 0;

	/*
	 * Iteration order: dom->pages is sorted by IOVA, so a single
	 * forward walk finds every entry that overlaps [base, limit).
	 * Multi-page user objects (umem spanning >1 page) yield
	 * multiple registry entries inside [base, limit), one per
	 * source-side sg, all retagged with the same key. The
	 * (instance_key, iova) secondary index disambiguates them
	 * (siblings have distinct iovas), so per-page inserts no
	 * longer self-collide.
	 *
	 * The "another uobject already claimed this (kind, fw_id)"
	 * case is caught by the kind != NONE check below before the
	 * insert, returning -EEXIST. A rare insert-time -EEXIST means
	 * a (instance_key, iova) duplicate, which is a kernel-state
	 * bug. Either error triggers the rollback walk, which reverts
	 * every entry we tagged with @new_instance_key in this call
	 * back to instance_key = 0 (the auto-numbered values are not
	 * preserved -- v0 retag callsites abandon the auto-numbered
	 * identity on success anyway).
	 */

	if (!dom || length == 0)
		return -EINVAL;
	limit = base + length;
	if (!IS_ALIGNED(base, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(length, VFMIG_IOVA_GRANULE))
		return -EINVAL;
	if (new_kind == VFMIG_HUOBJ_KIND_NONE)
		return -EINVAL;

	mutex_lock(&dom->lock);

	list_for_each_entry(p, &dom->pages, node) {
		u64 p_end = p->iova + p->len;

		if (p->iova >= limit)
			break;	/* sorted: past the requested range */
		if (p_end <= base)
			continue;	/* before the range */
		if (!p->external)
			continue;	/* kernel slot; skip silently */

		if (p->instance_key == new_instance_key) {
			/*
			 * Idempotent re-retag with the same key: no-op.
			 * Already in the secondary index from the first
			 * retag call.
			 */
			retagged++;
			continue;
		}
		if (VFMIG_HUOBJ_KIND(p->instance_key) !=
		    VFMIG_HUOBJ_KIND_NONE) {
			/*
			 * Already tagged with a different (kind, fw_id).
			 * Caller is asking to re-key an entry that's
			 * already been claimed by another uobject -- this
			 * is a source-side bug (overlapping umem-sgt
			 * ranges from two different MR/CQ/QP creations).
			 */
			err = -EEXIST;
			break;
		}
		/*
		 * Auto-numbered (kind == NONE) entry: overwrite the key
		 * and insert into the secondary index. With composite
		 * (instance_key, iova) ordering this only fails on a
		 * truly-duplicate (key, iova) pair (kernel bug). Either
		 * way roll back everything we did this call.
		 */
		p->instance_key = new_instance_key;
		err = vfmig_iova_user_index_insert_locked(dom, p);
		if (err) {
			/* This one didn't make it into the tree; revert
			 * the key write so the rollback walk below sees
			 * a consistent (instance_key, index-membership)
			 * pair on every entry.
			 */
			p->instance_key = 0;
			break;
		}
		retagged++;
	}

	if (err) {
		/*
		 * Roll back: revert every entry we just retagged in this
		 * call. They're the only entries in [base, limit) whose
		 * current key equals @new_instance_key. (Idempotent-
		 * re-retag entries we left alone get matched too, but
		 * reverting them is harmless because they're already
		 * properly indexed and we'd reinsert immediately.)
		 *
		 * The rollback walks the same range, finds entries with
		 * key == new_instance_key AND kind != 0, removes them
		 * from the rb-tree, and zeroes their instance_key.
		 */
		list_for_each_entry(p, &dom->pages, node) {
			u64 p_end = p->iova + p->len;

			if (p->iova >= limit)
				break;
			if (p_end <= base)
				continue;
			if (!p->external)
				continue;
			if (p->instance_key != new_instance_key)
				continue;
			if (!RB_EMPTY_NODE(&p->user_index_node)) {
				rb_erase(&p->user_index_node,
					 &dom->user_index);
				RB_CLEAR_NODE(&p->user_index_node);
			}
			p->instance_key = 0;
		}
		goto out_unlock;
	}

	if (retagged == 0) {
		/*
		 * Caller asked to retag a range that has no external
		 * entries on it. That means the source-side callsite is
		 * misordered relative to the dma_map_sgtable (which
		 * allocates the external entries): the retag must run
		 * AFTER the umem has been mapped through vfmig_dma_ops.
		 */
		err = -ENOENT;
	}

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

int vfmig_iova_bind_user_object(struct vfmig_iova_domain *dom,
				u8 kind, u64 fw_id,
				struct sg_table *sgt)
{
	struct vfmig_iova_page *head, *sib;
	struct scatterlist *sg;
	u64 instance_key;
	u64 iova_cur, iova_start, sib_walk;
	size_t reg_total = 0;
	size_t sgt_total = 0;
	unsigned int n_siblings = 0;
	unsigned int i;
	int err;

	if (!dom || !sgt || sgt->orig_nents == 0)
		return -EINVAL;
	if (kind == VFMIG_HUOBJ_KIND_NONE || kind >= VFMIG_HUOBJ_KIND_NR)
		return -EINVAL;

	instance_key = VFMIG_HUOBJ_KEY(kind, fw_id);

	mutex_lock(&dom->lock);

	head = vfmig_iova_user_index_lookup_locked(dom, instance_key);
	if (!head) {
		err = -ENOENT;
		goto out_unlock;
	}

	/*
	 * Walk every sibling under @instance_key (multi-page user
	 * MR/CQ/QP/SRQ produce one registry entry per source-side sg,
	 * all sharing the same instance_key but at distinct iovas).
	 *
	 * Validate, in one pass:
	 *   (a) every sibling is an external USER_PAGE entry,
	 *   (b) every sibling is still awaiting bind (a partial-bound
	 *       state is a kernel-state bug; we mark all-or-nothing),
	 *   (c) sibling iovas are tightly contiguous -- the source-side
	 *       map_sg path uses a bump cursor for the whole sg_table
	 *       in one call, so consecutive sgs of one umem land on
	 *       consecutive iovas; replay_external honours that exact
	 *       layout when re-installing the placeholders, so
	 *       contiguity should always hold here. Anything else is
	 *       SAVE/LOAD wire-format drift or registry corruption,
	 *       not a CRIU caller bug.
	 *
	 * Also accumulate reg_total = sum of sibling lens so we can
	 * verify the destination's umem covers the same byte range.
	 */
	sib_walk = head->iova;
	for (sib = head; sib;
	     sib = vfmig_iova_user_index_next_sibling_locked(sib)) {
		if (!sib->external || sib->slot != VFMIG_SLOT_USER_PAGE) {
			dev_err_ratelimited(&dom->vf_pdev->dev,
					    "vfmig_iova: vf %u bind_user_object: kind=%u fw_id=0x%llx sibling %u hit non-USER_PAGE entry (slot=%u external=%d)\n",
					    dom->vf_id, kind,
					    (unsigned long long)fw_id,
					    n_siblings, sib->slot,
					    sib->external);
			err = -EINVAL;
			goto out_unlock;
		}
		if (!sib->awaiting_bind) {
			err = -EBUSY;
			goto out_unlock;
		}
		if (sib->iova != sib_walk) {
			dev_err_ratelimited(&dom->vf_pdev->dev,
					    "vfmig_iova: vf %u bind_user_object: kind=%u fw_id=0x%llx sibling %u iova 0x%llx not contiguous with previous end 0x%llx\n",
					    dom->vf_id, kind,
					    (unsigned long long)fw_id,
					    n_siblings,
					    (unsigned long long)sib->iova,
					    (unsigned long long)sib_walk);
			err = -EINVAL;
			goto out_unlock;
		}
		sib_walk = sib->iova + sib->len;
		reg_total += sib->len;
		n_siblings++;
	}

	/*
	 * The aggregate placeholder length is the umem byte length the
	 * source SAVE'd, summed across all siblings. The destination's
	 * ib_umem_pin produces an sg_table whose summed sg lengths
	 * equal the same umem byte length, so a mismatch means the
	 * verb body is binding the wrong umem (different addr/size
	 * from SAVE). The destination's sg layout doesn't have to
	 * match the source's sibling layout 1:1 -- only the total byte
	 * coverage matches. We map the dst sgs sequentially at
	 * consecutive iovas starting from the head sibling's iova,
	 * filling the [head->iova, head->iova + reg_total) range the
	 * source SAVE'd, regardless of how the dst-side sg_table
	 * happens to be split.
	 */
	for_each_sgtable_sg(sgt, sg, i)
		sgt_total += sg->length;
	if (sgt_total != reg_total) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u bind_user_object: kind=%u fw_id=0x%llx sgt total %zu != registry total %zu (across %u sibling(s))\n",
				     dom->vf_id, kind,
				     (unsigned long long)fw_id, sgt_total,
				     reg_total, n_siblings);
		err = -EINVAL;
		goto out_unlock;
	}

	/*
	 * One iommu_map per dst sg, mapping the run of physically-
	 * contiguous pages sg_alloc_append_table_from_pages built.
	 * sg->offset is 0 and sg->length is PAGE_SIZE-aligned for
	 * umem-pinned sg_tables; we still validate per sg before each
	 * iommu_map so a future caller with non-umem sgt shapes fails
	 * loudly rather than tripping iommu_map's internal alignment
	 * WARN.
	 */
	iova_start = head->iova;
	iova_cur   = iova_start;
	for_each_sgtable_sg(sgt, sg, i) {
		phys_addr_t phys = page_to_phys(sg_page(sg)) + sg->offset;

		if (!IS_ALIGNED(phys, VFMIG_IOVA_GRANULE) ||
		    !IS_ALIGNED((size_t)sg->length, VFMIG_IOVA_GRANULE) ||
		    sg->length == 0) {
			dev_err_ratelimited(&dom->vf_pdev->dev,
					    "vfmig_iova: vf %u bind_user_object: sg[%u] not page-aligned (phys 0x%llx len %u offset %u)\n",
					    dom->vf_id, i,
					    (unsigned long long)phys,
					    sg->length, sg->offset);
			err = -EINVAL;
			goto out_rollback;
		}
		err = iommu_map(dom->iommu_dom, iova_cur, phys, sg->length,
				IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE,
				GFP_KERNEL);
		if (err)
			goto out_rollback;

		sg_dma_address(sg) = iova_cur;
		sg_dma_len(sg)	   = sg->length;
		iova_cur += sg->length;
	}

	/*
	 * All-or-nothing transition: every sibling flips together so
	 * a partial bind is never observable.
	 *
	 * @awaiting_bind_hits is the bind-call counter, so it
	 * increments once per uobject (one bind = one uobject =
	 * potentially many sibling pages). The QUERY_AWAITING_BIND
	 * ioctl, which iterates dom->pages, naturally reports per-
	 * page residency: a multi-page uobject contributes N to its
	 * kind's awaiting count until bind flips all N flags here.
	 */
	for (sib = head; sib;
	     sib = vfmig_iova_user_index_next_sibling_locked(sib))
		sib->awaiting_bind = false;
	atomic_long_inc(&dom->awaiting_bind_hits);
	err = 0;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;

out_rollback:
	/*
	 * Undo the iommu_maps issued earlier in this call. sg_dma_address
	 * / sg_dma_len on previously-bound sgs stay populated; per design
	 * §A.H L2 the caller treats the umem as opaque on the error path
	 * and uses ib_umem_release() to drop pins. vfmig_dma_ops.unmap_sg
	 * skips sgs with sg_dma_address == 0, so partially-bound sgs that
	 * we did *not* populate above (and the iommu range we just freed)
	 * see no second unmap attempt. Every sibling remains
	 * awaiting_bind=true so subsequent RESTORE_X retries on the same
	 * (kind, fw_id) can try again.
	 */
	if (iova_cur > iova_start)
		(void)iommu_unmap(dom->iommu_dom, iova_start,
				  iova_cur - iova_start);
	goto out_unlock;
}
EXPORT_SYMBOL(vfmig_iova_bind_user_object);

int vfmig_iova_for_each_external(struct vfmig_iova_domain *dom,
				 vfmig_iova_for_each_external_fn cb,
				 void *ctx)
{
	struct vfmig_iova_page *p;
	int ret = 0;

	if (!dom || !cb)
		return -EINVAL;

	mutex_lock(&dom->lock);
	list_for_each_entry(p, &dom->pages, node) {
		if (!p->external)
			continue;
		ret = cb(VFMIG_HUOBJ_KIND(p->instance_key),
			 VFMIG_HUOBJ_FWID(p->instance_key),
			 p->iova, p->len, p->awaiting_bind, ctx);
		if (ret)
			break;
	}
	mutex_unlock(&dom->lock);
	return ret;
}

int vfmig_iova_count_awaiting_bind(struct vfmig_iova_domain *dom,
				   u64 *total_out, u64 *count_by_kind)
{
	struct vfmig_iova_page *p;
	u64 total = 0;
	unsigned int k;

	if (!dom)
		return -EINVAL;

	if (count_by_kind) {
		for (k = 0; k < VFMIG_HUOBJ_KIND_NR; k++)
			count_by_kind[k] = 0;
	}

	mutex_lock(&dom->lock);
	list_for_each_entry(p, &dom->pages, node) {
		u8 kind;

		if (!p->external || !p->awaiting_bind)
			continue;
		total++;
		if (!count_by_kind)
			continue;
		kind = VFMIG_HUOBJ_KIND(p->instance_key);
		if (kind >= VFMIG_HUOBJ_KIND_NR)
			kind = VFMIG_HUOBJ_KIND_NONE;	/* shouldn't happen */
		count_by_kind[kind]++;
	}
	mutex_unlock(&dom->lock);

	if (total_out)
		*total_out = total;
	return 0;
}

unsigned long vfmig_iova_awaiting_bind_hits(struct vfmig_iova_domain *dom)
{
	if (!dom)
		return 0;
	return atomic_long_read(&dom->awaiting_bind_hits);
}

unsigned long vfmig_iova_kcoherent_fallback_hits(struct vfmig_iova_domain *dom)
{
	if (!dom)
		return 0;
	return atomic_long_read(&dom->kcoherent_fallback_hits);
}

int vfmig_iova_user_page_map_phys(struct vfmig_iova_domain *dom,
				  phys_addr_t phys, size_t len, gfp_t gfp,
				  dma_addr_t *iova_out)
{
	struct vfmig_iova_page *p;
	size_t aligned;
	u64 iova, slot_end;
	int err;

	if (!dom || !iova_out || len == 0)
		return -EINVAL;
	if (!IS_ALIGNED(phys, VFMIG_IOVA_GRANULE))
		return -EINVAL;

	aligned = ALIGN(len, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);

	iova = dom->cursor[VFMIG_SLOT_USER_PAGE];
	slot_end = vfmig_iova_slot_end(dom, VFMIG_SLOT_USER_PAGE);
	if (iova + aligned > slot_end) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE slot exhausted at cursor 0x%llx (slot_end 0x%llx, asked %zu)\n",
				     dom->vf_id, iova, slot_end, aligned);
		err = -ENOSPC;
		goto out_unlock;
	}

	/*
	 * Stage 1: never a HIT here. Stage 2 will check for an
	 * @awaiting_bind entry at @iova and bind the freshly-pinned
	 * @phys to it (incrementing dom->awaiting_bind_hits) instead
	 * of installing a new entry. Until that lands, every map_sg
	 * segment goes through install_external below.
	 */
	p = vfmig_iova_find_locked(dom, iova);
	if (p) {
		/*
		 * A HIT in stage 1 means somebody else (kernel-slot
		 * caller, transient_get, prior leaked map_sg) sat on
		 * an IOVA we believe to be the next free USER_PAGE
		 * cursor position. That's a kernel bug and we'd rather
		 * fail this mapping cleanly than silently overwrite
		 * the existing entry.
		 */
		dev_err_ratelimited(&dom->vf_pdev->dev,
				    "vfmig_iova: vf %u USER_PAGE: cursor 0x%llx already has a registry entry (slot %u key 0x%llx len %zu); refusing to overwrite\n",
				    dom->vf_id, iova, p->slot,
				    (unsigned long long)p->instance_key,
				    p->len);
		err = -EEXIST;
		goto out_unlock;
	}

	/*
	 * Auto-numbered key for stage 1 / stage 2. Stage 3 will
	 * overwrite the recorded key with mkey-derived identity in a
	 * separate retag step (vfmig_iova_tag_user_mr) so that
	 * cross-host LOAD can match by mkey rather than by cursor
	 * position.
	 */
	err = vfmig_iova_install_external_phys_locked(dom,
			VFMIG_SLOT_USER_PAGE,
			++dom->next_auto_key[VFMIG_SLOT_USER_PAGE],
			iova, phys, aligned, gfp, &p);
	if (err)
		goto out_unlock;

	dom->cursor[VFMIG_SLOT_USER_PAGE] = iova + aligned;
	dom->alloc_count[VFMIG_SLOT_USER_PAGE]++;
	*iova_out = iova;
	err = 0;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

int vfmig_iova_user_page_unmap_phys(struct vfmig_iova_domain *dom,
				    dma_addr_t iova, size_t len)
{
	struct vfmig_iova_page *p;
	size_t aligned;
	int err;

	if (!dom)
		return -EINVAL;

	aligned = ALIGN(len, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);

	p = vfmig_iova_find_locked(dom, (u64)iova);
	if (!p) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE unmap: no registry entry at IOVA 0x%llx (asked %zu)\n",
				     dom->vf_id, (u64)iova, aligned);
		err = -ENOENT;
		goto out_unlock;
	}
	if (p->slot != VFMIG_SLOT_USER_PAGE) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE unmap: IOVA 0x%llx belongs to slot %u (expected USER_PAGE)\n",
				     dom->vf_id, (u64)iova, p->slot);
		err = -EINVAL;
		goto out_unlock;
	}
	if (!p->external) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE unmap: IOVA 0x%llx is not an external entry\n",
				     dom->vf_id, (u64)iova);
		err = -EINVAL;
		goto out_unlock;
	}
	if (p->len != aligned) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE unmap size mismatch at IOVA 0x%llx: have %zu, asked %zu\n",
				     dom->vf_id, (u64)iova, p->len, aligned);
		/* still proceed: the entry is what it is */
	}
	list_del(&p->node);
	dom->n_pages--;
	vfmig_iova_destroy_page_locked(dom, p);
	err = 0;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

/* -------- kcoherent arena ----------------------------------------------- */

/*
 * Locking model
 * -------------
 * @kcoherent.lock is a dedicated spinlock NOT shared with dom->lock.
 * It guards two pieces of state:
 *   - the bump cursor (@cursor), and
 *   - the outstanding-coherent-allocations list (@pages, @n_pages).
 *
 * All sleeping operations (alloc_pages_exact, free_pages_exact,
 * kzalloc with GFP_KERNEL, iommu_map / iommu_unmap with caller-
 * specified gfp) happen OUTSIDE the lock. iommu_map's atomicity is
 * handled by the IOMMU subsystem's internal locking; we don't need
 * to serialize map calls against each other -- they're operating on
 * disjoint IOVA ranges by construction (cursor reservations are
 * unique).
 *
 * Why a spinlock and not the existing dom->lock mutex: mlx5e's RX
 * path calls dma_map_page from NAPI poll context (softirq), which
 * cannot sleep. Mutexes are out. spin_lock_irqsave handles both
 * softirq and process-context callers correctly, at the cost of
 * disabling local IRQs for the brief integer-arithmetic critical
 * section.
 *
 * Cursor leaks
 * ------------
 * If iommu_map fails AFTER cursor reservation, the IOVA range is
 * "leaked" -- the cursor doesn't reclaim. This is consistent with
 * the broader kcoherent model (bump-only, no fragmentation
 * reclaim) and the loss is bounded: iommu_map failures on a healthy
 * IOMMU are rare (-ENOMEM in the IOMMU page-table allocator), and
 * the arena has 1 GiB of room.
 */

/* Held @kcoherent.lock. Locate the entry mapped at exactly @iova, or NULL. */
static struct vfmig_kcoherent_page *
vfmig_kcoherent_find_locked(struct vfmig_iova_domain *dom, u64 iova)
{
	struct vfmig_kcoherent_page *kp;

	list_for_each_entry(kp, &dom->kcoherent.pages, node) {
		if (kp->iova == iova)
			return kp;
	}
	return NULL;
}

/*
 * Reserve @aligned bytes at the cursor under @kcoherent.lock.
 * Returns the reserved IOVA on success, U64_MAX on exhaustion (so
 * callers can release any pre-allocated backing pages without
 * needing the lock to test for failure).
 */
static u64 vfmig_kcoherent_reserve(struct vfmig_iova_domain *dom,
				   size_t aligned)
{
	struct vfmig_kcoherent_arena *a = &dom->kcoherent;
	unsigned long flags;
	u64 iova;

	spin_lock_irqsave(&a->lock, flags);
	if (a->cursor + aligned > a->end) {
		spin_unlock_irqrestore(&a->lock, flags);
		return U64_MAX;
	}
	iova = a->cursor;
	a->cursor += aligned;
	spin_unlock_irqrestore(&a->lock, flags);
	return iova;
}

int vfmig_iova_kcoherent_alloc(struct vfmig_iova_domain *dom,
			       size_t size, gfp_t gfp,
			       dma_addr_t *iova_out, void **vaddr_out)
{
	struct vfmig_kcoherent_page *kp;
	size_t aligned;
	gfp_t gfp_pages;
	unsigned long flags;
	u64 iova;
	void *vaddr;
	int err;

	if (!dom || !iova_out || !vaddr_out || size == 0)
		return -EINVAL;

	aligned = ALIGN(size, PAGE_SIZE);

	/*
	 * Sanitize the gfp passed by dma_alloc_coherent for
	 * alloc_pages_exact + iommu_map + page_address requirements.
	 * (See vfmig_iova.h's API doc for the full rationale.)
	 */
	gfp_pages = (gfp & ~(__GFP_HIGHMEM | __GFP_COMP |
			     __GFP_DMA | __GFP_DMA32)) | __GFP_ZERO;

	/* All allocations happen OUTSIDE the spinlock. */
	kp = kzalloc(sizeof(*kp), gfp_pages);
	if (!kp)
		return -ENOMEM;

	vaddr = alloc_pages_exact(aligned, gfp_pages);
	if (!vaddr) {
		err = -ENOMEM;
		goto err_free_kp;
	}

	iova = vfmig_kcoherent_reserve(dom, aligned);
	if (iova == U64_MAX) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u kcoherent alloc exhausted (asked %zu, end 0x%llx)\n",
				     dom->vf_id, aligned,
				     dom->kcoherent.end);
		err = -ENOSPC;
		goto err_free_pages;
	}

	/*
	 * iommu_map outside the cursor lock. Pass GFP_ATOMIC so the
	 * IOMMU page-table allocator stays atomic-safe in case the
	 * caller invoked us from a context that itself disallowed
	 * sleeping (uncommon for .alloc, but cheap insurance).
	 */
	err = iommu_map(dom->iommu_dom, iova, virt_to_phys(vaddr),
			aligned,
			IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE,
			GFP_ATOMIC);
	if (err)
		goto err_free_pages;

	kp->iova  = iova;
	kp->len   = aligned;
	kp->vaddr = vaddr;

	spin_lock_irqsave(&dom->kcoherent.lock, flags);
	list_add_tail(&kp->node, &dom->kcoherent.pages);
	dom->kcoherent.n_pages++;
	spin_unlock_irqrestore(&dom->kcoherent.lock, flags);

	*iova_out  = iova;
	*vaddr_out = vaddr;
	return 0;

err_free_pages:
	free_pages_exact(vaddr, aligned);
err_free_kp:
	kfree(kp);
	return err;
}

int vfmig_iova_kcoherent_map_phys(struct vfmig_iova_domain *dom,
				  phys_addr_t phys, size_t len, gfp_t gfp,
				  dma_addr_t *iova_out)
{
	size_t aligned;
	gfp_t gfp_iommu;
	u64 iova;
	int err;

	if (!dom || !iova_out || len == 0)
		return -EINVAL;
	if (!IS_ALIGNED(phys, PAGE_SIZE))
		return -EINVAL;

	aligned = ALIGN(len, PAGE_SIZE);

	/*
	 * Strip flags iommu_map rejects. No __GFP_ZERO: caller-owned
	 * @phys already points at populated pages. Many callers from
	 * softirq pass GFP_ATOMIC; leave the atomicity bits intact so
	 * iommu_map honours them.
	 */
	gfp_iommu = gfp & ~(__GFP_HIGHMEM | __GFP_COMP |
			    __GFP_DMA | __GFP_DMA32);

	iova = vfmig_kcoherent_reserve(dom, aligned);
	if (iova == U64_MAX) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u kcoherent map_phys exhausted (asked %zu, end 0x%llx)\n",
				     dom->vf_id, aligned,
				     dom->kcoherent.end);
		return -ENOSPC;
	}

	err = iommu_map(dom->iommu_dom, iova, phys, aligned,
			IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE,
			gfp_iommu);
	if (err)
		return err;	/* iova reservation leaked; see file header */

	*iova_out = iova;
	return 0;
}

void vfmig_iova_kcoherent_unmap_phys(struct vfmig_iova_domain *dom,
				     dma_addr_t iova, size_t len)
{
	size_t aligned;

	if (!dom || len == 0)
		return;

	aligned = ALIGN(len, PAGE_SIZE);

	/*
	 * No bookkeeping to remove: kcoherent map_phys is registry-
	 * less. Defensive sanity check that @iova falls in the
	 * kcoherent window so a stale dma_handle from another path
	 * (USER_PAGE, transient, kernel slot) can't accidentally
	 * unmap something here.
	 */
	if (iova < dom->kcoherent.base ||
	    (u64)iova + aligned > dom->kcoherent.end) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u kcoherent unmap_phys: IOVA 0x%llx + 0x%zx outside kcoherent window [0x%llx, 0x%llx); ignoring\n",
				     dom->vf_id, (u64)iova, aligned,
				     dom->kcoherent.base,
				     dom->kcoherent.end);
		return;
	}

	/* iommu_unmap has its own internal locking; no kcoherent.lock needed. */
	(void)iommu_unmap(dom->iommu_dom, (u64)iova, aligned);
}

void vfmig_iova_kcoherent_free(struct vfmig_iova_domain *dom,
			       dma_addr_t iova, size_t size, void *vaddr)
{
	struct vfmig_kcoherent_page *kp;
	struct vfmig_kcoherent_page found;
	unsigned long flags;
	size_t aligned;

	if (!dom)
		return;

	aligned = ALIGN(size, PAGE_SIZE);

	spin_lock_irqsave(&dom->kcoherent.lock, flags);
	kp = vfmig_kcoherent_find_locked(dom, (u64)iova);
	if (!kp) {
		spin_unlock_irqrestore(&dom->kcoherent.lock, flags);
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u kcoherent free: no entry at IOVA 0x%llx (asked %zu); ignoring\n",
				     dom->vf_id, (u64)iova, aligned);
		return;
	}
	if (kp->len != aligned)
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u kcoherent free size mismatch at IOVA 0x%llx: have %zu, asked %zu; using recorded size\n",
				     dom->vf_id, (u64)iova, kp->len, aligned);
	if (vaddr && vaddr != kp->vaddr)
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u kcoherent free vaddr mismatch at IOVA 0x%llx: have %p, asked %p\n",
				     dom->vf_id, (u64)iova, kp->vaddr, vaddr);

	/*
	 * Snapshot the entry and unlink it under the spinlock. The
	 * actual iommu_unmap and free_pages_exact happen outside the
	 * lock so they can sleep / take their own locks freely.
	 */
	found = *kp;
	list_del(&kp->node);
	dom->kcoherent.n_pages--;
	spin_unlock_irqrestore(&dom->kcoherent.lock, flags);

	kfree(kp);
	(void)iommu_unmap(dom->iommu_dom, found.iova, found.len);
	free_pages_exact(found.vaddr, found.len);
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
