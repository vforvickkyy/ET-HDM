/*
 * ehfs: command-line access to the Eastape HFS engine (read-only).
 *
 *   ehfs scan  <disk> [-t]                 list partitions and what they contain (-t: tab-separated)
 *   ehfs info  <disk> [-p N]               HFS+ volume details
 *   ehfs ls    <disk> [-p N] [-R] [path]   list a directory
 *   ehfs cat   <disk> [-p N] <path>        write a file to standard output
 *   ehfs hash  <disk> [-p N] [path]        SHA-256 of every file below path
 *
 * <disk> is an image file or a device ("/dev/sdb", "\\.\PhysicalDrive2").
 * -p selects a partition number as shown by "scan"; by default the first
 * mountable HFS+ volume is used. --allow-dirty opens volumes whose journal has
 * unsaved changes.
 */

#include "ehfs.h"
#include "sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

/* Windows passes argv in the ANSI code page; rebuild it as UTF-8 from the
 * UTF-16 command line so non-English paths work. */
static char** utf8_argv(int* argc) {
	wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), argc);
	if(!wargv)
		return NULL;
	char** argv = calloc((size_t)*argc + 1, sizeof(char*));
	for(int i = 0; argv && i < *argc; i++) {
		int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
		if(n <= 0 || !(argv[i] = malloc((size_t)n)))
			return NULL;
		WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
	}
	LocalFree(wargv);
	return argv;
}
#endif

#define READ_CHUNK (1u << 20)

static const char* scheme_name(ehfs_scheme s) {
	switch(s) {
		case EHFS_SCHEME_GPT: return "GPT";
		case EHFS_SCHEME_MBR: return "MBR";
		case EHFS_SCHEME_APM: return "APM";
		default:              return "none";
	}
}

static const char* fs_name(ehfs_fs_type t) {
	switch(t) {
		case EHFS_FS_HFSPLUS:      return "HFS+";
		case EHFS_FS_HFSX:         return "HFSX";
		case EHFS_FS_HFS_STANDARD: return "HFS";
		case EHFS_FS_APFS:         return "APFS";
		case EHFS_FS_CORESTORAGE:  return "CoreStorage";
		case EHFS_FS_EFI:          return "EFI";
		case EHFS_FS_NTFS:         return "NTFS";
		case EHFS_FS_EXFAT:        return "exFAT";
		case EHFS_FS_FAT:          return "FAT";
		case EHFS_FS_EMPTY:        return "empty";
		case EHFS_FS_OTHER:        return "other";
		default:                   return "unknown";
	}
}

static const char* support_name(ehfs_support s) {
	switch(s) {
		case EHFS_SUPPORT_SUPPORTED:   return "supported";
		case EHFS_SUPPORT_NEEDS_MAC:   return "needs-mac";
		case EHFS_SUPPORT_UNSUPPORTED: return "unsupported";
		case EHFS_SUPPORT_NOT_MAC:     return "not-mac";
		default:                       return "damaged";
	}
}

static void human_size(uint64_t bytes, char out[32]) {
	const char* units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
	double v = (double)bytes;
	int u = 0;
	while(v >= 1000 && u < 5) {
		v /= 1000;
		u++;
	}
	snprintf(out, 32, u ? "%.1f %s" : "%.0f %s", v, units[u]);
}

static void log_to_stderr(int level, const char* message, void* ctx) {
	(void)ctx;
	static const char* const names[] = { "debug", "info", "warning", "error" };
	if(level >= EHFS_LOG_WARNING)
		fprintf(stderr, "%s: %s\n", names[level & 3], message);
}

struct options {
	const char* command;
	const char* disk;
	const char* path;
	long partition;     /* -1 = automatic */
	int recursive;
	int allow_dirty;
	int tabular;
};

static int usage(void) {
	fprintf(stderr,
	        "Eastape HFS engine %s (read-only)\n"
	        "usage: ehfs scan <disk> [-t]\n"
	        "       ehfs info <disk> [-p N] [--allow-dirty]\n"
	        "       ehfs ls   <disk> [-p N] [-R] [path]\n"
	        "       ehfs cat  <disk> [-p N] <path>\n"
	        "       ehfs hash <disk> [-p N] [path]\n",
	        ehfs_version());
	return 2;
}

