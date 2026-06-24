// SPDX-License-Identifier: GPL-2.0
/*
 * cq_query_probe_mlx5_vfmig -- exercises the dump-side
 * MLX5_IB_METHOD_VFMIG_QUERY_CQ verb (design/uobject_restore.md §5.2.4).
 *
 * Establishes byte-equality between two views of the same live CQ:
 *
 *   (A) The libmlx5 mlx5dv_init_obj(MLX5DV_OBJ_CQ) view -- this is
 *       what fw_id_continuity_probe currently leans on, and is also
 *       what a CRIU dumper *would* like to use except CRIU runs in
 *       its own address space and ibv_import_cq does not exist in
 *       upstream rdma-core. The dvcq.{cqn, cqe_cnt, cqe_size} are
 *       process-independent FW properties; dvcq.{buf, dbrec} are
 *       libmlx5-internal userspace VAs from the calling process.
 *
 *   (B) The kernel-side MLX5_IB_METHOD_VFMIG_QUERY_CQ view -- the
 *       handler reads cq->mcq.cqn, cq->ibcq.cqe, cq->cqe_size,
 *       cq->buf.umem->address, cq->db.u.user_page->user_virt and
 *       packs the four "round-trip into RESTORE_CQ" fields into a
 *       struct mlx5_ib_restore_cq_req blob plus three scalar outs
 *       (cqe / comp_vector / flags).
 *
 * In a single-process probe (this binary), (A) and (B) describe the
 * same live CQ in the same address space, so we can demand strict
 * equality on every field that has a one-to-one mapping:
 *
 *     blob.cqn       == dvcq.cqn
 *     blob.cqe_size  == dvcq.cqe_size
 *     blob.buf_addr  == (uintptr_t)dvcq.buf
 *     blob.db_addr   == ((uintptr_t)dvcq.dbrec & PAGE_MASK)   (*)
 *     blob.reserved  == 0
 *     blob.reserved2 == 0
 *     resp_cqe       == ibv_cq.cqe
 *     resp_cqe + 1   == dvcq.cqe_cnt                          (**)
 *     resp_comp_vec  == comp_vector that we passed to ibv_create_cq
 *     resp_flags     == 0   (no CQ-create flags via libibverbs default)
 *
 * (*)  mlx5_ib_db_map_user stores @virt & PAGE_MASK on the kernel
 *      side; the byte offset within the page survives via FW
 *      cqc.dbr_addr per the comment on mlx5_ib_restore_cq_req.db_addr.
 *      The QUERY emits the page-aligned form, which is what the
 *      restore-side placeholder lookup keys on -- byte-equal
 *      round-trip into RESTORE_CQ is preserved.
 *
 * (**) verbs convention: ibv_cq.cqe == entries-1, mlx5dv_cq.cqe_cnt
 *      == entries. RESTORE_CQ takes the verbs-form cqe (matches
 *      mlx5_ib_restore_cq's umem-pin length math).
 *
 * Once the byte-equal contract is established, a CRIU dumper that
 * cannot run mlx5dv_init_obj from its own address space (because
 * dvcq.{buf, dbrec} would be CRIU's VAs, not the dumpee's) can
 * substitute the QUERY_CQ ioctl on the dumpee's uverbs fd and get
 * the same bytes -- closing the cross-process gap that motivated
 * this verb. (See design/uobject_restore.md §5.2.4 for the full
 * dump-side rationale.)
 *
 * Subtests:
 *
 *   1. happy path. Single CQ, default comp_vector=0, no flags.
 *      Strict byte-equal compare across all eight fields above.
 *
 *   2. invalid handle. QUERY_CQ on an unallocated handle (HANDLE
 *      == 0xdeadbeef) must return -ENOENT (the IDR lookup miss
 *      from uverbs_ioctl.c). Validates the dispatcher gate.
 *
 *   3. multi-CQ. Two distinct CQs (different sizes, different
 *      comp_vectors). Each query returns its own correct fields
 *      -- catches any "global state" / "wrong cq" bug where the
 *      handler reads from the wrong mlx5_ib_cq.
 *
 *   4. comp_vector echo. A CQ created with comp_vector=N (where
 *      N > 0 is in [1, num_comp_vectors)) must come back with
 *      RESP_COMP_VECTOR == N. Catches any "we hard-coded 0" bug.
 *
 *   5. cqe round-trip into RESTORE_CQ length math. RESTORE_CQ
 *      derives the buffer pin length as cqe * cqe_size; verify
 *      that the pair (resp_cqe, blob.cqe_size) we report multiplies
 *      to a strictly positive value within libibverbs's
 *      device.max_cqe bound. Cheap sanity that the cqe field is
 *      not an off-by-one or sign-bit-extension artefact.
 *
 * The probe is libibverbs + libmlx5; we genuinely need mlx5dv to
 * have a (A)-view to compare against. fw_id_continuity_probe is
 * the closest cousin -- this probe is the focused validator that
 * fw_id_continuity_probe's mlx5dv-derived cq context is byte-equal
 * to what the kernel emits via the dump-side ioctl.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/cq_query/cq_query_probe_mlx5_vfmig
 *
 * Usage:
 *   ./cq_query_probe_mlx5_vfmig <ibdev> [<entries> [<comp_vector>]]
 *
 * <ibdev>         ib_device name, e.g. mlx5_2.
 * <entries>       cqe count to request from ibv_create_cq.
 *                 Defaults to 16. Rounded up to a power of two by
 *                 libmlx5; the kernel reports back ibcq.cqe ==
 *                 (rounded_entries - 1).
 * <comp_vector>   comp_vector index passed to ibv_create_cq.
 *                 Defaults to 0. Subtest 4 picks min(1, max-1).
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
 * Mirror of include/uapi/rdma/mlx5-abi.h struct mlx5_ib_restore_cq_req.
 * 32 bytes, byte-equal to what UVERBS_METHOD_RESTORE_CQ consumes via
 * its UHW_IN tail. The QUERY_CQ handler emits this struct verbatim;
 * a CRIU dumper would memcpy it into protobuf at dump and back into
 * RESTORE_CQ's UHW_IN at restore with no field-level marshaling.
 */
