// SPDX-License-Identifier: GPL-2.0
/*
 * mr_restore_probe_mlx5_vfmig -- end-to-end driver-level validation
 * of S4b: UVERBS_METHOD_RESTORE_MR + mlx5_ib_restore_mr Model A
 * (design/uobject_restore.md S7.x + S9.1 S4b).
 *
 * What we validate against a bound, post-LOAD destination mlx5
 * ib_device:
 *
 *   1. Gate (negative). A ucontext opened WITHOUT
 *      MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE cannot invoke RESTORE_MR;
 *      the dispatcher must return -EPERM via the per-driver
 *      mlx5_ib_ucontext_is_restore_mode predicate. We allocate a
 *      throwaway ALLOC_PD on the non-restore-mode ucontext to hand
 *      RESTORE_MR a valid PD_HANDLE -- the IDR attr machinery
 *      resolves UVERBS_ATTR_TYPE_IDR before the dispatcher's gate
 *      runs, so a bogus PD_HANDLE would short-circuit on -ENOENT
 *      and we'd never observe -EPERM.
 *   2. UAPI reject (mkey_index=0). RESTORE_MR with the driver-
 *      private UHW payload mlx5_ib_restore_mr_req.mkey_index = 0
 *      must -EINVAL. mkey_index=0 is the kernel's reserved
 *      sentinel.
 *   3. UAPI reject (reserved nonzero). RESTORE_MR with
 *      mlx5_ib_restore_mr_req.reserved = 0xDEAD must -EINVAL.
 *      Defends the future-flags slot.
 *   4. UAPI reject (lkey != rkey). The mlx5 user-MR invariant is
 *      lkey == rkey; mismatched hints must -EINVAL.
 *   5. UAPI reject ((lkey >> 8) != mkey_index). The mlx5 wire-
 *      visible identity is lkey/rkey == (mkey_index << 8) |
 *      variant_byte. A mismatch between the core lkey/rkey hints
 *      and the UHW mkey_index catches a class of CRIU bugs that
 *      ship a restrack id where a FW key was expected.
 *   6. Happy path. After a prerequisite RESTORE_PD adoption of
 *      src_pdn at pd_target_handle, RESTORE_MR with the source's
 *      (mkey_index, lkey, rkey, addr, length, iova, access_flags)
 *      lands a fresh kernel-side mlx5_ib_mr at mr_target_handle.
 *      RESP_LKEY / RESP_RKEY must echo the hints (mlx5 always
 *      honours, unlike rxe). INFO_HANDLES(MR) must contain the
 *      target handle.
 *   7. Collision. A second RESTORE_MR(target_handle) on the same
 *      ucontext returns -EBUSY from the xa_insert collision inside
 *      rdma_alloc_begin_uobject_at_handle.
 *   8. READY checkpoint. The probe prints
 *
 *          ibdev=<n>
 *          adopted_pdn=<pdn>
 *          adopted_mkey_index=<mki>
 *          mr_target_handle=<h>
 *          pd_target_handle=<h>
 *          READY
 *
 *      and blocks on stdin until it reads "quit\n". While the
 *      probe is parked the adopted MR (and its parent adopted PD)
 *      are alive in firmware, so the driver script can:
 *
 *        - issue MLX5_VFMIG_IOC_PROBE_MKEY via the PF cdev to
 *          confirm FW-side liveness (mkey alive at the source
 *          index, mkc context byte-equal to source pre-SAVE), or
 *        - issue MLX5_VFMIG_IOC_PROBE_PD for parallel PD
 *          liveness, or
 *        - drive any other follow-on verification against the
 *          destination ibdev.
 *
 *      On "quit\n" the probe drops to subtest 9.
 *   9. v0 dealloc semantics. Asymmetric with PD. In the FW
 *      resource graph, mkey is a *leaf* under its parent PD; QPs
 *      reference an mkey by its (lkey/rkey) wire value rather
 *      than as a tracked FW resource dependency. So
 *      IB_USER_VERBS_CMD_DEREG_MR on the adopted mr_target_handle
 *      *succeeds* even with the source's mkey-referencing QPs
 *      still alive in FW post-LOAD: __mlx5_ib_dereg_mr collapses
 *      to FW DESTROY_MKEY (umem == NULL + cache_ent == NULL after
 *      Model A adoption), FW accepts it, and uverbs_destroy_uobject
 *      removes the uobj from the ufile. INFO_HANDLES(MR) must NOT
 *      see mr_target_handle after the successful dereg.
 *
 *      Compare with subtest 7 of pd_restore_probe_mlx5_vfmig, where
 *      DEALLOC_PD on an orphan adopted PD *fails* with BAD_RES_STATE
 *      because PD is a *parent* in the FW resource graph and its
 *      CQ/QP/MR/SRQ children are still alive. The two contracts
 *      diverge:
 *        - PD: FW enforces restore ordering -- orphan dealloc fails,
 *          kernel parks the uobj, CRIU is forced to wait for the
 *          dependent restores before the user's DEALLOC_PD can
 *          succeed.
 *        - MR: FW has no resource-graph dep on mkey -- orphan dereg
 *          succeeds, kernel frees the uobj. CRIU plugin policy must
 *          ensure DEREG_MR isn't issued ahead of the user's intent;
 *          the kernel won't refuse it.
 *
 *      Recorded in design/uobject_restore.md S4b (B5).
 *
 * The probe deliberately stays libibverbs-only and pure-uverbs (no
 * libmlx5, no mlx5dv) so it can drive the
 * MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE flag in udata (libmlx5 has no
 * plumbing for it) and the new mlx5_ib_restore_{pd,mr}_req UHW
 * payloads.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/mr_restore/mr_restore_probe_mlx5_vfmig
 *
 * Usage:
 *   ./mr_restore_probe_mlx5_vfmig <ibdev> <src_pdn> <src_mkey_index>
 *           <src_lkey> <src_mr_addr> <src_mr_length>
 *           <src_access_flags>
 *           [<mr_target_handle> [<pd_target_handle>]]
 *
 * <ibdev>           destination ib_device name (e.g. mlx5_2),
 *                   typically picked up from `find_ib_dev_for_pci
 *                   <dst_vf_bdf>` in the harness.
 * <src_pdn>         FW pdn captured pre-SAVE on the source
 *                   ucontext.
 * <src_mkey_index>  FW mkey_index captured pre-SAVE
 *                   (fw_id_continuity_probe's mkey_index= line,
 *                   which is src_lkey >> 8).
 * <src_lkey>        Full FW key from the source MR
 *                   (fw_id_continuity_probe's lkey= line). The
 *                   mlx5 invariant is lkey == rkey; the probe
 *                   uses this single value for both.
 * <src_mr_addr>     User VA of the source MR's pinned buffer
 *                   (fw_id_continuity_probe's mr_addr= line).
 * <src_mr_length>   Length of the source MR.
 * <src_access_flags> ib_access_flags as registered on the source.
 * <mr_target_handle> Defaults to 0x4242 (CRIU will use the
 *                   source ucontext's actual MR ufile handles).
 * <pd_target_handle> Defaults to 0x4241 (CRIU will use the
 *                   source ucontext's actual PD ufile handles).
 *
 * No dependency on libmlx5 or rdma-core's userspace mlx5 UAPI: the
 * mlx5-side ABI surface we need (GET_CONTEXT req/resp shape, the
 * RESTORE_PD UHW struct, the new RESTORE_MR UHW struct, the VFMIG
 * flag) is inlined below. Keep in sync with the authoritative
 * headers at include/uapi/rdma/mlx5-abi.h and
 * include/uapi/rdma/ib_user_verbs.h.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
 * Driver-private UHW payload for UVERBS_METHOD_RESTORE_PD on mlx5
 * (kernel commit 8ae030bebb60). 16 bytes -- > 8 forces the kernel's
 * uverbs UHW dispatch onto the ptr path (see pd_restore_probe_mlx5
 * for the inline-vs-ptr analysis).
 */
