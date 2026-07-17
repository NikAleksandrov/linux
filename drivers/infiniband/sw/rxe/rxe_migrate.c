// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * CRIU migration driver-private uverbs methods for rxe.
 *
 * This is the rxe arm of the dump-side choreography. The matching
 * restore path is the GENERIC UVERBS_OBJECT_RESTORE family
 * (RESTORE_PD/CQ/MR/QP) dispatched through ib_device_ops.restore_*;
 * rxe needs a driver-private object (RXE_IB_OBJECT_MIGRATE) only for the
 * dump-side verbs that have no hardware-generic shape:
 *
 *   FREEZE_DATAPATH  non-destructively pause a QP's send_task
 *                    (requester+completer) and recv_task (responder)
 *                    so the kernel worker threads cannot advance PSNs /
 *                    consumer indices while the plugin snapshots the QP
 *                    and CRIU snapshots the user VMAs. No IBTA state
 *                    transition. See uobject_restore.md §5.3.7.
 *
 *   FREEZE_CONTEXT   ucontext-scoped freeze-all: one call pauses/resumes
 *                    every user QP owned by the calling uverbs fd, for
 *                    CRIU's early CHECKPOINT_DEVICES hook (before per-QP
 *                    fds are resolved). Enumerates rxe's QP pool filtered
 *                    by owning ucontext.
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

/*
 * Emit one optional variable-length image attr on QUERY_QP / QUERY_CQ. The
 * dumper provides the output buffer; it learns the authoritative byte length
 * from the resp blob and round-trips the bytes opaquely. Absent attr (dumper
 * didn't ask) or zero-length image is a no-op; a provided-but-too-small
 * buffer is a hard error (no silent truncation). Used for fixed-shape arrays
 * (the RC responder-resources array); ring images use rxe_query_emit_ring.
 */
static int rxe_query_emit_image(struct uverbs_attr_bundle *attrs,
				u16 attr_id, const void *data, u32 len)
{
	int user_len;

	if (!uverbs_attr_is_valid(attrs, attr_id) || len == 0)
		return 0;

	user_len = uverbs_attr_get_len(attrs, attr_id);
	if (user_len < 0)
		return 0;
	if ((u32)user_len < len)
		return -ENOSPC;

	return uverbs_copy_to(attrs, attr_id, data, len);
}

/*
 * Emit one optional CQE/WQE ring image attr, shipping only the in-flight
 * [consumer, producer) subspan rather than the whole ring. @producer /
 * @consumer are the caller's coherent cursor snapshot; the emitted image
 * agrees byte-for-byte with the cursors carried in the resp blob. A level
 * ring (or a dumper that didn't ask for the attr) is a no-op; a
 * provided-but-too-small buffer is a hard error. Linearizing the live
 * subspan is what keeps a default ib_send_bw ring (rx_depth 512 ->
 * 128 KiB) under the u16 uverbs attr length that the whole-ring blit used
 * to overflow with -ENOSPC.
 */
