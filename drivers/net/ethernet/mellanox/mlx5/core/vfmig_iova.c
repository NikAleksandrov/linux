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
 */
struct vfmig_iova_page {
	struct list_head node;	/* dom->pages, sorted by iova ascending */
	u64		 iova;
	size_t		 len;
	struct page	*page;
	void		*vaddr;
};

struct vfmig_iova_domain {
	struct iommu_domain *iommu_dom;
	struct pci_dev	    *vf_pdev;	/* held via pci_dev_get() */
	u32		     vf_id;

	/* IOVA window for this VF: [base, base + VFMIG_IOVA_PER_VF). */
	u64		     base;
	u64		     end;

	struct mutex	     lock;
	u64		     cursor;	/* next fresh IOVA */
	struct list_head     pages;	/* of vfmig_iova_page, sorted */
	unsigned int	     n_pages;
};

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
 * because the meaning of "advance" differs between alloc_coherent and
 * replay.
 *
 * Returns 0 with *out_p set on success, negative errno otherwise.
 */
static int
vfmig_iova_install_page_locked(struct vfmig_iova_domain *dom,
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
	if (iova < dom->base || iova + len > dom->end)
		return -ERANGE;
	if (vfmig_iova_find_locked(dom, iova))
		return -EEXIST;

	p = kzalloc(sizeof(*p), gfp);
	if (!p)
		return -ENOMEM;

	order = get_order(len);
	p->page = alloc_pages(gfp | __GFP_ZERO, order);
	if (!p->page) {
		err = -ENOMEM;
		goto err_free_p;
	}
	p->vaddr = page_address(p->page);
	p->iova	 = iova;
	p->len	 = len;

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
	dom->vf_id  = vf_id;
	dom->base   = base;
	dom->end    = base + VFMIG_IOVA_PER_VF;
	dom->cursor = base;

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
	if (dom->base < dom->iommu_dom->geometry.aperture_start ||
	    dom->end - 1 > dom->iommu_dom->geometry.aperture_end) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u IOVA window [0x%llx, 0x%llx) does not fit IOMMU aperture [0x%llx, 0x%llx]\n",
			 vf_id, dom->base, dom->end,
			 dom->iommu_dom->geometry.aperture_start,
			 dom->iommu_dom->geometry.aperture_end);
		err = -EOPNOTSUPP;
		goto err_detach;
	}

	dom->vf_pdev = pci_dev_get(vf_pdev);

	dev_info(&vf_pdev->dev,
		 "vfmig_iova: vf %u domain attached, IOVA window [0x%llx, 0x%llx) within IOMMU aperture [0x%llx, 0x%llx]\n",
		 vf_id, dom->base, dom->end,
		 dom->iommu_dom->geometry.aperture_start,
		 dom->iommu_dom->geometry.aperture_end);

	*out = dom;
	return 0;

err_detach:
	iommu_detach_device(dom->iommu_dom, &vf_pdev->dev);

err_free_idom:
	iommu_domain_free(dom->iommu_dom);
err_free_dom:
	mutex_destroy(&dom->lock);
	kfree(dom);
	return err;
}

void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom)
{
	struct vfmig_iova_page *p, *tmp;
	struct pci_dev *vf_pdev;

	if (!dom)
		return;
	vf_pdev = dom->vf_pdev;

	mutex_lock(&dom->lock);
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

	mutex_destroy(&dom->lock);
	kfree(dom);
}