struct mlx5_ib_restore_pd_req {
	uint32_t	pdn;
	uint32_t	reserved;
	uint64_t	reserved2;	/* __aligned_u64 in kernel UAPI */
} __attribute__((aligned(8)));

/*
 * Driver-private UHW payload for UVERBS_METHOD_RESTORE_MR on mlx5
 * (kernel commit 87f2981ebea7). Same size + reserved-padding shape
 * as restore_pd_req, for the same reason.
 *
 * mkey_index is the FW mkey index to adopt (24 bits significant).
 * The handler additionally cross-checks (lkey_hint >> 8) ==
 * mkey_index == (rkey_hint >> 8) on the core attrs and lkey_hint
 * == rkey_hint, catching the class of CRIU bugs that ship a
 * restrack id where a FW key was expected.
 */
struct mlx5_ib_restore_mr_req {
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

#define UVERBS_METHOD_RESTORE_PD		0
#define UVERBS_METHOD_RESTORE_MR		1
#define UVERBS_ATTR_RESTORE_PD_HANDLE		0

enum {
	UVERBS_ATTR_RESTORE_MR_HANDLE		= 0,
	UVERBS_ATTR_RESTORE_MR_PD_HANDLE	= 1,
	UVERBS_ATTR_RESTORE_MR_ADDR		= 2,
	UVERBS_ATTR_RESTORE_MR_LENGTH		= 3,
	UVERBS_ATTR_RESTORE_MR_IOVA		= 4,
	UVERBS_ATTR_RESTORE_MR_ACCESS_FLAGS	= 5,
	UVERBS_ATTR_RESTORE_MR_LKEY_HINT	= 6,
	UVERBS_ATTR_RESTORE_MR_RKEY_HINT	= 7,
	UVERBS_ATTR_RESTORE_MR_RESP_LKEY	= 8,
	UVERBS_ATTR_RESTORE_MR_RESP_RKEY	= 9,
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

/*
 * Issue IB_USER_VERBS_CMD_GET_CONTEXT on the freshly-opened uverbs
 * cdev fd, with a mlx5_ib_alloc_ucontext_req_v2 in udata. The mlx5
 * flag is the only thing that varies between the gate-negative and
 * happy-path subtests, so it's the lone knob.
 */
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

/*
 * IB_USER_VERBS_CMD_ALLOC_PD on the open ucontext. Used only by
 * subtest 1 (gate-negative), which needs a valid PD_HANDLE on a
 * non-restore-mode ucontext to make UVERBS_ATTR_TYPE_IDR resolve
 * before the dispatcher's gate predicate runs.
 */
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

/*
 * UVERBS_METHOD_RESTORE_PD on the open ucontext fd. Used as a
 * prerequisite for the happy-path RESTORE_MR (the MR needs a
 * restored parent PD before the dispatcher can resolve PD_HANDLE).
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

/*
 * UVERBS_METHOD_RESTORE_MR on the open ucontext fd. Carries 8
 * core attrs + 1 UHW blob + 2 PTR_OUT response attrs (RESP_LKEY,
 * RESP_RKEY). The kernel echoes the wire-visible identity back
 * via the response attrs (mlx5 always honours the hint, unlike
 * rxe -- see design §4 + §9.1 S4a/S4b).
 */
struct restore_mr_response {
	uint32_t	lkey;
	uint32_t	rkey;
};

static int do_restore_mr(int fd, uint32_t target_handle, uint32_t pd_handle,
			 uint64_t addr, uint64_t length, uint64_t iova,
			 uint32_t access_flags, uint32_t lkey_hint,
			 uint32_t rkey_hint,
			 const struct mlx5_ib_restore_mr_req *uhw,
			 struct restore_mr_response *resp_out)
{
	/*
	 * resp_out must be non-NULL: RESP_LKEY/RESP_RKEY are
	 * UA_MANDATORY in the kernel UAPI and we always wire them up
	 * (see comment block before the n++ for the resp attrs).
	 */
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[11];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id = UVERBS_OBJECT_RESTORE;
	cmd.hdr.method_id = UVERBS_METHOD_RESTORE_MR;
	cmd.hdr.driver_id = RDMA_DRIVER_MLX5;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_HANDLE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= target_handle;
	n++;

