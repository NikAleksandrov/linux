/*
 * Copyright (c) 2018, Mellanox Technologies inc.  All rights reserved.
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

#ifndef MLX5_USER_IOCTL_CMDS_H
#define MLX5_USER_IOCTL_CMDS_H

#include <linux/types.h>
#include <rdma/ib_user_ioctl_cmds.h>

enum mlx5_ib_create_flow_action_attrs {
	/* This attribute belong to the driver namespace */
	MLX5_IB_ATTR_CREATE_FLOW_ACTION_FLAGS = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_dm_methods {
	MLX5_IB_METHOD_DM_MAP_OP_ADDR  = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_DM_QUERY,
};

enum mlx5_ib_dm_map_op_addr_attrs {
	MLX5_IB_ATTR_DM_MAP_OP_ADDR_REQ_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DM_MAP_OP_ADDR_REQ_OP,
	MLX5_IB_ATTR_DM_MAP_OP_ADDR_RESP_START_OFFSET,
	MLX5_IB_ATTR_DM_MAP_OP_ADDR_RESP_PAGE_INDEX,
};

enum mlx5_ib_query_dm_attrs {
	MLX5_IB_ATTR_QUERY_DM_REQ_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_QUERY_DM_RESP_START_OFFSET,
	MLX5_IB_ATTR_QUERY_DM_RESP_PAGE_INDEX,
	MLX5_IB_ATTR_QUERY_DM_RESP_LENGTH,
};

enum mlx5_ib_alloc_dm_attrs {
	MLX5_IB_ATTR_ALLOC_DM_RESP_START_OFFSET = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_ALLOC_DM_RESP_PAGE_INDEX,
	MLX5_IB_ATTR_ALLOC_DM_REQ_TYPE,
};

enum mlx5_ib_devx_methods {
	MLX5_IB_METHOD_DEVX_OTHER  = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_DEVX_QUERY_UAR,
	MLX5_IB_METHOD_DEVX_QUERY_EQN,
	MLX5_IB_METHOD_DEVX_SUBSCRIBE_EVENT,
};

enum  mlx5_ib_devx_other_attrs {
	MLX5_IB_ATTR_DEVX_OTHER_CMD_IN = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_OTHER_CMD_OUT,
};

enum mlx5_ib_devx_obj_create_attrs {
	MLX5_IB_ATTR_DEVX_OBJ_CREATE_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_OBJ_CREATE_CMD_IN,
	MLX5_IB_ATTR_DEVX_OBJ_CREATE_CMD_OUT,
};

enum  mlx5_ib_devx_query_uar_attrs {
	MLX5_IB_ATTR_DEVX_QUERY_UAR_USER_IDX = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_QUERY_UAR_DEV_IDX,
};

