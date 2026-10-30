# Phase 1: Research and Feasibility Report

| | |
|---|---|
| Project | Eastape HFS |
| Phase | 1 of 7: Research and source inspection |
| Date | 2026-10-30 |
| Status | **Complete, awaiting Eastape approval before Phase 2** |
| Deliverable | Feasibility verdict and selected implementation approach (this document), plus [architecture.md](architecture.md) and [third-party-licenses.md](third-party-licenses.md) |

---

## 1. Verdict

**Feasible, using only free and open-source components.** No commercial SDK is
needed, and we don't need to write our own Windows kernel driver.

Selected approach:

| Layer | Selected | Why |
|---|---|---|
| HFS+ engine | **hfsfuse** `libhfs` + `libhfsuser` (C), vendored at a pinned commit | Read-only by design, MIT/BSD licensed, actively maintained (commits this month), covers the HFS+ features media drives actually use, already designed for a FUSE-style mount layer |
| Windows mount layer | **WinFsp** native API, official signed installer | Signed kernel driver already exists, real drive letters via Mount Manager, kernel-enforced read-only volumes, mature (used by rclone and SSHFS-Win) |
| Service + mount host | C/C++ | Shortest path between WinFsp's native API and the C engine; best read throughput for video |
| Desktop app | C# / .NET / WinUI 3 | As specified; native Windows look, dark theme, Mica |
| Installer | **NSIS** (recommended, decision needed) | Free with no conditions; handles prerequisite MSI, service and uninstaller |

Five findings change the plan compared with the spec's assumptions. They are
explained in section 4:

1. hfsfuse **refuses HFS+ volumes with a dirty journal** (a drive unplugged
   from a Mac without ejecting). It doesn't replay the journal. We add an
   in-memory journal replay that never writes. Until then, such drives are
   refused with a clear message.
2. hfsfuse expects the device to **start at the HFS+ volume** and reads with
   POSIX `open`/`pread`. Windows raw disks need a **partition offset and
   sector-aligned reads**, so we add a small Win32 disk backend.
3. **WinFsp's license** is fine for internal use. If Eastape HFS is ever given
   to people outside Eastape, it must be open-sourced or a commercial WinFsp
   license bought.
4. **Both installer tools named in the spec now ask commercial users to pay**
   in some way. NSIS doesn't.
5. **This research environment is Linux.** The Windows app, service, mount and
   installer have to be built on Windows (GitHub Actions Windows runners or an
   Eastape PC). Real-drive testing has to be done by Eastape on real hardware.

---

## 2. Method

- Cloned and read the source, license files and recent history of every
  candidate (commits pinned in section 3). Read the parts of the code that
  decide safety and fit: device I/O, journal handling, callback interfaces,
  threading, and Windows build support.
- Checked WinFsp's license, mount-point API, volume flags and Mount Manager
  support in its source.
- Checked current licensing terms of installer tools (web sources listed at the
  end).
- Downloaded macOS-made test images from the dfvfs project and checked their
  on-disk signatures by reading raw bytes.
- **Not done:** building and running hfsfuse against the test images. The
  sandbox in this session blocked building third-party code. It's listed as
  the first task of Phase 2 (step 2.0), and needs either permission in this
  environment or a CI/Windows machine.

---

## 3. Candidates inspected

### 3.1 HFS+ implementations

