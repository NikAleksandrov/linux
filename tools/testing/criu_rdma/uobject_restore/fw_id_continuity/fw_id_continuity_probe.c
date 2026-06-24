// SPDX-License-Identifier: GPL-2.0
/*
 * fw_id_continuity_probe -- empirical probe for
 * design/uobject_restore.md §8.2 ("FW identity continuity") and §6.3
 * (QPC preservation piggyback experiment).
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
 * §6.3 / S6 piggyback extension:
 *   The QP can optionally be transitioned through the IBTA state
 *   machine RESET -> INIT -> RTR -> RTS via SELF-LOOPBACK (the QP
 *   targets its own qpn through its own port's GID). This populates
 *   the RTR/RTS-set QPC fields (path_mtu, min_rnr_nak, log_rra_max,
 *   primary_address_path, log_sra_max, retry_count, rnr_retry, ...)
 *   without needing a peer or fabric reachability. The harness then
 *   uses MLX5_VFMIG_IOC_QUERY_QP (which now returns the wider QPC
 *   subset) to byte-compare src/dst across SAVE/LOAD.
 *
 *   Self-loopback uses the first non-zero GID found in the port's
 *   GID table (RoCE: typically the link-local IPv6 GID auto-derived
 *   from the netdev MAC; IB: the SM-assigned GID at index 0). The
 *   QP never actually transmits anything across the wire because we
 *   don't post any send WRs; the RTR/RTS transition is enough to
 *   write the QPC fields we want to test.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/fw_id_continuity/fw_id_continuity_probe
 *
 * Usage:
 *   ./fw_id_continuity_probe <ibdev>
 *   ./fw_id_continuity_probe <ibdev> --qp-state {RESET|INIT|RTR|RTS}
 *                                              # Drive the K6 QP through
 *                                              # the listed states (default
 *                                              # RESET; INIT/RTR/RTS use
 *                                              # self-loopback for AV).
 *   ./fw_id_continuity_probe <ibdev> --post-recv-wrs N
 *                                              # Post N receive WRs to the
 *                                              # RQ for the §6.3 piggyback;
 *                                              # implies at least INIT.
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

struct cq_context_emit {
	uint32_t cqn;
	uint32_t cqe_cnt;
	uint32_t cqe_size;
	uint64_t buf_addr;	/* user VA of CQE ring buffer (mlx5dv_cq.buf) */
	uint64_t db_addr;	/* user VA of doorbell record (mlx5dv_cq.dbrec)
				 * -- not page-aligned; the kernel applies
				 * & PAGE_MASK on its side. Passed verbatim
				 * to mlx5_ib_restore_cq_req.db_addr where
				 * the unaligned offset within the page is
				 * preserved for the destination's
				 * cq->db.dma computation. */
};

/*
 * Capture every cqc-derivable field the S5b RESTORE_CQ verb body
 * needs from the source-side mlx5_ib_create_cq landing -- the cqn
 * (FW resource id), the userspace VAs of the CQE ring buffer and
 * doorbell record (which the destination kernel will rebind into
 * the LOAD_VHCA_STATE-replayed (KIND_CQ, cqn) and
 * (KIND_DBR, dbrec & PAGE_MASK) placeholders, respectively), the
 * user-visible cqe count (which the dispatcher copies into
 * attr->cqe and the handler stores at cq->ibcq.cqe), and cqe_size
 * (which the destination kernel needs to parse the adopted CQE
 * ring at poll time).
 *
 * mlx5dv_init_obj(MLX5DV_OBJ_CQ) reads these out of the
 * libmlx5-internal struct mlx5_cq -- they are not exposed by stock
 * libibverbs because there is no need to surface them at create
 * time (only the userspace polling code under the same libmlx5
 * library consumes them). For CRIU R3 they become wire-visible
 * input to RESTORE_CQ on the destination, hence this helper.
 */
