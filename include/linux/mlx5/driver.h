/*
 * Copyright (c) 2013-2015, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef MLX5_DRIVER_H
#define MLX5_DRIVER_H

#include <linux/kernel.h>
#include <linux/completion.h>
#include <linux/pci.h>
#include <linux/pci-tph.h>
#include <linux/irq.h>
#include <linux/spinlock_types.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/xarray.h>
#include <linux/workqueue.h>
#include <linux/mempool.h>
#include <linux/interrupt.h>
#include <linux/notifier.h>
#include <linux/refcount.h>
#include <linux/auxiliary_bus.h>
#include <linux/mutex.h>
#include <linux/uuid.h>

#include <linux/mlx5/device.h>
#include <linux/mlx5/doorbell.h>
#include <linux/mlx5/eq.h>
#include <linux/timecounter.h>
#include <net/devlink.h>

#define MLX5_ADEV_NAME "mlx5_core"

#define MLX5_IRQ_EQ_CTRL (U8_MAX)

enum {
	MLX5_BOARD_ID_LEN = 64,
};

enum {
	MLX5_CMD_WQ_MAX_NAME	= 32,
};

enum {
	CMD_OWNER_SW		= 0x0,
	CMD_OWNER_HW		= 0x1,
	CMD_STATUS_SUCCESS	= 0,
};

enum mlx5_sqp_t {
	MLX5_SQP_SMI		= 0,
	MLX5_SQP_GSI		= 1,
	MLX5_SQP_IEEE_1588	= 2,
	MLX5_SQP_SNIFFER	= 3,
	MLX5_SQP_SYNC_UMR	= 4,
};

enum {
	MLX5_MAX_PORTS	= 8,
};

enum {
	MLX5_ATOMIC_MODE_OFFSET = 16,
	MLX5_ATOMIC_MODE_IB_COMP = 1,
	MLX5_ATOMIC_MODE_CX = 2,
	MLX5_ATOMIC_MODE_8B = 3,
	MLX5_ATOMIC_MODE_16B = 4,
	MLX5_ATOMIC_MODE_32B = 5,
	MLX5_ATOMIC_MODE_64B = 6,
	MLX5_ATOMIC_MODE_128B = 7,
	MLX5_ATOMIC_MODE_256B = 8,
};

enum {
	MLX5_REG_SBPR            = 0xb001,
	MLX5_REG_SBCM            = 0xb002,
	MLX5_REG_QPTS            = 0x4002,
	MLX5_REG_QETCR		 = 0x4005,
	MLX5_REG_QTCT		 = 0x400a,
	MLX5_REG_QPDPM           = 0x4013,
	MLX5_REG_QCAM            = 0x4019,
	MLX5_REG_DCBX_PARAM      = 0x4020,
	MLX5_REG_DCBX_APP        = 0x4021,
	MLX5_REG_FPGA_CAP	 = 0x4022,
	MLX5_REG_FPGA_CTRL	 = 0x4023,
	MLX5_REG_FPGA_ACCESS_REG = 0x4024,
	MLX5_REG_CORE_DUMP	 = 0x402e,
	MLX5_REG_PCAP		 = 0x5001,
	MLX5_REG_PMTU		 = 0x5003,
	MLX5_REG_PTYS		 = 0x5004,
	MLX5_REG_PAOS		 = 0x5006,
	MLX5_REG_PFCC            = 0x5007,
	MLX5_REG_PPCNT		 = 0x5008,
	MLX5_REG_PPTB            = 0x500b,
	MLX5_REG_PBMC            = 0x500c,
	MLX5_REG_PMAOS		 = 0x5012,
	MLX5_REG_PUDE		 = 0x5009,
	MLX5_REG_PMPE		 = 0x5010,
	MLX5_REG_PELC		 = 0x500e,
	MLX5_REG_PVLC		 = 0x500f,
	MLX5_REG_PCMR		 = 0x5041,
	MLX5_REG_PDDR		 = 0x5031,
	MLX5_REG_PMLP		 = 0x5002,
	MLX5_REG_PPLM		 = 0x5023,
	MLX5_REG_PPHCR		 = 0x503E,
	MLX5_REG_PCAM		 = 0x507f,
	MLX5_REG_NODE_DESC	 = 0x6001,
	MLX5_REG_HOST_ENDIANNESS = 0x7004,
	MLX5_REG_MTCAP		 = 0x9009,
	MLX5_REG_MTMP		 = 0x900A,
	MLX5_REG_MCIA		 = 0x9014,
	MLX5_REG_MNVDA		 = 0x9024,
	MLX5_REG_MFRL		 = 0x9028,
	MLX5_REG_MLCR		 = 0x902b,
	MLX5_REG_MRTC		 = 0x902d,
	MLX5_REG_MTRC_CAP	 = 0x9040,
	MLX5_REG_MTRC_CONF	 = 0x9041,
	MLX5_REG_MTRC_STDB	 = 0x9042,
	MLX5_REG_MTRC_CTRL	 = 0x9043,
	MLX5_REG_MPEIN		 = 0x9050,
	MLX5_REG_MPCNT		 = 0x9051,
	MLX5_REG_MTPPS		 = 0x9053,
	MLX5_REG_MTPPSE		 = 0x9054,
	MLX5_REG_MTUTC		 = 0x9055,
	MLX5_REG_MPEGC		 = 0x9056,
	MLX5_REG_MPIR		 = 0x9059,
	MLX5_REG_MCQS		 = 0x9060,
	MLX5_REG_MCQI		 = 0x9061,
	MLX5_REG_MCC		 = 0x9062,
	MLX5_REG_MCDA		 = 0x9063,
	MLX5_REG_MCAM		 = 0x907f,
	MLX5_REG_MSECQ		 = 0x9155,
	MLX5_REG_MSEES		 = 0x9156,
	MLX5_REG_MIRC		 = 0x9162,
	MLX5_REG_MTPTM		 = 0x9180,
	MLX5_REG_MTCTR		 = 0x9181,
	MLX5_REG_MRTCQ		 = 0x9182,
	MLX5_REG_SBCAM		 = 0xB01F,
	MLX5_REG_RESOURCE_DUMP   = 0xC000,
	MLX5_REG_NIC_CAP	 = 0xC00D,
	MLX5_REG_DTOR            = 0xC00E,
	MLX5_REG_VHCA_ICM_CTRL	 = 0xC010,
};

enum mlx5_qpts_trust_state {
	MLX5_QPTS_TRUST_PCP  = 1,
	MLX5_QPTS_TRUST_DSCP = 2,
};

enum mlx5_dcbx_oper_mode {
	MLX5E_DCBX_PARAM_VER_OPER_HOST  = 0x0,
	MLX5E_DCBX_PARAM_VER_OPER_AUTO  = 0x3,
};

enum {
	MLX5_ATOMIC_OPS_CMP_SWAP	= 1 << 0,
	MLX5_ATOMIC_OPS_FETCH_ADD	= 1 << 1,
	MLX5_ATOMIC_OPS_EXTENDED_CMP_SWAP = 1 << 2,
	MLX5_ATOMIC_OPS_EXTENDED_FETCH_ADD = 1 << 3,
};

enum mlx5_page_fault_resume_flags {
	MLX5_PAGE_FAULT_RESUME_REQUESTOR = 1 << 0,
	MLX5_PAGE_FAULT_RESUME_WRITE	 = 1 << 1,
	MLX5_PAGE_FAULT_RESUME_RDMA	 = 1 << 2,
	MLX5_PAGE_FAULT_RESUME_ERROR	 = 1 << 7,
};

enum dbg_rsc_type {
	MLX5_DBG_RSC_QP,
	MLX5_DBG_RSC_EQ,
	MLX5_DBG_RSC_CQ,
};

enum port_state_policy {
	MLX5_POLICY_DOWN	= 0,
	MLX5_POLICY_UP		= 1,
	MLX5_POLICY_FOLLOW	= 2,
	MLX5_POLICY_INVALID	= 0xffffffff
};

enum mlx5_coredev_type {
	MLX5_COREDEV_PF,
	MLX5_COREDEV_VF,
	MLX5_COREDEV_SF,
};

struct mlx5_field_desc {
	int			i;
};

struct mlx5_rsc_debug {
	struct mlx5_core_dev   *dev;
	void		       *object;
	enum dbg_rsc_type	type;
	struct dentry	       *root;
	struct mlx5_field_desc	fields[];
};

enum mlx5_dev_event {
	MLX5_DEV_EVENT_SYS_ERROR = 128, /* 0 - 127 are FW events */
	MLX5_DEV_EVENT_PORT_AFFINITY = 129,
	MLX5_DEV_EVENT_MULTIPORT_ESW = 130,
};

enum mlx5_port_status {
	MLX5_PORT_UP        = 1,
	MLX5_PORT_DOWN      = 2,
};

enum mlx5_cmdif_state {
	MLX5_CMDIF_STATE_UNINITIALIZED,
	MLX5_CMDIF_STATE_UP,
	MLX5_CMDIF_STATE_DOWN,
};

struct mlx5_cmd_first {
	__be32		data[4];
};

struct mlx5_cmd_msg {
	struct list_head		list;
	struct cmd_msg_cache	       *parent;
	u32				len;
	struct mlx5_cmd_first		first;
	struct mlx5_cmd_mailbox	       *next;
};

struct mlx5_cmd_debug {
	struct dentry	       *dbg_root;
	void		       *in_msg;
	void		       *out_msg;
	u8			status;
	u16			inlen;
	u16			outlen;
};

struct cmd_msg_cache {
	/* protect block chain allocations
	 */
	spinlock_t		lock;
	struct list_head	head;
	unsigned int		max_inbox_size;
	unsigned int		num_ent;
};

enum {
	MLX5_NUM_COMMAND_CACHES = 5,
};

struct mlx5_cmd_stats {
	u64		sum;
	u64		n;
	/* number of times command failed */
	u64		failed;
	/* number of times command failed on bad status returned by FW */
	u64		failed_mbox_status;
	/* last command failed returned errno */
	u32		last_failed_errno;
	/* last bad status returned by FW */
	u8		last_failed_mbox_status;
	/* last command failed syndrome returned by FW */
	u32		last_failed_syndrome;
	struct dentry  *root;
	/* protect command average calculations */
	spinlock_t	lock;
};

struct mlx5_cmd {
	struct mlx5_nb    nb;

