# DESIGN: §S3b -- PDN registration table wiped by LOAD_VHCA_STATE,
# and the gated mlx5_ib_dealloc_pd workaround

> Companion to [`uobject_restore.md`](uobject_restore.md). Documents
> the empirical investigation that revised the §S3b "DEVX-adoption
> blind spot" framing (which conflated two distinct FW limitations
> into one), the architectural model that resolved it, the kernel-
> side mitigation we landed in `mlx5_ib_dealloc_pd`, and the
> follow-up FW-team escalation that would obviate the mitigation.

## TL;DR

`LOAD_VHCA_STATE` preserves enough state for **traffic to resume**
on the destination VHCA but does not preserve the per-VHCA
**registration tables** that map `(pdn -> owner_uid)` and
`(uid -> uctx_attrs)`. After LOAD on the destination:

1. The **PDN allocator's high-water mark** is advanced past the
   source's max_pdn -- the source's PDN slots are reserved (no
   collision with fresh post-restore `ALLOC_PD`).
2. The **`(pdn -> owner_uid)` registration entries** for the
   source's PDNs are gone. `DEALLOC_PD` against any of those PDNs
   returns `status=bad_resource_state(0x9) syndrome=0xef0c8a`,
   regardless of the asserting uid -- the **same** shape FW returns
   for definitely-bogus PDNs.
3. The **`(uid -> uctx_attrs)` registration entries** are also
   gone (this is the original §S3b "DEVX-adoption blind spot",
   already documented in `uobject_restore.md` §5.3.4).

Items 2 and 3 are facets of the same FW design choice. They were
diagnosed independently and merged into one architectural picture
during the work that produced this doc.

The kernel-side mitigation:

```c
static int mlx5_ib_dealloc_pd(struct ib_pd *pd, struct ib_udata *udata)
{
        ...
        /* mlx5_cmd_do, not mlx5_cmd_exec: the former returns
         * -EREMOTEIO on FW status mismatch and DOES NOT log; that
         * way the gated path stays silent in dmesg. The non-gated
         * path replays through mlx5_cmd_check below to get the
         * canonical mlx5_core error log + errno translation. */
        err = mlx5_cmd_do(mdev->mdev, in, sizeof(in), out, sizeof(out));
        if (!err) return 0;

        status   = MLX5_GET(dealloc_pd_out, out, status);
        syndrome = MLX5_GET(dealloc_pd_out, out, syndrome);

        /* §S3b registration-wipe gate -- restored PDs only. */
        if (mpd->vfmig_restored &&
            status   == MLX5_IB_VFMIG_DEALLOC_PD_UNKNOWN_PDN_STATUS &&
            syndrome == MLX5_IB_VFMIG_DEALLOC_PD_UNKNOWN_PDN_SYNDROME) {
                mlx5_ib_dbg(...); /* tolerated; FW reclaims at VHCA close */
                return 0;
        }

        /* Real failure: replay through mlx5_cmd_check for canonical
         * mlx5_core error log + errno (typically -EINVAL). */
        err = mlx5_cmd_check(mdev->mdev, err, in, out);
        return err;
}
```

Using `mlx5_cmd_do` (the quiet variant of `mlx5_cmd_exec`) for the
initial issue means the gated path leaves dmesg untouched -- no
`mlx5_cmd_out_err: DEALLOC_PD ... failed` line for every restored
PD's destructor. The non-gated path explicitly replays through
`mlx5_cmd_check` so a real bookkeeping bug still produces the
expected loud diagnostic. End-to-end verification ran clean: the
`pd_restore_probe_mlx5_vfmig` harness's subtest 7 PASSes with
empty dmesg traces other than the pre-existing `vfmig_pd_dbg`
restore/dealloc bracket lines.

`mpd->vfmig_restored` is set true only by `mlx5_ib_restore_pd`.
Locally allocated PDs (vanilla `ibv_alloc_pd` post-restore on the
same dest VHCA) keep the unmodified failure semantics.

