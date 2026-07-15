#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# test_running_p2p_hold_window.sh -- soak a host-bound VF at RUNNING_P2P
# (initiator parked, responder live) for the length of a restore and
# prove it returns cleanly to RUNNING. This is the D.4 prerequisite
# probe from design/datapath_pause_resume.md: before the cross-host
# barrier (Appendix D) is worth building, the kernel must show a
# host-bound VHCA can hold RUNNING_P2P for tens of seconds (budget
# ~5 min) with no FW health syndrome, device fatal, or silent FSM decay,
# and fully recover its datapath afterward.
#
# IMPORTANT: this probe never pokes the *parked* VF's own command ring
# (no hw_counters/QUERY_Q_COUNTER read while at P2P). Doing so was found
# to hang the ring and wedge SR-IOV teardown (host reboot); see the
# subtest-3 note below and design/datapath_pause_resume.md D.4.
#
# Single-host topology (two VFs on the same PF, VEB-hairpinned):
#     VF0 = DUT   : the VF we park to RUNNING_P2P and soak.
#     VF1 = peer  : drives a short ib_write_bw exchange before and after
#                   the hold to prove the datapath works and recovers.
#
# NETDEV-DOWN MODEL (why this matches reality and stays safe):
#   The realistic barrier holds both peers at RUNNING_P2P *quiescent* --
#   the peer is parked too, so there is only in-flight drain, no fresh
#   inbound traffic during the hold. And per Appendix B a host-bound VF
#   MUST keep its netdev DOWN while parked: an admin-up netdev queues a
#   stray Ethernet TX (ARP/ND) on the parked initiator's SQ, which never
#   drains and trips the netdev TX watchdog every ~15s -- and tearing
#   that fragile VF down can wedge in the known uverbs-SRCU limitation.
#   So this probe: establishes RDMA while up, brings the netdevs DOWN,
#   parks, holds quiescent, resumes, brings them back UP, and reverifies.
#
# State is read from the kernel "datapath A->B" log lines. The resume is
# the load-bearing anti-decay check: RESUME_VHCA(INITIATOR) is only valid
# from RUNNING_P2P, so a clean 1->0 after the hold proves the VHCA never
# silently decayed (decay to STOP -> resume-initiator EINVAL; decay to
# RUNNING -> no 1->0 line).
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_running_p2p_hold_window.sh
#   sudo HOLD=300 ./test_running_p2p_hold_window.sh     # full 5-min soak
#   sudo ./test_running_p2p_hold_window.sh              # PF auto-detect
#
# Env:
#   HOLD    seconds to hold RUNNING_P2P (default 60; 300 spans several
#           command-timeout intervals)
#   SUBNET  /24 for the two VF netdevs (default 10.99.0)
#   GIDX    force an ib_write_bw GID index (default: tool auto-select)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."

PF=${PF:-}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
HOLD=${HOLD:-60}
SUBNET=${SUBNET:-10.99.0}
GIDX=${GIDX:-}

if [ -z "$PF" ]; then
    PF=$(ls /dev/mlx5_vfmig/ 2>/dev/null | head -n1)
fi
[ -n "$PF" ] || { echo "no PF cdev found under /dev/mlx5_vfmig/ (set PF=)"; exit 1; }

CDEV="/dev/mlx5_vfmig/$PF"
[ -e "$CDEV" ] || { echo "missing $CDEV (mlx5_core with CONFIG_MLX5_VFMIG loaded?)"; exit 1; }
[ -x "$TOOL" ] || { echo "build $TOOL first: make -C $ROOT_DIR"; exit 1; }
command -v ib_write_bw >/dev/null 2>&1 || { echo "ib_write_bw not found (install perftest)"; exit 1; }

# Single-instance guard: two concurrent runs racing sriov_numvfs on the
# same PF can wedge a VF teardown with the PF dev->mutex held (needs a
# reboot). Refuse to run if another instance holds the per-PF lock.
LOCK="/tmp/criu_rdma_pf_${PF//[:.]/_}.lock"
exec 9>"$LOCK"
if ! flock -n 9; then
    echo "another harness already holds $LOCK for $PF -- refusing to run concurrently"
    exit 1
fi

