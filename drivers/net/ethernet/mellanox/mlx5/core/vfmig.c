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
};

static void vfmig_load_release_resources(struct mlx5_vfmig_load_ctx *ctx);
static bool vfmig_vf_id_busy_locked(struct mlx5_vfmig_pf *vfmig, u32 vf_id);
static void vfmig_pf_drop_pending_loads_locked(struct mlx5_vfmig_pf *vfmig);

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
	 * Tear down firmware-tied resources while pf_mdev is still alive.
	 * If the PF has already been unbound (dead), pf_cleanup() did the
	 * teardown synchronously and resources_freed is already set.
	 */
	down_read(&vfmig->lock);
	if (!vfmig->dead)
		vfmig_load_release_resources(ctx);
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
	 * Read cursor in bytes covering [0..16) header + [16..16+image_size)
	 * payload. Updated under io_lock.
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
 * Drain into @ubuf for one read(). Reads compose the 16-byte FW_DATA
 * header (read_pos < HDR_SZ) followed by image_size bytes from the
 * staging pages. EOF is read_pos == HDR_SZ + image_size. Caller holds
 * vfmig->lock for read AND ctx->io_lock; pf_mdev must be alive (used
 * only via the page list, which is mdev-independent, so this remains
 * safe even if vfmig->dead -- but we still bail early on dead).
 */
static ssize_t vfmig_save_drain(struct mlx5_vfmig_save_ctx *ctx,
				char __user *ubuf, size_t count)
{
	const u64 HDR_SZ = sizeof(struct vfmig_wire_header);
	u64 total = HDR_SZ + ctx->image_size;
	size_t copied = 0;
	ssize_t err = 0;

	if (ctx->read_pos >= total)
		return 0;

	/* Header bytes first. */
	if (ctx->read_pos < HDR_SZ && count) {
		struct vfmig_wire_header hdr;
		size_t want = min_t(size_t, count, HDR_SZ - ctx->read_pos);

		vfmig_save_build_header(ctx, &hdr);
		if (copy_to_user(ubuf, ((u8 *)&hdr) + ctx->read_pos, want))
			return -EFAULT;
		ctx->read_pos += want;
		ubuf += want;
		count -= want;
		copied += want;
	}

	/* Then payload bytes. */
	while (count && ctx->read_pos < total) {
		u64 payload_off = ctx->read_pos - HDR_SZ;
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
	vfmig->dead = true;
	vfmig->pf_mdev = NULL;
	up_write(&vfmig->lock);

	device_destroy(mlx5_vfmig_class, devno);
	cdev_del(&vfmig->cdev);

	vfmig_pf_put(vfmig);
}

/* -------- VF probe-time hook -------------------------------------------- */

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