	/* members which needs to be queried or reinitialized each reload */
	struct {
		u16		cmdif_rev;
		u8		log_sz;
		u8		log_stride;
		int		max_reg_cmds;
		unsigned long	bitmask;
		struct semaphore sem;
		struct semaphore pages_sem;
		struct semaphore throttle_sem;
		struct semaphore unprivileged_sem;
		struct xarray	privileged_uids;
	} vars;
	enum mlx5_cmdif_state	state;
	void	       *cmd_alloc_buf;
	dma_addr_t	alloc_dma;
	int		alloc_size;
	void	       *cmd_buf;
	dma_addr_t	dma;

	/*
	 * If non-NULL, the cmd ring page was allocated through the per-VF
	 * vfmig deterministic IOVA allocator (see vfmig_iova.h) instead of
	 * dma_alloc_coherent(), and free_cmd_page() must release it via
	 * vfmig_iova_free_coherent() against this domain. NULL means the
	 * default DMA allocator was used. Set unconditionally so cmd.c
	 * doesn't have to #ifdef around the field; the actual route-through
	 * is gated on CONFIG_MLX5_VFMIG via mlx5_vf_get_vfmig_iova_domain().
	 *
	 * Opaque on purpose: struct vfmig_iova_domain is mlx5_core-internal,
	 * not exported in this header.
	 */
	void	       *vfmig_iova_dom;

	/*
	 * Per-device opt-out of switching the cmd interface from polling
	 * to event mode at mlx5_eq_table_create() time. When true,
	 * create_async_eqs() leaves cmd_eq in polling mode (does not call
	 * mlx5_cmd_use_events()) and destroy_async_eqs() does not undo
	 * what was never set up. All cmds against this mdev for the
	 * lifetime of the device complete via lay->status_own polling on
	 * the cmd ring; the cmd EQ is created (so FW has a place to drop
	 * cmd-completion EQEs if it chooses to) but the host never reads
	 * from it.
	 *
	 * Targeted workaround for the post-LOAD slot-0 ghost EQE / lost
	 * real-completion failure mode. Empirically every observed
	 * variant of that failure happens after mlx5_cmd_use_events()
	 * runs on a freshly-restored VF. vfio-pci-mlx5 (the upstream
	 * SR-IOV live-migration driver) never calls mlx5_cmd_use_events()
	 * on the VF -- the VF cmd interface is guest-owned -- and never
	 * exhibits this failure. Mirroring that "no cmd_use_events on the
	 * VF" property is the most direct way to remove the trigger
	 * without re-architecting probe.
	 *
	 * Set by mlx5_load() before mlx5_eq_table_create() on a restored
	 * VHCA when vfmig_load_skip_cmd_use_events is Y; cleared only in
	 * mlx5_init_cmd_data() (i.e., next mdev probe). Default false on
	 * every other code path.
	 *
	 * Note: cmd-interface mode and force_polling are orthogonal.
	 * Setting this leaves the mdev in polling MODE (no EQ-fed
	 * completions on cmd_eq, no cmd-comp notifier registered);
	 * setting force_polling instead keeps the mdev in event mode and
	 * just routes individual cmds through the polling ring on the
	 * issue side. The two have different semantics on teardown:
	 * skipping cmd_use_events is symmetric (we also skip the matching
	 * cmd_use_polling at destroy_async_eqs), while force_polling has
	 * to be paired with a regular cmd_use_polling unwind.
	 */
	bool		vfmig_skip_cmd_use_events;

	/* protect command queue allocations
	 */
	spinlock_t	alloc_lock;

	/* protect token allocations
	 */
	spinlock_t	token_lock;
	u8		token;
	char		wq_name[MLX5_CMD_WQ_MAX_NAME];
	struct workqueue_struct *wq;
	int	mode;
	u16     allowed_opcode;
	struct mlx5_cmd_work_ent *ent_arr[MLX5_MAX_COMMANDS];
	struct dma_pool *pool;
	struct mlx5_cmd_debug dbg;
	struct cmd_msg_cache cache[MLX5_NUM_COMMAND_CACHES];
	int checksum_disabled;
	struct xarray stats;
};

struct mlx5_cmd_mailbox {
	void	       *buf;
	dma_addr_t	dma;
	struct mlx5_cmd_mailbox *next;
};

struct mlx5_buf_list {
	void		       *buf;
	dma_addr_t		map;
};

struct mlx5_frag_buf {
	struct mlx5_buf_list	*frags;
	int			npages;
	int			size;
	u8			page_shift;
	/*
	 * Internal use by the vfmig deterministic-IOVA allocator: the
	 * enum vfmig_iova_slot the backing pages were allocated from.
	 * Set by mlx5_frag_buf_alloc_node[_slot]; consumed by
	 * mlx5_frag_buf_free to route the symmetric free back to the
	 * same slot. 0 (== VFMIG_SLOT_INVALID) on bufs that pre-date
	 * this field's lifetime; the free path treats that as "use
	 * the legacy DMA_COHERENT slot" so external-module callers
	 * (mlx5_ib, vfio_pci_mlx5, vdpa) keep working without any
	 * recompile.
	 */
	u8			vfmig_slot;
};

struct mlx5_frag_buf_ctrl {
	struct mlx5_buf_list   *frags;
	u32			sz_m1;
	u16			frag_sz_m1;
	u16			strides_offset;
	u8			log_sz;
	u8			log_stride;
	u8			log_frag_strides;
};

struct mlx5_core_psv {
	u32	psv_idx;
	struct psv_layout {
		u32	pd;
		u16	syndrome;
		u16	reserved;
		u16	bg;
		u16	app_tag;
		u32	ref_tag;
	} psv;
};

struct mlx5_core_sig_ctx {
	struct mlx5_core_psv	psv_memory;
	struct mlx5_core_psv	psv_wire;
	struct ib_sig_err       err_item;
	bool			sig_status_checked;
	bool			sig_err_exists;
	u32			sigerr_count;
};

#define MLX5_24BIT_MASK		((1 << 24) - 1)

enum mlx5_res_type {
	MLX5_RES_QP	= MLX5_EVENT_QUEUE_TYPE_QP,
	MLX5_RES_RQ	= MLX5_EVENT_QUEUE_TYPE_RQ,
	MLX5_RES_SQ	= MLX5_EVENT_QUEUE_TYPE_SQ,
	MLX5_RES_SRQ	= 3,
	MLX5_RES_XSRQ	= 4,
	MLX5_RES_XRQ	= 5,
};

struct mlx5_core_rsc_common {
	enum mlx5_res_type	res;
	refcount_t		refcount;
	struct completion	free;
	bool			invalid;
};

struct mlx5_uars_page {
	void __iomem	       *map;
	bool			wc;
	u32			index;
	struct list_head	list;
	unsigned int		bfregs;
	unsigned long	       *reg_bitmap; /* for non fast path bf regs */
	unsigned long	       *fp_bitmap;
	unsigned int		reg_avail;
	unsigned int		fp_avail;
	struct kref		ref_count;
	struct mlx5_core_dev   *mdev;
};

struct mlx5_bfreg_head {
	/* protect blue flame registers allocations */
	struct mutex		lock;
	struct list_head	list;
};

struct mlx5_bfreg_data {
	struct mlx5_bfreg_head	reg_head;
	struct mlx5_bfreg_head	wc_head;
};

struct mlx5_sq_bfreg {
	void __iomem	       *map;
	struct mlx5_uars_page  *up;
	bool			wc;
	u32			index;
};

struct mlx5_core_health {
	struct health_buffer __iomem   *health;
	__be32 __iomem		       *health_counter;
	struct timer_list		timer;
	u32				prev;
	int				miss_counter;
	u8				synd;
	u32				fatal_error;
	u32				crdump_size;
	struct workqueue_struct	       *wq;
	unsigned long			flags;
	struct work_struct		fatal_report_work;
	struct work_struct		report_work;
	struct devlink_health_reporter *fw_reporter;
	struct devlink_health_reporter *fw_fatal_reporter;
	struct devlink_health_reporter *vnic_reporter;
	struct delayed_work		update_fw_log_ts_work;
};

enum {
	MLX5_PF_NOTIFY_DISABLE_VF,
	MLX5_PF_NOTIFY_ENABLE_VF,
};

/*
 * Datapath-quiesce state of a VF's VHCA on the firmware migration FSM's
 * RUNNING <-> RUNNING_P2P <-> STOP ladder. Tracked persistently per VF in
 * vfs_ctx[].vfmig_dp_state so SAVE, SR-IOV teardown, and the directional
 * SUSPEND/RESUME ioctls agree on the current parked depth.
 * See design/datapath_pause_resume.md Part A/C.
 */
enum mlx5_vfmig_dp_state {
	MLX5_VFMIG_DP_RUNNING = 0,	/* both directions live */
	MLX5_VFMIG_DP_P2P,		/* initiator parked, responder live */
	MLX5_VFMIG_DP_STOP,		/* fully parked; cmd ring dead */
};

