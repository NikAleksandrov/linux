#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# §S3b "DEVX-adoption blind spot, destroy direction" empirical
# probe. Validates -- or refutes -- the CRIU agent's hypothesis
# (2026-06-03) that FW silently no-ops uid-mismatched destroys on
# QPCs adopted from a DEVX-enabled source.
#
# Why we care
# -----------
# The §S3b matrix landed via test_pd_adopt.sh tested only the
# CREATE direction (CREATE_MKEY with various uid_hint values
# against an adopted PDN). The destroy direction was inferred from
# the fact that the v0 mitigation chose to open the destination
# ucontext under uid=0 -- i.e. we trusted the host-priv claim that
# uid=0 is ungated for destroys too. The CRIU agent's pd_cq_qp
# end-to-end repro on FW 28.48.1000 surfaced a smoking-gun
# asymmetry:
#
#   * mlx5_cmd_exec(DESTROY_QP, uid=0) returns err=0 against a
#     uid=src_devx_uid-owned QPC. So destroy_qp_common's
#     mlx5_ib_err diagnostic does NOT fire, mlx5_ib_destroy_qp
#     returns 0, and userspace believes the QP is gone.
#   * But the dispatcher's next opcode that propagates a FW errno
#     -- DEALLOC_PD -- fails with bad_resource_state syndrome
#     0xef0c8a-class. Something still pins the PDC.
#
# This harness measures the destroy-direction directly with a new
# PROBE_QP_TEARDOWN ioctl that brackets each operation between
# QUERY_QP calls. The post-op QUERY_QP is the smoking gun:
# QUERY_QP success after a "successful" DESTROY_QP means FW
# silently no-op'd.
#
# Test matrix
# -----------
# Two ops x three uid lanes = six cells:
#
#                |  DESTROY_QP             |  MODIFY_QP 2RST
#   -------------+-------------------------+----------------------
#   uid=0        |  C_destroy_zero         |  C_2rst_zero
#   uid=src_devx |  C_destroy_src          |  C_2rst_src
#   uid=hi_unalloc| C_destroy_hi           |  C_2rst_hi
#
# Per cell we record:
#   pre_qpc_state          QPC state pre-op (sanity check; should
#                          equal the source's pre-SAVE state)
#   pre_qpc_pd             QPC pd pre-op (sanity check)
#   op_accept              FW status from the op (0 = ack)
#   op_syndrome            FW syndrome from the op (0 = no syndrome)
#   qpc_alive_after_op     post QUERY_QP succeeded (1 = QPC still
#                          alive, 0 = QPC gone). This is the
#                          smoking-gun signal.
#   post_qpc_state         post-op QPC state (only meaningful when
#                          qpc_alive_after_op == 1)
#
# Decoding the verdict per cell
# -----------------------------
# DESTROY_QP rows:
#   op_accept=1, qpc_alive_after_op=0  -> destroy worked. uid lane
#                                         is host-priv.
#   op_accept=1, qpc_alive_after_op=1  -> SILENT NO-OP confirmed.
#   op_accept=0                        -> FW rejected the destroy
#                                         outright (loud failure
#                                         lane; the rejection is
#                                         caught by mlx5_ib_err).
#
# 2RST_QP rows:
#   op_accept=1, post_qpc_state=0      -> modify worked (state
#                                         transitioned to RESET).
#                                         uid lane is host-priv
#                                         for modify.
#   op_accept=1, post_qpc_state==pre   -> SILENT NO-OP confirmed
#                                         (FW ack'd, no state
#                                         change).
#   op_accept=0                        -> loud rejection.
#
# Source flow (mirrors test_pd_adopt.sh's setup, scaled to 6 QPs):
#
#   Phase A  Provision source VF, set_tracked + enable_migratable,
#            bind. Capture src_ibdev.
#   Phase B  Spawn 6 fw_id_continuity_probe instances against
#            src_ibdev, one per QP. Each opens its own
#            ibv_context (libmlx5 default = auto-DEVX), allocates
#            a fresh PD + CQ + QP, and reports its qpn / pdn /
#            src_devx_uid. We capture all six. Probes stay alive
#            holding their resources through SAVE.
#   Phase C  SAVE_VHCA_STATE on the source.
#   Phase D  Quit all 6 source probes; tear down the source VF.
#   Phase E  Provision a fresh destination VF: set_tracked,
#            enable_migratable, LOAD_VHCA_STATE, mark_restored,
#            bind. NB: we do NOT open a userspace ucontext on the
#            dest -- the probes run on the PF cdev and reach the
#            FW directly via the VF mdev cmdif under host-priv.
#   Phase F  Run the 6-cell matrix via PROBE_QP_TEARDOWN ioctl.
#   Phase G  Print the verdict table.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_qp_destroy_matrix.sh
#
# Optional knobs:
#   BLOB        Path for the SAVE blob (default /tmp/vf_qp_destroy_matrix.blob).
#   TOOL        mlx5_vfmig CLI (default $ROOT_DIR/tools/mlx5_vfmig).
#   PROBE       fw_id_continuity_probe binary
#               (default $SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe).
#   HI_UNALLOC  uid value to use for the "high-unalloc" lane.
#               Default 0xfffd. Picked to match test_pd_adopt.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$SCRIPT_DIR/../fw_id_continuity/fw_id_continuity_probe}
BLOB=${BLOB:-/tmp/vf_qp_destroy_matrix.blob}
META=${META:-${BLOB}.meta}
HI_UNALLOC=${HI_UNALLOC:-$((0xfffd))}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]  || { echo "build $TOOL first: make -C $ROOT_DIR";  exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first: make -C $ROOT_DIR"; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

WORKDIR=$(mktemp -d -t qpdmtx.XXXXXX)
declare -a SRC_PROBE_PIDS=()
declare -a SRC_PROBE_FIFOS=()
cleanup() {
    local i pid
    for i in "${!SRC_PROBE_PIDS[@]}"; do
        pid=${SRC_PROBE_PIDS[$i]}
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            local fifo_in="$WORKDIR/src_$i.in"
            echo "quit" > "$fifo_in" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

# --- helpers ---------------------------------------------------------

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

# Spawn one fw_id_continuity_probe and capture its READY-time
# manifest into "$prefix_<key>" variables (e.g. src1_pdn, src1_qpn).
# Probe stays alive holding its resources.
start_indexed_src_probe() {
    local idx=$1
    local ibdev=$2
    local prefix=$3
    local fifo_in="$WORKDIR/src_$idx.in"
    local fifo_out="$WORKDIR/src_$idx.out"

    mkfifo "$fifo_in" "$fifo_out"
    SRC_PROBE_FIFOS+=("$fifo_in")
    echo "=== source probe #$idx: $PROBE $ibdev ==="
    sudo "$PROBE" "$ibdev" < "$fifo_in" > "$fifo_out" 2>&1 &
    local pid=$!
    SRC_PROBE_PIDS+=("$pid")

    # Hold the writer end open across the harness so closing
    # write fd 7+idx does not race the probe's read.
    eval "exec $((10 + idx))> \"\$fifo_in\""

    local line
    while IFS= read -r line < "$fifo_out"; do
        echo "  [src#$idx] $line"
        case "$line" in
            READY) return 0 ;;
            *=*)
                local k="${line%%=*}"
                local v="${line#*=}"
                eval "${prefix}_${k}=\"\$v\""
                ;;
        esac
    done
    echo "  [src#$idx] EOF before READY -- probe failed"
    return 1
}