	/*
	 * PD_HANDLE is UVERBS_ATTR_IDR (UVERBS_ACCESS_READ) per the
	 * kernel DECLARE_UVERBS_NAMED_METHOD. The IDR attr wire
	 * format uses len == 0 + data = ufile handle; the kernel's
	 * uverbs_finalize_attrs path treats nonzero len on an IDR
	 * attr as a malformed bundle and -EINVALs the entire ioctl
	 * before the dispatcher runs (which would mask the gate
	 * EPERM, the driver EINVALs we want to test, the EBUSY on
	 * collision, etc.). Mirrors mr_restore_probe_rxe.c.
	 */
	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_PD_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= pd_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_ADDR;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= addr;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_LENGTH;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= length;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_IOVA;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= iova;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_ACCESS_FLAGS;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= access_flags;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_LKEY_HINT;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= lkey_hint;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_RKEY_HINT;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= rkey_hint;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_UHW_IN;
	cmd.attrs[n].len	= sizeof(*uhw);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)uhw;
	n++;

	/*
	 * RESP_LKEY / RESP_RKEY are UA_MANDATORY in the kernel
	 * DECLARE_UVERBS_NAMED_METHOD for RESTORE_MR. Omitting them
	 * would -EINVAL at attr validation, before the gate predicate
	 * or any UAPI-shape check fires, which would mask the errnos
	 * subtests 1-5 + 7 are looking for. So always wire them up.
	 * The dispatcher's uverbs_copy_to into RESP_* runs after a
	 * successful driver call; on a failed call the buffers stay
	 * untouched (caller must zero them before the call if it
	 * wants to detect that).
	 */
	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_RESP_LKEY;
	cmd.attrs[n].len	= sizeof(resp_out->lkey);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)&resp_out->lkey;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_RESP_RKEY;
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

/*
 * INFO_HANDLES(UVERBS_OBJECT_MR). Mirror of pd_restore's
 * do_info_handles_pd: copies up to capacity_handles entries into
 * handles_out and the kernel-filled count into *total_out.
 */
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
	cmd.attrs[n].len	= (uint16_t)(capacity_handles *
					     sizeof(uint32_t));
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
			"mr_restore_mlx5: ibv_get_device_list returned 0\n");
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
			"mr_restore_mlx5: ibdev '%s' not found; available:",
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

