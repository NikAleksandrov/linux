/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * UAPI for mlx5 host-driven VF migration / CRIU restore.
 *
 * One char device per mlx5_core PF, exposed as /dev/mlx5_vfmig/<bdf>,
 * exposes SAVE/LOAD/SUSPEND/RESUME of an mlx5 VF's firmware state from
 * the host PF, modelled on the VFIO mlx5 variant driver but driven by
 * the host's PF mlx5_core rather than by VFIO.
 *
 * Source-side lifecycle (capture a VHCA snapshot):
 *   1. sriov_drivers_autoprobe = 0 on the PF
 *   2. sriov_numvfs = N on the PF                    (VFs created, unbound)
 *   3. ioctl(MLX5_VFMIG_IOC_ENABLE_MIGRATABLE, vf_id)
 *   4. driver_override + bind the VF to mlx5_core    (workload runs)
 *   5. open /dev/mlx5_vfmig/<pf_bdf>
 *   6. ioctl(MLX5_VFMIG_IOC_SAVE_VHCA_STATE, vf_id)
 *      -> returns a read-only anon-inode fd. read() the blob to EOF,
 *         close(). By default the source VHCA is RESUMEd on close;
 *         pass MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED to leave it stopped
 *         (e.g. CRIU dump-then-destroy).
 *
 * Destination-side lifecycle (apply a VHCA snapshot):
 *   1. sriov_drivers_autoprobe = 0 on the PF
 *   2. sriov_numvfs = N on the PF                    (VFs created, unbound)
 *   3. ioctl(MLX5_VFMIG_IOC_ENABLE_MIGRATABLE, vf_id)
 *   4. open /dev/mlx5_vfmig/<pf_bdf>
 *   5. ioctl(MLX5_VFMIG_IOC_LOAD_VHCA_STATE, vf_id)
 *      -> returns a write-only anon-inode fd. write() the blob,
 *         close(). The blob's DMA-mapped pages and MKEY are staged
 *         on the PF's per-VF pending_load slot; no firmware command
 *         has been issued yet against the destination VHCA.
 *   6. ioctl(MLX5_VFMIG_IOC_MARK_RESTORED, vf_id)
 *   7. driver_override + bind the VF to mlx5_core
 *      -> mlx5_core's probe runs the deferred SUSPEND + LOAD_VHCA_STATE
 *         + RESUME pair via the PF mdev, then skips SET_ISSI / boot
 *         pages / INIT_HCA on the destination VHCA.
 *
 * Note on round-trip behaviour:
 *   The SAVE blob references DMA addresses (cmd ring, EQ buffers, MR
 *   backing pages, ...) captured from the source's mlx5_core. After
 *   LOAD on a destination that runs native mlx5_core (i.e. not behind
 *   vfio_mlx5_pci in a guest VM), those addresses do not point at the
 *   destination's buffers. The plumbing here exercises SAVE + LOAD
 *   end-to-end and the destination probe completes through LOAD, but
 *   subsequent VHCA-side firmware commands time out until the
 *   destination can reproduce the source's address layout (which on
 *   bare metal requires an IOMMU and a deterministic-IOVA allocator).
 *   That extension is not covered by this UAPI.
 */

#ifndef _UAPI_LINUX_MLX5_VFMIG_H
#define _UAPI_LINUX_MLX5_VFMIG_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define MLX5_VFMIG_IOC_MAGIC	0xB5

/*
 * MLX5_VFMIG_IOC_MARK_RESTORED:
 *   Mark VF @vf_id as having had its firmware state restored. The next
 *   mlx5_core probe of that VF will skip INIT_HCA.
 *   Returns 0 on success, -EINVAL if vf_id is out of range or @flags has
 *   unknown bits, -EALREADY if the flag was already set.
 *
 *   @flags:
 *     MLX5_VFMIG_MARK_RESTORED_DEFER_RESUME -- the snapshot-ordering
 *       restore mirror (design/snapshot_ordering_pause_capture.md Part
 *       A.4). When set, the next probe runs SUSPEND_VHCA +
 *       LOAD_VHCA_STATE but SKIPS the trailing RESUME pair, leaving the
 *       restored VHCA parked. Userspace (CRIU) must later issue
 *       MLX5_VFMIG_IOC_RESUME_VHCA -- at RESUME_DEVICES_LATE, after all
 *       MR/ring VMAs have been restored -- to bring the datapath live.
 *       Without this flag the probe resumes inline as before (legacy
 *       non-CRIU restore).
 */
#define MLX5_VFMIG_MARK_RESTORED_DEFER_RESUME	(1u << 0)
#define MLX5_VFMIG_MARK_RESTORED_FLAG_ALL \
	(MLX5_VFMIG_MARK_RESTORED_DEFER_RESUME)

struct mlx5_vfmig_mark_restored {
	__u32 vf_id;
	__u32 flags;	/* in: subset of MLX5_VFMIG_MARK_RESTORED_FLAG_* */
};
#define MLX5_VFMIG_IOC_MARK_RESTORED \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x01, struct mlx5_vfmig_mark_restored)

/*
 * MLX5_VFMIG_IOC_GET_VHCA_ID:
 *   PF-side query of the VF's vhca_id via QUERY_HCA_CAP(other_function=1).
 *   Debug helper that lets userspace confirm the PF can address the VF
 *   without binding any driver to it.
 */
struct mlx5_vfmig_get_vhca_id {
	__u32 vf_id;	/* in  */
	__u16 vhca_id;	/* out */
	__u16 reserved;
};
#define MLX5_VFMIG_IOC_GET_VHCA_ID \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x02, struct mlx5_vfmig_get_vhca_id)

/*
 * MLX5_VFMIG_IOC_QUERY_VF:
 *   Diagnostic snapshot of one VF on the owning PF. Returns the VF's
 *   live vhca_id (queried via QUERY_HCA_CAP(other_function=1)), the
 *   "restored" / "tracked" bits, the orchestrator-stamped vf_uuid (if
 *   any), and the total number of VFs the PF has provisioned.
 *   Userspace iterates 0..num_vfs-1 to enumerate; that's intentionally
 *   cheaper to maintain than a variable-length list ioctl.
 *
 *   Output fields:
 *     vhca_id:   live VHCA identifier from
 *                QUERY_HCA_CAP(other_function=1).
 *     restored:  1 if MLX5_VFMIG_IOC_MARK_RESTORED was issued for
 *                this VF (i.e. its next probe should skip the
 *                ENABLE_HCA / SET_ISSI / boot-pages / INIT_HCA
 *                sequence and apply the staged LOAD blob instead).
 *     tracked:   1 if MLX5_VFMIG_IOC_SET_TRACKED { enable=1 } is
 *                currently in effect for this VF -- i.e. its
 *                per-VF unmanaged IOMMU domain is allocated and
 *                attached, and probe-time DMA buffers will route
 *                through the deterministic IOVA allocator instead
 *                of dma_alloc_coherent. CRIU's mlx5_sriov_vfmig
 *                plugin uses this at startup to discover which
 *                PFs/VFs are eligible for save/restore without
 *                binding any driver. Returned as 0 on out-of-range
 *                vf_id (alongside -ERANGE), so it's safe to read
 *                in the error-path.
 *     vf_uuid:   16-byte orchestrator-stamped per-VF identity tag
 *                set via MLX5_VFMIG_IOC_SET_VF_UUID. All-zeros
 *                means the orchestrator has not (yet) stamped a
 *                UUID on this slot. Cleared on SR-IOV teardown
 *                (sriov_numvfs=0). See KS7.3 in
 *                tools/testing/mlx5_vfmig/design/vf_prerestore_split.md
 *                §3.5 for the dump-side / restore-side contract.
 *                Returned as all-zeros on out-of-range vf_id.
 *
 *   ABI note: this struct grew to add @vf_uuid + @reserved_out after
 *   the initial release. The encoded ioctl number changes with the
 *   struct size (sizeof in the _IOWR macro), so old userspace built
 *   against the smaller struct will get -ENOTTY from a new kernel
 *   rather than reading a partial / misaligned result. Recompile the
 *   in-tree tool (tools/testing/mlx5_vfmig) against this header.
 */
struct mlx5_vfmig_query_vf {
	__u32 vf_id;		/* in  */
	__u32 num_vfs;		/* out: total VFs provisioned on this PF */
	__u16 vhca_id;		/* out */
	__u8  restored;		/* out: 1 if MARK_RESTORED was issued */
	__u8  tracked;		/* out: 1 if SET_TRACKED { enable=1 }
				 *      currently in effect on this VF
				 */
	__u8  vf_uuid[16];	/* out: orchestrator-stamped UUID,
				 *      all-zeros if unset
				 */
	__u8  reserved_out[8];	/* out: zeroed */
};
#define MLX5_VFMIG_IOC_QUERY_VF \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x03, struct mlx5_vfmig_query_vf)

/*
 * MLX5_VFMIG_IOC_LOAD_VHCA_STATE:
 *   Open a write-only data session that consumes a previously-saved VF
 *   state blob and installs it into the firmware via LOAD_VHCA_STATE.
 *
 *   The returned @load_fd is an anon-inode fd. Userspace write()s the
 *   blob to it (chunked or whole; partial writes are fine) and close()s
 *   it. The driver runs LOAD_VHCA_STATE per parsed image record.
 *
 *   Wire format:
 *     The blob is byte-compatible with the migration data stream
 *     produced by the VFIO mlx5 variant driver in
 *     drivers/vfio/pci/mlx5/. It is a sequence of records, each
 *     prefixed by a 16-byte header (record_size:le64, flags:le32,
 *     tag:le32). Records carrying firmware state use tag 0
 *     (MLX5_MIGF_HEADER_TAG_FW_DATA, kernel-internal name) and are
 *     mandatory; unknown tags marked optional in flags are skipped,
 *     unknown mandatory tags fail the write with -EOPNOTSUPP.
 *     Userspace should treat the entire byte stream as opaque.
 *
 *   Returns 0 with @load_fd populated on success, -EINVAL if vf_id is
 *   out of range or @flags is non-zero, -ENODEV if the PF is gone.
 *   Closing the fd without writing anything is a no-op (no firmware
 *   commands issued).
 */