The leak budget is **bounded per-restore** (≤ source's PD count),
not per-cycle: fresh post-restore allocations are above max_pdn
and roundtrip cleanly. All FW state is reclaimed at VF unbind.

## 1. Background -- where `mlx5_ib_dealloc_pd` runs

`mlx5_ib_dealloc_pd` runs as the destructor for `mlx5_ib_pd` from
`uverbs_destroy_uobject` -> `pd_destroy_object` ->
`ib_dealloc_pd_user` -> `pd->device->ops.dealloc_pd`. Two triggers:

* **Explicit**: the dest target process calls `ibv_dealloc_pd`.
* **Implicit on process exit**: `release_file()` on
  `/dev/infiniband/uverbsN` -> `ib_uverbs_close` ->
  `__uverbs_cleanup_ufile` walks the uobject tree in
  reverse-dependency order (QPs -> CQs -> PDs), calling each
  driver destructor.

Both paths run unconditionally for every `mlx5_ib_pd` -- there is
no policy step that skips `DEALLOC_PD` for "special" PDs. This is
exactly the same shape the upstream guest mlx5_ib runs in an
SR-IOV VM-LM scenario.

## 2. The empirical investigation

Three test harnesses. Each lives under
`tools/testing/criu_rdma/uobject_restore/` and is a self-contained
shell harness that drives a single SAVE/LOAD cycle on a real
ConnectX-7 (FW 28.48.1000).

### 2.1 `qp_destroy_matrix/test_qp_destroy_matrix.sh`

**Question**: does FW silently no-op cross-uid `DESTROY_QP` /
`MODIFY_QP 2RST`, leaving an undestroyed QPC that pins its PDC and
explains the `DEALLOC_PD` failure CRIU was seeing?

**Result**: hypothesis refuted. FW honors any uid hint
(`uid=0`, `uid=src_devx`, `uid=hi_unalloc`) on `DESTROY_QP` against
a `uid=src_devx`-owned QPC. Post-op `QUERY_QP` confirms the QPC is
gone (`syndrome 0x23528a` ≡ "QPC not found"). The
`destroy_qp_common` warn-only logging in `drivers/infiniband/hw/
mlx5/qp.c` was never going to fire on this path -- FW returns
`err=0` on the destroy.

This refutation invalidated the design-doc claim that "DESTROY_QP /
2RST_QP modify against a uid != 0-owned QPC silently fail at FW"
(`uobject_restore.md`:970-972). That claim was based on a different
interpretation of `pd_adopt`'s `CREATE_MKEY`-only matrix, which did
not exercise the destroy direction.

### 2.2 `dealloc_pd_chain/test_dealloc_pd_chain.sh`

**Question**: with `DESTROY_QP` ruled out, where does the source
PDC go that makes `DEALLOC_PD` on the dest fail with `0xef0c8a`?

**Setup**: 3 source probes (full PD+CQ+QP shape, mirroring the
CRIU agent's libmlx5 repro). SAVE / tear down source / fresh dest /
LOAD / bind. Per source slot, on the dest:
1. `DESTROY_QP(qpn, uid=0)` -- destroys the QP cleanly.
2. `DEALLOC_PD(pdn, uid=lane)` -- the cell under test
   (lane ∈ {0, src_devx, hi_unalloc}).
3. `DEALLOC_PD(pdn, uid=lane)` -- second call, tight existence
   check via "does FW now return the bogus-PDN syndrome".

**Result**: all 9 cells (3 lanes × 3 PDs) report:
- `DESTROY_QP`: success.
- `DEALLOC_PD #1`: `op_status=-EINVAL syndrome=0xef0c8a`.
- `DEALLOC_PD #2`: same `0xef0c8a`.

Plus a sanity probe: `DEALLOC_PD(pdn=999, uid=0)`,
`DEALLOC_PD(pdn=5000, uid=0)`, `DEALLOC_PD(pdn=0xffff, uid=0)` all
return the **same** `0xef0c8a` against the dest VF. So `0xef0c8a`
is unambiguously "PDN unknown to allocator" -- not "PDN has
dependents", not "ownership uid mismatch", not "stale resource".

A follow-up confirmed `DEALLOC_PD` fails before any `DESTROY_QP`
runs (the failure is post-LOAD-time, not post-destroy-time -- see
TEST A in the inline `quick_test_a.sh` of the same dir):

```
=== TEST A: DEALLOC_PD with QP still alive (ref >= 1 from QPC) ===
op_status=0xffffffea op_syndrome=0x00ef0c8a op_accept=0
```

So the PDC is unreachable from the dest VHCA's allocator
**immediately after LOAD**, before any teardown the kernel runs.

### 2.3 `dealloc_pd_chain/test_pdn_highwater.sh`

**Question**: if the dest's allocator doesn't have the source's
PDN entry, does it also not have the source's high-water mark
advance? I.e., would a fresh `ibv_alloc_pd` post-LOAD return one
of the source's PDNs (collision risk) or skip past them?

**Result**: dest's first fresh `ALLOC_PD` returned `pdn=25`. Source
max_pdn was 22 (slots 23 and 24 were consumed by the dest's
kernel-internal `mlx5_ib_dev_res` PDs at ucontext-create). So the
allocator's **high-water counter** survived LOAD even though the
**registration entries** did not. Fresh post-restore allocations
are above the source's PDN range. No collision risk.

### 2.4 `dealloc_pd_matrix/test_dealloc_pd_matrix.sh` -- earlier

The PD-only source variant. Inverse to 2.2: same SAVE/LOAD cycle
but the source allocates only PDs (no CQ/QP/MR), so the PDC has
zero dependents at SAVE time.

**Result**: `DEALLOC_PD(pdn=src, uid=0/src/hi)` succeeded for all
three lanes on the dest. So PD-shape SAVE/LOAD apparently
**does** preserve the registration entry (or the dest's monotonic
allocator coincidentally landed on the same PDN -- distinguishing
the two would require an FW-team-side answer).

The PD-only success is what made me initially propose "skip
DEALLOC_PD always for restored PDs". That was wrong; PD-only PDs
*do* dealloc cleanly. The right discriminator is the syndrome.

## 3. The architectural model

The smallest model that fits all four tests:

* `SAVE_VHCA_STATE` captures **resource contexts** (QPC, CQC,
  MKey contexts, ICM, doorbells, WQ rings, ...) -- enough for
  traffic to resume on the destination.
* `SAVE_VHCA_STATE` captures **allocator counters** (next-free
  PDN, next-free uid, ...) but does **not** capture **allocator
  registration tables** (the per-PDN owner-uid map, the per-uid
  uctx-attrs map, ...).
* `LOAD_VHCA_STATE` reinstates the resource contexts and the
  counters. Registration tables start fresh on the destination.
* The QPC's `qpc.pd` field is preserved verbatim, so a destroy or
  modify of the QPC succeeds (these opcodes don't validate
  `qpc.pd` against the registration table). But any opcode that
  *queries* the registration table (`DEALLOC_PD`,
  `MODIFY_GENERAL_OBJECT` for DEVX-direct, ...) sees an empty
  entry and returns the "unknown" syndrome.

Why FW chose this design is opaque from outside. Plausible
rationale: registration tables are a host-kernel administrative
concern (who owns what), not a wire-state concern, and FW does not
attempt to migrate administrative state because the destination
host kernel will reconstruct it through normal alloc paths. That
rationale matches VM-LM (the *guest* host kernel reconstructs its
own registration view through guest-memory migration), but not
CRIU-on-host (where the destination host kernel uses
`MLX5_IB_METHOD_VFMIG_RESTORE_PD` as a kernel-state shortcut and
never calls `ALLOC_PD`).

## 4. Why VM-LM doesn't visibly hit this

If the model is right, SR-IOV VM-LM through vfio-mlx5 should
exhibit the same `DEALLOC_PD` failures post-migration. Three
reasons it does not surface visibly:

1. **The host kernel never runs `mlx5_ib`** against the migrated
   VF. The VF is bound to `vfio-pci`, not `mlx5_ib`. The host's
   destructor path is never exercised on migrated PDs.
2. **The guest's `mlx5_ib` does run `DEALLOC_PD`** when the guest
   user-app calls `ibv_dealloc_pd`. The current `mlx5_ib_dealloc_pd`
   returns the FW error to `ib_dealloc_pd_user`, which propagates
   it to userspace as `-EINVAL`. The kernel-side `mpd` is not
   freed (uverbs leaves it), and userspace typically does not
   check `ibv_dealloc_pd`'s return code, so the failure is silent
   from a top-level "is the application working" perspective. The
   bounded leak is masked by VM-shutdown VHCA close.
3. **Workload patterns**: typical VM-LM-aware workloads (cloud
   RDMA, GPU+RDMA) allocate PDs at startup and hold them until VM
   shutdown. They rarely cycle PDs post-migration, so the failure
   path is rarely entered.

The opcode sequence we use for SAVE/LOAD is **identical** to
vfio-mlx5's (`SUSPEND_VHCA(INITIATOR) -> SUSPEND_VHCA(RESPONDER)
-> SAVE_VHCA_STATE`, then on dest `SUSPEND -> LOAD -> RESUME ->
RESUME`). Cross-checked against `drivers/vfio/pci/mlx5/cmd.c`. So
this is a latent FW-kernel contract gap that VM-LM happens to
tolerate; CRIU-on-host is the first scenario where the host
kernel observes it directly through its own `mlx5_ib` instance.

## 5. The mitigation

### 5.1 Code shape (already landed)

Three edits, all in `drivers/infiniband/hw/mlx5/`:

* `mlx5_ib.h`: add `bool vfmig_restored` to `struct mlx5_ib_pd`,
  with a comment block linking back to this design doc.
* `main.c::mlx5_ib_restore_pd`: set `pd->vfmig_restored = true`
  on the success path, after the PDN/uid stamp.
* `main.c::mlx5_ib_dealloc_pd`: replace the one-line
  `mlx5_cmd_dealloc_pd` call with an inline `mlx5_cmd_exec` so we
  can read `status` and `syndrome` from the response blob, then
  gate the suppression on
  `(mpd->vfmig_restored && status == 0x9 && syndrome == 0xef0c8a)`.

### 5.2 What the gate does and does NOT cover

**Covers**:
* The exact `(status, syndrome)` tuple `(0x9, 0xef0c8a)` returned
  by FW 28.48.1000 for "PDN unknown to allocator".
* Only PDs created via `MLX5_IB_METHOD_VFMIG_RESTORE_PD`. Locally
  allocated PDs (post-restore `ibv_alloc_pd`, internal
  `mlx5_ib_dev_res` PDs, ...) keep their unmodified semantics.

**Does NOT cover**:
* `DESTROY_QP`/`DESTROY_CQ`/`DESTROY_MKEY` for restored
  QPs/CQs/MRs -- the three destroy-matrix harnesses (`qp_destroy
  _matrix/`, `cq_destroy_matrix/`, `mr_destroy_matrix/`) confirmed
  all of those honor cross-uid lanes. The "registration wipe"
  pattern is unique to PDs because the PD's per-VHCA storage IS
  the registration entry; CQs/QPs/MRs have dedicated context
  tables (CQC/QPC/MKC) that ARE preserved verbatim by LOAD, so
  destroy commands find them via the resource ID and succeed.
* DEVX-direct opcodes (`MODIFY_GENERAL_OBJECT`,
  `QUERY_GENERAL_OBJECT`, ...) on migrated objects -- those use
  the wiped uid registration table for ownership validation and
  there is no in-kernel destructor path for them. Workloads that
  use DEVX-direct manipulation post-migration are out of scope
  for v0.
* Workloads that explicitly cycle restored PDs with high
  frequency before VHCA close -- the leak budget here is bounded
  by the source's PD count, but for pathological workloads (many
  small short-lived PDs originally allocated on the source) the
  leak could become noticeable in long-lived processes.

### 5.3 Companion change: relaxed `RESTORE_UCONTEXT` devx_uid check

When the gated `mlx5_ib_dealloc_pd` workaround landed, the
strict-equality check on `meta.devx_uid` in
`MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT` (commit `c659ab66483d`)
became actively unhelpful. That check was originally added as
defense-in-depth on the (then-current) belief that mismatched
`devx_uid` between source and destination was the root cause of
the `DEALLOC_PD bad_resource_state syndrome 0xef0c8a-class`
failure. The empirical work in §3 / §6 established that:

* The failure is per-VHCA registration-table wipe, not a
  uid mismatch, so the strict check does not prevent it.
* The PD-destroy gate now suppresses the failure for restored
  PDs regardless of the asserting `uid`.
* `DESTROY_QP/CQ/MKEY` work cross-uid in their own right.
* Standard-verbs data path through default `libmlx5` ucontexts
  (auto-DEVX, fresh `devx_uid` per `ibv_open_device`) does NOT
  rely on `source.devx_uid == dest.devx_uid` -- doorbells and
  completions are HW-only paths that never consult the FW
  registration tables.

The strict check thus rejected the common case (default
`libmlx5` source with a fresh `devx_uid` on the dest) without
preventing any concrete failure. The follow-up relaxation in
`drivers/infiniband/hw/mlx5/vfmig_uctx.c::UVERBS_HANDLER(VFMIG
_RESTORE_UCONTEXT)`:

* Splits Precondition #3 into:
  * **#3a** (strict, retained): bfregi shape, `lib_caps`,
    `lib_uar_4k`, `lib_uar_dyn`, `cqe_version`. Mismatches here
    indicate structural drift that would corrupt the dest's
    UAR map and are still rejected `-EINVAL`.
  * **#3b** (log-and-continue, new): `meta.devx_uid != c->devx_uid`.
    Mismatches are logged via `mlx5_ib_dbg` but do not fail the
    method.
* Continues to refuse DEVX-direct opcodes against restored
  resources (those need uctx-registry preservation, which FW
  still does not provide -- see §7 escalation path).

The kernel-side regression test for the relaxed path is
end-to-end: any source ucontext that uses `libmlx5` auto-DEVX
followed by a destination ucontext opened without
`MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID` will land on the relaxed
path. A dedicated synthetic-mismatch probe (open
`VFMIG_RESTORE | DEVX` without `ADOPT_DEVX_UID` and call
`RESTORE_UCONTEXT` against a captured snapshot) is a future
artifact -- v0 relies on the empirical matrices + the gate +
end-to-end PD-restore harness for coverage.

### 5.4 Leak budget (formal)

Let `N_src` be the source's PD count at SAVE time (= number of
PDs that arrive on the destination via `RESTORE_PD`).

* **Best case** (no restored PD is dealloc'd until VHCA close):
  zero PDN slots leaked from the gate. VHCA close reclaims
  everything.
* **Worst case** (every restored PD is explicitly dealloc'd by
  the dest target before VHCA close): `N_src` PDN slots are
  leaked into the dest VHCA's PDN namespace.

The leak does **not** grow over time within a single VHCA
lifetime: fresh post-restore allocations land above the source's
high-water mark and roundtrip cleanly.

`N_src` is bounded by the source's own PD allocator state (24-bit
PDN -- `2^24` slots theoretical max, typically a handful in real
RDMA applications). The dest VHCA's PDN namespace is also
24-bit; the leak is a small fraction of available namespace per
restore.

## 6. Generalising to QP/CQ/MR

Empirically tested on FW 28.48.1000 with three destroy-direction
matrix harnesses (one per resource type). **PDs are unique** in
exhibiting the registration-wipe symptom; **QPs, CQs, and MRs all
roundtrip cleanly cross-uid** post-LOAD_VHCA_STATE.

| Resource | Harness | Result | Gate needed? |
|---|---|---|---|
| QP (RESET) | `qp_destroy_matrix/test_qp_destroy_matrix.sh` | `DESTROY_QP` and `2RST_QP` honor cross-uid for all three lanes (uid=0, uid=src_devx, uid=hi_unalloc). Post-op `QUERY_QP` returns syndrome `0x23528a` ("QPC not found"). | No |
| CQ | `cq_destroy_matrix/test_cq_destroy_matrix.sh` | After dropping the dependent QP first (`DESTROY_QP(uid=0)` works cross-uid for that), `DESTROY_CQ` succeeds cross-uid for all three lanes. Post-op `QUERY_CQ` returns syndrome `0x1fb6ec` ("CQC not found"). | No |
| MR (mkey) | `mr_destroy_matrix/test_mr_destroy_matrix.sh` | `DESTROY_MKEY` succeeds cross-uid for all three lanes; post-op `QUERY_MKEY` returns "MKC not found". MRs do not require pre-tearing dependents. | No |
| PD | `dealloc_pd_chain/test_dealloc_pd_chain.sh` | `DEALLOC_PD` on vfmig-restored PDs **fails** with status `0x9` syndrome `0xef0c8a` (PDN unknown to allocator) for all three lanes; same shape as definitely-bogus pdns. | **YES** -- gate landed in `mlx5_ib_dealloc_pd` |

This matches the architectural model in §3: FW preserves the
allocator counters and the resource contexts, but the per-resource
registration tables that map IDs to owner-uid are wiped only for
PDs. CQs and mkeys appear to have a different storage path inside
FW (likely in their dedicated context tables, which ARE preserved
verbatim across LOAD), so cross-uid destroy commands find them via
the resource ID and succeed. PDs are the outlier because the
"PD context" is essentially just the registration entry -- there
is no dedicated PDC ICM region for FW to preserve.

**Pre-tearing for CQs**: a first version of `cq_destroy_matrix`
ran `DESTROY_CQ` directly on a CQC that still had a dependent
QPC referencing it via `qpc.send_cqn`/`qpc.recv_cqn`, and FW
rejected with syndrome `0x1870ad` ("CQ has dependents"). This is
**not** the registration-wipe symptom; it is FW's normal
dependency enforcement and is uid-independent. The harness was
updated to drop the dependent QP first (cross-uid `DESTROY_QP`
under `uid=0` works fine per the QP matrix) before running the
CQ destroy under the lane's `uid_hint`. This mirrors the actual
mlx5_ib teardown order (`destroy_qp -> destroy_cq -> dealloc_pd`)
and isolates the cross-uid CQN-registration question from
dependent-tracking.

**MRs and dependents**: MRs do not require any pre-teardown.
The MKEY context references its parent PD via `mkc.pd` but FW
does not block `DESTROY_MKEY` on the PDN being unknown. Posted
WRs that reference the MKEY at WR-time are a transient
relationship and do not affect destroy.

**If a future test reveals a similar registration-wiped symptom**
on a different opcode (e.g., a non-RESET QP modify path, an
mlx5_ib_devx_modify path), the same pattern applies:

1. Add `bool vfmig_restored` to the relevant struct.
2. Set it in the corresponding `RESTORE_*` method.
3. Capture FW status and syndrome in the destructor (use
   `mlx5_cmd_do` instead of `mlx5_cmd_exec` to suppress the
   default loud `mlx5_cmd_out_err` log on the gated path; replay
   through `mlx5_cmd_check` on the warn path).
4. Gate suppression on the precise syndrome class observed
   empirically. Do **not** suppress arbitrary failures -- that
   would mask real bookkeeping bugs.

## 7. The proper fix (FW-side)

The mitigation is correct as a v0 ship-able workaround but does
not eliminate the underlying gap. Proper fix needs FW-side work,
either:

* **Option A**: extend `LOAD_VHCA_STATE` to additionally restore
  the `(pdn -> owner_uid)` and `(uid -> uctx_attrs)` registration
  tables, derived from the QPC/CQC/MKey/UAR contexts that
  reference them.
* **Option B**: expose explicit FW opcodes that the destination
  kernel can call from its `RESTORE_*` methods to register a
  `(pdn, owner_uid)` or `(uid, uctx_attrs)` pair after LOAD.
  Tentative names: `ADOPT_PD`, `RESTORE_UCTX_REGISTRATION`.

Both are small additions to the FW IFC. Option B is cleaner from
a layering POV (kernel explicitly opts in to ownership) but
requires more host-kernel state. Option A is the natural extension
of "VHCA migration captures everything needed".

This is the escalation question for the FW/HCA team. Once
addressed, the gate in `mlx5_ib_dealloc_pd` becomes dead code on
new FW versions; we can leave it in place for older-FW
compatibility until support drops.

## 8. Test artefacts

| Test | Path | Result |
|---|---|---|
| QP destroy matrix | `tools/testing/criu_rdma/uobject_restore/qp_destroy_matrix/test_qp_destroy_matrix.sh` | Cross-uid `DESTROY_QP`/`2RST_QP` work; "silent no-op" hypothesis refuted. |
| CQ destroy matrix | `tools/testing/criu_rdma/uobject_restore/cq_destroy_matrix/test_cq_destroy_matrix.sh` | Cross-uid `DESTROY_CQ` works after dropping dependent QP; no gate needed. |
| MR destroy matrix | `tools/testing/criu_rdma/uobject_restore/mr_destroy_matrix/test_mr_destroy_matrix.sh` | Cross-uid `DESTROY_MKEY` works directly; no gate needed. |
| DEALLOC_PD (PD-only) | `tools/testing/criu_rdma/uobject_restore/dealloc_pd_matrix/test_dealloc_pd_matrix.sh` | `DEALLOC_PD` succeeds on dest for source-PD-only shape. |
| DEALLOC_PD (PD+CQ+QP) | `tools/testing/criu_rdma/uobject_restore/dealloc_pd_chain/test_dealloc_pd_chain.sh` | `DEALLOC_PD` fails `0xef0c8a` for source-PD+CQ+QP shape. |
| PDN high-water | `tools/testing/criu_rdma/uobject_restore/dealloc_pd_chain/test_pdn_highwater.sh` | Dest's first fresh `ALLOC_PD` returns `pdn > src_max_pdn`; high-water survives LOAD. |

All four reproduce on FW 28.48.1000, ConnectX-7. Reproduction
instructions: each script takes `PF=<bdf>` env var and creates
fresh source/dest VFs through `sriov_numvfs`.

## 9. Cross-references

* `uobject_restore.md` §5.3.4 -- original "DEVX-adoption blind
  spot" framing, partially superseded by this doc's broader
  registration-wipe model.
* `uobject_restore.md` §5.1 -- PD restore design that this gate
  is a follow-up to.
* `drivers/infiniband/hw/mlx5/main.c::mlx5_ib_dealloc_pd` -- the
  gated implementation.
* `drivers/infiniband/hw/mlx5/main.c::mlx5_ib_restore_pd` --
  sets `vfmig_restored = true`.
* `drivers/vfio/pci/mlx5/cmd.c` -- upstream SR-IOV VM-LM path,
  identical opcode sequence to ours.
