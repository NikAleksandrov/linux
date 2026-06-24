// SPDX-License-Identifier: GPL-2.0
/*
 * cq_restore_probe_mlx5_vfmig -- end-to-end driver-level validation
 * of S5b: UVERBS_METHOD_RESTORE_CQ + mlx5_ib_restore_cq Model A
 * (design/uobject_restore.md S5b + S9.1 S5b).
 *
 * What we validate against a bound, post-LOAD destination mlx5
 * ib_device:
 *
 *   1. Gate (negative). A ucontext opened WITHOUT
 *      MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE cannot invoke RESTORE_CQ;
 *      the dispatcher must return -EPERM via the per-driver
 *      mlx5_ib_ucontext_is_restore_mode predicate.
 *   2. UAPI reject (cqn=0). RESTORE_CQ with the driver-private UHW
 *      payload mlx5_ib_restore_cq_req.cqn = 0 must -EINVAL. cqn=0
 *      is the kernel's reserved sentinel.
 *   3. UAPI reject (cqn out-of-range). A cqn whose high byte is
 *      non-zero (cqn is a 24-bit FW resource id) must -EINVAL.
 *   4. UAPI reject (cqe_size invalid). A cqe_size other than 64 or
 *      128 must -EINVAL.
 *   5. UAPI reject (reserved nonzero). RESTORE_CQ with reserved
 *      != 0 must -EINVAL. Defends the future-flags slot.
 *   6. Bad comp_vector. comp_vector >= dev->num_comp_vectors must
 *      come back as -EINVAL from the dispatcher's pre-driver guard.
 *   7. COMP_CHANNEL rejected. v0 plugin policy (S5c deferred to
 *      S10): allocate a comp_channel via the legacy
 *      IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL write-cmd, pass its
 *      fd as UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL on a fresh
 *      RESTORE_CQ call. The dispatcher's v0 forward-compat guard
 *      must reject with -EOPNOTSUPP.
 *   8. Happy path. RESTORE_CQ with the source's
 *      (cqn, cqe, cqe_size, buf_addr, db_addr, comp_vector) lands
 *      a fresh kernel-side mlx5_ib_cq at cq_target_handle on a
 *      restore-mode ucontext. RESP_CQE must echo the dispatcher's
 *      attr.cqe (mlx5 honours the source-reported value verbatim;
 *      no roundup). INFO_HANDLES(CQ) must contain the target
 *      handle.
 *   9. Collision. A second RESTORE_CQ(cq_target_handle) on the
 *      same ucontext returns -EBUSY from the xa_insert collision
 *      inside rdma_alloc_begin_uobject_at_handle.
 *  10. READY checkpoint. The probe prints
 *
 *          ibdev=<n>
 *          adopted_cqn=<cqn>
 *          adopted_cqe=<cqe>
 *          adopted_cqe_size=<sz>
 *          cq_target_handle=<h>
 *          READY
 *
 *      and blocks on stdin until it reads "quit\n". While the
 *      probe is parked the adopted CQ is alive in firmware, so
 *      the driver script can:
 *
 *        - issue MLX5_VFMIG_IOC_PROBE_CQN via the PF cdev to
 *          confirm FW-side liveness (cqc.eqn / log_cq_size etc.
 *          byte-equal to source pre-SAVE), or
 *        - drive any other follow-on verification against the
 *          destination ibdev.
 *
 *      On "quit\n" the probe drops to subtest 10.
 *  11. v0 dealloc semantics. Symmetric with PD's subtest 7
 *      (asymmetric with MR's subtest 8). In the FW resource
 *      graph CQ is a *parent*: QPCs / SRQCs reference cqn as a
 *      tracked FW resource dep. So FW DESTROY_CQ on the orphan
 *      adopted cqn is REJECTED with BAD_RES_STATE (mapped to
 *      -EINVAL by cmd_status_to_err) because LOAD_VHCA_STATE
 *      also carried over the source's cqn-referencing
 *      QPCs/SRQCs. v0 has not restored kernel uobjects for
 *      those, so the kernel cannot dealloc them first.
 *
 *      That rejection is the *correct* CRIU restore-ordering
 *      invariant:
 *        restore CQ -> restore QP/SRQ -> user destroys QP/SRQ
 *                   -> user destroys CQ -> FW DESTROY_CQ succeeds.
 *
 *      Subtest 11's v0 PASS criterion is the inverse of an
 *      alloc/dealloc round-trip: DESTROY_CQ must -EINVAL AND
 *      INFO_HANDLES(CQ) must still see the handle (the uobj is
 *      parked in the ufile so a later cascading teardown can
 *      retry once S6+S7 land). Asymmetric with mr_restore's
 *      subtest 8 (mkey is a leaf and DEREG_MR succeeds).
 *
 *      Recorded in design/uobject_restore.md S5b (B4 + S10.8).
 *
 * The probe deliberately stays libibverbs-only and pure-uverbs (no
 * libmlx5, no mlx5dv) so it can drive the
 * MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE flag in udata (libmlx5 has no
 * plumbing for it) and the new mlx5_ib_restore_cq_req UHW payload.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/cq_restore/cq_restore_probe_mlx5_vfmig
 *
 * Usage:
 *   ./cq_restore_probe_mlx5_vfmig <ibdev> <src_cqn> <src_cqe>
 *           <src_cqe_size> <src_cq_buf_addr> <src_cq_db_addr>
 *           [<comp_vector> [<cq_target_handle>]]
 *
 * <ibdev>           destination ib_device name (e.g. mlx5_2),
 *                   typically picked up from `find_ib_dev_for_pci
 *                   <dst_vf_bdf>` in the harness.
 * <src_cqn>         FW cqn captured pre-SAVE on the source
 *                   ucontext (from fw_id_continuity_probe's cqn=
 *                   line, also matches B0 PROBE_CQN's input).
 * <src_cqe>         The userspace-visible cqe count the source
 *                   ucontext got back from ibv_create_cq
 *                   (mlx5dv_cq.cqe_cnt; equal to cq->cqe).
 * <src_cqe_size>    64 or 128, from mlx5dv_cq.cqe_size.
 * <src_cq_buf_addr> User VA of the source's CQE ring buffer
 *                   (mlx5dv_cq.buf). The probe ignores this for
 *                   the actual RESTORE_CQ call (the kernel pins
 *                   a destination-side anonymous mapping
 *                   instead) but logs it for identity tracking.
 * <src_cq_db_addr>  User VA of the source's doorbell record
 *                   (mlx5dv_cq.dbrec). Same identity-only role
 *                   as src_cq_buf_addr; the actual RESTORE_CQ
 *                   uses local_db_addr, see local-mmap rationale.
 * <comp_vector>     Defaults to 0. Used as
 *                   UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR by
 *                   subtests 7 onward. A real CRIU restore
 *                   would pass the source's saved comp_vector;
 *                   the probe accepts whatever the harness
 *                   captured.
 * <cq_target_handle> Defaults to 0x4244 (CRIU will use the
 *                   source ucontext's actual CQ ufile handles).
 *                   Picked distinct from MR/PD probe defaults
 *                   so a future combined harness can keep them
 *                   apart.
 *
 * No dependency on libmlx5 or rdma-core's userspace mlx5 UAPI: the
 * mlx5-side ABI surface we need (GET_CONTEXT req/resp shape, the
 * RESTORE_CQ UHW struct, the VFMIG flag) is inlined below. Keep
 * in sync with the authoritative headers at
 * include/uapi/rdma/mlx5-abi.h and
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
	/*
	 * Only VFMIG_RESTORE here. The canonical kernel header at
	 * include/uapi/rdma/mlx5-abi.h also defines
	 * MLX5_IB_ALLOC_UCTX_DEVX (bit 0) and
	 * MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID (bit 2), but the v0
	 * RESTORE_CQ path explicitly does NOT set either:
	 *
	 *   - DEVX (bit 0): asks the kernel to allocate a fresh
	 *     devx_uid for this ucontext. Setting it on the restore
	 *     path means the destination ucontext is registered with
	 *     a *new* uid that has no claim over the source's
	 *     (pdn, owning_uid)-tagged FW records, defeating Model A.
	 *
	 *   - ADOPT_DEVX_UID (bit 2): the original "adopt source's
	 *     devx_uid" mechanism. The empirical matrix in
	 *     design/uobject_restore.md S9.1 S3b "DEVX-adoption
	 *     blind spot" (FW 28.48.1000) shows LOAD_VHCA_STATE
	 *     preserves next_free_uctx but wipes the uctx-
	 *     registration table itself, so any subsequent
	 *     CREATE_MKEY(pdn, uid=adopted) rejects with "unknown
	 *     uid". Vestigial for v0; gated only by an opt-in flag
	 *     no v0 caller sets.
	 *
	 * The v0 mitigation per the kernel header comment is to
	 * open with neither flag, leaving context->devx_uid = 0 --
	 * the FW-ungated lane proven by the P_zero / N_zero cells
	 * of the pd_adopt matrix. Restored processes lose DEVX
	 * features (mlx5dv_devx_obj_*, ...); basic verbs work --
	 * which is the v0 critical-path goal.
	 *
	 * If a future RESTORE_CQ test needs to exercise the DEVX-
	 * aware lane it should redefine the relevant bits here
	 * verbatim from include/uapi/rdma/mlx5-abi.h. Keeping them
	 * out of the enum today prevents accidental "DEVX |
	 * VFMIG_RESTORE" usage that would silently undermine the
	 * uid=0 lane.
	 */
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
 * Driver-private UHW payload for UVERBS_METHOD_RESTORE_CQ on mlx5
 * (kernel commit 33600a31a360). 32 bytes -- well above the 8-byte
 * inline-UHW threshold so the dispatcher always takes the
 * userspace-pointer path.
 *
 * cqn is the FW cqn to adopt (24 bits significant). cqe_size is
 * 64 or 128. buf_addr / db_addr are user VAs of the CQE ring and
 * doorbell record, respectively; the destination kernel binds
 * those into the LOAD_VHCA_STATE-replayed (KIND_CQ, cqn) and
 * (KIND_DBR, db_addr & PAGE_MASK) placeholders. Note db_addr
 * does not need to be page-aligned -- the kernel masks to
 * PAGE_MASK on its side, and the offset within the page is
 * preserved verbatim because cq->db.dma is computed as
 * sg_dma_address + (db_addr & ~PAGE_MASK).
 */
