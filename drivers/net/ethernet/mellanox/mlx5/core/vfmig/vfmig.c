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
#include <linux/crc32.h>
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
#include <linux/uuid.h>
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
 * VFMIG_WIRE_TAG_STREAM_HEADER
 * ----------------------------
 * vfmig-private. The very first record in any vfmig SAVE blob.
 * Carries:
 *
 *   - A magic ("VMIG") so a non-vfmig parser, or a vfmig parser
 *     pointed at a corrupt/foreign blob, refuses early instead of
 *     wandering into the FSM.
 *
 *   - A format version. There is exactly one defined value today
 *     (VFMIG_STREAM_VERSION). The field exists for forward
 *     compatibility: a future format-incompatible change bumps it
 *     and the LOAD parser refuses unknown values with -EOPNOTSUPP
 *     instead of silently misinterpreting fields. There is no
 *     down-version compatibility -- old SAVE blobs that predate this
 *     header don't exist outside development trees.
 *
 *   - The number of HOST_PAGE records that follow. Used by the
 *     LOAD parser as the gate for verifying @manifest_crc32 and as
 *     the trigger to reset the destination IOVA cursor before the
 *     FW_DATA record is staged.
 *
 *   - The number of HOST_USER_PAGE records that follow (always
 *     emitted AFTER all HOST_PAGE records). Always 0 on a kernel
 *     without user_mr_dma stage-2 source-side retag callsites
 *     (C6..C10 of the stage-2 series), so old SAVE blobs and fresh
 *     SAVE blobs from a tracked VF with no MR/CQ/QP/SRQ activity
 *     remain byte-equal to the pre-stage-2 format. Lives in the
 *     low 4 bytes of what used to be @reserved; the upper bytes
 *     stay zero for forward compatibility (older LOAD parsers
 *     don't validate them either way).
 *
 *   - A 32-bit CRC of the (slot, instance_key, iova, len) tuples
 *     of every HOST_PAGE record followed by the (flags, reserved,
 *     instance_key, iova, len) tuples of every HOST_USER_PAGE
 *     record, in the order they appear on the wire. This is a
 *     transport-integrity check at the identity level only;
 *     per-page contents are not in the CRC because the FW_DATA
 *     record's MKEY-based read covers any DMA-payload corruption.
 *     HOST_USER_PAGE records carry no contents at all -- only
 *     identity. A mismatch is -EPROTO, not -EIO -- some record's
 *     identity tuple got mangled in transit. With zero
 *     HOST_USER_PAGE records the fold is byte-equal to the
 *     pre-stage-2 CRC, so old and new blobs are wire-compatible
 *     for the no-user-records case.
 *
 * Tag value 0x4853 is "HS" (host-stream). Chosen well clear of the
 * VFIO mlx5 {0, 1} tag range. NOT marked OPTIONAL.
 *
 * VFMIG_WIRE_TAG_HOST_PAGE
 * ------------------------
 * vfmig-private. Carries a single deterministic-IOVA page snapshot
 * from the source VF's vfmig_iova_domain into the destination VF's
 * vfmig_iova_domain via vfmig_iova_replay_page(). One record per
 * source registry entry, in IOVA-ascending order. SAVE emits these
 * BEFORE the FW_DATA record (and AFTER the STREAM_HEADER record) so
 * that by the time the FW_DATA record is staged into the
 * pending_load slot, every IOVA the FW state references already
 * maps to a populated page in the destination's domain.
 *
 * Tag value 0x4842 is "HB" (host-buffer). NOT marked OPTIONAL: a
 * HOST_PAGE-bearing blob fed to a parser that doesn't understand
 * the tag should fail loudly with -EOPNOTSUPP, because silently
 * dropping the IOVA payload would mean a successful LOAD followed
 * by a dead VHCA -- the very failure mode the IOVA work exists to
 * eliminate.
 *
 * Per-record layout:
 *   [16 B] vfmig_wire_header   { record_size, flags=0, tag=HOST_PAGE }
 *   [40 B] vfmig_host_page_record { slot_id, instance_key, iova,
 *                                   len, flags }
 *   [len B] page contents, len % VFMIG_IOVA_GRANULE == 0
 *
 * record_size = sizeof(struct vfmig_host_page_record) + len.
 *
 * @flags is reserved-must-be-zero. Future bits will gate optional
 * per-page metadata (e.g. content CRC, IOVA mapping flags); a parser
 * encountering any unknown bit set must reject the record. Keeping
 * the field zero today lets us add bits later without re-bumping
 * VFMIG_STREAM_VERSION.
 *
 * VFMIG_HOST_PAGE_MAX_LEN is a defense-in-depth cap so a corrupt or
 * malicious blob can't kvmalloc the host out of memory before we
 * even reach the IOVA-window range check. 16 MiB is far above any
 * single registry entry the layered restore plan emits today
 * (PAGE_SIZE for cmd ring / FW pages / DMA coherent allocations).
 *
 * VFMIG_WIRE_TAG_HOST_USER_PAGE
 * -----------------------------
 * vfmig-private. Carries the identity of a single external
 * (USER_PAGE-slot) registry entry from the source VF's
 * vfmig_iova_domain into the destination's vfmig_iova_domain via
 * vfmig_iova_replay_external(). Identity-only: no page contents.
 * The destination installs an @awaiting_bind = true placeholder
 * (no iommu_map, no phys page) which user_mr_dma stage 3 binds in
 * place when the user-mode RESTORE_x verb fires on the destination.
 *
 * One record per external entry whose @instance_key has a non-zero
 * VFMIG_HUOBJ_KIND() byte -- i.e. per uobject (MR / DBR / CQ / QP /
 * SRQ) that had a source-side retag callsite fire. Auto-numbered
 * entries (kind == VFMIG_HUOBJ_KIND_NONE) are skipped because they
 * have no stable identity for the destination to bind to -- the
 * source's vfmig_dma_ops.map_sg installed them as part of stage 1
 * but no retag callsite has claimed them yet. On a kernel without
 * stage-2 retag callsites the SAVE walk skips every external entry
 * and the blob stays byte-equal to pre-stage-2 format.
 *
 * Tag value 0x4855 is "HU" (host-user). NOT marked OPTIONAL: a
 * blob bearing HOST_USER_PAGE records fed to a parser that does
 * not understand the tag must fail loudly with -EOPNOTSUPP --
 * silently dropping them would leave the destination with a FW
 * state that references user buffers the IOMMU does not know
 * about, exactly the failure mode user_mr_dma exists to prevent.
 *
 * Per-record layout:
 *   [16 B] vfmig_wire_header   { record_size = 32, flags=0,
 *                                tag=HOST_USER_PAGE }
 *   [32 B] vfmig_host_user_page_record { flags=0, reserved=0,
 *                                        instance_key, iova, len }
 *
 * Slot is implicit (always VFMIG_SLOT_USER_PAGE) -- HOST_USER_PAGE
 * is reserved for that slot. @instance_key is the source's
 * VFMIG_HUOBJ_KEY(kind, fw_id) blob. @iova is the source's
 * deterministic USER_PAGE IOVA for the umem range; the destination
 * replays at the SAME iova (the vfmig_iova_domain is symmetric
 * across LOAD because the per-VF window is FW-allocated and
 * preserved by LOAD_VHCA_STATE). @len is the umem range's PAGE_SIZE-
 * aligned byte length; the placeholder install path validates
 * @iova + @len fits within the USER_PAGE window of the destination.
 *
 * @flags is reserved-must-be-zero (future per-record metadata
 * gating); @reserved completes 8-byte alignment of @instance_key.
 */
#define VFMIG_WIRE_MAGIC		0x564D4947 /* "VMIG" little-endian */
#define VFMIG_STREAM_VERSION		1
#define VFMIG_WIRE_TAG_STREAM_HEADER	0x4853
#define VFMIG_WIRE_TAG_HOST_PAGE	0x4842
#define VFMIG_WIRE_TAG_HOST_USER_PAGE	0x4855
#define VFMIG_HOST_PAGE_MAX_LEN		(16ULL << 20)

struct vfmig_stream_header {
	__le32 magic;
	__le32 version;
	__le64 num_pages;
	__le32 manifest_crc32;
	__le32 num_user_pages;
};

struct vfmig_host_page_record {
	__le32 slot_id;
	__le32 flags;
	__le64 instance_key;
	__le64 iova;
	__le64 len;
};

struct vfmig_host_user_page_record {
	__le32 flags;
	__le32 reserved;
	__le64 instance_key;
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
 * MLX5_VF_LOAD_STATE_* enum in drivers/vfio/pci/mlx5/cmd.h, plus
 * vfmig-private states for the stream header and HOST_PAGE replay.
 */
enum vfmig_load_state {
	VFMIG_LS_READ_HEADER = 0,
	VFMIG_LS_READ_HEADER_DATA,
	VFMIG_LS_PREP_IMAGE,
	VFMIG_LS_READ_IMAGE,
	VFMIG_LS_LOAD_IMAGE,
	/*
	 * STREAM_HEADER sub-state. The very first record in a vfmig
	 * blob. Its presence is enforced by dispatch_header: a blob
	 * whose first record is anything else is rejected outright.
	 *   STREAM_HDR_READ -> reads the 24-byte stream header, parses
	 *                      magic / version / num_pages /
	 *                      manifest_crc32, returns to READ_HEADER.
	 */
	VFMIG_LS_STREAM_HDR_READ,
	/*
	 * HOST_PAGE record sub-states. After dispatch_header reads the
	 * 16-byte vfmig_wire_header and sees tag=HOST_PAGE, the parser:
	 *   HP_READ_SUBHDR -> reads sizeof(vfmig_host_page_record) more
	 *                     bytes (slot_id, flags, instance_key,
	 *                     iova, len)
	 *   HP_READ_DATA   -> reads @len bytes into a kvmalloc'd buffer
	 *   HP_REPLAY      -> folds the record's identity into the
	 *                     running CRC, calls vfmig_iova_replay_page
	 *                     with the wire-provided (slot,
	 *                     instance_key) against the destination
	 *                     VF's domain, frees the buffer, returns to
	 *                     READ_HEADER. When the per-fd page counter
	 *                     hits @hp_expected, the running CRC is
	 *                     checked against the value pinned by
	 *                     STREAM_HDR_READ.
	 */
	VFMIG_LS_HP_READ_SUBHDR,
	VFMIG_LS_HP_READ_DATA,
	VFMIG_LS_HP_REPLAY,
	/*
	 * HOST_USER_PAGE record sub-states. After dispatch_header reads
	 * the 16-byte vfmig_wire_header and sees tag=HOST_USER_PAGE,
	 * the parser:
	 *   HUP_READ_SUBHDR -> reads sizeof(vfmig_host_user_page_record)
	 *                      bytes (flags, reserved, instance_key,
	 *                      iova, len). HOST_USER_PAGE records are
	 *                      identity-only, so there is no separate
	 *                      DATA state -- the sub-header IS the
	 *                      whole record.
	 *   HUP_REPLAY      -> folds identity into the running CRC,
	 *                      calls vfmig_iova_replay_external() with
	 *                      VFMIG_SLOT_USER_PAGE + the wire-provided
	 *                      (instance_key, iova, len) against the
	 *                      destination VF's domain, advances
	 *                      hup_seen, returns to READ_HEADER. When
	 *                      both hp_seen == hp_expected and
	 *                      hup_seen == hup_expected at the same
	 *                      time, the running CRC is checked against
	 *                      the value pinned by STREAM_HDR_READ.
	 */
	VFMIG_LS_HUP_READ_SUBHDR,
	VFMIG_LS_HUP_REPLAY,
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
	enum vfmig_iova_slot hp_slot;
	u64 hp_instance_key;
	u64 hp_iova;
	u64 hp_len;
	void *hp_contents;
	u64 hp_filled;
	bool cursor_reset_done;

	/*
	 * HOST_USER_PAGE replay state. Identity-only records (no
	 * contents tail), so there's no hup_contents buffer. The
	 * sub-state set mirrors HOST_PAGE minus the DATA fields:
	 *   hup_subhdr_buf    -- 32-byte vfmig_host_user_page_record
	 *                        being read
	 *   hup_subhdr_filled -- bytes accumulated in @hup_subhdr_buf
	 *   hup_instance_key  -- VFMIG_HUOBJ_KEY(kind, fw_id) parsed
	 *                        out of @hup_subhdr_buf; kind byte
	 *                        must be non-NONE (the SAVE walker
	 *                        skips KIND_NONE entries) -- enforced
	 *                        at the end of HUP_READ_SUBHDR.
	 *   hup_iova / hup_len -- parsed from @hup_subhdr_buf.
	 * Slot is always VFMIG_SLOT_USER_PAGE for HOST_USER_PAGE
	 * records (the tag itself encodes that).
	 */
	u8  hup_subhdr_buf[sizeof(struct vfmig_host_user_page_record)];
	u32 hup_subhdr_filled;
	u64 hup_instance_key;
	u64 hup_iova;
	u64 hup_len;

	/*
	 * Stream header bookkeeping. Set by VFMIG_LS_STREAM_HDR_READ;
	 * consumed by HP_REPLAY (the per-record CRC fold and the
	 * "all pages received" gate).
	 *
	 *   stream_hdr_seen   -- true once a STREAM_HEADER has been
	 *                        successfully parsed for this fd. The
	 *                        very first record MUST set this; any
	 *                        non-STREAM_HEADER first record is
	 *                        rejected.
	 *   stream_hdr_buf    -- the 24-byte header being accumulated
	 *                        in VFMIG_LS_STREAM_HDR_READ.
	 *   stream_hdr_filled -- bytes accumulated in stream_hdr_buf.
	 *   hp_expected       -- num_pages from the stream header. The
	 *                        FW_DATA dispatch refuses to proceed
	 *                        until this many HOST_PAGE records have
	 *                        been replayed.
	 *   hp_seen           -- HOST_PAGE records replayed so far.
	 *   hup_expected      -- num_user_pages from the stream header.
	 *                        The FW_DATA dispatch additionally
	 *                        refuses to proceed until this many
	 *                        HOST_USER_PAGE records have been
	 *                        replayed.
	 *   hup_seen          -- HOST_USER_PAGE records replayed so
	 *                        far.
	 *   manifest_crc_want -- manifest_crc32 from the stream header.
	 *   manifest_crc_have -- running CRC over the (slot,
	 *                        instance_key, iova, len) tuples of the
	 *                        HOST_PAGE records replayed so far,
	 *                        followed by the (flags, reserved,
	 *                        instance_key, iova, len) tuples of the
	 *                        HOST_USER_PAGE records replayed so
	 *                        far. Compared with @manifest_crc_want
	 *                        when (hp_seen, hup_seen) reach
	 *                        (hp_expected, hup_expected); mismatch
	 *                        is -EPROTO.
	 *   records_finalized -- once-per-fd latch ensuring the
	 *                        finalize step (CRC verify + arm drift
	 *                        detection) fires exactly once, even
	 *                        though both HP_REPLAY and HUP_REPLAY
	 *                        check the (hp+hup)_seen ==
	 *                        (hp+hup)_expected condition.
	 */
	bool stream_hdr_seen;
	u8   stream_hdr_buf[sizeof(struct vfmig_stream_header)];
	u32  stream_hdr_filled;
	u64  hp_expected;
	u64  hp_seen;
	u64  hup_expected;
	u64  hup_seen;
	u32  manifest_crc_want;
	u32  manifest_crc_have;
	bool records_finalized;
};

static void vfmig_load_release_resources(struct mlx5_vfmig_load_ctx *ctx);
static bool vfmig_vf_id_busy_locked(struct mlx5_vfmig_pf *vfmig, u32 vf_id);
static void vfmig_pf_drop_pending_loads_locked(struct mlx5_vfmig_pf *vfmig);
static void vfmig_pf_drop_iova_domains_locked(struct mlx5_vfmig_pf *vfmig);
static void vfmig_pf_drop_vf_uuids_locked(struct mlx5_vfmig_pf *vfmig);

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
		/*
		 * Promoted from mlx5_core_dbg to mlx5_core_info on
		 * purpose: a "successful" SET_TRACKED that was actually
		 * a silent no-op is the exact symptom of running
		 * userspace against a stale mlx5_core.ko (e.g. kernel
		 * rebuilt but module not re-installed/reloaded). Without
		 * a default-visible breadcrumb, the next QUERY_VF or
		 * LOAD_VHCA_STATE failure is very hard to attribute to
		 * version skew. The line is one-per-explicit-call, not
		 * a hot path, so the noise cost is negligible.
		 */
		mlx5_core_info(pf_mdev,
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

/*
 * MLX5_VFMIG_IOC_SET_VF_UUID handler.
 *
 * Stamp the orchestrator's 16-byte UUID into vfs_ctx[vf_id].vf_uuid.
 * Read by CRIU dump/restore (and the prerestore binary) via QUERY_VF
 * to bind a saved-state image to a specific destination VF -- the
 * orchestrator is the only component that knows which destination
 * VF carries which workload's identity, so this is the orchestrator's
 * write surface.
 *
 * Locking
 *   vfmig->ctxs_lock serialises the per-PF set-or-collide decision.
 *   Concurrent SET_VF_UUID calls on the same vf_id must agree on the
 *   "first writer wins, second-with-different-value gets -EBUSY"
 *   semantics; without the mutex two simultaneous all-zeros-then-write
 *   races could end with an unintended last-writer-wins outcome.
 *   The matching read site (vfmig_ioc_query_vf) is intentionally
 *   lock-free, matching the rest of QUERY_VF; a torn read just yields
 *   a one-cycle stale snapshot for an observer racing a SET_VF_UUID.
 *
 * Lifecycle / -EBUSY semantics
 *   See the doc-comment in include/uapi/linux/mlx5_vfmig.h. Briefly:
 *     - vfs_ctx->vf_uuid all-zeros + non-zero @arg.vf_uuid:
 *       stamp; return 0.
 *     - vfs_ctx->vf_uuid == @arg.vf_uuid (both non-zero):
 *       no-op; return 0 (idempotent).
 *     - vfs_ctx->vf_uuid != @arg.vf_uuid, and vfs_ctx->vf_uuid
 *       is non-zero:
 *       refuse; return -EBUSY (defends against accidental
 *       cross-workload reuse of a slot).
 *
 * The kernel does NOT validate UUID uniqueness across slots / PFs;
 * that is the orchestrator's responsibility (it has the cross-PF
 * view we'd need anyway). Per-slot serialization is enough to keep
 * a single slot's value internally consistent.
 *
 * VF bind state is irrelevant: unlike SET_TRACKED, this ioctl does
 * not touch the VF's pci_dev, IOMMU domain, or any cmdif state. The
 * orchestrator is expected to call this before workload bind, but a
 * post-bind call is not catastrophic -- it just wouldn't normally
 * happen because the orchestrator's natural flow is to stamp at
 * provisioning time.
 */
static long vfmig_ioc_set_vf_uuid(struct mlx5_vfmig_pf *vfmig,
				  void __user *uarg)
{
	struct mlx5_vfmig_set_vf_uuid arg;
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_core_sriov *sriov;
	struct mlx5_vf_context *vfs_ctx;
	uuid_t new_uuid;
	int err = 0;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;
	import_uuid(&new_uuid, arg.vf_uuid);
	/*
	 * uuid_null is the "unset" sentinel. Reject as a write so
	 * userspace cannot accidentally clear a slot via SET_VF_UUID;
	 * the only legitimate path to uuid_null is teardown.
	 */
	if (uuid_is_null(&new_uuid))
		return -EINVAL;

	sriov = &pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	vfs_ctx = &sriov->vfs_ctx[arg.vf_id];

	mutex_lock(&vfmig->ctxs_lock);
	if (uuid_is_null(&vfs_ctx->vf_uuid)) {
		uuid_copy(&vfs_ctx->vf_uuid, &new_uuid);
		mlx5_core_info(pf_mdev,
			       "vfmig: SET_VF_UUID vf %u stamped\n",
			       arg.vf_id);
	} else if (uuid_equal(&vfs_ctx->vf_uuid, &new_uuid)) {
		/* Idempotent same-UUID re-stamp; no log line. */
	} else {
		mlx5_core_warn(pf_mdev,
			       "vfmig: SET_VF_UUID vf %u rejected: a different UUID is already stamped (orchestrator bug?)\n",
			       arg.vf_id);
		err = -EBUSY;
	}
	mutex_unlock(&vfmig->ctxs_lock);
	return err;
}

/*
 * MLX5_VFMIG_IOC_PROBE_UID handler -- experimental.
 *
 * Issues CREATE_UCTX(VF) + immediate DESTROY_UCTX(VF) on the bound
 * VF mdev and returns the uid the firmware allocated. The point of
 * the round-trip is to read FW's per-VHCA uctx-id allocator
 * high-water-mark *without* having to drive a real ucontext from
 * user space, so we can answer "did LOAD_VHCA_STATE preserve the
 * source's uctx-id space?" with a single ioctl on src and dst.
 *
 * VF mdev lookup
 *   CREATE_UCTX has no other_function variant; the command must
 *   target the VF's own VHCA via its own mdev's cmdif. We:
 *     1. Resolve the VF's pci_dev from (pf_pdev, vf_id) via
 *        vfmig_get_vf_pdev() (refcounted, must put on exit).
 *     2. Take the VF pci_dev's device_lock so ->dev.driver and
 *        drvdata are stable -- this is the same lock the PCI core
 *        takes around probe/remove.
 *     3. Match the bound driver by name (KBUILD_MODNAME) rather
 *        than by pci_driver pointer; mlx5_core_driver is static in
 *        main.c and we don't want to add a back-door export just
 *        for this debug ioctl.
 *     4. Fetch vf_mdev via pci_get_drvdata() and verify
 *        MLX5_INTERFACE_STATE_UP -- the cmdif is only valid then.
 *
 * Why we destroy immediately
 *   We're probing the *allocator state*, not creating a usable
 *   uctx. Holding a uid alive across the ioctl would (a) leak it
 *   on every probe, and (b) perturb the allocator we're trying to
 *   measure. Destroy-on-the-spot makes back-to-back ioctls return
 *   adjacent values that reveal monotonicity vs reuse. If the
 *   DESTROY_UCTX itself fails after a successful CREATE_UCTX (it
 *   shouldn't on healthy FW), we log and return success with the
 *   measured uid -- the leaked uctx survives only until the next
 *   sriov_numvfs=0 cycle, which is acceptable for an experimental
 *   debug surface.
 */
static long vfmig_ioc_probe_uid(struct mlx5_vfmig_pf *vfmig,
				void __user *uarg)
{
	u32 in[MLX5_ST_SZ_DW(create_uctx_in)] = {};
	u32 out[MLX5_ST_SZ_DW(create_uctx_out)] = {};
	u32 din[MLX5_ST_SZ_DW(destroy_uctx_in)] = {};
	u32 dout[MLX5_ST_SZ_DW(destroy_uctx_out)] = {};
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_probe_uid arg;
	struct mlx5_core_dev *vf_mdev;
	struct pci_dev *vf_pdev;
	struct device_driver *drv;
	u16 uid;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;
	if (arg.vf_id >= pf_mdev->priv.sriov.num_vfs)
		return -EINVAL;

	vf_pdev = vfmig_get_vf_pdev(pf_mdev->pdev, arg.vf_id);
	if (!vf_pdev)
		return -ENODEV;

	device_lock(&vf_pdev->dev);

	drv = vf_pdev->dev.driver;
	if (!drv || strcmp(drv->name, KBUILD_MODNAME)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_uid: vf %u not bound to %s (driver=%s)\n",
			       arg.vf_id, KBUILD_MODNAME,
			       drv ? drv->name : "<unbound>");
		err = -ENODEV;
		goto out_unlock;
	}

	vf_mdev = pci_get_drvdata(vf_pdev);
	if (!vf_mdev ||
	    !test_bit(MLX5_INTERFACE_STATE_UP, &vf_mdev->intf_state)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_uid: vf %u mdev not interface-up\n",
			       arg.vf_id);
		err = -ENODEV;
		goto out_unlock;
	}

	MLX5_SET(create_uctx_in, in, opcode, MLX5_CMD_OP_CREATE_UCTX);
	err = mlx5_cmd_exec(vf_mdev, in, sizeof(in), out, sizeof(out));
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_uid: CREATE_UCTX on vf %u failed %d\n",
			       arg.vf_id, err);
		goto out_unlock;
	}

	uid = MLX5_GET(create_uctx_out, out, uid);

	MLX5_SET(destroy_uctx_in, din, opcode, MLX5_CMD_OP_DESTROY_UCTX);
	MLX5_SET(destroy_uctx_in, din, uid, uid);
	err = mlx5_cmd_exec(vf_mdev, din, sizeof(din), dout, sizeof(dout));
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_uid: DESTROY_UCTX(vf=%u, uid=%u) failed %d -- uctx leaked until VHCA teardown\n",
			       arg.vf_id, uid, err);
		err = 0; /* we still return the measured uid */
	}

	mlx5_core_info(pf_mdev,
		       "vfmig: probe_uid: vf %u CREATE_UCTX returned uid=%u\n",
		       arg.vf_id, uid);

	arg.uid = uid;
	arg.reserved2 = 0;
	arg.reserved3 = 0;

	if (copy_to_user(uarg, &arg, sizeof(arg)))
		err = -EFAULT;

