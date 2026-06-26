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
 *   6. NLDEV identity. Walk RDMA_NLDEV_CMD_RES_CQ_GET (the same view
 *      `rdma resource show cq` consumes) and assert that exactly one
 *      entry in the dump carries pid=getpid() AND
 *      RDMA_NLDEV_ATTR_RES_HANDLE=TARGET_HANDLE. INFO_HANDLES already
 *      validates that the uobj is in the ufile xarray under
 *      target_handle (subtest 3); this subtest validates the
 *      complementary path -- that the restrack tree sees the CQ and
 *      reports the same handle. Catches a regression class
 *      INFO_HANDLES does not: forgetting `rdma_restrack_add` in the
 *      restore handler, or any future drift between the xarray and
 *      restrack views of the same CQ. Locks in the K8a claim of
 *      design §7.5.1 for the RESTORE_CQ path.
 *   7. Destroy round-trip. IB_USER_VERBS_CMD_DESTROY_CQ clears the
 *      CQ handle; INFO_HANDLES no longer reports it. The S5a-on-rxe
 *      half of the v0 dealloc-ordering invariant: rxe restored CQs
 *      destroy cleanly because rxe has no FW graph and no
 *      kernel-side QP children at v0. (mlx5 S5b's analogous subtest
 *      8 will assert the inverse -- BAD_RES_STATE -- since mlx5 FW
 *      tracks cqn in QPCs as a tracked dep; design §10.8.)
 *   8. In-flight CQE ring round-trip. RESTORE_CQ ships the ring bytes
 *      + producer/consumer cursors (the QP SQ/RQ image path applied to
 *      the CQ); QUERY_CQ reads them straight back. Asserts the cursors
 *      and the CQE image are byte-identical, and -- via the producer
 *      readback -- that the seed used the TO_CLIENT-direction
 *      rxe_cq_seed_ring (q->index = producer), not rxe_qp_seed_ring
 *      (q->index = consumer), which would clobber slot 0 on the next
 *      rxe_cq_post. This is the kernel half of the CRIU fix for the
 *      lost-CQE live-lock (the ring VMA CRIU never snapshots).
 *

 * Run on any host with CONFIG_RDMA_RXE=m. No root needed if the
 * caller is in the rdma group (or /dev/infiniband/uverbsN is
 * world-rw). Tested against rxe0 over loopback.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/netlink.h>

#include <infiniband/verbs.h>
#include <rdma/ib_user_verbs.h>

/*
 * In-tree kernel UAPI header (rather than rdma-core's installed copy)
 * so we always pick up the full set of NLDEV attribute enumerators
 * regardless of the host's rdma-core version. Same coupling shape as
 * tools/testing/criu_rdma/uobject_restore/nldev_res_handle/...
 */
#include "../../../../../include/uapi/rdma/rdma_netlink.h"

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

/*
 * UA_UHW()'s output half. UVERBS_ID_DRIVER_NS_SHIFT is 12 (1<<12 = 4096)
 * and UVERBS_ATTR_UHW_OUT = UVERBS_ID_DRIVER_NS + 1; mirrored from
 * include/uapi/rdma/ib_user_ioctl_cmds.h. Same constant the existing
 * pd_restore_probe_mlx5_vfmig and mr_restore_probe_mlx5_vfmig probes
 * inline for the same reason -- the headers-install rdma uapi layout
 * doesn't expose this through libibverbs.
 */
#define UVERBS_ATTR_UHW_OUT			((uint16_t)4097)
#define UVERBS_ATTR_UHW_IN			((uint16_t)4096)

#define UVERBS_ID_NS_SHIFT			12
#define UVERBS_ID_DRIVER_NS			(1u << UVERBS_ID_NS_SHIFT)

/* Mirror of include/uapi/rdma/rxe_user_ioctl_cmds.h (driver-private). */
#define RXE_IB_OBJECT_MIGRATE			(UVERBS_ID_DRIVER_NS + 0u)
#define RXE_IB_METHOD_QUERY_CQ			((1u << UVERBS_ID_NS_SHIFT) + 2u)
#define RXE_IB_ATTR_QUERY_CQ_HANDLE		(1u << UVERBS_ID_NS_SHIFT)
#define RXE_IB_ATTR_QUERY_CQ_RESP_BLOB		((1u << UVERBS_ID_NS_SHIFT) + 1u)
#define RXE_IB_ATTR_QUERY_CQ_RESP_CQE_IMAGE	((1u << UVERBS_ID_NS_SHIFT) + 2u)

