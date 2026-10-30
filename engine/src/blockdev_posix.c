/*
 * Eastape HFS engine: block device backend for Linux, macOS and other POSIX
 * systems. Used for tests and CI against disk-image files; can also read
 * block devices such as /dev/sdb. The file is opened O_RDONLY.
 */

#ifndef _WIN32

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#elif defined(__APPLE__)
#include <sys/disk.h>
#include <sys/ioctl.h>
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static int posix_read_aligned(struct ehfs_blockdev* dev, void* buf, uint64_t length, uint64_t offset) {
	int fd = (int)(intptr_t)dev->handle;
	char* p = buf;
	while(length) {
		size_t want = length > (1u << 30) ? (1u << 30) : (size_t)length;
		ssize_t got = pread(fd, p, want, (off_t)offset);
		if(got < 0) {
			if(errno == EINTR)
				continue;
			return -errno;
		}
		if(got == 0)
			return -EIO;   /* unexpected end of file or media removed */
		p += got;
		offset += (uint64_t)got;
		length -= (uint64_t)got;
	}
	return 0;
}

static void posix_close(struct ehfs_blockdev* dev) {
	close((int)(intptr_t)dev->handle);
	free(dev);
}

static const struct ehfs_blockdev_ops posix_ops = { posix_read_aligned, posix_close };

int ehfs_blockdev_open_platform(const char* path, struct ehfs_blockdev** out) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if(fd < 0)
		return -errno;

	struct stat st;
	if(fstat(fd, &st)) {
		int err = -errno;
		close(fd);
		return err;
	}

	struct ehfs_blockdev* dev = calloc(1, sizeof(*dev));
	if(!dev) {
		close(fd);
		return -ENOMEM;
	}
	dev->ops = &posix_ops;
	dev->handle = (void*)(intptr_t)fd;
	dev->sector_size = 512;
	dev->align = 1;

	if(S_ISREG(st.st_mode)) {
		dev->size = (uint64_t)st.st_size;
	}
	else if(S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
		dev->is_device = true;
#if defined(__linux__)
		uint64_t bytes = 0;
		int ss = 0;
		if(ioctl(fd, BLKGETSIZE64, &bytes) == 0)
			dev->size = bytes;
		if(ioctl(fd, BLKSSZGET, &ss) == 0 && ss > 0)
			dev->sector_size = (uint32_t)ss;
#elif defined(__APPLE__)
		uint32_t bs = 0;
		uint64_t count = 0;
		if(ioctl(fd, DKIOCGETBLOCKSIZE, &bs) == 0 && bs)
			dev->sector_size = bs;
		if(ioctl(fd, DKIOCGETBLOCKCOUNT, &count) == 0)
			dev->size = count * dev->sector_size;
#endif
		dev->align = dev->sector_size;
	}
	else {
		posix_close(dev);
		return -EINVAL;
	}

	if(!dev->size) {
		posix_close(dev);
		return -EIO;
	}

	*out = dev;
	return 0;
}

#endif
