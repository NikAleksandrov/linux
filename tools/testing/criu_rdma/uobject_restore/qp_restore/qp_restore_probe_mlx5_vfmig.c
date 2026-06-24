// SPDX-License-Identifier: GPL-2.0
/*
 * qp_restore_probe_mlx5_vfmig -- end-to-end driver-level validation
 * of S6b: UVERBS_METHOD_RESTORE_QP + mlx5_ib_restore_qp Model A
 * (design/uobject_restore.md S6b + S9.1 S6b).
 *
 * What we validate against a bound, post-LOAD destination mlx5
 * ib_device:
 *
 *   1. Gate (negative). A ucontext opened WITHOUT
 *      MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE cannot invoke RESTORE_QP;
 *      the dispatcher must return -EPERM via the per-driver
 *      mlx5_ib_ucontext_is_restore_mode predicate. Verified before
 *      we waste any setup work on a useless ucontext.
 *
 *   2. Setup. On a restore-mode ucontext, RESTORE_PD(src_pdn) and
 *      RESTORE_CQ(src_cqn) succeed -- we MUST have valid PD and CQ
 *      ufile handles in hand to satisfy RESTORE_QP's mandatory
 *      UVERBS_ATTR_RESTORE_QP_{PD,SEND_CQ,RECV_CQ}_HANDLE IDR
 *      references. Self-loopback test pattern: the same restored
 *      CQ handle is reused for both send and recv (matches what
 *      fw_id_continuity_probe set up source-side).
 *
 *   3-7. UAPI rejects -- subtests that exercise the driver-private
 *      mlx5_ib_restore_qp_req payload validation in
 *      mlx5_ib_restore_qp:
 *        3. qpn=0       -- kernel reserves cqn/qpn=0 as sentinel.
 *        4. qpn high    -- (qpn & ~0xffffff) != 0 must -EINVAL.
 *        5. reserved!=0 -- defends the future-flags slot.
 *        6. uidx high   -- (uidx & ~0xffffff) != 0 must -EINVAL.
 *        7. rq_shift    -- rq_wqe_shift outside [4,16] must
 *                          -EINVAL when rq_wqe_count > 0.
 *
 *   8-9. Dispatcher rejects -- exercise the generic
 *      uverbs_std_types_restore.c::UVERBS_HANDLER pre-driver guards:
 *        8. qp_type=XRC_INI -- v0 only accepts RC/UC/UD; the
 *           dispatcher's switch returns -EOPNOTSUPP.
 *        9. qp_state=SQE    -- v0 captured states are
 *           RESET/INIT/RTR/RTS; SQE/SQD/ERR -EOPNOTSUPP.
 *
 *   10. Happy path. RESTORE_QP with the source's
 *       (qpn, sq_wqe_count, rq_wqe_count, rq_wqe_shift, buf_addr,
 *       db_addr) and the harness-supplied qp_type/qp_state lands a
 *       fresh kernel-side mlx5_ib_qp at qp_target_handle on the
 *       restore-mode ucontext. RESP_QPN must equal req.qpn (mlx5
 *       stamps ibqp->qp_num verbatim from the UHW source qpn,
 *       wire-visible identity preservation; the dispatcher echoes
 *       qp->qp_num back via UVERBS_ATTR_RESTORE_QP_RESP_QPN).
 *       INFO_HANDLES(QP) must contain qp_target_handle.
 *
 *   11. Collision. A second RESTORE_QP(qp_target_handle) on the
 *       same ucontext returns -EBUSY from the xa_insert collision
 *       inside rdma_alloc_begin_uobject_at_handle.
 *
 *   12. READY checkpoint. The probe prints
 *
 *           ibdev=<n>
 *           adopted_qpn=<qpn>
 *           qp_target_handle=<h>
 *           READY
 *
 *       and blocks on stdin until it reads "quit\n". While the
 *       probe is parked the adopted QP is alive in firmware, so
 *       the driver script can:
 *
 *         - issue MLX5_VFMIG_IOC_QUERY_QP via the PF cdev to
 *           confirm FW-side liveness (qpc.state, qpc.next_send_psn,
 *           qpc.next_rcv_psn, primary_address_path, ... byte-equal
 *           to source pre-SAVE under the live verb path), or
 *         - drive any other follow-on verification against the
 *           destination ibdev.
 *
 *       On "quit\n" the probe drops to subtest 13.
 *
 *   13. v0 dealloc semantics. ASYMMETRIC with CQ's subtest 10 and
 *       PD's subtest 7 (SYMMETRIC with MR's subtest 8).
 *
 *       In the FW resource graph QP is a *leaf*: PDs / CQs /
 *       SRQs / MRs are tracked dependents OF the QP, but no FW
 *       resource is tagged with "depends on this qpn" (the peer
 *       QP's qpc.remote_qpn refers to a different qpn entirely
 *       and -- on K7 self-loopback -- happens to equal this qpn,
 *       but FW does NOT install a back-reference for the cross-
 *       link target). FW DESTROY_QP on the orphan adopted qpn is
 *       therefore ACCEPTED -- its only dependents (PD, CQs) are
 *       still alive and are themselves PARENTS, not children, of
 *       the qpn.
 *
 *       v0 PASS criterion is the standard alloc/dealloc round-trip:
 *       DESTROY_QP must succeed AND INFO_HANDLES(QP) must NO
 *       LONGER see the handle (uverbs_destroy_uobject removes the
 *       idr handle on a successful destroy_hw). Recorded in
 *       design/uobject_restore.md S6b + S10.8.
 *
 *       Note: under the harness flow the parent CQ/PD remain
 *       alive after this subtest (their dealloc is the next
 *       phase the harness itself drives, not this probe). The
 *       point of subtest 13 is to confirm "leaf-of-graph
 *       resources can be reaped without the cascade S6+S7 are
 *       still developing", which is the v0 happy-case for QP.
 *
 * The probe deliberately stays libibverbs-only and pure-uverbs (no
 * libmlx5, no mlx5dv) so it can drive the
 * MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE flag in udata (libmlx5 has no
 * plumbing for it) and the new mlx5_ib_restore_qp_req UHW payload.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/qp_restore/qp_restore_probe_mlx5_vfmig
 *
 * Usage:
 *   ./qp_restore_probe_mlx5_vfmig <ibdev> \
 *       <src_pdn> \
 *       <src_cqn> <src_cqe> <src_cqe_size> \
 *           <src_cq_buf_addr> <src_cq_db_addr> \
 *       <src_qpn> <sq_wqe_count> <rq_wqe_count> <rq_wqe_shift> \
 *           <src_qp_buf_addr> <src_qp_db_addr> \
 *       <qp_type> <qp_state> \
 *       [<qp_target_handle>]
 *
 * <ibdev>           destination ib_device name (e.g. mlx5_2),
 *                   typically picked up from `find_ib_dev_for_pci
 *                   <dst_vf_bdf>` in the harness.
 * <src_pdn>         FW pdn captured pre-SAVE on the source
 *                   ucontext (from fw_id_continuity_probe's pdn=
 *                   line).
 * <src_cqn>         FW cqn captured pre-SAVE (cqn= line). The
 *                   source uses one CQ for both send and recv on
 *                   self-loopback, so we restore one CQ and reuse
 *                   the resulting ufile handle for both
 *                   {SEND,RECV}_CQ_HANDLE.
 * <src_cqe>         The userspace-visible cqe count the source
 *                   ucontext got back from ibv_create_cq
 *                   (mlx5dv_cq.cqe_cnt; equal to cq->cqe).
 * <src_cqe_size>    64 or 128, from mlx5dv_cq.cqe_size.
 * <src_cq_buf_addr> User VA of the source's CQE ring buffer
 *                   (mlx5dv_cq.buf). Identity-only logging; the
 *                   actual RESTORE_CQ uses local_*_addr (anonymous
 *                   mmap on the destination).
 * <src_cq_db_addr>  User VA of the source's CQ doorbell record
 *                   (mlx5dv_cq.dbrec). Reused as the RESTORE_CQ
 *                   db_addr argument (kernel masks to PAGE_MASK
 *                   for the placeholder lookup; in-page offset is
 *                   preserved verbatim).
 * <src_qpn>         FW qpn captured pre-SAVE (qpn= line). The
 *                   destination kernel adopts this exact qpn into
 *                   the new mlx5_core_qp registration via
 *                   mlx5_qpc_adopt_qp.
 * <sq_wqe_count>    libmlx5-negotiated SQ depth (mlx5dv_qp.sq.wqe_cnt).
 * <rq_wqe_count>    libmlx5-negotiated RQ depth (mlx5dv_qp.rq.wqe_cnt).
 * <rq_wqe_shift>    log2(mlx5dv_qp.rq.stride).
 * <src_qp_buf_addr> User VA of the source's WQ-ring umem base
 *                   (mlx5dv_qp.rq.buf; libmlx5 always puts RQ at
 *                   offset 0). Same MAP_FIXED_NOREPLACE replay
 *                   pattern as cq_restore: simulates CRIU's
 *                   destination mm replay step.
 * <src_qp_db_addr>  User VA of the source's QP doorbell record
 *                   (mlx5dv_qp.dbrec). Same passthrough rule as
 *                   the CQ DBR.
 * <qp_type>         RC | UC | UD (matches the dispatcher's v0
 *                   accepted set). The harness pulls the source
 *                   probe's reported `qp_state=` line and
 *                   `qp_type` is implicit RC for K6.
 * <qp_state>        RESET | INIT | RTR | RTS (matches the
 *                   dispatcher's v0 accepted set). The harness
 *                   pulls this from `qp_state=` in the source
 *                   probe's READY-line dictionary.
 * <qp_target_handle> Defaults to 0x4248. Picked distinct from
 *                   PD/CQ/MR probe defaults (0x4242, 0x4244,
 *                   0x4246 respectively) so a future combined
 *                   harness can keep them apart.
 *
 * No dependency on libmlx5 or rdma-core's userspace mlx5 UAPI: the
 * mlx5-side ABI surface we need (GET_CONTEXT req/resp shape, the
 * RESTORE_QP UHW struct, the VFMIG flag) is inlined below. Keep
 * in sync with the authoritative headers at
 * include/uapi/rdma/mlx5-abi.h, include/uapi/rdma/ib_user_verbs.h,
 * and include/uapi/rdma/ib_user_ioctl_cmds.h.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <rdma/ib_user_verbs.h>

/* see uapi/rdma/ib_user_ioctl_verbs.h */
#define IB_UVERBS_QPT_RC		2
#define IB_UVERBS_QPT_UC		3
#define IB_UVERBS_QPT_UD		4
#define IB_UVERBS_QPT_XRC_INI		9

