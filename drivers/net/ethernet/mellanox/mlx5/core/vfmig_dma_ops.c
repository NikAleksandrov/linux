// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
// Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

/*
 * vfmig_dma_ops: per-VF dma_map_ops shim. See vfmig_dma_ops.h for
 * the why / lifetime; this file is the implementation.
 *
 * State association
 * -----------------
 * Each tracked VF's struct device gets entered into a global xarray
 * keyed by (unsigned long)dev. The dma_map_ops callbacks look up the
 * vfmig_iova_domain via xa_load(&vfmig_dma_ops_xa, (unsigned long)dev).
 * The xarray is small (one entry per tracked VF, bounded by SR-IOV
 * VF count) and accessed under RCU, so the dispatch overhead is a
 * single dependent load on the hot path.
 *
 * Why a global xarray and not e.g. dev->dma_priv: there is no
 * standard DMA-layer per-device opaque pointer. dev->iommu->priv is
 * owned by the iommu subsystem; dev_get_drvdata is for drivers; we
 * pick xarray as the least-intrusive option. dev_t / pci_dev are
 * stable for the duration of the (attach, detach) interval -- the
 * caller (vfmig_iova_domain_create / _destroy) holds device_lock on
 * the VF while attaching/detaching and the VF is unbound throughout.
 *
 * dev->dma_iommu override
 * -----------------------
 * __dma_map_sg_attrs() checks use_dma_iommu(dev) -> dev->dma_iommu
 * BEFORE falling through to ops->map_sg. If we don't clear that flag
 * our ops are silently bypassed. We save the original value on
 * attach (@orig_dma_iommu in struct vfmig_dma_ops_priv) and restore
 * it on detach so dma-iommu transparently resumes ownership when the
 * VF is untracked.
 *
 * Stage scope
 * -----------
 * Stage 1 (this file): map_sg / unmap_sg / map_phys / unmap_phys /
 * sync_* / dma_supported / get_required_mask. .alloc and .free are
 * intentionally NULL: the only kernel call sites that would dispatch
 * .alloc are the unconverted dma_alloc_coherent call sites that v1
 * already detects as "alloc against unmanaged domain" -- they fail
 * loudly, which is exactly what we want.
 *
 * Stage 2 (next PR) extends the (iova, len, phys) registry tracking
 * with awaiting_bind support so a destination's .map_sg can consume
 * a pre-replayed registry entry instead of allocating fresh IOVAs.
 *
 * Stage 3 (later PR) adds the source-side mkey retag step so that
 * cross-host LOAD can match by mkey identity rather than by cursor
 * position; that's a vfmig_iova-side change with no impact on this
 * shim.
 */

#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/iommu-dma.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include "mlx5_core.h"
#include "vfmig_dma_ops.h"
#include "vfmig_iova.h"

/*
 * Per-attach saved state. Lives in the xarray, keyed by
 * (unsigned long)dev. Allocated on attach, freed on detach.
 *
 * @dom is the back-pointer the dma_map_ops callbacks chase.
 * @orig_dma_iommu / @orig_dma_ops are the pre-attach values used to
 * undo set_dma_ops + dma_iommu = false on detach.
 */
struct vfmig_dma_ops_priv {
	struct vfmig_iova_domain	*dom;
	const struct dma_map_ops	*orig_dma_ops;
#ifdef CONFIG_IOMMU_DMA
	bool				 orig_dma_iommu;
#endif
};

/*
 * Per-arch helpers for the @dev->dma_iommu override. The struct
 * field itself is conditionally compiled under CONFIG_IOMMU_DMA
 * (see include/linux/device.h); on !IOMMU_DMA builds use_dma_iommu()
 * is hard-coded to false, so there is nothing for us to clobber and
 * these become no-ops.
 */
static inline bool vfmig_dma_iommu_read(struct device *dev)
{
#ifdef CONFIG_IOMMU_DMA
	return dev->dma_iommu;
#else
	return false;
#endif
}

static inline void vfmig_dma_iommu_write(struct device *dev, bool val)
{
#ifdef CONFIG_IOMMU_DMA
	dev->dma_iommu = val;
#else
	(void)dev; (void)val;
#endif
}

static DEFINE_XARRAY(vfmig_dma_ops_xa);

static inline struct vfmig_dma_ops_priv *
vfmig_dma_ops_priv_get(struct device *dev)
{
	return xa_load(&vfmig_dma_ops_xa, (unsigned long)dev);
}

static inline struct vfmig_iova_domain *
vfmig_dma_ops_dom_for(struct device *dev)
{
	struct vfmig_dma_ops_priv *priv = vfmig_dma_ops_priv_get(dev);

	return priv ? priv->dom : NULL;
}

