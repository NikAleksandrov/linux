# DESIGN: snapshot ordering -- datapath *pause* / state *capture* split
#         for CRIU RDMA migration (mlx5 + rxe)
#
# Audience: kernel agent (drivers/infiniband/sw/rxe, drivers/net/.../mlx5)
# Peer doc on the CRIU side: plugin + cr-dump/cr-restore hook wiring
# Scope: mlx5 (largest blast radius) and rxe control planes.
# Status: Part A (mlx5 SUSPEND/RESUME split) and Part B (rxe freeze
#         methods) are IMPLEMENTED. Part C (cross-host directional
#         resume, H1) is PROPOSED. See the change history in Appendix C.

## Overview

Checkpointing requires that the datapath DMA into a process's pinned
memory be quiesced *before* CRIU copies that memory into the image;
otherwise the copied pages and the QP/VHCA cursors captured later
disagree. The restore side has the mirror hazard: if the datapath is
brought back live *before* the memory (MR / ring VMAs) and device state
are fully in place, the device DMAs against half-restored memory, and a
peer's traffic can race an endpoint that is only partway up -- corrupting
the image or wedging the connection rather than cleanly resuming it. RDMA
breaks the naive CRIU assumption that ptrace-freezing the process threads
is enough, because the DMA engine is independent of the CPU threads:

- **rxe**: the `rxe_sender` / `rxe_responder` kernel tasks keep running
  even with userspace frozen. A peer pushing RDMA WRITE / SEND lands
  payload into pinned MR / recv-buffer pages, so without an explicit
  pause the copied image and the later-captured QP cursors disagree.
- **mlx5**: the VF hardware DMAs autonomously; the datapath is only
  stopped by an explicit `SUSPEND_VHCA`.

The design therefore **splits "pause" from "capture"**: a cheap pause is
issued early (as soon as the QP/VHCA set can be enumerated), and the
expensive capture stays in the existing late hook -- now correct because
the datapath is already frozen. The pause/capture split is the common
principle; the concrete pause and capture primitives are
device/transport-specific (mlx5 capture: `SAVE_VHCA_STATE`; rxe capture:
`QUERY_QP` + ring serialization -- see Parts A and B). Throughout this
doc, sections and terms are tagged mlx5 vs rxe wherever the detail is
specific to one. The pattern reuses CRIU hooks that already exist (the
CUDA plugin is the precedent).

### Canonical timelines (how it works today)

```
  CRIU dump timeline (cr-dump.c)
  ------------------------------------------------------------------
  collect_pstree()          ptrace-freeze userspace threads
  rdma_check_dump_coverage() (2230)   netlink QP enumeration (frozen)
  >>> CHECKPOINT_DEVICES    (2255)  PAUSE:
        rxe : FREEZE_CONTEXT (ctx-wide) or FREEZE_DATAPATH (per-QP)
        mlx5: MLX5_VFMIG_IOC_SUSPEND_VHCA
  dump_one_task() loop      (2286)  copy process memory  <- now safe
  rdma_dump_uobj_dag()      (2299)  QUERY_QP / SAVE_VHCA_STATE (capture)
  ------------------------------------------------------------------

  CRIU restore timeline (cr-restore.c) -- the mirror
  ------------------------------------------------------------------
  (master-restore) create QP / LOAD_VHCA_STATE   ... datapath FROZEN
  PIE: map MR / ring VMAs, restore memory
  >>> RESUME_DEVICES_LATE   (2347)  RESUME:
        rxe : FREEZE_CONTEXT / FREEZE_DATAPATH {freeze=0}
        mlx5: MLX5_VFMIG_IOC_RESUME_VHCA
  ------------------------------------------------------------------
```

The only window-feasibility requirement -- QP/VHCA *enumeration* while
frozen -- is already proven by `rdma_check_dump_coverage()`. Why the
whole uobj-DAG is *not* hoisted instead is in Appendix A.

---

## Part A: mlx5 control plane -- SUSPEND / RESUME split out of SAVE / LOAD

The mlx5 datapath pause/resume is exposed as two dedicated ioctls on
`/dev/mlx5_vfmig/<pf_bdf>`, decoupled from the heavy SAVE/LOAD. This
section is the API reference for the implemented behavior.

### A.1 Firmware migration FSM (the substrate)

