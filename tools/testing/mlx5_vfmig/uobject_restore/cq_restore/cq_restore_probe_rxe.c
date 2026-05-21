// SPDX-License-Identifier: GPL-2.0
/*
 * cq_restore_probe_rxe -- empirical validation of S5a:
 * UVERBS_METHOD_RESTORE_CQ + rxe_restore_cq
 * (design/uobject_restore.md §7.3 + §7.4 + §9.7).
 *
 * What we validate against a stock rxe0 device:
 *
 *   1. Gate (negative). A ucontext opened WITHOUT
 *      RXE_ALLOC_UCTX_RESTORE_MODE cannot invoke RESTORE_CQ;
 *      the dispatcher must return -EPERM via the per-driver
 *      ib_device_ops.ucontext_is_restore_mode predicate.
 *   2. Bad comp_vector. comp_vector >= dev->num_comp_vectors must
 *      come back as -EINVAL from the dispatcher's pre-driver guard.
 *      Validates the dispatcher pre-checks before reaching
 *      rxe_restore_cq.
 *   3. Happy path. A ucontext opened WITH the flag can mint a CQ
 *      uobject at the caller-chosen ufile handle TARGET_HANDLE.
 *      Cross-check via UVERBS_METHOD_INFO_HANDLES that the CQ
 *      handle shows up in the CQ list. Assert RESP_CQE is non-zero
 *      (rxe normalizes via roundup_pow_of_two, but exposes the
 *      actual installed count back to userspace just like
 *      CREATE_CQ does).
 *   4. Collision (ufile handle). A second
 *      RESTORE_CQ(target=TARGET_HANDLE) on the same ucontext returns
 *      -EBUSY (xa_insert collision inside
 *      rdma_alloc_begin_uobject_at_handle).
 *   5. COMP_CHANNEL rejected. Allocate a comp_channel via the legacy
 *      IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL write-cmd, pass its fd
 *      as UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL on a fresh
 *      RESTORE_CQ(target=TARGET_HANDLE_2). The dispatcher's v0
 *      forward-compat guard must reject with -EOPNOTSUPP. Locks in
 *      the design choice that the comp_channel attr is declared but
 *      refused at v0 -- any future regression that silently accepts
 *      it would surface here.
 *   6. Destroy round-trip. IB_USER_VERBS_CMD_DESTROY_CQ clears the
 *      CQ handle; INFO_HANDLES no longer reports it. The S5a-on-rxe
 *      half of the v0 dealloc-ordering invariant: rxe restored CQs
 *      destroy cleanly because rxe has no FW graph and no
 *      kernel-side QP children at v0. (mlx5 S5b's analogous subtest
 *      8 will assert the inverse -- BAD_RES_STATE -- since mlx5 FW
 *      tracks cqn in QPCs as a tracked dep; design §10.8.)
 *
 * Run on any host with CONFIG_RDMA_RXE=m. No root needed if the
 * caller is in the rdma group (or /dev/infiniband/uverbsN is
 * world-rw). Tested against rxe0 over loopback.
 *
 * Build:
 *   make -C tools/testing/mlx5_vfmig \
 *        uobject_restore/cq_restore/cq_restore_probe_rxe
 *
 * Usage:
 *   ./cq_restore_probe_rxe [<ibdev>]      # default rxe0
 *
 * Like pd_restore_probe_rxe / mr_restore_probe_rxe, we bypass
 * libibverbs's rxe provider and talk to the uverbs cdev directly so
 * we can pass the RXE_ALLOC_UCTX_RESTORE_MODE flag in udata.
 * libibverbs is only used as a device-discovery convenience to map
 * "<ibdev>" -> "uverbsN".
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
 * Mirrors include/uapi/rdma/rdma_user_rxe.h. See pd_restore_probe_rxe.c
 * for the rationale; the rxe alloc-ucontext UAPI bit is post-
 * headers-install for installed rdma-core.
 */
enum {
	RXE_ALLOC_UCTX_RESTORE_MODE = 1u << 0,
};

