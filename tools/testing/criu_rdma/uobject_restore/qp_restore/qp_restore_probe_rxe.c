// SPDX-License-Identifier: GPL-2.0
/*
 * qp_restore_probe_rxe -- end-to-end validation of S6a A5:
 * UVERBS_METHOD_RESTORE_QP + rxe_restore_qp + rxe_qp_restore_wire_state
 * (design/uobject_restore.md §9.1 S6a).
 *
 * Single process, single rxe0 device, two ucontexts:
 *
 *   SOURCE (libibverbs, normal ucontext)
 *     1. alloc PD + CQ, create an RC QP, drive it RESET->INIT->RTR->RTS
 *        as a self-loopback (dest_qp_num = own qpn, AV at our own GID).
 *     2. RXE_IB_METHOD_QUERY_QP snapshots the full wire state into
 *        a struct rxe_restore_qp_req ("the blob"). This is exactly what
 *        a CRIU dump would stash in the image.
 *     3. RXE_IB_METHOD_FREEZE_DATAPATH(freeze=1) quiesces it, then
 *        DESTROY_QP frees the source qpn back to rxe's qp_pool so the
 *        restore can re-install at the same number.
 *
 *   RESTORE (raw uverbs, RXE_ALLOC_UCTX_RESTORE_MODE ucontext)
 *     4. RESTORE_PD(target=PD_H) + RESTORE_CQ(target=CQ_H) rebuild the
 *        QP's dependencies at caller-chosen ufile handles.
 *     5. RESTORE_QP(target=QP_H, pd=PD_H, send_cq=recv_cq=CQ_H,
 *        type=RC, state=RTS, cap=<source cap>, uhw_in=blob) installs the
 *        QP. Assert RESP_QPN == source qpn (qpn identity contract: rxe
 *        qpns are wire-visible BTH DestQP, so the restored QP MUST land
 *        at the captured number).
 *     6. INFO_HANDLES(QP) reports QP_H.
 *     7. QUERY_QP(QP_H) re-snapshots the restored QP; assert it is
 *        byte-identical to the source blob (every wire-relevant field
 *        round-tripped through rxe_qp_restore_wire_state -- AV, PSN
 *        bases, live req/comp/resp cursors, wqe index, ssn, msn, mtu,
 *        timers, access flags, the forced sq/rq vm_pgoffs, ...).
 *     8. DESTROY_QP(QP_H) tears the restored QP down cleanly.
 *
 * Like the other rxe restore probes we bypass libibverbs's rxe provider
 * for the restore ucontext and talk to the uverbs cdev directly so we
 * can pass RXE_ALLOC_UCTX_RESTORE_MODE in udata; libibverbs is used for
 * the source QP (clean RC bring-up) and for device discovery.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/qp_restore/qp_restore_probe_rxe
 * Usage:
 *   ./qp_restore_probe_rxe [<ibdev>]      # default rxe0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/in.h>

#include <infiniband/verbs.h>
#include <rdma/ib_user_verbs.h>

/* ----------------------- ioctl wire format ------------------------------- */

#define RDMA_IOCTL_MAGIC	0x1b
#define RDMA_VERBS_IOCTL	_IOWR(RDMA_IOCTL_MAGIC, 1, \
					      struct ib_uverbs_ioctl_hdr)

enum {
	UVERBS_ATTR_F_MANDATORY		= 1u << 0,
	UVERBS_ATTR_F_VALID_OUTPUT	= 1u << 1,
};

struct ib_uverbs_attr {
	uint16_t attr_id;
	uint16_t len;
	uint16_t flags;
	uint16_t attr_data_reserved;
	uint64_t data;
};

struct ib_uverbs_ioctl_hdr {
	uint16_t length;
	uint16_t object_id;
	uint16_t method_id;
	uint16_t num_attrs;
	uint64_t reserved1;
	uint32_t driver_id;
	uint32_t reserved2;
	struct ib_uverbs_attr attrs[];
};

/* enum in include/uapi/rdma/ib_user_ioctl_verbs.h */
#define RDMA_DRIVER_RXE_LOCAL			14

#define UVERBS_ID_NS_SHIFT			12
#define UVERBS_ID_DRIVER_NS			(1u << UVERBS_ID_NS_SHIFT)

/* Core object/method ids from include/uapi/rdma/ib_user_ioctl_cmds.h. */
#define UVERBS_OBJECT_DEVICE			0
/* enum uverbs_default_objects: DEVICE=0, PD=1, COMP_CHANNEL=2, CQ=3, QP=4. */
#define UVERBS_OBJECT_QP			4
#define UVERBS_OBJECT_RESTORE			18

#define UVERBS_METHOD_INFO_HANDLES		1
#define UVERBS_ATTR_INFO_OBJECT_ID		0
#define UVERBS_ATTR_INFO_TOTAL_HANDLES		1
#define UVERBS_ATTR_INFO_HANDLES_LIST		2

/* enum uverbs_methods_restore: PD=0, MR=1, CQ=2, QP=3. */
#define UVERBS_METHOD_RESTORE_PD		0
#define UVERBS_METHOD_RESTORE_CQ		2
#define UVERBS_METHOD_RESTORE_QP		3

enum {
	UVERBS_ATTR_RESTORE_PD_HANDLE		= 0,
};

enum {
	UVERBS_ATTR_RESTORE_CQ_HANDLE		= 0,
	UVERBS_ATTR_RESTORE_CQ_CQE		= 1,
	UVERBS_ATTR_RESTORE_CQ_USER_HANDLE	= 2,
	UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR	= 3,
	UVERBS_ATTR_RESTORE_CQ_FLAGS		= 4,
	UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL	= 5,
	UVERBS_ATTR_RESTORE_CQ_EVENT_FD		= 6,
	UVERBS_ATTR_RESTORE_CQ_RESP_CQE		= 7,
};

enum {
	UVERBS_ATTR_RESTORE_QP_HANDLE		= 0,
	UVERBS_ATTR_RESTORE_QP_PD_HANDLE	= 1,
	UVERBS_ATTR_RESTORE_QP_SEND_CQ_HANDLE	= 2,
	UVERBS_ATTR_RESTORE_QP_RECV_CQ_HANDLE	= 3,
	UVERBS_ATTR_RESTORE_QP_SRQ_HANDLE	= 4,
	UVERBS_ATTR_RESTORE_QP_TYPE		= 5,
	UVERBS_ATTR_RESTORE_QP_STATE		= 6,
	UVERBS_ATTR_RESTORE_QP_USER_HANDLE	= 7,
	UVERBS_ATTR_RESTORE_QP_CAP		= 8,
	UVERBS_ATTR_RESTORE_QP_CREATE_FLAGS	= 9,
	UVERBS_ATTR_RESTORE_QP_EVENT_FD		= 10,
	UVERBS_ATTR_RESTORE_QP_RESP_QPN		= 11,
};

