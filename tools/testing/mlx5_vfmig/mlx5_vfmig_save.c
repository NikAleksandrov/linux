// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_vfmig_save - drain a VFIO mlx5 STOP_COPY data_fd into a file.
 *
 * Helper for the M2 end-to-end test: the source side uses the upstream
 * VFIO mlx5 variant driver (mlx5_vfio_pci) to produce a migration blob
 * which is then consumed by our mlx5_vfmig cdev's LOAD_VHCA_STATE.
 *
 * Build:
 *   cc -O2 -Wall -o mlx5_vfmig_save mlx5_vfmig_save.c
 *
 * Use:
 *   echo $VF > /sys/bus/pci/devices/$VF/driver_override   # already
 *   echo mlx5_vfio_pci > .../driver_override
 *   echo $VF > /sys/bus/pci/drivers/mlx5_vfio_pci/bind
 *   mlx5_vfmig_save <vf-bdf> <out-blob-path>
 *
 * The VF must already be bound to mlx5_vfio_pci. We open the VFIO cdev
 * for it via /sys/bus/pci/devices/<bdf>/vfio-dev/vfio<N>/dev.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/vfio.h>
#include <linux/iommufd.h>

static int find_vfio_cdev_path(const char *bdf, char *out, size_t outsz)
{
	char dirpath[256];
	DIR *d;
	struct dirent *de;
	int ret = -1;

	snprintf(dirpath, sizeof(dirpath),
		 "/sys/bus/pci/devices/%s/vfio-dev", bdf);
	d = opendir(dirpath);
	if (!d) {
		fprintf(stderr,
			"opendir %s: %s (is %s bound to vfio-pci?)\n",
			dirpath, strerror(errno), bdf);
		return -1;
	}
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "vfio", 4) != 0)
			continue;
		snprintf(out, outsz, "/dev/vfio/devices/%.32s",
			 de->d_name);
		ret = 0;
		break;
	}
	closedir(d);
	if (ret)
		fprintf(stderr, "no vfio<N> entry under %s\n", dirpath);
	return ret;
}

static int set_mig_state(int devfd, __u32 state, __s32 *out_data_fd)
{
	struct {
		struct vfio_device_feature hdr;
		struct vfio_device_feature_mig_state mig;
	} arg = {
		.hdr = {
			.argsz = sizeof(arg),
			.flags = VFIO_DEVICE_FEATURE_SET |
				 VFIO_DEVICE_FEATURE_MIG_DEVICE_STATE,
		},
		.mig = { .device_state = state, .data_fd = -1 },
	};

	if (ioctl(devfd, VFIO_DEVICE_FEATURE, &arg) < 0)
		return -errno;
	if (out_data_fd)
		*out_data_fd = arg.mig.data_fd;
	return 0;
}

static int drain_to_file(int data_fd, int out_fd, size_t *out_total)
{
	enum { CHUNK = 1u << 16 };
	char *buf = malloc(CHUNK);
	size_t total = 0;
	ssize_t r;

	if (!buf)
		return -ENOMEM;
	for (;;) {
		r = read(data_fd, buf, CHUNK);
		if (r == 0)
			break;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return -errno;
		}
		ssize_t off = 0;

		while (off < r) {
			ssize_t w = write(out_fd, buf + off, r - off);

			if (w < 0) {
				if (errno == EINTR)
					continue;
				free(buf);
				return -errno;
			}
			off += w;
			total += w;
		}
	}
	free(buf);
	*out_total = total;
	return 0;
}

int main(int argc, char **argv)
{
	const char *bdf, *outpath;
	char cdev_path[256];
	int iommufd_fd = -1, devfd = -1, datafd = -1, outfd = -1;
	struct vfio_device_bind_iommufd bind = { .argsz = sizeof(bind) };
	struct vfio_device_attach_iommufd_pt att = { .argsz = sizeof(att) };
	struct iommu_ioas_alloc ioas = { .size = sizeof(ioas) };
	size_t total = 0;
	int err = 1;

	if (argc != 3) {
		fprintf(stderr,
			"usage: %s <vf-bdf> <out-blob-path>\n", argv[0]);
		return 2;
	}
	bdf = argv[1];
	outpath = argv[2];

	if (find_vfio_cdev_path(bdf, cdev_path, sizeof(cdev_path)))
		return 1;

	iommufd_fd = open("/dev/iommu", O_RDWR | O_CLOEXEC);
	if (iommufd_fd < 0) {
		perror("open /dev/iommu");
		goto out;
	}

	devfd = open(cdev_path, O_RDWR | O_CLOEXEC);
	if (devfd < 0) {
		fprintf(stderr, "open %s: %s\n", cdev_path, strerror(errno));
		goto out;
	}

	bind.iommufd = iommufd_fd;
	if (ioctl(devfd, VFIO_DEVICE_BIND_IOMMUFD, &bind) < 0) {
		perror("VFIO_DEVICE_BIND_IOMMUFD");
		goto out;
	}

	if (ioctl(iommufd_fd, IOMMU_IOAS_ALLOC, &ioas) < 0) {
		perror("IOMMU_IOAS_ALLOC");
		goto out;
	}

	att.pt_id = ioas.out_ioas_id;
	if (ioctl(devfd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &att) < 0) {
		perror("VFIO_DEVICE_ATTACH_IOMMUFD_PT");
		goto out;
	}

	/*
	 * Move RUNNING -> STOP -> STOP_COPY. STOP_COPY's set returns a
	 * data_fd from which we drain the migration stream.
	 */
	if (set_mig_state(devfd, VFIO_DEVICE_STATE_STOP, NULL)) {
		perror("set state STOP");
		goto out;
	}
	if (set_mig_state(devfd, VFIO_DEVICE_STATE_STOP_COPY, &datafd)) {
		perror("set state STOP_COPY");
		goto out;
	}

	outfd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (outfd < 0) {
		fprintf(stderr, "open %s: %s\n", outpath, strerror(errno));
		goto out;
	}

	err = drain_to_file(datafd, outfd, &total);
	if (err) {
		errno = -err;
		fprintf(stderr, "drain: %s\n", strerror(errno));
		err = 1;
		goto out;
	}
	fprintf(stderr, "saved %zu bytes to %s\n", total, outpath);
	err = 0;

out:
	if (outfd >= 0)
		close(outfd);
	if (datafd >= 0)
		close(datafd);
	if (devfd >= 0)
		close(devfd);
	if (iommufd_fd >= 0)
		close(iommufd_fd);
	return err;
}
