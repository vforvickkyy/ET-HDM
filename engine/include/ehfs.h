/*
 * Eastape HFS engine: read-only access to Mac HFS+ / HFSX volumes.
 *
 * Plain C API, safe to call from any compiler: the engine owns every object it
 * returns and frees it in the matching *_close function.
 *
 * Error convention: functions returning int return 0 (or a count) on success
 * and a negative errno-style value on failure. ehfs_strerror() describes it.
 *
 * Paths inside a volume are UTF-8, '/'-separated and rooted at "/". A '/' that
 * appears inside a Mac file name is presented as ':' (the same convention the
 * macOS command line uses).
 */

#ifndef EHFS_H
#define EHFS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(EHFS_SHARED)
#  ifdef EHFS_BUILDING
#    define EHFS_API __declspec(dllexport)
#  else
#    define EHFS_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(EHFS_SHARED)
#  define EHFS_API __attribute__((visibility("default")))
#else
#  define EHFS_API
#endif

#define EHFS_NAME_MAX 765           /* bytes of one UTF-8 path element */
#define EHFS_MAX_PARTITIONS 128

typedef struct ehfs_disk ehfs_disk;
typedef struct ehfs_volume ehfs_volume;
typedef struct ehfs_dir ehfs_dir;
typedef struct ehfs_file ehfs_file;

/* ---- Library ------------------------------------------------------------ */

enum ehfs_log_level { EHFS_LOG_DEBUG, EHFS_LOG_INFO, EHFS_LOG_WARNING, EHFS_LOG_ERROR };

EHFS_API const char* ehfs_version(void);
EHFS_API const char* ehfs_strerror(int err);

/* Receives every engine and libhfs message. Default: messages go to stderr. */
EHFS_API void ehfs_set_log_handler(void (*handler)(int level, const char* message, void* ctx), void* ctx);

/* ---- Disks and partitions ----------------------------------------------- */

typedef enum {
	EHFS_SCHEME_NONE = 0,   /* no partition table: the whole device is one volume */
	EHFS_SCHEME_MBR,
	EHFS_SCHEME_GPT,
	EHFS_SCHEME_APM         /* Apple Partition Map (PowerPC-era Macs, some disk images) */
} ehfs_scheme;

typedef enum {
	EHFS_FS_UNKNOWN = 0,
	EHFS_FS_HFSPLUS,        /* Mac OS Extended */
	EHFS_FS_HFSX,           /* Mac OS Extended (Case-sensitive) */
	EHFS_FS_HFS_STANDARD,   /* Mac OS Standard (HFS without "+") */
	EHFS_FS_APFS,
	EHFS_FS_CORESTORAGE,    /* Core Storage (Fusion Drive / older FileVault) */
	EHFS_FS_EFI,            /* EFI System Partition */
	EHFS_FS_NTFS,
	EHFS_FS_EXFAT,
	EHFS_FS_FAT,
	EHFS_FS_EMPTY,          /* unused partition-table slot or free space */
	EHFS_FS_OTHER
} ehfs_fs_type;

typedef enum {
	EHFS_SUPPORT_SUPPORTED = 0,  /* can be mounted */
	EHFS_SUPPORT_NEEDS_MAC,      /* HFS+, but must be ejected properly on a Mac first (dirty journal) */
	EHFS_SUPPORT_UNSUPPORTED,    /* recognised, but not a format this version handles */
	EHFS_SUPPORT_NOT_MAC,        /* not a Mac volume (Windows can read it natively, or it is empty) */
	EHFS_SUPPORT_DAMAGED         /* looks like HFS+ but its structures are invalid */
} ehfs_support;

typedef struct {
	ehfs_fs_type fs_type;
	ehfs_support support;
	char reason[160];             /* human-readable explanation of `support` */
	char volume_name[EHFS_NAME_MAX + 1];
	uint16_t version;             /* 4 = HFS+, 5 = HFSX */
	uint32_t block_size;
	uint64_t total_bytes;
	uint64_t free_bytes;
	uint32_t file_count;
	uint32_t folder_count;
	int journaled;
	int journal_dirty;            /* journal holds changes not yet applied */
	int unmounted_cleanly;        /* volume header "unmounted" bit */
	int case_sensitive;
	int hfs_wrapper;              /* HFS+ embedded in an HFS Standard wrapper */
	char last_mounted_by[5];      /* e.g. "10.0" macOS, "HFSJ" macOS journaled, "H+Lx" Linux, "fsck" */
	int64_t created;              /* Unix seconds */
	int64_t modified;             /* Unix seconds */
} ehfs_volume_info;