/* UA_UHW() halves: UVERBS_ID_DRIVER_NS (4096) IN, +1 OUT. */
#define UVERBS_ATTR_UHW_IN			((uint16_t)4096)
#define UVERBS_ATTR_UHW_OUT			((uint16_t)4097)

/* Mirror of include/uapi/rdma/rxe_user_ioctl_cmds.h (dump-side verbs). */
#define RXE_IB_OBJECT_MIGRATE			(UVERBS_ID_DRIVER_NS + 0u)
#define RXE_IB_METHOD_FREEZE_DATAPATH	(1u << UVERBS_ID_NS_SHIFT)
#define RXE_IB_METHOD_QUERY_QP		((1u << UVERBS_ID_NS_SHIFT) + 1u)
#define RXE_IB_ATTR_FREEZE_DATAPATH_QP_HANDLE (1u << UVERBS_ID_NS_SHIFT)
#define RXE_IB_ATTR_FREEZE_DATAPATH_FREEZE    ((1u << UVERBS_ID_NS_SHIFT) + 1u)
#define RXE_IB_ATTR_QUERY_QP_HANDLE	(1u << UVERBS_ID_NS_SHIFT)
#define RXE_IB_ATTR_QUERY_QP_RESP_BLOB	((1u << UVERBS_ID_NS_SHIFT) + 1u)
#define RXE_IB_ATTR_QUERY_QP_RESP_USER_HANDLE ((1u << UVERBS_ID_NS_SHIFT) + 2u)
#define RXE_IB_ATTR_QUERY_QP_RESP_SQ_IMAGE ((1u << UVERBS_ID_NS_SHIFT) + 3u)
#define RXE_IB_ATTR_QUERY_QP_RESP_RQ_IMAGE ((1u << UVERBS_ID_NS_SHIFT) + 4u)
#define RXE_IB_ATTR_QUERY_QP_RESP_RES	((1u << UVERBS_ID_NS_SHIFT) + 5u)
/* Ucontext-scoped thaw trigger (method index 3 in rxe_ib_migrate_methods). */
#define RXE_IB_METHOD_FREEZE_CONTEXT	((1u << UVERBS_ID_NS_SHIFT) + 3u)
#define RXE_IB_ATTR_FREEZE_CONTEXT_FREEZE (1u << UVERBS_ID_NS_SHIFT)

/* ib_qp_type / ib_qp_state values used by the RESTORE_QP method args. */
#define IB_QPT_RC_LOCAL				2
#define IB_QPS_RTS_LOCAL			3

/*
 * Mirrors include/uapi/rdma/rdma_user_rxe.h. Re-declared locally to
 * avoid clashing with the system <netinet/in.h> pulled in by verbs.h --
 * same convention as cq_restore_probe_rxe.c / qp_query_probe_rxe.c.
 * Layouts MUST stay byte-identical to the kernel uapi structs.
 */
enum {
	RXE_ALLOC_UCTX_RESTORE_MODE = 1u << 0,
};

struct rxe_alloc_ucontext_req {
	uint32_t flags;
	uint32_t reserved;
};

struct rxe_mminfo_local {
	uint64_t	offset;
	uint32_t	size;
	uint32_t	pad;
};

struct rxe_create_cq_resp_local {
	struct rxe_mminfo_local mi;
};

struct rxe_create_qp_resp_local {
	struct rxe_mminfo_local rq_mi;
	struct rxe_mminfo_local sq_mi;
};

union rxe_gid_local {
	uint8_t  raw[16];
	struct {
		uint64_t subnet_prefix;
		uint64_t interface_id;
	} global;
};

struct rxe_global_route_local {
	union rxe_gid_local	dgid;
	uint32_t		flow_label;
	uint8_t			sgid_index;
	uint8_t			hop_limit;
	uint8_t			traffic_class;
};

struct rxe_av_local {
	uint8_t				port_num;
	uint8_t				network_type;
	uint8_t				dmac[6];
	struct rxe_global_route_local	grh;
	union {
		struct sockaddr_in	_sockaddr_in;
		struct sockaddr_in6	_sockaddr_in6;
	} sgid_addr, dgid_addr;
};

struct rxe_restore_qp_req_local {
	struct rxe_av_local	av;
	uint64_t		sq_vm_pgoff;
	uint64_t		rq_vm_pgoff;
	uint32_t		qpn;
	uint32_t		dest_qp_num;
	uint32_t		qkey;
	uint32_t		sq_psn;
	uint32_t		rq_psn;
	uint32_t		qp_access_flags;
	uint32_t		max_rd_atomic;
	uint32_t		max_dest_rd_atomic;
	uint32_t		req_psn;
	uint32_t		comp_psn;
	uint32_t		resp_psn;
	uint32_t		resp_msn;
	uint32_t		req_wqe_index;
	uint32_t		ssn;
	uint16_t		pkey_index;
	uint8_t			path_mtu;
	uint8_t			retry_cnt;
	uint8_t			rnr_retry;
	uint8_t			min_rnr_timer;
	uint8_t			timeout;
	uint8_t			port_num;
	uint8_t			sq_sig_all;
	uint8_t			resp_aeth_syndrome;
	uint16_t		reserved;
	uint32_t		sq_producer;
	uint32_t		sq_consumer;
	uint32_t		rq_producer;
	uint32_t		rq_consumer;
	uint32_t		resp_ack_psn;
	int32_t			resp_opcode;
	uint32_t		resp_status;
	uint32_t		res_head;
	uint32_t		res_tail;
	uint32_t		sq_image_bytes;
	uint32_t		rq_image_bytes;
	uint32_t		res_image_bytes;
	uint64_t		reserved2;
};

/* ib_uverbs_qp_cap (include/uapi/rdma/ib_user_verbs.h). */
struct ib_uverbs_qp_cap_local {
	uint32_t max_send_wr;
	uint32_t max_recv_wr;
	uint32_t max_send_sge;
	uint32_t max_recv_sge;
	uint32_t max_inline_data;
};

/*
 * Per-image output buffers for the B1 in-flight QUERY_QP attrs. NULL
 * buffer => that image attr is omitted (the kernel then skips it). The
 * authoritative byte length lands in blob->{sq,rq,res}_image_bytes.
 */