struct mr_args {
	uint32_t	src_pdn;
	uint32_t	src_mkey_index;	/* (src_lkey >> 8) by mlx5 invariant */
	uint32_t	src_lkey;	/* full key, equals rkey in mlx5 */
	uint64_t	src_addr;
	uint64_t	src_length;
	uint32_t	src_access_flags;
	uint32_t	mr_target_handle;
	uint32_t	pd_target_handle;
	/*
	 * Destination-side anonymous mapping that backs the RESTORE_MR
	 * call's addr/iova arguments. user_mr_dma Stage 3 D4
	 * (7c496744b43b) made mlx5_ib_restore_mr() call
	 * ib_umem_pin(current->mm, addr, length) -- the kernel needs a
	 * valid mapping in the *probe's* mm at @addr to pin pages from,
	 * whereas @a->src_addr is a userspace VA from the *source
	 * probe's* mm and cannot be walked here.
	 *
	 * In a real CRIU restore this slot is populated by CRIU's mm
	 * replay (the destination process mmaps the source's VA range
	 * with MAP_FIXED and seeds the bytes from the checkpoint). The
	 * probe is not a full CRIU restore, so we fake the mapping with
	 * a fresh anonymous mmap and pass that VA instead -- the bytes
	 * themselves don't matter for kernel-side validation (no WQE
	 * data path; the data path is Stage 3 D5's Phase J on the CRIU
	 * agent side). What matters is that ib_umem_pin succeeds and
	 * the sg_table built from the pinned pages has total length ==
	 * placeholder len so mlx5_vfmig_bind_user_mr's length check
	 * accepts it.
	 *
	 * @local_addr is the mmap return value (page-aligned, length ==
	 * src_length, anonymous PROT_READ|PROT_WRITE). @src_addr is
	 * preserved for logging only and is NOT passed as an addr/iova
	 * to any RESTORE_MR call after this point.
	 */
	uint64_t	local_addr;
};

/*
 * Allocate @a->local_addr by anonymous mmap'ing a->src_length bytes
 * at any address the kernel picks. Returns 0 on success, -errno on
 * mmap failure. The mapping leaks at probe exit, which is fine for a
 * test binary.
 */
static int mr_args_mmap_local(struct mr_args *a)
{
	void *p = mmap(NULL, (size_t)a->src_length,
		       PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		int err = errno;
		fprintf(stderr,
			"mr_restore_mlx5: mmap(%zu) for local_addr: %s\n",
			(size_t)a->src_length, strerror(err));
		return -err;
	}
	a->local_addr = (uint64_t)(uintptr_t)p;
	printf("mr_restore_mlx5: local_addr=0x%llx (anonymous mmap, %zu bytes)\n"
	       "                 src_addr=0x%llx kept for identity logging only\n",
	       (unsigned long long)a->local_addr,
	       (size_t)a->src_length,
	       (unsigned long long)a->src_addr);
	return 0;
}

/*
 * Subtest 1: gate-negative.
 *
 * Open a fresh ucontext WITHOUT MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE.
 * To reach the dispatcher's gate predicate the call must first
 * resolve UVERBS_ATTR_RESTORE_MR_PD_HANDLE (UVERBS_ATTR_TYPE_IDR)
 * to a live PD on this ucontext, otherwise the IDR layer
 * short-circuits with -ENOENT and we never observe -EPERM. So we
 * ALLOC_PD on the throwaway ucontext, hand the resulting handle
 * to RESTORE_MR, and assert -EPERM.
 */
