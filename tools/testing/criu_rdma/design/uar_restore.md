# DESIGN: mlx5_ib UAR restore for CRIU

Status: **proposed**, pre-implementation. This doc captures the design we
agreed on before any code lands. Subsequent docs will cover R3's per-uobject
restore (PD / MR / CQ / QP / SRQ) and the v2 user-MR allocator coverage.

## 1. Goal and scope

Restore a `struct mlx5_ib_ucontext`'s **UAR table** verbatim across CRIU
checkpoint/restore on a tracked + migrated VF. After restore the original
process must observe each UAR `mmap()`'d at the same virtual address, backed
by the same firmware UAR id, so existing user heap pointers into doorbell
pages remain valid.

**In scope**

* The per-`mlx5_ib_ucontext.bfregi.sys_pages[]` array of FW UAR ids
  (both static and dynamic slots).
* The per-`mlx5_ib_ucontext.bfregi.count[]` array (per-bfreg-slot live
  refcount; needed to reproduce the dynamic-UAR claim state).
* The mapping from `vm_pgoff` (encoded `mmap_cmd | sys_page_idx`) to a
  physical UAR page on a restored ucontext, for all five static/dynamic
  UAR mmap_cmd values: `REGULAR_PAGE`, `WC_PAGE`, `NC_PAGE`, `ALLOC_WC`.
* Restore replay of `MLX5_IB_MMAP_CLOCK_INFO` mappings -- handled
  entirely by VMA replay, no per-ucontext state to restore.
* Two new mlx5_ib uverbs vendor verbs (QUERY + RESTORE).
* One new `mlx5_ib_alloc_uctx_v2_flags` bit to gate `allocate_uars()` skip.
* One new branch in `uar_mmap()`'s dynamic path to suppress lazy
  `mlx5_cmd_uar_alloc` when the slot was seeded by RESTORE.
* Test harness for incremental validation.

**Out of scope (subsequent docs)**

