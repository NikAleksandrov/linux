/*
 * Copyright (c) 2013-2015, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>
#include <rdma/ib_umem.h>

#include "mlx5_ib.h"

struct mlx5_ib_user_db_page {
	struct list_head	list;
	struct ib_umem	       *umem;
	unsigned long		user_virt;
	int			refcnt;
	struct mm_struct	*mm;
};

int mlx5_ib_db_map_user(struct mlx5_ib_ucontext *context, unsigned long virt,
			struct mlx5_db *db)
{
	struct mlx5_ib_user_db_page *page;
	struct mlx5_ib_dev *dev = to_mdev(context->ibucontext.device);
	int err = 0;

	mutex_lock(&context->db_page_mutex);

	list_for_each_entry(page, &context->db_page_list, list)
		if ((current->mm == page->mm) &&
		    (page->user_virt == (virt & PAGE_MASK)))
			goto found;

	page = kmalloc(sizeof(*page), GFP_KERNEL);
	if (!page) {
		err = -ENOMEM;
		goto out;
	}

	page->user_virt = (virt & PAGE_MASK);
	page->refcnt    = 0;
	page->umem = ib_umem_get(context->ibucontext.device, virt & PAGE_MASK,
				 PAGE_SIZE, 0);
	if (IS_ERR(page->umem)) {
		err = PTR_ERR(page->umem);
		kfree(page);
		goto out;
	}
	mmgrab(current->mm);
	page->mm = current->mm;

	list_add(&page->list, &context->db_page_list);

	/*
	 * Stage-2 source-side retag for vfmig-tracked VFs (user_mr_dma.md
	 * §6 + §A.E). Only the miss branch needs to retag: ib_umem_get
	 * above is the only path that hands a fresh PAGE_SIZE umem to
	 * vfmig_dma_ops.map_sg, and that's where the KIND_NONE registry
	 * entry was just planted. The 'found' branch just bumps a refcount
	 * on a page we already retagged at its first install.
	 *
	 * Same callsite gate as create_real_mr's MR retag: a single
	 * lock-free load on dev->mdev->cmd.vfmig_iova_dom decides whether
	 * the underlying ucontext lives on a vfmig-tracked VF. NULL on
	 * PFs and on non-vfmig VFs, so non-vfmig deployments skip the
	 * iova_base compute and the helper call entirely -- no PF
	 * intf_state_mutex contention on user_db_page allocation.
	 *
	 * fw_id encoding is the userspace VA of the page (mlx5_ib_db_map's
	 * own dedup key), not a FW identifier -- DBR pages have no FW
	 * identity. See the docstring on mlx5_vfmig_retag_user_dbr in
	 * include/linux/mlx5/driver.h for the design rationale.
	 *
	 * Non-zero return is non-fatal: the CQ/QP/SRQ create that triggered
	 * this map continues; the resource is fully usable on the data
	 * path, just not CRIU-restorable. We log a single warning with
	 * the offending user_virt + IOVA range.
	 */
	if (dev->mdev->cmd.vfmig_iova_dom) {
		struct sg_table *sgt = &page->umem->sgt_append.sgt;
		dma_addr_t iova_base = sg_dma_address(sgt->sgl) & PAGE_MASK;
		int retag_err;

		retag_err = mlx5_vfmig_retag_user_dbr(dev->mdev,
						     page->user_virt,
						     iova_base, PAGE_SIZE);
		if (retag_err)
			mlx5_ib_warn(dev,
				"vfmig: source-side retag for DBR page failed: user_virt=0x%lx iova_base=0x%llx err=%d -- DBR page usable but not CRIU-restorable\n",
				page->user_virt, (u64)iova_base, retag_err);
	}

found:
	db->dma = sg_dma_address(page->umem->sgt_append.sgt.sgl) +
		  (virt & ~PAGE_MASK);
	db->u.user_page = page;
	++page->refcnt;

out:
	mutex_unlock(&context->db_page_mutex);

	return err;
}

void mlx5_ib_db_unmap_user(struct mlx5_ib_ucontext *context, struct mlx5_db *db)
{
	mutex_lock(&context->db_page_mutex);

	if (!--db->u.user_page->refcnt) {
		list_del(&db->u.user_page->list);
		mmdrop(db->u.user_page->mm);
		ib_umem_release(db->u.user_page->umem);
		kfree(db->u.user_page);
	}

	mutex_unlock(&context->db_page_mutex);
}