enum mlx5_ib_devx_obj_destroy_attrs {
	MLX5_IB_ATTR_DEVX_OBJ_DESTROY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_devx_obj_modify_attrs {
	MLX5_IB_ATTR_DEVX_OBJ_MODIFY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_OBJ_MODIFY_CMD_IN,
	MLX5_IB_ATTR_DEVX_OBJ_MODIFY_CMD_OUT,
};

enum mlx5_ib_devx_obj_query_attrs {
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_CMD_IN,
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_CMD_OUT,
};

enum mlx5_ib_devx_obj_query_async_attrs {
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_ASYNC_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_ASYNC_CMD_IN,
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_ASYNC_FD,
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_ASYNC_WR_ID,
	MLX5_IB_ATTR_DEVX_OBJ_QUERY_ASYNC_OUT_LEN,
};

enum mlx5_ib_devx_subscribe_event_attrs {
	MLX5_IB_ATTR_DEVX_SUBSCRIBE_EVENT_FD_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_SUBSCRIBE_EVENT_OBJ_HANDLE,
	MLX5_IB_ATTR_DEVX_SUBSCRIBE_EVENT_TYPE_NUM_LIST,
	MLX5_IB_ATTR_DEVX_SUBSCRIBE_EVENT_FD_NUM,
	MLX5_IB_ATTR_DEVX_SUBSCRIBE_EVENT_COOKIE,
};

enum  mlx5_ib_devx_query_eqn_attrs {
	MLX5_IB_ATTR_DEVX_QUERY_EQN_USER_VEC = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_QUERY_EQN_DEV_EQN,
};

enum mlx5_ib_devx_obj_methods {
	MLX5_IB_METHOD_DEVX_OBJ_CREATE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_DEVX_OBJ_DESTROY,
	MLX5_IB_METHOD_DEVX_OBJ_MODIFY,
	MLX5_IB_METHOD_DEVX_OBJ_QUERY,
	MLX5_IB_METHOD_DEVX_OBJ_ASYNC_QUERY,
};

enum mlx5_ib_var_alloc_attrs {
	MLX5_IB_ATTR_VAR_OBJ_ALLOC_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_VAR_OBJ_ALLOC_MMAP_OFFSET,
	MLX5_IB_ATTR_VAR_OBJ_ALLOC_MMAP_LENGTH,
	MLX5_IB_ATTR_VAR_OBJ_ALLOC_PAGE_ID,
};

enum mlx5_ib_var_obj_destroy_attrs {
	MLX5_IB_ATTR_VAR_OBJ_DESTROY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_var_obj_methods {
	MLX5_IB_METHOD_VAR_OBJ_ALLOC = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_VAR_OBJ_DESTROY,
};

enum mlx5_ib_uar_alloc_attrs {
	MLX5_IB_ATTR_UAR_OBJ_ALLOC_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_UAR_OBJ_ALLOC_TYPE,
	MLX5_IB_ATTR_UAR_OBJ_ALLOC_MMAP_OFFSET,
	MLX5_IB_ATTR_UAR_OBJ_ALLOC_MMAP_LENGTH,
	MLX5_IB_ATTR_UAR_OBJ_ALLOC_PAGE_ID,
};

enum mlx5_ib_uar_obj_destroy_attrs {
	MLX5_IB_ATTR_UAR_OBJ_DESTROY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_uar_obj_methods {
	MLX5_IB_METHOD_UAR_OBJ_ALLOC = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_UAR_OBJ_DESTROY,
};

enum mlx5_ib_devx_umem_reg_attrs {
	MLX5_IB_ATTR_DEVX_UMEM_REG_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_UMEM_REG_ADDR,
	MLX5_IB_ATTR_DEVX_UMEM_REG_LEN,
	MLX5_IB_ATTR_DEVX_UMEM_REG_ACCESS,
	MLX5_IB_ATTR_DEVX_UMEM_REG_OUT_ID,
	MLX5_IB_ATTR_DEVX_UMEM_REG_PGSZ_BITMAP,
	MLX5_IB_ATTR_DEVX_UMEM_REG_DMABUF_FD,
};

enum mlx5_ib_devx_umem_dereg_attrs {
	MLX5_IB_ATTR_DEVX_UMEM_DEREG_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_pp_obj_methods {
	MLX5_IB_METHOD_PP_OBJ_ALLOC = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_PP_OBJ_DESTROY,
};

enum mlx5_ib_pp_alloc_attrs {
	MLX5_IB_ATTR_PP_OBJ_ALLOC_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_PP_OBJ_ALLOC_CTX,
	MLX5_IB_ATTR_PP_OBJ_ALLOC_FLAGS,
	MLX5_IB_ATTR_PP_OBJ_ALLOC_INDEX,
};

enum mlx5_ib_pp_obj_destroy_attrs {
	MLX5_IB_ATTR_PP_OBJ_DESTROY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_devx_umem_methods {
	MLX5_IB_METHOD_DEVX_UMEM_REG = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_DEVX_UMEM_DEREG,
};

enum mlx5_ib_devx_async_cmd_fd_alloc_attrs {
	MLX5_IB_ATTR_DEVX_ASYNC_CMD_FD_ALLOC_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_devx_async_event_fd_alloc_attrs {
	MLX5_IB_ATTR_DEVX_ASYNC_EVENT_FD_ALLOC_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_DEVX_ASYNC_EVENT_FD_ALLOC_FLAGS,
};

enum mlx5_ib_devx_async_cmd_fd_methods {
	MLX5_IB_METHOD_DEVX_ASYNC_CMD_FD_ALLOC = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_devx_async_event_fd_methods {
	MLX5_IB_METHOD_DEVX_ASYNC_EVENT_FD_ALLOC = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_objects {
	MLX5_IB_OBJECT_DEVX = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_OBJECT_DEVX_OBJ,
	MLX5_IB_OBJECT_DEVX_UMEM,
	MLX5_IB_OBJECT_FLOW_MATCHER,
	MLX5_IB_OBJECT_DEVX_ASYNC_CMD_FD,
	MLX5_IB_OBJECT_DEVX_ASYNC_EVENT_FD,
	MLX5_IB_OBJECT_VAR,
	MLX5_IB_OBJECT_PP,
	MLX5_IB_OBJECT_UAR,
	MLX5_IB_OBJECT_STEERING_ANCHOR,
	/*
	 * Verb-only namespace (no per-instance state, no IDR) for the
	 * VFMIG (CRIU SR-IOV migration) per-ucontext save/restore verbs.
	 * See tools/testing/mlx5_vfmig/design/uar_restore.md.
	 */
	MLX5_IB_OBJECT_VFMIG,
};

enum mlx5_ib_flow_matcher_create_attrs {
	MLX5_IB_ATTR_FLOW_MATCHER_CREATE_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_FLOW_MATCHER_MATCH_MASK,
	MLX5_IB_ATTR_FLOW_MATCHER_FLOW_TYPE,
	MLX5_IB_ATTR_FLOW_MATCHER_MATCH_CRITERIA,
	MLX5_IB_ATTR_FLOW_MATCHER_FLOW_FLAGS,
	MLX5_IB_ATTR_FLOW_MATCHER_FT_TYPE,
	MLX5_IB_ATTR_FLOW_MATCHER_IB_PORT,
};

enum mlx5_ib_flow_matcher_destroy_attrs {
	MLX5_IB_ATTR_FLOW_MATCHER_DESTROY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_flow_matcher_methods {
	MLX5_IB_METHOD_FLOW_MATCHER_CREATE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_FLOW_MATCHER_DESTROY,
};

enum mlx5_ib_flow_steering_anchor_create_attrs {
	MLX5_IB_ATTR_STEERING_ANCHOR_CREATE_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_STEERING_ANCHOR_FT_TYPE,
	MLX5_IB_ATTR_STEERING_ANCHOR_PRIORITY,
	MLX5_IB_ATTR_STEERING_ANCHOR_FT_ID,
};

enum mlx5_ib_flow_steering_anchor_destroy_attrs {
	MLX5_IB_ATTR_STEERING_ANCHOR_DESTROY_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_steering_anchor_methods {
	MLX5_IB_METHOD_STEERING_ANCHOR_CREATE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_STEERING_ANCHOR_DESTROY,
};

enum mlx5_ib_device_query_context_attrs {
	MLX5_IB_ATTR_QUERY_CONTEXT_RESP_UCTX = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_create_cq_attrs {
	MLX5_IB_ATTR_CREATE_CQ_UAR_INDEX = UVERBS_ID_DRIVER_NS_WITH_UHW,
};

enum mlx5_ib_reg_dmabuf_mr_attrs {
	MLX5_IB_ATTR_REG_DMABUF_MR_ACCESS_FLAGS = (1U << UVERBS_ID_NS_SHIFT),
};

#define MLX5_IB_DW_MATCH_PARAM 0xA0

struct mlx5_ib_match_params {
	__u32	match_params[MLX5_IB_DW_MATCH_PARAM];
};

enum mlx5_ib_flow_type {
	MLX5_IB_FLOW_TYPE_NORMAL,
	MLX5_IB_FLOW_TYPE_SNIFFER,
	MLX5_IB_FLOW_TYPE_ALL_DEFAULT,
	MLX5_IB_FLOW_TYPE_MC_DEFAULT,
};

enum mlx5_ib_create_flow_flags {
	MLX5_IB_ATTR_CREATE_FLOW_FLAGS_DEFAULT_MISS = 1 << 0,
	MLX5_IB_ATTR_CREATE_FLOW_FLAGS_DROP = 1 << 1,
};

enum mlx5_ib_create_flow_attrs {
	MLX5_IB_ATTR_CREATE_FLOW_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_CREATE_FLOW_MATCH_VALUE,
	MLX5_IB_ATTR_CREATE_FLOW_DEST_QP,
	MLX5_IB_ATTR_CREATE_FLOW_DEST_DEVX,
	MLX5_IB_ATTR_CREATE_FLOW_MATCHER,
	MLX5_IB_ATTR_CREATE_FLOW_ARR_FLOW_ACTIONS,
	MLX5_IB_ATTR_CREATE_FLOW_TAG,
	MLX5_IB_ATTR_CREATE_FLOW_ARR_COUNTERS_DEVX,
	MLX5_IB_ATTR_CREATE_FLOW_ARR_COUNTERS_DEVX_OFFSET,
	MLX5_IB_ATTR_CREATE_FLOW_FLAGS,
};

enum mlx5_ib_destroy_flow_attrs {
	MLX5_IB_ATTR_DESTROY_FLOW_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum mlx5_ib_flow_methods {
	MLX5_IB_METHOD_CREATE_FLOW = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_DESTROY_FLOW,
};

enum mlx5_ib_flow_action_methods {
	MLX5_IB_METHOD_FLOW_ACTION_CREATE_MODIFY_HEADER = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_FLOW_ACTION_CREATE_PACKET_REFORMAT,
};

enum mlx5_ib_create_flow_action_create_modify_header_attrs {
	MLX5_IB_ATTR_CREATE_MODIFY_HEADER_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_CREATE_MODIFY_HEADER_ACTIONS_PRM,
	MLX5_IB_ATTR_CREATE_MODIFY_HEADER_FT_TYPE,
};

enum mlx5_ib_create_flow_action_create_packet_reformat_attrs {
	MLX5_IB_ATTR_CREATE_PACKET_REFORMAT_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_CREATE_PACKET_REFORMAT_TYPE,
	MLX5_IB_ATTR_CREATE_PACKET_REFORMAT_FT_TYPE,
	MLX5_IB_ATTR_CREATE_PACKET_REFORMAT_DATA_BUF,
};

enum mlx5_ib_query_pd_attrs {
	MLX5_IB_ATTR_QUERY_PD_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_QUERY_PD_RESP_PDN,
};

enum mlx5_ib_pd_methods {
	MLX5_IB_METHOD_PD_QUERY = (1U << UVERBS_ID_NS_SHIFT),

};

enum mlx5_ib_device_methods {
	MLX5_IB_METHOD_QUERY_PORT = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_GET_DATA_DIRECT_SYSFS_PATH,
};

enum mlx5_ib_query_port_attrs {
	MLX5_IB_ATTR_QUERY_PORT_PORT_NUM = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_QUERY_PORT,
};

enum mlx5_ib_get_data_direct_sysfs_path_attrs {
	MLX5_IB_ATTR_GET_DATA_DIRECT_SYSFS_PATH = (1U << UVERBS_ID_NS_SHIFT),
};

/*
 * VFMIG ucontext vendor verbs. Methods on the verb-only object
 * MLX5_IB_OBJECT_VFMIG; the calling fd's ucontext is implicit (resolved
 * via ib_uverbs_get_ucontext()).
 *
 * Wire-protocol contract: identifiers in UAR_TABLE / BFREG_COUNT are
 * opaque FW UAR ids and per-bfreg-slot counts respectively, as returned
 * by mlx5 firmware. They are valid only against a destination VHCA whose
 * state was imported by SAVE_VHCA_STATE / LOAD_VHCA_STATE from the
 * source VHCA they were captured on.
 *
 * See tools/testing/mlx5_vfmig/design/uar_restore.md.
 */
enum mlx5_ib_vfmig_methods {
	MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT,
	/*
	 * Dynamic UAR (lib_uar_dyn=true / MLX5_LIB_CAP_DYN_UAR) save/restore.
	 *
	 * libmlx5 in dynamic-UAR mode does not use the static
	 * bfregi->sys_pages[] table at all; UARs are allocated lazily as
	 * MLX5_IB_OBJECT_UAR uobjects via MLX5_IB_METHOD_UAR_OBJ_ALLOC and
	 * exposed to userspace via rdma_user_mmap_entry start_pgoff plus
	 * the uobject handle returned in MLX5_IB_ATTR_UAR_OBJ_ALLOC_HANDLE.
	 *
	 * QUERY_DYN_UARS snapshots, for the calling fd's ucontext, every
	 * outstanding MLX5_IB_OBJECT_UAR uobject as a record carrying
	 *   (handle, page_id, mmap_offset, alloc_type)
	 * so a destination ucontext (opened with
	 * MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE on a destination VHCA whose
	 * state was imported via SAVE_VHCA_STATE / LOAD_VHCA_STATE from
	 * the source) can replay them via RESTORE_DYN_UARS, which
	 * reconstructs the uobjects at the same handles and re-inserts
	 * the rdma_user_mmap_entry's at the same start_pgoff. After
	 * RESTORE_DYN_UARS, the userspace-visible (handle, mmap_offset)
	 * pairs that libmlx5 captured in its dump are valid against the
	 * destination ucontext byte-for-byte, so the existing libmlx5
	 * mmap()s can re-execute against the new fd unchanged.
	 *
	 * Two-pass call convention identical to QUERY_UCONTEXT: a first
	 * call without the RECORDS array reads back COUNT; a second call
	 * with RECORDS sized RECORDS[COUNT] fills them under the
	 * ucontext's locks.  RESTORE_DYN_UARS is single-shot per ucontext
	 * (mirrors RESTORE_UCONTEXT): it consumes c->vfmig_restore_pending.
	 */
	MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS,
	MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS,
	/*
	 * QUERY_CQ: dump-side counterpart to UVERBS_METHOD_RESTORE_CQ.
	 * The CRIU dumper holds the dumpee's uverbs fd (same fd it uses
	 * for INFO_HANDLES / NLDEV joins) and runs in its own address
	 * space, so it cannot derive the source userspace VAs of the
	 * CQE-ring buffer / doorbell page from libmlx5's mlx5dv_init_obj
	 * (whose @buf, @dbrec are the CALLING process's VAs -- here
	 * CRIU's, not the dumpee's). Likewise libibverbs has no
	 * ibv_import_cq verb, so manufacturing an ibv_cq* in CRIU's
	 * address space to feed mlx5dv_init_obj is not an option.
	 *
	 * QUERY_CQ closes that loop by reading the kernel's own copy of
	 * the source-side state. The kernel knows all five fields
	 * trivially: cqn = mcq->mcq.cqn, cqe = ibcq->cqe, cqe_size =
	 * mcq->cqe_size, buf_addr = mcq->buf.umem->address (set by
	 * ib_umem_get(@ucmd.buf_addr) at original CREATE_CQ),
	 * db_addr = mcq->db.u.user_page->user_virt (page-aligned at
	 * mlx5_ib_db_map_user). The 32-byte BLOB is byte-equal to
	 * struct mlx5_ib_restore_cq_req so the CRIU plugin can copy it
	 * verbatim into protobuf at dump and feed it back into the UHW
	 * tail of UVERBS_METHOD_RESTORE_CQ at restore. The companion
	 * outs (CQE / COMP_VECTOR / FLAGS) carry the per-CQ inputs
	 * RESTORE_CQ takes as core attrs (not in the UHW blob).
	 *
	 * Security boundary: the calling fd must own the CQ uobject
	 * referenced by HANDLE -- the IDR lookup against UVERBS_OBJECT_CQ
	 * resolves through ufile->idr, which is per-uverbs-fd. Same
	 * "if you can see the ucontext, you can read its metadata"
	 * boundary as UVERBS_METHOD_QUERY_MR (see core/uverbs_std_types_mr.c
	 * + tools/testing/mlx5_vfmig/design/uobject_restore.md §7.7).
	 *
	 * Kernel-mode CQs (no udata at create time -- mcq->buf.umem is
	 * NULL and mcq->db.u.pgdir is the kernel-allocated lane) reject
	 * with -ENXIO: there are no source userspace VAs to emit, and
	 * RESTORE_CQ would have nothing to consume even if we synthesised
	 * zeros.
	 *
	 * See tools/testing/mlx5_vfmig/design/uobject_restore.md §5.2.4.
	 */
	MLX5_IB_METHOD_VFMIG_QUERY_CQ,
	/*
	 * QUERY_QP: dump-side counterpart to UVERBS_METHOD_RESTORE_QP.
	 * Mirror of QUERY_CQ (above): same cross-process motivation
	 * (CRIU runs in its own address space so mlx5dv_init_obj on
	 * MLX5DV_OBJ_QP would return CRIU's VAs rather than the
	 * dumpee's; ibv_import_qp does not exist in upstream rdma-core),
	 * same security boundary (HANDLE resolves through the calling
	 * fd's ufile-idr; same "if you can see the ucontext, you can
	 * read its metadata" model), same byte-equal payload contract
	 * (RESP_BLOB is byte-equal to mlx5_ib_restore_qp_req so the CRIU
	 * plugin copies it verbatim into protobuf at dump and feeds it
	 * back into the UHW tail of UVERBS_METHOD_RESTORE_QP at restore).
	 *
	 * Per-QP scalar outs (RESP_TYPE / RESP_STATE / RESP_USER_HANDLE
	 * / RESP_CAP / RESP_CREATE_FLAGS) carry the inputs RESTORE_QP
	 * takes as core attrs (not in the UHW blob).
	 *
	 * Kernel-side reads:
	 *
	 *   qpn         -- mqp.trans_qp.base.mqp.qpn (24-bit FW resource
	 *                  id; non-zero for any live QP).
	 *   buf_addr    -- trans_qp.base.ubuffer.umem->address (the
	 *                  source userspace VA RESTORE_QP needs to look
	 *                  up the LOAD_VHCA_STATE-installed (KIND_QP,
	 *                  qpn) placeholder).
	 *   db_addr     -- mlx5_ib_db_user_virt(&mqp->db) (the page-
	 *                  aligned DBR-page user-virt; mlx5_ib_db_map_user
	 *                  dedup-keyed on this).
	 *   sq_wqe_count / rq_wqe_count / rq_wqe_shift -- mqp->sq.wqe_cnt
	 *                  / mqp->rq.wqe_cnt / mqp->rq.wqe_shift (the
	 *                  WQ-ring shape RESTORE_QP's set_user_buf_size-
	 *                  equivalent arithmetic re-derives buf_size from).
	 *   flags       -- mqp->flags_en (MLX5_QP_FLAG_* bitmask).
	 *
	 * Three of the round-trip UHW fields are FW-side state inside
	 * the adopted qpc, not mirrored on mlx5_ib_qp; they round-trip
	 * via LOAD_VHCA_STATE alone (K7 byte-equal proof) and the
	 * RESTORE_QP handler validates-and-discards them. The dump-side
	 * verb emits sentinels for them:
	 *
	 *   uidx          -- 0 (qpc.user_index is preserved by LOAD;
	 *                    RESTORE_QP only validates req.uidx & ~0xffffff)
	 *   bfreg_index   -- MLX5_IB_INVALID_BFREG (RESTORE_QP forces
	 *                    qp->bfregn = MLX5_IB_INVALID_BFREG anyway:
	 *                    the source's UAR mapping is encoded in the
	 *                    adopted qpc.uar_page, not re-derived from
	 *                    this field)
	 *   ece_options   -- 0 (FW negotiates ECE per-connection during
	 *                    MODIFY_QP; the QPC's ece_options round-trip
	 *                    via LOAD_VHCA_STATE)
	 *
	 * Per-QP scalar outs:
	 *
	 *   RESP_TYPE         u32, mqp->type (RC/UC/UD only at v0; the
	 *                     handler rejects RAW_PACKET / XRC / GSI /
	 *                     DCT/DCI with -EOPNOTSUPP, mirroring
	 *                     mlx5_ib_restore_qp's switch).
	 *   RESP_STATE        u32, mqp->state (the kernel-tracked QP
	 *                     state mlx5_ib_modify_qp updates on every
	 *                     transition; RESET / INIT / RTR / RTS / ERR
	 *                     for v0 -- §5.3.1).
	 *   RESP_USER_HANDLE  u64, ibqp->uobject->user_handle (the
	 *                     userspace tag the source's
	 *                     ib_uverbs_create_qp recorded).
	 *   RESP_CAP          struct ib_uverbs_qp_cap; best-effort echo
	 *                     of the cap that ibv_create_qp returned to
	 *                     the source. mlx5_ib_query_qp shows
	 *                     max_send_wr / max_send_sge are not tracked
	 *                     for user-mode QPs (only max_recv_wr /
	 *                     max_recv_sge / max_inline_data are); the
	 *                     handler emits qp->sq.wqe_cnt for max_send_wr
	 *                     to give a useful value, qp->rq.wqe_cnt for
	 *                     max_recv_wr, qp->rq.max_gs for max_recv_sge,
	 *                     qp->max_inline_data for max_inline_data,
	 *                     and 1 for max_send_sge (kernel doesn't
	 *                     track this on user QP). RESTORE_QP's mlx5
	 *                     handler doesn't validate cap (the actual
	 *                     WQ shape comes from the UHW's
	 *                     {sq,rq}_wqe_count); the field is forward-
	 *                     compat surface area for a future driver
	 *                     that may consult it.
	 *   RESP_CREATE_FLAGS u32, mqp->flags (the IB_QP_CREATE_* mask
	 *                     captured at create time; same field
	 *                     mlx5_ib_query_qp returns).
	 *
	 * Kernel-mode QPs (no udata at create time -- ibqp->uobject == NULL
	 * or trans_qp.base.ubuffer.umem == NULL) reject with -ENXIO:
	 * there are no source userspace VAs to emit.
	 *
	 * See tools/testing/mlx5_vfmig/design/uobject_restore.md §5.3.4.
	 */
	MLX5_IB_METHOD_VFMIG_QUERY_QP,
	/*
	 * QUERY_PD: dump-side counterpart to UVERBS_METHOD_RESTORE_PD.
	 * Same family as QUERY_CQ / QUERY_QP -- emit, for the PD resolved
	 * through UVERBS_OBJECT_PD on the calling fd's ufile, the bytes a
	 * CRIU plugin needs to drive RESTORE_PD on the destination side.
	 *
	 * This supersedes the earlier NLDEV driver-TLV discovery path
	 * (mlx5/restrack.c fill_res_pd_entry emitting "fw_pdn"/"fw_uid"
	 * under RDMA_NLDEV_ATTR_DRIVER), which was the odd one out: every
	 * other adopted FW resource id (cqn via QUERY_CQ, qpn via
	 * QUERY_QP) is discovered through a per-handle driver-private
	 * QUERY method that returns a byte-equal RESP_BLOB. Moving PD onto
	 * the same plane makes the family uniform and drops the
	 * CAP_NET_ADMIN / cross-netns NLDEV dependency: CRIU now learns
	 * the FW pdn on the uverbs fd it already holds for QUERY_CQ/_QP,
	 * under the per-ucontext "if you can see the ucontext, you can
	 * read its metadata" boundary.
	 *
	 * Two-part output:
	 *   RESP_BLOB  struct mlx5_ib_restore_pd_req, byte-equal to what
	 *              RESTORE_PD's UHW consumes (carries the FW pdn;
	 *              reserved fields left zero so the restore path's
	 *              "must be 0" checks pass round-trip). CRIU copies it
	 *              verbatim into the RESTORE_PD UHW tail.
	 *   RESP_UID   u32, the source PD's mpd->uid. DUMP-SIDE ONLY: this
	 *              is NOT a restore input (RESTORE_PD sets mpd->uid =
	 *              context->devx_uid from the adopted ucontext). It is
	 *              surfaced so the CRIU plugin can confirm the source
	 *              PD lived under the v0 host-privileged lane (uid == 0)
	 *              and fail the dump early on a DEVX uid rather than
	 *              producing an unrestorable image.
	 *
	 * Security boundary: the calling fd must own the PD uobject
	 * referenced by HANDLE -- the IDR lookup against UVERBS_OBJECT_PD
	 * resolves through ufile->idr, which is per-uverbs-fd. Same model
	 * as QUERY_CQ / QUERY_QP.
	 *
	 * See tools/testing/mlx5_vfmig/design/uobject_restore.md §5.1.4.
	 */
	MLX5_IB_METHOD_VFMIG_QUERY_PD,
};

/*
 * Attrs for MLX5_IB_METHOD_VFMIG_QUERY_CQ.
 *
 * The HANDLE is resolved via UVERBS_ATTR_IDR(UVERBS_OBJECT_CQ,
 * UVERBS_ACCESS_READ): the calling fd's ufile-idr must own this CQ.
 *
 * The four RESP_* outs together provide everything UVERBS_METHOD_RESTORE_CQ
 * consumes for an mlx5 CQ that was created from userspace:
 *
 *   RESP_BLOB         struct mlx5_ib_restore_cq_req (32 bytes; goes
 *                     verbatim into UVERBS_ATTR_RESTORE_CQ_UHW_IN /
 *                     attrs->driver_udata at restore time). Carries
 *                     buf_addr, db_addr, cqn, cqe_size, two reserved
 *                     u32s left zero by the handler. The byte-equal
 *                     contract is the v0 design's whole reason for
 *                     existing -- CRIU plugin code is memcpy in,
 *                     memcpy out.
 *
 *   RESP_CQE          u32, the source's ibcq->cqe (entries-1 in the
 *                     verbs convention; mlx5_ib_restore_cq's umem-pin
 *                     length math = (size_t)attr->cqe * cqe_size).
 *                     Goes into UVERBS_ATTR_RESTORE_CQ_CQE.
 *
 *   RESP_COMP_VECTOR  u32, the source's mcq->mcq.vector. Goes into
 *                     UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR. The
 *                     destination's mlx5_comp_eqn_get(comp_vector)
 *                     yields the destination-side eqn for that
 *                     vector index, which K6 + S5b B0 establish
 *                     matches the source-side eqn baked into the
 *                     adopted cqc.c_eqn_or_apu_element across
 *                     LOAD_VHCA_STATE.
 *
 *   RESP_FLAGS        u32, the source's cq->create_flags (mask of
 *                     IB_UVERBS_CQ_FLAGS_TIMESTAMP_COMPLETION /
 *                     IB_UVERBS_CQ_FLAGS_IGNORE_OVERRUN). Goes into
 *                     UVERBS_ATTR_RESTORE_CQ_FLAGS.
 *
 * All four outs are MANDATORY: a CRIU plugin that ignores any of
 * them at dump time will produce an unrestorable image.
 */
enum mlx5_ib_vfmig_query_cq_attrs {
	MLX5_IB_ATTR_VFMIG_QUERY_CQ_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_BLOB,
	MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_CQE,
	MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_COMP_VECTOR,
	MLX5_IB_ATTR_VFMIG_QUERY_CQ_RESP_FLAGS,
};

/*
 * Attrs for MLX5_IB_METHOD_VFMIG_QUERY_QP.
 *
 * The HANDLE is resolved via UVERBS_ATTR_IDR(UVERBS_OBJECT_QP,
 * UVERBS_ACCESS_READ): the calling fd's ufile-idr must own this QP.
 *
 * Principle: a driver-private QUERY_QP returns only QP state that the
 * standard IB_USER_VERBS_CMD_QUERY_QP verb (and NLDEV) cannot express.
 * The dumper already holds the owning uctx fd + the QP IDR handle, so it
 * sources cap from the standard query_qp (cap is creation-static, hence
 * order-insensitive w.r.t. FREEZE_DATAPATH), qp_type from NLDEV
 * RES_TYPE, and qp_state from query_qp / NLDEV RES_STATE. Those three
 * are therefore NOT re-exported here.
 *
 * The three RESP_* outs are everything left that has no standard /
 * NLDEV surface:
 *
 *   RESP_BLOB         struct mlx5_ib_restore_qp_req (64 bytes; goes
 *                     verbatim into UVERBS_ATTR_RESTORE_QP_UHW_IN /
 *                     attrs->driver_udata at restore time). Carries
 *                     buf_addr, db_addr, sq_buf_addr, qpn,
 *                     {sq,rq}_wqe_count, rq_wqe_shift, flags, plus
 *                     uidx / bfreg_index / ece_options (sentinels
 *                     for the FW-side fields LOAD_VHCA_STATE
 *                     preserves) and two reserved u32s the handler
 *                     leaves zero. The byte-equal contract is the
 *                     v0 design's whole reason for existing -- CRIU
 *                     plugin code is memcpy in, memcpy out.
 *
 *   RESP_USER_HANDLE  u64, the source's ibqp->uobject->user_handle.
 *                     Goes into UVERBS_ATTR_RESTORE_QP_USER_HANDLE.
 *                     Not standard-queryable.
 *
 *   RESP_CREATE_FLAGS u32, the source's mqp->flags. Goes into
 *                     UVERBS_ATTR_RESTORE_QP_CREATE_FLAGS. Not present
 *                     in the legacy query_qp resp.
 *
 * All four (HANDLE + three RESP_*) are MANDATORY: a CRIU plugin that
 * ignores any of them at dump time will produce an unrestorable image.
 */
enum mlx5_ib_vfmig_query_qp_attrs {
	MLX5_IB_ATTR_VFMIG_QUERY_QP_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_BLOB,
	MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_USER_HANDLE,
	MLX5_IB_ATTR_VFMIG_QUERY_QP_RESP_CREATE_FLAGS,
};

/*
 * Attrs for MLX5_IB_METHOD_VFMIG_QUERY_PD.
 *
 * The HANDLE is resolved via UVERBS_ATTR_IDR(UVERBS_OBJECT_PD,
 * UVERBS_ACCESS_READ): the calling fd's ufile-idr must own this PD.
 *
 *   RESP_BLOB  struct mlx5_ib_restore_pd_req (goes verbatim into the
 *              RESTORE_PD UHW tail at restore time). Carries the FW
 *              pdn; the handler leaves req.reserved / req.reserved2
 *              zero so the restore path's "must be 0" checks pass
 *              round-trip. The byte-equal contract mirrors QUERY_CQ /
 *              QUERY_QP -- CRIU plugin code is memcpy in, memcpy out.
 *
 *   RESP_UID   u32, the source PD's mpd->uid. Dump-side cross-check
 *              only (not consumed by RESTORE_PD, which takes uid from
 *              the adopted ucontext's devx_uid). Lets the plugin fail
 *              the dump early if the source PD is not under the v0
 *              host-privileged lane (uid != 0).
 *
 * Both outs are MANDATORY: a CRIU plugin that ignores either at dump
 * time will produce an unrestorable image.
 */
enum mlx5_ib_vfmig_query_pd_attrs {
	MLX5_IB_ATTR_VFMIG_QUERY_PD_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_VFMIG_QUERY_PD_RESP_BLOB,
	MLX5_IB_ATTR_VFMIG_QUERY_PD_RESP_UID,
};

/*
 * Two-pass call convention:
 *
 *  Pass 1 (sizing): pass META only. Kernel fills META; UAR_TABLE and
 *    BFREG_COUNT are absent (UA_OPTIONAL). Userspace reads
 *    meta.num_sys_pages and meta.total_num_bfregs from the result and
 *    allocates u32[num_sys_pages] and u32[total_num_bfregs] buffers.
 *
 *  Pass 2 (snapshot): pass META + UAR_TABLE + BFREG_COUNT, with the
 *    arrays sized exactly as learned from pass 1. Kernel snapshots
 *    bfregi->sys_pages[] and bfregi->count[] under bfregi->lock.
 *
 * The handler rejects with -EINVAL if the array buffer sizes don't
 * match the current ucontext shape, so racing a call against e.g. a
 * concurrent dynamic-UAR mmap is detectable rather than silently
 * truncating.
 */
enum mlx5_ib_vfmig_query_ucontext_attrs {
	MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT,
	MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_META,
};

/*
 * RESTORE_UCONTEXT consumes a snapshot previously emitted by
 * QUERY_UCONTEXT on a source ucontext (whose underlying VHCA was then
 * SAVE_VHCA_STATE'd and LOAD_VHCA_STATE'd onto this destination VHCA),
 * and seeds the destination ucontext's bfregi->sys_pages[] verbatim --
 * skipping the per-slot ALLOC_UAR FW commands that mlx5_ib_alloc_ucontext
 * would normally issue. This relies on the destination VHCA already
 * holding those FW UAR ids reserved as part of LOAD_VHCA_STATE.
 *
 * Preconditions (all enforced by the handler; -EINVAL on any failure):
 *   1. The ucontext was created with MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE
 *      (so sys_pages[] is sentinel-INVALID and ready to be seeded).
 *   2. lib_uar_dyn=false (v0 doesn't cover dynamic-UAR ucontexts).
 *   3. META cross-check: every shape-defining field of the supplied
 *      mlx5_ib_vfmig_ucontext_meta must match what the destination's
 *      mlx5_ib_alloc_ucontext computed for THIS ucontext (strict bitwise
 *      equality on num_static_sys_pages, num_sys_pages, num_dyn_bfregs,
 *      num_low_latency_bfregs, total_num_bfregs, lib_caps, lib_uar_4k,
 *      lib_uar_dyn, cqe_version, devx_uid). v0 targets a homogeneous
 *      fleet; this guarantee can be relaxed later if needed.
 *      @devx_uid being included makes the FW-owner-id seam loud at
 *      RESTORE_UCONTEXT instead of silently letting subsequent
 *      restore_pd/cq/qp stamp a mismatched uid that surfaces only at
 *      DEALLOC_PD teardown as bad_resource_state -- by the time
 *      RESTORE_UCONTEXT runs the dest's c->devx_uid is already final
 *      (set by mlx5_ib_alloc_ucontext from req.adopt_devx_uid when
 *      MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID was passed, or from a fresh
 *      mlx5_ib_devx_create otherwise), so a mismatch here = "the
 *      dump and restore plugins disagree about the source's FW
 *      owner-id" = unrestorable.
 *   4. UAR_TABLE length == num_sys_pages * sizeof(__u32) (mandatory).
 *   5. UAR_TABLE[0..num_static_sys_pages) must all be valid (none equal
 *      to MLX5_IB_INVALID_UAR_INDEX = BIT(31)). Static slots are
 *      structural; an INVALID there indicates a malformed snapshot.
 *   6. BFREG_COUNT, if supplied, length == total_num_bfregs *
 *      sizeof(__u32). v0 ALSO requires every entry to be zero --
 *      non-zero implies the source had live QPs / claimed dyn UARs,
 *      whose corresponding kernel/FW objects we don't yet rebuild.
 *      Locking the wire shape now lets a future MR/QP-restore step
 *      relax this without an ABI bump.
 *
 * On success the handler memcpy()s sys_pages[] (and zeros count[],
 * idempotently) under bfregi->lock, then clears
 * c->vfmig_restore_pending. A subsequent RESTORE on the same ucontext
 * therefore fails precondition #1 with -EINVAL: there's exactly one
 * RESTORE per ucontext.
 */
enum mlx5_ib_vfmig_restore_ucontext_attrs {
	MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_UAR_TABLE = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_BFREG_COUNT,
	MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_META,
};

/*
 * @devx_uid carries the source ucontext's mlx5_ib_ucontext::devx_uid
 * across the SAVE/LOAD seam. It is the FW-allocated (or 0 = "kernel /
 * non-DEVX") owner-id that LOAD_VHCA_STATE preserves on every imported
 * FW resource (PDC, CQC, QPC, MKC, SRQC, ...) -- but the matching
 * uctx-registration table entry is NOT preserved (per the §S3b
 * "DEVX-adoption blind spot" matrix on FW 28.48.1000). The dest VF's
 * FW will reject any low-range non-zero uid at CREATE_x / MODIFY_x /
 * DESTROY_x with the "unknown uid" syndrome 0x76555f.
 *
 * Consequences for the v0 restore path:
 *
 *   - source.devx_uid == 0 (non-DEVX libibverbs ucontext): the
 *     destination opens its restore-mode ucontext without DEVX, the
 *     dest's c->devx_uid is also 0, every adopted FW resource lands
 *     in the uid=0 host-privileged ungated lane, and modify/destroy
 *     all succeed. This is the v0 critical path and what the
 *     bring-up harnesses exercise.
 *
 *   - source.devx_uid != 0 (libmlx5 auto-DEVX or explicit DEVX): the
 *     destination CANNOT successfully open with
 *     MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID (the alloc_ucontext liveness
 *     check on ALLOC_TRANSPORT_DOMAIN will fail because the uid is
 *     not registered post-LOAD), and the v0 mitigation of "open
 *     without DEVX, run everything under uid=0" silently breaks the
 *     destroy chain: the FW QPC/CQC/PDC are owned by source.devx_uid
 *     but the destination kernel issues 2RST_QP / DESTROY_QP /
 *     DESTROY_CQ / DEALLOC_PD with uid=0. mlx5_ib_destroy_qp's path
 *     warn-only-logs FW failures and unconditionally returns 0, so
 *     the user-visible failure surfaces only at the first opcode
 *     that propagates its FW errno verbatim (DEALLOC_PD: bad_resource
 *     state, syndrome 0xef0c8a-class). The CRIU plugin therefore
 *     MUST refuse to dump a ucontext whose meta.devx_uid is non-zero
 *     until either the matrix's row for "uid=src_devx_uid" flips to
 *     accept (a future-FW capability that preserves the uctx
 *     registry) or one of the alternatives in §S3b options 1-3
 *     lands.
 *
 * Exposing the field rather than hiding it makes the failure mode
 * detectable at dump time by the plugin (refuse cleanly) and at
 * restore time by the kernel (see RESTORE_UCONTEXT precondition #3:
 * meta.devx_uid is included in the strict-equality check, so a
 * mis-matched dump/restore plugin pair fails -EINVAL early at
 * RESTORE_UCONTEXT instead of silently letting subsequent
 * restore_pd/cq/qp adopt resources whose FW commands then fail
 * obscurely at teardown).
 *
 * Backward compat: a kernel that doesn't yet emit @devx_uid leaves
 * it 0 (it occupies bytes that were previously reserved1[0..1]). A
 * userspace running against the older kernel sees only the existing
 * "non-DEVX-only" test surface, which is exactly the v0 critical
 * path; no behavior change.
 */
struct mlx5_ib_vfmig_ucontext_meta {
	__u32	num_static_sys_pages;
	__u32	num_sys_pages;
	__u32	num_dyn_bfregs;
	__u32	num_low_latency_bfregs;
	__u32	total_num_bfregs;
	__u32	reserved0;
	__aligned_u64 lib_caps;
	__u8	lib_uar_4k;
	__u8	lib_uar_dyn;
	__u8	cqe_version;
	__u8	reserved1[3];
	__u16	devx_uid;
};

/*
 * QUERY_DYN_UARS / RESTORE_DYN_UARS attribute IDs.
 *
 *   RECORDS:  array of struct mlx5_ib_vfmig_dyn_uar_record. Length must
 *             equal COUNT * sizeof(struct mlx5_ib_vfmig_dyn_uar_record).
 *             OPTIONAL on QUERY_DYN_UARS (omit for the sizing pass);
 *             MANDATORY on RESTORE_DYN_UARS.
 *
 *   COUNT:    __u32. On QUERY_DYN_UARS, kernel always writes the number
 *             of dynamic UARs currently held by this ucontext (so the
 *             sizing pass tells userspace exactly how big RECORDS must
 *             be, and the snapshot pass confirms RECORDS was sized
 *             correctly). RECORDS, if supplied, must be COUNT-sized.
 */
enum mlx5_ib_vfmig_query_dyn_uars_attrs {
	MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_RECORDS = (1U << UVERBS_ID_NS_SHIFT),
	MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_COUNT,
};

enum mlx5_ib_vfmig_restore_dyn_uars_attrs {
	MLX5_IB_ATTR_VFMIG_RESTORE_DYN_UARS_RECORDS = (1U << UVERBS_ID_NS_SHIFT),
};

/*
 * Per-dynamic-UAR record exchanged across migration.
 *
 *   handle:        ufile->idr handle of the MLX5_IB_OBJECT_UAR uobject
 *                  on the source ucontext (== userspace's opaque UAR
 *                  handle). RESTORE pins the destination's uobject at
 *                  this same handle via rdma_alloc_begin_uobject_at_handle
 *                  so libmlx5's captured (handle, mmap_offset) pairs
 *                  remain valid post-restore.
 *
 *   uar_index:     FW UAR id (page_idx). Valid only against a destination
 *                  VHCA whose state was imported via LOAD_VHCA_STATE from
 *                  the source the snapshot was captured on.
 *
 *   mmap_offset:   start_pgoff << PAGE_SHIFT of the rdma_user_mmap_entry
 *                  on the source ucontext (i.e. the offset libmlx5 used
 *                  in its mmap() syscall). RESTORE re-inserts the entry
 *                  on the destination at the same start_pgoff.
 *
 *   alloc_type:    MLX5_IB_UAPI_UAR_ALLOC_TYPE_BF (write-combining,
 *                  blue-flame doorbell page) or
 *                  MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC (uncached). Decides
 *                  the entry's mmap_flag and the prot used when libmlx5
 *                  remaps. v0 only restores these two types -- DEVX-mode
 *                  ucontexts and the MLX5_IB_OBJECT_VAR table are not
 *                  covered by these verbs.
 */
struct mlx5_ib_vfmig_dyn_uar_record {
	__u32	handle;
	__u32	uar_index;
	__aligned_u64 mmap_offset;
	__u8	alloc_type;
	__u8	reserved0[7];
};

#endif