This is mlx5-specific. The firmware VHCA migration FSM has three states
relevant here, driven by per-direction `SUSPEND_VHCA` / `RESUME_VHCA`
commands (`vfmig_cmd_suspend_vhca` / `vfmig_cmd_resume_vhca`, op_mods
`MLX5_{SUSPEND,RESUME}_VHCA_IN_OP_MOD_*_{INITIATOR,RESPONDER}`):

```
  States (top = most live, bottom = most parked):
    RUNNING       both directions live; datapath fully up
    RUNNING_P2P   responder still answers peers; initiator quiesced;
                  command ring STILL LIVE
    STOP          fully parked; command ring DEAD

  Transitions (SUSPEND_VHCA parks a direction; RESUME_VHCA revives it):
    RUNNING      --SUSPEND_VHCA(INITIATOR)--> RUNNING_P2P
    RUNNING_P2P  --SUSPEND_VHCA(RESPONDER)--> STOP
    STOP         --RESUME_VHCA (RESPONDER)--> RUNNING_P2P
    RUNNING_P2P  --RESUME_VHCA (INITIATOR)--> RUNNING
```

So firmware parks initiator-first (RUNNING -> RUNNING_P2P -> STOP) and
revives responder-first (STOP -> RUNNING_P2P -> RUNNING). LOAD is only
accepted on a fully-parked (`STOP`) VHCA (else bad-parameter, syndrome
0x2c9bb0 on CX-7). The current ioctls always drive the full pair, i.e.
RUNNING<->STOP in one call; the intermediate `RUNNING_P2P` is not yet
individually addressable (that is Part C).

### A.2 The ioctls

```c
/* MLX5_VFMIG_IOC_SUSPEND_VHCA (_IOW(MAGIC, 0x13, ...)) -- early-hook quiesce.
 *   Issues SUSPEND_VHCA(INITIATOR) then SUSPEND_VHCA(RESPONDER) on the
 *   vf_id's vhca_id (PF-issued, other_function=1) -> STOP. Latches
 *   priv.sriov.vfs_ctx[vf_id].vfmig_suspended = 1. Idempotent: returns 0
 *   with no FW traffic if already suspended. Requires migratable (same
 *   gate as SAVE). VF may be bound or unbound. */
struct mlx5_vfmig_suspend_vhca { __u32 vf_id; __u32 flags; __u32 reserved[2]; };

/* MLX5_VFMIG_IOC_RESUME_VHCA (_IOW(MAGIC, 0x14, ...)) -- late-hook
 *   un-quiesce / abort rollback. Issues RESUME_VHCA(RESPONDER) then
 *   RESUME_VHCA(INITIATOR) -> RUNNING. Clears vfmig_suspended and
 *   vfmig_defer_resume. Idempotent: returns 0 with no FW traffic if not
 *   suspended. Best-effort: a failed FW resume is logged and surfaced,
 *   but the parked bits are cleared regardless (a stuck bit would wrongly
 *   suppress a future SAVE's self-suspend). */
struct mlx5_vfmig_resume_vhca { __u32 vf_id; __u32 flags; __u32 reserved[2]; };
```

`flags` is currently reserved (must be 0); Part C gives it meaning.

### A.3 Persistent per-VF state

Because SUSPEND is issued *before* any SAVE session exists, the suspend
bit lives in the persistent per-VF context (`mlx5_core_sriov.vfs_ctx[]`,
alongside `vfmig_tracked` / `vfmig_pending_load`), not in the transient
save context:

```c
/* include/linux/mlx5/driver.h : struct mlx5_vf_context (vfs_ctx[vf_id]) */
u8  vfmig_suspended;     /* 1 once SUSPEND_VHCA latched STOP */
u8  vfmig_defer_resume;  /* restore: bind LOADs but leaves suspended;
                          * RESUME_VHCA (late hook) finishes it */
```

Teardown safety: `mlx5_device_disable_sriov()` drops `vfmig_pending_load`
slots and also force-`RESUME_VHCA`s (best-effort) + clears
`vfmig_suspended` for any VF still parked, so a crashed/aborted dumper
that latched SUSPEND without a matching RESUME cannot strand a VF
suspended into the next provisioning.

### A.4 SAVE is suspend-aware (back-compatible)

`MLX5_VFMIG_IOC_SAVE_VHCA_STATE` still requires a suspended VHCA, but it
no longer *unconditionally* suspends. The thing it tracks is **ownership
of the resume**: if SAVE parked the VF itself, SAVE resumes it on close;
if the VF was already parked by an earlier `SUSPEND_VHCA`, SAVE leaves it
parked and the resume belongs to whoever issued that suspend. The state
that carries this is `ctx->owns_suspend` (per save session) plus
`ctx->suspended_initiator` / `ctx->suspended_responder` (which FW ops this
session actually issued, so close never issues a stray RESUME).