/* Mirror of include/uapi/rdma/rdma_user_rxe.h struct rxe_query_cq_resp. */
struct rxe_query_cq_resp_local {
	uint64_t	vm_pgoff;
	uint32_t	cqe;
	uint32_t	producer;
	uint32_t	consumer;
	uint32_t	cqe_image_bytes;
	uint32_t	reserved[2];
};

/* Mirror of include/uapi/rdma/rdma_user_rxe.h struct rxe_restore_cq_req. */
struct rxe_restore_cq_req_local {
	uint64_t	vm_pgoff;
	uint32_t	producer;
	uint32_t	consumer;
	uint32_t	cqe_image_bytes;
	uint32_t	reserved;
};

/*
 * Mirrors include/uapi/rdma/rdma_user_rxe.h's struct rxe_create_cq_resp
 * (one struct mminfo: __aligned_u64 offset, __u32 size, __u32 pad).
 * rxe_restore_cq insists on udata->outlen >= sizeof(rxe_create_cq_resp)
 * and writes the kernel-allocated CQ ring's mmap offset/size into it.
 * The probe never actually mmaps the resulting buffer (subtest 7
 * tears it down via DESTROY_CQ before it would matter); we just need
 * a buffer of the right size to satisfy rxe's pre-check.
 */
struct rxe_mminfo_local {
	uint64_t	offset;
	uint32_t	size;
	uint32_t	pad;
};

struct rxe_create_cq_resp_local {
	struct rxe_mminfo_local mi;
};

/* Matches enum in include/uapi/rdma/ib_user_ioctl_verbs.h. */
#define RDMA_DRIVER_RXE_LOCAL			14

#define TARGET_HANDLE				0x4242u
#define TARGET_HANDLE_2				0x4243u
#define TARGET_HANDLE_3				0x4244u
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
 * and only include COMP_CHANNEL on the rejection subtest. We always
 * attach a UHW_OUT buffer because rxe_restore_cq insists on
 * udata->outlen >= sizeof(struct rxe_create_cq_resp); the buffer's
 * contents are written by the kernel but ignored by the probe.
 *
 * Wire encoding refresher (uverbs_ioctl.c):
 *   - PTR_IN(__u32):	len = 4, data = inline value
 *   - PTR_IN(__u64):	len = 8, data = inline value
 *   - PTR_OUT(__u32):	len = 4, data = (uintptr_t)&user_buf
 *   - FD/IDR class:	len = 0, data = fd / idr handle
 *   - UHW_IN/UHW_OUT:	len = sizeof(buf), data = (uintptr_t)&buf
 */
static int do_restore_cq(int fd, uint32_t target_handle, uint32_t cqe,
			 uint64_t user_handle, uint32_t comp_vector,
			 int comp_channel_fd, uint32_t *resp_cqe_out)
{
	struct rxe_create_cq_resp_local uhw_out = {};
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[7];
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

	/*
	 * UHW_OUT: rxe_restore_cq's mandatory uresp-buffer-size check.
	 * Optional in the dispatcher schema (UA_UHW), so flags = 0;
	 * we provide it unconditionally because the dispatcher passes
	 * the length straight through to rxe via attrs->driver_udata
	 * and rxe rejects with -EINVAL if outlen < sizeof(uresp).
	 */
	cmd.attrs[n].attr_id	= UVERBS_ATTR_UHW_OUT;
	cmd.attrs[n].len	= sizeof(uhw_out);
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)&uhw_out;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	if (ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return -errno;
	return 0;
}

/*
 * RESTORE_CQ with an in-flight CQE ring image. Same core attrs as
 * do_restore_cq, plus a UHW_IN tail carrying [rxe_restore_cq_req][image].
 * @vm_pgoff is left 0 (monotonic fallback) -- the round-trip checks
 * cursors + ring bytes via QUERY_CQ and never mmaps, so the forced offset
 * is irrelevant here. @image may be NULL/@image_len 0 for the cursor-only
 * (empty-CQ) variant.
 */
