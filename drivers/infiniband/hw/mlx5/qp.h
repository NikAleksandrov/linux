/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/*
 * Copyright (c) 2013-2020, Mellanox Technologies inc. All rights reserved.
 */

#ifndef _MLX5_IB_QP_H
#define _MLX5_IB_QP_H

struct mlx5_ib_dev;

struct mlx5_qp_table {
	struct notifier_block nb;
	struct xarray dct_xa;

	/* protect radix tree
	 */
	spinlock_t lock;
	struct radix_tree_root tree;
};

int mlx5_init_qp_table(struct mlx5_ib_dev *dev);
void mlx5_cleanup_qp_table(struct mlx5_ib_dev *dev);

int mlx5_core_create_dct(struct mlx5_ib_dev *dev, struct mlx5_core_dct *qp,
			 u32 *in, int inlen, u32 *out, int outlen);
int mlx5_qpc_create_qp(struct mlx5_ib_dev *dev, struct mlx5_core_qp *qp,
		       u32 *in, int inlen, u32 *out);
/*
 * vfmig CRIU restore companion to mlx5_qpc_create_qp. Caller stamps
 * qp->qpn (= source's adopted qpn) and qp->uid before invoking. No
 * FW CREATE_QP round-trip is issued; only the kernel-side qp_table
 * registration runs. See drivers/infiniband/hw/mlx5/qpc.c for the
 * empirical justification (K7 STRONG PASS).
 */
int mlx5_qpc_adopt_qp(struct mlx5_ib_dev *dev, struct mlx5_core_qp *qp);
/*
 * §S6b CRIU restore companion to mlx5_ib_restore_qp -- re-resolve
 * av.dmac on a RoCEv2 user-mode QP whose QPC LOAD'd in with the
 * source's resolved peer-MAC, then issue MODIFY_QP(RTS2RTS_QP,
 * opt=PRIMARY_ADDR_PATH) to write the destination-resolved dmac
 * into the QPC. Empirically justified by the §S6b stale-dmac
 * verdict in tools/testing/mlx5_vfmig/uobject_restore/qp_av_dmac/
 * check_qp_av_dmac.sh; rationale documented in
 * design/qp_av_dmac_swap.md (in particular §5.2's erratum on why
 * mlx5_ib_modify_qp(IB_QP_AV) is the wrong primitive at RTS).
 *
 * Operates entirely in mlx5_core_qp terms (qpn + uid out of qp,
 * port_num + dgid + sgid_index pulled fresh from FW QUERY_QP). The
 * caller is responsible for the ibverbs-level gates (skip non-RC/
 * UC, skip kernel-mode QPs, etc.); see the §10.4 callsite shape in
 * design/qp_av_dmac_swap.md.
 *
 * Skips internally only the cases this helper can detect itself
 * from QUERY_QP output -- in particular, IB-link-layer ports (no
 * L2 dmac to refresh) -- or that surface from the L2 lookup
 * (NUD_INCOMPLETE / missing neighbor entry, mapped to Policy A
 * "log + return 0").
 *
 * Returns 0 on success or skipped; negative errno on real failure
 * (FW transport error, MODIFY_QP rejected, etc.). Caller may treat
 * non-zero as non-fatal: the QP is already adopted and the
 * connection is no worse off than the pre-fix behavior.
 */
int mlx5_ib_restore_qp_refresh_av_dmac(struct mlx5_ib_dev *dev,
				       struct mlx5_core_qp *qp);
int mlx5_core_qp_modify(struct mlx5_ib_dev *dev, u16 opcode, u32 opt_param_mask,
			void *qpc, struct mlx5_core_qp *qp, u32 *ece);
int mlx5_core_destroy_qp(struct mlx5_ib_dev *dev, struct mlx5_core_qp *qp);
int mlx5_core_destroy_dct(struct mlx5_ib_dev *dev, struct mlx5_core_dct *dct);
int mlx5_core_qp_query(struct mlx5_ib_dev *dev, struct mlx5_core_qp *qp,
		       u32 *out, int outlen, bool qpc_ext);
int mlx5_core_dct_query(struct mlx5_ib_dev *dev, struct mlx5_core_dct *dct,
			u32 *out, int outlen);

int mlx5_core_set_delay_drop(struct mlx5_ib_dev *dev, u32 timeout_usec);

int mlx5_core_destroy_rq_tracked(struct mlx5_ib_dev *dev,
				 struct mlx5_core_qp *rq);
int mlx5_core_create_sq_tracked(struct mlx5_ib_dev *dev, u32 *in, int inlen,
				struct mlx5_core_qp *sq);
void mlx5_core_destroy_sq_tracked(struct mlx5_ib_dev *dev,
				  struct mlx5_core_qp *sq);

int mlx5_core_create_rq_tracked(struct mlx5_ib_dev *dev, u32 *in, int inlen,
				struct mlx5_core_qp *rq);

struct mlx5_core_rsc_common *mlx5_core_res_hold(struct mlx5_ib_dev *dev,
						int res_num,
						enum mlx5_res_type res_type);
void mlx5_core_res_put(struct mlx5_core_rsc_common *res);

int mlx5_core_xrcd_alloc(struct mlx5_ib_dev *dev, u32 *xrcdn);
int mlx5_core_xrcd_dealloc(struct mlx5_ib_dev *dev, u32 xrcdn);
int mlx5_ib_qp_set_counter(struct ib_qp *qp, struct rdma_counter *counter);
int mlx5_ib_qp_event_init(void);
void mlx5_ib_qp_event_cleanup(void);
int mlx5r_ib_rate(struct mlx5_ib_dev *dev, u8 rate);
#endif /* _MLX5_IB_QP_H */
