/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * Host-driver-side VF migration / CRIU restore support for mlx5_core.
 *
 * Goal
 * ----
 * Provide an in-driver SAVE+LOAD plumbing for mlx5 VFs, modelled on the
 * VFIO mlx5 variant driver (drivers/vfio/pci/mlx5/) but driven from the
 * host's PF mlx5_core via a per-PF cdev. Intended consumer: a CRIU-style
 * checkpoint/restore agent that snapshots a running RDMA workload's
 * VHCA state on one provisioning of the PF and re-applies it on the
 * next, without going through a guest VM.
 *
 * Architecture
 * ------------
 *   - One char device per PF mlx5_core, /dev/mlx5_vfmig/<pf_bdf>.
 *     UAPI for it lives in include/uapi/linux/mlx5_vfmig.h.
 *   - Per-VF "pending LOAD" slots live on the PF, in
 *     priv.sriov.vfs_ctx[vf_id].vfmig_pending_load. They are populated
 *     when a LOAD anon-inode fd closes after a complete blob has been
 *     staged into DMA-mapped pages. They are consumed at the next
 *     mlx5_core probe of that VF, from mlx5_function_enable(), via
 *     mlx5_vfmig_vf_apply_pending_load(): SUSPEND_VHCA(INITIATOR) +
 *     SUSPEND_VHCA(RESPONDER) + LOAD_VHCA_STATE + RESUME_VHCA(RESPONDER)
 *     + RESUME_VHCA(INITIATOR), all via the PF mdev.
 *   - The cdev only exists on PF mdevs. The vf-side hooks
 *     (mlx5_vfmig_vf_consume_restored, mlx5_vfmig_vf_apply_pending_load)
 *     work against any mlx5_core_dev: they no-op on PFs and on VFs
 *     whose owning PF has no slot.
 *
 * Lifetime
 * --------
 *   mlx5_vfmig_pf_init()             from mlx5_init_one_devl_locked()
 *                                    after the PF is fully up.
 *   mlx5_vfmig_pf_cleanup()          from mlx5_uninit_one().
 *   mlx5_vfmig_pf_drop_pending_loads from mlx5_device_disable_sriov(),
 *                                    so unconsumed slots don't outlive
 *                                    the VF generation they targeted.
 *
 * Known constraint (FW design, not a bug)
 * ---------------------------------------
 * SAVE_VHCA_STATE / LOAD_VHCA_STATE were designed for the VFIO mlx5
 * passthrough flow, where the VF stays bound to vfio_mlx5_pci on both
 * source and destination hosts and the actual mlx5_core consumer lives
 * inside a guest VM whose memory image carries the cmd ring and all
 * other DMA buffers verbatim across migration. In that model every
 * IOVA the firmware captured in the blob still resolves to the same
 * (guest-physical) buffer on the destination.
 *
 * If the destination instead binds native mlx5_core, the destination's
 * cmd ring lives at a different DMA address than the source's, but the
 * blob still carries the source's view -- so post-LOAD the firmware
 * silently ignores doorbells on the destination's cmd ring (every
 * command 60s timeout). Bisection on CX-7 (FW 28.48.1000):
 *
 *   ENABLE_HCA(self) -> LOAD                   : LOAD fails with
 *                                                bad parameter
 *                                                (syndrome 0x2c9bb0)
 *   LOAD -> ENABLE_HCA(self)                   : LOAD succeeds, but
 *                                                cmd ring is dead
 *                                                afterwards
 *
 * Making the round-trip work for native mlx5_core needs an IOMMU and a
 * deterministic IOVA allocator so the destination can reproduce the
 * source's address layout. That work is out of scope for this change;
 * what's here is the SAVE/LOAD plumbing and the deferred-load
 * infrastructure, on top of which the IOMMU work can land later.
 */

#ifndef __MLX5_CORE_VFMIG_H__
#define __MLX5_CORE_VFMIG_H__

#include <linux/mlx5/driver.h>

struct vfmig_iova_domain;

#if IS_ENABLED(CONFIG_MLX5_VFMIG)

int  mlx5_vfmig_pf_init(struct mlx5_core_dev *pf_mdev);
void mlx5_vfmig_pf_cleanup(struct mlx5_core_dev *pf_mdev);

