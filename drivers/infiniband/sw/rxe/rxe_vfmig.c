// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * VFMIG (CRIU SR-IOV migration) driver-private uverbs methods for rxe.
 *
 * This is the rxe arm of the dump-side choreography. The matching
 * restore path is the GENERIC UVERBS_OBJECT_RESTORE family
 * (RESTORE_PD/CQ/MR/QP) dispatched through ib_device_ops.restore_*;
 * rxe needs a driver-private object only for the two dump-side verbs
 * that have no hardware-generic shape:
 *
 *   FREEZE_DATAPATH  non-destructively pause a QP's send_task
 *                    (requester+completer) and recv_task (responder)
 *                    so the kernel worker threads cannot advance PSNs /
 *                    consumer indices while the plugin snapshots the QP
 *                    and CRIU snapshots the user VMAs. No IBTA state
 *                    transition. See uobject_restore.md §5.3.7.
 *
 *   QUERY_QP         dump-side counterpart to UVERBS_METHOD_RESTORE_QP.
 *                    Packs the full rxe wire state (AV, PSNs, cursors,
 *                    transport attrs, ring mmap offsets) into a payload
 *                    byte-equal to struct rxe_restore_qp_req, plus the
 *                    QP's userspace handle (the async-event cookie).
 *                    Solves the cross-process problem librxe
 *                    introspection cannot: CRIU runs in its own address
 *                    space, so the dumpee's kernel-side QP state must
 *                    come from the kernel. cap / qp_type / qp_state are
 *                    intentionally not emitted -- CRIU sources those
 *                    from the standard IB_USER_VERBS_CMD_QUERY_QP verb
 *                    and NLDEV; only state with no such surface lives
 *                    here.
 *
 *   QUERY_CQ         dump-side counterpart to UVERBS_METHOD_RESTORE_CQ.
 *                    Returns a CQ's ring mmap offset + entry count
 *                    (struct rxe_query_cq_resp) so the dumper sources both
 *                    RESTORE_CQ inputs from the kernel keyed by CQ handle,
 *                    rather than scraping the offset from the dumpee's
 *                    smaps cdev VMAs. That FIFO crutch cannot survive a
 *                    mixed CQ+QP ufile (QP rings are also cdev VMAs but
 *                    are sourced via QUERY_QP, so they pollute the FIFO);
 *                    QUERY_CQ makes CQ and QP uniformly kernel-sourced.
 *
 * The QP methods take a QP IDR handle and QUERY_CQ takes a CQ IDR handle
 * (UVERBS_ACCESS_READ) on the dumpee's own uverbs fd; the security
 * boundary is the ufile that owns the object.
 */

#include <rdma/uverbs_ioctl.h>
#include <rdma/uverbs_std_types.h>
#include <rdma/uverbs_types.h>
#include <rdma/rxe_user_ioctl_cmds.h>

#include "rxe.h"
#include "rxe_queue.h"

#define UVERBS_MODULE_NAME rdma_rxe
#include <rdma/uverbs_named_ioctl.h>