static int parse_args(int argc, char** argv, struct options* o) {
	memset(o, 0, sizeof(*o));
	o->partition = -1;
	int positional = 0;
	for(int i = 1; i < argc; i++) {
		const char* a = argv[i];
		if(!strcmp(a, "-p") && i + 1 < argc)
			o->partition = strtol(argv[++i], NULL, 10);
		else if(!strcmp(a, "-R"))
			o->recursive = 1;
		else if(!strcmp(a, "--allow-dirty"))
			o->allow_dirty = 1;
		else if(!strcmp(a, "-t"))
			o->tabular = 1;
		else if(a[0] == '-' && a[1])
			return -1;
		else if(positional == 0) { o->command = a; positional++; }
		else if(positional == 1) { o->disk = a; positional++; }
		else if(positional == 2) { o->path = a; positional++; }
		else return -1;
	}
	return o->command && o->disk ? 0 : -1;
}

static int cmd_scan(ehfs_disk* disk, int tabular) {
	ehfs_disk_layout* layout = malloc(sizeof(*layout));
	if(!layout)
		return 1;
	int err = ehfs_disk_scan(disk, layout);
	if(err) {
		fprintf(stderr, "scan failed: %s\n", ehfs_strerror(err));
		free(layout);
		return 1;
	}
	if(tabular) {
		/* one line per partition, tab-separated, for scripts and tests */
		for(uint32_t i = 0; i < layout->count; i++) {
			const ehfs_partition* p = &layout->partitions[i];
			const ehfs_volume_info* v = &p->volume;
			printf("%s\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu64 "\t%" PRIu64 "\t%s\t%s\t%s\t%d\t%d\t%d\t%s\n",
			       scheme_name(layout->scheme), layout->sector_size, p->number, p->offset, p->length, p->type,
			       fs_name(v->fs_type), support_name(v->support), v->journaled, v->journal_dirty,
			       v->case_sensitive, v->volume_name);
		}
		free(layout);
		return 0;
	}
	char size[32];
	human_size(layout->size, size);
	printf("disk: %s, sector %" PRIu32 " bytes, scheme %s, %" PRIu32 " partition(s)\n",
	       size, layout->sector_size, scheme_name(layout->scheme), layout->count);
	for(uint32_t i = 0; i < layout->count; i++) {
		const ehfs_partition* p = &layout->partitions[i];
		const ehfs_volume_info* v = &p->volume;
		human_size(p->length, size);
		printf("  #%" PRIu32 "  offset=%" PRIu64 "  %s  type=%s%s%s%s\n", p->number, p->offset, size, p->type,
		       p->name[0] ? "  label=\"" : "", p->name, p->name[0] ? "\"" : "");
		printf("      fs=%s  status=%s", fs_name(v->fs_type), support_name(v->support));
		if(v->volume_name[0])
			printf("  name=\"%s\"", v->volume_name);
		if(v->fs_type == EHFS_FS_HFSPLUS || v->fs_type == EHFS_FS_HFSX)
			printf("  journaled=%s  dirty=%s", v->journaled ? "yes" : "no", v->journal_dirty ? "yes" : "no");
		printf("\n      %s\n", v->reason);
	}
	free(layout);
	return 0;
}

