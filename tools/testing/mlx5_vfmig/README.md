# mlx5_vfmig — host-side VF migration test harness

This directory holds the userspace tools, empirical probes, design
documents, and shell-driven test harnesses used to develop the
in-driver `/dev/mlx5_vfmig/<pf_bdf>` SAVE/LOAD/SUSPEND/RESUME control
plane added in `drivers/net/ethernet/mellanox/mlx5/core/vfmig/vfmig.c`,
plus the RDMA-uobject restore work that builds on top of it.

The intended consumer is a CRIU-style checkpoint/restore agent that
snapshots a running RDMA workload's VHCA state on one provisioning of
an mlx5 PF and re-applies it on the next, **without** going through a
guest VM or the VFIO mlx5 variant driver.

## Layout

```
design/                  -- design documents (one per major workstream)
  uar_restore.md         -- dynamic-UAR uobject restore (landed)
  user_mr_dma.md         -- user-MR DMA continuity across LOAD
  uobject_restore.md     -- generic RDMA-uobject restore (PD/CQ/QP/...)
tools/                   -- userspace CLIs
  mlx5_vfmig             -- multi-purpose CLI talking to /dev/mlx5_vfmig/<bdf>
                            (mark_restored, get_vhca_id, save_vhca_state,
                             load_vhca_state, enable_migratable, query_qp,
                             ...).
  synthetic_blob_emit    -- generates a single-record VFIO-mlx5-format blob
                            with synthetic FW_DATA payload; used to smoke-test
                            the LOAD parser FSM without a real save side.
  ucontext_vendor_verbs  -- raw uverbs ioctl exerciser for mlx5_ib's vfmig
                            ucontext vendor methods (alloc-with-flag,
                            dyn-UAR restore, ...).
  vfio_stop_copy_save    -- standalone helper that drains a VFIO mlx5
                            STOP_COPY data_fd into a file; only needed by
                            save_load/test_vfio_save_load_roundtrip.sh.
save_load/               -- end-to-end SAVE/LOAD harnesses
  test_pf_cdev_smoke.sh                  -- cdev existence / get_vhca_id wiring.
  test_vfio_save_load_roundtrip.sh       -- M2 plan: VFIO source -> mlx5_vfmig
                                             dest. Kept for regression.
  test_synthetic_load_plumbing.sh        -- LOAD-only with a synthetic blob;
                                             header-format / parser smoke.
  test_inkernel_save_load_roundtrip.sh   -- single-host SAVE+LOAD round-trip
                                             using only the in-driver cdev.
  test_iova_tracked_save_load.sh         -- same, but with the deterministic
                                             IOVA + tracked-VF path enabled;
                                             this is the primary harness for
                                             the rest of the restore work.
uar_restore/             -- probes for design/uar_restore.md
  probe_uar_persistence.sh -- bfreg/UAR id-continuity verdict on a round-trip.
uobject_restore/         -- probes for design/uobject_restore.md
  info_handles/
    info_handles_probe   -- empirical validation of UVERBS_METHOD_INFO_HANDLES
                            (§7.2).
  fw_id_continuity/
    fw_id_continuity_probe         -- libibverbs + mlx5dv probe that allocates
                                       PD/CQ/QP/MR/SRQ, prints FW ids in
                                       key=value form, blocks on stdin (§8.2,
                                       §6.3).
    test_fw_id_continuity.sh       -- driver script that forks the probe on
                                       source + destination across a
                                       SAVE/LOAD round-trip and renders the
                                       FW-id continuity verdict.
```

## Building

```
make -C tools/testing/mlx5_vfmig
```

builds `tools/*` plus the empirical probes. The optional VFIO STOP_COPY
helper is gated behind:

```
make -C tools/testing/mlx5_vfmig vfio-helper
```

because it pulls in `<linux/iommufd.h>` and `<linux/vfio.h>` which need
sanitized userspace headers.

## Running the in-driver round-trip

Assuming the kernel module under test is loaded (`mlx5_core.ko` plus
its dependencies `mlxfw`, `tls`) and the PF BDF is `0000:00:08.0`:

```
sudo PF=0000:00:08.0 ./save_load/test_inkernel_save_load_roundtrip.sh
```

This will:

1. Provision one VF, bind it to mlx5_core, snapshot baseline state.
2. SAVE its VHCA state into `/tmp/vf_m2r.blob` via the SAVE ioctl.
   The driver issues SUSPEND_VHCA(INITIATOR/RESPONDER) →
   QUERY_VHCA_MIGRATION_STATE → SAVE_VHCA_STATE → RESUME_VHCA(*).
3. Tear down the VF (`sriov_numvfs=0`), re-create it with
   `sriov_drivers_autoprobe=0`.
4. Stage the blob into the destination VF's pending_load slot via the
   LOAD ioctl. Set `migratable`. Mark the VF as restored.
5. Bind mlx5_core to the destination VF. The probe path runs the
   deferred SUSPEND + LOAD_VHCA_STATE + RESUME pair via the PF mdev,
   then skips SET_ISSI / boot pages / INIT_HCA on the destination VHCA.

For the deterministic-IOVA variant (the path on which the rest of the
restore work depends), use
`save_load/test_iova_tracked_save_load.sh` instead.

## Architectural finding (read this before debugging)

The SAVE blob captures the firmware's view of the VHCA, which is keyed
by **DMA addresses**: the cmd ring address, EQ buffer addresses, MR
backing page addresses, page-table roots, etc. SAVE/LOAD was designed
for the VFIO mlx5 passthrough model, where the destination VF stays
bound to `vfio_mlx5_pci` on both source and destination hosts and the
actual mlx5_core consumer lives inside a guest VM. The guest's RAM
image is what's being migrated, with IOMMU/passthrough preserving every
guest-physical address — so every IOVA in the blob still resolves on
the destination.

If the destination instead binds **native mlx5_core**, the destination's
cmd ring lives at a different DMA address than the source's, and after
LOAD the firmware silently ignores doorbells on the destination's cmd
ring. Bisection on CX-7 (FW 28.48.1000):

| Order                                | LOAD result                            | Cmd ring afterwards                            |
|--------------------------------------|----------------------------------------|------------------------------------------------|
| `ENABLE_HCA(self)` → LOAD            | `bad parameter` (syndrome `0x2c9bb0`)  | n/a                                            |
| LOAD → `ENABLE_HCA(self)`            | succeeds                               | dead — every command 60s timeout               |

The fix path is to put every FW-known DMA buffer at a deterministic
IOVA on both source and destination, which requires:

1. An IOMMU on both endpoints (vIOMMU in our QEMU guest, or SMMU on
   bare metal).
2. A "deterministic IOVA" allocator inside `mlx5_core` and `mlx5_ib`
   for migration-capable VFs.
3. Saving the contents of all kernel-internal mlx5 DMA buffers
   alongside the FW blob, and restoring them at the same IOVAs before
   issuing LOAD.

The tracked-VF path (`save_load/test_iova_tracked_save_load.sh`)
exercises that work end-to-end.