static int extract_cq_context(struct ibv_cq *cq,
			      struct cq_context_emit *out)
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
	out->cqn = dvcq.cqn;
	out->cqe_cnt = dvcq.cqe_cnt;
	out->cqe_size = dvcq.cqe_size;
	out->buf_addr = (uint64_t)(uintptr_t)dvcq.buf;
	out->db_addr  = (uint64_t)(uintptr_t)dvcq.dbrec;
	return 0;
}

/*
 * Find the first non-zero GID in @ctx's GID table for @port. Returns
 * the index in *sgid_idx and the GID in *gid; -1 on error / nothing
 * found.
 *
 * Why "first non-zero": the RoCE GID table is sparse (some indices
 * are unset, encoded as the all-zero GID), and the index of the
 * link-local IPv6 GID isn't fixed across kernel versions. Scanning
 * for the first usable entry is robust and matches what other
 * userspace tools (perftest etc.) do.
 *
 * Bound at 256 indices defensively; ibv_query_port returns a
 * gid_tbl_len which is the real cap on most fabrics, but we don't
 * trust it for the iteration here.
 */
static int find_local_gid(struct ibv_context *ctx, uint8_t port,
			  union ibv_gid *gid, int *sgid_idx)
{
	struct ibv_port_attr pa = {};
	int err = ibv_query_port(ctx, port, &pa);
	int n, i;

	if (err) {
		fprintf(stderr, "k6: ibv_query_port(%u) failed: %s\n",
			port, strerror(err));
		return -1;
	}
	n = pa.gid_tbl_len > 0 ? pa.gid_tbl_len : 256;
	if (n > 256)
		n = 256;
	for (i = 0; i < n; i++) {
		union ibv_gid g = {};
		if (ibv_query_gid(ctx, port, i, &g))
			continue;
		if (memcmp(&g, &(union ibv_gid){0}, sizeof(g)) == 0)
			continue;
		*gid = g;
		*sgid_idx = i;
		return 0;
	}
	fprintf(stderr,
		"k6: no usable GID found on port %u "
		"(scanned %d entries; check the netdev's IP / GID table)\n",
		port, n);
	return -1;
}

/* RESET -> INIT. Same set as the original --post-recv-wrs path. */
static int qp_to_init(struct ibv_qp *qp, uint8_t port)
{
	struct ibv_qp_attr attr = {
		.qp_state        = IBV_QPS_INIT,
		.pkey_index      = 0,
		.port_num        = port,
		.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
				   IBV_ACCESS_REMOTE_WRITE |
				   IBV_ACCESS_REMOTE_READ,
	};
	int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX |
		   IBV_QP_PORT  | IBV_QP_ACCESS_FLAGS;
	int err = ibv_modify_qp(qp, &attr, mask);
	if (err) {
		fprintf(stderr, "k6: ibv_modify_qp(INIT) failed: %s\n",
			strerror(err));
		return -1;
	}
	return 0;
}

/*
 * INIT -> RTR with self-loopback AV. dest_qpn is the QP's own qpn,
 * dgid is a local GID from the port's GID table. RoCE link layer
 * sets is_global=1 / dlid=0; IB sets is_global=0 (or 1 if you want
 * to test GRH path) / dlid=lid-of-port. We ALWAYS set is_global=1
 * with the local GID at sgid_index, which works on RoCE (no LID)
 * and on IB (the GRH carries the GID, kernel resolves through SM).
 *
 * The QPC fields written here are exactly the §5.3.5 RTR-set group
 * we want to validate for byte-equality across SAVE/LOAD:
 *   path_mtu, min_rnr_nak, log_rra_max, primary_address_path
 *   (dgid/sgid_idx/sl/port/dmac/hop_limit/tclass/...), remote_qpn,
 *   rq_psn -> next_rcv_psn.
 */
