/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR Linux-OpenIB) */
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

#ifndef MLX5_ABI_USER_H
#define MLX5_ABI_USER_H

#include <linux/types.h>
#include <linux/if_ether.h>	/* For ETH_ALEN. */
#include <rdma/ib_user_ioctl_verbs.h>
#include <rdma/mlx5_user_ioctl_verbs.h>

enum {
	MLX5_QP_FLAG_SIGNATURE		= 1 << 0,
	MLX5_QP_FLAG_SCATTER_CQE	= 1 << 1,
	MLX5_QP_FLAG_TUNNEL_OFFLOADS	= 1 << 2,
	MLX5_QP_FLAG_BFREG_INDEX	= 1 << 3,
	MLX5_QP_FLAG_TYPE_DCT		= 1 << 4,
	MLX5_QP_FLAG_TYPE_DCI		= 1 << 5,
	MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_UC = 1 << 6,
	MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_MC = 1 << 7,
	MLX5_QP_FLAG_ALLOW_SCATTER_CQE	= 1 << 8,
	MLX5_QP_FLAG_PACKET_BASED_CREDIT_MODE	= 1 << 9,
	MLX5_QP_FLAG_UAR_PAGE_INDEX = 1 << 10,
	MLX5_QP_FLAG_DCI_STREAM	= 1 << 11,
};

enum {
	MLX5_SRQ_FLAG_SIGNATURE		= 1 << 0,
};

enum {
	MLX5_WQ_FLAG_SIGNATURE		= 1 << 0,
};

/* Increment this value if any changes that break userspace ABI
 * compatibility are made.
 */
#define MLX5_IB_UVERBS_ABI_VERSION	1

/* Make sure that all structs defined in this file remain laid out so
 * that they pack the same way on 32-bit and 64-bit architectures (to
 * avoid incompatibility between 32-bit userspace and 64-bit kernels).
 * In particular do not use pointer types -- pass pointers in __u64
 * instead.
 */

struct mlx5_ib_alloc_ucontext_req {
	__u32	total_num_bfregs;
	__u32	num_low_latency_bfregs;
};

enum mlx5_lib_caps {
	MLX5_LIB_CAP_4K_UAR	= (__u64)1 << 0,
	MLX5_LIB_CAP_DYN_UAR	= (__u64)1 << 1,
};

enum mlx5_ib_alloc_uctx_v2_flags {
	MLX5_IB_ALLOC_UCTX_DEVX			= 1 << 0,
	/*
	 * VFMIG ucontext restore: skip per-slot ALLOC_UAR in
	 * mlx5_ib_alloc_ucontext()'s allocate_uars(); leave
	 * bfregi->sys_pages[] sentinel-filled (MLX5_IB_INVALID_UAR_INDEX)
	 * pending a follow-up MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT call
	 * that seeds the table from the source process's snapshot.
	 *
	 * Any UAR mmap() between alloc and restore is rejected by
	 * uar_mmap()'s existing INVALID-slot check.
	 *
	 * See tools/testing/criu_rdma/design/uar_restore.md.
	 */
	MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE	= 1 << 1,
	/*
	 * Bound to the SR-IOV-VFMIG CRIU restore pipeline. When set
	 * together with MLX5_IB_ALLOC_UCTX_DEVX and
	 * MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE, the kernel skips
	 * mlx5_ib_devx_create() and assigns
	 *   context->devx_uid = mlx5_ib_alloc_ucontext_req_v2::adopt_devx_uid
	 * directly. adopt_devx_uid is mandatory (must be non-zero) when
	 * this flag is set, and forbidden (must be zero) when not set.
	 *
	 * VESTIGIAL FOR v0 -- DO NOT SET FROM USERSPACE. The original
	 * design premise was that LOAD_VHCA_STATE preserves the source
	 * ucontext's FW registration (uid -> uctx_attrs) so the
	 * destination could adopt the same uid and have all
	 * source-allocated FW resources (PDs/CQs/QPs/MKEYs whose
	 * owning_uid was that ucontext) remain usable. Empirical
	 * matrix from
	 * tools/testing/criu_rdma/uobject_restore/pd_adopt/test_pd_adopt.sh
	 * (DEVX-source variant, FW 28.48.1000) falsifies this: LOAD
	 * preserves the FW next_free_uctx counter but the
	 * uctx-registration table itself is wiped, so the adopted uid
	 * is unregistered post-LOAD and any subsequent
	 * CREATE_MKEY(pdn, uid=adopted) rejects with a consistent
	 * "unknown uid" syndrome. Re-issuing CREATE_UCTX on the dest
	 * yields a different uid that still cannot bind to the source's
	 * (pdn, owning_uid) FW records.
	 *
	 * The v0 mitigation is to open the destination ucontext WITHOUT
	 * this flag (and without MLX5_IB_ALLOC_UCTX_DEVX), leaving
	 * context->devx_uid = 0. All adopted PD/MR/CQ/QP records then
	 * live under uid=0 (host-privileged), which is the FW-ungated
	 * lane proven by the P_zero/N_zero cells of the matrix.
	 * Restored processes lose DEVX features (mlx5dv_*,
	 * devx_obj_create, ...); basic verbs work.
	 *
	 * The flag, the adopt_devx_uid field, and the kernel-side
	 * handler stay in tree as forward-compat for a future FW
	 * capability that preserves the uctx registry across
	 * LOAD_VHCA_STATE. Until then this path is reachable only via
	 * a future probe binary that sets the bit explicitly.
	 * See tools/testing/criu_rdma/design/uobject_restore.md §9.1
	 * S3b "DEVX-adoption blind spot" for the full empirical chain.
	 */
	MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID	= 1 << 2,
};
struct mlx5_ib_alloc_ucontext_req_v2 {
	__u32	total_num_bfregs;
	__u32	num_low_latency_bfregs;
	__u32	flags;
	__u32	comp_mask;
	__u8	max_cqe_version;
	__u8	reserved0;
	__u16	reserved1;
	__u32	reserved2;
	__aligned_u64 lib_caps;
	/*
	 * SR-IOV-VFMIG CRIU restore: the source ucontext's devx_uid to
	 * adopt on the destination ucontext. Only honoured when both
	 * MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID and
	 * MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE and MLX5_IB_ALLOC_UCTX_DEVX
	 * are set in @flags. Must be zero otherwise. See the
	 * MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID flag comment for semantics.
	 *
	 * Backward compat: this field is appended after lib_caps, so an
	 * older userspace that sends the pre-extension v2 struct
	 * (ending at lib_caps) is unaffected -- the kernel's
	 * ib_copy_from_udata sees udata->inlen < sizeof(req) and zeros
	 * the tail, leaving adopt_devx_uid implicitly 0.
	 */
	__u32	adopt_devx_uid;
	__u32	reserved3;
};