| Project (commit inspected) | Lang | License | Activity | Coverage relevant to Eastape | Windows | Decision |
|---|---|---|---|---|---|---|
| **hfsfuse** — `0x09/hfsfuse` @ `eed780c` | C | MIT (own code), BSD-2 (NetBSD libhfs), MIT/BSD bundled libs, APSL-2.0 (LZVN) | Active, last commit 2026-10-25 | HFS+ journaled (clean) and non-journaled, HFSX, Unicode normalization, hard links incl. directory hard links (Time Machine), decmpfs compression (zlib, LZVN, LZFSE), resource forks, xattrs. Read-only, no write code. | Support libraries build with MinGW-w64 / MSYS2. Its own FUSE front end is Unix-only (we replace it). | **Selected** |
| **libfshfs** — `libyal/libfshfs` @ `78321de` | C | LGPL-3.0-or-later | Active, 2026-10-11 | HFS+/HFSX (Mac OS 10.3+ only; older Unicode tables and HFS-wrapped HFS+ unsupported), compression (not LZBitmap), xattrs, hard links. Read-only. README marks it **"experimental"**. No journal replay found. | MSVC projects; mount tool uses Dokan, not WinFsp | Runner-up; use as a **second, independent reader** to cross-check test results |
| **HFSExplorer** — `unsound/hfsexplorer` (release 2026.4.9) | Java | GPL-3.0 | Active | Mature browse-and-extract GUI | Runs on Windows, but it's a Java app, not a mountable library | Not embeddable. Use as an **independent test oracle** only |
| **DiscUtils.HfsPlus** — `LTRData/DiscUtils` (2026-10-16) | C# | MIT | Active | Read-only, zlib compression only (no LZVN/LZFSE), no journal handling, no hard-link resolution found | .NET | Not selected: too many gaps |
| Linux kernel `hfsplus` | C | GPL-2.0 | — | Kernel code | — | Reference only |
| Apple `hfs` sources | C | APSL-2.0 | — | XNU kernel code | — | Format reference only |
| Paragon HFS+ / commercial SDKs | — | Proprietary | — | — | — | Excluded by project rules |

### 3.2 Windows mount layers

| Option | License | Assessment | Decision |
|---|---|---|---|
| **WinFsp** — `winfsp/winfsp` @ `4425d48` (2026-10-13) | GPLv3 + FLOSS exception, or commercial | Signed driver in the official MSI. Native C API, FUSE 2.8/3.2 compatibility, .NET API. `ReadOnlyVolume` flag. Drive letters via `DefineDosDevice` or the **Mount Manager** (`FspPathIsMountmgrDrive` in `src/dll/mount.c`). Passing `NULL` as the mount point picks the next free letter counting down from `Z:`. | **Selected** |
| **Dokany** — `dokan-dev/dokany` @ `49ae5ed` (2026-10-24) | LGPL-3.0 + MIT | Viable and more permissive license. Smaller ecosystem, and its network provider is known to interfere with WinFsp's. | Fallback only if WinFsp licensing ever becomes a blocker |
| Own kernel-mode filesystem driver | — | Months of extra work, needs Microsoft attestation signing and an EV certificate, and a bug can blue-screen the PC | Rejected |

### 3.3 Fit check: hfsfuse ↔ WinFsp

The spec asks us to "verify the actual compatibility of the selected HFS+
implementation with WinFsp". At the API level they match one-to-one:

| What Windows/WinFsp asks a read-only filesystem for | What hfsfuse already implements (`src/hfsfuse.c`) | Engine call |
|---|---|---|
| Open / Close | `hfsfuse_open`, `hfsfuse_release` | `hfs_file_open`, `hfs_file_close` |
| Read | `hfsfuse_read` | `hfs_file_pread` |
| GetFileInfo / GetSecurityByName | `hfsfuse_getattr`, `hfsfuse_fgetattr` | `hfs_lookup`, `hfs_stat` |
| ReadDirectory | `hfsfuse_opendir`, `hfsfuse_readdir` | `hfslib_get_directory_contents` |
| GetVolumeInfo | `hfsfuse_statfs` | volume header |
| Reparse (symlinks) | `hfsfuse_readlink` | catalog record |
| Named streams (resource fork) | `hfsfuse_getxattr` | `hfs_file_open(..., HFS_RSRCFORK)` |

The engine is also **thread-safe by design**: per-file read/write locks and
read mutexes (`src/hfsfuse.c`, `lib/libhfsuser/file.c`). That matters because
WinFsp calls the filesystem from several threads at once.

