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
 * What this still doesn't do: it doesn't notice when src and dst
 * disagree about the *number* of allocations in a slot. The per-slot
 * cursor on the destination just keeps bumping into fresh IOVAs that
 * have no SAVE-side counterpart, and the FW dereferences something
 * that was never set up. Detecting that requires comparing the
 * destination's alloc_slot call sequence against the source's
 * recorded sequence -- a follow-up that builds on top of the
 * (slot, instance_key) wire identity introduced here.
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
 *   VFMIG_SLOT_DMA_COHERENT -- legacy catch-all for the
 *                              mlx5_dma_zalloc_coherent_node call
 *                              site, retained for two reasons:
 *                                a) the existing exported
 *                                   mlx5_frag_buf_alloc_node /
 *                                   mlx5_db_alloc_node ABI is used
 *                                   by mlx5_ib, vfio_pci_mlx5, and
 *                                   vdpa, and those out-of-tree-ish
 *                                   consumers haven't yet been
 *                                   converted to the slot-aware
 *                                   variants. Their allocations
 *                                   land here without breaking
 *                                   linkage.
 *                                b) renumbering existing slots is a
 *                                   wire-incompatible change.
 *                              In-tree mlx5_core call sites have all
 *                              moved to one of the per-purpose slots
 *                              below; this slot's window stays
 *                              partitioned but is unused on the
 *                              tracked-VF probe path until a future
 *                              layer plumbs the user-resource paths
 *                              through their own slots.
 *   VFMIG_SLOT_EQ_BUF       -- EQ frag buffers allocated by eq.c via
 *                              mlx5_frag_buf_alloc_node_slot. One
 *                              alloc per EQ; a small fixed set per
 *                              probe (cmd EQ, async EQ, completion
 *                              EQs). Splitting EQ traffic out from
 *                              DB_PAGE / FRAG_BUF means adding a new
 *                              EQ doesn't shift WQ or doorbell
 *                              IOVAs.
 *   VFMIG_SLOT_FRAG_BUF     -- generic queue frag buffers (WQs, CQs,
 *                              SQs, RQs) allocated by wq.c via
 *                              mlx5_frag_buf_alloc_node_slot.
 *                              Variable count per probe depending on
 *                              configured channels / queue sizes.
 *   VFMIG_SLOT_DB_PAGE      -- doorbell pgdir pages allocated by
 *                              mlx5_alloc_db_pgdir. Each pgdir page
 *                              hosts up to db_per_page (~ 64 on a
 *                              64-byte cache line) shared doorbells.
 *                              All db_alloc_node callers (internal
 *                              and external) funnel here -- the
 *                              page is the same kind of resource
 *                              regardless of who asked.
 *   VFMIG_SLOT_USER_PAGE    -- user-space-pinned MR / CQ / QP / SRQ
 *                              buffers + doorbell records.
 *                              ib_umem_get -> dma_map_sgtable lands
 *                              here via vfmig_dma_ops's .map_sg. The
 *                              backing pages are owned by the umem
 *                              (already pinned via gup), so the
 *                              registry entries created in this slot
 *                              carry the @external flag and skip
 *                              alloc_pages / __free_pages on
 *                              install / destroy. Unlike kernel
 *                              slots this slot is "expand-to-fill":
 *                              it occupies the IOVA range between
 *                              the kcoherent sub-arena's end (see
 *                              VFMIG_IOVA_KCOHERENT_BYTES below)
 *                              and the transient arena's base, so
 *                              increasing
 *                              CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB
 *                              grows the user-MR budget without
 *                              perturbing any kernel slot's IOVAs.
 *
 * Sub-arenas (not exposed via enum vfmig_iova_slot):
 *   - kcoherent arena (see VFMIG_IOVA_KCOHERENT_BYTES below): a
 *     non-migrated, IOVA-not-stable region carved from the bottom
 *     of slot 7's window. Backs the dma_map_ops .alloc/.free path
 *     for kernel coherent allocations made by drivers like mlx5e
 *     on tracked VFs. Allocations here are NEVER recorded in the
 *     SAVE manifest (vfmig_iova_for_each does not see them).
 *   - transient arena (VFMIG_IOVA_TRANSIENT_BYTES, topmost slice
 *     of the per-VF window): cmd-mailbox-style short-lived,
 *     freelist-recycled, single-page allocations.
 */
