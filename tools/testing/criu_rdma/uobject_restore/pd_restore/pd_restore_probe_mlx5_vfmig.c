// SPDX-License-Identifier: GPL-2.0
/*
 * pd_restore_probe_mlx5_vfmig -- end-to-end driver-level validation
 * of S3b: UVERBS_METHOD_RESTORE_PD + mlx5_ib_restore_pd
 * (design/uobject_restore.md §7.3 + §9.1 S3b).
 *
 * What we validate against a bound, post-LOAD destination mlx5
 * ib_device:
 *
 *   1. Gate (negative). A ucontext opened WITHOUT
 *      MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE cannot invoke RESTORE_PD;
 *      the dispatcher must return -EPERM via the per-driver
 *      mlx5_ib_ucontext_is_restore_mode predicate.
 *   2. UAPI rejection (zero pdn). RESTORE_PD with the
 *      driver-private UHW payload mlx5_ib_restore_pd_req.pdn = 0
 *      must -EINVAL. pdn=0 belongs to the kernel-internal
 *      mlx5_ib_dev_res->p0 slot, which no userspace ucontext
 *      should ever claim.
 *   3. UAPI rejection (reserved nonzero). RESTORE_PD with
 *      mlx5_ib_restore_pd_req.reserved = 0xDEAD must -EINVAL.
 *      Defends the future-flags slot.
 *   4. Happy path. A ucontext opened WITH the flag can adopt the
 *      caller-supplied source pdn at the caller-chosen ufile
 *      handle. Cross-check via UVERBS_METHOD_INFO_HANDLES that the
 *      handle shows up in the PD list.
 *   5. Collision. A second RESTORE_PD(target_handle, pdn) on the
 *      same ucontext returns -EBUSY from the xa_insert collision
 *      inside rdma_alloc_begin_uobject_at_handle.
 *   6. READY checkpoint. The probe prints
 *
 *          ibdev=<n>
 *          adopted_pdn=<pdn>
 *          target_handle=<h>
 *          READY
 *
 *      and blocks on stdin until it reads "quit\n". While the probe
 *      is parked the adopted PD is still alive in firmware, so the
 *      driver script can:
 *
 *        - issue MLX5_VFMIG_IOC_PROBE_PD via the PF cdev to confirm
 *          FW-side liveness (CREATE_MKEY accepting the adopted
 *          pdn under the new ucontext's uid scope), or
 *        - drive any other follow-on verification against the
 *          destination ibdev.
 *
 *      On "quit\n" the probe drops to subtest 7.
 *   7. v0 dealloc semantics. IB_USER_VERBS_CMD_DEALLOC_PD on the
 *      adopted target_handle must FAIL with -EINVAL (FW status
 *      BAD_RES_STATE, mapped by cmd_status_to_err) because
 *      LOAD_VHCA_STATE also carried over the source's pdn-rooted
 *      CQ/QP/MR/SRQ. v0 has not restored kernel uobjects for those,
 *      so the kernel cannot dealloc them first, and FW correctly
 *      rejects an orphan DEALLOC_PD. INFO_HANDLES must still report
 *      target_handle: uverbs_destroy_uobject leaves the uobj parked
 *      on destroy_hw failure, waiting for the future
 *      RESTORE_CQ/MR/QP/SRQ cascades (S4..S7) to complete the
 *      teardown chain. This subtest is the orphan-PD analogue of
 *      the alloc/dealloc round-trip; it locks in the correct
 *      restore-ordering invariant for CRIU.
 *
 * The probe deliberately stays libibverbs-only and pure-uverbs (no
 * libmlx5, no mlx5dv) so it can drive the
 * MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE flag in udata (libmlx5 has no
 * plumbing for it) and the new mlx5_ib_restore_pd_req UHW payload.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/pd_restore/pd_restore_probe_mlx5_vfmig
 *
 * Usage:
 *   ./pd_restore_probe_mlx5_vfmig <ibdev> <src_pdn> [<target_handle>]
 *
 * <ibdev>           destination ib_device name (e.g. mlx5_2),
 *                   typically picked up from `find_ib_dev_for_pci
 *                   <dst_vf_bdf>` in the harness.
 * <src_pdn>         FW pdn captured pre-SAVE on the source
 *                   ucontext, typically the src_pdn key in
 *                   fw_id_continuity_probe's READY-line dictionary.
 * <target_handle>   ufile handle to install the adopted PD at.
 *                   Defaults to 0x4242. CRIU will use the source
 *                   ucontext's actual ufile handle for each PD here.
 *
 * No dependency on libmlx5 or rdma-core's userspace mlx5 UAPI: the
 * mlx5-side ABI surface we need (GET_CONTEXT req/resp shape, the new
 * RESTORE_PD UHW struct, the VFMIG flag) is inlined below. Keep in
 * sync with the authoritative headers at include/uapi/rdma/mlx5-abi.h
 * and include/uapi/rdma/ib_user_verbs.h.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <rdma/ib_user_verbs.h>

/* ---------------------------------------------------------------------- */
/*                       mlx5 alloc-ucontext ABI                          */
/* ---------------------------------------------------------------------- */