/* v0 QP type gate: the IBTA types rxe_restore_qp accepts. */
static int rxe_vfmig_chk_qp_type(const struct ib_qp *ibqp)
{
	switch (ibqp->qp_type) {
	case IB_QPT_RC:
	case IB_QPT_UC:
	case IB_QPT_UD:
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int UVERBS_HANDLER(RXE_IB_METHOD_VFMIG_FREEZE_DATAPATH)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_qp *ibqp = uverbs_attr_get_obj(
		attrs, RXE_IB_ATTR_VFMIG_FREEZE_DATAPATH_QP_HANDLE);
	struct rxe_qp *qp;
	u8 freeze;
	int err;

	if (IS_ERR(ibqp))
		return PTR_ERR(ibqp);

	err = rxe_vfmig_chk_qp_type(ibqp);
	if (err)
		return err;

	err = uverbs_copy_from(&freeze, attrs,
			       RXE_IB_ATTR_VFMIG_FREEZE_DATAPATH_FREEZE);
	if (err)
		return err;

	qp = to_rqp(ibqp);

	/* Kernel-mode QPs have no user datapath worth freezing. */
	if (!qp->is_user)
		return -ENXIO;

	if (freeze)
		rxe_qp_pause(qp);
	else
		rxe_qp_resume(qp);

	return 0;
}

static int UVERBS_HANDLER(RXE_IB_METHOD_VFMIG_QUERY_QP)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_qp *ibqp = uverbs_attr_get_obj(
		attrs, RXE_IB_ATTR_QUERY_QP_HANDLE);
	struct rxe_restore_qp_req blob = {};
	struct rxe_qp *qp;
	u64 user_handle;
	int err;

	if (IS_ERR(ibqp))
		return PTR_ERR(ibqp);

	err = rxe_vfmig_chk_qp_type(ibqp);
	if (err)
		return err;

	qp = to_rqp(ibqp);

	/* No user-side wire state to emit for kernel QPs. */
	if (!qp->is_user || !qp->sq.queue)
		return -ENXIO;

	/* identity + ring mmap offsets */
	blob.qpn = ibqp->qp_num;
	if (qp->sq.queue->ip)
		blob.sq_vm_pgoff = qp->sq.queue->ip->info.offset;
	if (qp->rq.queue && qp->rq.queue->ip)
		blob.rq_vm_pgoff = qp->rq.queue->ip->info.offset;

	/* ib_qp_attr-class wire state */
	memcpy(&blob.av, &qp->pri_av, sizeof(blob.av));
	blob.dest_qp_num	= qp->attr.dest_qp_num;
	blob.qkey		= qp->attr.qkey;
	blob.sq_psn		= qp->attr.sq_psn;
	blob.rq_psn		= qp->attr.rq_psn;
	blob.qp_access_flags	= qp->attr.qp_access_flags;
	blob.max_rd_atomic	= qp->attr.max_rd_atomic;
	blob.max_dest_rd_atomic = qp->attr.max_dest_rd_atomic;
	blob.pkey_index		= qp->attr.pkey_index;
	blob.path_mtu		= qp->attr.path_mtu;
	blob.retry_cnt		= qp->attr.retry_cnt;
	blob.rnr_retry		= qp->attr.rnr_retry;
	blob.min_rnr_timer	= qp->attr.min_rnr_timer;
	blob.timeout		= qp->attr.timeout;
	blob.port_num		= qp->attr.port_num;
	blob.sq_sig_all		= (qp->sq_sig_type == IB_SIGNAL_ALL_WR) ? 1 : 0;

	/* live cursors ib_modify_qp cannot express */
	blob.req_psn		= qp->req.psn;
	blob.comp_psn		= qp->comp.psn;
	blob.resp_psn		= qp->resp.psn;
	blob.resp_msn		= qp->resp.msn;
	blob.req_wqe_index	= qp->req.wqe_index;
	blob.ssn		= atomic_read(&qp->ssn);

	/*
	 * The async-event cookie the source's ibv_create_qp recorded on
	 * the QP uobject. Not standard-queryable, so CRIU must preserve
	 * it through the kernel-sourced dump (mirrors the mlx5 verb).
	 */
	user_handle = ib_qp_user_handle(ibqp);

	err = uverbs_copy_to(attrs, RXE_IB_ATTR_QUERY_QP_RESP_BLOB,
			     &blob, sizeof(blob));
	if (err)
		return err;

	return uverbs_copy_to(attrs, RXE_IB_ATTR_QUERY_QP_RESP_USER_HANDLE,
			      &user_handle, sizeof(user_handle));
}

static int UVERBS_HANDLER(RXE_IB_METHOD_VFMIG_QUERY_CQ)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_cq *ibcq = uverbs_attr_get_obj(
		attrs, RXE_IB_ATTR_VFMIG_QUERY_CQ_HANDLE);
	struct rxe_query_cq_resp blob = {};
	struct rxe_cq *cq;

	if (IS_ERR(ibcq))
		return PTR_ERR(ibcq);

	cq = to_rcq(ibcq);

	/* Kernel-mode CQs have no user mmap ring to round-trip. */
	if (!cq->is_user || !cq->queue || !cq->queue->ip)
		return -ENXIO;

	blob.vm_pgoff = cq->queue->ip->info.offset;
	blob.cqe      = ibcq->cqe;

	return uverbs_copy_to(attrs, RXE_IB_ATTR_VFMIG_QUERY_CQ_RESP_BLOB,
			      &blob, sizeof(blob));
}

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_VFMIG_FREEZE_DATAPATH,
	UVERBS_ATTR_IDR(RXE_IB_ATTR_VFMIG_FREEZE_DATAPATH_QP_HANDLE,
			UVERBS_OBJECT_QP,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(RXE_IB_ATTR_VFMIG_FREEZE_DATAPATH_FREEZE,
			   UVERBS_ATTR_TYPE(u8),
			   UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_VFMIG_QUERY_QP,
	UVERBS_ATTR_IDR(RXE_IB_ATTR_QUERY_QP_HANDLE,
			UVERBS_OBJECT_QP,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_BLOB,
			    UVERBS_ATTR_TYPE(struct rxe_restore_qp_req),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_USER_HANDLE,
			    UVERBS_ATTR_TYPE(u64),
			    UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_VFMIG_QUERY_CQ,
	UVERBS_ATTR_IDR(RXE_IB_ATTR_VFMIG_QUERY_CQ_HANDLE,
			UVERBS_OBJECT_CQ,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_VFMIG_QUERY_CQ_RESP_BLOB,
			    UVERBS_ATTR_TYPE(struct rxe_query_cq_resp),
			    UA_MANDATORY));

DECLARE_UVERBS_GLOBAL_METHODS(
	RXE_IB_OBJECT_VFMIG,
	&UVERBS_METHOD(RXE_IB_METHOD_VFMIG_FREEZE_DATAPATH),
	&UVERBS_METHOD(RXE_IB_METHOD_VFMIG_QUERY_QP),
	&UVERBS_METHOD(RXE_IB_METHOD_VFMIG_QUERY_CQ));

const struct uapi_definition rxe_vfmig_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(RXE_IB_OBJECT_VFMIG),
	{},
};
