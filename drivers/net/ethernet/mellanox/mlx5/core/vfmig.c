// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * mlx5 host-driver-side VF migration / CRIU restore - control plane.
 * See vfmig.h for the architecture overview.
 *
 * Lifetime model
 * --------------
 * One struct mlx5_vfmig_pf is created per PF mlx5_core in mlx5_vfmig_pf_init()
 * and destroyed in mlx5_vfmig_pf_cleanup(). It owns:
 *   - a cdev under /dev/mlx5_vfmig/<bdf>
 *   - a back-pointer to the PF mlx5_core_dev
 *   - lists of open LOAD_VHCA_STATE / SAVE_VHCA_STATE sessions
 *     (mlx5_vfmig_load_ctx / mlx5_vfmig_save_ctx)
 *
 * Userspace can hold the cdev (or any anon-inode fd) open across PF
 * unbind. cdev_del() does NOT wait for in-flight callers, so we use:
 *   - kref:    keeps the struct alive while any fd or ioctl holds a
 *              reference. Initial ref taken in pf_init(), released in
 *              pf_cleanup(). cdev open() takes a ref, cdev release()
 *              drops it. Each LOAD/SAVE session also takes a ref for
 *              the lifetime of its anon-inode fd.
 *   - lock:    rwsem protecting pf_mdev / dead. ioctl handlers, load
 *              fd .write handlers, and save fd .read handlers down_read()
 *              and bail with -ENODEV if dead. pf_cleanup() down_write()s
 *              once to neuter the cdev and synchronously tear down all
 *              session firmware resources before pf_mdev is freed by
 *              mlx5_uninit_one().
 *   - ctxs_lock: mutex protecting load_ctxs and save_ctxs list
 *              mutations and the cross-list "is vf_id already busy?"
 *              check. Held over open's "claim vf_id + list_add" and
 *              release's list_del. Never held while invoking fput().
 *
 * Lock order:
 *      vfmig->lock  ->  vfmig->ctxs_lock  ->  ctx->io_lock
 * pf_cleanup holds vfmig->lock for write, which blocks all readers; the
 * list walks inside pf_cleanup therefore need no further locking.
 */

#include <linux/anon_inodes.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mlx5/device.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/mlx5_ifc.h>
#include <linux/mlx5/vport.h>
#include <uapi/linux/mlx5_vfmig.h>

#include "mlx5_core.h"
#include "vfmig.h"
#include "vfmig_iova.h"

#define MLX5_VFMIG_MAX_DEVICES	256

/*
 * Hardware-imposed maximum bytes per single LOAD_VHCA_STATE command.
 * Mirrors MAX_LOAD_SIZE in drivers/vfio/pci/mlx5/main.c. Records larger
 * than this in the input blob are rejected; userspace must split them.
 */
#define VFMIG_MAX_LOAD_SIZE \
	(BIT_ULL(__mlx5_bit_sz(load_vhca_state_in, size)) - 1)

/*
 * Wire-format header: byte-compatible with the VFIO mlx5 variant driver's
 * migration stream (drivers/vfio/pci/mlx5/cmd.h:mlx5_vf_migration_header).
 * Duplicated here because the original is not exported as UAPI.
 *
 * TODO(vfmig-dedup): once we lift the shared helpers into mlx5_core, this
 * type should be promoted alongside them and the VFIO variant should
 * consume the shared definition.
 */
struct vfmig_wire_header {
	__le64 record_size;
	__le32 flags;
	__le32 tag;
};

/* Mirror MLX5_MIGF_HEADER_TAG_* / FLAGS_TAG_OPTIONAL from the VFIO driver. */
#define VFMIG_WIRE_TAG_FW_DATA		0
#define VFMIG_WIRE_TAG_STOP_COPY_SIZE	1
#define VFMIG_WIRE_FLAGS_TAG_OPTIONAL	BIT(0)

/*
 * VFMIG_WIRE_TAG_HOST_PAGE
 * ------------------------
 * vfmig-private (intentionally outside the VFIO mlx5 tag namespace):
 * carries a single deterministic-IOVA page snapshot from the source
 * VF's vfmig_iova_domain into the destination VF's vfmig_iova_domain
 * via vfmig_iova_replay_page(). One record per registry entry. SAVE
 * emits these BEFORE the FW_DATA record so that on the destination
 * side, by the time the FW_DATA record is staged into the
 * pending_load slot, every IOVA the FW state references already maps
 * to a populated page in the destination's domain. The destination
 * VF probe's first vfmig_iova_alloc_slot() will find the
 * replayed entry at the cursor and reuse it, instead of allocating
 * a fresh empty page.
 *
 * The tag value 0x4842 is "HB" (host-buffer); chosen well clear of
 * the {0, 1} VFIO mlx5 tag range and intentionally NOT marked
 * OPTIONAL: a HOST_PAGE-bearing blob fed to a parser that doesn't
 * understand the tag should fail loudly via the "unknown mandatory
 * tag" rule (-EOPNOTSUPP), because silently dropping the IOVA
 * payload would mean a successful LOAD followed by a dead VHCA --
 * the very failure mode the IOVA work exists to eliminate.
 *
 * On-wire layout per record:
 *   [16 B] struct vfmig_wire_header { record_size, flags=0, tag=HOST_PAGE }
 *   [16 B] struct vfmig_host_page_record { iova, len }
 *   [len bytes] page contents, len % VFMIG_IOVA_GRANULE == 0
 * with record_size = sizeof(struct vfmig_host_page_record) + len.
 *
 * Sanity cap on a single record's @len; records bigger than this are
 * rejected by the LOAD parser. 16 MiB is far above any single
 * registry entry the v1 layered restore plan emits (4 KiB cmd ring
 * page on Layer 1; small contiguous blocks on Layer 2). Bump if a
 * future layer legitimately needs larger atomic regions; for now,
 * the cap is a defense-in-depth check so a malicious or corrupt
 * blob can't kvmalloc the host out of memory before we even reach
 * the IOVA-window range check.
 */
#define VFMIG_WIRE_TAG_HOST_PAGE	0x4842
#define VFMIG_HOST_PAGE_MAX_LEN		(16ULL << 20)

struct vfmig_host_page_record {
	__le64 iova;
	__le64 len;
};

/* Module-wide cdev region; one minor per PF mlx5_core. */
static dev_t mlx5_vfmig_devt;
static struct class *mlx5_vfmig_class;
static DEFINE_IDA(mlx5_vfmig_minor_ida);

struct mlx5_vfmig_load_ctx;
struct mlx5_vfmig_save_ctx;

/*
 * Per-VF "pending LOAD_VHCA_STATE" slot. Owned by vfmig.c, lives on the
 * PF in sriov->vfs_ctx[vf_id].vfmig_pending_load (forward-declared in
 * include/linux/mlx5/driver.h).
 *
 * Populated when a LOAD anon-inode fd is closed after a complete blob
 * has been staged into DMA-mapped pages. Consumed by the VF's probe in
 * mlx5_function_enable() via mlx5_vfmig_vf_apply_pending_load(), which
 * walks the VFIO mlx5 destination arc:
 *
 *   SUSPEND_VHCA(INITIATOR) -> SUSPEND_VHCA(RESPONDER)
 *      -> LOAD_VHCA_STATE
 *      -> RESUME_VHCA(RESPONDER) -> RESUME_VHCA(INITIATOR)
 *
 * all via the PF mdev. The SUSPEND pair is required because firmware
 * rejects LOAD on a VHCA that isn't fully suspended (bad parameter,
 * syndrome 0x2c9bb0 on CX-7).
 *
 * Why this is a separate slot rather than just running the FW commands
 * inside the LOAD ioctl: at LOAD-ioctl time the destination VF is
 * unbound. mlx5_core has not yet brought the VHCA to a state that
 * accepts LOAD. We therefore stage the DMA-mapped pages in the LOAD
 * ioctl and defer all FW commands until the destination's
 * mlx5_function_enable() has the cmd interface up but has not yet
 * issued any VHCA-side bring-up command -- the only window in which
 * the FW will accept LOAD on a destination VHCA freshly created by
 * sriov_numvfs.
 *
 * All FW-tied resources (PD, MKEY, DMA mappings, MTT-input scratch)
 * belong to the PF mdev that owned the LOAD ioctl. They MUST be torn
 * down before that PF mdev unbinds; mlx5_vfmig_pf_cleanup() and
 * mlx5_vfmig_pf_drop_pending_loads() take care of that.
 */
struct mlx5_vfmig_vf_load {
	u32 vf_id;
	u16 vhca_id;
	u32 pdn;
	bool pd_allocated;
	u32 *mkey_in;		/* alloc_mkey_in() buffer; NULL if no MKEY */
	u32 mkey;
	bool mkey_created;
	bool dma_mapped;
	struct dma_iova_state dma_state;
	struct page **pages;
	u32 npages;
	u64 record_size;	/* bytes inside @pages that the FW should consume */
};

/* Per-PF state attached to mlx5_priv via .vfmig opaque pointer. */
struct mlx5_vfmig_pf {
	struct kref kref;
	struct rw_semaphore lock;	/* protects pf_mdev / dead */
	struct mlx5_core_dev *pf_mdev;	/* NULL once dead */
	bool dead;
	struct cdev cdev;
	int minor;

	/* Protects load_ctxs / save_ctxs list mutations and cross-list
	 * "is vf_id already in use?" checks.
	 */
	struct mutex ctxs_lock;
	struct list_head load_ctxs;	/* of struct mlx5_vfmig_load_ctx */
	struct list_head save_ctxs;	/* of struct mlx5_vfmig_save_ctx */
};

/*
 * Parser FSM for the load fd. Mirrors the VFIO variant's
 * MLX5_VF_LOAD_STATE_* enum in drivers/vfio/pci/mlx5/cmd.h.
 */
enum vfmig_load_state {
	VFMIG_LS_READ_HEADER = 0,
	VFMIG_LS_READ_HEADER_DATA,
	VFMIG_LS_PREP_IMAGE,
	VFMIG_LS_READ_IMAGE,
	VFMIG_LS_LOAD_IMAGE,
	/*
	 * HOST_PAGE record sub-states. After dispatch_header reads the
	 * 16-byte vfmig_wire_header and sees tag=HOST_PAGE, the parser:
	 *   HP_READ_SUBHDR -> reads 16 more bytes (iova, len)
	 *   HP_READ_DATA   -> reads @len bytes into a kvmalloc'd buffer
	 *   HP_REPLAY      -> calls vfmig_iova_replay_page() against
	 *                     the destination VF's domain, frees the
	 *                     buffer, returns to READ_HEADER
	 */
	VFMIG_LS_HP_READ_SUBHDR,
	VFMIG_LS_HP_READ_DATA,
	VFMIG_LS_HP_REPLAY,
};

/*
 * Per-LOAD-fd context. Hung off vfmig_pf->load_ctxs. The fd's private_data
 * points here; lifetime is tied to the fd.
 */
struct mlx5_vfmig_load_ctx {
	struct list_head node;		/* on vfmig->load_ctxs */
	struct mlx5_vfmig_pf *vfmig;	/* holds a kref; never NULL once
					 * load_ctx exists and is on the list
					 */
	struct mutex io_lock;		/* serializes concurrent write()s */
	u32 vf_id;
	u16 vhca_id;

	/* Resources tied to the PF mdev. Released by pf_cleanup or release.
	 * Mutated under vfmig->lock (read suffices: pf_cleanup takes write).
	 */
	bool resources_freed;
	bool pd_allocated;
	u32 pdn;

	/*
	 * True once a complete FW_DATA record has been parsed and staged
	 * into image_pages with a valid MKEY ready to hand to
	 * LOAD_VHCA_STATE. The actual FW command is NOT issued here --
	 * see struct mlx5_vfmig_vf_load for why. release() transfers the
	 * staged resources into the per-VF slot for the next probe to
	 * consume; closing without ever writing leaves the VHCA in its
	 * pristine fresh-VF state and we intentionally don't disturb that.
	 */
	bool image_staged;
	/*
	 * Set by release() once the staged DMA/MKEY/PD/pages have been
	 * handed off to the per-VF pending_load slot. Suppresses the
	 * usual ctx-side teardown so the new owner can free them later.
	 */
	bool image_transferred;

	/* Image staging buffer, sized to the largest record we've seen so
	 * far. Reallocated under vfmig->lock-read when a record exceeds it.
	 */
	struct page **image_pages;
	u32 image_npages;	/* allocated capacity, in PAGE_SIZE units */
	u64 image_filled;	/* bytes accumulated in current record */
	u32 *image_mkey_in;	/* alloc_mkey_in() buffer; NULL if no MKEY */
	u32 image_mkey;
	bool image_mkey_created;
	bool image_dma_mapped;
	struct dma_iova_state image_dma_state;

	/* Parser scratch */
	enum vfmig_load_state state;
	u8  hdr_buf[sizeof(struct vfmig_wire_header)];
	u32 hdr_buf_filled;
	u64 record_size;	/* current record's payload size */
	u32 record_tag;		/* current record's tag */
	u64 record_skipped;	/* bytes consumed-and-discarded for this rec */

	/*
	 * HOST_PAGE replay state. @iova_dom is the destination VF's
	 * deterministic IOVA domain, captured at LOAD-ioctl time from
	 * sriov->vfs_ctx[vf_id].vfmig_iova_dom. NULL iff the user has
	 * NOT issued SET_TRACKED { enable=1 } before LOAD; in that case
	 * any incoming HOST_PAGE record is rejected with -EINVAL by the
	 * parser dispatcher. The pointer's lifetime is governed by the
	 * SET_TRACKED { enable=0 } / sriov_disable / pf_unbind contract:
	 * all three teardown paths require the destination VF to be
	 * unbound, and the LOAD ioctl is itself only useful while the
	 * destination VF is unbound (binding it consumes the staged
	 * blob), so the captured pointer is guaranteed live for the
	 * fd's lifetime.
	 *
	 * Per-record sub-state (only meaningful while parser is in one
	 * of the VFMIG_LS_HP_* states):
	 *   hp_subhdr_buf    -- 16-byte vfmig_host_page_record being read
	 *   hp_subhdr_filled -- bytes accumulated in @hp_subhdr_buf
	 *   hp_iova / hp_len -- parsed from @hp_subhdr_buf at end of
	 *                       HP_READ_SUBHDR
	 *   hp_contents      -- kvmalloc'd payload buffer, sized @hp_len
	 *   hp_filled        -- bytes accumulated in @hp_contents
	 * @hp_contents is freed both on the happy REPLAY -> READ_HEADER
	 * transition and unconditionally on fd close (handles partial
	 * mid-record close).
	 *
	 * @cursor_reset_done is a once-per-fd latch ensuring the LOAD
	 * release path calls vfmig_iova_reset_cursor() exactly once
	 * before the staged FW_DATA blob gets handed off to the next VF
	 * probe. Reset cannot happen earlier (the parser may still emit
	 * more HOST_PAGE records, each of which advances the cursor).
	 */
	struct vfmig_iova_domain *iova_dom;
	u8  hp_subhdr_buf[sizeof(struct vfmig_host_page_record)];
	u32 hp_subhdr_filled;
	u64 hp_iova;
	u64 hp_len;
	void *hp_contents;
	u64 hp_filled;
	bool cursor_reset_done;
};

