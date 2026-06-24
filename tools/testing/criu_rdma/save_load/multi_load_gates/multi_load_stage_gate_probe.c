// SPDX-License-Identifier: GPL-2.0
/*
 * multi_load_stage_gate_probe -- empirical kernel-matrix test for the
 * staging-side LOAD gate (vfmig_vf_id_busy_locked) that the design
 * doc §3.5 disclaimer in vf_prerestore_split.md relies on. Companion
 * to the KS7.3 (vf_uuid) probe layout under uobject_restore/vf_uuid/.
 *
 * The probe answers, empirically, the kernel-side half of the
 * "what happens if userspace tries to stage two LOADs at once?"
 * architectural question:
 *
 *   - gate is per-PF, per-vf_id; gate is per-session-fd, not
 *     per-pending-load (closing the LOAD fd unconditionally
 *     releases the gate, even if no bytes were ever written);
 *   - a SAVE session on vf_id N likewise blocks a LOAD session
 *     on vf_id N (and vice versa), because the gate is shared
 *     across both kinds of sessions in vfmig_vf_id_busy_locked
 *     (line 4199 in drivers/net/ethernet/mellanox/mlx5/core/vfmig/vfmig.c).
 *
 * The probe does NOT exercise the second staging gate
 * (vfmig_install_pending_load_locked) -- that one fires inside the
 * LOAD fd's release path and surfaces only as a dmesg warning. In
 * practice the IOVA replay drift_armed gate fires *first* on the
 * second LOAD's HOST_PAGE prefix and aborts the second LOAD before
 * the install gate's release path runs. The drift_armed gate is
 * empirically validated end-to-end by test_multi_load_drift_gate.sh
 * in this same directory. The install gate remains a code-level
 * safety net for a small race window (apply takes the slot, the
 * load_fd is closed, a fresh stage races in before sriov teardown),
 * which is hard to manufacture cleanly without a second IOVA
 * domain. See vf_prerestore_split.md §3.5.5.1 for the gate-by-gate
 * table.
 *
 * Subtests (no SAVE / no FW interaction; idempotent across re-runs):
 *
 *   1. happy_open_close   LOAD ioctl returns a valid fd; closing the
 *                         fd with no bytes written cleans up.
 *   2. gate_concurrent    Second LOAD ioctl while the first fd is
 *                         still open returns -EBUSY (per-vf_id).
 *   3. gate_clears        After gate_concurrent closes the first fd,
 *                         a fresh LOAD ioctl returns 0 (the gate
 *                         honours fd lifetime).
 *   4. different_vf       Two LOAD fds on *different* vf_ids may
 *                         coexist; the gate is keyed on vf_id, not
 *                         on the PF. Skipped if num_vfs < 2.
 *
 * Side effects:
 *   None on success. No pending_load is installed: the probe never
 *   write()s any bytes through any LOAD fd, so on close the
 *   release path takes the !image_staged branch and only frees the
 *   PD it allocated for the (would-have-been) MKEY (see
 *   vfmig_load_release_resources, drivers/net/ethernet/mellanox/
 *   mlx5/core/vfmig/vfmig.c line ~3829). Slots are returned to the same
 *   "unset" state the probe found them in.
 *
 * Requires:
 *   - SR-IOV up with num_vfs >= 1 on the supplied PF;
 *   - sriov_drivers_autoprobe = 0 (so the VF is unbound and
 *     enable_migratable can be called);
 *   - vf_id supplied has had MLX5_VFMIG_IOC_ENABLE_MIGRATABLE
 *     called on it before the probe runs (the LOAD ioctl gates on
 *     the migratable cap before allocating the load_fd; the gate
 *     fires before the busy check, and we want to test the busy
 *     check, not migratable). The companion shell wrapper
 *     test_multi_load_stage_gate.sh handles this provisioning.
 *
 * Build (in-tree):
 *   make -C tools/testing/criu_rdma \
 *        save_load/multi_load_gates/multi_load_stage_gate_probe
 *
 * Usage:
 *   multi_load_stage_gate_probe <pf_bdf> <vf_id>
 *
 * Exit codes:
 *   0   all subtests PASSed
 *   1   setup error (bad args, open() failed, num_vfs == 0, etc.)
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

static int n_pass;
static int n_fail;

static void pass(const char *name)
{
	n_pass++;
	printf("PASS %s\n", name);
}

static void fail(const char *name, const char *fmt, ...)
{
	va_list ap;

	n_fail++;
	fprintf(stderr, "FAIL %-18s ", name);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/*
 * Wrap MLX5_VFMIG_IOC_LOAD_VHCA_STATE so we can capture the kernel
 * verdict (0 / -errno) and the returned load_fd in one shot. Mirrors
 * the pattern used by vf_uuid_probe_mlx5_vfmig.c::set_uuid().
 */
static int load_open(int cdev_fd, unsigned int vf_id, int *out_load_fd)
{
	struct mlx5_vfmig_load_state arg = {
		.vf_id    = vf_id,
		.flags    = 0,
		.load_fd  = -1,
		.reserved = 0,
	};

	if (ioctl(cdev_fd, MLX5_VFMIG_IOC_LOAD_VHCA_STATE, &arg) == 0) {
		*out_load_fd = arg.load_fd;
		return 0;
	}
	*out_load_fd = -1;
	return -errno;
}

static int discover_num_vfs(int cdev_fd)
{
	struct mlx5_vfmig_query_vf arg = { .vf_id = 0 };

	if (ioctl(cdev_fd, MLX5_VFMIG_IOC_QUERY_VF, &arg) < 0) {
		if (errno != ERANGE) {
			perror("QUERY_VF discover");
			return -errno;
		}
	}
	return (int)arg.num_vfs;
}