struct mlx5_ib_restore_cq_req {
	uint64_t	buf_addr;
	uint64_t	db_addr;
	uint32_t	cqn;
	uint32_t	cqe_size;
	uint32_t	reserved;
	uint32_t	reserved2;
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
#define UVERBS_OBJECT_CQ			3
#define UVERBS_OBJECT_RESTORE			18

#define UVERBS_METHOD_INFO_HANDLES		1
#define UVERBS_ATTR_INFO_OBJECT_ID		0
#define UVERBS_ATTR_INFO_TOTAL_HANDLES		1
#define UVERBS_ATTR_INFO_HANDLES_LIST		2

/*
 * UVERBS_METHOD_RESTORE_CQ = 2 (PD=0, MR=1, CQ=2 in
 * enum uverbs_methods_restore).
 */
#define UVERBS_METHOD_RESTORE_CQ		2
enum {
	UVERBS_ATTR_RESTORE_CQ_HANDLE		= 0,
	UVERBS_ATTR_RESTORE_CQ_CQE		= 1,
	UVERBS_ATTR_RESTORE_CQ_USER_HANDLE	= 2,
	UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR	= 3,
	UVERBS_ATTR_RESTORE_CQ_FLAGS		= 4,
	UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL	= 5,
	UVERBS_ATTR_RESTORE_CQ_EVENT_FD		= 6,
	UVERBS_ATTR_RESTORE_CQ_RESP_CQE		= 7,
};

#define UVERBS_ATTR_UHW_IN			((uint16_t)4096)

#define RDMA_DRIVER_MLX5			1

#define DEFAULT_CQ_TARGET_HANDLE		0x4244u
#define USER_HANDLE_TAG				0xDEADBEEFCAFEBABEull

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
 * Allocate a comp channel via the legacy write path. Returns the
 * new fd (which is also the kernel uobject id for the comp_channel
 * object, exactly the value the FD-class RESTORE_CQ_COMP_CHANNEL
 * attr expects) or -errno on failure. Used only by subtest 7.
 */
static int do_create_comp_channel(int fd)
{
	struct {
		struct ib_uverbs_cmd_hdr			hdr;
		struct ib_uverbs_create_comp_channel		core;
	} __attribute__((packed)) cmd = {};
	struct ib_uverbs_create_comp_channel_resp resp = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= sizeof(resp) / 4;
	cmd.core.response	= (uintptr_t)&resp;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	return (int)resp.fd;
}

static int do_destroy_cq(int fd, uint32_t cq_handle, uint32_t *async_events_out)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_destroy_cq	core;
	} __attribute__((packed)) cmd = {};
	struct ib_uverbs_destroy_cq_resp resp = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_DESTROY_CQ;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= sizeof(resp) / 4;
	cmd.core.response	= (uintptr_t)&resp;
	cmd.core.cq_handle	= cq_handle;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	if (async_events_out)
		*async_events_out = resp.async_events_reported;
	return 0;
}