static void vfmig_load_release_resources(struct mlx5_vfmig_load_ctx *ctx);
static bool vfmig_vf_id_busy_locked(struct mlx5_vfmig_pf *vfmig, u32 vf_id);
static void vfmig_pf_drop_pending_loads_locked(struct mlx5_vfmig_pf *vfmig);
static void vfmig_pf_drop_iova_domains_locked(struct mlx5_vfmig_pf *vfmig);

static void vfmig_pf_release(struct kref *kref)
{
	struct mlx5_vfmig_pf *vfmig =
		container_of(kref, struct mlx5_vfmig_pf, kref);

	WARN_ON(!list_empty(&vfmig->load_ctxs));
	WARN_ON(!list_empty(&vfmig->save_ctxs));
	mutex_destroy(&vfmig->ctxs_lock);
	ida_free(&mlx5_vfmig_minor_ida, vfmig->minor);
	kfree(vfmig);
}

static void vfmig_pf_get(struct mlx5_vfmig_pf *vfmig)
{
	kref_get(&vfmig->kref);
}

static void vfmig_pf_put(struct mlx5_vfmig_pf *vfmig)
{
	kref_put(&vfmig->kref, vfmig_pf_release);
}

/* -------- ioctl handlers ------------------------------------------------- */

/*
 * QUERY_HCA_CAP(other_function=1) - PF-side query of a VF's vhca_id.
 * Mirrors mlx5vf_cmd_get_vhca_id() in drivers/vfio/pci/mlx5/cmd.c.
 *
 * TODO(vfmig-dedup): hoist into mlx5_core proper and let the VFIO variant
 * call it instead of carrying its own copy.
 */
static int vfmig_query_vhca_id(struct mlx5_core_dev *pf_mdev,
			       u16 function_id, u16 *vhca_id)
{
	u32 in[MLX5_ST_SZ_DW(query_hca_cap_in)] = {};
	void *out;
	int out_size;
	int ret;

	out_size = MLX5_ST_SZ_BYTES(query_hca_cap_out);
	out = kzalloc(out_size, GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	MLX5_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	MLX5_SET(query_hca_cap_in, in, other_function, 1);
	MLX5_SET(query_hca_cap_in, in, function_id, function_id);
	MLX5_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE << 1 |
		 HCA_CAP_OPMOD_GET_CUR);

	ret = mlx5_cmd_exec_inout(pf_mdev, query_hca_cap, in, out);
	if (ret)
		goto out;

	*vhca_id = MLX5_GET(query_hca_cap_out, out,
			    capability.cmd_hca_cap.vhca_id);
out:
	kfree(out);
	return ret;
}

/*
 * Gating capabilities for SUSPEND/SAVE/LOAD/RESUME on a VF. The PF mdev
 * itself must report both `migration` and `vhca_resource_manager` --
 * matches what mlx5_devlink_port_fn_migratable_set checks before
 * letting userspace flip the per-VF migratable bit. Returns 0 if
 * supported, -EOPNOTSUPP otherwise (with a one-line warn).
 */
static int vfmig_check_pf_migration_caps(struct mlx5_core_dev *pf_mdev)
{
	if (!MLX5_CAP_GEN(pf_mdev, migration)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: PF firmware does not advertise migration capability\n");
		return -EOPNOTSUPP;
	}
	if (!MLX5_CAP_GEN(pf_mdev, vhca_resource_manager)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: PF firmware does not advertise vhca_resource_manager\n");
		return -EOPNOTSUPP;
	}
	return 0;
}

/*
 * Query whether a VF's HCA_CAP_2.migratable bit is set. The bit is
 * what SUSPEND/SAVE/LOAD/RESUME firmware commands gate on; without it
 * those commands return "bad parameter" (status 0x3).
 *
 * Setting the bit is split out into vfmig_set_vf_migratable() and
 * exposed as the dedicated MLX5_VFMIG_IOC_ENABLE_MIGRATABLE ioctl: the
 * firmware only accepts a modify-cap on a VHCA that has not yet been
 * ENABLE_HCA'd (in legacy eswitch mode). Trying to flip it from inside
 * SAVE/LOAD -- which by construction run on a VF mlx5_core has
 * already probed -- returns "bad resource state". Userspace must do
 * the enable in the pre-bind window.
 *
 * The vport number for VF index @vf_id under standard SR-IOV is
 * @vf_id + 1 (vport 0 is the PF).
 */
static int vfmig_query_vf_migratable(struct mlx5_core_dev *pf_mdev, u32 vf_id,
				     bool *enabled)
{
	int query_sz = MLX5_ST_SZ_BYTES(query_hca_cap_out);
	u16 vport = vf_id + 1;
	void *query_ctx;
	void *hca_caps;
	int err;

	query_ctx = kzalloc(query_sz, GFP_KERNEL);
	if (!query_ctx)
		return -ENOMEM;

	err = mlx5_vport_get_other_func_cap(pf_mdev, vport, query_ctx,
					    MLX5_CAP_GENERAL_2);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: query GENERAL_2 cap for vf %u (vport %u) failed: %d\n",
			       vf_id, vport, err);
		goto out;
	}

	hca_caps = MLX5_ADDR_OF(query_hca_cap_out, query_ctx, capability);
	*enabled = MLX5_GET(cmd_hca_cap_2, hca_caps, migratable);
out:
	kfree(query_ctx);
	return err;
}

/*
 * Pre-bind helper: idempotently set HCA_CAP_2.migratable=1 on @vf_id.
 * Caller is expected to hold off mlx5_core's ENABLE_HCA on this VF
 * (autoprobe=0, no manual bind yet). If the VF is already enabled,
 * the firmware will reject the SET_HCA_CAP with "bad resource state"
 * and we return -EBUSY.
 *
 * We intentionally never clear the bit again: a VF that's been
 * migration-enabled once stays migration-enabled for the lifetime of
 * the SR-IOV provisioning.
 */
static int vfmig_set_vf_migratable(struct mlx5_core_dev *pf_mdev, u32 vf_id)
{
	int query_sz = MLX5_ST_SZ_BYTES(query_hca_cap_out);
	u16 vport = vf_id + 1;
	void *query_ctx;
	void *hca_caps;
	int err;

	query_ctx = kzalloc(query_sz, GFP_KERNEL);
	if (!query_ctx)
		return -ENOMEM;

	err = mlx5_vport_get_other_func_cap(pf_mdev, vport, query_ctx,
					    MLX5_CAP_GENERAL_2);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: query GENERAL_2 cap for vf %u (vport %u) failed: %d\n",
			       vf_id, vport, err);
		goto out;
	}

	hca_caps = MLX5_ADDR_OF(query_hca_cap_out, query_ctx, capability);
	if (MLX5_GET(cmd_hca_cap_2, hca_caps, migratable)) {
		err = 0;
		goto out;
	}

	MLX5_SET(cmd_hca_cap_2, hca_caps, migratable, 1);
	err = mlx5_vport_set_other_func_cap(pf_mdev, hca_caps, vport,
					    MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE2);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: set GENERAL_2.migratable=1 for vf %u (vport %u) failed: %d (VF must be unbound)\n",
			       vf_id, vport, err);
		goto out;
	}
	mlx5_core_info(pf_mdev,
		       "vfmig: enabled migratable cap for vf %u (vport %u)\n",
		       vf_id, vport);
out:
	kfree(query_ctx);
	return err;
}

static long vfmig_ioc_enable_migratable(struct mlx5_vfmig_pf *vfmig,
					void __user *uarg)
{
	struct mlx5_vfmig_enable_migratable arg;
	struct mlx5_core_sriov *sriov;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;

	sriov = &vfmig->pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	err = vfmig_check_pf_migration_caps(vfmig->pf_mdev);
	if (err)
		return err;

	return vfmig_set_vf_migratable(vfmig->pf_mdev, arg.vf_id);
}

/*
 * Look up the pci_dev for VF @vf_id of @pf_pdev. Returns a refcounted
 * pci_dev (caller must pci_dev_put()) or NULL if no such VF currently
 * exists (e.g. sriov_numvfs has been dropped between the num_vfs
 * check and now).
 *
 * We can't use pci_get_domain_bus_and_slot(pci_iov_virtfn_bus(),
 * pci_iov_virtfn_devfn()) because pci_iov_virtfn_bus() is not exported
 * to modules (only the ..._devfn variant is, see drivers/pci/iov.c).
 * Walking the PCI device list and matching on (physfn, pci_iov_vf_id)
 * sidesteps that and is O(num_pci_devs) on a slow ioctl path -- fine.
 */
static struct pci_dev *vfmig_get_vf_pdev(struct pci_dev *pf_pdev, u32 vf_id)
{
	struct pci_dev *iter = NULL;

	for_each_pci_dev(iter) {
		if (iter->is_virtfn &&
		    iter->physfn == pf_pdev &&
		    pci_iov_vf_id(iter) == (int)vf_id)
			return iter;	/* for_each_pci_dev kept the ref */
	}
	return NULL;
}

/*
 * MLX5_VFMIG_IOC_SET_TRACKED handler.
 *
 * Toggles the per-VF @vfmig_tracked flag on the PF's vfs_ctx[] and
 * couples it to creation/destruction of the per-VF deterministic
 * IOVA domain (vfmig_iova.c). The flag and the domain pointer are
 * the two halves of "this VF's address space is owned by vfmig":
 * tracked=1 iff vfmig_iova_dom != NULL. mlx5_vf_is_vfmig_tracked()
 * and future probe-time hooks rely on this invariant.
 *
 * Contract:
 *   - VF must be currently unbound (no driver attached). We take
 *     the VF pci_dev's device_lock to read ->dev.driver atomically
 *     with the domain attach/detach + flag write; that's the same
 *     lock pci_device_probe / remove take, so the flag and any
 *     future probe see consistent ordering.
 *   - On enable=1: allocate an unmanaged paging iommu_domain,
 *     attach it to the VF, stash it on vfs_ctx[].vfmig_iova_dom,
 *     and set the flag. After this point dma_alloc_coherent on
 *     this VF will fail (the dma-iommu-managed default DMA domain
 *     is displaced); only callers routed through
 *     vfmig_iova_alloc_slot() will resolve to a valid IOVA.
 *   - On enable=0: clear the flag, detach + free the domain, NULL
 *     out the pointer. Restores the device's default DMA domain;
 *     subsequent normal mlx5_core probes work as before.
 *
 * Idempotent toggles (flag already in the requested state) are
 * silent no-ops -- they don't take device_lock or log.
 *
 * NOTE on -EBUSY semantics: SET_TRACKED { enable=0 } while a LOAD
 * blob is staged-but-unapplied would invalidate IOVAs the staged
 * blob expects to find at apply time. We don't yet enforce this
 * (Layer 1 will, once HOST_PAGE records actually populate the
 * domain at LOAD time); for now the worst case is the staged blob
 * pointing into a freed domain, which apply_pending_load will
 * detect on the next probe and refuse.
 */
static long vfmig_ioc_set_tracked(struct mlx5_vfmig_pf *vfmig,
				  void __user *uarg)
{
	struct mlx5_vfmig_set_tracked arg;
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_core_sriov *sriov;
	struct mlx5_vf_context *vfs_ctx;
	struct vfmig_iova_domain *new_dom = NULL;
	struct vfmig_iova_domain *old_dom = NULL;
	struct pci_dev *vf_pdev;
	bool desired;
	int err = 0;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.flags || arg.reserved)
		return -EINVAL;
	if (arg.enable > 1)
		return -EINVAL;

	sriov = &pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	vfs_ctx = &sriov->vfs_ctx[arg.vf_id];
	desired = (arg.enable == 1);

	if (!!vfs_ctx->vfmig_tracked == desired) {
		mlx5_core_dbg(pf_mdev,
			      "vfmig: SET_TRACKED vf %u: already %d, no-op\n",
			      arg.vf_id, desired);
		return 0;
	}

	vf_pdev = vfmig_get_vf_pdev(pf_mdev->pdev, arg.vf_id);
	if (!vf_pdev) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: SET_TRACKED vf %u: VF pci_dev lookup failed\n",
			       arg.vf_id);
		return -ENODEV;
	}

	device_lock(&vf_pdev->dev);
	if (vf_pdev->dev.driver) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: SET_TRACKED vf %u rejected: VF is bound to %s (must be unbound first)\n",
			       arg.vf_id, vf_pdev->dev.driver->name);
		err = -EBUSY;
		goto out_unlock;
	}

	if (desired) {
		/*
		 * Belt-and-suspenders: an existing domain pointer with
		 * tracked=0 is a state-machine bug. Detect, log, free
		 * it before allocating a new one rather than leaking.
		 */
		if (WARN_ON_ONCE(vfs_ctx->vfmig_iova_dom)) {
			old_dom = vfs_ctx->vfmig_iova_dom;
			vfs_ctx->vfmig_iova_dom = NULL;
		}
		err = vfmig_iova_domain_create(vf_pdev, arg.vf_id, &new_dom);
		if (err) {
			mlx5_core_warn(pf_mdev,
				       "vfmig: SET_TRACKED vf %u: iova_domain_create failed: %d\n",
				       arg.vf_id, err);
			goto out_unlock;
		}
		vfs_ctx->vfmig_iova_dom = new_dom;
		vfs_ctx->vfmig_tracked = 1;
		mlx5_core_info(pf_mdev,
			       "vfmig: vf %u tracked=1, iova domain attached\n",
			       arg.vf_id);
	} else {
		old_dom = vfs_ctx->vfmig_iova_dom;
		vfs_ctx->vfmig_iova_dom = NULL;
		vfs_ctx->vfmig_tracked = 0;
		mlx5_core_info(pf_mdev,
			       "vfmig: vf %u tracked=0, iova domain detaching\n",
			       arg.vf_id);
	}

out_unlock:
	device_unlock(&vf_pdev->dev);
	pci_dev_put(vf_pdev);

	/*
	 * Free the old domain *outside* device_lock: domain_destroy
	 * walks the iommu_domain's mapping tree, frees pages, and
	 * detaches via iommu_detach_device which takes iommu group
	 * locks. None of that benefits from holding device_lock
	 * here; not holding it also avoids any subtle iommu-group
	 * vs device-lock ordering hazards in iommu drivers.
	 */
	if (old_dom)
		vfmig_iova_domain_destroy(old_dom);
	return err;
}

