# SPDX-License-Identifier: GPL-2.0
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter

# Userspace tools that talk to /dev/mlx5_vfmig/* or to the mlx5_ib
# vendor-verbs ABI. Built by default by `make`.
TOOL_BINARIES :=					\
	tools/mlx5_vfmig				\
	tools/synthetic_blob_emit			\
	tools/ucontext_vendor_verbs

# Empirical probes for the various restore designs (see design/).
# Built by default; depend on libibverbs (and libmlx5 for the FW-id
# continuity probe).
PROBE_BINARIES :=						\
	uobject_restore/info_handles/info_handles_probe		\
	uobject_restore/fw_id_continuity/fw_id_continuity_probe	\
	uobject_restore/nldev_res_handle/nldev_res_handle_probe	\
	uobject_restore/pd_restore/pd_restore_probe_rxe		\
	uobject_restore/pd_restore/pd_restore_probe_mlx5_vfmig	\
	uobject_restore/mr_restore/mr_restore_probe_rxe	\
	uobject_restore/mr_restore/mr_restore_probe_mlx5_vfmig		\
	uobject_restore/cq_restore/cq_restore_probe_rxe		\
	uobject_restore/cq_restore/cq_restore_probe_mlx5_vfmig	\
	uobject_restore/cq_query/cq_query_probe_mlx5_vfmig	\
	uobject_restore/pd_query/pd_query_probe_mlx5_vfmig	\
	uobject_restore/qp_restore/qp_restore_probe_mlx5_vfmig	\
	uobject_restore/qp_restore/qp_restore_probe_rxe		\
	uobject_restore/qp_query/qp_query_probe_mlx5_vfmig	\
	uobject_restore/qp_query/qp_query_probe_rxe		\
	uobject_restore/qp_query/qp_nlattr_probe		\
	uobject_restore/vf_uuid/vf_uuid_probe_mlx5_vfmig	\
	save_load/multi_load_gates/multi_load_stage_gate_probe	\
	save_load/user_object_replay/user_object_replay_probe

# Optional VFIO STOP_COPY helper. Opt-in via `make vfio-helper`
# because it pulls in <linux/iommufd.h> + <linux/vfio.h> which require
# sanitized userspace headers (typically installed via
# `make headers_install INSTALL_HDR_PATH=...`).
VFIO_BINARIES := tools/vfio_stop_copy_save

# Where to find sanitized userspace versions of <linux/iommufd.h> and
# <linux/vfio.h>. Override on the command line if you have installed
# them somewhere other than the system default, e.g.:
#   make vfio-helper UAPI_INCLUDE=/opt/linux-headers/usr/include
UAPI_INCLUDE ?=

all: $(TOOL_BINARIES) $(PROBE_BINARIES)

vfio-helper: $(VFIO_BINARIES)

tools/mlx5_vfmig: tools/mlx5_vfmig.c ../../../include/uapi/linux/mlx5_vfmig.h
	$(CC) $(CFLAGS) -o $@ $<

tools/synthetic_blob_emit: tools/synthetic_blob_emit.c
	$(CC) $(CFLAGS) -o $@ $<

tools/ucontext_vendor_verbs: tools/ucontext_vendor_verbs.c
	$(CC) $(CFLAGS) -o $@ $<

# FW-id continuity probe (design/uobject_restore.md §8.2 + §6.3).
# Pulls in libibverbs + libmlx5 (mlx5dv) -- present on any host with
# rdma-core installed.
uobject_restore/fw_id_continuity/fw_id_continuity_probe: \
		uobject_restore/fw_id_continuity/fw_id_continuity_probe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs -lmlx5

# INFO_HANDLES exerciser (design/uobject_restore.md §7.2).
# libibverbs only (no mlx5dv).
uobject_restore/info_handles/info_handles_probe: \
		uobject_restore/info_handles/info_handles_probe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

# NLDEV RES_HANDLE attribute probe (design/uobject_restore.md §7.5.1).
# Raw NETLINK_RDMA dump + libibverbs alloc; no driver-private deps.
uobject_restore/nldev_res_handle/nldev_res_handle_probe: \
		uobject_restore/nldev_res_handle/nldev_res_handle_probe.c \
		../../../include/uapi/rdma/rdma_netlink.h
	$(CC) $(CFLAGS) -o $@ $< -libverbs