out_unlock:
	device_unlock(&vf_pdev->dev);
	pci_dev_put(vf_pdev);
	return err;
}

/*
 * MLX5_VFMIG_IOC_QUERY_QP handler -- experimental.
 *
 * Issues a raw FW QUERY_QP(opcode 0x50b) on the bound VF's mdev for
 * the supplied qpn and reports the subset of the QPC needed by the
 * ?6.3 piggyback experiment in tools/testing/mlx5_vfmig/design/uobject_restore.md.
 *
 * VF mdev lookup mirrors vfmig_ioc_probe_uid: resolve the VF pci_dev
 * from the PF + vf_id, take device_lock to keep ->driver and drvdata
 * stable, match driver by KBUILD_MODNAME, verify
 * MLX5_INTERFACE_STATE_UP. The command is issued on the VF mdev's
 * cmdif with host kernel uid; FW returns the QPC regardless of which
 * ucontext originally created the QP (no UID gating observed on
 * QUERY_QP today -- if that ever changes we'll surface the FW
 * syndrome and revisit).
 */
static long vfmig_ioc_query_qp(struct mlx5_vfmig_pf *vfmig,
			       void __user *uarg)
{
	u32 in[MLX5_ST_SZ_DW(query_qp_in)] = {};
	u32 out[MLX5_ST_SZ_DW(query_qp_out)] = {};
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_query_qp arg;
	struct mlx5_core_dev *vf_mdev;
	struct pci_dev *vf_pdev;
	struct device_driver *drv;
	void *qpc;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved_in[0] || arg.reserved_in[1])
		return -EINVAL;
	if (arg.vf_id >= pf_mdev->priv.sriov.num_vfs)
		return -EINVAL;
	if (arg.qpn & 0xff000000)	/* QPN is 24 bits */
		return -EINVAL;

	vf_pdev = vfmig_get_vf_pdev(pf_mdev->pdev, arg.vf_id);
	if (!vf_pdev)
		return -ENODEV;

	device_lock(&vf_pdev->dev);

	drv = vf_pdev->dev.driver;
	if (!drv || strcmp(drv->name, KBUILD_MODNAME)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: query_qp: vf %u not bound to %s (driver=%s)\n",
			       arg.vf_id, KBUILD_MODNAME,
			       drv ? drv->name : "<unbound>");
		err = -ENODEV;
		goto out_unlock;
	}

	vf_mdev = pci_get_drvdata(vf_pdev);
	if (!vf_mdev ||
	    !test_bit(MLX5_INTERFACE_STATE_UP, &vf_mdev->intf_state)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: query_qp: vf %u mdev not interface-up\n",
			       arg.vf_id);
		err = -ENODEV;
		goto out_unlock;
	}

	MLX5_SET(query_qp_in, in, opcode, MLX5_CMD_OP_QUERY_QP);
	MLX5_SET(query_qp_in, in, qpn, arg.qpn);
	err = mlx5_cmd_exec(vf_mdev, in, sizeof(in), out, sizeof(out));
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: query_qp: vf %u qpn 0x%x failed %d\n",
			       arg.vf_id, arg.qpn, err);
		goto out_unlock;
	}

	qpc = MLX5_ADDR_OF(query_qp_out, out, qpc);

	/* state-independent bookkeeping */
	arg.qpc_state             = MLX5_GET(qpc, qpc, state);
	arg.qpc_pd                = MLX5_GET(qpc, qpc, pd);
	arg.qpc_q_key             = MLX5_GET(qpc, qpc, q_key);
	arg.qpc_uar_page          = MLX5_GET(qpc, qpc, uar_page);
	arg.qpc_log_page_size     = MLX5_GET(qpc, qpc, log_page_size);
	arg.qpc_log_sq_size       = MLX5_GET(qpc, qpc, log_sq_size);
	arg.qpc_log_rq_size       = MLX5_GET(qpc, qpc, log_rq_size);
	arg.qpc_log_msg_max       = MLX5_GET(qpc, qpc, log_msg_max);
	arg.qpc_user_index        = MLX5_GET(qpc, qpc, user_index);

	/* cross-references */
	arg.qpc_remote_qpn        = MLX5_GET(qpc, qpc, remote_qpn);
	arg.qpc_cqn_snd           = MLX5_GET(qpc, qpc, cqn_snd);
	arg.qpc_cqn_rcv           = MLX5_GET(qpc, qpc, cqn_rcv);
	arg.qpc_srqn_rmpn_xrqn    = MLX5_GET(qpc, qpc, srqn_rmpn_xrqn);

	/* PSNs */
	arg.qpc_next_send_psn     = MLX5_GET(qpc, qpc, next_send_psn);
	arg.qpc_next_rcv_psn      = MLX5_GET(qpc, qpc, next_rcv_psn);
	arg.qpc_last_acked_psn    = MLX5_GET(qpc, qpc, last_acked_psn);

	/* queue counters */
	arg.qpc_hw_sq_wqebb_counter = MLX5_GET(qpc, qpc, hw_sq_wqebb_counter);
	arg.qpc_sw_sq_wqebb_counter = MLX5_GET(qpc, qpc, sw_sq_wqebb_counter);
	arg.qpc_hw_rq_counter     = MLX5_GET(qpc, qpc, hw_rq_counter);
	arg.qpc_sw_rq_counter     = MLX5_GET(qpc, qpc, sw_rq_counter);

	/* RTR-set */
	arg.qpc_path_mtu          = MLX5_GET(qpc, qpc, mtu);
	arg.qpc_min_rnr_nak       = MLX5_GET(qpc, qpc, min_rnr_nak);
	arg.qpc_log_rra_max       = MLX5_GET(qpc, qpc, log_rra_max);
	arg.qpc_pkey_index        = MLX5_GET(qpc, qpc,
					     primary_address_path.pkey_index);

	/* RTS-set */
	arg.qpc_log_sra_max       = MLX5_GET(qpc, qpc, log_sra_max);
	arg.qpc_retry_count       = MLX5_GET(qpc, qpc, retry_count);
	arg.qpc_rnr_retry         = MLX5_GET(qpc, qpc, rnr_retry);

	/*
	 * AV: copy the raw bytes of QPC.primary_address_path so the
	 * harness can do a byte-equal compare without tracking each ADS
	 * subfield individually. This catches regressions in any ADS
	 * field (dgid, dlid/mlid, sgid_index, sl, port, dmac, hop_limit,
	 * tclass, flow_label, udp_sport, ack_timeout, eth_prio, ...) at
	 * the cost of needing post-hoc inspection on a mismatch.
	 *
	 * The UAPI buffer is sized at 64 bytes -- rounded up to give
	 * headroom for any future IFC-level extension to ads_bits while
	 * keeping the struct nice and 8-byte aligned. We zero the buffer
	 * and copy exactly MLX5_FLD_SZ_BYTES(qpc, primary_address_path)
	 * bytes (currently 44 = 11 dwords of struct mlx5_ifc_ads_bits)
	 * out of the FW QPC; trailing bytes remain zero on both src and
	 * dst, so byte-equal comparison stays correct.
	 *
	 * The BUILD_BUG_ON guards against the IFC growing past our
	 * UAPI window -- a future ads_bits expansion would require a
	 * UAPI bump (the ioctl number changes with sizeof(struct ...)).
	 */
	BUILD_BUG_ON(MLX5_FLD_SZ_BYTES(qpc, primary_address_path) >
		     sizeof(arg.qpc_primary_address_path));
	memset(arg.qpc_primary_address_path, 0,
	       sizeof(arg.qpc_primary_address_path));
	memcpy(arg.qpc_primary_address_path,
	       MLX5_ADDR_OF(qpc, qpc, primary_address_path),
	       MLX5_FLD_SZ_BYTES(qpc, primary_address_path));

	memset(arg.reserved_out, 0, sizeof(arg.reserved_out));

	mlx5_core_dbg(pf_mdev,
		      "vfmig: query_qp: vf %u qpn 0x%x state=%u sw_rq=%u hw_rq=%u next_rcv_psn=0x%x next_send_psn=0x%x mtu=%u\n",
		      arg.vf_id, arg.qpn, arg.qpc_state,
		      arg.qpc_sw_rq_counter, arg.qpc_hw_rq_counter,
		      arg.qpc_next_rcv_psn, arg.qpc_next_send_psn,
		      arg.qpc_path_mtu);

	if (copy_to_user(uarg, &arg, sizeof(arg)))
		err = -EFAULT;

out_unlock:
	device_unlock(&vf_pdev->dev);
	pci_dev_put(vf_pdev);
	return err;
}

/*
 * MLX5_VFMIG_IOC_PROBE_PD handler -- experimental, ?S3b empirical.
 *
 * Issues a transient CREATE_MKEY(uid, pd, access_mode=PA, length64=1)
 * followed by DESTROY_MKEY on the bound VF mdev's cmdif. The whole
 * point is to answer the ?S3b question: "can a uid that has no
 * destination-side ucontext owner still be used by FW to validate a
 * PD reference in a fresh CREATE_MKEY?" If yes (FW accepts), Model A
 * for mlx5_ib_restore_pd is sound: we can build a kernel-side
 * mlx5_ib_pd wrapping (src_pdn, src_uid) without first allocating a
 * fresh PD via mlx5_cmd_alloc_pd.
 *
 * VF mdev lookup mirrors vfmig_ioc_query_qp (which itself mirrors
 * vfmig_ioc_probe_uid): resolve the VF pci_dev from PF + vf_id, take
 * device_lock to pin ->driver and drvdata, match driver by
 * KBUILD_MODNAME, require MLX5_INTERFACE_STATE_UP. CREATE_MKEY and
 * DESTROY_MKEY both run on the VF mdev's cmdif with cmdif-uid=0
 * (host-privileged); the create_mkey_in.uid field is set from
 * @uid_hint independently, and that is the field FW reads for the
 * (uid, pd) gating check we want to exercise.
 */
static long vfmig_ioc_probe_pd(struct mlx5_vfmig_pf *vfmig,
			       void __user *uarg)
{
	u32 in[MLX5_ST_SZ_DW(create_mkey_in)] = {};
	u32 out[MLX5_ST_SZ_DW(create_mkey_out)] = {};
	u32 dmk_in[MLX5_ST_SZ_DW(destroy_mkey_in)] = {};
	u32 dmk_out[MLX5_ST_SZ_DW(destroy_mkey_out)] = {};
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_probe_pd arg;
	struct mlx5_core_dev *vf_mdev;
	struct pci_dev *vf_pdev;
	struct device_driver *drv;
	u32 mkey_index;
	void *mkc;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved_in)
		return -EINVAL;
	if (arg.vf_id >= pf_mdev->priv.sriov.num_vfs)
		return -EINVAL;
	if (arg.pdn & 0xff000000)		/* pdn is 24 bits */
		return -EINVAL;
	if (arg.uid_hint & 0xffff0000)		/* uid is 16 bits */
		return -EINVAL;

	vf_pdev = vfmig_get_vf_pdev(pf_mdev->pdev, arg.vf_id);
	if (!vf_pdev)
		return -ENODEV;

	device_lock(&vf_pdev->dev);

	drv = vf_pdev->dev.driver;
	if (!drv || strcmp(drv->name, KBUILD_MODNAME)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_pd: vf %u not bound to %s (driver=%s)\n",
			       arg.vf_id, KBUILD_MODNAME,
			       drv ? drv->name : "<unbound>");
		err = -ENODEV;
		goto out_unlock;
	}

	vf_mdev = pci_get_drvdata(vf_pdev);
	if (!vf_mdev ||
	    !test_bit(MLX5_INTERFACE_STATE_UP, &vf_mdev->intf_state)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_pd: vf %u mdev not interface-up\n",
			       arg.vf_id);
		err = -ENODEV;
		goto out_unlock;
	}

	/*
	 * Minimal PA-mode mkey, cribbed from mlx5_ib_data_direct's
	 * placeholder mkey in drivers/infiniband/hw/mlx5/main.c. We
	 * want length64=1 + qpn=0xffffff (== "any qp may use this
	 * mkey") to skip any qp-pinning gating; the only field whose
	 * acceptance we care about empirically is the (uid, pd) pair.
	 */
	MLX5_SET(create_mkey_in, in, opcode, MLX5_CMD_OP_CREATE_MKEY);
	MLX5_SET(create_mkey_in, in, uid, arg.uid_hint);
	mkc = MLX5_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);
	MLX5_SET(mkc, mkc, access_mode_1_0, MLX5_MKC_ACCESS_MODE_PA);
	MLX5_SET(mkc, mkc, lr, 1);
	MLX5_SET(mkc, mkc, pd, arg.pdn);
	MLX5_SET(mkc, mkc, length64, 1);
	MLX5_SET(mkc, mkc, qpn, 0xffffff);

	err = mlx5_cmd_exec(vf_mdev, in, sizeof(in), out, sizeof(out));
	if (err) {
		/*
		 * mlx5_cmd_exec() converts FW syndromes to negative errnos
		 * before returning, but stamps the original 32-bit
		 * syndrome onto out[1] via cmd_status_to_err()'s caller.
		 * Surface it to userspace so the test can distinguish
		 * "invalid PD" (0x...) from "invalid UID" (0x...) from
		 * transport errors.
		 */
		arg.fw_syndrome = MLX5_GET(create_mkey_out, out, syndrome);
		mlx5_core_dbg(pf_mdev,
			      "vfmig: probe_pd: vf %u pdn 0x%x uid 0x%x CREATE_MKEY err %d syndrome 0x%x\n",
			      arg.vf_id, arg.pdn, arg.uid_hint, err,
			      arg.fw_syndrome);
		err = 0;
		goto out_copy;
	}

	arg.fw_syndrome = 0;
	mkey_index = MLX5_GET(create_mkey_out, out, mkey_index);

	mlx5_core_dbg(pf_mdev,
		      "vfmig: probe_pd: vf %u pdn 0x%x uid 0x%x CREATE_MKEY ok mkey_index=0x%x\n",
		      arg.vf_id, arg.pdn, arg.uid_hint, mkey_index);

	/*
	 * Tear down the probe mkey. We hand-build the destroy_mkey_in
	 * rather than calling mlx5_core_destroy_mkey() because the
	 * core helper hardcodes uid=0 in destroy_mkey_in (it has no
	 * caller that needs the uid form). If DESTROY_MKEY fails we
	 * log + warn but return success to the caller -- the leaked
	 * mkey is bounded by the VHCA lifetime, acceptable for a debug
	 * ioctl on a controlled experiment.
	 */
	MLX5_SET(destroy_mkey_in, dmk_in, opcode, MLX5_CMD_OP_DESTROY_MKEY);
	MLX5_SET(destroy_mkey_in, dmk_in, uid, arg.uid_hint);
	MLX5_SET(destroy_mkey_in, dmk_in, mkey_index, mkey_index);
	err = mlx5_cmd_exec(vf_mdev, dmk_in, sizeof(dmk_in),
			    dmk_out, sizeof(dmk_out));
	if (err)
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_pd: vf %u DESTROY_MKEY(mkey_index=0x%x) failed %d -- leaking probe mkey\n",
			       arg.vf_id, mkey_index, err);
	err = 0;	/* CREATE_MKEY succeeded; this is the result. */