/* Find the partition to use: -p N, or the first mountable HFS+ volume. */
static int pick_volume(ehfs_disk* disk, const struct options* o, uint64_t* offset, uint64_t* length) {
	ehfs_disk_layout* layout = malloc(sizeof(*layout));
	if(!layout)
		return -ENOMEM;
	int err = ehfs_disk_scan(disk, layout);
	if(err) {
		free(layout);
		return err;
	}
	err = -ENOENT;
	for(uint32_t i = 0; i < layout->count; i++) {
		const ehfs_partition* p = &layout->partitions[i];
		bool hfs = p->volume.fs_type == EHFS_FS_HFSPLUS || p->volume.fs_type == EHFS_FS_HFSX;
		if(o->partition >= 0 ? p->number == (uint32_t)o->partition
		                     : hfs && (p->volume.support == EHFS_SUPPORT_SUPPORTED ||
		                               (o->allow_dirty && p->volume.support == EHFS_SUPPORT_NEEDS_MAC))) {
			*offset = p->offset;
			*length = p->length;
			err = 0;
			break;
		}
	}
	if(err && o->partition < 0) {
		/* nothing mountable: report the first HFS+ volume's reason, if any */
		for(uint32_t i = 0; i < layout->count; i++)
			if(layout->partitions[i].volume.fs_type == EHFS_FS_HFSPLUS || layout->partitions[i].volume.fs_type == EHFS_FS_HFSX) {
				fprintf(stderr, "%s\n", layout->partitions[i].volume.reason);
				break;
			}
	}
	free(layout);
	return err;
}

static int open_volume(ehfs_disk* disk, const struct options* o, ehfs_volume** vol) {
	uint64_t offset, length;
	int err = pick_volume(disk, o, &offset, &length);
	if(err) {
		fprintf(stderr, "no usable HFS+ volume found\n");
		return err;
	}
	ehfs_open_options opts = { .allow_dirty_journal = o->allow_dirty };
	err = ehfs_volume_open(disk, offset, length, &opts, vol);
	if(err)
		fprintf(stderr, "cannot open volume: %s\n", ehfs_strerror(err));
	return err;
}

static int cmd_info(ehfs_volume* vol) {
	ehfs_volume_info v;
	ehfs_volume_get_info(vol, &v);
	char total[32], freeb[32];
	human_size(v.total_bytes, total);
	human_size(v.free_bytes, freeb);
	printf("name:            %s\n", v.volume_name);
	printf("format:          %s%s\n", fs_name(v.fs_type), v.case_sensitive ? " (case-sensitive)" : "");
	printf("capacity:        %s (%s free)\n", total, freeb);
	printf("block size:      %" PRIu32 "\n", v.block_size);
	printf("files/folders:   %" PRIu32 " / %" PRIu32 "\n", v.file_count, v.folder_count);
	printf("journaled:       %s%s\n", v.journaled ? "yes" : "no", v.journal_dirty ? " (dirty)" : "");
	printf("clean unmount:   %s\n", v.unmounted_cleanly ? "yes" : "no");
	printf("last mounted by: %s\n", v.last_mounted_by);
	return 0;
}

static const char* type_char(ehfs_type t) {
	return t == EHFS_TYPE_DIR ? "d" : t == EHFS_TYPE_SYMLINK ? "l" : t == EHFS_TYPE_FILE ? "-" : "?";
}

static int join_path(char* out, size_t size, const char* dir, const char* name) {
	size_t dl = strlen(dir);
	int n = snprintf(out, size, "%s%s%s", dir, (dl && dir[dl - 1] == '/') ? "" : "/", name);
	return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}

static int list_dir(ehfs_volume* vol, const char* path, int recursive) {
	ehfs_dir* d;
	int err = ehfs_dir_open(vol, path, &d);
	if(err) {
		fprintf(stderr, "%s: %s\n", path, ehfs_strerror(err));
		return err;
	}
	char name[EHFS_NAME_MAX + 1];
	ehfs_stat st;
	int ret = 0, r;
	while((r = ehfs_dir_next(d, name, &st)) > 0) {
		char full[4096];
		if(join_path(full, sizeof(full), path, name))
			continue;
		printf("%s %04o %12" PRIu64 " %s%s%s\n", type_char(st.type), (unsigned)st.mode, st.size, full,
		       (st.flags & EHFS_FLAG_HIDDEN) ? "  [hidden]" : "", (st.flags & EHFS_FLAG_COMPRESSED) ? "  [compressed]" : "");
		if(recursive && st.type == EHFS_TYPE_DIR) {
			int e = list_dir(vol, full, 1);
			if(e)
				ret = e;
		}
	}
	if(r < 0)
		ret = r;
	ehfs_dir_close(d);
	return ret;
}