```
SAVE(vf_id, flags):                       # vfmig_ioc_save_vhca_state()
  if vfs_ctx[vf_id].vfmig_suspended:       # already parked by an earlier
      ctx->owns_suspend = false            #   SUSPEND_VHCA ioctl -> SAVE
                                           #   issues no suspend of its own
  else:                                    # standalone SAVE parks it:
      SUSPEND_VHCA(INITIATOR)              #   initiator (egress) first,
      SUSPEND_VHCA(RESPONDER)              #   then responder (ingress)
      ctx->owns_suspend = true
      ctx->suspended_{initiator,responder} = true
  QUERY_SIZE / alloc PD+pages+MKEY / SAVE_VHCA_STATE   # (unchanged)

close(save_fd):                            # vfmig_save_release_resources()
  free image resources
  if !ctx->owns_suspend:      return        # someone else owns the resume;
                                            #   leave parked (vfmig_suspended
                                            #   stays set), CRIU RESUMEs later
  if flags & KEEP_SUSPENDED:  return        # caller asked to stay parked
  RESUME_VHCA(RESPONDER)                    # else undo our own suspend, in
  RESUME_VHCA(INITIATOR)                    #   inverse order, best-effort,
                                            #   gated by suspended_* bools
```

Same code, two callers:
- **Standalone SAVE** (no prior suspend, no `KEEP_SUSPENDED`): suspends on
  entry, resumes on close -- byte-identical to before this split, so every
  existing harness (`test_iova_tracked_save_load.sh` et al.) is unaffected.
- **CRIU flow**: the early hook already issued `SUSPEND_VHCA` (so
  `vfmig_suspended` is set), SAVE sees `owns_suspend = false` and never
  touches the suspend state, close leaves the VF parked, and CRIU resumes
  it later with `RESUME_VHCA` at the late restore hook.

### A.5 Restore side: where RESUME can and cannot be deferred

The intended mirror of the SAVE-side pause is to bring the destination VF
up with the datapath frozen and resume only after the MR/ring VMAs are
restored. One mechanism for this is implemented today (defer-through-bind,
for VFIO/VM VFs); it is infeasible for host-bound VFs for a firmware
reason, which is why the load-bearing guardrail is the SAVE-side suspend.

**Defer-through-bind (VFIO / VM-assigned VFs only).**
`MLX5_VFMIG_MARK_RESTORED { DEFER_RESUME }` sets `vfmig_defer_resume`;
`mlx5_vfmig_vf_apply_pending_load()` then does `SUSPEND_VHCA ->
LOAD_VHCA_STATE` and **skips the RESUME pair**, leaving the VHCA in STOP.
CRIU later issues `MLX5_VFMIG_IOC_RESUME_VHCA` at `RESUME_DEVICES_LATE`.

**This defer-through-bind is INFEASIBLE for a host-bound `mlx5_core`
VF** -- a firmware/command-ring fact, not a fixable bug:

- STOP kills the VF command ring (it needs at least `RUNNING_P2P`), and
  the fused SUSPEND has no "responder parked, ring alive" stopping point.
- The host `mlx5_core` probe issues `QUERY_HCA_CAP` and a long tail of
  ring commands (ALLOC_UAR, MANAGE_PAGES, CREATE_EQ, CREATE_QP/CQ...).
  With the VHCA parked through probe, the first `QUERY_HCA_CAP` hangs to
  the ~60s command timeout and wedges the bind (observed as the
  `iova_tracked_save_load` failure: `QUERY_HCA_CAP` stuck ~60s, then
  `resume_vhca` returns "vf_id 0 out of range" because SR-IOV teardown
  already reclaimed the ctx).

So for a host-bound restore there is **no parked-through-bind window**.
The load-bearing guardrail is the **SAVE-side suspend** (A.2-A.4): only
the source can stop its live peers from DMAing into memory mid-snapshot.
On restore, "memory restored before the datapath goes live" is enforced
by CRIU/orchestrator ordering (map VMAs in PIE, then bring the device up)
without a kernel-side defer.

`vfmig_defer_resume` is **retained** for the non-host-bound case (VFIO /
VM-assigned VF, where no host `mlx5_core` probe runs); it is harmless,
idempotent, and validated by `test_suspend_resume_split.sh` /
`test_synthetic_load_plumbing.sh`. Legacy (non-CRIU) restore paths leave
`vfmig_defer_resume = 0` and keep the inline resume during bind.

