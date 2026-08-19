// SPDX-License-Identifier: GPL-2.0
/*
 * mr_restore_probe_mlx5_vfmig_dmabuf -- end-to-end driver-level
 * validation of UVERBS_METHOD_RESTORE_MR_DMABUF + mlx5_ib_restore_mr_dmabuf
 * (gpu-dmabuf-criu-plan.md §3c/§3d).
 *
 * Sibling of mr_restore_probe_mlx5_vfmig.c (RESTORE_MR, the regular
 * ib_umem_pin-backed path). Same subtest shape and raw-ioctl
 * technique, but exercises the GPU/peer dma-buf restore path instead:
 *
 *   RESTORE_MR       addr (host VA, ib_umem_pin'able)  -> mlx5_ib_restore_mr
 *   RESTORE_MR_DMABUF dmabuf_fd + offset (fresh GPU alloc) -> mlx5_ib_restore_mr_dmabuf
 *
 * Where RESTORE_MR's probe fakes the destination-side backing memory
 * with an anonymous mmap (ib_umem_pin needs *some* mapping at @addr
 * in this process to pin), this probe fakes it with a FRESH CUDA VMM
 * allocation exported as a dma-buf -- ib_umem_dmabuf_get needs a real
 * dma-buf fd, and mlx5_ib_umem_restore_mr_dmabuf's dma-buf import path
 * is what we're actually testing.
 *
 * What we validate against a bound, post-LOAD destination mlx5
 * ib_device with a vfmig-tracked VF that has already replayed a
 * HOST_USER_MMIO record for (KIND_MR, src_mkey_index) into an
 * awaiting_bind USER_MMIO placeholder (i.e. a real SAVE_VHCA_STATE +
 * LOAD_VHCA_STATE cycle already ran against a source-side GPU dma-buf
 * MR registered at that mkey_index -- see gpu-dmabuf-criu-plan.md §1
 * Step 0 for how to register one, and the criu-test-harness VF
 * create/migrate steps for SAVE/LOAD):
 *
 *   1. Gate (negative). Same as RESTORE_MR: a ucontext opened WITHOUT
 *      MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE cannot invoke RESTORE_MR_DMABUF;
 *      -EPERM via mlx5_ib_ucontext_is_restore_mode.
 *   2. UAPI reject (mkey_index=0) -> -EINVAL.
 *   3. UAPI reject (reserved nonzero) -> -EINVAL.
 *   4. UAPI reject (lkey != rkey) -> -EINVAL.
 *   5. UAPI reject ((lkey>>8) != mkey_index) -> -EINVAL.
 *   6. Happy path. After a prerequisite RESTORE_PD adoption of
 *      src_pdn, RESTORE_MR_DMABUF with a FRESH CUDA dma-buf fd lands a
 *      fresh kernel-side mlx5_ib_mr at mr_target_handle, backed by
 *      the fresh GPU allocation relocated to the placeholder's IOVA
 *      (mlx5_vfmig_relocate_dmabuf_mr). RESP_LKEY/RESP_RKEY must echo
 *      the hints. INFO_HANDLES(MR) must contain the target handle.
 *   7. Collision. A second RESTORE_MR_DMABUF(target_handle) -> -EBUSY.
 *   8. READY checkpoint (same protocol as mr_restore_probe_mlx5_vfmig):
 *      prints identity + READY, blocks on stdin until "quit\n", so the
 *      harness can probe FW/vfmig state (mlx5_vfmig.py query-all,
 *      MLX5_VFMIG_IOC_PROBE_MKEY, etc.) while the MR is alive.
 *   9. v0 dealloc semantics, same as RESTORE_MR's subtest 8/9: mkey is
 *      a leaf in the FW resource graph, so DEREG_MR on the orphan
 *      adopted mkey succeeds even with dependents still alive.
 *
 * Stays libibverbs-only and pure-uverbs (no libmlx5, no mlx5dv) for
 * the ucontext/RESTORE_* plumbing, same as mr_restore_probe_mlx5_vfmig.
 * Uses the CUDA *driver* API (not the runtime API) for the dma-buf
 * allocation, matching gpu-dmabuf-criu-plan.md's step0_gpu_dmabuf_test.c.
 *
 * Build:
 *   gcc -O2 -o mr_restore_probe_mlx5_vfmig_dmabuf \
 *       mr_restore_probe_mlx5_vfmig_dmabuf.c \
 *       -I/usr/local/cuda/include -lcuda -libverbs
 *
 * Usage:
 *   ./mr_restore_probe_mlx5_vfmig_dmabuf <ibdev> <src_pdn> <src_mkey_index>
 *           <src_lkey> <src_mr_length> <src_access_flags>
 *           [<mr_target_handle> [<pd_target_handle> [<cuda_device_id>]]]
 *
 * Args are the same as mr_restore_probe_mlx5_vfmig's minus <src_mr_addr>
 * (no host VA for a dma-buf MR) plus an optional trailing CUDA device
 * ordinal (default 0).
 *
 * No dependency on libmlx5 or rdma-core's userspace mlx5 UAPI: the
 * mlx5-side ABI surface is inlined below, kept in sync with
 * include/uapi/rdma/mlx5-abi.h and include/uapi/rdma/ib_user_ioctl_cmds.h.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cuda.h>
#include <infiniband/verbs.h>
#include <rdma/ib_user_verbs.h>

/* ---------------------------------------------------------------------- */
/*                       mlx5 alloc-ucontext ABI                          */
/* ---------------------------------------------------------------------- */

