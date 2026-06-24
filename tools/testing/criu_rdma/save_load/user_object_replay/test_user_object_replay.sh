#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# user_mr_dma stage-2 success-criterion harness.
#
# Drives the empirical chain that validates the stage-2 wire path:
#
#   Source side: register N user-mode uobjects on a tracked VF so
#                vfmig_dma_ops.map_sg installs one external entry per
#                MR / CQ / QP / SRQ umem + per DBR pgdir page. Once
#                C6-C10 land each entry is retagged with
#                VFMIG_HUOBJ_KEY(kind, fw_id). SAVE_VHCA_STATE iterates
#                the external entries (C4) and emits one
#                HOST_USER_PAGE wire record per retagged entry.
#
#   Destination side: a fresh tracked VF with no driver bound. LOAD
#                parses the HOST_USER_PAGE records (C5) and calls
#                vfmig_iova_replay_external() to pre-install one
#                awaiting_bind=true placeholder per record.
#                MLX5_VFMIG_IOC_QUERY_AWAITING_BIND counts them.
#
# Verdict: observed (from ioctl) == expected, where the expected
# tally is the probe's view of what it actually created. The probe
# emits expected_mr / expected_cq / expected_qp / expected_srq /
# expected_dbr_min lines as part of its READY-time manifest, derived
# at runtime from whether each create succeeded -- so srq_ok=1 yields
# expected_srq=1 while srq_ok=0 yields expected_srq=0, and the harness
# self-calibrates against either outcome with no caller intervention.
#
# EXPECT_*_COUNT env vars still override on the command line for
# negative-control / regression runs (e.g. asserting everything stays
# at 0 on a kernel that hasn't landed C6..C10). On a current kernel
# the typical invocation is just:
#
#   sudo PF=0000:08:00.0 ./test_user_object_replay.sh
#
# and the harness expects total = N_MR + N_CQ + N_QP + N_SRQ + N_DBR
# matching what the probe reports.
#
# Optional knobs:
#   BLOB       SAVE blob path (default /tmp/user_object_replay.blob).
#   NUM_MRS    How many MRs the probe registers (default 4).
#   TOOL       mlx5_vfmig CLI (default $ROOT_DIR/tools/mlx5_vfmig).
#   PROBE      user_object_replay_probe (default $SCRIPT_DIR/...).
#   EXPECT_*_COUNT  Override the auto-calibrated value for any of MR,
#                   CQ, QP, SRQ, DBR. Useful for negative controls.
#   EXPECT_TOTAL  If set, overrides the sum of the per-kind expectations.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$SCRIPT_DIR/user_object_replay_probe}
BLOB=${BLOB:-/tmp/user_object_replay.blob}
NUM_MRS=${NUM_MRS:-4}

