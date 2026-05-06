// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_vfmig_uctx - userspace exerciser for mlx5_ib's VFMIG ucontext
 * vendor verbs. Each subcommand opens /dev/infiniband/uverbs<N>
 * directly, bypassing libibverbs, so we can drive raw flag bits and
 * vendor methods that libmlx5 doesn't expose.
 *
 * Step 1 scope (alloc-with-flag):
 *
 *   alloc_uctx_with_flag <ibdev>
 *     Positive: GET_CONTEXT with MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE.
 *     On success, dump a few resp fields and close. The close path
 *     exercises deallocate_uars()'s INVALID-skip, validating the
 *     abandoned-restore cleanup.
 *
 *   alloc_uctx_neg_dyn_uar <ibdev>
 *     Negative: GET_CONTEXT with VFMIG_RESTORE *and*
 *     MLX5_LIB_CAP_DYN_UAR. v0 rejects this combo because lib_uar_dyn
 *     bypasses the static sys_pages[] table that the restore path
 *     depends on. Expect EOPNOTSUPP.
 *
 *   alloc_uctx_neg_bad_flag <ibdev>
 *     Negative: GET_CONTEXT with an unsupported flag bit (1 << 31).
 *     Catches the ~(DEVX | VFMIG_RESTORE) reject mask. Expect
 *     EOPNOTSUPP. Sanity check that the mask widening to allow
 *     VFMIG_RESTORE didn't accidentally accept anything else.
 *
 *   Enable the kernel-side dbg log if you want to confirm the
 *   skip-allocate-uars path was taken on the positive case:
 *     echo 'file drivers/infiniband/hw/mlx5/main.c +p' | \
 *         sudo tee /proc/dynamic_debug/control
 *   Then watch dmesg for
 *     "vfmig_restore_pending: skipping ... UAR allocations"
 *
 * Future steps will add query_uctx_uar_table, restore_uctx, etc.
 *
 * Build:
 *   cc -O2 -Wall -o mlx5_vfmig_uctx mlx5_vfmig_uctx.c
 *
 * Use:
 *   mlx5_vfmig_uctx alloc_uctx_with_flag mlx5_2
 *
 * The <ibdev> argument is an InfiniBand device name as listed in
 * /sys/class/infiniband, e.g. "mlx5_2".
 *
 * NOTE on ABI inlining: rather than pulling in the kernel UAPI headers
 * (which require a sanitized userspace include tree from
 * `make headers_install`), the small struct and enum surface we need
 * is defined inline below. It is identical to the kernel definitions
 * in include/uapi/rdma/ib_user_verbs.h and include/uapi/rdma/mlx5-abi.h
 * at the time this tool was written; if those evolve, this file must
 * be kept in sync. (The same convention is followed by other helpers
 * in this directory, e.g. mlx5_vfmig.c references the in-tree
 * mlx5_vfmig.h via a relative path; the verbs UAPI is more entangled
 * with rdma-core and easier to inline.)
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ----- inlined kernel UAPI surface ----- */

/* From include/uapi/rdma/ib_user_verbs.h */
enum {
	IB_USER_VERBS_CMD_GET_CONTEXT = 0,
};

struct ib_uverbs_cmd_hdr {
	uint32_t command;
	uint16_t in_words;
	uint16_t out_words;
};

struct ib_uverbs_get_context {
	uint64_t response;
	uint64_t driver_data[];
} __attribute__((packed));

struct ib_uverbs_get_context_resp {
	uint32_t async_fd;
	uint32_t num_comp_vectors;
	uint64_t driver_data[];
} __attribute__((packed));

/* From include/uapi/rdma/mlx5-abi.h */
enum {
	MLX5_LIB_CAP_4K_UAR	= (uint64_t)1 << 0,
	MLX5_LIB_CAP_DYN_UAR	= (uint64_t)1 << 1,
};

enum {
	MLX5_IB_ALLOC_UCTX_DEVX			= 1 << 0,
	MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE	= 1 << 1, /* NEW */
};

struct mlx5_ib_alloc_ucontext_req_v2 {
	uint32_t total_num_bfregs;
	uint32_t num_low_latency_bfregs;
	uint32_t flags;
	uint32_t comp_mask;
	uint8_t  max_cqe_version;
	uint8_t  reserved0;
	uint16_t reserved1;
	uint32_t reserved2;
	uint64_t lib_caps;
} __attribute__((aligned(8)));

