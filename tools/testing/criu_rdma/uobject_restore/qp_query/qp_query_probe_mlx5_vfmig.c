// SPDX-License-Identifier: GPL-2.0
/*
 * qp_query_probe_mlx5_vfmig -- exercises the dump-side
 * MLX5_IB_METHOD_VFMIG_QUERY_QP verb (design/uobject_restore.md
 * section 5.3.4). Mirror of cq_query_probe_mlx5_vfmig: establishes
 * byte-equality between two views of the same live QP:
 *
 *   (A) The libmlx5 mlx5dv_init_obj(MLX5DV_OBJ_QP) view -- what
 *       fw_id_continuity_probe extract_qp_context already leans on,
 *       and what a CRIU dumper would like to use except CRIU runs
 *       in its own address space (so dvqp.{rq.buf,dbrec} would be
 *       CRIU's VAs rather than the dumpee's). The dvqp.qp_id is
 *       echoed via ibv_qp.qp_num. dvqp.{sq,rq}.{wqe_cnt,stride}
 *       are libmlx5-internal post-rounding values; dvqp.{rq.buf,
 *       dbrec} are libmlx5-internal userspace VAs.
 *
 *   (B) The kernel-side MLX5_IB_METHOD_VFMIG_QUERY_QP view -- the
 *       handler reads qp->trans_qp.base.{mqp.qpn, ubuffer.umem->
 *       address}, mlx5_ib_db_user_virt(&qp->db), the WQ-ring shape
 *       (sq/rq.wqe_cnt + rq.wqe_shift), and packs a 64-byte
 *       mlx5_ib_restore_qp_req blob plus two scalar outs (user_handle /
 *       create_flags). cap / qp_type / qp_state are no longer emitted
 *       by the verb -- CRIU sources those from the standard
 *       IB_USER_VERBS_CMD_QUERY_QP verb + NLDEV -- so this probe no
 *       longer cross-checks them.
 *
 * In a single-process probe (this binary), (A) and (B) describe the
 * same live QP in the same address space, so we can demand strict
 * equality on every field with a one-to-one mapping:
 *
 *     blob.qpn          == ibv_qp.qp_num
 *     blob.sq_wqe_count == dvqp.sq.wqe_cnt
 *     blob.rq_wqe_count == dvqp.rq.wqe_cnt
 *     blob.rq_wqe_shift == ilog2(dvqp.rq.stride)        (*)
 *     blob.buf_addr     == (uintptr_t)dvqp.rq.buf       (**)
 *     blob.db_addr      == ((uintptr_t)dvqp.dbrec & PAGE_MASK)  (***)
 *     blob.sq_buf_addr  == 0
 *     blob.uidx         == 0
 *     blob.bfreg_index  == MLX5_IB_INVALID_BFREG (0x80000000)
 *     blob.ece_options  == 0
 *     blob.reserved     == 0
 *     blob.reserved2    == 0
 *     resp_user_handle  == matches the source-side libibverbs handle
 *                          stamped by ib_uverbs_create_qp; we cannot
 *                          read it from libibverbs but multi-QP
 *                          disambiguation (subtest 3) checks it is
 *                          unique-per-QP and stable.
 *     resp_create_flags == 0 (no IB_QP_CREATE_* flags via
 *                          libibverbs default ibv_create_qp).
 *
 * (*)   For empty RQs (rq.wqe_cnt == 0), libmlx5 sets stride to 0
 *       too; the kernel tolerates rq_wqe_shift == 0 in that case
 *       (RESTORE_QP only validates the [4,16] range when
 *       rq_wqe_count > 0). We assert blob.rq_wqe_shift == 0
 *       consistently in that branch.
 *
 * (**)  fw_id_continuity_probe extract_qp_context's docstring locks
 *       in the "rq.buf is the shared WQ-buffer base" identity:
 *       libmlx5 lays out RQ at offset 0 within the shared WQ
 *       buffer; SQ then follows at (rq.wqe_cnt << rq_wqe_shift).
 *       So the kernel's base->ubuffer.umem->address (the value
 *       ib_umem_get(@ucmd.buf_addr) was called with at create
 *       time) byte-equals (uintptr_t)dvqp.rq.buf.
 *
 * (***) mlx5_ib_db_map_user stores @virt & PAGE_MASK on the kernel
 *       side; the byte offset within the page survives via FW
 *       qpc.dbr_addr. The QUERY emits the page-aligned form, which
 *       is what the restore-side placeholder lookup keys on --
 *       byte-equal round-trip into RESTORE_QP is preserved. Same
 *       page-mask invariant the CQ probe asserts.
 *
 * Once the byte-equal contract is established, a CRIU dumper that
 * cannot run mlx5dv_init_obj from its own address space (because
 * dvqp.{rq.buf, dbrec} would be CRIU's VAs, not the dumpee's) can
 * substitute the QUERY_QP ioctl on the dumpee's uverbs fd and get
 * the same bytes -- closing the cross-process gap that motivated
 * this verb.
 *
 * Subtests:
 *
 *   1. happy path. Single QP, IBTA RC, transitioned RESET -> INIT
 *      so qp->state is non-trivial and ibv_query_qp returns a
 *      well-defined value. Strict byte-equal compare across all
 *      blob/scalar fields above.
 *
 *   2. invalid handle. QUERY_QP on an unallocated handle (HANDLE
 *      == 0xdeadbeef) must return -ENOENT (the IDR lookup miss
 *      from uverbs_ioctl.c). Validates the dispatcher gate.
 *
 *   3. multi-QP. Two distinct QPs (different sizes / cqs). Each
 *      query returns its own correct fields -- catches any "global
 *      state" / "wrong qp" bug where the handler reads from the
 *      wrong mlx5_ib_qp. Asserts qpn / buf_addr / db_addr (modulo
 *      shared DBR page) / user_handle distinctness.
 *
 * The probe is libibverbs + libmlx5; we genuinely need mlx5dv to
 * have a (A)-view to compare against. fw_id_continuity_probe is
 * the closest cousin -- this probe is the focused validator that
 * fw_id_continuity_probe's mlx5dv-derived qp context is byte-equal
 * to what the kernel emits via the dump-side ioctl.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/qp_query/qp_query_probe_mlx5_vfmig
 *
 * Usage:
 *   ./qp_query_probe_mlx5_vfmig <ibdev>
 *
 * <ibdev>         ib_device name, e.g. mlx5_2.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

/* ---------------------------------------------------------------------- */
/*                       inlined kernel UAPI                              */
/* ---------------------------------------------------------------------- */