/* ---------------------------------------------------------------------- */
/*                         uverbs ioctl helpers                           */
/* ---------------------------------------------------------------------- */

/*
 * UVERBS_METHOD_RESTORE_CQ on the open ucontext fd. Always wires
 * up the mandatory core attrs (HANDLE / CQE / USER_HANDLE /
 * COMP_VECTOR / RESP_CQE) plus the UHW_IN payload; optionally
 * includes COMP_CHANNEL (only on the rejection subtest).
 *
 * Wire encoding (uverbs_ioctl.c):
 *   - PTR_IN(__u32):	len = 4, data = inline value
 *   - PTR_IN(__u64):	len = 8, data = inline value
 *   - PTR_OUT(__u32):	len = 4, data = (uintptr_t)&user_buf
 *   - FD-class:		len = 0, data = fd (uint32 truncated)
 *   - UHW_IN/UHW_OUT:	len = sizeof(buf), data = (uintptr_t)&buf
 *
 * mlx5_ib_restore_cq enforces udata->outlen == 0, so we DO NOT
 * attach UHW_OUT (unlike the rxe S5a probe which has to provide
 * a uresp buffer for rxe_restore_cq).
 */
static int do_restore_cq(int fd, uint32_t target_handle, uint32_t cqe,
			 uint64_t user_handle, uint32_t comp_vector,
			 int comp_channel_fd,
			 const struct mlx5_ib_restore_cq_req *uhw,
			 uint32_t *resp_cqe_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[7];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= UVERBS_OBJECT_RESTORE;
	cmd.hdr.method_id	= UVERBS_METHOD_RESTORE_CQ;
	cmd.hdr.driver_id	= RDMA_DRIVER_MLX5;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_HANDLE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= target_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_CQE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= cqe;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_USER_HANDLE;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= user_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= comp_vector;
	n++;

	if (comp_channel_fd >= 0) {
		cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL;
		cmd.attrs[n].len	= 0;
		cmd.attrs[n].flags	= 0;
		cmd.attrs[n].data	= (uint32_t)comp_channel_fd;
		n++;
	}

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_RESP_CQE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)resp_cqe_out;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_UHW_IN;
	cmd.attrs[n].len	= sizeof(*uhw);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)uhw;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length    = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

/*
 * INFO_HANDLES(UVERBS_OBJECT_CQ). Mirror of mr_restore's
 * do_info_handles_mr.
 */
static int do_info_handles_cq(int fd, uint32_t *handles_out,
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
	cmd.attrs[n].data	= UVERBS_OBJECT_CQ;
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
			"cq_restore_mlx5: ibv_get_device_list returned 0\n");
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
			"cq_restore_mlx5: ibdev '%s' not found; available:",
			ibdev_name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	ibv_free_device_list(list);
	return ret;
}

/*
 * Sentinel comp_vector for subtest 6. The dispatcher's pre-driver
 * guard rejects any value >= ufile->device->num_comp_vectors.
 * num_comp_vectors is bounded by the host's NR_CPUS-style irq
 * affinity domain (single-digit thousands at worst). 0xfffffffeu
 * is comfortably out-of-range on every plausible deployment and
 * also distinct from UINT32_MAX so a future kernel-side
 * "num_comp_vectors == UINT32_MAX" sentinel cannot mask the
 * subtest.
 */
#define BAD_COMP_VECTOR_SENTINEL	0xfffffffeu

/* ---------------------------------------------------------------------- */
/*                              subtests                                  */
/* ---------------------------------------------------------------------- */

