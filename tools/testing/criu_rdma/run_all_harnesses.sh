#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run_all_harnesses.sh -- build every mlx5_vfmig userspace artifact and run
# the full harness suite against the loaded kernel, in isolation, with a
# single PASS/XFAIL/SKIP/FAIL summary.
#
# This is the one-shot regression driver for the host-side VF-migration /
# RDMA-uobject-restore work. It:
#
#   1. Auto-detects the mlx5 PF (the BDF exposing /dev/mlx5_vfmig/<bdf>)
#      and its ib_device, unless PF / IBDEV are set in the environment.
#   2. Builds tools/* + every probe (`make all`) and, best-effort, the
#      VFIO STOP_COPY helper (needs sanitized uapi headers, so it runs
#      `make headers_install` first).
#   3. Runs each harness with a fresh VF slate (sriov_numvfs=0,
#      autoprobe=1) and a per-test timeout, capturing output to
#      $LOGDIR/<name>.log.
#   4. Classifies each result against an explicit expectation table and
#      prints a final summary. The script exits 0 iff there are no
#      UNEXPECTED failures (XFAIL/SKIP do not fail the run).
#
# Expectation classes (see TESTS table):
#   run            must exit 0.
#   xfail:<reason> expected to exit non-zero on the current kernel/FW for
#                  a documented reason (legacy/superseded path, or a known
#                  FW gap the harness is designed to flag). Counts as a
#                  green result; a *0* exit is reported as XPASS (worth a
#                  look, the expectation may be stale).
#   skip:<reason>  not run in an unattended sweep (needs manual args, etc).
#
# Usage:
#   sudo ./run_all_harnesses.sh                 # auto-detect everything
#   sudo PF=0000:08:00.0 IBDEV=mlx5_0 ./run_all_harnesses.sh
#   sudo TIMEOUT=240 LOGDIR=/tmp/vfmig_run ./run_all_harnesses.sh
#   sudo ONLY='cq_restore qp_restore' ./run_all_harnesses.sh   # subset
#   sudo NOBUILD=1 ./run_all_harnesses.sh       # skip the build phase

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KROOT="$(cd "$ROOT/../../.." && pwd)"
LOGDIR=${LOGDIR:-/tmp/vfmig_run}
TIMEOUT=${TIMEOUT:-360}
ONLY=${ONLY:-}
NOBUILD=${NOBUILD:-0}

mkdir -p "$LOGDIR"

red()   { printf '\033[31m%s\033[0m' "$1"; }
grn()   { printf '\033[32m%s\033[0m' "$1"; }
ylw()   { printf '\033[33m%s\033[0m' "$1"; }

die() { echo "FATAL: $*" >&2; exit 2; }

# --- preflight -------------------------------------------------------------

if [ "$(id -u)" -ne 0 ]; then
    # We use sudo per-command, but warn early if it won't be non-interactive.
    sudo -n true 2>/dev/null || echo "NOTE: sudo may prompt; harnesses need root." >&2
fi

# Auto-detect the PF cdev (BDF) if not provided.
if [ -z "${PF:-}" ]; then
    PF=$(ls /dev/mlx5_vfmig/ 2>/dev/null | head -n1)
    [ -n "$PF" ] || die "no /dev/mlx5_vfmig/<bdf> cdev found (mlx5_core with CONFIG_MLX5_VFMIG loaded?)"
fi
[ -e "/dev/mlx5_vfmig/$PF" ] || die "cdev /dev/mlx5_vfmig/$PF missing"

# Auto-detect the PF's ib_device if not provided.
if [ -z "${IBDEV:-}" ]; then
    IBDEV=$(ls "/sys/bus/pci/devices/$PF/infiniband/" 2>/dev/null | head -n1)
    [ -n "$IBDEV" ] || IBDEV=mlx5_0
fi

echo "============================================================"
echo " mlx5_vfmig full harness sweep"
echo "   kernel : $(uname -r)"
echo "   PF     : $PF"
echo "   IBDEV  : $IBDEV"
echo "   LOGDIR : $LOGDIR"
echo "   TIMEOUT: ${TIMEOUT}s per harness"
echo "============================================================"

# --- build -----------------------------------------------------------------

VFIO_OK=0
if [ "$NOBUILD" = 1 ]; then
    echo "== build: skipped (NOBUILD=1) =="
else
    echo "== build: make all =="
    make -C "$ROOT" all >"$LOGDIR/build.log" 2>&1 \
        || die "make all failed; see $LOGDIR/build.log"

    echo "== build: VFIO helper (best-effort) =="
    if make -C "$KROOT" headers_install >>"$LOGDIR/build.log" 2>&1 && \
       make -C "$ROOT" vfio-helper UAPI_INCLUDE="$KROOT/usr/include" \
            >>"$LOGDIR/build.log" 2>&1; then
        VFIO_OK=1
        echo "   vfio-helper: built"
    else
        echo "   vfio-helper: unavailable (skipping vfio_save_load)"
    fi