enum {
	MLX5_LIB_CAP_4K_UAR	= (uint64_t)1 << 0,
};

enum {
	MLX5_IB_ALLOC_UCTX_DEVX			= 1u << 0,
	MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE	= 1u << 1,
};

struct mlx5_ib_alloc_ucontext_req_v2 {
	uint32_t	total_num_bfregs;
	uint32_t	num_low_latency_bfregs;
	uint32_t	flags;
	uint32_t	comp_mask;
	uint8_t		max_cqe_version;
	uint8_t		reserved0;
	uint16_t	reserved1;
	uint32_t	reserved2;
	uint64_t	lib_caps;
} __attribute__((aligned(8)));

struct mlx5_ib_alloc_ucontext_resp {
	uint32_t	qp_tab_size;
	uint32_t	bf_reg_size;
	uint32_t	tot_bfregs;
	uint32_t	cache_line_size;
	uint16_t	max_sq_desc_sz;
	uint16_t	max_rq_desc_sz;
	uint32_t	max_send_wqebb;
	uint32_t	max_recv_wr;
	uint32_t	max_srq_recv_wr;
	uint16_t	num_ports;
	uint16_t	flow_action_flags;
	uint32_t	comp_mask;
	uint32_t	response_length;
	uint8_t		cqe_version;
	uint8_t		cmds_supp_uhw;
	uint8_t		eth_min_inline;
	uint8_t		clock_info_versions;
	uint64_t	hca_core_clock_offset;
	uint32_t	log_uar_size;
	uint32_t	num_uars_per_page;
	uint32_t	num_dyn_bfregs;
	uint32_t	dump_fill_mkey;
} __attribute__((aligned(8)));

struct mlx5_ib_restore_pd_req {
	uint32_t	pdn;
	uint32_t	reserved;
	uint64_t	reserved2;
} __attribute__((aligned(8)));

/*
 * Driver-private UHW payload for UVERBS_METHOD_RESTORE_MR_DMABUF on
 * mlx5. Same shape as mlx5_ib_restore_mr_req -- see
 * include/uapi/rdma/mlx5-abi.h's struct mlx5_ib_restore_mr_dmabuf_req.
 */
struct mlx5_ib_restore_mr_dmabuf_req {
	uint32_t	mkey_index;
	uint32_t	reserved;
	uint64_t	reserved2;
} __attribute__((aligned(8)));

/* ---------------------------------------------------------------------- */
/*                          uverbs ioctl shape                            */
/* ---------------------------------------------------------------------- */

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

/* Object / method / attr ids -- from include/uapi/rdma/ib_user_ioctl_cmds.h. */
#define UVERBS_OBJECT_DEVICE			0
#define UVERBS_OBJECT_PD			1
#define UVERBS_OBJECT_MR			7
#define UVERBS_OBJECT_RESTORE			18

#define UVERBS_METHOD_INFO_HANDLES		1
#define UVERBS_ATTR_INFO_OBJECT_ID		0
#define UVERBS_ATTR_INFO_TOTAL_HANDLES		1
#define UVERBS_ATTR_INFO_HANDLES_LIST		2

/*
 * enum uverbs_methods_restore order (ib_user_ioctl_cmds.h):
 *   RESTORE_PD=0, RESTORE_MR=1, RESTORE_CQ=2, RESTORE_QP=3,
 *   RESTORE_MR_DMABUF=4.
 */
#define UVERBS_METHOD_RESTORE_PD		0
#define UVERBS_METHOD_RESTORE_MR_DMABUF	4
#define UVERBS_ATTR_RESTORE_PD_HANDLE		0

enum {
	UVERBS_ATTR_RESTORE_MR_DMABUF_HANDLE		= 0,
	UVERBS_ATTR_RESTORE_MR_DMABUF_PD_HANDLE	= 1,
	UVERBS_ATTR_RESTORE_MR_DMABUF_FD		= 2,
	UVERBS_ATTR_RESTORE_MR_DMABUF_OFFSET		= 3,
	UVERBS_ATTR_RESTORE_MR_DMABUF_LENGTH		= 4,
	UVERBS_ATTR_RESTORE_MR_DMABUF_IOVA		= 5,
	UVERBS_ATTR_RESTORE_MR_DMABUF_ACCESS_FLAGS	= 6,
	UVERBS_ATTR_RESTORE_MR_DMABUF_LKEY_HINT	= 7,
	UVERBS_ATTR_RESTORE_MR_DMABUF_RKEY_HINT	= 8,
	UVERBS_ATTR_RESTORE_MR_DMABUF_RESP_LKEY	= 9,
	UVERBS_ATTR_RESTORE_MR_DMABUF_RESP_RKEY	= 10,
};

#define UVERBS_ATTR_UHW_IN			((uint16_t)4096)

#define RDMA_DRIVER_MLX5			1

#define DEFAULT_MR_TARGET_HANDLE		0x4242u
#define DEFAULT_PD_TARGET_HANDLE		0x4241u

#define MLX5_TOTAL_NUM_BFREGS			8u
#define MLX5_NUM_LOW_LATENCY_BFREGS		0u

/* ---------------------------------------------------------------------- */
/*                           legacy-write helpers                          */
/* ---------------------------------------------------------------------- */

static int do_get_context(int fd, uint32_t mlx5_flags,
			  struct ib_uverbs_get_context_resp *resp_core,
			  struct mlx5_ib_alloc_ucontext_resp *resp_drv)
{
	struct {
		struct ib_uverbs_cmd_hdr		hdr;
		struct ib_uverbs_get_context		core;
		struct mlx5_ib_alloc_ucontext_req_v2	req;
	} cmd = {};
	struct {
		struct ib_uverbs_get_context_resp	core;
		struct mlx5_ib_alloc_ucontext_resp	drv;
	} resp = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_GET_CONTEXT;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= sizeof(resp) / 4;
	cmd.core.response	= (uintptr_t)&resp;