out_copy:
	memset(arg.reserved_out, 0, sizeof(arg.reserved_out));
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		err = -EFAULT;

out_unlock:
	device_unlock(&vf_pdev->dev);
	pci_dev_put(vf_pdev);
	return err;
}

/*
 * MLX5_VFMIG_IOC_PROBE_MKEY handler -- experimental, ?S4b empirical.
 *
 * Issues a single QUERY_MKEY(mkey_index) on the bound VF mdev's
 * cmdif and -- on FW accept -- reads back the mkc pd / qpn / len /
 * start_addr fields. The whole point is to answer the ?S4b
 * existence question: "Does the source's user-mode MKEY at index N
 * survive LOAD_VHCA_STATE intact, with the same pd/length/iova the
 * source had at SAVE time?" If yes, Model A for mlx5_ib_restore_mr
 * is sound: a destination kernel-side mlx5_ib_mr can wrap the
 * adopted (mkey_index, pdn) pair without first issuing a fresh
 * CREATE_MKEY against the destination VHCA.
 *
 * Locking: same shape as vfmig_ioc_probe_pd / vfmig_ioc_probe_uid.
 * Resolve the VF pci_dev from PF + vf_id, take device_lock to pin
 * ->driver and drvdata, match driver by KBUILD_MODNAME, require
 * MLX5_INTERFACE_STATE_UP. QUERY_MKEY runs on the VF mdev's cmdif
 * with cmdif-uid=0 (host-privileged). mlx5_ifc_query_mkey_in has
 * no uid field of its own, so the question of "can a uid=N
 * ucontext use this mkey?" is not addressed here -- see PROBE_MKEY
 * UAPI doc.
 *
 * Memory layout: query_mkey_out is large (translations_octword
 * payload tail) but we only use the mkc header at offset 0x80
 * (start of memory_key_mkey_entry). Reading the entire blob is
 * fine; this is a debug-only path with no perf concerns.
 */
static long vfmig_ioc_probe_mkey(struct mlx5_vfmig_pf *vfmig,
				 void __user *uarg)
{
	u32 in[MLX5_ST_SZ_DW(query_mkey_in)] = {};
	int outlen = MLX5_ST_SZ_BYTES(query_mkey_out);
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_probe_mkey arg;
	struct mlx5_core_dev *vf_mdev;
	struct pci_dev *vf_pdev;
	struct device_driver *drv;
	void *out, *mkc;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (memchr_inv(arg.reserved_in, 0, sizeof(arg.reserved_in)))
		return -EINVAL;
	if (arg.vf_id >= pf_mdev->priv.sriov.num_vfs)
		return -EINVAL;
	if (arg.mkey_index & 0xff000000)	/* mkey_index is 24 bits */
		return -EINVAL;

	out = kzalloc(outlen, GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	vf_pdev = vfmig_get_vf_pdev(pf_mdev->pdev, arg.vf_id);
	if (!vf_pdev) {
		err = -ENODEV;
		goto out_free;
	}

	device_lock(&vf_pdev->dev);

	drv = vf_pdev->dev.driver;
	if (!drv || strcmp(drv->name, KBUILD_MODNAME)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_mkey: vf %u not bound to %s (driver=%s)\n",
			       arg.vf_id, KBUILD_MODNAME,
			       drv ? drv->name : "<unbound>");
		err = -ENODEV;
		goto out_unlock;
	}

	vf_mdev = pci_get_drvdata(vf_pdev);
	if (!vf_mdev ||
	    !test_bit(MLX5_INTERFACE_STATE_UP, &vf_mdev->intf_state)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_mkey: vf %u mdev not interface-up\n",
			       arg.vf_id);
		err = -ENODEV;
		goto out_unlock;
	}

	MLX5_SET(query_mkey_in, in, opcode, MLX5_CMD_OP_QUERY_MKEY);
	MLX5_SET(query_mkey_in, in, mkey_index, arg.mkey_index);

	err = mlx5_cmd_exec(vf_mdev, in, sizeof(in), out, outlen);
	if (err) {
		/*
		 * mlx5_cmd_exec converts FW syndromes to negative errnos
		 * but stamps the original 32-bit syndrome onto the output
		 * blob. Surface it to userspace so the test matrix can
		 * distinguish "invalid mkey" (FW gone) from transport
		 * errors. Fields stay zero in the reject lane, matching
		 * the UAPI contract.
		 */
		arg.fw_syndrome = MLX5_GET(query_mkey_out, out, syndrome);
		arg.fw_pd = 0;
		arg.fw_qpn = 0;
		arg.fw_start_addr = 0;
		arg.fw_length = 0;
		mlx5_core_dbg(pf_mdev,
			      "vfmig: probe_mkey: vf %u mkey_index 0x%x QUERY_MKEY err %d syndrome 0x%x\n",
			      arg.vf_id, arg.mkey_index, err,
			      arg.fw_syndrome);
		err = 0;
		goto out_copy;
	}

	arg.fw_syndrome = 0;
	mkc = MLX5_ADDR_OF(query_mkey_out, out, memory_key_mkey_entry);
	arg.fw_pd = MLX5_GET(mkc, mkc, pd);
	arg.fw_qpn = MLX5_GET(mkc, mkc, qpn);
	arg.fw_start_addr = MLX5_GET64(mkc, mkc, start_addr);
	arg.fw_length = MLX5_GET64(mkc, mkc, len);

	mlx5_core_dbg(pf_mdev,
		      "vfmig: probe_mkey: vf %u mkey_index 0x%x ok pd=0x%x qpn=0x%x len=0x%llx start=0x%llx\n",
		      arg.vf_id, arg.mkey_index, arg.fw_pd, arg.fw_qpn,
		      arg.fw_length, arg.fw_start_addr);

out_copy:
	memset(arg.reserved_out0, 0, sizeof(arg.reserved_out0));
	memset(arg.reserved_out1, 0, sizeof(arg.reserved_out1));
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		err = -EFAULT;

out_unlock:
	device_unlock(&vf_pdev->dev);
	pci_dev_put(vf_pdev);
out_free:
	kfree(out);
	return err;
}

/*
 * MLX5_VFMIG_IOC_PROBE_CQN handler -- experimental, ?S5b empirical.
 *
 * Issues a single QUERY_CQ(cqn) on the bound VF mdev's cmdif and --
 * on FW accept -- reads back the cqc fields the ?S5b byte-equality
 * assertion checks (status, oi, log_cq_size, log_page_size,
 * page_offset, c_eqn_or_apu_element). The whole point is to answer
 * the ?S5b existence question that mr_restore_probe asks for mkeys:
 * "Does the source's user-mode CQ at cqn=N survive LOAD_VHCA_STATE
 * intact, with the same eqn binding and shape parameters the source
 * had at SAVE time?" If yes, Model A for mlx5_ib_restore_cq is sound:
 * a destination kernel-side mlx5_ib_cq can wrap the adopted (cqn,
 * eqn) pair without first issuing a fresh CREATE_CQ against the
 * destination VHCA.
 *
 * Locking: same shape as vfmig_ioc_probe_pd / vfmig_ioc_probe_mkey.
 * Resolve the VF pci_dev from PF + vf_id, take device_lock to pin
 * ->driver and drvdata, match driver by KBUILD_MODNAME, require
 * MLX5_INTERFACE_STATE_UP. QUERY_CQ runs on the VF mdev's cmdif
 * with cmdif-uid=0 (host-privileged). mlx5_ifc_query_cq_in has no
 * uid field of its own, so the question of "can a uid=N ucontext
 * use this CQ?" is not addressed here -- see PROBE_CQN UAPI doc.
 *
 * Memory layout: query_cq_out is large (PAS payload tail of
 * 0x600 bits / 192 bytes after the cqc) but we only use the cqc
 * header at offset 0x80. Reading the entire blob is fine; this is
 * a debug-only path with no perf concerns. Same shape as the
 * PROBE_MKEY allocation pattern.
 */
static long vfmig_ioc_probe_cqn(struct mlx5_vfmig_pf *vfmig,
				void __user *uarg)
{
	u32 in[MLX5_ST_SZ_DW(query_cq_in)] = {};
	int outlen = MLX5_ST_SZ_BYTES(query_cq_out);
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_probe_cqn arg;
	struct mlx5_core_dev *vf_mdev;
	struct pci_dev *vf_pdev;
	struct device_driver *drv;
	void *out, *cqc;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (memchr_inv(arg.reserved_in, 0, sizeof(arg.reserved_in)))
		return -EINVAL;
	if (arg.vf_id >= pf_mdev->priv.sriov.num_vfs)
		return -EINVAL;
	if (arg.cqn & 0xff000000)		/* cqn is 24 bits */
		return -EINVAL;

	out = kzalloc(outlen, GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	vf_pdev = vfmig_get_vf_pdev(pf_mdev->pdev, arg.vf_id);
	if (!vf_pdev) {
		err = -ENODEV;
		goto out_free;
	}

	device_lock(&vf_pdev->dev);

	drv = vf_pdev->dev.driver;
	if (!drv || strcmp(drv->name, KBUILD_MODNAME)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_cqn: vf %u not bound to %s (driver=%s)\n",
			       arg.vf_id, KBUILD_MODNAME,
			       drv ? drv->name : "<unbound>");
		err = -ENODEV;
		goto out_unlock;
	}

	vf_mdev = pci_get_drvdata(vf_pdev);
	if (!vf_mdev ||
	    !test_bit(MLX5_INTERFACE_STATE_UP, &vf_mdev->intf_state)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_cqn: vf %u mdev not interface-up\n",
			       arg.vf_id);
		err = -ENODEV;
		goto out_unlock;
	}

	MLX5_SET(query_cq_in, in, opcode, MLX5_CMD_OP_QUERY_CQ);
	MLX5_SET(query_cq_in, in, cqn, arg.cqn);

	err = mlx5_cmd_exec(vf_mdev, in, sizeof(in), out, outlen);
	if (err) {
		/*
		 * mlx5_cmd_exec converts FW syndromes to negative errnos
		 * but stamps the original 32-bit syndrome onto the output
		 * blob. Surface it to userspace so the test matrix can
		 * distinguish "invalid cqn" / "BAD_RES_STATE" (FW gone)
		 * from transport errors. All cqc readback fields stay
		 * zero in the reject lane, matching the UAPI contract.
		 */
		arg.fw_syndrome = MLX5_GET(query_cq_out, out, syndrome);
		arg.fw_eqn = 0;
		arg.fw_status = 0;
		arg.fw_log_cq_size = 0;
		arg.fw_log_page_size = 0;
		arg.fw_page_offset = 0;
		arg.fw_oi = 0;
		arg.fw_cqe_sz = 0;
		arg.fw_apu_cq = 0;
		arg.fw_uar_page = 0;
		arg.fw_dbr_addr = 0;
		mlx5_core_dbg(pf_mdev,
			      "vfmig: probe_cqn: vf %u cqn 0x%x QUERY_CQ err %d syndrome 0x%x\n",
			      arg.vf_id, arg.cqn, err, arg.fw_syndrome);
		err = 0;
		goto out_copy;
	}

	arg.fw_syndrome = 0;
	cqc = MLX5_ADDR_OF(query_cq_out, out, cq_context);
	arg.fw_eqn = MLX5_GET(cqc, cqc, c_eqn_or_apu_element);
	arg.fw_status = MLX5_GET(cqc, cqc, status);
	arg.fw_log_cq_size = MLX5_GET(cqc, cqc, log_cq_size);
	arg.fw_log_page_size = MLX5_GET(cqc, cqc, log_page_size);
	arg.fw_page_offset = MLX5_GET(cqc, cqc, page_offset);
	arg.fw_oi = MLX5_GET(cqc, cqc, oi);
	arg.fw_cqe_sz = MLX5_GET(cqc, cqc, cqe_sz);
	arg.fw_apu_cq = MLX5_GET(cqc, cqc, apu_cq);
	arg.fw_uar_page = MLX5_GET(cqc, cqc, uar_page);
	arg.fw_dbr_addr = MLX5_GET64(cqc, cqc, dbr_addr);

	mlx5_core_dbg(pf_mdev,
		      "vfmig: probe_cqn: vf %u cqn 0x%x ok eqn=0x%x status=%u log_cq_size=%u log_page_size=%u page_offset=%u oi=%u cqe_sz=%u apu_cq=%u uar_page=0x%x dbr_addr=0x%llx\n",
		      arg.vf_id, arg.cqn, arg.fw_eqn, arg.fw_status,
		      arg.fw_log_cq_size, arg.fw_log_page_size,
		      arg.fw_page_offset, arg.fw_oi, arg.fw_cqe_sz,
		      arg.fw_apu_cq, arg.fw_uar_page,
		      (unsigned long long)arg.fw_dbr_addr);

out_copy:
	arg.reserved_out0 = 0;
	arg.reserved_out1 = 0;
	memset(arg.reserved_out2, 0, sizeof(arg.reserved_out2));
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		err = -EFAULT;

out_unlock:
	device_unlock(&vf_pdev->dev);
	pci_dev_put(vf_pdev);
out_free:
	kfree(out);
	return err;
}

/*
 * MLX5_VFMIG_IOC_PROBE_QP_TEARDOWN handler -- experimental, ?S3b
 * "DEVX-adoption blind spot, destroy direction" empirical.
 *
 * Three FW commands per call, on the same VF mdev cmdif:
 *   1. QUERY_QP(qpn)              -- pre-op snapshot
 *   2. DESTROY_QP(qpn, uid)       -- @op_mode = OP_DESTROY
 *      or MODIFY_QP 2RST(qpn, uid) -- @op_mode = OP_2RST
 *   3. QUERY_QP(qpn)              -- post-op snapshot
 *
 * Step 1 establishes "the FW QPC exists at the moment of the test
 * and looks like this". Step 2 issues the operation under test
 * with the caller-supplied uid in the IFC field (the field FW
 * reads for ownership-scope checks). Step 3 -- the smoking gun --
 * tells us whether the QPC was actually destroyed: a successful
 * QUERY_QP after a "successful" DESTROY_QP means FW silently
 * no-op'd the destroy.
 *
 * Why a single ioctl: the three commands need to run with no other
 * cmdif traffic in between (the VF is paused for migration but
 * other PF-cdev callers exist), and the bracketing needs to hold
 * device_lock across all three so a concurrent unbind can't pull
 * the rug. Composing this in userspace would race.
 *
 * What this is NOT trying to be: an in-tree consumer of the result.
 * The caller (test_qp_destroy_uid_matrix.sh) interprets the per-cell
 * verdict and updates the design doc's matrix. Once the empirical
 * picture is settled, this ioctl can be removed without breaking
 * any non-debug consumer.
 */
