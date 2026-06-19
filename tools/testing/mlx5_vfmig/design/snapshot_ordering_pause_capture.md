# DESIGN: snapshot ordering -- split datapath *pause* from state *capture*
#         so DMA is quiesced before CRIU copies the dumpee's memory
#
# Audience: kernel agent (drivers/infiniband/sw/rxe, drivers/net/.../mlx5)
# Peer doc on the CRIU side: plugin + cr-dump/cr-restore hook wiring
# Priority: mlx5 first (largest blast radius), rxe second.

## TL;DR

CRIU today freezes the *dumpee's CPU threads* (ptrace, at
`collect_pstree()`) and then **copies the process memory** in the
`dump_one_task()` loop (cr-dump.c:2286) **before** the RDMA plugin
quiesces the *datapath* in the late `RDMA_DUMP_UOBJ` hook
(cr-dump.c:2299). Freezing userspace does **not** stop the DMA engine:

- **rxe**: the `rxe_sender` / `rxe_responder` kernel tasks keep running.
  A peer that pushes RDMA WRITE / SEND lands payload into the dumpee's
  pinned MR / recv-buffer pages *after* those pages were copied into the
  image, and the QP cursors captured later are ahead of that memory.
- **mlx5**: worse -- `SUSPEND_VHCA` is bundled inside `SAVE_VHCA_STATE`
  which the plugin defers to fini, so the VF hardware DMAs into user
  memory throughout the entire memory-dump window.

This violates the stop-and-copy invariant of live migration: **quiesce
DMA, then snapshot memory**. Our existing tests don't trip it only
because the workload self-quiesces and no peer is pushing during dump.

The fix is to **split "pause" from "capture"** and reuse hooks that
already exist (the CUDA plugin is the precedent):

```
  CRIU dump timeline (cr-dump.c)
  ------------------------------------------------------------------
  collect_pstree()         ptrace-freeze userspace threads
  rdma_check_dump_coverage() (2230)   netlink QP enumeration (frozen)
  >>> CHECKPOINT_DEVICES   (2255)  *** NEW: issue PAUSE here ***
        rxe : FREEZE_DATAPATH (per-QP, or ctx-wide variant)
        mlx5: SUSPEND_VHCA ioctl (new, split out of SAVE)
  dump_one_task() loop     (2286)  copy process memory  <- now safe
  rdma_dump_uobj_dag()     (2299)  QUERY_QP / SAVE_VHCA_STATE (capture)
  ------------------------------------------------------------------

  CRIU restore timeline (cr-restore.c) -- the mirror
  ------------------------------------------------------------------
  (master-restore) create QP / LOAD_VHCA_STATE   ... datapath FROZEN
  PIE: map MR / ring VMAs, restore memory
  >>> RESUME_DEVICES_LATE  (2347)  *** NEW: issue RESUME here ***
        rxe : FREEZE_DATAPATH{freeze=0}
        mlx5: RESUME_VHCA ioctl (new, split out of LOAD/bind)
  ------------------------------------------------------------------
```

Capture (the heavy `QUERY_QP` / `SAVE_VHCA_STATE`) stays in the existing
late hook -- now correct because the datapath is already frozen. Only
the *pause* needs to move earlier, and the only window-feasibility
requirement (QP enumeration while frozen) is already proven by
`rdma_check_dump_coverage()`.

## Why moving the whole uobj-DAG earlier is NOT the answer

The uobj-DAG dump has a data dependency on the per-task ufile list, which
isn't built until `dump_one_task()`. The pause, by contrast, needs only
QP (or VHCA) *enumeration*, which is available at `CHECKPOINT_DEVICES`.
So we hoist the cheap pause, not the expensive capture.

---

## Part A (priority): mlx5 -- split SUSPEND/RESUME out of SAVE/LOAD

### A.0 Current shape (what we're changing)

- `MLX5_VFMIG_IOC_SAVE_VHCA_STATE` (0x05) synchronously does
  `SUSPEND_VHCA(INITIATOR)` + `SUSPEND_VHCA(RESPONDER)` ->
  `QUERY_VHCA_MIGRATION_STATE` -> alloc PD/pages/MKEY ->
  `SAVE_VHCA_STATE`, then **resumes on `save_fd` close** unless
  `MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED`. (`vfmig.c:4827-4845`,
  release path `:5001-5009`.)
- The suspend state lives only in the transient `struct
  mlx5_vfmig_save_ctx` (`suspended_initiator` / `suspended_responder`,
  `vfmig.c:4140`).