struct mlx5_vfmig_load_state {
	__u32 vf_id;	/* in  */
	__u32 flags;	/* in: must be 0 for now */
	__s32 load_fd;	/* out */
	__u32 reserved;
};
#define MLX5_VFMIG_IOC_LOAD_VHCA_STATE \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x04, struct mlx5_vfmig_load_state)

/*
 * MLX5_VFMIG_IOC_SAVE_VHCA_STATE:
 *   Open a read-only data session that captures a VF's current firmware
 *   state into a blob byte-compatible with LOAD_VHCA_STATE's input.
 *
 *   On the ioctl call, the driver synchronously:
 *     - queries the VF's vhca_id via QUERY_HCA_CAP(other_function=1)
 *     - SUSPEND_VHCA(INITIATOR), SUSPEND_VHCA(RESPONDER) to quiesce
 *     - QUERY_VHCA_MIGRATION_STATE to size the snapshot
 *     - allocates a PD + image pages + MKEY (DMA_FROM_DEVICE)
 *     - SAVE_VHCA_STATE to populate the pages
 *   then returns @save_fd, an anon-inode fd. Userspace read()s the blob
 *   from it (any chunk size) until EOF. The first read also emits a
 *   16-byte FW_DATA record header (record_size, flags=0, tag=0) so the
 *   resulting byte stream can be fed verbatim back into
 *   MLX5_VFMIG_IOC_LOAD_VHCA_STATE.
 *
 *   Resume policy on close():
 *     By default the driver issues RESUME_VHCA(RESPONDER) and
 *     RESUME_VHCA(INITIATOR) when the fd is released, leaving the source
 *     VF runnable again. Set MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED to
 *     skip the resume (e.g. CRIU dump-then-destroy where the VF is
 *     about to be torn down via sriov_numvfs=0 anyway).
 *
 *   Returns 0 with @save_fd populated on success, -EINVAL if vf_id is
 *   out of range or @flags has unknown bits, -EBUSY if a save session
 *   already exists for this vf_id, -ENODEV if the PF is gone, or any
 *   firmware error code (negated) if a SUSPEND/QUERY/SAVE step fails.
 *   On firmware failure no fd is returned and the VHCA is left as
 *   undisturbed as possible (failed SUSPENDs are not "undone").
 */
#define MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED	(1u << 0)
#define MLX5_VFMIG_SAVE_FLAG_ALL \
	(MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED)

struct mlx5_vfmig_save_state {
	__u32 vf_id;	/* in  */
	__u32 flags;	/* in: subset of MLX5_VFMIG_SAVE_FLAG_* */
	__s32 save_fd;	/* out */
	__u32 reserved;
};
#define MLX5_VFMIG_IOC_SAVE_VHCA_STATE \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x05, struct mlx5_vfmig_save_state)

/*
 * MLX5_VFMIG_IOC_ENABLE_MIGRATABLE:
 *   Set the per-VF cmd_hca_cap_2.migratable bit, which is the firmware
 *   gate for SUSPEND/SAVE/LOAD/RESUME. This MUST be called while the
 *   VF is unbound (no driver attached): the firmware accepts a
 *   modify-cap on a VHCA in pre-ENABLE_HCA state but rejects it
 *   ("bad resource state") on a VHCA that mlx5_core has already
 *   probed.
 *
 *   Standard ordering for SAVE on a freshly-provisioned VF:
 *     1. sriov_drivers_autoprobe = 0
 *     2. sriov_numvfs = N            (VFs created, unbound)
 *     3. ioctl(MLX5_VFMIG_IOC_ENABLE_MIGRATABLE, vf_id) for each VF
 *     4. driver_override + bind on the VF -> ENABLE_HCA latches
 *        migratable=1
 *     5. run workload, eventually issue MLX5_VFMIG_IOC_SAVE_VHCA_STATE
 *
 *   Idempotent: returns 0 (with no firmware traffic) if the bit is
 *   already set. Returns -EOPNOTSUPP if the PF firmware does not
 *   advertise migration / vhca_resource_manager. The bit is intentionally
 *   left set across mlx5_core probes -- a VF that's been migration-
 *   enabled once stays migration-enabled until sriov_numvfs is dropped.
 */
struct mlx5_vfmig_enable_migratable {
	__u32 vf_id;
	__u32 reserved;
};
#define MLX5_VFMIG_IOC_ENABLE_MIGRATABLE \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x06, struct mlx5_vfmig_enable_migratable)

/*
 * MLX5_VFMIG_IOC_SET_TRACKED:
 *   Toggle the host-driver-side "vfmig tracked" mode on a VF. When set,
 *   the PF allocates a per-VF unmanaged IOMMU domain and attaches it
 *   to the VF's pci_dev, and subsequent host-side allocators in the
 *   destination VF's mlx5_core probe (cmd ring, MANAGE_PAGES pages,
 *   EQ buffers, ...) route through a deterministic IOVA allocator
 *   instead of the kernel's default DMA allocator. This is the
 *   plumbing that lets a SAVE/LOAD round-trip preserve every IOVA
 *   captured in the firmware blob across host changes -- without it,
 *   LOAD_VHCA_STATE accepts the blob but the destination VHCA's
 *   cmd ring is dead afterwards (every command 60s timeout).
 *
 *   This flag is orthogonal to MLX5_VFMIG_IOC_ENABLE_MIGRATABLE: the
 *   migratable bit is the firmware gate for the SUSPEND/SAVE/LOAD/
 *   RESUME command family; @vfmig_tracked is the host-driver gate for
 *   the IOMMU/IOVA layer that makes those commands semantically
 *   correct on a native (non-VFIO) destination. Userspace will
 *   typically call both, in either order, before binding the VF.
 *
 *   Lifetime semantics:
 *     - @enable=1 must be called while the VF is unbound (no driver
 *       attached). Returns -EBUSY otherwise. Allocates the per-VF
 *       IOMMU domain (idempotent: returns 0 if the flag is already
 *       set).
 *     - @enable=0 must also be called while the VF is unbound. Frees
 *       the domain and clears the flag. Returns -EBUSY if the VF is
 *       currently bound or if a LOAD blob is staged-but-unapplied
 *       (the staged blob references IOVAs in this domain).
 *     - The domain survives a VF unbind/rebind cycle. It is destroyed
 *       implicitly when sriov_numvfs is dropped (the VF goes away) or
 *       when the PF is unloaded. This avoids repeated
 *       iommu_domain_alloc()/teardown across SAVE -> destroy -> create
 *       -> LOAD cycles, which is the common case for HW-failure
 *       recovery.
 *
 *   Returns 0 on success, -EINVAL if vf_id is out of range or @flags
 *   has unknown bits, -EBUSY per the above, -ENODEV if the PF is
 *   gone, -EOPNOTSUPP if the platform has no IOMMU coverage for the
 *   VF's pci_dev (no IOMMU group, etc.).
 *
 *   The flag's current state is observable via
 *   MLX5_VFMIG_IOC_QUERY_VF -- the @tracked output field on
 *   struct mlx5_vfmig_query_vf reflects whether SET_TRACKED is in
 *   effect for a given vf_id without requiring the caller to bind
 *   the VF or otherwise touch it. Userspace orchestrators (e.g.
 *   CRIU's mlx5_sriov_vfmig plugin) rely on QUERY_VF for cheap
 *   discovery of vfmig-eligible VFs at startup.
 */
struct mlx5_vfmig_set_tracked {
	__u32 vf_id;	/* in  */
	__u32 enable;	/* in: 0 = detach domain + clear flag,
			 *     1 = attach domain + set flag
			 */
	__u32 flags;	/* in: reserved, must be 0 */
	__u32 reserved;
};
#define MLX5_VFMIG_IOC_SET_TRACKED \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x07, struct mlx5_vfmig_set_tracked)

/*
 * MLX5_VFMIG_IOC_PROBE_UID:
 *   *** EXPERIMENTAL DEBUG IOCTL -- NOT PART OF THE M2/M3 RESTORE
 *       CONTRACT. ***
 *
 *   Issues a CREATE_UCTX(VF) immediately followed by DESTROY_UCTX(VF)
 *   on a *bound* VF and returns the @uid the firmware handed back.
 *   The point is to read the firmware's per-VHCA uctx-table allocator
 *   high-water-mark without having to drive a real ucontext from
 *   user space, so we can answer the L4 Rung 3 question:
 *
 *       "Does LOAD_VHCA_STATE preserve the source's uctx-id space, or
 *        does the destination's allocator start fresh from 0?"
 *
 *   Methodology:
 *     - Source post-bind: ioctl(PROBE_UID, vf_id) -> U_src_1, U_src_2.
 *       The two values reveal whether the VHCA already has live UIDs
 *       (gap from 0 -> U_src_1) and whether allocation is monotonic
 *       (U_src_2 == U_src_1 + 1 typically).
 *     - SAVE the source VHCA, transport blob, LOAD on destination.
 *     - Destination post-bind: ioctl(PROBE_UID, vf_id) -> U_dst_1.
 *       - U_dst_1 >  U_src_2  ==> FW preserved source's uctx table
 *                                 across LOAD (good for "uid persistence").
 *       - U_dst_1 <= U_src_1  ==> FW reset uctx table on LOAD
 *                                 (uid identity is *not* preserved;
 *                                  R3 must use a different binding
 *                                  mechanism than uid-on-the-wire).
 *
 *   This ioctl is used only to *answer* an empirical question about
 *   firmware behaviour during R3 design. The R3 user-context restore
 *   path will not reuse this surface; it will go through dedicated
 *   uverbs verbs that bind ib_uobjects to existing FW objects via
 *   QUERY_*. Once L4 Rung 3 design lands, this ioctl can be removed
 *   without breaking any in-tree consumer.
 *
 *   The VF must currently be bound to mlx5_core (i.e. its mdev is
 *   "interface up"); otherwise -ENODEV is returned. This is by design
 *   -- CREATE_UCTX has no other_function form, so the command must
 *   be issued by the VF's own mdev.
 *
 *   Returns 0 with @uid populated on success; -EINVAL if vf_id is
 *   out of range or any reserved field is non-zero; -ENODEV if the
 *   VF is not currently bound to mlx5_core or its interface is down;
 *   any negative firmware-error code if CREATE_UCTX itself fails.
 *   On a CREATE_UCTX success followed by a DESTROY_UCTX failure the
 *   ioctl logs at warn level and returns the allocated @uid (FW
 *   leaks the uctx until the VHCA is torn down -- acceptable for a
 *   debug ioctl on a controlled experiment).
 */
