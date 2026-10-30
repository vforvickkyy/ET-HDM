/*
 * Eastape HFS engine: HFS+ volume access built on libhfs + libhfsuser
 * (vendored from hfsfuse). Read-only: libhfs has no write path, and every read
 * is bounds-checked against the partition before it reaches the disk.
 */

#include "internal.h"

#include "hfsuser.h"
#include "libhfs.h"
#include "utf8proc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* hfs_file_pread takes off_t: large files need a 64-bit off_t on every platform */
_Static_assert(sizeof(off_t) == 8, "build with -D_FILE_OFFSET_BITS=64");

#define DEFAULT_LOOKUP_CACHE 4096

#define HFS_FINDER_INVISIBLE 0x4000
#define HFS_UF_IMMUTABLE     0x02
#define HFS_SF_IMMUTABLE     0x02

struct range_io;

struct ehfs_volume {
	struct ehfs_blockdev* dev;   /* borrowed from the ehfs_disk, which must outlive the volume */
	uint64_t offset;
	uint64_t length;
	struct range_io* io;         /* reader handed to libhfs; freed after hfslib_close_volume */
	hfs_volume hv;
	ehfs_volume_info info;
};

struct ehfs_dir {
	struct ehfs_volume* vol;
	hfs_catalog_keyed_record_t* records;
	hfs_unistr255_t* names;
	uint32_t count;
	uint32_t next;
	char* path;        /* directory path with trailing '/', for the lookup cache */
	size_t path_len;
};

struct ehfs_file {
	struct hfs_file* f;
};

/* ---- libhfs glue -------------------------------------------------------- */

struct range_io {
	struct ehfs_blockdev* dev;
	uint64_t offset, length;
};

static int range_read(void* ctx, void* buf, uint64_t length, uint64_t offset) {
	struct range_io* io = ctx;
	if(offset > io->length || length > io->length - offset) {
		ehfs_log(EHFS_LOG_ERROR, "refused read outside the partition (offset %llu, length %llu)",
		         (unsigned long long)offset, (unsigned long long)length);
		return -EIO;
	}
	return ehfs_blockdev_read(io->dev, buf, length, io->offset + offset);
}

static void libhfs_message(const char* message, void* ctx) {
	(void)ctx;
	ehfs_log(EHFS_LOG_ERROR, "libhfs: %s", message);
}

/* Open libhfs on a byte range. `io` must stay valid until hfslib_close_volume. */
static int open_hfs(struct range_io* io, uint32_t cache_size, hfs_volume* hv) {
	hfs_set_error_handler(libhfs_message, NULL);

	struct hfs_volume_config cfg;
	hfs_volume_config_defaults(&cfg);
	cfg.cache_size = cache_size;
	cfg.blksize = io->dev->sector_size;
	cfg.io_ctx = io;
	cfg.io_read = range_read;

	memset(hv, 0, sizeof(*hv));
	int err = hfs_open_volume("ehfs", hv, &cfg);
	if(err)
		return err > 0 ? -err : -EIO;
	return 0;
}

/* Volume names are display-only: return them composed (NFC), as Windows shows text.
 * HFS+ stores names decomposed, e.g. "e" + U+0301 for "é". */
static void volume_display_name(const hfs_volume* hv, char* out, size_t size) {
	char name[HFS_NAME_MAX + 1];
	if(hfs_unistr_to_utf8(&hv->name, name) < 0) {
		ehfs_copy_string(out, size, "");
		return;
	}
	utf8proc_uint8_t* nfc = utf8proc_NFC((const utf8proc_uint8_t*)name);
	ehfs_copy_string(out, size, nfc ? (const char*)nfc : name);
	free(nfc);
}

int ehfs_read_volume_name(struct ehfs_blockdev* dev, uint64_t offset, uint64_t length, char* out, size_t size) {
	struct range_io io = { dev, offset, length };
	hfs_volume hv;
	int err = open_hfs(&io, 16, &hv);
	if(err)
		return err;
	volume_display_name(&hv, out, size);
	hfslib_close_volume(&hv, NULL);
	return 0;
}

/* ---- Volumes ------------------------------------------------------------ */

