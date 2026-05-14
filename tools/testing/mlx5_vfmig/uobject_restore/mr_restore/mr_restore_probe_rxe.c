// SPDX-License-Identifier: GPL-2.0
/*
 * mr_restore_probe_rxe -- empirical validation of S4a:
 * UVERBS_METHOD_RESTORE_MR + rxe_restore_mr
 * (design/uobject_restore.md §7.x + §9.5).
 *
 * What we validate against a stock rxe0 device:
 *
 *   1. Gate (negative). A ucontext opened WITHOUT
 *      RXE_ALLOC_UCTX_RESTORE_MODE cannot invoke RESTORE_MR;
 *      the dispatcher must return -EPERM via the per-driver
 *      ib_device_ops.ucontext_is_restore_mode predicate.
 *   2. Happy path. A ucontext opened WITH the flag can mint an
 *      MR uobject at the caller-chosen ufile handle TARGET_HANDLE
 *      under a freshly-allocated parent PD, pinning a small
 *      mmap'd buffer. We additionally assert that the response
 *      lkey/rkey *differ* from the caller-supplied hints: rxe
 *      cannot honour hints (key bits are tied to the rxe_pool
 *      slot allocator), so the dispatcher echoes the *actual*
 *      installed lkey/rkey back, not the bounced-back hint.
 *      Cross-check via UVERBS_METHOD_INFO_HANDLES that the
 *      MR handle shows up in the MR list.
 *   3. Collision. A second RESTORE_MR(TARGET_HANDLE) on the same
 *      ucontext returns -EBUSY (xa_insert collision inside
 *      rdma_alloc_begin_uobject_at_handle).
 *   4. Parent PD missing. RESTORE_MR with a bogus pd_handle
 *      returns -ENOENT (IDR attr machinery's lookup miss).
 *   5. Cross-uobj refcount. While the restored MR is alive,
 *      DEALLOC_PD on its parent PD returns -EBUSY -- proves the
 *      atomic_inc(&pd->usecnt) the dispatcher installs is the
 *      right edge.
 *   6. Dereg round-trip. IB_USER_VERBS_CMD_DEREG_MR clears the
 *      MR handle; INFO_HANDLES no longer reports it; the parent
 *      PD's DEALLOC_PD now succeeds (cross-uobj edge released).
 *
 * Run on any host with CONFIG_RDMA_RXE=m. No root needed if the
 * caller is in the rdma group (or /dev/infiniband/uverbsN is
 * world-rw). Tested against rxe0 over loopback.
 *
 * Build:
 *   make -C tools/testing/mlx5_vfmig \
 *        uobject_restore/mr_restore/mr_restore_probe_rxe
 *
 * Usage:
 *   ./mr_restore_probe_rxe [<ibdev>]      # default rxe0
 *
 * Like pd_restore_probe_rxe, we bypass libibverbs's rxe provider
 * and talk to the uverbs cdev directly so we can pass the
 * RXE_ALLOC_UCTX_RESTORE_MODE flag in udata. libibverbs is only
 * used as a device-discovery convenience.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <rdma/ib_user_verbs.h>

/*
 * Mirrors include/uapi/rdma/rdma_user_rxe.h. See pd_restore_probe_rxe.c
 * for the rationale; the rxe alloc-ucontext UAPI bit is post-headers-install.
 */
enum {
	RXE_ALLOC_UCTX_RESTORE_MODE = 1u << 0,
};

struct rxe_alloc_ucontext_req {
	uint32_t flags;
	uint32_t reserved;
};

/* IB access flags (stable wire ABI, mirrored from include/rdma/ib_verbs.h). */
enum {
	IB_ACCESS_LOCAL_WRITE	= 1 << 0,
	IB_ACCESS_REMOTE_WRITE	= 1 << 1,
	IB_ACCESS_REMOTE_READ	= 1 << 2,
};

/* ----------------------- ioctl wire format ------------------------------- */

#define RDMA_IOCTL_MAGIC	0x1b
#define RDMA_VERBS_IOCTL	_IOWR(RDMA_IOCTL_MAGIC, 1, \
					      struct ib_uverbs_ioctl_hdr)

