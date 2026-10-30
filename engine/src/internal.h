/*
 * Eastape HFS engine: internal declarations shared between source files.
 */

#ifndef EHFS_INTERNAL_H
#define EHFS_INTERNAL_H

#include "ehfs.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- Logging ------------------------------------------------------------ */

void ehfs_log(int level, const char* fmt, ...)
#if defined(__GNUC__)
	__attribute__((format(printf, 2, 3)))
#endif
	;

/* ---- Block devices -------------------------------------------------------
 * A read-only view of a disk or image file. Platform backends implement
 * open/read/close; blockdev.c adds sector alignment on top.
 */

struct ehfs_blockdev;

struct ehfs_blockdev_ops {
	/* offset, length and buf are multiples of `align`; never reads past `size` */
	int  (*read_aligned)(struct ehfs_blockdev* dev, void* buf, uint64_t length, uint64_t offset);
	void (*close)(struct ehfs_blockdev* dev);
};

struct ehfs_blockdev {
	const struct ehfs_blockdev_ops* ops;
	void* handle;
	uint64_t size;            /* bytes */
	uint32_t sector_size;     /* logical sector size (partition-table LBA unit), power of two */
	uint32_t align;           /* required I/O alignment: sector_size for devices, 1 for image files */
	bool is_device;           /* physical disk rather than an image file */
};

/* platform backend: blockdev_posix.c or blockdev_win32.c */
int  ehfs_blockdev_open_platform(const char* path, struct ehfs_blockdev** out);

/* Read any byte range. Unaligned requests go through an aligned bounce buffer.
 * Thread-safe. Returns 0 or a negative errno value. */
int  ehfs_blockdev_read(struct ehfs_blockdev* dev, void* buf, uint64_t length, uint64_t offset);
void ehfs_blockdev_close(struct ehfs_blockdev* dev);

void* ehfs_aligned_alloc(size_t alignment, size_t size);
void  ehfs_aligned_free(void* p);

struct ehfs_disk {
	struct ehfs_blockdev* dev;
};

/* ---- Probing and partitions --------------------------------------------- */

/* read_name: also open the catalog to read the volume name (slower) */
int ehfs_probe_range(struct ehfs_blockdev* dev, uint64_t offset, uint64_t length,
                     const char* partition_type, bool read_name, ehfs_volume_info* out);
int ehfs_scan_partitions(struct ehfs_blockdev* dev, ehfs_disk_layout* out);

/* ---- Helpers ------------------------------------------------------------ */

static inline uint16_t ehfs_be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t ehfs_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint64_t ehfs_be64(const uint8_t* p) { return (uint64_t)ehfs_be32(p) << 32 | ehfs_be32(p + 4); }
static inline uint16_t ehfs_le16(const uint8_t* p) { return (uint16_t)(p[1] << 8 | p[0]); }
static inline uint32_t ehfs_le32(const uint8_t* p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static inline uint64_t ehfs_le64(const uint8_t* p) { return (uint64_t)ehfs_le32(p + 4) << 32 | ehfs_le32(p); }

/* HFS+ dates are seconds since 1904-01-01 */
#define EHFS_HFS_TO_UNIX(t) ((int64_t)(t) - 2082844800)

/* Convert UTF-16 (big- or little-endian) to UTF-8. Returns bytes written. */
size_t ehfs_utf16_to_utf8(const uint8_t* src, size_t units, bool big_endian, char* dst, size_t dst_size);

void ehfs_copy_string(char* dst, size_t dst_size, const char* src);

#endif
