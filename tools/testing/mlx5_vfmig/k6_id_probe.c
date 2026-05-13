// SPDX-License-Identifier: GPL-2.0
/*
 * k6_id_probe -- empirical probe for DESIGN_R3_uobj_restore.md §8.2
 * ("K6 FW identity continuity") and §6.3 (RQ-head/tail preservation
 * piggyback experiment).
 *
 * Allocates one of each FW-id-bearing user-mode resource class --
 * PD, CQ, QP (RC), MR, SRQ -- against the given ib_device, prints
 * the FW ids in a parseable shell-eval format, then BLOCKS on stdin
 * until it reads "quit\n". This shape lets a driver script:
 *
 *     - Fork the probe in the background.
 *     - Drain its stdout until it sees "READY" -- at that point all
 *       resources are alive in FW and tagged with the printed ids.
 *     - Issue SAVE_VHCA_STATE (the resources remain alive across SAVE
 *       because we still hold them).
 *     - Tell the probe to exit ("quit"), which closes the ucontext
 *       and lets the kernel dealloc-cascade run.
 *
 * The destination half of the experiment runs the same binary against
 * the restored ib_device after LOAD_VHCA_STATE + MARK_RESTORED; the
 * driver script then compares destination ids against source ids per
 * the §8.2.1 pass-condition table:
 *
 *     dest_id > max(source_id)  =>  FW preserves reservations
 *
 * Built against libibverbs + libmlx5 (mlx5dv); the FW ids that
 * libibverbs alone doesn't expose (pdn, cqn, srqn) are extracted via
 * mlx5dv_init_obj(), which reads them out of the libmlx5-internal
 * objects backing the ibv_* handles. qpn comes straight off ibv_qp,
 * and the MR's "mkey index" is the lkey/rkey value (lkey == rkey on
 * a standard MR; only the access bits differ between them).
 *
 * Build:
 *   make k6_id_probe
 *
 * Usage:
 *   ./k6_id_probe <ibdev>
 *   ./k6_id_probe <ibdev> --post-recv-wrs N   # §6.3 piggyback: also
 *                                              # post N receive WRs to
 *                                              # the RQ so a follow-on
 *                                              # QUERY_QP can compare
 *                                              # head/tail across SAVE.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

static struct ibv_device *find_ibdev(const char *name)
{
	struct ibv_device **list;
	int n, i;
	struct ibv_device *match = NULL;

	list = ibv_get_device_list(&n);
	if (!list || n == 0) {
		fprintf(stderr, "k6: ibv_get_device_list returned 0 devices\n");
		return NULL;
	}
	for (i = 0; i < n; i++) {
		if (!strcmp(ibv_get_device_name(list[i]), name)) {
			match = list[i];
			break;
		}
	}
	if (!match) {
		fprintf(stderr, "k6: ibdev '%s' not found; available:", name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	/* The list itself can be freed; the ibv_device pointers stay
	 * valid until ibv_open_device + the next ibv_get_device_list
	 * cycle, which is fine for our one-shot use. */
	ibv_free_device_list(list);
	return match;
}

static int extract_pdn(struct ibv_pd *pd, uint32_t *out)
{
	struct mlx5dv_pd dvpd = {};
	struct mlx5dv_obj obj = {
		.pd = { .in = pd, .out = &dvpd },
	};
	int err = mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD);
	if (err) {
		fprintf(stderr, "k6: mlx5dv_init_obj(PD) failed: %d\n", err);
		return -1;
	}
	*out = dvpd.pdn;
	return 0;
}

static int extract_cqn(struct ibv_cq *cq, uint32_t *out)
{
	struct mlx5dv_cq dvcq = {};
	struct mlx5dv_obj obj = {
		.cq = { .in = cq, .out = &dvcq },
	};
	int err = mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ);
	if (err) {
		fprintf(stderr, "k6: mlx5dv_init_obj(CQ) failed: %d\n", err);
		return -1;
	}
	*out = dvcq.cqn;
	return 0;
}

static int extract_srqn(struct ibv_srq *srq, uint32_t *out)
{
	/* mlx5dv exposes mlx5dv_obj.srq.out via MLX5DV_OBJ_SRQ. Field
	 * is `srqn` on recent rdma-core; named the same across the
	 * versions we target. */
	struct mlx5dv_srq dvsrq = {};
	struct mlx5dv_obj obj = {
		.srq = { .in = srq, .out = &dvsrq },
	};
	int err = mlx5dv_init_obj(&obj, MLX5DV_OBJ_SRQ);
	if (err) {
		fprintf(stderr, "k6: mlx5dv_init_obj(SRQ) failed: %d\n", err);
		return -1;
	}
	*out = dvsrq.srqn;
	return 0;
}