/*
 * Mirrors include/uapi/rdma/mlx5-abi.h. Inlined to avoid a hard
 * dependency on a freshly-`make headers_install`-d userspace tree
 * (the installed rdma-core uapi lags the in-tree kernel headers
 * for the new MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE flag).
 */
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

/*
 * Mirrors the C3 commit in include/uapi/rdma/mlx5-abi.h.
 * Driver-private UHW payload for UVERBS_METHOD_RESTORE_PD on mlx5.
 *
 * The struct is deliberately > 8 bytes so the kernel's uverbs UHW
 * dispatch takes the ptr path rather than the inline path:
 * len <= sizeof(u64) makes uverbs_fill_udata() set
 * udata->inbuf to a kernel-side staging slot, which
 * ib_copy_from_udata()'s copy_from_user() then clamps via
 * masked-user-access and zero-fills the destination. Keeping
 * the struct at 16 bytes forces udata->inbuf to be a real
 * userspace pointer.
 */
struct mlx5_ib_restore_pd_req {
	uint32_t	pdn;
	uint32_t	reserved;
	uint64_t	reserved2;	/* __aligned_u64 in kernel UAPI */
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
#define UVERBS_OBJECT_RESTORE			18

#define UVERBS_METHOD_INFO_HANDLES		1
#define UVERBS_ATTR_INFO_OBJECT_ID		0
#define UVERBS_ATTR_INFO_TOTAL_HANDLES		1
#define UVERBS_ATTR_INFO_HANDLES_LIST		2

#define UVERBS_METHOD_RESTORE_PD		0
#define UVERBS_ATTR_RESTORE_PD_HANDLE		0

/*
 * UHW is the generic driver-private blob attribute. attr_id 4096 is
 * reserved for it -- see UVERBS_ATTR_UHW_IN / UVERBS_ID_NS_SHIFT in
 * include/uapi/rdma/ib_user_ioctl_cmds.h. Marked as the namespace
 * boundary the kernel uses to split core attrs from driver attrs.
 */
#define UVERBS_ATTR_UHW_IN			((uint16_t)4096)

/*
 * Matches enum rdma_driver_id in
 * include/uapi/rdma/ib_user_ioctl_verbs.h. UNKNOWN=0, MLX5=1,
 * MLX4=2, ... The kernel's uverbs ioctl dispatcher uses
 * (object_id | method_id | attr_id | driver_id) to key the
 * radix-tree slot for every method; an unrecognized driver_id
 * gives -EINVAL on the very first attr lookup regardless of
 * whether the verb itself is a core (driver-id-agnostic) method.
 */
#define RDMA_DRIVER_MLX5			1

#define DEFAULT_TARGET_HANDLE			0x4242u

/*
 * Default UCTX shape that mirrors what libmlx5 sends today for a
 * plain ibv_open_device on a stock mlx5 VF: 8 bfregs total, the
 * 4K-UAR cap, max_cqe_version = 1. We're not exercising the UAR
 * path, so the static-bfreg defaults are sufficient.
 */
#define MLX5_TOTAL_NUM_BFREGS			8u
#define MLX5_NUM_LOW_LATENCY_BFREGS		0u

/* ---------------------------------------------------------------------- */
/*                           legacy-write helpers                          */
/* ---------------------------------------------------------------------- */

/*
 * Issue IB_USER_VERBS_CMD_GET_CONTEXT on the freshly-opened uverbs
 * cdev fd, with a mlx5_ib_alloc_ucontext_req_v2 in udata. The mlx5
 * flag is the only thing that varies between the gate-negative and
 * happy-path subtests, so it's the lone knob. Returns 0 on success,
 * -errno on failure.
 *
 * in_words / out_words are TOTAL payload (including hdr) in 4-byte
 * units. See verify_hdr() in drivers/infiniband/core/uverbs_main.c:
 * the legacy non-ex path requires `hdr.in_words * 4 == count`
 * (= total bytes passed to write()).
 */
static int do_get_context(int fd, uint32_t mlx5_flags,
			  struct ib_uverbs_get_context_resp *resp_core,
			  struct mlx5_ib_alloc_ucontext_resp *resp_drv)
{
	/*
	 * Members are already self-aligned (cmd_hdr = 8 bytes,
	 * ib_uverbs_get_context starts on an 8-byte boundary,
	 * mlx5_ib_alloc_ucontext_req_v2 begins at offset 16 and is
	 * 8-aligned). Not using __attribute__((packed)) here -- adding
	 * it on top of an aligned(8) inner struct triggers
	 * -Wpacked-not-aligned for no benefit.
	 */
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

static int do_dealloc_pd(int fd, uint32_t pd_handle)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_dealloc_pd	core;
	} __attribute__((packed)) cmd = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_DEALLOC_PD;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= 0;
	cmd.core.pd_handle	= pd_handle;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	return 0;
}

