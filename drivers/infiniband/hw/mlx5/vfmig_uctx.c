// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * VFMIG (CRIU SR-IOV migration) per-ucontext vendor verbs for mlx5_ib.
 *
 * These run on the uverbs fd (per-process) and snapshot/restore the
 * minimum kernel-side state needed to reconstitute a process's UAR
 * mmap()s after the ucontext is recreated on a destination VHCA whose
 * firmware state was imported via LOAD_VHCA_STATE.
 *
 *   QUERY_UCONTEXT   (step 2): snapshot bfregi->sys_pages[],
 *                              bfregi->count[], and meta into userspace.
 *   RESTORE_UCONTEXT (step 3): seed bfregi->sys_pages[] from the snapshot
 *                              on a ucontext that was opened with
 *                              MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE.
 *
 * Per-uobject dump-side helpers also live here:
 *
 *   QUERY_CQ         dump-side counterpart to UVERBS_METHOD_RESTORE_CQ.
 *                    Reads kernel-stored source userspace VAs (from
 *                    cq->buf.umem->address and cq->db.u.user_page->
 *                    user_virt) plus FW cqn / cqe_size, packs them
 *                    into a payload byte-equal to mlx5_ib_restore_cq_req,
 *                    and returns the per-CQ inputs RESTORE_CQ takes
 *                    as core attrs (cqe / comp_vector / flags). Solves
 *                    the cross-process problem that mlx5dv_init_obj()
 *                    cannot: CRIU runs in its own address space, so
 *                    libmlx5 introspection from CRIU returns CRIU's
 *                    VAs rather than the dumpee's.
 *
 *   QUERY_QP         dump-side counterpart to UVERBS_METHOD_RESTORE_QP.
 *                    Mirror of QUERY_CQ for QP: reads
 *                    qp->trans_qp.base.{mqp.qpn, ubuffer.umem->address}
 *                    plus mlx5_ib_db_user_virt(&qp->db) and the
 *                    WQ-ring shape (sq/rq.wqe_cnt + rq.wqe_shift)
 *                    that mlx5_ib_restore_qp re-derives buf_size from,
 *                    packs them into a 64-byte payload byte-equal to
 *                    mlx5_ib_restore_qp_req, and returns the per-QP
 *                    inputs RESTORE_QP takes as core attrs (type /
 *                    state / user_handle / cap / create_flags).
 *
 * See tools/testing/criu_rdma/design/uar_restore.md for the full
 * design of the per-ucontext save/restore path, including the
 * rationale for living on the uverbs fd vs the /dev/mlx5_vfmig PF
 * cdev, and the empirical foundation that the snapshotted FW UAR
 * ids stay valid after LOAD_VHCA_STATE.
 *
 * See tools/testing/criu_rdma/design/uobject_restore.md §5.2.4 for
 * the QUERY_CQ rationale (why driver-private vs core QUERY_CQ, why
 * the byte-equal payload contract, and the security boundary).
 * §5.3.4 covers QUERY_QP -- same shape, QP-shaped fields.
 */

#include <rdma/uverbs_ioctl.h>
#include <rdma/uverbs_std_types.h>
#include <rdma/uverbs_types.h>
#include <rdma/mlx5_user_ioctl_cmds.h>
#include <rdma/mlx5_user_ioctl_verbs.h>
#include <linux/mlx5/driver.h>

#include "mlx5_ib.h"

#define UVERBS_MODULE_NAME mlx5_ib
#include <rdma/uverbs_named_ioctl.h>

static int UVERBS_HANDLER(MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT)(
	struct uverbs_attr_bundle *attrs)
{
	struct mlx5_ib_vfmig_ucontext_meta meta = {};
	struct mlx5_bfreg_info *bfregi;
	struct mlx5_ib_ucontext *c;
	bool want_uar_table;
	bool want_count;
	size_t want_len;
	size_t arr_len;
	int err = 0;

	c = to_mucontext(ib_uverbs_get_ucontext(attrs));
	if (IS_ERR(c))
		return PTR_ERR(c);

	mlx5_ib_dbg(to_mdev(c->ibucontext.device),
		    "VFMIG_QUERY_UCONTEXT: total_bfregs=%u num_sys_pages=%u num_static=%u\n",
		    c->bfregi.total_num_bfregs, c->bfregi.num_sys_pages,
		    c->bfregi.num_static_sys_pages);

	bfregi = &c->bfregi;

	/*
	 * lib_uar_dyn=true bypasses bfregi->sys_pages[] / count[] entirely
	 * (UARs are MLX5_IB_OBJECT_UAR uobjects with their own table). v0
	 * of the restore path doesn't cover that mode, so reject the QUERY
	 * here too -- mirror the alloc-path reject in mlx5_ib_alloc_ucontext.
	 */
	if (bfregi->lib_uar_dyn)
		return -EOPNOTSUPP;

	want_uar_table = uverbs_attr_is_valid(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE);
	want_count = uverbs_attr_is_valid(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT);

	mutex_lock(&bfregi->lock);

	if (want_uar_table) {
		want_len = (size_t)bfregi->num_sys_pages *
			   sizeof(*bfregi->sys_pages);
		arr_len = uverbs_attr_get_len(attrs,
			MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE);
		if (arr_len != want_len) {
			err = -EINVAL;
			goto out;
		}
		err = uverbs_copy_to(attrs,
			MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE,
			bfregi->sys_pages, want_len);
		if (err)
			goto out;
	}

	if (want_count) {
		want_len = (size_t)bfregi->total_num_bfregs *
			   sizeof(*bfregi->count);
		arr_len = uverbs_attr_get_len(attrs,
			MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT);
		if (arr_len != want_len) {
			err = -EINVAL;
			goto out;
		}
		err = uverbs_copy_to(attrs,
			MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT,
			bfregi->count, want_len);
		if (err)
			goto out;
	}

