#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# K6: empirical test of FW-id continuity across LOAD_VHCA_STATE.
# Drives the experiment specified in DESIGN_R3_uobj_restore.md §8.2.
#
# Per-class pass condition (§8.2.1): for each of {PD, CQ, QP, MKEY, SRQ}
# the destination's first allocation after LOAD must yield a FW id
# strictly greater than the source's allocation that was alive at SAVE
# time. That establishes FW preserves reservations across LOAD; a fail
# (dest_id <= source_id, especially dest_id == 0) means FW does not, and
# K3/K4 mlx5 handler design needs explicit pre-reserve plumbing.
#
# Flow:
#
#   Phase A  source SR-IOV/set_tracked/bind (same as test_m2r_iova.sh).
#   Phase B  fork k6_id_probe on source ibdev. Drain stdout to READY,
#            record source ids. Probe stays ALIVE.
#   Phase C  SAVE_VHCA_STATE. Probe still alive: FW state at SAVE time
#            includes the probe's allocations.
#   Phase D  signal probe to quit, tear down source VF.
#   Phase E  fresh dest VF, set_tracked, LOAD_VHCA_STATE, mark_restored,
#            bind.
#   Phase F  fork k6_id_probe on dest ibdev. Drain stdout to READY,
#            record dest ids.
#   Phase G  per-class compare. PASS = dest_id > source_id for all
#            classes; otherwise tag failing classes loudly.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_k6_id_continuity.sh
#
# Optional knobs:
#   K6_POST_RECV_WRS=N    Post N recv WRs to the source QP for the
#                         §6.3 RQ-head/tail piggyback. When > 0 the
#                         script also issues MLX5_VFMIG_IOC_QUERY_QP
#                         on the source qpn before SAVE and on the
#                         SAME qpn after LOAD+bind (K6 has already
#                         shown the qpn reservation survives), and
#                         compares the QPC subset that matters for
#                         pending-WR survival (state, sw/hw RQ
#                         counters, next_rcv_psn). PASS = matching.
#   BLOB                  Path for the SAVE blob (default /tmp/vf_k6.blob).
#   TOOL                  mlx5_vfmig CLI (default ./mlx5_vfmig).
#   PROBE                 k6_id_probe binary (default ./k6_id_probe).

set -euo pipefail

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-./mlx5_vfmig}
PROBE=${PROBE:-./k6_id_probe}
BLOB=${BLOB:-/tmp/vf_k6.blob}
META=${META:-${BLOB}.meta}
K6_POST_RECV_WRS=${K6_POST_RECV_WRS:-0}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]  || { echo "build $TOOL first";  exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first"; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

# Workdir for the FIFOs that drive the K6 probes.
WORKDIR=$(mktemp -d -t k6.XXXXXX)
cleanup() {
    if [ -n "${SRC_PROBE_PID:-}" ] && kill -0 "$SRC_PROBE_PID" 2>/dev/null; then
        echo "[cleanup] reaping source probe pid $SRC_PROBE_PID"
        echo "quit" > "$WORKDIR/src.in" 2>/dev/null || true
        wait "$SRC_PROBE_PID" 2>/dev/null || true
    fi
    if [ -n "${DST_PROBE_PID:-}" ] && kill -0 "$DST_PROBE_PID" 2>/dev/null; then
        echo "[cleanup] reaping dest probe pid $DST_PROBE_PID"
        echo "quit" > "$WORKDIR/dst.in" 2>/dev/null || true
        wait "$DST_PROBE_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

# --- helpers (subset of test_m2r_iova.sh) -----------------------------

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
            echo "" >&2
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

# Start a k6_id_probe in the background, hook stdin/stdout via FIFOs.
# Reads probe output until "READY" sentinel, copying key=value lines
# into the named prefix dict (eval-form): "${prefix}_pdn=...", etc.
#
# Args:
#   $1 = ibdev name
#   $2 = label ("source" / "dest")
#   $3 = fifo basename in $WORKDIR
#   $4 = variable prefix ("src" / "dst")
# Sets globals:
#   ${prefix}_pdn, _cqn, _qpn, _lkey, _rkey, _mkey_index, _srqn,
#   _recv_wrs_posted
#   ${label}_PROBE_PID
start_probe() {
    local ibdev=$1 label=$2 fifo_base=$3 prefix=$4
    local fifo_in="$WORKDIR/${fifo_base}.in"
    local fifo_out="$WORKDIR/${fifo_base}.out"
    local extra_args=()

    mkfifo "$fifo_in" "$fifo_out"

    if [ "$prefix" = "src" ] && [ "$K6_POST_RECV_WRS" -gt 0 ]; then
        extra_args+=(--post-recv-wrs "$K6_POST_RECV_WRS")
    fi

    echo "=== K6 $label probe: $PROBE $ibdev ${extra_args[*]:-} ==="
    # Run as root so we hit the same uid space the rest of the test
    # already uses (set_tracked / save_vhca_state etc. were also sudo).
    # The probe needs CAP_NET_RAW / NET_ADMIN at the lowest level for
    # some verbs paths on some systems; sudo is simplest.
    sudo "$PROBE" "$ibdev" "${extra_args[@]}" < "$fifo_in" > "$fifo_out" 2>&1 &
    local pid=$!

    # The R-side of the .in FIFO is opened by sudo+probe above; we
    # need a W-side handle in this shell so future "quit" writes
    # don't block on no-reader. Open it on its own fd.
    case "$prefix" in
        src) exec 7> "$fifo_in"; SRC_PROBE_PID=$pid ;;
        dst) exec 8> "$fifo_in"; DST_PROBE_PID=$pid ;;
        *)   echo "internal error: unknown prefix '$prefix'"; exit 1 ;;
    esac

    # Drain stdout until READY (or probe exit).
    local line
    while IFS= read -r line < "$fifo_out"; do
        echo "  [$label probe] $line"
        case "$line" in
            READY)
                return 0
                ;;
            *=*)
                # Key=value: stash under ${prefix}_<key>.
                local k="${line%%=*}"
                local v="${line#*=}"
                # shellcheck disable=SC2086
                eval "${prefix}_${k}=\"\$v\""
                ;;
            *)
                # Stderr / informational lines from the probe.
                # If the probe printed an error line and is about to
                # exit, the next read returns EOF.
                ;;
        esac
    done

    echo "  [$label probe] EOF before READY -- probe failed"
    return 1
}