**Future improvement (not in the code today): host-bound post-bind
park.** An optional restore-side guardrail for host-bound VFs -- bind to
RUNNING, then park *after* the probe's command-ring work rather than
through it -- has been validated experimentally but is **not wired into
the restore path**. It is described, with the command-ring constraints it
must respect, in Appendix B. Until it is integrated, the SAVE-side
suspend above is the only active guardrail for host-bound restores.

---

## Part B: rxe control plane -- datapath freeze methods

rxe exposes two uverbs methods on `RXE_IB_OBJECT_MIGRATE` (rxe_migrate.c),
both driving the same per-QP `rxe_qp_pause()` / `rxe_qp_resume()`
primitives. They are idempotent and order-independent with respect to
each other.

### B.1 FREEZE_DATAPATH (per-QP)

`RXE_IB_METHOD_FREEZE_DATAPATH` resolves a QP uobject handle and
pauses/resumes that single QP's sender/responder tasks. Used when CRIU
already has the per-QP fd resolved.

### B.2 FREEZE_CONTEXT (context-wide)

`RXE_IB_METHOD_FREEZE_CONTEXT` is a handle-less method taking only
`freeze:u8`. The caller is identified by `ib_uverbs_get_ucontext(attrs)`;
the QP set is enumerated from rxe's own `qp_pool` (a driver cannot reach
the core-internal ufile object walk):

```
freeze=1: for each rxe_qp owned by this ucontext: rxe_qp_pause(qp)
freeze=0: ...                                       rxe_qp_resume(qp)
```

Implementation note: the handler walks `qp_pool->xa` under
`rcu_read_lock()`, takes `kref_get_unless_zero()` on each live elem, drops
RCU around the sleeping pause/resume, then `rxe_put()`s and re-acquires.
QPs are filtered to user QPs (`qp->is_user`) owned by the calling
ucontext (`qp->ibqp.uobject->context == ucontext`); kernel QPs are
skipped.

This exists because at the early `CHECKPOINT_DEVICES` hook CRIU holds the
*context* fd but has not yet resolved/dumped the per-QP fds, so a per-QP
freeze is awkward. FREEZE_CONTEXT lets the early hook be a single call.
It is convenience, not strictly required: CRIU *can* freeze per-QP by
enumerating QPs from its early netlink dump; both paths exist.

The in-flight rxe QP *capture* (variable-length ring + `resp.resources`
serialization) is a separate, larger job tracked in
`design/rxe_inflight_qp_restore.md`; it depends on this freeze being the
consistency point but is otherwise orthogonal.

---

## Part C (PROPOSED): cross-host directional suspend/resume (H1)

> Status: PROPOSED, not implemented. Contingent on the C.0 syndrome
> confirmation. This is the fix shape for the ib_write_bw post-restore
> ERR if that failure is confirmed to be a cross-host ordering race --
> on the resume side (C.4) and/or the dump side (C.7) -- rather than the
> Stage-1 MKEY/MTT gap (see the Known Limitation).

### C.0 Motivation and validation gate

`ib_write_bw_vfmig_criu_swap` regressed on the new setups: after a CRIU
swap the one-sided WRITE client makes brief forward progress
(`last_acked_psn` advances, one ~17.5 Gb/s sample) and then its RC QP
flips to ERR, while the peer responder stays healthy in RTR. The
two-sided `rdma_test_agent_..._after_qp` control *passes* on the same
setup -- and its post-restore round-trip acts as an **implicit readiness
barrier** (a single request cannot complete until both endpoints are up,
and RC retries paper over the startup gap), whereas ib_write_bw resumes
streaming with no barrier.

H1: the restore path resumes **each VF's datapath fully and independently
per host** (Part A's bind-time inline resume drives the RESPONDER+
INITIATOR pair, `vfmig.c:4730-4745` / `:5183-5189`). With no cross-host
ordering guarantee, the client initiator can go live and post WRITEs
before the peer responder has resumed at the restored `epsn`; the stream
hits a divergence seam, retries `retry_count=7` times, and the QP goes to
ERR. This is the VFIO P2P quiescing problem, but *between hosts*.