struct mlx5_vfmig_probe_uid {
	__u32 vf_id;		/* in  */
	__u32 reserved;		/* in: must be 0 */
	__u16 uid;		/* out: uid returned by CREATE_UCTX
				 *      and immediately released by
				 *      DESTROY_UCTX on the same VHCA.
				 */
	__u16 reserved2;	/* out: zeroed */
	__u32 reserved3;	/* out: zeroed */
};
#define MLX5_VFMIG_IOC_PROBE_UID \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x08, struct mlx5_vfmig_probe_uid)

/*
 * MLX5_VFMIG_IOC_QUERY_QP:
 *   *** EXPERIMENTAL DEBUG IOCTL -- like PROBE_UID, NOT part of the
 *       M2/M3 contract. ***
 *
 *   Issues firmware QUERY_QP(opcode 0x50b) on a *bound* VF mdev for
 *   the supplied @qpn and returns the subset of the QPC that lets a
 *   userspace test compare a QP's state across SAVE_VHCA_STATE +
 *   LOAD_VHCA_STATE without owning a userspace ib_qp handle for it.
 *
 *   Use-case: tools/testing/mlx5_vfmig/design/uobject_restore.md §6.3
 *   piggyback experiment. On the source, allocate an RC QP, transition
 *   it through INIT / RTR / RTS via self-loopback (post N receive WRs
 *   along the way). Snapshot the QPC via this ioctl. SAVE, then on
 *   the destination LOAD + bind and re-issue the ioctl at the SAME
 *   @qpn (which K6 has already established remains reserved in FW).
 *   If the destination QPC fields match the source's byte-for-byte
 *   across the populated subset for the captured qp_state, the
 *   pending RX/TX bookkeeping survived LOAD_VHCA_STATE intrinsically
 *   and §6.3 / S6 of the R3 design need no kernel-side WR-replay
 *   plumbing. Mismatch implies a driver-side replay path is needed.
 *
 *   Field set covers:
 *
 *     - state-independent bookkeeping: state, pd, q_key, uar_page
 *       (the SQ doorbell source UAR id), log_page_size,
 *       log_{sq,rq}_size, log_msg_max, user_index
 *     - cross-references: remote_qpn, cqn_snd, cqn_rcv,
 *       srqn_rmpn_xrqn
 *     - PSNs: next_send_psn, next_rcv_psn, last_acked_psn
 *     - queue counters: hw/sw {sq_wqebb,rq}_counter
 *     - RTR-set: path_mtu, min_rnr_nak, log_rra_max, pkey_index
 *     - RTS-set: log_sra_max, retry_count, rnr_retry
 *     - AV: a flat 64-byte snapshot of QPC.primary_address_path
 *       (struct mlx5_ifc_ads_bits) for byte-equal compare. Includes
 *       dgid, dlid/mlid, sgid_index, sl, port, dmac, hop_limit,
 *       tclass, flow_label, udp_sport, ack_timeout, eth_prio, etc.
 *       The harness compares the raw bytes; no interpretation here.
 *
 *   The VF must currently be bound to mlx5_core and its mdev must be
 *   MLX5_INTERFACE_STATE_UP, same constraint as PROBE_UID. UID gating
 *   is bypassed because the command issues on the VF's own cmdif with
 *   the kernel uid (host-privileged); FW returns the QPC regardless
 *   of which ucontext originally allocated the QP. If a future FW
 *   tightens this, the ioctl will return the FW-error syndrome and
 *   we'll need to add an explicit "as_uid" argument.
 *
 *   Once §6.3 / S6 is empirically settled, this ioctl can be removed
 *   without breaking any in-tree consumer (or kept around as a debug
 *   surface -- it's still a relatively small wrapper).
 *
 *   Returns 0 on success with the @qpc_* fields populated; -EINVAL
 *   if vf_id is out of range or reserved fields are non-zero;
 *   -ENODEV if the VF is unbound or its mdev interface is down;
 *   any negative FW-error code if QUERY_QP itself fails (most
 *   commonly "QP doesn't exist on this VHCA").
 *
 *   ABI note: this struct grew between kernel revisions to add the
 *   wider field set above. The encoded ioctl number changes with the
 *   struct size (sizeof in the _IOWR macro), so old userspace built
 *   against the smaller struct will get -ENOTTY from a new kernel
 *   rather than reading a partial / misaligned result. Recompile the
 *   in-tree tool (tools/testing/mlx5_vfmig) against this header.
 */
struct mlx5_vfmig_query_qp {
	/* --- in --- */
	__u32 vf_id;			/* target VF on this PF */
	__u32 qpn;			/* FW qpn (24 bits significant) */
	__u32 reserved_in[2];		/* must be 0 */

	/* --- out: state-independent bookkeeping --- */
	__u32 qpc_state;		/* QPC.state nibble
					 *   (RST=0/INIT=1/RTR=2/RTS=3/...)
					 */
	__u32 qpc_pd;			/* QPC.pd            (24 bits) */
	__u32 qpc_q_key;		/* QPC.q_key         (32 bits) */
	__u32 qpc_uar_page;		/* QPC.uar_page      (24 bits)
					 *   SQ doorbell source UAR id;
					 *   stale-on-restore would mean
					 *   the user's mmap'd UAR is bound
					 *   to a different FW UAR slot.
					 */
	__u32 qpc_log_page_size;	/* QPC.log_page_size  (5 bits) */
	__u32 qpc_log_sq_size;		/* QPC.log_sq_size    (4 bits) */
	__u32 qpc_log_rq_size;		/* QPC.log_rq_size    (4 bits) */
	__u32 qpc_log_msg_max;		/* QPC.log_msg_max    (5 bits) */
	__u32 qpc_user_index;		/* QPC.user_index    (24 bits)
					 *   CQE.user_index source */

	/* --- out: cross-references --- */
	__u32 qpc_remote_qpn;		/* QPC.remote_qpn      (24 bits;
					 *                      RTR-set) */
	__u32 qpc_cqn_snd;		/* QPC.cqn_snd         (24 bits) */
	__u32 qpc_cqn_rcv;		/* QPC.cqn_rcv         (24 bits) */
	__u32 qpc_srqn_rmpn_xrqn;	/* QPC.srqn_rmpn_xrqn  (24 bits) */

	/* --- out: PSNs --- */
	__u32 qpc_next_send_psn;	/* QPC.next_send_psn   (24 bits) */
	__u32 qpc_next_rcv_psn;		/* QPC.next_rcv_psn    (24 bits) */
	__u32 qpc_last_acked_psn;	/* QPC.last_acked_psn  (24 bits) */

	/* --- out: queue counters --- */
	__u32 qpc_hw_sq_wqebb_counter;	/* (16 bits) */
	__u32 qpc_sw_sq_wqebb_counter;	/* (16 bits) */
	__u32 qpc_hw_rq_counter;	/* (32 bits) */
	__u32 qpc_sw_rq_counter;	/* (32 bits) */

	/* --- out: RTR-set --- */
	__u32 qpc_path_mtu;		/* QPC.mtu             (3 bits) */
	__u32 qpc_min_rnr_nak;		/* QPC.min_rnr_nak     (5 bits) */
	__u32 qpc_log_rra_max;		/* QPC.log_rra_max     (3 bits) */
	__u32 qpc_pkey_index;		/* QPC.primary_address_path.pkey_index
					 *                     (16 bits;
					 *                      INIT-set) */

	/* --- out: RTS-set --- */
	__u32 qpc_log_sra_max;		/* QPC.log_sra_max     (3 bits) */
	__u32 qpc_retry_count;		/* QPC.retry_count     (3 bits) */
	__u32 qpc_rnr_retry;		/* QPC.rnr_retry       (3 bits) */

	/* --- out: AV (RTR-set; raw 64-byte primary_address_path for
	 *       byte-equal compare against the QPC's dgid/dlid/sgid_idx/
	 *       sl/port/dmac/hop_limit/tclass/flow_label/udp_sport/
	 *       ack_timeout/eth_prio/...). The harness compares as
	 *       opaque bytes; if anything differs across LOAD, byte-
	 *       compare fails loud and we drill down post-hoc. */
	__u8  qpc_primary_address_path[64];

	__u8  reserved_out[16];		/* zeroed */
};
#define MLX5_VFMIG_IOC_QUERY_QP \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x09, struct mlx5_vfmig_query_qp)