/* ---------------------------------------------------------------------- */
/*                             ioctl helpers                              */
/* ---------------------------------------------------------------------- */

/*
 * Invoke UVERBS_METHOD_RESTORE_PD on the given uverbs cdev fd.
 * The (independent) ufile target_handle is carried by the core
 * UVERBS_ATTR_RESTORE_PD_HANDLE attribute; the driver-private
 * mlx5_ib_restore_pd_req travels in the UHW blob.
 *
 * Returns 0 on success, -errno on ioctl failure.
 */
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

	/*
	 * PTR_IN(__u32) attr. Kernel decides inline-vs-pointer by
	 * comparing attr->len against sizeof(u64); len <= 8 means the
	 * payload is packed inline in attr->data. uverbs_copy_from then
	 * memcpys the low len bytes into the target. See
	 * uverbs_attr_ptr_is_inline() in include/rdma/uverbs_ioctl.h.
	 */
	cmd.attrs[0].attr_id	= UVERBS_ATTR_RESTORE_PD_HANDLE;
	cmd.attrs[0].len	= sizeof(uint32_t);
	cmd.attrs[0].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[0].data	= target_handle;

	/*
	 * UHW driver-private blob. Kernel translates this into
	 * attrs->driver_udata for the driver callback. struct is 8
	 * bytes so it fits inline in attr->data... but we pass it by
	 * pointer here to keep the (pdn, reserved) layout explicit
	 * and to match how a larger future UHW payload would look.
	 */
	cmd.attrs[1].attr_id	= UVERBS_ATTR_UHW_IN;
	cmd.attrs[1].len	= sizeof(*req);
	cmd.attrs[1].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[1].data	= (uintptr_t)req;

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