enum vfmig_iova_slot {
	VFMIG_SLOT_INVALID	= 0,
	VFMIG_SLOT_CMD_RING	= 1,
	VFMIG_SLOT_FW_PAGE	= 2,
	VFMIG_SLOT_DMA_COHERENT	= 3,
	VFMIG_SLOT_EQ_BUF	= 4,
	VFMIG_SLOT_FRAG_BUF	= 5,
	VFMIG_SLOT_DB_PAGE	= 6,
	VFMIG_SLOT_USER_PAGE	= 7,
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
 *   VFMIG_IOVA_PER_VF      -- IOVA space per VF, configurable via
 *                             CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB
 *                             (Kconfig int in GiB; default 4).
 *                             Plenty of room at the default for cmd
 *                             ring + MANAGE_PAGES + EQs + UARs at
 *                             typical sizes; raise if/when the user
 *                             ib_umem_get path is hooked through this
 *                             allocator and pinned MR pages dominate.
 *
 *                             Wire compatibility: SAVE/LOAD across
 *                             kernels built with different values is
 *                             rejected at slot-vs-IOVA cross-check
 *                             time (vfmig_iova_replay_page).
 *   VFMIG_IOVA_GRANULE     -- minimum allocation alignment. Matches
 *                             PAGE_SIZE; mlx5 hardware page size is
 *                             also 4 KB.
 */
#define VFMIG_IOVA_BASE		0x100000000ULL		/* 4 GiB */
#define VFMIG_IOVA_PER_VF \
	((u64)CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB << 30)
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
 * KCOHERENT sub-arena: carved from the BOTTOM of slot 7's
 * (VFMIG_SLOT_USER_PAGE) window. Backs vfmig_dma_ops's .alloc/.free
 * callbacks for kernel-side coherent allocations on tracked VFs --
 * the canonical caller is mlx5e bringing up its RX/TX rings, drop_rq,
 * and CQ/EQ buffers when the VF is tracked but the netdev still
 * needs to come up far enough for IP configuration and RoCE GID
 * population. ("Functional-enough" netdev: probe + ndo_open succeed,
 * notifier chains fire, packets are DMA'd in/out by the device, but
 * the actual RX/TX traffic is irrelevant for our use case because
 * RDMA goes through mlx5_ib's separate path.)
 *
 * Properties (vs. the deterministic kernel slots):
 *   - Allocations are NOT recorded in the SAVE manifest
 *     (vfmig_iova_for_each does not iterate this arena).
 *   - IOVAs are NOT stable across migration; the destination
 *     re-probes mlx5e fresh and gets whatever IOVAs it gets.
 *   - No drift detection, no replay: SAVE/LOAD are entirely
 *     bypassed.
 *   - Shape: bump cursor with no IOVA reuse on free; allocations
 *     are driver-lifetime so churn is negligible.
 *
 * Layout: the arena occupies
 *   [slot_base(USER_PAGE), slot_base(USER_PAGE) + KCOHERENT_BYTES);
 * USER_PAGE's effective base is shifted up by KCOHERENT_BYTES so
 * the user-MR sub-window starts at the arena's end.
 *
 * Wire compatibility: introducing this carve shifts USER_PAGE's
 * base up by KCOHERENT_BYTES, which is a wire-incompatible change
 * for USER_PAGE entries in any pre-existing SAVE blob -- those
 * entries will fail replay with -ERANGE on a kernel that has the
 * carve. We accept this break: USER_PAGE replay is itself stage-2
 * (not yet wired), so no in-the-wild SAVE blob carries USER_PAGE
 * entries today.
 */
#define VFMIG_IOVA_KCOHERENT_BYTES	(256ULL << 20)	/* 256 MiB */

/*
 * Per-VF slot fan-out for the deterministic allocator.
 *
 * Asymmetric layout (introduced when VFMIG_SLOT_USER_PAGE was added):
 *
 *   - Slots 0..6 (VFMIG_IOVA_KERNEL_NR_SLOTS == NR_SLOTS - 1) are
 *     fixed-size kernel slots, each VFMIG_IOVA_SLOT_BYTES (510 MiB)
 *     wide. Slot N occupies
 *       [base + N * SLOT_BYTES, base + (N+1) * SLOT_BYTES).
 *     Slot 0 is reserved for VFMIG_SLOT_INVALID and is never
 *     allocated from; real kernel allocations live in slots 1..6.
 *
 *   - Slot 7 (VFMIG_SLOT_USER_PAGE) is "expand-to-fill", with
 *     the bottom VFMIG_IOVA_KCOHERENT_BYTES of its window
 *     reserved for the non-migrated kcoherent sub-arena. The
 *     user-MR sub-window therefore occupies
 *       [base + 7 * SLOT_BYTES + KCOHERENT_BYTES, transient.base),
 *     which is exactly
 *       VFMIG_IOVA_PER_VF
 *         - VFMIG_IOVA_KERNEL_NR_SLOTS * VFMIG_IOVA_SLOT_BYTES
 *         - VFMIG_IOVA_KCOHERENT_BYTES
 *         - VFMIG_IOVA_TRANSIENT_BYTES
 *     bytes wide. At higher PER_VF values USER_PAGE absorbs the
 *     entire excess. Concretely (KCOHERENT = 256 MiB):
 *
 *       PER_VF (GiB)   USER_PAGE budget
 *       4              ~254 MiB
 *       16             ~12.25 GiB
 *       128 (default)  ~124.25 GiB
 *
 * Why kernel slots are pinned at 510 MiB rather than scaling with
 * PER_VF: the kernel call-site footprint (cmd ring, FW pages, EQs,
 * WQs, doorbells) is bounded by hardware capabilities, not by how
 * much IOVA the admin has handed us, so its budget shouldn't grow
 * elastically. Pinning the kernel slot size also makes a SAVE blob
 * captured on a PER_VF=N kernel still replayable on a PER_VF=M >= N
 * kernel for the kernel-slot wire records: kernel-slot IOVAs are
 * PER_VF-independent, only USER_PAGE records have a PER_VF-dependent
 * upper bound (and only when the destination's PER_VF is smaller
 * than the source's).
 *
 * The 8-slot fan-out is fixed: USER_PAGE pinned at index 7 means
 * any future kernel-slot additions must reuse one of slots 1..6 or
 * find a different way to grow (e.g. wide vs. narrow slot encoding)
 * because renumbering existing slots would change every deployed
 * kernel-slot IOVA -- a wire-incompatible change.
 */
#define VFMIG_IOVA_NR_SLOTS		8U
#define VFMIG_IOVA_KERNEL_NR_SLOTS	(VFMIG_IOVA_NR_SLOTS - 1U)
#define VFMIG_IOVA_SLOT_BYTES		(510ULL << 20)	/* 510 MiB, fixed */

static_assert(VFMIG_IOVA_PER_VF >
	      (u64)VFMIG_IOVA_KERNEL_NR_SLOTS * VFMIG_IOVA_SLOT_BYTES +
	      VFMIG_IOVA_KCOHERENT_BYTES +
	      VFMIG_IOVA_TRANSIENT_BYTES,
	      "CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB too small: must fit 7 fixed 510-MiB kernel slots + 256 MiB kcoherent + 16 MiB transient + at least one user-MR IOVA");
static_assert(VFMIG_IOVA_SLOT_BYTES >= (8ULL << 20),
	      "VFMIG_IOVA_SLOT_BYTES must be >= 8 MiB to host worst-case kernel allocations");
static_assert(VFMIG_IOVA_KCOHERENT_BYTES <= VFMIG_IOVA_SLOT_BYTES,
	      "VFMIG_IOVA_KCOHERENT_BYTES must not exceed slot 7's nominal window");

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
 *                             records the key on the registry entry,
 *                             SAVE puts it on the wire, and LOAD
 *                             round-trips it; uniqueness enforcement
 *                             and using the key for collision
 *                             detection are follow-ups.
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
 * registry tagged with caller-provided (@slot, @instance_key).
 *
 * Used by the LOAD path to rehydrate the destination's IOVA space
 * from HOST_PAGE wire records *before* LOAD_VHCA_STATE runs. The
 * subsequent vfmig_iova_alloc_slot calls during VF probe will find
 * these entries via the per-slot cursor lookup and reuse them.
 *
 * The wire record carries explicit (slot, instance_key) so this
 * function takes them as parameters. The implementation still
 * validates that @iova falls within @slot's sub-window in @dom;
 * a mismatch returns -ERANGE because that means the source-side
 * (slot, iova) layout no longer matches the destination's slot
 * partitioning (e.g. VFMIG_IOVA_NR_SLOTS or VFMIG_IOVA_PER_VF
 * differ between source and destination kernels) and we'd otherwise
 * silently install at an unexpected slot.
 *
 * @iova must be in this domain's deterministic window and
 * PAGE_SIZE-aligned. @len must be a multiple of PAGE_SIZE. Replaying
 * twice at the same IOVA is an error (returns -EEXIST).
 */
int  vfmig_iova_replay_page(struct vfmig_iova_domain *dom,
			    enum vfmig_iova_slot slot, u64 instance_key,
			    dma_addr_t iova, const void *contents,
			    size_t len);

/*
 * dma_ops dispatch helpers for VFMIG_SLOT_USER_PAGE entries.
 *
 * vfmig_dma_ops.c (the per-VF dma_map_ops shim that intercepts
 * dma_map_sgtable on tracked VFs) calls these to install / tear down
 * one IOMMU mapping per scatter-gather segment. The IOVA window for
 * these mappings is the USER_PAGE slot's expand-to-fill sub-window.
 *
 * The "external" qualifier in the registry distinguishes these
 * entries from kernel-slot entries: the backing pages are owned by
 * the caller (umem.c keeps them pinned via gup), so the registry
 * does NOT alloc_pages() at install time and does NOT __free_pages()
 * at destroy time. iommu_map / iommu_unmap are still called; the
 * registry keeps the (iova, len, phys) bookkeeping for unmap and
 * for SAVE-time iteration via vfmig_iova_for_each().
 *
 * vfmig_iova_user_page_map_phys:
 *   - Bumps the USER_PAGE slot cursor by ALIGN(@len, GRANULE).
 *     Returns -ENOSPC if the slot's expand-to-fill window would
 *     overflow.
 *   - iommu_maps @phys at the freshly-allocated IOVA for @len
 *     bytes, READ | WRITE | CACHE.
 *   - Inserts a registry entry with @external = true.
 *   - On success, *@iova_out is the allocated IOVA.
 *
 *   @phys must be PAGE_SIZE-aligned, @len a non-zero multiple of
 *   PAGE_SIZE. @gfp must not include __GFP_HIGHMEM/COMP/DMA/DMA32
 *   (same constraint as vfmig_iova_alloc_slot; the underlying
 *   iommu_map enforces it).
 *
 * vfmig_iova_user_page_unmap_phys:
 *   - Looks up the registry entry at exactly @iova, asserting the
 *     recorded length matches @len.
 *   - iommu_unmaps and removes the entry.
 *   - Stage 1 does NOT recycle the IOVA range: the per-slot bump
 *     cursor stays put. Stage 2 introduces a per-slot free bitmap
 *     for USER_PAGE so .map_sg can pull from freed ranges before
 *     bumping the cursor; the wire format already encodes the free
 *     bitmap as part of HOST_USER_PAGE delta records on the source
 *     so the destination's USER_PAGE cursor matches.
 *   - Safe with @iova not in the registry: warns and returns
 *     -ENOENT (caller bug; we don't WARN_ON_ONCE because the warn
 *     itself contains the offending iova/len for triage).
 */
int  vfmig_iova_user_page_map_phys(struct vfmig_iova_domain *dom,
				   phys_addr_t phys, size_t len, gfp_t gfp,
				   dma_addr_t *iova_out);
int  vfmig_iova_user_page_unmap_phys(struct vfmig_iova_domain *dom,
				     dma_addr_t iova, size_t len);

/*
 * Allocate a CPU+IOVA-coherent region from the per-VF domain's
 * non-migrated kcoherent sub-arena. Backs vfmig_dma_ops's .alloc
 * callback for kernel coherent allocations on tracked VFs (mlx5e
 * RX/TX rings, drop_rq, CQ/EQ buffers, page-pool coherent sources).
 *
 * Properties:
 *   - Allocations are NOT recorded in the SAVE manifest; SAVE-time
 *     iteration via vfmig_iova_for_each() does not see them.
 *   - IOVAs are NOT stable across migration. The destination
 *     re-probes its drivers fresh and gets whatever IOVAs are
 *     handed out.
 *   - Bump cursor with no IOVA reuse on free; suitable for the
 *     long-lived, low-churn allocations these drivers make.
 *
 * @size is rounded up to PAGE_SIZE. The backing memory is a
 * physically-contiguous run of pages obtained via alloc_pages_exact;
 * sizes larger than what the page allocator can satisfy as a
 * contiguous block fail with -ENOMEM.
 *
 * @gfp may include __GFP_COMP / __GFP_HIGHMEM / __GFP_DMA{,32};
 * the implementation strips flags incompatible with iommu_map and
 * with our requirement that page_address() be valid on the backing
 * memory (the dma-coherent contract requires a kernel-virtual
 * mapping). __GFP_ZERO is implied: the returned region is zeroed,
 * matching dma_alloc_coherent semantics.
 *
 * On success, *@iova_out is the allocated IOVA (also the dma_addr_t
 * the caller hands back from .alloc), and *@vaddr_out is the
 * kernel-virtual address. Both are page-aligned.
 *
 * Errors:
 *   -EINVAL  bad arguments (NULL @dom, zero @size, bad @iova_out)
 *   -ENOSPC  kcoherent arena exhausted (cursor would advance past
 *            VFMIG_IOVA_KCOHERENT_BYTES)
 *   -ENOMEM  alloc_pages_exact failure or kmalloc of bookkeeping
 *   <0       iommu_map failure
 */
int  vfmig_iova_kcoherent_alloc(struct vfmig_iova_domain *dom,
				size_t size, gfp_t gfp,
				dma_addr_t *iova_out, void **vaddr_out);

/*
 * Release a region previously obtained from
 * vfmig_iova_kcoherent_alloc(). @vaddr and @size MUST match the
 * values returned by / passed to _alloc(). @iova MUST equal what
 * _alloc() wrote to *iova_out.
 *
 * Behaviour: iommu_unmaps the region, frees the backing pages,
 * removes the bookkeeping entry. Does NOT reclaim IOVA range to
 * the cursor (kcoherent fragmentation is bounded by driver
 * footprint and isn't recycled at this stage).
 *
 * Safe with @dom == NULL (no-op).
 */
void vfmig_iova_kcoherent_free(struct vfmig_iova_domain *dom,
			       dma_addr_t iova, size_t size, void *vaddr);

/*
 * Read the awaiting-bind hit counter for @dom.
 *
 * Stage 1 always returns 0 -- there is no LOAD-side replay path
 * for USER_PAGE entries yet, so no entry ever has @awaiting_bind
 * set, so .map_sg never takes the "bind a freshly-pinned page to
 * a pre-replayed registry entry" branch. The counter is wired in
 * stage 1 so the test plumbing (debugfs export, pingpong-side
 * read) is in place; stage 2's order-discipline replay will
 * populate awaiting-bind entries on LOAD and increment this
 * counter on each hit.
 *
 * Read with READ_ONCE; writers use atomic_long_inc. Safe at any
 * time, no locking needed.
 */
unsigned long vfmig_iova_awaiting_bind_hits(struct vfmig_iova_domain *dom);

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
 * Reset every per-slot bump cursor to its slot's base IOVA, reset
 * every per-slot auto-key counter to 0, and reset every per-slot
 * alloc-counter (used by drift detection) to 0. Called once on the
 * destination after all HOST_PAGE records have been replayed but
 * before VF probe starts: subsequent vfmig_iova_alloc_slot calls
 * will then walk each slot from its bottom and find the replayed
 * entries.
 *
 * @expected_count is NOT reset -- it's the source's recorded
 * footprint and is frozen by vfmig_iova_arm_drift_detection().
 *
 * Idempotent. Safe to call on a domain that's never been allocated
 * from.
 */
void vfmig_iova_reset_cursor(struct vfmig_iova_domain *dom);

/*
 * Arm at-probe drift detection on @dom.
 *
 * Called by the LOAD path exactly once after every HOST_PAGE
 * record has been replayed AND the wire manifest CRC32 has
 * verified. From that point onward, vfmig_iova_alloc_slot
 * enforces:
 *
 *   - Pinned/auto sequence consistency on cursor HIT: the caller's
 *     resolved instance_key (auto-bumped if 0 was passed) must
 *     equal the replayed entry's recorded key. A mismatch means
 *     the destination's call sequence in this slot interleaves
 *     pinned and auto allocations differently from the source.
 *
 *   - Extra-alloc rejection on cursor MISS: if the slot ever had
 *     any replays (expected_count > 0), the destination is asking
 *     for an alloc the source didn't have at SAVE time -- a
 *     kernel-side change has added an allocation in this slot.
 *
 * Either path returns -EPROTO. The first occurrence per domain
 * also drops a kernel stack via dump_stack() so the offending
 * call site is identifiable in dmesg.
 *
 * Slots with expected_count == 0 (source never used the slot) are
 * unrestricted -- runtime allocations after probe completes (e.g.
 * dynamic FW_PAGE growth via MANAGE_PAGES) still go through this
 * function and would otherwise spuriously fire if the source
 * happened not to have any allocations in that slot at SAVE time.
 *
 * Idempotent. Safe to call on a domain with no replays (every
 * subsequent alloc is then in a slot with expected_count == 0,
 * i.e. unrestricted).
 */
void vfmig_iova_arm_drift_detection(struct vfmig_iova_domain *dom);

/*
 * Iterate the registry in IOVA-ascending order. @cb is invoked once
 * per entry with (slot, instance_key, iova, vaddr, len, ctx).
 * Iteration order is IOVA-sorted across all slots; SAVE uses this
 * order to emit HOST_PAGE records and the destination replays them
 * in the same order, which is what the per-slot cursor-lookup
 * correctness rests on (replayed entries land at exactly the IOVAs
 * the destination's subsequent alloc_slot calls will request).
 *
 * The @slot and @instance_key arguments reflect the registry
 * entry's stored tags, set by an earlier vfmig_iova_alloc_slot
 * call (or by vfmig_iova_replay_page on the destination). SAVE puts
 * both directly on the wire so the destination can replay with the
 * same identity rather than inferring slot from IOVA.
 *
 * @cb may not modify the registry (no alloc/free/replay calls).
 * Returning a non-zero value from @cb stops iteration and is
 * propagated as the return value.
 */
typedef int (*vfmig_iova_for_each_fn)(enum vfmig_iova_slot slot,
				      u64 instance_key,
				      dma_addr_t iova, const void *vaddr,
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
					 enum vfmig_iova_slot slot,
					 u64 instance_key,
					 dma_addr_t iova, const void *contents,
					 size_t len)
{
	return -EOPNOTSUPP;
}
static inline int vfmig_iova_user_page_map_phys(struct vfmig_iova_domain *dom,
						phys_addr_t phys, size_t len,
						gfp_t gfp,
						dma_addr_t *iova_out)
{
	return -EOPNOTSUPP;
}
static inline int vfmig_iova_user_page_unmap_phys(struct vfmig_iova_domain *dom,
						  dma_addr_t iova, size_t len)
{
	return -EOPNOTSUPP;
}
static inline int vfmig_iova_kcoherent_alloc(struct vfmig_iova_domain *dom,
					     size_t size, gfp_t gfp,
					     dma_addr_t *iova_out,
					     void **vaddr_out)
{
	return -EOPNOTSUPP;
}
static inline void vfmig_iova_kcoherent_free(struct vfmig_iova_domain *dom,
					     dma_addr_t iova, size_t size,
					     void *vaddr) { }
static inline unsigned long
vfmig_iova_awaiting_bind_hits(struct vfmig_iova_domain *dom) { return 0; }
static inline void vfmig_iova_reset_cursor(struct vfmig_iova_domain *dom) { }
static inline void
vfmig_iova_arm_drift_detection(struct vfmig_iova_domain *dom) { }

#endif /* CONFIG_MLX5_VFMIG */

#endif /* __MLX5_CORE_VFMIG_IOVA_H__ */