/*
 * MLX5_VFMIG_IOC_PROBE_PD:
 *   *** EXPERIMENTAL DEBUG IOCTL -- like PROBE_UID and QUERY_QP, NOT
 *       part of the M2/M3 contract. ***
 *
 *   Drives the §S3b empirical question: "After LOAD_VHCA_STATE the
 *   firmware has the source's user-mode PDs alive on the destination
 *   VF -- but can a *fresh* destination ucontext (with a *new*
 *   devx_uid allocated by mlx5_ib_devx_create) reference one of those
 *   PDs in a subsequent FW operation?"
 *
 *   This question matters because mlx5_ib gates every uid-scoped op
 *   (CREATE_MKEY / CREATE_QP / CREATE_TIS / CREATE_TIR / ...) by
 *   `to_mpd(pd)->uid`. That field is intentionally decoupled from
 *   `context->devx_uid`: in the steady state they happen to match
 *   because mlx5_ib_alloc_pd() sets `pd->uid = context->devx_uid`
 *   at create time, but the runtime path doesn't enforce equality.
 *   So a Model-A `mlx5_ib_restore_pd` can build a kernel-side
 *   mlx5_ib_pd with `pdn = src_pdn, uid = src_uid` regardless of
 *   the owning ucontext's fresh devx_uid -- PROVIDED the FW lets
 *   subsequent ops referencing that pd succeed under uid=src_uid
 *   even though no ucontext on the destination owns src_uid.
 *   That is what this ioctl validates.
 *
 *   Mechanism: issues a transient CREATE_MKEY(uid=@uid_hint,
 *   pd=@pdn, access_mode=PA, length64=1) followed immediately by
 *   DESTROY_MKEY(uid=@uid_hint, mkey_index=<returned>) on the bound
 *   VF mdev's cmdif. If CREATE_MKEY returns 0, the FW accepted the
 *   (uid, pd) pair as legitimate; pdn is alive in the uid scope.
 *   If CREATE_MKEY fails, the FW syndrome is returned in
 *   @fw_syndrome (the caller can distinguish "bad PDN" from "bad
 *   UID" by re-issuing with a known-good pair). On a CREATE_MKEY
 *   success followed by a DESTROY_MKEY failure we log at warn level
 *   and still return 0 to the caller: the test mkey leaks until
 *   the VHCA is torn down, which is acceptable for a debug ioctl.
 *
 *   Methodology (mirrors PROBE_UID + QUERY_QP from K6):
 *
 *     - Source post-bind, BEFORE opening any ucontext:
 *         ioctl(PROBE_UID, src_vf) -> U_baseline
 *     - ibv_open_device(src_vf) -- libmlx5 calls GET_CONTEXT which
 *       allocates a devx_uid via mlx5_ib_devx_create; the FW uid
 *       allocator yields U_baseline (we recorded it BEFORE the
 *       open, so the next allocation matches).
 *     - ibv_alloc_pd(...) -- returns a PD whose FW pdn we capture
 *       via mlx5dv_pd or the (now landed) NLDEV RES_HANDLE
 *       cross-reference.
 *     - SAVE_VHCA_STATE on src, LOAD_VHCA_STATE on dst.
 *     - On dst, BEFORE any ucontext is bound to the restored VHCA:
 *         ioctl(PROBE_PD, dst_vf, pdn=src_pdn, uid_hint=U_baseline)
 *       Expected: 0 (and @fw_syndrome == 0).
 *     - Negative control 1: same call with a bogus pdn (e.g.
 *       0x00ffffff). Expected: non-zero @fw_syndrome with the
 *       FW's "invalid PD" code.
 *     - Negative control 2: same call with a bogus uid_hint
 *       (e.g. PROBE_UID baseline + 1000). Expected: non-zero
 *       @fw_syndrome with the FW's "invalid UID" / "access
 *       denied" code.
 *
 *   The VF must currently be bound to mlx5_core and its mdev must
 *   be MLX5_INTERFACE_STATE_UP, same constraint as PROBE_UID and
 *   QUERY_QP. The command is issued on the VF mdev's cmdif with
 *   the kernel uid_hint=0 placeholder in the cmdif header (FW
 *   reads @uid_hint from the create_mkey_in.uid field, not the
 *   cmdif uid). UID gating on CREATE_MKEY is the very property we
 *   are validating, so this ioctl deliberately exposes it.
 *
 *   Once §S3b empirical validation is settled, this ioctl can be
 *   removed without breaking any in-tree consumer (or kept around
 *   as a debug surface -- it's a small ~40-LOC wrapper).
 *
 *   Returns 0 on success with @fw_syndrome=0 (FW accepted the
 *   (uid, pd) pair); 0 with @fw_syndrome!=0 if CREATE_MKEY itself
 *   reported a syndrome (caller inspects @fw_syndrome to classify
 *   the rejection); -EINVAL if @vf_id is out of range, @pdn or
 *   @uid_hint exceed their 24-bit / 16-bit ranges, or any reserved
 *   field is non-zero; -ENODEV if the VF is unbound or its mdev
 *   interface is down; any negative kernel/FW err code on cmdif
 *   transport failure.
 */
struct mlx5_vfmig_probe_pd {
	__u32 vf_id;			/* in:  target VF on this PF */
	__u32 pdn;			/* in:  FW pdn to test
					 *      (24 bits significant)
					 */
	__u32 uid_hint;			/* in:  FW uid scope to test
					 *      against (16 bits significant)
					 */
	__u32 reserved_in;		/* in:  must be 0 */

	__u32 fw_syndrome;		/* out: 0 on FW accept, else
					 *      the firmware syndrome
					 *      returned by CREATE_MKEY
					 *      (a 32-bit FW error code).
					 */
	__u8  reserved_out[12];		/* out: zeroed */
};
#define MLX5_VFMIG_IOC_PROBE_PD \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x0a, struct mlx5_vfmig_probe_pd)

/*
 * MLX5_VFMIG_IOC_PROBE_MKEY:
 *   *** EXPERIMENTAL DEBUG IOCTL -- like PROBE_PD / PROBE_UID /
 *       QUERY_QP, NOT part of the M2/M3 contract. ***
 *
 *   Drives the §S4b empirical question: "After LOAD_VHCA_STATE the
 *   firmware should still have the source's user-mode MKEY at index
 *   N alive on the destination VF -- is it actually still there, and
 *   does its mkc context match what the source had at SAVE time
 *   (pd, length, start_addr)?"
 *
 *   This is the MR analogue of PROBE_PD but uses a different FW
 *   primitive. PROBE_PD issues a *transient* CREATE_MKEY to test
 *   whether a (pdn, uid) pair is FW-bound. PROBE_MKEY issues a pure
 *   QUERY_MKEY(mkey_index) (no side effects, no uid scoping at the
 *   command layer because mlx5_ifc_query_mkey_in has no uid field)
 *   and reads back the mkey context if the entry exists.
 *
 *   The "can a destination ucontext at uid=0 actually USE this
 *   adopted mkey?" question -- the analogue of PROBE_PD's uid_hint
 *   gating test -- is NOT covered by this ioctl. That part is
 *   tested empirically by the end-to-end S4b probe
 *   (mr_restore_probe_mlx5_vfmig + a uid=0 destination ucontext
 *   that issues a wire-visible op against the adopted mkey). The
 *   split mirrors PROBE_PD vs. mlx5_ib_restore_pd: the PF cdev
 *   ioctl tests FW-side existence; the userspace probe tests
 *   destination-ucontext-side usability.
 *
 *   Use:
 *     - On dst, AFTER LOAD_VHCA_STATE has been applied (typically
 *       AFTER MLX5_VFMIG_IOC_MARK_RESTORED but BEFORE any user
 *       ucontext binds to the restored VF):
 *         ioctl(PROBE_MKEY, dst_vf, mkey_index=src_mkey_index)
 *       Expected: 0 (and @fw_syndrome == 0). The output @fw_pd /
 *       @fw_length / @fw_start_addr should match the source's
 *       per-MR snapshot CRIU captured via the extended QUERY_MR
 *       (lkey >> 8 maps to mkey_index; iova / length map to
 *       start_addr / len). A mismatch is a kernel bug, not a FW
 *       bug -- the source's mkc was not byte-preserved by
 *       SAVE/LOAD.
 *     - Negative control: same call with a bogus mkey_index
 *       (e.g. 0x00ffffff). Expected: non-zero @fw_syndrome with
 *       the FW's "invalid mkey" code; @fw_pd / @fw_length /
 *       @fw_start_addr zeroed.
 *
 *   The VF must currently be bound to mlx5_core and its mdev must
 *   be MLX5_INTERFACE_STATE_UP, same constraint as PROBE_UID and
 *   PROBE_PD. The command is issued on the VF mdev's cmdif.
 *
 *   Errors: -EFAULT on copy_{from,to}_user; -EINVAL if @mkey_index
 *   exceeds its 24-bit range, or any reserved field is non-zero;
 *   -ENODEV if the VF is unbound or its mdev interface is down;
 *   any negative kernel/FW err code on cmdif transport failure.
 */
struct mlx5_vfmig_probe_mkey {
	__u32 vf_id;			/* in:  target VF on this PF */
	__u32 mkey_index;		/* in:  FW mkey index to query
					 *      (24 bits significant)
					 */
	__u8  reserved_in[8];		/* in:  must be 0 */

	__u32 fw_syndrome;		/* out: 0 on FW accept, else
					 *      the firmware syndrome
					 *      returned by QUERY_MKEY
					 *      (a 32-bit FW error code).
					 */
	__u32 fw_pd;			/* out: mkc.pd (24 bits)
					 *      0 on reject.
					 */
	__u32 fw_qpn;			/* out: mkc.qpn (24 bits;
					 *      0xffffff means "any qp")
					 *      0 on reject.
					 */
	__u8  reserved_out0[4];		/* out: zeroed */
	__u64 fw_start_addr;		/* out: mkc.start_addr -- 0 on
					 *      reject. Compare with the
					 *      source iova captured at
					 *      dump.
					 */
	__u64 fw_length;		/* out: mkc.len -- 0 on reject.
					 *      Compare with the source
					 *      length captured at dump.
					 */
	__u8  reserved_out1[8];		/* out: zeroed */
};
#define MLX5_VFMIG_IOC_PROBE_MKEY \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x0b, struct mlx5_vfmig_probe_mkey)