static int do_restore_cq_inflight(int fd, uint32_t target_handle, uint32_t cqe,
				  uint32_t producer, uint32_t consumer,
				  const void *image, uint32_t image_len,
				  uint32_t *resp_cqe_out)
{
	struct rxe_create_cq_resp_local uhw_out = {};
	struct rxe_restore_cq_req_local *req;
	uint8_t *inbuf;
	size_t inlen = sizeof(*req) + image_len;
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[7];
	} cmd = {};
	unsigned int n = 0;
	int ret;

	inbuf = calloc(1, inlen);
	if (!inbuf)
		return -ENOMEM;
	req = (struct rxe_restore_cq_req_local *)inbuf;
	req->producer        = producer;
	req->consumer        = consumer;
	req->cqe_image_bytes = image_len;
	if (image && image_len)
		memcpy(inbuf + sizeof(*req), image, image_len);

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
	cmd.attrs[n].data	= USER_HANDLE_TAG;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_COMP_VECTOR;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= 0;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_RESTORE_CQ_RESP_CQE;
	cmd.attrs[n].len	= sizeof(uint32_t);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)resp_cqe_out;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_UHW_IN;
	cmd.attrs[n].len	= (uint16_t)inlen;
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)inbuf;
	n++;

	cmd.attrs[n].attr_id	= UVERBS_ATTR_UHW_OUT;
	cmd.attrs[n].len	= sizeof(uhw_out);
	cmd.attrs[n].flags	= 0;
	cmd.attrs[n].data	= (uintptr_t)&uhw_out;
	n++;

	cmd.hdr.num_attrs = n;
	cmd.hdr.length = sizeof(cmd.hdr) + n * sizeof(cmd.attrs[0]);

	ret = ioctl(fd, RDMA_VERBS_IOCTL, &cmd) < 0 ? -errno : 0;
	free(inbuf);
	return ret;
}

/*
 * QUERY_CQ (driver-private RXE_IB_OBJECT_MIGRATE). Reads the blob and, when
 * @image_out is provided, the CQE ring image into it (capacity @image_cap).
 */
static int do_query_cq(int fd, uint32_t cq_handle,
		       struct rxe_query_cq_resp_local *blob_out,
		       void *image_out, uint32_t image_cap)
{
	struct {
		struct ib_uverbs_ioctl_hdr	hdr;
		struct ib_uverbs_attr		attrs[3];
	} cmd = {};
	unsigned int n = 0;

	cmd.hdr.object_id	= RXE_IB_OBJECT_MIGRATE;
	cmd.hdr.method_id	= RXE_IB_METHOD_QUERY_CQ;
	cmd.hdr.driver_id	= RDMA_DRIVER_RXE_LOCAL;

	cmd.attrs[n].attr_id	= RXE_IB_ATTR_QUERY_CQ_HANDLE;
	cmd.attrs[n].len	= 0;
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= cq_handle;
	n++;

	cmd.attrs[n].attr_id	= RXE_IB_ATTR_QUERY_CQ_RESP_BLOB;
	cmd.attrs[n].len	= sizeof(*blob_out);
	cmd.attrs[n].flags	= UVERBS_ATTR_F_MANDATORY;
	cmd.attrs[n].data	= (uintptr_t)blob_out;
	n++;

	if (image_out && image_cap) {
		cmd.attrs[n].attr_id	= RXE_IB_ATTR_QUERY_CQ_RESP_CQE_IMAGE;
		cmd.attrs[n].len	= (uint16_t)image_cap;
		cmd.attrs[n].flags	= 0;
		cmd.attrs[n].data	= (uintptr_t)image_out;
		n++;
	}

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

/* ----------------------- NETLINK_RDMA helpers ---------------------------- *
 *
 * Specialised slice of the helpers in
 * tools/testing/criu_rdma/uobject_restore/nldev_res_handle/nldev_res_handle_probe.c
 * for the single (ibdev -> dev_index, dump RES_CQ -> find pid+handle)
 * lookup subtest 6 needs. Kept inline here rather than refactored into a
 * shared header because (a) the netlink surface is small enough that
 * duplication beats build-system coupling for probe binaries, and (b)
 * the two probes have different walk semantics (this one is
 * point-lookup, the other is full-validation across all classes).
 */

#define NL_BUFSZ		(64 * 1024)

#ifndef NLA_TYPE_MASK
#define NLA_TYPE_MASK    (~(NLA_F_NESTED | NLA_F_NET_BYTEORDER))
#endif

#define NLA_OK(p, rem)   ((rem) >= (int)sizeof(struct nlattr) &&         \
			  (p)->nla_len >= sizeof(struct nlattr) &&       \
			  (p)->nla_len <= (rem))
