/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * vfmig_dma_ops: per-VF custom dma_map_ops shim that intercepts
 * dma_map_sgtable / dma_map_phys on a tracked VF and routes them
 * through the per-VF unmanaged iommu_domain owned by vfmig_iova.
 *
 * Why
 * ---
 * v1 of the deterministic IOVA allocator covers only kernel-side
 * mlx5_core call sites that were converted to vfmig_iova_alloc_slot
 * (cmd ring, MANAGE_PAGES, EQs, doorbells, etc.). User-side
 * registrations (ib_umem_get -> dma_map_sgtable, used for MR / CQ /
 * QP / SRQ buffers + doorbell records) bypass that allocator and
 * land on the dma-iommu fast path, which maps against the *default*
 * DMA domain rather than our unmanaged domain. The hardware then
 * dereferences IOVAs that were never installed in the domain we
 * detached from when SET_TRACKED enabled tracking, and every umem-
 * backed RDMA op fails with IB_WC_MW_BIND_ERR.
 *
 * The fix this file implements: while a VF is tracked, install a
 * custom dma_map_ops on its struct device that:
 *   - routes .map_sg / .unmap_sg through the per-VF iommu_domain
 *     (vfmig_iova_user_page_map_phys / _unmap_phys), bumping the
 *     USER_PAGE slot's expand-to-fill cursor;
 *   - rejects .map_phys with DMA_ATTR_MMIO in stage 1 (peer-to-
 *     peer dma-buf is future work);
 *   - leaves .alloc / .free unimplemented so that any unconverted
 *     dma_alloc_coherent call site fails-loud rather than silently
 *     stashing a non-deterministic IOVA.
 *
 * The shim must also clear dev->dma_iommu while attached:
 * __dma_map_sg_attrs() checks use_dma_iommu(dev) -- which reads
 * dev->dma_iommu directly -- before falling through to ops->map_sg,
 * so without that override our ops are silently bypassed. Original
 * value is saved at attach time and restored on detach.
 *
 * Lifetime is tied to vfmig_iova_domain_create / _destroy: the shim
 * is installed right after iommu_attach_device and uninstalled just
 * before iommu_detach_device, so the (custom dma_ops, custom iommu
 * domain) pairing is atomic from the DMA layer's point of view.
 *
 * See tools/testing/mlx5_vfmig/design/user_mr_dma.md for the
 * three-stage plan and the wire-format / replay design that builds
 * on top of this shim.
 */

#ifndef __MLX5_CORE_VFMIG_DMA_OPS_H__
#define __MLX5_CORE_VFMIG_DMA_OPS_H__

#include <linux/types.h>

struct pci_dev;
struct vfmig_iova_domain;

#if IS_ENABLED(CONFIG_MLX5_VFMIG)

/*
 * Install vfmig_dma_ops on @vf_pdev->dev and register the (dev, dom)
 * association so the dma_map_ops callbacks can find their domain.
 *
 * Caller has already iommu_attach_device'd @dom->iommu_dom; this
 * function takes over the DMA-layer dispatch on top.
 *
 * Returns 0 on success. On failure no state is changed and the
 * caller's iommu_attach is still in effect.
 *
 * Errors:
 *   -EOPNOTSUPP	the architecture doesn't honour set_dma_ops
 *			(CONFIG_ARCH_HAS_DMA_OPS=n). The check is
 *			runtime so the same kernel binary works on
 *			ARCH_HAS_DMA_OPS-on builds and degrades
 *			cleanly elsewhere.
 *   -EBUSY		@vf_pdev already has a vfmig_dma_ops shim
 *			attached (refcounting bug in the caller)
 *   -ENOMEM		xa_store failed
 */
int  vfmig_dma_ops_attach(struct pci_dev *vf_pdev,
			  struct vfmig_iova_domain *dom);

/*
 * Reverse of vfmig_dma_ops_attach: restore the original dma_iommu
 * value, restore the original dev->dma_ops (typically NULL), and
 * drop the (dev, dom) association.
 *
 * Safe with @vf_pdev == NULL or with no prior attach (no-op + warn).
 */
void vfmig_dma_ops_detach(struct pci_dev *vf_pdev);

#else /* !CONFIG_MLX5_VFMIG */

static inline int  vfmig_dma_ops_attach(struct pci_dev *vf_pdev,
					struct vfmig_iova_domain *dom)
{
	return -EOPNOTSUPP;
}
static inline void vfmig_dma_ops_detach(struct pci_dev *vf_pdev) { }

#endif /* CONFIG_MLX5_VFMIG */

#endif /* __MLX5_CORE_VFMIG_DMA_OPS_H__ */
