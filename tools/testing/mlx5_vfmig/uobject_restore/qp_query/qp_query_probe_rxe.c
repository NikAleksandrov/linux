// SPDX-License-Identifier: GPL-2.0
/*
 * qp_query_probe_rxe -- empirical validation of S6a A4:
 * RXE_IB_METHOD_VFMIG_QUERY_QP (design/uobject_restore.md §5.3.7 +
 * §9.1 S6a). Single-process; no restore-mode ucontext needed -- this
 * probe only exercises the dump-side QUERY_QP verb and the
 * FREEZE_DATAPATH lifecycle against a live RC QP.
 *
 * Flow:
 *   1. libibverbs: open rxe0, alloc PD + CQ, create an RC QP, drive it
 *      RESET->INIT->RTR->RTS as a self-loopback (dest_qp_num = own qpn,
 *      AV pointing at our own GID). Post a recv + a signaled send so the
 *      requester/responder PSN cursors advance off their RTR/RTS bases.
 *   2. raw ioctl QUERY_QP on ctx->cmd_fd for qp->handle. Validate the
 *      returned struct rxe_restore_qp_req against the values we set and
 *      against ibv_query_qp's view:
 *        - blob.qpn        == qp->qp_num
 *        - blob.path_mtu   == attr.path_mtu
 *        - blob.dest_qp_num== attr.dest_qp_num
 *        - blob.rq_psn     == attr.rq_psn (modify-time base)
 *        - blob.sq_psn     == attr.sq_psn (modify-time base)
 *        - blob.{req,resp}_psn are sane (>= their bases, wrapped to 24b)
 *        - blob.sq_vm_pgoff / rq_vm_pgoff are non-zero (user rings)
 *   3. FREEZE_DATAPATH(freeze=1) then FREEZE_DATAPATH(freeze=0): both
 *      return 0 on the live user QP. (Negative gates: kernel QP -> ENXIO
 *      is exercised indirectly; bad handle -> ENOENT below.)
 *   4. QUERY_QP on a bogus handle -> -ENOENT.
 *
 * Build:
 *   make -C tools/testing/mlx5_vfmig \
 *        uobject_restore/qp_query/qp_query_probe_rxe
 * Usage:
 *   ./qp_query_probe_rxe [<ibdev>]      # default rxe0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <netinet/in.h>

#include <infiniband/verbs.h>

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

/* Mirror of include/uapi/rdma/rxe_user_ioctl_cmds.h. */
#define RXE_IB_OBJECT_VFMIG			(UVERBS_ID_DRIVER_NS + 0u)
#define RXE_IB_METHOD_VFMIG_FREEZE_DATAPATH	(1u << UVERBS_ID_NS_SHIFT)
#define RXE_IB_METHOD_VFMIG_QUERY_QP		((1u << UVERBS_ID_NS_SHIFT) + 1u)

#define RXE_IB_ATTR_VFMIG_FREEZE_DATAPATH_QP_HANDLE (1u << UVERBS_ID_NS_SHIFT)
#define RXE_IB_ATTR_VFMIG_FREEZE_DATAPATH_FREEZE    ((1u << UVERBS_ID_NS_SHIFT) + 1u)

#define RXE_IB_ATTR_VFMIG_QUERY_QP_HANDLE	(1u << UVERBS_ID_NS_SHIFT)
#define RXE_IB_ATTR_VFMIG_QUERY_QP_RESP_BLOB	((1u << UVERBS_ID_NS_SHIFT) + 1u)

/*
 * Mirror of include/uapi/rdma/rdma_user_rxe.h struct rxe_av and
 * struct rxe_restore_qp_req. Re-declared locally (rather than including
 * the in-tree kernel uapi header) to avoid clashing with the system
 * <netinet/in.h> that <infiniband/verbs.h> pulls in -- same convention
 * as cq_restore_probe_rxe.c. Layout MUST stay byte-identical to the
 * kernel struct.
 */
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
	uint8_t			reserved;
	uint16_t		reserved1;
	uint64_t		reserved2;
};

#define BTH_PSN_MASK	0x00ffffffu

/* ----------------------- ioctl helpers ----------------------------------- */

static int do_vfmig_query_qp(int fd, uint32_t qp_handle,
			     struct rxe_restore_qp_req_local *blob_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[2];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= RXE_IB_OBJECT_VFMIG;
	cmd.hdr.method_id	= RXE_IB_METHOD_VFMIG_QUERY_QP;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;

	cmd.attrs[n].attr_id	= RXE_IB_ATTR_VFMIG_QUERY_QP_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= qp_handle;
	n++;

	cmd.attrs[n].attr_id	= RXE_IB_ATTR_VFMIG_QUERY_QP_RESP_BLOB;
	cmd.attrs[n].len	= sizeof(*blob_out);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)blob_out;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