SKIP_RC=77   # report a teardown wedge as WEDGE to run_all_harnesses.sh

vf_path()       { echo "/sys/bus/pci/devices/$1"; }
wait_for_path() { for i in $(seq 1 50); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }
netdev_for_pci() { ls "$(vf_path $1)/net/" 2>/dev/null | head -n1; }
ibdev_for_pci() {
    local bdf=$1 d pci
    for d in /sys/class/infiniband/*; do
        [ -e "$d/device" ] || continue
        pci=$(basename "$(readlink "$d/device")")
        [ "$pci" = "$bdf" ] && { basename "$d"; return 0; }
    done
    return 1
}

PASS=0
FAIL=0
pass() { echo "  PASS: $*"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }
note() { echo "  NOTE: $*"; }

run_tool() {
    set +e
    TOOL_OUT=$(sudo "$TOOL" "$PF" "$@" 2>&1)
    TOOL_RC=$?
    set -e
    echo "    \$ $TOOL $PF $* -> rc=$TOOL_RC"
    [ -n "$TOOL_OUT" ] && echo "$TOOL_OUT" | sed 's/^/      /'
    return 0
}

# dmesg "datapath A->B" transition since last dmesg -C ?
saw_dp() { sudo dmesg | grep -Eq "vfmig: (suspended|resumed) VF 0 .*datapath $1"; }

HEALTH_RE='([Ff]atal error|NIC_DISABLED|firmware internal error|bad system state|cmd_ent.*timeout|mlx5_cmd.*[Tt]imeout|health.*[Ff]ail|SUSPEND_VHCA.*failed|RESUME_VHCA.*failed)'
health_bad() { sudo dmesg | grep -Eq "$HEALTH_RE"; }
tx_timeout() { sudo dmesg | grep -Eq "TX timeout"; }

# ib_write_bw data rows: "#bytes #iters peak avg msgrate". Bounded (-D)
# runs exit and flush, so parsing the last avg is reliable here.
bw_last_avg() { grep -E '^[[:space:]]*[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9.]+[[:space:]]+[0-9.]+' "$1" 2>/dev/null | tail -n1 | awk '{print $4}'; }

# Run one bounded ib_write_bw exchange <client_ibdev> -> <server_ibdev>
# writing to <server_ip>. Server + client both -D-bounded so they exit
# cleanly (no SIGKILL of a live RDMA app -> avoids the uverbs-SRCU
# teardown wedge). Sets LAST_BW. Returns client rc.
SLOG=/tmp/p2p_ibw_server.log
CLOG=/tmp/p2p_ibw_client.log
LAST_BW=""
ibw_exchange() {
    local sib=$1 cib=$2 sip=$3 port=$4 rc
    : > "$SLOG"; : > "$CLOG"
    sudo timeout 25 ib_write_bw -d "$sib" -p "$port" ${GIDX:+-x $GIDX} -D 4 >"$SLOG" 2>&1 &
    local spid=$!
    sleep 2
    set +e
    sudo timeout 25 ib_write_bw -d "$cib" -p "$port" ${GIDX:+-x $GIDX} -D 4 "$sip" >"$CLOG" 2>&1
    rc=$?
    set -e
    wait "$spid" 2>/dev/null || true
    LAST_BW=$(bw_last_avg "$CLOG")
    return $rc
}

VF0=""; VF1=""; ND0=""; ND1=""
nd_down() { [ -n "$ND0" ] && sudo ip link set "$ND0" down 2>/dev/null || true;
            [ -n "$ND1" ] && sudo ip link set "$ND1" down 2>/dev/null || true; }
cleanup() {
    sudo pkill -f "ib_write_bw" 2>/dev/null || true
    nd_down
    # Unbind cleanly (VFs are RUNNING + quiescent here, so this drains).
    for v in $VF0 $VF1; do
        [ -n "$v" ] && [ -e "/sys/bus/pci/drivers/mlx5_core/$v" ] && \
            timeout 90 bash -c "echo '$v' | sudo tee /sys/bus/pci/drivers/mlx5_core/unbind >/dev/null" 2>/dev/null || true
        [ -n "$v" ] && echo "" | sudo tee "$(vf_path $v)/driver_override" >/dev/null 2>&1 || true
    done
    echo 1 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null 2>&1 || true
    timeout 120 bash -c "echo 0 | sudo tee '$(vf_path $PF)/sriov_numvfs' >/dev/null" 2>/dev/null || true
}
trap cleanup EXIT

echo "============================================================"
echo " RUNNING_P2P hold-window probe (design/datapath_pause_resume.md D.4)"
echo "   PF=$PF  HOLD=${HOLD}s  subnet=${SUBNET}.0/24  (netdev-down model)"
echo "============================================================"

# --- setup ----------------------------------------------------------
echo "=== setup: provision + bind 2 VFs (VF0=DUT, VF1=peer) ==="
for fn in "$(vf_path $PF)"/virtfn*; do
    [ -e "$fn" ] || continue
    lv=$(basename "$(readlink "$fn")")
    [ -e "/sys/bus/pci/drivers/mlx5_core/$lv" ] && \
        timeout 60 bash -c "echo '$lv' | sudo tee /sys/bus/pci/drivers/mlx5_core/unbind >/dev/null" 2>/dev/null || true
done
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"            >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null
echo 2 | sudo tee "$(vf_path $PF)/sriov_numvfs"            >/dev/null
wait_for_path "$(vf_path $PF)/virtfn1" || { echo "FATAL: VFs did not appear"; exit 1; }
VF0=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
VF1=$(basename "$(readlink "$(vf_path $PF)/virtfn1")")
echo "VF0(DUT)=$VF0  VF1(peer)=$VF1"

run_tool enable_migratable 0; [ "$TOOL_RC" -eq 0 ] || { echo "FATAL: enable_migratable 0"; exit 1; }
run_tool enable_migratable 1; [ "$TOOL_RC" -eq 0 ] || { echo "FATAL: enable_migratable 1"; exit 1; }

for v in $VF0 $VF1; do
    echo mlx5_core | sudo tee "$(vf_path $v)/driver_override" >/dev/null
    timeout 120 bash -c "echo '$v' | sudo tee /sys/bus/pci/drivers/mlx5_core/bind >/dev/null" 2>/dev/null || true
    [ -e "/sys/bus/pci/drivers/mlx5_core/$v" ] || { echo "FATAL: bind $v to mlx5_core failed"; exit 1; }
done
sleep 2

ND0=$(netdev_for_pci $VF0); ND1=$(netdev_for_pci $VF1)
IB0=$(ibdev_for_pci $VF0);  IB1=$(ibdev_for_pci $VF1)
echo "VF0: netdev=$ND0 ibdev=$IB0 | VF1: netdev=$ND1 ibdev=$IB1"
[ -n "$ND0" ] && [ -n "$ND1" ] && [ -n "$IB0" ] && [ -n "$IB1" ] || { echo "FATAL: missing netdev/ibdev after bind"; exit 1; }

nd_up_ips() {
    sudo ip addr flush dev "$ND0" 2>/dev/null || true
    sudo ip addr flush dev "$ND1" 2>/dev/null || true
    sudo ip link set "$ND0" up; sudo ip link set "$ND1" up
    sudo ip addr add "$SUBNET.1/24" dev "$ND0" 2>/dev/null || true
    sudo ip addr add "$SUBNET.2/24" dev "$ND1" 2>/dev/null || true
    sleep 2
}
nd_up_ips

# --- subtest 1: baseline datapath works (netdev up, RUNNING) ---------
echo "=== subtest 1: baseline peer<->DUT RDMA works ==="
if ibw_exchange "$IB0" "$IB1" "$SUBNET.1" 18515 && [ -n "$LAST_BW" ] && awk "BEGIN{exit !($LAST_BW > 0)}"; then
    pass "baseline ib_write_bw VF1->VF0 flowed (avg ${LAST_BW} MB/s)"
else
    fail "baseline ib_write_bw VF1->VF0 did not flow (avg ${LAST_BW:-none}) -- VF<->VF path down"
    echo "  (client log tail:)"; tail -n 12 "$CLOG" | sed 's/^/    /'
    echo "==== hold-window summary ===="; echo "  PASS: $PASS"; echo "  FAIL: $FAIL"
    echo "RESULT: FAIL"; exit 1
fi

# --- quiesce netdevs BEFORE parking (Appendix B) --------------------
echo "=== quiesce: bring both VF netdevs DOWN before parking ==="
nd_down
sleep 1
sudo dmesg -C

# --- subtest 2: park DUT initiator -> RUNNING_P2P -------------------
echo "=== subtest 2: SUSPEND_VHCA(INITIATOR) on DUT -> RUNNING_P2P ==="
run_tool suspend_vhca 0 initiator
if [ "$TOOL_RC" -eq 0 ] && saw_dp "0->1"; then
    pass "DUT parked to RUNNING_P2P (datapath 0->1)"
else
    fail "DUT did not reach RUNNING_P2P (rc=$TOOL_RC)"
fi

# DO NOT read the DUT VF's own FW counters (e.g.
# hw_counters/rx_write_requests) while it is parked. Such a read issues
# a QUERY_Q_COUNTER on the *parked VF's* command ring, which was found
# to hang the ring at RUNNING_P2P (netdev down): the cat wedges in an
# unkillable D-state FW-command wait (timeout's SIGKILL cannot reap a
# D-state task) and then stalls the subsequent SR-IOV teardown -- a hard
# host wedge that needs a reboot. Responder liveness during a real
# barrier is proven by the *peer's* completions, never a DUT-side query
# on the parked ring. See design/datapath_pause_resume.md D.4.

# --- subtest 3: quiescent soak --------------------------------------
echo "=== subtest 3: idle soak ${HOLD}s at RUNNING_P2P (netdev down) ==="
sleep "$HOLD"
if health_bad; then
    fail "FW health / command-ring signature during hold"
    sudo dmesg | grep -E "$HEALTH_RE" | sed 's/^/    /' | tail -n 10
else
    pass "no FW health / command-ring signature during ${HOLD}s hold"
fi
if tx_timeout; then
    fail "netdev TX timeout during hold -- netdev was not quiescent"
    sudo dmesg | grep -E "TX timeout" | sed 's/^/    /' | tail -n 4
else
    pass "no netdev TX timeout during hold (netdev-down quiesce held)"
fi

# --- subtest 4: resume proves no decay ------------------------------
echo "=== subtest 4: RESUME_VHCA(INITIATOR) -> RUNNING (proves no decay) ==="
run_tool resume_vhca 0 initiator
if [ "$TOOL_RC" -eq 0 ] && saw_dp "1->0"; then
    pass "DUT resumed RUNNING via 1->0 (stayed at P2P the whole hold)"
else
    fail "resume-initiator did not log 1->0 (rc=$TOOL_RC) -- VHCA decayed or errored"
fi

# --- subtest 5: datapath fully recovers after the hold --------------
echo "=== subtest 5: DUT datapath recovers post-hold (initiator + responder) ==="
nd_up_ips
# DUT as CLIENT -> exercises the just-resumed initiator direction.
if ibw_exchange "$IB1" "$IB0" "$SUBNET.2" 18516 && [ -n "$LAST_BW" ] && awk "BEGIN{exit !($LAST_BW > 0)}"; then
    pass "DUT initiator originates after resume (VF0->VF1 avg ${LAST_BW} MB/s)"
else
    fail "DUT initiator did not work after resume (avg ${LAST_BW:-none})"
    tail -n 12 "$CLOG" | sed 's/^/    /'
fi
# DUT as SERVER -> exercises the responder direction post-hold.
if ibw_exchange "$IB0" "$IB1" "$SUBNET.1" 18517 && [ -n "$LAST_BW" ] && awk "BEGIN{exit !($LAST_BW > 0)}"; then
    pass "DUT responder serves after resume (VF1->VF0 avg ${LAST_BW} MB/s)"
else
    fail "DUT responder did not serve after resume (avg ${LAST_BW:-none})"
    tail -n 12 "$CLOG" | sed 's/^/    /'
fi

# --- summary --------------------------------------------------------
echo
echo "==== RUNNING_P2P hold-window summary (HOLD=${HOLD}s) ===="
echo "  PASS: $PASS"
echo "  FAIL: $FAIL"
if [ "$FAIL" -ne 0 ]; then
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: PASS"
exit 0
