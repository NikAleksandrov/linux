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
 * PF-side helper: detach the iommu_dom + dma_ops shim from every
 * vfs_ctx[].vfmig_iova_dom whose VF is not currently bound to a
 * driver, without freeing the domain struct itself. Counterpart to
 * the VF-side mlx5_vfmig_vf_detach_iova_domain() hook, covering the
 * gap when a tracked VF was made migratable but never driver-bound
 * (e.g. the destination VF in a checkpoint/restore measurement that
 * issues user-mode RESTORE_X verbs against the PF cdev before bind).
 *
 * Intended call site is mlx5_sriov_disable() *before*
 * pci_disable_sriov(): we need the iommu_dom gone before the PCI core
 * fires BUS_NOTIFY_REMOVED_DEVICE, otherwise the iommu core WARNs at
 * drivers/iommu/iommu.c:715 because the per-VF iommu_group still
 * holds our unmanaged paging domain. Driver-bound VFs are left alone
 * here -- their detach happens at remove_one() tail, after
 * mlx5_pci_close() has drained any in-flight FW DMA. The domain
 * structs are freed later by mlx5_vfmig_pf_drop_iova_domains() once
 * pci_disable_sriov() has returned.
 *
 * Safe to call when there is no vfmig PF context yet (no-op).
 */
void mlx5_vfmig_pf_detach_unbound_iova_domains(struct mlx5_core_dev *pf_mdev);

/*
 * Drop any orchestrator-stamped per-VF UUIDs on @pf_mdev's
 * sriov->vfs_ctx[]. Called from mlx5_device_disable_sriov() so the
 * lifecycle promised by the SET_VF_UUID UAPI doc-comment ("cleared on
 * sriov_numvfs=0") holds even though the underlying vfs_ctx[] array
 * itself survives the sriov_numvfs cycle.
 *
 * Motivation: a VF slot is an orchestration unit, not a workload
 * unit. After tearing the SR-IOV generation down (sriov_numvfs=0),
 * the orchestrator is free to provision the same vf_id slot for a
 * *different* workload identity on the next sriov_numvfs=N -- e.g.
 * when a host has been drained and re-targeted, or when a fresh
 * workload (with no SAVE blob at all) takes over a slot whose
 * previous occupant was a CRIU-restored workload. Without this
 * hook the previous identity tag would survive the cycle, and the
 * subsequent SET_VF_UUID with the new workload's UUID would get
 * -EBUSY with no in-kernel path to clear the stale stamp short of
 * PF unload/reload.
 *
 * This hook does NOT depend on (or imply support for) multiple
 * LOAD_VHCA_STATE invocations on the same VHCA without an
 * sriov_numvfs cycle in between. That workflow is structurally
 * blocked one layer down, in vfmig_iova_replay_page(): the
 * first LOAD's parser arms drift detection on the per-VF IOVA
 * domain after every HOST_PAGE record has been parsed, and any
 * later replay attempt (i.e. the HOST_PAGE prefix of a second
 * LOAD on the same domain) trips WARN_ON_ONCE(dom->drift_armed)
 * and returns -EBUSY, which the parser surfaces as -EINVAL.
 * LOAD_VHCA_STATE is never issued, so the firmware question
 * ("does FW accept LOAD on a VHCA that has already been LOADed
 * and then DISABLE_HCA / ENABLE_HCA'd?") is moot for in-tree
 * consumers. The cycle path, by contrast, drops the per-VF
 * IOVA domain in mlx5_sriov_disable() (drift_armed goes with
 * it) and is empirically validated end-to-end by
 * tools/testing/mlx5_vfmig/save_load/test_iova_tracked_save_load.sh.
 * See tools/testing/mlx5_vfmig/design/vf_prerestore_split.md
 * §3.5.5.1 for the gate-by-gate empirical table.
 *
 * The hook only unblocks identity-tag recycling across the
 * cycle that already has to happen for *any* repurposing of
 * the slot.
 *
 * Safe to call when there is no vfmig PF context yet (no-op).
 */