typedef struct {
	uint32_t number;              /* 1-based partition number; 0 = whole device */
	uint64_t offset;              /* bytes from the start of the device */
	uint64_t length;              /* bytes */
	char type[40];                /* GPT type GUID, MBR type "0xAF", or APM type "Apple_HFS" */
	char name[144];               /* partition label, UTF-8 (GPT / APM only) */
	ehfs_volume_info volume;      /* result of probing this partition */
} ehfs_partition;

typedef struct {
	ehfs_scheme scheme;
	uint64_t size;                /* bytes */
	uint32_t sector_size;
	uint32_t count;
	ehfs_partition partitions[EHFS_MAX_PARTITIONS];
} ehfs_disk_layout;

/*
 * Open a disk or image read-only. `path` is a file or device path such as
 * "drive.img", "/dev/sdb" or "\\.\PhysicalDrive2". The OS handle is opened for
 * reading only, so nothing in the engine can write to it.
 */
EHFS_API int  ehfs_disk_open(const char* path, ehfs_disk** out);
EHFS_API void ehfs_disk_close(ehfs_disk* disk);

/* Read the partition table and probe every partition. */
EHFS_API int  ehfs_disk_scan(ehfs_disk* disk, ehfs_disk_layout* out);

/* Probe a single volume at a byte range of the disk. */
EHFS_API int  ehfs_probe(ehfs_disk* disk, uint64_t offset, uint64_t length, ehfs_volume_info* out);

/* ---- Volumes ------------------------------------------------------------ */

typedef struct {
	/* Open a volume whose journal is dirty. The view may be missing the most
	 * recent changes made on the Mac. Default 0 = refuse with -EHFS_EDIRTY. */
	int allow_dirty_journal;
	/* Number of recently looked-up paths to cache. 0 = default (4096). */
	uint32_t lookup_cache_size;
} ehfs_open_options;

#define EHFS_EDIRTY 10001         /* returned negated: journal dirty, see allow_dirty_journal */
#define EHFS_ENOTHFS 10002        /* returned negated: not an HFS+ / HFSX volume */

EHFS_API int  ehfs_volume_open(ehfs_disk* disk, uint64_t offset, uint64_t length,
                               const ehfs_open_options* options, ehfs_volume** out);
EHFS_API void ehfs_volume_close(ehfs_volume* vol);
EHFS_API int  ehfs_volume_get_info(ehfs_volume* vol, ehfs_volume_info* out);

typedef enum {
	EHFS_TYPE_FILE = 1,
	EHFS_TYPE_DIR,
	EHFS_TYPE_SYMLINK,
	EHFS_TYPE_OTHER               /* FIFO, socket, device node */
} ehfs_type;

enum {
	EHFS_FLAG_HIDDEN     = 1 << 0, /* Finder "invisible" or name starts with '.' */
	EHFS_FLAG_LOCKED     = 1 << 1, /* Finder "Locked" (immutable) */
	EHFS_FLAG_COMPRESSED = 1 << 2, /* HFS+ transparent compression */
	EHFS_FLAG_HARDLINK   = 1 << 3  /* reached through a hard link */
};

typedef struct {
	uint32_t id;                  /* catalog node ID, stable for the volume's lifetime */
	ehfs_type type;
	uint32_t flags;
	uint64_t size;                /* data size in bytes (uncompressed); 0 for directories */
	uint64_t allocated;           /* bytes allocated on disk */
	uint64_t rsrc_size;           /* resource fork size in bytes */
	uint32_t mode;                /* POSIX permission bits */
	uint32_t uid, gid;
	uint32_t nlink;
	uint32_t entries;             /* directories: number of children */
	int64_t created, modified, changed, accessed;   /* Unix seconds */
} ehfs_stat;

EHFS_API int  ehfs_stat_path(ehfs_volume* vol, const char* path, ehfs_stat* out);

/* Directory listing. Hidden HFS+ private system files are never returned. */
EHFS_API int  ehfs_dir_open(ehfs_volume* vol, const char* path, ehfs_dir** out);
/* Returns 1 and fills name/st for the next entry, 0 at the end, or <0 on error.
 * `name` must hold EHFS_NAME_MAX + 1 bytes. */
EHFS_API int  ehfs_dir_next(ehfs_dir* dir, char* name, ehfs_stat* st);
EHFS_API void ehfs_dir_close(ehfs_dir* dir);

/* File content (data fork). Thread-safe: one ehfs_file may be read from
 * several threads at once. */
EHFS_API int     ehfs_file_open(ehfs_volume* vol, const char* path, ehfs_file** out);
EHFS_API int64_t ehfs_file_read(ehfs_file* file, void* buf, size_t size, uint64_t offset);
EHFS_API void    ehfs_file_close(ehfs_file* file);

/* Target of a symbolic link, UTF-8, NUL-terminated. */
EHFS_API int  ehfs_readlink(ehfs_volume* vol, const char* path, char* buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