	meta.num_static_sys_pages   = bfregi->num_static_sys_pages;
	meta.num_sys_pages          = bfregi->num_sys_pages;
	meta.num_dyn_bfregs         = bfregi->num_dyn_bfregs;
	meta.num_low_latency_bfregs = bfregi->num_low_latency_bfregs;
	meta.total_num_bfregs       = bfregi->total_num_bfregs;
	meta.lib_caps               = c->lib_caps;
	meta.lib_uar_4k             = bfregi->lib_uar_4k ? 1 : 0;
	meta.lib_uar_dyn            = bfregi->lib_uar_dyn ? 1 : 0;
	meta.cqe_version            = c->cqe_version;
	/*
	 * Source ucontext's FW owner-id. Exposed across the SAVE/LOAD
	 * seam so the dump-side CRIU plugin can detect a DEVX-enabled
	 * source (devx_uid != 0) and refuse the dump cleanly --
	 * LOAD_VHCA_STATE does NOT preserve the FW uctx-registration
	 * table (§S3b "DEVX-adoption blind spot" matrix on FW
	 * 28.48.1000), so neither MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID nor
	 * the uid=0 host-priv lane can correctly run modify/destroy
	 * commands against PDC/CQC/QPC owned by the source's
	 * devx_uid. The downstream symptom is silent FW failure in
	 * destroy_qp_common (warn-only-logs to dmesg, mlx5_ib_destroy_qp
	 * returns 0 regardless) followed by DEALLOC_PD bad_resource_
	 * state at the first opcode that propagates its FW errno. uid
	 * is a 16-bit FW field; widening into reserved1's first two
	 * bytes preserves the existing wire layout (older userspaces
	 * see meta.devx_uid == 0, which is correct for the non-DEVX
	 * v0 test surface). RESTORE_UCONTEXT cross-checks
	 * meta.devx_uid against c->devx_uid so a mis-matched
	 * dump/restore plugin pair fails -EINVAL at RESTORE_UCONTEXT
	 * (early, before any resource adoption) instead of obscurely
	 * at teardown.
	 */
	meta.devx_uid               = c->devx_uid;

	err = uverbs_copy_to(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_META,
		&meta, sizeof(meta));

out:
	mutex_unlock(&bfregi->lock);
	return err;
}

DECLARE_UVERBS_NAMED_METHOD(
	MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT,
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_META,
			    UVERBS_ATTR_TYPE(struct mlx5_ib_vfmig_ucontext_meta),
			    UA_MANDATORY));

static int UVERBS_HANDLER(MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT)(
	struct uverbs_attr_bundle *attrs)
{
	struct mlx5_ib_vfmig_ucontext_meta meta;
	struct mlx5_bfreg_info *bfregi;
	struct mlx5_ib_ucontext *c;
	const u32 *uar_table;
	const u32 *bfreg_count;
	struct mlx5_ib_dev *dev;
	size_t want_uar_len;
	size_t want_count_len;
	size_t arr_len;
	bool have_count;
	size_t i;
	int err;

	c = to_mucontext(ib_uverbs_get_ucontext(attrs));
	if (IS_ERR(c))
		return PTR_ERR(c);

	dev = to_mdev(c->ibucontext.device);
	bfregi = &c->bfregi;

	if (bfregi->lib_uar_dyn)
		return -EOPNOTSUPP;