- Restore bind path runs `SUSPEND_VHCA -> LOAD_VHCA_STATE -> RESUME_VHCA`
  inline inside the VF probe (`vfmig.c:5887-5939`).

The suspend is cheap and fast; the SAVE (PD + pages + MKEY +
`SAVE_VHCA_STATE`) is heavy. We want the cheap part early and the heavy
part late.

### A.1 New persistent per-VF suspend state

Suspend is now issued *before* any save session exists, so the
suspended bit must move from the per-session `save_ctx` to the
persistent per-VF context that already holds `vfmig_tracked` /
`vfmig_pending_load`:

```c
/* mlx5_core_sriov.vfs_ctx[vf_id] (same home as vfmig_tracked) */
u8  vfmig_suspended;       /* 1 once SUSPEND ioctl latched both dirs */
u8  vfmig_defer_resume;    /* restore: bind LOADs but leaves suspended;
                            * RESUME ioctl (late hook) finishes it */
```

Lifecycle / teardown: `mlx5_device_disable_sriov()` already drops
`pending_load` slots; it must also force-`RESUME_VHCA` (best-effort) and
clear `vfmig_suspended` for any VF still parked, so a crashed dumper
can't strand a VF suspended.

### A.2 New ioctls (next free number is 0x13)

```c
/* MLX5_VFMIG_IOC_SUSPEND_VHCA -- early-hook quiesce.
 *   SUSPEND_VHCA(INITIATOR) then SUSPEND_VHCA(RESPONDER) on vf_id's
 *   vhca_id (PF-issued, other_function=1). Idempotent: returns 0 with
 *   no FW traffic if vfmig_suspended is already set. Latches
 *   vfs_ctx[vf_id].vfmig_suspended = 1.
 *   Requires migratable (same gate as SAVE). VF may be bound or unbound.
 */
struct mlx5_vfmig_suspend_vhca { __u32 vf_id; __u32 flags; __u32 reserved[2]; };
#define MLX5_VFMIG_IOC_SUSPEND_VHCA _IOW(MAGIC, 0x13, ...)

/* MLX5_VFMIG_IOC_RESUME_VHCA -- late-hook un-quiesce / abort rollback.
 *   RESUME_VHCA(RESPONDER) then RESUME_VHCA(INITIATOR). Idempotent:
 *   returns 0 if not suspended. Clears vfmig_suspended + vfmig_defer_resume.
 */
struct mlx5_vfmig_resume_vhca { __u32 vf_id; __u32 flags; __u32 reserved[2]; };
#define MLX5_VFMIG_IOC_RESUME_VHCA _IOW(MAGIC, 0x14, ...)
```

### A.3 SAVE_VHCA_STATE becomes suspend-aware (back-compatible)

```
on SAVE:
  if vfs_ctx[vf_id].vfmig_suspended:
      skip the in-SAVE SUSPEND_VHCA pair;   # already parked by early hook
      record ctx->owns_suspend = false;     # caller owns resume
  else:
      SUSPEND_VHCA(INIT) + SUSPEND_VHCA(RESP);  # legacy self-suspend
      ctx->owns_suspend = true;
  ... QUERY_SIZE / alloc / SAVE_VHCA_STATE (unchanged) ...

on save_fd close:
  if ctx->owns_suspend and not KEEP_SUSPENDED:
      RESUME pair;                          # legacy behavior preserved
  else:
      leave parked;                         # caller (CRIU) will RESUME
```

This keeps every existing harness (`test_iova_tracked_save_load.sh`
et al.) working unchanged: they never call the new SUSPEND ioctl, so
SAVE self-suspends and resumes on close exactly as before. CRIU's new
flow calls SUSPEND early, then SAVE with `KEEP_SUSPENDED`.

### A.4 Restore mirror: where RESUME can and cannot be deferred

The naive mirror of the SAVE-side pause would be to bring the destination
VF up with the datapath **already frozen** and resume only after CRIU has
restored the MR/ring VMAs. The kernel mechanism for that exists:

- `vfmig_defer_resume` in the per-VF ctx, set by CRIU via the
  `MLX5_VFMIG_MARK_RESTORED_DEFER_RESUME` flag.
- `mlx5_vfmig_vf_apply_pending_load()` (`vfmig.c:5887-5939`): when
  `vfmig_defer_resume`, do `SUSPEND_VHCA -> LOAD_VHCA_STATE` and **skip
  the RESUME pair**, leaving `vfmig_suspended = 1`.