struct cq_args {
	uint32_t	src_cqn;
	uint32_t	src_cqe;	/* userspace-visible cqe count */
	uint32_t	src_cqe_size;	/* 64 or 128 */
	uint64_t	src_buf_addr;
	uint64_t	src_db_addr;
	uint32_t	comp_vector;
	uint32_t	cq_target_handle;
	/*
	 * Destination-side mappings that back the RESTORE_CQ call's
	 * buf_addr / db_addr arguments. The kernel pins user pages
	 * from current->mm at these VAs inside mlx5_ib_restore_cq's
	 * bind helpers, so a probe-side mapping is required.
	 *
	 * In a real CRIU restore these slots are populated by CRIU's
	 * mm replay (the destination process mmaps the source's VA
	 * range with MAP_FIXED and seeds the bytes from the
	 * checkpoint). The probe is not a full CRIU restore but it
	 * has to simulate the relevant invariants:
	 *
	 *   - @local_buf_addr (CQE ring): can be ANY VA in the
	 *     probe's mm. mlx5_vfmig_bind_user_cq's placeholder is
	 *     keyed by (KIND_CQ, cqn) -- the user VA is not part of
	 *     the lookup key, just the pinning target. We therefore
	 *     allocate this with a normal anonymous mmap (mr_restore
	 *     uses the same pattern with mkey_index-keyed lookup).
	 *
	 *   - @local_db_addr (doorbell record): MUST equal the
	 *     source's pre-SAVE user VA at PAGE_MASK granularity.
	 *     mlx5_vfmig_bind_user_dbr's placeholder is keyed by
	 *     (KIND_DBR, virt & PAGE_MASK). The DBR placeholder has
	 *     no FW-allocated id (no FW resource owns "the doorbell
	 *     page"); the only stable cross-mm key is the user VA.
	 *     mlx5_ib_db_map_user_restore therefore uses a single
	 *     'virt' argument both for ib_umem_pin (current->mm) AND
	 *     for the placeholder lookup. CRIU's MAP_FIXED replay
	 *     makes those equal naturally; the probe simulates that
	 *     with mmap(src_db_addr & PAGE_MASK, MAP_FIXED_NOREPLACE).
	 *
	 *     If MAP_FIXED_NOREPLACE collides with the probe's own
	 *     address space (e.g. heap extended into 0x55... range),
	 *     cq_args_mmap_local() reports a clear diagnostic and
	 *     returns -EADDRINUSE; the user can re-run since the
	 *     source's libmlx5 mmap VA is randomized per run.
	 *
	 * The bytes themselves don't matter for kernel-side validation
	 * (v0 holds the CQ but never arms it / writes a CQE), so the
	 * mappings can be anonymous-zero-filled. What matters is that
	 * ib_umem_pin succeeds, the resulting sg_table has total
	 * length matching the placeholder len so
	 * mlx5_vfmig_bind_user_cq's length check accepts it, and the
	 * doorbell mapping is exactly PAGE_SIZE so
	 * mlx5_vfmig_bind_user_dbr's single-page invariant holds.
	 */
	uint64_t	local_buf_addr;
	uint64_t	local_db_addr;
};

/*
 * Allocate the destination-side mappings backing local_buf_addr
 * (CQE ring) and local_db_addr (doorbell page). Returns 0 on
 * success, -errno on mmap failure. The mappings leak at probe
 * exit, which is fine for a test binary.
 *
 * The CQE-ring mmap is anonymous (any VA in the probe's mm; the
 * placeholder is cqn-keyed, not VA-keyed -- see struct cq_args
 * docstring). The doorbell mmap is MAP_FIXED_NOREPLACE at
 * src_db_addr & PAGE_MASK to satisfy mlx5_vfmig_bind_user_dbr's
 * (KIND_DBR, virt & PAGE_MASK) lookup invariant. If the source's
 * VA collides with the probe's own address space we report a clear
 * diagnostic and return -EADDRINUSE; the user can re-run, since
 * the source's libmlx5 anonymous mmap VA is randomized per run.
 *
 * The full src_db_addr (with its in-page offset preserved verbatim)
 * is then used as the db_addr argument to RESTORE_CQ. The kernel
 * masks to PAGE_MASK for the placeholder lookup but preserves the
 * offset for cq->db.dma = sg_dma_address + (virt & ~PAGE_MASK).
 * libmlx5 normally lays out mlx5dv_cq.dbrec at a 64-byte-aligned
 * offset within a shared doorbell page (multiple 8-byte records
 * packed per page across QPs/SRQs/CQs in the same ucontext), so a
 * non-zero in-page offset is the realistic case.
 */