enum mlx5_ib_alloc_ucontext_resp_mask {
	MLX5_IB_ALLOC_UCONTEXT_RESP_MASK_CORE_CLOCK_OFFSET = 1UL << 0,
	MLX5_IB_ALLOC_UCONTEXT_RESP_MASK_DUMP_FILL_MKEY    = 1UL << 1,
	MLX5_IB_ALLOC_UCONTEXT_RESP_MASK_ECE               = 1UL << 2,
	MLX5_IB_ALLOC_UCONTEXT_RESP_MASK_SQD2RTS           = 1UL << 3,
	MLX5_IB_ALLOC_UCONTEXT_RESP_MASK_REAL_TIME_TS	   = 1UL << 4,
	MLX5_IB_ALLOC_UCONTEXT_RESP_MASK_MKEY_UPDATE_TAG   = 1UL << 5,
};

enum mlx5_user_cmds_supp_uhw {
	MLX5_USER_CMDS_SUPP_UHW_QUERY_DEVICE = 1 << 0,
	MLX5_USER_CMDS_SUPP_UHW_CREATE_AH    = 1 << 1,
};

/* The eth_min_inline response value is set to off-by-one vs the FW
 * returned value to allow user-space to deal with older kernels.
 */
enum mlx5_user_inline_mode {
	MLX5_USER_INLINE_MODE_NA,
	MLX5_USER_INLINE_MODE_NONE,
	MLX5_USER_INLINE_MODE_L2,
	MLX5_USER_INLINE_MODE_IP,
	MLX5_USER_INLINE_MODE_TCP_UDP,
};

enum {
	MLX5_USER_ALLOC_UCONTEXT_FLOW_ACTION_FLAGS_ESP_AES_GCM = 1 << 0,
	MLX5_USER_ALLOC_UCONTEXT_FLOW_ACTION_FLAGS_ESP_AES_GCM_REQ_METADATA = 1 << 1,
	MLX5_USER_ALLOC_UCONTEXT_FLOW_ACTION_FLAGS_ESP_AES_GCM_SPI_STEERING = 1 << 2,
	MLX5_USER_ALLOC_UCONTEXT_FLOW_ACTION_FLAGS_ESP_AES_GCM_FULL_OFFLOAD = 1 << 3,
	MLX5_USER_ALLOC_UCONTEXT_FLOW_ACTION_FLAGS_ESP_AES_GCM_TX_IV_IS_ESN = 1 << 4,
};

struct mlx5_ib_alloc_ucontext_resp {
	__u32	qp_tab_size;
	__u32	bf_reg_size;
	__u32	tot_bfregs;
	__u32	cache_line_size;
	__u16	max_sq_desc_sz;
	__u16	max_rq_desc_sz;
	__u32	max_send_wqebb;
	__u32	max_recv_wr;
	__u32	max_srq_recv_wr;
	__u16	num_ports;
	__u16	flow_action_flags;
	__u32	comp_mask;
	__u32	response_length;
	__u8	cqe_version;
	__u8	cmds_supp_uhw;
	__u8	eth_min_inline;
	__u8	clock_info_versions;
	__aligned_u64 hca_core_clock_offset;
	__u32	log_uar_size;
	__u32	num_uars_per_page;
	__u32	num_dyn_bfregs;
	__u32	dump_fill_mkey;
};

struct mlx5_ib_alloc_pd_resp {
	__u32	pdn;
};