struct mlx5_vf_context {
	int	enabled;
	u64	port_guid;
	u64	node_guid;
	/* Valid bits are used to validate administrative guid only.
	 * Enabled after ndo_set_vf_guid
	 */
	u8	port_guid_valid:1;
	u8	node_guid_valid:1;
	/*
	 * Set by /dev/mlx5_vfmig MARK_RESTORED ioctl on the PF mdev. Read
	 * (and consumed) by the next mlx5_core probe of this VF in
	 * mlx5_function_open(), which uses it to skip INIT_HCA so that
	 * firmware state previously installed by LOAD_VHCA_STATE survives.
	 * @restored_vhca_id is captured at MARK_RESTORED time via
	 * QUERY_HCA_CAP(other_function=1) and surfaced in the probe-time
	 * log so that the VF-side message can identify the firmware vHCA
	 * by id rather than just BDF (the BDF is already in the dev_info
	 * prefix). See drivers/net/ethernet/mellanox/mlx5/core/vfmig.c.
	 */
	u8	restored:1;
	u16	restored_vhca_id;
	/*
	 * Opaque per-VF "pending LOAD_VHCA_STATE" slot, owned by
	 * drivers/net/ethernet/mellanox/mlx5/core/vfmig.c. Populated when
	 * the LOAD anon-inode fd is closed after a complete blob has been
	 * staged into DMA-mapped pages; consumed (and freed) by the next
	 * mlx5_core probe of this VF in mlx5_function_open(), which issues
	 * LOAD_VHCA_STATE + RESUME_VHCA(RESPONDER) + RESUME_VHCA(INITIATOR)
	 * via the PF mdev *after* ENABLE_HCA but *before* (skipping)
	 * INIT_HCA. NULL when no LOAD has been staged. See
	 * mlx5_vfmig_vf_apply_pending_load().
	 */
	struct mlx5_vfmig_vf_load *vfmig_pending_load;
	/*
	 * Set by /dev/mlx5_vfmig SET_TRACKED { enable=1 } ioctl on the PF
	 * mdev. When set, the per-VF IOMMU domain pointed to by
	 * @vfmig_iova_dom owns the VF's address space, and host-side
	 * allocators (cmd ring, MANAGE_PAGES, EQ buffers) route through
	 * the deterministic IOVA allocator in vfmig_iova.c. Read by
	 * mlx5_vf_is_vfmig_tracked() and consulted at probe time by
	 * mlx5_cmd_enable() and friends. Cleared by SET_TRACKED
	 * { enable=0 } or by SR-IOV teardown.
	 */
	u8	vfmig_tracked:1;
	/*
	 * Datapath-quiesce state for the snapshot-ordering fix (KS7.6,
	 * design/datapath_pause_resume.md Part A/C).
	 *
	 * @vfmig_dp_state: current parked depth on the RUNNING/RUNNING_P2P/
	 *   STOP ladder (enum mlx5_vfmig_dp_state). Moved by /dev/mlx5_vfmig
	 *   SUSPEND_VHCA / RESUME_VHCA (which may drive a single ladder step
	 *   or the full pair), and set to STOP by a DEFER_RESUME restore.
	 *   Persists across a save session so SAVE_VHCA_STATE can tell
	 *   whether the caller already parked the VF (skip/complete the
	 *   in-SAVE suspend, leave resume to the caller) vs. the legacy
	 *   self-suspend+resume-on-close flow. Force-driven back to RUNNING
	 *   (best-effort RESUME) at SR-IOV teardown so a crashed dumper
	 *   can't strand a VF parked.
	 *
	 * @vfmig_defer_resume: set via MARK_RESTORED { DEFER_RESUME } on
	 *   the destination. Tells mlx5_vfmig_vf_apply_pending_load() to
	 *   run SUSPEND+LOAD_VHCA_STATE but skip the trailing RESUME pair,
	 *   leaving the restored VHCA parked (STOP) until CRIU issues
	 *   RESUME_VHCA at RESUME_DEVICES_LATE (after all MR/ring VMAs are
	 *   restored). Cleared by RESUME_VHCA.
	 *
	 * Plain u8 (not bitfields) on purpose: they must not share a
	 * storage unit with @vfmig_tracked above, whose probe-time reader
	 * (mlx5_vf_is_vfmig_tracked) runs concurrently with the SUSPEND/
	 * RESUME ioctls and would otherwise see a torn read from the
	 * read-modify-write of an adjacent bitfield.
	 */
	u8	vfmig_dp_state;
	u8	vfmig_defer_resume;
	struct vfmig_iova_domain *vfmig_iova_dom;
	/*
	 * Set by /dev/mlx5_vfmig SET_VF_UUID ioctl on the PF mdev.
	 * Orchestrator-owned per-VF identity tag that survives across
	 * SAVE/LOAD on the same PF and is stamped symmetrically on
	 * source and destination by the orchestrator before workload
	 * bind, so CRIU's dump and restore paths can bind a saved-
	 * state image to a destination VF without depending on
	 * @vhca_id (per-PF allocator, unstable) or @vf_id (per-PF
	 * slot, may differ source-vs-destination). Read via QUERY_VF
	 * by CRIU dump/restore plus the prerestore binary. The all-
	 * zero value (uuid_null) means "unset" (initial state, and
	 * post-teardown); use uuid_is_null() to test. Cleared
	 * explicitly via mlx5_vfmig_pf_drop_vf_uuids() at SR-IOV
	 * teardown (sriov_numvfs=0) and PF unload. See KS7.3 in
	 * tools/testing/criu_rdma/design/vf_prerestore_split.md §3.5.
	 */
	uuid_t	vf_uuid;
	enum port_state_policy	policy;
	struct blocking_notifier_head notifier;
};

struct mlx5_core_sriov {
	struct mlx5_vf_context	*vfs_ctx;
	int			num_vfs;
	u16			max_vfs;
	u16			max_ec_vfs;
};

struct mlx5_events;
struct mlx5_mpfs;
struct mlx5_eswitch;
struct mlx5_lag;
struct mlx5_devcom_dev;
struct mlx5_fw_reset;
struct mlx5_eq_table;
struct mlx5_vfmig_pf;
struct mlx5_vfmig_vf_load;
struct vfmig_iova_domain;
struct mlx5_irq_table;
struct mlx5_sf_dev_table;
struct mlx5_sf_hw_table;
struct mlx5_sf_table;
struct mlx5_crypto_dek_priv;

struct mlx5_rate_limit {
	u32			rate;
	u32			max_burst_sz;
	u16			typical_pkt_sz;
};

struct mlx5_rl_entry {
	u8 rl_raw[MLX5_ST_SZ_BYTES(set_pp_rate_limit_context)];
	u64 refcount;
	u16 index;
	u16 uid;
	u8 dedicated : 1;
};

struct mlx5_rl_table {
	/* protect rate limit table */
	struct mutex            rl_lock;
	u16                     max_size;
	u32                     max_rate;
	u32                     min_rate;
	struct mlx5_rl_entry   *rl_entry;
	u64 refcount;
};

struct mlx5_core_roce {
	struct mlx5_flow_table *ft;
	struct mlx5_flow_group *fg;
	struct mlx5_flow_handle *allow_rule;
};

enum {
	MLX5_PRIV_FLAGS_DISABLE_IB_ADEV = 1 << 0,
	MLX5_PRIV_FLAGS_DISABLE_ALL_ADEV = 1 << 1,
	/* Set during device detach to block any further devices
	 * creation/deletion on drivers rescan. Unset during device attach.
	 */
	MLX5_PRIV_FLAGS_DETACH = 1 << 2,
	MLX5_PRIV_FLAGS_SWITCH_LEGACY = 1 << 3,
};

struct mlx5_adev {
	struct auxiliary_device adev;
	struct mlx5_core_dev *mdev;
	int idx;
};

struct mlx5_debugfs_entries {
	struct dentry *dbg_root;
	struct dentry *qp_debugfs;
	struct dentry *eq_debugfs;
	struct dentry *cq_debugfs;
	struct dentry *cmdif_debugfs;
	struct dentry *pages_debugfs;
	struct dentry *lag_debugfs;
};

enum mlx5_func_type {
	MLX5_PF,
	MLX5_VF,
	MLX5_SF,
	MLX5_HOST_PF,
	MLX5_EC_VF,
	MLX5_FUNC_TYPE_NUM,
};

struct mlx5_ft_pool;
struct mlx5_priv {
	/* IRQ table valid only for real pci devices PF or VF */
	struct mlx5_irq_table   *irq_table;
	struct mlx5_eq_table	*eq_table;

	/* pages stuff */
	struct mlx5_nb          pg_nb;
	struct workqueue_struct *pg_wq;
	struct xarray           page_root_xa;
	atomic_t		reg_pages;
	struct list_head	free_list;
	u32			fw_pages;
	u32			page_counters[MLX5_FUNC_TYPE_NUM];
	u32			fw_pages_alloc_failed;
	u32			give_pages_dropped;
	u32			reclaim_pages_discard;

	struct mlx5_core_health health;
	struct list_head	traps;

	struct mlx5_debugfs_entries dbg;

	/* start: alloc staff */
	/* protect buffer allocation according to numa node */
	struct mutex            alloc_mutex;
	int                     numa_node;

	struct mutex            pgdir_mutex;
	struct list_head        pgdir_list;
	/* end: alloc staff */

	struct mlx5_adev       **adev;
	int			adev_idx;
	int			sw_vhca_id;
	struct mlx5_events      *events;
	struct mlx5_vhca_events *vhca_events;

	struct mlx5_flow_steering *steering;
	struct mlx5_mpfs        *mpfs;
	struct blocking_notifier_head esw_n_head;
	struct mlx5_eswitch     *eswitch;
	struct mlx5_core_sriov	sriov;
	struct mlx5_lag		*lag;
	u32			flags;
	struct mlx5_devcom_dev	*devc;
	struct mlx5_devcom_comp_dev *hca_devcom_comp;
	struct mlx5_fw_reset	*fw_reset;
	struct mlx5_core_roce	roce;
	struct mlx5_fc_stats		*fc_stats;
	struct mlx5_rl_table            rl_table;
	struct mlx5_ft_pool		*ft_pool;

	struct mlx5_bfreg_data		bfregs;
	struct mlx5_sq_bfreg bfreg;
#ifdef CONFIG_MLX5_SF
	struct mlx5_nb vhca_state_nb;
	struct blocking_notifier_head vhca_state_n_head;
	struct notifier_block sf_dev_nb;
	struct mlx5_sf_dev_table *sf_dev_table;
	struct mlx5_core_dev *parent_mdev;
#endif
#ifdef CONFIG_MLX5_SF_MANAGER
	struct notifier_block sf_hw_table_vhca_nb;
	struct mlx5_sf_hw_table *sf_hw_table;
	struct notifier_block sf_table_esw_nb;
	struct notifier_block sf_table_vhca_nb;
	struct notifier_block sf_table_mdev_nb;
	struct mlx5_sf_table *sf_table;
#endif
	struct blocking_notifier_head lag_nh;

	/*
	 * Per-PF state for the host-driven VF migration / CRIU restore cdev
	 * (/dev/mlx5_vfmig/<bdf>). NULL on VFs and on PFs that haven't yet
	 * registered the cdev. Owned by drivers/net/ethernet/mellanox/mlx5/
	 * core/vfmig.c.
	 */
	struct mlx5_vfmig_pf *vfmig;

	/*
	 * Per-VF-mdev "this VF was brought up via the vfmig restore path"
	 * latch. Set in mlx5_function_enable() after the
	 * mlx5_vfmig_vf_consume_restored() branch has fired; stays set for
	 * the lifetime of this mdev. Read via mlx5_vf_is_restored() so that
	 * subsystems outside mlx5_core (mlx5_ib in particular) can refuse to
	 * post FW commands that would mutate VHCA state already installed
	 * by LOAD_VHCA_STATE.
	 *
	 * Lives on the VF mdev's own priv (not on the PF's
	 * sriov.vfs_ctx[].restored, which is consumed-and-cleared during
	 * probe) so that consumers can interrogate it at any point in the
	 * mdev's lifetime without coordinating with the PF.
	 */
	bool	vfmig_self_restored;
};

