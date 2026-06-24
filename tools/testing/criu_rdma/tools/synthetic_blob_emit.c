// SPDX-License-Identifier: GPL-2.0
/*
 * synthetic_blob_emit - emit a single-record VFIO-mlx5-format blob with
 * a synthetic FW_DATA payload. Used to smoke-test the kernel LOAD
 * plumbing (parser FSM, PD/MKEY/DMA registration, fd lifetime) without
 * needing a real save side. Firmware will of course reject the junk
 * bytes -- we expect a "LOAD_VHCA_STATE ... failed" warning in dmesg.
 * The point is to exercise everything *around* the firmware call.
 *
 * Build via the directory Makefile:
 *   make -C tools/testing/criu_rdma tools/synthetic_blob_emit
 *
 * Use:
 *   synthetic_blob_emit <out-blob-path> [size-bytes]
 *
 *   default size is 4096 bytes of 0xAA payload.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Mirror of struct mlx5_vf_migration_header from the VFIO mlx5 driver.
 * Kept here as plain bytes because the type isn't UAPI.
 */
struct synth_hdr {
	uint64_t record_size;	/* le64 */
	uint32_t flags;		/* le32 -- 0 = mandatory tag */
	uint32_t tag;		/* le32 -- 0 = FW_DATA */
};

int main(int argc, char **argv)
{
	const char *out;
	size_t payload_size = 4096;
	struct synth_hdr hdr = {0};
	int fd, err = 1;
	char *buf = NULL;

	if (argc < 2 || argc > 3) {
		fprintf(stderr,
			"usage: %s <out-blob-path> [size-bytes]\n", argv[0]);
		return 2;
	}
	out = argv[1];
	if (argc == 3)
		payload_size = strtoull(argv[2], NULL, 0);
	if (!payload_size) {
		fprintf(stderr, "size must be > 0\n");
		return 2;
	}

	hdr.record_size = (uint64_t)payload_size;
	hdr.flags = 0;
	hdr.tag = 0;

	buf = malloc(payload_size);
	if (!buf) {
		perror("malloc");
		return 1;
	}
	memset(buf, 0xAA, payload_size);

	fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", out, strerror(errno));
		goto done;
	}
	if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
		perror("write hdr");
		close(fd);
		goto done;
	}
	if (write(fd, buf, payload_size) != (ssize_t)payload_size) {
		perror("write payload");
		close(fd);
		goto done;
	}
	close(fd);
	fprintf(stderr,
		"wrote %s: 1 FW_DATA record, %zu bytes payload (total %zu bytes)\n",
		out, payload_size, sizeof(hdr) + payload_size);
	err = 0;
done:
	free(buf);
	return err;
}