	cmd.req.total_num_bfregs	= MLX5_TOTAL_NUM_BFREGS;
	cmd.req.num_low_latency_bfregs	= MLX5_NUM_LOW_LATENCY_BFREGS;
	cmd.req.flags			= mlx5_flags;
	cmd.req.max_cqe_version		= 1;
	cmd.req.lib_caps		= MLX5_LIB_CAP_4K_UAR;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	*resp_core = resp.core;
	*resp_drv = resp.drv;
	return 0;
}

struct mlx5_ib_alloc_pd_resp_drv {
	uint32_t	pdn;
};

static int do_alloc_pd(int fd, uint32_t *pd_handle_out)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_alloc_pd	core;
	} __attribute__((packed)) cmd = {};
	struct {
		struct ib_uverbs_alloc_pd_resp		core;
		struct mlx5_ib_alloc_pd_resp_drv	drv;
	} __attribute__((packed)) resp = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_ALLOC_PD;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= sizeof(resp) / 4;
	cmd.core.response	= (uintptr_t)&resp;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	*pd_handle_out = resp.core.pd_handle;
	return 0;
}

static int do_dereg_mr(int fd, uint32_t mr_handle)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_dereg_mr	core;
	} __attribute__((packed)) cmd = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_DEREG_MR;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= 0;
	cmd.core.mr_handle	= mr_handle;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	return 0;
}

/* ---------------------------------------------------------------------- */
/*                         uverbs ioctl helpers                           */
/* ---------------------------------------------------------------------- */

static int do_restore_pd(int fd, uint32_t target_handle,
			 const struct mlx5_ib_restore_pd_req *req)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[2];
	} cmd = {};

	cmd.hdr.object_id = UVERBS_OBJECT_RESTORE;
	cmd.hdr.method_id = UVERBS_METHOD_RESTORE_PD;
	cmd.hdr.driver_id = RDMA_DRIVER_MLX5;
	cmd.hdr.num_attrs = 2;
	cmd.hdr.length    = sizeof(cmd);

	cmd.attrs[0].attr_id	= UVERBS_ATTR_RESTORE_PD_HANDLE;
	cmd.attrs[0].len	= sizeof(uint32_t);
	cmd.attrs[0].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[0].data	= target_handle;

	cmd.attrs[1].attr_id	= UVERBS_ATTR_UHW_IN;
	cmd.attrs[1].len	= sizeof(*req);
	cmd.attrs[1].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[1].data	= (uintptr_t)req;

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

struct restore_mr_response {
	uint32_t	lkey;
	uint32_t	rkey;
};

static int do_restore_mr_dmabuf(int fd, uint32_t target_handle,
				uint32_t pd_handle, uint32_t dmabuf_fd,
				uint64_t offset, uint64_t length,
				uint64_t iova, uint32_t access_flags,
				uint32_t lkey_hint, uint32_t rkey_hint,
				const struct mlx5_ib_restore_mr_dmabuf_req *uhw,
				struct restore_mr_response *resp_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[12];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id = UVERBS_OBJECT_RESTORE;
	cmd.hdr.method_id = UVERBS_METHOD_RESTORE_MR_DMABUF;
	cmd.hdr.driver_id = RDMA_DRIVER_MLX5;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_DMABUF_HANDLE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= target_handle;
	n++;

	/* IDR attr: len == 0, data = ufile handle. See do_restore_mr's
	 * comment in mr_restore_probe_mlx5_vfmig.c for why nonzero len
	 * on an IDR attr would -EINVAL before the dispatcher runs. */
	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_DMABUF_PD_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= pd_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_DMABUF_FD;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= dmabuf_fd;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_DMABUF_OFFSET;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= offset;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_DMABUF_LENGTH;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= length;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_DMABUF_IOVA;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= iova;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_DMABUF_ACCESS_FLAGS;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= access_flags;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_DMABUF_LKEY_HINT;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= lkey_hint;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_DMABUF_RKEY_HINT;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= rkey_hint;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_UHW_IN;
	cmd.attrs[n].len	= sizeof(*uhw);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)uhw;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_DMABUF_RESP_LKEY;
	cmd.attrs[n].len	= sizeof(resp_out->lkey);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)&resp_out->lkey;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_DMABUF_RESP_RKEY;
	cmd.attrs[n].len	= sizeof(resp_out->rkey);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)&resp_out->rkey;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length    = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

static int do_info_handles_mr(int fd, uint32_t *handles_out,
			      uint32_t capacity_handles, uint32_t *total_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[3];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id = UVERBS_OBJECT_DEVICE;
	cmd.hdr.method_id = UVERBS_METHOD_INFO_HANDLES;
	cmd.hdr.driver_id = RDMA_DRIVER_MLX5;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_INFO_OBJECT_ID;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= UVERBS_OBJECT_MR;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_INFO_TOTAL_HANDLES;
	cmd.attrs[n].len	= sizeof(*total_out);
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)total_out;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_INFO_HANDLES_LIST;
	cmd.attrs[n].len	= (uint16_t)(capacity_handles * sizeof(uint32_t));
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)handles_out;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length    = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

static bool handle_present(const uint32_t *list, uint32_t n, uint32_t want)
{
	for (uint32_t i = 0; i < n; i++)
		if (list[i] == want)
			return true;
	return false;
}

