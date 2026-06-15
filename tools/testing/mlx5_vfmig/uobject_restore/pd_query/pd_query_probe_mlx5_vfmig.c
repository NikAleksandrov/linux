// SPDX-License-Identifier: GPL-2.0
/*
 * pd_query_probe_mlx5_vfmig -- exercises the dump-side
 * MLX5_IB_METHOD_VFMIG_QUERY_PD verb (design/uobject_restore.md §5.1.4).
 *
 * This verb supersedes the earlier NLDEV driver-TLV discovery path
 * (mlx5/restrack.c fill_res_pd_entry emitting "fw_pdn"/"fw_uid" under
 * RDMA_NLDEV_ATTR_DRIVER). PD was the odd one out: every other adopted
 * FW resource id (cqn via QUERY_CQ, qpn via QUERY_QP) is discovered
 * through a per-handle driver-private QUERY method that returns a
 * byte-equal RESP_BLOB. QUERY_PD puts PD on the same plane.
 *
 * Establishes equality between two views of the same live PD:
 *
 *   (A) The libmlx5 mlx5dv_init_obj(MLX5DV_OBJ_PD) view -- dvpd.pdn is
 *       the process-independent FW pdn. This is the same value a CRIU
 *       dumper would want, except CRIU runs in its own address space
 *       and (for the wider QP/CQ cases) mlx5dv yields the caller's VAs
 *       rather than the dumpee's. For PD there are no VAs, only the
 *       FW pdn, so the cross-process gap is narrower -- but routing PD
 *       discovery through the same uverbs fd as QUERY_CQ/_QP keeps the
 *       plugin's per-type code uniform and drops the CAP_NET_ADMIN /
 *       cross-netns NLDEV dependency.
 *
 *   (B) The kernel-side MLX5_IB_METHOD_VFMIG_QUERY_PD view -- the
 *       handler reads mpd->pdn into a struct mlx5_ib_restore_pd_req
 *       blob (byte-equal to what RESTORE_PD consumes) plus mpd->uid
 *       as a scalar dump-side cross-check out.
 *
 * In a single-process probe (A) and (B) describe the same live PD, so
 * the verb-mechanics contract we demand is:
 *
 *     blob.pdn       == dvpd.pdn          (byte-equal FW pdn)
 *     blob.reserved  == 0
 *     blob.reserved2 == 0
 *
 * The fourth out, resp_uid, is the source ucontext's devx_uid (every
 * PD under a context inherits it; mlx5_ib_alloc_pd sets mpd->uid =
 * context->devx_uid). It is NOT a fixed value this probe can demand:
 *
 *   - uid == 0  is the v0-supported lane: a non-DEVX ucontext. CRIU's
 *     dump policy accepts these; RESTORE_PD lands every adopted FW
 *     resource in the uid=0 host-privileged ungated lane.
 *   - uid != 0  is the libmlx5 auto-DEVX lane: modern rdma-core
 *     (>= ~v36) allocates a DEVX uid at context-open time when the
 *     process has the privilege for it (e.g. run under sudo). v0 of
 *     the CRIU plugin *refuses* such sources at the QUERY_UCONTEXT
 *     meta.devx_uid gate (see mlx5_user_ioctl_cmds.h @devx_uid and
 *     design/uobject_restore.md §5.1.4 / §S3b), because the dest VF's
 *     FW rejects the unregistered uid post-LOAD.
 *
 * So this probe validates the verb mechanics unconditionally and
 * *reports* the uid lane: uid==0 is annotated "v0-supported lane
 * confirmed"; uid!=0 is annotated "auto-DEVX lane (v0 dump policy
 * would refuse this source)" -- still a PASS for the verb, since the
 * pdn byte-equal contract is what QUERY_PD owes its caller. The
 * uid==0 dump-time refusal is CRIU plugin policy, enforced at the
 * QUERY_UCONTEXT seam, not inside QUERY_PD.
 *
 * Subtests:
 *
 *   1. happy path. Single PD. pdn byte-equality + reserved-zero;
 *      uid lane reported.
 *
 *   2. invalid handle. QUERY_PD on an unallocated handle
 *      (HANDLE == 0xdeadbeef) must return -ENOENT (the IDR lookup
 *      miss from uverbs_ioctl.c). Validates the dispatcher gate.
 *
 *   3. multi-PD. Two distinct PDs. Each query returns its own correct
 *      pdn -- catches any "global state" / "wrong pd" bug. Both PDs
 *      live under the same ucontext, so their uids must match
 *      (shared context->devx_uid invariant).
 *
 * Build:
 *   make -C tools/testing/mlx5_vfmig \
 *        uobject_restore/pd_query/pd_query_probe_mlx5_vfmig
 *
 * Usage:
 *   ./pd_query_probe_mlx5_vfmig <ibdev>
 *
 * <ibdev>   ib_device name, e.g. mlx5_2.
 */

