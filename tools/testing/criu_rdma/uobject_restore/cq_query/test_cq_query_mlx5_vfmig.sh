#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S5b B5 dump-side validator harness. Drives the
# MLX5_IB_METHOD_VFMIG_QUERY_CQ ioctl exerciser
# (cq_query_probe_mlx5_vfmig) against the chosen mlx5 ib_device
# and reports STRONG PASS / FAIL. See
# tools/testing/criu_rdma/design/uobject_restore.md §5.2.4.
#
# Unlike test_cq_restore_mlx5_vfmig.sh this harness does NOT
# orchestrate SAVE_VHCA_STATE / LOAD_VHCA_STATE: QUERY_CQ is a
# pure-introspection verb that runs in any process holding a
# uverbs fd, regardless of whether the underlying VF is
# vfmig-tracked. The kernel-side fields it reads (mcq->mcq.cqn,
# mcq->buf.umem->address, mlx5_ib_db_user_virt(&mcq->db),
# mcq->cqe_size, ibcq->cqe, mcq->mcq.vector, mcq->create_flags)
# exist on every user-mode CQ on every mlx5 ib_device. The probe
# is its own pass-criterion validator: it allocates real CQs via
# libibverbs, reads view (A) via mlx5dv_init_obj(MLX5DV_OBJ_CQ),
# reads view (B) via the new ioctl, and asserts strict byte-
# equality on every overlapping field. STRONG PASS == probe exits
# 0 with "ALL SUBTESTS PASS" on stderr.
#
# Usage:
#   sudo ./test_cq_query_mlx5_vfmig.sh                  # mlx5_0 default
#   sudo IBDEV=mlx5_2 ./test_cq_query_mlx5_vfmig.sh
#   sudo ENTRIES=64 COMP_VECTOR=1 ./test_cq_query_mlx5_vfmig.sh
#
# The harness does not require root strictly speaking, but
# uverbs ops typically need it on production hosts.

set -euo pipefail

IBDEV="${IBDEV:-mlx5_0}"
ENTRIES="${ENTRIES:-16}"
COMP_VECTOR="${COMP_VECTOR:-0}"

HERE="$(cd "$(dirname "$0")" && pwd)"
PROBE="${HERE}/cq_query_probe_mlx5_vfmig"

if [[ ! -x "${PROBE}" ]]; then
	echo "build: missing ${PROBE} -- did you run \`make\` in tools/testing/criu_rdma?" >&2
	exit 2
fi

if ! command -v ibv_devices >/dev/null 2>&1; then
	# best-effort warning; the probe will fail with a clear ibdev-not-found if needed
	echo "warn: ibv_devices not in PATH; cannot pre-validate ${IBDEV} exists." >&2
fi

echo "test_cq_query: IBDEV=${IBDEV} ENTRIES=${ENTRIES} COMP_VECTOR=${COMP_VECTOR}"
echo "test_cq_query: probe=${PROBE}"

if "${PROBE}" "${IBDEV}" "${ENTRIES}" "${COMP_VECTOR}"; then
	echo "test_cq_query: VERDICT STRONG PASS"
	exit 0
fi

echo "test_cq_query: VERDICT FAIL" >&2
exit 1