/* ---------------------------------------------------------------------- */
/*                          device discovery                              */
/* ---------------------------------------------------------------------- */

static int resolve_cdev_path(const char *ibdev_name, char *out, size_t outlen)
{
	struct ibv_device **list;
	int n, i, ret = -ENODEV;

	list = ibv_get_device_list(&n);
	if (!list || n == 0) {
		fprintf(stderr,
			"mr_restore_dmabuf: ibv_get_device_list returned 0\n");
		return -ENODEV;
	}
	for (i = 0; i < n; i++) {
		if (strcmp(ibv_get_device_name(list[i]), ibdev_name) == 0) {
			snprintf(out, outlen, "/dev/infiniband/%s",
				 list[i]->dev_name);
			ret = 0;
			break;
		}
	}
	if (ret) {
		fprintf(stderr,
			"mr_restore_dmabuf: ibdev '%s' not found; available:",
			ibdev_name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	ibv_free_device_list(list);
	return ret;
}

/* ---------------------------------------------------------------------- */
/*                     CUDA VMM dma-buf allocation                        */
/* ---------------------------------------------------------------------- */

#define CHECK_CU(call)                                                       \
	do {                                                                   \
		CUresult _res = (call);                                          \
		if (_res != CUDA_SUCCESS) {                                      \
			const char *_err = NULL;                                     \
			cuGetErrorString(_res, &_err);                               \
			fprintf(stderr, "mr_restore_dmabuf: CUDA error at %s:%d: %s (%d): %s\n", \
				__FILE__, __LINE__, #call, _res,                         \
				_err ? _err : "unknown");                                \
			return 2;                                                    \
		}                                                                  \
	} while (0)

/*
 * Allocates a fresh GPU buffer and exports it as a dma-buf, same
 * technique as gpu-dmabuf-criu-plan.md's step0_gpu_dmabuf_test.c.
 * Returns 0 on success (fills *dmabuf_fd_out and *aligned_size_out),
 * nonzero on failure (already logged).
 */
static int alloc_gpu_dmabuf(int gpu_ordinal, uint64_t requested_size,
			    int *dmabuf_fd_out, uint64_t *aligned_size_out)
{
	CUdevice dev;
	CUcontext ctx;
	CUmemAllocationProp prop;
	CUmemGenericAllocationHandle handle;
	CUmemAccessDesc access_desc;
	CUdeviceptr ptr;
	size_t granularity = 0;
	size_t size;
	int dmabuf_fd = -1;

	CHECK_CU(cuInit(0));
	CHECK_CU(cuDeviceGet(&dev, gpu_ordinal));
	CHECK_CU(cuCtxCreate(&ctx, NULL, 0, dev));

	memset(&prop, 0, sizeof(prop));
	prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
	prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
	prop.location.id = dev;

	CHECK_CU(cuMemGetAllocationGranularity(&granularity, &prop,
						CU_MEM_ALLOC_GRANULARITY_MINIMUM));
	size = ((requested_size + granularity - 1) / granularity) * granularity;

	CHECK_CU(cuMemCreate(&handle, size, &prop, 0));
	CHECK_CU(cuMemAddressReserve(&ptr, size, 0, 0, 0));
	CHECK_CU(cuMemMap(ptr, size, 0, handle, 0));

	memset(&access_desc, 0, sizeof(access_desc));
	access_desc.location = prop.location;
	access_desc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
	CHECK_CU(cuMemSetAccess(ptr, size, &access_desc, 1));

	/*
	 * gpu-dmabuf-criu-3f-restore-injection-design.md Phase 4: this
	 * call is what the LD_PRELOAD interposer
	 * (plugins/cuda/gpu_dmabuf_va_shim.c, Trees/criu) observes to
	 * capture this process's own CUdeviceptr for
	 * RESTORE_GPU_MRS_TEST -- run this probe with
	 * LD_PRELOAD=.../libgpu_dmabuf_va_shim.so for that mode to have
	 * anything to read. (An earlier version of this code skipped this
	 * call under RESTORE_GPU_MRS_TEST, based on a "second export on
	 * the same range fails" theory that further investigation proved
	 * wrong -- the real, since-fixed problem was that the VA can't be
	 * rediscovered at all via /proc/pid/maps, not export idempotency.
	 * See the design doc's VA-discovery investigation section.)
	 */
	CHECK_CU(cuMemGetHandleForAddressRange(
		&dmabuf_fd, ptr, size, CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0));

	printf("mr_restore_dmabuf: allocated fresh GPU buffer, %zu bytes, dma-buf fd=%d\n",
	       size, dmabuf_fd);

	*dmabuf_fd_out = dmabuf_fd;
	*aligned_size_out = (uint64_t)size;
	return 0;
}

/* ---------------------------------------------------------------------- */
/*                              subtests                                  */
/* ---------------------------------------------------------------------- */

struct mr_args {
	uint32_t	src_pdn;
	uint32_t	src_mkey_index;	/* (src_lkey >> 8) by mlx5 invariant */
	uint32_t	src_lkey;	/* full key, equals rkey in mlx5 */
	uint64_t	src_length;
	uint32_t	src_access_flags;
	uint32_t	mr_target_handle;
	uint32_t	pd_target_handle;
	int		gpu_ordinal;
	/*
	 * Fresh dma-buf allocated on THIS host (destination), exported
	 * via CUDA VMM. Unlike RESTORE_MR's local_addr (an anonymous
	 * mmap standing in for CRIU's mm replay), this dma-buf fd IS the
	 * real thing a CRIU restore would allocate at §3f time -- there
	 * is no host-VA equivalent for GPU memory to fake around.
	 */
	int		dmabuf_fd;
	uint64_t	dmabuf_size;	/* granularity-aligned; >= src_length */
};

static int mr_args_alloc_dmabuf(struct mr_args *a)
{
	return alloc_gpu_dmabuf(a->gpu_ordinal, a->src_length,
				&a->dmabuf_fd, &a->dmabuf_size);
}

/*
 * Subtest 1: gate-negative. Same technique as RESTORE_MR's: a
 * throwaway ALLOC_PD on a non-restore-mode ucontext gets us past IDR
 * resolution so the dispatcher's -EPERM gate actually fires. The
 * dmabuf fd doesn't need to be real here -- attrs 2 (FD) and offset
 * are plain PTR_IN u32/u64, not FD-object-resolved, so an invalid fd
 * value never gets dereferenced before the gate check runs (gate is
 * literally the first line of the handler, before any UHW/PTR_IN
 * copy_from).
 */
static int subtest_gate_negative(const char *cdev_path,
				 const struct mr_args *a)
{
	struct ib_uverbs_get_context_resp resp_core = {};
	struct mlx5_ib_alloc_ucontext_resp resp_drv = {};
	struct mlx5_ib_restore_mr_dmabuf_req uhw = {
		.mkey_index = a->src_mkey_index,
	};
	struct restore_mr_response resp = {};
	uint32_t throwaway_pd_handle = 0;
	int fd, ret;
	int fails = 0;

	printf("[1] gate: ucontext WITHOUT VFMIG_RESTORE -> RESTORE_MR_DMABUF must -EPERM\n");

	fd = open(cdev_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "  FAIL open(%s): %s\n", cdev_path,
			strerror(errno));
		return 1;
	}
	ret = do_get_context(fd, 0, &resp_core, &resp_drv);
	if (ret) {
		fprintf(stderr, "  FAIL GET_CONTEXT(flags=0): %s\n",
			strerror(-ret));
		close(fd);
		return 1;
	}
	ret = do_alloc_pd(fd, &throwaway_pd_handle);
	if (ret) {
		fprintf(stderr,
			"  FAIL ALLOC_PD on non-restore-mode ucontext: %s\n",
			strerror(-ret));
		close(fd);
		return 1;
	}

	ret = do_restore_mr_dmabuf(fd, a->mr_target_handle,
				   throwaway_pd_handle, a->dmabuf_fd, 0,
				   a->src_length, 0, a->src_access_flags,
				   a->src_lkey, a->src_lkey, &uhw, &resp);
	if (ret == -EPERM) {
		printf("  PASS RESTORE_MR_DMABUF on non-restore-mode ucontext -> -EPERM\n");
	} else if (ret == 0) {
		fprintf(stderr,
			"  FAIL RESTORE_MR_DMABUF on non-restore-mode ucontext succeeded "
			"(security regression: predicate not consulted)\n");
		fails++;
	} else {
		fprintf(stderr,
			"  FAIL RESTORE_MR_DMABUF on non-restore-mode ucontext -> %s "
			"(expected -EPERM)\n", strerror(-ret));
		fails++;
	}

	close(fd);
	return fails;
}

static int expect_restore_mr_dmabuf_errno(int fd, const struct mr_args *a,
					  const struct mlx5_ib_restore_mr_dmabuf_req *uhw,
					  uint32_t lkey_hint, uint32_t rkey_hint,
					  int want_errno, const char *desc)
{
	struct restore_mr_response resp = {};
	int ret = do_restore_mr_dmabuf(fd, a->mr_target_handle,
				       a->pd_target_handle, a->dmabuf_fd, 0,
				       a->src_length, 0, a->src_access_flags,
				       lkey_hint, rkey_hint, uhw, &resp);
	if (ret == want_errno) {
		printf("  PASS RESTORE_MR_DMABUF(%s) -> %s\n", desc,
		       strerror(-ret));
		return 0;
	}
	fprintf(stderr,
		"  FAIL RESTORE_MR_DMABUF(%s) -> %s (expected %s)\n",
		desc, ret ? strerror(-ret) : "0 (success)",
		strerror(-want_errno));
	return 1;
}

static int subtest_uapi_reject_zero_mkey(int fd, const struct mr_args *a)
{
	struct mlx5_ib_restore_mr_dmabuf_req uhw = { .mkey_index = 0 };

	printf("[2] uapi reject: mkey_index=0 must -EINVAL\n");
	return expect_restore_mr_dmabuf_errno(fd, a, &uhw, 0, 0, -EINVAL,
					      "mkey_index=0");
}

static int subtest_uapi_reject_reserved(int fd, const struct mr_args *a)
{
	struct mlx5_ib_restore_mr_dmabuf_req uhw = {
		.mkey_index = a->src_mkey_index,
		.reserved   = 0xDEADu,
	};

	printf("[3] uapi reject: reserved!=0 must -EINVAL\n");
	return expect_restore_mr_dmabuf_errno(fd, a, &uhw, a->src_lkey,
					      a->src_lkey, -EINVAL,
					      "reserved=0xDEAD");
}

static int subtest_uapi_reject_lkey_neq_rkey(int fd, const struct mr_args *a)
{
	struct mlx5_ib_restore_mr_dmabuf_req uhw = {
		.mkey_index = a->src_mkey_index,
	};

	printf("[4] uapi reject: lkey != rkey must -EINVAL (mlx5 invariant)\n");
	return expect_restore_mr_dmabuf_errno(fd, a, &uhw, a->src_lkey,
					      a->src_lkey ^ 1u, -EINVAL,
					      "lkey != rkey");
}

static int subtest_uapi_reject_lkey_mismatch_mkey(int fd,
						   const struct mr_args *a)
{
	struct mlx5_ib_restore_mr_dmabuf_req uhw = {
		.mkey_index = a->src_mkey_index,
	};
	uint32_t bogus_key = ((a->src_mkey_index ^ 0xfffu) << 8) |
			      (a->src_lkey & 0xffu);

	printf("[5] uapi reject: (lkey>>8) != mkey_index must -EINVAL\n");
	return expect_restore_mr_dmabuf_errno(fd, a, &uhw, bogus_key,
					      bogus_key, -EINVAL,
					      "(lkey>>8) != mkey_index");
}

static int subtest_prereq_restore_pd(int fd, const struct mr_args *a)
{
	struct mlx5_ib_restore_pd_req req = { .pdn = a->src_pdn };
	int ret;

	printf("[*] prereq: RESTORE_PD(target=0x%x, pdn=%u)\n",
	       a->pd_target_handle, a->src_pdn);
	ret = do_restore_pd(fd, a->pd_target_handle, &req);
	if (ret) {
		fprintf(stderr,
			"  FAIL prereq RESTORE_PD: %s\n", strerror(-ret));
		return 1;
	}
	printf("  PASS prereq RESTORE_PD landed\n");
	return 0;
}

static int subtest_happy_path(int fd, const struct mr_args *a)
{
	struct mlx5_ib_restore_mr_dmabuf_req uhw = {
		.mkey_index = a->src_mkey_index,
	};
	struct restore_mr_response resp = {};
	uint32_t list[64] = {};
	uint32_t total = 0;
	int ret;
	int fails = 0;

	printf("[6] happy path: RESTORE_MR_DMABUF(mr_target=0x%x, pd_target=0x%x, "
	       "mkey_index=%u, lkey=0x%x, dmabuf_fd=%d)\n",
	       a->mr_target_handle, a->pd_target_handle, a->src_mkey_index,
	       a->src_lkey, a->dmabuf_fd);

	ret = do_restore_mr_dmabuf(fd, a->mr_target_handle, a->pd_target_handle,
				   a->dmabuf_fd, 0, a->src_length, 0,
				   a->src_access_flags, a->src_lkey,
				   a->src_lkey, &uhw, &resp);
	if (ret) {
		fprintf(stderr,
			"  FAIL RESTORE_MR_DMABUF(target=0x%x): %s%s\n",
			a->mr_target_handle, strerror(-ret),
			ret == -EOPNOTSUPP
			? "  (mlx5_ib_restore_mr_dmabuf not registered in mlx5_ib_dev_ops?)"
			: ret == -EPERM
			? "  (mlx5_ib_ucontext_is_restore_mode not reporting true?)"
			: ret == -ENOENT
			? "  (no awaiting_bind USER_MMIO placeholder for this mkey_index -- "
			  "did SAVE_VHCA_STATE/LOAD_VHCA_STATE actually run against a "
			  "source-side GPU dma-buf MR at this mkey_index first?)"
			: "");
		return 1;
	}

	if (resp.lkey != a->src_lkey || resp.rkey != a->src_lkey) {
		fprintf(stderr,
			"  FAIL wire-visible identity not honoured: hint lkey/rkey=0x%x but resp lkey=0x%x rkey=0x%x\n",
			a->src_lkey, resp.lkey, resp.rkey);
		fails++;
	} else {
		printf("  PASS RESP_LKEY=0x%x RESP_RKEY=0x%x byte-identical to hint (mlx5 honours)\n",
		       resp.lkey, resp.rkey);
	}

	ret = do_info_handles_mr(fd, list, 64, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(MR): %s\n", strerror(-ret));
		return 1;
	}
	if (!handle_present(list, total, a->mr_target_handle)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(MR): handle 0x%x not in list (total=%u)\n",
			a->mr_target_handle, total);
		fails++;
	} else {
		printf("  PASS INFO_HANDLES(MR) returned 0x%x among %u entries\n",
		       a->mr_target_handle, total);
	}
	return fails;
}