quit_indexed_src_probe() {
    local idx=$1
    local fd=$((10 + idx))
    local pid=${SRC_PROBE_PIDS[$idx]}

    eval "echo quit >&${fd}"
    eval "exec ${fd}>&-"
    wait "$pid" 2>/dev/null || true
    SRC_PROBE_PIDS[$idx]=""
}

# `mlx5_vfmig $PF probe_qp_teardown $vf $qpn $uid_hint $op_mode`
# capture each key=value into "$prefix_<key>".
probe_qp_teardown_into() {
    local label=$1 vf=$2 qpn=$3 uid_hint=$4 op_mode=$5 prefix=$6
    local out line k v
    echo "=== PROBE_QP_TEARDOWN ($label): vf=$vf qpn=$qpn uid_hint=$uid_hint op_mode=$op_mode ==="
    if ! out=$(sudo "$TOOL" "$PF" probe_qp_teardown "$vf" "$qpn" "$uid_hint" "$op_mode" 2>&1); then
        echo "$out" | sed 's/^/  /'
        echo "  PROBE_QP_TEARDOWN ioctl failed (transport/arg error)"
        return 1
    fi
    while IFS= read -r line; do
        echo "  [$label] $line"
        case "$line" in
            *=*) k="${line%%=*}"; v="${line#*=}";
                 eval "${prefix}_${k}=\"\$v\"" ;;
        esac
    done <<<"$out"
    return 0
}

