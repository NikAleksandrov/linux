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

#include <rdma/ib_umem_odp.h>
#include "mlx5_ib.h"

/*
 * Stage-3 D3: pin a user MR's backing pages and bind them to the
 * Stage-2 placeholder that LOAD_VHCA_STATE-replayed HOST_USER_PAGE
 * records installed for the source's (KIND_MR, mkey_index) tuple.
 *
 * Composition (design doc §A.C):
 *
 *   1. ib_umem_pin(&dev->ib_dev, addr, size, access) -- pins user
 *      pages and builds the sg_append_table without calling
 *      dma_map_sgtable. The umem is returned in a "pinned but not
 *      DMA-mapped" state.
 *   2. mlx5_vfmig_bind_user_mr(dev->mdev, mkey_index, sgt) --
 *      iommu_maps each sg at consecutive IOVAs starting at the
 *      placeholder's recorded IOVA base, populates sg_dma_address /
 *      sg_dma_len so subsequent vfmig_dma_ops.unmap_sg under
 *      ib_umem_release() can match each entry, and transitions
 *      placeholder.awaiting_bind=true -> false.
 *
 * On bind failure (-ENOENT placeholder miss / -EBUSY double-bind /
 * -EINVAL sgt mismatch / iommu_map errno), the umem is unwound via
 * ib_umem_release(). Per design §A.H L2 this is safe on a tracked VF
 * even when sg_dma_address is partially populated: the underlying
 * vfmig_dma_ops.unmap_sg path skips sgs whose sg_dma_address is zero
 * (vfmig_dma_ops.c::vfmig_dma_ops_unmap_sg), so the un-bound sgs
 * are no-ops and the partially-bound ones (whose iommu_map this
 * helper *did* land before failing) match registry entries already
 * unmapped by mlx5_vfmig_bind_user_mr()'s rollback path. The net
 * effect is exactly one iommu_map / unmap pair per partially-bound
 * sg, identical to the success-then-release lifecycle.
 *
 * Caller (mlx5_ib_restore_mr) responsibilities:
 *   - Gate on context->vfmig_restore_mode + a tracked-VF ucontext
 *     before calling. A non-tracked VF reaches this helper only via
 *     direct driver-internal misuse; -ENODEV surfaces from the
 *     mlx5_vfmig_bind_user_mr fast-path gate so the verb fails
 *     loudly rather than silently leaking pins.
 *   - mkey_index must match what the SAVE-side retag emitted as the
 *     HOST_USER_PAGE record's fw_id (== source mkey >> 8).
 *
 * Returns the populated struct ib_umem on success (the caller stores
 * it in mr->umem); ERR_PTR on any failure with all resources
 * released.
 */
struct ib_umem *mlx5_ib_umem_restore_mr(struct mlx5_ib_dev *dev,
					u32 mkey_index, unsigned long addr,
					size_t size, int access)
{
	struct ib_umem *umem;
	int err;

	umem = ib_umem_pin(&dev->ib_dev, addr, size, access);
	if (IS_ERR(umem))
		return umem;

	err = mlx5_vfmig_bind_user_mr(dev->mdev, mkey_index,
				      &umem->sgt_append.sgt);
	if (err) {
		ib_umem_release(umem);
		return ERR_PTR(err);
	}
	return umem;
}

