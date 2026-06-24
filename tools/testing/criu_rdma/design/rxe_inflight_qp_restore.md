# DESIGN: rxe in-flight QP restore -- serialize the live SQ/RQ ring
#         payload + all cursors + RC responder resources (approach B1)
#
# Audience: kernel agent (drivers/infiniband/sw/rxe)
# Builds on: design/uobject_restore.md §5.3 (rxe S6a QP save/restore)
#            design/snapshot_ordering_pause_capture.md (consistency point)
# Goal: restore a non-quiesced (in-flight) rxe RC/UC/UD QP without
#       forcing the application to drain at checkpoint.
#
# STATUS: kernel side IMPLEMENTED (this tree). QUERY_QP capture +
#         RESTORE_QP UHW-tail apply + rxe_qp_seed_ring/rxe_qp_restore_inflight
#         land in rxe_migrate.c / rxe_verbs.c / rxe_qp.c; validated live by
#         uobject_restore/qp_restore/qp_restore_probe_rxe (non-empty RQ
#         pre-post + byte-identical SQ/RQ/RES round-trip). The full
#         non-drained peer-traffic harness modes (§6) remain CRIU-side TODO.
#         Sections below note "as built" where the implementation refined
#         the original plan.

## 1. The problem (recap, grounded in this tree)

A rxe QP's live work lives in shared-memory rings (kernel pages mmap'd
to userspace via `vm_pgoff`). The datapath state is split three ways,
and none of it currently survives restore:

SQ (`QUEUE_TYPE_FROM_CLIENT`) has **three** cursors; we capture one:

```
  consumer_index   <=   qp->req.wqe_index   <=   producer_index   (mod wrap)
  (completer: next      (requester: next         (userspace: count
   to retire on ACK)     WQE to transmit)          of WQEs posted)
```

- `producer > req.wqe_index`  => posted-but-unsent WQEs
- `req.wqe_index > consumer`   => sent-but-unacked WQEs (RC retransmit)

Today `QUERY_QP` emits only `req_wqe_index` (`rxe_migrate.c`) and
`rxe_qp_restore_wire_state()` stamps `qp->req.wqe_index = req_wqe_index`
(`rxe_qp.c:554`) **without seeding the fresh ring's producer/consumer**
-- which start at 0. So even a drained-but-nonzero QP comes back
inconsistent. The RQ cursor is not captured at all (and `producer >
consumer` is the *normal* pre-posted-recv state, not work to drain).

The killer: on restore the rings are re-created empty (CRIU re-maps the
device VMA but not its contents), so the `rxe_send_wqe` / `rxe_recv_wqe`
slots between consumer and producer -- work request, SGEs, DMA cursor,
per-WQE PSNs, state -- are gone. Seeding cursors alone points at empty
slots.

RC also has responder-side in-flight state: `qp->resp.resources`, a
circular array of `struct resp_res` (duplicate-read / atomic replay
slots), plus the scalar `qp->resp` bookkeeping.

## 2. Consistency point (no drain)

CRIU freezes the whole process before `QUERY_QP`, and (per
`design/snapshot_ordering_pause_capture.md`) the datapath is *paused*
(`rxe_qp_pause`) at the early hook before memory is copied. So userspace
cannot post and the rxe tasks cannot advance during capture -- the
snapshot is consistent **without** draining. We keep FREEZE == pause.
Optionally hold `qp->sq.q->producer`/`qp->rq.q` access under the
existing queue locks while serializing, belt-and-suspenders.

## 3. What to capture

### 3.1 SQ / RQ ring images (variable length)

Each ring is `q->buf` with `index_mask + 1` slots of `q->elem_size`
bytes. The simplest faithful capture is a **verbatim blit of the whole
slot data region** (`num_slots * elem_size`), which sidesteps per-slot
field surgery and handles wrap implicitly. We capture the raw bytes plus
the cursors and re-blit on restore.

Sizing (answer to "how big can these get?"): bounded by
`max_{send,recv}_wr * elem_size`, rounded to a power of two. `elem_size`
is `rxe_send_wqe` header + `max_send_sge * sizeof(rxe_sge)` (or inline
data). Realistic depths => tens-to-hundreds of KB; worst case (~16K
WQEs * ~512B) a few MB. Single-call capture is fine: CRIU already maps
the ring VMA so it knows the exact byte length to allocate, and the
kernel reports actual bytes written. No two-step size negotiation.

### 3.2 Cursors (fixed)

- SQ: `producer`, `qp->req.wqe_index`, `consumer`.
- RQ: `producer`, `consumer`.

### 3.3 RC responder resources (in scope for v0, per maintainer call)