static long vfmig_ioc_mark_restored(struct mlx5_vfmig_pf *vfmig,
				    void __user *uarg)
{
	struct mlx5_vfmig_mark_restored arg;
	struct mlx5_core_sriov *sriov;
	u16 vhca_id;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;

	sriov = &vfmig->pf_mdev->priv.sriov;

	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	/*
	 * The LOAD ioctl's close() now auto-installs restored=1 (and a
	 * pending_load slot) for the M2 happy path. Calling MARK_RESTORED
	 * after LOAD is therefore a no-op rather than an error -- this
	 * keeps the M1'-only test path (MARK_RESTORED without LOAD)
	 * working unchanged while not punishing M2 callers that still
	 * issue the explicit MARK_RESTORED for symmetry.
	 */
	if (sriov->vfs_ctx[arg.vf_id].restored)
		return 0;

	err = vfmig_query_vhca_id(vfmig->pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	sriov->vfs_ctx[arg.vf_id].restored_vhca_id = vhca_id;
	sriov->vfs_ctx[arg.vf_id].restored = 1;
	mlx5_core_info(vfmig->pf_mdev,
		       "vfmig: marked VF %u (vhca_id 0x%04x) as restored (next probe will skip INIT_HCA)\n",
		       arg.vf_id, vhca_id);
	return 0;
}

static long vfmig_ioc_get_vhca_id(struct mlx5_vfmig_pf *vfmig,
				  void __user *uarg)
{
	struct mlx5_vfmig_get_vhca_id arg;
	struct mlx5_core_sriov *sriov;
	u16 vhca_id;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;

	sriov = &vfmig->pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	err = vfmig_query_vhca_id(vfmig->pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	arg.vhca_id = vhca_id;
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		return -EFAULT;
	return 0;
}

static long vfmig_ioc_query_vf(struct mlx5_vfmig_pf *vfmig,
			       void __user *uarg)
{
	struct mlx5_vfmig_query_vf arg;
	struct mlx5_core_sriov *sriov;
	u16 vhca_id;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;

	sriov = &vfmig->pf_mdev->priv.sriov;

	arg.num_vfs = sriov->num_vfs;
	if (arg.vf_id >= sriov->num_vfs) {
		arg.vhca_id = 0;
		arg.restored = 0;
		if (copy_to_user(uarg, &arg, sizeof(arg)))
			return -EFAULT;
		return -ERANGE;
	}

	err = vfmig_query_vhca_id(vfmig->pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	arg.vhca_id = vhca_id;
	arg.restored = sriov->vfs_ctx[arg.vf_id].restored;
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		return -EFAULT;
	return 0;
}

/* -------- LOAD_VHCA_STATE: helpers cloned from VFIO mlx5 variant -------- */

/*
 * The four helpers below (alloc_mkey_in, create_mkey, register_dma_pages,
 * unregister_dma_pages) are line-for-line copies of the static helpers in
 * drivers/vfio/pci/mlx5/cmd.c (alloc_mkey_in @316, create_mkey @348,
 * register_dma_pages @378, unregister_dma_pages @357). We duplicate them
 * here so M2 doesn't need to touch the VFIO variant or invent an export
 * boundary; a future patch should hoist them into mlx5_core proper.
 *
 * TODO(vfmig-dedup): collapse with drivers/vfio/pci/mlx5/cmd.c counterparts.
 */
static u32 *vfmig_alloc_mkey_in(u32 npages, u32 pdn)
{
	int inlen;
	void *mkc;
	u32 *in;

	inlen = MLX5_ST_SZ_BYTES(create_mkey_in) +
		sizeof(__be64) * round_up(npages, 2);

	in = kvzalloc(inlen, GFP_KERNEL_ACCOUNT);
	if (!in)
		return NULL;

	MLX5_SET(create_mkey_in, in, translations_octword_actual_size,
		 DIV_ROUND_UP(npages, 2));

	mkc = MLX5_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);
	MLX5_SET(mkc, mkc, access_mode_1_0, MLX5_MKC_ACCESS_MODE_MTT);
	MLX5_SET(mkc, mkc, lr, 1);
	MLX5_SET(mkc, mkc, lw, 1);
	MLX5_SET(mkc, mkc, rr, 1);
	MLX5_SET(mkc, mkc, rw, 1);
	MLX5_SET(mkc, mkc, pd, pdn);
	MLX5_SET(mkc, mkc, bsf_octword_size, 0);
	MLX5_SET(mkc, mkc, qpn, 0xffffff);
	MLX5_SET(mkc, mkc, log_page_size, PAGE_SHIFT);
	MLX5_SET(mkc, mkc, translations_octword_size, DIV_ROUND_UP(npages, 2));
	MLX5_SET64(mkc, mkc, len, npages * PAGE_SIZE);

	return in;
}

static int vfmig_create_mkey(struct mlx5_core_dev *mdev, u32 npages,
			     u32 *mkey_in, u32 *mkey)
{
	int inlen = MLX5_ST_SZ_BYTES(create_mkey_in) +
		sizeof(__be64) * round_up(npages, 2);

	return mlx5_core_create_mkey(mdev, mkey, mkey_in, inlen);
}

static void vfmig_unregister_dma_pages(struct mlx5_core_dev *mdev, u32 npages,
				       u32 *mkey_in,
				       struct dma_iova_state *state,
				       enum dma_data_direction dir)
{
	dma_addr_t addr;
	__be64 *mtt;
	int i;

	if (dma_use_iova(state)) {
		dma_iova_destroy(mdev->device, state, npages * PAGE_SIZE, dir,
				 0);
	} else {
		mtt = (__be64 *)MLX5_ADDR_OF(create_mkey_in, mkey_in,
					     klm_pas_mtt);
		for (i = npages - 1; i >= 0; i--) {
			addr = be64_to_cpu(mtt[i]);
			dma_unmap_page(mdev->device, addr, PAGE_SIZE, dir);
		}
	}
}

static int vfmig_register_dma_pages(struct mlx5_core_dev *mdev, u32 npages,
				    struct page **page_list, u32 *mkey_in,
				    struct dma_iova_state *state,
				    enum dma_data_direction dir)
{
	dma_addr_t addr;
	size_t mapped = 0;
	__be64 *mtt;
	int i, err;

	mtt = (__be64 *)MLX5_ADDR_OF(create_mkey_in, mkey_in, klm_pas_mtt);

	if (dma_iova_try_alloc(mdev->device, state, 0, npages * PAGE_SIZE)) {
		addr = state->addr;
		for (i = 0; i < npages; i++) {
			err = dma_iova_link(mdev->device, state,
					    page_to_phys(page_list[i]), mapped,
					    PAGE_SIZE, dir, 0);
			if (err)
				goto error;
			*mtt++ = cpu_to_be64(addr);
			addr += PAGE_SIZE;
			mapped += PAGE_SIZE;
		}
		err = dma_iova_sync(mdev->device, state, 0, mapped);
		if (err)
			goto error;
	} else {
		for (i = 0; i < npages; i++) {
			addr = dma_map_page(mdev->device, page_list[i], 0,
					    PAGE_SIZE, dir);
			err = dma_mapping_error(mdev->device, addr);
			if (err)
				goto error;
			*mtt++ = cpu_to_be64(addr);
		}
	}
	return 0;

error:
	vfmig_unregister_dma_pages(mdev, i, mkey_in, state, dir);
	return err;
}

/*
 * Bulk page allocator equivalent to drivers/vfio/pci/mlx5/cmd.c's
 * mlx5vf_add_pages(). Out param @page_list is kvcalloc()'d.
 *
 * TODO(vfmig-dedup): see above.
 */
static int vfmig_alloc_pages(struct page ***page_list, unsigned int npages)
{
	unsigned int filled, done = 0;
	int i;

	*page_list = kvcalloc(npages, sizeof(struct page *),
			      GFP_KERNEL_ACCOUNT);
	if (!*page_list)
		return -ENOMEM;

	for (;;) {
		filled = alloc_pages_bulk(GFP_KERNEL_ACCOUNT, npages - done,
					  *page_list + done);
		if (!filled)
			goto err;

		done += filled;
		if (done == npages)
			break;
	}

	return 0;
err:
	for (i = 0; i < done; i++)
		__free_page((*page_list)[i]);

	kvfree(*page_list);
	*page_list = NULL;
	return -ENOMEM;
}

static void vfmig_free_pages(struct page **page_list, u32 npages)
{
	int i;

	if (!page_list)
		return;

	for (i = npages - 1; i >= 0; i--)
		__free_page(page_list[i]);

	kvfree(page_list);
}

/*
 * LOAD_VHCA_STATE firmware command. Slimmer than the VFIO variant
 * (cmd.c:832 mlx5vf_cmd_load_vhca_state) because we don't carry
 * mvdev/migf indirection -- the caller hands us the PF mdev and the
 * vhca_id directly.
 *
 * TODO(vfmig-dedup): same as helpers above.
 */
static int vfmig_cmd_load_vhca_state(struct mlx5_core_dev *pf_mdev,
				     u16 vhca_id, u32 mkey, size_t size)
{
	u32 out[MLX5_ST_SZ_DW(load_vhca_state_out)] = {};
	u32 in[MLX5_ST_SZ_DW(load_vhca_state_in)] = {};

	MLX5_SET(load_vhca_state_in, in, opcode, MLX5_CMD_OP_LOAD_VHCA_STATE);
	MLX5_SET(load_vhca_state_in, in, op_mod, 0);
	MLX5_SET(load_vhca_state_in, in, vhca_id, vhca_id);
	MLX5_SET(load_vhca_state_in, in, mkey, mkey);
	MLX5_SET(load_vhca_state_in, in, size, size);

	return mlx5_cmd_exec_inout(pf_mdev, load_vhca_state, in, out);
}

/* -------- SAVE_VHCA_STATE: helpers cloned from VFIO mlx5 variant -------- */

/*
 * Synchronous slices of mlx5vf_cmd_{suspend,resume}_vhca,
 * mlx5vf_cmd_query_vhca_migration_state, and mlx5vf_cmd_save_vhca_state.
 * The originals carry mvdev / state_mutex / mig_file / async-completion
 * indirection that we don't need: our SAVE/LOAD ioctls run synchronously
 * under vfmig->lock with no PRE_COPY, no incremental, no chunk_mode, and
 * no work-queue completion. Each helper takes pf_mdev + vhca_id directly.
 *
 * TODO(vfmig-dedup): once the dedup patch lifts these into mlx5_core
 * proper, the VFIO variant should call the shared versions.
 */
static int vfmig_cmd_suspend_vhca(struct mlx5_core_dev *pf_mdev, u16 vhca_id,
				  u16 op_mod)
{
	u32 out[MLX5_ST_SZ_DW(suspend_vhca_out)] = {};
	u32 in[MLX5_ST_SZ_DW(suspend_vhca_in)] = {};

	MLX5_SET(suspend_vhca_in, in, opcode, MLX5_CMD_OP_SUSPEND_VHCA);
	MLX5_SET(suspend_vhca_in, in, vhca_id, vhca_id);
	MLX5_SET(suspend_vhca_in, in, op_mod, op_mod);

	return mlx5_cmd_exec_inout(pf_mdev, suspend_vhca, in, out);
}

static int vfmig_cmd_resume_vhca(struct mlx5_core_dev *pf_mdev, u16 vhca_id,
				 u16 op_mod)
{
	u32 out[MLX5_ST_SZ_DW(resume_vhca_out)] = {};
	u32 in[MLX5_ST_SZ_DW(resume_vhca_in)] = {};

	MLX5_SET(resume_vhca_in, in, opcode, MLX5_CMD_OP_RESUME_VHCA);
	MLX5_SET(resume_vhca_in, in, vhca_id, vhca_id);
	MLX5_SET(resume_vhca_in, in, op_mod, op_mod);

	return mlx5_cmd_exec_inout(pf_mdev, resume_vhca, in, out);
}

/*
 * Stop-the-world slice of mlx5vf_cmd_query_vhca_migration_state: no
 * incremental queries, no chunk_mode (we do single-shot SAVE), no
 * PRE_COPY error handling. *@size_out gets required_umem_size in bytes.
 */
static int vfmig_cmd_query_vhca_migration_state(struct mlx5_core_dev *pf_mdev,
						u16 vhca_id, u64 *size_out)
{
	u32 out[MLX5_ST_SZ_DW(query_vhca_migration_state_out)] = {};
	u32 in[MLX5_ST_SZ_DW(query_vhca_migration_state_in)] = {};
	int err;

	MLX5_SET(query_vhca_migration_state_in, in, opcode,
		 MLX5_CMD_OP_QUERY_VHCA_MIGRATION_STATE);
	MLX5_SET(query_vhca_migration_state_in, in, vhca_id, vhca_id);
	MLX5_SET(query_vhca_migration_state_in, in, op_mod, 0);
	MLX5_SET(query_vhca_migration_state_in, in, incremental, 0);
	MLX5_SET(query_vhca_migration_state_in, in, chunk, 0);

	err = mlx5_cmd_exec_inout(pf_mdev, query_vhca_migration_state, in, out);
	if (err)
		return err;

	*size_out = MLX5_GET(query_vhca_migration_state_out, out,
			     required_umem_size);
	return 0;
}

/*
 * Synchronous SAVE_VHCA_STATE. Mirrors the relevant slice of
 * mlx5vf_cmd_save_vhca_state but uses mlx5_cmd_exec_inout instead of the
 * async cb path -- we have no chunked-output / track / pre-copy use case.
 *
 * @size is the buffer capacity we are offering to the firmware (bytes).
 * On success *@actual_size_out is set to the bytes the firmware actually
 * wrote (<= @size); use that value for the on-wire record_size.
 */
static int vfmig_cmd_save_vhca_state(struct mlx5_core_dev *pf_mdev, u16 vhca_id,
				     u32 mkey, size_t size,
				     u64 *actual_size_out)
{
	u32 out[MLX5_ST_SZ_DW(save_vhca_state_out)] = {};
	u32 in[MLX5_ST_SZ_DW(save_vhca_state_in)] = {};
	int err;

	MLX5_SET(save_vhca_state_in, in, opcode, MLX5_CMD_OP_SAVE_VHCA_STATE);
	MLX5_SET(save_vhca_state_in, in, op_mod, 0);
	MLX5_SET(save_vhca_state_in, in, vhca_id, vhca_id);
	MLX5_SET(save_vhca_state_in, in, mkey, mkey);
	MLX5_SET(save_vhca_state_in, in, size, size);
	MLX5_SET(save_vhca_state_in, in, incremental, 0);
	MLX5_SET(save_vhca_state_in, in, set_track, 0);

	err = mlx5_cmd_exec_inout(pf_mdev, save_vhca_state, in, out);
	if (err)
		return err;

	*actual_size_out = MLX5_GET(save_vhca_state_out, out,
				    actual_image_size);
	return 0;
}

/* -------- LOAD_VHCA_STATE: per-fd image-buffer plumbing ----------------- */

/*
 * Tear down whatever DMA state is currently held on @ctx's image buffer.
 * Caller must hold vfmig->lock for read AND ctx->vfmig->pf_mdev must be
 * non-NULL (i.e. !vfmig->dead).
 */
static void vfmig_load_drop_image_dma(struct mlx5_vfmig_load_ctx *ctx,
				      struct mlx5_core_dev *pf_mdev)
{
	if (ctx->image_mkey_created) {
		mlx5_core_destroy_mkey(pf_mdev, ctx->image_mkey);
		ctx->image_mkey_created = false;
	}
	if (ctx->image_dma_mapped) {
		vfmig_unregister_dma_pages(pf_mdev, ctx->image_npages,
					   ctx->image_mkey_in,
					   &ctx->image_dma_state,
					   DMA_TO_DEVICE);
		ctx->image_dma_mapped = false;
	}
	kvfree(ctx->image_mkey_in);
	ctx->image_mkey_in = NULL;
}

/*
 * Ensure ctx has an image buffer of at least @want_npages, with a fresh
 * MKEY suitable for handing to LOAD_VHCA_STATE. If the existing buffer
 * is large enough we just rebuild the MKEY-in scaffolding.
 *
 * Caller holds vfmig->lock for read.
 */
static int vfmig_load_prepare_image(struct mlx5_vfmig_load_ctx *ctx,
				    u32 want_npages)
{
	struct mlx5_core_dev *pf_mdev = ctx->vfmig->pf_mdev;
	int err;

	if (WARN_ON(!pf_mdev))
		return -ENODEV;

	vfmig_load_drop_image_dma(ctx, pf_mdev);

	if (ctx->image_npages < want_npages) {
		vfmig_free_pages(ctx->image_pages, ctx->image_npages);
		ctx->image_pages = NULL;
		ctx->image_npages = 0;

		err = vfmig_alloc_pages(&ctx->image_pages, want_npages);
		if (err)
			return err;
		ctx->image_npages = want_npages;
	}

	ctx->image_mkey_in = vfmig_alloc_mkey_in(want_npages, ctx->pdn);
	if (!ctx->image_mkey_in)
		return -ENOMEM;

	err = vfmig_register_dma_pages(pf_mdev, want_npages, ctx->image_pages,
				       ctx->image_mkey_in,
				       &ctx->image_dma_state, DMA_TO_DEVICE);
	if (err)
		goto err_register;
	ctx->image_dma_mapped = true;

	err = vfmig_create_mkey(pf_mdev, want_npages, ctx->image_mkey_in,
				&ctx->image_mkey);
	if (err)
		goto err_mkey;
	ctx->image_mkey_created = true;
	return 0;

err_mkey:
	vfmig_unregister_dma_pages(pf_mdev, want_npages, ctx->image_mkey_in,
				   &ctx->image_dma_state, DMA_TO_DEVICE);
	ctx->image_dma_mapped = false;
err_register:
	kvfree(ctx->image_mkey_in);
	ctx->image_mkey_in = NULL;
	return err;
}

/* -------- LOAD_VHCA_STATE: parser FSM ----------------------------------- */

static ssize_t vfmig_load_consume_image(struct mlx5_vfmig_load_ctx *ctx,
					const char __user *ubuf, size_t want)
{
	size_t copied = 0;

	while (want) {
		size_t page_off = ctx->image_filled & (PAGE_SIZE - 1);
		u32 page_idx = ctx->image_filled >> PAGE_SHIFT;
		size_t chunk = min_t(size_t, want, PAGE_SIZE - page_off);
		u8 *to;

		if (page_idx >= ctx->image_npages)
			return -EINVAL;

		to = kmap_local_page(ctx->image_pages[page_idx]);
		if (copy_from_user(to + page_off, ubuf, chunk)) {
			kunmap_local(to);
			return copied ? (ssize_t)copied : -EFAULT;
		}
		kunmap_local(to);

		ctx->image_filled += chunk;
		ubuf += chunk;
		want -= chunk;
		copied += chunk;
	}
	return copied;
}

static ssize_t vfmig_load_consume_header(struct mlx5_vfmig_load_ctx *ctx,
					 const char __user *ubuf, size_t want)
{
	size_t need = sizeof(ctx->hdr_buf) - ctx->hdr_buf_filled;
	size_t take = min(need, want);

	if (!take)
		return 0;
	if (copy_from_user(ctx->hdr_buf + ctx->hdr_buf_filled, ubuf, take))
		return -EFAULT;
	ctx->hdr_buf_filled += take;
	return take;
}

/*
 * Skip @want bytes of an unknown-but-optional record's payload. We
 * intentionally do not stage them anywhere -- the VFIO variant stages
 * STOP_COPY_SIZE into a small buffer to then size-up the next image
 * proactively, but for our prototype we just discard and let
 * PREP_IMAGE realloc pick the right size on demand.
 */
static ssize_t vfmig_load_skip_record(struct mlx5_vfmig_load_ctx *ctx,
				      const char __user *ubuf, size_t want)
{
	u8 sink[64];
	size_t total = 0;

	while (want && ctx->record_skipped < ctx->record_size) {
		size_t left = ctx->record_size - ctx->record_skipped;
		size_t chunk = min3(want, left, sizeof(sink));

		if (copy_from_user(sink, ubuf, chunk))
			return total ? (ssize_t)total : -EFAULT;
		ctx->record_skipped += chunk;
		ubuf += chunk;
		want -= chunk;
		total += chunk;
	}
	return total;
}

static int vfmig_load_dispatch_header(struct mlx5_vfmig_load_ctx *ctx)
{
	struct vfmig_wire_header *hdr =
		(struct vfmig_wire_header *)ctx->hdr_buf;
	u64 record_size = le64_to_cpu(hdr->record_size);
	u32 flags = le32_to_cpu(hdr->flags);
	u32 tag = le32_to_cpu(hdr->tag);

	if (record_size > VFMIG_MAX_LOAD_SIZE)
		return -EINVAL;

	ctx->record_size = record_size;
	ctx->record_tag = tag;
	ctx->record_skipped = 0;
	ctx->image_filled = 0;
	ctx->hdr_buf_filled = 0;

	switch (tag) {
	case VFMIG_WIRE_TAG_FW_DATA:
		ctx->state = VFMIG_LS_PREP_IMAGE;
		return 0;
	case VFMIG_WIRE_TAG_HOST_PAGE:
		/*
		 * HOST_PAGE replays into the per-VF IOVA domain. If the
		 * destination wasn't SET_TRACKED'd, there's nowhere to
		 * replay to -- this is a userspace ordering bug (the
		 * paired source must have been tracked, so the LOAD blob
		 * carries IOVA payload, but the destination is bare DMA),
		 * not something we can paper over silently.
		 */
		if (!ctx->iova_dom) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: HOST_PAGE record in blob but destination not SET_TRACKED'd; aborting LOAD\n",
				       ctx->vf_id);
			return -EINVAL;
		}
		if (record_size < sizeof(struct vfmig_host_page_record))
			return -EINVAL;
		ctx->hp_subhdr_filled = 0;
		ctx->hp_filled = 0;
		ctx->state = VFMIG_LS_HP_READ_SUBHDR;
		return 0;
	default:
		if (!(flags & VFMIG_WIRE_FLAGS_TAG_OPTIONAL))
			return -EOPNOTSUPP;
		ctx->state = VFMIG_LS_READ_HEADER_DATA;
		return 0;
	}
}

