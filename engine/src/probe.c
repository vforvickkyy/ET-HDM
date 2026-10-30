/*
 * Eastape HFS engine: identify what is stored in a byte range of a disk.
 *
 * Works from on-disk signatures, independent of libhfs, so it can classify
 * every volume (including ones we refuse to open) and explain why. For HFS+ it
 * also reports whether the journal holds changes that were never applied.
 *
 * References: Apple TN1150 "HFS Plus Volume Format"; Apple File System
 * Reference (container superblock magic).
 */

#include "internal.h"

#include <errno.h>
#include <string.h>

#define SIG_HFSPLUS 0x482B   /* "H+" */
#define SIG_HFSX    0x4858   /* "HX" */
#define SIG_HFS     0x4244   /* "BD" */

#define VOL_ATTR_UNMOUNTED  (1u << 8)
#define VOL_ATTR_JOURNALED  (1u << 13)

#define JIB_JOURNAL_IN_FS     0x1
#define JIB_JOURNAL_ON_OTHER  0x2
#define JIB_JOURNAL_NEED_INIT 0x4

#define JOURNAL_MAGIC  0x4a4e4c78   /* "JNLx" */
#define JOURNAL_ENDIAN 0x12345678

/* GPT partition type GUIDs used as hints */
#define GUID_EFI         "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define GUID_CORESTORAGE "53746F72-6167-11AA-AA11-00306543ECAC"
#define GUID_APFS        "7C3457EF-0000-11AA-AA11-00306543ECAC"

/* implemented in volume.c */
int ehfs_read_volume_name(struct ehfs_blockdev* dev, uint64_t offset, uint64_t length, char* out, size_t size);

static void set(ehfs_volume_info* out, ehfs_fs_type fs, ehfs_support support, const char* reason) {
	out->fs_type = fs;
	out->support = support;
	ehfs_copy_string(out->reason, sizeof(out->reason), reason);
}

/* Is the journal clean? Returns 1 clean, 0 dirty, <0 error/unsupported. */
static int check_journal(struct ehfs_blockdev* dev, uint64_t vol_offset, uint64_t vol_length,
                         uint32_t jib_block, uint32_t block_size, ehfs_volume_info* out) {
	uint8_t jib[512];
	uint64_t jib_pos = (uint64_t)jib_block * block_size;
	if(jib_pos + sizeof(jib) > vol_length || ehfs_blockdev_read(dev, jib, sizeof(jib), vol_offset + jib_pos))
		return -EIO;

	uint32_t flags = ehfs_be32(jib);
	if(flags & JIB_JOURNAL_ON_OTHER) {
		set(out, out->fs_type, EHFS_SUPPORT_UNSUPPORTED, "The journal of this volume is stored on another device");
		return -ENOTSUP;
	}
	if(flags & JIB_JOURNAL_NEED_INIT)
		return 1;   /* journal never used since creation */
	if(!(flags & JIB_JOURNAL_IN_FS))
		return -EIO;

	uint64_t joff = ehfs_be64(jib + 36);
	uint8_t jh[512];
	if(joff + sizeof(jh) > vol_length || ehfs_blockdev_read(dev, jh, sizeof(jh), vol_offset + joff))
		return -EIO;

	/* the journal header is written in the byte order of the Mac that wrote it */
	bool be;
	if(ehfs_be32(jh) == JOURNAL_MAGIC && ehfs_be32(jh + 4) == JOURNAL_ENDIAN)
		be = true;
	else if(ehfs_le32(jh) == JOURNAL_MAGIC && ehfs_le32(jh + 4) == JOURNAL_ENDIAN)
		be = false;
	else
		return -EIO;

	uint64_t start = be ? ehfs_be64(jh + 8) : ehfs_le64(jh + 8);
	uint64_t end = be ? ehfs_be64(jh + 16) : ehfs_le64(jh + 16);
	return start == end;
}