enum {
	UVERBS_ATTR_F_MANDATORY		= 1u << 0,
	UVERBS_ATTR_F_VALID_OUTPUT	= 1u << 1,
};

struct ib_uverbs_attr {
	uint16_t attr_id;
	uint16_t len;
	uint16_t flags;
	uint16_t attr_data_reserved;
	uint64_t data;
};

struct ib_uverbs_ioctl_hdr {
	uint16_t length;
	uint16_t object_id;
	uint16_t method_id;
	uint16_t num_attrs;
	uint64_t reserved1;
	uint32_t driver_id;
	uint32_t reserved2;
	struct ib_uverbs_attr attrs[];
};

/* Object / method / attr ids -- from include/uapi/rdma/ib_user_ioctl_cmds.h. */
#define UVERBS_OBJECT_DEVICE			0
#define UVERBS_OBJECT_PD			1
#define UVERBS_OBJECT_MR			7
#define UVERBS_OBJECT_RESTORE			18

#define UVERBS_METHOD_INFO_HANDLES		1
#define UVERBS_ATTR_INFO_OBJECT_ID		0
#define UVERBS_ATTR_INFO_TOTAL_HANDLES		1
#define UVERBS_ATTR_INFO_HANDLES_LIST		2

#define UVERBS_METHOD_RESTORE_MR		1
enum {
	UVERBS_ATTR_RESTORE_MR_HANDLE		= 0,
	UVERBS_ATTR_RESTORE_MR_PD_HANDLE	= 1,
	UVERBS_ATTR_RESTORE_MR_ADDR		= 2,
	UVERBS_ATTR_RESTORE_MR_LENGTH		= 3,
	UVERBS_ATTR_RESTORE_MR_IOVA		= 4,
	UVERBS_ATTR_RESTORE_MR_ACCESS_FLAGS	= 5,
	UVERBS_ATTR_RESTORE_MR_LKEY_HINT	= 6,
	UVERBS_ATTR_RESTORE_MR_RKEY_HINT	= 7,
	UVERBS_ATTR_RESTORE_MR_RESP_LKEY	= 8,
	UVERBS_ATTR_RESTORE_MR_RESP_RKEY	= 9,
};

/* Matches enum in include/uapi/rdma/ib_user_ioctl_verbs.h. */
#define RDMA_DRIVER_RXE_LOCAL			14

#define TARGET_HANDLE				0x4242u
#define BOGUS_PD_HANDLE				0xDEAD0042u

/* Synthetic identity hints rxe is expected to ignore. */
#define LKEY_HINT				0xCAFE0042u
#define RKEY_HINT				0xCAFE0043u

/* MR buffer size for the pinned umem. */
#define MR_BUF_LEN				4096u

/* ----------------------- legacy-write helpers ---------------------------- */

static int do_get_context(int fd, uint32_t rxe_flags,
			  struct ib_uverbs_get_context_resp *resp_out)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_get_context	core;
		struct rxe_alloc_ucontext_req	req;
	} __attribute__((packed)) cmd = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_GET_CONTEXT;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= sizeof(*resp_out) / 4;
	cmd.core.response	= (uintptr_t)resp_out;
	cmd.req.flags		= rxe_flags;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	return 0;
}

static int do_alloc_pd(int fd, uint32_t *pd_handle_out)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_alloc_pd	core;
	} __attribute__((packed)) cmd = {};
	struct ib_uverbs_alloc_pd_resp resp = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_ALLOC_PD;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= sizeof(resp) / 4;
	cmd.core.response	= (uintptr_t)&resp;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	*pd_handle_out = resp.pd_handle;
	return 0;
}

static int do_dealloc_pd(int fd, uint32_t pd_handle)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_dealloc_pd	core;
	} __attribute__((packed)) cmd = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_DEALLOC_PD;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= 0;
	cmd.core.pd_handle	= pd_handle;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	return 0;
}