/* see uapi/rdma/ib_user_verbs.h enum ib_qp_state */
#define IB_QPS_RESET			0
#define IB_QPS_INIT			1
#define IB_QPS_RTR			2
#define IB_QPS_RTS			3
#define IB_QPS_SQD			4
#define IB_QPS_SQE			5
#define IB_QPS_ERR			6

/* see uapi/rdma/mlx5-abi.h */
enum {
	MLX5_LIB_CAP_4K_UAR		= (uint64_t)1 << 0,
};
enum {
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
 * Driver-private UHW payload for UVERBS_METHOD_RESTORE_PD on mlx5.
 * Mirrors include/uapi/rdma/mlx5-abi.h. Inlined here so the probe
 * doesn't depend on a freshly-`make headers_install`-d userspace
 * tree.
 */
struct mlx5_ib_restore_pd_req {
	uint32_t	pdn;
	uint32_t	reserved;
	uint64_t	reserved2;
} __attribute__((aligned(8)));

/* Driver-private UHW payload for UVERBS_METHOD_RESTORE_CQ on mlx5. */
struct mlx5_ib_restore_cq_req {
	uint64_t	buf_addr;
	uint64_t	db_addr;
	uint32_t	cqn;
	uint32_t	cqe_size;
	uint32_t	reserved;
	uint32_t	reserved2;
} __attribute__((aligned(8)));

/*
 * Driver-private UHW payload for UVERBS_METHOD_RESTORE_QP on mlx5.
 * Mirrors include/uapi/rdma/mlx5-abi.h struct mlx5_ib_restore_qp_req
 * (kernel commit 92e08f3eb6f9 "RDMA/mlx5: Define mlx5_ib_restore_qp_req
 * UAPI"). 64 bytes -- well above the 8-byte inline-UHW threshold so
 * the dispatcher always takes the userspace-pointer path.
 */
struct mlx5_ib_restore_qp_req {
	uint64_t	buf_addr;
	uint64_t	db_addr;
	uint64_t	sq_buf_addr;
	uint32_t	qpn;
	uint32_t	sq_wqe_count;
	uint32_t	rq_wqe_count;
	uint32_t	rq_wqe_shift;
	uint32_t	flags;
	uint32_t	uidx;
	uint32_t	bfreg_index;
	uint32_t	ece_options;
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

/*
 * Object / method / attr ids -- see include/uapi/rdma/ib_user_ioctl_cmds.h.
 * UVERBS_OBJECT_PD = 1, _CQ = 3, _QP = 4, _RESTORE = 18 (the
 * uverbs_default_objects.UVERBS_OBJECT_RESTORE slot the dispatcher
 * registers).
 */
#define UVERBS_OBJECT_DEVICE			0
#define UVERBS_OBJECT_PD			1
#define UVERBS_OBJECT_CQ			3
#define UVERBS_OBJECT_QP			4
#define UVERBS_OBJECT_RESTORE			18

#define UVERBS_METHOD_INFO_HANDLES		1
#define UVERBS_ATTR_INFO_OBJECT_ID		0
#define UVERBS_ATTR_INFO_TOTAL_HANDLES		1
#define UVERBS_ATTR_INFO_HANDLES_LIST		2

#define UVERBS_METHOD_RESTORE_PD		0
#define UVERBS_ATTR_RESTORE_PD_HANDLE		0

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

#define UVERBS_METHOD_RESTORE_QP		3
enum {
	UVERBS_ATTR_RESTORE_QP_HANDLE		= 0,
	UVERBS_ATTR_RESTORE_QP_PD_HANDLE	= 1,
	UVERBS_ATTR_RESTORE_QP_SEND_CQ_HANDLE	= 2,
	UVERBS_ATTR_RESTORE_QP_RECV_CQ_HANDLE	= 3,
	UVERBS_ATTR_RESTORE_QP_SRQ_HANDLE	= 4,
	UVERBS_ATTR_RESTORE_QP_TYPE		= 5,
	UVERBS_ATTR_RESTORE_QP_STATE		= 6,
	UVERBS_ATTR_RESTORE_QP_USER_HANDLE	= 7,
	UVERBS_ATTR_RESTORE_QP_CAP		= 8,
	UVERBS_ATTR_RESTORE_QP_CREATE_FLAGS	= 9,
	UVERBS_ATTR_RESTORE_QP_EVENT_FD		= 10,
	UVERBS_ATTR_RESTORE_QP_RESP_QPN		= 11,
};

/*
 * struct ib_uverbs_qp_cap is supplied by <infiniband/ib_user_ioctl_verbs.h>
 * (pulled in transitively via <infiniband/verbs.h>); we just use it.
 */

#define UVERBS_ATTR_UHW_IN			((uint16_t)4096)

#define RDMA_DRIVER_MLX5			1

#define DEFAULT_PD_TARGET_HANDLE		0x4242u
#define DEFAULT_CQ_TARGET_HANDLE		0x4244u
#define DEFAULT_QP_TARGET_HANDLE		0x4248u
#define USER_HANDLE_TAG_PD			0xDEADBEEFCAFE0001ull
#define USER_HANDLE_TAG_CQ			0xDEADBEEFCAFE0002ull
#define USER_HANDLE_TAG_QP			0xDEADBEEFCAFE0003ull

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

static int do_destroy_qp(int fd, uint32_t qp_handle,
			 uint32_t *events_out)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_destroy_qp	core;
	} __attribute__((packed)) cmd = {};
	struct ib_uverbs_destroy_qp_resp resp = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_DESTROY_QP;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= sizeof(resp) / 4;
	cmd.core.response	= (uintptr_t)&resp;
	cmd.core.qp_handle	= qp_handle;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	if (events_out)
		*events_out = resp.events_reported;
	return 0;
}