int ehfs_volume_open(ehfs_disk* disk, uint64_t offset, uint64_t length,
                     const ehfs_open_options* options, ehfs_volume** out) {
	ehfs_open_options defaults = {0};
	if(!options)
		options = &defaults;

	struct ehfs_volume* v = calloc(1, sizeof(*v));
	if(!v)
		return -ENOMEM;
	v->dev = disk->dev;
	v->offset = offset;
	v->length = length;

	int err = ehfs_probe_range(disk->dev, offset, length, NULL, false, &v->info);
	if(err)
		goto fail;
	if(v->info.fs_type != EHFS_FS_HFSPLUS && v->info.fs_type != EHFS_FS_HFSX) {
		err = -EHFS_ENOTHFS;
		goto fail;
	}
	switch(v->info.support) {
		case EHFS_SUPPORT_SUPPORTED:
			break;
		case EHFS_SUPPORT_NEEDS_MAC:
			if(options->allow_dirty_journal) {
				ehfs_log(EHFS_LOG_WARNING, "opening a volume with a dirty journal: recent changes may be missing");
				break;
			}
			err = -EHFS_EDIRTY;
			goto fail;
		case EHFS_SUPPORT_UNSUPPORTED:
			err = -ENOTSUP;
			goto fail;
		default:
			err = -EIO;
			goto fail;
	}

	struct range_io* io = malloc(sizeof(*io));
	if(!io) {
		err = -ENOMEM;
		goto fail;
	}
	*io = (struct range_io){ disk->dev, offset, length };
	err = open_hfs(io, options->lookup_cache_size ? options->lookup_cache_size : DEFAULT_LOOKUP_CACHE, &v->hv);
	if(err) {
		free(io);
		ehfs_log(EHFS_LOG_ERROR, "could not open the HFS+ volume: %s", ehfs_strerror(err));
		goto fail;
	}
	v->io = io;

	volume_display_name(&v->hv, v->info.volume_name, sizeof(v->info.volume_name));
	v->info.case_sensitive = v->hv.chr.keycomp_type == HFS_KEY_BINARY;

	*out = v;
	return 0;

fail:
	free(v);
	return err;
}

void ehfs_volume_close(ehfs_volume* vol) {
	if(!vol)
		return;
	hfslib_close_volume(&vol->hv, NULL);
	free(vol->io);
	free(vol);
}

int ehfs_volume_get_info(ehfs_volume* vol, ehfs_volume_info* out) {
	*out = vol->info;
	return 0;
}

/* ---- Metadata ----------------------------------------------------------- */

static void fill_stat(struct ehfs_volume* v, hfs_catalog_keyed_record_t* rec, const char* name,
                      uint32_t extra_flags, ehfs_stat* st) {
	memset(st, 0, sizeof(*st));
	/* file and folder records share their leading fields (TN1150), as libhfsuser relies on too */
	hfs_file_record_t* f = &rec->file;
	st->id = f->cnid;
	st->flags = extra_flags;
	st->created = EHFS_HFS_TO_UNIX(f->date_created);
	st->modified = EHFS_HFS_TO_UNIX(f->date_content_mod);
	st->changed = EHFS_HFS_TO_UNIX(f->date_attrib_mod);
	st->accessed = EHFS_HFS_TO_UNIX(f->date_accessed);
	st->uid = f->bsd.owner_id;
	st->gid = f->bsd.group_id;

	uint16_t fmt = f->bsd.file_mode & HFS_S_IFMT;
	if(rec->type == HFS_REC_FLDR) {
		st->type = EHFS_TYPE_DIR;
		st->mode = fmt ? f->bsd.file_mode & 07777 : 0777;
		st->entries = rec->folder.valence;
		st->nlink = rec->folder.valence + 2;
	}
	else {
		st->type = fmt == HFS_S_IFLNK ? EHFS_TYPE_SYMLINK
		         : (fmt == HFS_S_IFREG || fmt == 0) ? EHFS_TYPE_FILE : EHFS_TYPE_OTHER;
		st->mode = fmt ? f->bsd.file_mode & 07777 : 0755;
		st->nlink = f->bsd.special.link_count ? f->bsd.special.link_count : 1;
		st->size = f->data_fork.logical_size;
		st->rsrc_size = f->rsrc_fork.logical_size;
		st->allocated = ((uint64_t)f->data_fork.total_blocks + f->rsrc_fork.total_blocks) * v->hv.vh.block_size;

		if(f->bsd.owner_flags & HFS_UF_COMPRESSED) {
			struct hfs_decmpfs_header h;
			st->flags |= EHFS_FLAG_COMPRESSED;
			if(!hfs_decmpfs_lookup(&v->hv, f, &h, NULL, NULL)) {
				st->size = h.logical_size;
				st->rsrc_size = 0;   /* the resource fork holds the compressed data */
			}
		}
	}

	if((f->user_info.finder_flags & HFS_FINDER_INVISIBLE) || (name && name[0] == '.'))
		st->flags |= EHFS_FLAG_HIDDEN;
	if((f->bsd.owner_flags & HFS_UF_IMMUTABLE) || (f->bsd.admin_flags & HFS_SF_IMMUTABLE))
		st->flags |= EHFS_FLAG_LOCKED;
}

