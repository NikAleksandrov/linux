# vfmig: post-LOAD cmd-EQ ghost completion (known FW issue + workaround)

Status: **landed**. Workaround is the default starting with the commit
that flips `vfmig_load_skip_cmd_use_events` to default Y.

## 1. TL;DR

After `LOAD_VHCA_STATE` on the destination, FW intermittently emits a
**stale completion EQE on cmd_eq slot 0** before producing the
legitimate completion for the next cmd issued on the freshly-restored
VF. In event-driven cmd mode that stale EQE either hits an empty slot
(logged as `Command completion arrived after timeout (entry idx = 0)`)
or, worse, "consumes" the completion that should have been delivered
for a real cmd at slot 0 -- which then times out 60 seconds later with
`No done completion`. The victim cmd is whatever the kernel is issuing
when FW's lazy ghost lands; in practice that's `ALLOC_UAR(0x802)` or
`CREATE_MKEY(0x200)` from `mlx5e_create_mdev_resources()` during the
mlx5_core.eth aux probe.

The fix is to **leave the cmd interface in polling mode** for the
lifetime of the restored VF's mdev. Concretely: skip the
`mlx5_cmd_use_events()` call inside `create_async_eqs()` (and the
matching `mlx5_cmd_use_polling()` in `destroy_async_eqs()`) when
`mlx5_vf_is_restored(dev)` is true. The cmd interface stays in
`CMD_MODE_POLLING` end-to-end, every cmd against the mdev completes
via `lay->status_own` polling on the cmd ring, and the cmd_eq -- which
FW still produces EQEs into -- is simply not read by the host, so the
ghost EQE is harmless.

This mirrors what `vfio-pci-mlx5` (the upstream SR-IOV live-migration
driver) does implicitly: it never calls `mlx5_cmd_use_events()` on a
migrated VF, because in that model the VF's cmd interface is guest-
owned. Vfmig drives the VF cmd interface from the host driver, which
is the only architectural difference that exposes the FW glitch.

The behaviour is gated on `mlx5_vf_is_restored(dev)`, so non-vfmig VFs
and PFs are entirely unaffected.

## 2. Failure signature

The most reliable way to identify this bug in dmesg is the pair:

```
mlx5_core 0000:XX:YY.Z: mlx5_cmd_comp_handler: Command completion arrived after timeout (entry idx = 0).
... ~14 ms later, while mlx5_core.eth probe is issuing cmds ...
mlx5_core 0000:XX:YY.Z: wait_func_handle_exec_timeout: cmd[0]: ALLOC_UAR(0x802) No done completion
mlx5_core 0000:XX:YY.Z: wait_func: ALLOC_UAR(0x802) timeout. Will cause a leak of a command resource
mlx5_core 0000:XX:YY.Z: alloc_uars_page: mlx5_cmd_alloc_uar() failed, -110
mlx5_core 0000:XX:YY.Z: mlx5e_create_mdev_resources: could only allocate 7/8 doorbells, err -110.
infiniband mlx5_X: Couldn't create ib_mad QP1
infiniband mlx5_X: Couldn't open port 1
```

Notes on reading that block:

* The `entry idx = 0` line is the ghost EQE arriving on cmd_eq for
  slot 0. It is emitted ~10-15 ms after `Rate limit:` (the last log
  line from `mlx5_init_once`). On a fresh-boot VF probe, this line
  never appears; it is exclusive to restored VFs.
* The `cmd[0]: ... No done completion` is a real cmd at slot 0 whose
  completion was lost. The opcode that is the victim depends on which
  cmd was at slot 0 when FW's ghost landed -- empirically `ALLOC_UAR`
  or `CREATE_MKEY` for the mlx5_core.eth aux probe path.
* The `7/8 doorbells` line is `mlx5e`'s soft tolerance: if it can get
  at least the minimum required UAR pages it continues. `CREATE_MKEY`
  has no such tolerance; if that's the victim, mlx5e probe fails hard
  with `_mlx5e_resume failed -110` and `mlx5_core.eth.X: probe with
  driver mlx5_core.eth failed with error -110`.
* `Couldn't create ib_mad QP1` / `Couldn't open port 1` are downstream
  consequences of the UAR pool being short (or not present at all).
  Note that on restored VFs these IB lines also appear for unrelated
  reasons (the L4 R1 mlx5_ib port-1 gating documented in the
  `tools/testing/criu_rdma/save_load/test_iova_tracked_save_load.sh`
  best-effort RDMA datapath check), so they are not by themselves a
  reliable indicator of this bug.

