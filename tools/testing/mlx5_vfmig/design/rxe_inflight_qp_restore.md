# DESIGN: rxe in-flight QP restore -- serialize the live SQ/RQ ring
#         payload + all cursors + RC responder resources (approach B1)
#
# Audience: kernel agent (drivers/infiniband/sw/rxe)
# Builds on: design/uobject_restore.md §5.3 (rxe S6a QP save/restore)
#            design/snapshot_ordering_pause_capture.md (consistency point)
# Goal: restore a non-quiesced (in-flight) rxe RC/UC/UD QP without
#       forcing the application to drain at checkpoint.

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

Today `QUERY_QP` emits only `req_wqe_index` (`rxe_vfmig.c:159`) and
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
   and the responder scalars + array length:
   - add `sq_producer`, `sq_consumer`, `rq_producer`, `rq_consumer`
     (`req_wqe_index` already present),
   - add `resp_ack_psn`, `resp_opcode`, `resp_status`,
     `resp_aeth_syndrome`, `res_head`, `res_tail`, `max_dest_rd_atomic`,
   - add `sq_image_bytes`, `rq_image_bytes`, `res_image_bytes`
     (informational; CRIU also derives sizes from the VMA / caps).
2. **Three new UVERBS attrs**, present on both `QUERY_QP` (OUT) and
   `RESTORE_QP` (IN tail):
   - `..._SQ_IMAGE`  -- raw SQ slot region
   - `..._RQ_IMAGE`  -- raw RQ slot region
   - `..._RESP_RES`  -- raw responder-resources array
   Marked optional/zero-length for drained/idle QPs and for UD/UC where
   the responder array is absent.
3. CRIU mirror struct + `_Static_assert`s move in lockstep (the plugin
   treats the blob + attrs as opaque bytes).

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

(Open verification item: confirm `qp->comp.psn` / `qp->req.psn` line up
with the rewound replay so the completer retires the right WQEs as ACKs
arrive. Add an assert/harness check.)

### 5.2 `rxe_qp_seed_ring()` helper

Introduce (the handoff referenced it; it is NOT in this tree yet):

```c
static void rxe_qp_seed_ring(struct rxe_queue *q, u32 prod, u32 cons)
{
    if (!q) return;
    q->index = cons & q->index_mask;             /* rxe-owned consumer */
    WRITE_ONCE(q->buf->producer_index, prod & q->index_mask);
    WRITE_ONCE(q->buf->consumer_index, cons & q->index_mask);
}
```

Rings are quiescent at restore, so plain `WRITE_ONCE` is sufficient
(no `smp_store_release` needed; revisit only if restore ever races a
live task).

## 6. Test harness (CRIU side, already scaffolded)

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