static int probe_hfsplus(struct ehfs_blockdev* dev, uint64_t offset, uint64_t length,
                         const uint8_t* vh, bool read_name, ehfs_volume_info* out) {
	uint16_t sig = ehfs_be16(vh);
	uint16_t version = ehfs_be16(vh + 2);
	uint32_t attributes = ehfs_be32(vh + 4);
	uint32_t block_size = ehfs_be32(vh + 40);
	uint32_t total_blocks = ehfs_be32(vh + 44);
	uint32_t free_blocks = ehfs_be32(vh + 48);

	out->fs_type = sig == SIG_HFSX ? EHFS_FS_HFSX : EHFS_FS_HFSPLUS;
	out->version = version;
	out->block_size = block_size;
	out->case_sensitive = sig == SIG_HFSX;
	out->journaled = !!(attributes & VOL_ATTR_JOURNALED);
	out->unmounted_cleanly = !!(attributes & VOL_ATTR_UNMOUNTED);
	memcpy(out->last_mounted_by, vh + 8, 4);
	for(int i = 0; i < 4; i++)
		if(out->last_mounted_by[i] < 0x20 || out->last_mounted_by[i] > 0x7E)
			out->last_mounted_by[i] = '?';
	out->created = EHFS_HFS_TO_UNIX(ehfs_be32(vh + 16));   /* local time on the Mac that created it */
	out->modified = EHFS_HFS_TO_UNIX(ehfs_be32(vh + 20));
	out->file_count = ehfs_be32(vh + 32);
	out->folder_count = ehfs_be32(vh + 36);

	bool block_size_ok = block_size >= 512 && (block_size & (block_size - 1)) == 0;
	if((sig == SIG_HFSPLUS && version != 4) || (sig == SIG_HFSX && version != 5) || !block_size_ok ||
	   !total_blocks || free_blocks > total_blocks || (uint64_t)total_blocks * block_size > length) {
		set(out, out->fs_type, EHFS_SUPPORT_DAMAGED, "The HFS+ volume header is invalid or the partition is too small for it");
		return 0;
	}
	out->total_bytes = (uint64_t)total_blocks * block_size;
	out->free_bytes = (uint64_t)free_blocks * block_size;

	if(out->journaled) {
		int clean = check_journal(dev, offset, length, ehfs_be32(vh + 12), block_size, out);
		if(clean == -ENOTSUP)
			return 0;
		if(clean < 0) {
			set(out, out->fs_type, EHFS_SUPPORT_DAMAGED, "The HFS+ journal could not be read");
			return 0;
		}
		out->journal_dirty = !clean;
	}

	if(out->journal_dirty)
		set(out, out->fs_type, EHFS_SUPPORT_NEEDS_MAC,
		    "This drive was not ejected properly on a Mac. Connect it to a Mac and eject it, then try again.");
	else if(!out->journaled && !out->unmounted_cleanly)
		set(out, out->fs_type, EHFS_SUPPORT_SUPPORTED,
		    "Not ejected properly on a Mac and not journaled: recent changes may be missing");
	else
		set(out, out->fs_type, EHFS_SUPPORT_SUPPORTED, out->case_sensitive ? "Mac OS Extended (Case-sensitive)" : "Mac OS Extended");

	if(read_name && (out->support == EHFS_SUPPORT_SUPPORTED || out->support == EHFS_SUPPORT_NEEDS_MAC)) {
		int err = ehfs_read_volume_name(dev, offset, length, out->volume_name, sizeof(out->volume_name));
		if(err) {
			set(out, out->fs_type, EHFS_SUPPORT_DAMAGED, "The HFS+ catalog could not be read");
			ehfs_log(EHFS_LOG_WARNING, "probe at offset %llu: catalog unreadable: %s",
			         (unsigned long long)offset, ehfs_strerror(err));
		}
	}
	return 0;
}