int main(int argc, char **argv)
{
	const char *ibdev_name;
	int post_recv_wrs = 0;
	struct ibv_device *dev;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pd = NULL;
	struct ibv_cq *cq = NULL;
	struct ibv_qp *qp = NULL;
	struct ibv_mr *mr = NULL;
	struct ibv_srq *srq = NULL;
	void *mr_buf = NULL;
	const size_t mr_len = 4096;
	uint32_t pdn = 0, cqn = 0, qpn = 0, lkey = 0, rkey = 0, srqn = 0;
	int rc = 1;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <ibdev> [--post-recv-wrs N]\n",
			argv[0]);
		return 2;
	}
	ibdev_name = argv[1];
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--post-recv-wrs") && i + 1 < argc) {
			post_recv_wrs = atoi(argv[++i]);
			if (post_recv_wrs < 0)
				post_recv_wrs = 0;
		} else {
			fprintf(stderr, "k6: unknown arg '%s'\n", argv[i]);
			return 2;
		}
	}

	dev = find_ibdev(ibdev_name);
	if (!dev)
		return 1;

	ctx = ibv_open_device(dev);
	if (!ctx) {
		fprintf(stderr, "k6: ibv_open_device(%s) failed: %s\n",
			ibdev_name, strerror(errno));
		return 1;
	}

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		fprintf(stderr, "k6: ibv_alloc_pd failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (extract_pdn(pd, &pdn))
		goto out;

	cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!cq) {
		fprintf(stderr, "k6: ibv_create_cq failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (extract_cqn(cq, &cqn))
		goto out;

	mr_buf = aligned_alloc(4096, mr_len);
	if (!mr_buf) {
		fprintf(stderr, "k6: aligned_alloc(MR buf) failed\n");
		goto out;
	}
	memset(mr_buf, 0, mr_len);
	mr = ibv_reg_mr(pd, mr_buf, mr_len,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
			IBV_ACCESS_REMOTE_READ);
	if (!mr) {
		fprintf(stderr, "k6: ibv_reg_mr failed: %s\n",
			strerror(errno));
		goto out;
	}
	lkey = mr->lkey;
	rkey = mr->rkey;

	/*
	 * Order matters: QP must come before SRQ. SRQ creation is
	 * known to fail with EINVAL on restored VFs today (the
	 * `mlx5_ib_dev_res_srq_init` gate is kept on restored VFs --
	 * separately tracked in the design doc as `track_known_srq_gate`),
	 * so if we did SRQ first we'd lose QP data on the destination
	 * leg of the K6 experiment. We treat SRQ as best-effort and
	 * emit `srqn=SKIPPED` when it fails so the rest of the
	 * experiment still produces a verdict.
	 */
	{
		struct ibv_qp_init_attr qa = {
			.send_cq = cq,
			.recv_cq = cq,
			.qp_type = IBV_QPT_RC,
			.cap = {
				.max_send_wr = 4,
				.max_recv_wr = 4,
				.max_send_sge = 1,
				.max_recv_sge = 1,
			},
		};
		qp = ibv_create_qp(pd, &qa);
		if (!qp) {
			fprintf(stderr, "k6: ibv_create_qp failed: %s\n",
				strerror(errno));
			goto out;
		}
		qpn = qp->qp_num;
	}

	{
		struct ibv_srq_init_attr sa = {
			.attr = {
				.max_wr = 4,
				.max_sge = 1,
			},
		};
		srq = ibv_create_srq(pd, &sa);
		if (!srq) {
			fprintf(stderr,
				"k6: ibv_create_srq failed (best-effort, "
				"continuing without SRQ data): %s\n",
				strerror(errno));
			/* leave srq = NULL, srqn unset -> emit sentinel below */
		} else if (extract_srqn(srq, &srqn)) {
			/* SRQ allocated but couldn't read srqn -- treat as
			 * unmeasurable; don't fail the whole experiment. */
			fprintf(stderr,
				"k6: mlx5dv_init_obj(SRQ) failed; SRQ id "
				"not reportable\n");
			ibv_destroy_srq(srq);
			srq = NULL;
		}
	}

	/*
	 * §6.3 piggyback: post N receive WRs to the QP's RQ. We have
	 * to take the QP at least into INIT for post_recv to be legal
	 * on some providers; on mlx5 it's accepted in RESET so we
	 * post directly. The receive WRs reference our local MR so
	 * they're self-contained; we never actually drive RTR/RTS.
	 *
	 * A follow-on QUERY_QP (driven by a PF cdev probe ioctl, not
	 * by this tool) reads the RQ head/tail before SAVE; the dest
	 * side compares after LOAD.
	 */
	if (post_recv_wrs > 0) {
		for (int i = 0; i < post_recv_wrs; i++) {
			struct ibv_sge sge = {
				.addr = (uintptr_t)mr_buf,
				.length = mr_len,
				.lkey = lkey,
			};
			struct ibv_recv_wr wr = {
				.wr_id = 0xCAFE0000u | i,
				.sg_list = &sge,
				.num_sge = 1,
			};
			struct ibv_recv_wr *bad = NULL;
			int err = ibv_post_recv(qp, &wr, &bad);
			if (err) {
				fprintf(stderr,
					"k6: ibv_post_recv #%d failed: %s\n",
					i, strerror(err));
				goto out;
			}
		}
	}

	/* Emit the manifest. Format is intentionally shell-eval-able:
	 * each line of the form "key=value" suitable for `eval src_$line`. */
	printf("ibdev=%s\n", ibdev_name);
	printf("pdn=%u\n", pdn);
	printf("cqn=%u\n", cqn);
	printf("qpn=%u\n", qpn);
	printf("lkey=0x%08x\n", lkey);
	printf("rkey=0x%08x\n", rkey);
	printf("mkey_index=%u\n", lkey >> 8);
	if (srq)
		printf("srqn=%u\n", srqn);
	else
		printf("srqn=SKIPPED\n");
	printf("recv_wrs_posted=%d\n", post_recv_wrs);
	printf("READY\n");
	fflush(stdout);

	{
		char line[64];
		while (fgets(line, sizeof(line), stdin)) {
			if (!strncmp(line, "quit", 4))
				break;
		}
	}

	rc = 0;

out:
	if (qp)
		ibv_destroy_qp(qp);
	if (srq)
		ibv_destroy_srq(srq);
	if (mr)
		ibv_dereg_mr(mr);
	free(mr_buf);
	if (cq)
		ibv_destroy_cq(cq);
	if (pd)
		ibv_dealloc_pd(pd);
	if (ctx)
		ibv_close_device(ctx);
	return rc;
}