static long vfmig_ioc_probe_qp_teardown(struct mlx5_vfmig_pf *vfmig,
					void __user *uarg)
{
	u32 q_in[MLX5_ST_SZ_DW(query_qp_in)] = {};
	u32 q_out[MLX5_ST_SZ_DW(query_qp_out)];
	u32 d_in[MLX5_ST_SZ_DW(destroy_qp_in)] = {};
	u32 d_out[MLX5_ST_SZ_DW(destroy_qp_out)] = {};
	u32 r_in[MLX5_ST_SZ_DW(qp_2rst_in)] = {};
	u32 r_out[MLX5_ST_SZ_DW(qp_2rst_out)] = {};
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_probe_qp_teardown arg;
	struct mlx5_core_dev *vf_mdev;
	struct pci_dev *vf_pdev;
	struct device_driver *drv;
	void *qpc;
	int err, op_err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (memchr_inv(arg.reserved_in, 0, sizeof(arg.reserved_in)))
		return -EINVAL;
	if (arg.vf_id >= pf_mdev->priv.sriov.num_vfs)
		return -EINVAL;
	if (arg.qpn & 0xff000000)		/* qpn is 24 bits */
		return -EINVAL;
	if (arg.uid_hint & 0xffff0000)		/* uid is 16 bits */
		return -EINVAL;
	if (arg.op_mode != MLX5_VFMIG_QP_TEARDOWN_OP_DESTROY &&
	    arg.op_mode != MLX5_VFMIG_QP_TEARDOWN_OP_2RST)
		return -EINVAL;

	/*
	 * Zero all output fields up-front so partial-fill paths (e.g.
	 * pre-query failure -> we skip op + post-query) don't leak
	 * stack contents to userspace.
	 */
	arg.pre_query_status = 0;
	arg.pre_query_syndrome = 0;
	arg.pre_qpc_state = 0;
	memset(arg.reserved_pre, 0, sizeof(arg.reserved_pre));
	arg.pre_qpc_pd = 0;
	arg.op_status = 0;
	arg.op_syndrome = 0;
	arg.post_query_status = 0;
	arg.post_query_syndrome = 0;
	arg.post_qpc_state = 0;
	memset(arg.reserved_post, 0, sizeof(arg.reserved_post));
	memset(arg.reserved_out, 0, sizeof(arg.reserved_out));

	vf_pdev = vfmig_get_vf_pdev(pf_mdev->pdev, arg.vf_id);
	if (!vf_pdev)
		return -ENODEV;

	device_lock(&vf_pdev->dev);

	drv = vf_pdev->dev.driver;
	if (!drv || strcmp(drv->name, KBUILD_MODNAME)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_qp_teardown: vf %u not bound to %s (driver=%s)\n",
			       arg.vf_id, KBUILD_MODNAME,
			       drv ? drv->name : "<unbound>");
		err = -ENODEV;
		goto out_unlock;
	}

	vf_mdev = pci_get_drvdata(vf_pdev);
	if (!vf_mdev ||
	    !test_bit(MLX5_INTERFACE_STATE_UP, &vf_mdev->intf_state)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_qp_teardown: vf %u mdev not interface-up\n",
			       arg.vf_id);
		err = -ENODEV;
		goto out_unlock;
	}

	/* ---- Step 1: pre-op QUERY_QP ---- */
	memset(q_out, 0, sizeof(q_out));
	MLX5_SET(query_qp_in, q_in, opcode, MLX5_CMD_OP_QUERY_QP);
	MLX5_SET(query_qp_in, q_in, qpn, arg.qpn);
	err = mlx5_cmd_exec(vf_mdev, q_in, sizeof(q_in), q_out, sizeof(q_out));
	if (err) {
		arg.pre_query_status = (u32)(int)err;
		arg.pre_query_syndrome = MLX5_GET(query_qp_out, q_out,
						  syndrome);
		mlx5_core_dbg(pf_mdev,
			      "vfmig: probe_qp_teardown: vf %u qpn 0x%x pre QUERY_QP err %d syndrome 0x%x -- skipping op + post-query\n",
			      arg.vf_id, arg.qpn, err,
			      arg.pre_query_syndrome);
		err = 0;
		goto out_copy;
	}
	qpc = MLX5_ADDR_OF(query_qp_out, q_out, qpc);
	arg.pre_qpc_state = MLX5_GET(qpc, qpc, state);
	arg.pre_qpc_pd    = MLX5_GET(qpc, qpc, pd);
	mlx5_core_dbg(pf_mdev,
		      "vfmig: probe_qp_teardown: vf %u qpn 0x%x pre-op state=%u pd=0x%x\n",
		      arg.vf_id, arg.qpn, arg.pre_qpc_state, arg.pre_qpc_pd);

	/* ---- Step 2: the op under test ---- */
	if (arg.op_mode == MLX5_VFMIG_QP_TEARDOWN_OP_DESTROY) {
		MLX5_SET(destroy_qp_in, d_in, opcode, MLX5_CMD_OP_DESTROY_QP);
		MLX5_SET(destroy_qp_in, d_in, qpn, arg.qpn);
		MLX5_SET(destroy_qp_in, d_in, uid, arg.uid_hint);
		op_err = mlx5_cmd_exec(vf_mdev, d_in, sizeof(d_in),
				       d_out, sizeof(d_out));
		arg.op_syndrome = MLX5_GET(destroy_qp_out, d_out, syndrome);
		mlx5_core_info(pf_mdev,
			       "vfmig: probe_qp_teardown: vf %u qpn 0x%x DESTROY_QP(uid=0x%x) -> err=%d syndrome=0x%x\n",
			       arg.vf_id, arg.qpn, arg.uid_hint, op_err,
			       arg.op_syndrome);
	} else {
		/* MLX5_VFMIG_QP_TEARDOWN_OP_2RST */
		MLX5_SET(qp_2rst_in, r_in, opcode, MLX5_CMD_OP_2RST_QP);
		MLX5_SET(qp_2rst_in, r_in, qpn, arg.qpn);
		MLX5_SET(qp_2rst_in, r_in, uid, arg.uid_hint);
		op_err = mlx5_cmd_exec(vf_mdev, r_in, sizeof(r_in),
				       r_out, sizeof(r_out));
		arg.op_syndrome = MLX5_GET(qp_2rst_out, r_out, syndrome);
		mlx5_core_info(pf_mdev,
			       "vfmig: probe_qp_teardown: vf %u qpn 0x%x 2RST_QP(uid=0x%x) -> err=%d syndrome=0x%x\n",
			       arg.vf_id, arg.qpn, arg.uid_hint, op_err,
			       arg.op_syndrome);
	}
	arg.op_status = (u32)op_err;	/* zero on FW accept; cast through int */

	/* ---- Step 3: post-op QUERY_QP ---- */
	memset(q_out, 0, sizeof(q_out));
	memset(q_in, 0, sizeof(q_in));
	MLX5_SET(query_qp_in, q_in, opcode, MLX5_CMD_OP_QUERY_QP);
	MLX5_SET(query_qp_in, q_in, qpn, arg.qpn);
	err = mlx5_cmd_exec(vf_mdev, q_in, sizeof(q_in), q_out, sizeof(q_out));
	if (err) {
		arg.post_query_status = (u32)(int)err;
		arg.post_query_syndrome = MLX5_GET(query_qp_out, q_out,
						   syndrome);
		mlx5_core_info(pf_mdev,
			       "vfmig: probe_qp_teardown: vf %u qpn 0x%x post QUERY_QP err %d syndrome 0x%x (QPC likely destroyed)\n",
			       arg.vf_id, arg.qpn, err,
			       arg.post_query_syndrome);
		err = 0;
		goto out_copy;
	}
	qpc = MLX5_ADDR_OF(query_qp_out, q_out, qpc);
	arg.post_qpc_state = MLX5_GET(qpc, qpc, state);
	mlx5_core_info(pf_mdev,
		       "vfmig: probe_qp_teardown: vf %u qpn 0x%x post-op QPC ALIVE state=%u (op_mode=%u uid_hint=0x%x op_status=0x%x op_syndrome=0x%x)\n",
		       arg.vf_id, arg.qpn, arg.post_qpc_state,
		       arg.op_mode, arg.uid_hint,
		       arg.op_status, arg.op_syndrome);

out_copy:
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		err = -EFAULT;

out_unlock:
	device_unlock(&vf_pdev->dev);
	pci_dev_put(vf_pdev);
	return err;
}

/*
 * MLX5_VFMIG_IOC_PROBE_DEALLOC_PD handler -- experimental, ?S3b
 * "DEALLOC_PD uid-gating" empirical (follow-up to PROBE_QP_TEARDOWN).
 *
 * Issues DEALLOC_PD(pdn=@pdn, uid=@uid_hint) on the bound VF mdev's
 * cmdif and reports the FW result. Unlike PROBE_PD (which is non-
 * destructive), this ioctl IS destructive on success: a uid-accepted
 * dealloc removes the source's PDC from the destination VHCA. Use
 * one source PDC per uid_hint cell.
 *
 * No bracketing query opcode exists for PDs (FW has no QUERY_PD),
 * so PDC existence pre/post is left to the caller via PROBE_PD's
 * CREATE_MKEY-acceptance test. The harness does this composition.
 *
 * Locking, VF mdev lookup, cmdif uid: same shape as PROBE_PD /
 * PROBE_QP_TEARDOWN.
 */
static long vfmig_ioc_probe_dealloc_pd(struct mlx5_vfmig_pf *vfmig,
				       void __user *uarg)
{
	u32 in[MLX5_ST_SZ_DW(dealloc_pd_in)] = {};
	u32 out[MLX5_ST_SZ_DW(dealloc_pd_out)] = {};
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_probe_dealloc_pd arg;
	struct mlx5_core_dev *vf_mdev;
	struct pci_dev *vf_pdev;
	struct device_driver *drv;
	int err, op_err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (memchr_inv(arg.reserved_in, 0, sizeof(arg.reserved_in)))
		return -EINVAL;
	if (arg.vf_id >= pf_mdev->priv.sriov.num_vfs)
		return -EINVAL;
	if (arg.pdn & 0xff000000)		/* pdn is 24 bits */
		return -EINVAL;
	if (arg.uid_hint & 0xffff0000)		/* uid is 16 bits */
		return -EINVAL;

	arg.op_status = 0;
	arg.op_syndrome = 0;
	memset(arg.reserved_out, 0, sizeof(arg.reserved_out));

	vf_pdev = vfmig_get_vf_pdev(pf_mdev->pdev, arg.vf_id);
	if (!vf_pdev)
		return -ENODEV;

	device_lock(&vf_pdev->dev);

	drv = vf_pdev->dev.driver;
	if (!drv || strcmp(drv->name, KBUILD_MODNAME)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_dealloc_pd: vf %u not bound to %s (driver=%s)\n",
			       arg.vf_id, KBUILD_MODNAME,
			       drv ? drv->name : "<unbound>");
		err = -ENODEV;
		goto out_unlock;
	}

	vf_mdev = pci_get_drvdata(vf_pdev);
	if (!vf_mdev ||
	    !test_bit(MLX5_INTERFACE_STATE_UP, &vf_mdev->intf_state)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_dealloc_pd: vf %u mdev not interface-up\n",
			       arg.vf_id);
		err = -ENODEV;
		goto out_unlock;
	}

	MLX5_SET(dealloc_pd_in, in, opcode, MLX5_CMD_OP_DEALLOC_PD);
	MLX5_SET(dealloc_pd_in, in, uid, arg.uid_hint);
	MLX5_SET(dealloc_pd_in, in, pd, arg.pdn);

	op_err = mlx5_cmd_exec(vf_mdev, in, sizeof(in), out, sizeof(out));
	arg.op_syndrome = MLX5_GET(dealloc_pd_out, out, syndrome);
	arg.op_status = (u32)op_err;

	mlx5_core_info(pf_mdev,
		       "vfmig: probe_dealloc_pd: vf %u pdn 0x%x DEALLOC_PD(uid=0x%x) -> err=%d syndrome=0x%x\n",
		       arg.vf_id, arg.pdn, arg.uid_hint, op_err,
		       arg.op_syndrome);

	if (copy_to_user(uarg, &arg, sizeof(arg))) {
		err = -EFAULT;
		goto out_unlock;
	}
	err = 0;

out_unlock:
	device_unlock(&vf_pdev->dev);
	pci_dev_put(vf_pdev);
	return err;
}

/*
 * MLX5_VFMIG_IOC_PROBE_CQ_DESTROY handler -- experimental, §S3b
 * "Generalising to QP/CQ" empirical (CQ branch).
 *
 * Issues QUERY_CQ -> DESTROY_CQ(@cqn, uid=@uid_hint) -> QUERY_CQ
 * on the bound VF mdev's cmdif and reports the bracketed FW
 * results so the harness can verify cross-uid DESTROY_CQ behaves
 * the same way as DESTROY_QP (cross-uid honored, no kernel gate
 * needed) versus DEALLOC_PD (cross-uid honored opcode-wise but
 * the resource is unknown to the dest VHCA's allocator, kernel
 * gate needed). The op-under-test errno is captured in
 * @op_status / @op_syndrome and the call returns 0 on FW reject.
 *
 * Locking, VF mdev lookup, and cmdif-uid handling all mirror
 * vfmig_ioc_probe_qp_teardown above; see that handler's comment
 * block for the full rationale.
 */
static long vfmig_ioc_probe_cq_destroy(struct mlx5_vfmig_pf *vfmig,
				       void __user *uarg)
{
	u32 q_in[MLX5_ST_SZ_DW(query_cq_in)] = {};
	u32 q_out[MLX5_ST_SZ_DW(query_cq_out)];
	u32 d_in[MLX5_ST_SZ_DW(destroy_cq_in)] = {};
	u32 d_out[MLX5_ST_SZ_DW(destroy_cq_out)] = {};
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_probe_cq_destroy arg;
	struct mlx5_core_dev *vf_mdev;
	struct pci_dev *vf_pdev;
	struct device_driver *drv;
	void *cqc;
	int err, op_err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (memchr_inv(arg.reserved_in, 0, sizeof(arg.reserved_in)))
		return -EINVAL;
	if (arg.vf_id >= pf_mdev->priv.sriov.num_vfs)
		return -EINVAL;
	if (arg.cqn & 0xff000000)
		return -EINVAL;
	if (arg.uid_hint & 0xffff0000)
		return -EINVAL;

	arg.pre_query_status = 0;
	arg.pre_query_syndrome = 0;
	arg.pre_cqc_status = 0;
	memset(arg.reserved_pre, 0, sizeof(arg.reserved_pre));
	arg.op_status = 0;
	arg.op_syndrome = 0;
	arg.post_query_status = 0;
	arg.post_query_syndrome = 0;
	arg.post_cqc_status = 0;
	memset(arg.reserved_post, 0, sizeof(arg.reserved_post));
	memset(arg.reserved_out, 0, sizeof(arg.reserved_out));

	vf_pdev = vfmig_get_vf_pdev(pf_mdev->pdev, arg.vf_id);
	if (!vf_pdev)
		return -ENODEV;

	device_lock(&vf_pdev->dev);

	drv = vf_pdev->dev.driver;
	if (!drv || strcmp(drv->name, KBUILD_MODNAME)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_cq_destroy: vf %u not bound to %s (driver=%s)\n",
			       arg.vf_id, KBUILD_MODNAME,
			       drv ? drv->name : "<unbound>");
		err = -ENODEV;
		goto out_unlock;
	}

	vf_mdev = pci_get_drvdata(vf_pdev);
	if (!vf_mdev ||
	    !test_bit(MLX5_INTERFACE_STATE_UP, &vf_mdev->intf_state)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_cq_destroy: vf %u mdev not interface-up\n",
			       arg.vf_id);
		err = -ENODEV;
		goto out_unlock;
	}

	memset(q_out, 0, sizeof(q_out));
	MLX5_SET(query_cq_in, q_in, opcode, MLX5_CMD_OP_QUERY_CQ);
	MLX5_SET(query_cq_in, q_in, cqn, arg.cqn);
	err = mlx5_cmd_exec(vf_mdev, q_in, sizeof(q_in), q_out, sizeof(q_out));
	if (err) {
		arg.pre_query_status = (u32)(int)err;
		arg.pre_query_syndrome = MLX5_GET(query_cq_out, q_out,
						  syndrome);
		mlx5_core_dbg(pf_mdev,
			      "vfmig: probe_cq_destroy: vf %u cqn 0x%x pre QUERY_CQ err %d syndrome 0x%x -- skipping op + post-query\n",
			      arg.vf_id, arg.cqn, err,
			      arg.pre_query_syndrome);
		err = 0;
		goto out_copy;
	}
	cqc = MLX5_ADDR_OF(query_cq_out, q_out, cq_context);
	arg.pre_cqc_status = MLX5_GET(cqc, cqc, status);
	mlx5_core_dbg(pf_mdev,
		      "vfmig: probe_cq_destroy: vf %u cqn 0x%x pre-op status=0x%x\n",
		      arg.vf_id, arg.cqn, arg.pre_cqc_status);

	MLX5_SET(destroy_cq_in, d_in, opcode, MLX5_CMD_OP_DESTROY_CQ);
	MLX5_SET(destroy_cq_in, d_in, cqn, arg.cqn);
	MLX5_SET(destroy_cq_in, d_in, uid, arg.uid_hint);
	op_err = mlx5_cmd_exec(vf_mdev, d_in, sizeof(d_in),
			       d_out, sizeof(d_out));
	arg.op_syndrome = MLX5_GET(destroy_cq_out, d_out, syndrome);
	arg.op_status = (u32)op_err;
	mlx5_core_info(pf_mdev,
		       "vfmig: probe_cq_destroy: vf %u cqn 0x%x DESTROY_CQ(uid=0x%x) -> err=%d syndrome=0x%x\n",
		       arg.vf_id, arg.cqn, arg.uid_hint, op_err,
		       arg.op_syndrome);

	memset(q_out, 0, sizeof(q_out));
	memset(q_in, 0, sizeof(q_in));
	MLX5_SET(query_cq_in, q_in, opcode, MLX5_CMD_OP_QUERY_CQ);
	MLX5_SET(query_cq_in, q_in, cqn, arg.cqn);
	err = mlx5_cmd_exec(vf_mdev, q_in, sizeof(q_in), q_out, sizeof(q_out));
	if (err) {
		arg.post_query_status = (u32)(int)err;
		arg.post_query_syndrome = MLX5_GET(query_cq_out, q_out,
						   syndrome);
		mlx5_core_info(pf_mdev,
			       "vfmig: probe_cq_destroy: vf %u cqn 0x%x post QUERY_CQ err %d syndrome 0x%x (CQC likely destroyed)\n",
			       arg.vf_id, arg.cqn, err,
			       arg.post_query_syndrome);
		err = 0;
		goto out_copy;
	}
	cqc = MLX5_ADDR_OF(query_cq_out, q_out, cq_context);
	arg.post_cqc_status = MLX5_GET(cqc, cqc, status);
	mlx5_core_info(pf_mdev,
		       "vfmig: probe_cq_destroy: vf %u cqn 0x%x post-op CQC ALIVE status=0x%x (uid_hint=0x%x op_status=0x%x op_syndrome=0x%x)\n",
		       arg.vf_id, arg.cqn, arg.post_cqc_status,
		       arg.uid_hint, arg.op_status, arg.op_syndrome);

out_copy:
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		err = -EFAULT;

out_unlock:
	device_unlock(&vf_pdev->dev);
	pci_dev_put(vf_pdev);
	return err;
}

/*
 * MLX5_VFMIG_IOC_PROBE_MR_DESTROY handler -- experimental, §S3b
 * "Generalising to QP/CQ" empirical (MR/mkey branch).
 *
 * Issues QUERY_MKEY -> DESTROY_MKEY(@mkey_index, uid=@uid_hint) ->
 * QUERY_MKEY on the bound VF mdev's cmdif. Identical bracketing
 * pattern to vfmig_ioc_probe_cq_destroy; see that comment block
 * for verdict semantics.
 */