/*
 * Issue INFO_HANDLES(UVERBS_OBJECT_PD) and copy up to
 * capacity_handles entries into handles_out, plus the total filled
 * count into *total_out. Mirrors info_handles_probe.c.
 */
static int do_info_handles_pd(int fd, uint32_t *handles_out,
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
	cmd.attrs[n].data	= UVERBS_OBJECT_PD;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_INFO_TOTAL_HANDLES;
	cmd.attrs[n].len	= sizeof(*total_out);
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)total_out;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_INFO_HANDLES_LIST;
	cmd.attrs[n].len	= (uint16_t)(capacity_handles *
					     sizeof(uint32_t));
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)handles_out;
	n++;

	cmd.hdr.num_attrs	= n;
	cmd.hdr.length		= sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

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
			"pd_restore_mlx5: ibv_get_device_list returned 0\n");
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
			"pd_restore_mlx5: ibdev '%s' not found; available:",
			ibdev_name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	ibv_free_device_list(list);
	return ret;
}

/* ---------------------------------------------------------------------- */
/*                              subtests                                  */
/* ---------------------------------------------------------------------- */

static int subtest_gate_negative(const char *cdev_path, uint32_t src_pdn,
				 uint32_t target_handle)
{
	struct ib_uverbs_get_context_resp resp_core = {};
	struct mlx5_ib_alloc_ucontext_resp resp_drv = {};
	struct mlx5_ib_restore_pd_req req = {
		.pdn = src_pdn,
		.reserved = 0,
	};
	int fd, ret;
	int fails = 0;

	printf("[1] gate: ucontext WITHOUT VFMIG_RESTORE -> RESTORE_PD must -EPERM\n");

	fd = open(cdev_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "  FAIL open(%s): %s\n", cdev_path,
			strerror(errno));
		return 1;
	}

	ret = do_get_context(fd, 0, &resp_core, &resp_drv);
	if (ret) {
		fprintf(stderr,
			"  FAIL GET_CONTEXT(flags=0): %s\n", strerror(-ret));
		close(fd);
		return 1;
	}

	ret = do_restore_pd(fd, target_handle, &req);
	if (ret == -EPERM) {
		printf("  PASS RESTORE_PD on non-restore-mode ucontext -> -EPERM\n");
	} else if (ret == 0) {
		fprintf(stderr,
			"  FAIL RESTORE_PD on non-restore-mode ucontext succeeded "
			"(security regression: predicate not consulted)\n");
		fails++;
	} else {
		fprintf(stderr,
			"  FAIL RESTORE_PD on non-restore-mode ucontext -> %s "
			"(expected -EPERM)\n", strerror(-ret));
		fails++;
	}

	close(fd);
	return fails;
}

static int subtest_uapi_reject_zero_pdn(int fd, uint32_t target_handle)
{
	struct mlx5_ib_restore_pd_req req = { .pdn = 0, .reserved = 0 };
	int ret;

	printf("[2] uapi reject: pdn=0 must -EINVAL\n");
	ret = do_restore_pd(fd, target_handle, &req);
	if (ret == -EINVAL) {
		printf("  PASS RESTORE_PD(pdn=0) -> -EINVAL\n");
		return 0;
	}
	fprintf(stderr,
		"  FAIL RESTORE_PD(pdn=0) -> %s (expected -EINVAL)\n",
		ret ? strerror(-ret) : "0 (success)");
	return 1;
}

static int subtest_uapi_reject_reserved(int fd, uint32_t src_pdn,
					uint32_t target_handle)
{
	struct mlx5_ib_restore_pd_req req = {
		.pdn = src_pdn,
		.reserved = 0xDEADu,
	};
	int ret;

	printf("[3] uapi reject: reserved!=0 must -EINVAL\n");
	ret = do_restore_pd(fd, target_handle, &req);
	if (ret == -EINVAL) {
		printf("  PASS RESTORE_PD(reserved=0xDEAD) -> -EINVAL\n");
		return 0;
	}
	fprintf(stderr,
		"  FAIL RESTORE_PD(reserved=0xDEAD) -> %s (expected -EINVAL)\n",
		ret ? strerror(-ret) : "0 (success)");
	return 1;
}