static int cq_args_mmap_local(struct cq_args *a)
{
	long page_size = sysconf(_SC_PAGESIZE);
	size_t cqe_buf_bytes = (size_t)a->src_cqe * a->src_cqe_size;
	size_t cqe_buf_aligned;
	uint64_t db_page_addr;
	void *cqe_p, *db_p;

	if (page_size <= 0)
		page_size = 4096;
	cqe_buf_aligned = (cqe_buf_bytes + page_size - 1) &
			  ~(size_t)(page_size - 1);
	if (cqe_buf_aligned == 0)
		cqe_buf_aligned = page_size;

	cqe_p = mmap(NULL, cqe_buf_aligned,
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (cqe_p == MAP_FAILED) {
		fprintf(stderr,
			"cq_restore_mlx5: mmap(cqe_buf %zu) failed: %s\n",
			cqe_buf_aligned, strerror(errno));
		return -errno;
	}
	a->local_buf_addr = (uint64_t)(uintptr_t)cqe_p;

	/*
	 * MAP_FIXED_NOREPLACE at the source's page-aligned doorbell
	 * VA. This simulates what CRIU's mm-replay step would do for
	 * the destination process: mmap the same VAs with MAP_FIXED so
	 * the user-virt -> umem mapping matches across save/load.
	 *
	 * If the probe's own address space (heap, .bss, libc mmaps,
	 * stack-guard, ...) already occupies that VA, MAP_FIXED_NOREPLACE
	 * fails with EEXIST without unmapping the existing range; we
	 * propagate that as -EADDRINUSE so the harness sees a clear
	 * "ASLR collision" failure rather than a silent succeed-then-
	 * fail-during-bind path.
	 */
	db_page_addr = a->src_db_addr & ~(uint64_t)(page_size - 1);
	db_p = mmap((void *)(uintptr_t)db_page_addr, page_size,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
		    -1, 0);
	if (db_p == MAP_FAILED) {
		int err = errno;
		fprintf(stderr,
			"cq_restore_mlx5: MAP_FIXED_NOREPLACE(0x%llx, %ld) for src DBR\n"
			"                 page failed: %s. The source's libmlx5\n"
			"                 doorbell mmap VA collides with this probe's\n"
			"                 own address space. Re-run -- the source's\n"
			"                 ASLR-randomized VA will likely land elsewhere.\n",
			(unsigned long long)db_page_addr, page_size, strerror(err));
		munmap(cqe_p, cqe_buf_aligned);
		return -EADDRINUSE;
	}
	if ((uint64_t)(uintptr_t)db_p != db_page_addr) {
		fprintf(stderr,
			"cq_restore_mlx5: MAP_FIXED_NOREPLACE returned 0x%llx instead of\n"
			"                 the requested 0x%llx (kernel ignored the hint?)\n",
			(unsigned long long)(uintptr_t)db_p,
			(unsigned long long)db_page_addr);
		munmap(db_p, page_size);
		munmap(cqe_p, cqe_buf_aligned);
		return -EFAULT;
	}
	/* Preserve the source's full in-page offset for the db_addr arg. */
	a->local_db_addr = a->src_db_addr;

	printf("cq_restore_mlx5: local_buf_addr=0x%llx (anonymous mmap, %zu bytes;\n"
	       "                                       cqn-keyed lookup, VA arbitrary)\n"
	       "                 local_db_addr=0x%llx (MAP_FIXED_NOREPLACE @ src page\n"
	       "                                      0x%llx; %ld bytes; in-page offset\n"
	       "                                      0x%llx preserved verbatim)\n"
	       "                 src_buf_addr=0x%llx kept for identity logging only\n",
	       (unsigned long long)a->local_buf_addr, cqe_buf_aligned,
	       (unsigned long long)a->local_db_addr,
	       (unsigned long long)db_page_addr, page_size,
	       (unsigned long long)(a->src_db_addr & (uint64_t)(page_size - 1)),
	       (unsigned long long)a->src_buf_addr);
	return 0;
}

/*
 * Subtest 1: gate-negative.
 *
 * Open a fresh ucontext WITHOUT MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE.
 * RESTORE_CQ has no IDR-class core attrs (unlike RESTORE_MR's
 * PD_HANDLE), so the dispatcher reaches the gate predicate
 * immediately -- no preliminary alloc dance is needed.
 */
static int subtest_gate_negative(const char *cdev_path,
				 const struct cq_args *a)
{
	struct ib_uverbs_get_context_resp resp_core = {};
	struct mlx5_ib_alloc_ucontext_resp resp_drv = {};
	struct mlx5_ib_restore_cq_req uhw = {
		.cqn      = a->src_cqn,
		.cqe_size = a->src_cqe_size,
		.buf_addr = a->local_buf_addr,
		.db_addr  = a->local_db_addr,
	};
	uint32_t resp_cqe = 0;
	int fd, ret;
	int fails = 0;

	printf("[1] gate: ucontext WITHOUT VFMIG_RESTORE -> RESTORE_CQ must -EPERM\n");

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

	ret = do_restore_cq(fd, a->cq_target_handle, a->src_cqe,
			    USER_HANDLE_TAG, a->comp_vector, -1, &uhw,
			    &resp_cqe);
	if (ret == -EPERM) {
		printf("  PASS RESTORE_CQ on non-restore-mode ucontext -> -EPERM\n");
	} else if (ret == 0) {
		fprintf(stderr,
			"  FAIL RESTORE_CQ on non-restore-mode ucontext succeeded "
			"(security regression: predicate not consulted)\n");
		fails++;
	} else {
		fprintf(stderr,
			"  FAIL RESTORE_CQ on non-restore-mode ucontext -> %s "
			"(expected -EPERM)\n", strerror(-ret));
		fails++;
	}

	close(fd);
	return fails;
}

/*
 * Helper: build a UHW + invoke RESTORE_CQ with the given args,
 * expecting the given errno. Used by subtests 2-6 that vary one
 * knob at a time.
 */
static int expect_restore_cq_errno(int fd, const struct cq_args *a,
				   const struct mlx5_ib_restore_cq_req *uhw,
				   uint32_t cqe, uint32_t comp_vector,
				   int want_errno, const char *desc)
{
	uint32_t resp_cqe = 0;
	int ret = do_restore_cq(fd, a->cq_target_handle, cqe,
				USER_HANDLE_TAG, comp_vector, -1, uhw,
				&resp_cqe);
	if (ret == want_errno) {
		printf("  PASS RESTORE_CQ(%s) -> %s\n", desc,
		       strerror(-ret));
		return 0;
	}
	fprintf(stderr,
		"  FAIL RESTORE_CQ(%s) -> %s (expected %s)\n",
		desc, ret ? strerror(-ret) : "0 (success)",
		strerror(-want_errno));
	return 1;
}

static int subtest_uapi_reject_zero_cqn(int fd, const struct cq_args *a)
{
	struct mlx5_ib_restore_cq_req uhw = {
		.cqn      = 0,
		.cqe_size = a->src_cqe_size,
		.buf_addr = a->local_buf_addr,
		.db_addr  = a->local_db_addr,
	};

	printf("[2] uapi reject: cqn=0 must -EINVAL\n");
	return expect_restore_cq_errno(fd, a, &uhw, a->src_cqe,
				       a->comp_vector, -EINVAL, "cqn=0");
}

static int subtest_uapi_reject_cqn_high_bits(int fd, const struct cq_args *a)
{
	struct mlx5_ib_restore_cq_req uhw = {
		.cqn      = a->src_cqn | 0x01000000u,	/* set bit 24 */
		.cqe_size = a->src_cqe_size,
		.buf_addr = a->local_buf_addr,
		.db_addr  = a->local_db_addr,
	};

	printf("[3] uapi reject: cqn high bits set must -EINVAL\n");
	return expect_restore_cq_errno(fd, a, &uhw, a->src_cqe,
				       a->comp_vector, -EINVAL,
				       "cqn>0xffffff");
}

static int subtest_uapi_reject_bad_cqe_size(int fd, const struct cq_args *a)
{
	struct mlx5_ib_restore_cq_req uhw = {
		.cqn      = a->src_cqn,
		.cqe_size = 42,				/* not 64/128 */
		.buf_addr = a->local_buf_addr,
		.db_addr  = a->local_db_addr,
	};

	printf("[4] uapi reject: cqe_size != {64,128} must -EINVAL\n");
	return expect_restore_cq_errno(fd, a, &uhw, a->src_cqe,
				       a->comp_vector, -EINVAL,
				       "cqe_size=42");
}

static int subtest_uapi_reject_reserved(int fd, const struct cq_args *a)
{
	struct mlx5_ib_restore_cq_req uhw = {
		.cqn      = a->src_cqn,
		.cqe_size = a->src_cqe_size,
		.buf_addr = a->local_buf_addr,
		.db_addr  = a->local_db_addr,
		.reserved = 0xDEADu,
	};

	printf("[5] uapi reject: reserved!=0 must -EINVAL\n");
	return expect_restore_cq_errno(fd, a, &uhw, a->src_cqe,
				       a->comp_vector, -EINVAL,
				       "reserved=0xDEAD");
}

static int subtest_bad_comp_vector(int fd, const struct cq_args *a)
{
	struct mlx5_ib_restore_cq_req uhw = {
		.cqn      = a->src_cqn,
		.cqe_size = a->src_cqe_size,
		.buf_addr = a->local_buf_addr,
		.db_addr  = a->local_db_addr,
	};

	printf("[6] dispatcher reject: comp_vector=0x%x out of range must -EINVAL\n",
	       BAD_COMP_VECTOR_SENTINEL);
	return expect_restore_cq_errno(fd, a, &uhw, a->src_cqe,
				       BAD_COMP_VECTOR_SENTINEL, -EINVAL,
				       "comp_vector out-of-range");
}

/*
 * Subtest 7: COMP_CHANNEL rejection. v0 plugin policy hard-rejects
 * the comp_channel attr in uverbs_std_types_restore.c's
 * RESTORE_CQ dispatcher; this subtest locks that in.
 *
 * Allocate a comp_channel via the legacy write path (the only
 * way to get one without going through libibverbs's per-driver
 * provider, which mlx5 doesn't expose for the raw cdev fd we have
 * here), pass its fd to RESTORE_CQ, expect -EOPNOTSUPP.
 */
static int subtest_comp_channel_rejected(int fd, const struct cq_args *a)
{
	struct mlx5_ib_restore_cq_req uhw = {
		.cqn      = a->src_cqn,
		.cqe_size = a->src_cqe_size,
		.buf_addr = a->local_buf_addr,
		.db_addr  = a->local_db_addr,
	};
	uint32_t resp_cqe = 0;
	int channel_fd, ret;
	int fails = 0;

	printf("[7] dispatcher reject: COMP_CHANNEL passed must -EOPNOTSUPP\n"
	       "    (v0 plugin policy: comp_channel restore deferred to S5c/S10)\n");

	channel_fd = do_create_comp_channel(fd);
	if (channel_fd < 0) {
		fprintf(stderr,
			"  FAIL CREATE_COMP_CHANNEL: %s\n",
			strerror(-channel_fd));
		return 1;
	}

	ret = do_restore_cq(fd, a->cq_target_handle, a->src_cqe,
			    USER_HANDLE_TAG, a->comp_vector, channel_fd,
			    &uhw, &resp_cqe);
	if (ret == -EOPNOTSUPP) {
		printf("  PASS RESTORE_CQ(comp_channel=%d) -> -EOPNOTSUPP\n",
		       channel_fd);
	} else if (ret == 0) {
		fprintf(stderr,
			"  FAIL RESTORE_CQ(comp_channel=%d) succeeded "
			"(v0 dispatcher should have hard-rejected)\n",
			channel_fd);
		fails++;
	} else {
		fprintf(stderr,
			"  FAIL RESTORE_CQ(comp_channel=%d) -> %s "
			"(expected -EOPNOTSUPP)\n",
			channel_fd, strerror(-ret));
		fails++;
	}

	close(channel_fd);
	return fails;
}

static int subtest_happy_path(int fd, const struct cq_args *a)
{
	struct mlx5_ib_restore_cq_req uhw = {
		.cqn      = a->src_cqn,
		.cqe_size = a->src_cqe_size,
		.buf_addr = a->local_buf_addr,
		.db_addr  = a->local_db_addr,
	};
	uint32_t resp_cqe = 0;
	uint32_t list[64] = {};
	uint32_t total = 0;
	int ret;
	int fails = 0;

	printf("[8] happy path: RESTORE_CQ(target=0x%x, cqn=%u, cqe=%u, cqe_size=%u, comp_vector=%u)\n",
	       a->cq_target_handle, a->src_cqn, a->src_cqe, a->src_cqe_size,
	       a->comp_vector);

	ret = do_restore_cq(fd, a->cq_target_handle, a->src_cqe,
			    USER_HANDLE_TAG, a->comp_vector, -1, &uhw,
			    &resp_cqe);
	if (ret) {
		fprintf(stderr,
			"  FAIL RESTORE_CQ(target=0x%x): %s%s\n",
			a->cq_target_handle, strerror(-ret),
			ret == -EOPNOTSUPP
			? "  (mlx5_ib_restore_cq not registered in mlx5_ib_dev_ops?)"
			: ret == -EPERM
			? "  (mlx5_ib_ucontext_is_restore_mode not reporting true?)"
			: "");
		return 1;
	}

	if (resp_cqe != a->src_cqe) {
		fprintf(stderr,
			"  FAIL RESP_CQE=%u (expected %u byte-identical to attr->cqe;\n"
			"       mlx5 honours the source-reported value verbatim, no roundup)\n",
			resp_cqe, a->src_cqe);
		fails++;
	} else {
		printf("  PASS RESP_CQE=%u byte-identical to attr->cqe (mlx5 honours)\n",
		       resp_cqe);
	}

	ret = do_info_handles_cq(fd, list, 64, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(CQ): %s\n",
			strerror(-ret));
		return 1;
	}
	if (!handle_present(list, total, a->cq_target_handle)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(CQ): handle 0x%x not in list (total=%u)\n",
			a->cq_target_handle, total);
		fails++;
	} else {
		printf("  PASS INFO_HANDLES(CQ) returned 0x%x among %u entries\n",
		       a->cq_target_handle, total);
	}
	return fails;
}

