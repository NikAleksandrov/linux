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
 * Step 2 scope (QUERY verb):
 *
 *   query_uctx_uar_table <ibdev>
 *     Open a *normal* ucontext (no VFMIG_RESTORE flag, libmlx5-style
 *     bfreg counts). Issue the new
 *     MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT verb twice -- first to learn
 *     sizes via META, then again to snapshot UAR_TABLE + BFREG_COUNT.
 *     Dump the snapshot. Cross-check uar_table[0..num_static-1] against
 *     mlx5_ib_dbg's "allocated uar N" log lines from allocate_uars()
 *     (enable dynamic_debug per the comment above).
 *
 * Future steps will add roundtrip_uctx (open A normal, QUERY, open B
 * with flag, RESTORE, QUERY, close).
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
#include <sys/ioctl.h>
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

/* From include/uapi/rdma/rdma_user_ioctl_cmds.h */
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

/* From include/uapi/rdma/ib_user_ioctl_verbs.h */
enum {
	RDMA_DRIVER_MLX5 = 1,
};

/*
 * From include/uapi/rdma/mlx5_user_ioctl_cmds.h, post-this-series.
 * Keep in sync with that header.
 *
 * UVERBS_ID_NS_SHIFT is defined in include/uapi/rdma/ib_user_ioctl_cmds.h
 * and used as the namespace bit for driver-defined object/method/attr ids:
 * driver ids have bit (1U << UVERBS_ID_NS_SHIFT) set so the kernel's
 * uapi_key_obj() / uapi_key_ioctl_method() can route them through the
 * driver-namespace branch. It is 12 (not 10), which matters here -- if we
 * disagree with the kernel by even one bit, hdr.object_id misses
 * UVERBS_API_NS_FLAG, the radix lookup falls into the "core" branch and
 * returns UVERBS_API_KEY_ERR, and the ioctl fails with EPROTONOSUPPORT.
 */
#define UVERBS_ID_NS_SHIFT 12
/*
 * Position in enum mlx5_ib_objects (mlx5_user_ioctl_cmds.h):
 *   MLX5_IB_OBJECT_DEVX = (1u << UVERBS_ID_NS_SHIFT);
 *   ... 9 entries between DEVX and VFMIG ...
 *   MLX5_IB_OBJECT_VFMIG = DEVX + 10.
 * Update if entries are inserted before VFMIG in the kernel enum.
 */
#define MLX5_IB_OBJECT_VFMIG				((1u << UVERBS_ID_NS_SHIFT) + 10)
#define MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT		(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT		((1u << UVERBS_ID_NS_SHIFT) + 1)
#define MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE	(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT	((1u << UVERBS_ID_NS_SHIFT) + 1)
#define MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_META		((1u << UVERBS_ID_NS_SHIFT) + 2)
#define MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_UAR_TABLE	(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_BFREG_COUNT	((1u << UVERBS_ID_NS_SHIFT) + 1)
#define MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_META	((1u << UVERBS_ID_NS_SHIFT) + 2)
#define MLX5_IB_INVALID_UAR_INDEX			(1u << 31)

struct mlx5_ib_vfmig_ucontext_meta {
	uint32_t num_static_sys_pages;
	uint32_t num_sys_pages;
	uint32_t num_dyn_bfregs;
	uint32_t num_low_latency_bfregs;
	uint32_t total_num_bfregs;
	uint32_t reserved0;
	uint64_t lib_caps;
	uint8_t  lib_uar_4k;
	uint8_t  lib_uar_dyn;
	uint8_t  cqe_version;
	uint8_t  reserved1[5];
} __attribute__((aligned(8)));

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
 * Build & issue a VFMIG QUERY_UCONTEXT ioctl. Caller fills uar_table /
 * bfreg_count buffers (or passes NULL+0 to opt out). Returns 0 on
 * success, -errno on failure (negated).
 */