static int subtest_happy_path(int fd, uint32_t src_pdn, uint32_t target_handle)
{
	struct mlx5_ib_restore_pd_req req = {
		.pdn = src_pdn,
		.reserved = 0,
	};
	uint32_t list[64] = {};
	uint32_t total = 0;
	int ret;

	printf("[4] happy path: RESTORE_PD(target=0x%x, pdn=%u)\n",
	       target_handle, src_pdn);

	ret = do_restore_pd(fd, target_handle, &req);
	if (ret) {
		fprintf(stderr,
			"  FAIL RESTORE_PD(target=0x%x, pdn=%u): %s%s\n",
			target_handle, src_pdn, strerror(-ret),
			ret == -EOPNOTSUPP
			? "  (mlx5_ib_restore_pd not registered in mlx5_ib_dev_ops?)"
			: ret == -EPERM
			? "  (mlx5_ib_ucontext_is_restore_mode not reporting true?)"
			: "");
		return 1;
	}
	printf("  PASS RESTORE_PD(target=0x%x, pdn=%u) -> 0\n",
	       target_handle, src_pdn);

	ret = do_info_handles_pd(fd, list, 64, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(PD): %s\n",
			strerror(-ret));
		return 1;
	}
	if (!handle_present(list, total, target_handle)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(PD): handle 0x%x not in list (total=%u)\n",
			target_handle, total);
		return 1;
	}
	printf("  PASS INFO_HANDLES(PD) returned 0x%x among %u entries\n",
	       target_handle, total);
	return 0;
}

static int subtest_collision(int fd, uint32_t src_pdn, uint32_t target_handle)
{
	struct mlx5_ib_restore_pd_req req = {
		.pdn = src_pdn,
		.reserved = 0,
	};
	int ret;

	printf("[5] collision: second RESTORE_PD(target=0x%x) must -EBUSY\n",
	       target_handle);

	ret = do_restore_pd(fd, target_handle, &req);
	if (ret == -EBUSY) {
		printf("  PASS second RESTORE_PD(target=0x%x) -> -EBUSY\n",
		       target_handle);
		return 0;
	}
	fprintf(stderr,
		"  FAIL second RESTORE_PD(target=0x%x) -> %s (expected -EBUSY)\n",
		target_handle, ret ? strerror(-ret) : "0 (success)");
	return 1;
}