/*
 * Driver-private UHW payload for UVERBS_METHOD_RESTORE_PD on
 * mlx5. CRIU-managed restore passes the source's FW pdn here so
 * mlx5_ib_restore_pd can adopt it into a fresh kernel-side
 * mlx5_ib_pd without re-issuing FW ALLOC_PD. The (independent)
 * ufile target handle is carried by the core
 * UVERBS_ATTR_RESTORE_PD_HANDLE attribute on the verb.
 *
 * The adopted pdn must come from the source's pre-SAVE state and
 * is expected to still be reserved in firmware on the
 * destination VF after LOAD_VHCA_STATE. See
 * tools/testing/criu_rdma/uobject_restore/fw_id_continuity/ for
 * the K6 evidence and uobject_restore/pd_adopt/ for the
 * empirical validation of the no-FW-round-trip adoption model.
 *
 * NOTE on size (>8 bytes): the uverbs UHW dispatch path treats a
 * UHW_IN payload with len <= sizeof(u64) as INLINE -- it stuffs
 * the attr->data u64 into a kernel-side staging slot and sets
 * udata->inbuf to a kernel pointer. ib_copy_from_udata() then
 * calls copy_from_user() on that kernel pointer, which on x86_64
 * with masked-user-access support clamps the address to
 * USER_PTR_MAX and zero-fills the destination. We deliberately
 * size this struct above that threshold (two u64-equivalent
 * payload + reserved bytes) so the dispatcher takes the ptr path
 * unconditionally: udata->inbuf becomes a real userspace pointer
 * and ib_copy_from_udata() works as expected. The extra reserved
 * bytes also leave room for future DEVX-uid hints and other
 * forward-compat flags without growing the struct again.
 */
struct mlx5_ib_restore_pd_req {
	__u32	pdn;		/* FW pdn to adopt (24 bits significant) */
	__u32	reserved;	/* must be 0 */
	__aligned_u64 reserved2; /* must be 0; pads above inline-UHW
				  * threshold and reserves room for
				  * future DEVX-uid / flags. */
};

/*
 * Driver-private UHW payload for UVERBS_METHOD_RESTORE_MR on
 * mlx5. CRIU-managed restore passes the source's FW mkey_index
 * here so mlx5_ib_restore_mr can adopt the existing destination-
 * side mkey (preserved across LOAD_VHCA_STATE) into a fresh
 * kernel-side mlx5_ib_mr without re-issuing FW CREATE_MKEY.
 *
 * The wire-visible identity of the MR is carried by the core
 * UVERBS_ATTR_RESTORE_MR_LKEY_HINT / _RKEY_HINT attributes on
 * the verb (mlx5 invariant: lkey == rkey == mkey_index << 8 |
 * variant_byte). mkey_index in this UHW must match
 * (lkey_hint >> 8) == (rkey_hint >> 8); the handler enforces.
 * Carrying mkey_index explicitly here -- even though the driver
 * could derive it from the core attrs -- is defense-in-depth:
 * a CRIU bug that ships the source's restrack id instead of the
 * FW mkey_index will be caught at the handler boundary, exactly
 * where the parallel (pdn vs restrack id) bug is caught for
 * RESTORE_PD.
 *
 * The adopted mkey_index must come from the source's pre-SAVE
 * state and is expected to still be reserved in firmware on the
 * destination VF after LOAD_VHCA_STATE. The empirical chain is:
 *   - tools/testing/criu_rdma/uobject_restore/fw_id_continuity/
 *     (K6 -- mkey allocator high-water survives LOAD);
 *   - tools/testing/criu_rdma/uobject_restore/mr_adopt/
 *     (S4b -- QUERY_MKEY confirms src_mkey_index alive on dest
 *     post-LOAD with mkc.{pd, start_addr, len} byte-equal to the
 *     source pre-SAVE view).
 *
 * NOTE on size (>8 bytes): same inline-UHW dodge as
 * mlx5_ib_restore_pd_req. Two u64-equivalent payload + reserved
 * bytes ensure the dispatcher always takes the userspace-pointer
 * path; ib_copy_from_udata() then works as expected on x86_64
 * with masked-user-access support. The extra reserved bytes
 * also reserve room for future per-MR DEVX-uid hints and other
 * forward-compat flags without growing the struct.
 */
struct mlx5_ib_restore_mr_req {
	__u32	mkey_index;	/* FW mkey index to adopt
				 * (24 bits significant). */
	__u32	reserved;	/* must be 0 */
	__aligned_u64 reserved2; /* must be 0; pads above inline-UHW
				  * threshold and reserves room for
				  * future DEVX-uid / flags. */
};