static long vfmig_ioc_probe_mr_destroy(struct mlx5_vfmig_pf *vfmig,
				       void __user *uarg)
{
	u32 q_in[MLX5_ST_SZ_DW(query_mkey_in)] = {};
	u32 q_out[MLX5_ST_SZ_DW(query_mkey_out)];
	u32 d_in[MLX5_ST_SZ_DW(destroy_mkey_in)] = {};
	u32 d_out[MLX5_ST_SZ_DW(destroy_mkey_out)] = {};
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_probe_mr_destroy arg;
	struct mlx5_core_dev *vf_mdev;
	struct pci_dev *vf_pdev;
	struct device_driver *drv;
	void *mkc;
	int err, op_err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (memchr_inv(arg.reserved_in, 0, sizeof(arg.reserved_in)))
		return -EINVAL;
	if (arg.vf_id >= pf_mdev->priv.sriov.num_vfs)
		return -EINVAL;
	if (arg.mkey_index & 0xff000000)
		return -EINVAL;
	if (arg.uid_hint & 0xffff0000)
		return -EINVAL;

	arg.pre_query_status = 0;
	arg.pre_query_syndrome = 0;
	arg.pre_mkc_free = 0;
	memset(arg.reserved_pre, 0, sizeof(arg.reserved_pre));
	arg.op_status = 0;
	arg.op_syndrome = 0;
	arg.post_query_status = 0;
	arg.post_query_syndrome = 0;
	arg.post_mkc_free = 0;
	memset(arg.reserved_post, 0, sizeof(arg.reserved_post));
	memset(arg.reserved_out, 0, sizeof(arg.reserved_out));

	vf_pdev = vfmig_get_vf_pdev(pf_mdev->pdev, arg.vf_id);
	if (!vf_pdev)
		return -ENODEV;

	device_lock(&vf_pdev->dev);

	drv = vf_pdev->dev.driver;
	if (!drv || strcmp(drv->name, KBUILD_MODNAME)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_mr_destroy: vf %u not bound to %s (driver=%s)\n",
			       arg.vf_id, KBUILD_MODNAME,
			       drv ? drv->name : "<unbound>");
		err = -ENODEV;
		goto out_unlock;
	}

	vf_mdev = pci_get_drvdata(vf_pdev);
	if (!vf_mdev ||
	    !test_bit(MLX5_INTERFACE_STATE_UP, &vf_mdev->intf_state)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: probe_mr_destroy: vf %u mdev not interface-up\n",
			       arg.vf_id);
		err = -ENODEV;
		goto out_unlock;
	}

	memset(q_out, 0, sizeof(q_out));
	MLX5_SET(query_mkey_in, q_in, opcode, MLX5_CMD_OP_QUERY_MKEY);
	MLX5_SET(query_mkey_in, q_in, mkey_index, arg.mkey_index);
	err = mlx5_cmd_exec(vf_mdev, q_in, sizeof(q_in), q_out, sizeof(q_out));
	if (err) {
		arg.pre_query_status = (u32)(int)err;
		arg.pre_query_syndrome = MLX5_GET(query_mkey_out, q_out,
						  syndrome);
		mlx5_core_dbg(pf_mdev,
			      "vfmig: probe_mr_destroy: vf %u mkey_idx 0x%x pre QUERY_MKEY err %d syndrome 0x%x -- skipping op + post-query\n",
			      arg.vf_id, arg.mkey_index, err,
			      arg.pre_query_syndrome);
		err = 0;
		goto out_copy;
	}
	mkc = MLX5_ADDR_OF(query_mkey_out, q_out, memory_key_mkey_entry);
	arg.pre_mkc_free = MLX5_GET(mkc, mkc, free);
	mlx5_core_dbg(pf_mdev,
		      "vfmig: probe_mr_destroy: vf %u mkey_idx 0x%x pre-op free=%u\n",
		      arg.vf_id, arg.mkey_index, arg.pre_mkc_free);

	MLX5_SET(destroy_mkey_in, d_in, opcode, MLX5_CMD_OP_DESTROY_MKEY);
	MLX5_SET(destroy_mkey_in, d_in, mkey_index, arg.mkey_index);
	MLX5_SET(destroy_mkey_in, d_in, uid, arg.uid_hint);
	op_err = mlx5_cmd_exec(vf_mdev, d_in, sizeof(d_in),
			       d_out, sizeof(d_out));
	arg.op_syndrome = MLX5_GET(destroy_mkey_out, d_out, syndrome);
	arg.op_status = (u32)op_err;
	mlx5_core_info(pf_mdev,
		       "vfmig: probe_mr_destroy: vf %u mkey_idx 0x%x DESTROY_MKEY(uid=0x%x) -> err=%d syndrome=0x%x\n",
		       arg.vf_id, arg.mkey_index, arg.uid_hint, op_err,
		       arg.op_syndrome);

	memset(q_out, 0, sizeof(q_out));
	memset(q_in, 0, sizeof(q_in));
	MLX5_SET(query_mkey_in, q_in, opcode, MLX5_CMD_OP_QUERY_MKEY);
	MLX5_SET(query_mkey_in, q_in, mkey_index, arg.mkey_index);
	err = mlx5_cmd_exec(vf_mdev, q_in, sizeof(q_in), q_out, sizeof(q_out));
	if (err) {
		arg.post_query_status = (u32)(int)err;
		arg.post_query_syndrome = MLX5_GET(query_mkey_out, q_out,
						   syndrome);
		mlx5_core_info(pf_mdev,
			       "vfmig: probe_mr_destroy: vf %u mkey_idx 0x%x post QUERY_MKEY err %d syndrome 0x%x (MKC likely destroyed)\n",
			       arg.vf_id, arg.mkey_index, err,
			       arg.post_query_syndrome);
		err = 0;
		goto out_copy;
	}
	mkc = MLX5_ADDR_OF(query_mkey_out, q_out, memory_key_mkey_entry);
	arg.post_mkc_free = MLX5_GET(mkc, mkc, free);
	mlx5_core_info(pf_mdev,
		       "vfmig: probe_mr_destroy: vf %u mkey_idx 0x%x post-op MKC ALIVE free=%u (uid_hint=0x%x op_status=0x%x op_syndrome=0x%x)\n",
		       arg.vf_id, arg.mkey_index, arg.post_mkc_free,
		       arg.uid_hint, arg.op_status, arg.op_syndrome);

out_copy:
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		err = -EFAULT;

out_unlock:
	device_unlock(&vf_pdev->dev);
	pci_dev_put(vf_pdev);
	return err;
}

/*
 * MLX5_VFMIG_IOC_QUERY_AWAITING_BIND handler -- user_mr_dma stage-2
 * success-criterion accessor.
 *
 * Counts the destination-side replay placeholders that landed in
 * the per-VF vfmig_iova_domain via VFMIG_WIRE_TAG_HOST_USER_PAGE
 * records during LOAD but have not yet been consumed by stage-3's
 * hint-aware vfmig_dma_ops.map_sg binder. The total + per-kind
 * breakdown lets the test_user_object_replay.sh harness assert
 * "source emitted N records of kind k -> destination installed N
 * placeholders of kind k".
 *
 * Locking: vfmig->lock held (read) by the ioctl dispatcher. The
 * SET_TRACKED handler installs/clears @vfmig_iova_dom under the
 * same lock taken for writing, so the pointer is stable for the
 * duration of this call. vfmig_iova_count_awaiting_bind() takes
 * the per-domain mutex internally to iterate the page registry.
 *
 * Unlike PROBE_PD / PROBE_MKEY / PROBE_UID this ioctl does NOT
 * require the VF to be bound to mlx5_core: the registry lives on
 * the PF (under the SET_TRACKED-allocated unmanaged iommu_domain),
 * so the canonical use case is post-LOAD, pre-bind validation by
 * the harness.
 */
static long vfmig_ioc_query_awaiting_bind(struct mlx5_vfmig_pf *vfmig,
					  void __user *uarg)
{
	struct mlx5_vfmig_query_awaiting_bind arg;
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_core_sriov *sriov = &pf_mdev->priv.sriov;
	struct vfmig_iova_domain *dom;
	u64 total = 0;
	u64 by_kind[VFMIG_HUOBJ_KIND_NR] = {};
	unsigned int k;
	int err;

	BUILD_BUG_ON(VFMIG_HUOBJ_KIND_NR >
		     MLX5_VFMIG_QUERY_AWAITING_BIND_NR_KINDS);

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved_in)
		return -EINVAL;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	dom = sriov->vfs_ctx[arg.vf_id].vfmig_iova_dom;
	if (!dom) {
		mlx5_core_dbg(pf_mdev,
			      "vfmig: query_awaiting_bind: vf %u not tracked\n",
			      arg.vf_id);
		return -ENODEV;
	}

	err = vfmig_iova_count_awaiting_bind(dom, &total, by_kind);
	if (err)
		return err;

	arg.total = total;
	memset(arg.count_by_kind, 0, sizeof(arg.count_by_kind));
	for (k = 0; k < VFMIG_HUOBJ_KIND_NR; k++)
		arg.count_by_kind[k] = by_kind[k];
	memset(arg.reserved_out, 0, sizeof(arg.reserved_out));

	mlx5_core_dbg(pf_mdev,
		      "vfmig: query_awaiting_bind: vf %u total=%llu (MR=%llu CQ=%llu QP=%llu SRQ=%llu DBR=%llu)\n",
		      arg.vf_id, total,
		      by_kind[VFMIG_HUOBJ_KIND_MR],
		      by_kind[VFMIG_HUOBJ_KIND_CQ],
		      by_kind[VFMIG_HUOBJ_KIND_QP],
		      by_kind[VFMIG_HUOBJ_KIND_SRQ],
		      by_kind[VFMIG_HUOBJ_KIND_DBR]);

	if (copy_to_user(uarg, &arg, sizeof(arg)))
		return -EFAULT;
	return 0;
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

	/*
	 * The byte that used to be @reserved is now @tracked (out).
	 * No input check on it -- the kernel always overwrites the
	 * field on success or on -ERANGE -- so old userspace that
	 * happens to have a non-zero byte in there still gets a clean
	 * answer instead of -EINVAL. Same posture for @vf_uuid /
	 * @reserved_out, which the kernel always overwrites.
	 */

	sriov = &vfmig->pf_mdev->priv.sriov;

	arg.num_vfs = sriov->num_vfs;
	memset(arg.reserved_out, 0, sizeof(arg.reserved_out));
	if (arg.vf_id >= sriov->num_vfs) {
		arg.vhca_id = 0;
		arg.restored = 0;
		arg.tracked = 0;
		export_uuid(arg.vf_uuid, &uuid_null);
		if (copy_to_user(uarg, &arg, sizeof(arg)))
			return -EFAULT;
		return -ERANGE;
	}

	err = vfmig_query_vhca_id(vfmig->pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	arg.vhca_id = vhca_id;
	/*
	 * @restored, @tracked, and @vf_uuid are read without explicit
	 * locking, matching the rest of the QUERY_VF path. They are
	 * mutated only via the SET_TRACKED / MARK_RESTORED /
	 * SET_VF_UUID ioctls, so a torn read just produces a
	 * one-cycle stale answer for a userspace observer that's
	 * racing those ioctls against this query. Stable values
	 * during the typical "orchestrator stamps once at
	 * provisioning, CRIU polls QUERY_VF at init" use case --
	 * which is the only access pattern these fields are designed
	 * for.
	 */
	arg.restored = sriov->vfs_ctx[arg.vf_id].restored;
	arg.tracked = sriov->vfs_ctx[arg.vf_id].vfmig_tracked;
	export_uuid(arg.vf_uuid, &sriov->vfs_ctx[arg.vf_id].vf_uuid);
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

/*
 * Once the parser has seen all the records the STREAM_HEADER promised
 * (both HOST_PAGE and HOST_USER_PAGE), verify the running manifest CRC
 * matches what the source pinned and arm at-probe drift detection on
 * the destination's IOVA domain. Latched by @records_finalized so
 * callers from both HP_REPLAY and HUP_REPLAY can invoke it without
 * duplicating side effects. Safe to call when only one of (HP, HUP)
 * has hit its expected count -- the helper is a no-op until both do.
 */
static int
vfmig_load_maybe_finalize_records(struct mlx5_vfmig_load_ctx *ctx)
{
	if (ctx->records_finalized)
		return 0;
	if (ctx->hp_seen != ctx->hp_expected ||
	    ctx->hup_seen != ctx->hup_expected)
		return 0;

	if (ctx->manifest_crc_have != ctx->manifest_crc_want) {
		mlx5_core_warn(ctx->vfmig->pf_mdev,
			       "vfmig: vf %u: manifest CRC mismatch (have 0x%08x, want 0x%08x); record identity stream corrupted\n",
			       ctx->vf_id,
			       ctx->manifest_crc_have,
			       ctx->manifest_crc_want);
		return -EPROTO;
	}
	vfmig_iova_arm_drift_detection(ctx->iova_dom);
	ctx->records_finalized = true;
	return 0;
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

	/*
	 * The very first record in any vfmig blob MUST be a
	 * STREAM_HEADER. Reject anything else as a malformed blob
	 * before we touch the IOVA domain or the FW. Older blobs from
	 * pre-stream-header development trees fall into this branch
	 * and surface as -EPROTO with a clear message rather than
	 * silently parsing fields with a different layout.
	 */
	if (!ctx->stream_hdr_seen && tag != VFMIG_WIRE_TAG_STREAM_HEADER) {
		mlx5_core_warn(ctx->vfmig->pf_mdev,
			       "vfmig: vf %u: first record tag 0x%x is not STREAM_HEADER (0x%x); blob is not in this kernel's wire format\n",
			       ctx->vf_id, tag,
			       VFMIG_WIRE_TAG_STREAM_HEADER);
		return -EPROTO;
	}

	switch (tag) {
	case VFMIG_WIRE_TAG_STREAM_HEADER:
		if (ctx->stream_hdr_seen) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: duplicate STREAM_HEADER record\n",
				       ctx->vf_id);
			return -EPROTO;
		}
		if (record_size != sizeof(struct vfmig_stream_header))
			return -EPROTO;
		ctx->stream_hdr_filled = 0;
		ctx->state = VFMIG_LS_STREAM_HDR_READ;
		return 0;
	case VFMIG_WIRE_TAG_FW_DATA:
		/*
		 * FW_DATA must come AFTER all promised HOST_PAGE +
		 * HOST_USER_PAGE records. If the source declared
		 * num_pages > 0 or num_user_pages > 0 in the stream
		 * header but the parser reaches FW_DATA before replaying
		 * the full set, the blob is truncated / malformed.
		 */
		if (ctx->hp_seen != ctx->hp_expected ||
		    ctx->hup_seen != ctx->hup_expected) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: FW_DATA before all records (HOST_PAGE %llu / %llu, HOST_USER_PAGE %llu / %llu); aborting\n",
				       ctx->vf_id,
				       (unsigned long long)ctx->hp_seen,
				       (unsigned long long)ctx->hp_expected,
				       (unsigned long long)ctx->hup_seen,
				       (unsigned long long)ctx->hup_expected);
			return -EPROTO;
		}
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
		if (ctx->hp_seen >= ctx->hp_expected) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: HOST_PAGE record beyond declared num_pages=%llu\n",
				       ctx->vf_id,
				       (unsigned long long)ctx->hp_expected);
			return -EPROTO;
		}
		ctx->hp_subhdr_filled = 0;
		ctx->hp_filled = 0;
		ctx->state = VFMIG_LS_HP_READ_SUBHDR;
		return 0;
	case VFMIG_WIRE_TAG_HOST_USER_PAGE:
		/*
		 * HOST_USER_PAGE replays into the per-VF IOVA domain as
		 * awaiting_bind=true placeholders. Same SET_TRACKED
		 * precondition as HOST_PAGE: a HOST_USER_PAGE record in
		 * an untracked-destination LOAD is a userspace ordering
		 * bug, not something we can paper over.
		 *
		 * Identity-only record, so record_size must equal exactly
		 * sizeof(struct vfmig_host_user_page_record) -- no
		 * trailing contents.
		 */
		if (!ctx->iova_dom) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: HOST_USER_PAGE record in blob but destination not SET_TRACKED'd; aborting LOAD\n",
				       ctx->vf_id);
			return -EINVAL;
		}
		if (record_size != sizeof(struct vfmig_host_user_page_record))
			return -EINVAL;
		if (ctx->hup_seen >= ctx->hup_expected) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: HOST_USER_PAGE record beyond declared num_user_pages=%llu\n",
				       ctx->vf_id,
				       (unsigned long long)ctx->hup_expected);
			return -EPROTO;
		}
		ctx->hup_subhdr_filled = 0;
		ctx->state = VFMIG_LS_HUP_READ_SUBHDR;
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

	case VFMIG_LS_STREAM_HDR_READ: {
		/*
		 * Pull the 24-byte stream header into stream_hdr_buf,
		 * verify magic / version, pin num_pages and
		 * manifest_crc32 for use by the HP_REPLAY path.
		 */
		size_t need = sizeof(ctx->stream_hdr_buf) -
			      ctx->stream_hdr_filled;
		size_t take = min(need, *left);
		struct vfmig_stream_header sh;

		if (take) {
			if (copy_from_user(ctx->stream_hdr_buf +
						ctx->stream_hdr_filled,
					   *ubuf, take))
				return -EFAULT;
			ctx->stream_hdr_filled += take;
			*ubuf += take;
			*left -= take;
			*progressed = true;
		}
		if (ctx->stream_hdr_filled < sizeof(ctx->stream_hdr_buf))
			return 0;

		memcpy(&sh, ctx->stream_hdr_buf, sizeof(sh));
		if (le32_to_cpu(sh.magic) != VFMIG_WIRE_MAGIC) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: stream header magic 0x%x != 0x%x\n",
				       ctx->vf_id,
				       le32_to_cpu(sh.magic),
				       VFMIG_WIRE_MAGIC);
			return -EPROTO;
		}
		if (le32_to_cpu(sh.version) != VFMIG_STREAM_VERSION) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: stream version %u not supported (this kernel: %u)\n",
				       ctx->vf_id,
				       le32_to_cpu(sh.version),
				       VFMIG_STREAM_VERSION);
			return -EOPNOTSUPP;
		}

		ctx->hp_expected	= le64_to_cpu(sh.num_pages);
		ctx->hup_expected	= le32_to_cpu(sh.num_user_pages);
		ctx->manifest_crc_want	= le32_to_cpu(sh.manifest_crc32);
		ctx->manifest_crc_have	= 0;
		ctx->hp_seen		= 0;
		ctx->hup_seen		= 0;
		ctx->records_finalized	= false;
		ctx->stream_hdr_seen	= true;

		/*
		 * If the source declared HOST_PAGE or HOST_USER_PAGE
		 * records, the destination must have a tracked IOVA
		 * domain to replay them into. Reject early so an
		 * untracked-destination misconfiguration surfaces here,
		 * not later when the first record arrives.
		 */
		if ((ctx->hp_expected || ctx->hup_expected) && !ctx->iova_dom) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: stream declares %llu HOST_PAGE + %llu HOST_USER_PAGE records but destination not SET_TRACKED'd\n",
				       ctx->vf_id,
				       (unsigned long long)ctx->hp_expected,
				       (unsigned long long)ctx->hup_expected);
			return -EINVAL;
		}

		/*
		 * Tracked source with zero records (HOST_PAGE +
		 * HOST_USER_PAGE both zero): no replays will arrive, so
		 * the manifest CRC -- which is folded over zero bytes on
		 * the SAVE side -- must be the initial crc32_le value of
		 * 0. Verify here and arm drift detection immediately,
		 * since neither HP_REPLAY nor HUP_REPLAY's finalize arms
		 * will ever be reached. (If hp_expected==0 but
		 * hup_expected>0, finalize fires from HUP_REPLAY; and
		 * vice versa.)
		 */
		if (ctx->iova_dom && ctx->hp_expected == 0 &&
		    ctx->hup_expected == 0) {
			if (ctx->manifest_crc_want != 0) {
				mlx5_core_warn(ctx->vfmig->pf_mdev,
					       "vfmig: vf %u: stream header declares 0 records but non-zero manifest CRC 0x%08x\n",
					       ctx->vf_id,
					       ctx->manifest_crc_want);
				return -EPROTO;
			}
			vfmig_iova_arm_drift_detection(ctx->iova_dom);
			ctx->records_finalized = true;
		}

		ctx->state = VFMIG_LS_READ_HEADER;
		*progressed = true;
		return 0;
	}

	case VFMIG_LS_HP_READ_SUBHDR: {
		/*
		 * Pull the vfmig_host_page_record out of the stream
		 * into ctx->hp_subhdr_buf, then parse the (slot,
		 * instance_key, iova, len, flags) tuple. The payload
		 * size MUST match what the record header advertised:
		 * record_size = sizeof(subhdr) + len.
		 */
		size_t need = sizeof(ctx->hp_subhdr_buf) - ctx->hp_subhdr_filled;
		size_t take = min(need, *left);
		struct vfmig_host_page_record subhdr;
		u64 declared_payload;
		u32 hp_flags;
		u32 slot_id;

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
		slot_id		   = le32_to_cpu(subhdr.slot_id);
		hp_flags	   = le32_to_cpu(subhdr.flags);
		ctx->hp_instance_key = le64_to_cpu(subhdr.instance_key);
		ctx->hp_iova	   = le64_to_cpu(subhdr.iova);
		ctx->hp_len	   = le64_to_cpu(subhdr.len);

		if (hp_flags) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: HOST_PAGE flags=0x%x set; this kernel reserves all bits (must be 0)\n",
				       ctx->vf_id, hp_flags);
			return -EOPNOTSUPP;
		}
		if (slot_id <= VFMIG_SLOT_INVALID || slot_id >= VFMIG_SLOT_NR) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: HOST_PAGE slot_id %u out of range [1, %u)\n",
				       ctx->vf_id, slot_id, VFMIG_SLOT_NR);
			return -EPROTO;
		}
		ctx->hp_slot = (enum vfmig_iova_slot)slot_id;

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

	case VFMIG_LS_HP_REPLAY: {
		struct vfmig_host_page_record subhdr;

		err = vfmig_iova_replay_page(ctx->iova_dom,
					     ctx->hp_slot,
					     ctx->hp_instance_key,
					     ctx->hp_iova,
					     ctx->hp_contents, ctx->hp_len);
		kvfree(ctx->hp_contents);
		ctx->hp_contents = NULL;
		if (err) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: replay_page(slot=%u key=0x%llx iova=0x%llx len=%llu) failed: %d\n",
				       ctx->vf_id, ctx->hp_slot,
				       (unsigned long long)ctx->hp_instance_key,
				       (unsigned long long)ctx->hp_iova,
				       (unsigned long long)ctx->hp_len, err);
			return err;
		}

		/*
		 * Fold this record's identity tuple into the running
		 * manifest CRC. Field encoding mirrors the SAVE side
		 * (see vfmig_save_hp_emit_cb): four __le fields in
		 * declaration order, with @subhdr re-encoded from
		 * ctx->hp_* so we hash exactly the wire bytes regardless
		 * of struct padding.
		 */
		subhdr.slot_id      = cpu_to_le32(ctx->hp_slot);
		subhdr.flags	    = 0;
		subhdr.instance_key = cpu_to_le64(ctx->hp_instance_key);
		subhdr.iova	    = cpu_to_le64(ctx->hp_iova);
		subhdr.len	    = cpu_to_le64(ctx->hp_len);
		ctx->manifest_crc_have =
			crc32_le(ctx->manifest_crc_have,
				 (const u8 *)&subhdr.slot_id,
				 sizeof(subhdr.slot_id));
		ctx->manifest_crc_have =
			crc32_le(ctx->manifest_crc_have,
				 (const u8 *)&subhdr.flags,
				 sizeof(subhdr.flags));
		ctx->manifest_crc_have =
			crc32_le(ctx->manifest_crc_have,
				 (const u8 *)&subhdr.instance_key,
				 sizeof(subhdr.instance_key));
		ctx->manifest_crc_have =
			crc32_le(ctx->manifest_crc_have,
				 (const u8 *)&subhdr.iova,
				 sizeof(subhdr.iova));
		ctx->manifest_crc_have =
			crc32_le(ctx->manifest_crc_have,
				 (const u8 *)&subhdr.len,
				 sizeof(subhdr.len));

		ctx->hp_seen++;
		mlx5_core_dbg(ctx->vfmig->pf_mdev,
			      "vfmig: vf %u: replayed HOST_PAGE %llu/%llu slot=%u key=0x%llx iova=0x%llx len=%llu\n",
			      ctx->vf_id,
			      (unsigned long long)ctx->hp_seen,
			      (unsigned long long)ctx->hp_expected,
			      ctx->hp_slot,
			      (unsigned long long)ctx->hp_instance_key,
			      (unsigned long long)ctx->hp_iova,
			      (unsigned long long)ctx->hp_len);

		/*
		 * If this completes the source-promised record set
		 * (HOST_PAGE + HOST_USER_PAGE), verify the running CRC
		 * matches the source's pin and arm at-probe drift
		 * detection on the destination's IOVA domain. The
		 * helper is a no-op if HUP_REPLAY hasn't also caught
		 * up to hup_expected yet -- in that case the same
		 * call from HUP_REPLAY's tail will fire the finalize.
		 */
		err = vfmig_load_maybe_finalize_records(ctx);
		if (err)
			return err;

		ctx->state = VFMIG_LS_READ_HEADER;
		*progressed = true;
		return 0;
	}

	case VFMIG_LS_HUP_READ_SUBHDR: {
		/*
		 * Pull the vfmig_host_user_page_record out of the
		 * stream into ctx->hup_subhdr_buf. Identity-only, so
		 * the sub-header IS the entire record (no DATA state
		 * follows). Validates (flags, reserved, kind != NONE,
		 * IOVA alignment) before transitioning to HUP_REPLAY.
		 */
		size_t need = sizeof(ctx->hup_subhdr_buf) -
			      ctx->hup_subhdr_filled;
		size_t take = min(need, *left);
		struct vfmig_host_user_page_record subhdr;
		u32 hup_flags;
		u32 hup_reserved;
		u8  kind;

		if (take) {
			if (copy_from_user(ctx->hup_subhdr_buf +
						ctx->hup_subhdr_filled,
					   *ubuf, take))
				return -EFAULT;
			ctx->hup_subhdr_filled += take;
			*ubuf += take;
			*left -= take;
			*progressed = true;
		}
		if (ctx->hup_subhdr_filled < sizeof(ctx->hup_subhdr_buf))
			return 0;

		memcpy(&subhdr, ctx->hup_subhdr_buf, sizeof(subhdr));
		hup_flags	     = le32_to_cpu(subhdr.flags);
		hup_reserved	     = le32_to_cpu(subhdr.reserved);
		ctx->hup_instance_key = le64_to_cpu(subhdr.instance_key);
		ctx->hup_iova	     = le64_to_cpu(subhdr.iova);
		ctx->hup_len	     = le64_to_cpu(subhdr.len);

		if (hup_flags) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: HOST_USER_PAGE flags=0x%x set; this kernel reserves all bits (must be 0)\n",
				       ctx->vf_id, hup_flags);
			return -EOPNOTSUPP;
		}
		if (hup_reserved) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: HOST_USER_PAGE reserved=0x%x (must be 0)\n",
				       ctx->vf_id, hup_reserved);
			return -EPROTO;
		}

		kind = VFMIG_HUOBJ_KIND(ctx->hup_instance_key);
		if (kind == VFMIG_HUOBJ_KIND_NONE ||
		    kind >= VFMIG_HUOBJ_KIND_NR) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: HOST_USER_PAGE instance_key=0x%llx has invalid kind byte=%u\n",
				       ctx->vf_id,
				       (unsigned long long)ctx->hup_instance_key,
				       kind);
			return -EPROTO;
		}
		if (ctx->hup_len == 0 ||
		    !IS_ALIGNED(ctx->hup_len, VFMIG_IOVA_GRANULE) ||
		    !IS_ALIGNED(ctx->hup_iova, VFMIG_IOVA_GRANULE)) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: malformed HOST_USER_PAGE record iova=0x%llx len=%llu (must be PAGE_SIZE-aligned, len > 0)\n",
				       ctx->vf_id,
				       (unsigned long long)ctx->hup_iova,
				       (unsigned long long)ctx->hup_len);
			return -EINVAL;
		}

		ctx->state = VFMIG_LS_HUP_REPLAY;
		*progressed = true;
		return 0;
	}

	case VFMIG_LS_HUP_REPLAY: {
		struct vfmig_host_user_page_record subhdr;

		err = vfmig_iova_replay_external(ctx->iova_dom,
						 VFMIG_SLOT_USER_PAGE,
						 ctx->hup_instance_key,
						 ctx->hup_iova,
						 ctx->hup_len,
						 GFP_KERNEL);
		if (err) {
			mlx5_core_warn(ctx->vfmig->pf_mdev,
				       "vfmig: vf %u: replay_external(slot=USER_PAGE key=0x%llx iova=0x%llx len=%llu) failed: %d\n",
				       ctx->vf_id,
				       (unsigned long long)ctx->hup_instance_key,
				       (unsigned long long)ctx->hup_iova,
				       (unsigned long long)ctx->hup_len, err);
			return err;
		}

		/*
		 * Fold this record's identity tuple into the running
		 * manifest CRC. Field encoding mirrors the SAVE side
		 * (see vfmig_save_hup_emit_cb): five __le fields in
		 * declaration order. @subhdr is re-encoded from
		 * ctx->hup_* so we hash exactly the wire bytes
		 * regardless of struct padding.
		 */
		subhdr.flags	    = 0;
		subhdr.reserved	    = 0;
		subhdr.instance_key = cpu_to_le64(ctx->hup_instance_key);
		subhdr.iova	    = cpu_to_le64(ctx->hup_iova);
		subhdr.len	    = cpu_to_le64(ctx->hup_len);
		ctx->manifest_crc_have =
			crc32_le(ctx->manifest_crc_have,
				 (const u8 *)&subhdr.flags,
				 sizeof(subhdr.flags));
		ctx->manifest_crc_have =
			crc32_le(ctx->manifest_crc_have,
				 (const u8 *)&subhdr.reserved,
				 sizeof(subhdr.reserved));
		ctx->manifest_crc_have =
			crc32_le(ctx->manifest_crc_have,
				 (const u8 *)&subhdr.instance_key,
				 sizeof(subhdr.instance_key));
		ctx->manifest_crc_have =
			crc32_le(ctx->manifest_crc_have,
				 (const u8 *)&subhdr.iova,
				 sizeof(subhdr.iova));
		ctx->manifest_crc_have =
			crc32_le(ctx->manifest_crc_have,
				 (const u8 *)&subhdr.len,
				 sizeof(subhdr.len));

		ctx->hup_seen++;
		mlx5_core_dbg(ctx->vfmig->pf_mdev,
			      "vfmig: vf %u: replayed HOST_USER_PAGE %llu/%llu kind=%u fw_id=0x%llx iova=0x%llx len=%llu\n",
			      ctx->vf_id,
			      (unsigned long long)ctx->hup_seen,
			      (unsigned long long)ctx->hup_expected,
			      VFMIG_HUOBJ_KIND(ctx->hup_instance_key),
			      (unsigned long long)VFMIG_HUOBJ_FWID(
				      ctx->hup_instance_key),
			      (unsigned long long)ctx->hup_iova,
			      (unsigned long long)ctx->hup_len);

		err = vfmig_load_maybe_finalize_records(ctx);
		if (err)
			return err;

		ctx->state = VFMIG_LS_READ_HEADER;
		*progressed = true;
		return 0;
	}
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
	 * Wire-prefix buffer (STREAM_HEADER + HOST_PAGE + HOST_USER_PAGE
	 * records). Built once at SAVE-ioctl time by snapshotting the
	 * source VF's vfmig_iova_domain registry into a contiguous
	 * kvmalloc'd buffer of fully-formed wire records, so the read()
	 * path in vfmig_save_drain() can stream it out alongside the
	 * FW_DATA payload without a second registry walk under
	 * DMA-coherent pressure. NULL if the source VF was untracked at
	 * SAVE time; @host_pages_size is then 0 and the wire stream
	 * contains only the FW_DATA record (no STREAM_HEADER / HOST_PAGE /
	 * HOST_USER_PAGE records).
	 *
	 * Layout when non-NULL:
	 *   [16 B vfmig_wire_header   tag=STREAM_HEADER]
	 *   [24 B vfmig_stream_header magic, version, num_pages,
	 *                             manifest_crc32, num_user_pages]
	 *   N x [16 B vfmig_wire_header tag=HOST_PAGE +
	 *        40 B vfmig_host_page_record (slot, flags, key,
	 *                                     iova, len) +
	 *        len B contents]
	 *   M x [16 B vfmig_wire_header tag=HOST_USER_PAGE +
	 *        32 B vfmig_host_user_page_record (flags, reserved,
	 *                                          key, iova, len)]
	 *
	 * Captured eagerly (rather than streamed lazily during
	 * read()) because (a) it's small -- Layer 1 produces ~4 KiB
	 * total, Layer 2 maybe ~MB, stage-2 user records are ~48 B
	 * per uobject -- and (b) it lets vfmig_save_drain remain a
	 * simple byte-cursor walk instead of a state machine.
	 */
	void *host_pages_buf;
	u64 host_pages_size;

	/*
	 * Read cursor in bytes covering the concatenation:
	 *   [0..host_pages_size)
	 *       STREAM_HEADER + HOST_PAGE + HOST_USER_PAGE records,
	 *       if any
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
 * Pass-1 callback for vfmig_iova_for_each: sum each entry's on-wire
 * footprint and count entries into @ctx so vfmig_save_build_host
 * _pages_buf can size the kvmalloc and emit a STREAM_HEADER with
 * the right num_pages.
 */