/*
 * Mirror of include/uapi/rdma/mlx5-abi.h struct mlx5_ib_restore_qp_req.
 * 64 bytes, byte-equal to what UVERBS_METHOD_RESTORE_QP consumes via
 * its UHW_IN tail. The QUERY_QP handler emits this struct verbatim;
 * a CRIU dumper would memcpy it into protobuf at dump and back into
 * RESTORE_QP's UHW_IN at restore with no field-level marshaling.
 */
struct mlx5_ib_restore_qp_req {
	uint64_t	buf_addr;
	uint64_t	db_addr;
	uint64_t	sq_buf_addr;
	uint32_t	qpn;
	uint32_t	sq_wqe_count;
	uint32_t	rq_wqe_count;
	uint32_t	rq_wqe_shift;
	uint32_t	flags;
	uint32_t	uidx;
	uint32_t	bfreg_index;
	uint32_t	ece_options;
	uint32_t	reserved;
	uint32_t	reserved2;
} __attribute__((aligned(8)));

/*
 * MLX5_IB_INVALID_BFREG -- sentinel the handler emits in
 * blob.bfreg_index. mirror of the kernel's #define.
 */
#define PROBE_MLX5_IB_INVALID_BFREG	(0x80000000U)

/*
 * Inlined uverbs ioctl shapes -- mirror of include/uapi/rdma/
 * ib_user_ioctl_cmds.h. Same approach as cq_query_probe_mlx5_vfmig.
 */
#define RDMA_IOCTL_MAGIC	0x1b
#define RDMA_VERBS_IOCTL	_IOWR(RDMA_IOCTL_MAGIC, 1, \
					      struct ib_uverbs_ioctl_hdr)

enum {
	UVERBS_ATTR_F_MANDATORY		= 1u << 0,
	UVERBS_ATTR_F_VALID_OUTPUT	= 1u << 1,
};

struct ib_uverbs_attr {
	uint16_t	attr_id;
	uint16_t	len;
	uint16_t	flags;
	uint16_t	attr_data_reserved;
	uint64_t	data;
};

