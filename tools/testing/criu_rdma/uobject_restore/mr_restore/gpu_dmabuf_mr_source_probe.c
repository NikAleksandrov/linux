// SPDX-License-Identifier: GPL-2.0
/*
 * gpu_dmabuf_mr_source_probe -- source-side half of the SAVE/LOAD +
 * mr_restore_probe_mlx5_vfmig_dmabuf test cycle
 * (gpu-dmabuf-criu-plan.md §1A/§3c/§3d).
 *
 * Same shape and stdin/stdout protocol as
 * tools/testing/criu_rdma/uobject_restore/fw_id_continuity/fw_id_continuity_probe.c
 * (READY-then-key=value-dictionary, blocks on stdin for "quit\n"), so
 * it drops into the same driver-script pattern as
 * uobject_restore/mr_adopt/test_mr_adopt.sh Phase B/D -- but registers
 * a GPU dma-buf MR (CUDA VMM allocation + cuMemGetHandleForAddressRange
 * + ibv_reg_dmabuf_mr, same technique as the plan's step0_gpu_dmabuf_test.c)
 * instead of a plain ib_umem-pinned MR.
 *
 * Flow for a driver script:
 *   - Fork this probe in the background on the SOURCE ibdev.
 *   - Drain stdout until READY; capture pdn=, mkey_index=, lkey=,
 *     mr_length=, access_flags= from the printed dictionary.
 *   - suspend_vhca + save_vhca_state + resume_vhca (the MR stays
 *     alive in FW because this probe still holds it).
 *   - Tell the probe to exit ("quit\n"), tear down the source VF.
 *   - Provision a fresh dest VF, load_vhca_state, mark_restored, bind.
 *   - Run mr_restore_probe_mlx5_vfmig_dmabuf against the dest ibdev
 *     with the captured (pdn, mkey_index, lkey, mr_length, access_flags).
 *
 * Build:
 *   gcc -O2 -o gpu_dmabuf_mr_source_probe gpu_dmabuf_mr_source_probe.c \
 *       -I/usr/local/cuda/include -lcuda -libverbs -lmlx5
 *
 * Usage:
 *   ./gpu_dmabuf_mr_source_probe <ibdev> [--size-mb N] [--gpu N]
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cuda.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

#define CHECK_CU(call)                                                       \
	do {                                                                   \
		CUresult _res = (call);                                          \
		if (_res != CUDA_SUCCESS) {                                      \
			const char *_err = NULL;                                     \
			cuGetErrorString(_res, &_err);                               \
			fprintf(stderr, "gpu_dmabuf_mr_source_probe: CUDA error at %s:%d: %s (%d): %s\n", \
				__FILE__, __LINE__, #call, _res,                         \
				_err ? _err : "unknown");                                \
			exit(1);                                                     \
		}                                                                  \
	} while (0)

static struct ibv_device *find_ibdev(const char *name)
{
	struct ibv_device **list;
	int n, i;
	struct ibv_device *match = NULL;

	list = ibv_get_device_list(&n);
	if (!list || n == 0) {
		fprintf(stderr, "gpu_dmabuf_mr_source_probe: ibv_get_device_list returned 0\n");
		return NULL;
	}
	for (i = 0; i < n; i++) {
		if (!strcmp(ibv_get_device_name(list[i]), name)) {
			match = list[i];
			break;
		}
	}
	if (!match) {
		fprintf(stderr, "gpu_dmabuf_mr_source_probe: ibdev '%s' not found; available:", name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	ibv_free_device_list(list);
	return match;
}

static int extract_pdn(struct ibv_pd *pd, uint32_t *out)
{
	struct mlx5dv_pd dvpd = {};
	struct mlx5dv_obj obj = {
		.pd = { .in = pd, .out = &dvpd },
	};
	int err = mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD);
	if (err) {
		fprintf(stderr, "gpu_dmabuf_mr_source_probe: mlx5dv_init_obj(PD) failed: %d\n", err);
		return -1;
	}
	*out = dvpd.pdn;
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s <ibdev> [--size-mb N] [--gpu N]\n", prog);
}

/*
 * gpu-dmabuf-criu-3f-restore-injection-design.md Phase 4: REAL_CRIU_TEST=1
 * mode blocks on a signal instead of stdin, so a REAL `criu dump`/`criu
 * restore` cycle (unlike every other test in this file, which drives
 * this probe via a FIFO held open by an external, non-dumped shell
 * process) has nothing external to reconnect -- a named-pipe fd with
 * its writer held by a process outside the dumped tree is exactly the
 * kind of external resource criu needs `--external pipe:[inode]` (or
 * similar) for, and reopening a FIFO for read after restore blocks
 * until a writer attaches, a real hang risk this sidesteps entirely.
 * `kill -TERM <pid>` cleanly wakes it for exit (used instead of the
 * normal "quit\n" stdin line).
 */
static volatile sig_atomic_t g_got_term;
static void term_handler(int sig)
{
	(void)sig;
	g_got_term = 1;
}