int vfmig_iova_alloc_coherent(struct vfmig_iova_domain *dom,
			      size_t size, gfp_t gfp,
			      dma_addr_t *iova_out, void **vaddr_out)
{
	struct vfmig_iova_page *p;
	size_t aligned;
	u64 iova;
	int err;

	if (!dom || !iova_out || !vaddr_out || size == 0)
		return -EINVAL;

	aligned = ALIGN(size, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);
	iova = dom->cursor;

	if (iova + aligned > dom->end) {
		err = -ENOSPC;
		goto out_unlock;
	}

	/*
	 * Lookup-or-alloc at the cursor. If an entry already exists at
	 * @iova it must have come from a prior replay; we expect the
	 * size to match what the source had at this allocation slot,
	 * but if it doesn't we error out rather than silently hand back
	 * a too-small or too-big mapping.
	 *
	 * Note: this lookup-by-cursor is the load-bearing piece of v1's
	 * order-based determinism shortcut. See the "Determinism
	 * contract" block at the top of vfmig_iova.h. The size check
	 * below is the one defensive cross-check we have without a slot
	 * identity; order drift between source and destination is NOT
	 * detected here -- a missing source-side allocation just shifts
	 * everything by one slot with all sizes still matching, and we'd
	 * silently hand the FW a buffer at the wrong IOVA. v2's slot
	 * store (vfmig_iova_alloc_slot) is the proper fix; landing it is
	 * a hard prerequisite for Layer 3 per the plan document.
	 */
	p = vfmig_iova_find_locked(dom, iova);
	if (p) {
		if (p->len != aligned) {
			dev_warn(&dom->vf_pdev->dev,
				 "vfmig_iova: vf %u replay/alloc size mismatch at IOVA 0x%llx: replayed %zu, requested %zu\n",
				 dom->vf_id, iova, p->len, aligned);
			err = -EINVAL;
			goto out_unlock;
		}
		*iova_out  = p->iova;
		*vaddr_out = p->vaddr;
		dom->cursor = iova + aligned;
		err = 0;
		goto out_unlock;
	}

	err = vfmig_iova_install_page_locked(dom, iova, aligned, gfp, &p);
	if (err)
		goto out_unlock;

	dom->cursor = iova + aligned;
	*iova_out  = p->iova;
	*vaddr_out = p->vaddr;
	err = 0;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

void vfmig_iova_free_coherent(struct vfmig_iova_domain *dom,
			      dma_addr_t iova, size_t size)
{
	struct vfmig_iova_page *p;
	size_t aligned;

	if (!dom)
		return;
	aligned = ALIGN(size, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);
	p = vfmig_iova_find_locked(dom, iova);
	if (!p) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u free of unknown IOVA 0x%llx\n",
			 dom->vf_id, (u64)iova);
		goto out_unlock;
	}
	if (p->len != aligned) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u free size mismatch at IOVA 0x%llx: have %zu, asked %zu\n",
			 dom->vf_id, (u64)iova, p->len, aligned);
		/* still proceed: the entry is what it is */
	}
	list_del(&p->node);
	dom->n_pages--;
	vfmig_iova_destroy_page_locked(dom, p);

out_unlock:
	mutex_unlock(&dom->lock);
}

int vfmig_iova_replay_page(struct vfmig_iova_domain *dom,
			   dma_addr_t iova, const void *contents,
			   size_t len)
{
	struct vfmig_iova_page *p;
	int err;

	if (!dom || !contents)
		return -EINVAL;

	mutex_lock(&dom->lock);
	err = vfmig_iova_install_page_locked(dom, iova, len, GFP_KERNEL, &p);
	if (err)
		goto out_unlock;

	memcpy(p->vaddr, contents, len);

	/*
	 * Push the cursor past the highest-replayed IOVA so a later
	 * vfmig_iova_reset_cursor() resets to base, not "ahead of
	 * everything"; and so that if no reset is issued, fresh allocs
	 * still don't collide with replayed ranges.
	 */
	if (iova + len > dom->cursor)
		dom->cursor = iova + len;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

void vfmig_iova_reset_cursor(struct vfmig_iova_domain *dom)
{
	if (!dom)
		return;
	mutex_lock(&dom->lock);
	dom->cursor = dom->base;
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
		ret = cb(p->iova, p->vaddr, p->len, ctx);
		if (ret)
			break;
	}
	mutex_unlock(&dom->lock);
	return ret;
}