enum mlx5_device_state {
	MLX5_DEVICE_STATE_UP = 1,
	MLX5_DEVICE_STATE_INTERNAL_ERROR,
};

enum mlx5_interface_state {
	MLX5_INTERFACE_STATE_UP = BIT(0),
	MLX5_BREAK_FW_WAIT = BIT(1),
};

enum mlx5_pci_status {
	MLX5_PCI_STATUS_DISABLED,
	MLX5_PCI_STATUS_ENABLED,
};

enum mlx5_pagefault_type_flags {
	MLX5_PFAULT_REQUESTOR = 1 << 0,
	MLX5_PFAULT_WRITE     = 1 << 1,
	MLX5_PFAULT_RDMA      = 1 << 2,
};

struct mlx5_td {
	/* protects tirs list changes while tirs refresh */
	struct mutex     list_lock;
	struct list_head tirs_list;
	u32              tdn;
};

struct mlx5e_resources {
	struct mlx5e_hw_objs {
		u32                        pdn;
		struct mlx5_td             td;
		u32			   mkey;
		struct mlx5_sq_bfreg      *bfregs;
		unsigned int               num_bfregs;
#define MLX5_MAX_NUM_TC 8
		u32                        tisn[MLX5_MAX_PORTS][MLX5_MAX_NUM_TC];
		bool			   tisn_valid;
	} hw_objs;
	struct net_device *uplink_netdev;
	netdevice_tracker tracker;
	struct mutex uplink_netdev_lock;
	struct mlx5_crypto_dek_priv *dek_priv;
};

enum mlx5_sw_icm_type {
	MLX5_SW_ICM_TYPE_STEERING,
	MLX5_SW_ICM_TYPE_HEADER_MODIFY,
	MLX5_SW_ICM_TYPE_HEADER_MODIFY_PATTERN,
	MLX5_SW_ICM_TYPE_SW_ENCAP,
};

#define MLX5_MAX_RESERVED_GIDS 8

struct mlx5_rsvd_gids {
	unsigned int start;
	unsigned int count;
	struct ida ida;
};

struct mlx5_clock;
struct mlx5_clock_dev_state;
struct mlx5_dm;
struct mlx5_fw_tracer;
struct mlx5_vxlan;
struct mlx5_geneve;
struct mlx5_hv_vhca;
struct mlx5_st;

#define MLX5_LOG_SW_ICM_BLOCK_SIZE(dev) (MLX5_CAP_DEV_MEM(dev, log_sw_icm_alloc_granularity))
#define MLX5_SW_ICM_BLOCK_SIZE(dev) (1 << MLX5_LOG_SW_ICM_BLOCK_SIZE(dev))

enum {
	MLX5_PROF_MASK_QP_SIZE		= (u64)1 << 0,
	MLX5_PROF_MASK_MR_CACHE		= (u64)1 << 1,
};

enum {
	MKEY_CACHE_LAST_STD_ENTRY = 20,
	MLX5_IMR_KSM_CACHE_ENTRY,
	MAX_MKEY_CACHE_ENTRIES
};

struct mlx5_profile {
	u64	mask;
	u8	log_max_qp;
	u8	num_cmd_caches;
	struct {
		int	size;
		int	limit;
	} mr_cache[MAX_MKEY_CACHE_ENTRIES];
};

struct mlx5_hca_cap {
	u32 cur[MLX5_UN_SZ_DW(hca_cap_union)];
	u32 max[MLX5_UN_SZ_DW(hca_cap_union)];
};

enum mlx5_wc_state {
	MLX5_WC_STATE_UNINITIALIZED,
	MLX5_WC_STATE_UNSUPPORTED,
	MLX5_WC_STATE_SUPPORTED,
};

struct mlx5_core_dev {
	struct device *device;
	enum mlx5_coredev_type coredev_type;
	struct pci_dev	       *pdev;
	/* sync pci state */
	struct mutex		pci_status_mutex;
	enum mlx5_pci_status	pci_status;
	u8			rev_id;
	char			board_id[MLX5_BOARD_ID_LEN];
	struct mlx5_cmd		cmd;
	struct {
		struct mlx5_hca_cap *hca[MLX5_CAP_NUM];
		u32 pcam[MLX5_ST_SZ_DW(pcam_reg)];
		u32 mcam[MLX5_MCAM_REGS_NUM][MLX5_ST_SZ_DW(mcam_reg)];
		u32 fpga[MLX5_ST_SZ_DW(fpga_cap)];
		u32 qcam[MLX5_ST_SZ_DW(qcam_reg)];
		u8  embedded_cpu;
	} caps;
	struct mlx5_timeouts	*timeouts;
	u64			sys_image_guid;
	phys_addr_t		iseg_base;
	struct mlx5_init_seg __iomem *iseg;
	phys_addr_t             bar_addr;
	enum mlx5_device_state	state;
	/* sync interface state */
	struct mutex		intf_state_mutex;
	struct lock_class_key	lock_key;
	unsigned long		intf_state;
	struct mlx5_priv	priv;
	struct mlx5_profile	profile;
	u32			issi;
	struct mlx5e_resources  mlx5e_res;
	struct mlx5_dm          *dm;
	struct mlx5_st          *st;
	struct mlx5_vxlan       *vxlan;
	struct mlx5_geneve      *geneve;
	struct {
		struct mlx5_rsvd_gids	reserved_gids;
		u32			roce_en;
	} roce;
#ifdef CONFIG_MLX5_FPGA
	struct mlx5_fpga_device *fpga;
#endif
	struct mlx5_clock       *clock;
	struct mlx5_clock_dev_state *clock_state;
	struct mlx5_ib_clock_info  *clock_info;
	struct mlx5_fw_tracer   *tracer;
	struct mlx5_rsc_dump    *rsc_dump;
	u32                      vsc_addr;
	struct mlx5_hv_vhca	*hv_vhca;
	struct mlx5_hwmon	*hwmon;
	u64			num_block_tc;
	u64			num_block_ipsec;
#ifdef CONFIG_MLX5_MACSEC
	struct mlx5_macsec_fs *macsec_fs;
	/* MACsec notifier chain to sync MACsec core and IB database */
	struct blocking_notifier_head macsec_nh;
#endif
	u64 num_ipsec_offloads;
	struct mlx5_sd          *sd;
	enum mlx5_wc_state wc_state;
	/* sync write combining state */
	struct mutex wc_state_lock;
};

struct mlx5_db {
	__be32			*db;
	union {
		struct mlx5_db_pgdir		*pgdir;
		struct mlx5_ib_user_db_page	*user_page;
	}			u;
	dma_addr_t		dma;
	int			index;
};

#define MLX5_DEFAULT_NUM_DOORBELLS 8

enum {
	MLX5_COMP_EQ_SIZE = 1024,
};

enum {
	MLX5_PTYS_IB = 1 << 0,
	MLX5_PTYS_EN = 1 << 2,
};

typedef void (*mlx5_cmd_cbk_t)(int status, void *context);

enum {
	MLX5_CMD_ENT_STATE_PENDING_COMP,
	MLX5_CMD_ENT_STATE_TIMEDOUT,
};

struct mlx5_cmd_work_ent {
	unsigned long		state;
	struct mlx5_cmd_msg    *in;
	struct mlx5_cmd_msg    *out;
	void		       *uout;
	int			uout_size;
	mlx5_cmd_cbk_t		callback;
	struct delayed_work	cb_timeout_work;
	void		       *context;
	int			idx;
	struct completion	handling;
	struct completion	slotted;
	struct completion	done;
	struct mlx5_cmd        *cmd;
	struct work_struct	work;
	struct mlx5_cmd_layout *lay;
	int			ret;
	int			page_queue;
	u8			status;
	u8			token;
	u64			ts1;
	u64			ts2;
	u16			op;
	bool			polling;
	/* Track the max comp handlers */
	refcount_t              refcnt;
};

enum phy_port_state {
	MLX5_AAA_111
};

struct mlx5_hca_vport_context {
	u32			field_select;
	bool			sm_virt_aware;
	bool			has_smi;
	bool			has_raw;
	enum port_state_policy	policy;
	enum phy_port_state	phys_state;
	enum ib_port_state	vport_state;
	u8			port_physical_state;
	u64			sys_image_guid;
	u64			port_guid;
	u64			node_guid;
	u32			cap_mask1;
	u32			cap_mask1_perm;
	u16			cap_mask2;
	u16			cap_mask2_perm;
	u16			lid;
	u8			init_type_reply; /* bitmask: see ib spec 14.2.5.6 InitTypeReply */
	u8			lmc;
	u8			subnet_timeout;
	u16			sm_lid;
	u8			sm_sl;
	u16			qkey_violation_counter;
	u16			pkey_violation_counter;
	bool			grh_required;
	u8			num_plane;
};