#define NLA_NEXT(p, rem) ((rem) -= NLA_ALIGN((p)->nla_len),              \
			  (struct nlattr *)((char *)(p) + NLA_ALIGN((p)->nla_len)))
#define NLA_DATA(p)      ((void *)((char *)(p) + NLA_HDRLEN))
#define NLA_PAYLOAD(p)   ((p)->nla_len - NLA_HDRLEN)

static int nl_open(void)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	int sk;

	sk = socket(AF_NETLINK, SOCK_RAW, NETLINK_RDMA);
	if (sk < 0) {
		perror("socket(NETLINK_RDMA)");
		return -1;
	}
	if (bind(sk, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind(NETLINK_RDMA)");
		close(sk);
		return -1;
	}
	return sk;
}

static struct nlattr *nla_find(void *buf, int len, uint16_t type)
{
	struct nlattr *nla;

	for (nla = buf; NLA_OK(nla, len); nla = NLA_NEXT(nla, len))
		if ((nla->nla_type & NLA_TYPE_MASK) == type)
			return nla;
	return NULL;
}

/*
 * Send a NETLINK_RDMA dump request. RDMA_NLDEV_CMD_RES_CQ_GET requires
 * RDMA_NLDEV_ATTR_DEV_INDEX in the request (kernel rejects with -EINVAL
 * via res_get_common_dumpit otherwise). The plain RDMA_NLDEV_CMD_GET
 * dump used to resolve ibdev_name -> dev_index does NOT take
 * DEV_INDEX (with_devix=false enumerates every device).
 */
static int nl_send_dump(int sk, uint16_t nlmsg_type, bool with_devix,
			uint32_t dev_index)
{
	struct {
		struct nlmsghdr  hdr;
		struct nlattr    devix_hdr;
		uint32_t         devix_val;
	} req = {0};
	struct iovec iov = { .iov_base = &req };
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct msghdr msg = {
		.msg_name = &sa,
		.msg_namelen = sizeof(sa),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};

	if (with_devix) {
		req.hdr.nlmsg_len  = NLMSG_HDRLEN +
				     NLA_HDRLEN + sizeof(uint32_t);
		req.devix_hdr.nla_len  = NLA_HDRLEN + sizeof(uint32_t);
		req.devix_hdr.nla_type = RDMA_NLDEV_ATTR_DEV_INDEX;
		req.devix_val          = dev_index;
		iov.iov_len = req.hdr.nlmsg_len;
	} else {
		req.hdr.nlmsg_len = NLMSG_HDRLEN;
		iov.iov_len = NLMSG_HDRLEN;
	}
	req.hdr.nlmsg_type  = nlmsg_type;
	req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.hdr.nlmsg_seq   = 1;

	if (sendmsg(sk, &msg, 0) < 0) {
		perror("sendmsg(NETLINK_RDMA dump)");
		return -1;
	}
	return 0;
}

/*
 * Walk RDMA_NLDEV_CMD_GET to translate ibdev_name -> kernel dev_index.
 * Returns 0 on hit (with *out populated), -1 on transport / parse error
 * or if the name was not in the dump.
 *
 * NB: dev_index 0 IS valid (xa_limit_31b starts at 0); never overload
 * 0 as "not found".
 */
