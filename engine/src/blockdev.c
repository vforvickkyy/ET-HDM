/*
 * Eastape HFS engine: platform-independent part of the block device layer.
 *
 * Raw disks only accept reads whose offset, length and buffer address are
 * multiples of the sector size. Callers (libhfs) read arbitrary ranges, so
 * unaligned requests are split and staged through an aligned buffer that is
 * allocated per call, which keeps this function thread-safe without locks.
 */

#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <malloc.h>
#endif

#define BOUNCE_MAX (1u << 20)   /* largest staged chunk */

void* ehfs_aligned_alloc(size_t alignment, size_t size) {
	if(alignment < sizeof(void*))
		alignment = sizeof(void*);
#ifdef _WIN32
	return _aligned_malloc(size, alignment);
#else
	void* p = NULL;
	return posix_memalign(&p, alignment, size) ? NULL : p;
#endif
}

void ehfs_aligned_free(void* p) {
#ifdef _WIN32
	_aligned_free(p);
#else
	free(p);
#endif
}

int ehfs_blockdev_read(struct ehfs_blockdev* dev, void* buf, uint64_t length, uint64_t offset) {
	if(!length)
		return 0;
	if(offset > dev->size || length > dev->size - offset)
		return -EIO;   /* beyond the end of the device */

	const uint64_t a = dev->align;
	if(offset % a == 0 && length % a == 0 && (uintptr_t)buf % a == 0)
		return dev->ops->read_aligned(dev, buf, length, offset);

	/* device sizes are whole sectors, so the padded range never passes the end */
	uint64_t start = offset - offset % a;
	uint64_t end = offset + length;
	if(end % a)
		end += a - end % a;

	size_t chunk_max = (size_t)(end - start < BOUNCE_MAX ? end - start : BOUNCE_MAX);
	uint8_t* bounce = ehfs_aligned_alloc((size_t)a, chunk_max);
	if(!bounce)
		return -ENOMEM;

	int ret = 0;
	uint8_t* out = buf;
	for(uint64_t pos = start; pos < end && !ret; ) {
		uint64_t chunk = end - pos < chunk_max ? end - pos : chunk_max;
		if((ret = dev->ops->read_aligned(dev, bounce, chunk, pos)))
			break;
		/* copy the overlap of [pos, pos+chunk) with [offset, offset+length) */
		uint64_t from = pos < offset ? offset : pos;
		uint64_t to = pos + chunk < offset + length ? pos + chunk : offset + length;
		if(to > from)
			memcpy(out + (from - offset), bounce + (from - pos), (size_t)(to - from));
		pos += chunk;
	}

	ehfs_aligned_free(bounce);
	return ret;
}

void ehfs_blockdev_close(struct ehfs_blockdev* dev) {
	if(dev)
		dev->ops->close(dev);
}