/*
 * Stage-3 D3: pin a user CQ's CQE-buffer pages and bind them to the
 * Stage-2 placeholder that LOAD_VHCA_STATE-replayed HOST_USER_PAGE
 * records installed for the source's (KIND_CQ, cqn) tuple.
 *
 * Identical composition shape to mlx5_ib_umem_restore_mr modulo the
 * kind enum:
 *
 *   1. ib_umem_pin(&dev->ib_dev, addr, size, IB_ACCESS_LOCAL_WRITE)
 *      -- pins user pages and builds the sg_append_table without
 *      DMA-mapping. Access is IB_ACCESS_LOCAL_WRITE because the FW
 *      writes CQEs into the buffer; matches the source-side
 *      mlx5_ib_create_cq's ib_umem_get(... IB_ACCESS_LOCAL_WRITE).
 *   2. mlx5_vfmig_bind_user_cq(dev->mdev, cqn, sgt) -- iommu_maps
 *      each sg at consecutive IOVAs starting at the placeholder's
 *      recorded IOVA base, populates sg_dma_address / sg_dma_len so
 *      subsequent vfmig_dma_ops.unmap_sg under ib_umem_release()
 *      can match each entry, and transitions placeholder
 *      awaiting_bind=true -> false.
 *
 * On bind failure (-ENOENT placeholder miss / -EBUSY double-bind /
 * -EINVAL sgt mismatch / iommu_map errno), the umem is unwound via
 * ib_umem_release(). Per design §A.H L2 the partial-bind unwind is
 * safe (vfmig_dma_ops.unmap_sg skips zero-iova sgs).
 *
 * Caller (mlx5_ib_restore_cq) responsibilities:
 *   - Gate on context->vfmig_restore_mode + a tracked-VF ucontext
 *     before calling. A non-tracked VF reaches this helper only via
 *     direct driver-internal misuse; -ENODEV surfaces from the
 *     mlx5_vfmig_bind_user_cq fast-path gate so the verb fails
 *     loudly rather than silently leaking pins.
 *   - cqn must match what the SAVE-side retag emitted as the
 *     HOST_USER_PAGE record's fw_id (== source cqn).
 *
 * Returns the populated struct ib_umem on success (the caller
 * stores it in cq->buf.umem); ERR_PTR on any failure with all
 * resources released.
 */
struct ib_umem *mlx5_ib_umem_restore_cq(struct mlx5_ib_dev *dev, u32 cqn,
					unsigned long addr, size_t size)
{
	struct ib_umem *umem;
	int err;

	umem = ib_umem_pin(&dev->ib_dev, addr, size, IB_ACCESS_LOCAL_WRITE);
	if (IS_ERR(umem))
		return umem;

	err = mlx5_vfmig_bind_user_cq(dev->mdev, cqn,
				      &umem->sgt_append.sgt);
	if (err) {
		ib_umem_release(umem);
		return ERR_PTR(err);
	}
	return umem;
}

/*
 * Stage-3 D3: pin a user QP's WQ-ring (RQ + SQ in one contiguous
 * mapping) pages and bind them to the Stage-2 placeholder that
 * LOAD_VHCA_STATE-replayed HOST_USER_PAGE records installed for the
 * source's (KIND_QP, qpn) tuple.
 *
 * Identical composition shape to mlx5_ib_umem_restore_cq modulo the
 * kind enum (VFMIG_HUOBJ_KIND_QP) and the FW-id semantics (qpn
 * instead of cqn):
 *
 *   1. ib_umem_pin(&dev->ib_dev, addr, size, 0) -- pins user pages
 *      and builds the sg_append_table without DMA-mapping. Access
 *      is 0 because the FW reads WQE descriptors out of the buffer
 *      (matching the source-side mlx5_ib_create_qp's _create_user_qp
 *      ib_umem_get(... 0)). The SQ doorbell + RQ doorbell mechanism
 *      writes 8-byte records into the doorbell-page umem (handled
 *      separately by mlx5_ib_db_map_user_restore), not the WQ-ring
 *      umem.
 *   2. mlx5_vfmig_bind_user_qp(dev->mdev, qpn, sgt) -- iommu_maps
 *      each sg at consecutive IOVAs starting at the placeholder's
 *      recorded IOVA base, populates sg_dma_address / sg_dma_len so
 *      subsequent vfmig_dma_ops.unmap_sg under ib_umem_release()
 *      can match each entry, and transitions placeholder
 *      awaiting_bind=true -> false.
 *
 * The caller (mlx5_ib_restore_qp) supplies @size = (rq_wqe_count <<
 * rq_wqe_shift) + (sq_wqe_count << ilog2(MLX5_SEND_WQE_BB)), the
 * same byte length _create_user_qp's set_user_buf_size composes for
 * QPC-managed (RC / UC / UD) QPs. A future S6c extension that
 * surfaces RAW_PACKET will need a separate split-SQ helper because
 * raw_packet QPs live in two umems (qp->raw_packet_qp.{sq,rq}.
 * ubuffer.umem); this helper is QPC-only by construction.
 *
 * On bind failure (-ENOENT placeholder miss / -EBUSY double-bind /
 * -EINVAL sgt mismatch / iommu_map errno), the umem is unwound via
 * ib_umem_release(). Per design §A.H L2 the partial-bind unwind is
 * safe (vfmig_dma_ops.unmap_sg skips zero-iova sgs).
 *
 * Caller (mlx5_ib_restore_qp) responsibilities:
 *   - Gate on context->vfmig_restore_mode + a tracked-VF ucontext
 *     before calling. A non-tracked VF reaches this helper only via
 *     direct driver-internal misuse; -ENODEV surfaces from the
 *     mlx5_vfmig_bind_user_qp fast-path gate so the verb fails
 *     loudly rather than silently leaking pins.
 *   - qpn must match what the SAVE-side retag emitted as the
 *     HOST_USER_PAGE record's fw_id (== source qpn).
 *
 * Returns the populated struct ib_umem on success (the caller stores
 * it in qp->trans_qp.base.ubuffer.umem); ERR_PTR on any failure with
 * all resources released.
 */