/*
 * Phase 3 of gpu-dmabuf-criu-3f-restore-injection-design.md: instead
 * of calling do_restore_mr_dmabuf() in-process (subtest_happy_path
 * above), print everything an EXTERNAL hijacker process needs and
 * block, so it can perform the RESTORE_MR_DMABUF ioctl itself via
 * ptrace call-injection against this already-set-up process -- the
 * same shape of thing cuda_plugin.c's resume_device() will eventually
 * need to do for real (Phase 4), but still standalone/not wired into
 * criu here. Strictly opt-in (HIJACK_INJECT_TEST=1) -- does not
 * change default behavior at all, so this cannot regress the existing
 * test_mr_adopt_dmabuf.sh flow.
 *
 * Verification split across both sides on purpose: the external
 * hijacker independently reads back the resp lkey/rkey from the
 * target scratch memory it wrote (same pattern as its dma-buf-fd
 * readback in Phase 2); THIS process verifies via INFO_HANDLES(MR)
 * after resuming, entirely through its own normal fd -- an
 * independent confirmation from a different code path than whatever
 * the hijacker itself believes happened.
 */
static int subtest_happy_path_wait_for_external_ioctl(int fd, const struct mr_args *a)
{
	uint32_t list[64] = {};
	uint32_t total = 0;
	int ret;

	printf("[6-inject] waiting for external RESTORE_MR_DMABUF injection\n");
	printf("inject_fd=%d\n", fd);
	printf("inject_mr_target_handle=0x%x\n", a->mr_target_handle);
	printf("inject_pd_target_handle=0x%x\n", a->pd_target_handle);
	printf("inject_dmabuf_fd=%d\n", a->dmabuf_fd);
	printf("inject_offset=0\n");
	printf("inject_length=%llu\n", (unsigned long long)a->src_length);
	printf("inject_iova=0\n");
	printf("inject_access_flags=%u\n", a->src_access_flags);
	printf("inject_lkey_hint=0x%x\n", a->src_lkey);
	printf("inject_rkey_hint=0x%x\n", a->src_lkey);
	printf("inject_mkey_index=%u\n", a->src_mkey_index);
	printf("INJECT_READY\n");
	fflush(stdout);

	{
		char line[64];
		while (fgets(line, sizeof(line), stdin)) {
			if (!strncmp(line, "go", 2))
				break;
		}
	}

	ret = do_info_handles_mr(fd, list, 64, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(MR): %s\n", strerror(-ret));
		return 1;
	}
	if (!handle_present(list, total, a->mr_target_handle)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(MR): handle 0x%x not in list (total=%u) after "
			"external injection -- injection did not land\n",
			a->mr_target_handle, total);
		return 1;
	}
	printf("  PASS INFO_HANDLES(MR) returned 0x%x among %u entries after external "
	       "injection\n", a->mr_target_handle, total);
	return 0;
}

