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

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/export.h>
#include <linux/bitmap.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <linux/mlx5/driver.h>

#include "mlx5_core.h"
#include "vfmig/vfmig_iova.h"

struct mlx5_db_pgdir {
	struct list_head	list;
	unsigned long	       *bitmap;
	__be32		       *db_page;
	dma_addr_t		db_dma;
	/*
	 * The vfmig deterministic-IOVA slot the backing db_page came
	 * from, recorded at alloc time so the symmetric free routes
	 * back to the same slot. On a tracked VF this is always
	 * VFMIG_SLOT_DB_PAGE; on an untracked VF (vfmig_dom == NULL)
	 * it stays 0 / VFMIG_SLOT_INVALID and the free path falls
	 * through to dma_free_coherent like every other untracked
	 * allocation.
	 */
	enum vfmig_iova_slot	vfmig_slot;
};

/* Handling for queue buffers -- we allocate a bunch of memory and
 * register it in a memory region at HCA virtual address 0.
 */

/*
 * Slot-aware DMA-coherent allocation helper.
 *
 * On a tracked VF, every coherent host buffer (EQ frag pages,
 * doorbell pages, queue frag pages, ...) is routed through the
 * per-VF deterministic IOVA allocator. The @slot argument tells the
 * allocator which IOVA sub-window to draw from so addition or
 * removal of allocations in one consumer category cannot shift
 * IOVAs in another (e.g. adding an EQ doesn't move WQ IOVAs).
 *
 * Pre-3c, every caller funneled into VFMIG_SLOT_DMA_COHERENT and
 * relied on global ordering across consumers; that was fragile and
 * easy to break with kernel-version drift. 3c keeps DMA_COHERENT as
 * the legacy slot for callers that haven't been converted yet
 * (notably the unchanged exported mlx5_frag_buf_alloc_node /
 * mlx5_db_alloc_node ABI used by mlx5_ib, vfio_pci_mlx5, and vdpa)
 * and adds per-purpose slots (EQ_BUF, FRAG_BUF, DB_PAGE) for the
 * in-tree mlx5_core call sites.
 *
 * NUMA hint: ignored on the vfmig path. vfmig_iova_alloc_slot uses
 * alloc_pages() (no node hint), so we pay a potential NUMA-locality
 * cost to keep the deterministic-IOVA invariant. The buffers
 * involved are small and allocated once per probe; the hit is
 * negligible.
 *
 * On an untracked VF (vfmig_dom == NULL) @slot is ignored and the
 * legacy dma_alloc_coherent path runs unchanged.
 */
static void *mlx5_dma_zalloc_coherent_node(struct mlx5_core_dev *dev,
					   size_t size, dma_addr_t *dma_handle,
					   int node,
					   enum vfmig_iova_slot slot)
{
	struct vfmig_iova_domain *vfmig_dom = dev->cmd.vfmig_iova_dom;
	struct device *device = mlx5_core_dma_dev(dev);
	struct mlx5_priv *priv = &dev->priv;
	int original_node;
	void *cpu_handle;

	if (vfmig_dom) {
		dma_addr_t iova;
		void *vaddr;
		int err;

		err = vfmig_iova_alloc_slot(vfmig_dom, slot,
					    /*instance_key=*/0, size,
					    GFP_KERNEL, &iova, &vaddr);
		if (err) {
			mlx5_core_warn(dev,
				       "vfmig: dma_zalloc_coherent_node: vfmig_iova_alloc_slot(slot=%u, size=%zu): %d\n",
				       slot, size, err);
			return NULL;
		}
		memset(vaddr, 0, size);
		*dma_handle = iova;
		return vaddr;
	}

	mutex_lock(&priv->alloc_mutex);
	original_node = dev_to_node(device);
	set_dev_node(device, node);
	cpu_handle = dma_alloc_coherent(device, size, dma_handle,
					GFP_KERNEL);
	set_dev_node(device, original_node);
	mutex_unlock(&priv->alloc_mutex);
	return cpu_handle;
}

/*
 * Symmetric release helper for buffers obtained via
 * mlx5_dma_zalloc_coherent_node(). @slot must match the slot the
 * allocation came from -- callers either remember it via the
 * surrounding struct (mlx5_frag_buf::vfmig_slot,
 * mlx5_db_pgdir::vfmig_slot) or pass a literal that was hard-coded
 * at the call site.
 *
 * On an untracked VF (vfmig_dom == NULL) @slot is ignored and we
 * fall through to dma_free_coherent, matching the alloc-time
 * branching.
 *
 * For backward-compat with external-module callers that allocated
 * via the legacy exported wrapper (which always picks
 * VFMIG_SLOT_DMA_COHERENT), the calling code coerces @slot to
 * DMA_COHERENT when its remembered value is INVALID (the zero-init
 * default). See mlx5_frag_buf_free / mlx5_db_free for the coercion.
 */