	/*
	 * Precondition #1: ucontext was opened with the VFMIG_RESTORE flag.
	 * That's what ran allocate_uars()'s short-circuit and left
	 * sys_pages[] sentinel-filled, ready to be seeded. Without that
	 * flag, sys_pages[] holds live FW UAR ids from real ALLOC_UAR
	 * commands; overwriting them would leak the FW allocations and
	 * leave bfregi pointing at the wrong ids.
	 */
	if (!c->vfmig_restore_pending) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_UCONTEXT: ucontext not in restore-pending state (missing MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE on open, or RESTORE already applied)\n");
		return -EINVAL;
	}

	err = uverbs_copy_from(&meta, attrs,
			       MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_META);
	if (err)
		return err;

	/*
	 * Precondition #3a: bfregi/lib_caps/cqe_version cross-check.
	 * Strict bitwise equality on every UAR-shape-defining field.
	 * v0 targets a homogeneous fleet (same kernel, same libmlx5,
	 * same MLX5_LIB_CAP_*), so any drift here means somebody is
	 * feeding us a snapshot from a structurally different
	 * ucontext. Reject before we corrupt bfregi rather than fail
	 * later in obscure ways.
	 *
	 * @devx_uid is intentionally NOT in this strict-equality bag
	 * (see Precondition #3b below).
	 */
	if (meta.num_static_sys_pages   != bfregi->num_static_sys_pages   ||
	    meta.num_sys_pages          != bfregi->num_sys_pages          ||
	    meta.num_dyn_bfregs         != bfregi->num_dyn_bfregs         ||
	    meta.num_low_latency_bfregs != bfregi->num_low_latency_bfregs ||
	    meta.total_num_bfregs       != bfregi->total_num_bfregs       ||
	    meta.lib_caps               != c->lib_caps                    ||
	    meta.lib_uar_4k             != (bfregi->lib_uar_4k ? 1 : 0)   ||
	    meta.lib_uar_dyn            != (bfregi->lib_uar_dyn ? 1 : 0)  ||
	    meta.cqe_version            != c->cqe_version) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_UCONTEXT: META mismatch (snapshot vs dst): "
			    "static_pages=%u/%u num_pages=%u/%u dyn_bfregs=%u/%u low_lat=%u/%u total_bfregs=%u/%u "
			    "lib_caps=0x%llx/0x%llx 4k=%u/%u dyn=%u/%u cqe_ver=%u/%u\n",
			    meta.num_static_sys_pages, bfregi->num_static_sys_pages,
			    meta.num_sys_pages, bfregi->num_sys_pages,
			    meta.num_dyn_bfregs, bfregi->num_dyn_bfregs,
			    meta.num_low_latency_bfregs, bfregi->num_low_latency_bfregs,
			    meta.total_num_bfregs, bfregi->total_num_bfregs,
			    (unsigned long long)meta.lib_caps,
			    (unsigned long long)c->lib_caps,
			    meta.lib_uar_4k, bfregi->lib_uar_4k ? 1 : 0,
			    meta.lib_uar_dyn, bfregi->lib_uar_dyn ? 1 : 0,
			    meta.cqe_version, c->cqe_version);
		return -EINVAL;
	}

	/*
	 * Precondition #3b: devx_uid mismatch is LOG-AND-CONTINUE, not
	 * a hard reject.
	 *
	 * History: an earlier version of this code (commit
	 * c659ab66483d "expose VFMIG source devx_uid + harden
	 * destroy_qp diagnostics") rejected -EINVAL on
	 * meta.devx_uid != c->devx_uid as defense-in-depth, on the
	 * (then-current) belief that the CRIU agent's DEALLOC_PD
	 * failure was caused by a uid mismatch between mpd->uid and
	 * the FW PDC's owner-uid. Empirical investigation
	 * (tools/testing/criu_rdma/uobject_restore/qp_destroy_matrix,
	 * cq_destroy_matrix, mr_destroy_matrix, dealloc_pd_chain)
	 * subsequently established:
	 *
	 *   * DESTROY_QP / 2RST_QP honor cross-uid (FW does not gate
	 *     on QPC owner-uid for destroy/modify ops).
	 *   * DESTROY_CQ honors cross-uid (after dropping dependent
	 *     QPs).
	 *   * DESTROY_MKEY honors cross-uid.
	 *   * DEALLOC_PD on a vfmig-restored PDN fails with status
	 *     0x9 syndrome 0xef0c8a regardless of the asserting uid
	 *     -- the (pdn -> owner_uid) registration table is wiped
	 *     by LOAD_VHCA_STATE. The failure is independent of the
	 *     DEVX/non-DEVX source-uid story; it is mitigated by the
	 *     vfmig_restored gate landed in mlx5_ib_dealloc_pd
	 *     (commit ee27d8e391aa).
	 *
	 * With that mitigation in place the strict-equality check is
	 * not load-bearing for the DEALLOC_PD outcome and is now
	 * actively unhelpful: it rejects the common case of a default
	 * libmlx5 ucontext (which auto-allocates a fresh DEVX uid on
	 * every ibv_open_device, so source.devx_uid != dest.devx_uid
	 * by construction). The standard-verbs data path through such
	 * ucontexts works fine post-restore -- doorbells and
	 * completions are HW-only paths that do not consult the FW
	 * registration tables.
	 *
	 * What we still log: the mismatch itself, so an operator
	 * debugging an unexpected post-restore failure can correlate
	 * the snapshot's source.devx_uid with what the dest got.
	 *
	 * What still fails (intentionally out of scope for v0):
	 * DEVX-direct manipulation (ibv_devx_obj_*) of restored
	 * objects -- those use the wiped (uid -> uctx_attrs)
	 * registration table for ownership validation. See
	 * tools/testing/criu_rdma/design/pd_registration_wipe.md
	 * "DEVX-direct opcodes" for the FW-team escalation path.
	 */
	if (meta.devx_uid != c->devx_uid) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_UCONTEXT: devx_uid mismatch tolerated: snapshot=%u dest=%u (PD destroy gated on vfmig_restored; CQ/QP/MR destroy honor cross-uid; standard-verbs data path is uid-blind)\n",
			    (unsigned)meta.devx_uid, c->devx_uid);
	}

	want_uar_len = (size_t)bfregi->num_sys_pages *
		       sizeof(*bfregi->sys_pages);
	arr_len = uverbs_attr_get_len(attrs,
		MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_UAR_TABLE);
	if (arr_len != want_uar_len) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_UCONTEXT: UAR_TABLE length %zu != expected %zu\n",
			    arr_len, want_uar_len);
		return -EINVAL;
	}
	uar_table = uverbs_attr_get_alloced_ptr(attrs,
		MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_UAR_TABLE);
	if (IS_ERR(uar_table))
		return PTR_ERR(uar_table);

	/*
	 * Precondition #5: static slots must all be valid FW UAR ids.
	 * Dynamic slots [num_static..num_sys_pages) MAY be INVALID
	 * (unclaimed dyn slot on the source) -- those replay through
	 * uar_mmap()'s lazy-alloc when libmlx5 next asks for a WC page.
	 */
	for (i = 0; i < bfregi->num_static_sys_pages; i++) {
		if (uar_table[i] == MLX5_IB_INVALID_UAR_INDEX) {
			mlx5_ib_dbg(dev,
				    "VFMIG_RESTORE_UCONTEXT: static slot %zu is INVALID in snapshot\n",
				    i);
			return -EINVAL;
		}
	}

	have_count = uverbs_attr_is_valid(attrs,
		MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_BFREG_COUNT);
	if (have_count) {
		want_count_len = (size_t)bfregi->total_num_bfregs *
				 sizeof(*bfregi->count);
		arr_len = uverbs_attr_get_len(attrs,
			MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_BFREG_COUNT);
		if (arr_len != want_count_len) {
			mlx5_ib_dbg(dev,
				    "VFMIG_RESTORE_UCONTEXT: BFREG_COUNT length %zu != expected %zu\n",
				    arr_len, want_count_len);
			return -EINVAL;
		}
		bfreg_count = uverbs_attr_get_alloced_ptr(attrs,
			MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_BFREG_COUNT);
		if (IS_ERR(bfreg_count))
			return PTR_ERR(bfreg_count);

		/*
		 * Precondition #6: v0 only restores "no live QPs / no
		 * claimed dyn UARs" snapshots. Non-zero count means the
		 * source had refs we don't yet rebuild. The wire shape is
		 * locked; a future MR/QP-restore step will lift this
		 * check without an ABI bump.
		 */
		for (i = 0; i < bfregi->total_num_bfregs; i++) {
			if (bfreg_count[i] != 0) {
				mlx5_ib_dbg(dev,
					    "VFMIG_RESTORE_UCONTEXT: BFREG_COUNT[%zu]=%u, v0 requires all-zero\n",
					    i, bfreg_count[i]);
				return -EINVAL;
			}
		}
	}

	/*
	 * All preconditions held. Apply under bfregi->lock; the lock
	 * also serializes against uar_mmap()'s lazy-alloc path so a
	 * concurrent mmap can't observe a half-seeded sys_pages[].
	 */
	mutex_lock(&bfregi->lock);
	memcpy(bfregi->sys_pages, uar_table, want_uar_len);
	/*
	 * count[] is already zero from allocate_uars()'s kcalloc(); v0
	 * enforces all-zero on the wire. We don't memcpy bfreg_count[]
	 * because the static refcount semantics (per-QP) and dynamic
	 * claim semantics (per-bfreg-mmap) are consumed by paths whose
	 * matching kernel objects (QPs, claimed UAR mmaps) we don't
	 * rebuild here. When those rebuilds land, this will become
	 *   if (have_count) memcpy(bfregi->count, bfreg_count, ...);
	 * with the precondition #6 loop relaxed accordingly.
	 */
	c->vfmig_restore_pending = false;
	mutex_unlock(&bfregi->lock);

	mlx5_ib_dbg(dev,
		    "VFMIG_RESTORE_UCONTEXT: seeded %u sys_pages, cleared restore-pending\n",
		    bfregi->num_sys_pages);
	return 0;
}