`qp->resp.resources` is `max_dest_rd_atomic` entries of `struct
resp_res`. Confirmed serializable as plain data: the `read` union
carries `rkey`/`va`/`length`/`resid` (the MR re-resolves from `rkey` on
replay -- no live kernel pointers in the entry). Capture:

- the resources array verbatim (`max_dest_rd_atomic * sizeof(resp_res)`),
- `res_head`, `res_tail`,
- scalar `qp->resp` fields not already covered: `ack_psn`, `opcode`,
  `status`, `aeth_syndrome` (msn/psn already in the blob).

NOT captured (transient / re-derivable): `resp.wqe`, `resp.mr`,
`resp.res` (the *current* pointer -- recomputed from `res_head`/array),
any in-flight reply skb. On restore these re-arm lazily from the
restored array + PSNs.

## 4. Transport (UAPI delta)

The fixed `struct rxe_restore_qp_req` (184 B, ~8 B reserved) cannot hold
the variable-length payload. Plan:

1. **Grow the fixed blob** (dev-only ABI; allowed) to carry all cursors
   and the responder scalars + array lengths (consumes the old
   `reserved1`):
   - add `sq_producer`, `sq_consumer`, `rq_producer`, `rq_consumer`
     (`req_wqe_index` already present),
   - add `resp_ack_psn`, `resp_opcode`, `resp_status`,
     `resp_aeth_syndrome`, `res_head`, `res_tail` (`max_dest_rd_atomic`
     was already in the blob),
   - add `sq_image_bytes`, `rq_image_bytes`, `res_image_bytes` (the
     authoritative per-image byte lengths; also used to slice the
     RESTORE_QP tail -- see below).
2. **Carry the variable images** -- *as built*, the transport is
   asymmetric because the dump and restore verbs differ in kind:
   - `QUERY_QP` is an rxe-private method, so it gains **three new
     optional `PTR_OUT` attrs**: `RXE_IB_ATTR_QUERY_QP_RESP_SQ_IMAGE`,
     `..._RQ_IMAGE`, `..._RES` (raw SQ slot region, RQ slot region,
     responder-resources array). Omitted/zero-length for a drained QP,
     for SRQ-backed RQs, and for UD/UC where the responder array is
     absent.
   - `RESTORE_QP` is the **core** `UVERBS_METHOD_RESTORE_QP` (dispatched
     via `ib_device_ops.restore_qp`); a driver cannot add attrs to it.
     The only driver channel is its single `UVERBS_ATTR_UHW()` blob, so
     the images ride **concatenated in the `UHW_IN` tail** after the
     fixed `struct rxe_restore_qp_req`, in slice order SQ, RQ, RES,
     located by the header's `{sq,rq,res}_image_bytes`. A drained
     restore sends the header alone (no tail) and keeps the cursor-only
     fast path.
   - **Size limit (as built)**: `struct ib_uverbs_attr::len` is `u16`,
     so any single image attr -- and the whole `UHW_IN` blob -- caps at
     65535 bytes. That holds normal QPs comfortably; rings larger than
     ~64 KB (deep SQ/RQ or wide SGE) need chunking across multiple
     attrs/calls, a v1 concern (noted in §7).
3. CRIU mirror struct + `_Static_assert`s move in lockstep (the plugin
   treats the blob + images as opaque bytes; the dumper reads the three
   QUERY_QP image attrs and re-concatenates them into the RESTORE_QP
   UHW tail).

## 5. Restore application (rxe_qp.c)

Order, after the fresh QP + rings are created (with forced `vm_pgoff`):

```
1. blit SQ_IMAGE back into qp->sq.queue->buf data region
   blit RQ_IMAGE back into qp->rq.queue->buf data region
2. seed cursors:
     producer/consumer on both rings (shared-page header)
     qp->req.wqe_index = sq_consumer        # REWIND (see below)
3. restore responder resources:
     alloc_rd_atomic_resources(qp, max_dest_rd_atomic)  # already exists
     blit RESP_RES into qp->resp.resources
     qp->resp.res_head/res_tail = captured
     qp->resp.{ack_psn,opcode,status,aeth_syndrome} = captured
4. PSNs already restored (req/comp/resp/msn) by current code.
```

### 5.1 The rewind, and why it's correct

Set `qp->req.wqe_index = sq_consumer` (not the captured `req.wqe_index`)
so the requester replays **everything** in `[consumer, producer)` from
scratch. rxe already does exactly this rewind on RC retry
(`rxe_req.c:51`, `:674`): `req.wqe_index = queue_get_consumer(...)`. The
already-sent-unacked WQEs are re-sent with their captured per-WQE PSNs
(`first_psn`/`last_psn`, carried in the blitted `rxe_send_wqe`), and the
peer drops duplicate PSNs -- so re-sending sent-but-unacked work is
benign and sent-but-not-yet-acked work gets retransmitted, which is the
intended recovery. Posted-but-unsent work (`> old req.wqe_index`) is
sent for the first time. Net: the single rewind covers all three SQ
regions correctly, given the restored PSNs.