static void mlx5_dma_free_coherent_node(struct mlx5_core_dev *dev,
					size_t size, void *cpu_handle,
					dma_addr_t dma_handle,
					enum vfmig_iova_slot slot)
{
	struct vfmig_iova_domain *vfmig_dom = dev->cmd.vfmig_iova_dom;

	if (vfmig_dom) {
		vfmig_iova_free_slot(vfmig_dom, slot, dma_handle, size);
		return;
	}
	dma_free_coherent(mlx5_core_dma_dev(dev), size, cpu_handle,
			  dma_handle);
}

/*
 * Slot-aware variant of mlx5_frag_buf_alloc_node, used by in-tree
 * mlx5_core call sites (eq.c, wq.c) to route the backing pages into
 * a per-purpose vfmig IOVA slot. The chosen slot is stamped onto
 * @buf so the symmetric mlx5_frag_buf_free can route the free back
 * to the same slot without an extra parameter (which would force an
 * exported-ABI change visible to mlx5_ib / vfio_pci_mlx5 / vdpa).
 */
int mlx5_frag_buf_alloc_node_slot(struct mlx5_core_dev *dev, int size,
				  struct mlx5_frag_buf *buf, int node,
				  enum vfmig_iova_slot slot)
{
	int i;

	buf->size = size;
	buf->npages = DIV_ROUND_UP(size, PAGE_SIZE);
	buf->page_shift = PAGE_SHIFT;
	buf->vfmig_slot = (u8)slot;
	buf->frags = kcalloc(buf->npages, sizeof(struct mlx5_buf_list),
			     GFP_KERNEL);
	if (!buf->frags)
		goto err_out;

	for (i = 0; i < buf->npages; i++) {
		struct mlx5_buf_list *frag = &buf->frags[i];
		int frag_sz = min_t(int, size, PAGE_SIZE);

		frag->buf = mlx5_dma_zalloc_coherent_node(dev, frag_sz,
							  &frag->map, node,
							  slot);
		if (!frag->buf)
			goto err_free_buf;
		if (frag->map & ((1 << buf->page_shift) - 1)) {
			mlx5_dma_free_coherent_node(dev, frag_sz,
						    buf->frags[i].buf,
						    buf->frags[i].map,
						    slot);
			mlx5_core_warn(dev, "unexpected map alignment: %pad, page_shift=%d\n",
				       &frag->map, buf->page_shift);
			goto err_free_buf;
		}
		size -= frag_sz;
	}

	return 0;

err_free_buf:
	while (i--)
		mlx5_dma_free_coherent_node(dev, PAGE_SIZE, buf->frags[i].buf,
					    buf->frags[i].map, slot);
	kfree(buf->frags);
err_out:
	return -ENOMEM;
}

/*
 * Backward-compat wrapper preserving the exported ABI used by
 * mlx5_ib / vfio_pci_mlx5 / vdpa. Routes the allocation through the
 * legacy DMA_COHERENT slot. In-tree mlx5_core code should use
 * mlx5_frag_buf_alloc_node_slot directly with a per-purpose slot.
 */
int mlx5_frag_buf_alloc_node(struct mlx5_core_dev *dev, int size,
			     struct mlx5_frag_buf *buf, int node)
{
	return mlx5_frag_buf_alloc_node_slot(dev, size, buf, node,
					     VFMIG_SLOT_DMA_COHERENT);
}
EXPORT_SYMBOL_GPL(mlx5_frag_buf_alloc_node);

void mlx5_frag_buf_free(struct mlx5_core_dev *dev, struct mlx5_frag_buf *buf)
{
	int size = buf->size;
	enum vfmig_iova_slot slot;
	int i;

	/*
	 * Coerce a zero-init / pre-3c slot back to DMA_COHERENT so an
	 * external-module caller that didn't go through the slot-aware
	 * alloc still routes its free into a non-INVALID slot. The
	 * vfmig_iova_free_slot path WARN_ON_ONCEs on INVALID, which is
	 * the right behaviour for a kernel bug but a confusing splat
	 * for the legitimate "external module zero-init" case.
	 */
	slot = buf->vfmig_slot ? buf->vfmig_slot : VFMIG_SLOT_DMA_COHERENT;

	for (i = 0; i < buf->npages; i++) {
		int frag_sz = min_t(int, size, PAGE_SIZE);

		mlx5_dma_free_coherent_node(dev, frag_sz, buf->frags[i].buf,
					    buf->frags[i].map, slot);
		size -= frag_sz;
	}
	kfree(buf->frags);
}
EXPORT_SYMBOL_GPL(mlx5_frag_buf_free);