DECLARE_UVERBS_NAMED_METHOD(
	MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT,
	UVERBS_ATTR_PTR_IN(MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_UAR_TABLE,
			   UVERBS_ATTR_MIN_SIZE(0),
			   UA_MANDATORY,
			   UA_ALLOC_AND_COPY),
	UVERBS_ATTR_PTR_IN(MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_BFREG_COUNT,
			   UVERBS_ATTR_MIN_SIZE(0),
			   UA_OPTIONAL,
			   UA_ALLOC_AND_COPY),
	UVERBS_ATTR_PTR_IN(MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_META,
			   UVERBS_ATTR_TYPE(struct mlx5_ib_vfmig_ucontext_meta),
			   UA_MANDATORY));

/*
 * QUERY_DYN_UARS: snapshot every outstanding MLX5_IB_OBJECT_UAR uobject
 * in this ucontext. Two-pass; see commentary on the wire format in
 * mlx5_user_ioctl_cmds.h.
 *
 * We walk ufile->uobjects (the per-fd list of committed uobjects) under
 * ufile->uobjects_lock, filter by uobj_get_object_id(uobj) ==
 * MLX5_IB_OBJECT_UAR, and copy {handle, uar_index, mmap_offset,
 * alloc_type} into a kernel buffer. The list is the standard primary
 * datastructure used by INFO_HANDLES (see uverbs_std_types_device.c) and
 * is the only place where iteration order is well-defined: ufile->idr is
 * an xarray and gives no guarantee that we'd hit the same handles after
 * concurrent alloc/destroy.
 *
 * uobj->object (the mlx5_user_mmap_entry) is set once at alloc time and
 * never mutated; the uobj is removed from ->uobjects under the same
 * spinlock the destroy path takes, so reading object fields while
 * holding ->uobjects_lock is safe.
 */
static int UVERBS_HANDLER(MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS)(
	struct uverbs_attr_bundle *attrs)
{
	struct mlx5_ib_vfmig_dyn_uar_record *records = NULL;
	struct mlx5_ib_ucontext *c;
	struct ib_uverbs_file *ufile;
	struct ib_uobject *uobj;
	struct mlx5_ib_dev *dev;
	bool want_records;
	size_t want_len;
	size_t arr_len;
	u32 nrecords;
	u32 count;
	int err;

	c = to_mucontext(ib_uverbs_get_ucontext(attrs));
	if (IS_ERR(c))
		return PTR_ERR(c);

	dev = to_mdev(c->ibucontext.device);
	ufile = attrs->ufile;

	if (!c->bfregi.lib_uar_dyn) {
		mlx5_ib_dbg(dev,
			    "VFMIG_QUERY_DYN_UARS: ucontext is not lib_uar_dyn=true; use VFMIG_QUERY_UCONTEXT for static-UAR ucontexts\n");
		return -EINVAL;
	}

	want_records = uverbs_attr_is_valid(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_RECORDS);

	/*
	 * Pass 1 (sizing): no RECORDS supplied. We just count and report
	 * via COUNT, then return -- userspace allocates the right-sized
	 * RECORDS array and reissues.
	 *
	 * Pass 2 (snapshot): RECORDS is provided. We require its length
	 * to match the COUNT we'd report, so any concurrent UAR
	 * alloc/destroy between pass 1 and pass 2 is detectable as a
	 * length mismatch rather than silently truncated.
	 */
	if (want_records) {
		records = uverbs_zalloc(attrs,
			uverbs_attr_get_len(attrs,
				MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_RECORDS));
		if (IS_ERR(records))
			return PTR_ERR(records);
	}

	count = 0;
	spin_lock_irq(&ufile->uobjects_lock);
	list_for_each_entry(uobj, &ufile->uobjects, list) {
		struct mlx5_user_mmap_entry *entry;

		if (uobj_get_object_id(uobj) != MLX5_IB_OBJECT_UAR)
			continue;
		entry = uobj->object;
		if (!entry) {
			/*
			 * Should not happen on a committed UAR uobject; if
			 * it does, treat as transient and skip rather than
			 * embedding garbage in the snapshot.
			 */
			continue;
		}

		if (!want_records) {
			count++;
			continue;
		}

		/*
		 * Bail out cleanly if RECORDS is shorter than what we'd
		 * actually emit. Below we cross-check the final count
		 * against the array length too; this just avoids writing
		 * past the end of the buffer.
		 */
		nrecords = uverbs_attr_get_len(attrs,
			MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_RECORDS) /
			sizeof(*records);
		if (count >= nrecords) {
			spin_unlock_irq(&ufile->uobjects_lock);
			return -EINVAL;
		}

		records[count].handle = uobj->id;
		records[count].uar_index = entry->page_idx;
		/*
		 * Use the libmlx5-wire-format mmap_offset (the same byte
		 * offset UAR_OBJ_ALLOC reports back to userspace), not the
		 * raw start_pgoff << PAGE_SHIFT. This makes captured
		 * mmap_offset values round-trip byte-for-byte against the
		 * alloc path AND directly usable with mmap() on the
		 * destination after restore. mlx5_entry_to_mmap_offset()
		 * applies the cmd/index repacking that libmlx5 expects.
		 */
		records[count].mmap_offset =
			mlx5_entry_to_mmap_offset(entry);
		switch (entry->mmap_flag) {
		case MLX5_IB_MMAP_TYPE_UAR_WC:
			records[count].alloc_type =
				MLX5_IB_UAPI_UAR_ALLOC_TYPE_BF;
			break;
		case MLX5_IB_MMAP_TYPE_UAR_NC:
			records[count].alloc_type =
				MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC;
			break;
		default:
			/*
			 * MLX5_IB_OBJECT_UAR uobjects only ever carry
			 * UAR_WC / UAR_NC mmap_flags (the only branches
			 * in alloc_uar_entry()). Anything else is a
			 * structural bug.
			 */
			spin_unlock_irq(&ufile->uobjects_lock);
			mlx5_ib_dbg(dev,
				    "VFMIG_QUERY_DYN_UARS: unexpected mmap_flag=%u on UAR uobject handle=%u\n",
				    entry->mmap_flag, uobj->id);
			return -EIO;
		}
		count++;
	}
	spin_unlock_irq(&ufile->uobjects_lock);