static int rxe_query_emit_ring(struct uverbs_attr_bundle *attrs, u16 attr_id,
			       const struct rxe_queue *q,
			       u32 producer, u32 consumer)
{
	u32 count = (producer - consumer) & q->index_mask;
	size_t bytes = (size_t)count << q->log2_elem_size;
	int user_len, ret;
	void *tmp;

	if (!uverbs_attr_is_valid(attrs, attr_id) || bytes == 0)
		return 0;

	user_len = uverbs_attr_get_len(attrs, attr_id);
	if (user_len < 0)
		return 0;
	if ((u32)user_len < bytes)
		return -ENOSPC;

	tmp = kmalloc(bytes, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	ret = queue_inflight_capture(q, producer, consumer, tmp, bytes);
	if (ret >= 0)
		ret = uverbs_copy_to(attrs, attr_id, tmp, bytes);

	kfree(tmp);
	return ret;
}

/* v0 QP type gate: the IBTA types rxe_restore_qp accepts. */
static int rxe_migrate_chk_qp_type(const struct ib_qp *ibqp)
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

static int UVERBS_HANDLER(RXE_IB_METHOD_FREEZE_DATAPATH)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_qp *ibqp = uverbs_attr_get_obj(
		attrs, RXE_IB_ATTR_FREEZE_DATAPATH_QP_HANDLE);
	struct rxe_qp *qp;
	u8 freeze;
	int err;

	if (IS_ERR(ibqp))
		return PTR_ERR(ibqp);

	err = rxe_migrate_chk_qp_type(ibqp);
	if (err)
		return err;

	err = uverbs_copy_from(&freeze, attrs,
			       RXE_IB_ATTR_FREEZE_DATAPATH_FREEZE);
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

/*
 * Ucontext-scoped freeze-all: pause (or resume) every user QP owned by
 * the calling uverbs fd in a single call, for CRIU's early
 * CHECKPOINT_DEVICES hook (one ioctl, before per-QP fds are dumped).
 *
 * A driver module cannot reach the core-internal ufile object walk, so
 * the QP set is enumerated from rxe's own QP pool and filtered by owning
 * ucontext. rxe_qp_pause() drains the worker tasks and can sleep, so we
 * take a pool reference on each element under RCU and run the
 * pause/resume outside the read-side critical section.
 */
static int UVERBS_HANDLER(RXE_IB_METHOD_FREEZE_CONTEXT)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_ucontext *ucontext = ib_uverbs_get_ucontext(attrs);
	struct rxe_pool_elem *elem;
	unsigned long index = 0;
	struct rxe_pool *pool;
	struct rxe_dev *rxe;
	u8 freeze;
	int err;

	if (IS_ERR(ucontext))
		return PTR_ERR(ucontext);

	err = uverbs_copy_from(&freeze, attrs,
			       RXE_IB_ATTR_FREEZE_CONTEXT_FREEZE);
	if (err)
		return err;

	rxe = to_rdev(ucontext->device);
	pool = &rxe->qp_pool;

	rcu_read_lock();
	for (elem = xa_find(&pool->xa, &index, ULONG_MAX, XA_PRESENT);
	     elem;
	     elem = xa_find_after(&pool->xa, &index, ULONG_MAX, XA_PRESENT)) {
		struct rxe_qp *qp = elem->obj;

		/* Pin across the (sleeping) pause; skip elems being freed. */
		if (!kref_get_unless_zero(&elem->ref_cnt))
			continue;
		rcu_read_unlock();

		/*
		 * Only this ucontext's user QPs. Skip kernel QPs (no user
		 * datapath) and QPs owned by other processes sharing the
		 * device. Freezing is non-destructive (no IBTA transition),
		 * so unlike the per-QP QUERY path there is no type gate.
		 */
		if (qp->is_user && ib_qp_ucontext(&qp->ibqp) == ucontext) {
			rxe_dbg_qp(qp, "FREEZE_CONTEXT: freeze=%u match\n",
				   freeze);
			if (freeze)
				rxe_qp_pause(qp);
			else
				rxe_qp_resume(qp);
		}

		rxe_put(qp);
		rcu_read_lock();
	}
	rcu_read_unlock();

	return 0;
}

static int UVERBS_HANDLER(RXE_IB_METHOD_QUERY_QP)(
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

	err = rxe_migrate_chk_qp_type(ibqp);
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
	 * B1 in-flight state (design/rxe_inflight_qp_restore.md). The QP is
	 * frozen (rxe_qp_pause) before this verb, so the rings and responder
	 * resources are quiescent; we read the shared-page cursors and report
	 * the in-flight [consumer, producer) subspan byte length (SQ/RQ) and
	 * the resource-array byte length, then emit the raw images below. The
	 * dumper hands these straight back on RESTORE_QP.
	 */
	blob.sq_producer = queue_get_producer(qp->sq.queue, qp->sq.queue->type);
	blob.sq_consumer = queue_get_consumer(qp->sq.queue, qp->sq.queue->type);
	blob.sq_image_bytes = ((blob.sq_producer - blob.sq_consumer) &
			       qp->sq.queue->index_mask)
			      << qp->sq.queue->log2_elem_size;

	if (qp->rq.queue && !qp->srq) {
		blob.rq_producer = queue_get_producer(qp->rq.queue,
						      qp->rq.queue->type);
		blob.rq_consumer = queue_get_consumer(qp->rq.queue,
						      qp->rq.queue->type);
		blob.rq_image_bytes = ((blob.rq_producer - blob.rq_consumer) &
				       qp->rq.queue->index_mask)
				      << qp->rq.queue->log2_elem_size;
	}

	blob.resp_ack_psn	= qp->resp.ack_psn;
	blob.resp_opcode	= qp->resp.opcode;
	blob.resp_status	= qp->resp.status;
	blob.resp_aeth_syndrome	= qp->resp.aeth_syndrome;
	blob.res_head		= qp->resp.res_head;
	blob.res_tail		= qp->resp.res_tail;
	if (qp->resp.resources && qp->attr.max_dest_rd_atomic)
		blob.res_image_bytes = qp->attr.max_dest_rd_atomic *
				       sizeof(struct resp_res);

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

	err = uverbs_copy_to(attrs, RXE_IB_ATTR_QUERY_QP_RESP_USER_HANDLE,
			     &user_handle, sizeof(user_handle));
	if (err)
		return err;

	err = rxe_query_emit_ring(attrs, RXE_IB_ATTR_QUERY_QP_RESP_SQ_IMAGE,
				  qp->sq.queue, blob.sq_producer,
				  blob.sq_consumer);
	if (err)
		return err;

	if (qp->rq.queue && !qp->srq) {
		err = rxe_query_emit_ring(attrs,
					  RXE_IB_ATTR_QUERY_QP_RESP_RQ_IMAGE,
					  qp->rq.queue, blob.rq_producer,
					  blob.rq_consumer);
		if (err)
			return err;
	}

	if (blob.res_image_bytes) {
		err = rxe_query_emit_image(attrs,
					      RXE_IB_ATTR_QUERY_QP_RESP_RES,
					      qp->resp.resources,
					      blob.res_image_bytes);
		if (err)
			return err;
	}

	return 0;
}