- CRIU calls `MLX5_VFMIG_IOC_RESUME_VHCA` at `RESUME_DEVICES_LATE`
  (cr-restore.c:2347), after all VMAs are mapped.

**This "defer through bind" path is INFEASIBLE for a host-bound
`mlx5_core` VF**, and that's a firmware/command-ring fact, not a bug we
can fix:

- `SUSPEND_VHCA` puts the VHCA in `STOP`, where the VF **command ring is
  dead** (it needs at least `RUNNING_P2P`). The firmware always suspends
  initiator-then-responder and resumes responder-then-initiator, so there
  is no "responder parked, cmd ring alive" state to exploit.
- The host `mlx5_core` probe issues `QUERY_HCA_CAP` (and then a long tail
  of ring commands: ALLOC_UAR, MANAGE_PAGES, CREATE_EQ, CREATE_QP/CQ/...)
  on the VF ring. With the VHCA parked before/through probe, the very
  first `QUERY_HCA_CAP` hangs until the ~60s command timeout, wedging the
  bind. Observed directly as the `iova_tracked_save_load` failure
  (`QUERY_HCA_CAP` pid stuck ~60s, then `resume_vhca` returns "vf_id 0 out
  of range" because SR-IOV teardown already reclaimed the ctx).

So for a host-bound restore there is **no parked-through-bind window**.
The load-bearing guardrail is the **SAVE-side suspend** (A.1-A.3): only
the source device can stop its live peers from DMAing into memory that is
mid-snapshot. On restore, "memory restored before the datapath goes live"
is enforceable by the CRIU/orchestrator ordering (map VMAs in PIE, then
bring the device up) without any kernel-side defer.

#### A.4.1 Viable restore-side guardrail: bind live, then *post-bind* park

If belt-and-suspenders restore-side gating is wanted, the feasible shape
is to park **after** the probe's command-ring work is done -- never
through it:

```
bind VF                -> RUNNING (inline resume; probe cmd ring works)
configure (RUNNING):      ip link set vf mac -> ip addr add (GID) -> ip neigh
SUSPEND_VHCA           -> STOP   (responder OFF; guardrail window begins)
CRIU restores VMAs / process memory   (pure host memory writes, no FW cmds)
RESUME_VHCA            -> RUNNING (guardrail window ends; datapath live)
```

Validated experimentally on this tree (08:00.0, FW 28.48.1000), see
A.6. Constraints that fall out of the command-ring coupling:

- **GID programming must happen while RUNNING.** `ip addr add` ->
  `mlx5_ib_add_gid` -> `mlx5_core_roce_gid_set(dev->mdev, ...)` issues
  `SET_ROCE_ADDRESS` on the **VF** ring, so it must precede the park.
- **VF MAC and static neighbors work while parked.** `ip link set <PF>
  vf N mac` is a PF-side eswitch op (PF ring); `ip neigh` is pure host
  ARP state -- neither touches the VF ring.
- **The VF netdev must stay quiescent while parked.** An admin-up netdev
  (stats, ethtool, link events) can issue a VF-ring command that queues
  on the dead ring and times out at ~60s. The realistic restore window
  keeps the netdev down, so this is a documented constraint, not a hazard
  we lean on.
- Combined with the natural **MAC fence** (a peer cannot even address the
  VF until its source-time MAC is set -- see `qp_av_dmac_swap.md`), this
  gives defense-in-depth: the restored VF is datapath-inaccessible until
  memory is in place *and* it is configured *and* resumed.

The `vfmig_defer_resume` kernel mechanism is **retained** (it is harmless,
idempotent, and validated by `test_suspend_resume_split.sh` /
`test_synthetic_load_plumbing.sh`) for the non-host-bound case (VFIO /
VM-assigned VF, where no host `mlx5_core` probe runs), but the
host-bound restore harnesses do **not** use defer-through-bind.

Legacy (non-CRIU) restore paths leave `vfmig_defer_resume = 0` and keep
the inline resume during bind.

### A.5 mlx5 test asks

- New harness `save_load/test_suspend_resume_split.sh`: SUSPEND (early)
  -> verify VF is parked (a wire op stalls / a counter freezes) ->
  SAVE{KEEP_SUSPENDED} -> RESUME -> verify datapath live again.
- Regression: confirm the legacy `test_iova_tracked_save_load.sh` is
  byte-identical green (SAVE still self-suspends + resumes on close).
- Abort path: SUSPEND then RESUME with no SAVE in between (rollback).

### A.6 Experimental validation -- post-bind park on a host-bound VF

Run on 08:00.0 (FW 28.48.1000) via a throwaway harness
(`/tmp/test_bind_suspend_resume.sh`, exploratory; not committed). All
bind/unbind steps were bounded so a wedge could not hang the runner.

Sequence: provision 1 VF (autoprobe off) -> `enable_migratable` ->
`driver_override=mlx5_core` -> bind (RUNNING) -> `SUSPEND_VHCA` -> PF-side
`ip link set <PF> vf 0 mac` -> idle hold -> `RESUME_VHCA` -> `query_vf`.

Result (both an 8s and a full 70s idle hold -- the latter spans a whole
~60s command-timeout interval): **clean PASS, 8/8 checks**.

- Bind brought up netdev + IB device while `RUNNING` (probe cmd ring OK).
- `SUSPEND_VHCA` on the *bound, running* VF returned 0 and logged the
  snapshot-ordering pause line; no health/firmware events.
- PF-side `ip link set vf mac` succeeded **while the VF was parked** and
  the vport MAC took effect.
- 70s parked idle hold: **no** cmd-timeout, **no** health/firmware events
  (i.e. an idle, quiescent host driver does not autonomously poke the
  dead VF ring within a full command-timeout window; health poll is MMIO,
  not a ring command).
- `RESUME_VHCA` returned 0; post-resume `query_vf` confirmed the ring is
  live again.

Conclusion: post-bind park (A.4.1) is mechanically safe for a host-bound
VF. What is *not* safe is parking through probe (A.4) -- that wedges on
`QUERY_HCA_CAP`. The one untested-on-purpose edge is an *active* (admin-up,
traffic-carrying) VF netdev during the parked window; keep it down.

---

## Part B: rxe -- context-level freeze-all variant

`RXE_IB_METHOD_VFMIG_FREEZE_DATAPATH` (rxe_vfmig.c:71) is per-QP: it
resolves a QP uobject handle and pauses that QP's tasks. At the early
`CHECKPOINT_DEVICES` hook CRIU has the *context* fd but has not yet
resolved/dumped per-QP fds, so a per-QP freeze is awkward.

### B.1 Proposed: a context-scoped freeze method

Add `RXE_IB_METHOD_VFMIG_FREEZE_CONTEXT` taking a `UVERBS_OBJECT_..`
context-ish handle (or operate over all QPs owned by the calling
ufile/ucontext) and `freeze:u8`:

```
freeze=1: for each rxe_qp owned by this ucontext: rxe_qp_pause(qp)
freeze=0: ... rxe_qp_resume(qp)
```

Implementation note: rxe can enumerate a ucontext's QPs via the
uverbs/uobject list for that ufile, or via the rxe qp pool filtered by
owning pd->ucontext. The method only needs to be idempotent and to skip
kernel-mode QPs (`!qp->is_user`), mirroring the per-QP handler's guard.

### B.2 Why this is "convenience, not required"

CRIU *can* freeze per-QP if it enumerates QPs at the early hook (it
already does a netlink dump there). The context-wide method just removes
the need to resolve each QP handle before fds are dumped and makes the
early hook a single call. We'll land the per-QP path first (already
exists) and add the context variant as an ergonomic follow-up.

### B.3 rxe test asks

- Extend `uverbs_ctx_holder` (CRIU side) with a peer that keeps pushing
  RDMA WRITE during the dump window; assert the post-restore image is
  consistent only when the early-hook freeze is wired (regression for
  the ordering bug itself).

---

## Sequencing / blast radius

1. **mlx5 SUSPEND/RESUME split (Part A)** -- priority. Touches the SAVE
   ioctl semantics + the bind/restore path + adds two ioctls + persistent
   per-VF state. Self-contained in `core/vfmig/vfmig.c` +
   `uapi/linux/mlx5_vfmig.h`. Back-compatible by construction (A.3).
2. **rxe context-freeze (Part B)** -- small, additive uverbs method.
3. **CRIU hook wiring** -- peer-side work (CHECKPOINT_DEVICES /
   RESUME_DEVICES_LATE), tracked in the CRIU repo.

The in-flight rxe QP *capture* (the variable-length ring + resp.resources
serialization) is a separate, larger job tracked in
`design/rxe_inflight_qp_restore.md`; it depends on Part B being the
consistency point but is otherwise orthogonal.