struct ib_uverbs_ioctl_hdr {
	uint16_t	length;
	uint16_t	object_id;
	uint16_t	method_id;
	uint16_t	num_attrs;
	uint64_t	reserved1;
	uint32_t	driver_id;
	uint32_t	reserved2;
	struct ib_uverbs_attr attrs[];
};

#define RDMA_DRIVER_MLX5			1

/*
 * Encoding mirrors include/uapi/rdma/ib_user_ioctl_cmds.h:
 * see cq_query_probe_mlx5_vfmig.c for the full UVERBS_ID_NS_SHIFT
 * rationale; condensed here.
 */
#define UVERBS_ID_NS_SHIFT			12
#define UVERBS_ID_DRIVER_NS			(1u << UVERBS_ID_NS_SHIFT)

/* MLX5_IB_OBJECT_VFMIG = 10th driver-namespace object. */
#define MLX5_IB_OBJECT_VFMIG			(UVERBS_ID_DRIVER_NS + 10u)

/*
 * Mirror of enum mlx5_ib_vfmig_methods. QUERY_QP is the 6th method
 * (after QUERY_UCONTEXT, RESTORE_UCONTEXT, QUERY_DYN_UARS,
 * RESTORE_DYN_UARS, QUERY_CQ).
 */
#define MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT	(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_METHOD_VFMIG_QUERY_QP		(MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT + 5u)

/*
 * Mirror of enum mlx5_ib_vfmig_query_qp_attrs. HANDLE is the IDR-
 * resolved QP uobject; the three RESP_* are blob / u64 / u32 outs.
 * cap / qp_type / qp_state were dropped from the verb (CRIU sources
 * them from the standard query_qp + NLDEV), so the ids renumbered.
 */
#define MLX5_IB_ATTR_VFMIG_QUERY_QP_HANDLE		(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_BLOB		(MLX5_IB_ATTR_VFMIG_QUERY_QP_HANDLE + 1u)
#define MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_USER_HANDLE	(MLX5_IB_ATTR_VFMIG_QUERY_QP_HANDLE + 2u)
#define MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_CREATE_FLAGS	(MLX5_IB_ATTR_VFMIG_QUERY_QP_HANDLE + 3u)

/* ---------------------------------------------------------------------- */
/*                            helpers                                     */
/* ---------------------------------------------------------------------- */

static struct ibv_device *find_ibdev(const char *name)
{
	struct ibv_device **list;
	int n, i;
	struct ibv_device *match = NULL;

	list = ibv_get_device_list(&n);
	if (!list || n == 0) {
		fprintf(stderr, "qpq: ibv_get_device_list returned 0 devices\n");
		return NULL;
	}
	for (i = 0; i < n; i++) {
		if (!strcmp(ibv_get_device_name(list[i]), name)) {
			match = list[i];
			break;
		}
	}
	if (!match) {
		fprintf(stderr, "qpq: ibdev '%s' not found; available:", name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	ibv_free_device_list(list);
	return match;
}

/*
 * mlx5dv_init_obj(MLX5DV_OBJ_QP) gives us the "calling-process VA"
 * view: dvqp.{rq.buf, dbrec} are libmlx5's userspace VAs in our own
 * mm. We treat this as ground truth in the single-process probe; the
 * QUERY_QP ioctl below must produce byte-equal numbers (modulo the
 * page-mask on dbrec).
 */
static int extract_dv_qp(struct ibv_qp *qp, struct mlx5dv_qp *out)
{
	struct mlx5dv_obj obj = {
		.qp = { .in = qp, .out = out },
	};
	int err = mlx5dv_init_obj(&obj, MLX5DV_OBJ_QP);

	if (err) {
		fprintf(stderr, "qpq: mlx5dv_init_obj(QP) failed: %d\n", err);
		return -1;
	}
	return 0;
}

/*
 * Returns ilog2(x) for x > 0 and a power of two; returns 0 for x == 0
 * (matches the kernel's "no RQ" convention where rq_wqe_shift is 0).
 * Returns -1 for non-power-of-two arguments (caller treats as error).
 */
static int probe_ilog2(uint32_t x)
{
	int s;
	if (x == 0)
		return 0;
	if (x & (x - 1))
		return -1;
	for (s = 0; (1u << s) != x; s++)
		;
	return s;
}

/*
 * Issue MLX5_IB_METHOD_VFMIG_QUERY_QP on @fd (the libibverbs cmd_fd)
 * for the QP at ufile @qp_handle. Fills @blob_out / @user_handle_out /
 * @flags_out on success. Returns 0 or -errno.
 */
static int do_vfmig_query_qp(int fd, uint32_t qp_handle,
			     struct mlx5_ib_restore_qp_req *blob_out,
			     uint64_t *user_handle_out,
			     uint32_t *flags_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[4];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= MLX5_IB_OBJECT_VFMIG;
	cmd.hdr.method_id	= MLX5_IB_METHOD_VFMIG_QUERY_QP;
	cmd.hdr.driver_id	= RDMA_DRIVER_MLX5;

	/*
	 * IDR attr: len == 0, data is the uobject handle. Same shape
	 * as cq_query_probe_mlx5_vfmig's HANDLE attr.
	 */
	cmd.attrs[n].attr_id	= MLX5_IB_ATTR_VFMIG_QUERY_QP_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= qp_handle;
	n++;

	cmd.attrs[n].attr_id	= MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_BLOB;
	cmd.attrs[n].len	= sizeof(*blob_out);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)blob_out;
	n++;

	cmd.attrs[n].attr_id	= MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_USER_HANDLE;
	cmd.attrs[n].len	= sizeof(*user_handle_out);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)user_handle_out;
	n++;