static int nl_resolve_ibdev_index(const char *ibdev, uint32_t *out)
{
	char buf[NL_BUFSZ];
	bool done = false;
	int sk = nl_open();

	if (sk < 0)
		return -1;
	if (nl_send_dump(sk, RDMA_NL_GET_TYPE(RDMA_NL_NLDEV,
					      RDMA_NLDEV_CMD_GET),
			 false, 0)) {
		close(sk);
		return -1;
	}

	while (!done) {
		ssize_t n = recv(sk, buf, sizeof(buf), 0);
		struct nlmsghdr *nh;

		if (n < 0) {
			perror("recv(NLDEV_CMD_GET dump)");
			close(sk);
			return -1;
		}
		for (nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, n);
		     nh = NLMSG_NEXT(nh, n)) {
			void *payload;
			int   len;
			struct nlattr *name_attr;
			struct nlattr *idx_attr;

			if (nh->nlmsg_type == NLMSG_DONE) {
				done = true;
				break;
			}
			if (nh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = NLMSG_DATA(nh);

				fprintf(stderr,
					"NLMSG_ERROR resolving %s: %d (%s)\n",
					ibdev, e->error, strerror(-e->error));
				close(sk);
				return -1;
			}
			payload = NLMSG_DATA(nh);
			len     = nh->nlmsg_len - NLMSG_HDRLEN;
			name_attr = nla_find(payload, len,
					     RDMA_NLDEV_ATTR_DEV_NAME);
			idx_attr  = nla_find(payload, len,
					     RDMA_NLDEV_ATTR_DEV_INDEX);
			if (!name_attr || !idx_attr)
				continue;
			if (strncmp(NLA_DATA(name_attr), ibdev,
				    NLA_PAYLOAD(name_attr)) != 0)
				continue;
			*out = *(uint32_t *)NLA_DATA(idx_attr);
			close(sk);
			return 0;
		}
	}
	close(sk);
	fprintf(stderr, "ibdev %s not found in NLDEV_CMD_GET dump\n", ibdev);
	return -1;
}

/*
 * Dump RDMA_NLDEV_CMD_RES_CQ_GET for @dev_index, look for an entry
 * with RES_PID == @want_pid and RES_HANDLE == @want_handle. On hit
 * sets *@found_out=true and returns the matching RES_CQN in
 * *@cqn_out (for diagnostic logging only). Returns 0 on success
 * (transport-level), -1 on netlink failure.
 */
static int nl_find_cq_handle(uint32_t dev_index, uint32_t want_pid,
			     uint32_t want_handle, bool *found_out,
			     uint32_t *cqn_out)
{
	char buf[NL_BUFSZ];
	bool done = false;
	int sk = nl_open();

	if (sk < 0)
		return -1;
	if (nl_send_dump(sk, RDMA_NL_GET_TYPE(RDMA_NL_NLDEV,
					      RDMA_NLDEV_CMD_RES_CQ_GET),
			 true, dev_index)) {
		close(sk);
		return -1;
	}

	*found_out = false;
	*cqn_out   = 0;

	while (!done) {
		ssize_t n = recv(sk, buf, sizeof(buf), 0);
		struct nlmsghdr *nh;

		if (n < 0) {
			perror("recv(NLDEV RES_CQ dump)");
			close(sk);
			return -1;
		}
		for (nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, n);
		     nh = NLMSG_NEXT(nh, n)) {
			void *payload;
			int   len;
			struct nlattr *table;
			struct nlattr *nla;
			int rem;

			if (nh->nlmsg_type == NLMSG_DONE) {
				done = true;
				break;
			}
			if (nh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = NLMSG_DATA(nh);

				fprintf(stderr,
					"NLMSG_ERROR (NLDEV RES_CQ dump): %d (%s)\n",
					e->error, strerror(-e->error));
				close(sk);
				return -1;
			}
			payload = NLMSG_DATA(nh);
			len     = nh->nlmsg_len - NLMSG_HDRLEN;

			table = nla_find(payload, len,
					 RDMA_NLDEV_ATTR_RES_CQ);
			if (!table)
				continue;

			rem = NLA_PAYLOAD(table);
			for (nla = NLA_DATA(table); NLA_OK(nla, rem);
			     nla = NLA_NEXT(nla, rem)) {
				bool h_seen = false, p_seen = false;
				uint32_t handle = 0, pid = 0, cqn = 0;
				int erem;
				struct nlattr *e;

				if ((nla->nla_type & NLA_TYPE_MASK) !=
				    RDMA_NLDEV_ATTR_RES_CQ_ENTRY)
					continue;

				erem = NLA_PAYLOAD(nla);
				for (e = NLA_DATA(nla); NLA_OK(e, erem);
				     e = NLA_NEXT(e, erem)) {
					uint16_t t = e->nla_type & NLA_TYPE_MASK;

					if (t == RDMA_NLDEV_ATTR_RES_HANDLE &&
					    NLA_PAYLOAD(e) >= 4) {
						h_seen = true;
						handle = *(uint32_t *)NLA_DATA(e);
					} else if (t == RDMA_NLDEV_ATTR_RES_PID &&
						   NLA_PAYLOAD(e) >= 4) {
						p_seen = true;
						pid = *(uint32_t *)NLA_DATA(e);
					} else if (t == RDMA_NLDEV_ATTR_RES_CQN &&
						   NLA_PAYLOAD(e) >= 4) {
						cqn = *(uint32_t *)NLA_DATA(e);
					}
				}

				if (h_seen && p_seen &&
				    pid == want_pid &&
				    handle == want_handle) {
					*found_out = true;
					*cqn_out   = cqn;
				}
			}
		}
	}
	close(sk);
	return 0;
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