/*
 * "Stage" the freshly-parsed FW_DATA record. Does NOT issue the
 * LOAD_VHCA_STATE firmware command -- the destination VHCA is not yet
 * ENABLE_HCA'd at LOAD-ioctl time and the command would be a silent
 * no-op. Instead, mark image_staged so release() transfers the staged
 * pages/MKEY/PD into the per-VF pending_load slot, where the VF's
 * mlx5_function_open() will pick them up after ENABLE_HCA and call
 * LOAD_VHCA_STATE itself.
 *
 * Multiple FW_DATA records per session are intentionally not
 * supported: SAVE produces exactly one and the staged buffer model
 * doesn't multiplex. Reject the second one with -EINVAL.
 */
static int vfmig_load_run_load(struct mlx5_vfmig_load_ctx *ctx)
{
	struct mlx5_core_dev *pf_mdev = ctx->vfmig->pf_mdev;

	if (WARN_ON(!ctx->image_mkey_created))
		return -EINVAL;
	if (ctx->image_staged) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: vf %u (vhca_id 0x%04x): multiple FW_DATA records per LOAD session not supported\n",
			       ctx->vf_id, ctx->vhca_id);
		return -EINVAL;
	}

	ctx->image_staged = true;
	mlx5_core_dbg(pf_mdev,
		      "vfmig: staged %llu bytes of state for vf %u (vhca_id 0x%04x); LOAD_VHCA_STATE will run on probe\n",
		      (unsigned long long)ctx->record_size, ctx->vf_id,
		      ctx->vhca_id);
	return 0;
}

static int vfmig_load_step(struct mlx5_vfmig_load_ctx *ctx,
			   const char __user **ubuf, size_t *left,
			   bool *progressed)
{
	ssize_t n;
	int err;
	u32 want_npages;

	switch (ctx->state) {
	case VFMIG_LS_READ_HEADER:
		n = vfmig_load_consume_header(ctx, *ubuf, *left);
		if (n < 0)
			return n;
		*ubuf += n;
		*left -= n;
		*progressed = n > 0;
		if (ctx->hdr_buf_filled == sizeof(ctx->hdr_buf)) {
			err = vfmig_load_dispatch_header(ctx);
			if (err)
				return err;
		}
		return 0;

	case VFMIG_LS_PREP_IMAGE:
		want_npages = max_t(u32, 1,
				    DIV_ROUND_UP(ctx->record_size, PAGE_SIZE));
		err = vfmig_load_prepare_image(ctx, want_npages);
		if (err)
			return err;
		ctx->state = VFMIG_LS_READ_IMAGE;
		*progressed = true;
		return 0;

	case VFMIG_LS_READ_IMAGE: {
		size_t want = min_t(size_t, *left,
				    ctx->record_size - ctx->image_filled);

		if (!want) {
			if (ctx->image_filled == ctx->record_size)
				ctx->state = VFMIG_LS_LOAD_IMAGE;
			else
				*progressed = false;
			return 0;
		}
		n = vfmig_load_consume_image(ctx, *ubuf, want);
		if (n < 0)
			return n;
		*ubuf += n;
		*left -= n;
		*progressed = n > 0;
		if (ctx->image_filled == ctx->record_size)
			ctx->state = VFMIG_LS_LOAD_IMAGE;
		return 0;
	}

	case VFMIG_LS_LOAD_IMAGE:
		err = vfmig_load_run_load(ctx);
		if (err)
			return err;
		ctx->state = VFMIG_LS_READ_HEADER;
		*progressed = true;
		return 0;

	case VFMIG_LS_READ_HEADER_DATA:
		n = vfmig_load_skip_record(ctx, *ubuf, *left);
		if (n < 0)
			return n;
		*ubuf += n;
		*left -= n;
		*progressed = n > 0;
		if (ctx->record_skipped == ctx->record_size)
			ctx->state = VFMIG_LS_READ_HEADER;
		return 0;

	case VFMIG_LS_HP_READ_SUBHDR: {
		/*
		 * Pull the 16-byte vfmig_host_page_record out of the
		 * stream into ctx->hp_subhdr_buf, then parse iova/len
		 * and kvmalloc a payload-sized buffer for HP_READ_DATA
		 * to fill. The payload size MUST match what the record
		 * header advertised: record_size = 16 + len.
		 */
		size_t need = sizeof(ctx->hp_subhdr_buf) - ctx->hp_subhdr_filled;
		size_t take = min(need, *left);
		struct vfmig_host_page_record subhdr;
		u64 declared_payload;

		if (take) {
			if (copy_from_user(ctx->hp_subhdr_buf + ctx->hp_subhdr_filled,
					   *ubuf, take))
				return -EFAULT;
			ctx->hp_subhdr_filled += take;
			*ubuf += take;
			*left -= take;
			*progressed = true;
		}
		if (ctx->hp_subhdr_filled < sizeof(ctx->hp_subhdr_buf))
			return 0;

		memcpy(&subhdr, ctx->hp_subhdr_buf, sizeof(subhdr));
		ctx->hp_iova = le64_to_cpu(subhdr.iova);
		ctx->hp_len  = le64_to_cpu(subhdr.len);

		declared_payload = ctx->record_size -
				   sizeof(struct vfmig_host_page_record);
		if (ctx->hp_len != declared_payload ||
		    ctx->hp_len == 0 ||
		    ctx->hp_len > VFMIG_HOST_PAGE_MAX_LEN ||
		    !IS_ALIGNED(ctx->hp_len, VFMIG_IOVA_GRANULE) ||
		    !IS_ALIGNED(ctx->hp_iova, VFMIG_IOVA_GRANULE)) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: malformed HOST_PAGE record iova=0x%llx len=%llu (rec=%llu, max=%llu)\n",
				       ctx->vf_id,
				       (unsigned long long)ctx->hp_iova,
				       (unsigned long long)ctx->hp_len,
				       (unsigned long long)ctx->record_size,
				       (unsigned long long)VFMIG_HOST_PAGE_MAX_LEN);
			return -EINVAL;
		}

		ctx->hp_contents = kvmalloc(ctx->hp_len, GFP_KERNEL);
		if (!ctx->hp_contents)
			return -ENOMEM;
		ctx->hp_filled = 0;
		ctx->state = VFMIG_LS_HP_READ_DATA;
		*progressed = true;
		return 0;
	}

	case VFMIG_LS_HP_READ_DATA: {
		size_t need = ctx->hp_len - ctx->hp_filled;
		size_t take = min(need, *left);

		if (!take) {
			if (ctx->hp_filled == ctx->hp_len)
				ctx->state = VFMIG_LS_HP_REPLAY;
			else
				*progressed = false;
			return 0;
		}
		if (copy_from_user((u8 *)ctx->hp_contents + ctx->hp_filled,
				   *ubuf, take))
			return -EFAULT;
		ctx->hp_filled += take;
		*ubuf += take;
		*left -= take;
		*progressed = true;
		if (ctx->hp_filled == ctx->hp_len)
			ctx->state = VFMIG_LS_HP_REPLAY;
		return 0;
	}

	case VFMIG_LS_HP_REPLAY:
		err = vfmig_iova_replay_page(ctx->iova_dom, ctx->hp_iova,
					     ctx->hp_contents, ctx->hp_len);
		kvfree(ctx->hp_contents);
		ctx->hp_contents = NULL;
		if (err) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: replay_page(iova=0x%llx, len=%llu) failed: %d\n",
				       ctx->vf_id,
				       (unsigned long long)ctx->hp_iova,
				       (unsigned long long)ctx->hp_len, err);
			return err;
		}
		mlx5_core_dbg(ctx->vfmig->pf_mdev,
			      "vfmig: vf %u: replayed HOST_PAGE iova=0x%llx len=%llu\n",
			      ctx->vf_id,
			      (unsigned long long)ctx->hp_iova,
			      (unsigned long long)ctx->hp_len);
		ctx->state = VFMIG_LS_READ_HEADER;
		*progressed = true;
		return 0;
	}
	return -EINVAL;
}

/* -------- LOAD_VHCA_STATE: file ops ------------------------------------- */

static ssize_t vfmig_load_write(struct file *filp, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct mlx5_vfmig_load_ctx *ctx = filp->private_data;
	struct mlx5_vfmig_pf *vfmig = ctx->vfmig;
	const char __user *cursor = ubuf;
	size_t left = count;
	ssize_t produced;
	bool progressed;
	int err = 0;

	if (!count)
		return 0;

	mutex_lock(&ctx->io_lock);
	down_read(&vfmig->lock);
	if (vfmig->dead || ctx->resources_freed) {
		err = -ENODEV;
		goto out;
	}

	/*
	 * Drive the FSM until it stops making progress, NOT until @left
	 * reaches zero: certain transitions (PREP_IMAGE -> READ_IMAGE,
	 * READ_IMAGE -> LOAD_IMAGE, LOAD_IMAGE -> READ_HEADER) consume
	 * zero bytes while still being mandatory work. With the old
	 * `while (left)` guard we exited the moment the last payload byte
	 * was consumed, which left the FSM parked in LOAD_IMAGE and meant
	 * vfmig_load_run_load() never fired -- a silent
	 * "LOAD-staged-but-never-issued" bug. By construction, every
	 * zero-byte progressing transition lands the FSM in a state that
	 * needs input on the very next step, so this loop is bounded.
	 */
	for (;;) {
		progressed = false;
		err = vfmig_load_step(ctx, &cursor, &left, &progressed);
		if (err)
			break;
		if (!progressed)
			break;
	}

out:
	up_read(&vfmig->lock);
	mutex_unlock(&ctx->io_lock);

	produced = (ssize_t)(count - left);
	if (!produced && err)
		return err;
	/*
	 * stream_open() set FMODE_STREAM, so ksys_write() passes ppos==NULL.
	 * Don't touch it.
	 */
	return produced;
}

/*
 * Free a per-VF pending_load slot's firmware-tied resources, plus the
 * pages (which are mdev-independent). @pf_mdev MUST be alive (FW
 * commands need to work) -- callers guarantee this via the vfmig
 * kref / vfmig->lock as appropriate.
 */
