/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR Linux-OpenIB) */
/*
 * Copyright (c) 2026, rxe CRIU migration. All rights reserved.
 *
 * Driver-private uverbs ioctl namespace for the Soft-RoCE (rxe) device.
 * Mirrors the mlx5 driver-private migration object surface but in the
 * RDMA_DRIVER_RXE namespace: object/method/attr ids are scoped to the
 * rxe driver and never collide with another provider's ids.
 *
 * RXE_IB_OBJECT_MIGRATE carries the rxe arm of the CRIU dump-side
 * choreography (S6a). Its methods run on a per-process uverbs fd:
 *
 *   FREEZE_DATAPATH  non-destructively pause a QP's req/resp/comp
 *                    worker tasks so a consistent PSN/cursor snapshot
 *                    can be taken; see uobject_restore.md §5.3.7.
 *   QUERY_QP         dump-side counterpart to UVERBS_METHOD_RESTORE_QP:
 *                    pack the full rxe wire state into a payload
 *                    byte-equal to struct rxe_restore_qp_req so the
 *                    destination can restore the QP single-shot, plus
 *                    the QP's userspace handle (the async-event cookie,
 *                    not standard-queryable). cap / qp_type / qp_state
 *                    are intentionally NOT emitted -- CRIU sources those
 *                    from the standard IB_USER_VERBS_CMD_QUERY_QP verb
 *                    and NLDEV.
 *   QUERY_CQ         dump-side counterpart to UVERBS_METHOD_RESTORE_CQ:
 *                    return a CQ's ring mmap offset + entry count so the
 *                    dumper sources both RESTORE_CQ inputs (the forced
 *                    vm_pgoff and the cqe) authoritatively from the
 *                    kernel, keyed by CQ handle. This retires the smaps
 *                    cdev-VMA FIFO crutch the CQ-only path relied on,
 *                    which a mixed PD+CQ+QP ufile would otherwise corrupt
 *                    (QP rings land in the same FIFO but are sourced via
 *                    QUERY_QP, never popped -- so a CQ could pop a QP
 *                    ring's offset). See uobject_restore.md §5.3.6.
 */
#ifndef RXE_USER_IOCTL_CMDS_H
#define RXE_USER_IOCTL_CMDS_H

#include <linux/types.h>
#include <rdma/ib_user_ioctl_cmds.h>

enum rxe_ib_objects {
	RXE_IB_OBJECT_MIGRATE = (1U << UVERBS_ID_NS_SHIFT),
};

enum rxe_ib_migrate_methods {
	RXE_IB_METHOD_FREEZE_DATAPATH = (1U << UVERBS_ID_NS_SHIFT),
	RXE_IB_METHOD_QUERY_QP,
	RXE_IB_METHOD_QUERY_CQ,
	RXE_IB_METHOD_FREEZE_CONTEXT,
};

/*
 * FREEZE_DATAPATH is per-QP: it pauses/resumes one QP's worker tasks,
 * keyed by a QP IDR handle. It composes cleanly with the plugin's
 * per-uobject enumeration (INFO_HANDLES -> FREEZE_DATAPATH -> QUERY_QP).
 * @FREEZE selects pause (1) vs resume (0); resume exists for test
 * symmetry, the dump flow never thaws (the dumpee is killed).
 */
enum rxe_ib_freeze_datapath_attrs {
	RXE_IB_ATTR_FREEZE_DATAPATH_QP_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	RXE_IB_ATTR_FREEZE_DATAPATH_FREEZE,
};

/*
 * FREEZE_CONTEXT is the ucontext-scoped freeze-all: a single call that
 * pauses (or resumes) every user QP owned by the calling uverbs fd, so
 * CRIU can quiesce the whole RDMA datapath at the early CHECKPOINT_DEVICES
 * hook with one ioctl, before per-QP fds are resolved/dumped. It takes no
 * QP handle -- the caller is identified by ib_uverbs_get_ucontext() and
 * the QP set is enumerated from rxe's own QP pool filtered by owning
 * ucontext (a driver cannot reach the core-internal ufile object walk).
 * Idempotent and order-independent vs FREEZE_DATAPATH (both drive the
 * same per-QP rxe_qp_pause/resume). @FREEZE selects pause (1) / resume (0).
 */
enum rxe_ib_freeze_context_attrs {
	RXE_IB_ATTR_FREEZE_CONTEXT_FREEZE = (1U << UVERBS_ID_NS_SHIFT),
};

enum rxe_ib_query_qp_attrs {
	RXE_IB_ATTR_QUERY_QP_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	RXE_IB_ATTR_QUERY_QP_RESP_BLOB,
	RXE_IB_ATTR_QUERY_QP_RESP_USER_HANDLE,
	/*
	 * B1 in-flight images (optional): variable-length raw byte regions
	 * the dumper round-trips opaquely into the RESTORE_QP UHW_IN tail.
	 * Lengths are reported in rxe_restore_qp_req::{sq,rq,res}_image_bytes.
	 * Absent/zero-length for a drained QP.
	 */
	RXE_IB_ATTR_QUERY_QP_RESP_SQ_IMAGE,
	RXE_IB_ATTR_QUERY_QP_RESP_RQ_IMAGE,
	RXE_IB_ATTR_QUERY_QP_RESP_RES,
};

enum rxe_ib_query_cq_attrs {
	RXE_IB_ATTR_QUERY_CQ_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	RXE_IB_ATTR_QUERY_CQ_RESP_BLOB,
	/*
	 * In-flight CQE ring image (optional): variable-length raw byte
	 * region appended on QUERY_CQ. Length is reported in
	 * rxe_query_cq_resp::cqe_image_bytes. Absent/zero-length for an
	 * empty CQ. Mirrors RXE_IB_ATTR_QUERY_QP_RESP_SQ_IMAGE.
	 */
	RXE_IB_ATTR_QUERY_CQ_RESP_CQE_IMAGE,
};

#endif