static int subtest_gate_negative(const char *cdev_path,
				 const struct mr_args *a)
{
	struct ib_uverbs_get_context_resp resp_core = {};
	struct mlx5_ib_alloc_ucontext_resp resp_drv = {};
	struct mlx5_ib_restore_mr_req uhw = {
		.mkey_index = a->src_mkey_index,
	};
	struct restore_mr_response resp = {};
	uint32_t throwaway_pd_handle = 0;
	int fd, ret;
	int fails = 0;

	printf("[1] gate: ucontext WITHOUT VFMIG_RESTORE -> RESTORE_MR must -EPERM\n");

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

	ret = do_restore_mr(fd, a->mr_target_handle, throwaway_pd_handle,
			    a->local_addr, a->src_length, a->local_addr,
			    a->src_access_flags, a->src_lkey, a->src_lkey,
			    &uhw, &resp);
	if (ret == -EPERM) {
		printf("  PASS RESTORE_MR on non-restore-mode ucontext -> -EPERM\n");
	} else if (ret == 0) {
		fprintf(stderr,
			"  FAIL RESTORE_MR on non-restore-mode ucontext succeeded "
			"(security regression: predicate not consulted)\n");
		fails++;
	} else {
		fprintf(stderr,
			"  FAIL RESTORE_MR on non-restore-mode ucontext -> %s "
			"(expected -EPERM)\n", strerror(-ret));
		fails++;
	}

	close(fd);
	return fails;
}

/*
 * Helper: build an UHW + invoke RESTORE_MR with the args, expecting
 * the given errno. Used by subtests 2-5 that vary one knob at a time.
 */
static int expect_restore_mr_errno(int fd, const struct mr_args *a,
				   const struct mlx5_ib_restore_mr_req *uhw,
				   uint32_t lkey_hint, uint32_t rkey_hint,
				   int want_errno, const char *desc)
{
	struct restore_mr_response resp = {};
	int ret = do_restore_mr(fd, a->mr_target_handle, a->pd_target_handle,
				a->local_addr, a->src_length, a->local_addr,
				a->src_access_flags, lkey_hint, rkey_hint,
				uhw, &resp);
	if (ret == want_errno) {
		printf("  PASS RESTORE_MR(%s) -> %s\n", desc,
		       strerror(-ret));
		return 0;
	}
	fprintf(stderr,
		"  FAIL RESTORE_MR(%s) -> %s (expected %s)\n",
		desc, ret ? strerror(-ret) : "0 (success)",
		strerror(-want_errno));
	return 1;
}

static int subtest_uapi_reject_zero_mkey(int fd, const struct mr_args *a)
{
	struct mlx5_ib_restore_mr_req uhw = { .mkey_index = 0 };

	printf("[2] uapi reject: mkey_index=0 must -EINVAL\n");
	/*
	 * lkey_hint=0 keeps (lkey_hint>>8)==0==mkey_index so we trip
	 * step 6 (mkey_index==0) ahead of step 8.
	 */
	return expect_restore_mr_errno(fd, a, &uhw, 0, 0, -EINVAL,
				       "mkey_index=0");
}

static int subtest_uapi_reject_reserved(int fd, const struct mr_args *a)
{
	struct mlx5_ib_restore_mr_req uhw = {
		.mkey_index = a->src_mkey_index,
		.reserved   = 0xDEADu,
	};

	printf("[3] uapi reject: reserved!=0 must -EINVAL\n");
	return expect_restore_mr_errno(fd, a, &uhw, a->src_lkey, a->src_lkey,
				       -EINVAL, "reserved=0xDEAD");
}

static int subtest_uapi_reject_lkey_neq_rkey(int fd, const struct mr_args *a)
{
	struct mlx5_ib_restore_mr_req uhw = {
		.mkey_index = a->src_mkey_index,
	};

	printf("[4] uapi reject: lkey != rkey must -EINVAL (mlx5 invariant)\n");
	return expect_restore_mr_errno(fd, a, &uhw, a->src_lkey,
				       a->src_lkey ^ 1u, -EINVAL,
				       "lkey != rkey");
}

static int subtest_uapi_reject_lkey_mismatch_mkey(int fd,
						  const struct mr_args *a)
{
	struct mlx5_ib_restore_mr_req uhw = {
		.mkey_index = a->src_mkey_index,
	};
	/*
	 * (bogus_key >> 8) == mkey_index ^ 0xfff -- guaranteed != src
	 * mkey_index. lkey == rkey so we get past step 7 and trip
	 * step 8 (lkey>>8 != mkey_index).
	 */
	uint32_t bogus_key = ((a->src_mkey_index ^ 0xfffu) << 8) |
			      (a->src_lkey & 0xffu);

	printf("[5] uapi reject: (lkey>>8) != mkey_index must -EINVAL\n");
	return expect_restore_mr_errno(fd, a, &uhw, bogus_key, bogus_key,
				       -EINVAL, "(lkey>>8) != mkey_index");
}