static void vfmig_vf_load_destroy(struct mlx5_core_dev *pf_mdev,
				  struct mlx5_vfmig_vf_load *load)
{
	if (!load)
		return;

	if (load->mkey_created) {
		mlx5_core_destroy_mkey(pf_mdev, load->mkey);
		load->mkey_created = false;
	}
	if (load->dma_mapped) {
		vfmig_unregister_dma_pages(pf_mdev, load->npages,
					   load->mkey_in,
					   &load->dma_state, DMA_TO_DEVICE);
		load->dma_mapped = false;
	}
	kvfree(load->mkey_in);
	if (load->pd_allocated)
		mlx5_core_dealloc_pd(pf_mdev, load->pdn);
	vfmig_free_pages(load->pages, load->npages);
	kfree(load);
}

/*
 * Install @load into the per-VF slot at vfs_ctx[load->vf_id]. Also
 * sets the restored bit + restored_vhca_id so the next probe both
 * skips INIT_HCA and runs LOAD_VHCA_STATE. Returns 0 on success or
 * -EBUSY if a slot is already staged for this VF (caller must free
 * @load itself in that case).
 *
 * Caller holds vfmig->lock (read suffices) AND vfmig->ctxs_lock.
 */
static int vfmig_install_pending_load_locked(struct mlx5_vfmig_pf *vfmig,
					     struct mlx5_vfmig_vf_load *load)
{
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_core_sriov *sriov;

	if (WARN_ON(!pf_mdev))
		return -ENODEV;
	sriov = &pf_mdev->priv.sriov;
	if (load->vf_id >= sriov->num_vfs)
		return -EINVAL;
	if (sriov->vfs_ctx[load->vf_id].vfmig_pending_load)
		return -EBUSY;

	sriov->vfs_ctx[load->vf_id].vfmig_pending_load = load;
	sriov->vfs_ctx[load->vf_id].restored_vhca_id = load->vhca_id;
	sriov->vfs_ctx[load->vf_id].restored = 1;
	return 0;
}

/*
 * Tear down firmware-tied resources held by @ctx. On the happy path
 * (a complete blob was staged), transfer those resources into the PF's
 * per-VF pending_load slot for the next probe to consume rather than
 * freeing them. Caller MUST hold vfmig->lock so pf_mdev doesn't
 * disappear under us. Idempotent via resources_freed.
 */
static void vfmig_load_release_resources(struct mlx5_vfmig_load_ctx *ctx)
{
	struct mlx5_core_dev *pf_mdev = ctx->vfmig->pf_mdev;
	struct mlx5_vfmig_vf_load *load;
	int err;

	if (ctx->resources_freed)
		return;
	ctx->resources_freed = true;

	if (!pf_mdev)
		return;

	/*
	 * Closing a never-written fd: free everything, leave no slot.
	 * INIT_HCA-style fresh-VF probe still works without our help.
	 */
	if (!ctx->image_staged) {
		vfmig_load_drop_image_dma(ctx, pf_mdev);
		if (ctx->pd_allocated) {
			mlx5_core_dealloc_pd(pf_mdev, ctx->pdn);
			ctx->pd_allocated = false;
		}
		return;
	}

	load = kzalloc(sizeof(*load), GFP_KERNEL);
	if (!load) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: vf %u (vhca_id 0x%04x): no memory for pending_load slot, dropping staged blob\n",
			       ctx->vf_id, ctx->vhca_id);
		goto drop_staged;
	}

	load->vf_id = ctx->vf_id;
	load->vhca_id = ctx->vhca_id;
	load->pdn = ctx->pdn;
	load->pd_allocated = ctx->pd_allocated;
	load->mkey_in = ctx->image_mkey_in;
	load->mkey = ctx->image_mkey;
	load->mkey_created = ctx->image_mkey_created;
	load->dma_mapped = ctx->image_dma_mapped;
	load->dma_state = ctx->image_dma_state;
	load->pages = ctx->image_pages;
	load->npages = ctx->image_npages;
	load->record_size = ctx->record_size;

	mutex_lock(&ctx->vfmig->ctxs_lock);
	err = vfmig_install_pending_load_locked(ctx->vfmig, load);
	mutex_unlock(&ctx->vfmig->ctxs_lock);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: vf %u (vhca_id 0x%04x): failed to install pending_load slot: %d\n",
			       ctx->vf_id, ctx->vhca_id, err);
		/*
		 * Slot install failed; @load now owns the resources we
		 * just populated. Destroy frees PD/MKEY/DMA/pages.
		 * Mark ctx as "transferred" too so the post-release
		 * path doesn't double-free pages.
		 */
		ctx->image_transferred = true;
		ctx->pd_allocated = false;
		ctx->image_mkey_in = NULL;
		ctx->image_mkey_created = false;
		ctx->image_dma_mapped = false;
		ctx->image_pages = NULL;
		ctx->image_npages = 0;
		vfmig_vf_load_destroy(pf_mdev, load);
		return;
	}

	ctx->image_transferred = true;
	ctx->pd_allocated = false;
	ctx->image_mkey_in = NULL;
	ctx->image_mkey_created = false;
	ctx->image_dma_mapped = false;
	ctx->image_pages = NULL;
	ctx->image_npages = 0;

	mlx5_core_info(pf_mdev,
		       "vfmig: staged %llu bytes of LOAD state for vf %u (vhca_id 0x%04x); next probe will apply\n",
		       (unsigned long long)load->record_size,
		       ctx->vf_id, ctx->vhca_id);
	return;

drop_staged:
	vfmig_load_drop_image_dma(ctx, pf_mdev);
	if (ctx->pd_allocated) {
		mlx5_core_dealloc_pd(pf_mdev, ctx->pdn);
		ctx->pd_allocated = false;
	}
}

static int vfmig_load_release(struct inode *inode, struct file *filp)
{
	struct mlx5_vfmig_load_ctx *ctx = filp->private_data;
	struct mlx5_vfmig_pf *vfmig = ctx->vfmig;

	/*
	 * Free any HOST_PAGE payload buffer that was mid-record at close
	 * time (the parser allocates it in HP_READ_SUBHDR and frees it
	 * on the HP_REPLAY -> READ_HEADER transition; close() between
	 * those two states would otherwise leak the kvmalloc'd buffer).
	 * Safe outside vfmig->lock: hp_contents is purely ctx-local.
	 */
	if (ctx->hp_contents) {
		kvfree(ctx->hp_contents);
		ctx->hp_contents = NULL;
	}

	/*
	 * Tear down firmware-tied resources while pf_mdev is still alive.
	 * If the PF has already been unbound (dead), pf_cleanup() did the
	 * teardown synchronously and resources_freed is already set.
	 */
	down_read(&vfmig->lock);
	if (!vfmig->dead) {
		vfmig_load_release_resources(ctx);
		/*
		 * Reset the deterministic IOVA cursor exactly once before
		 * the staged blob is consumed by the next VF probe. Replay
		 * advanced the cursor to (highest_iova + len) so subsequent
		 * vfmig_iova_alloc_slot() calls would otherwise hand
		 * out fresh (post-replay) IOVAs instead of finding the
		 * replayed entries via lookup-at-cursor. Done here under
		 * vfmig->lock-read so dom can't be torn down from
		 * SET_TRACKED { enable=0 } in parallel; ctx->iova_dom was
		 * captured at LOAD-ioctl time and outlives the fd by the
		 * lifetime contract documented on the field.
		 *
		 * Idempotent via cursor_reset_done so a double-release
		 * (impossible in practice but cheap to guard) doesn't
		 * scramble the cursor of an unrelated subsequent SET_TRACKED.
		 */
		if (ctx->iova_dom && !ctx->cursor_reset_done) {
			vfmig_iova_reset_cursor(ctx->iova_dom);
			ctx->cursor_reset_done = true;
		}
	}
	up_read(&vfmig->lock);

	mutex_lock(&vfmig->ctxs_lock);
	list_del(&ctx->node);
	mutex_unlock(&vfmig->ctxs_lock);

	vfmig_free_pages(ctx->image_pages, ctx->image_npages);
	mutex_destroy(&ctx->io_lock);
	vfmig_pf_put(vfmig);
	kfree(ctx);
	return 0;
}

static const struct file_operations mlx5_vfmig_load_fops = {
	.owner		= THIS_MODULE,
	.write		= vfmig_load_write,
	.release	= vfmig_load_release,
};

/*
 * Set up the LOAD session and hand back an anon-inode fd. Caller holds
 * vfmig->lock for read.
 */
static long vfmig_ioc_load_vhca_state(struct mlx5_vfmig_pf *vfmig,
				      void __user *uarg)
{
	struct mlx5_vfmig_load_state arg;
	struct mlx5_core_sriov *sriov;
	struct mlx5_vfmig_load_ctx *ctx;
	bool migratable = false;
	struct file *file;
	u16 vhca_id;
	int fd;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.flags || arg.reserved)
		return -EINVAL;

	sriov = &vfmig->pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	err = vfmig_check_pf_migration_caps(vfmig->pf_mdev);
	if (err)
		return err;
	
	err = vfmig_query_vf_migratable(vfmig->pf_mdev, arg.vf_id,
						&migratable);
	if (err)
		return err;

	if (!migratable) {
		mlx5_core_warn(vfmig->pf_mdev,
		       "vfmig: vf %u is not migration-enabled (issue MLX5_VFMIG_IOC_ENABLE_MIGRATABLE pre-bind)\n",
			       arg.vf_id);
		return -EOPNOTSUPP;
	}

	err = vfmig_query_vhca_id(vfmig->pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	INIT_LIST_HEAD(&ctx->node);
	mutex_init(&ctx->io_lock);
	ctx->vf_id = arg.vf_id;
	ctx->vhca_id = vhca_id;
	ctx->state = VFMIG_LS_READ_HEADER;

	/*
	 * Capture the destination VF's IOVA domain handle (if any) so
	 * the parser can replay HOST_PAGE records into it. The pointer
	 * is stable for the fd's lifetime: SET_TRACKED { enable=0 },
	 * sriov_disable, and PF unbind all require the destination VF to
	 * be unbound, and binding the VF is what consumes the staged
	 * blob -- i.e. the VF can't be bound while this fd is active.
	 * NULL is fine and means "untracked destination, HOST_PAGE
	 * records will be rejected by the parser as a config error".
	 */
	ctx->iova_dom = vfmig->pf_mdev->priv.sriov.vfs_ctx[arg.vf_id].vfmig_iova_dom;

	/*
	 * Claim the vf_id slot first so the dup check + list_add are
	 * atomic. ctx->vfmig is set here too because everything past this
	 * point may need to call vfmig_load_release_resources(), which
	 * dereferences ctx->vfmig.
	 */
	mutex_lock(&vfmig->ctxs_lock);
	if (vfmig_vf_id_busy_locked(vfmig, ctx->vf_id)) {
		mutex_unlock(&vfmig->ctxs_lock);
		err = -EBUSY;
		goto err_claim;
	}
	vfmig_pf_get(vfmig);
	ctx->vfmig = vfmig;
	list_add(&ctx->node, &vfmig->load_ctxs);
	mutex_unlock(&vfmig->ctxs_lock);

	err = mlx5_core_alloc_pd(vfmig->pf_mdev, &ctx->pdn);
	if (err)
		goto err_pd;
	ctx->pd_allocated = true;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		err = fd;
		goto err_fd;
	}

	file = anon_inode_getfile("mlx5_vfmig_load", &mlx5_vfmig_load_fops,
				  ctx, O_WRONLY | O_CLOEXEC);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_anon;
	}
	stream_open(file_inode(file), file);

	arg.load_fd = fd;
	if (copy_to_user(uarg, &arg, sizeof(arg))) {
		err = -EFAULT;
		goto err_copy;
	}

	fd_install(fd, file);
	mlx5_core_info(vfmig->pf_mdev,
		       "vfmig: LOAD session opened for vf %u (vhca_id 0x%04x)\n",
		       ctx->vf_id, ctx->vhca_id);
	return 0;

err_copy:
	fput(file);
err_anon:
	put_unused_fd(fd);
err_fd:
	mlx5_core_dealloc_pd(vfmig->pf_mdev, ctx->pdn);
	ctx->pd_allocated = false;
err_pd:
	mutex_lock(&vfmig->ctxs_lock);
	list_del(&ctx->node);
	mutex_unlock(&vfmig->ctxs_lock);
	vfmig_pf_put(vfmig);
err_claim:
	mutex_destroy(&ctx->io_lock);
	kfree(ctx);
	return err;
}

/* -------- SAVE_VHCA_STATE: per-fd state buffer -------------------------- */

/*
 * Per-SAVE-fd context. Hung off vfmig->save_ctxs. Resources are
 * allocated up-front in the ioctl handler (single SAVE_VHCA_STATE per
 * session, no streaming back into the firmware), and torn down on
 * release(). Fields named like the LOAD context, but DMA direction is
 * reversed and there is no parser FSM -- the fd's sole job is to drain
 * the staging pages, framed as one FW_DATA wire record.
 */
struct mlx5_vfmig_save_ctx {
	struct list_head node;		/* on vfmig->save_ctxs */
	struct mlx5_vfmig_pf *vfmig;	/* holds a kref */
	struct mutex io_lock;		/* serializes concurrent read()s */
	u32 vf_id;
	u16 vhca_id;
	u32 flags;			/* MLX5_VFMIG_SAVE_FLAG_* */

	/* Resources tied to the PF mdev. Released by pf_cleanup or
	 * release. Mutated under vfmig->lock.
	 */
	bool resources_freed;
	bool pd_allocated;
	u32 pdn;
	bool image_dma_mapped;
	bool image_mkey_created;
	struct page **image_pages;
	u32 image_npages;	/* allocated capacity (PAGE_SIZE units) */
	u32 *image_mkey_in;
	u32 image_mkey;
	struct dma_iova_state image_dma_state;

	/* Suspend bookkeeping for the resume-on-close policy. */
	bool suspended_initiator;
	bool suspended_responder;

	/*
	 * Wire-format payload size (bytes the firmware actually wrote
	 * into image_pages, derived from save_vhca_state_out
	 * ::actual_image_size).
	 */
	u64 image_size;

	/*
	 * HOST_PAGE prefix buffer. Built once at SAVE-ioctl time by
	 * snapshotting the source VF's vfmig_iova_domain registry into a
	 * contiguous kvmalloc'd buffer of fully-formed wire records, so
	 * vfmig_save_drain() can stream it out alongside the FW_DATA
	 * payload without a second registry walk under DMA-coherent
	 * pressure. NULL if the source VF was untracked at SAVE time;
	 * @host_pages_size is then 0 and the wire stream contains only
	 * the FW_DATA record (i.e. byte-identical to the pre-L1 stream
	 * shape).
	 *
	 * Each record on disk is:
	 *   16 B vfmig_wire_header (record_size, flags=0, tag=HOST_PAGE)
	 *   16 B vfmig_host_page_record (iova, len)
	 *   len  bytes of page contents
	 *
	 * Captured eagerly (rather than streamed lazily during
	 * read()) because (a) it's small -- Layer 1 produces ~4 KiB
	 * total, Layer 2 maybe ~MB -- and (b) it lets vfmig_save_drain
	 * remain a simple byte-cursor walk instead of a state machine.
	 */
	void *host_pages_buf;
	u64 host_pages_size;