static int read_file(ehfs_volume* vol, const char* path, FILE* out, sha256_ctx* hash, uint64_t* total) {
	ehfs_file* f;
	int err = ehfs_file_open(vol, path, &f);
	if(err)
		return err;
	char* buf = malloc(READ_CHUNK);
	if(!buf) {
		ehfs_file_close(f);
		return -ENOMEM;
	}
	uint64_t offset = 0;
	int64_t n;
	while((n = ehfs_file_read(f, buf, READ_CHUNK, offset)) > 0) {
		if(out && fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
			n = -EIO;
			break;
		}
		if(hash)
			sha256_update(hash, buf, (size_t)n);
		offset += (uint64_t)n;
	}
	free(buf);
	ehfs_file_close(f);
	if(total)
		*total = offset;
	return n < 0 ? (int)n : 0;
}

static int hash_tree(ehfs_volume* vol, const char* path) {
	ehfs_dir* d;
	int err = ehfs_dir_open(vol, path, &d);
	if(err) {
		fprintf(stderr, "%s: %s\n", path, ehfs_strerror(err));
		return err;
	}
	char name[EHFS_NAME_MAX + 1];
	ehfs_stat st;
	int ret = 0, r;
	while((r = ehfs_dir_next(d, name, &st)) > 0) {
		char full[4096];
		if(join_path(full, sizeof(full), path, name))
			continue;
		if(st.type == EHFS_TYPE_DIR) {
			int e = hash_tree(vol, full);
			if(e)
				ret = e;
			continue;
		}
		if(st.type != EHFS_TYPE_FILE)
			continue;
		sha256_ctx c;
		sha256_init(&c);
		uint64_t total = 0;
		int e = read_file(vol, full, NULL, &c, &total);
		if(e) {
			fprintf(stderr, "%s: %s\n", full, ehfs_strerror(e));
			ret = e;
			continue;
		}
		if(total != st.size) {
			fprintf(stderr, "%s: read %" PRIu64 " bytes, expected %" PRIu64 "\n", full, total, st.size);
			ret = -EIO;
		}
		uint8_t digest[32];
		sha256_final(&c, digest);
		for(int i = 0; i < 32; i++)
			printf("%02x", digest[i]);
		printf("  %s\n", full + 1);   /* path relative to the volume root */
	}
	if(r < 0)
		ret = r;
	ehfs_dir_close(d);
	return ret;
}

int main(int argc, char** argv) {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
	char** wide_argv = utf8_argv(&argc);
	if(wide_argv)
		argv = wide_argv;
#endif
	struct options o;
	if(parse_args(argc, argv, &o))
		return usage();
	ehfs_set_log_handler(log_to_stderr, NULL);

	ehfs_disk* disk;
	int err = ehfs_disk_open(o.disk, &disk);
	if(err) {
		fprintf(stderr, "%s: %s\n", o.disk, ehfs_strerror(err));
		return 1;
	}

	int ret = 0;
	if(!strcmp(o.command, "scan")) {
		ret = cmd_scan(disk, o.tabular);
	}
	else {
		ehfs_volume* vol;
		if(open_volume(disk, &o, &vol)) {
			ehfs_disk_close(disk);
			return 1;
		}
		const char* path = o.path ? o.path : "/";
		if(!strcmp(o.command, "info"))
			ret = cmd_info(vol);
		else if(!strcmp(o.command, "ls"))
			ret = list_dir(vol, path, o.recursive) ? 1 : 0;
		else if(!strcmp(o.command, "hash"))
			ret = hash_tree(vol, path) ? 1 : 0;
		else if(!strcmp(o.command, "cat") && o.path) {
#ifdef _WIN32
			_setmode(_fileno(stdout), _O_BINARY);
#endif
			int e = read_file(vol, path, stdout, NULL, NULL);
			if(e)
				fprintf(stderr, "%s: %s\n", path, ehfs_strerror(e));
			ret = e ? 1 : 0;
		}
		else
			ret = usage();
		ehfs_volume_close(vol);
	}
	ehfs_disk_close(disk);
	return ret;
}