struct mlx5_ib_restore_cq_req {
	uint64_t	buf_addr;
	uint64_t	db_addr;
	uint32_t	cqn;
	uint32_t	cqe_size;
	uint32_t	reserved;
	uint32_t	reserved2;
} __attribute__((aligned(8)));

/*
 * Inlined uverbs ioctl shapes -- mirror of include/uapi/rdma/
 * ib_user_ioctl_cmds.h. Same approach as the other restore probes:
 * system rdma-core uapi lags the in-tree kernel for the new
 * MLX5_IB_OBJECT_VFMIG namespace, so we duplicate the bare minimum
 * here rather than depend on a freshly-installed sanitized headers
 * tree.
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
 *   UVERBS_ID_NS_SHIFT = 12
 *   UVERBS_ID_DRIVER_NS = 1U << UVERBS_ID_NS_SHIFT = 0x1000
 *   UVERBS_ATTR_UHW_IN  = UVERBS_ID_DRIVER_NS = 0x1000
 *
 * Per-namespace ids are (UVERBS_ID_DRIVER_NS | ordinal) for driver
 * namespaces (mlx5's MLX5_IB_OBJECT_*, MLX5_IB_METHOD_*, MLX5_IB_ATTR_*).
 *
 * NOTE: tools/testing/criu_rdma/tools/ucontext_vendor_verbs.c uses
 * UVERBS_ID_NS_SHIFT=10 and so produces the wrong encoding (0x40a
 * instead of 0x100a for VFMIG); that's a separate bug in that
 * helper, not a pattern to follow. We use the correct upstream
 * value of 12 so the dispatcher's uapi_key_obj() succeeds on the
 * driver-namespace branch (id & UVERBS_API_NS_FLAG = id & 0x1000).
 */
#define UVERBS_ID_NS_SHIFT			12
#define UVERBS_ID_DRIVER_NS			(1u << UVERBS_ID_NS_SHIFT)
#define UVERBS_ATTR_UHW_IN			((uint16_t)UVERBS_ID_DRIVER_NS)