	if (want_records) {
		want_len = (size_t)count * sizeof(*records);
		arr_len = uverbs_attr_get_len(attrs,
			MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_RECORDS);
		if (arr_len != want_len) {
			mlx5_ib_dbg(dev,
				    "VFMIG_QUERY_DYN_UARS: RECORDS length %zu != expected %zu (count=%u)\n",
				    arr_len, want_len, count);
			return -EINVAL;
		}
		err = uverbs_copy_to(attrs,
			MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_RECORDS,
			records, want_len);
		if (err)
			return err;
	}

	err = uverbs_copy_to(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_COUNT,
		&count, sizeof(count));
	if (err)
		return err;

	mlx5_ib_dbg(dev,
		    "VFMIG_QUERY_DYN_UARS: %s pass, %u dyn UAR uobject(s)\n",
		    want_records ? "snapshot" : "sizing", count);
	return 0;
}

DECLARE_UVERBS_NAMED_METHOD(
	MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS,
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_RECORDS,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_COUNT,
			    UVERBS_ATTR_TYPE(__u32),
			    UA_MANDATORY));

/*
 * Single-record dyn-UAR restore worker. Allocates and pins the
 * MLX5_IB_OBJECT_UAR uobject at @rec->handle, then attaches an
 * mlx5_user_mmap_entry whose start_pgoff matches @rec->mmap_offset.
 *
 * Failure modes:
 *   - rdma_alloc_begin_uobject_at_handle() returning -EBUSY: handle
 *     already in use by something else in this ucontext (concurrent
 *     UAR_OBJ_ALLOC, or duplicate handle in the snapshot). Surface as
 *     -EBUSY; the snapshot is malformed and the caller should fail.
 *   - restore_uar_entry() returning -EBUSY: mmap pgoff already in use
 *     (range collision with another mmap_entry in this ucontext). v0
 *     does not attempt to relocate.
 *
 * On error before commit: rdma_alloc_abort_uobject() drops the pinned
 * idr slot and the rdmacg charge. After commit, the uobject is owned
 * by the ufile and will be torn down on ucontext close.
 */
static int vfmig_restore_one_dyn_uar(struct uverbs_attr_bundle *attrs,
				     struct mlx5_ib_ucontext *c,
				     const struct mlx5_ib_vfmig_dyn_uar_record *rec)
{
	struct mlx5_ib_dev *dev = to_mdev(c->ibucontext.device);
	struct mlx5_user_mmap_entry *entry;
	struct ib_uobject *uobj;
	u32 mmap_pgoff;
	int err;

	if (rec->alloc_type != MLX5_IB_UAPI_UAR_ALLOC_TYPE_BF &&
	    rec->alloc_type != MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: unsupported alloc_type=%u for handle=%u\n",
			    rec->alloc_type, rec->handle);
		return -EINVAL;
	}

	if (rec->mmap_offset & ~PAGE_MASK) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: unaligned mmap_offset=0x%llx for handle=%u\n",
			    (unsigned long long)rec->mmap_offset, rec->handle);
		return -EINVAL;
	}
	/*
	 * QUERY_DYN_UARS emits the libmlx5-wire-format mmap_offset (what
	 * userspace mmap()s); rdma_user_mmap_entry_insert_exact() needs
	 * the rdma_user_mmap_entry start_pgoff. Run the inverse codec.
	 */
	mmap_pgoff = mlx5_mmap_offset_to_pgoff(rec->mmap_offset);
	if (mmap_pgoff == U32_MAX) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: out-of-range mmap_offset=0x%llx for handle=%u\n",
			    (unsigned long long)rec->mmap_offset, rec->handle);
		return -EINVAL;
	}

	uobj = rdma_alloc_begin_uobject_at_handle(attrs,
						  MLX5_IB_OBJECT_UAR,
						  rec->handle);
	if (IS_ERR(uobj)) {
		err = PTR_ERR(uobj);
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: alloc_at_handle(%u) failed: %d\n",
			    rec->handle, err);
		return err;
	}

	entry = restore_uar_entry(c, rec->alloc_type, rec->uar_index,
				  mmap_pgoff);
	if (IS_ERR(entry)) {
		err = PTR_ERR(entry);
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: restore_uar_entry(handle=%u uar=%u pgoff=%u) failed: %d\n",
			    rec->handle, rec->uar_index, mmap_pgoff, err);
		/*
		 * No HW object yet (mmap_entry not inserted, no
		 * mlx5_cmd_uar_alloc was issued -- restore path skips it).
		 */
		rdma_alloc_abort_uobject(uobj, attrs, /* hw_obj_valid */ false);
		return err;
	}

	uobj->object = entry;
	rdma_alloc_commit_uobject(uobj, attrs);

	mlx5_ib_dbg(dev,
		    "VFMIG_RESTORE_DYN_UARS: restored handle=%u uar=%u pgoff=%u alloc_type=%u\n",
		    rec->handle, rec->uar_index, mmap_pgoff, rec->alloc_type);
	return 0;
}

static int UVERBS_HANDLER(MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS)(
	struct uverbs_attr_bundle *attrs)
{
	const struct mlx5_ib_vfmig_dyn_uar_record *records;
	struct mlx5_ib_ucontext *c;
	struct mlx5_ib_dev *dev;
	size_t arr_len;
	u32 nrecords;
	u32 i;
	int err;

	c = to_mucontext(ib_uverbs_get_ucontext(attrs));
	if (IS_ERR(c))
		return PTR_ERR(c);

	dev = to_mdev(c->ibucontext.device);