# --- Phase A ---------------------------------------------------------

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
SRC_IBDEV=$(find_ib_dev_for_pci "$SRC_VF") || { echo "FAIL: no ibdev for source $SRC_VF"; exit 1; }
echo "source ibdev: $SRC_IBDEV"

# --- Phase B ---------------------------------------------------------

echo "=== Phase B: 6 source probes, capturing 6 (pdn, cqn, qpn) ==="
sudo dmesg -C
# Enable the mlx5_ib_dbg trace in mlx5_ib_alloc_ucontext that emits
# the freshly-allocated devx_uid (vfmig_uctx_dbg). Replaced the older
# vfmig_pd_dbg pr_info that we cleaned up after the gate landed.
echo 'func mlx5_ib_alloc_ucontext +p' | \
    sudo tee /sys/kernel/debug/dynamic_debug/control >/dev/null 2>&1 || true
for i in 0 1 2 3 4 5; do
    start_indexed_src_probe "$i" "$SRC_IBDEV" "src${i}"
done

# Capture src_devx_uid from the new vfmig_uctx_dbg trace. libmlx5's
# default ibv_open_device auto-allocates a DEVX uid; we read its
# value directly from mlx5_ib_alloc_ucontext's debug print.
src_devx_uid=$(sudo dmesg | \
    grep -E "vfmig_uctx_dbg: alloc_ucontext ibdev=$SRC_IBDEV devx_uid=[0-9]+ adopted=0" | \
    tail -1 | sed -nE 's/.* devx_uid=([0-9]+) .*/\1/p')
src_devx_uid=${src_devx_uid:-0}
echo "captured: src_devx_uid=$src_devx_uid"

if [ "$src_devx_uid" = 0 ]; then
    echo "  WARNING: src_devx_uid == 0 -- libmlx5 did NOT auto-allocate a"
    echo "           DEVX uid for these probes. The matrix will still run"
    echo "           but the 'uid=src_devx' lane is degenerate (== uid=0)."
fi

# --- Phase C ---------------------------------------------------------

echo "=== Phase C: SAVE_VHCA_STATE ==="
# snapshot-ordering: pause datapath (CRIU CHECKPOINT_DEVICES), then
# capture (SAVE is suspend-aware and skips its own suspend), then resume.
sudo "$TOOL" "$PF" suspend_vhca 0
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" resume_vhca 0
sudo chmod 0644 "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
[ "$SAVE_BYTES" -gt 16 ] || { echo "FAIL: blob suspiciously small: $SAVE_BYTES"; exit 1; }
echo "  save bytes: $SAVE_BYTES"

# --- Phase D ---------------------------------------------------------

echo "=== Phase D: quit 6 src probes + tear down source VF ==="
for i in 0 1 2 3 4 5; do
    quit_indexed_src_probe "$i"
done
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null

# --- Phase E ---------------------------------------------------------