**The rewind alone is NOT sufficient**, however: rewinding
`req.wqe_index` does not touch the *per-WQE* DMA cursor. A blitted WQE
that was mid/post-transmission at dump carries its consumed cursor
(`wqe->dma.resid == 0`, `state == wqe_state_pending`), so `rxe_requester`
would send a 0-byte payload (`payload = wqe->dma.resid`). The replay must
therefore run through `req_retry()` (which resets
`dma.resid`/`cur_sge`/`sge_offset` and `state` across
`[sq_consumer, sq_producer)` and resumes the first unacked WQE from
`qp->comp.psn`). That is driven from `rxe_qp_resume()` by arming
`qp->req.need_retry = 1` (and clearing `wait_for_rnr_timer`) before the
`send_task` kick -- see §5.4. `req_retry()` `break`s at the first
`wqe_state_posted` WQE, so posted-but-unsent work is left untouched and
sent for the first time, and `continue`s past `wqe_state_done`: it serves
all three sub-cases.

(Verification item, now observable via the CRIU pd_cq_qp_sq pass: confirm
`qp->comp.psn` / `qp->req.psn` line up with the rewound replay so the
completer retires the right WQEs as ACKs arrive.)

### 5.2 `rxe_qp_seed_ring()` helper (as built, `rxe_qp.c`)

```c
static void rxe_qp_seed_ring(struct rxe_queue *q, u32 producer, u32 consumer)
{
    producer &= q->index_mask;
    consumer &= q->index_mask;
    q->buf->producer_index = producer;
    q->buf->consumer_index = consumer;
    q->index = consumer;            /* rxe-owned consumer copy (FROM_CLIENT) */
}
```

Rings are quiescent at restore (the QP is freshly created and not yet
finalized), so plain stores suffice -- no `smp_store_release`/`WRITE_ONCE`.
The caller (`rxe_qp_restore_inflight()`) only invokes this after
validating the image length equals the just-created ring's
`queue_data_size()`, so `q` is always non-NULL here.

### 5.3 Where the apply lives (as built)

- `rxe_restore_qp()` (`rxe_verbs.c`): if `udata->inlen > sizeof(req)`,
  calls `rxe_restore_qp_inflight()`, which copies the whole `UHW_IN`,
  slices the SQ/RQ/RES tail by the header byte-counts, and hands the
  opaque images to:
- `rxe_qp_restore_inflight()` (`rxe_qp.c`): blits each image (rejecting
  any geometry mismatch with `-EINVAL`), seeds cursors via
  `rxe_qp_seed_ring()`, rewinds `qp->req.wqe_index = sq_consumer`,
  restores `qp->resp.resources` + `res_head`/`res_tail`, and stamps the
  responder scalars. Runs after `rxe_qp_restore_wire_state()` (which
  already stamped PSNs/AV/attrs) and before `rxe_finalize()`.

### 5.4 Born-frozen + thaw-and-replay (as built)

Seeding the ring is necessary but not sufficient: a restored, non-drained
SQ has its WQEs in `[sq_consumer, sq_producer)` with cursors seeded, but
nothing reschedules the requester. `send_task` only advances when
scheduled, and on a fresh destination QP there is no triggering event --
no fresh `post_send` (the WQEs are already in the blitted ring) and no
inbound packet yet -- so the in-flight window idles forever.

**Do NOT kick the requester inside `rxe_restore_qp()`.** Tried and
reverted: firing `rxe_sched_task(&qp->send_task)` after `rxe_finalize()`
transmits while the rest of the restore tree is still being rebuilt --
peer QPs (cross-QP / cross-process) may not exist yet, and SGE-referenced
MR pages are not populated until CRIU's post-VMA Phase B. Result:
transmit-to-nonexistent-peer (retry burst -> `RETRY_EXC`) and/or stale
bytes on the wire. There is no tree-wide consistency barrier at that
point. See the NOTE at the `rxe_qp_pause()` site in `rxe_restore_qp()`.

The fix is **born-frozen + thaw-and-replay**, reusing the existing
`FREEZE_CONTEXT` path -- no new UAPI:

1. **Install paused.** `rxe_restore_qp()` calls `rxe_qp_pause(qp)` *before*
   `rxe_finalize(qp)` (i.e. before the QP becomes reachable to
   `rxe_pool_get_index` / `rxe_rcv`). Tasks are parked from birth. A fresh
   task is `TASK_STATE_IDLE`; `rxe_disable_task` on a quiescent task takes
   the fast path to `TASK_STATE_DRAINED` (no spin, no WARN -- the pool ref
   from `rxe_add_to_pool_at_index` keeps `rxe_read(qp) >= 1`). This also
   blocks the responder from acting on an early inbound packet (from a
   peer that thawed first) before our MR buffers are in place.