static const char* basename_of(const char* path) {
	const char* slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

int ehfs_stat_path(ehfs_volume* vol, const char* path, ehfs_stat* out) {
	hfs_catalog_keyed_record_t rec;
	int err = hfs_lookup(&vol->hv, path, &rec, NULL, NULL);
	if(err)
		return err < 0 ? err : -err;
	fill_stat(vol, &rec, basename_of(path), 0, out);
	return 0;
}

/* ---- Directories -------------------------------------------------------- */

int ehfs_dir_open(ehfs_volume* vol, const char* path, ehfs_dir** out) {
	hfs_catalog_keyed_record_t rec;
	int err = hfs_lookup(&vol->hv, path, &rec, NULL, NULL);
	if(err)
		return err < 0 ? err : -err;
	if(rec.type != HFS_REC_FLDR)
		return -ENOTDIR;

	struct ehfs_dir* d = calloc(1, sizeof(*d));
	if(!d)
		return -ENOMEM;
	d->vol = vol;

	size_t len = strlen(path);
	while(len > 1 && path[len - 1] == '/')
		len--;
	d->path = malloc(len + 2);
	if(!d->path) {
		free(d);
		return -ENOMEM;
	}
	memcpy(d->path, path, len);
	if(len == 0 || d->path[len - 1] != '/')
		d->path[len++] = '/';
	d->path[len] = '\0';
	d->path_len = len;

	if(hfslib_get_directory_contents(&vol->hv, rec.folder.cnid, &d->records, &d->names, &d->count, NULL)) {
		ehfs_log(EHFS_LOG_ERROR, "could not list directory %s", path);
		free(d->path);
		free(d);
		return -EIO;
	}
	*out = d;
	return 0;
}

int ehfs_dir_next(ehfs_dir* d, char* name, ehfs_stat* st) {
	while(d->next < d->count) {
		uint32_t i = d->next++;
		if(hfs_pathname_to_unix(&d->names[i], name) < 0) {
			ehfs_log(EHFS_LOG_ERROR, "could not convert a file name in %s (CNID %u), skipped",
			         d->path, (unsigned)d->records[i].file.cnid);
			continue;
		}

		hfs_catalog_keyed_record_t* rec = &d->records[i];
		hfs_catalog_keyed_record_t link;
		uint32_t flags = 0;
		if(rec->type == HFS_REC_FILE) {
			hfs_file_record_t* f = &rec->file;
			if(f->user_info.file_creator == HFS_HFSPLUS_CREATOR && f->user_info.file_type == HFS_HARD_LINK_FILE_TYPE) {
				if(!hfslib_get_hardlink(&d->vol->hv, f->bsd.special.inode_num, &link, NULL)) {
					rec = &link;
					flags = EHFS_FLAG_HARDLINK;
				}
			}
			else if(f->user_info.file_creator == HFS_MACS_CREATOR && f->user_info.file_type == HFS_DIR_HARD_LINK_FILE_TYPE) {
				if(!hfslib_get_directory_hardlink(&d->vol->hv, f->bsd.special.inode_num, &link, NULL)) {
					rec = &link;
					flags = EHFS_FLAG_HARDLINK;
				}
			}
		}

		size_t name_len = strlen(name);
		char* full = malloc(d->path_len + name_len + 1);
		if(full) {
			memcpy(full, d->path, d->path_len);
			memcpy(full + d->path_len, name, name_len + 1);
			hfs_cache_path(&d->vol->hv, full, d->path_len + name_len, rec);
			free(full);
		}

		if(st)
			fill_stat(d->vol, rec, name, flags, st);
		return 1;
	}
	return 0;
}

void ehfs_dir_close(ehfs_dir* d) {
	if(!d)
		return;
	free(d->records);
	free(d->names);
	free(d->path);
	free(d);
}

/* ---- Files -------------------------------------------------------------- */

int ehfs_file_open(ehfs_volume* vol, const char* path, ehfs_file** out) {
	int err = 0;
	struct hfs_file* f = hfs_file_open_path(&vol->hv, path, &err);
	if(!f)
		return err < 0 ? err : -EIO;
	hfs_catalog_keyed_record_t rec = hfs_file_get_catalog_record(f);
	if(rec.type != HFS_REC_FILE) {
		hfs_file_close(f);
		return -EISDIR;
	}
	struct ehfs_file* ef = malloc(sizeof(*ef));
	if(!ef) {
		hfs_file_close(f);
		return -ENOMEM;
	}
	ef->f = f;
	*out = ef;
	return 0;
}

int64_t ehfs_file_read(ehfs_file* file, void* buf, size_t size, uint64_t offset) {
	if(offset > INT64_MAX)
		return -EINVAL;
	ssize_t n = hfs_file_pread(file->f, buf, size, (off_t)offset);
	return n;
}

void ehfs_file_close(ehfs_file* file) {
	if(!file)
		return;
	hfs_file_close(file->f);
	free(file);
}

int ehfs_readlink(ehfs_volume* vol, const char* path, char* buf, size_t size) {
	if(size < 2)
		return -EINVAL;
	hfs_catalog_keyed_record_t rec;
	int err = hfs_lookup(&vol->hv, path, &rec, NULL, NULL);
	if(err)
		return err < 0 ? err : -err;
	if(rec.type != HFS_REC_FILE || (rec.file.bsd.file_mode & HFS_S_IFMT) != HFS_S_IFLNK)
		return -EINVAL;

	struct hfs_file* f = hfs_file_open(&vol->hv, &rec, HFS_DATAFORK, &err);
	if(!f)
		return err < 0 ? err : -EIO;
	ssize_t n = hfs_file_pread(f, buf, size - 1, 0);
	hfs_file_close(f);
	if(n < 0)
		return (int)n;
	buf[n] = '\0';
	return 0;
}