static int issue_vfmig_query(int fd,
			     uint32_t *uar_table, size_t uar_table_n,
			     uint32_t *bfreg_count, size_t bfreg_count_n,
			     struct mlx5_ib_vfmig_ucontext_meta *meta)
{
	struct {
		struct ib_uverbs_ioctl_hdr hdr;
		struct ib_uverbs_attr attrs[3];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id = MLX5_IB_OBJECT_VFMIG;
	cmd.hdr.method_id = MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT;
	cmd.hdr.driver_id = RDMA_DRIVER_MLX5;

	if (uar_table) {
		cmd.attrs[n].attr_id = MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE;
		cmd.attrs[n].len = (uint16_t)(uar_table_n * sizeof(*uar_table));
		cmd.attrs[n].flags = UVERBS_ATTR_F_MANDATORY;
		cmd.attrs[n].data = (uintptr_t)uar_table;
		n++;
	}
	if (bfreg_count) {
		cmd.attrs[n].attr_id = MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT;
		cmd.attrs[n].len = (uint16_t)(bfreg_count_n * sizeof(*bfreg_count));
		cmd.attrs[n].flags = UVERBS_ATTR_F_MANDATORY;
		cmd.attrs[n].data = (uintptr_t)bfreg_count;
		n++;
	}

	cmd.attrs[n].attr_id = MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_META;
	cmd.attrs[n].len = sizeof(*meta);
	cmd.attrs[n].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data = (uintptr_t)meta;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

static int do_query_uctx_uar_table(const char *ibdev)
{
	struct mlx5_ib_vfmig_ucontext_meta meta = {};
	struct resp_blob alloc_resp = {};
	uint32_t *uar_table = NULL;
	uint32_t *bfreg_count = NULL;
	int fd, rc;

	fd = open_uverbs_for_ibdev(ibdev);
	if (fd < 0)
		return 1;

	/* Open a normal ucontext (no VFMIG_RESTORE flag). */
	rc = send_get_context(fd, 0, MLX5_LIB_CAP_4K_UAR, &alloc_resp);
	if (rc < 0) {
		fprintf(stderr,
			"GET_CONTEXT (normal) failed: %s\n", strerror(-rc));
		close(fd);
		return 1;
	}

	/* Pass 1: META only -- learn array sizes. */
	rc = issue_vfmig_query(fd, NULL, 0, NULL, 0, &meta);
	if (rc < 0) {
		fprintf(stderr,
			"QUERY_UCONTEXT (META-only) failed: %s\n",
			strerror(-rc));
		close(fd);
		return 1;
	}

	printf("query_uctx_uar_table %s (pass 1, META only):\n", ibdev);
	printf("  num_static_sys_pages   = %u\n", meta.num_static_sys_pages);
	printf("  num_sys_pages          = %u\n", meta.num_sys_pages);
	printf("  num_dyn_bfregs         = %u\n", meta.num_dyn_bfregs);
	printf("  num_low_latency_bfregs = %u\n", meta.num_low_latency_bfregs);
	printf("  total_num_bfregs       = %u\n", meta.total_num_bfregs);
	printf("  lib_caps               = 0x%llx\n",
	       (unsigned long long)meta.lib_caps);
	printf("  lib_uar_4k             = %u\n", meta.lib_uar_4k);
	printf("  lib_uar_dyn            = %u\n", meta.lib_uar_dyn);
	printf("  cqe_version            = %u\n", meta.cqe_version);

	uar_table = calloc(meta.num_sys_pages, sizeof(*uar_table));
	bfreg_count = calloc(meta.total_num_bfregs, sizeof(*bfreg_count));
	if (!uar_table || !bfreg_count) {
		fprintf(stderr, "calloc failed\n");
		rc = 1;
		goto out;
	}

	/* Pass 2: full snapshot. */
	rc = issue_vfmig_query(fd, uar_table, meta.num_sys_pages,
			       bfreg_count, meta.total_num_bfregs, &meta);
	if (rc < 0) {
		fprintf(stderr,
			"QUERY_UCONTEXT (full) failed: %s\n", strerror(-rc));
		rc = 1;
		goto out;
	}

	printf("\npass 2: UAR_TABLE[0..%u-1] (FW UAR ids; 0x80000000 = INVALID):\n",
	       meta.num_sys_pages);
	for (uint32_t i = 0; i < meta.num_sys_pages; i++) {
		const char *tag = (i < meta.num_static_sys_pages) ? "static"
								  : "dyn   ";
		const char *invalid =
			(uar_table[i] == MLX5_IB_INVALID_UAR_INDEX)
			? " (INVALID)" : "";
		printf("  [%4u] %s  uar_id=0x%08x%s\n",
		       i, tag, uar_table[i], invalid);
		/* Truncate dump to first 16 entries to keep output sane. */
		if (i == 15 && meta.num_sys_pages > 16) {
			printf("  ... (%u more entries elided)\n",
			       meta.num_sys_pages - 16);
			break;
		}
	}

	printf("\nBFREG_COUNT[0..%u-1] non-zero entries:\n",
	       meta.total_num_bfregs);
	{
		unsigned int nz = 0;

		for (uint32_t i = 0; i < meta.total_num_bfregs; i++) {
			if (bfreg_count[i]) {
				printf("  [%4u] count=%u\n", i, bfreg_count[i]);
				nz++;
			}
		}
		if (!nz)
			printf("  (none -- expected on a fresh ucontext "
			       "with no QPs and no mmap(ALLOC_WC))\n");
	}

	rc = 0;
	printf("\nquery_uctx_uar_table %s: PASS\n", ibdev);

out:
	free(uar_table);
	free(bfreg_count);
	close(fd);
	return rc;
}

/*
 * Build & issue a VFMIG RESTORE_UCONTEXT ioctl. Caller fills uar_table /
 * bfreg_count buffers (uar_table is mandatory; bfreg_count is optional,
 * pass NULL+0 to omit) and the meta blob. Returns 0 on success, -errno
 * on failure (negated).
 */
static int issue_vfmig_restore(int fd,
			       const uint32_t *uar_table, size_t uar_table_n,
			       const uint32_t *bfreg_count,
			       size_t bfreg_count_n,
			       const struct mlx5_ib_vfmig_ucontext_meta *meta)
{
	struct {
		struct ib_uverbs_ioctl_hdr hdr;
		struct ib_uverbs_attr attrs[3];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id = MLX5_IB_OBJECT_VFMIG;
	cmd.hdr.method_id = MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT;
	cmd.hdr.driver_id = RDMA_DRIVER_MLX5;

	cmd.attrs[n].attr_id = MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_UAR_TABLE;
	cmd.attrs[n].len = (uint16_t)(uar_table_n * sizeof(*uar_table));
	cmd.attrs[n].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data = (uintptr_t)uar_table;
	n++;

	if (bfreg_count) {
		cmd.attrs[n].attr_id =
			MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_BFREG_COUNT;
		cmd.attrs[n].len =
			(uint16_t)(bfreg_count_n * sizeof(*bfreg_count));
		cmd.attrs[n].flags = UVERBS_ATTR_F_MANDATORY;
		cmd.attrs[n].data = (uintptr_t)bfreg_count;
		n++;
	}

	cmd.attrs[n].attr_id = MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_META;
	cmd.attrs[n].len = sizeof(*meta);
	cmd.attrs[n].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data = (uintptr_t)meta;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

/*
 * Snapshot a fully-populated ucontext via a two-pass QUERY. Caller owns
 * the returned buffers (free with free()) and must consult *meta_out
 * for their lengths. On failure, leaves *uar_table_out and
 * *bfreg_count_out NULL and returns -errno (negated).
 */
static int snapshot_ucontext(int fd, struct mlx5_ib_vfmig_ucontext_meta *meta_out,
			     uint32_t **uar_table_out,
			     uint32_t **bfreg_count_out)
{
	struct mlx5_ib_vfmig_ucontext_meta meta = {};
	uint32_t *uar = NULL;
	uint32_t *cnt = NULL;
	int rc;

	*uar_table_out = NULL;
	*bfreg_count_out = NULL;

	rc = issue_vfmig_query(fd, NULL, 0, NULL, 0, &meta);
	if (rc < 0)
		return rc;

	uar = calloc(meta.num_sys_pages, sizeof(*uar));
	cnt = calloc(meta.total_num_bfregs, sizeof(*cnt));
	if (!uar || !cnt) {
		free(uar);
		free(cnt);
		return -ENOMEM;
	}

	rc = issue_vfmig_query(fd, uar, meta.num_sys_pages,
			       cnt, meta.total_num_bfregs, &meta);
	if (rc < 0) {
		free(uar);
		free(cnt);
		return rc;
	}

	*meta_out = meta;
	*uar_table_out = uar;
	*bfreg_count_out = cnt;
	return 0;
}

/*
 * roundtrip_uctx <ibdev>:
 *   1. Open A normal -> QUERY A -> snapshot.
 *   2. Open B with VFMIG_RESTORE flag -> QUERY B; assert all
 *      sys_pages[] are INVALID and all count[] are 0 (proves
 *      step-1 alloc-skip is in effect).
 *   3. RESTORE B with A's snapshot -> assert success.
 *   4. QUERY B again -> assert it matches A bitwise on
 *      sys_pages[], count[], and meta.
 *   5. RESTORE B again -> assert -EINVAL (vfmig_restore_pending
 *      was cleared by step 3).
 *   6. Close both fds clean.
 */
static int do_roundtrip_uctx(const char *ibdev)
{
	struct mlx5_ib_vfmig_ucontext_meta meta_a = {}, meta_b1 = {}, meta_b2 = {};
	struct resp_blob alloc_a = {}, alloc_b = {};
	uint32_t *uar_a = NULL, *uar_b1 = NULL, *uar_b2 = NULL;
	uint32_t *cnt_a = NULL, *cnt_b1 = NULL, *cnt_b2 = NULL;
	int fda = -1, fdb = -1, rc, ret = 1;
	uint32_t i, mismatches;

	fda = open_uverbs_for_ibdev(ibdev);
	fdb = open_uverbs_for_ibdev(ibdev);
	if (fda < 0 || fdb < 0)
		goto out;

	rc = send_get_context(fda, 0, MLX5_LIB_CAP_4K_UAR, &alloc_a);
	if (rc < 0) {
		fprintf(stderr, "open A (normal): %s\n", strerror(-rc));
		goto out;
	}
	rc = send_get_context(fdb, MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
			      MLX5_LIB_CAP_4K_UAR, &alloc_b);
	if (rc < 0) {
		fprintf(stderr, "open B (VFMIG_RESTORE): %s\n", strerror(-rc));
		goto out;
	}

	rc = snapshot_ucontext(fda, &meta_a, &uar_a, &cnt_a);
	if (rc < 0) {
		fprintf(stderr, "QUERY A: %s\n", strerror(-rc));
		goto out;
	}
	printf("step 1: QUERY A: num_sys_pages=%u num_static=%u total_bfregs=%u "
	       "lib_caps=0x%llx 4k=%u dyn=%u cqe_ver=%u\n",
	       meta_a.num_sys_pages, meta_a.num_static_sys_pages,
	       meta_a.total_num_bfregs, (unsigned long long)meta_a.lib_caps,
	       meta_a.lib_uar_4k, meta_a.lib_uar_dyn, meta_a.cqe_version);

	rc = snapshot_ucontext(fdb, &meta_b1, &uar_b1, &cnt_b1);
	if (rc < 0) {
		fprintf(stderr, "QUERY B (pre-RESTORE): %s\n", strerror(-rc));
		goto out;
	}
	mismatches = 0;
	for (i = 0; i < meta_b1.num_sys_pages; i++)
		if (uar_b1[i] != MLX5_IB_INVALID_UAR_INDEX)
			mismatches++;
	for (i = 0; i < meta_b1.total_num_bfregs; i++)
		if (cnt_b1[i] != 0)
			mismatches++;
	if (mismatches) {
		fprintf(stderr,
			"step 2 FAIL: B pre-RESTORE has %u non-sentinel/non-zero entries (alloc-skip didn't take effect)\n",
			mismatches);
		goto out;
	}
	printf("step 2: QUERY B (pre-RESTORE): all %u sys_pages sentinel-INVALID, all %u count entries zero -- alloc-skip OK\n",
	       meta_b1.num_sys_pages, meta_b1.total_num_bfregs);

	if (memcmp(&meta_a, &meta_b1, sizeof(meta_a)) != 0) {
		fprintf(stderr,
			"step 2 FAIL: META(A) != META(B); aborting before strict RESTORE rejects us\n");
		goto out;
	}

	rc = issue_vfmig_restore(fdb, uar_a, meta_a.num_sys_pages,
				 cnt_a, meta_a.total_num_bfregs, &meta_a);
	if (rc < 0) {
		fprintf(stderr, "step 3 FAIL: RESTORE B: %s\n", strerror(-rc));
		goto out;
	}
	printf("step 3: RESTORE B: PASS\n");

	rc = snapshot_ucontext(fdb, &meta_b2, &uar_b2, &cnt_b2);
	if (rc < 0) {
		fprintf(stderr, "step 4 FAIL: QUERY B (post-RESTORE): %s\n",
			strerror(-rc));
		goto out;
	}
	if (memcmp(&meta_a, &meta_b2, sizeof(meta_a)) != 0) {
		fprintf(stderr,
			"step 4 FAIL: META(A) != META(B post-RESTORE)\n");
		goto out;
	}
	if (memcmp(uar_a, uar_b2,
		   meta_a.num_sys_pages * sizeof(*uar_a)) != 0) {
		fprintf(stderr,
			"step 4 FAIL: UAR_TABLE(A) != UAR_TABLE(B post-RESTORE)\n");
		goto out;
	}
	if (memcmp(cnt_a, cnt_b2,
		   meta_a.total_num_bfregs * sizeof(*cnt_a)) != 0) {
		fprintf(stderr,
			"step 4 FAIL: BFREG_COUNT(A) != BFREG_COUNT(B post-RESTORE)\n");
		goto out;
	}
	printf("step 4: QUERY B (post-RESTORE) bitwise matches A on meta + uar_table + bfreg_count: PASS\n");

	rc = issue_vfmig_restore(fdb, uar_a, meta_a.num_sys_pages,
				 cnt_a, meta_a.total_num_bfregs, &meta_a);
	if (rc != -EINVAL) {
		fprintf(stderr,
			"step 5 FAIL: second RESTORE returned %s, expected EINVAL\n",
			rc == 0 ? "0 (success)" : strerror(-rc));
		goto out;
	}
	printf("step 5: second RESTORE rejected with EINVAL (vfmig_restore_pending cleared): PASS\n");

	printf("\nroundtrip_uctx %s: PASS\n", ibdev);
	ret = 0;
out:
	free(uar_a);
	free(cnt_a);
	free(uar_b1);
	free(cnt_b1);
	free(uar_b2);
	free(cnt_b2);
	if (fda >= 0)
		close(fda);
	if (fdb >= 0)
		close(fdb);
	return ret;
}

/*
 * Negative: RESTORE on a B that was opened *without* VFMIG_RESTORE
 * must be rejected with EINVAL. Catches accidental RESTORE on a
 * normal ucontext (whose sys_pages[] holds live FW UAR ids).
 */
static int do_roundtrip_uctx_neg_no_flag(const char *ibdev)
{
	struct mlx5_ib_vfmig_ucontext_meta meta_a = {};
	struct resp_blob alloc_a = {}, alloc_b = {};
	uint32_t *uar_a = NULL, *cnt_a = NULL;
	int fda = -1, fdb = -1, rc, ret = 1;

	fda = open_uverbs_for_ibdev(ibdev);
	fdb = open_uverbs_for_ibdev(ibdev);
	if (fda < 0 || fdb < 0)
		goto out;

	rc = send_get_context(fda, 0, MLX5_LIB_CAP_4K_UAR, &alloc_a);
	if (rc < 0) {
		fprintf(stderr, "open A: %s\n", strerror(-rc));
		goto out;
	}
	rc = send_get_context(fdb, 0, MLX5_LIB_CAP_4K_UAR, &alloc_b);
	if (rc < 0) {
		fprintf(stderr, "open B (normal, NOT VFMIG_RESTORE): %s\n",
			strerror(-rc));
		goto out;
	}
	rc = snapshot_ucontext(fda, &meta_a, &uar_a, &cnt_a);
	if (rc < 0) {
		fprintf(stderr, "QUERY A: %s\n", strerror(-rc));
		goto out;
	}

	rc = issue_vfmig_restore(fdb, uar_a, meta_a.num_sys_pages,
				 cnt_a, meta_a.total_num_bfregs, &meta_a);
	if (rc != -EINVAL) {
		fprintf(stderr,
			"FAIL: RESTORE on non-VFMIG ucontext returned %s, expected EINVAL\n",
			rc == 0 ? "0 (success)" : strerror(-rc));
		goto out;
	}
	printf("roundtrip_uctx_neg_no_flag %s: PASS (rejected with EINVAL)\n",
	       ibdev);
	ret = 0;
out:
	free(uar_a);
	free(cnt_a);
	if (fda >= 0)
		close(fda);
	if (fdb >= 0)
		close(fdb);
	return ret;
}

/*
 * Negative: RESTORE with a META whose total_num_bfregs has been
 * tampered with must be rejected with EINVAL. Picks the field most
 * likely to drift in practice; precondition #3 is a strict
 * field-by-field check so any tweaked field would do.
 */
static int do_roundtrip_uctx_neg_meta_mismatch(const char *ibdev)
{
	struct mlx5_ib_vfmig_ucontext_meta meta_a = {}, meta_bad = {};
	struct resp_blob alloc_a = {}, alloc_b = {};
	uint32_t *uar_a = NULL, *cnt_a = NULL;
	int fda = -1, fdb = -1, rc, ret = 1;

	fda = open_uverbs_for_ibdev(ibdev);
	fdb = open_uverbs_for_ibdev(ibdev);
	if (fda < 0 || fdb < 0)
		goto out;

	if ((rc = send_get_context(fda, 0, MLX5_LIB_CAP_4K_UAR, &alloc_a)) < 0 ||
	    (rc = send_get_context(fdb, MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
				   MLX5_LIB_CAP_4K_UAR, &alloc_b)) < 0) {
		fprintf(stderr, "GET_CONTEXT: %s\n", strerror(-rc));
		goto out;
	}
	if ((rc = snapshot_ucontext(fda, &meta_a, &uar_a, &cnt_a)) < 0) {
		fprintf(stderr, "QUERY A: %s\n", strerror(-rc));
		goto out;
	}

	meta_bad = meta_a;
	meta_bad.total_num_bfregs = meta_a.total_num_bfregs ^ 0x1;

	rc = issue_vfmig_restore(fdb, uar_a, meta_a.num_sys_pages,
				 cnt_a, meta_a.total_num_bfregs, &meta_bad);
	if (rc != -EINVAL) {
		fprintf(stderr,
			"FAIL: RESTORE with mismatched META returned %s, expected EINVAL\n",
			rc == 0 ? "0 (success)" : strerror(-rc));
		goto out;
	}
	printf("roundtrip_uctx_neg_meta_mismatch %s: PASS (rejected with EINVAL)\n",
	       ibdev);
	ret = 0;
out:
	free(uar_a);
	free(cnt_a);
	if (fda >= 0)
		close(fda);
	if (fdb >= 0)
		close(fdb);
	return ret;
}

/*
 * Negative: RESTORE with a snapshot whose static slot 0 has been set
 * to MLX5_IB_INVALID_UAR_INDEX must be rejected with EINVAL.
 * Precondition #5 -- static slots are structural; an INVALID there
 * means a malformed snapshot.
 */
static int do_roundtrip_uctx_neg_static_invalid(const char *ibdev)
{
	struct mlx5_ib_vfmig_ucontext_meta meta_a = {};
	struct resp_blob alloc_a = {}, alloc_b = {};
	uint32_t *uar_a = NULL, *cnt_a = NULL;
	int fda = -1, fdb = -1, rc, ret = 1;

	fda = open_uverbs_for_ibdev(ibdev);
	fdb = open_uverbs_for_ibdev(ibdev);
	if (fda < 0 || fdb < 0)
		goto out;

	if ((rc = send_get_context(fda, 0, MLX5_LIB_CAP_4K_UAR, &alloc_a)) < 0 ||
	    (rc = send_get_context(fdb, MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
				   MLX5_LIB_CAP_4K_UAR, &alloc_b)) < 0) {
		fprintf(stderr, "GET_CONTEXT: %s\n", strerror(-rc));
		goto out;
	}
	if ((rc = snapshot_ucontext(fda, &meta_a, &uar_a, &cnt_a)) < 0) {
		fprintf(stderr, "QUERY A: %s\n", strerror(-rc));
		goto out;
	}
	if (meta_a.num_static_sys_pages == 0) {
		fprintf(stderr, "no static slots to corrupt; skipping\n");
		ret = 0;
		goto out;
	}

	uar_a[0] = MLX5_IB_INVALID_UAR_INDEX;

	rc = issue_vfmig_restore(fdb, uar_a, meta_a.num_sys_pages,
				 cnt_a, meta_a.total_num_bfregs, &meta_a);
	if (rc != -EINVAL) {
		fprintf(stderr,
			"FAIL: RESTORE with INVALID static slot returned %s, expected EINVAL\n",
			rc == 0 ? "0 (success)" : strerror(-rc));
		goto out;
	}
	printf("roundtrip_uctx_neg_static_invalid %s: PASS (rejected with EINVAL)\n",
	       ibdev);
	ret = 0;
out:
	free(uar_a);
	free(cnt_a);
	if (fda >= 0)
		close(fda);
	if (fdb >= 0)
		close(fdb);
	return ret;
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
		"  alloc_uctx_with_flag <ibdev>            (positive: alloc with VFMIG_RESTORE)\n"
		"  alloc_uctx_neg_dyn_uar <ibdev>          (negative: VFMIG_RESTORE | DYN_UAR -> EOPNOTSUPP)\n"
		"  alloc_uctx_neg_bad_flag <ibdev>         (negative: bogus flag bit -> EOPNOTSUPP)\n"
		"  query_uctx_uar_table <ibdev>            (open normal ucontext, QUERY two-pass)\n"
		"  roundtrip_uctx <ibdev>                  (positive: A normal -> QUERY -> B w/flag -> RESTORE -> QUERY match)\n"
		"  roundtrip_uctx_neg_no_flag <ibdev>      (negative: RESTORE on non-VFMIG ucontext -> EINVAL)\n"
		"  roundtrip_uctx_neg_meta_mismatch <ibdev>(negative: META total_num_bfregs tampered -> EINVAL)\n"
		"  roundtrip_uctx_neg_static_invalid <ibdev>(negative: static slot 0 = INVALID -> EINVAL)\n"
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
	if (argc == 3 && verb_eq(argv[1], "query_uctx_uar_table"))
		return do_query_uctx_uar_table(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "roundtrip_uctx"))
		return do_roundtrip_uctx(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "roundtrip_uctx_neg_no_flag"))
		return do_roundtrip_uctx_neg_no_flag(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "roundtrip_uctx_neg_meta_mismatch"))
		return do_roundtrip_uctx_neg_meta_mismatch(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "roundtrip_uctx_neg_static_invalid"))
		return do_roundtrip_uctx_neg_static_invalid(argv[2]);

	usage(argv[0]);
	return 1;
}