	/*
	 * Read cursor in bytes covering the concatenation:
	 *   [0..host_pages_size)
	 *       HOST_PAGE records snapshotted from the IOVA domain
	 *   [host_pages_size..host_pages_size + 16)
	 *       FW_DATA wire header
	 *   [host_pages_size + 16..host_pages_size + 16 + image_size)
	 *       FW state payload from image_pages[]
	 * Updated under io_lock.
	 */
	u64 read_pos;
};

static void vfmig_save_release_resources(struct mlx5_vfmig_save_ctx *ctx);

/* True iff @vf_id already has an open LOAD or SAVE session. ctxs_lock held. */
static bool vfmig_vf_id_busy_locked(struct mlx5_vfmig_pf *vfmig, u32 vf_id)
{
	struct mlx5_vfmig_load_ctx *l;
	struct mlx5_vfmig_save_ctx *s;

	list_for_each_entry(l, &vfmig->load_ctxs, node)
		if (l->vf_id == vf_id)
			return true;
	list_for_each_entry(s, &vfmig->save_ctxs, node)
		if (s->vf_id == vf_id)
			return true;
	return false;
}

/* Build the on-wire FW_DATA header for ctx->image_size, copy into @hdr. */
static void vfmig_save_build_header(struct mlx5_vfmig_save_ctx *ctx,
				    struct vfmig_wire_header *hdr)
{
	hdr->record_size = cpu_to_le64(ctx->image_size);
	hdr->flags = 0;
	hdr->tag = cpu_to_le32(VFMIG_WIRE_TAG_FW_DATA);
}

/*
 * Pass-1 callback for vfmig_iova_for_each: just sum each entry's
 * on-wire footprint into @ctx so vfmig_save_build_host_pages_buf can
 * size the kvmalloc.
 */
struct vfmig_save_hp_size_ctx {
	u64 total;
};

static int vfmig_save_hp_count_cb(dma_addr_t iova, const void *vaddr,
				  size_t len, void *ctx)
{
	struct vfmig_save_hp_size_ctx *sc = ctx;

	(void)iova;
	(void)vaddr;
	sc->total += sizeof(struct vfmig_wire_header) +
		     sizeof(struct vfmig_host_page_record) + len;
	return 0;
}

/*
 * Pass-2 callback: serialize one HOST_PAGE record into the buffer
 * pointed to by @ctx->cursor and advance the cursor.
 */
struct vfmig_save_hp_emit_ctx {
	u8 *buf;
	u64 capacity;
	u64 cursor;
};

static int vfmig_save_hp_emit_cb(dma_addr_t iova, const void *vaddr,
				 size_t len, void *ctx)
{
	struct vfmig_save_hp_emit_ctx *ec = ctx;
	struct vfmig_wire_header hdr;
	struct vfmig_host_page_record sub;
	u64 record_size = sizeof(sub) + len;
	u64 need = sizeof(hdr) + record_size;

	if (ec->cursor + need > ec->capacity)
		return -EOVERFLOW;

	hdr.record_size = cpu_to_le64(record_size);
	hdr.flags	= 0;
	hdr.tag		= cpu_to_le32(VFMIG_WIRE_TAG_HOST_PAGE);
	memcpy(ec->buf + ec->cursor, &hdr, sizeof(hdr));
	ec->cursor += sizeof(hdr);

	sub.iova = cpu_to_le64(iova);
	sub.len  = cpu_to_le64(len);
	memcpy(ec->buf + ec->cursor, &sub, sizeof(sub));
	ec->cursor += sizeof(sub);

	memcpy(ec->buf + ec->cursor, vaddr, len);
	ec->cursor += len;
	return 0;
}

/*
 * Snapshot the source VF's vfmig_iova_domain registry into a
 * contiguous wire-format buffer of HOST_PAGE records, owned by
 * @ctx->host_pages_buf and sized by @ctx->host_pages_size. NULL
 * domain (untracked source) leaves both fields zero and the wire
 * stream is byte-identical to pre-L1 (just a single FW_DATA record).
 *
 * Two-pass: size, then fill. This is racier than walking once with
 * a growable buffer, but vfmig_iova_for_each takes the domain mutex
 * each call which would have to release-and-reacquire to grow under
 * GFP_KERNEL anyway. The cost is two registry walks at a moment
 * when the source VF is SUSPEND_VHCA'd (so no concurrent allocs).
 */
static int vfmig_save_build_host_pages_buf(struct mlx5_vfmig_save_ctx *ctx,
					   struct vfmig_iova_domain *dom)
{
	struct vfmig_save_hp_size_ctx sc = {};
	struct vfmig_save_hp_emit_ctx ec;
	int err;

	if (!dom)
		return 0;

	err = vfmig_iova_for_each(dom, vfmig_save_hp_count_cb, &sc);
	if (err)
		return err;
	if (!sc.total)
		return 0;

	ctx->host_pages_buf = kvmalloc(sc.total, GFP_KERNEL);
	if (!ctx->host_pages_buf)
		return -ENOMEM;

	ec.buf	    = ctx->host_pages_buf;
	ec.capacity = sc.total;
	ec.cursor   = 0;
	err = vfmig_iova_for_each(dom, vfmig_save_hp_emit_cb, &ec);
	if (err) {
		kvfree(ctx->host_pages_buf);
		ctx->host_pages_buf = NULL;
		return err;
	}
	if (WARN_ON(ec.cursor != sc.total)) {
		kvfree(ctx->host_pages_buf);
		ctx->host_pages_buf = NULL;
		return -EIO;
	}
	ctx->host_pages_size = sc.total;
	return 0;
}

/*
 * Drain into @ubuf for one read(). Wire layout is the concatenation:
 *
 *   [0..host_pages_size)
 *       HOST_PAGE prefix records, snapshotted at SAVE-ioctl time
 *       from the source VF's vfmig_iova_domain. Empty if the source
 *       was untracked.
 *   [host_pages_size..host_pages_size + 16)
 *       16-byte FW_DATA wire header.
 *   [host_pages_size + 16..host_pages_size + 16 + image_size)
 *       FW state payload from image_pages[], one PAGE_SIZE chunk
 *       at a time.
 *
 * EOF is total. Caller holds vfmig->lock for read AND ctx->io_lock;
 * pf_mdev must be alive (only used transitively via the kmap of
 * image_pages, which is mdev-independent, so this remains safe even
 * if vfmig->dead -- the early bail happens in vfmig_save_read).
 */
static ssize_t vfmig_save_drain(struct mlx5_vfmig_save_ctx *ctx,
				char __user *ubuf, size_t count)
{
	const u64 HP_SZ  = ctx->host_pages_size;
	const u64 HDR_SZ = sizeof(struct vfmig_wire_header);
	const u64 FW_OFF = HP_SZ + HDR_SZ;
	const u64 total  = FW_OFF + ctx->image_size;
	size_t copied = 0;
	ssize_t err = 0;

	if (ctx->read_pos >= total)
		return 0;

	/* Section A: HOST_PAGE prefix bytes. */
	if (ctx->read_pos < HP_SZ && count) {
		size_t want = min_t(size_t, count, HP_SZ - ctx->read_pos);

		if (copy_to_user(ubuf, (u8 *)ctx->host_pages_buf + ctx->read_pos,
				 want))
			return -EFAULT;
		ctx->read_pos += want;
		ubuf += want;
		count -= want;
		copied += want;
	}

	/* Section B: FW_DATA wire header. */
	if (ctx->read_pos >= HP_SZ && ctx->read_pos < FW_OFF && count) {
		struct vfmig_wire_header hdr;
		u64 hoff = ctx->read_pos - HP_SZ;
		size_t want = min_t(size_t, count, HDR_SZ - hoff);

		vfmig_save_build_header(ctx, &hdr);
		if (copy_to_user(ubuf, ((u8 *)&hdr) + hoff, want))
			return copied ? (ssize_t)copied : -EFAULT;
		ctx->read_pos += want;
		ubuf += want;
		count -= want;
		copied += want;
	}

	/* Section C: FW data payload pages. */
	while (count && ctx->read_pos < total) {
		u64 payload_off = ctx->read_pos - FW_OFF;
		u32 page_idx = payload_off >> PAGE_SHIFT;
		size_t page_off = payload_off & (PAGE_SIZE - 1);
		size_t want = min3((size_t)(total - ctx->read_pos), count,
				   PAGE_SIZE - page_off);
		const u8 *from;

		if (page_idx >= ctx->image_npages)
			return copied ? (ssize_t)copied : -EINVAL;

		from = kmap_local_page(ctx->image_pages[page_idx]);
		if (copy_to_user(ubuf, from + page_off, want)) {
			kunmap_local(from);
			err = -EFAULT;
			break;
		}
		kunmap_local(from);
		ctx->read_pos += want;
		ubuf += want;
		count -= want;
		copied += want;
	}

	if (!copied && err)
		return err;
	return copied;
}

static ssize_t vfmig_save_read(struct file *filp, char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct mlx5_vfmig_save_ctx *ctx = filp->private_data;
	struct mlx5_vfmig_pf *vfmig = ctx->vfmig;
	ssize_t ret;

	if (!count)
		return 0;

	mutex_lock(&ctx->io_lock);
	down_read(&vfmig->lock);
	if (vfmig->dead || ctx->resources_freed) {
		ret = -ENODEV;
		goto out;
	}
	ret = vfmig_save_drain(ctx, ubuf, count);
out:
	up_read(&vfmig->lock);
	mutex_unlock(&ctx->io_lock);
	/*
	 * stream_open() set FMODE_STREAM, so ksys_read() passes ppos==NULL.
	 * Don't touch it.
	 */
	return ret;
}

/*
 * Drop firmware-tied resources held by @ctx and (unless KEEP_SUSPENDED)
 * resume the VHCA. Same caller contract as vfmig_load_release_resources.
 */
static void vfmig_save_release_resources(struct mlx5_vfmig_save_ctx *ctx)
{
	struct mlx5_core_dev *pf_mdev = ctx->vfmig->pf_mdev;
	int err;

	if (ctx->resources_freed)
		return;
	ctx->resources_freed = true;

	/*
	 * host_pages_buf is purely host memory, owned by ctx, with no
	 * dependence on pf_mdev being alive. Drop it eagerly so the
	 * pf_mdev==NULL early-bail below doesn't strand it.
	 */
	if (ctx->host_pages_buf) {
		kvfree(ctx->host_pages_buf);
		ctx->host_pages_buf = NULL;
		ctx->host_pages_size = 0;
	}

	if (!pf_mdev)
		return;

	if (ctx->image_mkey_created) {
		mlx5_core_destroy_mkey(pf_mdev, ctx->image_mkey);
		ctx->image_mkey_created = false;
	}
	if (ctx->image_dma_mapped) {
		vfmig_unregister_dma_pages(pf_mdev, ctx->image_npages,
					   ctx->image_mkey_in,
					   &ctx->image_dma_state,
					   DMA_FROM_DEVICE);
		ctx->image_dma_mapped = false;
	}
	kvfree(ctx->image_mkey_in);
	ctx->image_mkey_in = NULL;

	if (ctx->pd_allocated) {
		mlx5_core_dealloc_pd(pf_mdev, ctx->pdn);
		ctx->pd_allocated = false;
	}

	/*
	 * Resume in the inverse order of suspend (responder first, then
	 * initiator). Best-effort: a failed resume is logged but doesn't
	 * propagate -- userspace already consumed the blob and CRIU
	 * dump-then-destroy callers don't care. The bookkeeping bools mean
	 * we won't issue a stray RESUME if the corresponding SUSPEND
	 * never succeeded.
	 */
	if (ctx->flags & MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED)
		return;

	if (ctx->suspended_responder) {
		err = vfmig_cmd_resume_vhca(pf_mdev, ctx->vhca_id,
			MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_RESPONDER);
		if (err)
			mlx5_core_warn(pf_mdev,
				       "vfmig: RESUME_VHCA(RESPONDER) vf %u (vhca_id 0x%04x) failed: %d\n",
				       ctx->vf_id, ctx->vhca_id, err);
		ctx->suspended_responder = false;
	}
	if (ctx->suspended_initiator) {
		err = vfmig_cmd_resume_vhca(pf_mdev, ctx->vhca_id,
			MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_INITIATOR);
		if (err)
			mlx5_core_warn(pf_mdev,
				       "vfmig: RESUME_VHCA(INITIATOR) vf %u (vhca_id 0x%04x) failed: %d\n",
				       ctx->vf_id, ctx->vhca_id, err);
		ctx->suspended_initiator = false;
	}
}

static int vfmig_save_release(struct inode *inode, struct file *filp)
{
	struct mlx5_vfmig_save_ctx *ctx = filp->private_data;
	struct mlx5_vfmig_pf *vfmig = ctx->vfmig;

	down_read(&vfmig->lock);
	if (!vfmig->dead)
		vfmig_save_release_resources(ctx);
	up_read(&vfmig->lock);

	mutex_lock(&vfmig->ctxs_lock);
	list_del(&ctx->node);
	mutex_unlock(&vfmig->ctxs_lock);

	vfmig_free_pages(ctx->image_pages, ctx->image_npages);
	mutex_destroy(&ctx->io_lock);
	vfmig_pf_put(vfmig);
	kfree(ctx);
	return 0;
}

static const struct file_operations mlx5_vfmig_save_fops = {
	.owner		= THIS_MODULE,
	.read		= vfmig_save_read,
	.release	= vfmig_save_release,
};

/*
 * Set up the SAVE session: query vhca_id, suspend the VHCA, ask the FW
 * how big the snapshot is, allocate + register DMA pages, run
 * SAVE_VHCA_STATE, then hand back the read-only anon-inode fd. Caller
 * holds vfmig->lock for read.
 */
