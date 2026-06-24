// SPDX-License-Identifier: GPL-2.0
/*
 * pd_restore_probe_rxe -- empirical validation of S3a:
 * UVERBS_METHOD_RESTORE_PD + rxe_restore_pd
 * (design/uobject_restore.md §7.3 + §9.4).
 *
 * What we validate against a stock rxe0 device:
 *
 *   1. Gate (negative). A ucontext opened WITHOUT
 *      RXE_ALLOC_UCTX_RESTORE_MODE cannot invoke RESTORE_PD;
 *      the dispatcher must return -EPERM via the per-driver
 *      ib_device_ops.ucontext_is_restore_mode predicate.
 *   2. Happy path. A ucontext opened WITH the flag can mint a
 *      PD uobject at the caller-chosen ufile handle 0x4242.
 *      Cross-check via UVERBS_METHOD_INFO_HANDLES that 0x4242
 *      shows up in the PD list.
 *   3. Collision. A second RESTORE_PD(0x4242) on the same
 *      ucontext returns -EBUSY (xa_insert collision inside
 *      rdma_alloc_begin_uobject_at_handle).
 *   4. No interference with normal alloc. The legacy
 *      IB_USER_VERBS_CMD_ALLOC_PD write path picks some fresh
 *      handle that is NOT 0x4242, and INFO_HANDLES now reports
 *      both.
 *   5. Destroy round-trip. IB_USER_VERBS_CMD_DEALLOC_PD(0x4242)
 *      succeeds and INFO_HANDLES no longer returns 0x4242.
 *
 * Run on any host with CONFIG_RDMA_RXE=m. No root needed if the
 * caller is in the rdma group (or /dev/infiniband/uverbsN is
 * world-rw). Tested against rxe0 over loopback.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/pd_restore/pd_restore_probe_rxe
 *
 * Usage:
 *   ./pd_restore_probe_rxe [<ibdev>]      # default rxe0
 *
 * No dependency on librxe (we bypass libibverbs's rxe provider
 * and talk to the uverbs cdev directly so we can pass the
 * RXE_ALLOC_UCTX_RESTORE_MODE flag in udata, which libibverbs
 * has no way to plumb through). libibverbs is only used as a
 * device-discovery convenience to map "rxe0" -> "uverbsN".
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <rdma/ib_user_verbs.h>

/*
 * Mirrors include/uapi/rdma/rdma_user_rxe.h. We inline rather than
 * including <rdma/rdma_user_rxe.h> because the installed rdma-core
 * uapi tree lags the in-tree kernel UAPI (the new
 * struct rxe_alloc_ucontext_req + RXE_ALLOC_UCTX_RESTORE_MODE this
 * probe depends on lands only after a `make headers_install` from
 * the in-tree kernel). Keep in sync with the authoritative header
 * at include/uapi/rdma/rdma_user_rxe.h.
 *
 * RXE_ALLOC_UCTX_RESTORE_MODE latches a sticky bit on the rxe
 * ucontext that the per-driver
 * ib_device_ops.ucontext_is_restore_mode predicate reports to the
 * generic UVERBS_METHOD_RESTORE_<TYPE> dispatchers.
 */
enum {
	RXE_ALLOC_UCTX_RESTORE_MODE = 1u << 0,
};

struct rxe_alloc_ucontext_req {
	uint32_t flags;
	uint32_t reserved;
};

/*
 * Uverbs ioctl wire format -- same structs as info_handles_probe.c.
 */
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
#define UVERBS_OBJECT_RESTORE			18

#define UVERBS_METHOD_INFO_HANDLES		1
#define UVERBS_ATTR_INFO_OBJECT_ID		0
#define UVERBS_ATTR_INFO_TOTAL_HANDLES		1
#define UVERBS_ATTR_INFO_HANDLES_LIST		2

#define UVERBS_METHOD_RESTORE_PD		0
#define UVERBS_ATTR_RESTORE_PD_HANDLE		0

/* Matches enum in include/uapi/rdma/ib_user_ioctl_verbs.h. */
#define RDMA_DRIVER_RXE_LOCAL			14

#define TARGET_HANDLE				0x4242u

/* ----------------------- legacy-write helpers ---------------------------- */