int ehfs_probe_range(struct ehfs_blockdev* dev, uint64_t offset, uint64_t length,
                     const char* partition_type, bool read_name, ehfs_volume_info* out) {
	memset(out, 0, sizeof(*out));
	if(offset > dev->size || length > dev->size - offset)
		return -EINVAL;

	if(partition_type && !strcmp(partition_type, GUID_EFI)) {
		set(out, EHFS_FS_EFI, EHFS_SUPPORT_NOT_MAC, "EFI system partition (hidden, no user files)");
		return 0;
	}

	uint8_t buf[2048] = {0};
	size_t want = length < sizeof(buf) ? (size_t)length : sizeof(buf);
	int err = ehfs_blockdev_read(dev, buf, want, offset);
	if(err)
		return err;
	if(want < 1536) {
		set(out, EHFS_FS_UNKNOWN, EHFS_SUPPORT_NOT_MAC, "Too small to hold a volume");
		return 0;
	}

	const uint8_t* vh = buf + 1024;
	uint16_t sig = ehfs_be16(vh);

	if(sig == SIG_HFSPLUS || sig == SIG_HFSX)
		return probe_hfsplus(dev, offset, length, vh, read_name, out);

	if(sig == SIG_HFS) {
		/* An HFS Standard "wrapper" may embed an HFS+ volume (Mac OS 8/9 era). */
		if(ehfs_be16(vh + 0x7C) == SIG_HFSPLUS) {
			uint64_t first_block = ehfs_be16(vh + 0x1C);
			uint64_t alloc_size = ehfs_be32(vh + 0x14);
			uint64_t embedded = first_block * 512 + (uint64_t)ehfs_be16(vh + 0x7E) * alloc_size;
			uint8_t evh[512];
			if(embedded + 1024 + sizeof(evh) <= length &&
			   !ehfs_blockdev_read(dev, evh, sizeof(evh), offset + embedded + 1024) &&
			   ehfs_be16(evh) == SIG_HFSPLUS) {
				/* libhfs locates the embedded volume itself, so the outer range is what gets opened */
				int ret = probe_hfsplus(dev, offset + embedded, length - embedded, evh, false, out);
				out->hfs_wrapper = 1;
				if(!ret && read_name && out->support <= EHFS_SUPPORT_NEEDS_MAC)
					ehfs_read_volume_name(dev, offset, length, out->volume_name, sizeof(out->volume_name));
				return ret;
			}
		}
		set(out, EHFS_FS_HFS_STANDARD, EHFS_SUPPORT_UNSUPPORTED,
		    "Mac OS Standard (HFS) volumes from before 1998 are not supported");
		return 0;
	}

	if(!memcmp(buf + 32, "NXSB", 4) || (partition_type && !strcmp(partition_type, GUID_APFS))) {
		set(out, EHFS_FS_APFS, EHFS_SUPPORT_UNSUPPORTED,
		    "APFS is not supported in this version. Use a Mac to copy the files, or format the drive as Mac OS Extended (Journaled).");
		return 0;
	}
	if(partition_type && !strcmp(partition_type, GUID_CORESTORAGE)) {
		set(out, EHFS_FS_CORESTORAGE, EHFS_SUPPORT_UNSUPPORTED,
		    "Core Storage volumes (Fusion Drive or older FileVault encryption) are not supported");
		return 0;
	}
	if(!memcmp(buf + 3, "NTFS    ", 8)) {
		set(out, EHFS_FS_NTFS, EHFS_SUPPORT_NOT_MAC, "NTFS: Windows reads this natively");
		return 0;
	}
	if(!memcmp(buf + 3, "EXFAT   ", 8)) {
		set(out, EHFS_FS_EXFAT, EHFS_SUPPORT_NOT_MAC, "exFAT: Windows reads this natively");
		return 0;
	}
	if(!memcmp(buf + 82, "FAT32   ", 8) || !memcmp(buf + 54, "FAT12   ", 8) || !memcmp(buf + 54, "FAT16   ", 8)) {
		set(out, EHFS_FS_FAT, EHFS_SUPPORT_NOT_MAC, "FAT: Windows reads this natively");
		return 0;
	}

	bool all_zero = true;
	for(size_t i = 0; i < want && all_zero; i++)
		all_zero = !buf[i];
	set(out, all_zero ? EHFS_FS_EMPTY : EHFS_FS_UNKNOWN, EHFS_SUPPORT_NOT_MAC,
	    all_zero ? "Empty (no file system)" : "Unrecognised file system");
	return 0;
}