/*
 * Subtest 7: v0 dealloc semantics on a vfmig-restored PD (post-gate).
 *
 * Updated 2026-06 -- the previous expectation in this subtest ("FW
 * must -EINVAL because the pdn has FW dependents") was based on a
 * model that conflated two distinct FW behaviours; see
 * tools/testing/criu_rdma/design/pd_registration_wipe.md for the
 * full investigation that revised the model.
 *
 * Empirical findings (test_dealloc_pd_chain.sh and friends):
 *   1. After LOAD_VHCA_STATE the destination VHCA's PDN allocator
 *      preserves its high-water mark (a fresh ALLOC_PD post-LOAD
 *      returns pdn > src_max_pdn -- no collision risk for fresh
 *      post-restore allocations).
 *   2. The (pdn -> owner_uid) registration entries are NOT
 *      preserved. DEALLOC_PD against a source-restored pdn returns
 *      status=bad_resource_state(0x9) syndrome=0xef0c8a regardless
 *      of the asserting uid -- the same shape FW returns for
 *      definitely-bogus pdns. So the failure is "PDN unknown to
 *      allocator", NOT "PDC has dependents".
 *   3. The failure is independent of the source-side CQ/QP being
 *      present (TEST A in test_dealloc_pd_chain.sh confirmed the
 *      same syndrome before any DESTROY_QP runs). So the previous
 *      claim "FW dependents pin the PDC, which is why dealloc
 *      fails" was incorrect.
 *
 * Mitigation landed in mlx5_ib_dealloc_pd: gate on
 *   (mpd->vfmig_restored && status == 0x9 && syndrome == 0xef0c8a)
 * suppresses the FW failure as benign. The kernel-side mpd is
 * freed by ib_dealloc_pd_user; uverbs removes the handle from the
 * ufile idr; the FW state is reclaimed at VHCA close (VF unbind),
 * mirroring the upstream SR-IOV VM-LM contract that resource
 * cleanup is VHCA-lifecycle-scoped (the host kernel never runs
 * DEALLOC_PD against migrated PDs in the VM-LM scenario, so FW
 * never observes this asymmetry there).
 *
 * Post-gate v0 PASS criterion:
 *   - DEALLOC_PD(target_handle) returns 0 (gate suppressed the
 *     FW failure)
 *   - INFO_HANDLES(PD) no longer reports target_handle (uobj
 *     freed by ib_dealloc_pd_user)
 *
 * This is the inverse of the pre-gate criterion. If you are
 * looking at this on a kernel without the gate landed, expect
 * DEALLOC_PD to return -EINVAL and INFO_HANDLES to still report
 * the handle -- check that mlx5_ib_dealloc_pd contains the
 * MLX5_IB_VFMIG_DEALLOC_PD_UNKNOWN_PDN_SYNDROME match.
 */
static int subtest_destroy_round_trip(int fd, uint32_t target_handle)
{
	uint32_t list[64] = {};
	uint32_t total = 0;
	int ret;

	printf("[7] v0 dealloc semantics (post-gate): DEALLOC_PD(0x%x) on a\n"
	       "    vfmig_restored PD must succeed via the syndrome-gated\n"
	       "    suppression in mlx5_ib_dealloc_pd; INFO_HANDLES must\n"
	       "    NOT report it after the dealloc.\n",
	       target_handle);

	ret = do_dealloc_pd(fd, target_handle);
	if (ret != 0) {
		fprintf(stderr,
			"  FAIL DEALLOC_PD(0x%x) -> %s (expected 0 via the\n"
			"       MLX5_IB_VFMIG_DEALLOC_PD_UNKNOWN_PDN syndrome\n"
			"       gate in mlx5_ib_dealloc_pd; check that the\n"
			"       loaded mlx5_ib has commit landing the gate --\n"
			"       see design/pd_registration_wipe.md)\n",
			target_handle, strerror(-ret));
		return 1;
	}
	printf("  PASS DEALLOC_PD(0x%x) -> 0 (gate suppressed expected\n"
	       "       FW status=0x9 syndrome=0xef0c8a; mlx5_ib_dbg in dmesg)\n",
	       target_handle);

	ret = do_info_handles_pd(fd, list, 64, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(PD): %s\n",
			strerror(-ret));
		return 1;
	}
	if (handle_present(list, total, target_handle)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(PD) still reports 0x%x after a\n"
			"       successful dealloc (uobj should have been freed\n"
			"       by ib_dealloc_pd_user; check uverbs path is not\n"
			"       parking the uobj on a 0-return)\n",
			target_handle);
		return 1;
	}
	printf("  PASS INFO_HANDLES(PD) no longer reports 0x%x (uobj freed)\n",
	       target_handle);
	return 0;
}

