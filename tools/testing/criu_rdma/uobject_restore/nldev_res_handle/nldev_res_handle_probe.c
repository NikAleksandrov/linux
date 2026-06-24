// SPDX-License-Identifier: GPL-2.0
/*
 * nldev_res_handle_probe -- empirical validation of the new
 * RDMA_NLDEV_ATTR_RES_HANDLE attribute emitted by every fill_res_*_entry
 * helper that carries a user-created ib_uobject (PD / CQ / QP / MR / SRQ).
 * Pairs with design/uobject_restore.md §7.5.1 (K8a).
 *
 * Motivation
 * ----------
 * NLDEV's per-class dumps key resources by their restrack id
 * (RDMA_NLDEV_ATTR_RES_PDN / CQN / LQPN / MRN / SRQN). The uverbs
 * INFO_HANDLES method enumerates the same set of resources but keys them
 * by their ufile handle (== ib_uobject->id from the owning ucontext).
 * A CRIU dump plugin needs to JOIN these two views to know "the PD whose
 * restrack id is X lives at ufile handle Y in this ucontext, so on
 * restore install the restored PD at handle Y so user code's restored
 * memory still references it correctly". Without RES_HANDLE the only way
 * to perform that join is a per-ucontext-per-class extra dispatch that
 * doesn't yet exist (would require a new INFO_HANDLES variant returning
 * paired restrack ids -- design §7.5.2 calls this K8b).
 *
 * RES_HANDLE makes the join trivial: NLDEV already returns one entry per
 * resource per class, just add `handle = uobj->id` to that entry.
 *
 * What we validate
 * ----------------
 *   (a) For each user-creatable restracked class (PD / CQ / QP / MR /
 *       SRQ), the kernel emits RDMA_NLDEV_ATTR_RES_HANDLE inside the
 *       per-resource ENTRY nested attribute.
 *   (b) The emitted value equals libibverbs's obj->handle for the same
 *       resource (ie the same ufile handle uverbs SHOW operations
 *       return).
 *   (c) Kernel-internal restrack entries (e.g. mlx5_ib_dev_res's
 *       internal PD on a normal PF) do NOT carry the attribute. We do
 *       not actively allocate those, but we observe their absence by
 *       walking every entry returned in the dump and checking that
 *       entries with no RES_PID and/or with RES_KERN_NAME present omit
 *       the new attr.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/nldev_res_handle/nldev_res_handle_probe
 *
 * Usage:
 *   ./nldev_res_handle_probe <ibdev>      # e.g. mlx5_0
 *
 * Exits 0 on PASS, non-zero on FAIL. Each per-class result is printed.
 *
 * Notes
 *   - Uses raw NETLINK_RDMA sockets rather than libnl/libmnl to keep
 *     the binary self-contained alongside info_handles_probe.c.
 *   - SRQ alloc is best-effort on tracked/restored VFs; if it fails the
 *     SRQ subtest is reported as SKIP and the overall verdict reflects
 *     only the classes we could exercise.
 *   - Does not depend on driver-private uapi (no mlx5dv). Runs on rxe
 *     and mlx5 (and anything else with the standard restracked classes).
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <infiniband/verbs.h>

/*
 * Use the in-tree kernel UAPI header so we always pick up the new
 * enumerator (RDMA_NLDEV_ATTR_RES_HANDLE) without depending on what
 * version of rdma-core's headers happens to be installed. Same coupling
 * shape as tools/mlx5_vfmig.c's include of <linux/mlx5_vfmig.h>.
 */
#include "../../../../../include/uapi/rdma/rdma_netlink.h"

/* --- libnl-light replacements -----------------------------------------
 *
 * We open-code the bits of netlink message construction and parsing we
 * need. Keeping the surface tiny so the patch is easy to review and so
 * the test stays buildable in environments without libnl/libmnl.
 */

#define NL_BUFSZ		(64 * 1024)

