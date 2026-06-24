// SPDX-License-Identifier: GPL-2.0
/*
 * vf_uuid_probe_mlx5_vfmig -- exercises the kernel matrix for KS7.3
 * (orchestrator-owned per-VF UUID). See
 * tools/testing/criu_rdma/design/vf_prerestore_split.md §3.5.
 *
 * The probe is a single-process matrix runner that opens
 * /dev/mlx5_vfmig/<pf_bdf> and walks every cell of the kernel-side
 * SET_VF_UUID + QUERY_VF contract that does NOT require an SR-IOV
 * teardown / re-enable cycle. The teardown-related lifecycle cells
 * (clear-on-sriov_numvfs=0, stamp-after-recycle) live in the
 * accompanying shell wrapper because they need sysfs writes that
 * are out of scope for an in-process probe.
 *
 * Subtests
 *   1. happy_set   -- SET non-zero UUID U  expect 0;        QUERY -> U
 *   2. idempotent  -- SET same U           expect 0;        QUERY -> U
 *   3. ebusy       -- SET different UUID V expect -EBUSY;   QUERY -> U
 *                                                           (unchanged)
 *   4. einval_zero -- SET all-zeros UUID   expect -EINVAL;  QUERY -> U
 *                                                           (unchanged)
 *   5. einval_oob  -- SET on vf_id == num_vfs    expect -EINVAL
 *   6. erange_q    -- QUERY on vf_id == num_vfs  expect -ERANGE,
 *                                                UUID returned all-zeros
 *
 * Side effects
 *   The probe stamps a non-zero UUID on the supplied @vf_id. There is
 *   no "unset" path in the kernel surface (by design), so the slot
 *   keeps its UUID until SR-IOV teardown (sriov_numvfs=0). Pick a
 *   vf_id whose slot you can recycle, or call this on a slot the
 *   orchestrator was about to stamp anyway. The shell wrapper
 *   automates the recycle for end-to-end runs.
 *
 * Build (in-tree):
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/vf_uuid/vf_uuid_probe_mlx5_vfmig
 *
 * Usage:
 *   vf_uuid_probe_mlx5_vfmig <pf_bdf> <vf_id>
 *
 *   <pf_bdf>  PF BDF, e.g. 0000:08:00.0. Cdev resolved to
 *             /dev/mlx5_vfmig/<pf_bdf>.
 *   <vf_id>   VF index on that PF; must be in [0, num_vfs). The
 *             slot's vf_uuid will be left stamped at exit on
 *             success (cleared by sriov_numvfs=0 cycle).
 *
 * Exit codes:
 *   0   all subtests PASSed
 *   1   setup / usage error (bad args, open() failed, etc.)
 *   2   at least one subtest FAILed; details on stderr
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../../../../../include/uapi/linux/mlx5_vfmig.h"

/*
 * UUID picked by fair dice roll. Deliberately not RFC-4122 random so
 * a probe re-run on the same slot (idempotent on first pass) and a
 * pre-existing matching stamp from a prior probe both look like
 * "same UUID" rather than the EBUSY-on-stale-stamp diagnostic. The
 * shell wrapper picks a fresh UUID per run for the multi-VF cases.
 */
static const unsigned char UUID_U[16] = {
	0x5e, 0xdc, 0x7d, 0x3e, 0x7e, 0x44, 0x4d, 0x8a,
	0xb8, 0xa8, 0x50, 0xf7, 0xc8, 0xa0, 0xc3, 0xb1,
};
static const unsigned char UUID_V[16] = {
	0x10, 0x91, 0xb6, 0xa6, 0xb1, 0xa3, 0x4f, 0xee,
	0x9a, 0xb1, 0xc4, 0x66, 0x88, 0x2e, 0x1d, 0x55,
};
static const unsigned char UUID_ZERO[16] = {};

static int n_pass;
static int n_fail;

static void format_uuid(const unsigned char in[16], char *buf)
{
	snprintf(buf, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 in[0], in[1], in[2], in[3],
		 in[4], in[5],
		 in[6], in[7],
		 in[8], in[9],
		 in[10], in[11], in[12], in[13], in[14], in[15]);
}

static void pass(const char *name, const char *detail)
{
	n_pass++;
	if (detail && detail[0])
		printf("PASS %-14s %s\n", name, detail);
	else
		printf("PASS %s\n", name);
}