/* -------- dma_map_ops callbacks ----------------------------------------- */

/*
 * dma_map_sg / dma_map_sgtable land here for tracked VFs. The
 * inbound @sg has nents segments (each one the result of pinning a
 * contiguous run of pages in ib_umem_pgmap or peer driver's
 * coalescer). Per the dma_map_sgtable contract:
 *   - return value > 0: that many physical segments coalesced into
 *     output dma_addresses (we don't coalesce, so we return @nents);
 *   - return value < 0: errno; the caller treats this as failure.
 *
 * We allocate one IOVA range per @sg entry from the USER_PAGE slot
 * via vfmig_iova_user_page_map_phys, which iommu_maps the entry's
 * phys and inserts a registry entry tagged @external = true.
 *
 * On error, walk back from the failed entry and undo the partial
 * mapping; report the error code.
 *
 * Stage 1 does not coalesce contiguous-physical sg entries into a
 * single IOVA mapping. Doing so would shrink the registry on large
 * MRs but complicates stage 2 unmap (one map_sg entry no longer
 * maps 1:1 to one registry entry). Coalescing is a stage-2 follow-
 * up tracked in DESIGN_user_mr_dma.md §4.3.
 */
static int vfmig_dma_ops_map_sg(struct device *dev, struct scatterlist *sg,
				int nents, enum dma_data_direction dir,
				unsigned long attrs)
{
	struct vfmig_iova_domain *dom = vfmig_dma_ops_dom_for(dev);
	struct scatterlist *s;
	int i, mapped = 0;
	int err;

	if (unlikely(!dom))
		return -EIO;

	for_each_sg(sg, s, nents, i) {
		dma_addr_t iova;
		phys_addr_t phys = sg_phys(s);
		unsigned int off = s->offset & ~PAGE_MASK;
		unsigned int len = s->length;

		/*
		 * ib_umem_get hands us page-aligned segments; defensively
		 * round @phys down + @len up so callers that supply mid-
		 * page offsets (dma-buf system-memory exporters) still get
		 * a well-defined IOMMU mapping. The returned dma_address
		 * keeps the original byte offset.
		 */
		err = vfmig_iova_user_page_map_phys(dom,
						    phys & PAGE_MASK,
						    PAGE_ALIGN(len + off),
						    GFP_ATOMIC, &iova);
		if (err)
			goto err_undo;

		sg_dma_address(s) = iova + off;
		sg_dma_len(s)	  = len;
		mapped++;
	}

	return nents;

err_undo:
	for_each_sg(sg, s, mapped, i) {
		dma_addr_t iova = sg_dma_address(s);
		unsigned int off = iova & ~PAGE_MASK;
		unsigned int len = sg_dma_len(s);

		(void)vfmig_iova_user_page_unmap_phys(dom,
						      iova - off,
						      PAGE_ALIGN(len + off));
		sg_dma_address(s) = 0;
		sg_dma_len(s)	  = 0;
	}
	dev_warn_ratelimited(dev,
			     "vfmig_dma_ops: map_sg failed at entry %d/%d: %d\n",
			     mapped, nents, err);
	return err;
}

static void vfmig_dma_ops_unmap_sg(struct device *dev, struct scatterlist *sg,
				   int nents, enum dma_data_direction dir,
				   unsigned long attrs)
{
	struct vfmig_iova_domain *dom = vfmig_dma_ops_dom_for(dev);
	struct scatterlist *s;
	int i;

	if (unlikely(!dom))
		return;

	for_each_sg(sg, s, nents, i) {
		dma_addr_t iova = sg_dma_address(s);
		unsigned int off;
		unsigned int len = sg_dma_len(s);

		if (!iova && !len)
			continue;	/* never mapped (partial map_sg) */

		off = iova & ~PAGE_MASK;
		(void)vfmig_iova_user_page_unmap_phys(dom,
						      iova - off,
						      PAGE_ALIGN(len + off));
		sg_dma_address(s) = 0;
		sg_dma_len(s)	  = 0;
	}
}

/*
 * dma_map_phys lands here for callers that supply a single physical
 * address rather than a scatterlist. Same machinery as map_sg's
 * inner loop, single-entry.
 *
 * DMA_ATTR_MMIO indicates a peer-to-peer mapping of MMIO BAR space
 * rather than system memory. Our iommu_map call would still create
 * a mapping, but the resulting IOVA isn't deterministic across
 * SAVE/LOAD (BAR base addresses differ between source and
 * destination) and we'd need to coordinate with the peer driver to
 * reconstruct the correct phys on LOAD. Stage 1 rejects MMIO with
 * -EOPNOTSUPP; future work will add a "peer dma-buf" wire record
 * type that carries the peer device identity rather than phys.
 */