	cmd.attrs[n].attr_id	= MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_CREATE_FLAGS;
	cmd.attrs[n].len	= sizeof(*flags_out);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)flags_out;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length    = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

/*
 * Build a minimal RC QP: PD + send_cq + recv_cq + ibv_create_qp
 * with the supplied cap. Caller owns {pd, send_cq, recv_cq, qp}
 * and is responsible for tear-down. Returns 0 or -1.
 */
struct probe_qp {
	struct ibv_pd	*pd;
	struct ibv_cq	*send_cq;
	struct ibv_cq	*recv_cq;
	struct ibv_qp	*qp;
};

static int probe_qp_alloc(struct ibv_context *ctx, enum ibv_qp_type qpt,
			  uint32_t max_send_wr, uint32_t max_recv_wr,
			  uint32_t max_send_sge, uint32_t max_recv_sge,
			  struct probe_qp *out)
{
	struct ibv_qp_init_attr init = {};

	out->pd = ibv_alloc_pd(ctx);
	if (!out->pd) {
		fprintf(stderr, "qpq: ibv_alloc_pd: %s\n", strerror(errno));
		return -1;
	}
	out->send_cq = ibv_create_cq(ctx, max_send_wr + 1, NULL, NULL, 0);
	if (!out->send_cq) {
		fprintf(stderr, "qpq: ibv_create_cq(send): %s\n", strerror(errno));
		goto err_pd;
	}
	out->recv_cq = ibv_create_cq(ctx, max_recv_wr + 1, NULL, NULL, 0);
	if (!out->recv_cq) {
		fprintf(stderr, "qpq: ibv_create_cq(recv): %s\n", strerror(errno));
		goto err_send_cq;
	}

	init.qp_type = qpt;
	init.send_cq = out->send_cq;
	init.recv_cq = out->recv_cq;
	init.cap.max_send_wr  = max_send_wr;
	init.cap.max_recv_wr  = max_recv_wr;
	init.cap.max_send_sge = max_send_sge;
	init.cap.max_recv_sge = max_recv_sge;
	init.cap.max_inline_data = 0;
	init.sq_sig_all = 0;

	out->qp = ibv_create_qp(out->pd, &init);
	if (!out->qp) {
		fprintf(stderr, "qpq: ibv_create_qp(qpt=%d): %s\n",
			qpt, strerror(errno));
		goto err_recv_cq;
	}
	return 0;

err_recv_cq:
	ibv_destroy_cq(out->recv_cq);
	out->recv_cq = NULL;
err_send_cq:
	ibv_destroy_cq(out->send_cq);
	out->send_cq = NULL;
err_pd:
	ibv_dealloc_pd(out->pd);
	out->pd = NULL;
	return -1;
}