/*
 * Issue GET_CONTEXT on the freshly-opened uverbs cdev fd, with an
 * optional rxe_alloc_ucontext_req carried in udata. Returns 0 on
 * success and -errno on failure.
 */
static int do_get_context(int fd, uint32_t rxe_flags,
			  struct ib_uverbs_get_context_resp *resp_out)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_get_context	core;
		struct rxe_alloc_ucontext_req	req;
	} __attribute__((packed)) cmd = {};
	ssize_t n;

	cmd.hdr.command = IB_USER_VERBS_CMD_GET_CONTEXT;
	/*
	 * in_words / out_words are TOTAL payload (including hdr) in
	 * 4-byte units. See verify_hdr() in
	 * drivers/infiniband/core/uverbs_main.c: the legacy non-ex
	 * path requires `hdr.in_words * 4 == count` (= total bytes
	 * passed to write()).
	 */
	cmd.hdr.in_words = sizeof(cmd) / 4;
	cmd.hdr.out_words = sizeof(*resp_out) / 4;
	cmd.core.response = (uintptr_t)resp_out;
	cmd.req.flags = rxe_flags;

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

	cmd.hdr.command = IB_USER_VERBS_CMD_ALLOC_PD;
	cmd.hdr.in_words = sizeof(cmd) / 4;
	cmd.hdr.out_words = sizeof(resp) / 4;
	cmd.core.response = (uintptr_t)&resp;

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

	cmd.hdr.command = IB_USER_VERBS_CMD_DEALLOC_PD;
	cmd.hdr.in_words = sizeof(cmd) / 4;
	cmd.hdr.out_words = 0;
	cmd.core.pd_handle = pd_handle;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	return 0;
}

/* ----------------------- ioctl helpers ----------------------------------- */

/*
 * Invoke UVERBS_METHOD_RESTORE_PD on the given uverbs cdev fd.
 * Returns 0 on success, -errno on ioctl failure.
 */
static int do_restore_pd(int fd, uint32_t target_handle)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[1];
	} cmd = {};

	cmd.hdr.object_id = UVERBS_OBJECT_RESTORE;
	cmd.hdr.method_id = UVERBS_METHOD_RESTORE_PD;
	cmd.hdr.driver_id = RDMA_DRIVER_RXE_LOCAL;
	cmd.hdr.num_attrs = 1;
	cmd.hdr.length = sizeof(cmd);

	/*
	 * PTR_IN(__u32) attr. Kernel decides inline-vs-pointer by
	 * comparing attr->len against sizeof(u64); len <= 8 means the
	 * payload is packed inline in attr->data. uverbs_copy_from then
	 * memcpys the low len bytes into the target. See
	 * uverbs_attr_ptr_is_inline() in include/rdma/uverbs_ioctl.h.
	 */
	cmd.attrs[0].attr_id = UVERBS_ATTR_RESTORE_PD_HANDLE;
	cmd.attrs[0].len = sizeof(uint32_t);
	cmd.attrs[0].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[0].data = target_handle;

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

/*
 * Issue INFO_HANDLES(UVERBS_OBJECT_PD) and copy up to
 * capacity_handles entries into handles_out, plus the total filled
 * count into *total_out. Mirrors info_handles_probe.c.
 */