* PD / MR / CQ / SRQ / QP restore. Those are R3's per-uobject layers.
* `MLX5_IB_MMAP_DEVICE_MEM` (on-chip HCA memory regions, "DM") restore.
  DM is a distinct uobject class (`MLX5_IB_OBJECT_DM`) that has its own
  user-visible handle and its own mmap path
  (`device_mem_mmap` via `rdma_user_mmap_entry`). It is **not** the same
  problem as UAR restore (DM isn't in the bfregi table) and **not** the
  same problem as the v2 allocator (DM is on-chip SRAM, not host-DMA, so
  it bypasses `ib_umem_get` / `dma_map_sgtable` entirely). DM restore
  shape: capture DM handle + start/length/alignment on DUMP, RESTORE_DM
  verb on RESTORE, then VMA replay against the restored DM's mmap entry.
  Lands as a sibling R3 uobject-restore patch alongside PD/MR/CQ/QP.
* The v2 user-MR allocator hook (`ib_umem_get` -> `dma_map_sgtable` going
  through `vfmig_iova_map_at()`). Tracked separately as
  `v1_allocator_user_mr_dma_gap`.

## 2. Background: how mlx5_ib UARs are organized today

```
mlx5_ib_ucontext
 |- bfregi (struct mlx5_bfreg_info)
 |   |- sys_pages[0 .. num_sys_pages-1]   <- u32, FW UAR id per slot
 |   |- num_static_sys_pages              <- count allocated at ucontext create
 |   |- num_dyn_bfregs                    <- count reserved for ALLOC_WC
 |   |- num_sys_pages                     <- static + dynamic
 |   |- count[0 .. max_bfregs-1]          <- per-bfreg-slot live refcount
 |   |- lib_uar_4k, lib_uar_dyn           <- request flags
 |- devx_uid, cqe_version, lib_caps, tdn  <- additional ucontext metadata
```

Key relationships (from `drivers/infiniband/hw/mlx5/main.c:1801`,
`drivers/infiniband/hw/mlx5/qp.c:697-787`, `drivers/infiniband/hw/mlx5/main.c:2355`):

* **The ucontext owns whole UARs**, one per `sys_pages[i]` slot. Each slot
  is exactly one `mlx5_cmd_uar_alloc()` call against firmware.
* **A bfreg is a sub-region of a UAR**, used as a blueflame doorbell. A
  4 KiB UAR carries multiple bfreg slots (typically 8 bfregs/UAR with 64-byte
  stride; one of them is the non-blueflame slot). bfregs are claimed by QPs
  via `alloc_bfreg()` at QP-create time.
* **`mmap()` operates at UAR granularity, not bfreg granularity.**
  `uar_mmap()` (drivers/infiniband/hw/mlx5/main.c:2355) parses
  `vm_pgoff = (mmap_cmd << 8) | uar_slot_idx`, looks up
  `bfregi->sys_pages[uar_slot_idx]`, and maps the corresponding 4 KiB MMIO
  page. Userspace (libmlx5) then carves bfreg sub-regions out of the
  mapping itself.
* **`mmap_cmd` selects the cacheability**:
  | cmd | name | meaning |
  |-----|------|---------|
  |  0  | `REGULAR_PAGE` | best-effort WC, fall back to NC |
  |  2  | `WC_PAGE`      | write-combining required |
  |  3  | `NC_PAGE`      | non-cacheable / strict-ordered |
  |  6  | `ALLOC_WC`     | dynamic UAR alloc + WC mmap (in scope, see below) |

  The same physical UAR page may be `mmap()`'d twice in one ucontext with
  different cacheability (libmlx5 selects per doorbell type). That makes
  `mmap_cmd` part of the per-VMA identity, not bookkeeping.

## 3. Empirical foundation: FW preserves per-VHCA allocator state

The whole design above ("seed `bfregi->sys_pages[]` verbatim with the
source's FW UAR ids and skip the per-slot `ALLOC_UAR`") only works if FW
treats those ids as **already reserved on the destination VHCA after
`LOAD_VHCA_STATE`**. If FW reset its per-VHCA allocators on `LOAD`, the
source ids would conflict with whatever ids FW hands out next, and we'd
need an explicit "pin id N to ucontext slot S" verb on the wire.

Two experiments this milestone falsified the "FW resets" hypothesis on a
**different physical FW instance** (cross-host), which is the regime we
actually care about:

### 3.1 Setup

* Two physically distinct hosts, host A (source) and host B (destination),
  each with their own ConnectX HCA + FW boot.
* On each host: a tracked VF provisioned via `MLX5_VFMIG_IOC_SET_TRACKED`,
  bound to mlx5_core. On the destination, additionally `LOAD_VHCA_STATE`
  with the blob from host A and `MARK_RESTORED` before bind.
* Two kernel-side probes added for this experiment:
  - `mlx5_load()` instrumented with a `mlx5_core_dbg` print of
    `(uar.index, bfreg.index)` for the first kernel BFREG that the VF
    allocates after probe. `uar.index` is the FW UAR id returned by
    `ALLOC_UAR`.
  - New PF-cdev ioctl `MLX5_VFMIG_IOC_PROBE_UID` that calls
    `CREATE_UCTX` -> log the returned `uid` -> `DESTROY_UCTX`. This
    probes "what's the next FW UCTX id FW would hand out on this VHCA?"
* Driver script: `tools/testing/criu_rdma/uar_restore/probe_uar_persistence.sh` runs
  the SAVE side on host A then walks the operator through LOAD on host B
  and aggregates kernel-log results from `journalctl --since=$start_ts`.

### 3.2 What each branch of the result would have meant

For each of the two allocators (UAR and UCTX), a `LOAD`-resets-allocator
hypothesis predicts the destination VHCA starts numbering from FW's
fresh-VHCA baseline (UAR `idx=0` for the first BFREG, UID `1` for the
first `CREATE_UCTX`). A `LOAD`-preserves-HWM hypothesis predicts
destination ids strictly above the source's high-water mark.

| Hypothesis  | predicted dst UAR id | predicted dst UID |
|-------------|----------------------|-------------------|
| FW resets   | `0`                  | `1`               |
| FW preserves HWM | `>= src HWM`    | `>= src HWM`      |

### 3.3 Observed values

| Allocator       | src observation        | dst observation (fresh FW instance) | verdict |
|-----------------|------------------------|-------------------------------------|---------|
| UAR (`mlx5_load`'s first bfreg) | `uar.index=16` | `uar.index=22` (+6) | preserves (PASS-B) |
| UCTX (`PROBE_UID` via CREATE_UCTX) | `uid=2,2` (alloc-then-free reuses) | `uid=3` (no reuse path on fresh ucontext) | preserves (PASS-A) |

The +6 UAR delta is consistent with destination-side mlx5_core's own
boot-time UAR claims (PF, e-switch manager, etc.) before the VF's first
BFREG -- i.e., the dst started at `>= 17` and arrived at `22` after the
intervening kernel-side allocations. The UID step from HWM `2` to `3` is
exactly the next-id behavior of an allocator that survived `LOAD`.

### 3.4 Why this only convinces us if it's cross-host

A single-host run could be explained by "FW allocators are sticky across
`SET_TRACKED`/`LOAD_VHCA_STATE` *within one FW boot*" -- which would tell
us nothing about migration. The actual physical-FW-instance change between
host A and host B is what makes the result load-bearing: the destination
FW has never seen these ids before, and the only way it could refuse to
hand them out via a fresh `ALLOC_UAR` / `CREATE_UCTX` is if `LOAD_VHCA_STATE`
explicitly transferred the source's HWM into the destination VHCA's
allocator state.

### 3.5 What this proves and does not prove

**Proves:** FW preserves the per-VHCA UAR and UCTX allocator high-water
marks across `LOAD_VHCA_STATE`. Destination `ALLOC_UAR` / `CREATE_UCTX`
will not collide with source ids.

**Does NOT prove (and we don't need it to):**

* That **specific** source ids can be re-acquired by repeated
  `ALLOC_UAR` on the destination. We don't try -- the design instead
  *skips* `ALLOC_UAR` entirely and just remembers the source's ids in
  `bfregi->sys_pages[]`. FW only sees those ids again as they're
  embedded in subsequent FW commands (e.g., `CREATE_QP`'s `qpc.uar_page`).
* That FW will accept those ids in subsequent commands. This is implied
  by FW's own consistency guarantees -- the source's ids are valid
  references to FW objects whose state was just imported -- and is
  validated end-to-end by the integration test in step 4 (mmap +
  doorbell write/read roundtrip on a restored VF).

### 3.6 Implication for this design

When the destination kernel issues a future FW command that names a UAR
or UCTX id (e.g., `qpc.uar_page` in `CREATE_QP`, or `uid` in any
uid-stamped command), FW resolves it against state that was imported by
`LOAD_VHCA_STATE`. The mlx5_ib restore mechanism therefore is:
"tell mlx5_ib to skip its per-slot `ALLOC_UAR` call, seed
`bfregi->sys_pages[]` with the source's ids, and let those ids flow
naturally into subsequent FW commands". No new FW operation is needed,
and no per-id "pin" verb on the wire is needed.

## 4. The split: PF cdev vs uverbs vendor verbs

Two cdevs are involved in the CRIU flow. They handle different layers:

| Layer | cdev | Verbs | Lifecycle |
|-------|------|-------|-----------|
| VF FW state | `/dev/mlx5_vfmig/<pf-bdf>` | `SAVE_VHCA_STATE`, `LOAD_VHCA_STATE`, `SET_TRACKED`, `MARK_RESTORED`, `ENABLE_MIGRATABLE` | admin (CRIU's `init()`/`fini()` hooks) |
| Per-process ucontext | `/dev/infiniband/uverbs<N>` (mlx5_ib vendor namespace) | `MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT`, `MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT` | per-process (CRIU plugin runs in dumped/restored process) |

Per-ucontext verbs live on the uverbs fd because:

1. The dumped/restored process holds the uverbs fd but typically *not* the
   PF cdev (which is privileged). Per-ucontext queries via the PF cdev
   would require out-of-band privileged access for every ucontext.
2. The ucontext is the natural object: the existing `UVERBS_ATTR_IDR`
   handle resolution gives us the `mlx5_ib_ucontext` for free; no need to
   plumb `(vf_id, vhca_id, fd)` through a sibling cdev.
3. mlx5_ib already serves vendor-specific verbs on the uverbs fd via the
   `UVERBS_OBJECT_*` / `UVERBS_METHOD_*` framework
   (`MLX5_IB_OBJECT_UAR`, `_DEVX_*`, `_DM`, `_FLOW_*`, `_VAR`, etc.) The
   HW-agnostic `IB_USER_VERBS_CMD_*` write/ioctl path stays untouched.

## 5. Verb specifications

### 5.1 New UAPI: `mlx5_ib_alloc_uctx_v2_flags`

Add one bit to the existing `mlx5_ib_alloc_ucontext_req_v2.flags` field
(include/uapi/rdma/mlx5-abi.h):

```c
enum mlx5_ib_alloc_uctx_v2_flags {
    MLX5_IB_ALLOC_UCTX_DEVX           = 1 << 0,  /* existing */
    MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE  = 1 << 1,  /* NEW */
};
```

Semantics: when set, `mlx5_ib_alloc_ucontext()` performs all normal setup
(bfregi sizing, sys_pages[] array allocation, count[] array allocation,
xa_init for objects, etc.) **except** it skips per-slot
`mlx5_cmd_uar_alloc()`. Each `bfregi->sys_pages[i]` is left at the existing
sentinel `MLX5_IB_INVALID_UAR_INDEX`. The caller is required to follow up
with `MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT` before any UAR `mmap()` is
attempted; otherwise `uar_mmap()` will reject the request with `-EINVAL`
via the existing INVALID-slot check.

### 5.2 New UAPI: `MLX5_IB_OBJECT_VFMIG_UCONTEXT`

```c
/* include/uapi/rdma/mlx5_user_ioctl_cmds.h */

enum mlx5_ib_vfmig_ucontext_methods {
    MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT,
    MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT,
};

enum mlx5_ib_vfmig_ucontext_attrs {
    MLX5_IB_ATTR_VFMIG_UCONTEXT_HANDLE,        /* IDR, MANDATORY */
    MLX5_IB_ATTR_VFMIG_UCONTEXT_UAR_TABLE,     /* PTR_OUT/IN */
    MLX5_IB_ATTR_VFMIG_UCONTEXT_BFREG_COUNT,   /* PTR_OUT/IN */
    MLX5_IB_ATTR_VFMIG_UCONTEXT_META,          /* PTR_OUT/IN */
};

struct mlx5_ib_vfmig_ucontext_meta {
    __u32 num_static_sys_pages;
    __u32 num_sys_pages;        /* static + dynamic. Entries
                                 * [num_static..num_sys] are
                                 * MLX5_IB_INVALID_UAR_INDEX for unused
                                 * dynamic slots (i.e., slots libmlx5
                                 * never mmap'd via ALLOC_WC).
                                 */
    __u32 num_dyn_bfregs;
    __u32 num_low_latency_bfregs;
    __u32 total_num_bfregs;     /* size of count[] array */
    __aligned_u64 lib_caps;
    __u8  lib_uar_4k;
    __u8  lib_uar_dyn;
    __u8  cqe_version;
    __u8  reserved[5];
};
```

`UAR_TABLE` is a `__u32[num_sys_pages]` raw array carrying the FW UAR ids
verbatim (`MLX5_IB_INVALID_UAR_INDEX` for unused dynamic slots).

`BFREG_COUNT` is a `__u32[total_num_bfregs]` raw array carrying
`bfregi->count[]`. The semantics of this array are split:

* **Static bfreg slots** (indices `< first_hi_bfreg`): per-bfreg-slot
  refcount of the QPs that have claimed it (qp.c:735-760, `bfregn`
  allocation). For v0, R3 QP-restore is **out of scope**, so a freshly-
  restored ucontext has no QPs and these slots should naturally be 0.
  RESTORE rejects any non-zero entry here as -EINVAL with a message
  pointing at R3 QP-restore.
* **Dynamic-UAR slot boundaries** (each `bfreg_dyn_idx` corresponding to
  the first bfreg of a dynamic UAR page; indices
  `>= num_static_sys_pages * uars_per_page * MLX5_NON_FP_BFREGS_PER_UAR`):
  "this dynamic UAR is claimed" boolean. Set by `uar_mmap()` on
  successful `mmap(ALLOC_WC)` (main.c:2422), checked on subsequent
  attempts to refuse double-claim (main.c:2416). RESTORE seeds these
  verbatim from the source so dynamic-UAR claim state survives.

This split lives in the kernel-side validator; userspace just ships
`count[]` in full. Without `BFREG_COUNT` on the wire, a restored
process would silently allow `mmap(ALLOC_WC)` against an already-claimed
dynamic slot.

`META` carries every ucontext-create-time setting needed to detect
cross-build incompatibility at restore time.

#### 5.2.1 `MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT`

Takes: ucontext handle (IDR).
Returns: UAR_TABLE (out), BFREG_COUNT (out), META (out).

Body: trivial copy-out of `bfregi->sys_pages[]`, `bfregi->count[]` and the
META fields. No FW interaction. Mutex-protected by `bfregi->lock` to
snapshot consistently against concurrent dynamic-UAR allocation in
`uar_mmap()`.

#### 5.2.2 `MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT`

Takes: ucontext handle (IDR), UAR_TABLE (in), BFREG_COUNT (in), META (in).
Returns: nothing.

Pre-conditions, all checked and error-returned with -EINVAL on mismatch:

* `context->vfmig_restore_pending == true` (i.e. the ucontext was created
  with `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE`).
* META's `num_sys_pages`, `num_static_sys_pages`, `num_dyn_bfregs`,
  `num_low_latency_bfregs`, `total_num_bfregs`, `lib_uar_4k`,
  `lib_uar_dyn`, `cqe_version`, `lib_caps` agree with what the destination
  ucontext was built with. Mismatch means the source process was built
  against different libmlx5 / different FW caps and a verbatim UAR-id
  replay would land at the wrong bfregi indices.
* No UAR `mmap()` has yet been performed against this ucontext.

Body:

```c
mutex_lock(&bfregi->lock);
for (i = 0; i < num_sys_pages; i++)
    bfregi->sys_pages[i] = uar_table[i];   /* trust the caller's payload;
                                            * FW already has these reserved
                                            * via LOAD_VHCA_STATE. */
/* Validate the static-vs-dynamic split (see §5.2's BFREG_COUNT spec):
 * static bfreg slots [0..first_hi_bfreg) MUST be zero on input (v0:
 * no QPs are restored, so any refcount here is a wire bug); dynamic
 * boundary entries are seeded verbatim.
 */
for (i = 0; i < first_hi_bfreg(dev, bfregi); i++) {
    if (bfreg_count[i] != 0) {
        mutex_unlock(&bfregi->lock);
        return -EINVAL;  /* static bfreg refcount on the wire; QP
                          * restore is R3, not v0. */
    }
}
for (i = first_hi_bfreg(dev, bfregi); i < total_num_bfregs; i++)
    bfregi->count[i] = bfreg_count[i];  /* reproduce dynamic-UAR claim state */
context->vfmig_restore_pending = false;
mutex_unlock(&bfregi->lock);
```

After this returns, `uar_mmap()` resolves `vm_pgoff` exactly as on the
source process: `idx = vm_pgoff & 0xff` -> `sys_pages[idx]` -> physical
UAR page. See §6.3 for the small `uar_mmap()` extension that lets the
dynamic path skip lazy `ALLOC_UAR` when a slot has been seeded by
RESTORE.

## 6. Driver-side changes

### 6.1 `mlx5_ib_alloc_ucontext()` skip-allocate-uars hook

```c
/* drivers/infiniband/hw/mlx5/main.c */

static int allocate_uars(struct mlx5_ib_dev *dev,
                         struct mlx5_ib_ucontext *context)
{
    if (context->vfmig_restore_pending)
        return 0;  /* sys_pages[] left INVALID; vfmig RESTORE seeds it. */
    /* ... existing body unchanged ... */
}
```

The flag bit from §5.1 sets `context->vfmig_restore_pending` early in
`mlx5_ib_alloc_ucontext()` (right after request-struct parsing). One short-
circuit at the top of `allocate_uars`. No nested if/else.

### 6.2 `mlx5_ib_ucontext` field

```c
struct mlx5_ib_ucontext {
    /* ... */
    bool vfmig_restore_pending;
    /* ... */
};
```

Cleared by `MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT` once the table is
seeded. If the ucontext is destroyed while still pending (e.g. CRIU
process exits mid-restore), `deallocate_uars()` already correctly skips
INVALID slots (`drivers/infiniband/hw/mlx5/main.c:1838-1841`), so cleanup
is a no-op for unseeded entries. This is a free correctness win.

### 6.3 `uar_mmap()` dynamic-path: skip lazy alloc on restored slots

The static-UAR path in `uar_mmap()` already does the right thing once
`bfregi->sys_pages[]` is seeded -- it's a straight read of
`sys_pages[idx]` (main.c:2432) into a `pfn` and an `rdma_user_mmap_io`.

The dynamic-UAR path (main.c:2401-2433) is different: it calls
`mlx5_cmd_uar_alloc()` lazily on first mmap, *unconditionally*. For a
restored ucontext, the slot at `sys_pages[idx]` is already populated by
RESTORE_UCONTEXT and `count[bfreg_dyn_idx]` is already non-zero from the
source's claim state. We need one branch:

```c
if (dyn_uar) {
    /* ... existing bfreg_dyn_idx computation ... */

    /* Restored ucontext: slot is pre-seeded (sys_pages[idx] != INVALID,
     * count[] already carries source's claim state). Skip the lazy
     * ALLOC_UAR, treat it as already-allocated, fall through to mmap.
     */
    if (bfregi->sys_pages[idx] != MLX5_IB_INVALID_UAR_INDEX) {
        uar_index = bfregi->sys_pages[idx];
        /* Note: do NOT bump count[bfreg_dyn_idx] -- RESTORE_UCONTEXT
         * already seeded the count, this mmap is just remapping the
         * already-claimed slot.
         */
    } else {
        /* ... existing lazy-alloc body unchanged:
         *     check count[], bump it, mlx5_cmd_uar_alloc(), error
         *     unwind ...
         */
    }
}
```

Note the asymmetry vs the source: on the source, `count[]` is bumped by
`uar_mmap()` itself the first time a dynamic slot is mmap'd. On the
destination, `count[]` is bumped by RESTORE_UCONTEXT once, before any
mmap. After the first mmap on the destination, source and destination
states converge.

Rough size: ~15 LOC + comment.

### 6.4 Verb dispatch table registration

A new translation unit `drivers/infiniband/hw/mlx5/vfmig_uctx.c` (or
extension to existing `main.c`) defines:

* `mlx5_ib_vfmig_query_ucontext` handler
* `mlx5_ib_vfmig_restore_ucontext` handler
* `DECLARE_UVERBS_NAMED_METHOD(MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT, ...)`
* `DECLARE_UVERBS_NAMED_METHOD(MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT, ...)`
* `DECLARE_UVERBS_NAMED_OBJECT(MLX5_IB_OBJECT_VFMIG_UCONTEXT, ...)` (no
  `UVERBS_TYPE_ALLOC_IDR`; this object is a verb-only namespace, no
  per-instance state.)
* `mlx5_ib_vfmig_defs[]` table.

Add `&mlx5_ib_vfmig_defs` to `drivers/infiniband/hw/mlx5/main.c`'s
`mlx5_ib_dev_ops_devx` / equivalent driver_def aggregation (template:
existing `mlx5_ib_devx_defs`, `mlx5_ib_std_types_defs` pattern).

## 7. CRIU plugin integration

```
DUMP                                            RESTORE
---------------------------------------------- ----------------------------------------------
HANDLE_DEVICE_VMA per VMA backed by             init() (per CRIU plugin invocation)
  /dev/infiniband/uverbs<N>                       LOAD_VHCA_STATE on PF cdev
  - capture (vm_start, vm_end, vm_pgoff,          MARK_RESTORED on PF cdev
             prot, flags, fd_id) into             bind VF
             plugin VMA blob                    foreach saved ucontext:
  - mark VMA_EXT_PLUGIN; CRIU skips                ALLOC_UCONTEXT(MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE)
              page-content dump                    MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT(uar_table, meta)
                                                   cache the resulting fd
DUMP_UVERBS_CONTEXT per ucontext                UPDATE_VMA_MAP per VMA_EXT_PLUGIN
  - MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT            mmap(vm_start, len, prot,
       (uar_table, meta) -> plugin blob                  flags | MAP_FIXED,
                                                          cached_uverbs_fd,
                                                          vm_pgoff << PAGE_SHIFT)
fini()                                           OPEN_UVERBS_CDEV per cached context
  - SAVE_VHCA_STATE on PF cdev                     reattach ib_uobjects (PD/MR/CQ/QP)
                                                   [out of scope for this design;
                                                    handled by R3 follow-on]
```

Ordering invariants:

* **DUMP**: `MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT` and the VMA capture must
  both happen *before* `SAVE_VHCA_STATE`. The query reads kernel-side
  `bfregi->sys_pages[]` (no FW round trip), so the only requirement is
  consistency with the eventual SAVE — the source process must be quiesced
  before SAVE, which CRIU's freeze does by the time `fini()` runs.
* **RESTORE**: `LOAD_VHCA_STATE` must complete before any per-ucontext
  ALLOC. The PF cdev's existing flow already enforces this (LOAD must run
  before the VF binds, and the uverbs cdev is not exposed until the bind
  publishes the IB device).

## 8. Cross-host caveats

The design assumes **same-CPU/IOMMU-class src and dst**. Specifically:

| Property | Why it matters | What breaks if mismatched |
|---|---|---|
| WC vs NC capability for the BAR | libmlx5 picks `mmap_cmd` based on platform detection; CRIU replays src's choice | If dst's CPU/IOMMU truly can't sustain WC for that BAR, the WC mmap may behave incorrectly (silent data loss on doorbells) |
| `lib_uar_4k` / `lib_uar_dyn` | Determines `bfregs_per_sys_page` and the sys_pages[] layout | Mismatch would change `bfreg_idx -> uar_slot_idx` resolution; the META cross-check rejects with -EINVAL |
| `cqe_version` | Affects WQE / CQE encoding, indirectly the doorbell addresses | Mismatch caught by META cross-check |

This restricts v0 cross-host migration to homogeneous fleets. Cross-class
migration is an M3+ problem (would need a "rewire UARs" / "translate
mmap_cmd" logic, conceptually similar to MR rebinding).

## 9. Test plan: incremental landing

### Step 1: alloc-with-flag (skip allocate_uars)

* Land §5.1 + §6.1 + §6.2 only.
* Extend `tools/testing/criu_rdma/tools/mlx5_vfmig.c` with a probe subcommand
  `alloc_uctx_with_flag <ibdev>` that opens `/dev/infiniband/uverbs<N>`,
  issues `IB_USER_VERBS_CMD_GET_CONTEXT` with the new flag bit, dumps the
  `mlx5_ib_alloc_ucontext_resp.qp_tab_size` etc. for sanity.
* Trace via dynamic_debug a new `mlx5_ib_dbg("vfmig_restore_pending=1, skipping allocate_uars\n")`.
* Verify ucontext close cleanly without the matching seed call (cleanup
  correctness for the abandoned-restore path).

### Step 2: QUERY verb

* Land §5.2.1 + §6.3 partial (just the QUERY method).
* CLI: `query_uctx_uar_table <ibdev>` opens a *normal* ucontext (no flag),
  calls QUERY, dumps the returned `uar_table[]` and meta.
* Cross-check `uar_table[i]` against the existing
  `mlx5_ib_dbg(dev, "allocated uar %d\n", ...)` log lines from
  `allocate_uars()` (line 1814).
* Verifies the read path; doesn't depend on RESTORE existing.

### Step 3: RESTORE verb

* Land §5.2.2 + §6.3 complete.
* CLI: `roundtrip_uctx <ibdev>`:
  1. Open ucontext A (no flag), QUERY -> `uar_table_A`.
  2. Open ucontext B (with flag), QUERY -> all-INVALID.
  3. RESTORE on B with `uar_table_A` (verbatim). On a same-host ucontext
     this is technically wrong (FW UAR ids 0..15 are owned by A, not B),
     but it exercises the kernel-side seeding plumbing.
  4. QUERY B again -> matches `uar_table_A`.
  5. Close A, close B (verify no FW-deallocation noise on B's side; B's
     sys_pages[] are not "owned" by B in the FW sense).
* For a *correct* end-to-end roundtrip, requires step 4.

### Step 4: end-to-end with SAVE/LOAD

* Extend `save_load/test_iova_tracked_save_load.sh` with a new `UCTX=1` opt-in:
  1. Phase A: open ucontext on tracked source VF, QUERY -> save
     `uar_table`, `bfreg_count`, `meta`.
  2. Phase B: SAVE_VHCA_STATE.
  3. Phase C: tear down, re-provision dst.
  4. Phase D: LOAD_VHCA_STATE.
  5. Phase E: open ucontext on restored dst VF *with VFMIG_RESTORE flag*,
     RESTORE with the saved `uar_table` + `bfreg_count` + `meta`, QUERY
     -> matches.
  6. mmap each of `REGULAR_PAGE`, `WC_PAGE`, `NC_PAGE` against the same
     `sys_pages[idx]` at distinct VAs; for each, write a doorbell-shaped
     pattern, read it back. MMIO is a no-op-but-not-an-error sanity
     check.
* Extension for dynamic UARs (same step 4, adds one phase):
  7. Phase A continues: also `mmap(ALLOC_WC, dyn_idx)` on the source
     ucontext (forces a lazy `mlx5_cmd_uar_alloc` populating
     `sys_pages[num_static + dyn_idx]` and bumping
     `count[bfreg_dyn_idx]`).
  8. Phase E continues: after RESTORE, `mmap(ALLOC_WC, dyn_idx)` on the
     dst ucontext should hit the §6.3 fast path (slot already populated)
     and produce the same MMIO mapping without issuing a fresh
     `ALLOC_UAR`. Verify via dynamic_debug log that the lazy-alloc was
     skipped.
* Extension for clock-info (independent of UAR table):
  9. After RESTORE, `mmap(MLX5_IB_MMAP_CLOCK_INFO)` on dst ucontext;
     compare returned page contents against a freshly-opened reference
     ucontext on the same dst VF. Should match exactly (same page, same
     `vm_insert_page` target).
* Extends naturally to the eventual end-to-end pingpong once R3 lands.

Each step's CLI subcommand should be small (~30-50 LOC userspace). The
tests stand alone without CRIU integration.

## 10. Open questions

* **Should we capture and replay `bfregi->ver`?** It's set during ucontext
  creation based on FW caps. Same value will be re-derived on dst from the
  same FW caps. v0: not in META; revisit if a real cross-fleet migration
  surfaces a mismatch.
* **Multi-ucontext processes.** A single CRIU-dumped process may hold
  multiple uverbs cdev fds with multiple ucontexts. The plugin per-fd blob
  scales; nothing in this design is per-process. Just call out for the
  CRIU plugin author.

## 11. Sequencing relative to other work

* Lands **before** v2 user-MR allocator coverage: what CRIU is hitting
  first; smaller scope; orthogonal to ib_umem_get / dma_map_sgtable.
* Lands **before** R3's per-uobject restore (PD/MR/CQ/QP): ucontext is the
  parent of all those, so it has to come first anyway.
* Independent of L4 R2 (mlx5_ib dev-resource import) and L4 R1 (already
  done; gates dev-res init on restored VFs).

## 12. References

* `drivers/infiniband/hw/mlx5/main.c:1763-1843` — `set_ucontext_resp`,
  `allocate_uars`, `deallocate_uars`.
* `drivers/infiniband/hw/mlx5/main.c:1965-2230` — `mlx5_ib_alloc_ucontext`,
  `mlx5_ib_dealloc_ucontext`.
* `drivers/infiniband/hw/mlx5/main.c:2355-2464` — `uar_mmap`,
  `mlx5_ib_mmap` dispatch.
* `drivers/infiniband/hw/mlx5/main.c:4169-4198` — existing
  `MLX5_IB_OBJECT_UAR` declaration; template for the new VFMIG object.
* `drivers/infiniband/hw/mlx5/qp.c:697-787` — bfreg allocation; clarifies
  `bfreg` vs `UAR` separation.
* `include/uapi/rdma/mlx5-abi.h:77-100` — `mlx5_ib_alloc_ucontext_req_v2`,
  the natural slot for the new `MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE` flag.
* Cross-host UAR + UID persistence empirical results (this milestone, see
  agent transcript / todo `uar_uid_persistence_experiment`).