static int UVERBS_HANDLER(RXE_IB_METHOD_QUERY_CQ)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_cq *ibcq = uverbs_attr_get_obj(
		attrs, RXE_IB_ATTR_QUERY_CQ_HANDLE);
	struct rxe_query_cq_resp blob = {};
	struct rxe_cq *cq;
	int err;

	if (IS_ERR(ibcq))
		return PTR_ERR(ibcq);

	cq = to_rcq(ibcq);

	/* Kernel-mode CQs have no user mmap ring to round-trip. */
	if (!cq->is_user || !cq->queue || !cq->queue->ip)
		return -ENXIO;

	blob.vm_pgoff = cq->queue->ip->info.offset;
	blob.cqe      = ibcq->cqe;

	/*
	 * In-flight CQE ring round-trip (mirrors QUERY_QP). FREEZE_CONTEXT
	 * only pauses QP tasks (it walks the QP pool), so the CQ itself is
	 * not datapath-frozen; an unfrozen producer (e.g. a QP in another
	 * ucontext sharing this CQ) could advance the cursor under us. Take
	 * cq_lock just long enough to snapshot the two cursors coherently,
	 * then drop it -- cq_lock is an irqsave spinlock and the blob/image
	 * copies below fault to userspace and can sleep, so they must not run
	 * under it. In the real CRIU flow the dumpee is stopped and all its
	 * feeding QPs are frozen, so the ring is quiescent and the post-drop
	 * image read is stable; the lock just closes the cross-context race.
	 *
	 * cqe_image_bytes is the in-flight [consumer, producer) subspan
	 * (unreaped completions), not queue_data_size(): the whole ring
	 * overflows the u16 uverbs attr length for any non-trivial CQ.
	 */
	spin_lock_irq(&cq->cq_lock);
	blob.producer = queue_get_producer(cq->queue, cq->queue->type);
	blob.consumer = queue_get_consumer(cq->queue, cq->queue->type);
	spin_unlock_irq(&cq->cq_lock);

	blob.cqe_image_bytes = ((blob.producer - blob.consumer) &
				cq->queue->index_mask)
			       << cq->queue->log2_elem_size;

	err = uverbs_copy_to(attrs, RXE_IB_ATTR_QUERY_CQ_RESP_BLOB,
			     &blob, sizeof(blob));
	if (err)
		return err;

	return rxe_query_emit_ring(attrs, RXE_IB_ATTR_QUERY_CQ_RESP_CQE_IMAGE,
				   cq->queue, blob.producer, blob.consumer);
}

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_FREEZE_DATAPATH,
	UVERBS_ATTR_IDR(RXE_IB_ATTR_FREEZE_DATAPATH_QP_HANDLE,
			UVERBS_OBJECT_QP,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(RXE_IB_ATTR_FREEZE_DATAPATH_FREEZE,
			   UVERBS_ATTR_TYPE(u8),
			   UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_FREEZE_CONTEXT,
	UVERBS_ATTR_PTR_IN(RXE_IB_ATTR_FREEZE_CONTEXT_FREEZE,
			   UVERBS_ATTR_TYPE(u8),
			   UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_QUERY_QP,
	UVERBS_ATTR_IDR(RXE_IB_ATTR_QUERY_QP_HANDLE,
			UVERBS_OBJECT_QP,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_BLOB,
			    UVERBS_ATTR_TYPE(struct rxe_restore_qp_req),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_USER_HANDLE,
			    UVERBS_ATTR_TYPE(u64),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_SQ_IMAGE,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_RQ_IMAGE,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_RES,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL));

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_QUERY_CQ,
	UVERBS_ATTR_IDR(RXE_IB_ATTR_QUERY_CQ_HANDLE,
			UVERBS_OBJECT_CQ,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_CQ_RESP_BLOB,
			    UVERBS_ATTR_TYPE(struct rxe_query_cq_resp),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_CQ_RESP_CQE_IMAGE,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL));

DECLARE_UVERBS_GLOBAL_METHODS(
	RXE_IB_OBJECT_MIGRATE,
	&UVERBS_METHOD(RXE_IB_METHOD_FREEZE_DATAPATH),
	&UVERBS_METHOD(RXE_IB_METHOD_QUERY_QP),
	&UVERBS_METHOD(RXE_IB_METHOD_QUERY_CQ),
	&UVERBS_METHOD(RXE_IB_METHOD_FREEZE_CONTEXT));

const struct uapi_definition rxe_migrate_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(RXE_IB_OBJECT_MIGRATE),
	{},
};