# RESTORE_PD + rxe_restore_pd S3a probe (design/uobject_restore.md §9.4).
# libibverbs only used for ibdev->uverbs cdev mapping; the GET_CONTEXT
# udata path needs the rxe RXE_ALLOC_UCTX_RESTORE_MODE flag which
# librxe has no plumbing for, so the probe opens /dev/infiniband/uverbsN
# directly and issues raw write/ioctl. UAPI structs are inlined in the
# probe source (system rdma-core uapi lags the in-tree kernel headers
# for the new rxe_alloc_ucontext_req).
uobject_restore/pd_restore/pd_restore_probe_rxe: \
		uobject_restore/pd_restore/pd_restore_probe_rxe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

# RESTORE_PD + mlx5_ib_restore_pd S3b end-to-end probe
# (design/uobject_restore.md §9.1 S3b). Same shape as the rxe S3a
# probe: libibverbs only for ibdev->uverbs cdev mapping, then we
# open /dev/infiniband/uverbsN directly so we can drive the
# MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE flag in GET_CONTEXT udata and
# pass the new struct mlx5_ib_restore_pd_req as the RESTORE_PD UHW
# blob. Both are inlined in the probe; libmlx5 has no plumbing for
# either today.
uobject_restore/pd_restore/pd_restore_probe_mlx5_vfmig: \
		uobject_restore/pd_restore/pd_restore_probe_mlx5_vfmig.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

# RESTORE_MR + rxe_restore_mr S4a probe (design/uobject_restore.md §9.5).
# Same libibverbs-only / raw-uverbs pattern as pd_restore_probe_rxe; UAPI
# bits inlined in the source for the same reason (system rdma-core uapi
# lags the in-tree kernel for the new RESTORE_MR attrs).
uobject_restore/mr_restore/mr_restore_probe_rxe: \
		uobject_restore/mr_restore/mr_restore_probe_rxe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

# RESTORE_MR + mlx5_ib_restore_mr S4b end-to-end probe
# (design/uobject_restore.md §9.1 S4b). Mirrors
# pd_restore_probe_mlx5_vfmig structurally: libibverbs only for
# ibdev->uverbs cdev mapping, then raw write/ioctl on the cdev so we
# can drive MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE in GET_CONTEXT udata and
# pass struct mlx5_ib_restore_mr_req as the RESTORE_MR UHW blob.
# Both are inlined in the probe; libmlx5 has no plumbing for either.
uobject_restore/mr_restore/mr_restore_probe_mlx5_vfmig: \
		uobject_restore/mr_restore/mr_restore_probe_mlx5_vfmig.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

# RESTORE_CQ + rxe_restore_cq S5a probe (design/uobject_restore.md §9.7).
# Same libibverbs-only / raw-uverbs pattern as pd_restore_probe_rxe:
# library is used only for ibdev->uverbs cdev mapping; the GET_CONTEXT
# udata path needs the rxe RXE_ALLOC_UCTX_RESTORE_MODE flag, plus the
# probe issues a raw RESTORE_CQ ioctl with FD-class attrs (COMP_CHANNEL),
# none of which librxe / libibverbs has plumbing for. UAPI ids inlined
# in the source for the same reason as the other rxe restore probes.
uobject_restore/cq_restore/cq_restore_probe_rxe: \
		uobject_restore/cq_restore/cq_restore_probe_rxe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

# RESTORE_CQ + mlx5_ib_restore_cq S5b end-to-end probe
# (design/uobject_restore.md §9.1 S5b). Mirrors
# mr_restore_probe_mlx5_vfmig structurally: libibverbs only for
# ibdev->uverbs cdev mapping, then raw write/ioctl on the cdev so we
# can drive MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE in GET_CONTEXT udata and
# pass struct mlx5_ib_restore_cq_req as the RESTORE_CQ UHW blob.
# Both are inlined in the probe; libmlx5 has no plumbing for either.
uobject_restore/cq_restore/cq_restore_probe_mlx5_vfmig: \
		uobject_restore/cq_restore/cq_restore_probe_mlx5_vfmig.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