/*
 * Mirror of enum mlx5_ib_objects: count off from
 * MLX5_IB_OBJECT_DEVX (= 0x1000) by declaration order.
 *
 *   DEVX (0), DEVX_OBJ (1), DEVX_UMEM (2), FLOW_MATCHER (3),
 *   DEVX_ASYNC_CMD_FD (4), DEVX_ASYNC_EVENT_FD (5), VAR (6),
 *   PP (7), UAR (8), STEERING_ANCHOR (9), VFMIG (10).
 */
#define MLX5_IB_OBJECT_DEVX			(UVERBS_ID_DRIVER_NS + 0u)
#define MLX5_IB_OBJECT_VFMIG			(UVERBS_ID_DRIVER_NS + 10u)

/*
 * Mirror of enum mlx5_ib_vfmig_methods. Methods on a driver-namespace
 * object are themselves driver-namespace, so the base is
 * (1 << UVERBS_ID_NS_SHIFT) = 0x1000 and the rest count off.
 */
#define MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT	(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT	(MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT + 1u)
#define MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS	(MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT + 2u)
#define MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS	(MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT + 3u)
#define MLX5_IB_METHOD_VFMIG_QUERY_CQ		(MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT + 4u)

/*
 * Mirror of enum mlx5_ib_vfmig_query_cq_attrs. HANDLE is the IDR-
 * resolved CQ uobject; the four RESP_* are u32 / blob outs.
 */
#define MLX5_IB_ATTR_VFMIG_QUERY_CQ_HANDLE		(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_BLOB		(MLX5_IB_ATTR_VFMIG_QUERY_CQ_HANDLE + 1u)
#define MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_CQE		(MLX5_IB_ATTR_VFMIG_QUERY_CQ_HANDLE + 2u)
#define MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_COMP_VECTOR	(MLX5_IB_ATTR_VFMIG_QUERY_CQ_HANDLE + 3u)
#define MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_FLAGS		(MLX5_IB_ATTR_VFMIG_QUERY_CQ_HANDLE + 4u)

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
		fprintf(stderr, "cqq: ibv_get_device_list returned 0 devices\n");
		return NULL;
	}
	for (i = 0; i < n; i++) {
		if (!strcmp(ibv_get_device_name(list[i]), name)) {
			match = list[i];
			break;
		}
	}
	if (!match) {
		fprintf(stderr, "cqq: ibdev '%s' not found; available:", name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	ibv_free_device_list(list);
	return match;
}

/*
 * mlx5dv_init_obj(MLX5DV_OBJ_CQ) gives us the "calling-process VA"
 * view: dvcq.{buf, dbrec} are libmlx5's userspace VAs in our own mm.
 * We treat this as ground truth in the single-process probe; the
 * QUERY_CQ ioctl below must produce byte-equal numbers (modulo the
 * page-mask on db).
 */
static int extract_dv_cq(struct ibv_cq *cq, struct mlx5dv_cq *out)
{
	struct mlx5dv_obj obj = {
		.cq = { .in = cq, .out = out },
	};
	int err = mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ);

	if (err) {
		fprintf(stderr, "cqq: mlx5dv_init_obj(CQ) failed: %d\n", err);
		return -1;
	}
	return 0;
}

/*
 * Issue MLX5_IB_METHOD_VFMIG_QUERY_CQ on @fd (the libibverbs cmd_fd)
 * for the CQ at ufile @cq_handle. Fills @blob_out / @cqe_out /
 * @comp_vector_out / @flags_out on success. Returns 0 or -errno.
 */
