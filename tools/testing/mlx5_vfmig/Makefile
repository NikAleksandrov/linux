# SPDX-License-Identifier: GPL-2.0
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter

# Default build: only the binaries required by test_m2r.sh and
# test_m2_synth.sh (the in-driver round-trip + synthetic LOAD smoke
# test). The VFIO STOP_COPY helper (mlx5_vfmig_save) is opt-in via
# `make vfio-helper` because it is only needed by the legacy
# test_m2.sh path and pulls in <linux/iommufd.h> + <linux/vfio.h>
# which require sanitized userspace headers.
BINARIES := mlx5_vfmig mlx5_vfmig_synth mlx5_vfmig_uctx k6_id_probe k2_info_handles_probe
VFIO_BINARIES := mlx5_vfmig_save

# Where to find sanitized userspace versions of <linux/iommufd.h> and
# <linux/vfio.h>. Override on the command line if you have installed
# them somewhere other than the system default, e.g.:
#   make vfio-helper UAPI_INCLUDE=/opt/linux-headers/usr/include
# Producing a tree at that path is typically:
#   make -C <kernel-src> headers_install INSTALL_HDR_PATH=/opt/linux-headers
UAPI_INCLUDE ?=

all: $(BINARIES)

vfio-helper: $(VFIO_BINARIES)

mlx5_vfmig: mlx5_vfmig.c ../../../include/uapi/linux/mlx5_vfmig.h
	$(CC) $(CFLAGS) -o $@ $<

mlx5_vfmig_synth: mlx5_vfmig_synth.c
	$(CC) $(CFLAGS) -o $@ $<

mlx5_vfmig_uctx: mlx5_vfmig_uctx.c
	$(CC) $(CFLAGS) -o $@ $<

# K6 FW-id continuity probe (DESIGN_R3_uobj_restore.md §8.2).
# Pulls in libibverbs + libmlx5 (mlx5dv) -- present on any host with
# rdma-core installed.
k6_id_probe: k6_id_probe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs -lmlx5

# K2 INFO_HANDLES exerciser (DESIGN_R3_uobj_restore.md §7.2).
# libibverbs only (no mlx5dv).
k2_info_handles_probe: k2_info_handles_probe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

mlx5_vfmig_save: mlx5_vfmig_save.c
	$(CC) $(CFLAGS) $(if $(UAPI_INCLUDE),-I$(UAPI_INCLUDE),) -o $@ $<

clean:
	rm -f $(BINARIES) $(VFIO_BINARIES)

.PHONY: all clean vfio-helper