struct mlx5_ib_alloc_ucontext_resp {
	uint32_t qp_tab_size;
	uint32_t bf_reg_size;
	uint32_t tot_bfregs;
	uint32_t cache_line_size;
	uint16_t max_sq_desc_sz;
	uint16_t max_rq_desc_sz;
	uint32_t max_send_wqebb;
	uint32_t max_recv_wr;
	uint32_t max_srq_recv_wr;
	uint16_t num_ports;
	uint16_t flow_action_flags;
	uint32_t comp_mask;
	uint32_t response_length;
	uint8_t  cqe_version;
	uint8_t  cmds_supp_uhw;
	uint8_t  eth_min_inline;
	uint8_t  clock_info_versions;
	uint64_t hca_core_clock_offset;
	uint32_t log_uar_size;
	uint32_t num_uars_per_page;
	uint32_t num_dyn_bfregs;
	uint32_t dump_fill_mkey;
} __attribute__((aligned(8)));

/* ----- impl ----- */

/*
 * Resolve an IB device name (e.g. "mlx5_2") to its
 * /dev/infiniband/uverbs<N> character device by enumerating
 * /sys/class/infiniband/<ibdev>/device/infiniband_verbs/, which
 * contains a single uverbs<N> entry.
 */
static int open_uverbs_for_ibdev(const char *ibdev)
{
	char path[256];
	char node[256] = {};
	DIR *d;
	struct dirent *de;
	int fd;

	snprintf(path, sizeof(path),
		 "/sys/class/infiniband/%s/device/infiniband_verbs", ibdev);
	d = opendir(path);
	if (!d) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}

	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "uverbs", 6) == 0) {
			snprintf(node, sizeof(node), "%s", de->d_name);
			break;
		}
	}
	closedir(d);

	if (!node[0]) {
		fprintf(stderr, "no uverbs<N> node under %s\n", path);
		return -1;
	}

	snprintf(path, sizeof(path), "/dev/infiniband/%s", node);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}
	return fd;
}

/*
 * Issue IB_USER_VERBS_CMD_GET_CONTEXT via the legacy write() path.
 *
 * Wire layout (single write):
 *   [ib_uverbs_cmd_hdr]
 *   [ib_uverbs_get_context.response = (uintptr_t)&resp_blob]
 *   [mlx5_ib_alloc_ucontext_req_v2 driver_data]
 *
 * Response (kernel writes back to *response):
 *   [ib_uverbs_get_context_resp]
 *   [mlx5_ib_alloc_ucontext_resp]
 *
 * We don't need the async fd for this test.
 */
/*
 * Wire the kernel expects: a single contiguous header + req + driver_data,
 * followed by the kernel writing back resp + driver_data into *response.
 * The members are already self-aligned (cmd_hdr is 8 bytes, response field
 * is 8-byte aligned, req struct begins on an 8-byte boundary), so no
 * explicit packing is needed -- and adding it triggers
 * -Wpacked-not-aligned.
 */
struct cmd_blob {
	struct ib_uverbs_cmd_hdr hdr;
	struct ib_uverbs_get_context get_ctx;
	struct mlx5_ib_alloc_ucontext_req_v2 req;
};

struct resp_blob {
	struct ib_uverbs_get_context_resp resp;
	struct mlx5_ib_alloc_ucontext_resp drv;
};

/*
 * Build and send a GET_CONTEXT command. Returns:
 *   >= 0  on kernel success (cmd fully written)
 *   < 0   on kernel rejection (negated errno)
 *
 * Caller owns the open fd.
 */
static int send_get_context(int fd, uint32_t flags, uint64_t lib_caps,
			    struct resp_blob *resp_out)
{
	struct cmd_blob cmd = {};
	ssize_t n;

	cmd.hdr.command   = IB_USER_VERBS_CMD_GET_CONTEXT;
	cmd.hdr.in_words  = sizeof(cmd) / 4;
	cmd.hdr.out_words = sizeof(*resp_out) / 4;

	cmd.get_ctx.response = (uintptr_t)resp_out;

	cmd.req.total_num_bfregs = 8;
	cmd.req.num_low_latency_bfregs = 0;
	cmd.req.flags = flags;
	cmd.req.max_cqe_version = 1;
	cmd.req.lib_caps = lib_caps;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if ((size_t)n != sizeof(cmd))
		return -EIO;
	return 0;
}

static int do_alloc_uctx_with_flag(const char *ibdev)
{
	struct resp_blob resp = {};
	int fd, rc;

	fd = open_uverbs_for_ibdev(ibdev);
	if (fd < 0)
		return 1;

	rc = send_get_context(fd, MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
			      MLX5_LIB_CAP_4K_UAR, &resp);
	if (rc < 0) {
		fprintf(stderr,
			"GET_CONTEXT(VFMIG_RESTORE) failed: %s\n",
			strerror(-rc));
		close(fd);
		return 1;
	}

	printf("alloc_uctx_with_flag %s: PASS\n", ibdev);
	printf("  qp_tab_size       = %u\n", resp.drv.qp_tab_size);
	printf("  bf_reg_size       = %u\n", resp.drv.bf_reg_size);
	printf("  tot_bfregs        = %u\n", resp.drv.tot_bfregs);
	printf("  num_dyn_bfregs    = %u\n", resp.drv.num_dyn_bfregs);
	printf("  log_uar_size      = %u\n", resp.drv.log_uar_size);
	printf("  num_uars_per_page = %u\n", resp.drv.num_uars_per_page);
	printf("  cqe_version       = %u\n", resp.drv.cqe_version);
	printf("  num_ports         = %u\n", resp.drv.num_ports);
	printf("\n");
	printf("sys_pages[] is now sentinel-filled (MLX5_IB_INVALID_UAR_INDEX);\n");
	printf("RESTORE_UCONTEXT not yet implemented. Closing fd exercises\n");
	printf("deallocate_uars() abandoned-restore cleanup.\n");

	close(fd);
	return 0;
}