static int subtest_nldev_handle_match(const char *ibdev)
{
	uint32_t dev_index = 0;
	uint32_t cqn = 0;
	bool found = false;
	uint32_t self_pid = (uint32_t)getpid();
	int ret;

	printf("[6] nldev identity: RES_CQ dump must report pid=%u handle=0x%x\n",
	       self_pid, TARGET_HANDLE);

	if (nl_resolve_ibdev_index(ibdev, &dev_index) != 0) {
		fprintf(stderr, "  FAIL nl_resolve_ibdev_index(%s)\n", ibdev);
		return 1;
	}

	ret = nl_find_cq_handle(dev_index, self_pid, TARGET_HANDLE, &found,
				&cqn);
	if (ret) {
		fprintf(stderr, "  FAIL NLDEV RES_CQ dump (transport)\n");
		return 1;
	}

	if (!found) {
		fprintf(stderr,
			"  FAIL NLDEV RES_CQ dump: no entry with pid=%u and\n"
			"       RES_HANDLE=0x%x. Possible regressions:\n"
			"       - rdma_restrack_add() not called in restore_cq\n"
			"         dispatcher path (CQ missing from restrack)\n"
			"       - cq->uobject not pointing at the restored uobj\n"
			"         (RES_HANDLE read site in fill_res_cq_entry)\n"
			"       - rdma_alloc_begin_uobject_at_handle() fell back\n"
			"         to a fresh id instead of honoring target_handle\n",
			self_pid, TARGET_HANDLE);
		return 1;
	}

	printf("  PASS NLDEV RES_CQ entry pid=%u res_cqn=%u handle=0x%x\n",
	       self_pid, cqn, TARGET_HANDLE);
	return 0;
}