static int do_vfmig_freeze(int fd, uint32_t qp_handle, uint8_t freeze)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[2];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= RXE_IB_OBJECT_VFMIG;
	cmd.hdr.method_id	= RXE_IB_METHOD_VFMIG_FREEZE_DATAPATH;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;

	cmd.attrs[n].attr_id	= RXE_IB_ATTR_VFMIG_FREEZE_DATAPATH_QP_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= qp_handle;
	n++;

	cmd.attrs[n].attr_id	= RXE_IB_ATTR_VFMIG_FREEZE_DATAPATH_FREEZE;
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

/* ----------------------- RC self-loopback setup -------------------------- */

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
		/* skip all-zero (empty slot) */
		if (!g.global.subnet_prefix && !g.global.interface_id)
			continue;
		/* skip link-local fe80::/10 */
		if (g.raw[0] == 0xfe && (g.raw[1] & 0xc0) == 0x80)
			continue;
		*gid_out = g;
		return i;
	}
	/* nothing better; fall back to index 0 */
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
		fprintf(stderr, "qpq: ibv_query_port: %s\n", strerror(errno));
		return -1;
	}
	sgid_index = pick_gid(ctx, 1, &gid);
	if (sgid_index < 0) {
		fprintf(stderr, "qpq: no usable GID on port 1\n");
		return -1;
	}

	out->pd = ibv_alloc_pd(ctx);
	if (!out->pd) {
		fprintf(stderr, "qpq: ibv_alloc_pd: %s\n", strerror(errno));
		return -1;
	}
	out->cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!out->cq) {
		fprintf(stderr, "qpq: ibv_create_cq: %s\n", strerror(errno));
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
		fprintf(stderr, "qpq: ibv_create_qp: %s\n", strerror(errno));
		return -1;
	}

	/* RESET -> INIT */
	attr.qp_state	= IBV_QPS_INIT;
	attr.pkey_index	= 0;
	attr.port_num	= 1;
	attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
			       IBV_ACCESS_REMOTE_WRITE |
			       IBV_ACCESS_REMOTE_READ;
	flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
		IBV_QP_ACCESS_FLAGS;
	if (ibv_modify_qp(out->qp, &attr, flags)) {
		fprintf(stderr, "qpq: modify INIT: %s\n", strerror(errno));
		return -1;
	}

	/* INIT -> RTR (self-loopback: dest_qp_num = own qpn) */
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
		fprintf(stderr, "qpq: modify RTR: %s\n", strerror(errno));
		return -1;
	}

	/* RTR -> RTS */
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
		fprintf(stderr, "qpq: modify RTS: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

/* ----------------------- subtests ---------------------------------------- */

static int subtest_query_fields(struct ibv_context *ctx, struct rc_qp *p)
{
	struct rxe_restore_qp_req_local blob = {};
	struct ibv_qp_attr attr = {};
	struct ibv_qp_init_attr iattr = {};
	int ret, fails = 0;

	printf("[1] QUERY_QP field fidelity vs ibv_query_qp\n");

	if (ibv_query_qp(p->qp, &attr,
			 IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
			 IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_SQ_PSN,
			 &iattr)) {
		fprintf(stderr, "  FAIL ibv_query_qp: %s\n", strerror(errno));
		return 1;
	}

	ret = do_vfmig_query_qp(ctx->cmd_fd, p->qp->handle, &blob);
	if (ret) {
		fprintf(stderr, "  FAIL QUERY_QP ioctl: %s%s\n", strerror(-ret),
			ret == -EOPNOTSUPP
			? " (rxe_vfmig_defs not wired into driver_def?)"
			: "");
		return 1;
	}

#define CHECK(cond, fmt, ...)						\
	do {								\
		if (cond) {						\
			printf("  PASS " fmt "\n", ##__VA_ARGS__);	\
		} else {						\
			fprintf(stderr, "  FAIL " fmt "\n", ##__VA_ARGS__); \
			fails++;					\
		}							\
	} while (0)

	CHECK(blob.qpn == p->qp->qp_num, "qpn=0x%x (ibv qp_num=0x%x)",
	      blob.qpn, p->qp->qp_num);
	CHECK(blob.path_mtu == attr.path_mtu, "path_mtu=%u (ibv=%u)",
	      blob.path_mtu, attr.path_mtu);
	CHECK(blob.dest_qp_num == attr.dest_qp_num,
	      "dest_qp_num=0x%x (ibv=0x%x)", blob.dest_qp_num,
	      attr.dest_qp_num);
	CHECK(blob.rq_psn == (attr.rq_psn & BTH_PSN_MASK),
	      "rq_psn=0x%x (ibv base=0x%x)", blob.rq_psn,
	      attr.rq_psn & BTH_PSN_MASK);
	CHECK(blob.sq_psn == (attr.sq_psn & BTH_PSN_MASK),
	      "sq_psn=0x%x (ibv base=0x%x)", blob.sq_psn,
	      attr.sq_psn & BTH_PSN_MASK);
	CHECK(blob.sq_vm_pgoff != 0, "sq_vm_pgoff=0x%llx (non-zero user ring)",
	      (unsigned long long)blob.sq_vm_pgoff);
	CHECK(blob.rq_vm_pgoff != 0, "rq_vm_pgoff=0x%llx (non-zero user ring)",
	      (unsigned long long)blob.rq_vm_pgoff);
	CHECK(blob.sq_sig_all == 1, "sq_sig_all=%u (created with sq_sig_all=1)",
	      blob.sq_sig_all);
	CHECK(blob.port_num == 1, "port_num=%u", blob.port_num);
	/* live cursors must be within 24-bit PSN width */
	CHECK((blob.req_psn & ~BTH_PSN_MASK) == 0 &&
	      (blob.resp_psn & ~BTH_PSN_MASK) == 0 &&
	      (blob.comp_psn & ~BTH_PSN_MASK) == 0,
	      "live cursors masked to 24b (req=0x%x comp=0x%x resp=0x%x)",
	      blob.req_psn, blob.comp_psn, blob.resp_psn);
#undef CHECK
	return fails;
}

static int subtest_freeze_lifecycle(struct ibv_context *ctx, struct rc_qp *p)
{
	int ret, fails = 0;

	printf("[2] FREEZE_DATAPATH freeze/resume lifecycle\n");

	ret = do_vfmig_freeze(ctx->cmd_fd, p->qp->handle, 1);
	if (ret == 0)
		printf("  PASS FREEZE_DATAPATH(freeze=1) -> 0\n");
	else {
		fprintf(stderr, "  FAIL FREEZE_DATAPATH(freeze=1) -> %s\n",
			strerror(-ret));
		fails++;
	}

	ret = do_vfmig_freeze(ctx->cmd_fd, p->qp->handle, 0);
	if (ret == 0)
		printf("  PASS FREEZE_DATAPATH(freeze=0) -> 0\n");
	else {
		fprintf(stderr, "  FAIL FREEZE_DATAPATH(freeze=0) -> %s\n",
			strerror(-ret));
		fails++;
	}
	return fails;
}

static int subtest_bad_handle(struct ibv_context *ctx)
{
	struct rxe_restore_qp_req_local blob = {};
	int ret;

	printf("[3] QUERY_QP(bogus handle) -> -ENOENT\n");
	ret = do_vfmig_query_qp(ctx->cmd_fd, 0xdeadbeefu, &blob);
	if (ret == -ENOENT) {
		printf("  PASS QUERY_QP(0xdeadbeef) -> -ENOENT\n");
		return 0;
	}
	fprintf(stderr, "  FAIL QUERY_QP(0xdeadbeef) -> %s (expected -ENOENT)\n",
		ret ? strerror(-ret) : "0 (success)");
	return 1;
}

int main(int argc, char **argv)
{
	const char *ibdev = argc > 1 ? argv[1] : "rxe0";
	struct ibv_device **list;
	struct ibv_device *dev = NULL;
	struct ibv_context *ctx;
	struct rc_qp p = {};
	int n, i, fails = 0;

	list = ibv_get_device_list(&n);
	if (!list || n == 0) {
		fprintf(stderr, "qpq: no rdma devices\n");
		return 2;
	}
	for (i = 0; i < n; i++)
		if (!strcmp(ibv_get_device_name(list[i]), ibdev))
			dev = list[i];
	if (!dev) {
		fprintf(stderr, "qpq: ibdev '%s' not found\n", ibdev);
		ibv_free_device_list(list);
		return 2;
	}

	ctx = ibv_open_device(dev);
	ibv_free_device_list(list);
	if (!ctx) {
		fprintf(stderr, "qpq: ibv_open_device(%s): %s\n", ibdev,
			strerror(errno));
		return 2;
	}
	printf("qp_query_probe_rxe: ibdev=%s cmd_fd=%d\n", ibdev, ctx->cmd_fd);

	if (build_rts_loopback(ctx, &p) != 0) {
		ibv_close_device(ctx);
		return 2;
	}
	printf("  setup: RC QP 0x%x at RTS (self-loopback)\n", p.qp->qp_num);

	fails += subtest_query_fields(ctx, &p);
	fails += subtest_freeze_lifecycle(ctx, &p);
	fails += subtest_bad_handle(ctx);

	if (p.qp)
		ibv_destroy_qp(p.qp);
	if (p.cq)
		ibv_destroy_cq(p.cq);
	if (p.pd)
		ibv_dealloc_pd(p.pd);
	ibv_close_device(ctx);

	if (fails) {
		fprintf(stderr, "\nqp_query_probe_rxe: FAIL (%d failure(s))\n",
			fails);
		return 1;
	}
	printf("\nqp_query_probe_rxe: PASS\n");
	return 0;
}