static void fail(const char *name, const char *fmt, ...)
{
	va_list ap;

	n_fail++;
	fprintf(stderr, "FAIL %-14s ", name);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/*
 * Wrapper around the SET_VF_UUID ioctl that captures the kernel's
 * decision (0 / -errno) without aborting. The probe always wants the
 * kernel's verdict on a cell so we can compare against the expected
 * one; this function never propagates errno to the caller.
 */
static int set_uuid(int fd, unsigned int vf_id, const unsigned char uuid[16])
{
	struct mlx5_vfmig_set_vf_uuid arg = { .vf_id = vf_id };

	memcpy(arg.vf_uuid, uuid, sizeof(arg.vf_uuid));
	if (ioctl(fd, MLX5_VFMIG_IOC_SET_VF_UUID, &arg) == 0)
		return 0;
	return -errno;
}

/*
 * Wrapper around QUERY_VF that returns 0 / -errno and copies the
 * struct out so the caller can inspect every output field even on
 * the -ERANGE path (the kernel populates the struct even on out-of-
 * range, so callers can read num_vfs without re-issuing).
 */
static int query_vf(int fd, unsigned int vf_id, struct mlx5_vfmig_query_vf *out)
{
	struct mlx5_vfmig_query_vf arg = { .vf_id = vf_id };
	int err;

	err = ioctl(fd, MLX5_VFMIG_IOC_QUERY_VF, &arg);
	*out = arg;
	if (err == 0)
		return 0;
	return -errno;
}

static int discover_num_vfs(int fd)
{
	struct mlx5_vfmig_query_vf info;
	int err;

	err = query_vf(fd, 0, &info);
	if (err && err != -ERANGE) {
		fprintf(stderr, "QUERY_VF vf 0: %s\n", strerror(-err));
		return err;
	}
	return (int)info.num_vfs;
}

/* 1. happy_set: SET U on a known-clear slot (or one already stamped with U). */
static void test_happy_set(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_query_vf info;
	int err;

	err = set_uuid(fd, vf_id, UUID_U);
	if (err == -EBUSY) {
		/*
		 * Probe re-run: slot already carries a previous stamp.
		 * The kernel's collision policy is "same value -> 0",
		 * "different value -> -EBUSY". So an EBUSY here means a
		 * previous probe (or another tenant) stamped a UUID
		 * different from ours; that's a real collision the user
		 * needs to resolve via sriov_numvfs=0 cycle.
		 */
		fail("happy_set",
		     "SET vf %u: -EBUSY -- the slot carries a different UUID; sriov_numvfs=0 + sriov_numvfs=N to clear, then re-run.",
		     vf_id);
		return;
	}
	if (err) {
		fail("happy_set", "SET vf %u: %s (expected 0)",
		     vf_id, strerror(-err));
		return;
	}
	err = query_vf(fd, vf_id, &info);
	if (err) {
		fail("happy_set", "QUERY vf %u: %s (expected 0)",
		     vf_id, strerror(-err));
		return;
	}
	if (memcmp(info.vf_uuid, UUID_U, sizeof(UUID_U)) != 0) {
		char got[37], want[37];

		format_uuid(info.vf_uuid, got);
		format_uuid(UUID_U, want);
		fail("happy_set", "QUERY vf %u: vf_uuid=%s (expected %s)",
		     vf_id, got, want);
		return;
	}
	pass("happy_set", "");
}

/* 2. idempotent: re-stamp same U on the same slot. */
static void test_idempotent(int fd, unsigned int vf_id)
{
	int err = set_uuid(fd, vf_id, UUID_U);

	if (err) {
		fail("idempotent", "SET vf %u (same U): %s (expected 0)",
		     vf_id, strerror(-err));
		return;
	}
	pass("idempotent", "");
}

/* 3. ebusy: SET a different UUID V -- expect -EBUSY, slot value unchanged. */
static void test_ebusy(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_query_vf info;
	int err;

	err = set_uuid(fd, vf_id, UUID_V);
	if (err != -EBUSY) {
		fail("ebusy", "SET vf %u (V): %s (expected -EBUSY)",
		     vf_id, err ? strerror(-err) : "0");
		return;
	}
	err = query_vf(fd, vf_id, &info);
	if (err) {
		fail("ebusy", "QUERY vf %u: %s",
		     vf_id, strerror(-err));
		return;
	}
	if (memcmp(info.vf_uuid, UUID_U, sizeof(UUID_U)) != 0) {
		char got[37];

		format_uuid(info.vf_uuid, got);
		fail("ebusy", "QUERY vf %u: vf_uuid=%s (expected unchanged U)",
		     vf_id, got);
		return;
	}
	pass("ebusy", "");
}

/* 4. einval_zero: SET all-zeros -- expect -EINVAL, slot value unchanged. */
static void test_einval_zero(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_query_vf info;
	int err;

	err = set_uuid(fd, vf_id, UUID_ZERO);
	if (err != -EINVAL) {
		fail("einval_zero",
		     "SET vf %u (zero): %s (expected -EINVAL)",
		     vf_id, err ? strerror(-err) : "0");
		return;
	}
	err = query_vf(fd, vf_id, &info);
	if (err) {
		fail("einval_zero", "QUERY vf %u: %s",
		     vf_id, strerror(-err));
		return;
	}
	if (memcmp(info.vf_uuid, UUID_U, sizeof(UUID_U)) != 0) {
		fail("einval_zero",
		     "QUERY vf %u: vf_uuid changed despite -EINVAL",
		     vf_id);
		return;
	}
	pass("einval_zero", "");
}

/* 5. einval_oob: SET on vf_id == num_vfs -- expect -EINVAL (out of range). */
static void test_einval_oob(int fd, unsigned int num_vfs)
{
	int err = set_uuid(fd, num_vfs, UUID_U);

	if (err != -EINVAL) {
		fail("einval_oob",
		     "SET vf %u (oob): %s (expected -EINVAL)",
		     num_vfs, err ? strerror(-err) : "0");
		return;
	}
	pass("einval_oob", "");
}

/*
 * 6. erange_q: QUERY on vf_id == num_vfs -- expect -ERANGE with @vf_uuid
 *    populated as all-zeros (per the kernel doc-comment).
 */
static void test_erange_query(int fd, unsigned int num_vfs)
{
	struct mlx5_vfmig_query_vf info;
	int err;

	err = query_vf(fd, num_vfs, &info);
	if (err != -ERANGE) {
		fail("erange_q",
		     "QUERY vf %u: %s (expected -ERANGE)",
		     num_vfs, err ? strerror(-err) : "0");
		return;
	}
	if (memcmp(info.vf_uuid, UUID_ZERO, sizeof(UUID_ZERO)) != 0) {
		fail("erange_q",
		     "QUERY vf %u (-ERANGE): vf_uuid not zeroed",
		     num_vfs);
		return;
	}
	pass("erange_q", "");
}

int main(int argc, char **argv)
{
	const char *pf_bdf;
	unsigned int vf_id;
	char path[256];
	int fd, num_vfs;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <pf_bdf> <vf_id>\n", argv[0]);
		return 1;
	}
	pf_bdf = argv[1];
	vf_id  = strtoul(argv[2], NULL, 0);

	snprintf(path, sizeof(path), "/dev/mlx5_vfmig/%s", pf_bdf);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}

	num_vfs = discover_num_vfs(fd);
	if (num_vfs < 0) {
		close(fd);
		return 1;
	}
	if (num_vfs == 0) {
		fprintf(stderr,
			"no VFs provisioned on %s (sriov_numvfs == 0); enable SR-IOV first.\n",
			pf_bdf);
		close(fd);
		return 1;
	}
	if (vf_id >= (unsigned int)num_vfs) {
		fprintf(stderr,
			"vf_id %u out of range; PF has %u VFs.\n",
			vf_id, num_vfs);
		close(fd);
		return 1;
	}

	/*
	 * Order matters across cells: happy_set is the only one that
	 * places the slot into a known U-stamped state; idempotent /
	 * ebusy / einval_zero rely on that placement to read back. If
	 * the slot already carried a *different* UUID before the
	 * probe started, happy_set fails with a clear EBUSY message
	 * and we still try the OOB / ERANGE cells (those don't depend
	 * on the slot state) so the user gets full coverage on a
	 * partial run.
	 */
	test_happy_set(fd, vf_id);
	test_idempotent(fd, vf_id);
	test_ebusy(fd, vf_id);
	test_einval_zero(fd, vf_id);
	test_einval_oob(fd, num_vfs);
	test_erange_query(fd, num_vfs);

	close(fd);

	printf("\n%d passed, %d failed\n", n_pass, n_fail);
	return n_fail == 0 ? 0 : 2;
}