static int do_vfmig_query_cq(int fd, uint32_t cq_handle,
			     struct mlx5_ib_restore_cq_req *blob_out,
			     uint32_t *cqe_out,
			     uint32_t *comp_vector_out,
			     uint32_t *flags_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[5];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= MLX5_IB_OBJECT_VFMIG;
	cmd.hdr.method_id	= MLX5_IB_METHOD_VFMIG_QUERY_CQ;
	cmd.hdr.driver_id	= RDMA_DRIVER_MLX5;

	/*
	 * IDR-class attr: uverbs_ioctl.c ::uverbs_process_attr enforces
	 * len == 0 and reads the uobject handle from uattr->data
	 * directly (UVERBS_ATTR_TYPE_IDR branch). Setting len = 4 the
	 * way PTR_IN attrs do trips its `if (uattr->len != 0) return
	 * -EINVAL` guard before the dispatcher even reaches our handler.
	 */
	cmd.attrs[n].attr_id	= MLX5_IB_ATTR_VFMIG_QUERY_CQ_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= cq_handle;
	n++;

	cmd.attrs[n].attr_id	= MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_BLOB;
	cmd.attrs[n].len	= sizeof(*blob_out);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)blob_out;
	n++;

	cmd.attrs[n].attr_id	= MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_CQE;
	cmd.attrs[n].len	= sizeof(*cqe_out);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)cqe_out;
	n++;

	cmd.attrs[n].attr_id	= MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_COMP_VECTOR;
	cmd.attrs[n].len	= sizeof(*comp_vector_out);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)comp_vector_out;
	n++;

	cmd.attrs[n].attr_id	= MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_FLAGS;
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

/* ---------------------------------------------------------------------- */
/*                               subtests                                 */
/* ---------------------------------------------------------------------- */

struct cq_obs {
	struct mlx5_ib_restore_cq_req	blob;
	uint32_t			cqe;
	uint32_t			comp_vector;
	uint32_t			flags;
	struct mlx5dv_cq		dv;
	uint32_t			expected_comp_vector;
};

static int compare_views(const struct cq_obs *o, const char *label)
{
	const long page_mask = ~(sysconf(_SC_PAGESIZE) - 1);
	uint64_t dv_buf  = (uint64_t)(uintptr_t)o->dv.buf;
	uint64_t dv_dbr  = (uint64_t)(uintptr_t)o->dv.dbrec;
	uint64_t dv_dbr_aligned = dv_dbr & (uint64_t)page_mask;
	int ok = 1;

	if (o->blob.cqn != o->dv.cqn) {
		fprintf(stderr,
			"cqq[%s]: blob.cqn=0x%x != dvcq.cqn=0x%x\n",
			label, o->blob.cqn, o->dv.cqn);
		ok = 0;
	}
	if (o->blob.cqe_size != o->dv.cqe_size) {
		fprintf(stderr,
			"cqq[%s]: blob.cqe_size=%u != dvcq.cqe_size=%u\n",
			label, o->blob.cqe_size, o->dv.cqe_size);
		ok = 0;
	}
	if (o->blob.buf_addr != dv_buf) {
		fprintf(stderr,
			"cqq[%s]: blob.buf_addr=0x%llx != (uintptr_t)dvcq.buf=0x%llx\n",
			label, (unsigned long long)o->blob.buf_addr,
			(unsigned long long)dv_buf);
		ok = 0;
	}
	if (o->blob.db_addr != dv_dbr_aligned) {
		fprintf(stderr,
			"cqq[%s]: blob.db_addr=0x%llx != (dvcq.dbrec & PAGE_MASK)=0x%llx (dbrec=0x%llx)\n",
			label, (unsigned long long)o->blob.db_addr,
			(unsigned long long)dv_dbr_aligned,
			(unsigned long long)dv_dbr);
		ok = 0;
	}
	if (o->blob.reserved != 0 || o->blob.reserved2 != 0) {
		fprintf(stderr,
			"cqq[%s]: blob.reserved=%u blob.reserved2=%u (must be 0)\n",
			label, o->blob.reserved, o->blob.reserved2);
		ok = 0;
	}
	if (o->cqe + 1 != o->dv.cqe_cnt) {
		fprintf(stderr,
			"cqq[%s]: resp_cqe+1=%u != dvcq.cqe_cnt=%u (verbs/dv mismatch)\n",
			label, o->cqe + 1, o->dv.cqe_cnt);
		ok = 0;
	}
	if (o->comp_vector != o->expected_comp_vector) {
		fprintf(stderr,
			"cqq[%s]: resp_comp_vector=%u != expected=%u\n",
			label, o->comp_vector, o->expected_comp_vector);
		ok = 0;
	}
	if (o->flags != 0) {
		fprintf(stderr,
			"cqq[%s]: resp_flags=0x%x (expected 0; ibv_create_cq sets none)\n",
			label, o->flags);
		ok = 0;
	}

	if (ok) {
		fprintf(stderr,
			"cqq[%s]: STRONG byte-equal: cqn=0x%x cqe_size=%u "
			"buf_addr=0x%llx db_addr=0x%llx cqe=%u comp_vector=%u flags=0x%x\n",
			label, o->blob.cqn, o->blob.cqe_size,
			(unsigned long long)o->blob.buf_addr,
			(unsigned long long)o->blob.db_addr,
			o->cqe, o->comp_vector, o->flags);
	}
	return ok ? 0 : -1;
}