## 3. The trigger

The single architectural difference that distinguishes vfmig from
vfio-pci-mlx5 on a migrated VF is who drives the VF's cmd interface
state machine:

```
Path                 vfmig                       vfio-pci-mlx5
====================================================================
Owner of VF cmd      host mlx5_core driver       guest VM (passed-thru)
Calls cmd_use_events on VF after LOAD?
                     YES (inside                 NO (the host driver
                     mlx5_eq_table_create()      does not bind to the
                     during VF probe)            migrated VF)
Manifests ghost EQE  YES (consistent across      NO (never observed)
on cmd_eq post-LOAD? 100% of LOAD cycles in
                     this configuration)
```

Empirically: every variant of this failure happens **after** the host
driver's `mlx5_cmd_use_events()` call switches the freshly-restored
VF's cmd interface into event mode. Before that call, cmd is in
polling mode and the cmd ring is read directly; cmd_eq EQEs are not
consumed by the host, so any FW glitch on cmd_eq is invisible.

Whether FW emits the ghost EQE because of a producer-index mismatch
across `LOAD_VHCA_STATE`, a stale completion from a SAVE-time cmd
that didn't drain on the source side, or a transient dispatcher state
on the HCA is not known. The microarchitectural cause does not need
to be known to ship the workaround: the trigger is the host driver
reading from cmd_eq, and the workaround is to not do that.

## 4. What was tried and discarded

This was a long debug arc. Each of the following was implemented as a
behind-a-module-param knob, tested end-to-end, and rejected once it
was clear the underlying ghost EQE was still firing.

### 4.1 SAVE-side drain barriers

* `vfmig_save_drain_barrier_cmds=N` -- after the source VF has
  serialized its outstanding cmds and before issuing
  `SUSPEND_VHCA(initiator)`, dispatch N synthetic cmds (NOP or
  QUERY_ISSI) to push FW's cmd-EQ producer index past any in-flight
  completions that might otherwise be visible to the destination as
  ghosts.
* `vfmig_save_drain_barrier_op` -- choose `nop` or `query_issi` for
  the barrier.
* `vfmig_save_post_drain_settle_ms` -- `msleep(N)` after the barrier
  cmds, to let any FW-internal queue drain.

Result: **does not eliminate the ghost**. With `nop` barriers the
ghost still landed at `idx=0`; with `query_issi` barriers (which run
slightly slower than NOP and so shift the kernel's cmd issuance
timeline on the destination by the time the ghost arrives), the ghost
landed on `ALLOC_UAR` instead of `CREATE_MKEY`. This produces a
single-iteration "PASS" for the test framework -- because mlx5e
tolerates a partial UAR pool (`could only allocate 7/8 doorbells`)
where it does not tolerate `CREATE_MKEY` failure -- but iteration 2
on the same FW reliably shifts the victim back to `CREATE_MKEY` and
fails hard. This is one of the more dangerous false-positives in this
debug arc; it is documented here so future readers do not re-discover
it as a "fix".

### 4.2 LOAD-side drain barriers and settle delays

* `vfmig_load_drain_cmd_eq` -- after `mlx5_eq_table_create()` and
  before any post-LOAD cmd, walk cmd_eq once to drain anything FW
  may have left buffered.
* `vfmig_load_drain_barrier_nops=N` -- issue N event-mode NOPs on
  the restored VF as a "warm-up" / pump for the cmd-EQ pipeline.
* `vfmig_load_settle_ms=N` -- `msleep(N)` after the drain/NOP
  barriers, on the destination, before the rest of probe.
* `vfmig_load_warmup_nop` -- single warm-up NOP before
  mlx5_register_device.

Result: **does not eliminate the ghost**. The drain found zero EQEs
buffered (the ghost is *lazily* emitted by FW some milliseconds
after the cmd interface is brought up), the warm-up NOPs only shifted
which cmd lands at slot 0 when the ghost arrives, and `msleep(5)` /
`msleep(10)` settles only re-randomized timing without changing the
outcome.

### 4.3 Slot-0 reservation

* `vfmig_load_reserve_slot0` -- on a restored VF, permanently
  reserve cmd-ring slot 0 in `cmd_alloc_index()` so that no real cmd
  is ever issued at slot 0; the ghost EQE then lands on a NULL
  `ent_arr[0]` and is absorbed by the layer-1 dup-EQE filter.