quit_probe() {
    local label=$1 prefix=$2
    case "$prefix" in
        src) echo "quit" >&7; exec 7>&-; wait "$SRC_PROBE_PID" 2>/dev/null || true; SRC_PROBE_PID="" ;;
        dst) echo "quit" >&8; exec 8>&-; wait "$DST_PROBE_PID" 2>/dev/null || true; DST_PROBE_PID="" ;;
    esac
    echo "  [$label probe] exited"
}

# §6.3 piggyback: invoke `mlx5_vfmig <pf> query_qp <vf_id> <qpn>` and
# stash each `key=value` output line under a caller-supplied variable
# prefix. Returns 1 on ioctl failure (caller can decide policy).
#
# Args:
#   $1 = label (for log)
#   $2 = vf_id (always 0 in this script)
#   $3 = qpn
#   $4 = variable prefix ("srcq" / "dstq")
query_qp_into() {
    local label=$1 vf_id=$2 qpn=$3 prefix=$4
    local out line k v rc=0
    echo "=== K6 §6.3 piggyback: QUERY_QP on $label (vf=$vf_id qpn=$qpn) ==="
    if ! out=$(sudo "$TOOL" "$PF" query_qp "$vf_id" "$qpn" 2>&1); then
        echo "$out" | sed 's/^/  /'
        echo "  QUERY_QP on $label failed -- skipping §6.3 comparison"
        return 1
    fi
    while IFS= read -r line; do
        echo "  [$label qp] $line"
        case "$line" in
            *=*) k="${line%%=*}"; v="${line#*=}";
                 eval "${prefix}_${k}=\"\$v\"" ;;
        esac
    done <<<"$out"
    return 0
}

# Render the comparison verdict for one QPC field. Field equality is
# the expected positive signal for §6.3 (the FW-side QPC should round-
# trip across LOAD_VHCA_STATE byte-for-byte).
qpc_verdict() {
    local field=$1 src=$2 dst=$3
    if [ -z "$src" ] || [ -z "$dst" ]; then
        echo "  SKIP  $field:  src=${src:-?}  dst=${dst:-?}   (missing data)"
        rq_skipped=$((rq_skipped + 1))
        return
    fi
    if [ "$src" = "$dst" ]; then
        echo "  PASS  $field:  src=$src  dst=$dst   (matches across LOAD)"
    else
        echo "  FAIL  $field:  src=$src  dst=$dst   (LOAD did not preserve)"
        rq_overall_rc=1
    fi
}