echo "=== Phase E: fresh DEST VF + LOAD + bind ==="
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
wait_for_path "$(vf_path $PF)/virtfn0"
DST_VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "dest VF: $DST_VF"

sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0

sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" mark_restored 0

echo mlx5_core | sudo tee "$(vf_path $DST_VF)/driver_override" >/dev/null
bind_vf_safe "$DST_VF" 60 "Phase E: dest VF bind"
sleep 1
DST_IBDEV=$(find_ib_dev_for_pci "$DST_VF") || { echo "FAIL: no ibdev for dest $DST_VF"; exit 1; }
echo "dest ibdev: $DST_IBDEV"

# --- Phase F: the 6-cell matrix --------------------------------------

echo
echo "================ Phase F: 6-cell PROBE_QP_TEARDOWN matrix ================"
echo "src_devx_uid=$src_devx_uid hi_unalloc=$HI_UNALLOC"
echo "QPNs per probe slot:"
for i in 0 1 2 3 4 5; do
    eval "echo \"  src${i}: pdn=\$src${i}_pdn cqn=\$src${i}_cqn qpn=\$src${i}_qpn\""
done

# Cell 0: DESTROY_QP, uid=0
probe_qp_teardown_into "destroy_zero" 0 \
    "$src0_qpn" 0 0 c_destroy_zero
# Cell 1: DESTROY_QP, uid=src_devx_uid
probe_qp_teardown_into "destroy_src" 0 \
    "$src1_qpn" "$src_devx_uid" 0 c_destroy_src
# Cell 2: DESTROY_QP, uid=hi_unalloc
probe_qp_teardown_into "destroy_hi" 0 \
    "$src2_qpn" "$HI_UNALLOC" 0 c_destroy_hi
# Cell 3: 2RST_QP, uid=0
probe_qp_teardown_into "2rst_zero" 0 \
    "$src3_qpn" 0 1 c_2rst_zero
# Cell 4: 2RST_QP, uid=src_devx_uid
probe_qp_teardown_into "2rst_src" 0 \
    "$src4_qpn" "$src_devx_uid" 1 c_2rst_src
# Cell 5: 2RST_QP, uid=hi_unalloc
probe_qp_teardown_into "2rst_hi" 0 \
    "$src5_qpn" "$HI_UNALLOC" 1 c_2rst_hi

# --- Phase G: verdict ------------------------------------------------

echo
echo "================ Phase G: §S3b destroy-direction matrix ================"
printf "%-22s %-22s %-22s %-22s\n" \
    "uid lane" "DESTROY_QP" "MODIFY_QP 2RST" "smoking-gun"
printf "%-22s %-22s %-22s %-22s\n" \
    "----------------------" "----------------------" \
    "----------------------" "----------------------"

decode_destroy_cell() {
    local prefix=$1
    local op_acc qpc_alive
    eval "op_acc=\${${prefix}_op_accept:-?}"
    eval "qpc_alive=\${${prefix}_qpc_alive_after_op:-?}"
    if [ "$op_acc" = 1 ] && [ "$qpc_alive" = 0 ]; then
        echo "destroyed"
    elif [ "$op_acc" = 1 ] && [ "$qpc_alive" = 1 ]; then
        echo "SILENT NO-OP"
    elif [ "$op_acc" = 0 ]; then
        echo "loud reject"
    else
        echo "?"
    fi
}

decode_2rst_cell() {
    local prefix=$1
    local op_acc qpc_alive pre_state post_state
    eval "op_acc=\${${prefix}_op_accept:-?}"
    eval "qpc_alive=\${${prefix}_qpc_alive_after_op:-?}"
    eval "pre_state=\${${prefix}_pre_qpc_state:-?}"
    eval "post_state=\${${prefix}_post_qpc_state:-?}"
    if [ "$op_acc" = 1 ] && [ "$post_state" = 0 ]; then
        echo "to RESET"
    elif [ "$op_acc" = 1 ] && [ "$post_state" = "$pre_state" ]; then
        echo "SILENT NO-OP"
    elif [ "$op_acc" = 1 ]; then
        echo "modified ($pre_state->$post_state)"
    elif [ "$op_acc" = 0 ]; then
        echo "loud reject"
    else
        echo "?"
    fi
}