static int do_dereg_mr(int fd, uint32_t mr_handle)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_dereg_mr	core;
	} __attribute__((packed)) cmd = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_DEREG_MR;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= 0;
	cmd.core.mr_handle	= mr_handle;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	return 0;
}

/* ----------------------- ioctl helpers ----------------------------------- */

/*
 * RESTORE_MR ioctl request. All 10 core attrs plus the optional UHW
 * (skipped here since rxe doesn't consume driver-private data).
 *
 * PTR_IN(__u32) and PTR_IN(__u64) <= 8 bytes are inlined into
 * attr->data per uverbs_attr_ptr_is_inline(). PTR_OUT attrs MUST
 * have a real user pointer in attr->data; the kernel will copy
 * sizeof(value) bytes there with copy_to_user.
 */
struct restore_mr_resp {
	uint32_t lkey;
	uint32_t rkey;
};

static int do_restore_mr(int fd, uint32_t target_handle, uint32_t pd_handle,
			 uint64_t addr, uint64_t length, uint64_t iova,
			 uint32_t access_flags,
			 uint32_t lkey_hint, uint32_t rkey_hint,
			 struct restore_mr_resp *resp_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[10];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= UVERBS_OBJECT_RESTORE;
	cmd.hdr.method_id	= UVERBS_METHOD_RESTORE_MR;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;

	/* HANDLE: PTR_IN(u32) -- inline */
	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_HANDLE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= target_handle;
	n++;

	/* PD_HANDLE: IDR(u32) -- per the IDR-class wire format the
	 * attr->data carries the ufile handle of the parent PD.
	 */
	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_PD_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= pd_handle;
	n++;

	/* ADDR / LENGTH / IOVA: PTR_IN(u64) -- inline */
	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_ADDR;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= addr;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_LENGTH;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= length;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_IOVA;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= iova;
	n++;

	/* ACCESS_FLAGS: FLAGS_IN(u32) -- inline */
	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_ACCESS_FLAGS;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= access_flags;
	n++;

	/* LKEY_HINT / RKEY_HINT: PTR_IN(u32) -- inline */
	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_LKEY_HINT;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= lkey_hint;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_RKEY_HINT;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= rkey_hint;
	n++;

	/* RESP_LKEY / RESP_RKEY: PTR_OUT(u32) -- real user pointer */
	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_RESP_LKEY;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)&resp_out->lkey;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_MR_RESP_RKEY;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)&resp_out->rkey;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

static int do_info_handles(int fd, uint16_t object_id, uint32_t *handles_out,
			   uint32_t capacity_handles, uint32_t *total_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[3];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= UVERBS_OBJECT_DEVICE;
	cmd.hdr.method_id	= UVERBS_METHOD_INFO_HANDLES;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_INFO_OBJECT_ID;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= object_id;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_INFO_TOTAL_HANDLES;
	cmd.attrs[n].len	= sizeof(*total_out);
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)total_out;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_INFO_HANDLES_LIST;
	cmd.attrs[n].len	= (uint16_t)(capacity_handles * sizeof(uint32_t));
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)handles_out;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

static bool handle_present(const uint32_t *list, uint32_t n, uint32_t want)
{
	for (uint32_t i = 0; i < n; i++)
		if (list[i] == want)
			return true;
	return false;
}

/* ----------------------- device discovery -------------------------------- */