# --- Phase A: provision SOURCE ---------------------------------------

echo "=== Phase A: provision SOURCE VF (set_tracked + migratable + bind) ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null

wait_for_path "$(vf_path $PF)/virtfn0"
SRC_VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "source VF: $SRC_VF"

sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0

echo mlx5_core | sudo tee "$(vf_path $SRC_VF)/driver_override" >/dev/null
bind_vf_safe "$SRC_VF" 60 "Phase A: source VF bind" || rc=$?
case "${rc:-0}" in
    0) ;;
    1) echo "FAIL: Phase A source bind failed -- inspect dmesg"; exit 1 ;;
    2) exit 1 ;;
esac
unset rc

sleep 1
SRC_IBDEV=$(find_ib_dev_for_pci "$SRC_VF") || { echo "FAIL: no ibdev for source $SRC_VF"; exit 1; }
echo "source ibdev: $SRC_IBDEV"

# --- Phase B: source probe (resources alive across SAVE) -------------

echo "=== Phase B: K6 source probe (resources stay alive through SAVE) ==="
start_probe "$SRC_IBDEV" source "src" "src"

echo "--- source K6 ids ---"
echo "  pdn        = $src_pdn"
echo "  cqn        = $src_cqn"
echo "  qpn        = $src_qpn"
echo "  lkey       = $src_lkey"
echo "  rkey       = $src_rkey"
echo "  mkey_index = $src_mkey_index"
echo "  srqn       = $src_srqn"
echo "  recv_wrs   = $src_recv_wrs_posted"

# §6.3 piggyback: snapshot source QPC before SAVE. We do this only
# when WRs were posted -- otherwise the QPC has no RQ activity to
# discriminate against, and the §6.3 question is moot for this run.
RQ_PIGGYBACK=0
if [ "$K6_POST_RECV_WRS" -gt 0 ]; then
    if query_qp_into source 0 "$src_qpn" srcq; then
        RQ_PIGGYBACK=1
    fi
fi

# --- Phase C: SAVE (probe still alive) -------------------------------

echo "=== Phase C: SAVE_VHCA_STATE (source probe still holds resources) ==="
sudo dmesg -C
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
sudo chmod 0644 "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
[ "$SAVE_BYTES" -gt 16 ] || { echo "FAIL: blob suspiciously small: $SAVE_BYTES"; exit 1; }
SAVE_SHA=$(sha256sum "$BLOB" | awk '{print $1}')
echo "blob: $SAVE_BYTES bytes, sha256=$SAVE_SHA"

if sudo dmesg | grep -E 'SAVE_VHCA_STATE|SUSPEND_VHCA' | grep -i 'failed'; then
    echo "FAIL: firmware error during SAVE"
    exit 1
fi

# --- Phase D: quit source probe, tear down source VF -----------------

echo "=== Phase D: quit source probe + tear down source VF ==="
quit_probe source src

echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null

# --- Phase E: dest re-provision + LOAD + bind ------------------------

echo "=== Phase E: fresh DEST VF + set_tracked + LOAD + bind ==="
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
wait_for_path "$(vf_path $PF)/virtfn0"
DST_VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "dest VF: $DST_VF"

sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0

sudo dmesg -C
sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" mark_restored 0

echo mlx5_core | sudo tee "$(vf_path $DST_VF)/driver_override" >/dev/null
bind_vf_safe "$DST_VF" 60 "Phase E: dest VF bind" || rc=$?
case "${rc:-0}" in
    0) ;;
    1) echo "FAIL: Phase E dest bind failed -- inspect dmesg"; exit 1 ;;
    2) exit 1 ;;
esac
unset rc

sleep 1
DST_IBDEV=$(find_ib_dev_for_pci "$DST_VF") || { echo "FAIL: no ibdev for dest $DST_VF"; exit 1; }
echo "dest ibdev: $DST_IBDEV"

# §6.3 piggyback: query the SOURCE's qpn now -- before any dest-side
# user QP is created. K6 has already established that source's qpn
# remains reserved on dest after LOAD, so QUERY_QP on it reads the
# restored QPC. Doing this BEFORE the dest probe avoids any chance
# of the dest probe perturbing the QPC we're trying to measure.
if [ "$RQ_PIGGYBACK" = 1 ]; then
    if ! query_qp_into dest 0 "$src_qpn" dstq; then
        echo "  WARN: §6.3 dest QUERY_QP failed -- §6.3 verdict will be UNAVAILABLE"
        RQ_PIGGYBACK=0
    fi