destroy_zero=$(decode_destroy_cell c_destroy_zero)
destroy_src=$(decode_destroy_cell c_destroy_src)
destroy_hi=$(decode_destroy_cell c_destroy_hi)
twoRst_zero=$(decode_2rst_cell c_2rst_zero)
twoRst_src=$(decode_2rst_cell c_2rst_src)
twoRst_hi=$(decode_2rst_cell c_2rst_hi)

printf "%-22s %-22s %-22s\n" \
    "uid=0 (host-priv)" "$destroy_zero" "$twoRst_zero"
printf "%-22s %-22s %-22s\n" \
    "uid=src_devx=$src_devx_uid" "$destroy_src" "$twoRst_src"
printf "%-22s %-22s %-22s\n" \
    "uid=hi_unalloc=$HI_UNALLOC" "$destroy_hi" "$twoRst_hi"

echo
echo "Detailed output (for forensic decode):"
for cell in destroy_zero destroy_src destroy_hi 2rst_zero 2rst_src 2rst_hi; do
    eval "echo \"  c_${cell}: op_status=\${c_${cell}_op_status:-?} op_syndrome=\${c_${cell}_op_syndrome:-?} pre_state=\${c_${cell}_pre_qpc_state:-?} post_state=\${c_${cell}_post_qpc_state:-?} qpc_alive_after_op=\${c_${cell}_qpc_alive_after_op:-?}\""
done

echo
overall_verdict="UNKNOWN"
if [ "$destroy_src" = "SILENT NO-OP" ]; then
    overall_verdict="HYPOTHESIS CONFIRMED: FW silently no-ops cross-uid DESTROY_QP"
elif [ "$destroy_src" = "destroyed" ] && [ "$destroy_zero" = "destroyed" ]; then
    overall_verdict="HYPOTHESIS REFUTED: cross-uid destroy actually works (look elsewhere for the PD pin)"
elif [ "$destroy_src" = "loud reject" ]; then
    overall_verdict="HYPOTHESIS PARTIALLY REFUTED: FW does loud-reject cross-uid (the agent's repro must have a different cause)"
fi
echo "Overall: $overall_verdict"

# --- manifest --------------------------------------------------------

sudo tee "$META" >/dev/null <<EOF
# qp_destroy_matrix manifest
src_pf=$PF
src_vf=$SRC_VF
src_ibdev=$SRC_IBDEV
src_devx_uid=$src_devx_uid
hi_unalloc=$HI_UNALLOC
save_bytes=$SAVE_BYTES
dst_vf=$DST_VF
dst_ibdev=$DST_IBDEV

# six per-probe captures
src0_pdn=$src0_pdn src0_cqn=$src0_cqn src0_qpn=$src0_qpn
src1_pdn=$src1_pdn src1_cqn=$src1_cqn src1_qpn=$src1_qpn
src2_pdn=$src2_pdn src2_cqn=$src2_cqn src2_qpn=$src2_qpn
src3_pdn=$src3_pdn src3_cqn=$src3_cqn src3_qpn=$src3_qpn
src4_pdn=$src4_pdn src4_cqn=$src4_cqn src4_qpn=$src4_qpn
src5_pdn=$src5_pdn src5_cqn=$src5_cqn src5_qpn=$src5_qpn

# 6-cell matrix verdict (label-style)
destroy_zero=$destroy_zero
destroy_src=$destroy_src
destroy_hi=$destroy_hi
twoRst_zero=$twoRst_zero
twoRst_src=$twoRst_src
twoRst_hi=$twoRst_hi

overall_verdict=$overall_verdict
EOF
echo
echo "manifest: $META"
echo "blob: $BLOB ($SAVE_BYTES bytes)"