static struct mlx5_db_pgdir *mlx5_alloc_db_pgdir(struct mlx5_core_dev *dev,
						 int node)
{
	u32 db_per_page = PAGE_SIZE / cache_line_size();
	struct mlx5_db_pgdir *pgdir;

	pgdir = kzalloc_node(sizeof(*pgdir), GFP_KERNEL, node);
	if (!pgdir)
		return NULL;

	pgdir->bitmap = bitmap_zalloc_node(db_per_page, GFP_KERNEL, node);
	if (!pgdir->bitmap) {
		kfree(pgdir);
		return NULL;
	}

	bitmap_fill(pgdir->bitmap, db_per_page);

	/*
	 * All doorbell pgdir pages -- regardless of which subsystem
	 * (mlx5_core wq.c, mlx5_ib, vfio_pci_mlx5, vdpa) triggered the
	 * outer mlx5_db_alloc_node call -- land in the dedicated
	 * DB_PAGE slot on a tracked VF. The page is the same kind of
	 * resource regardless of who asked, and a single per-purpose
	 * slot keeps adding/removing a doorbell consumer from
	 * perturbing the IOVAs of unrelated slots (EQ_BUF, FRAG_BUF).
	 */
	pgdir->vfmig_slot = VFMIG_SLOT_DB_PAGE;
	pgdir->db_page = mlx5_dma_zalloc_coherent_node(dev, PAGE_SIZE,
						       &pgdir->db_dma, node,
						       pgdir->vfmig_slot);
	if (!pgdir->db_page) {
		bitmap_free(pgdir->bitmap);
		kfree(pgdir);
		return NULL;
	}

	return pgdir;
}

static int mlx5_alloc_db_from_pgdir(struct mlx5_db_pgdir *pgdir,
				    struct mlx5_db *db)
{
	u32 db_per_page = PAGE_SIZE / cache_line_size();
	int offset;
	int i;

	i = find_first_bit(pgdir->bitmap, db_per_page);
	if (i >= db_per_page)
		return -ENOMEM;

	__clear_bit(i, pgdir->bitmap);

	db->u.pgdir = pgdir;
	db->index   = i;
	offset = db->index * cache_line_size();
	db->db      = pgdir->db_page + offset / sizeof(*pgdir->db_page);
	db->dma     = pgdir->db_dma  + offset;

	db->db[0] = 0;
	db->db[1] = 0;

	return 0;
}

int mlx5_db_alloc_node(struct mlx5_core_dev *dev, struct mlx5_db *db, int node)
{
	struct mlx5_db_pgdir *pgdir;
	int ret = 0;

	mutex_lock(&dev->priv.pgdir_mutex);

	list_for_each_entry(pgdir, &dev->priv.pgdir_list, list)
		if (!mlx5_alloc_db_from_pgdir(pgdir, db))
			goto out;

	pgdir = mlx5_alloc_db_pgdir(dev, node);
	if (!pgdir) {
		ret = -ENOMEM;
		goto out;
	}

	list_add(&pgdir->list, &dev->priv.pgdir_list);

	/* This should never fail -- we just allocated an empty page: */
	WARN_ON(mlx5_alloc_db_from_pgdir(pgdir, db));

out:
	mutex_unlock(&dev->priv.pgdir_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(mlx5_db_alloc_node);

void mlx5_db_free(struct mlx5_core_dev *dev, struct mlx5_db *db)
{
	u32 db_per_page = PAGE_SIZE / cache_line_size();

	mutex_lock(&dev->priv.pgdir_mutex);

	__set_bit(db->index, db->u.pgdir->bitmap);

	if (bitmap_full(db->u.pgdir->bitmap, db_per_page)) {
		mlx5_dma_free_coherent_node(dev, PAGE_SIZE,
					    db->u.pgdir->db_page,
					    db->u.pgdir->db_dma,
					    db->u.pgdir->vfmig_slot ?
					    db->u.pgdir->vfmig_slot :
					    VFMIG_SLOT_DMA_COHERENT);
		list_del(&db->u.pgdir->list);
		bitmap_free(db->u.pgdir->bitmap);
		kfree(db->u.pgdir);
	}

	mutex_unlock(&dev->priv.pgdir_mutex);
}
EXPORT_SYMBOL_GPL(mlx5_db_free);

void mlx5_fill_page_frag_array_perm(struct mlx5_frag_buf *buf, __be64 *pas, u8 perm)
{
	int i;

	WARN_ON(perm & 0xfc);
	for (i = 0; i < buf->npages; i++)
		pas[i] = cpu_to_be64(buf->frags[i].map | perm);
}
EXPORT_SYMBOL_GPL(mlx5_fill_page_frag_array_perm);

void mlx5_fill_page_frag_array(struct mlx5_frag_buf *buf, __be64 *pas)
{
	mlx5_fill_page_frag_array_perm(buf, pas, 0);
}
EXPORT_SYMBOL_GPL(mlx5_fill_page_frag_array);