static int subtest_collision(int fd, const struct mr_args *a)
{
	struct mlx5_ib_restore_mr_dmabuf_req uhw = {
		.mkey_index = a->src_mkey_index,
	};
	struct restore_mr_response resp = {};
	int ret;

	printf("[7] collision: second RESTORE_MR_DMABUF(target=0x%x) must -EBUSY\n",
	       a->mr_target_handle);

	ret = do_restore_mr_dmabuf(fd, a->mr_target_handle, a->pd_target_handle,
				   a->dmabuf_fd, 0, a->src_length, 0,
				   a->src_access_flags, a->src_lkey,
				   a->src_lkey, &uhw, &resp);
	if (ret == -EBUSY) {
		printf("  PASS second RESTORE_MR_DMABUF(target=0x%x) -> -EBUSY\n",
		       a->mr_target_handle);
		return 0;
	}
	fprintf(stderr,
		"  FAIL second RESTORE_MR_DMABUF(target=0x%x) -> %s (expected -EBUSY)\n",
		a->mr_target_handle, ret ? strerror(-ret) : "0 (success)");
	return 1;
}

static int subtest_v0_dealloc(int fd, const struct mr_args *a)
{
	uint32_t list[64] = {};
	uint32_t total = 0;
	int ret;

	printf("[8] v0 dealloc semantics: DEREG_MR(0x%x) on an orphan adopted\n"
	       "    mkey must succeed; INFO_HANDLES(MR) must drop it\n",
	       a->mr_target_handle);

	ret = do_dereg_mr(fd, a->mr_target_handle);
	if (ret) {
		fprintf(stderr,
			"  FAIL DEREG_MR(0x%x) -> %s (expected 0)\n",
			a->mr_target_handle, strerror(-ret));
		return 1;
	}
	printf("  PASS DEREG_MR(0x%x) -> 0\n", a->mr_target_handle);

	ret = do_info_handles_mr(fd, list, 64, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(MR): %s\n", strerror(-ret));
		return 1;
	}
	if (handle_present(list, total, a->mr_target_handle)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(MR) still reports 0x%x after a successful DEREG_MR\n",
			a->mr_target_handle);
		return 1;
	}
	printf("  PASS INFO_HANDLES(MR) no longer reports 0x%x (uobj freed; total=%u)\n",
	       a->mr_target_handle, total);
	return 0;
}