static dma_addr_t vfmig_dma_ops_map_phys(struct device *dev, phys_addr_t phys,
					 size_t size,
					 enum dma_data_direction dir,
					 unsigned long attrs)
{
	struct vfmig_iova_domain *dom = vfmig_dma_ops_dom_for(dev);
	dma_addr_t iova;
	unsigned int off;
	int err;

	if (unlikely(!dom))
		return DMA_MAPPING_ERROR;

	if (attrs & DMA_ATTR_MMIO) {
		dev_warn_ratelimited(dev,
				     "vfmig_dma_ops: map_phys with DMA_ATTR_MMIO not supported (peer-to-peer dma-buf is stage-1 out-of-scope); use cpu-memory dma-buf or wait for stage-2\n");
		return DMA_MAPPING_ERROR;
	}

	off = phys & ~PAGE_MASK;
	err = vfmig_iova_user_page_map_phys(dom, phys & PAGE_MASK,
					    PAGE_ALIGN(size + off),
					    GFP_ATOMIC, &iova);
	if (err) {
		dev_warn_ratelimited(dev,
				     "vfmig_dma_ops: map_phys(0x%llx, %zu) failed: %d\n",
				     (u64)phys, size, err);
		return DMA_MAPPING_ERROR;
	}
	return iova + off;
}

static void vfmig_dma_ops_unmap_phys(struct device *dev, dma_addr_t handle,
				     size_t size,
				     enum dma_data_direction dir,
				     unsigned long attrs)
{
	struct vfmig_iova_domain *dom = vfmig_dma_ops_dom_for(dev);
	unsigned int off;

	if (unlikely(!dom))
		return;
	if (attrs & DMA_ATTR_MMIO)
		return;	/* never mapped, see map_phys */

	off = handle & ~PAGE_MASK;
	(void)vfmig_iova_user_page_unmap_phys(dom, handle - off,
					      PAGE_ALIGN(size + off));
}

/*
 * Cache-coherent x86 / arm64 with IOMMU_CACHE in the iommu_map call
 * means CPU-side caches and the device see the same memory; the
 * sync_* hooks have nothing to flush. Provided as no-ops so the DMA
 * layer doesn't WARN on missing callbacks.
 */
static void vfmig_dma_ops_sync_single_for_cpu(struct device *dev,
					      dma_addr_t handle, size_t size,
					      enum dma_data_direction dir) { }
static void vfmig_dma_ops_sync_single_for_device(struct device *dev,
						 dma_addr_t handle,
						 size_t size,
						 enum dma_data_direction dir) { }
static void vfmig_dma_ops_sync_sg_for_cpu(struct device *dev,
					  struct scatterlist *sg, int nents,
					  enum dma_data_direction dir) { }
static void vfmig_dma_ops_sync_sg_for_device(struct device *dev,
					     struct scatterlist *sg, int nents,
					     enum dma_data_direction dir) { }

static int vfmig_dma_ops_dma_supported(struct device *dev, u64 mask)
{
	/*
	 * VFMIG only attaches to mlx5 VFs, all of which are 64-bit
	 * DMA capable. Our IOVAs live well above the 32-bit boundary
	 * (VFMIG_IOVA_BASE = 4 GiB). Require a mask that covers at
	 * least the IOMMU aperture floor (39 bits, the smallest we
	 * support) so a hypothetical caller with a sub-39-bit mask
	 * fails up front rather than after DMAs fire into nowhere.
	 */
	return mask >= DMA_BIT_MASK(39);
}

static u64 vfmig_dma_ops_get_required_mask(struct device *dev)
{
	return DMA_BIT_MASK(64);
}

static const struct dma_map_ops vfmig_dma_ops = {
	/*
	 * .alloc / .free deliberately NULL. dma_alloc_coherent on a
	 * tracked VF is a v1 fail-loud signal: the call site hasn't
	 * been converted to vfmig_iova_alloc_slot, so its IOVA isn't
	 * deterministic across SAVE/LOAD. The DMA layer fails with
	 * -ENOSYS rather than handing back a bogus mapping.
	 */
	.alloc			= NULL,
	.free			= NULL,

	.map_phys		= vfmig_dma_ops_map_phys,
	.unmap_phys		= vfmig_dma_ops_unmap_phys,
	.map_sg			= vfmig_dma_ops_map_sg,
	.unmap_sg		= vfmig_dma_ops_unmap_sg,

	.sync_single_for_cpu	= vfmig_dma_ops_sync_single_for_cpu,
	.sync_single_for_device	= vfmig_dma_ops_sync_single_for_device,
	.sync_sg_for_cpu	= vfmig_dma_ops_sync_sg_for_cpu,
	.sync_sg_for_device	= vfmig_dma_ops_sync_sg_for_device,

	.dma_supported		= vfmig_dma_ops_dma_supported,
	.get_required_mask	= vfmig_dma_ops_get_required_mask,
};

