/*
 * Eastape HFS engine: partition table scanner.
 *
 * Supports GUID Partition Table (Intel Macs, Windows), Apple Partition Map
 * (PowerPC Macs, some disk images and hybrid ISOs) and MBR. Windows does not
 * understand APM at all, so this scanner is the only way such drives are seen.
 * Every partition found is probed with ehfs_probe_range().
 */

#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define GPT_MAX_ENTRIES 4096

static uint32_t crc32_ieee(const uint8_t* p, size_t n) {
	uint32_t crc = 0xFFFFFFFFu;
	while(n--) {
		crc ^= *p++;
		for(int k = 0; k < 8; k++)
			crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

static void guid_to_string(const uint8_t* g, char out[37]) {
	static const char hex[] = "0123456789ABCDEF";
	/* first three fields little-endian, last two big-endian */
	static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
	int o = 0;
	for(int i = 0; i < 16; i++) {
		if(i == 4 || i == 6 || i == 8 || i == 10)
			out[o++] = '-';
		out[o++] = hex[g[order[i]] >> 4];
		out[o++] = hex[g[order[i]] & 15];
	}
	out[o] = '\0';
}

static ehfs_partition* add_partition(ehfs_disk_layout* out, uint64_t offset, uint64_t length) {
	if(out->count >= EHFS_MAX_PARTITIONS) {
		ehfs_log(EHFS_LOG_WARNING, "more than %d partitions, ignoring the rest", EHFS_MAX_PARTITIONS);
		return NULL;
	}
	ehfs_partition* p = &out->partitions[out->count++];
	memset(p, 0, sizeof(*p));
	p->number = out->count;
	p->offset = offset;
	p->length = length;
	return p;
}

/* ---- GPT ---------------------------------------------------------------- */

/* Parse a GPT header at byte `pos` using logical block size `lbs`. 1 = found, 0 = none, <0 = error */
static int read_gpt(struct ehfs_blockdev* dev, uint64_t pos, uint32_t lbs, ehfs_disk_layout* out) {
	uint8_t hdr[512];
	if(pos + sizeof(hdr) > dev->size || ehfs_blockdev_read(dev, hdr, sizeof(hdr), pos))
		return 0;
	if(memcmp(hdr, "EFI PART", 8))
		return 0;

	uint32_t hdr_size = ehfs_le32(hdr + 12);
	if(hdr_size < 92 || hdr_size > sizeof(hdr))
		return -EINVAL;
	uint32_t stored_crc = ehfs_le32(hdr + 16);
	memset(hdr + 16, 0, 4);
	if(crc32_ieee(hdr, hdr_size) != stored_crc) {
		ehfs_log(EHFS_LOG_WARNING, "GPT header at byte %llu has a bad checksum", (unsigned long long)pos);
		return -EINVAL;
	}

	uint64_t entries_lba = ehfs_le64(hdr + 72);
	uint32_t count = ehfs_le32(hdr + 80);
	uint32_t entry_size = ehfs_le32(hdr + 84);
	uint32_t entries_crc = ehfs_le32(hdr + 88);
	if(count > GPT_MAX_ENTRIES || entry_size < 128 || entry_size > 1024 || entry_size % 8)
		return -EINVAL;

	size_t table_size = (size_t)count * entry_size;
	uint64_t table_pos = entries_lba * lbs;
	if(table_pos > dev->size || table_size > dev->size - table_pos)
		return -EINVAL;
	uint8_t* table = malloc(table_size ? table_size : 1);
	if(!table)
		return -ENOMEM;
	int err = ehfs_blockdev_read(dev, table, table_size, table_pos);
	if(err) {
		free(table);
		return err;
	}
	if(crc32_ieee(table, table_size) != entries_crc) {
		ehfs_log(EHFS_LOG_WARNING, "GPT partition entries have a bad checksum");
		free(table);
		return -EINVAL;
	}

	out->scheme = EHFS_SCHEME_GPT;
	out->sector_size = lbs;
	for(uint32_t i = 0; i < count; i++) {
		const uint8_t* e = table + (size_t)i * entry_size;
		static const uint8_t zero[16];
		if(!memcmp(e, zero, 16))
			continue;
		uint64_t first = ehfs_le64(e + 32), last = ehfs_le64(e + 40);
		if(last < first || last >= dev->size / lbs) {
			ehfs_log(EHFS_LOG_WARNING, "GPT entry %u points outside the disk, skipped", i + 1);
			continue;
		}
		ehfs_partition* p = add_partition(out, first * lbs, (last - first + 1) * lbs);
		if(!p)
			break;
		p->number = i + 1;
		guid_to_string(e, p->type);
		ehfs_utf16_to_utf8(e + 56, 36, false, p->name, sizeof(p->name));
	}
	free(table);
	return 1;
}

static int scan_gpt(struct ehfs_blockdev* dev, ehfs_disk_layout* out) {
	uint32_t sizes[3] = { dev->sector_size, 512, 4096 };
	for(int i = 0; i < 3; i++) {
		uint32_t lbs = sizes[i];
		if(i && lbs == dev->sector_size)
			continue;
		int r = read_gpt(dev, lbs, lbs, out);
		if(r == 1)
			return 1;
		if(r < 0) {
			/* primary header damaged: try the backup copy in the last block */
			uint64_t last = dev->size / lbs - 1;
			out->count = 0;
			if(read_gpt(dev, last * lbs, lbs, out) == 1) {
				ehfs_log(EHFS_LOG_WARNING, "using the backup GPT");
				return 1;
			}
			out->count = 0;
			return r;
		}
	}
	return 0;
}

/* ---- Apple Partition Map ------------------------------------------------ */

static int read_apm(struct ehfs_blockdev* dev, uint32_t blk, ehfs_disk_layout* out) {
	uint8_t e[512];
	if(2 * (uint64_t)blk > dev->size || ehfs_blockdev_read(dev, e, sizeof(e), blk))
		return 0;
	if(ehfs_be16(e) != 0x504D)   /* "PM" */
		return 0;
	uint32_t entries = ehfs_be32(e + 4);
	if(!entries || entries > EHFS_MAX_PARTITIONS)
		return 0;

	out->scheme = EHFS_SCHEME_APM;
	for(uint32_t i = 1; i <= entries; i++) {
		uint64_t pos = (uint64_t)i * blk;
		if(pos + sizeof(e) > dev->size || ehfs_blockdev_read(dev, e, sizeof(e), pos) || ehfs_be16(e) != 0x504D)
			break;
		char type[33] = {0}, name[33] = {0};
		memcpy(name, e + 16, 32);
		memcpy(type, e + 48, 32);
		if(!strcmp(type, "Apple_partition_map") || !strcmp(type, "Apple_Free") || !strcmp(type, "Apple_Void"))
			continue;
		uint64_t start = (uint64_t)ehfs_be32(e + 8) * blk;
		uint64_t length = (uint64_t)ehfs_be32(e + 12) * blk;
		if(start > dev->size)
			continue;
		if(length > dev->size - start)
			length = dev->size - start;   /* images are often truncated after the last partition */
		ehfs_partition* p = add_partition(out, start, length);
		if(!p)
			break;
		p->number = i;
		ehfs_copy_string(p->type, sizeof(p->type), type);
		ehfs_copy_string(p->name, sizeof(p->name), name);
	}
	return 1;
}

static int scan_apm(struct ehfs_blockdev* dev, ehfs_disk_layout* out) {
	uint8_t ddm[512];
	if(ehfs_blockdev_read(dev, ddm, sizeof(ddm), 0))
		return 0;
	uint32_t blk = 512;
	if(ehfs_be16(ddm) == 0x4552) {   /* "ER": driver descriptor map gives the block size */
		uint16_t bs = ehfs_be16(ddm + 2);
		if(bs == 512 || bs == 1024 || bs == 2048 || bs == 4096)
			blk = bs;
	}
	if(read_apm(dev, blk, out))
		return 1;
	return blk != 512 ? read_apm(dev, 512, out) : 0;
}

/* ---- MBR ---------------------------------------------------------------- */

static bool looks_like_boot_sector(const uint8_t* s) {
	/* a volume boot record (FAT/NTFS/exFAT superfloppy) also ends in 55 AA */
	return (s[0] == 0xEB || s[0] == 0xE9) &&
	       (!memcmp(s + 3, "NTFS", 4) || !memcmp(s + 3, "EXFAT", 5) || !memcmp(s + 3, "MSDOS", 5) ||
	        !memcmp(s + 3, "MSWIN", 5) || !memcmp(s + 54, "FAT", 3) || !memcmp(s + 82, "FAT32", 5));
}

static void mbr_type(uint8_t t, char out[40]) {
	static const char hex[] = "0123456789ABCDEF";
	out[0] = '0'; out[1] = 'x'; out[2] = hex[t >> 4]; out[3] = hex[t & 15]; out[4] = '\0';
}

static int scan_mbr(struct ehfs_blockdev* dev, const uint8_t* s, ehfs_disk_layout* out) {
	if(s[510] != 0x55 || s[511] != 0xAA || looks_like_boot_sector(s))
		return 0;
	for(int i = 0; i < 4; i++) {
		uint8_t boot = s[446 + 16 * i];
		if(boot != 0 && boot != 0x80)
			return 0;
	}

	const uint64_t ss = dev->sector_size;
	out->scheme = EHFS_SCHEME_MBR;
	for(int i = 0; i < 4; i++) {
		const uint8_t* e = s + 446 + 16 * i;
		uint8_t type = e[4];
		uint64_t start = ehfs_le32(e + 8) * ss, length = ehfs_le32(e + 12) * ss;
		if(!type || !length)
			continue;
		if(type == 0x05 || type == 0x0F || type == 0x85) {
			/* walk the extended partition chain */
			uint64_t ext_base = start, next = start;
			for(int guard = 0; guard < 64 && next; guard++) {
				uint8_t ebr[512];
				if(next + sizeof(ebr) > dev->size || ehfs_blockdev_read(dev, ebr, sizeof(ebr), next) ||
				   ebr[510] != 0x55 || ebr[511] != 0xAA)
					break;
				const uint8_t* l = ebr + 446;
				uint64_t lstart = next + ehfs_le32(l + 8) * ss, llen = ehfs_le32(l + 12) * ss;
				if(l[4] && llen && lstart < dev->size) {
					ehfs_partition* p = add_partition(out, lstart, llen > dev->size - lstart ? dev->size - lstart : llen);
					if(p)
						mbr_type(l[4], p->type);
				}
				const uint8_t* n = ebr + 446 + 16;
				next = (n[4] == 0x05 || n[4] == 0x0F) ? ext_base + ehfs_le32(n + 8) * ss : 0;
			}
			continue;
		}
		if(start >= dev->size)
			continue;
		ehfs_partition* p = add_partition(out, start, length > dev->size - start ? dev->size - start : length);
		if(!p)
			break;
		p->number = (uint32_t)i + 1;
		mbr_type(type, p->type);
	}
	return 1;
}

/* ---- Entry point -------------------------------------------------------- */

int ehfs_scan_partitions(struct ehfs_blockdev* dev, ehfs_disk_layout* out) {
	memset(out, 0, sizeof(*out));
	out->size = dev->size;
	out->sector_size = dev->sector_size;

	uint8_t s0[512];
	int err = ehfs_blockdev_read(dev, s0, sizeof(s0), 0);
	if(err)
		return err;

	bool protective = false;
	if(s0[510] == 0x55 && s0[511] == 0xAA)
		for(int i = 0; i < 4; i++)
			protective |= s0[446 + 16 * i + 4] == 0xEE;

	int found = scan_gpt(dev, out);
	if(found < 0 || (!found && protective)) {
		ehfs_log(EHFS_LOG_ERROR, "the disk has a GUID partition table, but it is damaged");
		out->scheme = EHFS_SCHEME_GPT;
		out->count = 0;
		return 0;
	}
	if(!found)
		found = scan_apm(dev, out);
	if(!found)
		found = scan_mbr(dev, s0, out);
	if(!found) {
		out->scheme = EHFS_SCHEME_NONE;
		ehfs_partition* p = add_partition(out, 0, dev->size);
		p->number = 0;
	}

	for(uint32_t i = 0; i < out->count; i++) {
		ehfs_partition* p = &out->partitions[i];
		ehfs_probe_range(dev, p->offset, p->length, out->scheme == EHFS_SCHEME_GPT ? p->type : NULL,
		                 true, &p->volume);
	}
	return 0;
}