#include <errno.h>
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
 * Mirror of include/uapi/rdma/mlx5-abi.h struct mlx5_ib_restore_pd_req.
 * 16 bytes, byte-equal to what UVERBS_METHOD_RESTORE_PD consumes via
 * its UHW_IN tail. The QUERY_PD handler emits this struct verbatim; a
 * CRIU dumper would memcpy it into protobuf at dump and back into
 * RESTORE_PD's UHW_IN at restore with no field-level marshaling.
 */
struct mlx5_ib_restore_pd_req {
	uint32_t	pdn;
	uint32_t	reserved;
	uint64_t	reserved2;
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
 * Per-namespace ids are (UVERBS_ID_DRIVER_NS | ordinal) for driver
 * namespaces. See the longer note in cq_query_probe_mlx5_vfmig.c.
 */
#define UVERBS_ID_NS_SHIFT			12
#define UVERBS_ID_DRIVER_NS			(1u << UVERBS_ID_NS_SHIFT)

/*
 * Mirror of enum mlx5_ib_objects: VFMIG is the 11th member
 * (DEVX=0 ... VFMIG=10). See cq_query_probe_mlx5_vfmig.c.
 */
#define MLX5_IB_OBJECT_VFMIG			(UVERBS_ID_DRIVER_NS + 10u)

/*
 * Mirror of enum mlx5_ib_vfmig_methods declaration order:
 *   QUERY_UCONTEXT(0), RESTORE_UCONTEXT(1), QUERY_DYN_UARS(2),
 *   RESTORE_DYN_UARS(3), QUERY_CQ(4), QUERY_QP(5), QUERY_PD(6).
 */
#define MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT	(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_METHOD_VFMIG_QUERY_PD		(MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT + 6u)

/*
 * Mirror of enum mlx5_ib_vfmig_query_pd_attrs. HANDLE is the IDR-
 * resolved PD uobject; RESP_BLOB / RESP_UID are the two outs.
 */
#define MLX5_IB_ATTR_VFMIG_QUERY_PD_HANDLE	(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_ATTR_VFMIG_QUERY_PD_RESP_BLOB	(MLX5_IB_ATTR_VFMIG_QUERY_PD_HANDLE + 1u)
#define MLX5_IB_ATTR_VFMIG_QUERY_PD_RESP_UID	(MLX5_IB_ATTR_VFMIG_QUERY_PD_HANDLE + 2u)

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
		fprintf(stderr, "pdq: ibv_get_device_list returned 0 devices\n");
		return NULL;
	}
	for (i = 0; i < n; i++) {
		if (!strcmp(ibv_get_device_name(list[i]), name)) {
			match = list[i];
			break;
		}
	}
	if (!match) {
		fprintf(stderr, "pdq: ibdev '%s' not found; available:", name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	ibv_free_device_list(list);
	return match;
}

/*
 * mlx5dv_init_obj(MLX5DV_OBJ_PD) gives us the FW pdn for the PD. We
 * treat this as ground truth in the single-process probe; the
 * QUERY_PD ioctl below must produce a byte-equal pdn.
 */
static int extract_dv_pd(struct ibv_pd *pd, struct mlx5dv_pd *out)
{
	struct mlx5dv_obj obj = {
		.pd = { .in = pd, .out = out },
	};
	int err = mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD);

	if (err) {
		fprintf(stderr, "pdq: mlx5dv_init_obj(PD) failed: %d\n", err);
		return -1;
	}
	return 0;
}

