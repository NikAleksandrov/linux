#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S6b B5 dump-side validator harness. Drives the
# MLX5_IB_METHOD_VFMIG_QUERY_QP ioctl exerciser
# (qp_query_probe_mlx5_vfmig) against the chosen mlx5 ib_device
# and reports STRONG PASS / FAIL. Mirror of
# test_cq_query_mlx5_vfmig.sh -- see
# tools/testing/criu_rdma/design/uobject_restore.md §5.3.4.
#
# Like the CQ counterpart, this harness does NOT orchestrate
# SAVE_VHCA_STATE / LOAD_VHCA_STATE: QUERY_QP is a pure-
# introspection verb that runs in any process holding a uverbs
# fd, regardless of whether the underlying VF is vfmig-tracked.
# The kernel-side fields it reads
# (qp->trans_qp.base.{mqp.qpn, ubuffer.umem->address},
# mlx5_ib_db_user_virt(&qp->db), qp->{sq,rq}.{wqe_cnt}, qp->
# rq.wqe_shift, qp->flags_en, qp->flags, the
# ib_uobject->user_handle via ib_qp_user_handle()) exist on
# every user-mode QP on every mlx5 ib_device. cap / qp_type /
# qp_state are no longer part of the verb -- CRIU sources those
# from the standard query_qp + NLDEV. The probe is its
# own pass-criterion validator: it allocates real QPs via
# libibverbs, reads view (A) via mlx5dv_init_obj(MLX5DV_OBJ_QP),
# reads view (B) via the new ioctl, and asserts strict byte-
# equality on every overlapping field. STRONG PASS == probe
# exits 0 with "ALL SUBTESTS PASS" on stderr.
#
# Subtests the probe runs internally:
#   1. happy path     -- single RC QP, RESET -> INIT, byte-equal blob+scalars
#   2. invalid handle -- HANDLE=0xdeadbeef must -ENOENT
#   3. multi-QP       -- two distinct QPs, each query returns its own fields
#
# Usage:
#   sudo ./test_qp_query_mlx5_vfmig.sh                  # mlx5_0 default
#   sudo IBDEV=mlx5_2 ./test_qp_query_mlx5_vfmig.sh
#
# The harness does not require root strictly speaking, but
# uverbs ops typically need it on production hosts.

set -euo pipefail

IBDEV="${IBDEV:-mlx5_0}"

HERE="$(cd "$(dirname "$0")" && pwd)"
PROBE="${HERE}/qp_query_probe_mlx5_vfmig"

if [[ ! -x "${PROBE}" ]]; then
	echo "build: missing ${PROBE} -- did you run \`make\` in tools/testing/criu_rdma?" >&2
	exit 2
fi

if ! command -v ibv_devices >/dev/null 2>&1; then
	# best-effort warning; the probe will fail with a clear ibdev-not-found if needed
	echo "warn: ibv_devices not in PATH; cannot pre-validate ${IBDEV} exists." >&2
fi

echo "test_qp_query: IBDEV=${IBDEV}"
echo "test_qp_query: probe=${PROBE}"

if "${PROBE}" "${IBDEV}"; then
	echo "test_qp_query: VERDICT STRONG PASS"
	exit 0
fi

echo "test_qp_query: VERDICT FAIL" >&2
exit 1