int main(int argc, char **argv)
{
	const char *ibdev_name;
	size_t size_mb = 2;
	int gpu_ordinal = 0;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	ibdev_name = argv[1];
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--size-mb") && i + 1 < argc) {
			size_mb = strtoul(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--gpu") && i + 1 < argc) {
			gpu_ordinal = atoi(argv[++i]);
		} else {
			usage(argv[0]);
			return 2;
		}
	}
	size_t requested_size = size_mb * 1024ULL * 1024ULL;

	/* --- CUDA: allocate GPU memory via the VMM API, export as dma-buf --- */
	CHECK_CU(cuInit(0));

	CUdevice dev;
	CHECK_CU(cuDeviceGet(&dev, gpu_ordinal));

	CUcontext ctx;
	CHECK_CU(cuCtxCreate(&ctx, NULL, 0, dev));

	CUmemAllocationProp prop;
	memset(&prop, 0, sizeof(prop));
	prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
	prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
	prop.location.id = dev;

	size_t granularity = 0;
	CHECK_CU(cuMemGetAllocationGranularity(&granularity, &prop,
						CU_MEM_ALLOC_GRANULARITY_MINIMUM));
	size_t size = ((requested_size + granularity - 1) / granularity) * granularity;

	CUmemGenericAllocationHandle handle;
	CHECK_CU(cuMemCreate(&handle, size, &prop, 0));

	CUdeviceptr ptr;
	CHECK_CU(cuMemAddressReserve(&ptr, size, 0, 0, 0));
	CHECK_CU(cuMemMap(ptr, size, 0, handle, 0));

	CUmemAccessDesc access_desc;
	memset(&access_desc, 0, sizeof(access_desc));
	access_desc.location = prop.location;
	access_desc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
	CHECK_CU(cuMemSetAccess(ptr, size, &access_desc, 1));

	int dmabuf_fd = -1;
	CHECK_CU(cuMemGetHandleForAddressRange(
		&dmabuf_fd, ptr, size, CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0));

	/* --- ibverbs: open device, alloc PD, register the dma-buf MR --- */
	struct ibv_device *ibdev = find_ibdev(ibdev_name);
	if (!ibdev)
		return 1;

	struct ibv_context *ibctx = ibv_open_device(ibdev);
	if (!ibctx) {
		fprintf(stderr, "gpu_dmabuf_mr_source_probe: ibv_open_device(%s) failed\n",
			ibdev_name);
		return 1;
	}

	struct ibv_pd *pd = ibv_alloc_pd(ibctx);
	if (!pd) {
		perror("gpu_dmabuf_mr_source_probe: ibv_alloc_pd");
		return 1;
	}

	uint32_t pdn = 0;
	if (extract_pdn(pd, &pdn))
		return 1;

	int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
		     IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;

	struct ibv_mr *mr = ibv_reg_dmabuf_mr(pd, 0, size, 0, dmabuf_fd, access);
	if (!mr) {
		perror("gpu_dmabuf_mr_source_probe: ibv_reg_dmabuf_mr");
		return 1;
	}
	/*
	 * ibv_reg_dmabuf_mr() takes its own dma_buf_attach() reference;
	 * this fd isn't needed after registration succeeds. Close it
	 * here rather than holding it open until final cleanup --
	 * criu's generic file-dump code has no handler for a bare
	 * dma-buf fd ("Can't dump file N of that type [600] (unknown
	 * /dmabuf:)", confirmed 2026-08-19 exercising a real `criu
	 * dump` against this probe), so an application that means to
	 * stay criu-dumpable while holding a dma-buf MR shouldn't keep
	 * the fd open past registration anyway.
	 */
	close(dmabuf_fd);
	dmabuf_fd = -1;

	/*
	 * mlx5 invariant: lkey == rkey == (mkey_index << 8) | variant.
	 * access_flags printed here is the raw ib_access_flags bitmask
	 * (same encoding UVERBS_ATTR_RESTORE_MR_DMABUF_ACCESS_FLAGS
	 * expects), truncated to 8 bits per the driver-script convention
	 * in mr_restore_probe_mlx5_vfmig.c's usage doc.
	 */
	printf("pdn=%u\n", pdn);
	printf("mkey_index=%u\n", mr->lkey >> 8);
	printf("lkey=%u\n", mr->lkey);
	printf("rkey=%u\n", mr->rkey);
	printf("mr_length=%zu\n", size);
	printf("access_flags=%u\n", (unsigned int)(access & 0xff));
	printf("READY\n");
	fflush(stdout);

	if (getenv("REAL_CRIU_TEST")) {
		signal(SIGTERM, term_handler);
		while (!g_got_term)
			pause();
	} else {
		char line[64];
		while (fgets(line, sizeof(line), stdin)) {
			if (!strncmp(line, "quit", 4))
				break;
		}
	}

	ibv_dereg_mr(mr);
	ibv_dealloc_pd(pd);
	ibv_close_device(ibctx);
	cuMemUnmap(ptr, size);
	cuMemAddressFree(ptr, size);
	cuMemRelease(handle);
	cuCtxDestroy(ctx);

	return 0;
}