/* ---------------------------------------------------------------------- */
/*                         uverbs ioctl helpers                           */
/* ---------------------------------------------------------------------- */

/*
 * Invoke UVERBS_METHOD_RESTORE_PD on the open ucontext fd. Used as
 * setup -- the resulting target_handle is passed as
 * RESTORE_QP_PD_HANDLE downstream.
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
 * Invoke UVERBS_METHOD_RESTORE_CQ on the open ucontext fd. Mirrors
 * cq_restore_probe_mlx5_vfmig::do_restore_cq with comp_channel
 * always omitted (this probe never exercises the v0 dispatcher
 * COMP_CHANNEL rejection path -- that's covered by cq_restore's
 * own subtest 7). Used as setup; the resulting target_handle is
 * passed as RESTORE_QP_{SEND,RECV}_CQ_HANDLE downstream.
 */
static int do_restore_cq(int fd, uint32_t target_handle, uint32_t cqe,
			 uint64_t user_handle, uint32_t comp_vector,
			 const struct mlx5_ib_restore_cq_req *uhw,
			 uint32_t *resp_cqe_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[6];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id = UVERBS_OBJECT_RESTORE;
	cmd.hdr.method_id = UVERBS_METHOD_RESTORE_CQ;
	cmd.hdr.driver_id = RDMA_DRIVER_MLX5;

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
 * UVERBS_METHOD_RESTORE_QP on the open ucontext fd. Always wires
 * up the mandatory core attrs (HANDLE / PD_HANDLE / SEND_CQ_HANDLE
 * / RECV_CQ_HANDLE / TYPE / STATE / USER_HANDLE / CAP / RESP_QPN)
 * plus the UHW_IN payload; CREATE_FLAGS is optional and passed via
 * @create_flags (UINT32_MAX means "omit the attr").
 *
 * Wire encoding (uverbs_ioctl.c):
 *   - PTR_IN(__u32):       len = 4, data = inline value
 *   - PTR_IN(__u64):       len = 8, data = inline value
 *   - PTR_IN(struct):      len = sizeof(struct), data = (uintptr_t)&buf
 *   - PTR_OUT(__u32):      len = 4, data = (uintptr_t)&user_buf
 *   - IDR-class:           len = 0, data = handle (uint32 inline)
 *   - CONST_IN(enum):      len = 8, data = inline enum value
 *   - FLAGS_IN:            len = 8, data = inline flag bits
 *   - UHW_IN:              len = sizeof(buf), data = (uintptr_t)&buf
 *
 * mlx5_ib_restore_qp enforces udata->outlen == 0, so we DO NOT
 * attach UHW_OUT.
 */
static int do_restore_qp(int fd, uint32_t target_handle,
			 uint32_t pd_handle,
			 uint32_t send_cq_handle, uint32_t recv_cq_handle,
			 uint64_t qp_type, uint64_t qp_state,
			 uint64_t user_handle,
			 const struct ib_uverbs_qp_cap *cap,
			 uint32_t create_flags,
			 const struct mlx5_ib_restore_qp_req *uhw,
			 uint32_t *resp_qpn_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[12];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id = UVERBS_OBJECT_RESTORE;
	cmd.hdr.method_id = UVERBS_METHOD_RESTORE_QP;
	cmd.hdr.driver_id = RDMA_DRIVER_MLX5;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_HANDLE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= target_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_PD_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= pd_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_SEND_CQ_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= send_cq_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_RECV_CQ_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= recv_cq_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_TYPE;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= qp_type;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_STATE;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= qp_state;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_USER_HANDLE;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= user_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_CAP;
	cmd.attrs[n].len	= sizeof(*cap);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)cap;
	n++;

	if (create_flags != UINT32_MAX) {
		cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_CREATE_FLAGS;
		cmd.attrs[n].len	= sizeof(uint64_t);
		cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
		cmd.attrs[n].data	= create_flags;
		n++;
	}

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_QP_RESP_QPN;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)resp_qpn_out;
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
 * INFO_HANDLES(@object_id). Mirrors info_handles_probe.c. Used to
 * confirm the QP target handle is present after RESTORE_QP and
 * absent after DESTROY_QP.
 */
static int do_info_handles(int fd, uint32_t object_id,
			   uint32_t *handles_out, uint32_t capacity_handles,
			   uint32_t *total_out)
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
	cmd.attrs[n].data	= object_id;
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
			"qp_restore_mlx5: ibv_get_device_list returned 0\n");
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
			"qp_restore_mlx5: ibdev '%s' not found; available:",
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

struct qp_args {
	/* PD setup. */
	uint32_t	src_pdn;

	/* CQ setup. */
	uint32_t	src_cqn;
	uint32_t	src_cqe;
	uint32_t	src_cqe_size;	/* 64 or 128 */
	uint64_t	src_cq_buf_addr;
	uint64_t	src_cq_db_addr;
	uint64_t	local_cq_buf_addr;
	uint64_t	local_cq_db_addr;

	/* QP setup. */
	uint32_t	src_qpn;
	uint32_t	sq_wqe_count;
	uint32_t	rq_wqe_count;
	uint32_t	rq_wqe_shift;
	uint64_t	src_qp_buf_addr;
	uint64_t	src_qp_db_addr;
	uint64_t	local_qp_buf_addr;
	uint64_t	local_qp_db_addr;

	/* QP shape parameters from the harness. */
	uint64_t	qp_type;	/* IB_UVERBS_QPT_RC / UC / UD */
	uint64_t	qp_state;	/* IB_QPS_RESET / INIT / RTR / RTS */

	/* Target ufile handles. */
	uint32_t	pd_target_handle;
	uint32_t	cq_target_handle;
	uint32_t	qp_target_handle;
};