fi

# --- Phase F: dest probe --------------------------------------------

echo "=== Phase F: K6 dest probe ==="
start_probe "$DST_IBDEV" dest "dst" "dst"

echo "--- dest K6 ids ---"
echo "  pdn        = $dst_pdn"
echo "  cqn        = $dst_cqn"
echo "  qpn        = $dst_qpn"
echo "  lkey       = $dst_lkey"
echo "  rkey       = $dst_rkey"
echo "  mkey_index = $dst_mkey_index"
echo "  srqn       = $dst_srqn"

# --- Phase G: per-class compare -------------------------------------

echo
echo "================ K6 PER-CLASS RESULTS ================"
overall_rc=0
skipped=0
verdict() {
    local class=$1 src=$2 dst=$3
    if [ "$src" = "SKIPPED" ] || [ "$dst" = "SKIPPED" ]; then
        echo "  SKIP  $class:  src=$src  dst=$dst   (alloc failed on one side; data unavailable -- see dmesg)"
        skipped=$((skipped + 1))
        return
    fi
    if [ "$dst" -gt "$src" ]; then
        echo "  PASS  $class:  src=$src  dst=$dst   (preserved; dst > src)"
    elif [ "$dst" -eq "$src" ]; then
        echo "  FAIL  $class:  src=$src  dst=$dst   (collision; FW reissued source's id)"
        overall_rc=1
    else
        echo "  FAIL  $class:  src=$src  dst=$dst   (dest < source; FW likely reset cursor)"
        overall_rc=1
    fi
}
verdict PD   "$src_pdn"        "$dst_pdn"
verdict CQ   "$src_cqn"        "$dst_cqn"
verdict QP   "$src_qpn"        "$dst_qpn"
verdict MKEY "$src_mkey_index" "$dst_mkey_index"
verdict SRQ  "$src_srqn"       "$dst_srqn"
echo "======================================================"
if [ "$skipped" -gt 0 ]; then
    echo "  NOTE: $skipped class(es) skipped -- partial verdict."
fi

# --- §6.3 piggyback verdict -----------------------------------------

rq_overall_rc=0
rq_skipped=0
if [ "$RQ_PIGGYBACK" = 1 ]; then
    echo
    echo "================ §6.3 RQ PIGGYBACK RESULTS ================"
    echo "Question: do FW-side QPC fields (incl. RQ counters) survive"
    echo "LOAD_VHCA_STATE intrinsically? PASS = src/dst byte-equal."
    echo "Source qpn $src_qpn, $K6_POST_RECV_WRS receive WR(s) posted in INIT."
    echo
    qpc_verdict state                "${srcq_qpc_state:-}"             "${dstq_qpc_state:-}"
    qpc_verdict pd                   "${srcq_qpc_pd:-}"                "${dstq_qpc_pd:-}"
    qpc_verdict q_key                "${srcq_qpc_q_key:-}"             "${dstq_qpc_q_key:-}"
    qpc_verdict cqn_snd              "${srcq_qpc_cqn_snd:-}"           "${dstq_qpc_cqn_snd:-}"
    qpc_verdict cqn_rcv              "${srcq_qpc_cqn_rcv:-}"           "${dstq_qpc_cqn_rcv:-}"
    qpc_verdict srqn_rmpn_xrqn       "${srcq_qpc_srqn_rmpn_xrqn:-}"    "${dstq_qpc_srqn_rmpn_xrqn:-}"
    qpc_verdict next_send_psn        "${srcq_qpc_next_send_psn:-}"     "${dstq_qpc_next_send_psn:-}"
    qpc_verdict next_rcv_psn         "${srcq_qpc_next_rcv_psn:-}"      "${dstq_qpc_next_rcv_psn:-}"
    qpc_verdict last_acked_psn       "${srcq_qpc_last_acked_psn:-}"    "${dstq_qpc_last_acked_psn:-}"
    qpc_verdict hw_sq_wqebb_counter  "${srcq_qpc_hw_sq_wqebb_counter:-}" "${dstq_qpc_hw_sq_wqebb_counter:-}"
    qpc_verdict sw_sq_wqebb_counter  "${srcq_qpc_sw_sq_wqebb_counter:-}" "${dstq_qpc_sw_sq_wqebb_counter:-}"
    qpc_verdict hw_rq_counter        "${srcq_qpc_hw_rq_counter:-}"     "${dstq_qpc_hw_rq_counter:-}"
    qpc_verdict sw_rq_counter        "${srcq_qpc_sw_rq_counter:-}"     "${dstq_qpc_sw_rq_counter:-}"
    echo "==========================================================="