static long vfmig_ioc_save_vhca_state(struct mlx5_vfmig_pf *vfmig,
				      void __user *uarg)
{
	struct mlx5_vfmig_save_state arg;
	struct mlx5_core_sriov *sriov;
	struct mlx5_vfmig_save_ctx *ctx;
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	bool migratable = false;
	u64 query_size = 0;
	u64 actual_size = 0;
	struct file *file;
	u32 npages;
	u16 vhca_id;
	int fd;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved || (arg.flags & ~MLX5_VFMIG_SAVE_FLAG_ALL))
		return -EINVAL;

	sriov = &pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	err = vfmig_check_pf_migration_caps(pf_mdev);
	if (err)
		return err;

	err = vfmig_query_vf_migratable(pf_mdev, arg.vf_id, &migratable);
	if (err)
		return err;
	if (!migratable) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: vf %u is not migration-enabled (issue MLX5_VFMIG_IOC_ENABLE_MIGRATABLE pre-bind)\n",
			       arg.vf_id);
		return -EOPNOTSUPP;
	}

	err = vfmig_query_vhca_id(pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	INIT_LIST_HEAD(&ctx->node);
	mutex_init(&ctx->io_lock);
	ctx->vf_id = arg.vf_id;
	ctx->vhca_id = vhca_id;
	ctx->flags = arg.flags;

	/* Claim vf_id atomically vs both LOAD and SAVE sessions. */
	mutex_lock(&vfmig->ctxs_lock);
	if (vfmig_vf_id_busy_locked(vfmig, ctx->vf_id)) {
		mutex_unlock(&vfmig->ctxs_lock);
		err = -EBUSY;
		goto err_claim;
	}
	vfmig_pf_get(vfmig);
	ctx->vfmig = vfmig;
	list_add(&ctx->node, &vfmig->save_ctxs);
	mutex_unlock(&vfmig->ctxs_lock);

	err = mlx5_core_alloc_pd(pf_mdev, &ctx->pdn);
	if (err)
		goto err_pd;
	ctx->pd_allocated = true;

	/* Quiesce: initiator (egress) first, then responder (ingress). */
	err = vfmig_cmd_suspend_vhca(pf_mdev, vhca_id,
		MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_INITIATOR);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: SUSPEND_VHCA(INITIATOR) vf %u (vhca_id 0x%04x) failed: %d\n",
			       arg.vf_id, vhca_id, err);
		goto err_suspend;
	}
	ctx->suspended_initiator = true;

	err = vfmig_cmd_suspend_vhca(pf_mdev, vhca_id,
		MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_RESPONDER);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: SUSPEND_VHCA(RESPONDER) vf %u (vhca_id 0x%04x) failed: %d\n",
			       arg.vf_id, vhca_id, err);
		goto err_suspend;
	}
	ctx->suspended_responder = true;

	err = vfmig_cmd_query_vhca_migration_state(pf_mdev, vhca_id,
						   &query_size);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: QUERY_VHCA_MIGRATION_STATE vf %u (vhca_id 0x%04x) failed: %d\n",
			       arg.vf_id, vhca_id, err);
		goto err_suspend;
	}
	if (!query_size || query_size > VFMIG_MAX_LOAD_SIZE) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: implausible migration size %llu for vf %u\n",
			       (unsigned long long)query_size, arg.vf_id);
		err = -ERANGE;
		goto err_suspend;
	}

	npages = max_t(u32, 1, DIV_ROUND_UP(query_size, PAGE_SIZE));
	err = vfmig_alloc_pages(&ctx->image_pages, npages);
	if (err)
		goto err_suspend;
	ctx->image_npages = npages;

	ctx->image_mkey_in = vfmig_alloc_mkey_in(npages, ctx->pdn);
	if (!ctx->image_mkey_in) {
		err = -ENOMEM;
		goto err_pages;
	}

	err = vfmig_register_dma_pages(pf_mdev, npages, ctx->image_pages,
				       ctx->image_mkey_in,
				       &ctx->image_dma_state,
				       DMA_FROM_DEVICE);
	if (err)
		goto err_mkey_in;
	ctx->image_dma_mapped = true;

	err = vfmig_create_mkey(pf_mdev, npages, ctx->image_mkey_in,
				&ctx->image_mkey);
	if (err)
		goto err_dma;
	ctx->image_mkey_created = true;

	err = vfmig_cmd_save_vhca_state(pf_mdev, vhca_id, ctx->image_mkey,
					npages * PAGE_SIZE, &actual_size);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: SAVE_VHCA_STATE vf %u (vhca_id 0x%04x) failed: %d\n",
			       arg.vf_id, vhca_id, err);
		goto err_save;
	}
	if (!actual_size || actual_size > (u64)npages * PAGE_SIZE) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: SAVE_VHCA_STATE returned implausible size %llu (cap %llu) for vf %u\n",
			       (unsigned long long)actual_size,
			       (unsigned long long)((u64)npages * PAGE_SIZE),
			       arg.vf_id);
		err = -EIO;
		goto err_save;
	}
	ctx->image_size = actual_size;

	/*
	 * Snapshot the source VF's deterministic-IOVA registry into a
	 * HOST_PAGE prefix buffer. Done here, after SUSPEND_VHCA and
	 * before user-visible read()s start, so the snapshot is
	 * coherent with the FW state captured by SAVE_VHCA_STATE: both
	 * reflect the VHCA at the same quiesced point.
	 *
	 * vfs_ctx[].vfmig_iova_dom is read under vfmig->lock-read (held
	 * by the ioctl dispatcher); SET_TRACKED { enable=0 } can't free
	 * it concurrently because the source VF is currently bound (it
	 * has to be, for SAVE_VHCA_STATE to make sense), and SET_TRACKED
	 * rejects toggles on bound VFs with -EBUSY. NULL domain is fine
	 * and yields a zero-byte prefix (legacy single-FW_DATA stream).
	 */
	err = vfmig_save_build_host_pages_buf(ctx,
		sriov->vfs_ctx[arg.vf_id].vfmig_iova_dom);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: vf %u: build_host_pages_buf failed: %d\n",
			       arg.vf_id, err);
		goto err_save;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		err = fd;
		goto err_save;
	}

	file = anon_inode_getfile("mlx5_vfmig_save", &mlx5_vfmig_save_fops,
				  ctx, O_RDONLY | O_CLOEXEC);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_anon;
	}
	stream_open(file_inode(file), file);

	arg.save_fd = fd;
	if (copy_to_user(uarg, &arg, sizeof(arg))) {
		err = -EFAULT;
		goto err_copy;
	}

	fd_install(fd, file);
	mlx5_core_info(pf_mdev,
		       "vfmig: SAVE session opened for vf %u (vhca_id 0x%04x), %llu bytes\n",
		       arg.vf_id, vhca_id, (unsigned long long)actual_size);
	return 0;

err_copy:
	fput(file);
	/*
	 * fput() runs vfmig_save_release asynchronously, which will tear
	 * down the resources we set up here. Skip the unwind path.
	 */
	put_unused_fd(fd);
	return err;
err_anon:
	put_unused_fd(fd);
err_save:
	/*
	 * host_pages_buf may or may not have been built by this point
	 * (depends on which goto err_save brought us here). kvfree(NULL)
	 * is a no-op, so the unconditional drop is correct for both the
	 * pre-build error gotos (save_vhca_state failure, size check
	 * failure) and the post-build ones (get_unused_fd / anon_inode
	 * failure). Without this the post-build paths would leak the
	 * snapshot buffer on the unwind.
	 */
	kvfree(ctx->host_pages_buf);
	ctx->host_pages_buf = NULL;
	ctx->host_pages_size = 0;
	if (ctx->image_mkey_created) {
		mlx5_core_destroy_mkey(pf_mdev, ctx->image_mkey);
		ctx->image_mkey_created = false;
	}
err_dma:
	if (ctx->image_dma_mapped) {
		vfmig_unregister_dma_pages(pf_mdev, npages, ctx->image_mkey_in,
					   &ctx->image_dma_state,
					   DMA_FROM_DEVICE);
		ctx->image_dma_mapped = false;
	}
err_mkey_in:
	kvfree(ctx->image_mkey_in);
	ctx->image_mkey_in = NULL;
err_pages:
	vfmig_free_pages(ctx->image_pages, ctx->image_npages);
	ctx->image_pages = NULL;
	ctx->image_npages = 0;
err_suspend:
	/* Best-effort resume to undo any successful SUSPEND. */
	if (ctx->suspended_responder) {
		(void)vfmig_cmd_resume_vhca(pf_mdev, vhca_id,
			MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_RESPONDER);
		ctx->suspended_responder = false;
	}
	if (ctx->suspended_initiator) {
		(void)vfmig_cmd_resume_vhca(pf_mdev, vhca_id,
			MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_INITIATOR);
		ctx->suspended_initiator = false;
	}
	if (ctx->pd_allocated) {
		mlx5_core_dealloc_pd(pf_mdev, ctx->pdn);
		ctx->pd_allocated = false;
	}
err_pd:
	mutex_lock(&vfmig->ctxs_lock);
	list_del(&ctx->node);
	mutex_unlock(&vfmig->ctxs_lock);
	vfmig_pf_put(vfmig);
err_claim:
	mutex_destroy(&ctx->io_lock);
	kfree(ctx);
	return err;
}

/* -------- cdev file ops ------------------------------------------------- */

static int vfmig_open(struct inode *inode, struct file *filp)
{
	struct mlx5_vfmig_pf *vfmig =
		container_of(inode->i_cdev, struct mlx5_vfmig_pf, cdev);

	vfmig_pf_get(vfmig);
	filp->private_data = vfmig;
	return 0;
}

static int vfmig_release(struct inode *inode, struct file *filp)
{
	struct mlx5_vfmig_pf *vfmig = filp->private_data;

	vfmig_pf_put(vfmig);
	return 0;
}