/*
 * Allocate destination-side mappings backing both the CQ and QP
 * umems. CRIU's mm-replay normally provides these via MAP_FIXED;
 * the probe simulates that with MAP_FIXED_NOREPLACE for the
 * doorbell pages (the placeholder lookup is keyed by virt &
 * PAGE_MASK so the destination VA must match the source's). The
 * ring-buffer mappings can land anywhere because their placeholder
 * keys are FW-resource-id-based (KIND_CQ + cqn / KIND_QP + qpn);
 * we use plain anonymous mmap there.
 *
 * Returns 0 on success, -errno on mmap failure (typically
 * -EADDRINUSE if the source's randomized libmlx5 doorbell VA
 * happens to collide with this probe's own address space; user
 * can re-run since the ASLR'd VA changes per run).
 *
 * The mappings leak at probe exit, which is fine for a test
 * binary.
 */
static int qp_args_mmap_local(struct qp_args *a)
{
	long page_size = sysconf(_SC_PAGESIZE);
	size_t cqe_buf_bytes, cqe_buf_aligned;
	size_t qp_buf_bytes, qp_buf_aligned;
	uint64_t cq_db_page, qp_db_page;
	void *cqe_p, *cq_db_p, *qp_p, *qp_db_p;

	if (page_size <= 0)
		page_size = 4096;

	/* CQE ring -- anonymous, cqn-keyed lookup. */
	cqe_buf_bytes = (size_t)a->src_cqe * a->src_cqe_size;
	cqe_buf_aligned = (cqe_buf_bytes + page_size - 1) &
			  ~(size_t)(page_size - 1);
	if (cqe_buf_aligned == 0)
		cqe_buf_aligned = page_size;
	cqe_p = mmap(NULL, cqe_buf_aligned,
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (cqe_p == MAP_FAILED) {
		fprintf(stderr,
			"qp_restore_mlx5: mmap(cqe_buf %zu): %s\n",
			cqe_buf_aligned, strerror(errno));
		return -errno;
	}
	a->local_cq_buf_addr = (uint64_t)(uintptr_t)cqe_p;

	/* CQ doorbell -- MAP_FIXED_NOREPLACE at source's page-aligned VA. */
	cq_db_page = a->src_cq_db_addr & ~(uint64_t)(page_size - 1);
	cq_db_p = mmap((void *)(uintptr_t)cq_db_page, page_size,
		       PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
		       -1, 0);
	if (cq_db_p == MAP_FAILED ||
	    (uint64_t)(uintptr_t)cq_db_p != cq_db_page) {
		fprintf(stderr,
			"qp_restore_mlx5: MAP_FIXED_NOREPLACE(0x%llx, %ld) for src CQ DBR\n"
			"                 page failed: %s. The source's libmlx5 doorbell mmap\n"
			"                 VA collides with this probe's own address space.\n"
			"                 Re-run -- the source's ASLR-randomized VA will\n"
			"                 likely land elsewhere.\n",
			(unsigned long long)cq_db_page, page_size,
			strerror(errno));
		munmap(cqe_p, cqe_buf_aligned);
		return -EADDRINUSE;
	}
	a->local_cq_db_addr = a->src_cq_db_addr;

	/*
	 * QP WQ ring -- anonymous, qpn-keyed lookup. buf_size mirrors
	 * mlx5_ib_restore_qp's own arithmetic in main.c so the umem
	 * length passed to mlx5_vfmig_bind_user_qp matches the
	 * placeholder's recorded length verbatim.
	 *
	 * MLX5_SEND_WQE_BB == 64 -- the BlueField-3+ basic block size,
	 * stable across all mlx5 generations the probe runs on.
	 */
	qp_buf_bytes = ((size_t)a->rq_wqe_count << a->rq_wqe_shift) +
		       ((size_t)a->sq_wqe_count * 64);
	qp_buf_aligned = (qp_buf_bytes + page_size - 1) &
			 ~(size_t)(page_size - 1);
	if (qp_buf_aligned == 0)
		qp_buf_aligned = page_size;
	qp_p = mmap(NULL, qp_buf_aligned,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (qp_p == MAP_FAILED) {
		fprintf(stderr,
			"qp_restore_mlx5: mmap(qp_buf %zu): %s\n",
			qp_buf_aligned, strerror(errno));
		munmap(cq_db_p, page_size);
		munmap(cqe_p, cqe_buf_aligned);
		return -errno;
	}
	a->local_qp_buf_addr = (uint64_t)(uintptr_t)qp_p;

	/*
	 * QP doorbell -- MAP_FIXED_NOREPLACE at source's page-aligned
	 * VA. Note: libmlx5 packs CQ and QP doorbell records into the
	 * SAME shared doorbell page on the source (one PAGE_SIZE
	 * mmap, multiple 8-byte slots indexed via mlx5_alloc_dbrec).
	 * If src_cq_db_addr and src_qp_db_addr land on the same page
	 * the second mmap below will EEXIST -- treat that as benign
	 * because the existing mapping ALREADY satisfies both
	 * lookups (the kernel's
	 * mlx5_ib_db_map_user_restore -> vfmig_iova_bind_user_dbr
	 * dedup logic refcounts a single placeholder for both
	 * tenants).
	 *
	 * On a same-page hit we simply reuse src_qp_db_addr as
	 * local_qp_db_addr (preserving the in-page offset for the
	 * RESTORE_QP db_addr arg) and skip the second mmap. The
	 * existing CQ DBR mapping covers the QP DBR page too.
	 */
	qp_db_page = a->src_qp_db_addr & ~(uint64_t)(page_size - 1);
	if (qp_db_page == cq_db_page) {
		a->local_qp_db_addr = a->src_qp_db_addr;
		printf("qp_restore_mlx5: QP DBR page (0x%llx) shares the CQ DBR page;\n"
		       "                 reusing the existing mmap. In-page offsets:\n"
		       "                 cq_db=0x%llx qp_db=0x%llx\n",
		       (unsigned long long)qp_db_page,
		       (unsigned long long)(a->src_cq_db_addr & (uint64_t)(page_size - 1)),
		       (unsigned long long)(a->src_qp_db_addr & (uint64_t)(page_size - 1)));
	} else {
		qp_db_p = mmap((void *)(uintptr_t)qp_db_page, page_size,
			       PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS |
			       MAP_FIXED_NOREPLACE, -1, 0);
		if (qp_db_p == MAP_FAILED ||
		    (uint64_t)(uintptr_t)qp_db_p != qp_db_page) {
			fprintf(stderr,
				"qp_restore_mlx5: MAP_FIXED_NOREPLACE(0x%llx, %ld) for src QP DBR\n"
				"                 page failed: %s. ASLR collision with this\n"
				"                 probe's own address space; re-run.\n",
				(unsigned long long)qp_db_page, page_size,
				strerror(errno));
			munmap(qp_p, qp_buf_aligned);
			munmap(cq_db_p, page_size);
			munmap(cqe_p, cqe_buf_aligned);
			return -EADDRINUSE;
		}
		a->local_qp_db_addr = a->src_qp_db_addr;
	}

	printf("qp_restore_mlx5: local_cq_buf=0x%llx (anon, %zu bytes)\n"
	       "                 local_cq_db =0x%llx (FIXED page 0x%llx)\n"
	       "                 local_qp_buf=0x%llx (anon, %zu bytes)\n"
	       "                 local_qp_db =0x%llx (FIXED page 0x%llx)\n",
	       (unsigned long long)a->local_cq_buf_addr, cqe_buf_aligned,
	       (unsigned long long)a->local_cq_db_addr,
	       (unsigned long long)cq_db_page,
	       (unsigned long long)a->local_qp_buf_addr, qp_buf_aligned,
	       (unsigned long long)a->local_qp_db_addr,
	       (unsigned long long)qp_db_page);
	return 0;
}

/*
 * Build a "good" mlx5_ib_restore_qp_req from @a -- the exact
 * payload the happy-path subtest sends. The negative subtests
 * stamp specific fields with bad values on top of this baseline.
 */
static void make_good_uhw(const struct qp_args *a,
			  struct mlx5_ib_restore_qp_req *u)
{
	memset(u, 0, sizeof(*u));
	u->buf_addr	= a->local_qp_buf_addr;
	u->db_addr	= a->local_qp_db_addr;
	u->qpn		= a->src_qpn;
	u->sq_wqe_count	= a->sq_wqe_count;
	u->rq_wqe_count	= a->rq_wqe_count;
	u->rq_wqe_shift	= a->rq_wqe_shift;
	/*
	 * uidx == 0xFFFFFF is libmlx5's MLX5_IB_DEFAULT_UIDX -- the
	 * default user_index used by stock ibv_create_qp callers
	 * (the value the source's ucmd carried into _create_user_qp,
	 * mirrored into the qpc.user_index field that LOAD_VHCA_STATE
	 * preserved on the destination). RESTORE_QP's only validation
	 * is uidx & ~0xffffff == 0; passing 0xFFFFFF is in-range and
	 * matches the source's actual qpc state.
	 */
	u->uidx		= 0xFFFFFFu;
	/* bfreg_index, ece_options, flags: 0 (probe doesn't exercise
	 * vendor flags / DCT / scatter-CQE / etc.). */
}

/*
 * Build a "good" ib_uverbs_qp_cap from @a. The cap echoes the
 * source's libmlx5-negotiated WQ depths -- the kernel handler
 * does not currently validate cap against the WQ-shape UHW fields
 * (S6b B3 stub: cap is informational), but the dispatcher copies
 * it into kcap and passes it through, so we send a coherent value
 * for forward-compat.
 */
static void make_good_cap(const struct qp_args *a,
			  struct ib_uverbs_qp_cap *c)
{
	memset(c, 0, sizeof(*c));
	c->max_send_wr		= a->sq_wqe_count;
	c->max_recv_wr		= a->rq_wqe_count;
	c->max_send_sge		= 1;
	c->max_recv_sge		= 1;
	c->max_inline_data	= 0;
}

/*
 * Subtest 1: gate-negative.
 *
 * Verify the dispatcher's restore_check_ucontext gate (the
 * mlx5_ib_ucontext_is_restore_mode predicate) rejects RESTORE_*
 * verbs on a ucontext that was opened WITHOUT
 * MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE.
 *
 * We exercise the gate via RESTORE_PD rather than RESTORE_QP for a
 * specific reason: RESTORE_QP declares PD_HANDLE / SEND_CQ_HANDLE /
 * RECV_CQ_HANDLE as UVERBS_ATTR_TYPE_IDR + UA_MANDATORY, which
 * means the uverbs core's IDR-resolve layer runs BEFORE the
 * dispatcher and short-circuits with -ENOENT against bogus
 * (handle=0) refs -- masking the gate's -EPERM. Adding the
 * machinery to mint real PD/CQ handles on the throwaway ucontext
 * (do_alloc_pd plus a full do_create_cq UAR/comp_vector dance)
 * would be a meaningful chunk of code with no extra coverage,
 * because the restore_check_ucontext helper is *shared* across the
 * RESTORE_PD / RESTORE_MR / RESTORE_CQ / RESTORE_QP dispatchers.
 * RESTORE_PD has zero core IDR attrs, so the gate fires
 * immediately and -EPERM bubbles back up cleanly. A regression in
 * mlx5_ib_ucontext_is_restore_mode (or in the per-dispatcher
 * `ret = restore_check_ucontext(...)` call) shows up here.
 */
static int subtest_gate_negative(const char *cdev_path,
				 const struct qp_args *a)
{
	struct ib_uverbs_get_context_resp resp_core = {};
	struct mlx5_ib_alloc_ucontext_resp resp_drv = {};
	struct mlx5_ib_restore_pd_req uhw = {
		.pdn = a->src_pdn,
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
		fprintf(stderr, "  FAIL GET_CONTEXT(flags=0): %s\n",
			strerror(-ret));
		close(fd);
		return 1;
	}

	ret = do_restore_pd(fd, a->pd_target_handle, &uhw);
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

/*
 * Setup A: RESTORE_PD on the restore-mode ucontext. We fail-fast
 * here because every downstream subtest depends on a valid PD
 * ufile handle.
 */
static int setup_restore_pd(int fd, const struct qp_args *a)
{
	struct mlx5_ib_restore_pd_req uhw = {
		.pdn = a->src_pdn,
	};
	int ret;

	printf("[setup A] RESTORE_PD(target=0x%x, pdn=%u)\n",
	       a->pd_target_handle, a->src_pdn);

	ret = do_restore_pd(fd, a->pd_target_handle, &uhw);
	if (ret) {
		fprintf(stderr,
			"  FAIL RESTORE_PD(pdn=%u): %s%s\n",
			a->src_pdn, strerror(-ret),
			ret == -EOPNOTSUPP
			? "  (mlx5_ib_restore_pd not registered?)"
			: ret == -EPERM
			? "  (mlx5_ib_ucontext_is_restore_mode false?)"
			: "");
		return 1;
	}
	printf("  PASS RESTORE_PD(target=0x%x, pdn=%u)\n",
	       a->pd_target_handle, a->src_pdn);
	return 0;
}

/*
 * Setup B: RESTORE_CQ on the restore-mode ucontext. Both send_cq
 * and recv_cq of the QP point at this same handle (self-loopback
 * pattern -- the source QP was created with .send_cq == .recv_cq
 * inside fw_id_continuity_probe).
 */
static int setup_restore_cq(int fd, const struct qp_args *a)
{
	struct mlx5_ib_restore_cq_req uhw = {
		.cqn      = a->src_cqn,
		.cqe_size = a->src_cqe_size,
		.buf_addr = a->local_cq_buf_addr,
		.db_addr  = a->local_cq_db_addr,
	};
	uint32_t resp_cqe = 0;
	int ret;

	printf("[setup B] RESTORE_CQ(target=0x%x, cqn=%u, cqe=%u)\n",
	       a->cq_target_handle, a->src_cqn, a->src_cqe);

	ret = do_restore_cq(fd, a->cq_target_handle, a->src_cqe,
			    USER_HANDLE_TAG_CQ, /*comp_vector=*/0,
			    &uhw, &resp_cqe);
	if (ret) {
		fprintf(stderr,
			"  FAIL RESTORE_CQ(cqn=%u): %s\n",
			a->src_cqn, strerror(-ret));
		return 1;
	}
	if (resp_cqe != a->src_cqe) {
		fprintf(stderr,
			"  FAIL RESTORE_CQ RESP_CQE=%u (expected %u)\n",
			resp_cqe, a->src_cqe);
		return 1;
	}
	printf("  PASS RESTORE_CQ(target=0x%x, cqn=%u) RESP_CQE=%u\n",
	       a->cq_target_handle, a->src_cqn, resp_cqe);
	return 0;
}

/*
 * Helper: build a RESTORE_QP call with one knob varied from the
 * baseline UHW, expecting the given errno. Used by subtests 3-9.
 */
static int expect_restore_qp_errno(int fd, const struct qp_args *a,
				   const struct mlx5_ib_restore_qp_req *uhw,
				   uint64_t qp_type, uint64_t qp_state,
				   int want_errno, const char *desc)
{
	struct ib_uverbs_qp_cap cap;
	uint32_t resp_qpn = 0;
	int ret;

	make_good_cap(a, &cap);
	ret = do_restore_qp(fd, a->qp_target_handle,
			    a->pd_target_handle,
			    a->cq_target_handle, a->cq_target_handle,
			    qp_type, qp_state, USER_HANDLE_TAG_QP,
			    &cap, UINT32_MAX, uhw, &resp_qpn);
	if (ret == want_errno) {
		printf("  PASS RESTORE_QP(%s) -> %s\n", desc, strerror(-ret));
		return 0;
	}
	fprintf(stderr,
		"  FAIL RESTORE_QP(%s) -> %s (expected %s)\n",
		desc, ret ? strerror(-ret) : "0 (success)",
		strerror(-want_errno));
	return 1;
}

static int subtest_uapi_reject_zero_qpn(int fd, const struct qp_args *a)
{
	struct mlx5_ib_restore_qp_req uhw;
	make_good_uhw(a, &uhw);
	uhw.qpn = 0;
	printf("[3] uapi reject: qpn=0 must -EINVAL\n");
	return expect_restore_qp_errno(fd, a, &uhw, a->qp_type, a->qp_state,
				       -EINVAL, "qpn=0");
}

static int subtest_uapi_reject_qpn_high_bits(int fd, const struct qp_args *a)
{
	struct mlx5_ib_restore_qp_req uhw;
	make_good_uhw(a, &uhw);
	uhw.qpn = a->src_qpn | 0x01000000u;	/* set bit 24 */
	printf("[4] uapi reject: qpn out-of-24-bit-range must -EINVAL\n");
	return expect_restore_qp_errno(fd, a, &uhw, a->qp_type, a->qp_state,
				       -EINVAL, "qpn|=0x01000000");
}

static int subtest_uapi_reject_reserved(int fd, const struct qp_args *a)
{
	struct mlx5_ib_restore_qp_req uhw;
	make_good_uhw(a, &uhw);
	uhw.reserved = 0xDEAD;
	printf("[5] uapi reject: reserved!=0 must -EINVAL\n");
	return expect_restore_qp_errno(fd, a, &uhw, a->qp_type, a->qp_state,
				       -EINVAL, "reserved=0xDEAD");
}

static int subtest_uapi_reject_uidx_high_bits(int fd, const struct qp_args *a)
{
	struct mlx5_ib_restore_qp_req uhw;
	make_good_uhw(a, &uhw);
	uhw.uidx = 0x01FFFFFFu;	/* bit 24 set */
	printf("[6] uapi reject: uidx out-of-24-bit-range must -EINVAL\n");
	return expect_restore_qp_errno(fd, a, &uhw, a->qp_type, a->qp_state,
				       -EINVAL, "uidx|=0x01000000");
}

static int subtest_uapi_reject_rq_wqe_shift(int fd, const struct qp_args *a)
{
	struct mlx5_ib_restore_qp_req uhw;

	/*
	 * Skip if the source's rq_wqe_count is 0 -- the kernel only
	 * validates rq_wqe_shift when has_rq is true (rq_wqe_count >
	 * 0). A zero-RQ source has nothing to test here.
	 */
	if (a->rq_wqe_count == 0) {
		printf("[7] uapi reject: rq_wqe_shift -- SKIP (zero-RQ source)\n");
		return 0;
	}

	make_good_uhw(a, &uhw);
	uhw.rq_wqe_shift = 3;	/* below the kernel's [4,16] band */
	printf("[7] uapi reject: rq_wqe_shift=3 must -EINVAL\n");
	return expect_restore_qp_errno(fd, a, &uhw, a->qp_type, a->qp_state,
				       -EINVAL, "rq_wqe_shift=3");
}

static int subtest_dispatcher_reject_bad_type(int fd, const struct qp_args *a)
{
	struct mlx5_ib_restore_qp_req uhw;
	make_good_uhw(a, &uhw);
	printf("[8] dispatcher reject: qp_type=XRC_INI must -EOPNOTSUPP\n");
	/*
	 * v0 dispatcher accepted set is {RC, UC, UD}. XRC_INI is the
	 * canonical out-of-set value -- a future S6c that adds XRC
	 * support will need to update this subtest.
	 */
	return expect_restore_qp_errno(fd, a, &uhw,
				       IB_UVERBS_QPT_XRC_INI, a->qp_state,
				       -EOPNOTSUPP, "qp_type=XRC_INI");
}

static int subtest_dispatcher_reject_bad_state(int fd, const struct qp_args *a)
{
	struct mlx5_ib_restore_qp_req uhw;
	make_good_uhw(a, &uhw);
	printf("[9] dispatcher reject: qp_state=SQE must -EOPNOTSUPP\n");
	/*
	 * v0 dispatcher accepted set is {RESET, INIT, RTR, RTS}.
	 * SQE/SQD/ERR are out of scope per design S6b -- destroyed-
	 * mid-error QPs serialize through a different rung.
	 */
	return expect_restore_qp_errno(fd, a, &uhw, a->qp_type, IB_QPS_SQE,
				       -EOPNOTSUPP, "qp_state=SQE");
}

static int subtest_happy_path(int fd, const struct qp_args *a)
{
	struct mlx5_ib_restore_qp_req uhw;
	struct ib_uverbs_qp_cap cap;
	uint32_t resp_qpn = 0;
	uint32_t list[64] = {};
	uint32_t total = 0;
	int ret;
	int fails = 0;

	make_good_uhw(a, &uhw);
	make_good_cap(a, &cap);

	printf("[10] happy path: RESTORE_QP(target=0x%x, qpn=%u, type=%llu, state=%llu)\n",
	       a->qp_target_handle, a->src_qpn,
	       (unsigned long long)a->qp_type,
	       (unsigned long long)a->qp_state);

	ret = do_restore_qp(fd, a->qp_target_handle,
			    a->pd_target_handle,
			    a->cq_target_handle, a->cq_target_handle,
			    a->qp_type, a->qp_state, USER_HANDLE_TAG_QP,
			    &cap, UINT32_MAX, &uhw, &resp_qpn);
	if (ret) {
		fprintf(stderr,
			"  FAIL RESTORE_QP(target=0x%x): %s%s\n",
			a->qp_target_handle, strerror(-ret),
			ret == -EOPNOTSUPP
			? "  (mlx5_ib_restore_qp not registered in mlx5_ib_dev_ops?)"
			: ret == -EPERM
			? "  (mlx5_ib_ucontext_is_restore_mode false?)"
			: "");
		return 1;
	}

	if (resp_qpn != a->src_qpn) {
		fprintf(stderr,
			"  FAIL RESP_QPN=%u (expected %u byte-identical to req.qpn;\n"
			"       mlx5 stamps ibqp->qp_num verbatim from UHW)\n",
			resp_qpn, a->src_qpn);
		fails++;
	} else {
		printf("  PASS RESP_QPN=%u byte-identical to req.qpn\n",
		       resp_qpn);
	}

	ret = do_info_handles(fd, UVERBS_OBJECT_QP, list, 64, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(QP): %s\n",
			strerror(-ret));
		return 1;
	}
	if (!handle_present(list, total, a->qp_target_handle)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(QP): handle 0x%x not in list (total=%u)\n",
			a->qp_target_handle, total);
		fails++;
	} else {
		printf("  PASS INFO_HANDLES(QP) returned 0x%x among %u entries\n",
		       a->qp_target_handle, total);
	}
	return fails;
}

static int subtest_collision(int fd, const struct qp_args *a)
{
	struct mlx5_ib_restore_qp_req uhw;
	struct ib_uverbs_qp_cap cap;
	uint32_t resp_qpn = 0;
	int ret;

	make_good_uhw(a, &uhw);
	make_good_cap(a, &cap);

	printf("[11] collision: second RESTORE_QP(target=0x%x) must -EBUSY\n",
	       a->qp_target_handle);

	ret = do_restore_qp(fd, a->qp_target_handle,
			    a->pd_target_handle,
			    a->cq_target_handle, a->cq_target_handle,
			    a->qp_type, a->qp_state, USER_HANDLE_TAG_QP,
			    &cap, UINT32_MAX, &uhw, &resp_qpn);
	if (ret == -EBUSY) {
		printf("  PASS second RESTORE_QP(target=0x%x) -> -EBUSY\n",
		       a->qp_target_handle);
		return 0;
	}
	fprintf(stderr,
		"  FAIL second RESTORE_QP(target=0x%x) -> %s (expected -EBUSY)\n",
		a->qp_target_handle, ret ? strerror(-ret) : "0 (success)");
	return 1;
}

/*
 * Subtest 13: v0 dealloc semantics on a still-loaded VHCA.
 *
 * ASYMMETRIC with CQ's subtest 10 / PD's subtest 7; SYMMETRIC with
 * MR's subtest 8. QP is a *leaf* in the FW resource graph: nothing
 * has the qpn as a tracked dependent (peer remote_qpn does not
 * install a back-reference). FW DESTROY_QP on the orphan adopted
 * qpn is therefore ACCEPTED -- the qpn's own dependents (PD, CQs)
 * are PARENTS of the QP and remain alive across the destroy.
 *
 * v0 PASS criterion: DESTROY_QP must succeed AND INFO_HANDLES(QP)
 * must NO LONGER see the handle (uverbs_destroy_uobject removes
 * the idr handle on a successful destroy_hw). Recorded in
 * design/uobject_restore.md S6b + S10.8.
 */
static int subtest_v0_dealloc(int fd, const struct qp_args *a)
{
	uint32_t list[64] = {};
	uint32_t total = 0;
	uint32_t events_reported = 0;
	int ret;

	printf("[13] v0 dealloc semantics: DESTROY_QP(0x%x) on adopted qpn=%u\n"
	       "     must succeed (QP is a leaf -- no FW-level dependents)\n",
	       a->qp_target_handle, a->src_qpn);

	ret = do_destroy_qp(fd, a->qp_target_handle, &events_reported);
	if (ret) {
		fprintf(stderr,
			"  FAIL DESTROY_QP(0x%x) -> %s (expected success;\n"
			"       QP is a leaf in the FW resource graph)\n",
			a->qp_target_handle, strerror(-ret));
		return 1;
	}
	printf("  PASS DESTROY_QP(0x%x) succeeded (events_reported=%u)\n",
	       a->qp_target_handle, events_reported);

	ret = do_info_handles(fd, UVERBS_OBJECT_QP, list, 64, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(QP): %s\n",
			strerror(-ret));
		return 1;
	}
	if (handle_present(list, total, a->qp_target_handle)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(QP) still reports 0x%x after a\n"
			"       SUCCESSFUL DESTROY_QP (uobj should be gone)\n",
			a->qp_target_handle);
		return 1;
	}
	printf("  PASS INFO_HANDLES(QP) no longer reports 0x%x (uobj reaped)\n",
	       a->qp_target_handle);
	return 0;
}

