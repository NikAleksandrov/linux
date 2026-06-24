// SPDX-License-Identifier: GPL-2.0
/*
 * info_handles_probe -- empirical validation of generic uobject
 * enumeration via UVERBS_METHOD_INFO_HANDLES (design/uobject_restore.md
 * §7.2).
 *
 * The design originally specified a NEW generic uverbs method
 * `UVERBS_METHOD_INFO_LIST_UOBJS(type)` that walks `ufile->uobjects`
 * filtered by type and returns the matching handles. While reviewing
 * the kernel for K2 we found that **this primitive already exists** as
 * `UVERBS_METHOD_INFO_HANDLES` on `UVERBS_OBJECT_DEVICE`:
 *
 *     drivers/infiniband/core/uverbs_std_types_device.c
 *
 * It takes a `UVERBS_ATTR_INFO_OBJECT_ID` (u16, any value from
 * `enum uverbs_default_objects` AND any driver-namespace object id
 * because `uapi_get_object()` keys by `uapi_key_obj()` which already
 * encodes the namespace bit), walks `ufile->uobjects` under
 * `uobjects_lock` filtered by `obj->uapi_object`, and returns the
 * filled handles list plus the filled count in
 * `UVERBS_ATTR_INFO_TOTAL_HANDLES`.
 *
 * One subtlety worth recording explicitly because it shapes how the
 * CRIU dump plugin must call this: the kernel returns the FILLED
 * count (= min(real_total, buffer_capacity)), NOT the true total. The
 * caller can only detect saturation by comparing the returned count
 * against its allocated capacity. If they're equal, retry with a
 * larger buffer; otherwise the enumeration was complete. This program
 * exercises both the happy path and the saturation path.
 *
 * What we validate (against a normal ucontext on an unmodified ibdev,
 * no tracked-VF involvement so this test runs on any mlx5 PF):
 *
 *   (a) INFO_HANDLES returns exactly the handles we just allocated
 *       via libibverbs, in some order. Compared against the
 *       libibverbs-reported handle (qp->handle / pd->handle / etc).
 *   (b) INFO_HANDLES filters by type: enumerating PDs after we've
 *       allocated PDs+CQs+MRs+QPs returns only the PD handles.
 *   (c) INFO_HANDLES correctly drains across the driver namespace
 *       boundary by enumerating a driver-namespace type
 *       (MLX5_IB_OBJECT_UAR) on demand. We don't allocate UARs in
 *       this test (would require mlx5dv_devx_alloc_uar() and a
 *       lib_uar_dyn=1 ucontext), so the expected count is 0; the
 *       point is to prove the ioctl ACCEPTS driver-namespace ids
 *       without returning -EINVAL / -ENOENT.
 *   (d) Saturation: requesting fewer slots than the real total
 *       returns exactly `capacity` handles with TOTAL == capacity.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/info_handles/info_handles_probe
 *
 * Usage:
 *   ./info_handles_probe <ibdev>     # e.g. mlx5_0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <infiniband/verbs.h>

/*
 * Local copy of the uverbs ioctl wire format -- same as
 * tools/ucontext_vendor_verbs.c, kept minimal here. The kernel headers
 * (rdma_user_ioctl_cmds.h, ib_user_ioctl_cmds.h) carry the
 * authoritative definitions; we duplicate just the fields we need to
 * avoid pulling rdma-core uapi into a tools/testing build.
 */
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

/*
 * Core ids from include/uapi/rdma/ib_user_ioctl_cmds.h. Bare values
 * (no namespace shift) because UVERBS_OBJECT_DEVICE / INFO_HANDLES /
 * INFO_* live in the core namespace.
 */
#define UVERBS_OBJECT_DEVICE			0
#define UVERBS_OBJECT_PD			1
#define UVERBS_OBJECT_COMP_CHANNEL		2
#define UVERBS_OBJECT_CQ			3
#define UVERBS_OBJECT_QP			4
#define UVERBS_OBJECT_SRQ			5
#define UVERBS_OBJECT_AH			6
#define UVERBS_OBJECT_MR			7
#define UVERBS_OBJECT_MW			8
#define UVERBS_OBJECT_FLOW			9
#define UVERBS_OBJECT_XRCD			10
#define UVERBS_OBJECT_RWQ_IND_TBL		11
#define UVERBS_OBJECT_WQ			12
#define UVERBS_OBJECT_FLOW_ACTION		13
#define UVERBS_OBJECT_DM			14
#define UVERBS_OBJECT_COUNTERS			15
#define UVERBS_OBJECT_ASYNC_EVENT		16
#define UVERBS_OBJECT_DMAH			17