#ifndef NLA_TYPE_MASK
#define NLA_TYPE_MASK    (~(NLA_F_NESTED | NLA_F_NET_BYTEORDER))
#endif

#define NLA_OK(p, rem)   ((rem) >= (int)sizeof(struct nlattr) &&         \
			  (p)->nla_len >= sizeof(struct nlattr) &&       \
			  (p)->nla_len <= (rem))
#define NLA_NEXT(p, rem) ((rem) -= NLA_ALIGN((p)->nla_len),              \
			  (struct nlattr *)((char *)(p) + NLA_ALIGN((p)->nla_len)))
#define NLA_DATA(p)      ((void *)((char *)(p) + NLA_HDRLEN))
#define NLA_PAYLOAD(p)   ((p)->nla_len - NLA_HDRLEN)

static int nl_open(void)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	int fd;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_RDMA);
	if (fd < 0) {
		perror("socket(NETLINK_RDMA)");
		return -1;
	}
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind(NETLINK_RDMA)");
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * Find a top-level nlattr inside a buffer of length 'len'. Returns NULL
 * if not present.
 */
static struct nlattr *nla_find(void *buf, int len, uint16_t type)
{
	struct nlattr *nla;

	for (nla = buf; NLA_OK(nla, len); nla = NLA_NEXT(nla, len))
		if ((nla->nla_type & NLA_TYPE_MASK) == type)
			return nla;
	return NULL;
}

/*
 * Send a NETLINK_RDMA dump request. All res-class dumps require a
 * RDMA_NLDEV_ATTR_DEV_INDEX attribute in the request (the kernel
 * rejects the dump with -EINVAL otherwise -- nldev.c
 * res_get_common_dumpit). For the generic NLDEV_CMD_GET dump (used to
 * resolve ibdev name -> dev_index in the first place), pass
 * with_devix=false to omit the attr and enumerate all devices.
 *
 * Note: dev_index 0 IS a valid kernel-assigned index (xa_limit_31b
 * starts at 0), so we cannot overload 0 as "omit".
 */
static int nl_send_dump(int fd, uint16_t nlmsg_type, bool with_devix,
			uint32_t dev_index)
{
	struct {
		struct nlmsghdr  hdr;
		struct nlattr    devix_hdr;
		uint32_t         devix_val;
	} req = {0};
	struct iovec iov = { .iov_base = &req };
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct msghdr msg = {
		.msg_name = &sa,
		.msg_namelen = sizeof(sa),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};

	if (with_devix) {
		req.hdr.nlmsg_len  = NLMSG_HDRLEN +
				     NLA_HDRLEN + sizeof(uint32_t);
		req.devix_hdr.nla_len  = NLA_HDRLEN + sizeof(uint32_t);
		req.devix_hdr.nla_type = RDMA_NLDEV_ATTR_DEV_INDEX;
		req.devix_val          = dev_index;
		iov.iov_len = req.hdr.nlmsg_len;
	} else {
		req.hdr.nlmsg_len = NLMSG_HDRLEN;
		iov.iov_len = NLMSG_HDRLEN;
	}
	req.hdr.nlmsg_type  = nlmsg_type;
	req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.hdr.nlmsg_seq   = 1;

	if (sendmsg(fd, &msg, 0) < 0) {
		perror("sendmsg(dump)");
		return -1;
	}
	return 0;
}

/*
 * Walk a generic NLDEV_CMD_GET dump to learn the kernel-assigned
 * device index for an ibdev name. Returns 0 on success.
 */