/*
 * MLX5_VFMIG_IOC_QUERY_AWAITING_BIND:
 *   user_mr_dma stage-2 success-criterion accessor (see
 *   tools/testing/mlx5_vfmig/design/user_mr_dma.md section 6.4).
 *
 *   Walks the target VF's per-VF vfmig_iova_domain registry and
 *   reports how many external (USER_PAGE / vfmig_dma_ops-backed)
 *   entries are currently in the @awaiting_bind = true state --
 *   i.e. pre-installed by VFMIG_WIRE_TAG_HOST_USER_PAGE replay
 *   during LOAD but not yet bound to a concrete destination phys
 *   page by stage-3's hint-aware vfmig_dma_ops.map_sg.
 *
 *   @count_by_kind[k] (k in enum vfmig_huobj_kind) splits the
 *   count by uobject kind: MR, CQ, QP, SRQ, DBR. Untagged auto-
 *   numbered placeholders are accounted under index
 *   VFMIG_HUOBJ_KIND_NONE (= 0) but in practice never appear
 *   because the wire-emit side only puts retagged entries on the
 *   wire. The 8-slot array gives headroom for future kinds (ODP,
 *   DEVX, dma-buf) without an ABI break -- userspace iterates up
 *   to its compiled-in VFMIG_HUOBJ_KIND_NR.
 *
 *   Methodology (stage-2 PASS):
 *     - Source: register N MRs / CQ / QP / SRQ on a tracked VF,
 *       SAVE. Wire dumps NR_RECORDS = sum across kinds.
 *     - Destination: bind dst VF, LOAD, ioctl(QUERY_AWAITING_BIND).
 *       Expected: @total == NR_RECORDS, count_by_kind[k] matches
 *       the source's per-kind tally.
 *     - Negative control: query an untracked VF -> -ENODEV.
 *     - Negative control: query a tracked VF that hasn't LOADed
 *       any wire records yet -> @total == 0.
 *
 *   The VF does NOT need to be bound to mlx5_core to query --
 *   the registry lives on the PF, and SET_TRACKED's "domain
 *   survives unbind" property means we can read post-LOAD,
 *   pre-bind. This is unlike PROBE_PD / PROBE_MKEY / PROBE_UID
 *   which need a bound VF mdev to issue raw FW commands.
 *
 *   Errors:
 *     -EFAULT  copy_{from,to}_user
 *     -EINVAL  vf_id out of range, or @reserved_in non-zero
 *     -ENODEV  VF is not tracked (no per-VF vfmig_iova_domain),
 *              or PF is gone
 */
#define MLX5_VFMIG_QUERY_AWAITING_BIND_NR_KINDS	8

struct mlx5_vfmig_query_awaiting_bind {
	__u32 vf_id;			/* in:  target VF on this PF */
	__u32 reserved_in;		/* in:  must be 0 */

	__u64 total;			/* out: total awaiting_bind=true
					 *      external entries across all
					 *      kinds.
					 */
	__u64 count_by_kind		/* out: per-kind breakdown,
					 *      indexed by enum
					 *      vfmig_huobj_kind. Slots
					 *      beyond the kernel's current
					 *      KIND_NR read as 0.
					 */
		[MLX5_VFMIG_QUERY_AWAITING_BIND_NR_KINDS];

	__u8  reserved_out[16];		/* out: zeroed */
};
#define MLX5_VFMIG_IOC_QUERY_AWAITING_BIND \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x0c, struct mlx5_vfmig_query_awaiting_bind)

/*
 * MLX5_VFMIG_IOC_PROBE_CQN:
 *   *** EXPERIMENTAL DEBUG IOCTL -- like PROBE_PD / PROBE_MKEY /
 *       PROBE_UID / QUERY_QP, NOT part of the M2/M3 contract. ***
 *
 *   Drives the §S5b empirical question: "After LOAD_VHCA_STATE the
 *   firmware should still have the source's user-mode CQ at index N
 *   alive on the destination VF -- is it actually still there, and
 *   does its cqc context match what the source had at SAVE time
 *   (cqn, eqn, log_cq_size, log_page_size, page_offset, status,
 *   oi)?"
 *
 *   This is the CQ analogue of PROBE_MKEY, with the same FW-side
 *   primitive shape: a pure QUERY_CQ(cqn) (opcode 0x402) with no
 *   side effects, and on FW accept the handler reads back the cqc
 *   header so the userspace test can byte-compare with the source's
 *   pre-SAVE view.
 *
 *   The load-bearing field over PROBE_MKEY is @fw_eqn -- a CQ's
 *   completions flow through an event queue, and the EQ binding
 *   must survive LOAD_VHCA_STATE for adopted CQs to deliver
 *   completions on the destination side. K6 already shows
 *   LOAD_VHCA_STATE preserves the cqn high-water mark; PROBE_CQN
 *   adds the byte-equality check that the cqc *contents* (and in
 *   particular the eqn binding) are also preserved.
 *
 *   The "can a destination ucontext at uid=0 actually USE this
 *   adopted CQ?" question -- the analogue of PROBE_PD's uid_hint
 *   gating test, and PROBE_MKEY's split with mr_restore_probe_*
 *   -- is NOT covered by this ioctl. That part is tested
 *   empirically by the end-to-end S5b probe
 *   (cq_restore_probe_mlx5_vfmig) once it lands. The split is
 *   intentional: PROBE_CQN tests FW-side existence with zero side
 *   effects (so it can run freely in test harnesses); the
 *   userspace probe tests destination-ucontext usability with
 *   mlx5_ib in the loop.
 *
 *   Use:
 *     - On dst, AFTER LOAD_VHCA_STATE has been applied (typically
 *       AFTER MLX5_VFMIG_IOC_MARK_RESTORED but BEFORE any user
 *       ucontext binds to the restored VF):
 *         ioctl(PROBE_CQN, dst_vf, cqn=src_cqn)
 *       Expected: 0 (and @fw_syndrome == 0). The output @fw_eqn /
 *       @fw_log_cq_size / @fw_log_page_size / @fw_page_offset /
 *       @fw_status / @fw_oi should match the source's per-CQ
 *       snapshot CRIU captured pre-SAVE. A mismatch is a kernel/FW
 *       SAVE-LOAD bug, not a usage bug.
 *     - Negative control: same call with a bogus cqn (e.g. one
 *       above the source's high-water mark, or 0x00ffffff).
 *       Expected: non-zero @fw_syndrome with the FW's "BAD_RES_STATE"
 *       / "invalid cqn" code; all @fw_* output fields zeroed.
 *
 *   The VF must currently be bound to mlx5_core and its mdev must
 *   be MLX5_INTERFACE_STATE_UP, same constraint as PROBE_UID /
 *   PROBE_PD / PROBE_MKEY. The command is issued on the VF mdev's
 *   cmdif.
 *
 *   Errors: -EFAULT on copy_{from,to}_user; -EINVAL if @cqn exceeds
 *   its 24-bit range, or any reserved field is non-zero; -ENODEV
 *   if the VF is unbound or its mdev interface is down; any
 *   negative kernel/FW err code on cmdif transport failure.
 */
struct mlx5_vfmig_probe_cqn {
	__u32 vf_id;			/* in:  target VF on this PF */
	__u32 cqn;			/* in:  FW cqn to query
					 *      (24 bits significant)
					 */
	__u8  reserved_in[8];		/* in:  must be 0 */

	__u32 fw_syndrome;		/* out: 0 on FW accept, else
					 *      the firmware syndrome
					 *      returned by QUERY_CQ
					 *      (a 32-bit FW error code).
					 */
	__u32 fw_eqn;			/* out: cqc.c_eqn_or_apu_element
					 *      (32 bits). When @fw_apu_cq
					 *      is 0 (the only case at v0
					 *      of S5b) this is the
					 *      destination-side EQ id the
					 *      adopted CQ delivers
					 *      completions on. Must equal
					 *      the source's eqn for the
					 *      §S5b byte-equality
					 *      assertion.  0 on reject.
					 */
	__u8  fw_status;		/* out: cqc.status (4 bits).
					 *      0 = OK; nonzero means the
					 *      saved CQ was already in an
					 *      error state. 0 on reject.
					 */
	__u8  fw_log_cq_size;		/* out: cqc.log_cq_size (5 bits).
					 *      0 on reject.
					 */
	__u8  fw_log_page_size;		/* out: cqc.log_page_size
					 *      (5 bits). 0 on reject.
					 */
	__u8  fw_page_offset;		/* out: cqc.page_offset (6 bits).
					 *      0 on reject.
					 */
	__u8  fw_oi;			/* out: cqc.oi (1 bit) --
					 *      overrun-ignore. 0 on reject.
					 */
	__u8  fw_cqe_sz;		/* out: cqc.cqe_sz (3 bits).
					 *      0=64B 1=128B 2=256B 3=512B.
					 *      Forensic only -- not part
					 *      of the byte-equality
					 *      assertion. 0 on reject.
					 */
	__u8  fw_apu_cq;		/* out: cqc.apu_cq (1 bit).
					 *      v0 of S5b expects 0; if 1,
					 *      @fw_eqn is an APU element
					 *      id rather than an eqn and
					 *      adoption is unsupported.
					 *      0 on reject.
					 */
	__u8  reserved_out0;		/* out: zeroed */

	__u32 fw_uar_page;		/* out: cqc.uar_page (24 bits).
					 *      Forensic only -- KIND_DBR
					 *      doorbell-page binding will
					 *      sanity-check this against
					 *      the source's uar_page in B3.
					 *      0 on reject.
					 */
	__u32 reserved_out1;		/* out: zeroed */
	__u64 fw_dbr_addr;		/* out: cqc.dbr_addr (64 bits)
					 *      -- user-VA of the doorbell
					 *      ring in the source ucontext.
					 *      Forensic only at B0; will
					 *      be the input to KIND_DBR
					 *      umem-bind in B3. 0 on reject.
					 */
	__u8  reserved_out2[16];	/* out: zeroed */
};
#define MLX5_VFMIG_IOC_PROBE_CQN \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x0d, struct mlx5_vfmig_probe_cqn)