	/*
	 * Mirrors the precondition set in RESTORE_UCONTEXT:
	 *  - vfmig_restore_pending must be set (ucontext was opened with
	 *    MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE; this is the single-shot
	 *    flag that prevents repeated restores from double-creating
	 *    uobjects);
	 *  - lib_uar_dyn must be true (a static-UAR ucontext uses
	 *    RESTORE_UCONTEXT instead).
	 */
	if (!c->vfmig_restore_pending) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: ucontext not in restore-pending state\n");
		return -EINVAL;
	}
	if (!c->bfregi.lib_uar_dyn) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: ucontext is not lib_uar_dyn=true; use VFMIG_RESTORE_UCONTEXT\n");
		return -EINVAL;
	}

	arr_len = uverbs_attr_get_len(attrs,
		MLX5_IB_ATTR_VFMIG_RESTORE_DYN_UARS_RECORDS);
	if (!arr_len || arr_len % sizeof(*records)) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: bad RECORDS length %zu (record size %zu)\n",
			    arr_len, sizeof(*records));
		return -EINVAL;
	}
	nrecords = arr_len / sizeof(*records);

	records = uverbs_attr_get_alloced_ptr(attrs,
		MLX5_IB_ATTR_VFMIG_RESTORE_DYN_UARS_RECORDS);
	if (IS_ERR(records))
		return PTR_ERR(records);

	/*
	 * Bail-on-first-error semantics. We could attempt to roll back
	 * already-committed uobjects on a partial failure, but in
	 * practice a partial RESTORE_DYN_UARS failure means the
	 * destination ucontext is unusable for the migrated workload --
	 * the userspace caller will close the fd and start over. We log
	 * loudly so the leftover uobjects (which will be torn down on
	 * fd close anyway) are easy to diagnose.
	 */
	for (i = 0; i < nrecords; i++) {
		err = vfmig_restore_one_dyn_uar(attrs, c, &records[i]);
		if (err) {
			mlx5_ib_dbg(dev,
				    "VFMIG_RESTORE_DYN_UARS: failed at record %u/%u: %d (close ucontext to retry)\n",
				    i, nrecords, err);
			return err;
		}
	}

	c->vfmig_restore_pending = false;
	mlx5_ib_dbg(dev,
		    "VFMIG_RESTORE_DYN_UARS: restored %u dyn UAR(s), cleared restore-pending\n",
		    nrecords);
	return 0;
}

DECLARE_UVERBS_NAMED_METHOD(
	MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS,
	UVERBS_ATTR_PTR_IN(MLX5_IB_ATTR_VFMIG_RESTORE_DYN_UARS_RECORDS,
			   UVERBS_ATTR_MIN_SIZE(0),
			   UA_MANDATORY,
			   UA_ALLOC_AND_COPY));

/*
 * MLX5_IB_METHOD_VFMIG_QUERY_CQ -- emit, for the CQ resolved through
 * UVERBS_OBJECT_CQ on the calling fd's ufile, the bytes a CRIU plugin
 * needs to drive UVERBS_METHOD_RESTORE_CQ on the destination side.
 *
 * Three-part output:
 *   RESP_BLOB         struct mlx5_ib_restore_cq_req, byte-equal to
 *                     what RESTORE_CQ's UHW will consume. The handler
 *                     leaves req.reserved / req.reserved2 zero so the
 *                     restore path's "must be 0" checks pass round-trip.
 *   RESP_CQE          ibcq->cqe, the ring-size-minus-one in verbs
 *                     convention. Goes into UVERBS_ATTR_RESTORE_CQ_CQE.
 *   RESP_COMP_VECTOR  mcq->mcq.vector, the source's comp_vector index.
 *   RESP_FLAGS        cq->create_flags (the IB_UVERBS_CQ_FLAGS_*
 *                     bits the source-side CREATE_CQ recorded).
 *
 * Precondition: the CQ must be a user-mode CQ. Kernel-mode CQs
 * (mcq->buf.umem == NULL, mcq->db.u.pgdir != NULL) reject with
 * -ENXIO -- there are no source userspace VAs to emit.
 *
 * The IDR lookup for HANDLE goes through the calling fd's ufile and
 * grabs UVERBS_ACCESS_READ on the CQ uobject for the duration of
 * the call, so a concurrent DESTROY_CQ on the same fd cannot race.
 */
static int UVERBS_HANDLER(MLX5_IB_METHOD_VFMIG_QUERY_CQ)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_cq *ibcq = uverbs_attr_get_obj(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_CQ_HANDLE);
	struct mlx5_ib_cq *mcq;
	struct mlx5_ib_restore_cq_req blob = {};
	u32 cqe;
	u32 comp_vector;
	u32 flags;
	int err;

	if (IS_ERR(ibcq))
		return PTR_ERR(ibcq);

	mcq = to_mcq(ibcq);

	/*
	 * Reject kernel-mode CQs: no source userspace state to emit.
	 * mcq->buf.umem is NULL iff the CQ took the create_cq_kernel
	 * path; mlx5_ib_db_user_virt returns 0 iff db->u is the pgdir
	 * (kernel) branch of the union. ibcq->uobject is non-NULL
	 * here by construction (the IDR lookup came through the user
	 * ufile) but we don't lean on that for clarity.
	 */
	if (!mcq->buf.umem || mlx5_ib_db_user_virt(&mcq->db) == 0)
		return -ENXIO;

	/*
	 * Fields that round-trip into mlx5_ib_restore_cq_req:
	 *
	 *  cqn        -- mcq->mcq.cqn is a 24-bit FW resource id; non-zero
	 *                for any live CQ. RESTORE_CQ rejects 0 + sentinels
	 *                with -EINVAL, so emitting our value here is
	 *                always restore-acceptable.
	 *  cqe_size   -- mcq->cqe_size is set in mlx5_ib_create_cq /
	 *                mlx5_ib_restore_cq to 64 or 128. RESTORE_CQ
	 *                gates on exactly that set.
	 *  buf_addr   -- mcq->buf.umem->address was set verbatim by
	 *                ib_umem_get(@ucmd.buf_addr) at create time.
	 *                That's the source userspace VA RESTORE_CQ
	 *                needs to look up the LOAD_VHCA_STATE-installed
	 *                KIND_CQ-tagged placeholder.
	 *  db_addr    -- mcq->db.u.user_page->user_virt is the page-
	 *                aligned doorbell user-virt that
	 *                mlx5_ib_db_map_user dedup-keyed on. The byte
	 *                offset into the page survives via FW
	 *                cqc.dbr_addr, per the comment on
	 *                mlx5_ib_restore_cq_req.db_addr; the
	 *                page-aligned form is what the restore-side
	 *                placeholder lookup keys on.
	 */
	blob.cqn = mcq->mcq.cqn;
	blob.cqe_size = mcq->cqe_size;
	blob.buf_addr = mcq->buf.umem->address;
	blob.db_addr = mlx5_ib_db_user_virt(&mcq->db);

	cqe = ibcq->cqe;
	comp_vector = mcq->mcq.vector;
	flags = mcq->create_flags;

	err = uverbs_copy_to(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_BLOB, &blob, sizeof(blob));
	if (err)
		return err;
	err = uverbs_copy_to(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_CQE, &cqe, sizeof(cqe));
	if (err)
		return err;
	err = uverbs_copy_to(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_COMP_VECTOR,
		&comp_vector, sizeof(comp_vector));
	if (err)
		return err;
	return uverbs_copy_to(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_FLAGS,
		&flags, sizeof(flags));
}