struct vfmig_save_hp_size_ctx {
	u64 total;
	u64 count;
};

static int vfmig_save_hp_count_cb(enum vfmig_iova_slot slot, u64 instance_key,
				  dma_addr_t iova, const void *vaddr,
				  size_t len, void *ctx)
{
	struct vfmig_save_hp_size_ctx *sc = ctx;

	(void)slot;
	(void)instance_key;
	(void)iova;
	(void)vaddr;
	sc->total += sizeof(struct vfmig_wire_header) +
		     sizeof(struct vfmig_host_page_record) + len;
	sc->count++;
	return 0;
}

/*
 * Pass-2 callback: serialize one HOST_PAGE record into the buffer
 * pointed to by @ctx->cursor, fold the record's identity tuple into
 * the running manifest CRC, and advance the cursor.
 */
struct vfmig_save_hp_emit_ctx {
	u8 *buf;
	u64 capacity;
	u64 cursor;
	u32 crc;
};

static int vfmig_save_hp_emit_cb(enum vfmig_iova_slot slot, u64 instance_key,
				 dma_addr_t iova, const void *vaddr,
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

	sub.slot_id      = cpu_to_le32(slot);
	sub.flags	 = 0;
	sub.instance_key = cpu_to_le64(instance_key);
	sub.iova	 = cpu_to_le64(iova);
	sub.len		 = cpu_to_le64(len);
	memcpy(ec->buf + ec->cursor, &sub, sizeof(sub));
	ec->cursor += sizeof(sub);

	/*
	 * Fold the on-wire identity tuple into the manifest CRC. The
	 * destination computes the same fold across the records it
	 * receives and compares against the value pinned by the
	 * STREAM_HEADER. Field-by-field rather than memcpy(&sub,...)
	 * because @sub still has padding bytes (the struct is naturally
	 * aligned but a future field reorder could change that, and
	 * the CRC must be exactly the destination's view).
	 */
	ec->crc = crc32_le(ec->crc, (const u8 *)&sub.slot_id,
			   sizeof(sub.slot_id));
	ec->crc = crc32_le(ec->crc, (const u8 *)&sub.flags,
			   sizeof(sub.flags));
	ec->crc = crc32_le(ec->crc, (const u8 *)&sub.instance_key,
			   sizeof(sub.instance_key));
	ec->crc = crc32_le(ec->crc, (const u8 *)&sub.iova,
			   sizeof(sub.iova));
	ec->crc = crc32_le(ec->crc, (const u8 *)&sub.len,
			   sizeof(sub.len));

	memcpy(ec->buf + ec->cursor, vaddr, len);
	ec->cursor += len;
	return 0;
}

/*
 * Pass-1 callback for vfmig_iova_for_each_external: sum each
 * external entry's on-wire footprint and count entries into @ctx
 * so vfmig_save_build_host_pages_buf can grow the kvmalloc by the
 * HOST_USER_PAGE footprint and stash the num_user_pages count for
 * the STREAM_HEADER.
 *
 * Skip entries with VFMIG_HUOBJ_KIND_NONE: those are stage-1
 * auto-numbered placeholders installed by vfmig_dma_ops.map_sg
 * before any source-side retag callsite has claimed them. They
 * have no stable identity for the destination to bind to, so
 * emitting them would just create awaiting_bind entries the
 * destination can never consume. On a kernel WITHOUT any of
 * C6..C10 of stage 2 landed, every external entry is KIND_NONE,
 * the count comes out 0, and the blob stays byte-equal to the
 * pre-stage-2 wire format.
 */
struct vfmig_save_hup_size_ctx {
	u64 total;
	u64 count;
};

static int vfmig_save_hup_count_cb(u8 kind, u64 fw_id,
				   dma_addr_t iova, size_t len,
				   bool awaiting_bind, void *ctx)
{
	struct vfmig_save_hup_size_ctx *sc = ctx;

	(void)fw_id;
	(void)iova;
	(void)len;
	(void)awaiting_bind;
	if (kind == VFMIG_HUOBJ_KIND_NONE)
		return 0;
	sc->total += sizeof(struct vfmig_wire_header) +
		     sizeof(struct vfmig_host_user_page_record);
	sc->count++;
	return 0;
}

/*
 * Pass-2 callback: serialize one HOST_USER_PAGE record into the
 * buffer pointed to by @ctx->cursor, fold the record's identity
 * tuple into the running manifest CRC (which has already absorbed
 * the HOST_PAGE identities), and advance the cursor. Identity-only
 * record -- no contents tail. Skip KIND_NONE entries for the same
 * reason the count pass does.
 */
struct vfmig_save_hup_emit_ctx {
	u8 *buf;
	u64 capacity;
	u64 cursor;
	u32 crc;
};

static int vfmig_save_hup_emit_cb(u8 kind, u64 fw_id,
				  dma_addr_t iova, size_t len,
				  bool awaiting_bind, void *ctx)
{
	struct vfmig_save_hup_emit_ctx *ec = ctx;
	struct vfmig_wire_header hdr;
	struct vfmig_host_user_page_record sub;
	u64 record_size = sizeof(sub);
	u64 need = sizeof(hdr) + record_size;

	(void)awaiting_bind;
	if (kind == VFMIG_HUOBJ_KIND_NONE)
		return 0;

	if (ec->cursor + need > ec->capacity)
		return -EOVERFLOW;

	hdr.record_size = cpu_to_le64(record_size);
	hdr.flags	= 0;
	hdr.tag		= cpu_to_le32(VFMIG_WIRE_TAG_HOST_USER_PAGE);
	memcpy(ec->buf + ec->cursor, &hdr, sizeof(hdr));
	ec->cursor += sizeof(hdr);

	sub.flags	 = 0;
	sub.reserved	 = 0;
	sub.instance_key = cpu_to_le64(VFMIG_HUOBJ_KEY(kind, fw_id));
	sub.iova	 = cpu_to_le64(iova);
	sub.len		 = cpu_to_le64(len);
	memcpy(ec->buf + ec->cursor, &sub, sizeof(sub));
	ec->cursor += sizeof(sub);

	/*
	 * Continue the running manifest CRC (initialized by the
	 * HOST_PAGE emit pass). Field-by-field for the same reason as
	 * vfmig_save_hp_emit_cb -- the destination's view of the
	 * record's identity bytes must be exactly what's CRC'd here.
	 */
	ec->crc = crc32_le(ec->crc, (const u8 *)&sub.flags,
			   sizeof(sub.flags));
	ec->crc = crc32_le(ec->crc, (const u8 *)&sub.reserved,
			   sizeof(sub.reserved));
	ec->crc = crc32_le(ec->crc, (const u8 *)&sub.instance_key,
			   sizeof(sub.instance_key));
	ec->crc = crc32_le(ec->crc, (const u8 *)&sub.iova,
			   sizeof(sub.iova));
	ec->crc = crc32_le(ec->crc, (const u8 *)&sub.len,
			   sizeof(sub.len));
	return 0;
}

/*
 * Snapshot the source VF's vfmig_iova_domain registry into a
 * contiguous wire-format buffer prefix of:
 *
 *   [16 B] vfmig_wire_header   { record_size = sizeof(hdr_payload),
 *                                flags=0, tag=STREAM_HEADER }
 *   [24 B] vfmig_stream_header { magic, version, num_pages,
 *                                manifest_crc32, num_user_pages }
 *   N x [16 B vfmig_wire_header + 40 B vfmig_host_page_record + len B
 *        contents]
 *   M x [16 B vfmig_wire_header + 32 B vfmig_host_user_page_record]
 *       (HOST_USER_PAGE records are identity-only, no contents)
 *
 * The result is owned by @ctx->host_pages_buf and sized by
 * @ctx->host_pages_size. The whole prefix is always emitted -- even
 * for a tracked source with zero registry entries -- so the
 * destination's LOAD parser can rely on STREAM_HEADER being the
 * first record. NULL @dom (untracked source) leaves both fields zero
 * and the wire stream is just a single FW_DATA record.
 *
 * Four-pass: HP-size, HUP-size, HP-emit, HUP-emit. Each pass takes
 * dom->lock independently; this is safe because the source VF is
 * SUSPEND_VHCA'd at SAVE time, so no concurrent registry mutations
 * race against the count/emit pairs. The growable-buffer alternative
 * would have to release-and-reacquire the lock under GFP_KERNEL
 * pressure anyway.
 */
