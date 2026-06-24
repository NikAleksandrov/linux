// SPDX-License-Identifier: GPL-2.0
/*
 * ucontext_vendor_verbs - userspace exerciser for mlx5_ib's VFMIG
 * ucontext vendor verbs. Each subcommand opens /dev/infiniband/uverbs<N>
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
 *   alloc_uctx_dyn_uar <ibdev>
 *     Positive: GET_CONTEXT with VFMIG_RESTORE *and*
 *     MLX5_LIB_CAP_DYN_UAR. The v0 rejection of this combo has been
 *     replaced by MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS, which restores
 *     dyn-UAR uobjects at their preserved handles; the alloc path now
 *     just bypasses the static sys_pages[] init and waits for the
 *     restore verb to seed the MLX5_IB_OBJECT_UAR uobjects.
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
 *   make -C tools/testing/criu_rdma tools/ucontext_vendor_verbs
 *
 * Use:
 *   ucontext_vendor_verbs alloc_uctx_with_flag mlx5_2
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
#include <sys/mman.h>
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
/*
 * Position in enum mlx5_ib_objects (mlx5_user_ioctl_cmds.h):
 *   MLX5_IB_OBJECT_DEVX = (1u << UVERBS_ID_NS_SHIFT);
 *   ... DEVX(0) DEVX_OBJ(1) DEVX_UMEM(2) FLOW_MATCHER(3)
 *       DEVX_ASYNC_CMD_FD(4) DEVX_ASYNC_EVENT_FD(5)
 *       VAR(6) PP(7) UAR(8) STEERING_ANCHOR(9) ...
 *   MLX5_IB_OBJECT_UAR = DEVX + 8.
 * Update if entries are inserted before UAR in the kernel enum.
 */
#define MLX5_IB_OBJECT_UAR				((1u << UVERBS_ID_NS_SHIFT) + 8)
#define MLX5_IB_METHOD_UAR_OBJ_ALLOC			(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_ATTR_UAR_OBJ_ALLOC_HANDLE		(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_ATTR_UAR_OBJ_ALLOC_TYPE			((1u << UVERBS_ID_NS_SHIFT) + 1)
#define MLX5_IB_ATTR_UAR_OBJ_ALLOC_MMAP_OFFSET		((1u << UVERBS_ID_NS_SHIFT) + 2)
#define MLX5_IB_ATTR_UAR_OBJ_ALLOC_MMAP_LENGTH		((1u << UVERBS_ID_NS_SHIFT) + 3)
#define MLX5_IB_ATTR_UAR_OBJ_ALLOC_PAGE_ID		((1u << UVERBS_ID_NS_SHIFT) + 4)
#define MLX5_IB_OBJECT_VFMIG				((1u << UVERBS_ID_NS_SHIFT) + 10)
#define MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT		(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT		((1u << UVERBS_ID_NS_SHIFT) + 1)
#define MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS		((1u << UVERBS_ID_NS_SHIFT) + 2)
#define MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS		((1u << UVERBS_ID_NS_SHIFT) + 3)
#define MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE	(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT	((1u << UVERBS_ID_NS_SHIFT) + 1)
#define MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_META		((1u << UVERBS_ID_NS_SHIFT) + 2)
#define MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_UAR_TABLE	(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_BFREG_COUNT	((1u << UVERBS_ID_NS_SHIFT) + 1)
#define MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_META	((1u << UVERBS_ID_NS_SHIFT) + 2)
#define MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_RECORDS	(1u << UVERBS_ID_NS_SHIFT)
#define MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_COUNT		((1u << UVERBS_ID_NS_SHIFT) + 1)
#define MLX5_IB_ATTR_VFMIG_RESTORE_DYN_UARS_RECORDS	(1u << UVERBS_ID_NS_SHIFT)
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
	uint8_t  reserved1[3];
	uint16_t devx_uid;
} __attribute__((aligned(8)));

/*
 * MLX5_IB_UAPI_UAR_ALLOC_TYPE_* from include/uapi/rdma/mlx5_user_ioctl_verbs.h.
 * BF (write-combining, blue-flame doorbell) and NC (uncached) are the only
 * two types alloc_uar_entry()/restore_uar_entry() emit.
 */
enum {
	MLX5_IB_UAPI_UAR_ALLOC_TYPE_BF = 0x0,
	MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC = 0x1,
};

struct mlx5_ib_vfmig_dyn_uar_record {
	uint32_t handle;
	uint32_t uar_index;
	uint64_t mmap_offset;
	uint8_t  alloc_type;
	uint8_t  reserved0[7];
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
 * Positive: VFMIG_RESTORE + lib_uar_dyn must be ACCEPTED. Pairs with the
 * MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS path (an earlier revision of
 * this tool used to assert EOPNOTSUPP here, when v0 only covered the
 * static sys_pages[] case; the rejection has since been replaced by a
 * proper dyn-UAR restore story).
 */
static int do_alloc_uctx_dyn_uar(const char *ibdev)
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

