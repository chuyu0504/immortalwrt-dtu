// SPDX-License-Identifier: GPL-2.0-only
/* Read-only geometry and paired bad-block verification for the RAM target. */
#include <fcntl.h>
#include <inttypes.h>
#include <mtd/mtd-user.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
	struct mtd_info_user info;
	int fd, bad, first = 1;
	uint32_t peb;
	int64_t offset;

	fd = open("/dev/mtd3", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("open /dev/mtd3 read-only");
		return 1;
	}
	if (ioctl(fd, MEMGETINFO, &info) < 0) {
		perror("MEMGETINFO");
		close(fd);
		return 1;
	}
	if (info.type != MTD_NANDFLASH || info.flags & MTD_WRITEABLE ||
	    info.size != 526385152 || info.erasesize != 524288 ||
	    info.writesize != 8192 || info.oobsize != 256) {
		fprintf(stderr, "Refusing verification: unexpected geometry or writable MTD\n");
		close(fd);
		return 1;
	}
	printf("{\"size\":%" PRIu32 ",\"writesize\":%" PRIu32
	       ",\"erasesize\":%" PRIu32 ",\"flags\":%" PRIu32 ",\"bad_pebs\":[",
	       info.size, info.writesize, info.erasesize, info.flags);
	fflush(stdout);
	for (peb = 0; peb < info.size / info.erasesize; peb++) {
		offset = (int64_t)peb * info.erasesize;
		bad = ioctl(fd, MEMGETBADBLOCK, &offset);
		if (bad < 0) {
			perror("MEMGETBADBLOCK");
			close(fd);
			return 1;
		}
		if (bad) {
			printf("%s%" PRIu32, first ? "" : ",", peb);
			first = 0;
		}
	}
	puts("]}");
	close(fd);
	return 0;
}