/*
 * Drop any per-VF "pending LOAD_VHCA_STATE" slots staged on @pf_mdev's
 * sriov->vfs_ctx[]. Called from mlx5_device_disable_sriov() before
 * vfs_ctx slots get reused by the next sriov_numvfs cycle (any unconsumed
 * slot would otherwise refer to a vhca_id that no longer exists). Safe
 * to call when there is no vfmig PF context yet (e.g. PF has no cdev
 * because it was created before the vfmig subsystem was active) -- it's
 * a no-op in that case.
 *
 * Caller must guarantee @pf_mdev is alive (i.e. its FW commands still
 * work) so that PD/MKEY/DMA teardown for staged loads can run.
 */
void mlx5_vfmig_pf_drop_pending_loads(struct mlx5_core_dev *pf_mdev);

/*
 * Drop any per-VF deterministic IOVA domains staged on @pf_mdev's
 * sriov->vfs_ctx[]. Called from mlx5_sriov_disable() *before*
 * pci_disable_sriov() runs: the IOVA domain is attached to the VF's
 * struct device, so it must be detached before the PCI core tears
 * the VF pci_dev down. Also called from mlx5_vfmig_pf_cleanup() so
 * PF unbind cleans up any orphaned domains the user forgot to
 * release explicitly via SET_TRACKED { enable=0 }.
 *
 * Safe to call when there is no vfmig PF context yet (no-op).
 */
void mlx5_vfmig_pf_drop_iova_domains(struct mlx5_core_dev *pf_mdev);

/*
 * Returns true iff @dev is a VF and its PF has marked it as restored.
 * Safe to call unconditionally on any mlx5_core_dev. Internally takes
 * and releases mlx5_vf_get_core_dev() / mlx5_vf_put_core_dev() on the
 * PF, so it must NOT be called while already holding the PF's
 * intf_state_mutex.
 *
 * On true, @vhca_id_out (if non-NULL) is populated with the VF's
 * vhca_id captured at MARK_RESTORED time. Used purely to identify the
 * firmware vHCA in probe-time logs.
 *
 * The flag is consumed (cleared) by this call so that a subsequent
 * unbind/rebind of the same VF without an explicit MARK_RESTORED falls
 * back to the normal probe path. This is a deliberate choice: mis-
 * replays of probe should fail loudly rather than silently keep
 * skipping VHCA-side bring-up commands.
 */
bool mlx5_vfmig_vf_consume_restored(struct mlx5_core_dev *dev, u16 *vhca_id_out);

/*
 * Apply any "pending LOAD_VHCA_STATE" blob staged on @vf_dev's PF for
 * this VF. Issues, via the PF mdev:
 *
 *   SUSPEND_VHCA(INITIATOR) -> SUSPEND_VHCA(RESPONDER)
 *      -> LOAD_VHCA_STATE
 *      -> RESUME_VHCA(RESPONDER) -> RESUME_VHCA(INITIATOR)
 *
 * then frees the staged resources. MUST be called from the VF's
 * mlx5_function_enable() AFTER mlx5_cmd_enable() and mlx5_cmd_set_state(UP),
 * but BEFORE any VHCA-side firmware command (ENABLE_HCA(self), SET_ISSI,
 * MANAGE_PAGES, INIT_HCA): the firmware rejects LOAD with bad parameter
 * (syndrome 0x2c9bb0 on CX-7) once any of those have mutated the
 * destination VHCA off the source's saved state shape.
 *
 * Returns 0 on success or when no slot was staged (the test path that
 * uses MARK_RESTORED without LOAD). Returns a negative errno if a
 * staged slot existed but firmware command(s) failed; in that case the
 * slot is still freed and the caller should treat the probe as
 * failing.
 *
 * Same caller contract as mlx5_vfmig_vf_consume_restored: takes the
 * PF reference internally, so must NOT be called while already
 * holding the PF's intf_state_mutex.
 *
 * NOTE: even when this returns 0, the destination VHCA's own command
 * interface remains non-functional after LOAD on a native (non-VFIO,
 * non-VM) mlx5_core probe -- see the FW-design constraint at the top
 * of this header. The bind will still succeed all the way through this
 * call; subsequent VHCA-targeted commands in mlx5_function_open()
 * (e.g. mlx5_query_hca_caps) are what time out.
 */
int mlx5_vfmig_vf_apply_pending_load(struct mlx5_core_dev *vf_dev);