/* -------- attach / detach ----------------------------------------------- */

int vfmig_dma_ops_attach(struct pci_dev *vf_pdev,
			 struct vfmig_iova_domain *dom)
{
	struct vfmig_dma_ops_priv *priv;
	struct device *dev;
	void *old;
	int err;

	if (!vf_pdev || !dom)
		return -EINVAL;
	dev = &vf_pdev->dev;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dom		= dom;
#ifdef CONFIG_IOMMU_DMA
	priv->orig_dma_iommu	= dev->dma_iommu;
#endif
	priv->orig_dma_ops	= dev->dma_ops;

	/*
	 * Insert into the xarray BEFORE flipping dma_ops / dma_iommu
	 * so the first dispatched callback can find @priv. xa_insert
	 * fails -EBUSY if there's already a mapping, which catches
	 * double-attach bugs.
	 */
	old = xa_cmpxchg(&vfmig_dma_ops_xa, (unsigned long)dev, NULL, priv,
			 GFP_KERNEL);
	if (xa_is_err(old)) {
		err = xa_err(old);
		goto err_free;
	}
	if (old) {
		dev_warn(dev,
			 "vfmig_dma_ops: attach: device already has a vfmig_dma_ops priv attached (caller bug)\n");
		err = -EBUSY;
		goto err_free;
	}

	vfmig_dma_iommu_write(dev, false);
	set_dma_ops(dev, &vfmig_dma_ops);

	/*
	 * Runtime sanity check: on architectures without
	 * CONFIG_ARCH_HAS_DMA_OPS, set_dma_ops is a no-op and
	 * get_dma_ops returns get_arch_dma_ops() rather than our
	 * pointer. Bail loudly so SET_TRACKED fails with a clear
	 * reason rather than the user-MR DMA path silently going
	 * back to iommu_dma_map_sg.
	 */
	if (get_dma_ops(dev) != &vfmig_dma_ops) {
		dev_warn(dev,
			 "vfmig_dma_ops: attach: set_dma_ops did not stick (CONFIG_ARCH_HAS_DMA_OPS=n?); user-MR DMA cannot be intercepted on this kernel\n");
		err = -EOPNOTSUPP;
		goto err_restore;
	}

	dev_info(dev,
		 "vfmig_dma_ops: attached on tracked VF (orig_dma_iommu=%d, orig_dma_ops=%pS)\n",
		 vfmig_dma_iommu_read(dev), priv->orig_dma_ops);
	return 0;

err_restore:
	set_dma_ops(dev, priv->orig_dma_ops);
#ifdef CONFIG_IOMMU_DMA
	vfmig_dma_iommu_write(dev, priv->orig_dma_iommu);
#endif
	xa_erase(&vfmig_dma_ops_xa, (unsigned long)dev);
err_free:
	kfree(priv);
	return err;
}

void vfmig_dma_ops_detach(struct pci_dev *vf_pdev)
{
	struct vfmig_dma_ops_priv *priv;
	struct device *dev;

	if (!vf_pdev)
		return;
	dev = &vf_pdev->dev;

	priv = xa_erase(&vfmig_dma_ops_xa, (unsigned long)dev);
	if (!priv) {
		dev_warn(dev,
			 "vfmig_dma_ops: detach without prior attach (caller bug)\n");
		return;
	}

	/*
	 * Restore in the reverse order of attach: dma_ops first
	 * (otherwise a racing dma_map_sgtable could see vfmig_dma_ops
	 * with dma_iommu = priv->orig_dma_iommu and route through the
	 * iommu-dma path with our shim still installed). Then
	 * dma_iommu. Both writes are tearing-free WRITE_ONCE in
	 * practice; the device is unbound by the caller's contract so
	 * no concurrent DMA path is in flight anyway.
	 */
	set_dma_ops(dev, priv->orig_dma_ops);
#ifdef CONFIG_IOMMU_DMA
	vfmig_dma_iommu_write(dev, priv->orig_dma_iommu);
#endif

	dev_info(dev,
		 "vfmig_dma_ops: detached (restored dma_iommu=%d, dma_ops=%pS)\n",
		 vfmig_dma_iommu_read(dev), priv->orig_dma_ops);
	kfree(priv);
}