/* 1. happy_open_close: LOAD opens, fd closes cleanly with no bytes. */
static void test_happy_open_close(int cdev_fd, unsigned int vf_id)
{
	int load_fd;
	int err = load_open(cdev_fd, vf_id, &load_fd);

	if (err) {
		fail("happy_open_close",
		     "LOAD vf %u: %s (expected 0; ENABLE_MIGRATABLE done?)",
		     vf_id, strerror(-err));
		return;
	}
	if (close(load_fd) < 0) {
		fail("happy_open_close",
		     "close(load_fd): %s", strerror(errno));
		return;
	}
	pass("happy_open_close");
}

/* 2. gate_concurrent: second LOAD while first fd open returns -EBUSY. */
static void test_gate_concurrent(int cdev_fd, unsigned int vf_id)
{
	int fd1 = -1, fd2 = -1;
	int err;

	err = load_open(cdev_fd, vf_id, &fd1);
	if (err) {
		fail("gate_concurrent",
		     "first LOAD vf %u: %s (expected 0)",
		     vf_id, strerror(-err));
		return;
	}

	err = load_open(cdev_fd, vf_id, &fd2);
	if (err != -EBUSY) {
		fail("gate_concurrent",
		     "second LOAD vf %u (fd1 still open): %s (expected -EBUSY)",
		     vf_id, err ? strerror(-err) : "0");
		if (fd2 >= 0)
			close(fd2);
		close(fd1);
		return;
	}

	if (close(fd1) < 0) {
		fail("gate_concurrent",
		     "close(fd1): %s", strerror(errno));
		return;
	}
	pass("gate_concurrent");
}

/*
 * 3. gate_clears: after the busy fd from #2 closes, the gate should
 *    release; a fresh LOAD ioctl on the same vf_id returns 0.
 *    Run AFTER test_gate_concurrent.
 */
static void test_gate_clears(int cdev_fd, unsigned int vf_id)
{
	int fd;
	int err = load_open(cdev_fd, vf_id, &fd);

	if (err) {
		fail("gate_clears",
		     "LOAD vf %u after gate_concurrent close: %s (expected 0)",
		     vf_id, strerror(-err));
		return;
	}
	if (close(fd) < 0) {
		fail("gate_clears", "close: %s", strerror(errno));
		return;
	}
	pass("gate_clears");
}

/*
 * 4. different_vf: gate is per-vf_id, so concurrent LOADs on
 *    different vf_ids both succeed. Skipped if num_vfs < 2.
 */
static void test_different_vf(int cdev_fd, unsigned int vf_id, int num_vfs)
{
	unsigned int peer_vf;
	int fd_self = -1, fd_peer = -1;
	int err;

	if (num_vfs < 2) {
		printf("SKIP different_vf      (num_vfs=%d, need >= 2)\n",
		       num_vfs);
		return;
	}
	peer_vf = (vf_id == 0) ? 1 : 0;

	err = load_open(cdev_fd, vf_id, &fd_self);
	if (err) {
		fail("different_vf",
		     "LOAD vf %u: %s (expected 0)",
		     vf_id, strerror(-err));
		return;
	}
	err = load_open(cdev_fd, peer_vf, &fd_peer);
	if (err) {
		fail("different_vf",
		     "LOAD vf %u (with vf %u still open): %s (expected 0; per-vf gate?)",
		     peer_vf, vf_id, strerror(-err));
		close(fd_self);
		return;
	}
	if (close(fd_peer) < 0 || close(fd_self) < 0) {
		fail("different_vf", "close(): %s", strerror(errno));
		return;
	}
	pass("different_vf");
}

int main(int argc, char **argv)
{
	const char *pf_bdf;
	unsigned int vf_id;
	char path[256];
	int cdev_fd, num_vfs;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <pf_bdf> <vf_id>\n", argv[0]);
		return 1;
	}
	pf_bdf = argv[1];
	vf_id  = strtoul(argv[2], NULL, 0);

	snprintf(path, sizeof(path), "/dev/mlx5_vfmig/%s", pf_bdf);
	cdev_fd = open(path, O_RDWR);
	if (cdev_fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}

	num_vfs = discover_num_vfs(cdev_fd);
	if (num_vfs < 0) {
		close(cdev_fd);
		return 1;
	}
	if (num_vfs == 0) {
		fprintf(stderr,
			"no VFs provisioned on %s (sriov_numvfs == 0); enable SR-IOV first.\n",
			pf_bdf);
		close(cdev_fd);
		return 1;
	}
	if (vf_id >= (unsigned int)num_vfs) {
		fprintf(stderr,
			"vf_id %u out of range; PF has %d VFs.\n",
			vf_id, num_vfs);
		close(cdev_fd);
		return 1;
	}

	/*
	 * Order matters: gate_clears reads back the slot state that
	 * gate_concurrent placed it in. different_vf is independent
	 * but must run last so a stray peer-vf failure does not poison
	 * the gate cells.
	 */
	test_happy_open_close(cdev_fd, vf_id);
	test_gate_concurrent(cdev_fd, vf_id);
	test_gate_clears(cdev_fd, vf_id);
	test_different_vf(cdev_fd, vf_id, num_vfs);

	close(cdev_fd);
	printf("\n%d passed, %d failed\n", n_pass, n_fail);
	return n_fail == 0 ? 0 : 2;
}