/* ---------------------------------------------------------------------- */
/*                                main                                    */
/* ---------------------------------------------------------------------- */

static int parse_qp_type(const char *s, uint64_t *out)
{
	if (!strcasecmp(s, "RC")) { *out = IB_UVERBS_QPT_RC; return 0; }
	if (!strcasecmp(s, "UC")) { *out = IB_UVERBS_QPT_UC; return 0; }
	if (!strcasecmp(s, "UD")) { *out = IB_UVERBS_QPT_UD; return 0; }
	return -1;
}

static int parse_qp_state(const char *s, uint64_t *out)
{
	if (!strcasecmp(s, "RESET")) { *out = IB_QPS_RESET; return 0; }
	if (!strcasecmp(s, "INIT"))  { *out = IB_QPS_INIT;  return 0; }
	if (!strcasecmp(s, "RTR"))   { *out = IB_QPS_RTR;   return 0; }
	if (!strcasecmp(s, "RTS"))   { *out = IB_QPS_RTS;   return 0; }
	return -1;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <ibdev> <src_pdn>\n"
		"           <src_cqn> <src_cqe> <src_cqe_size>\n"
		"               <src_cq_buf_addr> <src_cq_db_addr>\n"
		"           <src_qpn> <sq_wqe_count> <rq_wqe_count> <rq_wqe_shift>\n"
		"               <src_qp_buf_addr> <src_qp_db_addr>\n"
		"           <qp_type:RC|UC|UD> <qp_state:RESET|INIT|RTR|RTS>\n"
		"           [<qp_target_handle>]\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *ibdev;
	struct qp_args a = {
		.pd_target_handle = DEFAULT_PD_TARGET_HANDLE,
		.cq_target_handle = DEFAULT_CQ_TARGET_HANDLE,
		.qp_target_handle = DEFAULT_QP_TARGET_HANDLE,
	};
	char cdev_path[128];
	struct ib_uverbs_get_context_resp resp_core = {};
	struct mlx5_ib_alloc_ucontext_resp resp_drv = {};
	int fd_restore = -1, ret;
	int fails = 0;
	unsigned long u;

	/* Force line buffering on stdout so harness/log readers see
	 * subtest progress in real time even if we get SIGKILL'd
	 * mid-flight. Without this, glibc full-buffers stdout when
	 * connected to a FIFO and we lose all subtest progress info
	 * if we hang in a kernel ioctl. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc < 16 || argc > 17) {
		usage(argv[0]);
		return 2;
	}
	ibdev = argv[1];

	/* src_pdn */
	u = strtoul(argv[2], NULL, 0);
	if (u == 0 || u > 0xffffffu) {
		fprintf(stderr,
			"qp_restore_mlx5: src_pdn=%lu out of range (1..0xffffff)\n",
			u);
		return 2;
	}
	a.src_pdn = (uint32_t)u;

	/* src_cqn / cqe / cqe_size / cq_buf_addr / cq_db_addr */
	u = strtoul(argv[3], NULL, 0);
	if (u == 0 || u > 0xffffffu) {
		fprintf(stderr,
			"qp_restore_mlx5: src_cqn=%lu out of range (1..0xffffff)\n",
			u);
		return 2;
	}
	a.src_cqn = (uint32_t)u;
	u = strtoul(argv[4], NULL, 0);
	if (u == 0 || u > 0xffffu) {
		fprintf(stderr,
			"qp_restore_mlx5: src_cqe=%lu out of range (1..0xffff)\n",
			u);
		return 2;
	}
	a.src_cqe = (uint32_t)u;
	u = strtoul(argv[5], NULL, 0);
	if (u != 64 && u != 128) {
		fprintf(stderr,
			"qp_restore_mlx5: src_cqe_size=%lu must be 64 or 128\n",
			u);
		return 2;
	}
	a.src_cqe_size = (uint32_t)u;
	a.src_cq_buf_addr = strtoull(argv[6], NULL, 0);
	a.src_cq_db_addr  = strtoull(argv[7], NULL, 0);

	/* src_qpn / sq_wqe_count / rq_wqe_count / rq_wqe_shift */
	u = strtoul(argv[8], NULL, 0);
	if (u == 0 || u > 0xffffffu) {
		fprintf(stderr,
			"qp_restore_mlx5: src_qpn=%lu out of range (1..0xffffff)\n",
			u);
		return 2;
	}
	a.src_qpn = (uint32_t)u;
	u = strtoul(argv[9], NULL, 0);
	if (u > 0xffffu) {
		fprintf(stderr,
			"qp_restore_mlx5: sq_wqe_count=%lu out of range\n", u);
		return 2;
	}
	a.sq_wqe_count = (uint32_t)u;
	u = strtoul(argv[10], NULL, 0);
	if (u > 0xffffu) {
		fprintf(stderr,
			"qp_restore_mlx5: rq_wqe_count=%lu out of range\n", u);
		return 2;
	}
	a.rq_wqe_count = (uint32_t)u;
	u = strtoul(argv[11], NULL, 0);
	if (u > 16u) {
		fprintf(stderr,
			"qp_restore_mlx5: rq_wqe_shift=%lu out of range (0..16)\n",
			u);
		return 2;
	}
	a.rq_wqe_shift = (uint32_t)u;
	a.src_qp_buf_addr = strtoull(argv[12], NULL, 0);
	a.src_qp_db_addr  = strtoull(argv[13], NULL, 0);

	/* qp_type / qp_state */
	if (parse_qp_type(argv[14], &a.qp_type) != 0) {
		fprintf(stderr,
			"qp_restore_mlx5: bad qp_type '%s' (want RC|UC|UD)\n",
			argv[14]);
		return 2;
	}
	if (parse_qp_state(argv[15], &a.qp_state) != 0) {
		fprintf(stderr,
			"qp_restore_mlx5: bad qp_state '%s' (want RESET|INIT|RTR|RTS)\n",
			argv[15]);
		return 2;
	}

	if (argc >= 17) {
		u = strtoul(argv[16], NULL, 0);
		if (u > 0xffffffffu) {
			fprintf(stderr,
				"qp_restore_mlx5: qp_target_handle out of range\n");
			return 2;
		}
		a.qp_target_handle = (uint32_t)u;
	}

	if (resolve_cdev_path(ibdev, cdev_path, sizeof(cdev_path)) != 0)
		return 2;
	printf("qp_restore_mlx5: ibdev=%s cdev=%s\n"
	       "                 src_pdn=%u src_cqn=%u src_cqe=%u src_cqe_size=%u\n"
	       "                 src_cq_buf=0x%llx src_cq_db=0x%llx\n"
	       "                 src_qpn=%u sq_wqe_count=%u rq_wqe_count=%u rq_wqe_shift=%u\n"
	       "                 src_qp_buf=0x%llx src_qp_db=0x%llx\n"
	       "                 qp_type=%llu qp_state=%llu\n"
	       "                 pd_target=0x%x cq_target=0x%x qp_target=0x%x\n",
	       ibdev, cdev_path, a.src_pdn, a.src_cqn, a.src_cqe, a.src_cqe_size,
	       (unsigned long long)a.src_cq_buf_addr,
	       (unsigned long long)a.src_cq_db_addr,
	       a.src_qpn, a.sq_wqe_count, a.rq_wqe_count, a.rq_wqe_shift,
	       (unsigned long long)a.src_qp_buf_addr,
	       (unsigned long long)a.src_qp_db_addr,
	       (unsigned long long)a.qp_type, (unsigned long long)a.qp_state,
	       a.pd_target_handle, a.cq_target_handle, a.qp_target_handle);

	if (qp_args_mmap_local(&a) != 0)
		return 2;

	/* Subtest 1 uses its own short-lived non-restore-mode ucontext. */
	fails += subtest_gate_negative(cdev_path, &a);

	/* Subtests 2-13 share one restore-mode ucontext. */
	fd_restore = open(cdev_path, O_RDWR | O_CLOEXEC);
	if (fd_restore < 0) {
		fprintf(stderr,
			"qp_restore_mlx5: open(%s) for restore-mode ucontext: %s\n",
			cdev_path, strerror(errno));
		return 2;
	}
	ret = do_get_context(fd_restore, MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE,
			     &resp_core, &resp_drv);
	if (ret) {
		fprintf(stderr,
			"qp_restore_mlx5: GET_CONTEXT(VFMIG_RESTORE): %s\n",
			strerror(-ret));
		close(fd_restore);
		return 2;
	}

	/* setup PD / CQ -- needed before any RESTORE_QP can resolve handles */
	if (setup_restore_pd(fd_restore, &a) != 0) {
		close(fd_restore);
		return 1;
	}
	if (setup_restore_cq(fd_restore, &a) != 0) {
		close(fd_restore);
		return 1;
	}

	fails += subtest_uapi_reject_zero_qpn(fd_restore, &a);
	fails += subtest_uapi_reject_qpn_high_bits(fd_restore, &a);
	fails += subtest_uapi_reject_reserved(fd_restore, &a);
	fails += subtest_uapi_reject_uidx_high_bits(fd_restore, &a);
	fails += subtest_uapi_reject_rq_wqe_shift(fd_restore, &a);
	fails += subtest_dispatcher_reject_bad_type(fd_restore, &a);
	fails += subtest_dispatcher_reject_bad_state(fd_restore, &a);
	fails += subtest_happy_path(fd_restore, &a);
	fails += subtest_collision(fd_restore, &a);

	if (fails) {
		fprintf(stderr,
			"\nqp_restore_probe_mlx5_vfmig: %d subtest failure(s) "
			"before READY; aborting\n", fails);
		close(fd_restore);
		return 1;
	}

	/*
	 * READY checkpoint. The adopted QP lives on fd_restore at
	 * the requested target handle. The harness can now run any
	 * out-of-process FW-side verifier -- typically
	 * `mlx5_vfmig <pf> query_qp <vf> <qpn>` -- while the object
	 * is alive in destination FW. On "quit\n" we drop to
	 * subtest 13.
	 */
	printf("ibdev=%s\n", ibdev);
	printf("adopted_qpn=%u\n", a.src_qpn);
	printf("qp_target_handle=0x%x\n", a.qp_target_handle);
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
			"\nqp_restore_probe_mlx5_vfmig: FAIL (%d subtest failure(s))\n",
			fails);
		return 1;
	}
	printf("\nqp_restore_probe_mlx5_vfmig: PASS\n");
	return 0;
}