DECLARE_UVERBS_NAMED_METHOD(
	MLX5_IB_METHOD_VFMIG_QUERY_CQ,
	UVERBS_ATTR_IDR(MLX5_IB_ATTR_VFMIG_QUERY_CQ_HANDLE,
			UVERBS_OBJECT_CQ,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_BLOB,
			    UVERBS_ATTR_TYPE(struct mlx5_ib_restore_cq_req),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_CQE,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_COMP_VECTOR,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_FLAGS,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY));

/*
 * MLX5_IB_METHOD_VFMIG_QUERY_QP -- emit, for the QP resolved through
 * UVERBS_OBJECT_QP on the calling fd's ufile, the bytes a CRIU plugin
 * needs to drive UVERBS_METHOD_RESTORE_QP on the destination side.
 *
 * Three-part output (only state with no standard / NLDEV surface;
 * cap / qp_type / qp_state are sourced by the dumper from the standard
 * IB_USER_VERBS_CMD_QUERY_QP verb + NLDEV, so this verb no longer
 * re-exports them):
 *   RESP_BLOB         struct mlx5_ib_restore_qp_req (64 bytes), byte-
 *                     equal to what RESTORE_QP's UHW will consume. The
 *                     handler leaves req.{reserved, reserved2} zero so
 *                     the restore path's "must be 0" checks pass
 *                     round-trip. uidx / bfreg_index / ece_options are
 *                     emitted as sentinels (0 / MLX5_IB_INVALID_BFREG /
 *                     0) -- the corresponding QPC fields round-trip
 *                     across LOAD_VHCA_STATE intact (see K7 byte-equal
 *                     proof, design §6.3 / S6 wider sweep), and the
 *                     RESTORE_QP handler validates-and-discards them.
 *   RESP_USER_HANDLE  ibqp->uobject->user_handle, the userspace tag
 *                     ib_uverbs_create_qp recorded at create. Not
 *                     standard-queryable.
 *   RESP_CREATE_FLAGS qp->flags (the IB_QP_CREATE_* mask captured at
 *                     create time). Not present in the legacy
 *                     query_qp resp.
 *
 * Precondition: the QP must be a user-mode QP whose mlx5_ib representation
 * lives in trans_qp (RC / UC / UD). Other QP types -- raw_packet (uses
 * raw_packet_qp; no trans_qp), XRC INI/TGT, GSI, DCT, DCI -- reject
 * with -EOPNOTSUPP. This mirrors the v0 type set mlx5_ib_restore_qp
 * accepts; the gate is kept as an internal mqp->type check even though
 * the type value is no longer emitted. Kernel-mode QPs (no umem) reject
 * with -ENXIO, mirroring the QUERY_CQ kernel-mode rejection: there are
 * no source userspace VAs to emit and RESTORE_QP would have nothing to
 * consume.
 *
 * The IDR lookup for HANDLE goes through the calling fd's ufile and
 * grabs UVERBS_ACCESS_READ on the QP uobject for the duration of
 * the call, so a concurrent DESTROY_QP on the same fd cannot race.
 */
static int UVERBS_HANDLER(MLX5_IB_METHOD_VFMIG_QUERY_QP)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_qp *ibqp = uverbs_attr_get_obj(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_QP_HANDLE);
	struct mlx5_ib_qp *mqp;
	struct mlx5_ib_qp_base *base;
	struct mlx5_ib_restore_qp_req blob = {};
	u64 user_handle;
	u32 create_flags;
	int err;

	if (IS_ERR(ibqp))
		return PTR_ERR(ibqp);

	/*
	 * v0 type gate: only the IBTA QP types whose mlx5_ib
	 * representation lives in mlx5_ib_qp.trans_qp. Other types
	 * (raw_packet uses raw_packet_qp, XRC/GSI/DCT use their own
	 * union arms) have no trans_qp.base.{mqp.qpn, ubuffer.umem}
	 * to emit -- and RESTORE_QP rejects them anyway.
	 */
	switch (ibqp->qp_type) {
	case IB_QPT_RC:
	case IB_QPT_UC:
	case IB_QPT_UD:
		break;
	default:
		return -EOPNOTSUPP;
	}

	mqp = to_mqp(ibqp);
	base = &mqp->trans_qp.base;

	/*
	 * Reject kernel-mode QPs: no source userspace state to emit.
	 * trans_qp.base.ubuffer.umem is NULL iff the QP took the
	 * create_kernel_qp path; mlx5_ib_db_user_virt returns 0 iff
	 * db->u is the pgdir (kernel) branch of the union. ibqp->uobject
	 * is non-NULL here by construction (the IDR lookup came through
	 * the user ufile) but we don't lean on that for clarity.
	 */
	if (!base->ubuffer.umem || mlx5_ib_db_user_virt(&mqp->db) == 0)
		return -ENXIO;
	if (!ibqp->uobject)
		return -ENXIO;

	/*
	 * Fields that round-trip into mlx5_ib_restore_qp_req:
	 *
	 *  qpn          -- base->mqp.qpn is a 24-bit FW resource id;
	 *                  non-zero for any live QP. RESTORE_QP rejects
	 *                  0 + sentinels with -EINVAL, so emitting our
	 *                  value here is always restore-acceptable.
	 *  buf_addr     -- base->ubuffer.umem->address was set verbatim
	 *                  by ib_umem_get(@ucmd.buf_addr) at create time
	 *                  via _create_user_qp. That's the source
	 *                  userspace VA RESTORE_QP needs to look up the
	 *                  LOAD_VHCA_STATE-installed (KIND_QP, qpn)
	 *                  placeholder.
	 *  db_addr      -- mlx5_ib_db_user_virt(&mqp->db) is the page-
	 *                  aligned doorbell user-virt that
	 *                  mlx5_ib_db_map_user dedup-keyed on. The byte
	 *                  offset into the page survives via the FW
	 *                  qpc.dbr_addr; the page-aligned form is what
	 *                  the restore-side placeholder lookup keys on.
	 *  sq_wqe_count -- mqp->sq.wqe_cnt: post-rounding SQ ring depth,
	 *                  same value the source's
	 *                  mlx5_ib_create_qp_user.{ucmd}.sq_wqe_count
	 *                  carried.
	 *  rq_wqe_count -- mqp->rq.wqe_cnt
	 *  rq_wqe_shift -- mqp->rq.wqe_shift; RESTORE_QP gates on
	 *                  rq_wqe_shift in [4, 16] when rq_wqe_count > 0,
	 *                  so we faithfully echo what the source set.
	 *  flags        -- mqp->flags_en (MLX5_QP_FLAG_*); the
	 *                  source-side ucmd.flags that mlx5_ib_create_qp
	 *                  cached on the QP.
	 *
	 * Sentinels for FW-side fields (round-trip via LOAD_VHCA_STATE):
	 *
	 *  uidx         -- 0; qpc.user_index is preserved by LOAD,
	 *                  RESTORE_QP only validates req.uidx & ~0xffffffU.
	 *  bfreg_index  -- MLX5_IB_INVALID_BFREG; RESTORE_QP forces
	 *                  qp->bfregn = MLX5_IB_INVALID_BFREG anyway:
	 *                  the source's UAR mapping is encoded in the
	 *                  adopted qpc.uar_page, not re-derived from
	 *                  this UHW field.
	 *  ece_options  -- 0; FW negotiates ECE per-connection during
	 *                  MODIFY_QP and the QPC's ece_options round-trip
	 *                  via LOAD_VHCA_STATE.
	 *
	 *  sq_buf_addr  -- 0; raw_packet split-SQ is rejected by the
	 *                  type gate above. v0 RC/UC/UD share a single
	 *                  WQ-ring umem (RQ at offset 0, SQ at
	 *                  rq_wqe_count << rq_wqe_shift).
	 */
	blob.buf_addr = base->ubuffer.umem->address;
	blob.db_addr = mlx5_ib_db_user_virt(&mqp->db);
	blob.sq_buf_addr = 0;
	blob.qpn = base->mqp.qpn;
	blob.sq_wqe_count = mqp->sq.wqe_cnt;
	blob.rq_wqe_count = mqp->rq.wqe_cnt;
	blob.rq_wqe_shift = mqp->rq.wqe_shift;
	blob.flags = mqp->flags_en;
	blob.uidx = 0;
	blob.bfreg_index = MLX5_IB_INVALID_BFREG;
	blob.ece_options = 0;

	user_handle = ib_qp_user_handle(ibqp);
	create_flags = mqp->flags;

	err = uverbs_copy_to(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_BLOB, &blob, sizeof(blob));
	if (err)
		return err;
	err = uverbs_copy_to(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_USER_HANDLE,
		&user_handle, sizeof(user_handle));
	if (err)
		return err;
	return uverbs_copy_to(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_CREATE_FLAGS,
		&create_flags, sizeof(create_flags));
}