/*
 * Driver-private UHW payload for UVERBS_METHOD_RESTORE_CQ on
 * mlx5. CRIU-managed restore passes the source's FW cqn here so
 * mlx5_ib_restore_cq can adopt the existing destination-side CQ
 * (preserved across LOAD_VHCA_STATE) into a fresh kernel-side
 * mlx5_ib_cq without re-issuing FW CREATE_CQ.
 *
 * The wire-visible identity of the CQ is carried by @cqn -- there
 * is no core "cqn_hint" attribute on the verb (CQs do not have
 * lkey/rkey-style core hints; cqn is a strictly mlx5-private
 * concept that travels only through this UHW). The handler
 * enforces @cqn != 0 and @cqn fits in 24 bits (FW resource id
 * range), and rejects 0 as a documented sentinel.
 *
 * @cqe_size, @buf_addr, @db_addr capture the source-side
 * userspace state that mlx5_ib_create_cq's create_cq_user normally
 * derives from struct mlx5_ib_create_cq.{cqe_size,buf_addr,db_addr}
 * at fresh-CQ time. We carry them via the restore UHW because they
 * are NOT representable in the core RESTORE_CQ attrs (which only
 * carry cqe / comp_vector / flags), and the kernel needs them to:
 *
 *   - parse the destination-side CQE ring at poll time
 *     (cqe_size: 64 or 128)
 *   - bind the CQE ring umem onto the destination-side
 *     KIND_CQ-tagged placeholder LOAD_VHCA_STATE installed
 *     (buf_addr: source userspace VA; the wider mlx5_ib_cq
 *     adopts the source's IOVA via the placeholder)
 *   - bind the doorbell-page umem onto the destination-side
 *     KIND_DBR-tagged placeholder LOAD_VHCA_STATE installed
 *     (db_addr: source userspace VA, page-aligned by handler)
 *
 * The adopted cqn must come from the source's pre-SAVE state and
 * is expected to still be reserved in firmware on the destination
 * VF after LOAD_VHCA_STATE. The empirical chain is:
 *   - tools/testing/criu_rdma/uobject_restore/fw_id_continuity/
 *     (K6 -- cqn allocator high-water survives LOAD);
 *   - tools/testing/criu_rdma/uobject_restore/cq_adopt/
 *     (S5b B0 -- QUERY_CQ confirms src_cqn alive on dest post-LOAD
 *     with cqc.{eqn, log_cq_size, log_page_size, page_offset,
 *     status, oi} byte-equal to the source pre-SAVE view; STRONG
 *     PASS on FW 28.48.1000).
 *
 * NOTE on size (32 bytes): well above the 8-byte inline-UHW
 * threshold, so the dispatcher always takes the userspace-pointer
 * path and ib_copy_from_udata() works as expected on x86_64 with
 * masked-user-access. Same dodge as mlx5_ib_restore_pd_req /
 * mlx5_ib_restore_mr_req. The reserved[] tail reserves room for
 * future per-CQ DEVX-uid hints, CQE compression layout, and
 * flags without growing the struct.
 */
struct mlx5_ib_restore_cq_req {
	__aligned_u64 buf_addr;	/* source userspace VA of CQE ring buffer
				 * -- bound on the destination via
				 * mlx5_ib_umem_restore_cq into the
				 * KIND_CQ-tagged placeholder
				 * LOAD_VHCA_STATE installed. */
	__aligned_u64 db_addr;	/* source userspace VA of doorbell page
				 * -- bound on the destination via
				 * mlx5_ib_db_map_user_restore into the
				 * KIND_DBR-tagged placeholder. Handler
				 * page-aligns @db_addr before binding;
				 * the offset within the page survives
				 * verbatim because cqc.dbr_addr (FW)
				 * already encodes it. */
	__u32	cqn;		/* FW cqn to adopt (24 bits significant);
				 * 0 is reserved as a sentinel. */
	__u32	cqe_size;	/* 64 or 128. Must match the source
				 * mkx5_ib_create_cq.cqe_size used at
				 * source-side CREATE_CQ; mismatched values
				 * (or any value other than 64/128) reject
				 * with -EINVAL. */
	__u32	reserved;	/* must be 0 */
	__u32	reserved2;	/* must be 0; reserves a 32-bit slot
				 * for future per-CQ flags (CQE_128_PAD
				 * adoption, REAL_TIME_TS adoption,
				 * cqe_comp_en adoption). */
};