# QUERY_CQ + mlx5_ib QUERY_CQ dump-side probe
# (design/uobject_restore.md §5.2.4). Single-process byte-equality
# validator: libibverbs+libmlx5 to create a real CQ and read the
# (A) "calling-process VA" view via mlx5dv_init_obj, then raw uverbs
# ioctl to drive MLX5_IB_METHOD_VFMIG_QUERY_CQ for the (B) kernel
# view. Asserts strict equality across cqn / cqe_size / buf_addr /
# (db_addr & PAGE_MASK) / cqe / comp_vector / flags so a CRIU dumper
# can substitute the QUERY_CQ ioctl for mlx5dv_init_obj across the
# process boundary.
uobject_restore/cq_query/cq_query_probe_mlx5_vfmig: \
		uobject_restore/cq_query/cq_query_probe_mlx5_vfmig.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs -lmlx5

# QUERY_PD + mlx5_ib QUERY_PD dump-side probe
# (design/uobject_restore.md §5.1.4). Single-process byte-equality
# validator: libibverbs+libmlx5 to alloc a real PD and read the
# (A) "FW pdn" view via mlx5dv_init_obj(MLX5DV_OBJ_PD), then raw
# uverbs ioctl to drive MLX5_IB_METHOD_VFMIG_QUERY_PD for the (B)
# kernel view. Asserts blob.pdn == dvpd.pdn and resp_uid == 0 (the
# v0 host-privileged lane) so a CRIU dumper can substitute the
# QUERY_PD ioctl for the NLDEV fw_pdn driver-TLV path it replaced.
uobject_restore/pd_query/pd_query_probe_mlx5_vfmig: \
		uobject_restore/pd_query/pd_query_probe_mlx5_vfmig.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs -lmlx5

# RESTORE_QP + mlx5_ib_restore_qp S6b end-to-end probe
# (design/uobject_restore.md §9.1 S6b). Mirrors
# cq_restore_probe_mlx5_vfmig structurally: libibverbs only for
# ibdev->uverbs cdev mapping, then raw write/ioctl on the cdev so
# we can drive MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE in GET_CONTEXT
# udata, perform the RESTORE_PD + RESTORE_CQ setup chain, then
# pass struct mlx5_ib_restore_qp_req as the RESTORE_QP UHW blob.
# All UAPI bits inlined in the probe; libmlx5 has no plumbing.
uobject_restore/qp_restore/qp_restore_probe_mlx5_vfmig: \
		uobject_restore/qp_restore/qp_restore_probe_mlx5_vfmig.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

# QUERY_QP + mlx5_ib_handler S6b B5 dump-side probe
# (design/uobject_restore.md §5.3.4). Mirrors cq_query_probe_mlx5_vfmig:
# create QPs via libibverbs, get the (A) "calling-process VA" view via
# mlx5dv_init_obj(MLX5DV_OBJ_QP), then raw uverbs ioctl to drive
# MLX5_IB_METHOD_VFMIG_QUERY_QP for the (B) kernel view. Asserts
# strict equality across qpn / sq_wqe_count / rq_wqe_count /
# rq_wqe_shift / buf_addr / (db_addr & PAGE_MASK) / type / state /
# create_flags so a CRIU dumper can substitute the QUERY_QP ioctl
# for mlx5dv_init_obj across the process boundary.
uobject_restore/qp_query/qp_query_probe_mlx5_vfmig: \
		uobject_restore/qp_query/qp_query_probe_mlx5_vfmig.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs -lmlx5

# QUERY_QP + FREEZE_DATAPATH rxe dump-side probe (S6a A4,
# design/uobject_restore.md §5.3.7). Single-process: libibverbs builds a
# real RC QP self-loopback at RTS, then raw uverbs ioctl drives
# RXE_IB_METHOD_VFMIG_QUERY_QP (field fidelity vs ibv_query_qp) and the
# RXE_IB_METHOD_VFMIG_FREEZE_DATAPATH freeze/resume lifecycle. rxe object
# ids inlined in the source (system rdma-core uapi has no rxe driver
# namespace). No restore-mode ucontext needed.
uobject_restore/qp_query/qp_query_probe_rxe: \
		uobject_restore/qp_query/qp_query_probe_rxe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