/*
 * Issue MLX5_IB_METHOD_VFMIG_QUERY_PD on @fd (the libibverbs cmd_fd)
 * for the PD at ufile @pd_handle. Fills @blob_out / @uid_out on
 * success. Returns 0 or -errno.
 */
static int do_vfmig_query_pd(int fd, uint32_t pd_handle,
			     struct mlx5_ib_restore_pd_req *blob_out,
			     uint32_t *uid_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[3];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= MLX5_IB_OBJECT_VFMIG;
	cmd.hdr.method_id	= MLX5_IB_METHOD_VFMIG_QUERY_PD;
	cmd.hdr.driver_id	= RDMA_DRIVER_MLX5;

	/*
	 * IDR-class attr: uverbs_ioctl.c enforces len == 0 and reads
	 * the uobject handle from uattr->data directly. Setting len = 4
	 * the way PTR_IN attrs do trips its `if (uattr->len != 0)
	 * return -EINVAL` guard before the dispatcher reaches our
	 * handler.
	 */
	cmd.attrs[n].attr_id	= MLX5_IB_ATTR_VFMIG_QUERY_PD_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= pd_handle;
	n++;

	cmd.attrs[n].attr_id	= MLX5_IB_ATTR_VFMIG_QUERY_PD_RESP_BLOB;
	cmd.attrs[n].len	= sizeof(*blob_out);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)blob_out;
	n++;

	cmd.attrs[n].attr_id	= MLX5_IB_ATTR_VFMIG_QUERY_PD_RESP_UID;
	cmd.attrs[n].len	= sizeof(*uid_out);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)uid_out;
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

struct pd_obs {
	struct mlx5_ib_restore_pd_req	blob;
	uint32_t			uid;
	struct mlx5dv_pd		dv;
};

static int compare_views(const struct pd_obs *o, const char *label)
{
	int ok = 1;

	if (o->blob.pdn != o->dv.pdn) {
		fprintf(stderr,
			"pdq[%s]: blob.pdn=0x%x != dvpd.pdn=0x%x\n",
			label, o->blob.pdn, o->dv.pdn);
		ok = 0;
	}
	if (o->blob.reserved != 0 || o->blob.reserved2 != 0) {
		fprintf(stderr,
			"pdq[%s]: blob.reserved=%u blob.reserved2=%llu (must be 0)\n",
			label, o->blob.reserved,
			(unsigned long long)o->blob.reserved2);
		ok = 0;
	}

	if (ok) {
		fprintf(stderr,
			"pdq[%s]: STRONG byte-equal: pdn=0x%x uid=%u\n",
			label, o->blob.pdn, o->uid);
		if (o->uid == 0)
			fprintf(stderr,
				"pdq[%s]: uid lane: 0 -- v0-supported (non-DEVX) lane confirmed\n",
				label);
		else
			fprintf(stderr,
				"pdq[%s]: uid lane: %u -- libmlx5 auto-DEVX lane; v0 dump policy would refuse this source at the QUERY_UCONTEXT meta.devx_uid gate (verb mechanics still valid)\n",
				label, o->uid);
	}
	return ok ? 0 : -1;
}

/* Subtest 1: single-PD happy path. */
static int subtest_happy(struct ibv_context *ctx, const char *label)
{
	struct ibv_pd *pd;
	struct pd_obs obs = {};
	int rc = -1;

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		fprintf(stderr, "pdq[%s]: ibv_alloc_pd failed: %s\n",
			label, strerror(errno));
		return -1;
	}

	if (extract_dv_pd(pd, &obs.dv))
		goto out_dealloc;

	if (do_vfmig_query_pd(ctx->cmd_fd, pd->handle, &obs.blob, &obs.uid)) {
		fprintf(stderr, "pdq[%s]: VFMIG_QUERY_PD failed: %s\n",
			label, strerror(errno));
		goto out_dealloc;
	}

	if (compare_views(&obs, label))
		goto out_dealloc;

	rc = 0;