/*
 * Driver-private UHW payload for UVERBS_METHOD_RESTORE_QP on
 * mlx5. CRIU-managed restore passes the source's FW qpn here so
 * mlx5_ib_restore_qp can adopt the existing destination-side QP
 * (preserved across LOAD_VHCA_STATE) into a fresh kernel-side
 * mlx5_ib_qp without re-issuing FW CREATE_QP.
 *
 * The wire-visible identity of the QP is carried by @qpn -- there
 * is no core "qpn_hint" attribute on the verb (QPs do not have
 * lkey/rkey-style core hints; qpn is a strictly mlx5-private
 * concept that travels only through this UHW). The handler
 * enforces @qpn != 0 (kernel-mode reservation sentinel) and @qpn
 * fits in 24 bits (FW resource id range), and rejects 0 as a
 * documented sentinel.
 *
 * Empirical justification (the bulk of what the handler would
 * otherwise need to replay sits inside the FW QPC and is preserved
 * by LOAD_VHCA_STATE): test_fw_id_continuity.sh
 * K6_QP_STATE={INIT,RTR,RTS} self-loopback STRONG PASS on
 * 2026-06-01 -- the entire FW-side QPC subset round-trips byte-
 * equal across SAVE/LOAD, including state, pd, q_key, uar_page,
 * log_{page,sq,rq}_size, log_msg_max, user_index, cqn_snd,
 * cqn_rcv, srqn_rmpn_xrqn, hw/sw {sq_wqebb,rq}_counter, the PSNs
 * (next_send_psn, next_rcv_psn, last_acked_psn), the RTR-set
 * (path_mtu, min_rnr_nak, log_rra_max, primary_address_path:
 * dgid, sgid_index, dlid/mlid, sl, port, dmac, hop_limit, tclass,
 * flow_label, udp_sport, ack_timeout, eth_prio), and the RTS-set
 * (log_sra_max, retry_count, rnr_retry). RESTORE_QP therefore
 * does NOT carry path_mtu / retry_count / AV / PSNs / etc.
 * through the UHW -- FW preserves them. The UHW only needs the
 * userspace-side identity the FW QPC cannot re-derive.
 *
 * Field roles -- mirrors mlx5_ib_create_qp's role-by-role:
 *
 *   @buf_addr     source userspace VA of the WQ ring buffer (SQ +
 *                 RQ combined for RC/UC/UD; SQ-only for raw_packet
 *                 with @sq_buf_addr split). Same value
 *                 mlx5_ib_create_qp.buf_addr held at fresh-create
 *                 time on the source. Bound on the destination via
 *                 mlx5_ib_umem_restore_qp (S6b B3) into the
 *                 KIND_QP-tagged placeholder LOAD_VHCA_STATE
 *                 installed via the Stage-2 source-side retag.
 *
 *   @db_addr      source userspace VA of the doorbell record (DBR
 *                 page). Same as mlx5_ib_create_qp.db_addr. Bound
 *                 via mlx5_ib_db_map_user_restore against the
 *                 KIND_DBR placeholder, page-aligned by the
 *                 handler exactly like mlx5_ib_restore_cq's
 *                 db_addr. The byte offset within the page
 *                 survives verbatim because qpc.dbr_addr (FW)
 *                 encodes it.
 *
 *   @sq_buf_addr  split-SQ source userspace VA, RAW_PACKET-only.
 *                 For v0 (RC scope) this MUST be 0; the handler
 *                 rejects any non-zero value with -EOPNOTSUPP.
 *                 Carried in the v0 UAPI to match the
 *                 mlx5_ib_create_qp shape and avoid a UAPI bump
 *                 when raw_packet support lands later.
 *
 *   @qpn          FW qpn to adopt (24 bits significant). 0 is a
 *                 documented sentinel (kernel-mode QP reservation)
 *                 and rejects with -EINVAL.
 *
 *   @sq_wqe_count, @rq_wqe_count, @rq_wqe_shift
 *                 the queue sizing the source used at fresh-create
 *                 time. Same as in mlx5_ib_create_qp; the adopt
 *                 path uses these to size the umem pin and to
 *                 stamp mlx5_ib_qp.{sq,rq}.wqe_cnt. Independently
 *                 cross-checked against qpc.log_{sq,rq}_size which
 *                 K7 has shown survives byte-equal -- if the
 *                 caller's count disagrees with FW, the handler
 *                 rejects with -EINVAL.
 *
 *   @flags        create-time MLX5_QP_FLAG_* bitmask. Captured at
 *                 source SAVE and replayed verbatim. v0 policy: if
 *                 the dst kernel does not recognise a flag bit
 *                 (newer-source-than-dst), reject loud with
 *                 -EOPNOTSUPP rather than silently zero.
 *
 *   @uidx         user_index for WC routing (qpc.user_index).
 *                 Captured by the dumper from the source-side
 *                 mlx5_ib_qp.uidx; replayed here so the
 *                 destination's mlx5_ib_qp.uidx matches. K7 has
 *                 shown qpc.user_index survives byte-equal at the
 *                 FW level; this UHW field is what keeps the
 *                 *kernel*-side mlx5_ib_qp.uidx aligned with FW.
 *
 *   @bfreg_index  source BFREG slot index in the source ucontext's
 *                 bfregs[] table. The source-side
 *                 mlx5_ib_create_qp_user resolves bfreg_index to a
 *                 FW UAR id; the destination kernel (after the
 *                 ucontext was opened with
 *                 MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE and the
 *                 RESTORE_DYN_UARS records replayed) does the
 *                 inverse lookup and validates the resolved FW UAR
 *                 id against qpc.uar_page (which K7 confirms is
 *                 byte-equal across LOAD).
 *
 *   @ece_options  ECE word the source negotiated. Same semantics
 *                 as mlx5_ib_create_qp.ece_options. 0 on RESET
 *                 state QPs. The handler stamps mlx5_ib_qp.ece.
 *
 * NOTE on size (64 bytes): well above the 8-byte inline-UHW
 * threshold so the dispatcher always takes the userspace-pointer
 * path and ib_copy_from_udata() works as expected on x86_64 with
 * masked-user-access. Same dodge as mlx5_ib_restore_cq_req. The
 * reserved[] tail reserves slots for future per-QP DEVX-uid hints,
 * DCI-stream adoption, raw_packet TIRN/TISN adoption, scatter-CQE
 * adoption, and TYPE_DCT-specific fields without growing the
 * struct.
 */