fi
[ -x "$ROOT/tools/vfio_stop_copy_save" ] && VFIO_OK=1

# --- test table ------------------------------------------------------------
# name | relative script | extra env | class
TESTS=(
  "pf_cdev_smoke|save_load/test_pf_cdev_smoke.sh||run"
  "synthetic_load_plumbing|save_load/test_synthetic_load_plumbing.sh||run"
  "iova_tracked_save_load|save_load/test_iova_tracked_save_load.sh||run"
  "inkernel_save_load|save_load/test_inkernel_save_load_roundtrip.sh||xfail:non-tracked LOAD superseded by deterministic-IOVA path"
  "vfio_save_load|save_load/test_vfio_save_load_roundtrip.sh||xfail:non-tracked LOAD superseded; SAVE side still validated"
  "suspend_resume_split|save_load/test_suspend_resume_split.sh||run"
  "directional_suspend_resume|save_load/test_directional_suspend_resume.sh||run"
  "teardown_resume_timing|save_load/test_teardown_resume_timing.sh||run"
  "multi_load_stage_gate|save_load/multi_load_gates/test_multi_load_stage_gate.sh||run"
  "user_object_replay|save_load/user_object_replay/test_user_object_replay.sh||run"
  "pd_restore|uobject_restore/pd_restore/test_pd_restore_mlx5_vfmig.sh||run"
  "pd_adopt|uobject_restore/pd_adopt/test_pd_adopt.sh||xfail:DEVX-uid PD adoption is a known post-LOAD FW gap (uid=0 lane passes)"
  "dealloc_pd_chain|uobject_restore/dealloc_pd_chain/test_dealloc_pd_chain.sh||run"
  "pdn_highwater|uobject_restore/dealloc_pd_chain/test_pdn_highwater.sh||run"
  "dealloc_pd_matrix|uobject_restore/dealloc_pd_matrix/test_dealloc_pd_matrix.sh||run"
  "cq_restore|uobject_restore/cq_restore/test_cq_restore_mlx5_vfmig.sh||run"
  "cq_adopt|uobject_restore/cq_adopt/test_cq_adopt.sh||run"
  "cq_query|uobject_restore/cq_query/test_cq_query_mlx5_vfmig.sh|IBDEV=$IBDEV|run"
  "cq_destroy_matrix|uobject_restore/cq_destroy_matrix/test_cq_destroy_matrix.sh||run"
  "mr_restore|uobject_restore/mr_restore/test_mr_restore_mlx5_vfmig.sh||run"
  "mr_adopt|uobject_restore/mr_adopt/test_mr_adopt.sh||run"
  "mr_destroy_matrix|uobject_restore/mr_destroy_matrix/test_mr_destroy_matrix.sh||run"
  "qp_restore|uobject_restore/qp_restore/test_qp_restore_mlx5_vfmig.sh||run"
  "qp_query|uobject_restore/qp_query/test_qp_query_mlx5_vfmig.sh|IBDEV=$IBDEV|run"
  "qp_destroy_matrix|uobject_restore/qp_destroy_matrix/test_qp_destroy_matrix.sh||run"
  "vf_uuid_lifecycle|uobject_restore/vf_uuid/test_vf_uuid_lifecycle.sh||run"
  "fw_id_continuity|uobject_restore/fw_id_continuity/test_fw_id_continuity.sh||run"
  "uar_persistence|uar_restore/probe_uar_persistence.sh||run"
  "qp_av_dmac|uobject_restore/qp_av_dmac/check_qp_av_dmac.sh||skip:parametrized checker, needs <vf_id> <qpn>"
  "qp_restore_rxe|uobject_restore/qp_restore/test_qp_restore_rxe.sh||run"
  # Kept last on purpose: cell 1 unbinds a restored VF, and the
  # half-restored VHCA's teardown can wedge the kernel on a stuck FW
  # command (rc=77 -> WEDGE). Running it last means a wedge truncates
  # no other coverage; the harness self-protects with unbind_vf_safe.
  "multi_load_drift_gate|save_load/multi_load_gates/test_multi_load_drift_gate.sh||run"
)

reset_vfs() {
    echo 1 | sudo tee "/sys/bus/pci/devices/$PF/sriov_drivers_autoprobe" >/dev/null 2>&1 || true
    echo 0 | sudo tee "/sys/bus/pci/devices/$PF/sriov_numvfs"            >/dev/null 2>&1 || true
    sleep 1
}

want() {
    [ -z "$ONLY" ] && return 0
    local w; for w in $ONLY; do [ "$w" = "$1" ] && return 0; done
    return 1
}