void mlx5_vfmig_pf_drop_vf_uuids(struct mlx5_core_dev *pf_mdev);

/*
 * VF-side helper: detach this VF's vfmig_iova_dom from its PCI device
 * (iommu_dom + dma_ops shim) without freeing the domain struct itself.
 * Idempotent; no-op on PFs, untracked VFs, and orphaned VFs whose PF
 * has gone away. Intended call site is the VF's mlx5_core remove_one()
 * tail, run after mlx5_pci_close() has drained all FW DMA and before
 * pci_disable_sriov()'s device_del() fires the iommu core's notifier.
 * The matching domain struct is freed later by
 * mlx5_vfmig_pf_drop_iova_domains() at the end of mlx5_sriov_disable().
 */
void mlx5_vfmig_vf_detach_iova_domain(struct mlx5_core_dev *vf_mdev);

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
 * Reconstitute the destination VF's mlx5_core page rb-tree
 * (priv->page_root_xa[function=0]) from the per-VF deterministic
 * IOVA domain's FW_PAGE entries, mirroring the
 * alloc_system_page() -> insert_page() path that ran on the source.
 *
 * Without this step, mlx5_reclaim_root_pages() at restored-VF
 * teardown finds an empty rb-tree, returns 0 pages reclaimed, and
 * the IOVA allocator never frees the restored backing pages -- per-
 * VF leak that grows unbounded across bind/unbind cycles. See the
 * function comment in vfmig.c for the full rationale and the
 * symptoms of the missing-import regression.
 *
 * MUST be called between mlx5_cmd_enable() (which initialises
 * priv->page_root_xa) and any FW give/take-pages event on the
 * restored VHCA. Current caller is the restored-VF branch of
 * mlx5_function_open() in main.c, after
 * mlx5_vfmig_vf_apply_pending_load() returns success.
 *
 * No-op on PFs, on VFs whose cmd ring isn't tracked (vfmig_iova_dom
 * is NULL), and on tracked VFs whose IOVA registry happens to have
 * no FW_PAGE entries (e.g. a SAVE that captured zero FW pages).
 *
 * Returns 0 on success or a negative errno from the first failing
 * mlx5_pages_import_replayed_fw_page() call. Partial inserts are
 * NOT rolled back; mlx5_reclaim_root_pages() at the next teardown
 * frees them via the same vfmig branch.
 */
int mlx5_vfmig_vf_import_replayed_fw_pages(struct mlx5_core_dev *vf_dev);

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
 * coherent allocations through vfmig_iova_alloc_slot() instead of
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
static inline void mlx5_vfmig_pf_drop_vf_uuids(struct mlx5_core_dev *pf_mdev) { }
static inline void mlx5_vfmig_pf_detach_unbound_iova_domains(struct mlx5_core_dev *pf_mdev) { }
static inline void mlx5_vfmig_vf_detach_iova_domain(struct mlx5_core_dev *vf_mdev) { }
static inline bool mlx5_vfmig_vf_consume_restored(struct mlx5_core_dev *dev,
						  u16 *vhca_id_out)
{
	return false;
}
static inline int  mlx5_vfmig_vf_apply_pending_load(struct mlx5_core_dev *vf_dev) { return 0; }
static inline int  mlx5_vfmig_vf_import_replayed_fw_pages(struct mlx5_core_dev *vf_dev) { return 0; }
static inline bool mlx5_vf_is_vfmig_tracked(struct mlx5_core_dev *dev) { return false; }
static inline struct vfmig_iova_domain *
mlx5_vf_get_vfmig_iova_domain(struct mlx5_core_dev *vf_dev) { return NULL; }
static inline int  mlx5_vfmig_module_init(void) { return 0; }
static inline void mlx5_vfmig_module_exit(void) { }

#endif /* CONFIG_MLX5_VFMIG */

#endif /* __MLX5_CORE_VFMIG_H__ */