struct rxe_alloc_ucontext_req {
	uint32_t flags;
	uint32_t reserved;
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
#define UVERBS_OBJECT_CQ			3
#define UVERBS_OBJECT_RESTORE			18

#define UVERBS_METHOD_INFO_HANDLES		1
#define UVERBS_ATTR_INFO_OBJECT_ID		0
#define UVERBS_ATTR_INFO_TOTAL_HANDLES		1
#define UVERBS_ATTR_INFO_HANDLES_LIST		2

/*
 * UVERBS_METHOD_RESTORE_CQ = 2 (PD=0, MR=1, CQ=2 in
 * enum uverbs_methods_restore).
 */
#define UVERBS_METHOD_RESTORE_CQ		2
enum {
	UVERBS_ATTR_RESTORE_CQ_HANDLE		= 0,
	UVERBS_ATTR_RESTORE_CQ_CQE		= 1,
	UVERBS_ATTR_RESTORE_CQ_USER_HANDLE	= 2,
	UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR	= 3,
	UVERBS_ATTR_RESTORE_CQ_FLAGS		= 4,
	UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL	= 5,
	UVERBS_ATTR_RESTORE_CQ_EVENT_FD		= 6,
	UVERBS_ATTR_RESTORE_CQ_RESP_CQE		= 7,
};

/* Matches enum in include/uapi/rdma/ib_user_ioctl_verbs.h. */
#define RDMA_DRIVER_RXE_LOCAL			14

#define TARGET_HANDLE				0x4242u
#define TARGET_HANDLE_2				0x4243u
#define USER_HANDLE_TAG				0xDEADBEEFCAFEBABEull
#define CQE_REQUESTED				64u

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

/*
 * Allocate a comp channel via the legacy write path. Returns the new
 * fd (which is also the kernel uobject id for the comp_channel object,
 * exactly the value the FD-class RESTORE_CQ_COMP_CHANNEL attr expects)
 * or -errno on failure.
 *
 * Used only by subtest 5: a v0 caller MUST NOT pass a comp_channel to
 * RESTORE_CQ; the dispatcher must reject -EOPNOTSUPP. We allocate one
 * to deliberately try.
 */
static int do_create_comp_channel(int fd)
{
	struct {
		struct ib_uverbs_cmd_hdr		hdr;
		struct ib_uverbs_create_comp_channel	core;
	} __attribute__((packed)) cmd = {};
	struct ib_uverbs_create_comp_channel_resp resp = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= sizeof(resp) / 4;
	cmd.core.response	= (uintptr_t)&resp;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	return (int)resp.fd;
}

static int do_destroy_cq(int fd, uint32_t cq_handle)
{
	struct {
		struct ib_uverbs_cmd_hdr	hdr;
		struct ib_uverbs_destroy_cq	core;
	} __attribute__((packed)) cmd = {};
	struct ib_uverbs_destroy_cq_resp resp = {};
	ssize_t n;

	cmd.hdr.command		= IB_USER_VERBS_CMD_DESTROY_CQ;
	cmd.hdr.in_words	= sizeof(cmd) / 4;
	cmd.hdr.out_words	= sizeof(resp) / 4;
	cmd.core.response	= (uintptr_t)&resp;
	cmd.core.cq_handle	= cq_handle;

	n = write(fd, &cmd, sizeof(cmd));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(cmd))
		return -EIO;
	return 0;
}

/* ----------------------- ioctl helpers ----------------------------------- */

/*
 * RESTORE_CQ ioctl request. core: HANDLE/CQE/USER_HANDLE/COMP_VECTOR
 * mandatory + RESP_CQE mandatory out; FLAGS/COMP_CHANNEL/EVENT_FD
 * optional. We omit FLAGS + EVENT_FD on every call (v0 plugin path),
 * and only include COMP_CHANNEL on the rejection subtest.
 *
 * Wire encoding refresher (uverbs_ioctl.c):
 *   - PTR_IN(__u32):	len = 4, data = inline value
 *   - PTR_IN(__u64):	len = 8, data = inline value
 *   - PTR_OUT(__u32):	len = 4, data = (uintptr_t)&user_buf
 *   - FD/IDR class:	len = 0, data = fd / idr handle
 */
static int do_restore_cq(int fd, uint32_t target_handle, uint32_t cqe,
			 uint64_t user_handle, uint32_t comp_vector,
			 int comp_channel_fd, uint32_t *resp_cqe_out)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[6];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= UVERBS_OBJECT_RESTORE;
	cmd.hdr.method_id	= UVERBS_METHOD_RESTORE_CQ;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_HANDLE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= target_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_CQE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= cqe;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_USER_HANDLE;
	cmd.attrs[n].len	= sizeof(uint64_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= user_handle;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= comp_vector;
	n++;

	if (comp_channel_fd >= 0) {
		cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_COMP_CHANNEL;
		cmd.attrs[n].len	= 0;
		cmd.attrs[n].flags	= 0;
		cmd.attrs[n].data	= (uint32_t)comp_channel_fd;
		n++;
	}

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_RESP_CQE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)resp_cqe_out;
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
			"cq_restore: ibv_get_device_list returned no devices\n");
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
		fprintf(stderr, "cq_restore: ibdev '%s' not found; available:",
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
	uint32_t resp_cqe = 0;
	int fd, ret;
	int fails = 0;

	printf("[1] gate: ucontext WITHOUT restore mode -> RESTORE_CQ must -EPERM\n");

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

	ret = do_restore_cq(fd, TARGET_HANDLE, CQE_REQUESTED,
			    USER_HANDLE_TAG, 0, -1, &resp_cqe);
	if (ret == -EPERM) {
		printf("  PASS RESTORE_CQ on non-restore-mode ucontext -> -EPERM\n");
	} else if (ret == 0) {
		fprintf(stderr,
			"  FAIL RESTORE_CQ on non-restore-mode ucontext succeeded\n"
			"       (security regression: predicate not consulted)\n");
		fails++;
	} else {
		fprintf(stderr,
			"  FAIL RESTORE_CQ on non-restore-mode ucontext -> %s\n"
			"       (expected -EPERM)\n", strerror(-ret));
		fails++;
	}

	close(fd);
	return fails;
}