static int resolve_ibdev_index(const char *ibdev, uint32_t *out)
{
	char buf[NL_BUFSZ];
	bool done = false;
	int fd = nl_open();

	if (fd < 0)
		return -1;
	if (nl_send_dump(fd, RDMA_NL_GET_TYPE(RDMA_NL_NLDEV,
					      RDMA_NLDEV_CMD_GET),
			 false, 0)) {
		close(fd);
		return -1;
	}

	while (!done) {
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		struct nlmsghdr *nh;

		if (n < 0) { perror("recv(NLDEV_CMD_GET dump)"); close(fd); return -1; }
		for (nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, n);
		     nh = NLMSG_NEXT(nh, n)) {
			void *payload;
			int   len;
			struct nlattr *name_attr;
			struct nlattr *idx_attr;

			if (nh->nlmsg_type == NLMSG_DONE) { done = true; break; }
			if (nh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = NLMSG_DATA(nh);
				fprintf(stderr,
					"NLMSG_ERROR resolving %s: %d (%s)\n",
					ibdev, e->error, strerror(-e->error));
				close(fd);
				return -1;
			}
			payload = NLMSG_DATA(nh);
			len     = nh->nlmsg_len - NLMSG_HDRLEN;
			name_attr = nla_find(payload, len,
					     RDMA_NLDEV_ATTR_DEV_NAME);
			idx_attr  = nla_find(payload, len,
					     RDMA_NLDEV_ATTR_DEV_INDEX);
			if (!name_attr || !idx_attr) continue;
			if (strncmp(NLA_DATA(name_attr), ibdev,
				    NLA_PAYLOAD(name_attr)) != 0)
				continue;
			*out = *(uint32_t *)NLA_DATA(idx_attr);
			close(fd);
			return 0;
		}
	}
	close(fd);
	fprintf(stderr, "ibdev %s not found in NLDEV_CMD_GET dump\n", ibdev);
	return -1;
}

struct entry_view {
	bool     has_handle;
	uint32_t handle;
	bool     has_pid;
	uint32_t pid;
	bool     has_restrack_id;
	uint32_t restrack_id;
	bool     has_kern_name;
};

/*
 * Walk one ENTRY nested attr and pull out the bits we care about.
 *
 * restrack_id_attr names the per-class id (RES_PDN / RES_CQN /
 * RES_LQPN / RES_MRN / RES_SRQN); we store it in entry_view.restrack_id
 * so the caller can correlate logs.
 */
static void parse_entry(struct nlattr *entry, uint16_t restrack_id_attr,
			struct entry_view *out)
{
	int rem = NLA_PAYLOAD(entry);
	struct nlattr *nla;

	memset(out, 0, sizeof(*out));
	for (nla = NLA_DATA(entry); NLA_OK(nla, rem); nla = NLA_NEXT(nla, rem)) {
		uint16_t t = nla->nla_type & NLA_TYPE_MASK;

		if (t == RDMA_NLDEV_ATTR_RES_HANDLE && NLA_PAYLOAD(nla) >= 4) {
			out->has_handle = true;
			out->handle = *(uint32_t *)NLA_DATA(nla);
		} else if (t == RDMA_NLDEV_ATTR_RES_PID && NLA_PAYLOAD(nla) >= 4) {
			out->has_pid = true;
			out->pid = *(uint32_t *)NLA_DATA(nla);
		} else if (t == restrack_id_attr && NLA_PAYLOAD(nla) >= 4) {
			out->has_restrack_id = true;
			out->restrack_id = *(uint32_t *)NLA_DATA(nla);
		} else if (t == RDMA_NLDEV_ATTR_RES_KERN_NAME) {
			out->has_kern_name = true;
		}
	}
}

struct walk_ctx {
	const char *ibdev;	/* if non-NULL, filter by RDMA_NLDEV_ATTR_DEV_NAME match */
	uint16_t    table_attr;	/* RDMA_NLDEV_ATTR_RES_PD etc */
	uint16_t    entry_attr;	/* RDMA_NLDEV_ATTR_RES_PD_ENTRY etc */
	uint16_t    restrack_id;/* RES_PDN, RES_CQN, RES_LQPN, RES_MRN, RES_SRQN */

	/* Filter state populated during the walk. */
	uint32_t    self_pid;
	uint32_t    expect_handle;	/* libibverbs's obj->handle */