**Not yet verified by running.** The first real proof is the Phase 2 spike
(engine on images) and the Phase 3 deliverable (drive visible in Explorer).

---

## 4. Findings, limitations and how we handle them

### 4.1 Dirty journal: refused, not replayed

`libhfs.c` reads the journal header and only checks `jh.start == jh.end`
(`hfslib_is_journal_clean`). There's no replay code. hfsfuse refuses dirty
volumes unless `--force` is passed, and `--force` can show **stale or
inconsistent** directories, because the latest changes still sit in the
journal.

Dirty journals happen whenever a drive is unplugged from a Mac without
ejecting, or the Mac crashes. That's common in production.

- **Phase 2a (v1 minimum):** detect a dirty journal and refuse with: "This drive
  was not ejected properly on a Mac. Connect it to a Mac and eject it, then try
  again." Safe, but inconvenient.
- **Phase 2b (recommended for v1):** in-memory journal replay. Read the pending
  transactions (TN1150 documents the format) into a block overlay, and serve
  those blocks from memory instead of the disk. **Nothing is written.** This is
  Eastape's main original contribution to the engine. Tested by hashing the
  disk image before and after, which must be identical.

### 4.2 Windows raw-disk access

`hfs_open` in `lib/libhfsuser/hfsuser.c` opens a path with POSIX `open()` and
reads with `pread`. On Windows it falls back to `lseek` + `read` behind a
global mutex. Block-size detection uses Unix `ioctl`s, so on Windows it
reports 0 and reads aren't aligned, which **fails on raw disks**. It also
assumes the device starts at the HFS+ volume (it only understands the old HFS
wrapper, not GPT or APM).

Fix: libhfs takes a callback table (`hfslib_init(hfs_callbacks*)` with
open/close/read), so we plug in our own Win32 backend:

- `CreateFile("\\.\PhysicalDriveN", GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, …)`
- byte offset of the partition added to every read
- sector-aligned overlapped reads, which are thread-safe without the seek mutex
- sector size from `IOCTL_STORAGE_QUERY_PROPERTY`

This is a small, contained patch, and no HFS+ logic changes. Partition
scanning (GPT, MBR, and Apple Partition Map, which Windows can't read) is our
own code in the service.

### 4.3 Windows file-name rules

HFS+ allows names Windows can't show (`:`, `?`, `*`, `"`, `<`, `>`, `|`, `\`,
trailing dots or spaces), and stores accented letters decomposed (NFD). The
mount layer converts names to NFC and swaps forbidden characters for
full-width look-alikes, keeping a per-directory map back to the exact on-disk
name. HFSX (case-sensitive) volumes can contain `A.mov` and `a.mov` side by
side. Those volumes are mounted with case-sensitive search, and collisions are
logged.

### 4.4 Large folders

`hfslib_get_directory_contents` loads a whole directory into memory per
listing. That's fine for normal folders. For EXR sequences with 100,000+ frames
we cache the listing per directory handle, and set long WinFsp metadata
timeouts (safe because a read-only mount never changes). This is measured in
Phase 3 with a 100k-file test folder.

### 4.5 Licensing

Full table in [third-party-licenses.md](third-party-licenses.md). Summary:

- Engine code: MIT/BSD, no restrictions for us.
- **WinFsp:** no obligations for internal use (installing on Eastape's own PCs
  is not distribution). If ever distributed outside Eastape, Eastape HFS must
  be released as open source (with the WinFsp notice in the UI), or a
  commercial license bought.
- **LZVN decompressor:** APSL-2.0. Internal use: no obligation.
- Recommendation: confirm in writing that Eastape HFS is internal-only.

### 4.6 Installer tools