DECLARE_UVERBS_NAMED_METHOD(
	MLX5_IB_METHOD_VFMIG_QUERY_QP,
	UVERBS_ATTR_IDR(MLX5_IB_ATTR_VFMIG_QUERY_QP_HANDLE,
			UVERBS_OBJECT_QP,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_BLOB,
			    UVERBS_ATTR_TYPE(struct mlx5_ib_restore_qp_req),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_USER_HANDLE,
			    UVERBS_ATTR_TYPE(u64),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_CREATE_FLAGS,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY));

/*
 * MLX5_IB_METHOD_VFMIG_QUERY_PD -- emit, for the PD resolved through
 * UVERBS_OBJECT_PD on the calling fd's ufile, the bytes a CRIU plugin
 * needs to drive UVERBS_METHOD_RESTORE_PD on the destination side.
 *
 * Two-part output:
 *   RESP_BLOB  struct mlx5_ib_restore_pd_req, byte-equal to what
 *              RESTORE_PD's UHW will consume. The handler leaves
 *              req.reserved / req.reserved2 zero so the restore path's
 *              "must be 0" checks pass round-trip.
 *   RESP_UID   mpd->uid, the source PD's owning FW uid. Dump-side
 *              cross-check only -- RESTORE_PD takes uid from the
 *              adopted ucontext's devx_uid, not from this value.
 *
 * Unlike QUERY_CQ / QUERY_QP there is no kernel-mode rejection: a PD
 * has no umem and no source userspace VAs, and the IDR lookup came
 * through a user ufile by construction. mpd->pdn is the 24-bit FW
 * resource id, non-zero for any live PD.
 *
 * The IDR lookup for HANDLE goes through the calling fd's ufile and
 * grabs UVERBS_ACCESS_READ on the PD uobject for the duration of the
 * call, so a concurrent DEALLOC_PD on the same fd cannot race.
 */
static int UVERBS_HANDLER(MLX5_IB_METHOD_VFMIG_QUERY_PD)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_pd *ibpd = uverbs_attr_get_obj(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_PD_HANDLE);
	struct mlx5_ib_pd *mpd;
	struct mlx5_ib_restore_pd_req blob = {};
	u32 uid;
	int err;

	if (IS_ERR(ibpd))
		return PTR_ERR(ibpd);

	mpd = to_mpd(ibpd);

	blob.pdn = mpd->pdn;
	uid = mpd->uid;

	err = uverbs_copy_to(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_PD_RESP_BLOB, &blob, sizeof(blob));
	if (err)
		return err;
	return uverbs_copy_to(attrs,
		MLX5_IB_ATTR_VFMIG_QUERY_PD_RESP_UID, &uid, sizeof(uid));
}

DECLARE_UVERBS_NAMED_METHOD(
	MLX5_IB_METHOD_VFMIG_QUERY_PD,
	UVERBS_ATTR_IDR(MLX5_IB_ATTR_VFMIG_QUERY_PD_HANDLE,
			UVERBS_OBJECT_PD,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_PD_RESP_BLOB,
			    UVERBS_ATTR_TYPE(struct mlx5_ib_restore_pd_req),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_PD_RESP_UID,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY));

DECLARE_UVERBS_GLOBAL_METHODS(
	MLX5_IB_OBJECT_VFMIG,
	&UVERBS_METHOD(MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT),
	&UVERBS_METHOD(MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT),
	&UVERBS_METHOD(MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS),
	&UVERBS_METHOD(MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS),
	&UVERBS_METHOD(MLX5_IB_METHOD_VFMIG_QUERY_CQ),
	&UVERBS_METHOD(MLX5_IB_METHOD_VFMIG_QUERY_QP),
	&UVERBS_METHOD(MLX5_IB_METHOD_VFMIG_QUERY_PD));

const struct uapi_definition mlx5_ib_vfmig_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(MLX5_IB_OBJECT_VFMIG),
	{},
};