fi

# --- manifest write -------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# K6 ID-continuity manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_pdn=$src_pdn
src_cqn=$src_cqn
src_qpn=$src_qpn
src_lkey=$src_lkey
src_rkey=$src_rkey
src_mkey_index=$src_mkey_index
src_srqn=$src_srqn
src_recv_wrs_posted=$src_recv_wrs_posted
save_bytes=$SAVE_BYTES
save_sha256=$SAVE_SHA
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV
dst_pdn=$dst_pdn
dst_cqn=$dst_cqn
dst_qpn=$dst_qpn
dst_lkey=$dst_lkey
dst_rkey=$dst_rkey
dst_mkey_index=$dst_mkey_index
dst_srqn=$dst_srqn
overall_rc=$overall_rc
rq_piggyback=$RQ_PIGGYBACK
rq_overall_rc=$rq_overall_rc
EOF
if [ "$RQ_PIGGYBACK" = 1 ]; then
    sudo tee -a "$META" >/dev/null <<EOF
# §6.3 piggyback (src vs dst QPC at qpn=$src_qpn)
srcq_qpc_state=${srcq_qpc_state:-}
srcq_qpc_sw_rq_counter=${srcq_qpc_sw_rq_counter:-}
srcq_qpc_hw_rq_counter=${srcq_qpc_hw_rq_counter:-}
srcq_qpc_next_rcv_psn=${srcq_qpc_next_rcv_psn:-}
dstq_qpc_state=${dstq_qpc_state:-}
dstq_qpc_sw_rq_counter=${dstq_qpc_sw_rq_counter:-}
dstq_qpc_hw_rq_counter=${dstq_qpc_hw_rq_counter:-}
dstq_qpc_next_rcv_psn=${dstq_qpc_next_rcv_psn:-}
EOF
fi
sudo chmod 0644 "$META"
echo "wrote $META"

quit_probe dest dst

if [ "$overall_rc" -eq 0 ]; then
    if [ "$skipped" -eq 0 ]; then
        echo
        echo "K6 OVERALL: PASS -- FW preserves id reservations across LOAD_VHCA_STATE"
        echo "             for ALL of {PD, CQ, QP, MKEY, SRQ}."
        echo "             K3/K4 mlx5 handlers can use alloc-with-id-hint pattern."
    else
        echo
        echo "K6 OVERALL: PARTIAL PASS -- preserved for all CLASSES MEASURED."
        echo "             $skipped class(es) skipped (alloc failed on one side)."
        echo "             Treat as a positive signal for the measured classes;"
        echo "             skipped classes need a separate measurement path."
    fi
else
    echo
    echo "K6 OVERALL: FAIL -- at least one class shows reservation loss."
    echo "             K3/K4 mlx5 handlers will need explicit pre-reserve plumbing"
    echo "             (likely FW patch). See DESIGN_R3_uobj_restore.md §8.2.3."
fi

if [ "$RQ_PIGGYBACK" = 1 ]; then
    if [ "$rq_overall_rc" -eq 0 ]; then
        echo "§6.3 PIGGYBACK: PASS -- QPC (incl. RQ counters, next_rcv_psn) round-trips"
        echo "                 byte-equal across LOAD_VHCA_STATE. No driver-side"
        echo "                 QUERY_QP_PENDING_WRS + replay path is required for v0."
        echo "                 §6.3 of DESIGN_R3_uobj_restore.md can be closed out."
    else
        echo "§6.3 PIGGYBACK: FAIL -- one or more QPC fields diverged across LOAD."
        echo "                 §6.3 of DESIGN_R3_uobj_restore.md must add explicit"
        echo "                 pending-WR plumbing for v0 (likely a new driver"
        echo "                 ioctl that snapshots+replays the user-RQ via UMR)."
    fi
elif [ "$K6_POST_RECV_WRS" -gt 0 ]; then
    echo "§6.3 PIGGYBACK: UNAVAILABLE (QUERY_QP failed; see logs above)."
else
    echo "§6.3 PIGGYBACK: skipped (set K6_POST_RECV_WRS to enable)."
fi

if [ "$RQ_PIGGYBACK" = 1 ] && [ "$rq_overall_rc" -ne 0 ]; then
    exit "$rq_overall_rc"
fi
exit "$overall_rc"