static int resolve_cdev_path(const char *ibdev_name, char *out, size_t outlen)
{
	struct ibv_device **list;
	int n, i, ret = -ENODEV;

	list = ibv_get_device_list(&n);
	if (!list || n == 0) {
		fprintf(stderr,
			"mr_restore: ibv_get_device_list returned no devices\n");
		return -ENODEV;
	}
	for (i = 0; i < n; i++) {
		if (strcmp(ibv_get_device_name(list[i]), ibdev_name) == 0) {
			snprintf(out, outlen, "/dev/infiniband/%s",
				 list[i]->dev_name);
			ret = 0;
			break;
		}
	}
	if (ret) {
		fprintf(stderr, "mr_restore: ibdev '%s' not found; available:",
			ibdev_name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	ibv_free_device_list(list);
	return ret;
}

/* ----------------------- subtests ---------------------------------------- */

static int subtest_gate_negative(const char *cdev_path, uint32_t access_flags,
				 void *buf)
{
	struct ib_uverbs_get_context_resp resp = {};
	struct restore_mr_resp mr_resp = {};
	uint32_t pd_handle = 0;
	int fd, ret;
	int fails = 0;

	printf("[1] gate: ucontext WITHOUT restore mode -> RESTORE_MR must -EPERM\n");

	fd = open(cdev_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "  FAIL open(%s): %s\n", cdev_path,
			strerror(errno));
		return 1;
	}

	ret = do_get_context(fd, 0, &resp);
	if (ret) {
		fprintf(stderr,
			"  FAIL GET_CONTEXT(flags=0): %s\n", strerror(-ret));
		close(fd);
		return 1;
	}

	ret = do_alloc_pd(fd, &pd_handle);
	if (ret) {
		fprintf(stderr, "  FAIL ALLOC_PD: %s\n", strerror(-ret));
		close(fd);
		return 1;
	}

	ret = do_restore_mr(fd, TARGET_HANDLE, pd_handle,
			    (uintptr_t)buf, MR_BUF_LEN, (uintptr_t)buf,
			    access_flags, LKEY_HINT, RKEY_HINT, &mr_resp);
	if (ret == -EPERM) {
		printf("  PASS RESTORE_MR on non-restore-mode ucontext -> -EPERM\n");
	} else if (ret == 0) {
		fprintf(stderr,
			"  FAIL RESTORE_MR on non-restore-mode ucontext succeeded\n"
			"       (security regression: predicate not consulted)\n");
		fails++;
	} else {
		fprintf(stderr,
			"  FAIL RESTORE_MR on non-restore-mode ucontext -> %s\n"
			"       (expected -EPERM)\n", strerror(-ret));
		fails++;
	}

	close(fd);
	return fails;
}

static int subtest_happy_path(int fd, uint32_t pd_handle, uint32_t access_flags,
			      void *buf, struct restore_mr_resp *resp_out)
{
	uint32_t list[16] = {};
	uint32_t total = 0;
	int ret;

	printf("[2] happy path: RESTORE_MR(target=0x%x, pd=%u, hints lkey=0x%x rkey=0x%x)\n",
	       TARGET_HANDLE, pd_handle, LKEY_HINT, RKEY_HINT);

	ret = do_restore_mr(fd, TARGET_HANDLE, pd_handle,
			    (uintptr_t)buf, MR_BUF_LEN, (uintptr_t)buf,
			    access_flags, LKEY_HINT, RKEY_HINT, resp_out);
	if (ret) {
		fprintf(stderr,
			"  FAIL RESTORE_MR(target=0x%x): %s%s\n",
			TARGET_HANDLE, strerror(-ret),
			ret == -EOPNOTSUPP
			? "  (rxe_restore_mr not registered in rxe_dev_ops?)"
			: ret == -EPERM
			? "  (rxe_ucontext_is_restore_mode not reporting true?)"
			: "");
		return 1;
	}
	printf("  PASS RESTORE_MR(target=0x%x) -> 0  (resp lkey=0x%x rkey=0x%x)\n",
	       TARGET_HANDLE, resp_out->lkey, resp_out->rkey);

	/*
	 * Tighter check: rxe's lkey/rkey come from its internal pool
	 * allocator + rxe_get_next_key(), so they CANNOT equal the
	 * caller's hints. If they did, the dispatcher would be
	 * bouncing the hint back instead of echoing the driver's
	 * actual installed keys -- silent regression.
	 */
	if (resp_out->lkey == LKEY_HINT || resp_out->rkey == RKEY_HINT) {
		fprintf(stderr,
			"  FAIL response lkey/rkey equals the caller's hint\n"
			"       (rxe cannot honour hints; dispatcher must echo\n"
			"       the driver's actual installed keys)\n");
		return 1;
	}
	printf("  PASS resp keys differ from hints (rxe correctly ignored hints,\n"
	       "       dispatcher echoed driver-assigned keys)\n");

	ret = do_info_handles(fd, UVERBS_OBJECT_MR, list, 16, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(MR): %s\n",
			strerror(-ret));
		return 1;
	}
	if (!handle_present(list, total, TARGET_HANDLE)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(MR): handle 0x%x not in list (total=%u)\n",
			TARGET_HANDLE, total);
		return 1;
	}
	printf("  PASS INFO_HANDLES(MR) returned 0x%x among %u entries\n",
	       TARGET_HANDLE, total);
	return 0;
}