	/* Accumulated results. */
	int   our_entries;		/* entries whose RES_PID == self_pid */
	int   our_entries_with_handle;	/* of those, how many carry RES_HANDLE */
	bool  saw_matching_handle;	/* RES_HANDLE == expect_handle */
	int   kern_entries_total;	/* sanity: kernel-internal entries seen */
	int   kern_entries_with_handle;	/* MUST stay 0 */
};

/* Walk one received nlmsg, looking for entries inside table_attr. */
static void walk_one_msg(struct nlmsghdr *nh, struct walk_ctx *ctx)
{
	void *payload = NLMSG_DATA(nh);
	int len = nh->nlmsg_len - NLMSG_HDRLEN;
	struct nlattr *dev_name_attr;
	struct nlattr *table_attr;
	struct nlattr *nla;
	int rem;

	/* Optional device-name filter: skip messages whose DEV_NAME doesn't
	 * match the caller's ibdev. Saves us walking every device's tables.
	 */
	if (ctx->ibdev) {
		dev_name_attr = nla_find(payload, len,
					 RDMA_NLDEV_ATTR_DEV_NAME);
		if (!dev_name_attr)
			return;
		if (strncmp(NLA_DATA(dev_name_attr), ctx->ibdev,
			    NLA_PAYLOAD(dev_name_attr)) != 0)
			return;
	}

	table_attr = nla_find(payload, len, ctx->table_attr);
	if (!table_attr)
		return;

	rem = NLA_PAYLOAD(table_attr);
	for (nla = NLA_DATA(table_attr); NLA_OK(nla, rem);
	     nla = NLA_NEXT(nla, rem)) {
		struct entry_view view;

		if ((nla->nla_type & NLA_TYPE_MASK) != ctx->entry_attr)
			continue;

		parse_entry(nla, ctx->restrack_id, &view);

		/* User-created entries from our process. */
		if (view.has_pid && view.pid == ctx->self_pid) {
			ctx->our_entries++;
			if (view.has_handle) {
				ctx->our_entries_with_handle++;
				if (view.handle == ctx->expect_handle)
					ctx->saw_matching_handle = true;
			}
			continue;
		}

		/* Kernel-internal entries: must NOT carry RES_HANDLE. */
		if (view.has_kern_name) {
			ctx->kern_entries_total++;
			if (view.has_handle)
				ctx->kern_entries_with_handle++;
		}
	}
}

/* Read the dump until NLMSG_DONE. */
static int dump_walk(int fd, struct walk_ctx *ctx)
{
	char buf[NL_BUFSZ];
	bool done = false;

	while (!done) {
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		struct nlmsghdr *nh;

		if (n < 0) {
			perror("recv(NETLINK_RDMA)");
			return -1;
		}

		for (nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, n);
		     nh = NLMSG_NEXT(nh, n)) {
			if (nh->nlmsg_type == NLMSG_DONE) {
				done = true;
				break;
			}
			if (nh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = NLMSG_DATA(nh);
				fprintf(stderr, "NLMSG_ERROR %d (%s)\n",
					e->error, strerror(-e->error));
				return -1;
			}
			walk_one_msg(nh, ctx);
		}
	}
	return 0;
}

struct subtest_spec {
	const char *name;
	uint16_t    cmd;	/* RDMA_NLDEV_CMD_RES_*_GET */
	uint16_t    table_attr;
	uint16_t    entry_attr;
	uint16_t    restrack_id_attr;
};

static int run_subtest(const char *ibdev, uint32_t dev_index,
		       struct subtest_spec *spec, uint32_t expect_handle)
{
	struct walk_ctx ctx = {
		.ibdev          = ibdev,
		.table_attr     = spec->table_attr,
		.entry_attr     = spec->entry_attr,
		.restrack_id    = spec->restrack_id_attr,
		.self_pid       = getpid(),
		.expect_handle  = expect_handle,
	};
	int rc;
	int fd = nl_open();