**Validation gate**: H1 vs the Stage-1 MKEY/MTT gap is decided by the
error-CQE syndrome -- `0x15/0x16` (retry-exceeded) confirms H1;
`0x04/0x11` (local protection/access) points at the MTT gap. As of the
last run the syndrome had not been captured (no `mlx5_poll_one` error-cqe
line surfaced), and the "client restored before VF/link exists" sub-theory
was refuted by the restore timeline (both links active before CRIU
restore). Do not implement Part C until the syndrome confirms H1.

### C.1 Enabling primitive: RUNNING_P2P (already in firmware)

`RUNNING_P2P` (A.1) is exactly "responder live, initiator parked, command
ring alive". Both direction op_mods already exist in the driver; what is
missing is the ability for a caller to *stop at* RUNNING_P2P instead of
always driving the full RUNNING<->STOP pair.

### C.2 API change: directional flags (back-compatible)

No new ioctl numbers or structs: give meaning to the reserved `flags`
word on the existing `mlx5_vfmig_suspend_vhca` / `mlx5_vfmig_resume_vhca`
(A.2). `flags == 0` stays the fused pair, so today's callers are
byte-identical.

```c
#define MLX5_VFMIG_DIR_FLAG_INITIATOR (1u << 0)
#define MLX5_VFMIG_DIR_FLAG_RESPONDER (1u << 1)
/* flags == 0 is treated as INITIATOR|RESPONDER == the legacy fused pair. */

/* MLX5_VFMIG_IOC_SUSPEND_VHCA, flags = MLX5_VFMIG_DIR_FLAG_INITIATOR:
 *   RUNNING -> RUNNING_P2P. Parks only the initiator; the responder keeps
 *   answering peers and the command ring stays live. Sets
 *   vfs_ctx[vf_id].vfmig_dp_state = VFMIG_DP_P2P.
 *   -EINVAL unless the VHCA is currently RUNNING (VFMIG_DP_RUNNING).
 *
 * MLX5_VFMIG_IOC_RESUME_VHCA, flags = MLX5_VFMIG_DIR_FLAG_INITIATOR:
 *   RUNNING_P2P -> RUNNING. Revives the initiator. Sets vfmig_dp_state =
 *   VFMIG_DP_RUNNING. -EINVAL unless currently RUNNING_P2P.
 *
 * MLX5_VFMIG_IOC_RESUME_VHCA, flags = MLX5_VFMIG_DIR_FLAG_RESPONDER:
 *   STOP -> RUNNING_P2P. Revives the responder from a full park (the
 *   path that starts from a LOAD/defer, which leaves the VHCA in STOP).
 *   Sets vfmig_dp_state = VFMIG_DP_P2P. -EINVAL unless currently STOP.
 *
 * MLX5_VFMIG_IOC_SUSPEND_VHCA, flags = MLX5_VFMIG_DIR_FLAG_RESPONDER:
 *   RUNNING_P2P -> STOP. Deepens a P2P park to a full stop. Not used by
 *   the H1 restore flow, but IS the second phase of the dump-side
 *   two-phase quiesce (C.7).
 *   Sets vfmig_dp_state = VFMIG_DP_STOP. -EINVAL unless RUNNING_P2P.
 *
 * flags = 0 (both): RUNNING<->STOP in one call, exactly as A.2 today.
 */
```

The driver validates the requested direction against the current
`vfmig_dp_state` and returns `-EINVAL` for an out-of-order request (e.g.
`RESUME(INITIATOR)` on a STOP VHCA) rather than issuing an FW command
that will fault.

**Why the H1 restore flow issues no standalone `SUSPEND_VHCA(RESPONDER)`.**
The flow (C.4) never drives the destination VHCA down to STOP. It starts
from a fully-bound **RUNNING** VF (bind runs the fused resume), drops it
one step to **RUNNING_P2P** with `SUSPEND(INITIATOR)`, and later lifts it
back with `RESUME(INITIATOR)`. The `RUNNING_P2P -> STOP` edge --
`SUSPEND(RESPONDER)` -- is only needed to reach the full STOP that
SAVE/LOAD require, and that is already covered by the fused
`SUSPEND(flags=0)`. `RESUME(RESPONDER)` is defined because the *other*
restore path (defer-resume, which leaves the VHCA in STOP after LOAD) has
to climb `STOP -> RUNNING_P2P -> RUNNING`; if that path is combined with
the cross-host barrier, phase 1 is `RESUME(RESPONDER)` and phase 2 is
`RESUME(INITIATOR)`. Standalone `SUSPEND(RESPONDER)` is not on the
*restore* path at all -- but it is the second phase of the *dump*-side
two-phase quiesce (C.7), so it is not merely a symmetry knob.