static void probe_qp_free(struct probe_qp *p)
{
	if (p->qp) {
		ibv_destroy_qp(p->qp);
		p->qp = NULL;
	}
	if (p->recv_cq) {
		ibv_destroy_cq(p->recv_cq);
		p->recv_cq = NULL;
	}
	if (p->send_cq) {
		ibv_destroy_cq(p->send_cq);
		p->send_cq = NULL;
	}
	if (p->pd) {
		ibv_dealloc_pd(p->pd);
		p->pd = NULL;
	}
}

/*
 * Drive the QP through RESET -> INIT so qp->state is non-trivial
 * on the kernel side and ibv_query_qp returns IBV_QPS_INIT.
 * Returns 0 or -1.
 */
static int probe_qp_to_init(struct ibv_qp *qp, uint8_t port_num)
{
	struct ibv_qp_attr attr = {};
	int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX |
		   IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
	int err;

	attr.qp_state	     = IBV_QPS_INIT;
	attr.pkey_index	     = 0;
	attr.port_num	     = port_num;
	attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
			       IBV_ACCESS_REMOTE_READ |
			       IBV_ACCESS_REMOTE_WRITE;

	err = ibv_modify_qp(qp, &attr, mask);
	if (err) {
		fprintf(stderr, "qpq: ibv_modify_qp(INIT) failed: %s\n",
			strerror(err));
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------------- */
/*                               subtests                                 */
/* ---------------------------------------------------------------------- */

struct qp_obs {
	struct mlx5_ib_restore_qp_req	blob;
	uint64_t			user_handle;
	uint32_t			create_flags;
	struct mlx5dv_qp		dv;
	uint32_t			expected_qpn;
};

/*
 * Compare the (A) mlx5dv view and (B) QUERY_QP view of @qp_obs.
 * Returns 0 (PASS) or -1 (FAIL with diagnostic stderr trail).
 *
 * @label is a short subtest tag for the stderr lines.
 */
static int compare_views(const struct qp_obs *o, const char *label)
{
	const long page_mask = ~(sysconf(_SC_PAGESIZE) - 1);
	uint64_t dv_buf  = (uint64_t)(uintptr_t)o->dv.rq.buf;
	uint64_t dv_dbr  = (uint64_t)(uintptr_t)o->dv.dbrec;
	uint64_t dv_dbr_aligned = dv_dbr & (uint64_t)page_mask;
	int dv_rq_shift;
	int ok = 1;

	if (o->blob.qpn != o->expected_qpn) {
		fprintf(stderr,
			"qpq[%s]: blob.qpn=0x%x != expected qp_num=0x%x\n",
			label, o->blob.qpn, o->expected_qpn);
		ok = 0;
	}
	if (o->blob.sq_wqe_count != o->dv.sq.wqe_cnt) {
		fprintf(stderr,
			"qpq[%s]: blob.sq_wqe_count=%u != dvqp.sq.wqe_cnt=%u\n",
			label, o->blob.sq_wqe_count, o->dv.sq.wqe_cnt);
		ok = 0;
	}
	if (o->blob.rq_wqe_count != o->dv.rq.wqe_cnt) {
		fprintf(stderr,
			"qpq[%s]: blob.rq_wqe_count=%u != dvqp.rq.wqe_cnt=%u\n",
			label, o->blob.rq_wqe_count, o->dv.rq.wqe_cnt);
		ok = 0;
	}
	dv_rq_shift = probe_ilog2(o->dv.rq.stride);
	if (dv_rq_shift < 0) {
		fprintf(stderr,
			"qpq[%s]: dvqp.rq.stride=%u not a power of two; cannot ilog2\n",
			label, o->dv.rq.stride);
		ok = 0;
	} else if (o->blob.rq_wqe_shift != (uint32_t)dv_rq_shift) {
		fprintf(stderr,
			"qpq[%s]: blob.rq_wqe_shift=%u != ilog2(dvqp.rq.stride=%u)=%d\n",
			label, o->blob.rq_wqe_shift, o->dv.rq.stride, dv_rq_shift);
		ok = 0;
	}
	if (o->blob.buf_addr != dv_buf) {
		fprintf(stderr,
			"qpq[%s]: blob.buf_addr=0x%llx != (uintptr_t)dvqp.rq.buf=0x%llx\n",
			label, (unsigned long long)o->blob.buf_addr,
			(unsigned long long)dv_buf);
		ok = 0;
	}
	if (o->blob.db_addr != dv_dbr_aligned) {
		fprintf(stderr,
			"qpq[%s]: blob.db_addr=0x%llx != (dvqp.dbrec & PAGE_MASK)=0x%llx (dbrec=0x%llx)\n",
			label, (unsigned long long)o->blob.db_addr,
			(unsigned long long)dv_dbr_aligned,
			(unsigned long long)dv_dbr);
		ok = 0;
	}
	if (o->blob.sq_buf_addr != 0) {
		fprintf(stderr,
			"qpq[%s]: blob.sq_buf_addr=0x%llx (must be 0 for v0 RC/UC/UD)\n",
			label, (unsigned long long)o->blob.sq_buf_addr);
		ok = 0;
	}
	if (o->blob.uidx != 0) {
		fprintf(stderr,
			"qpq[%s]: blob.uidx=0x%x (handler must emit sentinel 0)\n",
			label, o->blob.uidx);
		ok = 0;
	}
	if (o->blob.bfreg_index != PROBE_MLX5_IB_INVALID_BFREG) {
		fprintf(stderr,
			"qpq[%s]: blob.bfreg_index=0x%x != MLX5_IB_INVALID_BFREG=0x%x\n",
			label, o->blob.bfreg_index, PROBE_MLX5_IB_INVALID_BFREG);
		ok = 0;
	}
	if (o->blob.ece_options != 0) {
		fprintf(stderr,
			"qpq[%s]: blob.ece_options=0x%x (handler must emit sentinel 0)\n",
			label, o->blob.ece_options);
		ok = 0;
	}
	if (o->blob.reserved != 0 || o->blob.reserved2 != 0) {
		fprintf(stderr,
			"qpq[%s]: blob.reserved=%u blob.reserved2=%u (must be 0)\n",
			label, o->blob.reserved, o->blob.reserved2);
		ok = 0;
	}

	if (o->create_flags != 0) {
		fprintf(stderr,
			"qpq[%s]: resp_create_flags=0x%x (expected 0; ibv_create_qp default sets none)\n",
			label, o->create_flags);
		ok = 0;
	}

	if (ok) {
		fprintf(stderr,
			"qpq[%s]: STRONG byte-equal: qpn=0x%x sq_wqe_count=%u "
			"rq_wqe_count=%u rq_wqe_shift=%u buf_addr=0x%llx "
			"db_addr=0x%llx user_handle=0x%llx create_flags=0x%x\n",
			label, o->blob.qpn, o->blob.sq_wqe_count,
			o->blob.rq_wqe_count, o->blob.rq_wqe_shift,
			(unsigned long long)o->blob.buf_addr,
			(unsigned long long)o->blob.db_addr,
			(unsigned long long)o->user_handle,
			o->create_flags);
	}
	return ok ? 0 : -1;
}

/*
 * Subtest 1 + 5 (single-QP happy path with INIT-state echo).
 */
static int subtest_happy(struct ibv_context *ctx, uint8_t port_num,
			 const char *label)
{
	struct probe_qp p = {};
	struct qp_obs obs = {};
	int rc = -1;

	if (probe_qp_alloc(ctx, IBV_QPT_RC, 16, 16, 1, 1, &p))
		return -1;

	if (probe_qp_to_init(p.qp, port_num))
		goto out;

	if (extract_dv_qp(p.qp, &obs.dv))
		goto out;

	obs.expected_qpn = p.qp->qp_num;

	if (do_vfmig_query_qp(ctx->cmd_fd, p.qp->handle,
			      &obs.blob, &obs.user_handle,
			      &obs.create_flags)) {
		fprintf(stderr, "qpq[%s]: VFMIG_QUERY_QP failed: %s\n",
			label, strerror(errno));
		goto out;
	}

	if (compare_views(&obs, label))
		goto out;

	rc = 0;
out:
	probe_qp_free(&p);
	return rc;
}

/* Subtest 2: invalid handle. */
static int subtest_invalid_handle(struct ibv_context *ctx)
{
	struct mlx5_ib_restore_qp_req blob;
	uint32_t flags = 0;
	uint64_t user_handle = 0;
	int err;

	err = do_vfmig_query_qp(ctx->cmd_fd, 0xdeadbeefu,
				&blob, &user_handle, &flags);
	if (err == 0) {
		fprintf(stderr, "qpq[invalid]: succeeded on bogus handle 0xdeadbeef (must -ENOENT)\n");
		return -1;
	}
	if (err != -ENOENT && err != -EINVAL) {
		fprintf(stderr, "qpq[invalid]: unexpected errno=%d (-%s); want -ENOENT or -EINVAL\n",
			-err, strerror(-err));
		return -1;
	}
	fprintf(stderr, "qpq[invalid]: PASS (errno=%d -%s)\n",
		-err, strerror(-err));
	return 0;
}

/*
 * Subtest 3: multi-QP. Allocate two QPs (different sizes), query both,
 * assert each matches its own (A)-view (no cross-talk) and that key
 * fields (qpn, buf_addr, user_handle) are distinct between them.
 */
static int subtest_multi_qp(struct ibv_context *ctx)
{
	struct probe_qp pa = {}, pb = {};
	struct qp_obs oa = {}, ob = {};
	int rc = -1;

	if (probe_qp_alloc(ctx, IBV_QPT_RC, 16, 16, 1, 1, &pa))
		return -1;
	if (probe_qp_alloc(ctx, IBV_QPT_RC, 64, 64, 1, 1, &pb))
		goto out_a;

	if (extract_dv_qp(pa.qp, &oa.dv) || extract_dv_qp(pb.qp, &ob.dv))
		goto out;

	oa.expected_qpn   = pa.qp->qp_num;
	ob.expected_qpn   = pb.qp->qp_num;

	if (do_vfmig_query_qp(ctx->cmd_fd, pa.qp->handle,
			      &oa.blob, &oa.user_handle, &oa.create_flags) ||
	    do_vfmig_query_qp(ctx->cmd_fd, pb.qp->handle,
			      &ob.blob, &ob.user_handle, &ob.create_flags)) {
		fprintf(stderr, "qpq[multi]: QUERY_QP failed: %s\n",
			strerror(errno));
		goto out;
	}

	if (compare_views(&oa, "multi.A") || compare_views(&ob, "multi.B"))
		goto out;

	if (oa.blob.qpn == ob.blob.qpn) {
		fprintf(stderr, "qpq[multi]: qpn collision (both 0x%x)\n",
			oa.blob.qpn);
		goto out;
	}
	if (oa.blob.buf_addr == ob.blob.buf_addr) {
		fprintf(stderr, "qpq[multi]: buf_addr collision (both 0x%llx)\n",
			(unsigned long long)oa.blob.buf_addr);
		goto out;
	}
	if (oa.user_handle == ob.user_handle) {
		fprintf(stderr,
			"qpq[multi]: user_handle collision (both 0x%llx) -- libibverbs handle stamping bug?\n",
			(unsigned long long)oa.user_handle);
		goto out;
	}

	rc = 0;
out:
	probe_qp_free(&pb);
out_a:
	probe_qp_free(&pa);
	return rc;
}

/* ---------------------------------------------------------------------- */
/*                                main                                    */
/* ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *ibdev_name;
	struct ibv_device *dev;
	struct ibv_context *ctx = NULL;
	struct ibv_device_attr dev_attr = {};
	uint8_t port_num = 1;
	int rc = 1;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <ibdev>\n", argv[0]);
		return 2;
	}
	ibdev_name = argv[1];

	dev = find_ibdev(ibdev_name);
	if (!dev)
		return 1;
	ctx = ibv_open_device(dev);
	if (!ctx) {
		fprintf(stderr, "qpq: ibv_open_device(%s) failed: %s\n",
			ibdev_name, strerror(errno));
		return 1;
	}
	if (ibv_query_device(ctx, &dev_attr)) {
		fprintf(stderr, "qpq: ibv_query_device failed\n");
		goto out;
	}

	fprintf(stderr,
		"qpq: ibdev=%s phys_port_cnt=%u num_comp_vectors=%d\n",
		ibdev_name, dev_attr.phys_port_cnt, ctx->num_comp_vectors);

	if (subtest_happy(ctx, port_num, "happy"))
		goto out;
	if (subtest_invalid_handle(ctx))
		goto out;
	if (subtest_multi_qp(ctx))
		goto out;

	fprintf(stderr, "qpq: ALL SUBTESTS PASS\n");
	rc = 0;
out:
	if (ctx)
		ibv_close_device(ctx);
	return rc;
}