static int subtest_collision(int fd, uint32_t pd_handle, uint32_t access_flags,
			     void *buf)
{
	struct restore_mr_resp resp = {};
	int ret;

	printf("[3] collision: second RESTORE_MR(target=0x%x) must -EBUSY\n",
	       TARGET_HANDLE);

	ret = do_restore_mr(fd, TARGET_HANDLE, pd_handle,
			    (uintptr_t)buf, MR_BUF_LEN, (uintptr_t)buf,
			    access_flags, LKEY_HINT, RKEY_HINT, &resp);
	if (ret == -EBUSY) {
		printf("  PASS second RESTORE_MR(target=0x%x) -> -EBUSY\n",
		       TARGET_HANDLE);
		return 0;
	}
	fprintf(stderr,
		"  FAIL second RESTORE_MR(target=0x%x) -> %s (expected -EBUSY)\n",
		TARGET_HANDLE, ret ? strerror(-ret) : "0 (success)");
	return 1;
}

static int subtest_bogus_pd(int fd, uint32_t access_flags, void *buf)
{
	struct restore_mr_resp resp = {};
	int ret;

	printf("[4] bogus parent: RESTORE_MR(pd=0x%x) must -ENOENT\n",
	       BOGUS_PD_HANDLE);

	ret = do_restore_mr(fd, TARGET_HANDLE + 1, BOGUS_PD_HANDLE,
			    (uintptr_t)buf, MR_BUF_LEN, (uintptr_t)buf,
			    access_flags, LKEY_HINT, RKEY_HINT, &resp);
	if (ret == -ENOENT) {
		printf("  PASS RESTORE_MR(pd=0x%x) -> -ENOENT\n",
		       BOGUS_PD_HANDLE);
		return 0;
	}
	fprintf(stderr,
		"  FAIL RESTORE_MR(pd=0x%x) -> %s (expected -ENOENT)\n",
		BOGUS_PD_HANDLE, ret ? strerror(-ret) : "0 (success)");
	return 1;
}

static int subtest_pd_dealloc_busy_with_live_mr(int fd, uint32_t pd_handle)
{
	int ret;

	printf("[5] cross-uobj refcount: DEALLOC_PD(%u) with live MR must -EBUSY\n",
	       pd_handle);

	ret = do_dealloc_pd(fd, pd_handle);
	if (ret == -EBUSY) {
		printf("  PASS DEALLOC_PD(%u) -> -EBUSY (atomic_inc(&pd->usecnt)\n"
		       "       installed by the dispatcher is the right edge)\n",
		       pd_handle);
		return 0;
	}
	fprintf(stderr,
		"  FAIL DEALLOC_PD(%u) with live MR -> %s (expected -EBUSY;\n"
		"       missing atomic_inc(&pd->usecnt) in the dispatcher?)\n",
		pd_handle, ret ? strerror(-ret) : "0 (success)");
	return 1;
}