struct mlx5_ib_restore_qp_req {
	__aligned_u64 buf_addr;		/* source userspace VA of WQ ring */
	__aligned_u64 db_addr;		/* source userspace VA of DBR page */
	__aligned_u64 sq_buf_addr;	/* raw_packet split-SQ; 0 for v0 RC */
	__u32	qpn;			/* FW qpn to adopt (24 bits) */
	__u32	sq_wqe_count;		/* same as mlx5_ib_create_qp */
	__u32	rq_wqe_count;
	__u32	rq_wqe_shift;
	__u32	flags;			/* MLX5_QP_FLAG_* bitmask */
	__u32	uidx;			/* qpc.user_index (24 bits) */
	__u32	bfreg_index;		/* source BFREG slot for SQ doorbell */
	__u32	ece_options;
	__u32	reserved;		/* must be 0 */
	__u32	reserved2;		/* must be 0; reserves a 32-bit slot
					 * for future per-QP flags (DCI-stream
					 * adoption, scatter-CQE adoption,
					 * tunnel-offload adoption). */
};

struct mlx5_ib_tso_caps {
	__u32 max_tso; /* Maximum tso payload size in bytes */

	/* Corresponding bit will be set if qp type from
	 * 'enum ib_qp_type' is supported, e.g.
	 * supported_qpts |= 1 << IB_QPT_UD
	 */
	__u32 supported_qpts;
};

struct mlx5_ib_rss_caps {
	__aligned_u64 rx_hash_fields_mask; /* enum mlx5_rx_hash_fields */
	__u8 rx_hash_function; /* enum mlx5_rx_hash_function_flags */
	__u8 reserved[7];
};

enum mlx5_ib_cqe_comp_res_format {
	MLX5_IB_CQE_RES_FORMAT_HASH	= 1 << 0,
	MLX5_IB_CQE_RES_FORMAT_CSUM	= 1 << 1,
	MLX5_IB_CQE_RES_FORMAT_CSUM_STRIDX = 1 << 2,
};

struct mlx5_ib_cqe_comp_caps {
	__u32 max_num;
	__u32 supported_format; /* enum mlx5_ib_cqe_comp_res_format */
};

enum mlx5_ib_packet_pacing_cap_flags {
	MLX5_IB_PP_SUPPORT_BURST	= 1 << 0,
};

struct mlx5_packet_pacing_caps {
	__u32 qp_rate_limit_min;
	__u32 qp_rate_limit_max; /* In kpbs */

	/* Corresponding bit will be set if qp type from
	 * 'enum ib_qp_type' is supported, e.g.
	 * supported_qpts |= 1 << IB_QPT_RAW_PACKET
	 */
	__u32 supported_qpts;
	__u8  cap_flags; /* enum mlx5_ib_packet_pacing_cap_flags */
	__u8  reserved[3];
};

enum mlx5_ib_mpw_caps {
	MPW_RESERVED		= 1 << 0,
	MLX5_IB_ALLOW_MPW	= 1 << 1,
	MLX5_IB_SUPPORT_EMPW	= 1 << 2,
};

enum mlx5_ib_sw_parsing_offloads {
	MLX5_IB_SW_PARSING = 1 << 0,
	MLX5_IB_SW_PARSING_CSUM = 1 << 1,
	MLX5_IB_SW_PARSING_LSO = 1 << 2,
};

struct mlx5_ib_sw_parsing_caps {
	__u32 sw_parsing_offloads; /* enum mlx5_ib_sw_parsing_offloads */

	/* Corresponding bit will be set if qp type from
	 * 'enum ib_qp_type' is supported, e.g.
	 * supported_qpts |= 1 << IB_QPT_RAW_PACKET
	 */
	__u32 supported_qpts;
};

struct mlx5_ib_striding_rq_caps {
	__u32 min_single_stride_log_num_of_bytes;
	__u32 max_single_stride_log_num_of_bytes;
	__u32 min_single_wqe_log_num_of_strides;
	__u32 max_single_wqe_log_num_of_strides;

	/* Corresponding bit will be set if qp type from
	 * 'enum ib_qp_type' is supported, e.g.
	 * supported_qpts |= 1 << IB_QPT_RAW_PACKET
	 */
	__u32 supported_qpts;
	__u32 reserved;
};

struct mlx5_ib_dci_streams_caps {
	__u8 max_log_num_concurent;
	__u8 max_log_num_errored;
};

enum mlx5_ib_query_dev_resp_flags {
	/* Support 128B CQE compression */
	MLX5_IB_QUERY_DEV_RESP_FLAGS_CQE_128B_COMP = 1 << 0,
	MLX5_IB_QUERY_DEV_RESP_FLAGS_CQE_128B_PAD  = 1 << 1,
	MLX5_IB_QUERY_DEV_RESP_PACKET_BASED_CREDIT_MODE = 1 << 2,
	MLX5_IB_QUERY_DEV_RESP_FLAGS_SCAT2CQE_DCT = 1 << 3,
	MLX5_IB_QUERY_DEV_RESP_FLAGS_OOO_DP = 1 << 4,
};