static long vfmig_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct mlx5_vfmig_pf *vfmig = filp->private_data;
	void __user *uarg = (void __user *)arg;
	long ret;

	down_read(&vfmig->lock);
	if (vfmig->dead) {
		ret = -ENODEV;
		goto out;
	}

	switch (cmd) {
	case MLX5_VFMIG_IOC_MARK_RESTORED:
		ret = vfmig_ioc_mark_restored(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_GET_VHCA_ID:
		ret = vfmig_ioc_get_vhca_id(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_QUERY_VF:
		ret = vfmig_ioc_query_vf(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_LOAD_VHCA_STATE:
		ret = vfmig_ioc_load_vhca_state(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_SAVE_VHCA_STATE:
		ret = vfmig_ioc_save_vhca_state(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_ENABLE_MIGRATABLE:
		ret = vfmig_ioc_enable_migratable(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_SET_TRACKED:
		ret = vfmig_ioc_set_tracked(vfmig, uarg);
		break;
	default:
		ret = -ENOTTY;
		break;
	}
out:
	up_read(&vfmig->lock);
	return ret;
}

static const struct file_operations mlx5_vfmig_fops = {
	.owner		= THIS_MODULE,
	.open		= vfmig_open,
	.release	= vfmig_release,
	.unlocked_ioctl	= vfmig_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

/* -------- per-PF init / cleanup ----------------------------------------- */

int mlx5_vfmig_pf_init(struct mlx5_core_dev *pf_mdev)
{
	struct mlx5_vfmig_pf *vfmig;
	struct device *dev;
	dev_t devno;
	int minor, err;

	if (mlx5_core_is_vf(pf_mdev))
		return 0;

	vfmig = kzalloc(sizeof(*vfmig), GFP_KERNEL);
	if (!vfmig)
		return -ENOMEM;

	kref_init(&vfmig->kref);
	init_rwsem(&vfmig->lock);
	mutex_init(&vfmig->ctxs_lock);
	INIT_LIST_HEAD(&vfmig->load_ctxs);
	INIT_LIST_HEAD(&vfmig->save_ctxs);
	vfmig->pf_mdev = pf_mdev;

	minor = ida_alloc_max(&mlx5_vfmig_minor_ida,
			      MLX5_VFMIG_MAX_DEVICES - 1, GFP_KERNEL);
	if (minor < 0) {
		err = minor;
		goto err_free;
	}
	vfmig->minor = minor;
	devno = MKDEV(MAJOR(mlx5_vfmig_devt), minor);

	cdev_init(&vfmig->cdev, &mlx5_vfmig_fops);
	vfmig->cdev.owner = THIS_MODULE;

	err = cdev_add(&vfmig->cdev, devno, 1);
	if (err)
		goto err_minor;

	dev = device_create(mlx5_vfmig_class, pf_mdev->device, devno, vfmig,
			    "mlx5_vfmig!%s", dev_name(pf_mdev->device));
	if (IS_ERR(dev)) {
		err = PTR_ERR(dev);
		goto err_cdev;
	}

	pf_mdev->priv.vfmig = vfmig;
	mlx5_core_info(pf_mdev, "vfmig: cdev /dev/mlx5_vfmig/%s ready\n",
		       dev_name(pf_mdev->device));
	return 0;

err_cdev:
	cdev_del(&vfmig->cdev);
err_minor:
	ida_free(&mlx5_vfmig_minor_ida, minor);
err_free:
	mutex_destroy(&vfmig->ctxs_lock);
	kfree(vfmig);
	return err;
}

void mlx5_vfmig_pf_cleanup(struct mlx5_core_dev *pf_mdev)
{
	struct mlx5_vfmig_pf *vfmig = pf_mdev->priv.vfmig;
	struct mlx5_vfmig_load_ctx *load_ctx;
	struct mlx5_vfmig_save_ctx *save_ctx;
	dev_t devno;

	if (!vfmig)
		return;

	pf_mdev->priv.vfmig = NULL;
	devno = MKDEV(MAJOR(mlx5_vfmig_devt), vfmig->minor);

	/*
	 * Neuter the device. Synchronously tear down LOAD-session
	 * firmware resources (PD/MKEY/DMA mappings) while pf_mdev is still
	 * alive; the page lists themselves are mdev-independent and get
	 * freed when each fd is later closed.
	 *
	 * The down_write blocks until all in-flight readers
	 * (vfmig_ioctl, vfmig_load_write, vfmig_load_release) drop their
	 * read locks. Once we hold the write lock the load_ctxs list is
	 * stable without taking ctxs_lock.
	 */
	down_write(&vfmig->lock);
	list_for_each_entry(load_ctx, &vfmig->load_ctxs, node)
		vfmig_load_release_resources(load_ctx);
	list_for_each_entry(save_ctx, &vfmig->save_ctxs, node)
		vfmig_save_release_resources(save_ctx);
	/*
	 * Drain any per-VF pending_load slots (including ones we just
	 * promoted out of the load_ctxs above). Must happen while pf_mdev
	 * is still alive so PD/MKEY/DMA teardown works.
	 */
	vfmig_pf_drop_pending_loads_locked(vfmig);
	/*
	 * Drop any per-VF IOVA domains. Same ordering rationale: must run
	 * while the VF pci_devs are still live so iommu_detach_device()
	 * inside vfmig_iova_domain_destroy() finds a real device. PF unbind
	 * unloads after this returns (mlx5_unload), so VFs are still here.
	 */
	vfmig_pf_drop_iova_domains_locked(vfmig);
	vfmig->dead = true;
	vfmig->pf_mdev = NULL;
	up_write(&vfmig->lock);

	device_destroy(mlx5_vfmig_class, devno);
	cdev_del(&vfmig->cdev);

	vfmig_pf_put(vfmig);
}

/* -------- VF probe-time hook -------------------------------------------- */

bool mlx5_vf_is_vfmig_tracked(struct mlx5_core_dev *dev)
{
	struct pci_dev *vf_pdev = dev->pdev;
	struct mlx5_core_dev *pf_mdev;
	struct mlx5_core_sriov *sriov;
	bool tracked = false;
	int vf_id;

	if (!vf_pdev || !vf_pdev->is_virtfn)
		return false;

	vf_id = pci_iov_vf_id(vf_pdev);
	if (vf_id < 0)
		return false;

	pf_mdev = mlx5_vf_get_core_dev(vf_pdev);
	if (!pf_mdev)
		return false;

	sriov = &pf_mdev->priv.sriov;
	if (vf_id < sriov->num_vfs)
		tracked = sriov->vfs_ctx[vf_id].vfmig_tracked;
	mlx5_vf_put_core_dev(pf_mdev);
	return tracked;
}

struct vfmig_iova_domain *
mlx5_vf_get_vfmig_iova_domain(struct mlx5_core_dev *vf_dev)
{
	struct pci_dev *vf_pdev = vf_dev->pdev;
	struct vfmig_iova_domain *dom = NULL;
	struct mlx5_core_dev *pf_mdev;
	struct mlx5_core_sriov *sriov;
	int vf_id;

	if (!vf_pdev || !vf_pdev->is_virtfn)
		return NULL;

	vf_id = pci_iov_vf_id(vf_pdev);
	if (vf_id < 0)
		return NULL;

	pf_mdev = mlx5_vf_get_core_dev(vf_pdev);
	if (!pf_mdev)
		return NULL;

	/*
	 * The vfmig_iova_dom pointer is set under the PF's vfmig->lock by
	 * the SET_TRACKED handler, but reading it here is unlocked: the
	 * lifetime contract documented on this function (VF must be unbound
	 * for either SET_TRACKED { enable=0 } or sriov_disable to free the
	 * domain, and we are mid-probe of the VF, so the VF is bound) means
	 * the pointer cannot be torn down underneath us. mlx5_vf_get_core_dev
	 * also pins the PF mdev until put, so the vfs_ctx[] array stays alive.
	 *
	 * Tracked-bit and domain-pointer set/clear together in the SET_TRACKED
	 * handler, so it's enough to check vfmig_iova_dom directly.
	 */
	sriov = &pf_mdev->priv.sriov;
	if (vf_id < sriov->num_vfs)
		dom = sriov->vfs_ctx[vf_id].vfmig_iova_dom;
	mlx5_vf_put_core_dev(pf_mdev);
	return dom;
}

bool mlx5_vfmig_vf_consume_restored(struct mlx5_core_dev *dev, u16 *vhca_id_out)
{
	struct pci_dev *vf_pdev = dev->pdev;
	struct mlx5_core_dev *pf_mdev;
	struct mlx5_core_sriov *sriov;
	bool restored = false;
	int vf_id;

	if (!vf_pdev || !vf_pdev->is_virtfn)
		return false;

	vf_id = pci_iov_vf_id(vf_pdev);
	if (vf_id < 0)
		return false;

	pf_mdev = mlx5_vf_get_core_dev(vf_pdev);
	if (!pf_mdev)
		return false;

	sriov = &pf_mdev->priv.sriov;
	if (vf_id < sriov->num_vfs && sriov->vfs_ctx[vf_id].restored) {
		if (vhca_id_out)
			*vhca_id_out = sriov->vfs_ctx[vf_id].restored_vhca_id;
		sriov->vfs_ctx[vf_id].restored_vhca_id = 0;
		sriov->vfs_ctx[vf_id].restored = 0;
		restored = true;
	}
	mlx5_vf_put_core_dev(pf_mdev);

	return restored;
}

/*
 * Pop @vf_id's pending_load slot off the PF's vfs_ctx[]. Returns the
 * detached slot or NULL if none was staged. Caller takes ownership and
 * must eventually call vfmig_vf_load_destroy().
 *
 * Caller holds vfmig->lock for read AND vfmig->ctxs_lock.
 */
static struct mlx5_vfmig_vf_load *
vfmig_take_pending_load_locked(struct mlx5_vfmig_pf *vfmig, u32 vf_id)
{
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_core_sriov *sriov;
	struct mlx5_vfmig_vf_load *load;

	if (!pf_mdev)
		return NULL;
	sriov = &pf_mdev->priv.sriov;
	if (vf_id >= sriov->num_vfs)
		return NULL;

	load = sriov->vfs_ctx[vf_id].vfmig_pending_load;
	sriov->vfs_ctx[vf_id].vfmig_pending_load = NULL;
	return load;
}

int mlx5_vfmig_vf_apply_pending_load(struct mlx5_core_dev *vf_dev)
{
	struct pci_dev *vf_pdev = vf_dev->pdev;
	struct mlx5_core_dev *pf_mdev;
	struct mlx5_vfmig_pf *vfmig;
	struct mlx5_vfmig_vf_load *load;
	int vf_id;
	int err = 0;
	int err_resume;

	if (!vf_pdev || !vf_pdev->is_virtfn)
		return 0;
	vf_id = pci_iov_vf_id(vf_pdev);
	if (vf_id < 0)
		return 0;

	pf_mdev = mlx5_vf_get_core_dev(vf_pdev);
	if (!pf_mdev)
		return 0;

	vfmig = pf_mdev->priv.vfmig;
	if (!vfmig) {
		mlx5_vf_put_core_dev(pf_mdev);
		return 0;
	}

	/*
	 * Take the kref so the vfmig context (and its locks) survive
	 * even if pf_cleanup races us. The PF mdev itself is pinned by
	 * mlx5_vf_get_core_dev's intf_state_mutex.
	 */
	vfmig_pf_get(vfmig);

	down_read(&vfmig->lock);
	if (vfmig->dead) {
		up_read(&vfmig->lock);
		vfmig_pf_put(vfmig);
		mlx5_vf_put_core_dev(pf_mdev);
		return 0;
	}

	mutex_lock(&vfmig->ctxs_lock);
	load = vfmig_take_pending_load_locked(vfmig, vf_id);
	mutex_unlock(&vfmig->ctxs_lock);

	if (!load) {
		up_read(&vfmig->lock);
		vfmig_pf_put(vfmig);
		mlx5_vf_put_core_dev(pf_mdev);
		return 0;
	}

	/*
	 * The destination VHCA has just been ENABLE_HCA'd and from the
	 * firmware's point of view is in the RUNNING state. LOAD_VHCA_STATE
	 * is only valid on a fully-suspended VHCA, so walk the VFIO mlx5
	 * destination arc RUNNING -> RUNNING_P2P -> STOP first by issuing
	 * SUSPEND_INITIATOR followed by SUSPEND_RESPONDER. Without these
	 * the firmware rejects the subsequent LOAD with bad parameter.
	 */
	mlx5_core_dbg(pf_mdev,
		      "vfmig: apply pending LOAD: vhca_id 0x%04x mkey 0x%08x size %llu\n",
		      load->vhca_id, load->mkey,
		      (unsigned long long)load->record_size);

	err = vfmig_cmd_suspend_vhca(pf_mdev, load->vhca_id,
		MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_INITIATOR);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: SUSPEND_VHCA(INITIATOR) vf %u (vhca_id 0x%04x) failed: %d\n",
			       load->vf_id, load->vhca_id, err);
		goto out_destroy;
	}
	mlx5_core_dbg(pf_mdev, "vfmig: SUSPEND(INITIATOR) ok vhca_id 0x%04x\n",
		      load->vhca_id);

	err = vfmig_cmd_suspend_vhca(pf_mdev, load->vhca_id,
		MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_RESPONDER);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: SUSPEND_VHCA(RESPONDER) vf %u (vhca_id 0x%04x) failed: %d\n",
			       load->vf_id, load->vhca_id, err);
		goto out_destroy;
	}
	mlx5_core_dbg(pf_mdev, "vfmig: SUSPEND(RESPONDER) ok vhca_id 0x%04x\n",
		      load->vhca_id);

	err = vfmig_cmd_load_vhca_state(pf_mdev, load->vhca_id, load->mkey,
					load->record_size);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: LOAD_VHCA_STATE vf %u (vhca_id 0x%04x) size %llu failed: %d\n",
			       load->vf_id, load->vhca_id,
			       (unsigned long long)load->record_size, err);
		goto out_destroy;
	}

	/*
	 * After LOAD_VHCA_STATE the firmware leaves the VHCA in the
	 * "loaded but stopped" state. Walk the VFIO state machine's
	 * STOP -> RUNNING_P2P (RESPONDER) -> RUNNING (INITIATOR) arc
	 * so subsequent FW commands (including the QUERY_ADAPTER that
	 * mlx5_function_open issues right after us) succeed.
	 */
	err_resume = vfmig_cmd_resume_vhca(pf_mdev, load->vhca_id,
		MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_RESPONDER);
	if (err_resume) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: RESUME_VHCA(RESPONDER) vf %u (vhca_id 0x%04x) failed: %d\n",
			       load->vf_id, load->vhca_id, err_resume);
		err = err ? : err_resume;
	}

	err_resume = vfmig_cmd_resume_vhca(pf_mdev, load->vhca_id,
		MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_INITIATOR);
	if (err_resume) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: RESUME_VHCA(INITIATOR) vf %u (vhca_id 0x%04x) failed: %d\n",
			       load->vf_id, load->vhca_id, err_resume);
		err = err ? : err_resume;
	}

	if (!err)
		mlx5_core_info(pf_mdev,
			       "vfmig: applied %llu bytes of LOAD state to vf %u (vhca_id 0x%04x); resumed\n",
			       (unsigned long long)load->record_size,
			       load->vf_id, load->vhca_id);

out_destroy:
	vfmig_vf_load_destroy(pf_mdev, load);
	up_read(&vfmig->lock);
	vfmig_pf_put(vfmig);
	mlx5_vf_put_core_dev(pf_mdev);
	return err;
}

/*
 * Drop any per-VF pending_load slots. Used both from
 * mlx5_vfmig_pf_cleanup() (under vfmig->lock for write, just before
 * pf_mdev = NULL) and from mlx5_device_disable_sriov() to prevent
 * stale slots from outliving the VF generation they targeted.
 *
 * Caller-supplied @hold_lock controls whether we take vfmig->lock
 * ourselves: cleanup callers already hold it for write; sriov-disable
 * callers don't and so should pass true. ctxs_lock is always taken
 * here to serialize against vfmig_install_pending_load_locked().
 */
static void vfmig_pf_drop_pending_loads_locked(struct mlx5_vfmig_pf *vfmig)
{
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_core_sriov *sriov;
	int total_vfs;
	int i;

	if (!pf_mdev)
		return;
	sriov = &pf_mdev->priv.sriov;
	if (!sriov->vfs_ctx)
		return;

	/*
	 * vfs_ctx[] is sized by sriov_init() to pci_sriov_get_totalvfs(),
	 * not by the current num_vfs. We don't have a cheap accessor for
	 * that capacity here, so use num_vfs as an upper bound: any slot
	 * outside that range can only have been left over from a stale
	 * generation we already disabled past, in which case
	 * vfmig_install_pending_load_locked would have rejected the
	 * install in the first place. Safe.
	 */
	total_vfs = sriov->num_vfs;
	mutex_lock(&vfmig->ctxs_lock);
	for (i = 0; i < total_vfs; i++) {
		struct mlx5_vfmig_vf_load *load =
			sriov->vfs_ctx[i].vfmig_pending_load;

		if (!load)
			continue;
		sriov->vfs_ctx[i].vfmig_pending_load = NULL;
		sriov->vfs_ctx[i].restored = 0;
		sriov->vfs_ctx[i].restored_vhca_id = 0;
		mutex_unlock(&vfmig->ctxs_lock);
		mlx5_core_info(pf_mdev,
			       "vfmig: dropping unconsumed pending_load for vf %d (vhca_id 0x%04x)\n",
			       i, load->vhca_id);
		vfmig_vf_load_destroy(pf_mdev, load);
		mutex_lock(&vfmig->ctxs_lock);
	}
	mutex_unlock(&vfmig->ctxs_lock);
}

void mlx5_vfmig_pf_drop_pending_loads(struct mlx5_core_dev *pf_mdev)
{
	struct mlx5_vfmig_pf *vfmig;

	if (!pf_mdev || !mlx5_core_is_pf(pf_mdev))
		return;
	vfmig = pf_mdev->priv.vfmig;
	if (!vfmig)
		return;

	vfmig_pf_get(vfmig);
	down_read(&vfmig->lock);
	if (!vfmig->dead)
		vfmig_pf_drop_pending_loads_locked(vfmig);
	up_read(&vfmig->lock);
	vfmig_pf_put(vfmig);
}

/*
 * Drop any per-VF deterministic IOVA domains. Mirror image of
 * vfmig_pf_drop_pending_loads_locked() but for vfs_ctx[].vfmig_iova_dom.
 *
 * Ordering contract (CRITICAL — get this wrong and you get a UAF in
 * vfmig_iova_free_slot on teardown):
 *
 *   The IOVA domain MUST outlive every code path on the VF side that
 *   can call vfmig_iova_alloc_slot() / vfmig_iova_free_slot().
 *   On the sriov_numvfs=0 path that means we run AFTER
 *   pci_disable_sriov() has finished -- i.e. after every VF has been
 *   fully unbound (mlx5_core remove_one -> mlx5_unregister_device ->
 *   mlx5_ib teardown -> mlx5_function_disable -> mlx5_cmd_disable ->
 *   free_cmd_page) and no caller can dereference dev->cmd.vfmig_iova_dom
 *   any more. See the comment in mlx5_sriov_disable() for the full
 *   reasoning.
 *
 *   pci_dev_get() in vfmig_iova_domain_create() pins the VF pci_dev,
 *   so iommu_detach_device() is safe to invoke even after the VF has
 *   been removed from its IOMMU group by device_del(): the iommu core
 *   short-circuits on a NULL group.
 *
 * Caller-side locking matches drop_pending_loads_locked: vfmig->lock is
 * held read or write by the caller. Inside, we splice each domain
 * pointer out of vfs_ctx[] under ctxs_lock, then drop ctxs_lock to do
 * the actual destroy (which can sleep / take iommu group locks).
 */
static void vfmig_pf_drop_iova_domains_locked(struct mlx5_vfmig_pf *vfmig)
{
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_core_sriov *sriov;
	int total_vfs;
	int i;

	if (!pf_mdev)
		return;
	sriov = &pf_mdev->priv.sriov;
	if (!sriov->vfs_ctx)
		return;

	/*
	 * As in drop_pending_loads_locked, num_vfs is the upper bound we
	 * have without a cheap accessor for the vfs_ctx[] capacity; any
	 * slot outside that range can only have been left over from a
	 * stale generation (set_tracked rejects bogus vf_ids).
	 */
	total_vfs = sriov->num_vfs;
	mutex_lock(&vfmig->ctxs_lock);
	for (i = 0; i < total_vfs; i++) {
		struct vfmig_iova_domain *dom =
			sriov->vfs_ctx[i].vfmig_iova_dom;

		if (!dom)
			continue;
		sriov->vfs_ctx[i].vfmig_iova_dom = NULL;
		sriov->vfs_ctx[i].vfmig_tracked = 0;
		mutex_unlock(&vfmig->ctxs_lock);
		mlx5_core_info(pf_mdev,
			       "vfmig: dropping iova domain for vf %d on PF teardown / sriov disable\n",
			       i);
		vfmig_iova_domain_destroy(dom);
		mutex_lock(&vfmig->ctxs_lock);
	}
	mutex_unlock(&vfmig->ctxs_lock);
}

void mlx5_vfmig_pf_drop_iova_domains(struct mlx5_core_dev *pf_mdev)
{
	struct mlx5_vfmig_pf *vfmig;

	if (!pf_mdev || !mlx5_core_is_pf(pf_mdev))
		return;
	vfmig = pf_mdev->priv.vfmig;
	if (!vfmig)
		return;

	vfmig_pf_get(vfmig);
	down_read(&vfmig->lock);
	if (!vfmig->dead)
		vfmig_pf_drop_iova_domains_locked(vfmig);
	up_read(&vfmig->lock);
	vfmig_pf_put(vfmig);
}

/* -------- module init/exit ---------------------------------------------- */

int mlx5_vfmig_module_init(void)
{
	int err;

	err = alloc_chrdev_region(&mlx5_vfmig_devt, 0,
				  MLX5_VFMIG_MAX_DEVICES, "mlx5_vfmig");
	if (err)
		return err;

	mlx5_vfmig_class = class_create("mlx5_vfmig");
	if (IS_ERR(mlx5_vfmig_class)) {
		err = PTR_ERR(mlx5_vfmig_class);
		unregister_chrdev_region(mlx5_vfmig_devt,
					 MLX5_VFMIG_MAX_DEVICES);
		return err;
	}

	return 0;
}

void mlx5_vfmig_module_exit(void)
{
	class_destroy(mlx5_vfmig_class);
	unregister_chrdev_region(mlx5_vfmig_devt, MLX5_VFMIG_MAX_DEVICES);
	ida_destroy(&mlx5_vfmig_minor_ida);
}