static int qp_to_rtr(struct ibv_qp *qp, struct ibv_context *ctx,
		     uint8_t port, uint32_t dest_qpn,
		     const union ibv_gid *dgid, int sgid_idx)
{
	struct ibv_qp_attr attr = {
		.qp_state           = IBV_QPS_RTR,
		.path_mtu           = IBV_MTU_1024,
		.dest_qp_num        = dest_qpn,
		.rq_psn             = 0,
		.max_dest_rd_atomic = 1,
		.min_rnr_timer      = 12,
	};
	int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
		   IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
		   IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
	int err;

	(void)ctx;

	attr.ah_attr.is_global       = 1;
	attr.ah_attr.dlid            = 0;
	attr.ah_attr.sl              = 0;
	attr.ah_attr.src_path_bits   = 0;
	attr.ah_attr.port_num        = port;
	attr.ah_attr.grh.dgid        = *dgid;
	attr.ah_attr.grh.sgid_index  = (uint8_t)sgid_idx;
	attr.ah_attr.grh.hop_limit   = 1;
	attr.ah_attr.grh.traffic_class = 0;
	attr.ah_attr.grh.flow_label  = 0;

	err = ibv_modify_qp(qp, &attr, mask);
	if (err) {
		fprintf(stderr, "k6: ibv_modify_qp(RTR) failed: %s\n",
			strerror(err));
		return -1;
	}
	return 0;
}

/* RTR -> RTS. Stamps log_sra_max / retry_count / rnr_retry into the
 * QPC. We never post send WRs; the QP just sits in RTS with all the
 * RTR/RTS-set fields populated, ready for QUERY_QP to read them. */
static int qp_to_rts(struct ibv_qp *qp)
{
	struct ibv_qp_attr attr = {
		.qp_state      = IBV_QPS_RTS,
		.timeout       = 14,
		.retry_cnt     = 7,
		.rnr_retry     = 7,
		.sq_psn        = 0,
		.max_rd_atomic = 1,
	};
	int mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
		   IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
		   IBV_QP_MAX_QP_RD_ATOMIC;
	int err = ibv_modify_qp(qp, &attr, mask);
	if (err) {
		fprintf(stderr, "k6: ibv_modify_qp(RTS) failed: %s\n",
			strerror(err));
		return -1;
	}
	return 0;
}

struct qp_context_emit {
	uint32_t sq_wqe_count;
	uint32_t rq_wqe_count;
	uint32_t rq_wqe_shift;	/* ilog2(rq.stride); 0 for empty RQ */
	uint64_t buf_addr;	/* user VA of WQ-ring umem base
				 * (== mlx5dv_qp.rq.buf because libmlx5
				 * lays out RQ at offset 0 within the
				 * shared WQ buffer; SQ then follows at
				 * rq.wqe_cnt << rq_wqe_shift) */
	uint64_t db_addr;	/* user VA of doorbell record
				 * (mlx5dv_qp.dbrec) -- not page-aligned;
				 * the kernel applies & PAGE_MASK on its
				 * side. Same passthrough rule as the
				 * CQ DBR */
};

/*
 * Capture every QP-create-time userspace-visible field the S6b
 * RESTORE_QP verb body needs from the source-side mlx5_ib_create_qp
 * landing -- the WQ-ring umem base address, the doorbell record VA,
 * and the WQ shape (sq_wqe_count, rq_wqe_count, rq_wqe_shift). The
 * destination kernel needs:
 *
 *   - buf_addr / db_addr to ib_umem_pin from current->mm and rebind
 *     into the LOAD_VHCA_STATE-replayed (KIND_QP, qpn) and
 *     (KIND_DBR, dbrec & PAGE_MASK) placeholders inside
 *     mlx5_ib_restore_qp's ib_umem_pin + mlx5_vfmig_bind_user_qp
 *     calls.
 *   - sq_wqe_count / rq_wqe_count / rq_wqe_shift to recompute
 *     buf_size verbatim and re-populate qp->{sq,rq}.{wqe_cnt,
 *     wqe_shift,offset} so the destination userspace's WQ
 *     producer/consumer indices stay in sync with the kernel's
 *     view of the ring layout.
 *
 * mlx5dv_init_obj(MLX5DV_OBJ_QP) reads these out of the libmlx5-
 * internal struct mlx5_qp -- they are not exposed via stock
 * libibverbs because they are libmlx5-private layout details (only
 * libmlx5's own send/recv path consumes them at runtime). For
 * CRIU R3 they become wire-visible input to RESTORE_QP on the
 * destination, hence this helper.
 *
 * Why rq.buf is the umem base
 * ---------------------------
 * libmlx5 lays out a *single* mmap (struct mlx5_buf) shared by
 * both queues:
 *     qp->rq.offset = 0
 *     qp->sq.offset = qp->rq.wqe_cnt << qp->rq.wqe_shift
 * mlx5dv_init_obj(QP) then sets:
 *     out->rq.buf = qp->buf.buf + qp->rq.offset    // == qp->buf.buf
 *     out->sq.buf = qp->buf.buf + qp->sq.offset
 * so dvqp.rq.buf is the user VA of the shared umem at byte 0,
 * which is exactly what mlx5_ib_create_qp_user passed into
 * mlx5_ib_create_qp_v2.buf_addr at create time. Always-true for
 * RC/UC/UD with rq.wqe_cnt > 0; if a future caller drives a
 * srq-backed QP with rq.wqe_cnt == 0 the same identity still
 * holds because libmlx5 keeps the offset at 0 even for a
 * zero-length RQ.
 */
