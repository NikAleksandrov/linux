# mlx5_vfmig — host-side VF migration test harness

This directory holds the userspace tools and shell tests used to drive
`/dev/mlx5_vfmig/<pf_bdf>`, the in-driver SAVE/LOAD/SUSPEND/RESUME
control plane added in `drivers/net/ethernet/mellanox/mlx5/core/vfmig.c`.

The intended consumer is a CRIU-style checkpoint/restore agent that
snapshots a running RDMA workload's VHCA state on one provisioning of
an mlx5 PF and re-applies it on the next, **without** going through a
guest VM or the VFIO mlx5 variant driver.

## Layout

| File                   | What it is                                                                 |
|------------------------|----------------------------------------------------------------------------|
| `mlx5_vfmig.c`         | Multi-purpose CLI: `get_vhca_id`, `enable_migratable`, `mark_restored`, `save_vhca_state`, `load_vhca_state`, `query_vf`. Built as `./mlx5_vfmig`. |
| `mlx5_vfmig_save.c`    | Standalone SAVE-only tool, retained for scripting convenience.             |
| `mlx5_vfmig_synth.c`   | Synthetic-blob generator used by `test_m2_synth.sh` for header-only LOAD plumbing tests. |
| `test_m2r.sh`          | The end-to-end SAVE+LOAD round-trip on one host (no IOMMU, no QEMU, no VM). |
| `test_m2.sh`           | Earlier M2 plan: SAVE only. Kept for regression.                            |
| `test_m2_synth.sh`     | Earlier M2 plan: LOAD-only with a synthetic 16-byte blob. Kept for regression. |
| `test.sh`              | Smoke test for the cdev existence / get_vhca_id wiring.                     |

## Building

```
make -C tools/testing/mlx5_vfmig
```

## Running the round-trip

Assuming the kernel module under test is loaded (`mlx5_core.ko` plus
its dependencies `mlxfw`, `tls`) and the PF BDF is `0000:00:08.0`:

```
sudo PF=0000:00:08.0 ./test_m2r.sh
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

| Order                                | LOAD result                            | Cmd ring afterwards                           |
|--------------------------------------|----------------------------------------|------------------------------------------------|
| `ENABLE_HCA(self)` → LOAD            | `bad parameter` (syndrome `0x2c9bb0`)  | n/a                                            |
| LOAD → `ENABLE_HCA(self)`            | succeeds                               | dead — every command 60s timeout               |

What works, end-to-end:

* `MLX5_VFMIG_IOC_SAVE_VHCA_STATE` produces a valid blob (~4.5 MB on a
  freshly-bound VF on this firmware).
* `MLX5_VFMIG_IOC_LOAD_VHCA_STATE` consumes the blob and the driver
  successfully runs SUSPEND + LOAD_VHCA_STATE + RESUME against the
  destination VHCA.

What does **not** yet work on this configuration:

* The destination VF reaching operational parity with the pre-save VF.
  `mlx5_query_hca_caps()` (the first VHCA-targeted command in
  `mlx5_function_open()`) times out, the bind fails, the netdev never
  reappears.

The fix path is to put every FW-known DMA buffer at a deterministic
IOVA on both source and destination, which requires:

1. An IOMMU on both endpoints (vIOMMU in our QEMU guest, or SMMU on
   bare metal).
2. A "deterministic IOVA" allocator inside `mlx5_core` and `mlx5_ib`
   for migration-capable VFs.
3. Saving the contents of all kernel-internal mlx5 DMA buffers
   alongside the FW blob, and restoring them at the same IOVAs before
   issuing LOAD.

That work is **not** in this tree. The plumbing here is the foundation
on which it can land.