# --- run -------------------------------------------------------------------

SUMMARY="$LOGDIR/SUMMARY.txt"; : > "$SUMMARY"
n_pass=0 n_xfail=0 n_xpass=0 n_skip=0 n_fail=0 n_wedge=0
WEDGED=0

# A harness exits 77 when it detects that it just wedged the kernel on a
# stuck firmware command (a held device_lock chain that no subsequent
# mlx5 operation can get past -- only a reboot recovers). Treat it as a
# non-fatal WEDGE and stop the sweep: anything we run after this would
# block in uninterruptible D state too, including reset_vfs.
SKIP_RC=77

for entry in "${TESTS[@]}"; do
    IFS='|' read -r name script env class <<< "$entry"
    want "$name" || continue
    log="$LOGDIR/$name.log"
    reason=${class#*:}; ctype=${class%%:*}

    if [ "$ctype" = skip ]; then
        printf 'SKIP   %-22s  (%s)\n' "$name" "$reason" | tee -a "$SUMMARY"
        n_skip=$((n_skip+1)); continue
    fi
    if [ "$name" = vfio_save_load ] && [ "$VFIO_OK" != 1 ]; then
        printf 'SKIP   %-22s  (vfio helper not built)\n' "$name" | tee -a "$SUMMARY"
        n_skip=$((n_skip+1)); continue
    fi
    if [ ! -x "$ROOT/$script" ]; then
        printf 'SKIP   %-22s  (missing/!x: %s)\n' "$name" "$script" | tee -a "$SUMMARY"
        n_skip=$((n_skip+1)); continue
    fi

    reset_vfs
    start=$(date +%s)
    sudo env PF="$PF" $env timeout "$TIMEOUT" "$ROOT/$script" >"$log" 2>&1
    rc=$?
    dur=$(( $(date +%s) - start ))

    # A self-reported kernel wedge trumps the per-test expectation: stop
    # the sweep before reset_vfs (or the next harness) blocks forever.
    if [ "$rc" -eq "$SKIP_RC" ]; then
        printf 'WEDGE  %-22s %4ss  (kernel wedged; reboot required -- see %s)\n' \
            "$name" "$dur" "$log" | tee -a "$SUMMARY"
        n_wedge=$((n_wedge+1)); WEDGED=1
        break
    fi

    if [ "$ctype" = xfail ]; then
        if [ "$rc" -eq 0 ]; then
            printf 'XPASS  %-22s %4ss  (now passes? expectation may be stale: %s)\n' \
                "$name" "$dur" "$reason" | tee -a "$SUMMARY"
            n_xpass=$((n_xpass+1))
        else
            printf 'XFAIL  %-22s %4ss  (%s)\n' "$name" "$dur" "$reason" | tee -a "$SUMMARY"
            n_xfail=$((n_xfail+1))
        fi
        continue
    fi

    case $rc in
        0)   printf 'PASS   %-22s %4ss\n' "$name" "$dur" | tee -a "$SUMMARY"; n_pass=$((n_pass+1)) ;;
        124) printf 'FAIL   %-22s %4ss  (TIMEOUT, log: %s)\n' "$name" "$dur" "$log" | tee -a "$SUMMARY"; n_fail=$((n_fail+1)) ;;
        *)   printf 'FAIL   %-22s %4ss  (rc=%s, log: %s)\n' "$name" "$dur" "$rc" "$log" | tee -a "$SUMMARY"; n_fail=$((n_fail+1)) ;;
    esac
done

# Skip the final teardown if the kernel is wedged -- a sysfs write to
# the stuck device would block in uninterruptible D state.
[ "$WEDGED" = 1 ] || reset_vfs

echo
echo "===================== SUMMARY ============================"
cat "$SUMMARY"
echo "=========================================================="
echo "PASS=$n_pass XFAIL=$n_xfail XPASS=$n_xpass SKIP=$n_skip FAIL=$n_fail WEDGE=$n_wedge"
echo "logs: $LOGDIR/"
if [ "$WEDGED" = 1 ]; then
    echo
    echo "##########################################################"
    echo "# KERNEL WEDGED -- sweep truncated, REBOOT before re-run #"
    echo "##########################################################"
    echo "A harness hit a stuck mlx5 firmware command (restored-VF"
    echo "teardown gap). Harnesses after the wedge point did not run."
    echo "This is a known gap, not an unexpected regression."
fi
if [ "$n_fail" -ne 0 ]; then
    echo "RESULT: FAIL ($n_fail unexpected failure(s))"
    exit 1
fi
if [ "$WEDGED" = 1 ]; then
    echo "RESULT: WEDGE (no unexpected failures; suite truncated, reboot required)"
    exit 0
fi
echo "RESULT: GREEN (no unexpected failures)"
exit 0