static int subtest_destroy_round_trip(int fd)
{
	uint32_t list[16] = {};
	uint32_t total = 0;
	int ret;

	printf("[7] destroy round-trip: DESTROY_CQ(0x%x) succeeds + handle gone\n",
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

/*
 * [8] In-flight CQE ring round-trip. RESTORE_CQ ships the ring bytes +
 * cursors (the QP SQ/RQ image path, applied to the CQ), QUERY_CQ reads them
 * straight back. We validate the new kernel paths end to end:
 *   - QUERY_CQ emits producer/consumer/cqe_image_bytes + the CQE image;
 *   - RESTORE_CQ blits the image (rxe_restore_cq_inflight) and seeds the
 *     cursors via the TO_CLIENT-direction rxe_cq_seed_ring;
 *   - the producer readback proves q->index was seeded from the *producer*
 *     (not the consumer) -- the exact clobber bug that reusing
 *     rxe_qp_seed_ring would introduce.
 * A full "next rxe_cq_post lands at slot==producer" check needs a live QP
 * datapath (out of scope for a raw-cdev probe); the producer readback is the
 * proxy, since q->index==producer is precisely what makes the next post land.
 */
static int subtest_inflight_round_trip(int fd)
{
	struct rxe_query_cq_resp_local blob = {};
	const uint32_t producer = 5, consumer = 2;
	uint32_t resp_cqe = 0;
	uint32_t image_bytes;
	uint8_t *src = NULL, *dst = NULL;
	int ret, fails = 0;

	printf("[8] in-flight round-trip: RESTORE_CQ(image+cursors) -> QUERY_CQ byte-identical\n");

	/*
	 * Learn the authoritative ring geometry from a freshly-created CQ:
	 * the kernel rounds cqe / elem_size, so cqe_image_bytes can't be
	 * computed here.
	 */
	ret = do_restore_cq(fd, TARGET_HANDLE_3, CQE_REQUESTED, USER_HANDLE_TAG,
			    0, -1, &resp_cqe);
	if (ret) {
		fprintf(stderr, "  FAIL seed RESTORE_CQ(0x%x): %s\n",
			TARGET_HANDLE_3, strerror(-ret));
		return 1;
	}
	ret = do_query_cq(fd, TARGET_HANDLE_3, &blob, NULL, 0);
	if (ret) {
		fprintf(stderr, "  FAIL QUERY_CQ(0x%x): %s%s\n", TARGET_HANDLE_3,
			strerror(-ret),
			ret == -EOPNOTSUPP ? "  (QUERY_CQ not registered?)" : "");
		do_destroy_cq(fd, TARGET_HANDLE_3);
		return 1;
	}
	image_bytes = blob.cqe_image_bytes;
	if (image_bytes == 0) {
		fprintf(stderr, "  FAIL QUERY_CQ reported cqe_image_bytes=0\n");
		do_destroy_cq(fd, TARGET_HANDLE_3);
		return 1;
	}
	if (blob.producer != 0 || blob.consumer != 0) {
		fprintf(stderr,
			"  FAIL fresh CQ cursors not zero (prod=%u cons=%u)\n",
			blob.producer, blob.consumer);
		fails++;
	}
	printf("  PASS QUERY_CQ(fresh) cqe=%u image_bytes=%u prod=0 cons=0\n",
	       blob.cqe, image_bytes);

	/* Re-mint at the same handle with a synthetic image + cursors. */
	(void)do_destroy_cq(fd, TARGET_HANDLE_3);

	src = malloc(image_bytes);
	dst = malloc(image_bytes);
	if (!src || !dst) {
		fprintf(stderr, "  FAIL malloc image buffers\n");
		free(src);
		free(dst);
		return 1;
	}
	for (uint32_t i = 0; i < image_bytes; i++)
		src[i] = (uint8_t)(i * 7u + 0x11u);

	ret = do_restore_cq_inflight(fd, TARGET_HANDLE_3, CQE_REQUESTED,
				     producer, consumer, src, image_bytes,
				     &resp_cqe);
	if (ret) {
		fprintf(stderr, "  FAIL RESTORE_CQ(image): %s\n", strerror(-ret));
		free(src);
		free(dst);
		return 1;
	}

	memset(&blob, 0, sizeof(blob));
	ret = do_query_cq(fd, TARGET_HANDLE_3, &blob, dst, image_bytes);
	if (ret) {
		fprintf(stderr, "  FAIL QUERY_CQ(restored): %s\n", strerror(-ret));
		do_destroy_cq(fd, TARGET_HANDLE_3);
		free(src);
		free(dst);
		return 1;
	}

	if (blob.producer != producer || blob.consumer != consumer) {
		fprintf(stderr,
			"  FAIL cursor mismatch: got prod=%u cons=%u want prod=%u cons=%u\n"
			"       (producer readback proves q->index seeded from producer;\n"
			"       reusing rxe_qp_seed_ring would report producer=%u)\n",
			blob.producer, blob.consumer, producer, consumer, consumer);
		fails++;
	} else {
		printf("  PASS cursors round-tripped: prod=%u cons=%u\n",
		       blob.producer, blob.consumer);
	}

	if (blob.cqe_image_bytes != image_bytes) {
		fprintf(stderr, "  FAIL image_bytes mismatch: got %u want %u\n",
			blob.cqe_image_bytes, image_bytes);
		fails++;
	} else if (memcmp(src, dst, image_bytes) != 0) {
		uint32_t i;

		for (i = 0; i < image_bytes && src[i] == dst[i]; i++)
			;
		fprintf(stderr,
			"  FAIL CQE ring image differs at byte %u (src=0x%02x dst=0x%02x)\n",
			i, src[i], dst[i]);
		fails++;
	} else {
		printf("  PASS CQE ring image byte-identical (%u bytes)\n",
		       image_bytes);
	}

	(void)do_destroy_cq(fd, TARGET_HANDLE_3);
	free(src);
	free(dst);
	return fails;
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
	fails += subtest_nldev_handle_match(ibdev);
	fails += subtest_destroy_round_trip(fd_restore);
	fails += subtest_inflight_round_trip(fd_restore);

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