static int extract_qp_context(struct ibv_qp *qp,
			      struct qp_context_emit *out)
{
	struct mlx5dv_qp dvqp = {};
	struct mlx5dv_obj obj = {
		.qp = { .in = qp, .out = &dvqp },
	};
	int err = mlx5dv_init_obj(&obj, MLX5DV_OBJ_QP);
	if (err) {
		fprintf(stderr, "k6: mlx5dv_init_obj(QP) failed: %d\n", err);
		return -1;
	}
	out->sq_wqe_count = dvqp.sq.wqe_cnt;
	out->rq_wqe_count = dvqp.rq.wqe_cnt;
	/*
	 * rq.stride is the byte size of one RQ WQE (typically 16, 32,
	 * 64). RESTORE_QP's UHW carries the *shift* (the RQ shift the
	 * source's create_qp caller sent into the kernel). For
	 * rq.wqe_cnt == 0 mlx5dv reports stride == 0; emit shift 0
	 * (the kernel ignores rq_wqe_shift when rq is empty -- the
	 * has_rq predicate is false).
	 */
	out->rq_wqe_shift = dvqp.rq.stride ?
			    (uint32_t)__builtin_ctz(dvqp.rq.stride) : 0;
	out->buf_addr = (uint64_t)(uintptr_t)dvqp.rq.buf;
	out->db_addr  = (uint64_t)(uintptr_t)dvqp.dbrec;
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

enum k6_qp_state {
	K6_QPS_RESET = 0,
	K6_QPS_INIT  = 1,
	K6_QPS_RTR   = 2,
	K6_QPS_RTS   = 3,
};

static const char *k6_qp_state_name(enum k6_qp_state s)
{
	switch (s) {
	case K6_QPS_RESET: return "RESET";
	case K6_QPS_INIT:  return "INIT";
	case K6_QPS_RTR:   return "RTR";
	case K6_QPS_RTS:   return "RTS";
	}
	return "?";
}

static int parse_qp_state(const char *s, enum k6_qp_state *out)
{
	if (!strcasecmp(s, "RESET")) { *out = K6_QPS_RESET; return 0; }
	if (!strcasecmp(s, "INIT"))  { *out = K6_QPS_INIT;  return 0; }
	if (!strcasecmp(s, "RTR"))   { *out = K6_QPS_RTR;   return 0; }
	if (!strcasecmp(s, "RTS"))   { *out = K6_QPS_RTS;   return 0; }
	return -1;
}

int main(int argc, char **argv)
{
	const char *ibdev_name;
	int post_recv_wrs = 0;
	enum k6_qp_state target_state = K6_QPS_RESET;
	int pd_only = 0;
	const uint8_t port = 1;
	struct ibv_device *dev;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pd = NULL;
	struct ibv_cq *cq = NULL;
	struct ibv_qp *qp = NULL;
	struct ibv_mr *mr = NULL;
	struct ibv_srq *srq = NULL;
	void *mr_buf = NULL;
	const size_t mr_len = 4096;
	uint32_t pdn = 0, qpn = 0, lkey = 0, rkey = 0, srqn = 0;
	struct cq_context_emit cqc = {};
	struct qp_context_emit qpc = {};
	int rc = 1;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <ibdev> "
			"[--qp-state {RESET|INIT|RTR|RTS}] "
			"[--post-recv-wrs N] "
			"[--pd-only]\n",
			argv[0]);
		return 2;
	}
	ibdev_name = argv[1];
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--post-recv-wrs") && i + 1 < argc) {
			post_recv_wrs = atoi(argv[++i]);
			if (post_recv_wrs < 0)
				post_recv_wrs = 0;
		} else if (!strcmp(argv[i], "--qp-state") && i + 1 < argc) {
			if (parse_qp_state(argv[++i], &target_state)) {
				fprintf(stderr,
					"k6: invalid --qp-state '%s' "
					"(want RESET/INIT/RTR/RTS)\n",
					argv[i]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--pd-only")) {
			/*
			 * §S3b DEALLOC_PD uid-gating probe: skip CQ /
			 * QP / MR / SRQ creation so the PDC has zero
			 * dependents at SAVE time. Lets the destination-
			 * side PROBE_DEALLOC_PD test FW's uid semantics
			 * in isolation, with no "PD has dependents"
			 * confound.
			 */
			pd_only = 1;
		} else {
			fprintf(stderr, "k6: unknown arg '%s'\n", argv[i]);
			return 2;
		}
	}

	/*
	 * --post-recv-wrs N implies the QP must be at least in INIT
	 * (post_recv on RESET is rejected). Auto-promote so callers
	 * can pass --post-recv-wrs N without thinking about the QP-
	 * state plumbing.
	 */
	if (post_recv_wrs > 0 && target_state < K6_QPS_INIT)
		target_state = K6_QPS_INIT;

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

	if (pd_only) {
		/*
		 * Skip CQ / QP / MR / SRQ. The harness still wants
		 * a READY-followed-by-key=value manifest; emit pdn
		 * + sentinels so existing capture loops keep working.
		 */
		printf("ibdev=%s\n", ibdev_name);
		printf("pdn=%u\n", pdn);
		printf("cqn=SKIPPED\n");
		printf("qpn=SKIPPED\n");
		printf("lkey=SKIPPED\n");
		printf("rkey=SKIPPED\n");
		printf("mkey_index=SKIPPED\n");
		printf("srqn=SKIPPED\n");
		printf("qp_state=PD_ONLY\n");
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
		goto out;
	}

	cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!cq) {
		fprintf(stderr, "k6: ibv_create_cq failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (extract_cq_context(cq, &cqc))
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
		if (extract_qp_context(qp, &qpc))
			goto out;
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
	 * §6.3 / S6 piggyback: drive the QP through the IBTA state
	 * machine via SELF-LOOPBACK so the QPC carries the full
	 * INIT/RTR/RTS-set field group when we (the harness) snapshot
	 * it via MLX5_VFMIG_IOC_QUERY_QP. Self-loopback means the
	 * QP's own port-GID becomes both src and dst of the AV; we
	 * never actually transmit anything (no send WRs are posted),
	 * so the netdev's TX-drop policy on tracked VFs is irrelevant.
	 *
	 * Order matters: post_recv must happen AFTER INIT so the RQ
	 * is in a state to accept WRs, and BEFORE the higher-level
	 * transitions so the subsequent QUERY_QP reads them once
	 * RTR/RTS has stamped its own QPC fields.
	 */
	if (target_state >= K6_QPS_INIT) {
		if (qp_to_init(qp, port))
			goto out;
	}

	for (int i = 0; i < post_recv_wrs; i++) {
		struct ibv_sge sge = {
			.addr   = (uintptr_t)mr_buf,
			.length = mr_len,
			.lkey   = lkey,
		};
		struct ibv_recv_wr wr = {
			.wr_id   = 0xCAFE0000u | i,
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

	if (target_state >= K6_QPS_RTR) {
		union ibv_gid local_gid = {};
		int sgid_idx = 0;

		if (find_local_gid(ctx, port, &local_gid, &sgid_idx))
			goto out;
		if (qp_to_rtr(qp, ctx, port, qpn, &local_gid, sgid_idx))
			goto out;
	}

	if (target_state >= K6_QPS_RTS) {
		if (qp_to_rts(qp))
			goto out;
	}

	/* Emit the manifest. Format is intentionally shell-eval-able:
	 * each line of the form "key=value" suitable for `eval src_$line`. */
	printf("ibdev=%s\n", ibdev_name);
	printf("pdn=%u\n", pdn);
	printf("cqn=%u\n", cqc.cqn);
	/*
	 * cqe / cqe_size / cq_buf_addr / cq_db_addr feed the
	 * S5b RESTORE_CQ verb body on the destination side via
	 * struct mlx5_ib_restore_cq_req.{cqn, cqe_size, buf_addr,
	 * db_addr}. The dispatcher's UVERBS_ATTR_RESTORE_CQ_CQE
	 * core attr carries the cqe number. mlx5dv exposes the
	 * libmlx5-internal struct mlx5_cq fields via
	 * mlx5dv_init_obj(MLX5DV_OBJ_CQ); see extract_cq_context().
	 */
	printf("cqe=%u\n", cqc.cqe_cnt);
	printf("cqe_size=%u\n", cqc.cqe_size);
	printf("cq_buf_addr=0x%016llx\n", (unsigned long long)cqc.buf_addr);
	printf("cq_db_addr=0x%016llx\n", (unsigned long long)cqc.db_addr);
	printf("qpn=%u\n", qpn);
	/*
	 * sq_wqe_count / rq_wqe_count / rq_wqe_shift / qp_buf_addr /
	 * qp_db_addr feed the S6b RESTORE_QP verb body on the
	 * destination side via struct mlx5_ib_restore_qp_req. The
	 * dispatcher's UVERBS_ATTR_RESTORE_QP_CAP carries the high-
	 * level cap; the WQ-shape fields below are the libmlx5-
	 * negotiated layout the destination kernel needs to
	 * recompute buf_size and re-populate qp->{sq,rq}.
	 * {wqe_cnt,wqe_shift,offset}. mlx5dv exposes these via
	 * mlx5dv_init_obj(MLX5DV_OBJ_QP); see extract_qp_context().
	 */
	printf("sq_wqe_count=%u\n", qpc.sq_wqe_count);
	printf("rq_wqe_count=%u\n", qpc.rq_wqe_count);
	printf("rq_wqe_shift=%u\n", qpc.rq_wqe_shift);
	printf("qp_buf_addr=0x%016llx\n", (unsigned long long)qpc.buf_addr);
	printf("qp_db_addr=0x%016llx\n", (unsigned long long)qpc.db_addr);
	printf("lkey=0x%08x\n", lkey);
	printf("rkey=0x%08x\n", rkey);
	printf("mkey_index=%u\n", lkey >> 8);
	/*
	 * iova (mlx5 sets mr->iova == mr_buf for non-zero-based MRs)
	 * and length, used by test_mr_adopt.sh to byte-compare against
	 * PROBE_MKEY's fw_start_addr / fw_length output on the
	 * destination. ibv_reg_mr's IOVA equals the user VA when
	 * IBV_ACCESS_ZERO_BASED is not set, which is the default.
	 */
	printf("mr_addr=0x%016llx\n", (unsigned long long)(uintptr_t)mr_buf);
	printf("mr_length=0x%016zx\n", mr_len);
	if (srq)
		printf("srqn=%u\n", srqn);
	else
		printf("srqn=SKIPPED\n");
	printf("recv_wrs_posted=%d\n", post_recv_wrs);
	printf("qp_state=%s\n", k6_qp_state_name(target_state));
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