/*
 * Negative: VFMIG_RESTORE + lib_uar_dyn must be rejected with EOPNOTSUPP.
 * The static sys_pages[] table that the restore path depends on doesn't
 * exist when lib_uar_dyn=true; UARs go through MLX5_IB_OBJECT_UAR uobjects
 * with their own restore story (out of v0 scope).
 */
static int do_alloc_uctx_neg_dyn_uar(const char *ibdev)
{
	struct resp_blob resp = {};
	int fd, rc;

	fd = open_uverbs_for_ibdev(ibdev);
	if (fd < 0)
		return 1;

	rc = send_get_context(fd,
			      MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
			      MLX5_LIB_CAP_4K_UAR | MLX5_LIB_CAP_DYN_UAR,
			      &resp);
	close(fd);

	if (rc == -EOPNOTSUPP) {
		printf("alloc_uctx_neg_dyn_uar %s: PASS (rejected with EOPNOTSUPP)\n",
		       ibdev);
		return 0;
	}
	if (rc == 0) {
		fprintf(stderr,
			"alloc_uctx_neg_dyn_uar %s: FAIL (kernel ACCEPTED VFMIG_RESTORE | DYN_UAR; should reject)\n",
			ibdev);
		return 1;
	}
	fprintf(stderr,
		"alloc_uctx_neg_dyn_uar %s: FAIL (got %s, expected EOPNOTSUPP)\n",
		ibdev, strerror(-rc));
	return 1;
}

/*
 * Negative: an unsupported flag bit (1 << 31) must be rejected with
 * EOPNOTSUPP. Sanity check on the
 * "req.flags & ~(DEVX | VFMIG_RESTORE)" reject mask -- specifically
 * that widening it to admit VFMIG_RESTORE didn't accidentally let
 * other bits through.
 */
static int do_alloc_uctx_neg_bad_flag(const char *ibdev)
{
	struct resp_blob resp = {};
	int fd, rc;

	fd = open_uverbs_for_ibdev(ibdev);
	if (fd < 0)
		return 1;

	rc = send_get_context(fd, 1u << 31, MLX5_LIB_CAP_4K_UAR, &resp);
	close(fd);

	if (rc == -EOPNOTSUPP) {
		printf("alloc_uctx_neg_bad_flag %s: PASS (rejected with EOPNOTSUPP)\n",
		       ibdev);
		return 0;
	}
	if (rc == 0) {
		fprintf(stderr,
			"alloc_uctx_neg_bad_flag %s: FAIL (kernel ACCEPTED 0x80000000 in flags; reject mask broken)\n",
			ibdev);
		return 1;
	}
	fprintf(stderr,
		"alloc_uctx_neg_bad_flag %s: FAIL (got %s, expected EOPNOTSUPP)\n",
		ibdev, strerror(-rc));
	return 1;
}

static int verb_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char ca = (*a == '-') ? '_' : *a;
		char cb = (*b == '-') ? '_' : *b;

		if (ca != cb)
			return 0;
	}
	return *a == 0 && *b == 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <verb> [args]\n"
		"  alloc_uctx_with_flag <ibdev>     (positive: alloc with VFMIG_RESTORE)\n"
		"  alloc_uctx_neg_dyn_uar <ibdev>   (negative: VFMIG_RESTORE | DYN_UAR -> EOPNOTSUPP)\n"
		"  alloc_uctx_neg_bad_flag <ibdev>  (negative: bogus flag bit -> EOPNOTSUPP)\n"
		"\n"
		"<ibdev> is the IB device name as listed in /sys/class/infiniband\n"
		"(e.g. mlx5_2). Verbs accept '-' or '_' interchangeably.\n",
		argv0);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	if (argc == 3 && verb_eq(argv[1], "alloc_uctx_with_flag"))
		return do_alloc_uctx_with_flag(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "alloc_uctx_neg_dyn_uar"))
		return do_alloc_uctx_neg_dyn_uar(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "alloc_uctx_neg_bad_flag"))
		return do_alloc_uctx_neg_bad_flag(argv[2]);

	usage(argv[0]);
	return 1;
}