struct qp_images {
	void		*sq;
	void		*rq;
	void		*res;
	uint32_t	cap;	/* capacity of each buffer */
};

#define PD_TARGET_HANDLE			0x4242u
#define CQ_TARGET_HANDLE			0x4244u
#define QP_TARGET_HANDLE			0x4248u
#define USER_HANDLE_TAG				0xDEADBEEFCAFE0003ull
#define CQE_REQUESTED				16u

/* ----------------------- legacy-write helpers ---------------------------- */

static int do_get_context(int fd, uint32_t rxe_flags,
			  struct ib_uverbs_get_context_resp *resp_out)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_get_context	core;
		struct rxe_alloc_ucontext_req	req;
	} __attribute__((packed)) cmd = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_GET_CONTEXT;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= sizeof(*resp_out) / 4;
	cmd.core.response	= (uintptr_t)resp_out;
	cmd.req.flags		= rxe_flags;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	return 0;
}

static int do_destroy_qp(int fd, uint32_t qp_handle)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_destroy_qp	core;
	} __attribute__((packed)) cmd = {};
	struct ib_uverbs_destroy_qp_resp resp = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_DESTROY_QP;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= sizeof(resp) / 4;
	cmd.core.response	= (uintptr_t)&resp;
	cmd.core.qp_handle	= qp_handle;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	return 0;
}

/* ----------------------- ioctl helpers ----------------------------------- */

/*
 * RESTORE_PD: rxe_restore_pd is a thin shim over rxe_alloc_pd and takes
 * no UHW, so the HANDLE attr is the only thing on the wire.
 */
static int do_restore_pd(int fd, uint32_t target_handle)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[1];
	} cmd = {};

	cmd.hdr.object_id	= UVERBS_OBJECT_RESTORE;
	cmd.hdr.method_id	= UVERBS_METHOD_RESTORE_PD;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;
	cmd.hdr.num_attrs	= 1;
	cmd.hdr.length		= sizeof(cmd);

	cmd.attrs[0].attr_id	= UVERBS_ATTR_RESTORE_PD_HANDLE;
	cmd.attrs[0].len	= sizeof(uint32_t);
	cmd.attrs[0].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[0].data	= target_handle;

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

/*
 * RESTORE_CQ: mirrors cq_restore_probe_rxe::do_restore_cq, comp_channel
 * always omitted (v0 plugin path). UHW_OUT carries rxe_create_cq_resp
 * which rxe_restore_cq requires (outlen >= sizeof).
 */
static int do_restore_cq(int fd, uint32_t target_handle, uint32_t cqe,
			 uint64_t user_handle, uint32_t comp_vector,
			 uint32_t *resp_cqe_out,
			 struct rxe_mminfo_local *mi_out)
{
	struct rxe_create_cq_resp_local uhw_out = {};
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[6];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= UVERBS_OBJECT_RESTORE;
	cmd.hdr.method_id	= UVERBS_METHOD_RESTORE_CQ;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_HANDLE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= target_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_CQE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= cqe;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_USER_HANDLE;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= user_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= comp_vector;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_RESP_CQE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)resp_cqe_out;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_UHW_OUT;
	cmd.attrs[n].len	= sizeof(uhw_out);
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)&uhw_out;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	if (mi_out)
		*mi_out = uhw_out.mi;
	return 0;
}

/*
 * RESTORE_QP. Core mandatory attrs (HANDLE / PD / SEND_CQ / RECV_CQ /
 * TYPE / STATE / USER_HANDLE / CAP / RESP_QPN) + UHW_IN (the wire-state
 * blob) + UHW_OUT (rxe_create_qp_resp; rxe_restore_qp requires
 * outlen >= sizeof). CREATE_FLAGS omitted (rxe rejects non-zero).
 *
 * Wire encoding (uverbs_ioctl.c):
 *   PTR_IN(u32): len 4, inline value      IDR-class: len 0, inline handle
 *   PTR_IN(u64): len 8, inline value      CONST_IN(enum): len 8, inline
 *   PTR_OUT(u32): len 4, &user_buf        PTR_IN(struct)/UHW: len sz, &buf
 */
static int do_restore_qp(int fd, uint32_t target_handle, uint32_t pd_handle,
			 uint32_t send_cq_handle, uint32_t recv_cq_handle,
			 uint64_t qp_type, uint64_t qp_state,
			 uint64_t user_handle,
			 const struct ib_uverbs_qp_cap_local *cap,
			 const struct rxe_restore_qp_req_local *uhw,
			 const struct qp_images *imgs,
			 uint32_t *resp_qpn_out,
			 struct rxe_create_qp_resp_local *resp_out)
{
	struct rxe_create_qp_resp_local uhw_out = {};
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[12];
	} cmd = {};
	uint8_t *uhw_in = NULL;
	size_t uhw_in_len = sizeof(*uhw);
	const void *uhw_in_ptr = uhw;
	unsigned int n = 0;
	int rc;

	/*
	 * B1: concatenate the captured ring/responder images after the
	 * fixed header, in the kernel's slice order (SQ, RQ, RES). A
	 * drained restore passes imgs==NULL and the header alone.
	 */
	if (imgs) {
		size_t off, tail = (size_t)uhw->sq_image_bytes +
				   uhw->rq_image_bytes + uhw->res_image_bytes;

		uhw_in_len = sizeof(*uhw) + tail;
		uhw_in = malloc(uhw_in_len);
		if (!uhw_in)
			return -ENOMEM;
		memcpy(uhw_in, uhw, sizeof(*uhw));
		off = sizeof(*uhw);
		if (uhw->sq_image_bytes) {
			memcpy(uhw_in + off, imgs->sq, uhw->sq_image_bytes);
			off += uhw->sq_image_bytes;
		}
		if (uhw->rq_image_bytes) {
			memcpy(uhw_in + off, imgs->rq, uhw->rq_image_bytes);
			off += uhw->rq_image_bytes;
		}
		if (uhw->res_image_bytes)
			memcpy(uhw_in + off, imgs->res, uhw->res_image_bytes);
		uhw_in_ptr = uhw_in;
	}

	cmd.hdr.object_id	= UVERBS_OBJECT_RESTORE;
	cmd.hdr.method_id	= UVERBS_METHOD_RESTORE_QP;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_HANDLE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= target_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_PD_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= pd_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_SEND_CQ_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= send_cq_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_RECV_CQ_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= recv_cq_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_TYPE;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= qp_type;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_STATE;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= qp_state;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_USER_HANDLE;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= user_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_CAP;
	cmd.attrs[n].len	= sizeof(*cap);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)cap;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_RESP_QPN;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)resp_qpn_out;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_UHW_IN;
	cmd.attrs[n].len	= uhw_in_len;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)uhw_in_ptr;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_UHW_OUT;
	cmd.attrs[n].len	= sizeof(uhw_out);
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)&uhw_out;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	rc = ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0 ? -errno : 0;
	free(uhw_in);
	if (rc)
		return rc;
	if (resp_out)
		*resp_out = uhw_out;
	return 0;
}