#define STRUCT_FIELD(header, field) \
	.struct_offset_bytes = offsetof(struct ib_unpacked_ ## header, field),      \
	.struct_size_bytes   = sizeof((struct ib_unpacked_ ## header *)0)->field

extern struct dentry *mlx5_debugfs_root;

static inline u16 fw_rev_maj(struct mlx5_core_dev *dev)
{
	return ioread32be(&dev->iseg->fw_rev) & 0xffff;
}

static inline u16 fw_rev_min(struct mlx5_core_dev *dev)
{
	return ioread32be(&dev->iseg->fw_rev) >> 16;
}

static inline u16 fw_rev_sub(struct mlx5_core_dev *dev)
{
	return ioread32be(&dev->iseg->cmdif_rev_fw_sub) & 0xffff;
}

static inline u32 mlx5_base_mkey(const u32 key)
{
	return key & 0xffffff00u;
}

static inline u32 wq_get_byte_sz(u8 log_sz, u8 log_stride)
{
	return ((u32)1 << log_sz) << log_stride;
}

static inline void mlx5_init_fbc_offset(struct mlx5_buf_list *frags,
					u8 log_stride, u8 log_sz,
					u16 strides_offset,
					struct mlx5_frag_buf_ctrl *fbc)
{
	fbc->frags      = frags;
	fbc->log_stride = log_stride;
	fbc->log_sz     = log_sz;
	fbc->sz_m1	= (1 << fbc->log_sz) - 1;
	fbc->log_frag_strides = PAGE_SHIFT - fbc->log_stride;
	fbc->frag_sz_m1	= (1 << fbc->log_frag_strides) - 1;
	fbc->strides_offset = strides_offset;
}

static inline void mlx5_init_fbc(struct mlx5_buf_list *frags,
				 u8 log_stride, u8 log_sz,
				 struct mlx5_frag_buf_ctrl *fbc)
{
	mlx5_init_fbc_offset(frags, log_stride, log_sz, 0, fbc);
}

static inline void *mlx5_frag_buf_get_wqe(struct mlx5_frag_buf_ctrl *fbc,
					  u32 ix)
{
	unsigned int frag;

	ix  += fbc->strides_offset;
	frag = ix >> fbc->log_frag_strides;

	return fbc->frags[frag].buf + ((fbc->frag_sz_m1 & ix) << fbc->log_stride);
}

static inline u32
mlx5_frag_buf_get_idx_last_contig_stride(struct mlx5_frag_buf_ctrl *fbc, u32 ix)
{
	u32 last_frag_stride_idx = (ix + fbc->strides_offset) | fbc->frag_sz_m1;

	return min_t(u32, last_frag_stride_idx - fbc->strides_offset, fbc->sz_m1);
}

enum {
	CMD_ALLOWED_OPCODE_ALL,
};

void mlx5_cmd_use_events(struct mlx5_core_dev *dev);
void mlx5_cmd_use_polling(struct mlx5_core_dev *dev);
void mlx5_cmd_allowed_opcode(struct mlx5_core_dev *dev, u16 opcode);

struct mlx5_async_ctx {
	struct mlx5_core_dev *dev;
	atomic_t num_inflight;
	struct completion inflight_done;
};

struct mlx5_async_work;

typedef void (*mlx5_async_cbk_t)(int status, struct mlx5_async_work *context);

struct mlx5_async_work {
	struct mlx5_async_ctx *ctx;
	mlx5_async_cbk_t user_callback;
	u16 opcode; /* cmd opcode */
	u16 op_mod; /* cmd op_mod */
	u8 throttle_locked:1;
	u8 unpriv_locked:1;
	void *out; /* pointer to the cmd output buffer */
};

void mlx5_cmd_init_async_ctx(struct mlx5_core_dev *dev,
			     struct mlx5_async_ctx *ctx);
void mlx5_cmd_cleanup_async_ctx(struct mlx5_async_ctx *ctx);
int mlx5_cmd_exec_cb(struct mlx5_async_ctx *ctx, void *in, int in_size,
		     void *out, int out_size, mlx5_async_cbk_t callback,
		     struct mlx5_async_work *work);
void mlx5_cmd_out_err(struct mlx5_core_dev *dev, u16 opcode, u16 op_mod, void *out);
int mlx5_cmd_do(struct mlx5_core_dev *dev, void *in, int in_size, void *out, int out_size);
int mlx5_cmd_check(struct mlx5_core_dev *dev, int err, void *in, void *out);
int mlx5_cmd_exec(struct mlx5_core_dev *dev, void *in, int in_size, void *out,
		  int out_size);

#define mlx5_cmd_exec_inout(dev, ifc_cmd, in, out)                             \
	({                                                                     \
		mlx5_cmd_exec(dev, in, MLX5_ST_SZ_BYTES(ifc_cmd##_in), out,    \
			      MLX5_ST_SZ_BYTES(ifc_cmd##_out));                \
	})

#define mlx5_cmd_exec_in(dev, ifc_cmd, in)                                     \
	({                                                                     \
		u32 _out[MLX5_ST_SZ_DW(ifc_cmd##_out)] = {};                   \
		mlx5_cmd_exec_inout(dev, ifc_cmd, in, _out);                   \
	})

int mlx5_cmd_exec_polling(struct mlx5_core_dev *dev, void *in, int in_size,
			  void *out, int out_size);
bool mlx5_cmd_is_down(struct mlx5_core_dev *dev);
int mlx5_cmd_add_privileged_uid(struct mlx5_core_dev *dev, u16 uid);
void mlx5_cmd_remove_privileged_uid(struct mlx5_core_dev *dev, u16 uid);

void mlx5_core_uplink_netdev_set(struct mlx5_core_dev *mdev, struct net_device *netdev);
void mlx5_core_uplink_netdev_event_replay(struct mlx5_core_dev *mdev);

void mlx5_core_mp_event_replay(struct mlx5_core_dev *dev, u32 event, void *data);

void mlx5_health_cleanup(struct mlx5_core_dev *dev);
int mlx5_health_init(struct mlx5_core_dev *dev);
void mlx5_start_health_poll(struct mlx5_core_dev *dev);
void mlx5_stop_health_poll(struct mlx5_core_dev *dev, bool disable_health);
void mlx5_start_health_fw_log_up(struct mlx5_core_dev *dev);
void mlx5_drain_health_wq(struct mlx5_core_dev *dev);
void mlx5_trigger_health_work(struct mlx5_core_dev *dev);
int mlx5_frag_buf_alloc_node(struct mlx5_core_dev *dev, int size,
			     struct mlx5_frag_buf *buf, int node);
void mlx5_frag_buf_free(struct mlx5_core_dev *dev, struct mlx5_frag_buf *buf);
int mlx5_core_create_mkey(struct mlx5_core_dev *dev, u32 *mkey, u32 *in,
			  int inlen);
int mlx5_core_destroy_mkey(struct mlx5_core_dev *dev, u32 mkey);
int mlx5_core_query_mkey(struct mlx5_core_dev *dev, u32 mkey, u32 *out,
			 int outlen);
int mlx5_core_alloc_pd(struct mlx5_core_dev *dev, u32 *pdn);
int mlx5_core_dealloc_pd(struct mlx5_core_dev *dev, u32 pdn);
int mlx5_pagealloc_init(struct mlx5_core_dev *dev);
void mlx5_pagealloc_cleanup(struct mlx5_core_dev *dev);
void mlx5_pagealloc_start(struct mlx5_core_dev *dev);
void mlx5_pagealloc_stop(struct mlx5_core_dev *dev);
void mlx5_pages_debugfs_init(struct mlx5_core_dev *dev);
void mlx5_pages_debugfs_cleanup(struct mlx5_core_dev *dev);
int mlx5_satisfy_startup_pages(struct mlx5_core_dev *dev, int boot);
int mlx5_reclaim_startup_pages(struct mlx5_core_dev *dev);
void mlx5_register_debugfs(void);
void mlx5_unregister_debugfs(void);

void mlx5_fill_page_frag_array_perm(struct mlx5_frag_buf *buf, __be64 *pas, u8 perm);
void mlx5_fill_page_frag_array(struct mlx5_frag_buf *frag_buf, __be64 *pas);
int mlx5_comp_eqn_get(struct mlx5_core_dev *dev, u16 vecidx, int *eqn);
int mlx5_core_attach_mcg(struct mlx5_core_dev *dev, union ib_gid *mgid, u32 qpn);
int mlx5_core_detach_mcg(struct mlx5_core_dev *dev, union ib_gid *mgid, u32 qpn);

struct dentry *mlx5_debugfs_get_dev_root(struct mlx5_core_dev *dev);
void mlx5_qp_debugfs_init(struct mlx5_core_dev *dev);
void mlx5_qp_debugfs_cleanup(struct mlx5_core_dev *dev);
int mlx5_access_reg(struct mlx5_core_dev *dev, void *data_in, int size_in,
		    void *data_out, int size_out, u16 reg_id, int arg,
		    int write, bool verbose);
int mlx5_core_access_reg(struct mlx5_core_dev *dev, void *data_in,
			 int size_in, void *data_out, int size_out,
			 u16 reg_num, int arg, int write);

int mlx5_db_alloc_node(struct mlx5_core_dev *dev, struct mlx5_db *db,
		       int node);

static inline int mlx5_db_alloc(struct mlx5_core_dev *dev, struct mlx5_db *db)
{
	return mlx5_db_alloc_node(dev, db, dev->priv.numa_node);
}

void mlx5_db_free(struct mlx5_core_dev *dev, struct mlx5_db *db);

const char *mlx5_command_str(int command);
void mlx5_cmdif_debugfs_init(struct mlx5_core_dev *dev);
void mlx5_cmdif_debugfs_cleanup(struct mlx5_core_dev *dev);
int mlx5_core_create_psv(struct mlx5_core_dev *dev, u32 pdn,
			 int npsvs, u32 *sig_index);
int mlx5_core_destroy_psv(struct mlx5_core_dev *dev, int psv_num);
__be32 mlx5_core_get_terminate_scatter_list_mkey(struct mlx5_core_dev *dev);
void mlx5_core_put_rsc(struct mlx5_core_rsc_common *common);

int mlx5_init_rl_table(struct mlx5_core_dev *dev);
void mlx5_cleanup_rl_table(struct mlx5_core_dev *dev);
int mlx5_rl_add_rate(struct mlx5_core_dev *dev, u16 *index,
		     struct mlx5_rate_limit *rl);
void mlx5_rl_remove_rate(struct mlx5_core_dev *dev, struct mlx5_rate_limit *rl);
bool mlx5_rl_is_in_range(struct mlx5_core_dev *dev, u32 rate);
int mlx5_rl_add_rate_raw(struct mlx5_core_dev *dev, void *rl_in, u16 uid,
			 bool dedicated_entry, u16 *index);
void mlx5_rl_remove_rate_raw(struct mlx5_core_dev *dev, u16 index);
bool mlx5_rl_are_equal(struct mlx5_rate_limit *rl_0,
		       struct mlx5_rate_limit *rl_1);
int mlx5_alloc_bfreg(struct mlx5_core_dev *mdev, struct mlx5_sq_bfreg *bfreg,
		     bool map_wc, bool fast_path);
void mlx5_free_bfreg(struct mlx5_core_dev *mdev, struct mlx5_sq_bfreg *bfreg);

unsigned int mlx5_comp_vectors_max(struct mlx5_core_dev *dev);
int mlx5_comp_vector_get_cpu(struct mlx5_core_dev *dev, int vector);
unsigned int mlx5_core_reserved_gids_count(struct mlx5_core_dev *dev);
int mlx5_core_roce_gid_set(struct mlx5_core_dev *dev, unsigned int index,
			   u8 roce_version, u8 roce_l3_type, const u8 *gid,
			   const u8 *mac, bool vlan, u16 vlan_id, u8 port_num);

static inline u32 mlx5_mkey_to_idx(u32 mkey)
{
	return mkey >> 8;
}

static inline u32 mlx5_idx_to_mkey(u32 mkey_idx)
{
	return mkey_idx << 8;
}

static inline u8 mlx5_mkey_variant(u32 mkey)
{
	return mkey & 0xff;
}

/* Async-atomic event notifier used by mlx5 core to forward FW
 * evetns received from event queue to mlx5 consumers.
 * Optimise event queue dipatching.
 */
int mlx5_notifier_register(struct mlx5_core_dev *dev, struct notifier_block *nb);
int mlx5_notifier_unregister(struct mlx5_core_dev *dev, struct notifier_block *nb);

/* Async-atomic event notifier used for forwarding
 * evetns from the event queue into the to mlx5 events dispatcher,
 * eswitch, clock and others.
 */
int mlx5_eq_notifier_register(struct mlx5_core_dev *dev, struct mlx5_nb *nb);
int mlx5_eq_notifier_unregister(struct mlx5_core_dev *dev, struct mlx5_nb *nb);

/* Blocking event notifier used to forward SW events, used for slow path */
int mlx5_blocking_notifier_register(struct mlx5_core_dev *dev, struct notifier_block *nb);
int mlx5_blocking_notifier_unregister(struct mlx5_core_dev *dev, struct notifier_block *nb);
int mlx5_blocking_notifier_call_chain(struct mlx5_core_dev *dev, unsigned int event,
				      void *data);

int mlx5_core_query_vendor_id(struct mlx5_core_dev *mdev, u32 *vendor_id);

int mlx5_cmd_create_vport_lag(struct mlx5_core_dev *dev);
int mlx5_cmd_destroy_vport_lag(struct mlx5_core_dev *dev);
bool mlx5_lag_is_roce(struct mlx5_core_dev *dev);
bool mlx5_lag_is_sriov(struct mlx5_core_dev *dev);
bool mlx5_lag_is_active(struct mlx5_core_dev *dev);
bool mlx5_lag_mode_is_hash(struct mlx5_core_dev *dev);
bool mlx5_lag_is_master(struct mlx5_core_dev *dev);
bool mlx5_lag_is_shared_fdb(struct mlx5_core_dev *dev);
bool mlx5_lag_is_mpesw(struct mlx5_core_dev *dev);
u8 mlx5_lag_get_slave_port(struct mlx5_core_dev *dev,
			   struct net_device *slave);
int mlx5_lag_query_cong_counters(struct mlx5_core_dev *dev,
				 u64 *values,
				 int num_counters,
				 size_t *offsets);
struct mlx5_core_dev *mlx5_lag_get_next_peer_mdev(struct mlx5_core_dev *dev, int *i);

#define mlx5_lag_for_each_peer_mdev(dev, peer, i)				\
	for (i = 0, peer = mlx5_lag_get_next_peer_mdev(dev, &i);		\
	     peer;								\
	     peer = mlx5_lag_get_next_peer_mdev(dev, &i))

u8 mlx5_lag_get_num_ports(struct mlx5_core_dev *dev);
struct mlx5_uars_page *mlx5_get_uars_page(struct mlx5_core_dev *mdev);
void mlx5_put_uars_page(struct mlx5_core_dev *mdev, struct mlx5_uars_page *up);
int mlx5_dm_sw_icm_alloc(struct mlx5_core_dev *dev, enum mlx5_sw_icm_type type,
			 u64 length, u32 log_alignment, u16 uid,
			 phys_addr_t *addr, u32 *obj_id);
int mlx5_dm_sw_icm_dealloc(struct mlx5_core_dev *dev, enum mlx5_sw_icm_type type,
			   u64 length, u16 uid, phys_addr_t addr, u32 obj_id);

#ifdef CONFIG_PCIE_TPH
int mlx5_st_alloc_index(struct mlx5_core_dev *dev, enum tph_mem_type mem_type,
			unsigned int cpu_uid, u16 *st_index);
int mlx5_st_dealloc_index(struct mlx5_core_dev *dev, u16 st_index);
#else
static inline int mlx5_st_alloc_index(struct mlx5_core_dev *dev,
				      enum tph_mem_type mem_type,
				      unsigned int cpu_uid, u16 *st_index)
{
	return -EOPNOTSUPP;
}
static inline int mlx5_st_dealloc_index(struct mlx5_core_dev *dev, u16 st_index)
{
	return -EOPNOTSUPP;
}
#endif

struct mlx5_core_dev *mlx5_vf_get_core_dev(struct pci_dev *pdev);
void mlx5_vf_put_core_dev(struct mlx5_core_dev *mdev);

int mlx5_sriov_blocking_notifier_register(struct mlx5_core_dev *mdev,
					  int vf_id,
					  struct notifier_block *nb);
void mlx5_sriov_blocking_notifier_unregister(struct mlx5_core_dev *mdev,
					     int vf_id,
					     struct notifier_block *nb);
int mlx5_rdma_rn_get_params(struct mlx5_core_dev *mdev,
			    struct ib_device *device,
			    struct rdma_netdev_alloc_params *params);

enum {
	MLX5_PCI_DEV_IS_VF		= 1 << 0,
};

static inline bool mlx5_core_is_pf(const struct mlx5_core_dev *dev)
{
	return dev->coredev_type == MLX5_COREDEV_PF;
}

static inline bool mlx5_core_is_vf(const struct mlx5_core_dev *dev)
{
	return dev->coredev_type == MLX5_COREDEV_VF;
}

/*
 * Returns true iff this mdev is a VF that was brought up via the vfmig
 * restore path (mlx5_vfmig_vf_consume_restored() fired during probe and
 * LOAD_VHCA_STATE was applied to the underlying VHCA). Latched in
 * priv.vfmig_self_restored for the lifetime of the mdev.
 *
 * Consumers should treat a "true" answer as "this VHCA's FW state was
 * inherited from the source -- do NOT post commands that recreate or
 * reconfigure top-level objects FW already considers populated."
 *
 * Always false on PFs and on VFs probed via the normal path.
 */
static inline bool mlx5_vf_is_restored(const struct mlx5_core_dev *dev)
{
	return dev->priv.vfmig_self_restored;
}

/*
 * Source-side retag for a freshly-registered user MR's IOVA range inside
 * the per-VF vfmig deterministic IOVA domain.
 *
 * Called by mlx5_ib's create_real_mr() chokepoint after the FW mkey is
 * fully wired (post-mlx5r_umr_update_mr_pas in the xlt-with-umr path,
 * or post-reg_create in the slow path). The retag walks the umem's
 * DMA-mapped IOVA range -- which vfmig_dma_ops.map_sg populated with
 * auto-numbered (KIND_NONE) entries at ib_umem_get time -- and rewrites
 * their @instance_key to VFMIG_HUOBJ_KEY(MR, mkey_index), promoting
 * them into the secondary (kind, fw_id) index. SAVE_VHCA_STATE later
 * emits one HOST_USER_PAGE wire record per retagged entry, and LOAD on
 * the destination re-installs them as awaiting_bind=true placeholders.
 *
 * @vf_dev:     this MR's underlying mlx5_core_dev (mlx5_ib_dev->mdev).
 *              No-ops cheaply (O(1) NULL load, no locks) on PFs and on
 *              VFs that aren't vfmig-tracked, so callers can invoke
 *              unconditionally from the MR creation hot path without
 *              imposing PF intf_state_mutex contention on existing
 *              non-vfmig deployments.
 * @mkey_index: the FW-allocated mkey_index, == mr->mmkey.key >> 8.
 * @iova_base:  PAGE_SIZE-aligned lower bound of the umem's DMA IOVA
 *              footprint (sg_dma_address(umem->sgt_append.sgt.sgl)
 *              rounded down to PAGE_SIZE).
 * @length:     PAGE_SIZE-aligned byte length that covers every page
 *              backing the umem.
 *
 * Returns 0 on success or graceful no-op (VF not tracked, no domain,
 * or umem registered via a non-vfmig DMA path so the registry has no
 * matching range -- e.g. a dmabuf umem). Returns a negative errno on
 * hard failures (e.g. -EEXIST if @mkey_index collides with a prior
 * retag, -EINVAL on misaligned arguments). Callers should warn on
 * non-zero returns but should NOT fail the MR registration -- the MR
 * remains fully usable for data path, just not CRIU-restorable.
 *
 * Recorded in tools/testing/criu_rdma/design/user_mr_dma.md §6
 * (Stage 2 source-side retag).
 */
#if IS_ENABLED(CONFIG_MLX5_VFMIG)
int mlx5_vfmig_retag_user_mr(struct mlx5_core_dev *vf_dev, u32 mkey_index,
			     dma_addr_t iova_base, size_t length);
#else
static inline int
mlx5_vfmig_retag_user_mr(struct mlx5_core_dev *vf_dev, u32 mkey_index,
			 dma_addr_t iova_base, size_t length)
{
	return 0;
}
#endif

/*
 * Source-side retag for a freshly-allocated user doorbell page inside
 * the per-VF vfmig deterministic IOVA domain.
 *
 * Called by mlx5_ib_db_map_user()'s miss branch -- the only place the
 * mlx5_ib user doorbell allocator hands a fresh PAGE_SIZE umem to
 * ib_umem_get (and thus to vfmig_dma_ops.map_sg). The hit branch just
 * bumps a refcount on the cached page and does not need to retag,
 * because the page was retagged on its first install.
 *
 * Unlike the MR helper, the second tuple component is a userspace
 * virtual address (page-aligned), not a FW identifier. DBR pages have
 * no FW identity -- the FW only knows individual 8-byte doorbell
 * records' DMA addresses, not "the page". The destination's Stage-3
 * bind path needs a stable key across the SAVE -> LOAD boundary, and
 * mlx5_ib_db_map_user already dedups on (mm, user_virt & PAGE_MASK),
 * so we lean on that key.
 *
 * @vf_dev:     this ucontext's underlying mlx5_core_dev
 *              (mlx5_ib_dev->mdev). Same fast-path semantics as the
 *              MR helper: O(1) NULL load, no locks; no PF
 *              intf_state_mutex hit on non-vfmig deployments.
 * @user_virt:  page-aligned source-side userspace virtual address of
 *              the doorbell page (== mlx5_ib_user_db_page.user_virt).
 *              The helper masks this with PAGE_MASK internally so
 *              callers don't have to.
 * @iova_base:  PAGE_SIZE-aligned DMA address of the doorbell page
 *              (sg_dma_address(umem->sgt_append.sgt.sgl) rounded down
 *              to PAGE_SIZE).
 * @length:     PAGE_SIZE -- one doorbell page is one registry entry.
 *
 * Returns 0 on success or graceful no-op (VF not tracked, no domain,
 * or the umem was registered via a non-vfmig DMA path so the
 * registry has no matching range). Returns a negative errno on hard
 * failures (e.g. -EEXIST if @user_virt collides with a prior retag
 * inside the same VF domain, -EINVAL on misaligned arguments).
 * Callers should warn on non-zero returns but should NOT fail the
 * underlying CQ / QP / SRQ create -- the resource remains fully
 * usable for data path, just not CRIU-restorable.
 *
 * Recorded in tools/testing/criu_rdma/design/user_mr_dma.md §6
 * (Stage 2 source-side retag) and §A.E (DBR walkthrough).
 */
#if IS_ENABLED(CONFIG_MLX5_VFMIG)
int mlx5_vfmig_retag_user_dbr(struct mlx5_core_dev *vf_dev,
			      unsigned long user_virt,
			      dma_addr_t iova_base, size_t length);
#else
static inline int
mlx5_vfmig_retag_user_dbr(struct mlx5_core_dev *vf_dev,
			  unsigned long user_virt,
			  dma_addr_t iova_base, size_t length)
{
	return 0;
}
#endif

/*
 * Source-side retag for a freshly-registered user CQ's IOVA range
 * inside the per-VF vfmig deterministic IOVA domain.
 *
 * Identical contract to mlx5_vfmig_retag_user_mr modulo the second
 * tuple component: @cqn is the FW-allocated cqn (== cq->mcq.cqn),
 * populated by mlx5_core_create_cq's response. Called by mlx5_ib's
 * mlx5_ib_create_cq() right after FW create succeeds, where both
 * cqn and cq->buf.umem are available. The retag promotes the
 * KIND_NONE entries vfmig_dma_ops.map_sg planted during ib_umem_get
 * (in create_cq_user) into VFMIG_HUOBJ_KEY(CQ, cqn)-keyed entries.
 *
 * Same fast-path / error semantics as the MR helper.
 *
 * Recorded in tools/testing/criu_rdma/design/user_mr_dma.md §6.
 */
#if IS_ENABLED(CONFIG_MLX5_VFMIG)
int mlx5_vfmig_retag_user_cq(struct mlx5_core_dev *vf_dev, u32 cqn,
			     dma_addr_t iova_base, size_t length);
#else
static inline int
mlx5_vfmig_retag_user_cq(struct mlx5_core_dev *vf_dev, u32 cqn,
			 dma_addr_t iova_base, size_t length)
{
	return 0;
}
#endif

/*
 * Source-side retag for a freshly-registered user QP's IOVA range
 * inside the per-VF vfmig deterministic IOVA domain.
 *
 * Same contract as the CQ helper modulo the kind: @qpn is the
 * FW-allocated qpn (== qp->trans_qp.base.mqp.qpn for QPC-managed
 * QPs). Called by mlx5_ib's create_user_qp() right after
 * mlx5_qpc_create_qp returns, where qpn is first populated and
 * base->ubuffer.umem (one umem covering both SQ + RQ WQE buffers)
 * is still valid.
 *
 * v0 scope is QPC-managed QP types (RC / UC / UD). RAW_PACKET and
 * SOURCE_QPN QPs go through create_raw_packet_qp() with split
 * SQ/RQ umems and aren't covered by this entry point -- the caller
 * is expected to skip those at the callsite.
 *
 * Same fast-path / error semantics as the MR / CQ helpers.
 *
 * Recorded in tools/testing/criu_rdma/design/user_mr_dma.md §6.
 */
#if IS_ENABLED(CONFIG_MLX5_VFMIG)
int mlx5_vfmig_retag_user_qp(struct mlx5_core_dev *vf_dev, u32 qpn,
			     dma_addr_t iova_base, size_t length);
#else
static inline int
mlx5_vfmig_retag_user_qp(struct mlx5_core_dev *vf_dev, u32 qpn,
			 dma_addr_t iova_base, size_t length)
{
	return 0;
}
#endif

/*
 * Source-side retag for a freshly-registered user SRQ's IOVA range
 * inside the per-VF vfmig deterministic IOVA domain.
 *
 * Same contract as the CQ / QP helpers modulo the kind: @srqn is
 * the FW-allocated srqn (== mlx5_ib_srq->msrq.srqn). Called by
 * mlx5_ib_create_srq() right after mlx5_cmd_create_srq returns,
 * where srqn is first populated and srq->umem is still valid.
 * Covers BASIC / XRC / TM SRQ types -- they share create_srq_user()
 * and msrq.srqn is always populated by the time the create returns.
 *
 * Same fast-path / error semantics as the MR / CQ / QP helpers.
 *
 * Recorded in tools/testing/criu_rdma/design/user_mr_dma.md §6.
 */
#if IS_ENABLED(CONFIG_MLX5_VFMIG)
int mlx5_vfmig_retag_user_srq(struct mlx5_core_dev *vf_dev, u32 srqn,
			      dma_addr_t iova_base, size_t length);
#else
static inline int
mlx5_vfmig_retag_user_srq(struct mlx5_core_dev *vf_dev, u32 srqn,
			  dma_addr_t iova_base, size_t length)
{
	return 0;
}
#endif

struct sg_table;

/*
 * Stage-3 D3: destination-side bind for a freshly-pinned user MR umem
 * inside the per-VF vfmig deterministic IOVA domain.
 *
 * Mirrors the SAVE-side retag chain (mlx5_vfmig_retag_user_mr) but
 * runs on the destination after LOAD_VHCA_STATE has installed the
 * (kind=MR, fw_id=mkey_index) placeholder and after the RESTORE_MR
 * verb body has pinned the user pages via ib_umem_pin(). The bind
 * iommu_maps each sg in @sgt at the placeholder's IOVA range,
 * populates sg_dma_address / sg_dma_len so subsequent
 * vfmig_dma_ops.unmap_sg under ib_umem_release() can find and unmap
 * each entry, and transitions the placeholder awaiting_bind=true ->
 * false.
 *
 * @vf_dev:        this ucontext's underlying mlx5_core_dev
 *                 (mlx5_ib_dev->mdev). O(1) NULL-load gate on
 *                 vf_dev->cmd.vfmig_iova_dom; no PF intf_state_mutex
 *                 hit on non-vfmig deployments.
 * @mkey_index:    24-bit FW mkey_index that identifies the source
 *                 MR. Same value the SAVE-side retag emitted on the
 *                 wire via HOST_USER_PAGE record's instance_key.
 * @sgt:          umem->sgt_append.sgt from ib_umem_pin(). All sgs
 *                 must have phys + length both PAGE_SIZE-aligned;
 *                 summed length must equal the placeholder's
 *                 recorded len (== the source umem's byte length).
 *
 * Returns 0 on success, or:
 *   -ENODEV   non-vfmig deployment / VF not tracked (no domain).
 *             Callers gate at a higher level and shouldn't reach
 *             this errno; surface it so the verb body fails loudly
 *             rather than silently leaving the umem un-bound.
 *   -ENOENT   no placeholder at (KIND_MR, mkey_index) -- CRIU
 *             plugin is binding a uobject the source never
 *             SAVE'd, or whose mkey_index doesn't match Stage 2's
 *             emit.
 *   -EBUSY    placeholder already bound (double-bind).
 *   -EINVAL   sgt total != placeholder len / non-page-aligned sg /
 *             KIND validation.
 *   <0        iommu_map failure (rolled back inside the primitive;
 *             placeholder retained for retry).
 *
 * Per design §A.H L2, on a non-zero return the caller MUST NOT
 * consume sg_dma_address on any sg; partially-populated values are
 * left in place so the caller's ib_umem_release() unwind reaches
 * vfmig_dma_ops.unmap_sg which skips zero-iova sgs safely.
 *
 * Recorded in tools/testing/criu_rdma/design/user_mr_dma.md §7 and
 * §A.C (Stage 3 D3: mlx5_ib_umem_restore -> mlx5_vfmig_bind_user_mr
 * -> vfmig_iova_bind_user_object).
 */
#if IS_ENABLED(CONFIG_MLX5_VFMIG)
int mlx5_vfmig_bind_user_mr(struct mlx5_core_dev *vf_dev, u32 mkey_index,
			    struct sg_table *sgt);
#else
static inline int
mlx5_vfmig_bind_user_mr(struct mlx5_core_dev *vf_dev, u32 mkey_index,
			struct sg_table *sgt)
{
	return -EOPNOTSUPP;
}
#endif

/*
 * Stage-3 D3: destination-side bind for a freshly-pinned user CQ
 * CQE-buffer umem inside the per-VF vfmig deterministic IOVA domain.
 *
 * Mirrors mlx5_vfmig_bind_user_mr modulo the kind enum
 * (VFMIG_HUOBJ_KIND_CQ) and the FW-id semantics (cqn instead of
 * mkey_index). The caller is mlx5_ib_umem_restore_cq's ib_umem_pin
 * -> mlx5_vfmig_bind_user_cq composition (S5b B3), called from
 * mlx5_ib_restore_cq's verb body once LOAD_VHCA_STATE has installed
 * the (kind=CQ, fw_id=cqn) placeholder.
 *
 * @vf_dev:        this ucontext's underlying mlx5_core_dev. O(1)
 *                 NULL-load gate on cmd.vfmig_iova_dom.
 * @cqn:           24-bit FW cqn that identifies the source CQ. Same
 *                 value the SAVE-side retag emitted on the wire via
 *                 HOST_USER_PAGE record's instance_key.
 * @sgt:          umem->sgt_append.sgt from ib_umem_pin().
 *
 * Same return-code semantics as mlx5_vfmig_bind_user_mr.
 *
 * Recorded in tools/testing/criu_rdma/design/uobject_restore.md
 * §S5b and tools/testing/criu_rdma/design/user_mr_dma.md §A.D.
 */
#if IS_ENABLED(CONFIG_MLX5_VFMIG)
int mlx5_vfmig_bind_user_cq(struct mlx5_core_dev *vf_dev, u32 cqn,
			    struct sg_table *sgt);
#else
static inline int
mlx5_vfmig_bind_user_cq(struct mlx5_core_dev *vf_dev, u32 cqn,
			struct sg_table *sgt)
{
	return -EOPNOTSUPP;
}
#endif

/*
 * Stage-3 D3: destination-side bind for a freshly-pinned user
 * doorbell-page umem inside the per-VF vfmig deterministic IOVA
 * domain.
 *
 * Differs from the MR/CQ variants because DBR is the only kind whose
 * fw_id is a userspace virtual address rather than an FW-allocated
 * identifier (no FW resource owns "the doorbell page"; the FW only
 * ever sees the DMA address of individual 8-byte doorbell records
 * inside it). mlx5_ib_db_map_user already dedups on
 * (mm, user_virt & PAGE_MASK), and the SAVE-side retag
 * (mlx5_vfmig_retag_user_dbr) emits HOST_USER_PAGE records keyed by
 * VFMIG_HUOBJ_KEY(KIND_DBR, user_virt & PAGE_MASK). This bind helper
 * uses the same key.
 *
 * @vf_dev:        this ucontext's underlying mlx5_core_dev.
 * @user_virt:     userspace VA of the doorbell record (does not have
 *                 to be page-aligned; the helper masks to PAGE_MASK
 *                 internally to compose the instance_key).
 * @sgt:           umem->sgt_append.sgt from ib_umem_pin(). Must
 *                 describe exactly one PAGE_SIZE entry; multi-page
 *                 sgts are rejected with -EINVAL.
 *
 * Same return-code semantics as mlx5_vfmig_bind_user_mr.
 *
 * Recorded in tools/testing/criu_rdma/design/user_mr_dma.md §A.E.
 */
#if IS_ENABLED(CONFIG_MLX5_VFMIG)
int mlx5_vfmig_bind_user_dbr(struct mlx5_core_dev *vf_dev,
			     unsigned long user_virt, struct sg_table *sgt);
#else
static inline int
mlx5_vfmig_bind_user_dbr(struct mlx5_core_dev *vf_dev,
			 unsigned long user_virt, struct sg_table *sgt)
{
	return -EOPNOTSUPP;
}
#endif

/*
 * Stage-3 D3: destination-side bind for a freshly-pinned user QP
 * WQ-ring umem inside the per-VF vfmig deterministic IOVA domain.
 *
 * Mirrors mlx5_vfmig_bind_user_cq modulo the kind enum
 * (VFMIG_HUOBJ_KIND_QP) and the FW-id semantics (qpn instead of
 * cqn). The caller is mlx5_ib_umem_restore_qp's ib_umem_pin ->
 * mlx5_vfmig_bind_user_qp composition (S6b B3), called from
 * mlx5_ib_restore_qp's verb body once LOAD_VHCA_STATE has installed
 * the (kind=QP, fw_id=qpn) placeholder.
 *
 * The SAVE-side mlx5_vfmig_retag_user_qp (already exported, fired
 * from create_user_qp post-FW-create) emits the matching
 * HOST_USER_PAGE record covering the single contiguous mapping the
 * user allocates for both SQ and RQ WQE buffers (RQ at offset 0, SQ
 * at qp->sq.offset within the same umem). One bind covers the whole
 * WQE footprint; the FW-side qpc.{log_page_size, page_offset} the
 * source's create_qp_in encoded ride along inside the inherited QPC.
 *
 * @vf_dev:        this ucontext's underlying mlx5_core_dev. O(1)
 *                 NULL-load gate on cmd.vfmig_iova_dom.
 * @qpn:           24-bit FW qpn that identifies the source QP. Same
 *                 value the SAVE-side retag emitted on the wire via
 *                 HOST_USER_PAGE record's instance_key.
 * @sgt:           umem->sgt_append.sgt from ib_umem_pin().
 *
 * Same return-code semantics as mlx5_vfmig_bind_user_mr.
 *
 * Recorded in tools/testing/criu_rdma/design/uobject_restore.md
 * §S6b and tools/testing/criu_rdma/design/user_mr_dma.md §A.D.
 */
#if IS_ENABLED(CONFIG_MLX5_VFMIG)
int mlx5_vfmig_bind_user_qp(struct mlx5_core_dev *vf_dev, u32 qpn,
			    struct sg_table *sgt);
#else
static inline int
mlx5_vfmig_bind_user_qp(struct mlx5_core_dev *vf_dev, u32 qpn,
			struct sg_table *sgt)
{
	return -EOPNOTSUPP;
}
#endif

static inline bool mlx5_core_same_coredev_type(const struct mlx5_core_dev *dev1,
					       const struct mlx5_core_dev *dev2)
{
	return dev1->coredev_type == dev2->coredev_type;
}

static inline bool mlx5_core_is_ecpf(const struct mlx5_core_dev *dev)
{
	return dev->caps.embedded_cpu;
}

static inline bool
mlx5_core_is_ecpf_esw_manager(const struct mlx5_core_dev *dev)
{
	return dev->caps.embedded_cpu && MLX5_CAP_GEN(dev, eswitch_manager);
}

static inline bool mlx5_ecpf_vport_exists(const struct mlx5_core_dev *dev)
{
	return mlx5_core_is_pf(dev) && MLX5_CAP_ESW(dev, ecpf_vport_exists);
}

static inline u16 mlx5_core_max_vfs(const struct mlx5_core_dev *dev)
{
	return dev->priv.sriov.max_vfs;
}

static inline int mlx5_lag_is_lacp_owner(struct mlx5_core_dev *dev)
{
	/* LACP owner conditions:
	 * 1) Function is physical.
	 * 2) LAG is supported by FW.
	 * 3) LAG is managed by driver (currently the only option).
	 */
	return  MLX5_CAP_GEN(dev, vport_group_manager) &&
		   (MLX5_CAP_GEN(dev, num_lag_ports) > 1) &&
		    MLX5_CAP_GEN(dev, lag_master);
}

static inline u16 mlx5_core_max_ec_vfs(const struct mlx5_core_dev *dev)
{
	return dev->priv.sriov.max_ec_vfs;
}

static inline int mlx5_get_gid_table_len(u16 param)
{
	if (param > 4) {
		pr_warn("gid table length is zero\n");
		return 0;
	}

	return 8 * (1 << param);
}

static inline bool mlx5_rl_is_supported(struct mlx5_core_dev *dev)
{
	return !!(dev->priv.rl_table.max_size);
}

static inline int mlx5_core_is_mp_slave(struct mlx5_core_dev *dev)
{
	return MLX5_CAP_GEN(dev, affiliate_nic_vport_criteria) &&
	       MLX5_CAP_GEN(dev, num_vhca_ports) <= 1;
}

static inline int mlx5_core_is_mp_master(struct mlx5_core_dev *dev)
{
	return MLX5_CAP_GEN(dev, num_vhca_ports) > 1;
}

static inline int mlx5_core_mp_enabled(struct mlx5_core_dev *dev)
{
	return mlx5_core_is_mp_slave(dev) ||
	       mlx5_core_is_mp_master(dev);
}

static inline int mlx5_core_native_port_num(struct mlx5_core_dev *dev)
{
	if (!mlx5_core_mp_enabled(dev))
		return 1;

	return MLX5_CAP_GEN(dev, native_port_num);
}

static inline int mlx5_get_dev_index(struct mlx5_core_dev *dev)
{
	int idx = MLX5_CAP_GEN(dev, native_port_num);

	if (idx >= 1 && idx <= MLX5_MAX_PORTS)
		return idx - 1;
	else
		return PCI_FUNC(dev->pdev->devfn);
}

enum {
	MLX5_TRIGGERED_CMD_COMP = (u64)1 << 32,
};

bool mlx5_is_roce_on(struct mlx5_core_dev *dev);

static inline bool mlx5_get_roce_state(struct mlx5_core_dev *dev)
{
	if (MLX5_CAP_GEN(dev, roce_rw_supported))
		return MLX5_CAP_GEN(dev, roce);

	/* If RoCE cap is read-only in FW, get RoCE state from devlink
	 * in order to support RoCE enable/disable feature
	 */
	return mlx5_is_roce_on(dev);
}

#ifdef CONFIG_MLX5_MACSEC
static inline bool mlx5e_is_macsec_device(const struct mlx5_core_dev *mdev)
{
	if (!(MLX5_CAP_GEN_64(mdev, general_obj_types) &
	    MLX5_GENERAL_OBJ_TYPES_CAP_MACSEC_OFFLOAD))
		return false;

	if (!MLX5_CAP_GEN(mdev, log_max_dek))
		return false;

	if (!MLX5_CAP_MACSEC(mdev, log_max_macsec_offload))
		return false;

	if (!MLX5_CAP_FLOWTABLE_NIC_RX(mdev, macsec_decrypt) ||
	    !MLX5_CAP_FLOWTABLE_NIC_RX(mdev, reformat_remove_macsec))
		return false;

	if (!MLX5_CAP_FLOWTABLE_NIC_TX(mdev, macsec_encrypt) ||
	    !MLX5_CAP_FLOWTABLE_NIC_TX(mdev, reformat_add_macsec))
		return false;

	if (!MLX5_CAP_MACSEC(mdev, macsec_crypto_esp_aes_gcm_128_encrypt) &&
	    !MLX5_CAP_MACSEC(mdev, macsec_crypto_esp_aes_gcm_256_encrypt))
		return false;

	if (!MLX5_CAP_MACSEC(mdev, macsec_crypto_esp_aes_gcm_128_decrypt) &&
	    !MLX5_CAP_MACSEC(mdev, macsec_crypto_esp_aes_gcm_256_decrypt))
		return false;

	return true;
}

#define NIC_RDMA_BOTH_DIRS_CAPS (MLX5_FT_NIC_RX_2_NIC_RX_RDMA | MLX5_FT_NIC_TX_RDMA_2_NIC_TX)

static inline bool mlx5_is_macsec_roce_supported(struct mlx5_core_dev *mdev)
{
	if (((MLX5_CAP_GEN_2(mdev, flow_table_type_2_type) &
	     NIC_RDMA_BOTH_DIRS_CAPS) != NIC_RDMA_BOTH_DIRS_CAPS) ||
	     !MLX5_CAP_FLOWTABLE_RDMA_TX(mdev, max_modify_header_actions) ||
	     !mlx5e_is_macsec_device(mdev) || !mdev->macsec_fs)
		return false;

	return true;
}
#endif

enum {
	MLX5_OCTWORD = 16,
};

bool mlx5_wc_support_get(struct mlx5_core_dev *mdev);

static inline struct net *mlx5_core_net(struct mlx5_core_dev *dev)
{
	return devlink_net(priv_to_devlink(dev));
}

#define MLX5_SW_IMAGE_GUID_MAX_BYTES 9

#endif /* MLX5_DRIVER_H */