| Tool | Current terms | Fit |
|---|---|---|
| **NSIS** | zlib license, free for any use | **Recommended.** Supports silent install of the WinFsp MSI, service registration, Start Menu and desktop shortcuts, Installed Apps entry, upgrade detection |
| Inno Setup | Since 6.5 the authors ask commercial users, explicitly including in-house use, to buy a license. They say it's not strictly required. | Conflicts with the "no paid tools" rule |
| WiX v7 | Open Source Maintenance Fee. Its EULA asks for a fee once a project earns US$10,000+/year | Likely free for a zero-revenue internal tool, but needs EULA acceptance and a judgement call |

### 4.7 Build and test environment

This cloud session runs Linux. It can build and test the C engine against disk
images, but it can't build WinUI 3, run WinFsp, produce `Setup.exe`, or touch
real drives. Proposal:

- **CI:** GitHub Actions `windows-latest` runners build `ehfs.dll`, the service,
  the app and `Eastape HFS Setup.exe` on every push, and run automated mount
  tests against image files with WinFsp installed on the runner. Linux runners
  run engine tests and fuzzing. (Private repos have a monthly minutes quota, and
  Windows minutes count double.)
- **Hands-on:** from Phase 3, someone at Eastape installs CI builds on a test PC
  with real drives. Optionally, Claude Code runs directly on that Windows PC for
  faster debug loops.
- **Real-hardware sign-off** (spec section 11) can only be done by Eastape.

### 4.8 Windows version support

WinFsp and WinUI 3 support Windows 10 and 11. Windows 10 reached end of
support in October 2025. Proposal: Windows 11 (x64) fully supported, Windows 10
22H2 (x64) supported on a best-effort basis, ARM64 later.

---

## 5. Data-safety design (summary)