/*
 * MLX5_VFMIG_IOC_PROBE_QP_TEARDOWN:
 *   *** EXPERIMENTAL DEBUG IOCTL -- like PROBE_PD / PROBE_MKEY /
 *       PROBE_CQN / PROBE_UID / QUERY_QP, NOT part of the M2/M3
 *       contract. ***
 *
 *   Drives the §S3b "DEVX-adoption blind spot, destroy direction"
 *   empirical question: "Given a QPC adopted from a DEVX-enabled
 *   source (qpc owner-uid == src.devx_uid != 0), what does the FW
 *   actually do when the destination kernel issues DESTROY_QP or
 *   2RST_QP with a different uid in the IFC uid field?"
 *
 *   Background: the existing §S3b matrix in test_pd_adopt.sh only
 *   probed the CREATE direction (CREATE_MKEY with various uid_hints
 *   against an adopted PDN). The destroy direction was inferred,
 *   not measured. The CRIU agent's pd_cq_qp end-to-end repro on
 *   FW 28.48.1000 surfaced a smoking-gun asymmetry: kernel-side
 *   mlx5_cmd_exec(DESTROY_QP, uid=0) returns err=0 against a uid=2-
 *   owned QPC (so destroy_qp_common's mlx5_ib_err diagnostic does
 *   NOT fire), but the next opcode that propagates a FW errno
 *   (DEALLOC_PD, in the v0 mitigation chain that opens the dest
 *   ucontext without DEVX) fails with bad_resource_state syndrome
 *   0xef0c8a-class -- meaning *something* still pins the PDC after
 *   the kernel believed the QP was destroyed.
 *
 *   This ioctl bracket-tests that scenario directly:
 *
 *     1. QUERY_QP(qpn) -- pre-op snapshot. fw_status / fw_syndrome
 *        from FW; @pre_qpc_state is qpc.state, @pre_qpc_pd is
 *        qpc.pd. Establishes "the FW QPC exists and has these
 *        contents at the moment of the test".
 *
 *     2. The op being tested:
 *          @op_mode = MLX5_VFMIG_QP_TEARDOWN_OP_DESTROY (0):
 *            DESTROY_QP(qpn, uid=@uid_hint).
 *          @op_mode = MLX5_VFMIG_QP_TEARDOWN_OP_2RST (1):
 *            MODIFY_QP 2RST(qpn, uid=@uid_hint).
 *        @op_status / @op_syndrome from FW; for 2RST_QP a non-zero
 *        FW syndrome with err != 0 is a hard reject; for
 *        DESTROY_QP a zero @op_status from FW is the "looks like
 *        success" lane the agent observed -- the bracketing
 *        QUERY_QP below is what tells us if the QPC actually
 *        died.
 *
 *     3. QUERY_QP(qpn) -- post-op snapshot. The interpretation:
 *          @post_query_status == 0 => QPC is still alive in FW.
 *            For op_mode==DESTROY this is the "silent no-op"
 *            verdict: FW ack'd the destroy but did not destroy.
 *            For op_mode==2RST this means modify either succeeded
 *            (and qpc.state should be 0 = RESET) or silently
 *            no-op'd (qpc.state unchanged).
 *          @post_query_status != 0 => QUERY_QP rejected. Most
 *            likely "QPC not found" syndrome class, meaning the
 *            destroy actually destroyed. Caller decodes
 *            @post_query_syndrome to be sure.
 *
 *   What we deliberately do NOT measure:
 *     - The QPC owner-uid. Looking at struct mlx5_ifc_qpc_bits the
 *       FW does not surface the owning uid in the queryable QPC
 *       (it is FW-internal allocator metadata). Inference about
 *       cross-uid behaviour comes from "we issued DESTROY/2RST
 *       with uid=@uid_hint, the QPC was ?provably not? destroyed
 *       afterwards, and we know what uid the source ucontext had
 *       allocated this QP under from the harness's dmesg capture".
 *     - Subsequent FW resource refcount state. Even if QUERY_QP
 *       reports "QPC gone", the bug surface in the agent's repro
 *       was a PDC pin that survived the destroy chain. Followup
 *       PROBE_PD (existing) + DEALLOC_PD attempt (caller-driven)
 *       checks that.
 *
 *   Locking, VF mdev lookup, and cmdif uid all mirror PROBE_PD /
 *   QUERY_QP / PROBE_UID. The VF mdev's cmdif runs cmdif-uid=0
 *   (host-privileged); we reach the @uid_hint via the IFC field
 *   on the destroy/modify/query commands. QUERY_QP itself has no
 *   uid field on the input (struct mlx5_ifc_query_qp_in_bits
 *   reserved_at_10 stays 0), so the bracketing queries always run
 *   under host-priv and observe whatever state the actual op
 *   produced.
 *
 *   Errors: -EFAULT on copy_{from,to}_user; -EINVAL if @vf_id is
 *   out of range, @qpn or @uid_hint exceed their 24-bit / 16-bit
 *   ranges, @op_mode is unknown, or any reserved field is non-zero;
 *   -ENODEV if the VF is unbound or its mdev interface is down;
 *   any negative kernel/FW err code on cmdif transport failure for
 *   the bracketing QUERY_QP calls. The op-under-test (destroy or
 *   modify) does NOT propagate its negative errno: a FW reject is
 *   recorded in @op_status / @op_syndrome and the call returns 0
 *   so the caller can see the bracketing query result.
 */
enum mlx5_vfmig_qp_teardown_op {
	MLX5_VFMIG_QP_TEARDOWN_OP_DESTROY = 0,
	MLX5_VFMIG_QP_TEARDOWN_OP_2RST    = 1,
};

struct mlx5_vfmig_probe_qp_teardown {
	__u32 vf_id;			/* in:  target VF on this PF */
	__u32 qpn;			/* in:  FW QP number to test
					 *      (24 bits significant)
					 */
	__u32 uid_hint;			/* in:  uid value to write into the
					 *      destroy/modify command's IFC
					 *      uid field (16 bits significant)
					 */
	__u32 op_mode;			/* in:  enum mlx5_vfmig_qp_teardown_op:
					 *      0 = DESTROY_QP (default)
					 *      1 = MODIFY_QP 2RST
					 */
	__u8  reserved_in[16];		/* in:  must be 0 */

	/* pre-op QUERY_QP */
	__u32 pre_query_status;		/* out: 0 = QPC found pre-op;
					 *      non-zero = QUERY_QP failed
					 *      pre-op (QPC missing? FW
					 *      transport failure?). On
					 *      pre-query failure the @op
					 *      step is SKIPPED and @op_*
					 *      / @post_* are zeroed.
					 */
	__u32 pre_query_syndrome;	/* out: FW syndrome if
					 *      pre-query rejected; 0 on
					 *      accept.
					 */
	__u8  pre_qpc_state;		/* out: qpc.state pre-op
					 *      (0=RESET 1=INIT 2=RTR
					 *      3=RTS 4=SQEr 5=SQD
					 *      6=ERR 9=Suspended 10=SQDC).
					 *      0 if pre_query_status != 0.
					 */
	__u8  reserved_pre[3];		/* out: zeroed */
	__u32 pre_qpc_pd;		/* out: qpc.pd pre-op (24 bits).
					 *      0 if pre_query_status != 0.
					 *      Useful for cross-checking
					 *      with the source's src_pdn
					 *      capture before SAVE.
					 */

	/* the destroy or modify-2RST result */
	__u32 op_status;		/* out: -ERRNO returned by
					 *      mlx5_cmd_exec for the op
					 *      under test (0 = FW ack;
					 *      < 0 = FW reject converted
					 *      to errno). Cast through
					 *      (int).
					 */
	__u32 op_syndrome;		/* out: FW syndrome from the op
					 *      output blob; 0 on FW
					 *      accept.
					 */

	/* post-op QUERY_QP */
	__u32 post_query_status;	/* out: 0 = QPC still alive;
					 *      non-zero = QUERY_QP
					 *      rejected (QPC gone if
					 *      syndrome class is
					 *      "not found"). The smoking-
					 *      gun signal for "did the
					 *      destroy actually destroy?":
					 *        op_status==0 &&
					 *        post_query_status==0 ==
					 *          silent no-op (QPC
					 *          survived a successful-
					 *          looking destroy).
					 *        op_status==0 &&
					 *        post_query_status!=0 ==
					 *          destroy worked.
					 */
	__u32 post_query_syndrome;	/* out: FW syndrome if post-
					 *      query rejected; 0 on
					 *      accept.
					 */
	__u8  post_qpc_state;		/* out: qpc.state post-op (only
					 *      meaningful when
					 *      post_query_status==0). 0 if
					 *      QPC gone or pre-query failed.
					 */
	__u8  reserved_post[3];		/* out: zeroed */
	__u8  reserved_out[16];		/* out: zeroed */
};

#define MLX5_VFMIG_IOC_PROBE_QP_TEARDOWN \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x0e, struct mlx5_vfmig_probe_qp_teardown)

/*
 * MLX5_VFMIG_IOC_PROBE_DEALLOC_PD:
 *   *** EXPERIMENTAL DEBUG IOCTL -- like PROBE_QP_TEARDOWN, NOT
 *       part of the M2/M3 contract. ***
 *
 *   Drives the §S3b "DEALLOC_PD uid-gating" empirical question:
 *   "Does FW honor a caller-supplied uid_hint in DEALLOC_PD's
 *   alloc_pd_in.uid field, or does it gate on the PDC's owning
 *   uid (the one captured at ALLOC_PD time)?"
 *
 *   Background: PROBE_QP_TEARDOWN (ioctl 0x0e) refuted the
 *   silent-no-op hypothesis on DESTROY_QP / 2RST_QP -- those
 *   honor the uid_hint and actually destroy / reset uid=src-owned
 *   QPCs under uid=0 / src_devx / hi_unalloc lanes alike. But the
 *   CRIU agent's pd_cq_qp end-to-end repro still fails at
 *   DEALLOC_PD with bad_resource_state syndrome 0xef0c8a-class
 *   under the v0 mitigation (dest ucontext opened without DEVX,
 *   so mpd->uid=0 is asserted on the dealloc against a uid=src-
 *   owned PDC).
 *
 *   This ioctl tests DEALLOC_PD's uid semantics in isolation,
 *   on a dependent-free source-owned PDC (the harness uses a
 *   PD-only source probe so the PDC has no QP/CQ/MR/SRQ
 *   dependents). Three cells, one per uid lane:
 *     uid=0           -- v0 mitigation lane (host-priv claim)
 *     uid=src_devx    -- speculative kernel-only fix: cache
 *                        source's devx_uid at restore_pd and
 *                        assert it on dealloc
 *     uid=hi_unalloc  -- alternative host-priv bucket from
 *                        test_pd_adopt's matrix
 *
 *   Per cell we report op_status / op_syndrome (FW result of the
 *   dealloc). PDC existence pre/post is left to the caller via the
 *   existing MLX5_VFMIG_IOC_PROBE_PD (CREATE_MKEY-acceptance test):
 *     pre  PROBE_PD(pdn, uid=0)  expected fw_accept=1 (PDC alive)
 *     PROBE_DEALLOC_PD(pdn, uid_hint)
 *     post PROBE_PD(pdn, uid=0)  fw_accept=0 (with "unknown PD"
 *                                syndrome class) means the PDC
 *                                was actually deallocated.
 *
 *   Locking, VF mdev lookup, cmdif uid all mirror PROBE_PD /
 *   PROBE_QP_TEARDOWN. The VF mdev's cmdif runs cmdif-uid=0
 *   (host-privileged); we reach the @uid_hint via the IFC field
 *   on the dealloc command.
 *
 *   This ioctl IS destructive on success: a successful dealloc
 *   removes the source's PDC from the destination VHCA. Use one
 *   PDC per uid_hint cell (the harness allocates 3 source PDs).
 *
 *   Errors: -EFAULT on copy_{from,to}_user; -EINVAL if @vf_id is
 *   out of range, @pdn or @uid_hint exceed their 24-bit / 16-bit
 *   ranges, or any reserved field is non-zero; -ENODEV if the VF
 *   is unbound or its mdev interface is down. The op-under-test
 *   does NOT propagate its negative errno: a FW reject is
 *   recorded in @op_status / @op_syndrome and the call returns 0.
 */