static int subtest_collision(int fd, const struct cq_args *a)
{
	struct mlx5_ib_restore_cq_req uhw = {
		.cqn      = a->src_cqn,
		.cqe_size = a->src_cqe_size,
		.buf_addr = a->local_buf_addr,
		.db_addr  = a->local_db_addr,
	};
	uint32_t resp_cqe = 0;
	int ret;

	printf("[9] collision: second RESTORE_CQ(target=0x%x) must -EBUSY\n",
	       a->cq_target_handle);

	ret = do_restore_cq(fd, a->cq_target_handle, a->src_cqe,
			    USER_HANDLE_TAG, a->comp_vector, -1, &uhw,
			    &resp_cqe);
	if (ret == -EBUSY) {
		printf("  PASS second RESTORE_CQ(target=0x%x) -> -EBUSY\n",
		       a->cq_target_handle);
		return 0;
	}
	fprintf(stderr,
		"  FAIL second RESTORE_CQ(target=0x%x) -> %s (expected -EBUSY)\n",
		a->cq_target_handle, ret ? strerror(-ret) : "0 (success)");
	return 1;
}

/*
 * Subtest 10: v0 dealloc semantics on a still-loaded VHCA.
 *
 * Symmetric with PD's subtest 7 (asymmetric with MR's subtest 8).
 * After LOAD_VHCA_STATE the destination FW still holds the source's
 * cqn-referencing QPCs/SRQCs as a tracked FW resource dep. v0 only
 * restores CQs into the kernel ufile; the dependent FW objects
 * have no kernel uobjects yet, so the kernel cannot dealloc them
 * first. Issuing MLX5_CMD_OP_DESTROY_CQ against an FW cqn that
 * still has FW-side dependents must therefore be rejected by
 * firmware -- the FW returns status BAD_RES_STATE (0x9), which
 * cmd_status_to_err maps to -EINVAL.
 *
 * That rejection is the *correct* CRIU restore-ordering invariant:
 *   restore CQ -> restore QP/SRQ -> user destroys QP/SRQ
 *              -> user destroys CQ -> FW DESTROY_CQ succeeds.
 *
 * Because uverbs_destroy_uobject propagates the FW error out of
 * destroy_hw_struct without clearing uobj->object or removing the
 * idr handle (gated on the RESTORE-mode ufile teardown carve-out
 * landed in beea656e494d), INFO_HANDLES *must* still report
 * cq_target_handle after the failed DESTROY_CQ -- the uobj stays
 * parked in the ufile so a later cascading teardown (once S6+S7
 * land) can retry.
 *
 * v0 PASS criterion: DESTROY_CQ must -EINVAL AND INFO_HANDLES(CQ)
 * must still see the handle. We accept -EREMOTEIO and -EBUSY in
 * addition to -EINVAL to stay forward-compatible with FW versions
 * that map "has dependents" to RES_BUSY or surface the raw remote-
 * IO error.
 *
 * Recorded in design/uobject_restore.md S5b + S10.8.
 */