static int do_info_handles_pd(int fd, uint32_t *handles_out,
			      uint32_t capacity_handles,
			      uint32_t *total_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[3];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id = UVERBS_OBJECT_DEVICE;
	cmd.hdr.method_id = UVERBS_METHOD_INFO_HANDLES;
	cmd.hdr.driver_id = RDMA_DRIVER_RXE_LOCAL;

	cmd.attrs[n].attr_id = UVERBS_ATTR_INFO_OBJECT_ID;
	cmd.attrs[n].len = sizeof(uint64_t);
	cmd.attrs[n].flags = UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data = UVERBS_OBJECT_PD;
	n++;

	cmd.attrs[n].attr_id = UVERBS_ATTR_INFO_TOTAL_HANDLES;
	cmd.attrs[n].len = sizeof(*total_out);
	cmd.attrs[n].flags = 0;
	cmd.attrs[n].data = (uintptr_t)total_out;
	n++;

	cmd.attrs[n].attr_id = UVERBS_ATTR_INFO_HANDLES_LIST;
	cmd.attrs[n].len = (uint16_t)(capacity_handles * sizeof(uint32_t));
	cmd.attrs[n].flags = 0;
	cmd.attrs[n].data = (uintptr_t)handles_out;
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

/*
 * Map "<ibdev>" -> "/dev/infiniband/<uverbs_cdev>" by walking the
 * libibverbs device list. We never call ibv_open_device on it; we
 * open the cdev path ourselves so the GET_CONTEXT udata is fully
 * under our control (libibverbs has no plumbing for the rxe
 * RXE_ALLOC_UCTX_RESTORE_MODE flag).
 */
static int resolve_cdev_path(const char *ibdev_name, char *out, size_t outlen)
{
	struct ibv_device **list;
	int n, i, ret = -ENODEV;

	list = ibv_get_device_list(&n);
	if (!list || n == 0) {
		fprintf(stderr,
			"pd_restore: ibv_get_device_list returned no devices\n");
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
		fprintf(stderr, "pd_restore: ibdev '%s' not found; available:",
			ibdev_name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	ibv_free_device_list(list);
	return ret;
}

/* ----------------------- subtests ---------------------------------------- */

static int subtest_gate_negative(const char *cdev_path)
{
	struct ib_uverbs_get_context_resp resp = {};
	int fd, ret;
	int fails = 0;

	printf("[1] gate: ucontext WITHOUT restore mode -> RESTORE_PD must -EPERM\n");

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

	ret = do_restore_pd(fd, TARGET_HANDLE);
	if (ret == -EPERM) {
		printf("  PASS RESTORE_PD on non-restore-mode ucontext -> -EPERM\n");
	} else if (ret == 0) {
		fprintf(stderr,
			"  FAIL RESTORE_PD on non-restore-mode ucontext succeeded "
			"(security regression: predicate not consulted)\n");
		fails++;
	} else {
		fprintf(stderr,
			"  FAIL RESTORE_PD on non-restore-mode ucontext -> %s "
			"(expected -EPERM)\n", strerror(-ret));
		fails++;
	}

	close(fd);
	return fails;
}

static int subtest_happy_path(int fd)
{
	uint32_t list[8] = {};
	uint32_t total = 0;
	int ret;

	printf("[2] happy path: RESTORE_PD(0x%x) on restore-mode ucontext\n",
	       TARGET_HANDLE);

	ret = do_restore_pd(fd, TARGET_HANDLE);
	if (ret) {
		fprintf(stderr,
			"  FAIL RESTORE_PD(0x%x): %s%s\n",
			TARGET_HANDLE, strerror(-ret),
			ret == -EOPNOTSUPP
			? "  (rxe_restore_pd not registered in rxe_dev_ops?)"
			: ret == -EPERM
			? "  (rxe_ucontext_is_restore_mode not reporting true?)"
			: "");
		return 1;
	}
	printf("  PASS RESTORE_PD(0x%x) -> 0\n", TARGET_HANDLE);

	ret = do_info_handles_pd(fd, list, 8, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(PD): %s\n",
			strerror(-ret));
		return 1;
	}
	if (!handle_present(list, total, TARGET_HANDLE)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(PD): handle 0x%x not in list (total=%u)\n",
			TARGET_HANDLE, total);
		return 1;
	}
	printf("  PASS INFO_HANDLES(PD) returned 0x%x among %u entries\n",
	       TARGET_HANDLE, total);
	return 0;
}

static int subtest_collision(int fd)
{
	int ret;

	printf("[3] collision: second RESTORE_PD(0x%x) must -EBUSY\n",
	       TARGET_HANDLE);

	ret = do_restore_pd(fd, TARGET_HANDLE);
	if (ret == -EBUSY) {
		printf("  PASS second RESTORE_PD(0x%x) -> -EBUSY\n",
		       TARGET_HANDLE);
		return 0;
	}
	fprintf(stderr,
		"  FAIL second RESTORE_PD(0x%x) -> %s (expected -EBUSY)\n",
		TARGET_HANDLE, ret ? strerror(-ret) : "0 (success)");
	return 1;
}

static int subtest_alloc_pd_does_not_collide(int fd, uint32_t *fresh_out)
{
	uint32_t fresh = 0;
	uint32_t list[8] = {};
	uint32_t total = 0;
	int ret;

	printf("[4] non-interference: legacy ALLOC_PD picks a fresh handle != 0x%x\n",
	       TARGET_HANDLE);

	ret = do_alloc_pd(fd, &fresh);
	if (ret) {
		fprintf(stderr, "  FAIL ALLOC_PD: %s\n", strerror(-ret));
		return 1;
	}
	if (fresh == TARGET_HANDLE) {
		fprintf(stderr,
			"  FAIL ALLOC_PD returned 0x%x (collides with reserved handle)\n",
			fresh);
		return 1;
	}
	printf("  PASS ALLOC_PD -> handle=%u (!= 0x%x)\n", fresh, TARGET_HANDLE);

	ret = do_info_handles_pd(fd, list, 8, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(PD): %s\n",
			strerror(-ret));
		return 1;
	}
	if (!handle_present(list, total, TARGET_HANDLE) ||
	    !handle_present(list, total, fresh)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(PD): expected both 0x%x and %u in list (total=%u)\n",
			TARGET_HANDLE, fresh, total);
		return 1;
	}
	printf("  PASS INFO_HANDLES(PD) lists both 0x%x and %u\n",
	       TARGET_HANDLE, fresh);

	*fresh_out = fresh;
	return 0;
}