# EXPECT_*_COUNT defaults are deferred to after Phase B captures the
# probe's manifest, so they pick up the probe-emitted expected_*
# values automatically. Env vars provided here on the command line
# still win via the standard ${VAR:-default} expansion below. The
# expected_* counts the probe emits reflect what it actually created
# in the current run -- e.g. srq_ok=1 yields expected_srq=1, srq_ok=0
# yields expected_srq=0 -- so the harness self-calibrates against the
# wire+ioctl reality on every invocation. Hard-coded baselines in env
# vars are only needed for negative-control runs (e.g. assert
# everything stays at 0 on a kernel that hasn't landed C6..C10 yet).

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]  || { echo "build $TOOL first: make -C $ROOT_DIR";  exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first: make -C $ROOT_DIR"; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

WORKDIR=$(mktemp -d -t uor.XXXXXX)
cleanup() {
    if [ -n "${SRC_PROBE_PID:-}" ] && kill -0 "$SRC_PROBE_PID" 2>/dev/null; then
        echo "quit" > "$WORKDIR/src.in" 2>/dev/null || true
        wait "$SRC_PROBE_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

# --- helpers (mirror test_pd_adopt.sh / test_fw_id_continuity.sh) ----

vf_path()       { echo "/sys/bus/pci/devices/$1"; }
wait_for_path() { for i in $(seq 1 50); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }

bind_vf_safe() {
    local vf=$1
    local timeout=${2:-60}
    local label=${3:-bind}
    local started bg
    started=$(date +%s)

    ( echo "$vf" | sudo tee /sys/bus/pci/drivers/mlx5_core/bind ) \
        >/dev/null 2>&1 &
    bg=$!

    while :; do
        if [ -e "$(vf_path $vf)/driver" ]; then
            wait "$bg" 2>/dev/null || true
            return 0
        fi
        if ! kill -0 "$bg" 2>/dev/null; then
            sleep 0.2
            if [ -e "$(vf_path $vf)/driver" ]; then
                return 0
            fi
            return 1
        fi
        if [ $(( $(date +%s) - started )) -ge "$timeout" ]; then
            echo "ERROR: $label has been pending for ${timeout}s; probe wedged." >&2
            return 2
        fi
        sleep 0.5
    done
}

find_ib_dev_for_pci() {
    local bdf=$1
    local d pci
    for d in /sys/class/infiniband/*; do
        [ -e "$d/device" ] || continue
        pci=$(basename "$(readlink "$d/device")")
        if [ "$pci" = "$bdf" ]; then
            basename "$d"
            return 0
        fi
    done
    return 1
}

# Start user_object_replay_probe in the background hooked to FIFOs.
# Drains READY then captures key=value lines into src_<key>.
start_src_probe() {
    local ibdev=$1
    local num_mrs=$2
    local fifo_in="$WORKDIR/src.in"
    local fifo_out="$WORKDIR/src.out"

    mkfifo "$fifo_in" "$fifo_out"
    echo "=== source probe: $PROBE $ibdev --num-mrs $num_mrs ==="
    sudo "$PROBE" "$ibdev" --num-mrs "$num_mrs" \
        < "$fifo_in" > "$fifo_out" 2>&1 &
    SRC_PROBE_PID=$!
    exec 7> "$fifo_in"

    local line
    while IFS= read -r line < "$fifo_out"; do
        echo "  [src probe] $line"
        case "$line" in
            READY) return 0 ;;
            *=*)
                local k="${line%%=*}"
                local v="${line#*=}"
                eval "src_${k}=\"\$v\""
                ;;
        esac
    done
    echo "  [src probe] EOF before READY -- probe failed"
    return 1
}

quit_src_probe() {
    echo "quit" >&7; exec 7>&-
    wait "$SRC_PROBE_PID" 2>/dev/null || true
    SRC_PROBE_PID=""
    echo "  [src probe] exited"
}

# --- Phase A: provision SOURCE VF -----------------------------------

echo "=== Phase A: provision SOURCE VF ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null

wait_for_path "$(vf_path $PF)/virtfn0"
SRC_VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "source VF: $SRC_VF"

sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0

echo mlx5_core | sudo tee "$(vf_path $SRC_VF)/driver_override" >/dev/null
bind_vf_safe "$SRC_VF" 60 "Phase A: source VF bind"
sleep 1
SRC_IBDEV=$(find_ib_dev_for_pci "$SRC_VF") || \
    { echo "FAIL: no ibdev for source $SRC_VF"; exit 1; }
echo "source ibdev: $SRC_IBDEV"

# --- Phase B: source probe ------------------------------------------

echo "=== Phase B: source probe (registers $NUM_MRS MRs + CQ + QP +/- SRQ) ==="
sudo dmesg -C
start_src_probe "$SRC_IBDEV" "$NUM_MRS"

# Quick sanity dump of what the probe registered.
echo "captured manifest:"
echo "  src_pdn         = ${src_pdn:-?}"
echo "  src_num_mrs     = ${src_num_mrs:-?}"
echo "  src_cqn         = ${src_cqn:-?}"
echo "  src_qpn         = ${src_qpn:-?}"
echo "  src_srqn        = ${src_srqn:-?}"
echo "  src_expected_mr = ${src_expected_mr:-?}"
echo "  src_expected_cq = ${src_expected_cq:-?}"
echo "  src_expected_qp = ${src_expected_qp:-?}"
echo "  src_expected_srq= ${src_expected_srq:-?}"

# Self-calibrate expectations against the probe-emitted manifest.
# Env-var overrides on the harness command line still win (the inner
# ${EXPECT_X:-...} expansion preserves any value already set in the
# environment); only unset variables default to the probe's view.
EXPECT_MR_COUNT=${EXPECT_MR_COUNT:-${src_expected_mr:-0}}
EXPECT_CQ_COUNT=${EXPECT_CQ_COUNT:-${src_expected_cq:-0}}
EXPECT_QP_COUNT=${EXPECT_QP_COUNT:-${src_expected_qp:-0}}
EXPECT_SRQ_COUNT=${EXPECT_SRQ_COUNT:-${src_expected_srq:-0}}
EXPECT_DBR_COUNT=${EXPECT_DBR_COUNT:-${src_expected_dbr_min:-0}}

# --- Phase C: SAVE --------------------------------------------------

echo "=== Phase C: SAVE_VHCA_STATE on source ==="
# snapshot-ordering: pause datapath (CRIU CHECKPOINT_DEVICES), then
# capture (SAVE is suspend-aware and skips its own suspend), then resume.
sudo "$TOOL" "$PF" suspend_vhca 0
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" resume_vhca 0
sudo chmod 0644 "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
echo "saved $SAVE_BYTES bytes to $BLOB"
[ "$SAVE_BYTES" -gt 16 ] || { echo "FAIL: blob suspiciously small: $SAVE_BYTES"; exit 1; }

# --- Phase D: quit probe + tear down source -------------------------

echo "=== Phase D: quit source probe + tear down source VF ==="
quit_src_probe
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null

# --- Phase E: provision DEST VF + LOAD (no bind needed) -------------

echo "=== Phase E: provision DEST VF + LOAD ==="
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
wait_for_path "$(vf_path $PF)/virtfn0"
DST_VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "dest VF: $DST_VF"

# set_tracked creates the per-VF unmanaged iommu_domain. LOAD's
# HOST_USER_PAGE replay arc (C5) populates awaiting_bind placeholders
# in this domain. The harness's ioctl query reads them back. No
# driver bind happens on the destination -- this lets us measure
# the placeholder count BEFORE any user-mode RESTORE_X verb has a
# chance to consume them (stage 3's responsibility, lands later).
sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0

sudo dmesg -C
sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"

# --- Phase F: query awaiting_bind on dest ---------------------------

echo "=== Phase F: QUERY_AWAITING_BIND on destination ==="
QUERY_OUT=$(sudo "$TOOL" "$PF" query_awaiting_bind 0 2>&1)
echo "$QUERY_OUT" | sed 's/^/  /'
while IFS= read -r line; do
    case "$line" in
        *=*)
            k="${line%%=*}"; v="${line#*=}"
            eval "obs_${k}=\"\$v\""
            ;;
    esac
done <<<"$QUERY_OUT"

# --- Verdict --------------------------------------------------------

# Sum the per-kind expectations into a default total. Allow
# EXPECT_TOTAL override for the corner case where the user wants to
# assert the aggregate without specifying per-kind.
expected_total_default=$(( EXPECT_MR_COUNT + EXPECT_CQ_COUNT +
                           EXPECT_QP_COUNT + EXPECT_SRQ_COUNT +
                           EXPECT_DBR_COUNT ))
EXPECT_TOTAL=${EXPECT_TOTAL:-$expected_total_default}

obs_total=${obs_total:-?}
obs_mr=${obs_by_kind_MR:-?}
obs_cq=${obs_by_kind_CQ:-?}
obs_qp=${obs_by_kind_QP:-?}
obs_srq=${obs_by_kind_SRQ:-?}
obs_dbr=${obs_by_kind_DBR:-?}

overall_rc=0
fail_one() { echo "  FAIL: $1"; overall_rc=1; }

echo
echo "================ STAGE-2 USER OBJECT REPLAY VERDICT ====="
echo "  source (probe-emitted):"
echo "    pdn         = ${src_pdn:-?}"
echo "    num_mrs     = ${src_num_mrs:-?}"
echo "    cqn         = ${src_cqn:-?}, qpn = ${src_qpn:-?}, srqn = ${src_srqn:-?}"
echo
echo "  destination (QUERY_AWAITING_BIND on dst vf 0):"
printf "    %-12s %-8s %-8s\n" "kind" "observed" "expected"
printf "    %-12s %-8s %-8s\n" "----" "--------" "--------"
printf "    %-12s %-8s %-8s\n" "MR"    "$obs_mr"  "$EXPECT_MR_COUNT"
printf "    %-12s %-8s %-8s\n" "CQ"    "$obs_cq"  "$EXPECT_CQ_COUNT"
printf "    %-12s %-8s %-8s\n" "QP"    "$obs_qp"  "$EXPECT_QP_COUNT"
printf "    %-12s %-8s %-8s\n" "SRQ"   "$obs_srq" "$EXPECT_SRQ_COUNT"
printf "    %-12s %-8s %-8s\n" "DBR"   "$obs_dbr" "$EXPECT_DBR_COUNT"
printf "    %-12s %-8s %-8s\n" "TOTAL" "$obs_total" "$EXPECT_TOTAL"
echo

[ "$obs_total" = "$EXPECT_TOTAL" ] || fail_one "total: observed=$obs_total expected=$EXPECT_TOTAL"
[ "$obs_mr"    = "$EXPECT_MR_COUNT" ]    || fail_one "MR:    observed=$obs_mr expected=$EXPECT_MR_COUNT"
[ "$obs_cq"    = "$EXPECT_CQ_COUNT" ]    || fail_one "CQ:    observed=$obs_cq expected=$EXPECT_CQ_COUNT"
[ "$obs_qp"    = "$EXPECT_QP_COUNT" ]    || fail_one "QP:    observed=$obs_qp expected=$EXPECT_QP_COUNT"
[ "$obs_srq"   = "$EXPECT_SRQ_COUNT" ]   || fail_one "SRQ:   observed=$obs_srq expected=$EXPECT_SRQ_COUNT"
[ "$obs_dbr"   = "$EXPECT_DBR_COUNT" ]   || fail_one "DBR:   observed=$obs_dbr expected=$EXPECT_DBR_COUNT"

if [ "$overall_rc" = 0 ]; then
    if [ "$EXPECT_TOTAL" = 0 ]; then
        echo "  BASELINE PASS (C3-era):"
        echo "      ioctl callable, no HOST_USER_PAGE records on the wire"
        echo "      (stage-2 SAVE emit / LOAD replay / source retag not"
        echo "      yet wired), destination domain has zero placeholders."
        echo "      This is the expected state at the C3 commit; bump the"
        echo "      EXPECT_* env vars as later commits in the series land"
        echo "      to re-assert against non-zero counts."
    else
        echo "  FULL PASS:"
        echo "      source emit chain -> wire records -> destination"
        echo "      placeholders all match across the $((EXPECT_TOTAL)) entries"
        echo "      enumerated above. Stage-2 identity infrastructure is"
        echo "      empirically validated end-to-end."
    fi
else
    echo "  PARTIAL FAIL: see line-by-line above. Most likely cause(s):"
    echo "    - Source-side retag callsite missing or fired on the wrong"
    echo "      umem range (look for vfmig_iova: dev_warn entries)."
    echo "    - SAVE/LOAD wire mismatch (record_size, byte order, missing"
    echo "      wire tag handler)."
    echo "    - vfmig_iova_replay_external() returning early (-EEXIST or"
    echo "      -ERANGE on placeholder install -- check dmesg)."
fi
echo

# Always tear down on exit -- trap cleanup handles the probe; here we
# drop the dest VF so subsequent runs start clean.
echo "=== teardown: sriov_numvfs=0 ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null || true

exit "$overall_rc"