### C.3 Per-VF state: distinguish STOP from RUNNING_P2P

Replace the single `vfmig_suspended` bool with a tri-state so teardown and
SAVE-skip logic can tell the two parked states apart:

```c
enum { VFMIG_DP_RUNNING = 0, VFMIG_DP_P2P, VFMIG_DP_STOP };
u8 vfmig_dp_state;
```

- SAVE self-suspend skip: skip iff `vfmig_dp_state != RUNNING`.
- Teardown force-resume: drive **any** non-RUNNING VF back to RUNNING
  (STOP needs the full pair; RUNNING_P2P needs only RESUME(INITIATOR)).
- Idempotency: an already-satisfied phase op returns 0, no FW traffic.

### C.4 Restore integration: post-bind initiator park

Reuse the *proven* bind->RUNNING arc (Appendix B) and park only the
initiator afterward -- do NOT bind straight into RUNNING_P2P through the
probe until that is proven safe (same command-ring hazard as A.5):

```
bind VF                 -> RUNNING (inline resume pair; probe cmd ring OK)
configure while RUNNING:    ip link set vf mac; ip addr add (GID); ip neigh
SUSPEND_VHCA(INITIATOR) -> RUNNING_P2P  (responder live; initiator parked)
CRIU restores VMAs / process memory
=== cross-host barrier: every migrated VF is RUNNING_P2P AND memory in place ===
RESUME_VHCA(INITIATOR)  -> RUNNING       (first WRITE hits a live, PSN-aligned peer)
```

Parking only the initiator is **one option**, not a kernel mandate. The
CRIU agent may instead resume both directions (the fused pair) exactly as
today, or use the two-phase form above; the kernel just exposes both and
the choice is CRIU-agent policy. The symmetric question on the *dump*
side, however, is more than a policy knob -- see C.7.

### C.5 Where the barrier lives (NOT the kernel)

