# Phase 2: HFS+ Engine, Progress Report 1

| | |
|---|---|
| Project | Eastape HFS |
| Phase | 2 of 7: HFS+ filesystem engine |
| Date | 2026-10-30 |
| Status | **Read path working and tested. Phase 2 not yet complete (see "Remaining").** |

Eastape approved going ahead after Phase 1 ("go for it") and asked for read
**and** write support like Paragon HFS+. Where Phase 1 decisions were left
open, these defaults were used and can still be changed:

- product name "Eastape HFS",
- NSIS installer (Phase 6),
- v1 read-only on the hfsfuse engine, then a read/write track based on Apple's
  open-source user-space HFS+ engine (see the end of this report).

---

## 1. What was built

| Component | Files | Status |
|---|---|---|
| Vendored HFS+ libraries (hfsfuse `eed780c`) with a patch log | `engine/third_party/hfsfuse/` | Done |
| Read-only block device layer: Linux/macOS backend and Windows backend (`GENERIC_READ` only, overlapped I/O, sector alignment) | `engine/src/blockdev*.c` | Done; Windows backend tested on image files |
| Partition scanner: GPT (CRC-checked, backup header fallback, 512/4096-byte sectors), Apple Partition Map, MBR with extended partitions | `engine/src/partition.c` | Done |
| Volume prober: HFS+, HFSX, HFS wrapper, journal state (both byte orders), HFS Standard, APFS, Core Storage, EFI, NTFS/exFAT/FAT, with a user-facing reason for each | `engine/src/probe.c` | Done |
| Engine C API (`ehfs.h`): disks, partitions, volumes, stat, directory listing, thread-safe file reads, symlinks | `engine/include/ehfs.h`, `engine/src/volume.c`, `disk.c`, `util.c` | Done |
| `ehfs` command-line tool: scan, info, ls, cat, hash (SHA-256) | `tools/ehfs-cli/` | Done (Linux and Windows) |
| Automated tests, fixture generators, CI | `tests/`, `.github/workflows/engine.yml` | Done |
| Build docs | `docs/building.md` | Done |

## 2. Test results

`tests/engine/run_tests.sh` on Linux: **45 passed, 0 failed** (including the
optional >4 GiB file test).

| Area | What is checked | Result |
|---|---|---|
| File contents | 2,017 files (2,000-frame EXR-style sequence, 24 MB clip, sizes around block boundaries, empty files, deep folders, Spanish/Hindi/Japanese/emoji names) read back byte-for-byte via SHA-256, through APM, raw volume, GPT, GPT with 4096-byte sectors, MBR, and GPT with a damaged primary header | All match |
| Large files | One file of 4 GiB + 12,345 bytes | Matches |
| Detection | HFS+, HFSX, HFS wrapper, accented and emoji volume names, EFI, APFS (raw and on GPT), HFS Standard, random data | All correct |
| Journal | Fresh (uninitialised), clean, and dirty journals in Intel and PowerPC byte order; dirty refused unless `--allow-dirty` | All correct |
| Damaged volumes | Broken header, truncated volume, corrupted catalog: error message and exit code 1, never a crash or hang | All correct |
| macOS-made images (dfvfs) | Volume name, known file content, symlink, APM, APFS/HFS refusal | All correct |
| Read-only | SHA-256 of all 18 test images before and after the run | Unchanged |

**Windows build.** Cross-compiled with MinGW-w64, no warnings. Run under Wine
(Windows API layer on Linux), `ehfs.exe` passes the same file-content checks on
the APM, GPT, GPT-4K and MBR images, plus non-English path arguments and dirty
journal refusal. The CI workflow repeats this on a real Windows Server runner
for every push.

**Not yet tested:** reading a real physical drive (`\\.\PhysicalDriveN`) on a
real Windows PC. This needs Eastape hardware. Instructions are in
[building.md](building.md#using-ehfs).

## 3. Bugs found and fixed

The test suite found a **hang in upstream libhfs**: listing a folder whose
catalog records run to the end of the B-tree read the header node forever, at
100% CPU. On a real drive, opening the most recently created empty folder
could have frozen Explorer. Two related upstream problems were fixed with it
(a wrong jump in index nodes, and no protection against loops in damaged
B-trees). Details are in
[EASTAPE-PATCHES.md](../engine/third_party/hfsfuse/EASTAPE-PATCHES.md). These
are worth reporting to the hfsfuse project. I haven't done so, because posting
publicly needs your OK.

Other findings, now handled:
- Volume names are returned in the composed Unicode form Windows displays
  ("é", not "e" + combining accent).
- The Windows CLI reads its arguments as UTF-16, so non-English paths work.
- A freshly formatted journaled volume has an uninitialised journal. That's
  correctly treated as clean, not dirty.

## 4. Remaining Phase 2 work

| Step | Work | Notes |
|---|---|---|
| 2.5b | In-memory journal replay, so drives not ejected properly can be read without a Mac | Format documented in TN1150. Will be tested with dirty journals containing real transactions. |
| 2.6 | Fuzzing the parsers (libFuzzer) for hours | The damaged-volume tests pass; fuzzing goes much deeper |
| 2.x | Mac-made test images (journaled, dirty, HFSX, compressed files, hard links, Time Machine, Greek and other scripts) | Needs an Eastape Mac; script to follow |
| 2.x | Real-drive check on Windows: `ehfs scan \\.\PhysicalDriveN` and `ehfs hash` on a real Mac-formatted USB drive | Needs an Eastape PC (admin prompt) |
| 2.7 | Phase 2 completion report | After the above |

## 5. Read/write track (requested by Eastape)

Plan, as agreed:
1. v1 stays read-only on this engine. It can't write by construction, which is
   the safe default for original footage.
2. **Write spike:** port Apple's open-source user-space HFS+ engine
   (`livefiles_hfs_plugin`, APSL-2.0) to Linux and Windows, then write to test
   images and require Apple's disk checker (`fsck.hfsplus`, available in CI) to
   pass after every write. Includes power-cut simulation. Go/no-go after this
   spike.
3. **v2 read/write:** per-drive "Enable writing", journaled writes, Time Machine
   drives always read-only, an Eject button, and the full test programme from
   the earlier plan.

The engine API was kept small and handle-based so a second (read/write) engine
can sit behind the same interface.

## 6. Decisions still open

1. **Spike timing:** start the read/write spike now, in parallel with 2.5b/2.6,
   or after Phase 2 is complete?
2. **Upstream bug reports:** may we report the libhfs bugs to the hfsfuse
   project (public GitHub issue)?
3. **Hardware:** who at Eastape can run the `ehfs` checks on a Windows PC with a
   real Mac-formatted drive, and make test images on a Mac?
4. Phase 1 items still unanswered: name spelling, installer tool, Windows 10
   support level, internal-only distribution.