struct mlx5_vfmig_probe_dealloc_pd {
	__u32 vf_id;			/* in:  target VF on this PF */
	__u32 pdn;			/* in:  FW pdn to dealloc
					 *      (24 bits significant)
					 */
	__u32 uid_hint;			/* in:  uid value to write into
					 *      DEALLOC_PD's IFC uid field
					 *      (16 bits significant)
					 */
	__u8  reserved_in[12];		/* in:  must be 0 */

	__u32 op_status;		/* out: -ERRNO returned by
					 *      mlx5_cmd_exec for DEALLOC_PD
					 *      (cast through int; 0 = FW
					 *      ack).
					 */
	__u32 op_syndrome;		/* out: FW syndrome from the
					 *      DEALLOC_PD output blob;
					 *      0 on FW accept.
					 */
	__u8  reserved_out[16];		/* out: zeroed */
};

#define MLX5_VFMIG_IOC_PROBE_DEALLOC_PD \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x0f, struct mlx5_vfmig_probe_dealloc_pd)

/*
 * MLX5_VFMIG_IOC_PROBE_CQ_DESTROY:
 *   *** EXPERIMENTAL DEBUG IOCTL -- like PROBE_QP_TEARDOWN /
 *       PROBE_DEALLOC_PD, NOT part of the M2/M3 contract. ***
 *
 *   Drives the §S3b "Generalising to QP/CQ" empirical question for
 *   CQs: "Given a CQC adopted from a DEVX-enabled source (cqc owner-
 *   uid == src.devx_uid != 0), does the FW honor a different uid
 *   in DESTROY_CQ's IFC uid field?"
 *
 *   The PD gate analysis (design/pd_registration_wipe.md) showed
 *   DEALLOC_PD is sensitive to the (pdn -> owner_uid) registration
 *   wipe imposed by LOAD_VHCA_STATE. This ioctl asks whether
 *   DESTROY_CQ has the same sensitivity: if it returns "CQN unknown
 *   to allocator" with vfmig-restored CQs, we need a parallel gate
 *   in mlx5_ib_destroy_cq; if it works cross-uid like DESTROY_QP,
 *   no kernel gate is needed.
 *
 *   Bracketing pattern mirrors PROBE_QP_TEARDOWN:
 *     1. pre-op QUERY_CQ   -- prove the CQC exists post-LOAD
 *     2. DESTROY_CQ(cqn, uid_hint)
 *     3. post-op QUERY_CQ  -- prove the CQC is actually gone
 *
 *   "DESTROY worked across uid" verdict:
 *     op_status == 0 && post_query_status != 0
 *   "DESTROY silent no-op" verdict:
 *     op_status == 0 && post_query_status == 0
 *   "DESTROY failed cross-uid (gate needed)" verdict:
 *     op_status != 0 -- inspect op_syndrome for the class
 *
 *   Locking, VF mdev lookup, cmdif uid all mirror PROBE_PD /
 *   PROBE_QP_TEARDOWN. The VF mdev's cmdif runs cmdif-uid=0
 *   (host-privileged); the @uid_hint is asserted via the IFC
 *   uid field on the destroy command.
 *
 *   This ioctl IS destructive on success. Use one source CQ per
 *   uid_hint cell (the harness allocates 3 source CQs).
 *
 *   Errors: -EFAULT on copy_{from,to}_user; -EINVAL if @vf_id is
 *   out of range, @cqn or @uid_hint exceed their 24-bit / 16-bit
 *   ranges, or any reserved field is non-zero; -ENODEV if the VF
 *   is unbound or its mdev interface is down. The op-under-test
 *   does NOT propagate its negative errno: a FW reject is recorded
 *   in @op_status / @op_syndrome and the call returns 0, so the
 *   caller can see the bracketing query result.
 */
struct mlx5_vfmig_probe_cq_destroy {
	__u32 vf_id;			/* in:  target VF on this PF */
	__u32 cqn;			/* in:  FW CQ number to destroy
					 *      (24 bits significant)
					 */
	__u32 uid_hint;			/* in:  uid value to write into
					 *      DESTROY_CQ's IFC uid field
					 *      (16 bits significant)
					 */
	__u8  reserved_in[16];		/* in:  must be 0 */

	/* pre-op QUERY_CQ */
	__u32 pre_query_status;		/* out: 0 = CQC found pre-op;
					 *      non-zero = QUERY_CQ failed
					 *      pre-op (CQC missing? FW
					 *      transport failure?). On
					 *      pre-query failure the @op
					 *      step is SKIPPED and @op_*
					 *      / @post_* are zeroed.
					 */
	__u32 pre_query_syndrome;	/* out: FW syndrome if
					 *      pre-query rejected; 0 on
					 *      accept.
					 */
	__u8  pre_cqc_status;		/* out: cqc.status pre-op (4 bits
					 *      significant). 0 if
					 *      pre_query_status != 0.
					 */
	__u8  reserved_pre[3];		/* out: zeroed */

	/* DESTROY_CQ */
	__u32 op_status;		/* out: -ERRNO returned by
					 *      mlx5_cmd_exec for DESTROY_CQ
					 *      (0 = FW ack; <0 = FW reject
					 *      converted to errno). Cast
					 *      through int.
					 */
	__u32 op_syndrome;		/* out: FW syndrome from the
					 *      DESTROY_CQ output blob;
					 *      0 on FW accept.
					 */

	/* post-op QUERY_CQ */
	__u32 post_query_status;	/* out: 0 = CQC still alive
					 *      (silent no-op if op_status
					 *      was also 0); non-zero =
					 *      CQC gone (the "destroy
					 *      worked" lane).
					 */
	__u32 post_query_syndrome;	/* out: FW syndrome if post-
					 *      query rejected; 0 on
					 *      accept.
					 */
	__u8  post_cqc_status;		/* out: cqc.status post-op (only
					 *      meaningful when
					 *      post_query_status==0). 0 if
					 *      CQC gone or pre-query failed.
					 */
	__u8  reserved_post[3];		/* out: zeroed */
	__u8  reserved_out[16];		/* out: zeroed */
};

#define MLX5_VFMIG_IOC_PROBE_CQ_DESTROY \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x10, struct mlx5_vfmig_probe_cq_destroy)

/*
 * MLX5_VFMIG_IOC_PROBE_MR_DESTROY:
 *   *** EXPERIMENTAL DEBUG IOCTL -- like PROBE_QP_TEARDOWN /
 *       PROBE_DEALLOC_PD / PROBE_CQ_DESTROY, NOT part of the
 *       M2/M3 contract. ***
 *
 *   Drives the same §S3b "Generalising to QP/CQ" empirical
 *   question for memory keys: does DESTROY_MKEY honor a uid_hint
 *   that differs from the mkey's owning uid (which after
 *   LOAD_VHCA_STATE is a wiped registration entry on the dest)?
 *
 *   Bracketing pattern mirrors PROBE_CQ_DESTROY: pre-op
 *   QUERY_MKEY, DESTROY_MKEY, post-op QUERY_MKEY. The "destroy
 *   worked across uid" verdict is op_status==0 && post_query
 *   _status != 0. "Silent no-op" is op_status==0 && post_query
 *   _status==0. "Cross-uid rejection (gate needed)" is op_status
 *   != 0 with op_syndrome in the registration-wipe class.
 *
 *   Note: mkc.free is the post-destroy life-status indicator
 *   (free=1 means the mkey index is freed back to the allocator)
 *   but a freshly-allocated mkey has free=0. After a successful
 *   DESTROY_MKEY the post-op QUERY_MKEY will return a "not
 *   found" syndrome, which is the actual pass signal.
 *
 *   Destructive on success. Use one source mkey per uid_hint
 *   cell; the harness allocates 3 source MRs.
 *
 *   Errors / locking semantics mirror PROBE_QP_TEARDOWN.
 */
struct mlx5_vfmig_probe_mr_destroy {
	__u32 vf_id;			/* in:  target VF on this PF */
	__u32 mkey_index;		/* in:  FW mkey index to destroy
					 *      (24 bits significant; this
					 *      is the upper-24 of the full
					 *      mkey == mkey_variant<<24 |
					 *      mkey_index).
					 */
	__u32 uid_hint;			/* in:  uid value to write into
					 *      DESTROY_MKEY's IFC uid field
					 *      (16 bits significant)
					 */
	__u8  reserved_in[16];		/* in:  must be 0 */

