/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR Linux-OpenIB) */
/*
 * Copyright (c) 2026, rxe CRIU migration. All rights reserved.
 *
 * Driver-private uverbs ioctl namespace for the Soft-RoCE (rxe) device.
 * Mirrors the mlx5 MLX5_IB_OBJECT_VFMIG surface but in the
 * RDMA_DRIVER_RXE namespace: object/method/attr ids are scoped to the
 * rxe driver and never collide with another provider's ids.
 *
 * RXE_IB_OBJECT_VFMIG carries the rxe arm of the CRIU dump-side
 * choreography (S6a). Its methods run on a per-process uverbs fd:
 *
 *   FREEZE_DATAPATH  non-destructively pause a QP's req/resp/comp
 *                    worker tasks so a consistent PSN/cursor snapshot
 *                    can be taken; see uobject_restore.md §5.3.7.
 *   QUERY_QP         dump-side counterpart to UVERBS_METHOD_RESTORE_QP:
 *                    pack the full rxe wire state into a payload
 *                    byte-equal to struct rxe_restore_qp_req so the
 *                    destination can restore the QP single-shot.
 */
#ifndef RXE_USER_IOCTL_CMDS_H
#define RXE_USER_IOCTL_CMDS_H

#include <linux/types.h>
#include <rdma/ib_user_ioctl_cmds.h>

enum rxe_ib_objects {
	RXE_IB_OBJECT_VFMIG = (1U << UVERBS_ID_NS_SHIFT),
};

enum rxe_ib_vfmig_methods {
	RXE_IB_METHOD_VFMIG_FREEZE_DATAPATH = (1U << UVERBS_ID_NS_SHIFT),
	RXE_IB_METHOD_VFMIG_QUERY_QP,
};

/*
 * FREEZE_DATAPATH is per-QP rather than the ucontext-scope shape
 * sketched in uobject_restore.md §5.3.7: a driver module cannot reach
 * the core-internal ufile QP-type walk (uapi_get_object lives in
 * rdma_core.h), and per-QP composes cleanly with the plugin's existing
 * per-uobject enumeration (INFO_HANDLES -> FREEZE_DATAPATH -> QUERY_QP).
 * @FREEZE selects pause (1) vs resume (0); resume exists for test
 * symmetry, the dump flow never thaws (the dumpee is killed).
 */
enum rxe_ib_vfmig_freeze_datapath_attrs {
	RXE_IB_ATTR_VFMIG_FREEZE_DATAPATH_QP_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	RXE_IB_ATTR_VFMIG_FREEZE_DATAPATH_FREEZE,
};

enum rxe_ib_vfmig_query_qp_attrs {
	RXE_IB_ATTR_VFMIG_QUERY_QP_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	RXE_IB_ATTR_VFMIG_QUERY_QP_RESP_BLOB,
};

#endif