struct ib_umem *mlx5_ib_umem_restore_qp(struct mlx5_ib_dev *dev, u32 qpn,
					unsigned long addr, size_t size)
{
	struct ib_umem *umem;
	int err;

	umem = ib_umem_pin(&dev->ib_dev, addr, size, 0);
	if (IS_ERR(umem))
		return umem;

	err = mlx5_vfmig_bind_user_qp(dev->mdev, qpn,
				      &umem->sgt_append.sgt);
	if (err) {
		ib_umem_release(umem);
		return ERR_PTR(err);
	}
	return umem;
}

/*
 * Fill in a physical address list. ib_umem_num_dma_blocks() entries will be
 * filled in the pas array.
 */
void mlx5_ib_populate_pas(struct ib_umem *umem, size_t page_size, __be64 *pas,
			  u64 access_flags)
{
	struct ib_block_iter biter;

	rdma_umem_for_each_dma_block (umem, &biter, page_size) {
		*pas = cpu_to_be64(rdma_block_iter_dma_address(&biter) |
				   access_flags);
		pas++;
	}
}

/*
 * Compute the page shift and page_offset for mailboxes that use a quantized
 * page_offset. The granulatity of the page offset scales according to page
 * size.
 */
unsigned long __mlx5_umem_find_best_quantized_pgoff(
	struct ib_umem *umem, unsigned long pgsz_bitmap,
	unsigned int page_offset_bits, u64 pgoff_bitmask, unsigned int scale,
	unsigned int *page_offset_quantized)
{
	const u64 page_offset_mask = (1UL << page_offset_bits) - 1;
	unsigned long page_size;
	u64 page_offset;

	page_size = ib_umem_find_best_pgoff(umem, pgsz_bitmap, pgoff_bitmask);
	if (!page_size)
		return 0;

	/*
	 * page size is the largest possible page size.
	 *
	 * Reduce the page_size, and thus the page_offset and quanta, until the
	 * page_offset fits into the mailbox field. Once page_size < scale this
	 * loop is guaranteed to terminate.
	 */
	page_offset = ib_umem_dma_offset(umem, page_size);
	while (page_offset & ~(u64)(page_offset_mask * (page_size / scale))) {
		page_size /= 2;
		page_offset = ib_umem_dma_offset(umem, page_size);
	}

	/*
	 * The address is not aligned, or otherwise cannot be represented by the
	 * page_offset.
	 */
	if (!(pgsz_bitmap & page_size))
		return 0;

	*page_offset_quantized =
		(unsigned long)page_offset / (page_size / scale);
	if (WARN_ON(*page_offset_quantized > page_offset_mask))
		return 0;
	return page_size;
}
