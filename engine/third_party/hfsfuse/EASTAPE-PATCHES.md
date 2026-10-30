# Vendored hfsfuse: source and Eastape changes

| | |
|---|---|
| Upstream | https://github.com/0x09/hfsfuse |
| Commit | `eed780c` (2026-10-25) |
| License | MIT (hfsfuse), BSD-2-Clause (NetBSD libhfs), MIT (utf8proc), BSD-3-Clause (LZFSE), APSL-2.0 (LZVN). See `COPYING` and each library's license file. |

## What was copied

Only the libraries the engine links. hfsfuse's FUSE driver, `hfsdump` and
`hfstar` are not included.

| Path | Purpose |
|---|---|
| `lib/libhfs/` | NetBSD HFS+ reader: volume header, B-trees, catalog, extents, attributes |
| `lib/libhfsuser/` | hfsfuse support layer: path lookup and cache, Unicode handling, compressed files |
| `lib/utf8proc/` | Unicode normalization (sources and license only) |
| `lib/LZFSE/src/` | LZFSE decoder (decode sources only) |
| `lib/LZVN/` | LZVN decoder (decode source only) |

Upstream Makefiles were removed; `engine/CMakeLists.txt` builds these files.

## Eastape changes

Every change is marked `Eastape HFS patch` in the source. `eastape.patch` is
the exact diff against the upstream commit above.

1. **Caller-provided block reader** (`hfsuser.h`, `hfsuser.c`).
   `struct hfs_volume_config` gains `io_ctx` and `io_read`. When set,
   libhfsuser reads through that function instead of opening a file. The engine
   uses it to add the partition offset, keep every read inside the partition,
   and do sector-aligned raw-disk reads on Windows.
2. **Error handler** (`hfsuser.h`, `hfsuser.c`). `hfs_set_error_handler()`
   routes libhfs messages to the engine log instead of stderr. The Windows
   service has no console.
3. **Directory listing hang** (`libhfs.c`, `hfslib_get_directory_contents`).
   When a folder's catalog records ran to the end of the last leaf node
   (`flink == 0`), the loop read node 0 (the B-tree header node) forever. Found
   by our tests: listing the root of a freshly formatted, non-journaled volume
   hung at 100% CPU. On real drives this would hit, for example, the most
   recently created empty folder. The loop now stops at the end of the leaf chain.
4. **Directory listing descent** (`libhfs.c`, same function). When the first
   record of an index node matched the folder, the code jumped to a node
   remembered from the level above instead of descending. It now descends into
   that record's child, which is where the folder's thread record (its first
   key) lives.
5. **Loop guards for damaged volumes** (`libhfs.c`). Catalog lookup, extents
   lookup and directory listing stop with an error after visiting more nodes
   than the B-tree contains, and catalog lookup rejects unexpected node kinds.
   A damaged drive now produces an error instead of hanging.

Changes 3–5 are upstream bugs, also present in NetBSD's libhfs. They are worth
reporting upstream (not done yet, pending Eastape's approval).

## Updating

1. Copy the same paths from a newer upstream commit.
2. Re-apply `eastape.patch` (`patch -p1 < eastape.patch` from this directory)
   and regenerate it.
3. Update the commit in this file and in `docs/third-party-licenses.md`.
4. Run `tests/engine/run_tests.sh`.