Details in [architecture.md](architecture.md#read-only-safety-model). Three
independent layers each block writes on their own:

1. The disk handle is opened `GENERIC_READ` only, so Windows refuses writes
   through it.
2. The engine has no write code (libhfs's callback table has no write entry),
   and journal replay stays in memory.
3. WinFsp `ReadOnlyVolume` makes the kernel driver reject all modifications
   before they reach our code.

Plus: each mounted volume runs in its own process (a corrupt drive can't crash
the service or other mounts). No state-changing disk IOCTLs are used, there is
no automatic repair, "Mounted" is shown only after Windows confirms the drive
letter, and every error is logged with a reference shown in the UI.

**User-education item:** if Windows ever shows "You need to format the disk in
drive X:", the user must click **Cancel**. Windows only shows this for drives it
gives a letter to itself. It normally doesn't do that for Apple partitions, but
the user guide and the app's first-run screen will warn about it.

---

## 6. Test fixtures identified

| Fixture | Source | Checked | Use |
|---|---|---|---|
| `hfsplus.raw` | dfvfs (made on macOS) | Signature `H+`, attributes `0x80000100` (cleanly unmounted, not journaled) | Basic HFS+ read |
| `apm.dmg` | dfvfs | `PM` at byte 512 (Apple Partition Map) | APM partition detection |
| `hfs.raw` | dfvfs | Signature `BD` (HFS Standard) | Must be rejected as unsupported |
| `apfs.raw`, `apfs_encrypted.dmg` | dfvfs | No `H+` header | Must be rejected as APFS |
| Journaled HFS+ (clean) | `mkfs.hfsplus -J` (hfsprogs, available here) | — | Journal detection |
| Realistic journaled volumes, dirty journal, HFSX, compressed files, hard links, 100k-file folder, >4 GB files, Unicode/emoji names | **To be made on an Eastape Mac** with a script delivered in Phase 2 | — | Main correctness suite, with SHA-256 manifests generated on the Mac |
| Real drives (HDD, SSD, Time Machine, old APM drive if available) | Eastape | — | Phase 7 hardware sign-off |

---

## 7. Phase 2 plan: HFS+ filesystem engine

| Step | Work | Done when |
|---|---|---|
| 2.0 | **Spike:** build hfsfuse unmodified, run `hfsdump` on all fixtures, compare file lists and SHA-256 against HFSExplorer/libfshfs | Results match. Needs build permission here, or CI |
| 2.1 | Repo scaffolding: CMake, `engine/third_party/hfsfuse` at a pinned commit with a patch log, Linux + Windows CI | CI green on both |
| 2.2 | Win32 block-device backend (offset, alignment, overlapped reads); Linux file backend for tests | Reads images on Linux and a raw disk on Windows |
| 2.3 | Partition scanner (GPT, MBR, APM) and volume prober (signature, version, journal state, volume name); reasons for unsupported volumes | All fixtures classified correctly |
| 2.4 | `ehfs` C API and `ehfs-cli` (`info`, `ls`, `cat`, `hash-tree`) for tests and diagnostics | Manifests match the Mac-generated SHA-256 lists |
| 2.5a | Dirty-journal detection and refusal | Dirty fixture refused with the correct message |
| 2.5b | In-memory journal replay | Dirty fixture reads correctly; image hash unchanged before/after |
| 2.6 | Fuzzing of the parsers (libFuzzer) and corrupted-image tests | No crashes over a multi-hour run; errors are reported, never ignored |
| 2.7 | Phase 2 report | Written and approved |

Rough timeline after approval (assuming CI and a Mac for fixtures are
available): Phase 2 takes 2–3 weeks; Phases 3–7 take about 2–3 months
altogether, depending mostly on how fast real-drive testing rounds can happen
at Eastape.

---

## 8. Decisions needed from Eastape

1. **Approve the selected architecture** (hfsfuse engine + WinFsp + C/C++
   service + WinUI 3 app).
2. **Product name spelling:** the spec uses both "Eastape HFS" and
   "EStape HFS". These documents use "Eastape HFS" until confirmed.
3. **Installer tool:** NSIS (recommended, free with no conditions), WiX v7, or
   Inno Setup (commercial license requested).
4. **Build environment:** OK to use GitHub Actions Windows runners for builds?
   Is a Windows test PC with real drives available from Phase 3?
5. **Permission to build and run open-source code in this environment** for
   the Phase 2 spike (blocked by the session sandbox during Phase 1).
6. **Test material:** access to a Mac to run the fixture-generation script, and
   a few real HFS+ drives (journaled, Time Machine, HFSX or old APM drive if
   any exist).
7. **Dirty-journal policy for v1:** ship with in-memory replay (recommended,
   2.5b), or refuse-only (2.5a) for the first release?
8. **Windows 10:** confirm "best-effort" support is acceptable.
9. **Distribution scope:** confirm Eastape HFS stays internal. This keeps WinFsp
   and LZVN licensing obligation-free.

---

## Sources

- hfsfuse: https://github.com/0x09/hfsfuse (README, `COPYING`, `lib/libhfs/libhfs.c`, `lib/libhfsuser/hfsuser.c`, `src/hfsfuse.c`)
- libfshfs: https://github.com/libyal/libfshfs (README)
- HFSExplorer: https://github.com/unsound/hfsexplorer
- DiscUtils: https://github.com/LTRData/DiscUtils (`Library/DiscUtils.HfsPlus`)
- WinFsp: https://github.com/winfsp/winfsp (`License.txt`, `inc/winfsp/winfsp.h`, `inc/winfsp/fsctl.h`, `src/dll/mount.c`, `doc/Frequently-Asked-Questions.asciidoc`)
- Dokany: https://github.com/dokan-dev/dokany
- Apple Technical Note TN1150, HFS Plus Volume Format: https://developer.apple.com/library/archive/technotes/tn/tn1150.html
- dfvfs test data: https://github.com/log2timeline/dfvfs/tree/main/test_data
- Inno Setup purchase FAQ: https://jrsoftware.org/isorder.php
- WiX Open Source Maintenance Fee: https://docs.firegiant.com/wix/osmf/