static int do_migrate_query_qp(int fd, uint32_t qp_handle,
			     struct rxe_restore_qp_req_local *blob_out,
			     const struct qp_images *imgs)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[6];
	} cmd = {};
	/* RESP_USER_HANDLE is mandatory but unused by the restore probe. */
	uint64_t user_handle = 0;
	unsigned int n = 0;

	cmd.hdr.object_id	= RXE_IB_OBJECT_MIGRATE;
	cmd.hdr.method_id	= RXE_IB_METHOD_QUERY_QP;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;

	cmd.attrs[n].attr_id	= RXE_IB_ATTR_QUERY_QP_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= qp_handle;
	n++;

	cmd.attrs[n].attr_id	= RXE_IB_ATTR_QUERY_QP_RESP_BLOB;
	cmd.attrs[n].len	= sizeof(*blob_out);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)blob_out;
	n++;

	cmd.attrs[n].attr_id	= RXE_IB_ATTR_QUERY_QP_RESP_USER_HANDLE;
	cmd.attrs[n].len	= sizeof(user_handle);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)&user_handle;
	n++;

	if (imgs && imgs->sq) {
		cmd.attrs[n].attr_id	= RXE_IB_ATTR_QUERY_QP_RESP_SQ_IMAGE;
		cmd.attrs[n].len	= imgs->cap;
		cmd.attrs[n].flags	= 0;
		cmd.attrs[n].data	= (uintptr_t)imgs->sq;
		n++;
	}
	if (imgs && imgs->rq) {
		cmd.attrs[n].attr_id	= RXE_IB_ATTR_QUERY_QP_RESP_RQ_IMAGE;
		cmd.attrs[n].len	= imgs->cap;
		cmd.attrs[n].flags	= 0;
		cmd.attrs[n].data	= (uintptr_t)imgs->rq;
		n++;
	}
	if (imgs && imgs->res) {
		cmd.attrs[n].attr_id	= RXE_IB_ATTR_QUERY_QP_RESP_RES;
		cmd.attrs[n].len	= imgs->cap;
		cmd.attrs[n].flags	= 0;
		cmd.attrs[n].data	= (uintptr_t)imgs->res;
		n++;
	}

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

static int do_migrate_freeze(int fd, uint32_t qp_handle, uint8_t freeze)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[2];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= RXE_IB_OBJECT_MIGRATE;
	cmd.hdr.method_id	= RXE_IB_METHOD_FREEZE_DATAPATH;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;

	cmd.attrs[n].attr_id	= RXE_IB_ATTR_FREEZE_DATAPATH_QP_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= qp_handle;
	n++;

	cmd.attrs[n].attr_id	= RXE_IB_ATTR_FREEZE_DATAPATH_FREEZE;
	cmd.attrs[n].len	= sizeof(freeze);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= freeze;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

/*
 * Ucontext-scoped thaw: the production restore flow installs every QP
 * born-frozen and resumes them all with one FREEZE_CONTEXT(freeze=0) once
 * the whole tree is consistent. We mirror that here to validate that a
 * born-frozen restored QP can be thawed (and exercises rxe_qp_resume's
 * replay kick) rather than leaving it parked.
 */
static int do_migrate_freeze_context(int fd, uint8_t freeze)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[1];
	} cmd = {};

	cmd.hdr.object_id	= RXE_IB_OBJECT_MIGRATE;
	cmd.hdr.method_id	= RXE_IB_METHOD_FREEZE_CONTEXT;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;

	cmd.attrs[0].attr_id	= RXE_IB_ATTR_FREEZE_CONTEXT_FREEZE;
	cmd.attrs[0].len	= sizeof(freeze);
	cmd.attrs[0].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[0].data	= freeze;

	cmd.hdr.num_attrs = 1;
	cmd.hdr.length = sizeof(cmd.hdr) + sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

static int do_info_handles(int fd, uint16_t object_id, uint32_t *handles_out,
			   uint32_t capacity_handles, uint32_t *total_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[3];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= UVERBS_OBJECT_DEVICE;
	cmd.hdr.method_id	= UVERBS_METHOD_INFO_HANDLES;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_INFO_OBJECT_ID;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= object_id;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_INFO_TOTAL_HANDLES;
	cmd.attrs[n].len	= sizeof(*total_out);
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)total_out;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_INFO_HANDLES_LIST;
	cmd.attrs[n].len	= (uint16_t)(capacity_handles * sizeof(uint32_t));
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)handles_out;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

static bool handle_present(const uint32_t *list, uint32_t n, uint32_t want)
{
	for (uint32_t i = 0; i < n; i++)
		if (list[i] == want)
			return true;
	return false;
}

/* ----------------------- source RC self-loopback ------------------------- */

struct rc_qp {
	struct ibv_pd	*pd;
	struct ibv_cq	*cq;
	struct ibv_qp	*qp;
};

/*
 * Pick a routable GID index for a self-loopback RC QP. rxe's GID[0] is
 * the netdev link-local fe80:: address, which has no route on loopback
 * (modify-to-RTR fails -ENETUNREACH). Skip link-local GIDs and return
 * the first usable index (typically the ::ffff:127.0.0.1 or ::1 entry
 * when rxe is layered over lo). Falls back to 0 if nothing else found.
 */
static int pick_gid(struct ibv_context *ctx, uint8_t port,
		    union ibv_gid *gid_out)
{
	struct ibv_port_attr pa = {};
	int i;

	if (ibv_query_port(ctx, port, &pa))
		return -1;
	for (i = 0; i < pa.gid_tbl_len; i++) {
		union ibv_gid g = {};

		if (ibv_query_gid(ctx, port, i, &g))
			continue;
		if (!g.global.subnet_prefix && !g.global.interface_id)
			continue;
		if (g.raw[0] == 0xfe && (g.raw[1] & 0xc0) == 0x80)
			continue;
		*gid_out = g;
		return i;
	}
	if (ibv_query_gid(ctx, port, 0, gid_out))
		return -1;
	return 0;
}