enum mlx5_ib_tunnel_offloads {
	MLX5_IB_TUNNELED_OFFLOADS_VXLAN  = 1 << 0,
	MLX5_IB_TUNNELED_OFFLOADS_GRE    = 1 << 1,
	MLX5_IB_TUNNELED_OFFLOADS_GENEVE = 1 << 2,
	MLX5_IB_TUNNELED_OFFLOADS_MPLS_GRE = 1 << 3,
	MLX5_IB_TUNNELED_OFFLOADS_MPLS_UDP = 1 << 4,
};

struct mlx5_ib_query_device_resp {
	__u32	comp_mask;
	__u32	response_length;
	struct	mlx5_ib_tso_caps tso_caps;
	struct	mlx5_ib_rss_caps rss_caps;
	struct	mlx5_ib_cqe_comp_caps cqe_comp_caps;
	struct	mlx5_packet_pacing_caps packet_pacing_caps;
	__u32	mlx5_ib_support_multi_pkt_send_wqes;
	__u32	flags; /* Use enum mlx5_ib_query_dev_resp_flags */
	struct mlx5_ib_sw_parsing_caps sw_parsing_caps;
	struct mlx5_ib_striding_rq_caps striding_rq_caps;
	__u32	tunnel_offloads_caps; /* enum mlx5_ib_tunnel_offloads */
	struct  mlx5_ib_dci_streams_caps dci_streams_caps;
	__u16 reserved;
	struct mlx5_ib_uapi_reg reg_c0;
};

enum mlx5_ib_create_cq_flags {
	MLX5_IB_CREATE_CQ_FLAGS_CQE_128B_PAD	= 1 << 0,
	MLX5_IB_CREATE_CQ_FLAGS_UAR_PAGE_INDEX  = 1 << 1,
	MLX5_IB_CREATE_CQ_FLAGS_REAL_TIME_TS	= 1 << 2,
};

struct mlx5_ib_create_cq {
	__aligned_u64 buf_addr;
	__aligned_u64 db_addr;
	__u32	cqe_size;
	__u8    cqe_comp_en;
	__u8    cqe_comp_res_format;
	__u16	flags;
	__u16	uar_page_index;
	__u16	reserved0;
	__u32	reserved1;
};

struct mlx5_ib_create_cq_resp {
	__u32	cqn;
	__u32	reserved;
};

struct mlx5_ib_resize_cq {
	__aligned_u64 buf_addr;
	__u16	cqe_size;
	__u16	reserved0;
	__u32	reserved1;
};

struct mlx5_ib_create_srq {
	__aligned_u64 buf_addr;
	__aligned_u64 db_addr;
	__u32	flags;
	__u32	reserved0; /* explicit padding (optional on i386) */
	__u32	uidx;
	__u32	reserved1;
};

struct mlx5_ib_create_srq_resp {
	__u32	srqn;
	__u32	reserved;
};

struct mlx5_ib_create_qp_dci_streams {
	__u8 log_num_concurent;
	__u8 log_num_errored;
};

struct mlx5_ib_create_qp {
	__aligned_u64 buf_addr;
	__aligned_u64 db_addr;
	__u32	sq_wqe_count;
	__u32	rq_wqe_count;
	__u32	rq_wqe_shift;
	__u32	flags;
	__u32	uidx;
	__u32	bfreg_index;
	union {
		__aligned_u64 sq_buf_addr;
		__aligned_u64 access_key;
	};
	__u32  ece_options;
	struct  mlx5_ib_create_qp_dci_streams dci_streams;
	__u16 reserved;
};

/* RX Hash function flags */
enum mlx5_rx_hash_function_flags {
	MLX5_RX_HASH_FUNC_TOEPLITZ	= 1 << 0,
};

/*
 * RX Hash flags, these flags allows to set which incoming packet's field should
 * participates in RX Hash. Each flag represent certain packet's field,
 * when the flag is set the field that is represented by the flag will
 * participate in RX Hash calculation.
 * Note: *IPV4 and *IPV6 flags can't be enabled together on the same QP
 * and *TCP and *UDP flags can't be enabled together on the same QP.
*/
enum mlx5_rx_hash_fields {
	MLX5_RX_HASH_SRC_IPV4	= 1 << 0,
	MLX5_RX_HASH_DST_IPV4	= 1 << 1,
	MLX5_RX_HASH_SRC_IPV6	= 1 << 2,
	MLX5_RX_HASH_DST_IPV6	= 1 << 3,
	MLX5_RX_HASH_SRC_PORT_TCP	= 1 << 4,
	MLX5_RX_HASH_DST_PORT_TCP	= 1 << 5,
	MLX5_RX_HASH_SRC_PORT_UDP	= 1 << 6,
	MLX5_RX_HASH_DST_PORT_UDP	= 1 << 7,
	MLX5_RX_HASH_IPSEC_SPI		= 1 << 8,
	/* Save bits for future fields */
	MLX5_RX_HASH_INNER		= (1UL << 31),
};