# RESTORE_QP + rxe_restore_qp S6a A5 end-to-end probe
# (design/uobject_restore.md §9.1 S6a). Builds an RC QP via raw uverbs,
# QUERY_QP-snapshots it, FREEZE_DATAPATHs + destroys the source, then on
# a RXE_ALLOC_UCTX_RESTORE_MODE ucontext replays RESTORE_PD + RESTORE_CQ
# + RESTORE_QP at the source qpn from the snapshot and re-QUERY_QPs the
# restored QP to assert byte-equality. UAPI ids inlined for the same
# reason as the other rxe restore probes.
uobject_restore/qp_restore/qp_restore_probe_rxe: \
		uobject_restore/qp_restore/qp_restore_probe_rxe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

# Raw NLDEV netlink dumper for QP + CQ resource entries
# (design/uobject_restore.md §5.3.4). Validation tool for the
# RDMA_NLDEV_ATTR_RES_SEND_CQN / _RECV_CQN attrs the QP entry now
# carries -- iproute2's `rdma resource show qp` predates these ids
# and silently skips them, so this probe sends a raw
# RDMA_NLDEV_CMD_RES_QP_GET (and RDMA_NLDEV_CMD_RES_CQ_GET) on a
# NETLINK_RDMA socket and walks every nested attribute to prove the
# kernel emits both ids on the wire and that the join-by-id-equality
# QP -> CQ contract holds. No driver-private deps; uses in-tree
# rdma-core UAPI ids via -isystem on include/uapi.
uobject_restore/qp_query/qp_nlattr_probe: \
		uobject_restore/qp_query/qp_nlattr_probe.c \
		../../../include/uapi/rdma/rdma_netlink.h
	$(CC) $(CFLAGS) -isystem ../../../include/uapi -o $@ $<

# KS7.3 SET_VF_UUID + extended QUERY_VF kernel-matrix probe
# (design/vf_prerestore_split.md §3.5). Single-process matrix runner
# that exercises every kernel cell of the orchestrator-owned per-VF
# UUID surface that does NOT need an SR-IOV teardown / re-enable
# cycle (the lifecycle teardown cells live in the accompanying shell
# wrapper since they need sysfs writes). Pure cdev ioctls; no
# libibverbs / libmlx5 deps.
uobject_restore/vf_uuid/vf_uuid_probe_mlx5_vfmig: \
		uobject_restore/vf_uuid/vf_uuid_probe_mlx5_vfmig.c \
		../../../include/uapi/linux/mlx5_vfmig.h
	$(CC) $(CFLAGS) -o $@ $<

# Multi-LOAD staging-gate probe (design/vf_prerestore_split.md §3.5,
# "Empirical multi-LOAD validation"). Single-process kernel-matrix
# runner that exercises the per-vf_id LOAD-session busy gate
# (vfmig_vf_id_busy_locked): concurrent fds, gate-clears-on-close,
# and per-vf_id isolation. Pure cdev ioctls; no libibverbs / libmlx5
# deps; never writes blob bytes so no FW interaction. The companion
# wrapper test_multi_load_stage_gate.sh handles SR-IOV provisioning
# and enable_migratable.
save_load/multi_load_gates/multi_load_stage_gate_probe: \
		save_load/multi_load_gates/multi_load_stage_gate_probe.c \
		../../../include/uapi/linux/mlx5_vfmig.h
	$(CC) $(CFLAGS) -o $@ $<

# user_mr_dma stage-2 success-criterion probe
# (design/user_mr_dma.md section 6.4). Allocates a configurable set of
# user-mode uobjects (PD, N MRs, CQ, QP, SRQ best-effort) whose backing
# buffers flow through vfmig_dma_ops on a tracked VF, then holds them
# across SAVE via stdin "quit" handshake. The accompanying shell
# harness (test_user_object_replay.sh) drives SAVE, dest LOAD, and
# the MLX5_VFMIG_IOC_QUERY_AWAITING_BIND ioctl, then asserts the
# placeholder count matches the source-side registration tally.
save_load/user_object_replay/user_object_replay_probe: \
		save_load/user_object_replay/user_object_replay_probe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs -lmlx5

tools/vfio_stop_copy_save: tools/vfio_stop_copy_save.c
	$(CC) $(CFLAGS) $(if $(UAPI_INCLUDE),-I$(UAPI_INCLUDE),) -o $@ $<

clean:
	rm -f $(TOOL_BINARIES) $(PROBE_BINARIES) $(VFIO_BINARIES)

.PHONY: all clean vfio-helper
