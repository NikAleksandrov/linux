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
 *   - The cdev only exists on PF mdevs.
 *
 * Lifetime
 * --------
 *   mlx5_vfmig_pf_init()             from mlx5_init_one_devl_locked()
 *                                    after the PF is fully up.
 *   mlx5_vfmig_pf_cleanup()          from mlx5_uninit_one().
 *
 * This header declares only the framework hooks (cdev open / per-PF
 * lifecycle / module init+exit). Subsequent patches add the SAVE / LOAD
 * machinery and the per-VF deferred-load infrastructure on top.
 */

#ifndef __MLX5_CORE_VFMIG_H__
#define __MLX5_CORE_VFMIG_H__

#include <linux/mlx5/driver.h>

int  mlx5_vfmig_pf_init(struct mlx5_core_dev *pf_mdev);
void mlx5_vfmig_pf_cleanup(struct mlx5_core_dev *pf_mdev);

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

/* Module init/exit hooks for the cdev region. */
int  mlx5_vfmig_module_init(void);
void mlx5_vfmig_module_exit(void);

#endif /* __MLX5_CORE_VFMIG_H__ */