static int subtest_dereg_round_trip(int fd, uint32_t pd_handle)
{
	uint32_t list[16] = {};
	uint32_t total = 0;
	int ret;

	printf("[6] dereg round-trip: DEREG_MR(0x%x) + handle gone + DEALLOC_PD succeeds\n",
	       TARGET_HANDLE);

	ret = do_dereg_mr(fd, TARGET_HANDLE);
	if (ret) {
		fprintf(stderr, "  FAIL DEREG_MR(0x%x): %s\n",
			TARGET_HANDLE, strerror(-ret));
		return 1;
	}

	ret = do_info_handles(fd, UVERBS_OBJECT_MR, list, 16, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(MR): %s\n",
			strerror(-ret));
		return 1;
	}
	if (handle_present(list, total, TARGET_HANDLE)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(MR) still reports 0x%x after dereg\n",
			TARGET_HANDLE);
		return 1;
	}
	printf("  PASS DEREG_MR(0x%x) cleared the restored MR\n", TARGET_HANDLE);

	ret = do_dealloc_pd(fd, pd_handle);
	if (ret) {
		fprintf(stderr,
			"  FAIL DEALLOC_PD(%u) after dereg: %s\n"
			"       (cross-uobj edge not released?)\n",
			pd_handle, strerror(-ret));
		return 1;
	}
	printf("  PASS DEALLOC_PD(%u) -> 0 (edge released after dereg)\n",
	       pd_handle);
	return 0;
}

/* ----------------------- main -------------------------------------------- */

int main(int argc, char **argv)
{
	const char *ibdev = argc > 1 ? argv[1] : "rxe0";
	char cdev_path[128];
	struct ib_uverbs_get_context_resp resp = {};
	struct restore_mr_resp mr_resp = {};
	void *buf;
	int fd_restore, ret;
	uint32_t pd_handle = 0;
	uint32_t access_flags = IB_ACCESS_LOCAL_WRITE |
				IB_ACCESS_REMOTE_READ |
				IB_ACCESS_REMOTE_WRITE;
	int fails = 0;

	if (resolve_cdev_path(ibdev, cdev_path, sizeof(cdev_path)) != 0)
		return 2;
	printf("mr_restore: ibdev=%s cdev=%s\n", ibdev, cdev_path);

	/*
	 * Pin a page-aligned anonymous buffer; rxe's ib_umem_get() will
	 * lock it into the destination process's mm.
	 */
	buf = mmap(NULL, MR_BUF_LEN, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) {
		fprintf(stderr, "mmap(%u): %s\n", MR_BUF_LEN, strerror(errno));
		return 2;
	}
	memset(buf, 0, MR_BUF_LEN);

	fails += subtest_gate_negative(cdev_path, access_flags, buf);

	fd_restore = open(cdev_path, O_RDWR | O_CLOEXEC);
	if (fd_restore < 0) {
		fprintf(stderr, "open(%s) for restore-mode ucontext: %s\n",
			cdev_path, strerror(errno));
		munmap(buf, MR_BUF_LEN);
		return 2;
	}
	ret = do_get_context(fd_restore, RXE_ALLOC_UCTX_RESTORE_MODE, &resp);
	if (ret) {
		fprintf(stderr,
			"GET_CONTEXT(flags=RXE_ALLOC_UCTX_RESTORE_MODE): %s\n",
			strerror(-ret));
		close(fd_restore);
		munmap(buf, MR_BUF_LEN);
		return 2;
	}
	ret = do_alloc_pd(fd_restore, &pd_handle);
	if (ret) {
		fprintf(stderr,
			"ALLOC_PD on restore-mode ucontext: %s\n",
			strerror(-ret));
		close(fd_restore);
		munmap(buf, MR_BUF_LEN);
		return 2;
	}
	printf("parent PD: handle=%u\n", pd_handle);

	fails += subtest_happy_path(fd_restore, pd_handle, access_flags, buf,
				    &mr_resp);
	fails += subtest_collision(fd_restore, pd_handle, access_flags, buf);
	fails += subtest_bogus_pd(fd_restore, access_flags, buf);
	fails += subtest_pd_dealloc_busy_with_live_mr(fd_restore, pd_handle);
	fails += subtest_dereg_round_trip(fd_restore, pd_handle);

	close(fd_restore);
	munmap(buf, MR_BUF_LEN);

	if (fails) {
		fprintf(stderr,
			"\nmr_restore_probe_rxe: FAIL (%d subtest failure(s))\n",
			fails);
		return 1;
	}
	printf("\nmr_restore_probe_rxe: PASS\n");
	return 0;
}