/*
 * Returns true iff @dev is a VF whose owning PF has SET_TRACKED { enable=1 }
 * latched on the PF's vfs_ctx[vf_id]. Probe-time predicate consulted by
 * host-side allocators (mlx5_cmd_enable, pages.c, ...) to decide whether
 * to route through the deterministic IOVA allocator.
 *
 * Safe to call on any mlx5_core_dev: returns false on PFs and on VFs
 * whose owning PF either has no /dev/mlx5_vfmig cdev or has not had
 * SET_TRACKED issued for this slot. Internally takes
 * mlx5_vf_get_core_dev() / mlx5_vf_put_core_dev() on the PF, so it must
 * NOT be called while already holding the PF's intf_state_mutex.
 */
bool mlx5_vf_is_vfmig_tracked(struct mlx5_core_dev *dev);

/*
 * If @vf_dev is a VF whose owning PF has SET_TRACKED { enable=1 }
 * latched on this slot, return the per-VF deterministic IOVA domain
 * allocated for it; otherwise return NULL.
 *
 * Probe-time DMA hook used by mlx5_cmd_enable, pages.c, etc. to route
 * coherent allocations through vfmig_iova_alloc_coherent() instead of
 * dma_alloc_coherent() so that source/destination IOVAs match across a
 * SAVE/LOAD round-trip. Callers stash the returned pointer alongside
 * the allocation so the matching free path can dispatch to the right
 * allocator without re-running the lookup.
 *
 * Lifetime contract:
 *   - The returned pointer is owned by the PF's mlx5_vfmig_pf and lives
 *     until SET_TRACKED { enable=0 } or sriov_disable / PF unbind. All
 *     three of those paths require the VF to be unbound first
 *     (SET_TRACKED via the device_lock check; sriov_disable because the
 *     PCI core unbinds the VFs first; PF unbind because mlx5_unload
 *     drops sriov before the cdev cleanup). So as long as @vf_dev is
 *     mid-probe (i.e. between mlx5_cmd_enable and mlx5_cmd_disable),
 *     the returned domain is guaranteed to outlive that probe.
 *   - Same caller contract as mlx5_vf_is_vfmig_tracked: takes the PF
 *     reference internally for the lookup, so must NOT be called while
 *     already holding the PF's intf_state_mutex.
 *
 * Returns NULL on PFs, on VFs whose owning PF has no /dev/mlx5_vfmig
 * cdev, on VFs that have not had SET_TRACKED { enable=1 } issued, and
 * on transient PF lookup failure.
 */
struct vfmig_iova_domain *
mlx5_vf_get_vfmig_iova_domain(struct mlx5_core_dev *vf_dev);

/* Module init/exit hooks for the cdev region. */
int  mlx5_vfmig_module_init(void);
void mlx5_vfmig_module_exit(void);

#else /* !CONFIG_MLX5_VFMIG */

/*
 * Stubs for builds with vfmig compiled out. They make the symbol
 * surface available unconditionally so callers in main.c / sriov.c
 * stay free of #ifdef CONFIG_MLX5_VFMIG sprinkles.
 *
 * Semantically the stubs match "no VF was ever marked restored, no
 * pending load was ever staged, no PF state ever existed", which is
 * the correct null behaviour for a tree without the migration
 * subsystem.
 */
static inline int  mlx5_vfmig_pf_init(struct mlx5_core_dev *pf_mdev) { return 0; }
static inline void mlx5_vfmig_pf_cleanup(struct mlx5_core_dev *pf_mdev) { }
static inline void mlx5_vfmig_pf_drop_pending_loads(struct mlx5_core_dev *pf_mdev) { }
static inline void mlx5_vfmig_pf_drop_iova_domains(struct mlx5_core_dev *pf_mdev) { }
static inline bool mlx5_vfmig_vf_consume_restored(struct mlx5_core_dev *dev,
						  u16 *vhca_id_out)
{
	return false;
}
static inline int  mlx5_vfmig_vf_apply_pending_load(struct mlx5_core_dev *vf_dev) { return 0; }
static inline bool mlx5_vf_is_vfmig_tracked(struct mlx5_core_dev *dev) { return false; }
static inline struct vfmig_iova_domain *
mlx5_vf_get_vfmig_iova_domain(struct mlx5_core_dev *vf_dev) { return NULL; }
static inline int  mlx5_vfmig_module_init(void) { return 0; }
static inline void mlx5_vfmig_module_exit(void) { }

#endif /* CONFIG_MLX5_VFMIG */

#endif /* __MLX5_CORE_VFMIG_H__ */