Result: **partial**. With slot 0 reserved, the ghost EQE is harmless
and the `Command completion arrived after timeout (entry idx = 0)`
warning becomes idle. *However*, a real cmd at slot 1 still times
out 60 s later. This led to the **substitution model** of the bug:
FW emits **one** completion EQE that is mistakenly tagged as slot 0,
and that emission **consumes** the completion that should have been
delivered for the legitimate cmd at slot 1 (or wherever the real
cmd ended up). Slot-0 reservation only fixes the cosmetic ghost-on-
empty-slot symptom, not the underlying lost-completion.

### 4.4 Layer-1 / Layer-2 dup-EQE filters in `mlx5_cmd_comp_handler()`

* `vfmig_cmd_filter_dup_eqe_refcount` -- decline to refcount-decrement
  a cmd entry on a duplicate EQE.
* `vfmig_cmd_filter_dup_eqe_status_own` -- before processing an EQE,
  re-check `ent->lay->status_own` to detect EQEs whose underlying
  HW completion never actually flipped the OWN bit.

Result: **valuable as a backstop, irrelevant as a fix**. Layer-1 and
Layer-2 prevented refcount underflows and `WARN_ON` cascades (so the
host doesn't wedge), but neither produces a delivered completion to
the legitimate cmd, so the timeout still fires and downstream state
is the same.

### 4.5 Per-cmd polling routing on restored VFs

* `vfmig_polling_restored_vf` / `vfmig_polling_alloc_uar` /
  `dev->cmd.force_polling` -- keep the cmd interface in **event
  mode** (so the cmd-comp notifier is registered on cmd_eq) but route
  individual cmds (or all cmds on a restored VF) through polling at
  `mlx5_cmd_invoke()` time.

Result: **historically caused VF-destroy wedges**. Because the
notifier is registered on cmd_eq but no cmd entries are awaiting
event-driven completion, the teardown path gets confused and the host
hangs during VF disable. This is the dynamic the final fix
deliberately avoids: leaving the cmd interface cleanly in
**polling mode** end-to-end (mode, not per-cmd routing) means
`destroy_async_eqs()` symmetrically skips `mlx5_cmd_use_polling()`
and the cmd-comp notifier is never registered on cmd_eq in the first
place.

### 4.6 Architectural comparison: vfio-pci-mlx5

A subagent walked `drivers/vfio/pci/mlx5/main.c` and `cmd.c` end-to-
end and produced a side-by-side LOAD-time cmd-interface diagram for
vfmig vs vfio-pci-mlx5. The key finding from that comparison:
**vfio-pci-mlx5 never invokes `mlx5_cmd_use_events()` on the migrated
VF**, because the host driver does not bind the migrated VF to a host
mlx5_core instance -- the VF is exposed straight through to a guest
VM, and the guest's mlx5_core does its own event-mode setup against
the guest-side IRQ vectors, well after `LOAD_VHCA_STATE` has settled.
The host never reads from the VF's cmd_eq, so the FW ghost (whether
or not it is also emitted in the vfio-pci-mlx5 path) is invisible.

This gave us the architectural justification to ship a workaround
that simply mirrors that property in the vfmig path -- without re-
architecting LOAD or the aux-device probe stack.

## 5. The fix

Implementation summary:

1. New `bool dev->cmd.vfmig_skip_cmd_use_events` per-mdev flag.
2. New `mlx5_load()` arming step on a restored VF that, gated by
   `vfmig_load_skip_cmd_use_events` (default Y), sets the per-mdev
   flag *before* `mlx5_eq_table_create()` is called, so that
   `create_async_eqs()` can read it during cmd_eq setup.
3. `create_async_eqs()` skips `mlx5_cmd_use_events(dev)` when the
   flag is set. `cmd_eq` is still **created** (FW gets a cmd-comp EQ
   to write into if it chooses); the host just never registers a
   notifier on it and `dev->cmd.mode` stays `CMD_MODE_POLLING`.
4. `destroy_async_eqs()` symmetrically skips `mlx5_cmd_use_polling()`
   when the flag is set. (The notifier-unregister inside
   `mlx5_cmd_use_polling()` would walk a list the notifier was never
   added to.)
5. The module parameter `vfmig_load_skip_cmd_use_events` defaults to
   Y on a restored VF and is a runtime emergency disable, not an
   opt-in. Set to N **only** when a future FW revision is verified to
   no longer exhibit the post-LOAD ghost EQE behavior described
   here.

What is unchanged:

* `async_eq` and `pages_eq` are still created with notifiers; only
  cmd-completion delivery changes.
* PF probes and non-vfmig VF probes (`mlx5_vf_is_restored(dev) ==
  false`) are entirely unaffected.
* The cmd ring layout is identical. cmds still go through
  `cmd_alloc_index() -> mlx5_cmd_invoke()`; the only change is that
  the wait path is `lay->status_own` polling instead of
  `wait_event_interruptible()` on `ent->done`.
* Async events (port up/down, port management, FW health, etc.)
  still flow through async_eq as before; nothing about RoCE async
  notification is altered.

## 6. Validation

End-to-end test, both hosts, with the workaround enabled:

```
Iteration 1:  LOAD on host 1, LOAD on host 2  -- PASS (both)
Iteration 2:  LOAD on host 1, LOAD on host 2  -- PASS (both)
Iteration 3:  LOAD on host 1, LOAD on host 2  -- PASS (both)
```

Per-cycle dmesg signature on a restored VF (every cycle):

```
mlx5_core 0000:08:00.2: vfmig: post-LOAD cmd interface staying in polling mode (skip mlx5_cmd_use_events)
mlx5_core 0000:08:00.2: MLX5E: StrdRq(1) RqSz(8) StrdSz(2048) RxCqeCmprss(0 enhanced)
... mlx5e probe completes promptly, 7-12 s later ...
mlx5_core 0000:08:00.2 eth4: Link up
mlx5_core 0000:08:00.2 mlx5_2: Port: 1 Link ACTIVE
```

Signals that are **not** present in any cycle:

* `Command completion arrived after timeout` (would be the ghost
  EQE landing on cmd_eq).
* `cmd[N]: ... No done completion` for any cmd opcode.
* `could only allocate 7/8 doorbells, err -110`.
* `mlx5e_create_mdev_resources: create mkey failed, -110`.
* `_mlx5e_resume failed, -110`.
* `mlx5_core.eth ...: probe with driver mlx5_core.eth failed with
  error -110`.

Time-from-LOAD to `Link ACTIVE` dropped from ~76 s (60 s of
`ALLOC_UAR` timeout + recovery + retry path) to ~8-25 s (dominated
by IOVA replay and link negotiation, not the cmd interface). No
progressive degradation across iterations: iteration 3 looks
identical to iteration 1.

## 7. Pre-existing artifacts that look related but are not

These appear in restored-VF dmesg both before and after the fix.
Document here so future debug sessions don't conflate them:

* `infiniband mlx5_X: Couldn't create ib_mad QP1` /
  `Couldn't open port 1`.
  This is the L4 R1 `mlx5_ib` port-1 gating. Tracked separately and
  documented in
  `tools/testing/criu_rdma/save_load/test_iova_tracked_save_load.sh`
  ("L4 R1 mlx5_ib port-1 gating"). Expected on every restored VF
  until the dev_res FW objects are imported by R2; the CRIU
  `restore_pd ... vfmig_restore_mode=1` path runs immediately
  afterwards.
* `mlx5_core 0000:XX:YY.Z: poll_health: Fatal error 3 detected`
  during VF teardown, ~3 s after `Link DOWN`.
  This is `MLX5_SENSOR_NIC_DISABLED` (see
  `drivers/net/ethernet/mellanox/mlx5/core/health.c`), which fires
  when the VF's iseg `nic_interface` field flips to disabled --
  expected behaviour when the PF takes the VF offline as part of
  SRIOV disable. Cosmetically alarming due to upstream's choice of
  `Fatal error %u detected` wording; not a real fatal.

## 8. Future work (not blocking)

* If a future FW revision is verified to no longer emit the post-LOAD
  ghost EQE, flip the default of `vfmig_load_skip_cmd_use_events`
  back to N (or remove it entirely). The cost of staying with the
  current default is per-cmd polling overhead -- microseconds per
  cmd, vs ~1 us EQ-delivered -- which is currently negligible because
  post-probe cmd traffic on the VF is sparse.
* Investigate whether the same ghost-EQE behaviour appears on the
  source-side cmd_eq during SUSPEND_VHCA. We have no evidence it
  does, but if it does, the same "stay in polling mode for the
  lifetime of the migrated VF" treatment may be wanted there too.
* The IOVA `USER_PAGE unmap size mismatch` message observed on a
  third iteration of one host (`have 8192, asked 4096`) is a
  separate vfmig_iova bookkeeping signal during cleanup; not in the
  cmd path. Tracked independently.
