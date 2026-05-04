// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_vfmig - tiny userspace helper for the /dev/mlx5_vfmig/<bdf> cdev.
 * Intended for development and triage; production users should speak the
 * ioctls from a real CRIU plugin.
 *
 * Build:
 *   cc -O2 -Wall -o mlx5_vfmig mlx5_vfmig.c
 *
 * Use:
 *   mlx5_vfmig <pf-bdf> mark_restored    <vf_id>
 *   mlx5_vfmig <pf-bdf> get_vhca_id      <vf_id>
 *   mlx5_vfmig <pf-bdf> query_vf         <vf_id>
 *   mlx5_vfmig <pf-bdf> list
 *   mlx5_vfmig <pf-bdf> load_vhca_state  <vf_id> <blob_path>
 *   mlx5_vfmig <pf-bdf> save_vhca_state  <vf_id> <blob_path> [keep_suspended]
 *   mlx5_vfmig <pf-bdf> enable_migratable <vf_id>
 *
 * Verbs accept either '_' or '-' between words.
 *
 * Examples:
 *   mlx5_vfmig 0000:00:08.0 list
 *   mlx5_vfmig 0000:00:08.0 save_vhca_state 0 /tmp/vf.blob
 *   mlx5_vfmig 0000:00:08.0 load_vhca_state 0 /tmp/vf.blob
 *   mlx5_vfmig 0000:00:08.0 mark_restored 0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../include/uapi/linux/mlx5_vfmig.h"

static int do_enable_migratable(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_enable_migratable arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_ENABLE_MIGRATABLE, &arg) < 0) {
		perror("ENABLE_MIGRATABLE");
		return 1;
	}
	printf("vf %u: migratable cap enabled (call before driver bind)\n",
	       vf_id);
	return 0;
}

/*
 * EXPERIMENTAL: probe the firmware's per-VHCA uctx-id allocator on a
 * bound VF by issuing CREATE_UCTX + immediate DESTROY_UCTX from the
 * PF. Used to answer "did LOAD_VHCA_STATE preserve the source's
 * uctx-id space?" empirically. See
 * include/uapi/linux/mlx5_vfmig.h for methodology.
 */
static int do_probe_uid(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_probe_uid arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_PROBE_UID, &arg) < 0) {
		if (errno == ENODEV)
			fprintf(stderr,
				"vf %u: not bound to mlx5_core (PROBE_UID requires the VF mdev to be interface-up)\n",
				vf_id);
		else
			perror("PROBE_UID");
		return 1;
	}
	printf("vf %u: probe_uid -> uid=%u\n", vf_id, arg.uid);
	return 0;
}

static int do_set_tracked(int fd, unsigned int vf_id, unsigned int enable)
{
	struct mlx5_vfmig_set_tracked arg = {
		.vf_id  = vf_id,
		.enable = enable ? 1 : 0,
	};

	if (ioctl(fd, MLX5_VFMIG_IOC_SET_TRACKED, &arg) < 0) {
		perror("SET_TRACKED");
		return 1;
	}
	printf("vf %u: vfmig tracked=%u (call before driver bind)\n",
	       vf_id, arg.enable);
	return 0;
}

static int do_mark(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_mark_restored arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_MARK_RESTORED, &arg) < 0) {
		if (errno == EALREADY)
			fprintf(stderr,
				"vf %u is already marked restored\n", vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"vf_id %u out of range (have you set sriov_numvfs?)\n",
				vf_id);
		else
			perror("MARK_RESTORED");
		return 1;
	}
	printf("marked vf %u as restored\n", vf_id);
	return 0;
}

static int do_get(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_get_vhca_id arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_GET_VHCA_ID, &arg) < 0) {
		perror("GET_VHCA_ID");
		return 1;
	}
	printf("vf %u vhca_id 0x%04x\n", vf_id, arg.vhca_id);
	return 0;
}

static int query_one(int fd, unsigned int vf_id, struct mlx5_vfmig_query_vf *out)
{
	struct mlx5_vfmig_query_vf arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_QUERY_VF, &arg) < 0)
		return -errno;
	*out = arg;
	return 0;
}

static int do_query(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_query_vf info;
	int err = query_one(fd, vf_id, &info);

	if (err == -ERANGE) {
		fprintf(stderr,
			"vf_id %u out of range; PF has %u VFs\n",
			vf_id, info.num_vfs);
		return 1;
	}
	if (err) {
		errno = -err;
		perror("QUERY_VF");
		return 1;
	}
	printf("vf %u vhca_id 0x%04x restored=%u tracked=%u (num_vfs=%u)\n",
	       vf_id, info.vhca_id, info.restored, info.tracked,
	       info.num_vfs);
	return 0;
}