#define UVERBS_METHOD_INFO_HANDLES		1	/* position in
							 * enum uverbs_methods_device
							 */

#define UVERBS_ATTR_INFO_OBJECT_ID		0
#define UVERBS_ATTR_INFO_TOTAL_HANDLES		1
#define UVERBS_ATTR_INFO_HANDLES_LIST		2

/* Driver-namespace probe target (we don't allocate any; we just
 * confirm the ioctl accepts the id). Keep in sync with
 * tools/ucontext_vendor_verbs.c's MLX5_IB_OBJECT_UAR.
 */
#define UVERBS_ID_NS_SHIFT			12
#define MLX5_IB_OBJECT_UAR			((1u << UVERBS_ID_NS_SHIFT) + 8)
#define RDMA_DRIVER_MLX5			1

/*
 * Issue INFO_HANDLES.
 *   want_capacity_handles: how many u32 slots we reserve in the LIST
 *                          buffer. 0 means "TOTAL only, no list".
 *   handles_out, total_out: caller-owned; we copy_to from the kernel.
 * Returns 0 on success, -errno on ioctl failure (negated).
 */
static int issue_info_handles(int cmd_fd, uint16_t object_id,
			      uint32_t *handles_out,
			      uint32_t want_capacity_handles,
			      uint32_t *total_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr hdr;
		struct ib_uverbs_attr attrs[3];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id = UVERBS_OBJECT_DEVICE;
	cmd.hdr.method_id = UVERBS_METHOD_INFO_HANDLES;
	/*
	 * ib_uverbs_cmd_verbs() rejects any hdr->driver_id that doesn't
	 * match the bound device's uapi->driver_id, REGARDLESS of
	 * whether the object_id is in the core or driver namespace
	 * (see drivers/infiniband/core/uverbs_ioctl.c). For an mlx5
	 * ibdev that means we must always pass RDMA_DRIVER_MLX5 here;
	 * passing 0 fails with -EINVAL before the method ever runs.
	 */
	cmd.hdr.driver_id = RDMA_DRIVER_MLX5;

	/* INFO_OBJECT_ID (CONST_IN u16): value packed inline in .data,
	 * len=sizeof(u64) per the inline-attr convention.
	 */
	cmd.attrs[n].attr_id = UVERBS_ATTR_INFO_OBJECT_ID;
	cmd.attrs[n].len = sizeof(uint64_t);
	cmd.attrs[n].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data = object_id;
	n++;

	cmd.attrs[n].attr_id = UVERBS_ATTR_INFO_TOTAL_HANDLES;
	cmd.attrs[n].len = sizeof(*total_out);
	cmd.attrs[n].flags = 0;	/* UA_OPTIONAL on kernel side */
	cmd.attrs[n].data = (uintptr_t)total_out;
	n++;

	if (want_capacity_handles) {
		cmd.attrs[n].attr_id = UVERBS_ATTR_INFO_HANDLES_LIST;
		cmd.attrs[n].len = (uint16_t)(want_capacity_handles *
					      sizeof(uint32_t));
		cmd.attrs[n].flags = 0;	/* UA_OPTIONAL */
		cmd.attrs[n].data = (uintptr_t)handles_out;
		n++;
	}

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(cmd_fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

static struct ibv_device *find_ibdev(const char *name)
{
	struct ibv_device **list;
	struct ibv_device *match = NULL;
	int n, i;

	list = ibv_get_device_list(&n);
	if (!list || n == 0) {
		fprintf(stderr, "k2: ibv_get_device_list returned 0 devices\n");
		return NULL;
	}
	for (i = 0; i < n; i++) {
		if (!strcmp(ibv_get_device_name(list[i]), name)) {
			match = list[i];
			break;
		}
	}
	if (!match) {
		fprintf(stderr, "k2: ibdev '%s' not found; available:", name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	ibv_free_device_list(list);
	return match;
}

/*
 * Sort + uniq a small array of u32 handles in-place so we can compare
 * INFO_HANDLES output (kernel walks the uobjects list in LIFO order)
 * against the set of handles we recorded at create time without
 * depending on the kernel's traversal order.
 */
static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a;
	uint32_t y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

static int compare_handle_sets(const char *what,
			       uint32_t *expected, unsigned int n_expected,
			       uint32_t *got, unsigned int n_got)
{
	unsigned int i;

	if (n_expected != n_got) {
		fprintf(stderr,
			"  FAIL %s: count mismatch -- expected %u, got %u\n",
			what, n_expected, n_got);
		return 1;
	}
	qsort(expected, n_expected, sizeof(*expected), cmp_u32);
	qsort(got, n_got, sizeof(*got), cmp_u32);
	for (i = 0; i < n_expected; i++) {
		if (expected[i] != got[i]) {
			fprintf(stderr,
				"  FAIL %s: handle[%u] mismatch -- expected %u, got %u\n",
				what, i, expected[i], got[i]);
			return 1;
		}
	}
	printf("  PASS %s: %u handle(s) match libibverbs view\n",
	       what, n_got);
	return 0;
}

#define N_PD	3
#define N_CQ	2
#define N_QP	2
#define N_MR	2

int main(int argc, char **argv)
{
	const char *ibdev_name;
	struct ibv_device *dev;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pds[N_PD] = {};
	struct ibv_cq *cqs[N_CQ] = {};
	struct ibv_qp *qps[N_QP] = {};
	struct ibv_mr *mrs[N_MR] = {};
	void *mr_bufs[N_MR] = {};
	const size_t mr_len = 4096;
	uint32_t expected_pds[N_PD], expected_cqs[N_CQ];
	uint32_t expected_qps[N_QP], expected_mrs[N_MR];
	uint32_t got_handles[16];
	uint32_t total;
	int failed = 0;
	int i, err;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <ibdev>\n", argv[0]);
		return 2;
	}
	ibdev_name = argv[1];

	dev = find_ibdev(ibdev_name);
	if (!dev)
		return 1;

	ctx = ibv_open_device(dev);
	if (!ctx) {
		fprintf(stderr, "k2: ibv_open_device(%s) failed: %s\n",
			ibdev_name, strerror(errno));
		return 1;
	}

	/* Sanity check: ibv_context exposes cmd_fd as a public field;
	 * we'll be issuing the INFO_HANDLES ioctl against this fd
	 * directly so it sees the same ufile our libibverbs handles
	 * live under.
	 */
	printf("ibdev=%s cmd_fd=%d\n", ibdev_name, ctx->cmd_fd);

	for (i = 0; i < N_PD; i++) {
		pds[i] = ibv_alloc_pd(ctx);
		if (!pds[i]) {
			fprintf(stderr, "ibv_alloc_pd[%d] failed: %s\n",
				i, strerror(errno));
			goto out;
		}
		expected_pds[i] = pds[i]->handle;
	}
	for (i = 0; i < N_CQ; i++) {
		cqs[i] = ibv_create_cq(ctx, 16, NULL, NULL, 0);
		if (!cqs[i]) {
			fprintf(stderr, "ibv_create_cq[%d] failed: %s\n",
				i, strerror(errno));
			goto out;
		}
		expected_cqs[i] = cqs[i]->handle;
	}
	for (i = 0; i < N_MR; i++) {
		mr_bufs[i] = aligned_alloc(4096, mr_len);
		if (!mr_bufs[i]) {
			fprintf(stderr, "aligned_alloc(MR[%d]) failed\n", i);
			goto out;
		}
		memset(mr_bufs[i], 0, mr_len);
		mrs[i] = ibv_reg_mr(pds[0], mr_bufs[i], mr_len,
				    IBV_ACCESS_LOCAL_WRITE);
		if (!mrs[i]) {
			fprintf(stderr, "ibv_reg_mr[%d] failed: %s\n",
				i, strerror(errno));
			goto out;
		}
		expected_mrs[i] = mrs[i]->handle;
	}
	for (i = 0; i < N_QP; i++) {
		struct ibv_qp_init_attr qa = {
			.send_cq = cqs[0],
			.recv_cq = cqs[0],
			.qp_type = IBV_QPT_RC,
			.cap = {
				.max_send_wr = 4,
				.max_recv_wr = 4,
				.max_send_sge = 1,
				.max_recv_sge = 1,
			},
		};

		qps[i] = ibv_create_qp(pds[0], &qa);
		if (!qps[i]) {
			fprintf(stderr, "ibv_create_qp[%d] failed: %s\n",
				i, strerror(errno));
			goto out;
		}
		expected_qps[i] = qps[i]->handle;
	}

	printf("Allocated: %u PD(s), %u CQ(s), %u MR(s), %u QP(s)\n",
	       N_PD, N_CQ, N_MR, N_QP);

	/* ---- (a)+(b) per-type enumeration ----------------------- */
	printf("--- INFO_HANDLES enumeration ---\n");

	err = issue_info_handles(ctx->cmd_fd, UVERBS_OBJECT_PD,
				 got_handles, 16, &total);
	if (err) {
		fprintf(stderr, "  FAIL PD: ioctl returned %d (%s)\n",
			err, strerror(-err));
		failed = 1;
	} else {
		failed |= compare_handle_sets("PD",
			expected_pds, N_PD, got_handles, total);
	}

	err = issue_info_handles(ctx->cmd_fd, UVERBS_OBJECT_CQ,
				 got_handles, 16, &total);
	if (err) {
		fprintf(stderr, "  FAIL CQ: ioctl returned %d (%s)\n",
			err, strerror(-err));
		failed = 1;
	} else {
		failed |= compare_handle_sets("CQ",
			expected_cqs, N_CQ, got_handles, total);
	}

	err = issue_info_handles(ctx->cmd_fd, UVERBS_OBJECT_MR,
				 got_handles, 16, &total);
	if (err) {
		fprintf(stderr, "  FAIL MR: ioctl returned %d (%s)\n",
			err, strerror(-err));
		failed = 1;
	} else {
		failed |= compare_handle_sets("MR",
			expected_mrs, N_MR, got_handles, total);
	}

	err = issue_info_handles(ctx->cmd_fd, UVERBS_OBJECT_QP,
				 got_handles, 16, &total);
	if (err) {
		fprintf(stderr, "  FAIL QP: ioctl returned %d (%s)\n",
			err, strerror(-err));
		failed = 1;
	} else {
		failed |= compare_handle_sets("QP",
			expected_qps, N_QP, got_handles, total);
	}

	/* Negative-on-empty: AH wasn't allocated, expect 0 handles.
	 * (We deliberately don't create an AH because it needs a live
	 * port + GID/path and we want the test to work on any mlx5
	 * device, including ones whose ports happen to be down.)
	 */
	err = issue_info_handles(ctx->cmd_fd, UVERBS_OBJECT_AH,
				 got_handles, 16, &total);
	if (err) {
		fprintf(stderr, "  FAIL AH-empty: ioctl returned %d (%s)\n",
			err, strerror(-err));
		failed = 1;
	} else if (total != 0) {
		fprintf(stderr,
			"  FAIL AH-empty: expected 0 handles, got %u\n",
			total);
		failed = 1;
	} else {
		printf("  PASS AH-empty: 0 handle(s) as expected\n");
	}

	/* SRQ: also not created -- exercises an FD-class-adjacent IDR
	 * type that's expected to be empty.
	 */
	err = issue_info_handles(ctx->cmd_fd, UVERBS_OBJECT_SRQ,
				 got_handles, 16, &total);
	if (err) {
		fprintf(stderr, "  FAIL SRQ-empty: ioctl returned %d (%s)\n",
			err, strerror(-err));
		failed = 1;
	} else if (total != 0) {
		fprintf(stderr,
			"  FAIL SRQ-empty: expected 0 handles, got %u\n",
			total);
		failed = 1;
	} else {
		printf("  PASS SRQ-empty: 0 handle(s) as expected\n");
	}

	/* ---- (c) driver-namespace acceptance --------------------
	 * MLX5_IB_OBJECT_UAR. The point of this case is to prove the
	 * core INFO_HANDLES handler honours driver-namespace object
	 * ids transparently (uapi_get_object() routes via uapi_key_obj
	 * which already encodes the NS bit). The observed count is
	 * incidental and depends on whether the installed libmlx5
	 * allocates dynamic UAR uobjects per-QP -- recent rdma-core
	 * does, older versions don't -- but any count >= 0 still
	 * proves the ioctl path is sound.
	 */
	err = issue_info_handles(ctx->cmd_fd, MLX5_IB_OBJECT_UAR,
				 got_handles, 16, &total);
	if (err) {
		fprintf(stderr,
			"  FAIL MLX5_IB_OBJECT_UAR: ioctl returned %d (%s)\n",
			err, strerror(-err));
		failed = 1;
	} else {
		printf("  PASS MLX5_IB_OBJECT_UAR: driver-namespace id "
		       "accepted, %u handle(s) reported "
		       "(libmlx5-internal UAR uobjects -- not asserted)\n",
		       total);
	}

	/* ---- (d) saturation -------------------------------------
	 * Ask for only 1 PD slot when we have N_PD. Per the kernel
	 * convention TOTAL == returned (filled) count, so we expect
	 * TOTAL == 1 here -- and the caller would then know to retry
	 * with a bigger buffer (real total > 1).
	 */
	err = issue_info_handles(ctx->cmd_fd, UVERBS_OBJECT_PD,
				 got_handles, 1, &total);
	if (err) {
		fprintf(stderr,
			"  FAIL PD-saturation: ioctl returned %d (%s)\n",
			err, strerror(-err));
		failed = 1;
	} else if (total != 1) {
		fprintf(stderr,
			"  FAIL PD-saturation: expected TOTAL=1 with cap=1, got %u\n",
			total);
		failed = 1;
	} else {
		printf("  PASS PD-saturation: TOTAL=%u with cap=1 "
		       "(caller can detect via TOTAL==cap and re-issue)\n",
		       total);
	}

	/* ---- TOTAL-only (sizing pass equivalent) -----------------
	 * No LIST attr at all -- caller only wants to know "should I
	 * bother enumerating?". The handler currently fills TOTAL with
	 * the FILTERED filled count, which when there's no buffer is
	 * just 0 (it breaks on the very first iteration before
	 * incrementing). Document the observed behaviour so the CRIU
	 * plugin doesn't get surprised: TOTAL-only is NOT a true count
	 * primitive on the current kernel; callers should size their
	 * buffer optimistically and grow on TOTAL==cap.
	 */
	err = issue_info_handles(ctx->cmd_fd, UVERBS_OBJECT_PD,
				 NULL, 0, &total);
	if (err) {
		printf("  NOTE PD-total-only: ioctl returned %d (%s) -- "
		       "kernel rejects len<=0 LIST. Callers must always "
		       "pass at least one slot.\n",
		       err, strerror(-err));
	} else {
		printf("  NOTE PD-total-only: TOTAL=%u (vs %u real). "
		       "TOTAL-only sizing pass is %s on current kernel.\n",
		       total, N_PD,
		       total == N_PD ? "supported" : "NOT a real count");
	}

	if (!failed)
		printf("\nK2 OVERALL: PASS -- INFO_HANDLES meets the "
		       "design's K2 requirements without new kernel work.\n");
	else
		printf("\nK2 OVERALL: FAIL -- see PASS/FAIL lines above.\n");

out:
	for (i = 0; i < N_QP; i++)
		if (qps[i]) ibv_destroy_qp(qps[i]);
	for (i = 0; i < N_MR; i++) {
		if (mrs[i]) ibv_dereg_mr(mrs[i]);
		free(mr_bufs[i]);
	}
	for (i = 0; i < N_CQ; i++)
		if (cqs[i]) ibv_destroy_cq(cqs[i]);
	for (i = 0; i < N_PD; i++)
		if (pds[i]) ibv_dealloc_pd(pds[i]);
	if (ctx)
		ibv_close_device(ctx);
	return failed ? 1 : 0;
}