	if (fd < 0)
		return -1;
	if (nl_send_dump(fd, RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, spec->cmd),
			 true, dev_index)) {
		close(fd);
		return -1;
	}
	rc = dump_walk(fd, &ctx);
	close(fd);
	if (rc)
		return -1;

	printf("  %-4s: our_entries=%d with_handle=%d match_expected=%s  "
	       "kern_entries=%d (with_handle=%d, must be 0)\n",
	       spec->name, ctx.our_entries, ctx.our_entries_with_handle,
	       ctx.saw_matching_handle ? "yes" : "NO",
	       ctx.kern_entries_total, ctx.kern_entries_with_handle);

	if (ctx.our_entries == 0) {
		fprintf(stderr, "    FAIL: no %s entries seen for pid %u "
				"-- alloc didn't take?\n",
			spec->name, ctx.self_pid);
		return -1;
	}
	if (ctx.our_entries_with_handle != ctx.our_entries) {
		fprintf(stderr, "    FAIL: %d/%d %s entries missing RES_HANDLE\n",
			ctx.our_entries - ctx.our_entries_with_handle,
			ctx.our_entries, spec->name);
		return -1;
	}
	if (!ctx.saw_matching_handle) {
		fprintf(stderr, "    FAIL: no %s entry had RES_HANDLE == %u "
				"(libibverbs reported)\n",
			spec->name, expect_handle);
		return -1;
	}
	if (ctx.kern_entries_with_handle != 0) {
		fprintf(stderr, "    FAIL: %d kernel-internal %s entries "
				"erroneously carry RES_HANDLE\n",
			ctx.kern_entries_with_handle, spec->name);
		return -1;
	}
	return 0;
}

/* ----------------------------------------------------------------- */
/*  libibverbs resource setup + main                                  */
/* ----------------------------------------------------------------- */

static struct ibv_context *open_ibdev(const char *name)
{
	struct ibv_device **list = ibv_get_device_list(NULL);
	struct ibv_context *ctx = NULL;
	int i;

	if (!list) {
		perror("ibv_get_device_list");
		return NULL;
	}
	for (i = 0; list[i]; i++) {
		if (strcmp(ibv_get_device_name(list[i]), name) == 0) {
			ctx = ibv_open_device(list[i]);
			break;
		}
	}
	ibv_free_device_list(list);
	if (!ctx)
		fprintf(stderr, "ibdev %s not found / open failed\n", name);
	return ctx;
}