static int subtest_v0_dealloc(int fd, const struct cq_args *a)
{
	uint32_t list[64] = {};
	uint32_t total = 0;
	uint32_t async_events_reported = 0;
	int ret;

	printf("[10] v0 dealloc semantics: DESTROY_CQ(0x%x) on a cqn with FW\n"
	       "     QP/SRQ dependents must fail; INFO_HANDLES must still see it\n",
	       a->cq_target_handle);

	ret = do_destroy_cq(fd, a->cq_target_handle, &async_events_reported);
	if (ret == 0) {
		fprintf(stderr,
			"  FAIL DESTROY_CQ(0x%x) -> 0 (unexpected: cqn should\n"
			"       still have FW QPC/SRQC dependents from\n"
			"       LOAD_VHCA_STATE; FW should have rejected with\n"
			"       BAD_RES_STATE)\n", a->cq_target_handle);
		return 1;
	}
	if (ret != -EINVAL && ret != -EBUSY && ret != -EREMOTEIO) {
		fprintf(stderr,
			"  FAIL DESTROY_CQ(0x%x) -> %s (expected -EINVAL /\n"
			"       -EBUSY / -EREMOTEIO from FW BAD_RES_STATE)\n",
			a->cq_target_handle, strerror(-ret));
		return 1;
	}
	printf("  PASS DESTROY_CQ(0x%x) -> %s (FW QPC/SRQC dependents present -- as expected for v0)\n",
	       a->cq_target_handle, strerror(-ret));

	ret = do_info_handles_cq(fd, list, 64, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(CQ): %s\n",
			strerror(-ret));
		return 1;
	}
	if (!handle_present(list, total, a->cq_target_handle)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(CQ) lost 0x%x after a FAILED\n"
			"       DESTROY_CQ (uobj should still be parked)\n",
			a->cq_target_handle);
		return 1;
	}
	printf("  PASS INFO_HANDLES(CQ) still reports 0x%x after the failed destroy\n",
	       a->cq_target_handle);
	return 0;
}