The kernel only exposes the two phases and the `query_vf`-observable
`vfmig_dp_state`. The cross-host rendezvous ("hold all initiators until
every peer responder is up") is an **orchestrator / CRIU-plugin**
responsibility: `RESUME_DEVICES_LATE` grows two sub-steps with a barrier
between them -- responder-live (already the case here: bind reaches
RUNNING, then only the initiator is parked to RUNNING_P2P) ->
control-channel barrier -> resume-initiator.

### C.6 Open decisions

- [decision] flags on the existing ioctls (recommended: zero UAPI churn,
  back-compatible) vs. a new ioctl pair.
- [decision] tri-state `vfmig_dp_state` (cleaner, touches every current
  reader of `vfmig_suspended`) vs. adding a second
  `vfmig_initiator_parked` bit.
- [validate] RUNNING_P2P hold-window safety for a host-bound VF (mirror
  Appendix B but park at RUNNING_P2P via SUSPEND(INITIATOR); confirm no
  cmd-timeout / health event and that the responder still ACKs an
  incoming WRITE while parked).

### C.7 Dump-side symmetry: two-phase quiesce (candidate root cause)

The resume-ordering race (C.0) has a dump-side twin that may be the
*actual* corruption source. Today the SAVE side parks each VF with the
fused pair (`SUSPEND(INITIATOR)` then `SUSPEND(RESPONDER)` -> STOP),
**independently per host, with no cross-host barrier**. For a connected RC
pair that is not safe:

> If host A completes *both* suspends (reaches STOP) while host B is still
> RUNNING, B's initiator keeps sending to an A whose responder is already
> dead. Those in-flight requests are dropped, and A's captured responder
> state (`epsn`, `msn`, ...) does not account for them. On restore the two
> images disagree at exactly one PSN seam -- the shape we see in
> ib_write_bw. One side finishing both operations before the other starts
> either is precisely the hazard.

This is the classic reason VFIO quiesces an entire P2P group to
`RUNNING_P2P` before any member goes to STOP. The symmetric dump flow is
the inverse of the C.4 resume order:

```
SUSPEND_VHCA(INITIATOR) on every VF   -> all peers at RUNNING_P2P
                                         (no new requests originated;
                                          responders still drain + ACK)
=== cross-host barrier: all initiators parked, in-flight drained ===
SUSPEND_VHCA(RESPONDER) on every VF   -> all peers at STOP
capture (SAVE_VHCA_STATE)
```

Note this uses the standalone `SUSPEND_VHCA(RESPONDER)` phase that C.2
defines -- the dump side is exactly where that phase earns its keep, even
though the restore flow alone does not need it. A full H1 fix therefore
likely touches **both** the dump and restore sides. As with C.0, gate
implementation on confirming the failure is a PSN/ordering seam (the CQE
syndrome) before building the two-phase dump.

---

## Validation & tests

mlx5 (Part A):
- `save_load/test_suspend_resume_split.sh`: SUSPEND (early) -> verify VF
  parked (a wire op stalls / a counter freezes) -> SAVE{KEEP_SUSPENDED}
  -> RESUME -> verify datapath live again.
- Regression: `test_iova_tracked_save_load.sh` byte-identical green (SAVE
  still self-suspends + resumes on close).
- Abort path: SUSPEND then RESUME with no SAVE in between (rollback).

rxe (Part B):
- Extend `uverbs_ctx_holder` (CRIU side) with a peer that keeps pushing
  RDMA WRITE during the dump window; assert the post-restore image is
  consistent only when the early-hook freeze is wired (regression for the
  ordering bug itself).

---

## Known limitation (PARKED): post-restore reg_mr hangs in UMR on mlx5 VFs

Observed in `rdma_test_agent_vfmig_criu_swap_after_context`: after a vfmig
CRIU swap onto a host-bound VF (`mlx5_core`-bound, with a netdev + RDMA
device), the restored agent's first `ibv_reg_mr` wedges forever in
`mlx5r_umr_post_send_wait()` and the process is stuck in uninterruptible
`D` (SIGKILL has no effect). A later `sriov_numvfs=0` then also wedges in
`synchronize_srcu()` (`uverbs_disassociate_api_pre` -> `ib_uverbs_remove_one`)
-- but that teardown hang is purely secondary: `ib_unregister_device()` is
waiting on the uverbs SRCU read lock held by the stuck `reg_mr` ioctl. Fix
the primary and the teardown clears.

What it is NOT (ruled out from the `-b -1` journal):
- Not a resume-ordering / still-parked VHCA bug. The LOAD path logs
  `applied N bytes of LOAD state to vf 0 (vhca_id 0x...); resumed` every
  iteration -- the VHCA reaches firmware `RUNNING` before userspace runs.
- Not a teardown/SR-IOV-disable bug.
- Unrelated to the rxe work.

Root cause: the Stage-1 MKEY/UAR reconstitution gap. `reg_user_mr` on a
restored VF runs `create_real_mr -> mlx5r_umr_update_mr_pas ->
mlx5r_umr_post_send_wait()`, which programs the MR translation by posting a
WQE on the kernel UMR QP and ringing a UAR doorbell. On a restored VF the
UMR/UAR/MKEY translation resources are not reconstituted coherently with
the `LOAD_VHCA_STATE`'d firmware, so the WQE never completes. `umr.c`'s
`wait_for_completion()` is untimed, hence the permanent `D` state; the VF
then trips `poll_health` "Fatal error 3" (`MLX5_SENSOR_NIC_DISABLED`) and
`DEALLOC_UAR` fails with `bad resource state(0x9)`. This is the
`FIXME(stage2+)` in `core/dev.c` / `core/en_tx.c` ("rebuild kernel MKEYs
against destination IOVAs").

Fix class (Stage 2, not yet scoped): on LOAD, rebuild/rebind the kernel
MKEY + UAR + translation state against destination IOVAs so the UMR
datapath is coherent before userspace touches it.

Defense-in-depth (optional, separate decision): bound the UMR wait so a
restored-VF `reg_mr` fails cleanly (`-ETIMEDOUT`/`-EIO`) instead of wedging
the uverbs fd -> SRCU -> sriov teardown. This touches core mlx5 UMR shared
by all devices, so it needs care and is not part of this parking.

Next-time triage: full dmesg of the hung iteration -- expect `Fatal error
3` + `DEALLOC_UAR ... bad resource state` around the `reg_mr`; kernel
stack `mlx5r_umr_post_send_wait <- _mlx5r_umr_update_mr_pas <-
create_real_mr <- mlx5_ib_reg_user_mr <- ib_uverbs_reg_mr`.

Relationship to Part C / H1: this MKEY/MTT gap is the *other* candidate
for the ib_write_bw ERR (the `0x04/0x11` syndrome branch in C.0). The two
are distinguished by the CQE syndrome; whichever the syndrome points to is
the one to pursue.

---

## Appendix A: why the whole uobj-DAG is not hoisted instead

An alternative to hoisting only the pause would be to move the entire
uobj-DAG dump early. That does not work: the uobj-DAG dump has a data
dependency on the per-task ufile list, which isn't built until
`dump_one_task()`. The pause, by contrast, needs only QP (or VHCA)
*enumeration*, which is available at `CHECKPOINT_DEVICES`. So we hoist the
cheap pause, not the expensive capture.

## Appendix B: future improvement -- host-bound post-bind park

> Status: FUTURE IMPROVEMENT. The flow below is validated experimentally
> (see the run at the end of this appendix) but is **not** wired into the
> restore path. A.5 references it; the SAVE-side suspend remains the only
> active host-bound guardrail until this is integrated.

If restore-side gating is wanted on a host-bound VF, the feasible shape
is to bind to RUNNING and park *after* the probe's command-ring work --
never through it (parking through probe wedges on `QUERY_HCA_CAP`, A.5):

```
bind VF        -> RUNNING (inline resume; probe cmd ring works)
configure:        ip link set vf mac -> ip addr add (GID) -> ip neigh
SUSPEND_VHCA   -> STOP    (guardrail window begins)
CRIU restores VMAs / process memory   (pure host writes, no FW cmds)
RESUME_VHCA    -> RUNNING (guardrail window ends; datapath live)
```

Constraints that fall out of the command-ring coupling:
- **GID programming must happen while RUNNING**: `ip addr add` ->
  `mlx5_ib_add_gid` -> `mlx5_core_roce_gid_set` issues `SET_ROCE_ADDRESS`
  on the **VF** ring, so it must precede the park.
- **VF MAC and static neighbors work while parked**: `ip link set <PF> vf
  N mac` is a PF-side eswitch op; `ip neigh` is pure host ARP state.
- **The VF netdev must stay quiescent while parked**: an admin-up netdev
  can queue a VF-ring command on the dead ring and time out at ~60s. The
  realistic restore window keeps the netdev down.
- Combined with the natural **MAC fence** (a peer cannot address the VF
  until its source-time MAC is set -- see `qp_av_dmac_swap.md`), this is
  defense-in-depth: the restored VF is datapath-inaccessible until memory
  is in place *and* it is configured *and* resumed.

### Experimental validation of the flow above

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
  (an idle host driver does not autonomously poke the dead VF ring within
  a full command-timeout window; health poll is MMIO, not a ring command).
- `RESUME_VHCA` returned 0; post-resume `query_vf` confirmed the ring is
  live again.

Conclusion: post-bind park (A.5) is mechanically safe for a host-bound VF.
What is *not* safe is parking through probe -- that wedges on
`QUERY_HCA_CAP`. The one untested-on-purpose edge is an *active* (admin-up,
traffic-carrying) VF netdev during the parked window; keep it down.

## Appendix C: change history

- Initial design: identified the datapath-ordering violation (RDMA DMA
  continues after CRIU copies memory, and the mirror hazard on restore)
  and proposed splitting pause from capture, reusing CHECKPOINT_DEVICES /
  RESUME_DEVICES_LATE.
- Part A implemented: `MLX5_VFMIG_IOC_SUSPEND_VHCA` (0x13) /
  `RESUME_VHCA` (0x14); SAVE made suspend-aware (`owns_suspend` +
  `KEEP_SUSPENDED`); persistent `vfmig_suspended` / `vfmig_defer_resume`
  in `vfs_ctx[]`; teardown force-resume.
- Part A restore analysis: `defer_resume` found infeasible through a
  host-bound VF probe (`QUERY_HCA_CAP` ~60s wedge); retained for VFIO/VM
  VFs; post-bind park documented + experimentally validated (Appendix B).
- Part B implemented: rxe `FREEZE_DATAPATH` (per-QP) and `FREEZE_CONTEXT`
  (ctx-wide) uverbs methods.
- Documented the parked Stage-1 reg_mr/UMR hang limitation; corrected the
  stale `en_tx.c` claim that userspace verbs were unaffected.
- Part C proposed (H1): cross-host directional suspend/resume via
  RUNNING_P2P for the ib_write_bw post-restore ERR, gated on the
  CQE-syndrome confirmation. Added the dump-side two-phase quiesce (C.7)
  as a candidate root cause (one host reaching STOP while its peer still
  DMAs to it).
- Doc reworked from issue-status into a design doc: current-state
  architecture + API reference up front; rationale, experiment, and this
  history moved to appendices.