static int subtest_destroy_round_trip(int fd, uint32_t fresh)
{
	uint32_t list[8] = {};
	uint32_t total = 0;
	int ret;

	printf("[5] destroy round-trip: DEALLOC_PD(0x%x) succeeds and handle is gone\n",
	       TARGET_HANDLE);

	ret = do_dealloc_pd(fd, TARGET_HANDLE);
	if (ret) {
		fprintf(stderr, "  FAIL DEALLOC_PD(0x%x): %s\n",
			TARGET_HANDLE, strerror(-ret));
		return 1;
	}

	ret = do_info_handles_pd(fd, list, 8, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(PD): %s\n",
			strerror(-ret));
		return 1;
	}
	if (handle_present(list, total, TARGET_HANDLE)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(PD) still reports 0x%x after dealloc\n",
			TARGET_HANDLE);
		return 1;
	}
	if (!handle_present(list, total, fresh)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(PD) lost the legacy-allocated handle %u\n",
			fresh);
		return 1;
	}
	printf("  PASS DEALLOC_PD(0x%x) cleared the restored PD; "
	       "%u still present\n", TARGET_HANDLE, fresh);
	return 0;
}

/* ----------------------- main -------------------------------------------- */

int main(int argc, char **argv)
{
	const char *ibdev = argc > 1 ? argv[1] : "rxe0";
	char cdev_path[128];
	struct ib_uverbs_get_context_resp resp = {};
	int fd_restore, ret;
	uint32_t fresh_pd = 0;
	int fails = 0;

	if (resolve_cdev_path(ibdev, cdev_path, sizeof(cdev_path)) != 0)
		return 2;
	printf("pd_restore: ibdev=%s cdev=%s\n", ibdev, cdev_path);

	fails += subtest_gate_negative(cdev_path);

	fd_restore = open(cdev_path, O_RDWR | O_CLOEXEC);
	if (fd_restore < 0) {
		fprintf(stderr, "open(%s) for restore-mode ucontext: %s\n",
			cdev_path, strerror(errno));
		return 2;
	}
	ret = do_get_context(fd_restore, RXE_ALLOC_UCTX_RESTORE_MODE, &resp);
	if (ret) {
		fprintf(stderr,
			"GET_CONTEXT(flags=RXE_ALLOC_UCTX_RESTORE_MODE): %s\n",
			strerror(-ret));
		close(fd_restore);
		return 2;
	}

	fails += subtest_happy_path(fd_restore);
	fails += subtest_collision(fd_restore);
	fails += subtest_alloc_pd_does_not_collide(fd_restore, &fresh_pd);
	fails += subtest_destroy_round_trip(fd_restore, fresh_pd);

	close(fd_restore);

	if (fails) {
		fprintf(stderr, "\npd_restore_probe_rxe: FAIL (%d subtest failure(s))\n",
			fails);
		return 1;
	}
	printf("\npd_restore_probe_rxe: PASS\n");
	return 0;
}