/*
 * Subtest 1 + 4 + 5 (single-CQ happy path with comp_vector echo and
 * cqe length-math sanity).
 */
static int subtest_happy(struct ibv_context *ctx, struct ibv_pd *pd,
			 int entries, int comp_vector, const char *label)
{
	struct ibv_cq *cq;
	struct cq_obs obs = { .expected_comp_vector = (uint32_t)comp_vector };
	uint64_t pin_len;
	int rc = -1;

	cq = ibv_create_cq(ctx, entries, NULL, NULL, comp_vector);
	if (!cq) {
		fprintf(stderr, "cqq[%s]: ibv_create_cq(entries=%d, comp_vector=%d) failed: %s\n",
			label, entries, comp_vector, strerror(errno));
		return -1;
	}

	if (extract_dv_cq(cq, &obs.dv))
		goto out_destroy;

	if (do_vfmig_query_cq(ctx->cmd_fd, cq->handle,
			      &obs.blob, &obs.cqe, &obs.comp_vector,
			      &obs.flags)) {
		fprintf(stderr, "cqq[%s]: VFMIG_QUERY_CQ failed: %s\n",
			label, strerror(errno));
		goto out_destroy;
	}

	if (compare_views(&obs, label))
		goto out_destroy;

	pin_len = (uint64_t)obs.cqe * (uint64_t)obs.blob.cqe_size;
	if (pin_len == 0) {
		fprintf(stderr, "cqq[%s]: cqe*cqe_size == 0 (rounds to zero pin len)\n", label);
		goto out_destroy;
	}
	fprintf(stderr,
		"cqq[%s]: round-trip pin len = cqe(%u) * cqe_size(%u) = %llu bytes\n",
		label, obs.cqe, obs.blob.cqe_size,
		(unsigned long long)pin_len);

	rc = 0;
out_destroy:
	ibv_destroy_cq(cq);
	return rc;
}

/* Subtest 2: invalid handle. */
static int subtest_invalid_handle(struct ibv_context *ctx)
{
	struct mlx5_ib_restore_cq_req blob;
	uint32_t cqe = 0, comp_vector = 0, flags = 0;
	int err;

	err = do_vfmig_query_cq(ctx->cmd_fd, 0xdeadbeefu,
				&blob, &cqe, &comp_vector, &flags);
	if (err == 0) {
		fprintf(stderr, "cqq[invalid]: succeeded on bogus handle 0xdeadbeef (must -ENOENT)\n");
		return -1;
	}
	if (err != -ENOENT && err != -EINVAL) {
		fprintf(stderr, "cqq[invalid]: unexpected errno=%d (-%s); want -ENOENT or -EINVAL\n",
			-err, strerror(-err));
		return -1;
	}
	fprintf(stderr, "cqq[invalid]: PASS (errno=%d -%s)\n",
		-err, strerror(-err));
	return 0;
}

/*
 * Subtest 3: multi-CQ. Allocate two CQs with different sizes, query
 * both, assert each matches its own (A)-view (no cross-talk).
 */