	if (rc == 0) {
		printf("alloc_uctx_dyn_uar %s: PASS (VFMIG_RESTORE | DYN_UAR accepted)\n",
		       ibdev);
		return 0;
	}
	fprintf(stderr,
		"alloc_uctx_dyn_uar %s: FAIL (got %s, expected success)\n",
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
 * On-disk format for the cross-host UCTX snapshot used by step 4 of
 * the UAR-restore plan (test_iova_tracked_save_load.sh's UCTX=1 mode).
 *
 *   [vfmig_uctx_blob_hdr]            16 bytes
 *   [mlx5_ib_vfmig_ucontext_meta]    32 bytes
 *   [u32 uar_table[meta.num_sys_pages]]
 *   [u32 bfreg_count[meta.total_num_bfregs]]
 *
 * No driver_id, no kernel-version or libmlx5-version stamp: v0 already
 * relies on a homogeneous fleet (same kernel, same libmlx5, same
 * MLX5_LIB_CAP_*), and the in-handler strict META cross-check at
 * RESTORE time is the actual gatekeeper. The magic + version on this
 * file are just the minimum needed to catch "wrong file" mistakes
 * (truncation, scp'd the netdev blob by accident, etc.). When the
 * homogeneous-fleet caveat is relaxed in a later milestone, this
 * format gains kernel/libmlx5 stamps and a bumped version.
 */
struct vfmig_uctx_blob_hdr {
	char     magic[8];   /* "MLX5VFUC" */
	uint32_t version;    /* 1 */
	uint32_t reserved;
};

#define VFMIG_UCTX_BLOB_MAGIC   "MLX5VFUC"
#define VFMIG_UCTX_BLOB_VERSION 1u

static int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	size_t remaining = len;

	while (remaining) {
		ssize_t n = write(fd, p, remaining);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		p += n;
		remaining -= n;
	}
	return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
	char *p = buf;
	size_t remaining = len;

	while (remaining) {
		ssize_t n = read(fd, p, remaining);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EPROTO;	/* short file */
		p += n;
		remaining -= n;
	}
	return 0;
}

/*
 * save_uctx_snapshot <ibdev> <file>:
 *   Open a normal ucontext on <ibdev>, two-pass QUERY it, and write
 *   the resulting (meta, uar_table, bfreg_count) tuple to <file>.
 *   This is the "Phase A" emitter for cross-host UAR restore: pair
 *   with restore_uctx_snapshot on the destination host.
 */
static int do_save_uctx_snapshot(const char *ibdev, const char *path)
{
	struct vfmig_uctx_blob_hdr hdr = {};
	struct mlx5_ib_vfmig_ucontext_meta meta = {};
	struct resp_blob alloc_resp = {};
	uint32_t *uar_table = NULL;
	uint32_t *bfreg_count = NULL;
	int fd = -1, out_fd = -1, rc, ret = 1;

	fd = open_uverbs_for_ibdev(ibdev);
	if (fd < 0)
		return 1;

	rc = send_get_context(fd, 0, MLX5_LIB_CAP_4K_UAR, &alloc_resp);
	if (rc < 0) {
		fprintf(stderr, "GET_CONTEXT (normal): %s\n", strerror(-rc));
		goto out;
	}

	rc = snapshot_ucontext(fd, &meta, &uar_table, &bfreg_count);
	if (rc < 0) {
		fprintf(stderr, "QUERY (snapshot): %s\n", strerror(-rc));
		goto out;
	}

	out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out_fd < 0) {
		fprintf(stderr, "open %s for write: %s\n", path,
			strerror(errno));
		goto out;
	}

	memcpy(hdr.magic, VFMIG_UCTX_BLOB_MAGIC, sizeof(hdr.magic));
	hdr.version = VFMIG_UCTX_BLOB_VERSION;

	if ((rc = write_all(out_fd, &hdr, sizeof(hdr)))   < 0 ||
	    (rc = write_all(out_fd, &meta, sizeof(meta))) < 0 ||
	    (rc = write_all(out_fd, uar_table,
			    meta.num_sys_pages * sizeof(*uar_table)))   < 0 ||
	    (rc = write_all(out_fd, bfreg_count,
			    meta.total_num_bfregs * sizeof(*bfreg_count))) < 0) {
		fprintf(stderr, "write %s: %s\n", path, strerror(-rc));
		goto out;
	}

	printf("save_uctx_snapshot %s -> %s: PASS\n", ibdev, path);
	printf("  num_sys_pages=%u num_static=%u total_bfregs=%u\n",
	       meta.num_sys_pages, meta.num_static_sys_pages,
	       meta.total_num_bfregs);
	printf("  static FW UAR ids:");
	for (uint32_t i = 0; i < meta.num_static_sys_pages; i++)
		printf(" 0x%x", uar_table[i]);
	printf("\n");
	ret = 0;
out:
	free(uar_table);
	free(bfreg_count);
	if (out_fd >= 0)
		close(out_fd);
	if (fd >= 0)
		close(fd);
	return ret;
}

/*
 * Sanity-mmap one static NC UAR page on a freshly-restored ucontext.
 *
 * mmap_offset encoding (see mlx5_ib_mmap, get_extended_index):
 *   vm_pgoff = (cmd << 8) | (idx & 0xff) | ((idx >> 8) << 16)
 *   mmap byte offset = vm_pgoff * PAGE_SIZE
 *
 * For static slot 0 / NC (cmd=3): offset = (3 << 8) << 12 = 0x300000.
 * NC pages are 4 KiB regardless of FW UAR size; PROT_WRITE-only
 * because libmlx5 typically maps these write-only for doorbells.
 *
 * The mmap succeeding proves three things in one go:
 *   - bfregi->sys_pages[0] holds a valid FW UAR id (else uar_mmap
 *     would short-circuit on MLX5_IB_INVALID_UAR_INDEX);
 *   - that id is recognized by the destination's mlx5_core (else
 *     io_remap_pfn_range would fail with -EFAULT or -EIO);
 *   - the destination's BAR mapping is sane.
 *
 * We don't do any read or write through the mapping. MMIO doorbell
 * registers have no observable read semantics, and writing without
 * subsequent QP work is undefined. The mapping itself is the test.
 */
static int sanity_mmap_one_uar(int fd)
{
	const long page = sysconf(_SC_PAGESIZE);
	const off_t offset = (off_t)(3 /* MLX5_IB_MMAP_NC_PAGE */ << 8)
			     * page;
	void *p;

	p = mmap(NULL, page, PROT_WRITE, MAP_SHARED, fd, offset);
	if (p == MAP_FAILED) {
		fprintf(stderr,
			"  sanity mmap(NC, idx=0, offset=0x%lx) FAILED: %s\n",
			(long)offset, strerror(errno));
		return -errno;
	}

	if (munmap(p, page) < 0) {
		fprintf(stderr, "  munmap: %s\n", strerror(errno));
		return -errno;
	}
	printf("  sanity mmap(NC, idx=0): PASS (mapped + unmapped 1 page)\n");
	return 0;
}

/*
 * restore_uctx_snapshot <ibdev> <file>:
 *   Open a ucontext on <ibdev> with MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
 *   read the snapshot from <file>, RESTORE, then QUERY to confirm
 *   bitwise match, then mmap a static NC UAR page as a sanity probe.
 *   This is the "Phase D" consumer for cross-host UAR restore.
 */
static int do_restore_uctx_snapshot(const char *ibdev, const char *path)
{
	struct vfmig_uctx_blob_hdr hdr;
	struct mlx5_ib_vfmig_ucontext_meta meta;
	struct mlx5_ib_vfmig_ucontext_meta meta_post = {};
	struct resp_blob alloc_resp = {};
	uint32_t *uar_table = NULL;
	uint32_t *bfreg_count = NULL;
	uint32_t *uar_post = NULL;
	uint32_t *cnt_post = NULL;
	int fd = -1, in_fd = -1, rc, ret = 1;

	in_fd = open(path, O_RDONLY);
	if (in_fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}
	rc = read_all(in_fd, &hdr, sizeof(hdr));
	if (rc < 0) {
		fprintf(stderr, "read %s hdr: %s\n", path, strerror(-rc));
		goto out;
	}
	if (memcmp(hdr.magic, VFMIG_UCTX_BLOB_MAGIC,
		   sizeof(hdr.magic)) != 0) {
		fprintf(stderr,
			"%s: bad magic (not a vfmig uctx snapshot)\n", path);
		goto out;
	}
	if (hdr.version != VFMIG_UCTX_BLOB_VERSION) {
		fprintf(stderr,
			"%s: version %u, this tool understands %u\n",
			path, hdr.version, VFMIG_UCTX_BLOB_VERSION);
		goto out;
	}
	rc = read_all(in_fd, &meta, sizeof(meta));
	if (rc < 0) {
		fprintf(stderr, "read %s meta: %s\n", path, strerror(-rc));
		goto out;
	}
	uar_table = calloc(meta.num_sys_pages, sizeof(*uar_table));
	bfreg_count = calloc(meta.total_num_bfregs, sizeof(*bfreg_count));
	if (!uar_table || !bfreg_count) {
		fprintf(stderr, "calloc failed\n");
		goto out;
	}
	if ((rc = read_all(in_fd, uar_table,
			   meta.num_sys_pages * sizeof(*uar_table))) < 0 ||
	    (rc = read_all(in_fd, bfreg_count,
			   meta.total_num_bfregs *
				sizeof(*bfreg_count))) < 0) {
		fprintf(stderr, "read %s body: %s\n", path, strerror(-rc));
		goto out;
	}

	fd = open_uverbs_for_ibdev(ibdev);
	if (fd < 0)
		goto out;
	rc = send_get_context(fd, MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
			      MLX5_LIB_CAP_4K_UAR, &alloc_resp);
	if (rc < 0) {
		fprintf(stderr, "GET_CONTEXT (VFMIG_RESTORE): %s\n",
			strerror(-rc));
		goto out;
	}

	rc = issue_vfmig_restore(fd, uar_table, meta.num_sys_pages,
				 bfreg_count, meta.total_num_bfregs, &meta);
	if (rc < 0) {
		fprintf(stderr, "RESTORE_UCONTEXT: %s\n", strerror(-rc));
		goto out;
	}

	rc = snapshot_ucontext(fd, &meta_post, &uar_post, &cnt_post);
	if (rc < 0) {
		fprintf(stderr, "QUERY (post-RESTORE): %s\n", strerror(-rc));
		goto out;
	}
	if (memcmp(&meta, &meta_post, sizeof(meta)) != 0 ||
	    memcmp(uar_table, uar_post,
		   meta.num_sys_pages * sizeof(*uar_table)) != 0 ||
	    memcmp(bfreg_count, cnt_post,
		   meta.total_num_bfregs * sizeof(*bfreg_count)) != 0) {
		fprintf(stderr, "post-RESTORE QUERY does not match snapshot\n");
		goto out;
	}

	printf("restore_uctx_snapshot %s <- %s: RESTORE+QUERY match: PASS\n",
	       ibdev, path);

	rc = sanity_mmap_one_uar(fd);
	if (rc < 0)
		goto out;

	printf("restore_uctx_snapshot %s: PASS\n", ibdev);
	ret = 0;
out:
	free(uar_table);
	free(bfreg_count);
	free(uar_post);
	free(cnt_post);
	if (fd >= 0)
		close(fd);
	if (in_fd >= 0)
		close(in_fd);
	return ret;
}

/*
 * Build & issue a VFMIG QUERY_DYN_UARS ioctl. RECORDS may be NULL+0 to
 * request a sizing pass (kernel writes COUNT only). On success returns
 * 0 and writes *count_out. On failure returns -errno (negated).
 */
static int issue_vfmig_query_dyn_uars(int fd,
		struct mlx5_ib_vfmig_dyn_uar_record *records, size_t records_n,
		uint32_t *count_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr hdr;
		struct ib_uverbs_attr attrs[2];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id = MLX5_IB_OBJECT_VFMIG;
	cmd.hdr.method_id = MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS;
	cmd.hdr.driver_id = RDMA_DRIVER_MLX5;

	if (records) {
		cmd.attrs[n].attr_id = MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_RECORDS;
		cmd.attrs[n].len = (uint16_t)(records_n * sizeof(*records));
		cmd.attrs[n].flags = UVERBS_ATTR_F_MANDATORY;
		cmd.attrs[n].data = (uintptr_t)records;
		n++;
	}

	cmd.attrs[n].attr_id = MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_COUNT;
	cmd.attrs[n].len = sizeof(*count_out);
	cmd.attrs[n].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data = (uintptr_t)count_out;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

/*
 * Build & issue a VFMIG RESTORE_DYN_UARS ioctl. RECORDS is mandatory
 * with non-zero length on the wire (kernel rejects len % sizeof(record)
 * != 0 or len == 0). Returns 0 on kernel success, -errno (negated) on
 * failure.
 */
static int issue_vfmig_restore_dyn_uars(int fd,
		const struct mlx5_ib_vfmig_dyn_uar_record *records,
		size_t records_n)
{
	struct {
		struct ib_uverbs_ioctl_hdr hdr;
		struct ib_uverbs_attr attrs[1];
	} cmd = {};

	cmd.hdr.object_id = MLX5_IB_OBJECT_VFMIG;
	cmd.hdr.method_id = MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS;
	cmd.hdr.driver_id = RDMA_DRIVER_MLX5;
	cmd.hdr.num_attrs = 1;
	cmd.hdr.length = sizeof(cmd.hdr) + sizeof(cmd.attrs[0]);

	cmd.attrs[0].attr_id = MLX5_IB_ATTR_VFMIG_RESTORE_DYN_UARS_RECORDS;
	cmd.attrs[0].len = (uint16_t)(records_n * sizeof(*records));
	cmd.attrs[0].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[0].data = (uintptr_t)records;

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

/*
 * Result tuple returned by an UAR_OBJ_ALLOC ioctl.
 *
 *   handle:       ufile->idr id assigned to the new MLX5_IB_OBJECT_UAR
 *                 uobject. The kernel writes this into the HANDLE attr's
 *                 data field for IDR/ACCESS_NEW attributes (see
 *                 uverbs_process_attr() / put_user(id, &user_attrs[i].data)).
 *   page_id:      FW UAR id (== mlx5_user_mmap_entry::page_idx).
 *   mmap_offset:  start_pgoff << PAGE_SHIFT, i.e. the byte offset libmlx5
 *                 uses with mmap(MAP_SHARED, fd, ...) to reach this UAR.
 *   mmap_length:  region length (1 page on every supported platform).
 */
struct uar_alloc_result {
	uint32_t handle;
	uint32_t page_id;
	uint64_t mmap_offset;
	uint32_t mmap_length;
};

/*
 * Issue MLX5_IB_METHOD_UAR_OBJ_ALLOC via the raw uverbs ioctl, bypassing
 * libibverbs / libmlx5 (libmlx5 wraps this in mlx5dv_devx_alloc_uar(),
 * which we don't want a hard dependency on for a kernel test).
 *
 * Wire format for each attr (see uverbs_process_attr):
 *   - HANDLE (IDR/ACCESS_NEW):
 *       len=0, attr_data.reserved=0. Kernel allocates the uobject and
 *       writes the assigned id back into attrs[].data via put_user().
 *   - TYPE (CONST_IN, i.e. inline u64-sized PTR_IN):
 *       len=8. Value is packed *inline* in attrs[].data (the kernel
 *       reads it directly because uverbs_attr_ptr_is_inline() returns
 *       true for len <= 8); no userspace pointer dereference happens.
 *   - PAGE_ID / MMAP_LENGTH / MMAP_OFFSET (PTR_OUT):
 *       len=sizeof(field), data=(uintptr_t)&field. Kernel copies via
 *       copy_to_user.
 *
 * Returns 0 on kernel success, -errno (negated) on failure.
 */
static int issue_uar_obj_alloc(int fd, uint8_t alloc_type,
			       struct uar_alloc_result *out)
{
	struct {
		struct ib_uverbs_ioctl_hdr hdr;
		struct ib_uverbs_attr attrs[5];
	} cmd = {};

	memset(out, 0, sizeof(*out));

	cmd.hdr.object_id = MLX5_IB_OBJECT_UAR;
	cmd.hdr.method_id = MLX5_IB_METHOD_UAR_OBJ_ALLOC;
	cmd.hdr.driver_id = RDMA_DRIVER_MLX5;
	cmd.hdr.num_attrs = 5;
	cmd.hdr.length = sizeof(cmd.hdr) + 5 * sizeof(cmd.attrs[0]);

	cmd.attrs[0].attr_id = MLX5_IB_ATTR_UAR_OBJ_ALLOC_HANDLE;
	cmd.attrs[0].len = 0;
	cmd.attrs[0].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[0].data = 0;

	cmd.attrs[1].attr_id = MLX5_IB_ATTR_UAR_OBJ_ALLOC_TYPE;
	cmd.attrs[1].len = sizeof(uint64_t);
	cmd.attrs[1].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[1].data = alloc_type;

	cmd.attrs[2].attr_id = MLX5_IB_ATTR_UAR_OBJ_ALLOC_PAGE_ID;
	cmd.attrs[2].len = sizeof(out->page_id);
	cmd.attrs[2].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[2].data = (uintptr_t)&out->page_id;

	cmd.attrs[3].attr_id = MLX5_IB_ATTR_UAR_OBJ_ALLOC_MMAP_LENGTH;
	cmd.attrs[3].len = sizeof(out->mmap_length);
	cmd.attrs[3].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[3].data = (uintptr_t)&out->mmap_length;

	cmd.attrs[4].attr_id = MLX5_IB_ATTR_UAR_OBJ_ALLOC_MMAP_OFFSET;
	cmd.attrs[4].len = sizeof(out->mmap_offset);
	cmd.attrs[4].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[4].data = (uintptr_t)&out->mmap_offset;

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;

	/*
	 * IDR/ACCESS_NEW: kernel wrote the assigned handle back into
	 * attrs[0].data via put_user(). The data field is u64; the
	 * handle is an unsigned 32-bit id (xa_limit_32b in idr_add_uobj).
	 */
	out->handle = (uint32_t)cmd.attrs[0].data;
	return 0;
}

/*
 * Find a record in @records (length @n) with handle == @handle. Returns
 * a pointer into @records, or NULL if not found.
 */
static const struct mlx5_ib_vfmig_dyn_uar_record *
find_record_by_handle(const struct mlx5_ib_vfmig_dyn_uar_record *records,
		      uint32_t n, uint32_t handle)
{
	for (uint32_t i = 0; i < n; i++)
		if (records[i].handle == handle)
			return &records[i];
	return NULL;
}

/*
 * roundtrip_dyn_uars <ibdev>:
 *
 * Same-VHCA smoke test for the dyn-UAR save/restore path. Validates the
 * userspace-visible end-to-end shape:
 *
 *   1. fd_a opens a normal dynamic-UAR ucontext.
 *   2. fd_a issues N UAR_OBJ_ALLOC ioctls; remembers (handle, page_id,
 *      mmap_offset, alloc_type) for each.
 *   3. fd_a runs QUERY_DYN_UARS (two-pass) and asserts every record
 *      matches step (2) by handle.
 *   4. fd_b opens a ucontext with VFMIG_RESTORE | DYN_UAR.
 *   5. fd_b runs QUERY_DYN_UARS and asserts COUNT == 0 (the alloc-skip
 *      path took effect; no live UARs yet).
 *   6. fd_b runs RESTORE_DYN_UARS with fd_a's snapshot.
 *   7. fd_b runs QUERY_DYN_UARS again and asserts records match fd_a
 *      bitwise -- this confirms rdma_alloc_begin_uobject_at_handle()
 *      pinned the handles and restore_uar_entry() reinserted the mmap
 *      pgoffs at the saved offsets.
 *   8. fd_b mmap()s one of the saved offsets and confirms it succeeds
 *      (kernel ran rdma_user_mmap_entry_get_pgoff() against fd_b's
 *      mmap table, found the restored entry, and io_remap_pfn_range()d
 *      the BAR for the saved page_id). No read/write through the
 *      mapping -- doorbell semantics are out of scope here.
 *   9. Cleanup: close fd_b first, then fd_a. Kernel will dmesg one
 *      benign DEALLOC_UAR failure on fd_a's close because both
 *      ucontexts hold the same FW UAR id on the same VHCA -- this is
 *      the design difference vs cross-host (LOAD_VHCA_STATE), which
 *      transfers the FW id ownership and avoids the double-free.
 *
 * SCOPE CAVEAT: this is a same-VHCA smoke that exercises the kernel
 * verbs end-to-end, not a substitute for the cross-host pingpong gate.
 * It does NOT exercise:
 *   - LOAD_VHCA_STATE FW id ownership transfer.
 *   - QP doorbell semantics (FW would reject doorbells on a UAR that
 *     fd_a still holds and fd_b restored; we never doorbell, only mmap).
 */
#define ROUNDTRIP_DYN_UARS_NR 2

static int do_roundtrip_dyn_uars(const char *ibdev)
{
	struct uar_alloc_result alloc[ROUNDTRIP_DYN_UARS_NR] = {};
	struct mlx5_ib_vfmig_dyn_uar_record *recs_a = NULL;
	struct mlx5_ib_vfmig_dyn_uar_record *recs_b = NULL;
	struct resp_blob alloc_resp_a = {};
	struct resp_blob alloc_resp_b = {};
	uint32_t count_a = 0, count_b = 0;
	int fd_a = -1, fd_b = -1, rc, ret = 1;
	const long page = sysconf(_SC_PAGESIZE);
	uint32_t i;
	uint8_t alloc_type;
	void *p;

	fd_a = open_uverbs_for_ibdev(ibdev);
	if (fd_a < 0)
		return 1;
	fd_b = open_uverbs_for_ibdev(ibdev);
	if (fd_b < 0)
		goto out;

	/* Step 1: open fd_a as a normal dyn-UAR ucontext. */
	rc = send_get_context(fd_a, 0,
			      MLX5_LIB_CAP_4K_UAR | MLX5_LIB_CAP_DYN_UAR,
			      &alloc_resp_a);
	if (rc < 0) {
		fprintf(stderr,
			"step 1 FAIL: GET_CONTEXT(A, dyn-UAR): %s\n",
			strerror(-rc));
		goto out;
	}

	/*
	 * Step 2: alloc N=2 dyn UARs on fd_a. Try BF (write-combining)
	 * first; if the device doesn't support WC pages, fall back to NC
	 * for the remaining slots. Either way we exercise the same code
	 * path -- alloc_type just decides mmap_flag (UAR_WC vs UAR_NC).
	 */
	for (i = 0; i < ROUNDTRIP_DYN_UARS_NR; i++) {
		alloc_type = (i == 0) ? MLX5_IB_UAPI_UAR_ALLOC_TYPE_BF
				      : MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC;
		rc = issue_uar_obj_alloc(fd_a, alloc_type, &alloc[i]);
		if (rc == -EOPNOTSUPP &&
		    alloc_type == MLX5_IB_UAPI_UAR_ALLOC_TYPE_BF) {
			/* WC not supported; fall back to NC. */
			alloc_type = MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC;
			rc = issue_uar_obj_alloc(fd_a, alloc_type, &alloc[i]);
		}
		if (rc < 0) {
			fprintf(stderr,
				"step 2 FAIL: UAR_OBJ_ALLOC[%u, type=%u]: %s\n",
				i, alloc_type, strerror(-rc));
			goto out;
		}
		printf("step 2: UAR_OBJ_ALLOC[%u]: handle=%u page_id=%u "
		       "mmap_offset=0x%llx mmap_length=%u alloc_type=%u\n",
		       i, alloc[i].handle, alloc[i].page_id,
		       (unsigned long long)alloc[i].mmap_offset,
		       alloc[i].mmap_length, alloc_type);
	}

	/*
	 * Step 3: snapshot fd_a via QUERY_DYN_UARS (two-pass). Cross-
	 * check by handle against the live UAR_OBJ_ALLOC tuples.
	 */
	rc = issue_vfmig_query_dyn_uars(fd_a, NULL, 0, &count_a);
	if (rc < 0) {
		fprintf(stderr,
			"step 3 FAIL: QUERY_DYN_UARS(A, sizing): %s\n",
			strerror(-rc));
		goto out;
	}
	if (count_a != ROUNDTRIP_DYN_UARS_NR) {
		fprintf(stderr,
			"step 3 FAIL: QUERY_DYN_UARS(A) count=%u, expected %u\n",
			count_a, ROUNDTRIP_DYN_UARS_NR);
		goto out;
	}
	recs_a = calloc(count_a, sizeof(*recs_a));
	if (!recs_a) {
		fprintf(stderr, "calloc: %s\n", strerror(errno));
		goto out;
	}
	rc = issue_vfmig_query_dyn_uars(fd_a, recs_a, count_a, &count_a);
	if (rc < 0) {
		fprintf(stderr,
			"step 3 FAIL: QUERY_DYN_UARS(A, snapshot): %s\n",
			strerror(-rc));
		goto out;
	}
	for (i = 0; i < ROUNDTRIP_DYN_UARS_NR; i++) {
		const struct mlx5_ib_vfmig_dyn_uar_record *r =
			find_record_by_handle(recs_a, count_a,
					      alloc[i].handle);
		if (!r) {
			fprintf(stderr,
				"step 3 FAIL: handle=%u not present in QUERY(A) snapshot\n",
				alloc[i].handle);
			goto out;
		}
		if (r->uar_index != alloc[i].page_id ||
		    r->mmap_offset != alloc[i].mmap_offset) {
			fprintf(stderr,
				"step 3 FAIL: QUERY(A) record for handle=%u "
				"differs from UAR_OBJ_ALLOC: "
				"snapshot uar=%u offset=0x%llx vs alloc uar=%u offset=0x%llx\n",
				alloc[i].handle,
				r->uar_index,
				(unsigned long long)r->mmap_offset,
				alloc[i].page_id,
				(unsigned long long)alloc[i].mmap_offset);
			goto out;
		}
	}
	printf("step 3: QUERY_DYN_UARS(A) bitwise matches live UAR_OBJ_ALLOC tuples: PASS\n");

	/* Step 4: open fd_b with VFMIG_RESTORE | DYN_UAR. */
	rc = send_get_context(fd_b,
			      MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
			      MLX5_LIB_CAP_4K_UAR | MLX5_LIB_CAP_DYN_UAR,
			      &alloc_resp_b);
	if (rc < 0) {
		fprintf(stderr,
			"step 4 FAIL: GET_CONTEXT(B, VFMIG_RESTORE | DYN_UAR): %s\n",
			strerror(-rc));
		goto out;
	}

	/*
	 * Step 5: confirm fd_b is empty pre-RESTORE. The
	 * MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE | DYN_UAR alloc path
	 * unconditionally bypasses allocate_uars(); no MLX5_IB_OBJECT_UAR
	 * uobjects exist yet, so QUERY_DYN_UARS must report COUNT == 0.
	 */
	rc = issue_vfmig_query_dyn_uars(fd_b, NULL, 0, &count_b);
	if (rc < 0) {
		fprintf(stderr,
			"step 5 FAIL: QUERY_DYN_UARS(B, pre-RESTORE): %s\n",
			strerror(-rc));
		goto out;
	}
	if (count_b != 0) {
		fprintf(stderr,
			"step 5 FAIL: QUERY_DYN_UARS(B, pre-RESTORE) count=%u, expected 0\n",
			count_b);
		goto out;
	}
	printf("step 5: QUERY_DYN_UARS(B) pre-RESTORE: count=0 (alloc-skip OK)\n");

	/* Step 6: RESTORE_DYN_UARS on fd_b with fd_a's snapshot. */
	rc = issue_vfmig_restore_dyn_uars(fd_b, recs_a, count_a);
	if (rc < 0) {
		fprintf(stderr,
			"step 6 FAIL: RESTORE_DYN_UARS(B): %s\n",
			strerror(-rc));
		goto out;
	}
	printf("step 6: RESTORE_DYN_UARS(B) %u records: PASS\n", count_a);

	/* Step 7: re-query fd_b and assert bitwise match against fd_a. */
	rc = issue_vfmig_query_dyn_uars(fd_b, NULL, 0, &count_b);
	if (rc < 0 || count_b != count_a) {
		fprintf(stderr,
			"step 7 FAIL: QUERY_DYN_UARS(B, post-RESTORE) count=%u expected=%u rc=%d\n",
			count_b, count_a, rc);
		goto out;
	}
	recs_b = calloc(count_b, sizeof(*recs_b));
	if (!recs_b) {
		fprintf(stderr, "calloc: %s\n", strerror(errno));
		goto out;
	}
	rc = issue_vfmig_query_dyn_uars(fd_b, recs_b, count_b, &count_b);
	if (rc < 0) {
		fprintf(stderr,
			"step 7 FAIL: QUERY_DYN_UARS(B, post-RESTORE snapshot): %s\n",
			strerror(-rc));
		goto out;
	}
	/*
	 * Compare by handle (list-iteration order is not guaranteed: the
	 * uobject list is FIFO, but RESTORE prepends each new uobj and
	 * QUERY walks list_for_each_entry which is forward-from-head).
	 */
	for (i = 0; i < count_a; i++) {
		const struct mlx5_ib_vfmig_dyn_uar_record *a = &recs_a[i];
		const struct mlx5_ib_vfmig_dyn_uar_record *b =
			find_record_by_handle(recs_b, count_b, a->handle);
		if (!b) {
			fprintf(stderr,
				"step 7 FAIL: handle=%u missing in QUERY(B post-RESTORE)\n",
				a->handle);
			goto out;
		}
		if (b->uar_index != a->uar_index ||
		    b->mmap_offset != a->mmap_offset ||
		    b->alloc_type != a->alloc_type) {
			fprintf(stderr,
				"step 7 FAIL: handle=%u record drift: "
				"A{uar=%u offset=0x%llx alloc=%u} vs "
				"B{uar=%u offset=0x%llx alloc=%u}\n",
				a->handle, a->uar_index,
				(unsigned long long)a->mmap_offset, a->alloc_type,
				b->uar_index,
				(unsigned long long)b->mmap_offset, b->alloc_type);
			goto out;
		}
	}
	printf("step 7: QUERY_DYN_UARS(B post-RESTORE) bitwise matches A: PASS\n");

	/*
	 * Step 8: mmap one of fd_b's restored UAR offsets. We pick
	 * record [0] since both BF (WC) and NC are mmap'd PROT_WRITE
	 * 1 page; the exact prot doesn't matter for the success-of-mmap
	 * test (mlx5_ib_mmap picks pgprot via mmap_flag and io_remap_pfn
	 * succeeds on either). munmap() right after; we never touch the
	 * mapping (see DOORBELL caveat in this function's docstring).
	 */
	p = mmap(NULL, page, PROT_WRITE, MAP_SHARED, fd_b,
		 (off_t)recs_b[0].mmap_offset);
	if (p == MAP_FAILED) {
		fprintf(stderr,
			"step 8 FAIL: mmap(fd_b, offset=0x%llx) on restored UAR handle=%u: %s\n",
			(unsigned long long)recs_b[0].mmap_offset,
			recs_b[0].handle, strerror(errno));
		goto out;
	}
	if (munmap(p, page) < 0)
		fprintf(stderr, "step 8 warning: munmap: %s\n",
			strerror(errno));
	printf("step 8: mmap(fd_b, offset=0x%llx, len=%ld) on restored UAR handle=%u: PASS\n",
	       (unsigned long long)recs_b[0].mmap_offset, page,
	       recs_b[0].handle);

	printf("\nroundtrip_dyn_uars %s: PASS\n", ibdev);
	printf("  (NOTE: closing fd_a will dmesg one benign DEALLOC_UAR error per\n");
	printf("   restored UAR -- expected on a same-VHCA smoke; cross-host\n");
	printf("   LOAD_VHCA_STATE transfers the FW id ownership and avoids it.)\n");
	ret = 0;

out:
	free(recs_a);
	free(recs_b);
	if (fd_b >= 0)
		close(fd_b);	/* close B first to deallocate the ids exactly once */
	if (fd_a >= 0)
		close(fd_a);
	return ret;
}

/*
 * query_dyn_uars <ibdev>:
 *   Open a *dynamic-UAR* ucontext (lib_caps |= MLX5_LIB_CAP_DYN_UAR) and
 *   issue MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS twice -- pass 1 (sizing)
 *   for COUNT only, pass 2 (snapshot) for the records. On a freshly
 *   opened ucontext that hasn't yet issued any UAR_OBJ_ALLOC verbs,
 *   COUNT is expected to be 0; this subcommand is the reachability
 *   probe for the new verb. roundtrip_dyn_uars actually populates
 *   dyn UARs and exercises the full save/restore round-trip.
 */
static int do_query_dyn_uars(const char *ibdev)
{
	struct resp_blob alloc_resp = {};
	struct mlx5_ib_vfmig_dyn_uar_record *records = NULL;
	uint32_t count = 0;
	int fd, rc, ret = 1;

	fd = open_uverbs_for_ibdev(ibdev);
	if (fd < 0)
		return 1;

	rc = send_get_context(fd, 0,
			      MLX5_LIB_CAP_4K_UAR | MLX5_LIB_CAP_DYN_UAR,
			      &alloc_resp);
	if (rc < 0) {
		fprintf(stderr,
			"GET_CONTEXT (dyn-UAR) failed: %s\n", strerror(-rc));
		goto out;
	}

	rc = issue_vfmig_query_dyn_uars(fd, NULL, 0, &count);
	if (rc < 0) {
		fprintf(stderr,
			"QUERY_DYN_UARS (sizing) failed: %s\n",
			strerror(-rc));
		goto out;
	}
	printf("query_dyn_uars %s: pass 1 count=%u\n", ibdev, count);

	if (count) {
		records = calloc(count, sizeof(*records));
		if (!records) {
			fprintf(stderr, "calloc failed\n");
			goto out;
		}
		rc = issue_vfmig_query_dyn_uars(fd, records, count, &count);
		if (rc < 0) {
			fprintf(stderr,
				"QUERY_DYN_UARS (snapshot) failed: %s\n",
				strerror(-rc));
			goto out;
		}
		printf("pass 2: %u dyn UAR record(s):\n", count);
		for (uint32_t i = 0; i < count; i++) {
			printf("  [%4u] handle=%u uar_index=%u "
			       "mmap_offset=0x%llx alloc_type=%u\n",
			       i, records[i].handle, records[i].uar_index,
			       (unsigned long long)records[i].mmap_offset,
			       records[i].alloc_type);
		}
	} else {
		printf("(no dyn UARs allocated on this ucontext yet -- "
		       "expected on a fresh open without UAR_OBJ_ALLOC)\n");
	}

	printf("query_dyn_uars %s: PASS\n", ibdev);
	ret = 0;
out:
	free(records);
	if (fd >= 0)
		close(fd);
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
		"  alloc_uctx_dyn_uar <ibdev>              (positive: VFMIG_RESTORE | DYN_UAR accepted)\n"
		"  alloc_uctx_neg_bad_flag <ibdev>         (negative: bogus flag bit -> EOPNOTSUPP)\n"
		"  query_uctx_uar_table <ibdev>            (open normal ucontext, QUERY two-pass)\n"
		"  query_dyn_uars <ibdev>                  (open dyn-UAR ucontext, QUERY_DYN_UARS two-pass)\n"
		"  roundtrip_dyn_uars <ibdev>              (positive: alloc N dyn UARs on A -> snapshot -> restore on B -> mmap)\n"
		"  roundtrip_uctx <ibdev>                  (positive: A normal -> QUERY -> B w/flag -> RESTORE -> QUERY match)\n"
		"  roundtrip_uctx_neg_no_flag <ibdev>      (negative: RESTORE on non-VFMIG ucontext -> EINVAL)\n"
		"  roundtrip_uctx_neg_meta_mismatch <ibdev>(negative: META total_num_bfregs tampered -> EINVAL)\n"
		"  roundtrip_uctx_neg_static_invalid <ibdev>(negative: static slot 0 = INVALID -> EINVAL)\n"
		"  save_uctx_snapshot <ibdev> <file>       (Phase A emitter: QUERY -> file)\n"
		"  restore_uctx_snapshot <ibdev> <file>    (Phase D consumer: file -> RESTORE -> QUERY+mmap)\n"
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
	if (argc == 3 && verb_eq(argv[1], "alloc_uctx_dyn_uar"))
		return do_alloc_uctx_dyn_uar(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "alloc_uctx_neg_bad_flag"))
		return do_alloc_uctx_neg_bad_flag(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "query_uctx_uar_table"))
		return do_query_uctx_uar_table(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "query_dyn_uars"))
		return do_query_dyn_uars(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "roundtrip_dyn_uars"))
		return do_roundtrip_dyn_uars(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "roundtrip_uctx"))
		return do_roundtrip_uctx(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "roundtrip_uctx_neg_no_flag"))
		return do_roundtrip_uctx_neg_no_flag(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "roundtrip_uctx_neg_meta_mismatch"))
		return do_roundtrip_uctx_neg_meta_mismatch(argv[2]);
	if (argc == 3 && verb_eq(argv[1], "roundtrip_uctx_neg_static_invalid"))
		return do_roundtrip_uctx_neg_static_invalid(argv[2]);
	if (argc == 4 && verb_eq(argv[1], "save_uctx_snapshot"))
		return do_save_uctx_snapshot(argv[2], argv[3]);
	if (argc == 4 && verb_eq(argv[1], "restore_uctx_snapshot"))
		return do_restore_uctx_snapshot(argv[2], argv[3]);

	usage(argv[0]);
	return 1;
}