out_dealloc:
	ibv_dealloc_pd(pd);
	return rc;
}

/* Subtest 2: invalid handle. */
static int subtest_invalid_handle(struct ibv_context *ctx)
{
	struct mlx5_ib_restore_pd_req blob;
	uint32_t uid = 0;
	int err;

	err = do_vfmig_query_pd(ctx->cmd_fd, 0xdeadbeefu, &blob, &uid);
	if (err == 0) {
		fprintf(stderr, "pdq[invalid]: succeeded on bogus handle 0xdeadbeef (must -ENOENT)\n");
		return -1;
	}
	if (err != -ENOENT && err != -EINVAL) {
		fprintf(stderr, "pdq[invalid]: unexpected errno=%d (-%s); want -ENOENT or -EINVAL\n",
			-err, strerror(-err));
		return -1;
	}
	fprintf(stderr, "pdq[invalid]: PASS (errno=%d -%s)\n",
		-err, strerror(-err));
	return 0;
}

/*
 * Subtest 3: multi-PD. Allocate two PDs, query both, assert each
 * matches its own (A)-view and the two pdns differ (no cross-talk).
 */
static int subtest_multi_pd(struct ibv_context *ctx)
{
	struct ibv_pd *pd_a = NULL, *pd_b = NULL;
	struct pd_obs obs_a = {}, obs_b = {};
	int rc = -1;

	pd_a = ibv_alloc_pd(ctx);
	pd_b = ibv_alloc_pd(ctx);
	if (!pd_a || !pd_b) {
		fprintf(stderr, "pdq[multi]: ibv_alloc_pd failed\n");
		goto out;
	}

	if (extract_dv_pd(pd_a, &obs_a.dv) || extract_dv_pd(pd_b, &obs_b.dv))
		goto out;
	if (do_vfmig_query_pd(ctx->cmd_fd, pd_a->handle, &obs_a.blob, &obs_a.uid) ||
	    do_vfmig_query_pd(ctx->cmd_fd, pd_b->handle, &obs_b.blob, &obs_b.uid)) {
		fprintf(stderr, "pdq[multi]: QUERY_PD failed\n");
		goto out;
	}
	if (compare_views(&obs_a, "multi.A") ||
	    compare_views(&obs_b, "multi.B"))
		goto out;
	if (obs_a.blob.pdn == obs_b.blob.pdn) {
		fprintf(stderr, "pdq[multi]: pdn collision (both 0x%x)\n",
			obs_a.blob.pdn);
		goto out;
	}
	if (obs_a.uid != obs_b.uid) {
		fprintf(stderr,
			"pdq[multi]: uid mismatch (A=%u B=%u) -- both PDs share one ucontext, devx_uid must match\n",
			obs_a.uid, obs_b.uid);
		goto out;
	}
	rc = 0;
out:
	if (pd_a)
		ibv_dealloc_pd(pd_a);
	if (pd_b)
		ibv_dealloc_pd(pd_b);
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
		fprintf(stderr, "pdq: ibv_open_device(%s) failed: %s\n",
			ibdev_name, strerror(errno));
		return 1;
	}

	fprintf(stderr, "pdq: ibdev=%s\n", ibdev_name);

	if (subtest_happy(ctx, "happy"))
		goto out;
	if (subtest_invalid_handle(ctx))
		goto out;
	if (subtest_multi_pd(ctx))
		goto out;

	fprintf(stderr, "pdq: ALL SUBTESTS PASS\n");
	rc = 0;
out:
	if (ctx)
		ibv_close_device(ctx);
	return rc;
}