static int do_list(int fd)
{
	struct mlx5_vfmig_query_vf info;
	unsigned int i, n;
	int err;

	err = query_one(fd, 0, &info);
	if (err && err != -ERANGE) {
		errno = -err;
		perror("QUERY_VF");
		return 1;
	}
	n = info.num_vfs;
	if (n == 0) {
		printf("no VFs provisioned (sriov_numvfs == 0)\n");
		return 0;
	}

	printf("%-6s %-9s %-9s %s\n",
	       "vf_id", "vhca_id", "restored", "tracked");
	for (i = 0; i < n; i++) {
		err = query_one(fd, i, &info);
		if (err) {
			errno = -err;
			fprintf(stderr, "QUERY_VF vf %u: %s\n",
				i, strerror(errno));
			continue;
		}
		printf("%-6u 0x%04x    %-9u %u\n",
		       i, info.vhca_id, info.restored, info.tracked);
	}
	return 0;
}

static int pump_blob(int load_fd, int blob_fd, size_t *out_bytes)
{
	/*
	 * Use a moderately large buffer so the FSM in the kernel sees
	 * full records in fewer write() syscalls; partial writes are
	 * also fine.
	 */
	enum { CHUNK = 1u << 16 };
	char *buf = malloc(CHUNK);
	size_t total = 0;

	if (!buf)
		return -ENOMEM;

	for (;;) {
		ssize_t r = read(blob_fd, buf, CHUNK);
		ssize_t off = 0;

		if (r == 0)
			break;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return -errno;
		}
		while (off < r) {
			ssize_t w = write(load_fd, buf + off, r - off);

			if (w < 0) {
				if (errno == EINTR)
					continue;
				free(buf);
				return -errno;
			}
			if (w == 0) {
				free(buf);
				return -EIO;
			}
			off += w;
			total += w;
		}
	}

	free(buf);
	*out_bytes = total;
	return 0;
}

static int do_load(int fd, unsigned int vf_id, const char *blob_path)
{
	struct mlx5_vfmig_load_state arg = { .vf_id = vf_id };
	int blob_fd, load_fd, err;
	size_t bytes = 0;
	struct stat st;

	blob_fd = open(blob_path, O_RDONLY);
	if (blob_fd < 0) {
		fprintf(stderr, "open %s: %s\n", blob_path, strerror(errno));
		return 1;
	}
	if (fstat(blob_fd, &st) == 0)
		fprintf(stderr,
			"vfmig: streaming %lld bytes from %s into vf %u\n",
			(long long)st.st_size, blob_path, vf_id);

	if (ioctl(fd, MLX5_VFMIG_IOC_LOAD_VHCA_STATE, &arg) < 0) {
		perror("LOAD_VHCA_STATE");
		close(blob_fd);
		return 1;
	}
	load_fd = arg.load_fd;

	err = pump_blob(load_fd, blob_fd, &bytes);
	close(blob_fd);
	if (err) {
		errno = -err;
		fprintf(stderr,
			"vfmig: write failed after %zu bytes: %s\n",
			bytes, strerror(errno));
		close(load_fd);
		return 1;
	}

	if (close(load_fd) < 0) {
		perror("close(load_fd)");
		return 1;
	}
	printf("loaded %zu bytes of state into vf %u\n", bytes, vf_id);
	return 0;
}

static int drain_to_file(int save_fd, int blob_fd, size_t *out_bytes)
{
	enum { CHUNK = 1u << 16 };
	char *buf = malloc(CHUNK);
	size_t total = 0;

	if (!buf)
		return -ENOMEM;

	for (;;) {
		ssize_t r = read(save_fd, buf, CHUNK);
		ssize_t off = 0;

		if (r == 0)
			break;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return -errno;
		}
		while (off < r) {
			ssize_t w = write(blob_fd, buf + off, r - off);

			if (w < 0) {
				if (errno == EINTR)
					continue;
				free(buf);
				return -errno;
			}
			if (w == 0) {
				free(buf);
				return -EIO;
			}
			off += w;
			total += w;
		}
	}

	free(buf);
	*out_bytes = total;
	return 0;
}