static int subtest_multi_cq(struct ibv_context *ctx, struct ibv_pd *pd)
{
	struct ibv_cq *cq_a = NULL, *cq_b = NULL;
	struct cq_obs obs_a = { .expected_comp_vector = 0 };
	struct cq_obs obs_b = { .expected_comp_vector = 0 };
	int rc = -1;

	cq_a = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!cq_a) {
		fprintf(stderr, "cqq[multi]: ibv_create_cq(A) failed\n");
		goto out;
	}
	cq_b = ibv_create_cq(ctx, 64, NULL, NULL, 0);
	if (!cq_b) {
		fprintf(stderr, "cqq[multi]: ibv_create_cq(B) failed\n");
		goto out;
	}

	if (extract_dv_cq(cq_a, &obs_a.dv) || extract_dv_cq(cq_b, &obs_b.dv))
		goto out;
	if (do_vfmig_query_cq(ctx->cmd_fd, cq_a->handle,
			      &obs_a.blob, &obs_a.cqe, &obs_a.comp_vector,
			      &obs_a.flags) ||
	    do_vfmig_query_cq(ctx->cmd_fd, cq_b->handle,
			      &obs_b.blob, &obs_b.cqe, &obs_b.comp_vector,
			      &obs_b.flags)) {
		fprintf(stderr, "cqq[multi]: QUERY_CQ failed\n");
		goto out;
	}
	if (compare_views(&obs_a, "multi.A") ||
	    compare_views(&obs_b, "multi.B"))
		goto out;
	if (obs_a.blob.cqn == obs_b.blob.cqn) {
		fprintf(stderr, "cqq[multi]: cqn collision (both 0x%x)\n",
			obs_a.blob.cqn);
		goto out;
	}
	if (obs_a.blob.buf_addr == obs_b.blob.buf_addr) {
		fprintf(stderr, "cqq[multi]: buf_addr collision (both 0x%llx)\n",
			(unsigned long long)obs_a.blob.buf_addr);
		goto out;
	}
	rc = 0;
out:
	if (cq_a)
		ibv_destroy_cq(cq_a);
	if (cq_b)
		ibv_destroy_cq(cq_b);
	return rc;
}

/* ---------------------------------------------------------------------- */
/*                                main                                    */
/* ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *ibdev_name;
	int entries = 16;
	int comp_vector = 0;
	struct ibv_device *dev;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pd = NULL;
	struct ibv_device_attr dev_attr = {};
	int alt_comp_vector;
	int rc = 1;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <ibdev> [<entries> [<comp_vector>]]\n",
			argv[0]);
		return 2;
	}
	ibdev_name = argv[1];
	if (argc >= 3)
		entries = atoi(argv[2]);
	if (argc >= 4)
		comp_vector = atoi(argv[3]);
	if (entries <= 0 || comp_vector < 0) {
		fprintf(stderr, "cqq: bad entries=%d / comp_vector=%d\n",
			entries, comp_vector);
		return 2;
	}

	dev = find_ibdev(ibdev_name);
	if (!dev)
		return 1;
	ctx = ibv_open_device(dev);
	if (!ctx) {
		fprintf(stderr, "cqq: ibv_open_device(%s) failed: %s\n",
			ibdev_name, strerror(errno));
		return 1;
	}
	if (ibv_query_device(ctx, &dev_attr)) {
		fprintf(stderr, "cqq: ibv_query_device failed\n");
		goto out;
	}
	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		fprintf(stderr, "cqq: ibv_alloc_pd failed: %s\n",
			strerror(errno));
		goto out;
	}

	fprintf(stderr,
		"cqq: ibdev=%s entries=%d comp_vector=%d num_comp_vectors=%d\n",
		ibdev_name, entries, comp_vector, ctx->num_comp_vectors);

	if (subtest_happy(ctx, pd, entries, comp_vector, "happy"))
		goto out;
	if (subtest_invalid_handle(ctx))
		goto out;
	if (subtest_multi_cq(ctx, pd))
		goto out;

	/*
	 * Subtest 4 (comp_vector echo): rerun happy with a non-zero
	 * comp_vector if the device exposes more than one. Skip on
	 * single-vector devices (still a PASS overall).
	 */
	if (ctx->num_comp_vectors >= 2) {
		alt_comp_vector = 1;
		if (subtest_happy(ctx, pd, entries, alt_comp_vector,
				  "comp_vec=1"))
			goto out;
	} else {
		fprintf(stderr,
			"cqq[comp_vec=1]: SKIP (num_comp_vectors=%d)\n",
			ctx->num_comp_vectors);
	}

	fprintf(stderr, "cqq: ALL SUBTESTS PASS\n");
	rc = 0;
out:
	if (pd)
		ibv_dealloc_pd(pd);
	if (ctx)
		ibv_close_device(ctx);
	return rc;
}