/*
 * Subtest 6: prerequisite RESTORE_PD + happy-path RESTORE_MR.
 *
 * The MR's PD_HANDLE attr is a UVERBS_ATTR_TYPE_IDR resolution, so
 * the dispatcher needs a live PD at pd_target_handle on this
 * ucontext before any of subtests 2-5 / 6 / 7 can succeed.
 *
 * Run order: prereq RESTORE_PD lands first, all subsequent uapi-
 * reject subtests share the same parent PD, then subtest 6 asserts
 * the wire-visible identity echo and INFO_HANDLES presence.
 */
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
	struct mlx5_ib_restore_mr_req uhw = {
		.mkey_index = a->src_mkey_index,
	};
	struct restore_mr_response resp = {};
	uint32_t list[64] = {};
	uint32_t total = 0;
	int ret;
	int fails = 0;

	printf("[6] happy path: RESTORE_MR(mr_target=0x%x, pd_target=0x%x, mkey_index=%u, lkey=0x%x)\n",
	       a->mr_target_handle, a->pd_target_handle, a->src_mkey_index,
	       a->src_lkey);

	ret = do_restore_mr(fd, a->mr_target_handle, a->pd_target_handle,
			    a->local_addr, a->src_length, a->local_addr,
			    a->src_access_flags, a->src_lkey, a->src_lkey,
			    &uhw, &resp);
	if (ret) {
		fprintf(stderr,
			"  FAIL RESTORE_MR(target=0x%x): %s%s\n",
			a->mr_target_handle, strerror(-ret),
			ret == -EOPNOTSUPP
			? "  (mlx5_ib_restore_mr not registered in mlx5_ib_dev_ops?)"
			: ret == -EPERM
			? "  (mlx5_ib_ucontext_is_restore_mode not reporting true?)"
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
		fprintf(stderr, "  FAIL INFO_HANDLES(MR): %s\n",
			strerror(-ret));
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

static int subtest_collision(int fd, const struct mr_args *a)
{
	struct mlx5_ib_restore_mr_req uhw = {
		.mkey_index = a->src_mkey_index,
	};
	struct restore_mr_response resp = {};
	int ret;

	printf("[7] collision: second RESTORE_MR(target=0x%x) must -EBUSY\n",
	       a->mr_target_handle);

	ret = do_restore_mr(fd, a->mr_target_handle, a->pd_target_handle,
			    a->local_addr, a->src_length, a->local_addr,
			    a->src_access_flags, a->src_lkey, a->src_lkey,
			    &uhw, &resp);
	if (ret == -EBUSY) {
		printf("  PASS second RESTORE_MR(target=0x%x) -> -EBUSY\n",
		       a->mr_target_handle);
		return 0;
	}
	fprintf(stderr,
		"  FAIL second RESTORE_MR(target=0x%x) -> %s (expected -EBUSY)\n",
		a->mr_target_handle, ret ? strerror(-ret) : "0 (success)");
	return 1;
}

/*
 * Subtest 8: v0 dealloc semantics for an adopted MR.
 *
 * Asymmetric with PD's subtest 7. In the FW resource graph mkey is
 * a leaf under PD; QPs reference an mkey by (lkey/rkey) wire value
 * rather than as a tracked FW resource dep, so DESTROY_MKEY on the
 * orphan adopted mkey *succeeds* even with the source's mkey-using
 * QPs still alive in destination FW post-LOAD_VHCA_STATE.
 *
 * Path: __mlx5_ib_dereg_mr -> mlx5r_handle_mkey_cleanup ->
 * FW DESTROY_MKEY -> 0; uverbs_destroy_uobject removes the uobj
 * from the ufile. So:
 *   - DEREG_MR(0x4242) returns 0
 *   - INFO_HANDLES(MR) must NOT contain 0x4242 afterwards
 *
 * Compare with subtest 7 of pd_restore_probe_mlx5_vfmig: DEALLOC_PD
 * on an orphan adopted PD must FAIL with -EINVAL (BAD_RES_STATE)
 * because PD is a parent in the FW graph and its CQ/QP/MR/SRQ
 * children are still alive.
 *
 * v0 implication for CRIU: the kernel cannot enforce MR teardown
 * ordering (no FW dep to refuse against). The CRIU plugin must not
 * issue DEREG_MR on adopted MRs ahead of the user's intent. This
 * is the inverse of the PD invariant -- PD's protection is
 * automatic via FW; MR's protection is policy-only at the plugin
 * layer. Documented in design/uobject_restore.md S4b.
 */
static int subtest_v0_dealloc(int fd, const struct mr_args *a)
{
	uint32_t list[64] = {};
	uint32_t total = 0;
	int ret;

	printf("[8] v0 dealloc semantics: DEREG_MR(0x%x) on an orphan adopted\n"
	       "    mkey must succeed (mkey is a leaf in the FW resource graph;\n"
	       "    no FW dep to refuse against); INFO_HANDLES(MR) must drop it\n",
	       a->mr_target_handle);

	ret = do_dereg_mr(fd, a->mr_target_handle);
	if (ret) {
		fprintf(stderr,
			"  FAIL DEREG_MR(0x%x) -> %s (expected 0; FW DESTROY_MKEY\n"
			"       should accept since mkey has no FW resource deps,\n"
			"       unlike PD which has CQ/QP/MR/SRQ children)\n",
			a->mr_target_handle, strerror(-ret));
		return 1;
	}
	printf("  PASS DEREG_MR(0x%x) -> 0 (mkey has no FW resource deps)\n",
	       a->mr_target_handle);

	ret = do_info_handles_mr(fd, list, 64, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(MR): %s\n",
			strerror(-ret));
		return 1;
	}
	if (handle_present(list, total, a->mr_target_handle)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(MR) still reports 0x%x after a successful\n"
			"       DEREG_MR (uobj should have been removed from the ufile)\n",
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
		"           <src_mr_addr> <src_mr_length> <src_access_flags>\n"
		"           [<mr_target_handle> [<pd_target_handle>]]\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *ibdev;
	struct mr_args a = {
		.mr_target_handle = DEFAULT_MR_TARGET_HANDLE,
		.pd_target_handle = DEFAULT_PD_TARGET_HANDLE,
	};
	char cdev_path[128];
	struct ib_uverbs_get_context_resp resp_core = {};
	struct mlx5_ib_alloc_ucontext_resp resp_drv = {};
	int fd_restore = -1, ret;
	int fails = 0;
	unsigned long u;

	if (argc < 8 || argc > 10) {
		usage(argv[0]);
		return 2;
	}
	ibdev = argv[1];

	u = strtoul(argv[2], NULL, 0);
	if (u == 0 || u > 0xffffffu) {
		fprintf(stderr,
			"mr_restore_mlx5: src_pdn=%lu out of range (1..0xffffff)\n",
			u);
		return 2;
	}
	a.src_pdn = (uint32_t)u;

	u = strtoul(argv[3], NULL, 0);
	if (u == 0 || u > 0xffffffu) {
		fprintf(stderr,
			"mr_restore_mlx5: src_mkey_index=%lu out of range (1..0xffffff)\n",
			u);
		return 2;
	}
	a.src_mkey_index = (uint32_t)u;

	u = strtoul(argv[4], NULL, 0);
	if (u > 0xffffffffu) {
		fprintf(stderr, "mr_restore_mlx5: src_lkey out of u32 range\n");
		return 2;
	}
	a.src_lkey = (uint32_t)u;
	if ((a.src_lkey >> 8) != a.src_mkey_index) {
		fprintf(stderr,
			"mr_restore_mlx5: src_lkey=0x%x does not encode src_mkey_index=%u\n"
			"                (mlx5 invariant: lkey == (mkey_index << 8) | variant)\n",
			a.src_lkey, a.src_mkey_index);
		return 2;
	}

	a.src_addr = strtoull(argv[5], NULL, 0);
	a.src_length = strtoull(argv[6], NULL, 0);
	if (a.src_length == 0) {
		fprintf(stderr, "mr_restore_mlx5: src_mr_length must be > 0\n");
		return 2;
	}

	u = strtoul(argv[7], NULL, 0);
	if (u > 0xffu) {
		fprintf(stderr,
			"mr_restore_mlx5: src_access_flags=0x%lx wider than 8 bits\n",
			u);
		return 2;
	}
	a.src_access_flags = (uint32_t)u;

	if (argc >= 9) {
		u = strtoul(argv[8], NULL, 0);
		if (u > 0xffffffffu) {
			fprintf(stderr,
				"mr_restore_mlx5: mr_target_handle out of range\n");
			return 2;
		}
		a.mr_target_handle = (uint32_t)u;
	}
	if (argc >= 10) {
		u = strtoul(argv[9], NULL, 0);
		if (u > 0xffffffffu) {
			fprintf(stderr,
				"mr_restore_mlx5: pd_target_handle out of range\n");
			return 2;
		}
		a.pd_target_handle = (uint32_t)u;
	}
	if (a.mr_target_handle == a.pd_target_handle) {
		fprintf(stderr,
			"mr_restore_mlx5: mr_target_handle == pd_target_handle (0x%x); "
			"must differ since they live in independent ufile namespaces "
			"but the probe relies on differentiating them in logs\n",
			a.mr_target_handle);
		return 2;
	}

	if (resolve_cdev_path(ibdev, cdev_path, sizeof(cdev_path)) != 0)
		return 2;
	printf("mr_restore_mlx5: ibdev=%s cdev=%s\n"
	       "                  src_pdn=%u src_mkey_index=%u src_lkey=0x%x\n"
	       "                  src_addr=0x%llx src_length=0x%llx access_flags=0x%x\n"
	       "                  mr_target_handle=0x%x pd_target_handle=0x%x\n",
	       ibdev, cdev_path, a.src_pdn, a.src_mkey_index, a.src_lkey,
	       (unsigned long long)a.src_addr, (unsigned long long)a.src_length,
	       a.src_access_flags, a.mr_target_handle, a.pd_target_handle);

	/*
	 * Allocate the destination-side anonymous mapping that backs
	 * the addr/iova arguments to every RESTORE_MR call below. See
	 * the docstring on @local_addr in struct mr_args -- the kernel
	 * (post-Stage-3 D4) calls ib_umem_pin(current->mm, addr,
	 * length) inside mlx5_ib_restore_mr, so without this mapping
	 * the happy-path subtest fails -EFAULT on
	 * pin_user_pages_fast().
	 */
	if (mr_args_mmap_local(&a) != 0)
		return 2;

	/* Subtest 1 uses its own short-lived non-restore-mode ucontext. */
	fails += subtest_gate_negative(cdev_path, &a);

	/* Subtests 2-7 + 8 share one restore-mode ucontext. */
	fd_restore = open(cdev_path, O_RDWR | O_CLOEXEC);
	if (fd_restore < 0) {
		fprintf(stderr,
			"mr_restore_mlx5: open(%s) for restore-mode ucontext: %s\n",
			cdev_path, strerror(errno));
		return 2;
	}
	ret = do_get_context(fd_restore,
			     MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
			     &resp_core, &resp_drv);
	if (ret) {
		fprintf(stderr,
			"mr_restore_mlx5: GET_CONTEXT(flags=VFMIG_RESTORE): %s\n",
			strerror(-ret));
		close(fd_restore);
		return 2;
	}

	/*
	 * Prereq: the parent PD has to live in the ufile before any
	 * RESTORE_MR can resolve UVERBS_ATTR_RESTORE_MR_PD_HANDLE.
	 */
	if (subtest_prereq_restore_pd(fd_restore, &a)) {
		close(fd_restore);
		return 1;
	}

	fails += subtest_uapi_reject_zero_mkey(fd_restore, &a);
	fails += subtest_uapi_reject_reserved(fd_restore, &a);
	fails += subtest_uapi_reject_lkey_neq_rkey(fd_restore, &a);
	fails += subtest_uapi_reject_lkey_mismatch_mkey(fd_restore, &a);
	fails += subtest_happy_path(fd_restore, &a);
	fails += subtest_collision(fd_restore, &a);

	if (fails) {
		fprintf(stderr,
			"\nmr_restore_probe_mlx5_vfmig: %d subtest failure(s) "
			"before READY; aborting\n", fails);
		close(fd_restore);
		return 1;
	}

	/*
	 * READY checkpoint. The adopted MR + parent PD live on
	 * fd_restore at the requested target handles. The harness
	 * can now run any out-of-process FW-side verifier --
	 * typically `mlx5_vfmig probe_mkey <vf> <mkey_index>` and
	 * `mlx5_vfmig probe_pd <vf> <pdn>` -- while both objects are
	 * alive in destination FW. On "quit\n" we drop to subtest 8.
	 */
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
			"\nmr_restore_probe_mlx5_vfmig: FAIL (%d subtest failure(s))\n",
			fails);
		return 1;
	}
	printf("\nmr_restore_probe_mlx5_vfmig: PASS\n");
	return 0;
}