static int do_save(int fd, unsigned int vf_id, const char *blob_path,
		   unsigned int flags)
{
	struct mlx5_vfmig_save_state arg = {
		.vf_id = vf_id,
		.flags = flags,
	};
	int blob_fd, save_fd, err;
	size_t bytes = 0;

	blob_fd = open(blob_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (blob_fd < 0) {
		fprintf(stderr, "open %s: %s\n", blob_path, strerror(errno));
		return 1;
	}

	if (ioctl(fd, MLX5_VFMIG_IOC_SAVE_VHCA_STATE, &arg) < 0) {
		perror("SAVE_VHCA_STATE");
		close(blob_fd);
		return 1;
	}
	save_fd = arg.save_fd;

	err = drain_to_file(save_fd, blob_fd, &bytes);
	if (close(save_fd) < 0)
		perror("close(save_fd)");
	if (close(blob_fd) < 0)
		perror("close(blob_fd)");
	if (err) {
		errno = -err;
		fprintf(stderr,
			"vfmig: drain failed after %zu bytes: %s\n",
			bytes, strerror(errno));
		return 1;
	}
	printf("saved %zu bytes from vf %u to %s%s\n", bytes, vf_id, blob_path,
	       (flags & MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED) ?
		" (left suspended)" : "");
	return 0;
}

/*
 * Verb match that accepts either "mark_restored" or "mark-restored",
 * etc. Treats '_' and '-' as equivalent so callers don't have to
 * remember which spelling the tool uses.
 */
static int verb_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char ca = (*a == '-') ? '_' : *a;
		char cb = (*b == '-') ? '_' : *b;

		if (ca != cb)
			return 0;
	}
	return *a == 0 && *b == 0;
}

static int looks_like_bdf(const char *s)
{
	/* "DDDD:BB:DD.F" -- at least one ':' and no '/' */
	return strchr(s, ':') && !strchr(s, '/');
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <pf-bdf> <verb> [args]\n"
		"  mark_restored    <vf_id>\n"
		"  get_vhca_id      <vf_id>\n"
		"  query_vf         <vf_id>\n"
		"  list\n"
		"  load_vhca_state  <vf_id> <blob_path>\n"
		"  save_vhca_state  <vf_id> <blob_path> [keep_suspended]\n"
		"  enable_migratable <vf_id>\n"
		"  set_tracked       <vf_id> <0|1>\n"
		"  probe_uid         <vf_id>     (experimental)\n"
		"verbs accept '-' or '_' interchangeably\n",
		argv0);
}

int main(int argc, char **argv)
{
	char path[256];
	const char *target;
	const char *verb;
	int fd, ret;

	if (argc < 3) {
		usage(argv[0]);
		return 2;
	}
	verb = argv[2];

	if (looks_like_bdf(argv[1])) {
		snprintf(path, sizeof(path), "/dev/mlx5_vfmig/%s", argv[1]);
		target = path;
	} else {
		target = argv[1];
	}

	fd = open(target, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", target, strerror(errno));
		return 1;
	}

	if (verb_eq(verb, "list")) {
		if (argc != 3)
			goto badargs;
		ret = do_list(fd);
	} else if (verb_eq(verb, "mark_restored")) {
		if (argc != 4)
			goto badargs;
		ret = do_mark(fd, strtoul(argv[3], NULL, 0));
	} else if (verb_eq(verb, "get_vhca_id")) {
		if (argc != 4)
			goto badargs;
		ret = do_get(fd, strtoul(argv[3], NULL, 0));
	} else if (verb_eq(verb, "query_vf")) {
		if (argc != 4)
			goto badargs;
		ret = do_query(fd, strtoul(argv[3], NULL, 0));
	} else if (verb_eq(verb, "load_vhca_state")) {
		if (argc != 5)
			goto badargs;
		ret = do_load(fd, strtoul(argv[3], NULL, 0), argv[4]);
	} else if (verb_eq(verb, "save_vhca_state")) {
		unsigned int flags = 0;

		if (argc < 5 || argc > 6)
			goto badargs;
		if (argc == 6) {
			if (!strcmp(argv[5], "keep_suspended") ||
			    !strcmp(argv[5], "keep-suspended"))
				flags |= MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED;
			else
				goto badargs;
		}
		ret = do_save(fd, strtoul(argv[3], NULL, 0), argv[4], flags);
	} else if (verb_eq(verb, "enable_migratable")) {
		if (argc != 4)
			goto badargs;
		ret = do_enable_migratable(fd, strtoul(argv[3], NULL, 0));
	} else if (verb_eq(verb, "set_tracked")) {
		if (argc != 5)
			goto badargs;
		ret = do_set_tracked(fd, strtoul(argv[3], NULL, 0),
				     strtoul(argv[4], NULL, 0));
	} else if (verb_eq(verb, "probe_uid")) {
		if (argc != 4)
			goto badargs;
		ret = do_probe_uid(fd, strtoul(argv[3], NULL, 0));
	} else {
		fprintf(stderr, "unknown verb: %s\n", verb);
		ret = 2;
	}

	close(fd);
	return ret;

badargs:
	close(fd);
	usage(argv[0]);
	return 2;
}