/*
 * Read-only accessor for the page-aligned source userspace VA of the
 * doorbell-page user_page that backs @db. Returns 0 for kernel-mode db
 * slots (db->u.pgdir branch -- no user_page). Used by
 * MLX5_IB_METHOD_VFMIG_QUERY_CQ (and future per-uobject QUERY verbs)
 * to round-trip the source VA into struct mlx5_ib_restore_*_req.db_addr
 * without exposing struct mlx5_ib_user_db_page across translation units.
 *
 * Returns u64 to match the UAPI seam (struct mlx5_ib_restore_*_req
 * .db_addr is __aligned_u64); page->user_virt is unsigned long for
 * kernel-internal reasons (mlx5_ib_db_map_user / ib_umem.address /
 * the doorbell dedup key) and is widened here at the export point.
 *
 * No locking: db->u.user_page is set once at uobject create / restore
 * time and cleared in mlx5_ib_db_unmap_user only after the parent
 * uobject's destroy verb has already serialised against any other
 * users. Callers reach @db through a UVERBS_ACCESS_READ-pinned CQ /
 * QP / SRQ uobject so the parent stays alive for the duration of the
 * read.
 */
u64 mlx5_ib_db_user_virt(const struct mlx5_db *db)
{
	return db->u.user_page ? (u64)db->u.user_page->user_virt : 0;
}

/*
 * Restore-time variant of mlx5_ib_db_map_user. The fast-path (cache
 * hit) is identical to mlx5_ib_db_map_user: a previously-restored
 * uobject in the same ucontext already pinned this DBR page and
 * bound it to the destination's KIND_DBR placeholder, so we just
 * refcount up and reuse db->u.user_page / db->dma. The miss-path
 * differs from the fresh-create variant in two ways:
 *
 *   1. It uses ib_umem_pin() (no DMA mapping) instead of
 *      ib_umem_get() (DMA-mapped via vfmig_dma_ops.map_sg). This is
 *      the same asymmetry as mlx5_ib_umem_restore_mr vs the create-
 *      time mlx5_ib_reg_user_mr: at restore time the IOVA must
 *      match what LOAD_VHCA_STATE installed for the placeholder
 *      (not a freshly-allocated one), which the explicit
 *      mlx5_vfmig_bind_user_dbr handles synchronously.
 *
 *   2. It calls mlx5_vfmig_bind_user_dbr() instead of relying on
 *      mlx5_ib_db_map_user's source-side retag. The bind helper
 *      consumes the awaiting_bind=true placeholder LOAD_VHCA_STATE
 *      replayed at HOST_USER_PAGE time keyed by
 *      VFMIG_HUOBJ_KEY(KIND_DBR, virt & PAGE_MASK) -- exactly the
 *      key the SAVE-side mlx5_vfmig_retag_user_dbr emitted.
 *
 * The miss-path bind is a hard prerequisite for FW data-path use of
 * the doorbell: the FW writes 8-byte doorbell records via the IOVA
 * encoded in cqc.dbr_addr / qpc.dbr_umem_id; that address points at
 * the placeholder's IOVA range, and unbound placeholders hard-fail
 * the Stage-3 D2 invariant. Bind failure here therefore propagates
 * to the verb body and aborts the restore.
 *
 * The dedup machinery (db_page_list per-ucontext list) treats hits
 * identically to mlx5_ib_db_map_user: refcount++ and reuse the
 * existing umem + dma. The first restored uobject (CQ / QP / SRQ)
 * pointing at a given DBR page does the bind; subsequent ones share
 * the bound umem. No "clear on cache hit" cleanup is needed because
 * nothing was set in the hit path.
 *
 * Lifetime: the resulting db is matched 1-to-1 by mlx5_ib_db_unmap_user
 * (no parallel restore variant required); the unmap path's
 * ib_umem_release fires the standard vfmig_dma_ops.unmap_sg unbind
 * which honours the bound iova range.
 */
int mlx5_ib_db_map_user_restore(struct mlx5_ib_ucontext *context,
				unsigned long virt, struct mlx5_db *db)
{
	struct mlx5_ib_user_db_page *page;
	struct mlx5_ib_dev *dev = to_mdev(context->ibucontext.device);
	int err = 0;

	mutex_lock(&context->db_page_mutex);

	list_for_each_entry(page, &context->db_page_list, list)
		if ((current->mm == page->mm) &&
		    (page->user_virt == (virt & PAGE_MASK)))
			goto found;

	page = kmalloc(sizeof(*page), GFP_KERNEL);
	if (!page) {
		err = -ENOMEM;
		goto out;
	}

	page->user_virt = (virt & PAGE_MASK);
	page->refcnt    = 0;
	page->umem = ib_umem_pin(context->ibucontext.device, virt & PAGE_MASK,
				 PAGE_SIZE, 0);
	if (IS_ERR(page->umem)) {
		err = PTR_ERR(page->umem);
		kfree(page);
		goto out;
	}

	err = mlx5_vfmig_bind_user_dbr(dev->mdev, page->user_virt,
				       &page->umem->sgt_append.sgt);
	if (err) {
		ib_umem_release(page->umem);
		kfree(page);
		goto out;
	}

	mmgrab(current->mm);
	page->mm = current->mm;

	list_add(&page->list, &context->db_page_list);

found:
	db->dma = sg_dma_address(page->umem->sgt_append.sgt.sgl) +
		  (virt & ~PAGE_MASK);
	db->u.user_page = page;
	++page->refcnt;

out:
	mutex_unlock(&context->db_page_mutex);

	return err;
}