struct mlx5_ib_create_qp_rss {
	__aligned_u64 rx_hash_fields_mask; /* enum mlx5_rx_hash_fields */
	__u8 rx_hash_function; /* enum mlx5_rx_hash_function_flags */
	__u8 rx_key_len; /* valid only for Toeplitz */
	__u8 reserved[6];
	__u8 rx_hash_key[128]; /* valid only for Toeplitz */
	__u32   comp_mask;
	__u32	flags;
};

enum mlx5_ib_create_qp_resp_mask {
	MLX5_IB_CREATE_QP_RESP_MASK_TIRN = 1UL << 0,
	MLX5_IB_CREATE_QP_RESP_MASK_TISN = 1UL << 1,
	MLX5_IB_CREATE_QP_RESP_MASK_RQN  = 1UL << 2,
	MLX5_IB_CREATE_QP_RESP_MASK_SQN  = 1UL << 3,
	MLX5_IB_CREATE_QP_RESP_MASK_TIR_ICM_ADDR  = 1UL << 4,
};

struct mlx5_ib_create_qp_resp {
	__u32	bfreg_index;
	__u32   ece_options;
	__u32	comp_mask;
	__u32	tirn;
	__u32	tisn;
	__u32	rqn;
	__u32	sqn;
	__u32   reserved1;
	__u64	tir_icm_addr;
};

struct mlx5_ib_alloc_mw {
	__u32	comp_mask;
	__u8	num_klms;
	__u8	reserved1;
	__u16	reserved2;
};

enum mlx5_ib_create_wq_mask {
	MLX5_IB_CREATE_WQ_STRIDING_RQ	= (1 << 0),
};

struct mlx5_ib_create_wq {
	__aligned_u64 buf_addr;
	__aligned_u64 db_addr;
	__u32   rq_wqe_count;
	__u32   rq_wqe_shift;
	__u32   user_index;
	__u32   flags;
	__u32   comp_mask;
	__u32	single_stride_log_num_of_bytes;
	__u32	single_wqe_log_num_of_strides;
	__u32	two_byte_shift_en;
};

struct mlx5_ib_create_ah_resp {
	__u32	response_length;
	__u8	dmac[ETH_ALEN];
	__u8	reserved[6];
};

struct mlx5_ib_burst_info {
	__u32       max_burst_sz;
	__u16       typical_pkt_sz;
	__u16       reserved;
};

enum mlx5_ib_modify_qp_mask {
	MLX5_IB_MODIFY_QP_OOO_DP = 1 << 0,
};

struct mlx5_ib_modify_qp {
	__u32			   comp_mask;
	struct mlx5_ib_burst_info  burst_info;
	__u32			   ece_options;
};

struct mlx5_ib_modify_qp_resp {
	__u32	response_length;
	__u32	dctn;
	__u32   ece_options;
	__u32   reserved;
};

struct mlx5_ib_create_wq_resp {
	__u32	response_length;
	__u32	reserved;
};

struct mlx5_ib_create_rwq_ind_tbl_resp {
	__u32	response_length;
	__u32	reserved;
};

struct mlx5_ib_modify_wq {
	__u32	comp_mask;
	__u32	reserved;
};

struct mlx5_ib_clock_info {
	__u32 sign;
	__u32 resv;
	__aligned_u64 nsec;
	__aligned_u64 cycles;
	__aligned_u64 frac;
	__u32 mult;
	__u32 shift;
	__aligned_u64 mask;
	__aligned_u64 overflow_period;
};

enum mlx5_ib_mmap_cmd {
	MLX5_IB_MMAP_REGULAR_PAGE               = 0,
	MLX5_IB_MMAP_GET_CONTIGUOUS_PAGES       = 1,
	MLX5_IB_MMAP_WC_PAGE                    = 2,
	MLX5_IB_MMAP_NC_PAGE                    = 3,
	/* 5 is chosen in order to be compatible with old versions of libmlx5 */
	MLX5_IB_MMAP_CORE_CLOCK                 = 5,
	MLX5_IB_MMAP_ALLOC_WC                   = 6,
	MLX5_IB_MMAP_CLOCK_INFO                 = 7,
	MLX5_IB_MMAP_DEVICE_MEM                 = 8,
};

enum {
	MLX5_IB_CLOCK_INFO_KERNEL_UPDATING = 1,
};

/* Bit indexes for the mlx5_alloc_ucontext_resp.clock_info_versions bitmap */
enum {
	MLX5_IB_CLOCK_INFO_V1              = 0,
};

struct mlx5_ib_flow_counters_desc {
	__u32	description;
	__u32	index;
};

struct mlx5_ib_flow_counters_data {
	RDMA_UAPI_PTR(struct mlx5_ib_flow_counters_desc *, counters_data);
	__u32   ncounters;
	__u32   reserved;
};

struct mlx5_ib_create_flow {
	__u32   ncounters_data;
	__u32   reserved;
	/*
	 * Following are counters data based on ncounters_data, each
	 * entry in the data[] should match a corresponding counter object
	 * that was pointed by a counters spec upon the flow creation
	 */
	struct mlx5_ib_flow_counters_data data[];
};

#endif /* MLX5_ABI_USER_H */