static int vfmig_save_build_host_pages_buf(struct mlx5_vfmig_save_ctx *ctx,
					   struct vfmig_iova_domain *dom)
{
	struct vfmig_save_hp_size_ctx sc = {};
	struct vfmig_save_hup_size_ctx sc_hup = {};
	struct vfmig_save_hp_emit_ctx ec;
	struct vfmig_save_hup_emit_ctx ec_hup;
	struct vfmig_wire_header sh_hdr;
	struct vfmig_stream_header sh_payload;
	const u64 sh_total = sizeof(sh_hdr) + sizeof(sh_payload);
	u64 buf_total;
	int err;

	if (!dom)
		return 0;

	err = vfmig_iova_for_each(dom, vfmig_save_hp_count_cb, &sc);
	if (err)
		return err;

	err = vfmig_iova_for_each_external(dom, vfmig_save_hup_count_cb,
					   &sc_hup);
	if (err)
		return err;

	buf_total = sh_total + sc.total + sc_hup.total;
	ctx->host_pages_buf = kvmalloc(buf_total, GFP_KERNEL);
	if (!ctx->host_pages_buf)
		return -ENOMEM;

	/*
	 * Emit HOST_PAGE records first into the tail of the buffer so
	 * we have the manifest CRC running. HOST_USER_PAGE records
	 * follow immediately after and continue the same CRC fold.
	 * The STREAM_HEADER is serialized last because it has to
	 * advertise the final CRC.
	 */
	ec.buf	    = (u8 *)ctx->host_pages_buf + sh_total;
	ec.capacity = sc.total;
	ec.cursor   = 0;
	ec.crc	    = 0;
	err = vfmig_iova_for_each(dom, vfmig_save_hp_emit_cb, &ec);
	if (err)
		goto err_free;
	if (WARN_ON(ec.cursor != sc.total)) {
		err = -EIO;
		goto err_free;
	}

	ec_hup.buf	= (u8 *)ctx->host_pages_buf + sh_total + sc.total;
	ec_hup.capacity = sc_hup.total;
	ec_hup.cursor	= 0;
	ec_hup.crc	= ec.crc;
	err = vfmig_iova_for_each_external(dom, vfmig_save_hup_emit_cb,
					   &ec_hup);
	if (err)
		goto err_free;
	if (WARN_ON(ec_hup.cursor != sc_hup.total)) {
		err = -EIO;
		goto err_free;
	}

	sh_hdr.record_size = cpu_to_le64(sizeof(sh_payload));
	sh_hdr.flags	   = 0;
	sh_hdr.tag	   = cpu_to_le32(VFMIG_WIRE_TAG_STREAM_HEADER);
	memcpy(ctx->host_pages_buf, &sh_hdr, sizeof(sh_hdr));

	sh_payload.magic	  = cpu_to_le32(VFMIG_WIRE_MAGIC);
	sh_payload.version	  = cpu_to_le32(VFMIG_STREAM_VERSION);
	sh_payload.num_pages	  = cpu_to_le64(sc.count);
	sh_payload.manifest_crc32 = cpu_to_le32(ec_hup.crc);
	sh_payload.num_user_pages = cpu_to_le32((u32)sc_hup.count);
	memcpy((u8 *)ctx->host_pages_buf + sizeof(sh_hdr),
	       &sh_payload, sizeof(sh_payload));

	ctx->host_pages_size = buf_total;
	return 0;

err_free:
	kvfree(ctx->host_pages_buf);
	ctx->host_pages_buf = NULL;
	return err;
}