/* ---------------------------------------------------------------------- */
/*                                main                                    */
/* ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *ibdev;
	unsigned long src_pdn_arg;
	uint32_t src_pdn;
	uint32_t target_handle = DEFAULT_TARGET_HANDLE;
	char cdev_path[128];
	struct ib_uverbs_get_context_resp resp_core = {};
	struct mlx5_ib_alloc_ucontext_resp resp_drv = {};
	int fd_restore = -1, ret;
	int fails = 0;

	if (argc < 3 || argc > 4) {
		fprintf(stderr,
			"usage: %s <ibdev> <src_pdn> [<target_handle>]\n",
			argv[0]);
		return 2;
	}
	ibdev = argv[1];
	src_pdn_arg = strtoul(argv[2], NULL, 0);
	if (src_pdn_arg == 0 || src_pdn_arg > 0xffffffu) {
		fprintf(stderr,
			"pd_restore_mlx5: src_pdn=%lu out of range (1..0xffffff)\n",
			src_pdn_arg);
		return 2;
	}
	src_pdn = (uint32_t)src_pdn_arg;
	if (argc == 4) {
		unsigned long h = strtoul(argv[3], NULL, 0);
		if (h > 0xffffffffu) {
			fprintf(stderr,
				"pd_restore_mlx5: target_handle out of range\n");
			return 2;
		}
		target_handle = (uint32_t)h;
	}

	if (resolve_cdev_path(ibdev, cdev_path, sizeof(cdev_path)) != 0)
		return 2;
	printf("pd_restore_mlx5: ibdev=%s cdev=%s src_pdn=%u target_handle=0x%x\n",
	       ibdev, cdev_path, src_pdn, target_handle);

	/*
	 * Subtest 1 uses its own short-lived non-restore-mode ucontext.
	 */
	fails += subtest_gate_negative(cdev_path, src_pdn, target_handle);

	/*
	 * Subtests 2-5 + 7 share one restore-mode ucontext.
	 */
	fd_restore = open(cdev_path, O_RDWR | O_CLOEXEC);
	if (fd_restore < 0) {
		fprintf(stderr,
			"pd_restore_mlx5: open(%s) for restore-mode ucontext: %s\n",
			cdev_path, strerror(errno));
		return 2;
	}
	ret = do_get_context(fd_restore,
			     MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
			     &resp_core, &resp_drv);
	if (ret) {
		fprintf(stderr,
			"pd_restore_mlx5: GET_CONTEXT(flags=VFMIG_RESTORE): %s\n",
			strerror(-ret));
		close(fd_restore);
		return 2;
	}

	fails += subtest_uapi_reject_zero_pdn(fd_restore, target_handle);
	fails += subtest_uapi_reject_reserved(fd_restore, src_pdn, target_handle);
	fails += subtest_happy_path(fd_restore, src_pdn, target_handle);
	fails += subtest_collision(fd_restore, src_pdn, target_handle);

	if (fails) {
		fprintf(stderr,
			"\npd_restore_probe_mlx5_vfmig: %d subtest failure(s) "
			"before READY; aborting\n", fails);
		close(fd_restore);
		return 1;
	}

	/*
	 * READY checkpoint: the adopted PD lives on fd_restore at
	 * target_handle. The harness can now run any out-of-process
	 * FW-side verifier (typically `mlx5_vfmig probe_pd <vf> <pdn>`)
	 * while the PD is alive. Once it writes "quit\n" we tear down.
	 */
	printf("ibdev=%s\n", ibdev);
	printf("adopted_pdn=%u\n", src_pdn);
	printf("target_handle=0x%x\n", target_handle);
	printf("READY\n");
	fflush(stdout);

	{
		char line[64];
		while (fgets(line, sizeof(line), stdin)) {
			if (!strncmp(line, "quit", 4))
				break;
		}
	}

	fails += subtest_destroy_round_trip(fd_restore, target_handle);

	close(fd_restore);

	if (fails) {
		fprintf(stderr,
			"\npd_restore_probe_mlx5_vfmig: FAIL (%d subtest failure(s))\n",
			fails);
		return 1;
	}
	printf("\npd_restore_probe_mlx5_vfmig: PASS\n");
	return 0;
}