/* ---------------------------------------------------------------------- */
/*                                main                                    */
/* ---------------------------------------------------------------------- */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <ibdev> <src_pdn> <src_mkey_index> <src_lkey>\n"
		"           <src_mr_length> <src_access_flags>\n"
		"           [<mr_target_handle> [<pd_target_handle> [<cuda_device_id>]]]\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *ibdev;
	struct mr_args a = {
		.mr_target_handle = DEFAULT_MR_TARGET_HANDLE,
		.pd_target_handle = DEFAULT_PD_TARGET_HANDLE,
		.gpu_ordinal = 0,
	};
	char cdev_path[128];
	struct ib_uverbs_get_context_resp resp_core = {};
	struct mlx5_ib_alloc_ucontext_resp resp_drv = {};
	int fd_restore = -1, ret;
	int fails = 0;
	unsigned long u;

	if (argc < 7 || argc > 10) {
		usage(argv[0]);
		return 2;
	}
	ibdev = argv[1];

	u = strtoul(argv[2], NULL, 0);
	if (u == 0 || u > 0xffffffu) {
		fprintf(stderr,
			"mr_restore_dmabuf: src_pdn=%lu out of range (1..0xffffff)\n", u);
		return 2;
	}
	a.src_pdn = (uint32_t)u;

	u = strtoul(argv[3], NULL, 0);
	if (u == 0 || u > 0xffffffu) {
		fprintf(stderr,
			"mr_restore_dmabuf: src_mkey_index=%lu out of range (1..0xffffff)\n", u);
		return 2;
	}
	a.src_mkey_index = (uint32_t)u;

	u = strtoul(argv[4], NULL, 0);
	if (u > 0xffffffffu) {
		fprintf(stderr, "mr_restore_dmabuf: src_lkey out of u32 range\n");
		return 2;
	}
	a.src_lkey = (uint32_t)u;
	if ((a.src_lkey >> 8) != a.src_mkey_index) {
		fprintf(stderr,
			"mr_restore_dmabuf: src_lkey=0x%x does not encode src_mkey_index=%u\n"
			"                (mlx5 invariant: lkey == (mkey_index << 8) | variant)\n",
			a.src_lkey, a.src_mkey_index);
		return 2;
	}

	a.src_length = strtoull(argv[5], NULL, 0);
	if (a.src_length == 0) {
		fprintf(stderr, "mr_restore_dmabuf: src_mr_length must be > 0\n");
		return 2;
	}

	u = strtoul(argv[6], NULL, 0);
	if (u > 0xffu) {
		fprintf(stderr,
			"mr_restore_dmabuf: src_access_flags=0x%lx wider than 8 bits\n", u);
		return 2;
	}
	a.src_access_flags = (uint32_t)u;

	if (argc >= 8) {
		u = strtoul(argv[7], NULL, 0);
		if (u > 0xffffffffu) {
			fprintf(stderr, "mr_restore_dmabuf: mr_target_handle out of range\n");
			return 2;
		}
		a.mr_target_handle = (uint32_t)u;
	}
	if (argc >= 9) {
		u = strtoul(argv[8], NULL, 0);
		if (u > 0xffffffffu) {
			fprintf(stderr, "mr_restore_dmabuf: pd_target_handle out of range\n");
			return 2;
		}
		a.pd_target_handle = (uint32_t)u;
	}
	if (argc >= 10)
		a.gpu_ordinal = atoi(argv[9]);

	if (a.mr_target_handle == a.pd_target_handle) {
		fprintf(stderr,
			"mr_restore_dmabuf: mr_target_handle == pd_target_handle (0x%x); must differ\n",
			a.mr_target_handle);
		return 2;
	}

	if (resolve_cdev_path(ibdev, cdev_path, sizeof(cdev_path)) != 0)
		return 2;
	printf("mr_restore_dmabuf: ibdev=%s cdev=%s\n"
	       "                  src_pdn=%u src_mkey_index=%u src_lkey=0x%x\n"
	       "                  src_length=0x%llx access_flags=0x%x gpu_ordinal=%d\n"
	       "                  mr_target_handle=0x%x pd_target_handle=0x%x\n",
	       ibdev, cdev_path, a.src_pdn, a.src_mkey_index, a.src_lkey,
	       (unsigned long long)a.src_length, a.src_access_flags,
	       a.gpu_ordinal, a.mr_target_handle, a.pd_target_handle);

	if (mr_args_alloc_dmabuf(&a) != 0)
		return 2;

	/* Subtest 1 uses its own short-lived non-restore-mode ucontext. */
	fails += subtest_gate_negative(cdev_path, &a);

	/* Subtests 2-7 share one restore-mode ucontext. */
	fd_restore = open(cdev_path, O_RDWR | O_CLOEXEC);
	if (fd_restore < 0) {
		fprintf(stderr,
			"mr_restore_dmabuf: open(%s) for restore-mode ucontext: %s\n",
			cdev_path, strerror(errno));
		return 2;
	}
	ret = do_get_context(fd_restore, MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
			     &resp_core, &resp_drv);
	if (ret) {
		fprintf(stderr,
			"mr_restore_dmabuf: GET_CONTEXT(flags=VFMIG_RESTORE): %s\n",
			strerror(-ret));
		close(fd_restore);
		return 2;
	}

	if (subtest_prereq_restore_pd(fd_restore, &a)) {
		close(fd_restore);
		return 1;
	}

	fails += subtest_uapi_reject_zero_mkey(fd_restore, &a);
	fails += subtest_uapi_reject_reserved(fd_restore, &a);
	fails += subtest_uapi_reject_lkey_neq_rkey(fd_restore, &a);
	fails += subtest_uapi_reject_lkey_mismatch_mkey(fd_restore, &a);
	if (getenv("HIJACK_INJECT_TEST") || getenv("RESTORE_GPU_MRS_TEST"))
		fails += subtest_happy_path_wait_for_external_ioctl(fd_restore, &a);
	else
		fails += subtest_happy_path(fd_restore, &a);
	fails += subtest_collision(fd_restore, &a);

	if (fails) {
		fprintf(stderr,
			"\nmr_restore_probe_mlx5_vfmig_dmabuf: %d subtest failure(s) "
			"before READY; aborting\n", fails);
		close(fd_restore);
		return 1;
	}

	printf("ibdev=%s\n", ibdev);
	printf("adopted_pdn=%u\n", a.src_pdn);
	printf("adopted_mkey_index=%u\n", a.src_mkey_index);
	printf("mr_target_handle=0x%x\n", a.mr_target_handle);
	printf("pd_target_handle=0x%x\n", a.pd_target_handle);
	printf("READY\n");
	fflush(stdout);

	{
		char line[64];
		while (fgets(line, sizeof(line), stdin)) {
			if (!strncmp(line, "quit", 4))
				break;
		}
	}

	fails += subtest_v0_dealloc(fd_restore, &a);

	close(fd_restore);

	if (fails) {
		fprintf(stderr,
			"\nmr_restore_probe_mlx5_vfmig_dmabuf: FAIL (%d subtest failure(s))\n",
			fails);
		return 1;
	}
	printf("\nmr_restore_probe_mlx5_vfmig_dmabuf: PASS\n");
	return 0;
}