/* ---------------------------------------------------------------------- */
/*                                main                                    */
/* ---------------------------------------------------------------------- */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <ibdev> <src_cqn> <src_cqe> <src_cqe_size>\n"
		"           <src_cq_buf_addr> <src_cq_db_addr>\n"
		"           [<comp_vector> [<cq_target_handle>]]\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *ibdev;
	struct cq_args a = {
		.comp_vector      = 0,
		.cq_target_handle = DEFAULT_CQ_TARGET_HANDLE,
	};
	char cdev_path[128];
	struct ib_uverbs_get_context_resp resp_core = {};
	struct mlx5_ib_alloc_ucontext_resp resp_drv = {};
	int fd_restore = -1, ret;
	int fails = 0;
	unsigned long u;

	if (argc < 7 || argc > 9) {
		usage(argv[0]);
		return 2;
	}
	ibdev = argv[1];

	u = strtoul(argv[2], NULL, 0);
	if (u == 0 || u > 0xffffffu) {
		fprintf(stderr,
			"cq_restore_mlx5: src_cqn=%lu out of range (1..0xffffff)\n",
			u);
		return 2;
	}
	a.src_cqn = (uint32_t)u;

	u = strtoul(argv[3], NULL, 0);
	if (u == 0 || u > 0xffffu) {
		fprintf(stderr,
			"cq_restore_mlx5: src_cqe=%lu out of range (1..0xffff)\n",
			u);
		return 2;
	}
	a.src_cqe = (uint32_t)u;

	u = strtoul(argv[4], NULL, 0);
	if (u != 64 && u != 128) {
		fprintf(stderr,
			"cq_restore_mlx5: src_cqe_size=%lu must be 64 or 128\n",
			u);
		return 2;
	}
	a.src_cqe_size = (uint32_t)u;

	a.src_buf_addr = strtoull(argv[5], NULL, 0);
	a.src_db_addr  = strtoull(argv[6], NULL, 0);

	if (argc >= 8) {
		u = strtoul(argv[7], NULL, 0);
		if (u > 0xffffffffu) {
			fprintf(stderr,
				"cq_restore_mlx5: comp_vector out of range\n");
			return 2;
		}
		a.comp_vector = (uint32_t)u;
	}
	if (argc >= 9) {
		u = strtoul(argv[8], NULL, 0);
		if (u > 0xffffffffu) {
			fprintf(stderr,
				"cq_restore_mlx5: cq_target_handle out of range\n");
			return 2;
		}
		a.cq_target_handle = (uint32_t)u;
	}

	if (resolve_cdev_path(ibdev, cdev_path, sizeof(cdev_path)) != 0)
		return 2;
	printf("cq_restore_mlx5: ibdev=%s cdev=%s\n"
	       "                 src_cqn=%u src_cqe=%u src_cqe_size=%u\n"
	       "                 src_buf_addr=0x%llx src_db_addr=0x%llx\n"
	       "                 comp_vector=%u cq_target_handle=0x%x\n",
	       ibdev, cdev_path, a.src_cqn, a.src_cqe, a.src_cqe_size,
	       (unsigned long long)a.src_buf_addr,
	       (unsigned long long)a.src_db_addr,
	       a.comp_vector, a.cq_target_handle);

	/*
	 * Allocate the destination-side anonymous mappings that back
	 * the buf_addr / db_addr arguments to every RESTORE_CQ call
	 * below. See the docstring on @local_buf_addr / @local_db_addr
	 * in struct cq_args -- the kernel pins user pages from
	 * current->mm at those VAs, so without these mappings the
	 * happy-path subtest fails -EFAULT on pin_user_pages_fast().
	 */
	if (cq_args_mmap_local(&a) != 0)
		return 2;

	/* Subtest 1 uses its own short-lived non-restore-mode ucontext. */
	fails += subtest_gate_negative(cdev_path, &a);

	/* Subtests 2-9 + 10 share one restore-mode ucontext. */
	fd_restore = open(cdev_path, O_RDWR | O_CLOEXEC);
	if (fd_restore < 0) {
		fprintf(stderr,
			"cq_restore_mlx5: open(%s) for restore-mode ucontext: %s\n",
			cdev_path, strerror(errno));
		return 2;
	}
	ret = do_get_context(fd_restore,
			     MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
			     &resp_core, &resp_drv);
	if (ret) {
		fprintf(stderr,
			"cq_restore_mlx5: GET_CONTEXT(flags=VFMIG_RESTORE): %s\n",
			strerror(-ret));
		close(fd_restore);
		return 2;
	}

	fails += subtest_uapi_reject_zero_cqn(fd_restore, &a);
	fails += subtest_uapi_reject_cqn_high_bits(fd_restore, &a);
	fails += subtest_uapi_reject_bad_cqe_size(fd_restore, &a);
	fails += subtest_uapi_reject_reserved(fd_restore, &a);
	fails += subtest_bad_comp_vector(fd_restore, &a);
	fails += subtest_comp_channel_rejected(fd_restore, &a);
	fails += subtest_happy_path(fd_restore, &a);
	fails += subtest_collision(fd_restore, &a);

	if (fails) {
		fprintf(stderr,
			"\ncq_restore_probe_mlx5_vfmig: %d subtest failure(s) "
			"before READY; aborting\n", fails);
		close(fd_restore);
		return 1;
	}

	/*
	 * READY checkpoint. The adopted CQ lives on fd_restore at the
	 * requested target handle. The harness can now run any
	 * out-of-process FW-side verifier -- typically
	 * `mlx5_vfmig probe_cqn <vf> <cqn>` -- while the object is
	 * alive in destination FW. On "quit\n" we drop to subtest 10.
	 */
	printf("ibdev=%s\n", ibdev);
	printf("adopted_cqn=%u\n", a.src_cqn);
	printf("adopted_cqe=%u\n", a.src_cqe);
	printf("adopted_cqe_size=%u\n", a.src_cqe_size);
	printf("cq_target_handle=0x%x\n", a.cq_target_handle);
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
			"\ncq_restore_probe_mlx5_vfmig: FAIL (%d subtest failure(s))\n",
			fails);
		return 1;
	}
	printf("\ncq_restore_probe_mlx5_vfmig: PASS\n");
	return 0;
}