/*
 * Drain into @ubuf for one read(). Wire layout is the concatenation:
 *
 *   [0..host_pages_size)
 *       STREAM_HEADER + HOST_PAGE + HOST_USER_PAGE records,
 *       snapshotted at SAVE-ioctl time from the source VF's
 *       vfmig_iova_domain. Empty if the source was untracked.
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

	/* Section A: STREAM_HEADER + HOST_PAGE prefix bytes. */
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
	 * Snapshot the source VF's deterministic-IOVA registry into the
	 * STREAM_HEADER + HOST_PAGE wire prefix. Done here, after
	 * SUSPEND_VHCA and before user-visible read()s start, so the
	 * snapshot is coherent with the FW state captured by
	 * SAVE_VHCA_STATE: both reflect the VHCA at the same quiesced
	 * point.
	 *
	 * vfs_ctx[].vfmig_iova_dom is read under vfmig->lock-read (held
	 * by the ioctl dispatcher); SET_TRACKED { enable=0 } can't free
	 * it concurrently because the source VF is currently bound (it
	 * has to be, for SAVE_VHCA_STATE to make sense), and SET_TRACKED
	 * rejects toggles on bound VFs with -EBUSY. NULL domain is fine
	 * and yields a zero-byte prefix (single-FW_DATA stream).
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
	case MLX5_VFMIG_IOC_SET_VF_UUID:
		ret = vfmig_ioc_set_vf_uuid(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_PROBE_UID:
		ret = vfmig_ioc_probe_uid(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_QUERY_QP:
		ret = vfmig_ioc_query_qp(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_PROBE_PD:
		ret = vfmig_ioc_probe_pd(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_PROBE_MKEY:
		ret = vfmig_ioc_probe_mkey(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_QUERY_AWAITING_BIND:
		ret = vfmig_ioc_query_awaiting_bind(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_PROBE_CQN:
		ret = vfmig_ioc_probe_cqn(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_PROBE_QP_TEARDOWN:
		ret = vfmig_ioc_probe_qp_teardown(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_PROBE_DEALLOC_PD:
		ret = vfmig_ioc_probe_dealloc_pd(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_PROBE_CQ_DESTROY:
		ret = vfmig_ioc_probe_cq_destroy(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_PROBE_MR_DESTROY:
		ret = vfmig_ioc_probe_mr_destroy(vfmig, uarg);
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
	/*
	 * Clear orchestrator-stamped per-VF UUIDs. Cheap (memset only,
	 * no FW / IOMMU traffic), and keeps the SET_VF_UUID lifetime
	 * promise ("cleared on PF unload") intact for callers that
	 * tear the PF down via driver unbind without first dropping
	 * sriov_numvfs to 0.
	 */
	vfmig_pf_drop_vf_uuids_locked(vfmig);
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

/*
 * Public Stage-2 source-side retag entry point for the mlx5_ib MR
 * creation path. Header docstring lives in include/linux/mlx5/driver.h
 * (the public surface mlx5_ib calls through).
 *
 * Hot-path lookup contract:
 *
 *   - Read vf_dev->cmd.vfmig_iova_dom directly (set in cmd.c during
 *     the cmd-ring allocation that runs at VF probe time, iff the VF
 *     was vfmig-tracked when probe fired). This is an O(1) load with
 *     no locking. NULL on:
 *       (a) PFs (cmd-ring on PFs never uses the vfmig allocator);
 *       (b) VFs that were not vfmig-tracked at bind time; and
 *       (c) unbound mdevs (the field is cleared on cmd-ring free).
 *
 *   - The same NULL test is also performed at the callsite (see
 *     mlx5_ib's create_real_mr) so that non-vfmig deployments skip
 *     the iova_base/retag_length compute entirely. The check here is
 *     defense-in-depth + the natural way to obtain the dom pointer
 *     we operate on. Both reads avoid the alternative
 *     mlx5_vf_is_vfmig_tracked() + mlx5_vf_get_vfmig_iova_domain()
 *     dance, both of which would acquire pf_mdev->intf_state_mutex
 *     via mlx5_vf_get_core_dev() on every user-MR registration.
 *
 *   - The pointer's lifetime is the VF's bound lifetime: cmd.c clears
 *     it on cmd-ring free, and SET_TRACKED{enable=0} is itself
 *     gated to unbound VFs (see the docstring on
 *     mlx5_vf_get_vfmig_iova_domain), so once we've read non-NULL
 *     here the dom struct cannot be freed underneath us for the
 *     duration of this call.
 *
 * Error mapping:
 *   - -ENOENT (no matching registry entries in the requested range)
 *     converts to 0: the umem went through a DMA path other than
 *     vfmig_dma_ops.map_sg (dmabuf, peer driver, ODP fault path) so
 *     there's nothing to retag. Logging that case would be noise.
 *   - -EEXIST (mkey_index collides with a prior retag) and -EINVAL
 *     (misaligned arguments) propagate unchanged so the mlx5_ib
 *     caller can log a single warning with the offending mkey_index +
 *     iova range. The MR registration itself is not failed on this
 *     path -- v0 treats such an MR as "not CRIU-restorable" but
 *     still fully usable for data path.
 */
int mlx5_vfmig_retag_user_mr(struct mlx5_core_dev *vf_dev, u32 mkey_index,
			     dma_addr_t iova_base, size_t length)
{
	struct vfmig_iova_domain *dom;
	u64 instance_key;
	int err;

	if (!vf_dev)
		return 0;

	dom = vf_dev->cmd.vfmig_iova_dom;
	if (!dom)
		return 0;

	instance_key = VFMIG_HUOBJ_KEY(VFMIG_HUOBJ_KIND_MR, mkey_index);
	err = vfmig_iova_retag_external_range(dom, iova_base, length,
					      instance_key);
	if (err == -ENOENT)
		return 0;
	return err;
}
EXPORT_SYMBOL(mlx5_vfmig_retag_user_mr);

/*
 * Public Stage-3 D3 destination-side bind entry point for the mlx5_ib
 * RESTORE_MR verb body. Header docstring lives in
 * include/linux/mlx5/driver.h.
 *
 * Unlike the retag family (which gracefully no-ops on a non-vfmig
 * deployment by returning 0), bind is a hard "yes vfmig is here, the
 * placeholder exists, bind the umem to it" operation: a caller that
 * reaches this entry point has already gated on vfmig_restore_mode +
 * a tracked-VF ucontext and *needs* the bind to land. A NULL
 * vfmig_iova_dom here therefore surfaces as -ENODEV so the verb body
 * fails loudly rather than silently leaving the umem un-bound and
 * the awaiting_bind placeholder dangling (which would later trip the
 * Stage-3 D2 invariant that placeholders consumed at FW data-path
 * time must have already been bound).
 */
int mlx5_vfmig_bind_user_mr(struct mlx5_core_dev *vf_dev, u32 mkey_index,
			    struct sg_table *sgt)
{
	struct vfmig_iova_domain *dom;

	if (!vf_dev || !sgt)
		return -EINVAL;
	if (mkey_index == 0 || (mkey_index & ~0xffffffU))
		return -EINVAL;

	dom = vf_dev->cmd.vfmig_iova_dom;
	if (!dom)
		return -ENODEV;

	return vfmig_iova_bind_user_object(dom, VFMIG_HUOBJ_KIND_MR,
					   (u64)mkey_index, sgt);
}
EXPORT_SYMBOL(mlx5_vfmig_bind_user_mr);

/*
 * Public Stage-3 D3 destination-side bind entry point for the mlx5_ib
 * RESTORE_CQ verb body. Header docstring lives in
 * include/linux/mlx5/driver.h.
 *
 * Identical shape and error semantics as mlx5_vfmig_bind_user_mr
 * modulo the kind enum. The caller is mlx5_ib_umem_restore_cq's
 * ib_umem_pin -> mlx5_vfmig_bind_user_cq composition (S5b B3),
 * mirroring S4b's ib_umem_pin -> mlx5_vfmig_bind_user_mr.
 *
 * The instance_key is VFMIG_HUOBJ_KEY(KIND_CQ, cqn). The source-side
 * retag (mlx5_vfmig_retag_user_cq, fired from mlx5_ib_create_cq
 * post-FW-create) installed the matching placeholder in the
 * SAVE-side IOVA domain; LOAD_VHCA_STATE replays it onto the
 * destination ahead of this bind.
 */
int mlx5_vfmig_bind_user_cq(struct mlx5_core_dev *vf_dev, u32 cqn,
			    struct sg_table *sgt)
{
	struct vfmig_iova_domain *dom;

	if (!vf_dev || !sgt)
		return -EINVAL;
	if (cqn == 0 || (cqn & ~0xffffffU))
		return -EINVAL;

	dom = vf_dev->cmd.vfmig_iova_dom;
	if (!dom)
		return -ENODEV;

	return vfmig_iova_bind_user_object(dom, VFMIG_HUOBJ_KIND_CQ,
					   (u64)cqn, sgt);
}
EXPORT_SYMBOL(mlx5_vfmig_bind_user_cq);

/*
 * Public Stage-3 D3 destination-side bind entry point for the mlx5_ib
 * RESTORE_QP verb body. Header docstring lives in
 * include/linux/mlx5/driver.h.
 *
 * Identical shape and error semantics as mlx5_vfmig_bind_user_cq
 * modulo the kind enum. The caller is mlx5_ib_umem_restore_qp's
 * ib_umem_pin -> mlx5_vfmig_bind_user_qp composition (S6b B3),
 * mirroring S5b's ib_umem_pin -> mlx5_vfmig_bind_user_cq.
 *
 * The instance_key is VFMIG_HUOBJ_KEY(KIND_QP, qpn). The source-side
 * retag (mlx5_vfmig_retag_user_qp, fired from create_user_qp
 * post-FW-create) installed the matching placeholder in the SAVE-
 * side IOVA domain; LOAD_VHCA_STATE replays it onto the destination
 * ahead of this bind.
 */
int mlx5_vfmig_bind_user_qp(struct mlx5_core_dev *vf_dev, u32 qpn,
			    struct sg_table *sgt)
{
	struct vfmig_iova_domain *dom;

	if (!vf_dev || !sgt)
		return -EINVAL;
	if (qpn == 0 || (qpn & ~0xffffffU))
		return -EINVAL;

	dom = vf_dev->cmd.vfmig_iova_dom;
	if (!dom)
		return -ENODEV;

	return vfmig_iova_bind_user_object(dom, VFMIG_HUOBJ_KIND_QP,
					   (u64)qpn, sgt);
}
EXPORT_SYMBOL(mlx5_vfmig_bind_user_qp);

/*
 * Public Stage-3 D3 destination-side bind entry point for the mlx5_ib
 * RESTORE_CQ / RESTORE_QP / RESTORE_SRQ verb bodies' doorbell-page
 * binds. Header docstring lives in include/linux/mlx5/driver.h.
 *
 * The instance_key is VFMIG_HUOBJ_KEY(KIND_DBR, user_virt & PAGE_MASK)
 * -- DBR is the only kind whose fw_id is a userspace virtual address
 * rather than a FW-allocated identifier (no FW resource owns "the
 * doorbell page"; the FW only sees the DMA address of individual
 * 8-byte doorbell records inside it). The destination's Stage-3
 * bind path needs a stable key that spans the SAVE -> LOAD boundary;
 * mlx5_ib_db_map_user already dedups on (mm, user_virt & PAGE_MASK),
 * so we lean on that key, matching the SAVE-side
 * mlx5_vfmig_retag_user_dbr emission.
 *
 * @user_virt does not need to be page-aligned -- this helper masks
 * to PAGE_MASK before composing the instance_key, just like the
 * source-side retag does. Callers may pass the raw user VA.
 *
 * Same fast-path / error semantics as mlx5_vfmig_bind_user_mr, with
 * one extra invariant check: @sgt must describe exactly one
 * PAGE_SIZE entry, since mlx5_ib_db_map_user pins a single page per
 * doorbell umem. Multi-page sgts are rejected with -EINVAL.
 */
int mlx5_vfmig_bind_user_dbr(struct mlx5_core_dev *vf_dev,
			     unsigned long user_virt, struct sg_table *sgt)
{
	struct vfmig_iova_domain *dom;

	if (!vf_dev || !sgt)
		return -EINVAL;
	if (sgt->nents != 1 || sg_dma_len(sgt->sgl) > PAGE_SIZE)
		return -EINVAL;
	if (sgt->sgl->length != PAGE_SIZE)
		return -EINVAL;

	dom = vf_dev->cmd.vfmig_iova_dom;
	if (!dom)
		return -ENODEV;

	return vfmig_iova_bind_user_object(dom, VFMIG_HUOBJ_KIND_DBR,
					   (u64)(user_virt & PAGE_MASK), sgt);
}
EXPORT_SYMBOL(mlx5_vfmig_bind_user_dbr);

/*
 * Public Stage-2 source-side retag entry point for the mlx5_ib user
 * doorbell-page allocation path. Header docstring lives in
 * include/linux/mlx5/driver.h.
 *
 * Differs from mlx5_vfmig_retag_user_mr in two ways that justify a
 * dedicated entry point rather than a generic "retag for any kind"
 * helper:
 *
 *   - The instance_key for DBR is VFMIG_HUOBJ_KEY(KIND_DBR, user_virt
 *     & PAGE_MASK). DBR is the only kind whose fw_id is a userspace
 *     virtual address rather than a FW-allocated identifier (no FW
 *     resource owns "the doorbell page" -- the FW only ever sees the
 *     DMA address of individual 8-byte doorbell records inside it).
 *     The destination's Stage-3 bind path needs a stable key that
 *     spans the SAVE -> LOAD boundary; mlx5_ib_db_map_user already
 *     dedups on (mm, user_virt & PAGE_MASK), so we lean on that key.
 *     user_mr_dma.md ?6.3 + ?A.E.
 *
 *   - The shape is always one PAGE_SIZE entry per doorbell page (the
 *     allocator only ever maps single pages via ib_umem_get(..,
 *     PAGE_SIZE, 0)). Many uobjects in the same ucontext typically
 *     share one DBR page -- libibverbs's mlx5dv allocator hands out
 *     8-byte slots from a single page, so 1 CQ + 1 QP + 1 SRQ
 *     normally land on a single page and produce a single registry
 *     entry. The retag fires once -- in the miss branch of
 *     mlx5_ib_db_map_user, where ib_umem_get actually allocates the
 *     umem -- not on the hit-with-refcount-bump branch.
 *
 * The cmd.vfmig_iova_dom fast path is identical to the MR helper:
 * O(1) lock-free NULL load, fast no-op on PFs / non-vfmig VFs.
 *
 * Error mapping is also identical: -ENOENT collapses to 0 (we expect
 * the registry to have a matching range every time vfmig_dma_ops is
 * the active DMA path, which is precisely the condition cmd.vfmig_iova_dom
 * indicates -- but be defensive in case a future doorbell allocator
 * variant routes around the shim). -EEXIST and -EINVAL propagate so
 * the caller can warn.
 */
int mlx5_vfmig_retag_user_dbr(struct mlx5_core_dev *vf_dev,
			      unsigned long user_virt,
			      dma_addr_t iova_base, size_t length)
{
	struct vfmig_iova_domain *dom;
	u64 instance_key;
	int err;

	if (!vf_dev)
		return 0;

	dom = vf_dev->cmd.vfmig_iova_dom;
	if (!dom)
		return 0;

	instance_key = VFMIG_HUOBJ_KEY(VFMIG_HUOBJ_KIND_DBR,
				       user_virt & PAGE_MASK);
	err = vfmig_iova_retag_external_range(dom, iova_base, length,
					      instance_key);
	if (err == -ENOENT)
		return 0;
	return err;
}
EXPORT_SYMBOL(mlx5_vfmig_retag_user_dbr);

/*
 * Public Stage-2 source-side retag entry point for the mlx5_ib user
 * CQ creation path. Header docstring lives in
 * include/linux/mlx5/driver.h.
 *
 * Identical shape to mlx5_vfmig_retag_user_mr modulo the kind enum:
 * a single user umem covers the CQE ring buffer, FW assigns @cqn at
 * mlx5_core_create_cq time, and we promote the registry entries
 * vfmig_dma_ops.map_sg planted at ib_umem_get to
 * VFMIG_HUOBJ_KEY(KIND_CQ, cqn). The CQ's doorbell page is retagged
 * separately by mlx5_vfmig_retag_user_dbr when mlx5_ib_db_map_user
 * fires from create_cq_user (same ucontext db_page_list shared with
 * QPs/SRQs, may dedup).
 *
 * Same cmd.vfmig_iova_dom O(1) fast path and -ENOENT-to-0 error
 * mapping as the MR helper.
 */
int mlx5_vfmig_retag_user_cq(struct mlx5_core_dev *vf_dev, u32 cqn,
			     dma_addr_t iova_base, size_t length)
{
	struct vfmig_iova_domain *dom;
	u64 instance_key;
	int err;

	if (!vf_dev)
		return 0;

	dom = vf_dev->cmd.vfmig_iova_dom;
	if (!dom)
		return 0;

	instance_key = VFMIG_HUOBJ_KEY(VFMIG_HUOBJ_KIND_CQ, cqn);
	err = vfmig_iova_retag_external_range(dom, iova_base, length,
					      instance_key);
	if (err == -ENOENT)
		return 0;
	return err;
}
EXPORT_SYMBOL(mlx5_vfmig_retag_user_cq);

/*
 * Public Stage-2 source-side retag entry point for the mlx5_ib user
 * QP creation path. Header docstring lives in
 * include/linux/mlx5/driver.h.
 *
 * v0 scope: the regular QPC-managed QP types (RC, UC, UD) that share
 * one umem covering both SQ and RQ work-queue buffers. RAW_PACKET /
 * SOURCE_QPN QPs use create_raw_packet_qp() with split SQ/RQ umems
 * and are not retagged by this entry point -- the caller is
 * expected to skip those at the callsite.
 *
 * Same cmd.vfmig_iova_dom fast path and -ENOENT-to-0 error mapping
 * as the MR/CQ helpers.
 */
int mlx5_vfmig_retag_user_qp(struct mlx5_core_dev *vf_dev, u32 qpn,
			     dma_addr_t iova_base, size_t length)
{
	struct vfmig_iova_domain *dom;
	u64 instance_key;
	int err;

	if (!vf_dev)
		return 0;

	dom = vf_dev->cmd.vfmig_iova_dom;
	if (!dom)
		return 0;

	instance_key = VFMIG_HUOBJ_KEY(VFMIG_HUOBJ_KIND_QP, qpn);
	err = vfmig_iova_retag_external_range(dom, iova_base, length,
					      instance_key);
	if (err == -ENOENT)
		return 0;
	return err;
}
EXPORT_SYMBOL(mlx5_vfmig_retag_user_qp);

/*
 * Public Stage-2 source-side retag entry point for the mlx5_ib user
 * SRQ creation path. Header docstring lives in
 * include/linux/mlx5/driver.h.
 *
 * SRQ user creates (BASIC, XRC, TM variants) all go through
 * create_srq_user() -> mlx5_cmd_create_srq, with @srqn set on the
 * mlx5_core_srq's msrq.srqn field by the time the create returns.
 * The user-mode umem (srq->umem) was DMA-mapped earlier in
 * create_srq_user, so its KIND_NONE entries are already in the
 * registry awaiting promotion to VFMIG_HUOBJ_KEY(KIND_SRQ, srqn).
 *
 * Same cmd.vfmig_iova_dom fast path and -ENOENT-to-0 error mapping
 * as the MR / CQ / QP helpers.
 */
int mlx5_vfmig_retag_user_srq(struct mlx5_core_dev *vf_dev, u32 srqn,
			      dma_addr_t iova_base, size_t length)
{
	struct vfmig_iova_domain *dom;
	u64 instance_key;
	int err;

	if (!vf_dev)
		return 0;

	dom = vf_dev->cmd.vfmig_iova_dom;
	if (!dom)
		return 0;

	instance_key = VFMIG_HUOBJ_KEY(VFMIG_HUOBJ_KIND_SRQ, srqn);
	err = vfmig_iova_retag_external_range(dom, iova_base, length,
					      instance_key);
	if (err == -ENOENT)
		return 0;
	return err;
}
EXPORT_SYMBOL(mlx5_vfmig_retag_user_srq);

/*
 * Detach the per-VF vfmig_iova_domain from this VF's PCI device.
 * Called from mlx5_core remove_one() for VFs so the iommu attachment
 * is gone before pci_disable_sriov() fires device_del. See the comment
 * on vfmig_iova_domain_detach_dev() for the WARN this avoids.
 *
 * No-op on PFs, on untracked VFs, on a VF whose PF has gone away, and
 * on a domain that's already been detached. The domain struct itself
 * remains in vfs_ctx[vf_id].vfmig_iova_dom and is freed later by
 * vfmig_pf_drop_iova_domains_locked() at the end of mlx5_sriov_disable.
 */
void mlx5_vfmig_vf_detach_iova_domain(struct mlx5_core_dev *vf_mdev)
{
	struct pci_dev *vf_pdev;
	struct mlx5_core_dev *pf_mdev;
	struct mlx5_core_sriov *sriov;
	struct vfmig_iova_domain *dom;
	int vf_id;

	if (!vf_mdev)
		return;
	vf_pdev = vf_mdev->pdev;
	if (!vf_pdev || !vf_pdev->is_virtfn)
		return;

	vf_id = pci_iov_vf_id(vf_pdev);
	if (vf_id < 0)
		return;

	pf_mdev = mlx5_vf_get_core_dev(vf_pdev);
	if (!pf_mdev)
		return;

	sriov = &pf_mdev->priv.sriov;
	dom = NULL;
	if (sriov->vfs_ctx && vf_id < sriov->num_vfs)
		dom = sriov->vfs_ctx[vf_id].vfmig_iova_dom;

	/*
	 * The detach itself can sleep (iommu_detach_device may take an
	 * iommu group mutex), so we don't hold any vfmig locks while
	 * doing it. The dom pointer is stable for the duration of this
	 * call because vfmig_pf_drop_iova_domains_locked() only runs
	 * after pci_disable_sriov() returns, which is well after this
	 * hook in remove_one() has completed.
	 */
	mlx5_vf_put_core_dev(pf_mdev);

	if (dom)
		vfmig_iova_domain_detach_dev(dom);
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

struct vfmig_import_fwp_ctx {
	struct mlx5_core_dev *vf_dev;
	u32 imported;
	int err;
};

static int vfmig_import_fwp_cb(enum vfmig_iova_slot slot, u64 instance_key,
			       dma_addr_t iova, const void *vaddr, size_t len,
			       void *ctx)
{
	struct vfmig_import_fwp_ctx *ic = ctx;
	int err;

	/*
	 * Only FW_PAGE entries went through alloc_system_page() ->
	 * insert_page() on the source. The other slots (CMD_RING,
	 * EQ_BUF, FRAG_BUF, DB_PAGE, DMA_COHERENT) live in their own
	 * lifetime trackers (cmd ring buffer, struct mlx5_frag_buf,
	 * mlx5_db_pgdir, etc.) and never appear in priv->page_root_xa.
	 * Skip them here -- their reconstruction is the matching
	 * consumer's job.
	 */
	if (slot != VFMIG_SLOT_FW_PAGE)
		return 0;

	/*
	 * alloc_system_page() always allocates exactly PAGE_SIZE per
	 * call; the wire mirrors that 1:1. Defensive check rather than
	 * silently importing a malformed-len entry.
	 */
	if (WARN_ON_ONCE(len != PAGE_SIZE))
		return 0;

	/*
	 * function=0 is the VF reclaiming-its-own-pages encoding
	 * (func_id=0, ec_function=0). Matches what give_pages() passes
	 * on the source for VF-self give-pages events, which is the
	 * only flavour alloc_system_page() ever drives on a VF mdev.
	 */
	err = mlx5_pages_import_replayed_fw_page(ic->vf_dev, /*function=*/0,
						 (u64)iova);
	if (err) {
		ic->err = err;
		return err;
	}
	ic->imported++;
	return 0;
}

/*
 * Reconstitute mlx5_core's per-VF page rb-tree (priv->page_root_xa)
 * for a restored VF, mirroring the source's give_pages() output.
 *
 * On the source, every FW_PAGE the IOVA allocator handed out was
 * also recorded in priv->page_root_xa[function] via insert_page() in
 * alloc_system_page(); priv->fw_pages and priv->page_counters[VF]
 * tracked the running total. Both data structures together back
 * mlx5_reclaim_root_pages() at VF teardown: it walks page_root, calls
 * free_fwp() on each entry, and free_fwp()'s vfmig branch routes the
 * page back through vfmig_iova_free_slot().
 *
 * On the destination, vfmig_iova_replay_page() during LOAD installed
 * the IOVA mapping and the page contents, but it did NOT touch
 * page_root_xa -- the VF mdev didn't even exist yet (the LOAD ioctl
 * runs on the PF cdev pre-bind). Without this reconstruction step
 * the restored VF probes with an empty page_root for its own
 * function and:
 *
 *   - mlx5_reclaim_root_pages() at teardown finds nothing, returns 0
 *     pages reclaimed; FW thinks it still owns the pages and the
 *     IOVA allocator never frees them -> per-VF leak that grows
 *     unbounded with bind/unbind cycles.
 *   - any FW-initiated MANAGE_PAGES { take_pages } walks the (empty)
 *     rb-tree, returns -EEXIST/0-pages, and FW state diverges from
 *     mlx5_core's view.
 *   - priv->fw_pages and the per-type page_counters[] under-report
 *     by exactly the number of pages LOAD restored, tripping
 *     debug-kernel sanity checks in mlx5_destroy_mkey() and the
 *     pages_debugfs reader.
 *
 * Walks the per-VF deterministic IOVA domain in IOVA-ascending order
 * (matching the source's give-pages order, which is what
 * vfmig_iova_for_each guarantees) and calls
 * mlx5_pages_import_replayed_fw_page() for each FW_PAGE entry. Other
 * slots are skipped -- they live in their own consumer-side
 * lifetime trackers (cmd ring buffer, mlx5_frag_buf, mlx5_db_pgdir)
 * which the corresponding consumer reconstructs on its own probe.
 *
 * MUST run between the destination VF's mlx5_cmd_enable() (which
 * initialises priv->page_root_xa) and any FW give-pages event on the
 * restored VHCA. The current caller is mlx5_function_open()'s
 * restored branch, right after mlx5_vfmig_vf_apply_pending_load()
 * returns success and before the post-LOAD ENABLE_HCA(self) attempt.
 *
 * Returns 0 on success (including the no-domain / no-FW_PAGE-entries
 * case) or a negative errno from the first failed
 * mlx5_pages_import_replayed_fw_page(). On error, partial inserts
 * are NOT rolled back: the pages live in priv->page_root_xa and will
 * be reclaimed by mlx5_reclaim_root_pages() at VF teardown via the
 * same vfmig branch as a successful import. The probe should still
 * fail loudly via the err return so the operator sees the divergence.
 */
int mlx5_vfmig_vf_import_replayed_fw_pages(struct mlx5_core_dev *vf_dev)
{
	struct vfmig_iova_domain *dom;
	struct vfmig_import_fwp_ctx ic = { .vf_dev = vf_dev };
	struct pci_dev *vf_pdev;
	int err;

	if (!vf_dev)
		return 0;

	vf_pdev = vf_dev->pdev;
	if (!vf_pdev || !vf_pdev->is_virtfn)
		return 0;

	/*
	 * Read the IOVA domain off vf_dev->cmd, NOT via
	 * mlx5_vf_get_vfmig_iova_domain(): the latter takes the
	 * PF reference and goes through the cdev lookup path, which
	 * is heavier than what we need here and (more importantly)
	 * acquires intf_state_mutex on the PF -- a lock our caller
	 * (mlx5_function_open) doesn't hold but would inherit a
	 * deadlock risk against if the lookup ever started running on
	 * a path that does. The cmd-side pointer was set by
	 * mlx5_cmd_enable() upstream of us and is guaranteed live for
	 * the duration of this VF probe (see vfmig.h docstring on
	 * mlx5_vf_get_vfmig_iova_domain for the lifetime contract).
	 */
	dom = vf_dev->cmd.vfmig_iova_dom;
	if (!dom)
		return 0;

	err = vfmig_iova_for_each(dom, vfmig_import_fwp_cb, &ic);
	if (err) {
		mlx5_core_warn(vf_dev,
			       "vfmig: import_replayed_fw_pages: failed after %u entries: %d\n",
			       ic.imported, err);
		return err;
	}

	mlx5_core_info(vf_dev,
		       "vfmig: imported %u replayed FW_PAGE entries into priv->page_root\n",
		       ic.imported);
	return 0;
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
 * Detach the iommu_domain side of every vfs_ctx[].vfmig_iova_dom that
 * belongs to a *driverless* VF, without freeing the domain struct.
 *
 * Why this exists: the VF's per-VF unmanaged paging domain is normally
 * detached from its pci_dev by mlx5_core's remove_one() at the tail of
 * device_release_driver(), well before pci_disable_sriov() reaches the
 * device_del() that fires the iommu bus notifier. That hook only runs
 * for *driver-bound* VFs, though. A VF that was made tracked +
 * migratable but never bound to mlx5_core (e.g. the destination VF in
 * a checkpoint/restore measurement, where the user-mode RESTORE_X
 * verbs are intentionally invoked before driver bind) would otherwise
 * keep its iommu_dom attached all the way into pci_disable_sriov(),
 * and the iommu core's BUS_NOTIFY_REMOVED_DEVICE notifier would WARN
 * at drivers/iommu/iommu.c:715 (group->domain != group->default_domain
 * with no driver to "own" the override).
 *
 * Calling iommu_detach_device() on a driver-bound VF here would race
 * its still-active FW DMA (mlx5_pci_close() hasn't drained the cmd
 * ring + EQs yet at this point in the teardown), so we explicitly
 * skip those: vf_pdev->driver != NULL means the VF is bound and the
 * existing remove_one() hook will do the right thing once
 * pci_disable_sriov() walks it.
 *
 * vfmig_iova_domain_detach_dev() sets @dev_detached, so the later
 * vfmig_pf_drop_iova_domains_locked() -> vfmig_iova_domain_destroy()
 * skips the redundant iommu_detach and proceeds straight to
 * iommu_domain_free() + kfree.
 *
 * Caller-side locking matches drop_iova_domains_locked: vfmig->lock is
 * held by the caller; we take ctxs_lock to read each
 * vfs_ctx[].vfmig_iova_dom, then drop ctxs_lock to do the actual
 * detach (which can sleep / take iommu group locks). We do NOT splice
 * the dom out -- the existing drop helper still owns the free path.
 */
static void
vfmig_pf_detach_unbound_iova_domains_locked(struct mlx5_vfmig_pf *vfmig)
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

	total_vfs = sriov->num_vfs;
	mutex_lock(&vfmig->ctxs_lock);
	for (i = 0; i < total_vfs; i++) {
		struct vfmig_iova_domain *dom =
			sriov->vfs_ctx[i].vfmig_iova_dom;

		if (!dom)
			continue;
		mutex_unlock(&vfmig->ctxs_lock);
		vfmig_iova_domain_detach_dev_if_unbound(dom);
		mutex_lock(&vfmig->ctxs_lock);
	}
	mutex_unlock(&vfmig->ctxs_lock);
}

void mlx5_vfmig_pf_detach_unbound_iova_domains(struct mlx5_core_dev *pf_mdev)
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
		vfmig_pf_detach_unbound_iova_domains_locked(vfmig);
	up_read(&vfmig->lock);
	vfmig_pf_put(vfmig);
}

/*
 * Drop any per-VF deterministic IOVA domains. Mirror image of
 * vfmig_pf_drop_pending_loads_locked() but for vfs_ctx[].vfmig_iova_dom.
 *
 * Ordering contract (CRITICAL ? get this wrong and you get a UAF in
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

/*
 * Zero every vfs_ctx[].vf_uuid on PF teardown / sriov_numvfs=0.
 *
 * Unlike vfmig_pf_drop_pending_loads_locked / drop_iova_domains_locked
 * (which only touch slots that *had* the relevant resource), this hook
 * iterates every slot in @sriov->num_vfs unconditionally: the cost is
 * one uuid_copy(&uuid_null) per slot and we want the simple invariant
 * "after this runs, QUERY_VF returns uuid_null for every slot in this
 * generation".
 *
 * What this is NOT: this hook is not an assertion that mlx5 FW
 * accepts a second LOAD_VHCA_STATE on the same VHCA across a
 * bind/unbind cycle without an sriov_numvfs cycle in between.
 * That path is structurally blocked one layer down, in
 * vfmig_iova_replay_page(): the first LOAD's parser arms
 * dom->drift_armed=1 after every HOST_PAGE record has been
 * parsed, and any later replay attempt (i.e. the HOST_PAGE
 * prefix of a second LOAD on the same domain) trips
 * WARN_ON_ONCE(dom->drift_armed) and returns -EBUSY, surfaced
 * as -EINVAL on the second write(). LOAD_VHCA_STATE is never
 * issued, so the firmware question is moot for in-tree
 * consumers (the vfio mlx5 LM variant driver also assumes one
 * LOAD per VM lifecycle). Empirically validated end-to-end on
 * 2026-06-09 by tools/testing/mlx5_vfmig/save_load/
 * multi_load_gates/test_multi_load_drift_gate.sh. See
 * vf_prerestore_split.md §3.5.5.1 for the gate-by-gate table.
 *
 * What this IS: the orchestrator's natural slot-repurposing path
 * already has to go through sriov_numvfs=0 + sriov_numvfs=N (any
 * other shape requires firmware behaviour we haven't validated).
 * Across that cycle, the orchestrator may want to assign a
 * *different* identity tag to vf_id slot N -- because the new
 * occupant is a different workload, not a re-instantiation of the
 * old one. Without this hook the prior tag would survive the
 * cycle and SET_VF_UUID with the new workload's UUID would get
 * -EBUSY with no in-kernel clear path. The clear-on-teardown
 * semantics published in the SET_VF_UUID UAPI doc-comment make
 * the cycle the natural reset point.
 *
 * ctxs_lock serialises against concurrent SET_VF_UUID, matching the
 * write-side handler. Within the loop we don't drop ctxs_lock --
 * uuid_copy() is non-blocking and the iteration count is bounded by
 * num_vfs (the FW caps total_vfs at low hundreds), so a tail-end
 * SET_VF_UUID is briefly delayed but never forever.
 */
static void vfmig_pf_drop_vf_uuids_locked(struct mlx5_vfmig_pf *vfmig)
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

	total_vfs = sriov->num_vfs;
	mutex_lock(&vfmig->ctxs_lock);
	for (i = 0; i < total_vfs; i++)
		uuid_copy(&sriov->vfs_ctx[i].vf_uuid, &uuid_null);
	mutex_unlock(&vfmig->ctxs_lock);
}

void mlx5_vfmig_pf_drop_vf_uuids(struct mlx5_core_dev *pf_mdev)
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
		vfmig_pf_drop_vf_uuids_locked(vfmig);
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