2. **Resume = enable + replay.** `rxe_qp_resume()` re-enables both tasks,
   then: if `qp_state == RTS && sq_producer != sq_consumer`, it arms a
   retry (`qp->req.need_retry = 1`, `qp->req.wait_for_rnr_timer = 0`,
   under `state_lock`) and `rxe_sched_task(&qp->send_task)`. The
   `need_retry` arming is essential, not cosmetic: without it
   `rxe_requester` sends `payload = wqe->dma.resid == 0` for the blitted
   WQEs (consumed cursor); arming it makes the gate
   `need_retry && !wait_for_rnr_timer` run `req_retry()`, which resets the
   per-WQE DMA state across `[sq_consumer, sq_producer)` and replays the
   in-flight window from `qp->comp.psn` (see §5.1). This mirrors
   `rnr_nak_timer()` verbatim. If the responder inbound queue
   (`qp->req_pkts`) is non-empty, `rxe_sched_task(&qp->recv_task)` (drains
   packets queued while frozen, which `rxe_enable_task` alone would not
   re-run). Producer/consumer were already seeded by
   `rxe_qp_restore_inflight()`, so resume decides per-QP with no extra
   caller state. All of this is a no-op for a source QP resumed after a
   dump-freeze with an empty ring/queue; when the source ring is
   non-empty, forcing the retry is the same recovery the retransmit timer
   would have run (a freeze is indistinguishable from a network stall),
   so one path serves both the dump-resume and the restore-thaw callers.
3. **Thaw == replay trigger.** `FREEZE_CONTEXT(freeze=0)` already walks the
   ucontext QP pool calling `rxe_qp_resume()` per QP, so it becomes the
   one-shot "thaw + replay every QP in this ucontext."

**Caller contract (the critical half, enforced by the orchestrator, not
the kernel).** The thaw must be the *last* tree-wide restore step: do not
issue `FREEZE_CONTEXT(freeze=0)` until, tree-wide, all uobjects are
restored (Phase A + post-VMA Phase B), all peer QPs in the snapshot
exist, and MR-backed memory is in place. A single-process self-loopback
tree can thaw at the Phase B tail; a multi-process tree needs a global
barrier near end-of-restore. A per-ufile fini is too weak -- the
first-thawed requester would transmit to a peer qpn a sibling task has
not installed yet. The kernel cannot enforce this; it only guarantees
nothing moves until thawed.

## 6. Test harness

### 6.1 In-tree plumbing probe (as built, DONE)

`uobject_restore/qp_restore/qp_restore_probe_rxe` now exercises the B1
kernel path end-to-end in a single process: it **pre-posts recv WQEs**
on the source RC QP (so `rq_producer != rq_consumer` and the RQ slot
region is genuinely populated), captures the SQ/RQ/RES images via the
new `QUERY_QP` attrs, destroys the source, replays them in the
`RESTORE_QP` UHW tail, then re-queries the restored QP and asserts the
wire state **and** all three images are byte-identical. This validates
capture -> tail-concat -> blit -> seed -> re-capture for a non-empty
ring. (`qp_query_probe_rxe` covers QUERY_QP/FREEZE field fidelity.)

What it does *not* cover: semantic replay against a live peer (actual
unsent-SQ transmit, pre-posted-RQ receive, in-flight RC-READ reply) --
that needs two endpoints and belongs in the CRIU-side harness below.

### 6.2 CRIU side (scaffolded; peer-traffic modes still TODO)

- `qp_pair` pass (`UVERBS_CR_RUN_QP_PAIR=1`) currently passes only
  because the holder drains pre-dump.
- **New must-add modes**:
  - non-drained SQ: post send WQE(s), dump *without* reaping completion
    (producer/req.wqe_index/consumer differ), restore, verify WQE
    completes + data lands.
  - pre-posted RQ: post recv WQE(s), dump with `rq producer != consumer`,
    restore, peer SENDs, verify the pre-posted buffer receives.
  - RC responder: in-flight RDMA-READ across the dump boundary, verify
    the read reply completes post-restore (exercises resp.resources).
- Holder needs a "leave work outstanding at snapshot" mode (current
  holder always quiesces before signaling READY).

## 7. Phasing

- **v0 (this effort)**: SQ + RQ image + all cursors + resp.resources,
  rewind-replay, the three new harness modes. RC/UC/UD.
- Deferred: requester `comp` micro-state beyond PSNs if the rewind proves
  insufficient for some corner (TBD by the verification item in §5.1);
  SRQ-backed RQ (the RQ image path assumes a per-QP RQ -- SRQ rings are a
  separate uobject and out of scope here).