static int subtest_bad_comp_vector(int fd)
{
	uint32_t resp_cqe = 0;
	int ret;

	printf("[2] comp_vector range: comp_vector=UINT_MAX must -EINVAL\n");

	ret = do_restore_cq(fd, TARGET_HANDLE, CQE_REQUESTED,
			    USER_HANDLE_TAG, UINT32_MAX, -1, &resp_cqe);
	if (ret == -EINVAL) {
		printf("  PASS RESTORE_CQ(comp_vector=UINT_MAX) -> -EINVAL\n");
		return 0;
	}
	fprintf(stderr,
		"  FAIL RESTORE_CQ(comp_vector=UINT_MAX) -> %s (expected -EINVAL;\n"
		"       dispatcher should range-check comp_vector >= num_comp_vectors)\n",
		ret ? strerror(-ret) : "0 (success)");
	return 1;
}

static int subtest_happy_path(int fd)
{
	uint32_t list[16] = {};
	uint32_t total = 0;
	uint32_t resp_cqe = 0;
	int ret;

	printf("[3] happy path: RESTORE_CQ(target=0x%x, cqe=%u, comp_vector=0)\n",
	       TARGET_HANDLE, CQE_REQUESTED);

	ret = do_restore_cq(fd, TARGET_HANDLE, CQE_REQUESTED,
			    USER_HANDLE_TAG, 0, -1, &resp_cqe);
	if (ret) {
		fprintf(stderr,
			"  FAIL RESTORE_CQ(target=0x%x): %s%s\n",
			TARGET_HANDLE, strerror(-ret),
			ret == -EOPNOTSUPP
			? "  (rxe_restore_cq not registered in rxe_dev_ops?)"
			: ret == -EPERM
			? "  (rxe_ucontext_is_restore_mode not reporting true?)"
			: "");
		return 1;
	}
	printf("  PASS RESTORE_CQ(target=0x%x) -> 0  (resp_cqe=%u)\n",
	       TARGET_HANDLE, resp_cqe);

	/*
	 * RESP_CQE must be at least the requested CQE count; rxe
	 * normalises via roundup_pow_of_two inside rxe_cq_chk_attr but
	 * never returns 0. A zero here would mean the dispatcher's
	 * uverbs_copy_to(RESP_CQE) didn't run (UA_MANDATORY violation
	 * would be caught earlier; this is the post-callback wire-up
	 * sanity check, mirroring §9.6's RESP_LKEY check on
	 * mr_restore_probe_mlx5_vfmig).
	 */
	if (resp_cqe == 0) {
		fprintf(stderr,
			"  FAIL RESP_CQE=0; dispatcher's uverbs_copy_to(RESP_CQE)\n"
			"       must populate cq->cqe back to userspace\n");
		return 1;
	}
	if (resp_cqe < CQE_REQUESTED) {
		fprintf(stderr,
			"  FAIL RESP_CQE=%u < requested cqe=%u; rxe_cq_chk_attr\n"
			"       only rounds UP\n",
			resp_cqe, CQE_REQUESTED);
		return 1;
	}
	printf("  PASS resp_cqe=%u >= requested cqe=%u\n",
	       resp_cqe, CQE_REQUESTED);

	ret = do_info_handles(fd, UVERBS_OBJECT_CQ, list, 16, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(CQ): %s\n",
			strerror(-ret));
		return 1;
	}
	if (!handle_present(list, total, TARGET_HANDLE)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(CQ): handle 0x%x not in list (total=%u)\n",
			TARGET_HANDLE, total);
		return 1;
	}
	printf("  PASS INFO_HANDLES(CQ) returned 0x%x among %u entries\n",
	       TARGET_HANDLE, total);
	return 0;
}