static int build_rts_loopback(struct ibv_context *ctx, struct rc_qp *out)
{
	struct ibv_port_attr port = {};
	union ibv_gid gid = {};
	struct ibv_qp_init_attr iattr = {};
	struct ibv_qp_attr attr = {};
	int flags, sgid_index;

	if (ibv_query_port(ctx, 1, &port)) {
		fprintf(stderr, "qpr: ibv_query_port: %s\n", strerror(errno));
		return -1;
	}
	sgid_index = pick_gid(ctx, 1, &gid);
	if (sgid_index < 0) {
		fprintf(stderr, "qpr: no usable GID on port 1\n");
		return -1;
	}

	out->pd = ibv_alloc_pd(ctx);
	if (!out->pd) {
		fprintf(stderr, "qpr: ibv_alloc_pd: %s\n", strerror(errno));
		return -1;
	}
	out->cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!out->cq) {
		fprintf(stderr, "qpr: ibv_create_cq: %s\n", strerror(errno));
		return -1;
	}

	iattr.send_cq = out->cq;
	iattr.recv_cq = out->cq;
	iattr.cap.max_send_wr = 8;
	iattr.cap.max_recv_wr = 8;
	iattr.cap.max_send_sge = 1;
	iattr.cap.max_recv_sge = 1;
	iattr.qp_type = IBV_QPT_RC;
	iattr.sq_sig_all = 1;
	out->qp = ibv_create_qp(out->pd, &iattr);
	if (!out->qp) {
		fprintf(stderr, "qpr: ibv_create_qp: %s\n", strerror(errno));
		return -1;
	}

	attr.qp_state	= IBV_QPS_INIT;
	attr.pkey_index	= 0;
	attr.port_num	= 1;
	attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
			       IBV_ACCESS_REMOTE_WRITE |
			       IBV_ACCESS_REMOTE_READ;
	flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
		IBV_QP_ACCESS_FLAGS;
	if (ibv_modify_qp(out->qp, &attr, flags)) {
		fprintf(stderr, "qpr: modify INIT: %s\n", strerror(errno));
		return -1;
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state		= IBV_QPS_RTR;
	attr.path_mtu		= IBV_MTU_1024;
	attr.dest_qp_num	= out->qp->qp_num;
	attr.rq_psn		= 0x100;
	attr.max_dest_rd_atomic	= 1;
	attr.min_rnr_timer	= 12;
	attr.ah_attr.is_global	= 1;
	attr.ah_attr.port_num	= 1;
	attr.ah_attr.grh.hop_limit = 1;
	attr.ah_attr.grh.sgid_index = sgid_index;
	memcpy(&attr.ah_attr.grh.dgid, &gid, sizeof(gid));
	flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
		IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
		IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
	if (ibv_modify_qp(out->qp, &attr, flags)) {
		fprintf(stderr, "qpr: modify RTR: %s\n", strerror(errno));
		return -1;
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state	= IBV_QPS_RTS;
	attr.sq_psn	= 0x200;
	attr.timeout	= 14;
	attr.retry_cnt	= 7;
	attr.rnr_retry	= 7;
	attr.max_rd_atomic = 1;
	flags = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
		IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;
	if (ibv_modify_qp(out->qp, &attr, flags)) {
		fprintf(stderr, "qpr: modify RTS: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Field-by-field compare of the source snapshot against the re-query of
 * the restored QP. Returns the number of mismatches and logs each one.
 * Every field here is wire-relevant; rxe_qp_restore_wire_state must have
 * stamped it back identically (and forced the queue vm_pgoffs).
 */
static int blob_compare(const struct rxe_restore_qp_req_local *src,
			const struct rxe_restore_qp_req_local *got)
{
	int fails = 0;

#define CMP(field, fmt)							\
	do {								\
		if (src->field != got->field) {				\
			fprintf(stderr,					\
				"  FAIL " #field ": src=" fmt		\
				" restored=" fmt "\n",			\
				src->field, got->field);		\
			fails++;					\
		}							\
	} while (0)

	CMP(qpn, "0x%x");
	CMP(dest_qp_num, "0x%x");
	CMP(qkey, "0x%x");
	CMP(sq_psn, "0x%x");
	CMP(rq_psn, "0x%x");
	CMP(qp_access_flags, "0x%x");
	CMP(max_rd_atomic, "%u");
	CMP(max_dest_rd_atomic, "%u");
	CMP(req_psn, "0x%x");
	CMP(comp_psn, "0x%x");
	CMP(resp_psn, "0x%x");
	CMP(resp_msn, "0x%x");
	CMP(req_wqe_index, "%u");
	CMP(ssn, "%u");
	CMP(pkey_index, "%u");
	CMP(path_mtu, "%u");
	CMP(retry_cnt, "%u");
	CMP(rnr_retry, "%u");
	CMP(min_rnr_timer, "%u");
	CMP(timeout, "%u");
	CMP(port_num, "%u");
	CMP(sq_sig_all, "%u");
	/* B1 in-flight cursors + responder scalars + image geometry */
	CMP(sq_producer, "%u");
	CMP(sq_consumer, "%u");
	CMP(rq_producer, "%u");
	CMP(rq_consumer, "%u");
	CMP(resp_ack_psn, "0x%x");
	CMP(resp_opcode, "%d");
	CMP(resp_status, "%u");
	CMP(resp_aeth_syndrome, "%u");
	CMP(res_head, "%u");
	CMP(res_tail, "%u");
	CMP(sq_image_bytes, "%u");
	CMP(rq_image_bytes, "%u");
	CMP(res_image_bytes, "%u");
#undef CMP

	if (src->sq_vm_pgoff != got->sq_vm_pgoff) {
		fprintf(stderr, "  FAIL sq_vm_pgoff: src=0x%llx restored=0x%llx\n",
			(unsigned long long)src->sq_vm_pgoff,
			(unsigned long long)got->sq_vm_pgoff);
		fails++;
	}
	if (src->rq_vm_pgoff != got->rq_vm_pgoff) {
		fprintf(stderr, "  FAIL rq_vm_pgoff: src=0x%llx restored=0x%llx\n",
			(unsigned long long)src->rq_vm_pgoff,
			(unsigned long long)got->rq_vm_pgoff);
		fails++;
	}

	if (memcmp(&src->av, &got->av, sizeof(src->av)) != 0) {
		fprintf(stderr, "  FAIL av: address vector differs\n");
		fails++;
	}
	return fails;
}

/* ----------------------- device discovery -------------------------------- */

static int resolve_cdev_path(const char *ibdev_name, char *out, size_t outlen)
{
	struct ibv_device **list;
	int n, i, ret = -ENODEV;

	list = ibv_get_device_list(&n);
	if (!list || n == 0) {
		fprintf(stderr, "qpr: ibv_get_device_list returned nothing\n");
		return -ENODEV;
	}
	for (i = 0; i < n; i++) {
		if (strcmp(ibv_get_device_name(list[i]), ibdev_name) == 0) {
			snprintf(out, outlen, "/dev/infiniband/%s",
				 list[i]->dev_name);
			ret = 0;
			break;
		}
	}
	if (ret)
		fprintf(stderr, "qpr: ibdev '%s' not found\n", ibdev_name);
	ibv_free_device_list(list);
	return ret;
}

/* ----------------------- main -------------------------------------------- */

int main(int argc, char **argv)
{
	const char *ibdev = argc > 1 ? argv[1] : "rxe0";
	char cdev_path[128];
	struct ibv_context *ctx;
	struct ibv_device **list;
	struct ibv_device *dev = NULL;
	struct rc_qp src = {};
	struct rxe_restore_qp_req_local snap = {};
	struct rxe_restore_qp_req_local re = {};
	struct ib_uverbs_qp_cap_local cap = {};
	struct ibv_qp_attr qattr = {};
	struct ibv_qp_init_attr qiattr = {};
	struct ib_uverbs_get_context_resp gctx = {};
	uint32_t src_qpn, resp_qpn = 0, resp_cqe = 0;
	struct rxe_mminfo_local cq_mi = {};
	struct rxe_create_qp_resp_local qp_resp = {};
	uint32_t list_handles[32] = {};
	uint32_t total = 0;
	int n, i, fd_restore = -1, ret, fails = 0;
	/*
	 * B1 in-flight image buffers (SQ/RQ/RES). Capacity is bounded by the
	 * uverbs ioctl attr length field (struct ib_uverbs_attr::len is u16),
	 * so a single image attr cannot exceed 65535 bytes. That comfortably
	 * holds these small-cap rings; large rings would need chunking, a v1
	 * concern.
	 */
	const uint32_t img_cap = 65535u;
	struct ibv_mr *rq_mr = NULL;
	static char rq_recv_buf[4096];
	uint8_t *snap_sq = NULL, *snap_rq = NULL, *snap_res = NULL;
	uint8_t *re_sq = NULL, *re_rq = NULL, *re_res = NULL;
	struct qp_images snap_imgs = {}, re_imgs = {};

	if (resolve_cdev_path(ibdev, cdev_path, sizeof(cdev_path)) != 0)
		return 2;

	/* --- SOURCE: build a live RC QP via libibverbs --------------- */
	list = ibv_get_device_list(&n);
	for (i = 0; i < n; i++)
		if (!strcmp(ibv_get_device_name(list[i]), ibdev))
			dev = list[i];
	if (!dev) {
		fprintf(stderr, "qpr: ibdev '%s' vanished\n", ibdev);
		ibv_free_device_list(list);
		return 2;
	}
	ctx = ibv_open_device(dev);
	ibv_free_device_list(list);
	if (!ctx) {
		fprintf(stderr, "qpr: ibv_open_device: %s\n", strerror(errno));
		return 2;
	}
	printf("qp_restore_probe_rxe: ibdev=%s cdev=%s\n", ibdev, cdev_path);

	if (build_rts_loopback(ctx, &src) != 0) {
		fprintf(stderr, "qpr: source QP bring-up failed\n");
		fails++;
		goto out_src;
	}
	src_qpn = src.qp->qp_num;
	printf("  source: RC QP 0x%x at RTS (self-loopback)\n", src_qpn);

	/*
	 * B1 setup: pre-post recv WQEs so the RQ ring is genuinely
	 * non-empty (rq_producer advances, slot data populated). This is
	 * the in-flight state a frozen-not-drained snapshot must carry; the
	 * SQ stays empty (no peer to ack a send in self-loopback) but its
	 * slot region still round-trips. The buffers are oversized; the
	 * kernel reports the true image lengths in the blob.
	 */
	snap_sq = malloc(img_cap); snap_rq = malloc(img_cap);
	snap_res = malloc(img_cap);
	re_sq = malloc(img_cap); re_rq = malloc(img_cap); re_res = malloc(img_cap);
	if (!snap_sq || !snap_rq || !snap_res || !re_sq || !re_rq || !re_res) {
		fprintf(stderr, "qpr: image buffer alloc failed\n");
		fails++;
		goto out_src;
	}
	snap_imgs.sq = snap_sq; snap_imgs.rq = snap_rq;
	snap_imgs.res = snap_res; snap_imgs.cap = img_cap;
	re_imgs.sq = re_sq; re_imgs.rq = re_rq;
	re_imgs.res = re_res; re_imgs.cap = img_cap;

	rq_mr = ibv_reg_mr(src.pd, rq_recv_buf, sizeof(rq_recv_buf),
			   IBV_ACCESS_LOCAL_WRITE);
	if (!rq_mr) {
		fprintf(stderr, "qpr: ibv_reg_mr(rq): %s\n", strerror(errno));
		fails++;
		goto out_src;
	}
	for (i = 0; i < 3; i++) {
		struct ibv_sge sge = {
			.addr	= (uintptr_t)rq_recv_buf,
			.length	= 64,
			.lkey	= rq_mr->lkey,
		};
		struct ibv_recv_wr wr = {
			.wr_id		= 0x1100u + i,
			.sg_list	= &sge,
			.num_sge	= 1,
		};
		struct ibv_recv_wr *bad = NULL;

		if (ibv_post_recv(src.qp, &wr, &bad)) {
			fprintf(stderr, "qpr: ibv_post_recv[%d]: %s\n", i,
				strerror(errno));
			fails++;
			goto out_src;
		}
	}
	printf("  source: pre-posted 3 recv WQEs (RQ non-empty for B1)\n");

	/* Capture the create-time cap for the RESTORE_QP CAP attr. */
	if (ibv_query_qp(src.qp, &qattr, IBV_QP_CAP, &qiattr)) {
		fprintf(stderr, "qpr: ibv_query_qp(CAP): %s\n",
			strerror(errno));
		fails++;
		goto out_src;
	}
	cap.max_send_wr		= qiattr.cap.max_send_wr;
	cap.max_recv_wr		= qiattr.cap.max_recv_wr;
	cap.max_send_sge	= qiattr.cap.max_send_sge;
	cap.max_recv_sge	= qiattr.cap.max_recv_sge;
	cap.max_inline_data	= qiattr.cap.max_inline_data;

	/* [1] snapshot the source wire state (the "dump"). */
	printf("[1] QUERY_QP snapshot of source QP 0x%x (+ ring images)\n",
	       src_qpn);
	ret = do_migrate_query_qp(ctx->cmd_fd, src.qp->handle, &snap, &snap_imgs);
	if (ret) {
		fprintf(stderr, "  FAIL QUERY_QP(source): %s%s\n",
			strerror(-ret),
			ret == -EOPNOTSUPP
			? " (rxe_migrate_defs not wired into driver_def?)" : "");
		fails++;
		goto out_src;
	}
	if (snap.qpn != src_qpn) {
		fprintf(stderr, "  FAIL snapshot qpn=0x%x != src 0x%x\n",
			snap.qpn, src_qpn);
		fails++;
		goto out_src;
	}
	printf("  PASS snapshot captured (qpn=0x%x mtu=%u sq_psn=0x%x "
	       "rq_psn=0x%x)\n", snap.qpn, snap.path_mtu, snap.sq_psn,
	       snap.rq_psn);
	printf("      images: sq=%uB rq=%uB res=%uB; rq_prod=%u rq_cons=%u\n",
	       snap.sq_image_bytes, snap.rq_image_bytes, snap.res_image_bytes,
	       snap.rq_producer, snap.rq_consumer);
	if (snap.rq_image_bytes == 0 || snap.rq_producer == 0) {
		fprintf(stderr,
			"  FAIL expected non-empty RQ image after pre-post\n");
		fails++;
		goto out_src;
	}

	/* [2] freeze + destroy the source to free the qpn. */
	printf("[2] FREEZE_DATAPATH(source) + DESTROY_QP to free qpn 0x%x\n",
	       src_qpn);
	ret = do_migrate_freeze(ctx->cmd_fd, src.qp->handle, 1);
	if (ret) {
		fprintf(stderr, "  FAIL FREEZE_DATAPATH(source): %s\n",
			strerror(-ret));
		fails++;
		goto out_src;
	}
	if (ibv_destroy_qp(src.qp)) {
		fprintf(stderr, "  FAIL ibv_destroy_qp(source): %s\n",
			strerror(errno));
		src.qp = NULL;
		fails++;
		goto out_src;
	}
	src.qp = NULL;
	printf("  PASS source frozen + destroyed; qpn 0x%x returned to pool\n",
	       src_qpn);

	/* --- RESTORE: raw uverbs restore-mode ucontext --------------- */
	fd_restore = open(cdev_path, O_RDWR | O_CLOEXEC);
	if (fd_restore < 0) {
		fprintf(stderr, "qpr: open(%s): %s\n", cdev_path,
			strerror(errno));
		fails++;
		goto out_src;
	}
	ret = do_get_context(fd_restore, RXE_ALLOC_UCTX_RESTORE_MODE, &gctx);
	if (ret) {
		fprintf(stderr, "qpr: GET_CONTEXT(restore mode): %s\n",
			strerror(-ret));
		fails++;
		goto out_restore;
	}

	/* [3] rebuild PD + CQ dependencies. */
	printf("[3] RESTORE_PD(0x%x) + RESTORE_CQ(0x%x)\n",
	       PD_TARGET_HANDLE, CQ_TARGET_HANDLE);
	ret = do_restore_pd(fd_restore, PD_TARGET_HANDLE);
	if (ret) {
		fprintf(stderr, "  FAIL RESTORE_PD: %s\n", strerror(-ret));
		fails++;
		goto out_restore;
	}
	ret = do_restore_cq(fd_restore, CQ_TARGET_HANDLE, CQE_REQUESTED,
			    USER_HANDLE_TAG, 0, &resp_cqe, &cq_mi);
	if (ret) {
		fprintf(stderr, "  FAIL RESTORE_CQ: %s\n", strerror(-ret));
		fails++;
		goto out_restore;
	}
	printf("  PASS RESTORE_PD + RESTORE_CQ (resp_cqe=%u)\n", resp_cqe);

	/* [4] restore the QP at the source qpn from the snapshot. */
	printf("[4] RESTORE_QP(0x%x) from snapshot, expect RESP_QPN=0x%x\n",
	       QP_TARGET_HANDLE, src_qpn);
	ret = do_restore_qp(fd_restore, QP_TARGET_HANDLE, PD_TARGET_HANDLE,
			    CQ_TARGET_HANDLE, CQ_TARGET_HANDLE,
			    IB_QPT_RC_LOCAL, IB_QPS_RTS_LOCAL,
			    USER_HANDLE_TAG, &cap, &snap, &snap_imgs,
			    &resp_qpn, &qp_resp);
	if (ret) {
		fprintf(stderr, "  FAIL RESTORE_QP: %s%s\n", strerror(-ret),
			ret == -EOPNOTSUPP
			? " (rxe_restore_qp not in rxe_dev_ops?)"
			: ret == -EPERM
			? " (rxe_ucontext_is_restore_mode false?)"
			: ret == -EBUSY
			? " (qpn slot still occupied -- source not freed?)"
			: "");
		fails++;
		goto out_restore;
	}
	if (resp_qpn != src_qpn) {
		fprintf(stderr,
			"  FAIL RESP_QPN=0x%x != source qpn 0x%x\n"
			"       (qpn identity contract violated: restored QP\n"
			"        must land at the captured wire-visible number)\n",
			resp_qpn, src_qpn);
		fails++;
		goto out_restore;
	}
	printf("  PASS RESTORE_QP -> RESP_QPN=0x%x (== source qpn)\n",
	       resp_qpn);

	/* [5] INFO_HANDLES(QP) must report the restored handle. */
	printf("[5] INFO_HANDLES(QP) must include 0x%x\n", QP_TARGET_HANDLE);
	ret = do_info_handles(fd_restore, UVERBS_OBJECT_QP, list_handles, 32,
			      &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(QP): %s\n",
			strerror(-ret));
		fails++;
		goto out_restore;
	}
	if (!handle_present(list_handles, total, QP_TARGET_HANDLE)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(QP): 0x%x absent (total=%u)\n",
			QP_TARGET_HANDLE, total);
		fails++;
		goto out_restore;
	}
	printf("  PASS INFO_HANDLES(QP) reports 0x%x among %u entries\n",
	       QP_TARGET_HANDLE, total);

	/* [6] re-query the restored QP; must be byte-equal to snapshot. */
	printf("[6] QUERY_QP(restored) must match the source snapshot\n");
	ret = do_migrate_query_qp(fd_restore, QP_TARGET_HANDLE, &re, &re_imgs);
	if (ret) {
		fprintf(stderr, "  FAIL QUERY_QP(restored): %s\n",
			strerror(-ret));
		fails++;
		goto out_restore;
	}
	ret = blob_compare(&snap, &re);
	if (ret) {
		fprintf(stderr,
			"  FAIL restored wire state diverged in %d field(s)\n",
			ret);
		fails += ret;
	} else {
		printf("  PASS restored QP wire state byte-identical to source\n");
	}

	/*
	 * B1: the ring slot images and responder resources must round-trip
	 * byte-for-byte through capture -> UHW_IN tail -> blit -> re-query.
	 * The pre-posted RQ WQEs make this a real (non-empty) comparison.
	 */
	if (re.sq_image_bytes == snap.sq_image_bytes &&
	    memcmp(snap_sq, re_sq, snap.sq_image_bytes) != 0) {
		fprintf(stderr, "  FAIL SQ ring image differs after restore\n");
		fails++;
	}
	if (snap.rq_image_bytes && re.rq_image_bytes == snap.rq_image_bytes &&
	    memcmp(snap_rq, re_rq, snap.rq_image_bytes) != 0) {
		fprintf(stderr, "  FAIL RQ ring image differs after restore\n");
		fails++;
	}
	if (snap.res_image_bytes && re.res_image_bytes == snap.res_image_bytes &&
	    memcmp(snap_res, re_res, snap.res_image_bytes) != 0) {
		fprintf(stderr,
			"  FAIL responder-resources image differs after restore\n");
		fails++;
	}
	if (!fails)
		printf("  PASS SQ/RQ/RES images round-tripped byte-identical\n");

	/*
	 * [6b] Born-frozen thaw. RESTORE_QP installs the QP datapath-paused;
	 * the production flow resumes the whole ucontext with one
	 * FREEZE_CONTEXT(freeze=0) once the restore tree is consistent. Drive
	 * that here to confirm the born-frozen QP thaws cleanly via
	 * rxe_qp_resume. The byte-equality checks above ran on the as-restored
	 * (frozen) state, matching the frozen source snapshot. The SQ is empty
	 * and no peer traffic exists in this single-process probe, so neither
	 * resume kick (send_task replay / recv_task drain) fires -- the QP
	 * state is unperturbed.
	 */
	printf("[6b] FREEZE_CONTEXT(freeze=0) thaws the born-frozen restored QP\n");
	ret = do_migrate_freeze_context(fd_restore, 0);
	if (ret) {
		fprintf(stderr, "  FAIL FREEZE_CONTEXT(thaw): %s\n",
			strerror(-ret));
		fails++;
	} else {
		printf("  PASS born-frozen restored QP thawed\n");
	}

	/*
	 * [7] Actually mmap the three forced-offset rings (CQ + SQ + RQ)
	 * on the single restore-mode cdev fd. This is the load-bearing
	 * multi-mmap-per-ufile check: a realistic restored QP needs >= 3
	 * mappings to coexist on one fd, each pinned at the *source*
	 * vm_pgoff via rxe_create_mmap_info(forced_offset=...). rxe_mmap
	 * matches a pending entry by (context, byte-offset) and removes
	 * it; distinct source offsets must therefore all resolve. A
	 * regression in the forced-offset claim (e.g. a too-broad
	 * collision that drops a pending entry, or an offset the kernel
	 * never published) surfaces here as the second/third mmap()
	 * failing -EINVAL ("unable to find pending mmap info").
	 *
	 * The mminfo.offset values are byte offsets (vm_pgoff <<
	 * PAGE_SHIFT) straight from each restore method's UHW_OUT, so
	 * they feed mmap()'s offset argument directly.
	 */
	printf("[7] mmap CQ+SQ+RQ rings (3 forced offsets) on one cdev fd\n");
	{
		const struct {
			const char		*name;
			struct rxe_mminfo_local	mi;
		} rings[] = {
			{ "CQ", cq_mi },
			{ "SQ", qp_resp.sq_mi },
			{ "RQ", qp_resp.rq_mi },
		};
		void *maps[3] = { MAP_FAILED, MAP_FAILED, MAP_FAILED };
		unsigned int k;

		for (k = 0; k < 3; k++) {
			if (rings[k].mi.size == 0) {
				fprintf(stderr,
					"  FAIL %s ring: kernel published size=0"
					" (UHW_OUT mminfo not filled)\n",
					rings[k].name);
				fails++;
				continue;
			}
			maps[k] = mmap(NULL, rings[k].mi.size,
				       PROT_READ | PROT_WRITE, MAP_SHARED,
				       fd_restore,
				       (off_t)rings[k].mi.offset);
			if (maps[k] == MAP_FAILED) {
				fprintf(stderr,
					"  FAIL mmap %s ring (off=0x%llx"
					" size=%u): %s\n",
					rings[k].name,
					(unsigned long long)rings[k].mi.offset,
					rings[k].mi.size, strerror(errno));
				fails++;
			} else {
				/* touch first word to fault the page in */
				*(volatile uint32_t *)maps[k];
				printf("  PASS mmap %s ring off=0x%llx size=%u\n",
				       rings[k].name,
				       (unsigned long long)rings[k].mi.offset,
				       rings[k].mi.size);
			}
		}
		for (k = 0; k < 3; k++)
			if (maps[k] != MAP_FAILED)
				munmap(maps[k], rings[k].mi.size);
	}

	/* [8] tear the restored QP down cleanly. */
	printf("[8] DESTROY_QP(0x%x)\n", QP_TARGET_HANDLE);
	ret = do_destroy_qp(fd_restore, QP_TARGET_HANDLE);
	if (ret) {
		fprintf(stderr, "  FAIL DESTROY_QP(restored): %s\n",
			strerror(-ret));
		fails++;
	} else {
		printf("  PASS DESTROY_QP(0x%x)\n", QP_TARGET_HANDLE);
	}

out_restore:
	if (fd_restore >= 0)
		close(fd_restore);
out_src:
	if (rq_mr)
		ibv_dereg_mr(rq_mr);
	if (src.qp)
		ibv_destroy_qp(src.qp);
	if (src.cq)
		ibv_destroy_cq(src.cq);
	if (src.pd)
		ibv_dealloc_pd(src.pd);
	ibv_close_device(ctx);
	free(snap_sq); free(snap_rq); free(snap_res);
	free(re_sq); free(re_rq); free(re_res);

	if (fails) {
		fprintf(stderr,
			"\nqp_restore_probe_rxe: FAIL (%d failure(s))\n", fails);
		return 1;
	}
	printf("\nqp_restore_probe_rxe: PASS\n");
	return 0;
}