	/* pre-op QUERY_MKEY */
	__u32 pre_query_status;		/* out: 0 = MKEY found pre-op;
					 *      non-zero = QUERY_MKEY failed
					 *      pre-op. On pre-query failure
					 *      the @op step is SKIPPED and
					 *      @op_* / @post_* are zeroed.
					 */
	__u32 pre_query_syndrome;	/* out: FW syndrome if
					 *      pre-query rejected; 0 on
					 *      accept.
					 */
	__u8  pre_mkc_free;		/* out: mkc.free pre-op (1 bit
					 *      significant). 0 if
					 *      pre_query_status != 0.
					 */
	__u8  reserved_pre[3];		/* out: zeroed */

	/* DESTROY_MKEY */
	__u32 op_status;		/* out: -ERRNO returned by
					 *      mlx5_cmd_exec for
					 *      DESTROY_MKEY.
					 */
	__u32 op_syndrome;		/* out: FW syndrome from the
					 *      DESTROY_MKEY output blob;
					 *      0 on FW accept.
					 */

	/* post-op QUERY_MKEY */
	__u32 post_query_status;	/* out: 0 = MKC still alive
					 *      (silent no-op if op_status
					 *      was also 0); non-zero =
					 *      MKC gone.
					 */
	__u32 post_query_syndrome;	/* out: FW syndrome if post-
					 *      query rejected; 0 on
					 *      accept.
					 */
	__u8  post_mkc_free;		/* out: mkc.free post-op (only
					 *      meaningful when
					 *      post_query_status==0). 0 if
					 *      MKC gone or pre-query failed.
					 */
	__u8  reserved_post[3];		/* out: zeroed */
	__u8  reserved_out[16];		/* out: zeroed */
};

#define MLX5_VFMIG_IOC_PROBE_MR_DESTROY \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x11, struct mlx5_vfmig_probe_mr_destroy)

/*
 * MLX5_VFMIG_IOC_SET_VF_UUID:
 *   Stamp the orchestrator's 16-byte UUID into the VF's per-VF
 *   context slot.  Read back via MLX5_VFMIG_IOC_QUERY_VF on either
 *   the source or destination host.  This is the orchestrator's
 *   handle on "this VF carries this workload's identity"; it is
 *   the *only* identity tag CRIU's dump and restore paths consult
 *   when binding a saved-state image to a destination VF, because
 *   neither @vhca_id (per-PF allocator, unstable across SAVE/LOAD)
 *   nor @vf_id (per-PF slot, may differ source-vs-destination) is
 *   a workload-stable identifier.  See KS7.3 in
 *   tools/testing/mlx5_vfmig/design/vf_prerestore_split.md §3.5
 *   for the full contract.
 *
 *   Intended caller and timing:
 *     - The orchestrator (libvirt / kubevirt / equivalent SR-IOV
 *       provisioning layer) on both the source host (before the
 *       workload binds the VF) and the destination host (before
 *       any LOAD_VHCA_STATE / criu restore step).  The same UUID
 *       is stamped on both sides so the destination-side iterator
 *       in CRIU finds the matching slot.
 *     - CRIU NEVER calls this ioctl.  Both the dump path and the
 *       restore path (including the prerestore binary) only READ
 *       @vf_uuid via MLX5_VFMIG_IOC_QUERY_VF.  The kernel does not
 *       enforce that contract -- root-owned userspace can call
 *       either ioctl from anywhere -- but documenting the split
 *       keeps the ownership model clear.
 *
 *   Lifecycle:
 *     - Initial state on @sriov_numvfs=N: all-zeros (unset).
 *     - SET_VF_UUID(vf_id, U) first call:        @vf_uuid := U.
 *     - SET_VF_UUID(vf_id, U) repeat (same U):   no-op, returns 0.
 *     - SET_VF_UUID(vf_id, V) where V != U && U != 0:
 *         returns -EBUSY; @vf_uuid unchanged.  Defends against the
 *         orchestrator accidentally re-tagging a slot that already
 *         carries a workload's identity.
 *     - sriov_numvfs=0 / PF unload:              all-zeros (slot
 *         torn down).
 *
 *   The "set once until teardown" semantics mean a workflow that
 *   wants to repurpose a vf_id slot for a *different* workload
 *   identity must go through sriov_numvfs=0 -> sriov_numvfs=N
 *   first.  This matches the orchestrator's existing slot-
 *   repurposing flow: the cycle is what already tears down per-
 *   slot resources (cmd ring, EQ buffers, MR-backing pages,
 *   tracked IOVA domain) so the new workload can come up clean.
 *   Idempotent re-stamps with the *same* UUID are explicitly
 *   fine and don't require any teardown.
 *
 *   What this UAPI does NOT promise: a "wipe a bound VHCA's
 *   state and accept a fresh LOAD_VHCA_STATE" primitive. A
 *   second LOAD_VHCA_STATE on the same VHCA without an
 *   sriov_numvfs cycle is structurally blocked by the IOVA
 *   replay layer -- the first LOAD's parser arms drift
 *   detection on the per-VF IOVA domain, and any later
 *   HOST_PAGE replay trips WARN_ON_ONCE(dom->drift_armed) and
 *   returns -EBUSY, surfaced as -EINVAL on the second
 *   MLX5_VFMIG_IOC_LOAD_VHCA_STATE write(). The cycle path
 *   (sriov_numvfs=0 -> sriov_numvfs=N -> SET_VF_UUID -> LOAD)
 *   is the validated route and is what the orchestrator
 *   workflow KS7.3 was designed for; see
 *   tools/testing/mlx5_vfmig/design/vf_prerestore_split.md
 *   §3.5.5.1 for the gate-by-gate empirical table.
 *
 *   Authorization is the cdev FD, same as the rest of the
 *   /dev/mlx5_vfmig cdev family.
 *
 *   Errors:
 *     -EFAULT  copy_{from,to}_user
 *     -EINVAL  @vf_id out of range,
 *              @reserved non-zero, OR
 *              @vf_uuid all-zeros (we treat all-zeros as "unset"
 *              and reject it as a write so userspace can't
 *              accidentally stamp a no-op UUID).
 *     -EBUSY   a different non-zero UUID is already set on
 *              @vf_id (per the lifecycle table above).
 *     -ENODEV  PF is gone.
 */
struct mlx5_vfmig_set_vf_uuid {
	__u32 vf_id;		/* in  */
	__u32 reserved;		/* in: must be 0 */
	__u8  vf_uuid[16];	/* in: orchestrator-supplied 16-byte
				 *     UUID; must not be all-zeros.
				 */
};
#define MLX5_VFMIG_IOC_SET_VF_UUID \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x12, struct mlx5_vfmig_set_vf_uuid)

/*
 * MLX5_VFMIG_IOC_SUSPEND_VHCA:
 *   Quiesce VF @vf_id's datapath by issuing SUSPEND_VHCA(INITIATOR)
 *   followed by SUSPEND_VHCA(RESPONDER) on its vhca_id (PF-issued,
 *   other_function=1). This is the "pause" half of the stop-and-copy
 *   snapshot-ordering fix: CRIU calls it at the early CHECKPOINT_DEVICES
 *   hook, BEFORE the dumpee's memory is copied, so no peer RDMA
 *   WRITE/SEND (and no VF self-DMA) lands in pinned MR pages mid-
 *   snapshot. The heavy state capture stays in the late
 *   MLX5_VFMIG_IOC_SAVE_VHCA_STATE.
 *
 *   Latches priv.sriov.vfs_ctx[vf_id].vfmig_suspended. Idempotent:
 *   returns 0 with no firmware traffic if the VF is already suspended.
 *   The VF may be bound or unbound (SUSPEND_VHCA is an other_function
 *   command issued by the PF). Requires the migratable cap, same gate
 *   as SAVE.
 *
 *   Relationship to SAVE_VHCA_STATE: if the VF is already suspended via
 *   this ioctl, a subsequent SAVE skips its in-SAVE SUSPEND pair and
 *   does NOT auto-resume on save_fd close -- the caller owns the resume
 *   via MLX5_VFMIG_IOC_RESUME_VHCA. If SAVE is used standalone (no prior
 *   SUSPEND), it self-suspends and resumes on close as before.
 *
 *   Returns 0 on success; -EINVAL if @vf_id is out of range or @flags
 *   is non-zero; -EOPNOTSUPP if the VF is not migration-enabled;
 *   -ENODEV if the PF is gone; any negative firmware-error code if a
 *   SUSPEND step fails (a failed INITIATOR suspend is not "undone").
 */
struct mlx5_vfmig_suspend_vhca {
	__u32 vf_id;	/* in  */
	__u32 flags;	/* in: must be 0 */
	__u32 reserved[2];
};
#define MLX5_VFMIG_IOC_SUSPEND_VHCA \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x13, struct mlx5_vfmig_suspend_vhca)

/*
 * MLX5_VFMIG_IOC_RESUME_VHCA:
 *   Un-quiesce VF @vf_id's datapath by issuing RESUME_VHCA(RESPONDER)
 *   followed by RESUME_VHCA(INITIATOR) (reverse order of suspend). This
 *   is the "resume" half of the snapshot-ordering fix:
 *     - on the source after an aborted/rolled-back dump, to bring the
 *       VF back to runnable;
 *     - on the destination at RESUME_DEVICES_LATE, after a
 *       MARK_RESTORED { DEFER_RESUME } + bind has applied
 *       LOAD_VHCA_STATE and left the VHCA parked, once all MR/ring VMAs
 *       are restored.
 *
 *   Clears priv.sriov.vfs_ctx[vf_id].vfmig_suspended and
 *   @vfmig_defer_resume. Idempotent: returns 0 with no firmware traffic
 *   if the VF is not currently suspended.
 *
 *   Returns 0 on success; -EINVAL if @vf_id is out of range or @flags
 *   is non-zero; -ENODEV if the PF is gone; any negative firmware-error
 *   code if a RESUME step fails.
 */
struct mlx5_vfmig_resume_vhca {
	__u32 vf_id;	/* in  */
	__u32 flags;	/* in: must be 0 */
	__u32 reserved[2];
};
#define MLX5_VFMIG_IOC_RESUME_VHCA \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x14, struct mlx5_vfmig_resume_vhca)

#endif /* _UAPI_LINUX_MLX5_VFMIG_H */