int main(int argc, char **argv)
{
	const char *ibdev = argc > 1 ? argv[1] : "mlx5_0";
	struct ibv_context *ctx;
	struct ibv_pd      *pd  = NULL;
	struct ibv_cq      *cq  = NULL;
	struct ibv_qp      *qp  = NULL;
	struct ibv_mr      *mr  = NULL;
	struct ibv_srq     *srq = NULL;
	void *mr_mem = NULL;
	int    fail_count = 0;
	int    skip_count = 0;
	int    rc;
	uint32_t dev_index = 0;

	if (resolve_ibdev_index(ibdev, &dev_index) < 0)
		return 2;

	ctx = open_ibdev(ibdev);
	if (!ctx)
		return 2;

	pd = ibv_alloc_pd(ctx);
	if (!pd) { perror("ibv_alloc_pd"); rc = 2; goto out; }

	cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!cq) { perror("ibv_create_cq"); rc = 2; goto out; }

	{
		struct ibv_qp_init_attr qpa = {
			.send_cq = cq,
			.recv_cq = cq,
			.qp_type = IBV_QPT_RC,
			.cap = { .max_send_wr = 1, .max_recv_wr = 1,
				 .max_send_sge = 1, .max_recv_sge = 1 },
		};
		qp = ibv_create_qp(pd, &qpa);
		if (!qp) { perror("ibv_create_qp"); rc = 2; goto out; }
	}

	mr_mem = aligned_alloc(4096, 4096);
	if (!mr_mem) { perror("aligned_alloc"); rc = 2; goto out; }
	memset(mr_mem, 0, 4096);
	mr = ibv_reg_mr(pd, mr_mem, 4096, IBV_ACCESS_LOCAL_WRITE);
	if (!mr) { perror("ibv_reg_mr"); rc = 2; goto out; }

	{
		struct ibv_srq_init_attr sa = {
			.attr = { .max_wr = 4, .max_sge = 1 },
		};
		errno = 0;
		srq = ibv_create_srq(pd, &sa);
		if (!srq)
			fprintf(stderr,
				"WARN: ibv_create_srq failed (%s) -- SRQ "
				"subtest will be SKIPPED. This is expected on "
				"a restored-VF mlx5 ibdev (see "
				"design/uobject_restore.md §S7).\n",
				strerror(errno));
	}

	printf("== nldev_res_handle_probe on %s (dev_index=%u self pid %u) ==\n",
	       ibdev, dev_index, getpid());
	printf("  libibverbs handles: pd=%u cq=%u qp=%u mr=%u srq=%s\n",
	       pd->handle, cq->handle, qp->handle, mr->handle,
	       srq ? "alloc'd" : "SKIP");

	struct subtest_spec pd_spec = {
		"PD",  RDMA_NLDEV_CMD_RES_PD_GET,
		RDMA_NLDEV_ATTR_RES_PD,  RDMA_NLDEV_ATTR_RES_PD_ENTRY,
		RDMA_NLDEV_ATTR_RES_PDN,
	};
	struct subtest_spec cq_spec = {
		"CQ",  RDMA_NLDEV_CMD_RES_CQ_GET,
		RDMA_NLDEV_ATTR_RES_CQ,  RDMA_NLDEV_ATTR_RES_CQ_ENTRY,
		RDMA_NLDEV_ATTR_RES_CQN,
	};
	struct subtest_spec qp_spec = {
		"QP",  RDMA_NLDEV_CMD_RES_QP_GET,
		RDMA_NLDEV_ATTR_RES_QP,  RDMA_NLDEV_ATTR_RES_QP_ENTRY,
		RDMA_NLDEV_ATTR_RES_LQPN,
	};
	struct subtest_spec mr_spec = {
		"MR",  RDMA_NLDEV_CMD_RES_MR_GET,
		RDMA_NLDEV_ATTR_RES_MR,  RDMA_NLDEV_ATTR_RES_MR_ENTRY,
		RDMA_NLDEV_ATTR_RES_MRN,
	};
	struct subtest_spec srq_spec = {
		"SRQ", RDMA_NLDEV_CMD_RES_SRQ_GET,
		RDMA_NLDEV_ATTR_RES_SRQ, RDMA_NLDEV_ATTR_RES_SRQ_ENTRY,
		RDMA_NLDEV_ATTR_RES_SRQN,
	};

	if (run_subtest(ibdev, dev_index, &pd_spec, pd->handle))  fail_count++;
	if (run_subtest(ibdev, dev_index, &cq_spec, cq->handle))  fail_count++;
	if (run_subtest(ibdev, dev_index, &qp_spec, qp->handle))  fail_count++;
	if (run_subtest(ibdev, dev_index, &mr_spec, mr->handle))  fail_count++;
	if (srq) {
		if (run_subtest(ibdev, dev_index, &srq_spec, srq->handle))
			fail_count++;
	} else {
		printf("  SRQ : SKIP (alloc failed earlier)\n");
		skip_count++;
	}

	printf("== verdict: failures=%d skipped=%d ==\n",
	       fail_count, skip_count);

	rc = fail_count ? 1 : 0;
out:
	if (srq) ibv_destroy_srq(srq);
	if (mr)  ibv_dereg_mr(mr);
	free(mr_mem);
	if (qp)  ibv_destroy_qp(qp);
	if (cq)  ibv_destroy_cq(cq);
	if (pd)  ibv_dealloc_pd(pd);
	ibv_close_device(ctx);
	return rc;
}