static int subtest_collision_handle(int fd)
{
	uint32_t resp_cqe = 0;
	int ret;

	printf("[4] ufile collision: second RESTORE_CQ(target=0x%x) must -EBUSY\n",
	       TARGET_HANDLE);

	ret = do_restore_cq(fd, TARGET_HANDLE, CQE_REQUESTED,
			    USER_HANDLE_TAG, 0, -1, &resp_cqe);
	if (ret == -EBUSY) {
		printf("  PASS second RESTORE_CQ(target=0x%x) -> -EBUSY\n",
		       TARGET_HANDLE);
		return 0;
	}
	fprintf(stderr,
		"  FAIL second RESTORE_CQ(target=0x%x) -> %s (expected -EBUSY)\n",
		TARGET_HANDLE, ret ? strerror(-ret) : "0 (success)");
	return 1;
}

static int subtest_comp_channel_rejected(int fd)
{
	uint32_t resp_cqe = 0;
	int cc_fd, ret;

	printf("[5] forward-compat reject: RESTORE_CQ + COMP_CHANNEL must -EOPNOTSUPP\n");

	cc_fd = do_create_comp_channel(fd);
	if (cc_fd < 0) {
		fprintf(stderr, "  FAIL CREATE_COMP_CHANNEL: %s\n",
			strerror(-cc_fd));
		return 1;
	}

	ret = do_restore_cq(fd, TARGET_HANDLE_2, CQE_REQUESTED,
			    USER_HANDLE_TAG, 0, cc_fd, &resp_cqe);
	if (ret == -EOPNOTSUPP) {
		printf("  PASS RESTORE_CQ(comp_channel=fd) -> -EOPNOTSUPP\n");
		ret = 0;
	} else if (ret == 0) {
		fprintf(stderr,
			"  FAIL RESTORE_CQ(comp_channel=fd) succeeded\n"
			"       (dispatcher's v0 forward-compat guard is disabled?)\n");
		ret = 1;
	} else {
		fprintf(stderr,
			"  FAIL RESTORE_CQ(comp_channel=fd) -> %s (expected -EOPNOTSUPP)\n",
			strerror(-ret));
		ret = 1;
	}

	close(cc_fd);
	return ret;
}

static int subtest_destroy_round_trip(int fd)
{
	uint32_t list[16] = {};
	uint32_t total = 0;
	int ret;

	printf("[6] destroy round-trip: DESTROY_CQ(0x%x) succeeds + handle gone\n",
	       TARGET_HANDLE);

	ret = do_destroy_cq(fd, TARGET_HANDLE);
	if (ret) {
		fprintf(stderr,
			"  FAIL DESTROY_CQ(0x%x): %s\n"
			"       (S5a v0 invariant: rxe restored CQs destroy cleanly\n"
			"       since no QPs are attached and rxe has no FW graph)\n",
			TARGET_HANDLE, strerror(-ret));
		return 1;
	}

	ret = do_info_handles(fd, UVERBS_OBJECT_CQ, list, 16, &total);
	if (ret) {
		fprintf(stderr, "  FAIL INFO_HANDLES(CQ): %s\n",
			strerror(-ret));
		return 1;
	}
	if (handle_present(list, total, TARGET_HANDLE)) {
		fprintf(stderr,
			"  FAIL INFO_HANDLES(CQ) still reports 0x%x after destroy\n",
			TARGET_HANDLE);
		return 1;
	}
	printf("  PASS DESTROY_CQ(0x%x) cleared the restored CQ\n",
	       TARGET_HANDLE);
	return 0;
}

/* ----------------------- main -------------------------------------------- */

int main(int argc, char **argv)
{
	const char *ibdev = argc > 1 ? argv[1] : "rxe0";
	char cdev_path[128];
	struct ib_uverbs_get_context_resp resp = {};
	int fd_restore, ret;
	int fails = 0;

	if (resolve_cdev_path(ibdev, cdev_path, sizeof(cdev_path)) != 0)
		return 2;
	printf("cq_restore: ibdev=%s cdev=%s\n", ibdev, cdev_path);

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

	fails += subtest_bad_comp_vector(fd_restore);
	fails += subtest_happy_path(fd_restore);
	fails += subtest_collision_handle(fd_restore);
	fails += subtest_comp_channel_rejected(fd_restore);
	fails += subtest_destroy_round_trip(fd_restore);

	close(fd_restore);

	if (fails) {
		fprintf(stderr,
			"\ncq_restore_probe_rxe: FAIL (%d subtest failure(s))\n",
			fails);
		return 1;
	}
	printf("\ncq_restore_probe_rxe: PASS\n");
	return 0;
}
