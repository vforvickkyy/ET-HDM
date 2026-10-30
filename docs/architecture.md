# Eastape HFS: Architecture

Status: proposed in Phase 1, pending approval. This document will be kept up to
date as each phase lands.

## Overview

```
 ┌───────────────────────────────┐
 │ Eastape HFS.exe  (C#, WinUI 3)│  runs as the logged-in user, no admin
 │ dashboard · mount/unmount ·   │
 │ settings · logs               │
 └──────────────┬────────────────┘
                │ named pipe \\.\pipe\EastapeHfs  (ACL: interactive users)
 ┌──────────────┴────────────────┐
 │ EastapeHfsService.exe (C/C++) │  Windows service, LocalSystem
 │ ├ Device watcher              │  disk arrival/removal notifications
 │ ├ Disk & partition scanner    │  GPT / MBR / Apple Partition Map
 │ ├ Volume prober               │  reads HFS+ header, name, state
 │ ├ Mount manager               │  starts/stops one mount host per volume
 │ └ Log writer                  │  %ProgramData%\Eastape HFS\Logs
 └──────────────┬────────────────┘
                │ spawns one per mounted volume
 ┌──────────────┴────────────────┐
 │ EastapeHfsMount.exe (C/C++)   │  isolated: a bad volume can only crash
 │ ├ WinFsp adapter              │  its own process
 │ ├ Name mapper (Mac → Windows) │
 │ └ Eastape HFS engine (C lib)  │
 │    ├ libhfs + libhfsuser      │  vendored from hfsfuse (read-only)
 │    ├ Win32 block device       │  raw disk, sector-aligned, offset
 │    └ Journal overlay          │  in-memory replay, never writes
 └──────────────┬────────────────┘
                │ GENERIC_READ only
        \\.\PhysicalDriveN            WinFsp kernel driver (signed, unmodified)
```

## Components

### 1. Eastape HFS engine (`engine/`, C, `ehfs.dll`)

- Upstream: `lib/libhfs` and `lib/libhfsuser` from
  [hfsfuse](https://github.com/0x09/hfsfuse), copied in at a pinned commit with
  a written patch log. Bundled decompressors: zlib, LZFSE, LZVN. Unicode:
  utf8proc.
- Eastape additions:
  - **Win32 block device**: replaces hfsfuse's POSIX `open`/`pread` backend.
    Opens the physical disk with `GENERIC_READ` only, adds the partition byte
    offset, and issues sector-aligned overlapped reads, so it is thread-safe
    without a seek lock. The sector size comes from
    `IOCTL_STORAGE_QUERY_PROPERTY` (`StorageAccessAlignmentProperty`).
  - **Journal overlay** (Phase 2b): when an HFS+ journal is dirty, parse the
    pending transactions (TN1150 "Journal" section) and serve those blocks from
    memory. The disk is never written. Until this ships, dirty volumes are
    refused with a clear message.
  - A small C API (`ehfs_open`, `ehfs_stat`, `ehfs_readdir`, `ehfs_read`,
    `ehfs_close`) so the mount host never touches libhfs internals. The engine
    owns all memory it hands out, so the API is safe across compilers.
- Packaged as `ehfs.dll`. Upstream already builds with MinGW-w64; Phase 2
  decides between a MinGW-w64 build (plain C ABI, used by the MSVC-built mount
  host) and a small MSVC port (pthread shim). It also builds on Linux, so engine
  tests run in CI against disk-image files without Windows or real drives.

### 2. Mount host (`mount/`, C/C++, `EastapeHfsMount.exe`)

- WinFsp native API (`FspFileSystemCreate`), one process per mounted volume.
- Volume parameters: `ReadOnlyVolume = 1`, `CasePreservedNames = 1`,
  `UnicodeOnDisk = 1`, `CaseSensitiveSearch` = 1 only for HFSX volumes, long
  `FileInfoTimeout`/`DirInfoTimeout` (nothing changes while mounted read-only,
  so Windows can cache metadata aggressively; this matters for EXR sequences).
- Drive letter through the Windows Mount Manager, so it appears under
  **This PC → Devices and drives** for every user, not as a network location.
- Security descriptor: read and execute for Everyone. Mac UID/GID are ignored,
  the same way macOS treats external drives with "Ignore ownership".
- Name mapping: HFS+ stores decomposed Unicode (NFD). Windows gets NFC. Names
  with characters Windows forbids (`: ? * " < > | \`, trailing dot or space) are
  shown with full-width look-alikes. The mapping is kept per directory so
  lookups go back to the exact on-disk name.
- Mac metadata (`.DS_Store`, `.Spotlight-V100`, `.fseventsd`, `.Trashes`,
  `.TemporaryItems`, `.DocumentRevisions-V100`) is marked Hidden + System, not
  removed.
- Hard links (including Time Machine directory links) are resolved by libhfs.
  Symlinks that point inside the volume are resolved; others are listed in the
  log. Resource forks are hidden in v1 (later: `:AFP_Resource` named stream).

### 3. Drive service (`service/`, C/C++, `EastapeHfsService.exe`)

- Runs as LocalSystem because raw disk reads need administrator rights. The
  user is asked for admin only once, at install time.
- Detection: `RegisterDeviceNotification` for `GUID_DEVINTERFACE_DISK`, plus a
  full rescan at start-up and after resume from sleep.
- Disk information: `IOCTL_STORAGE_GET_DEVICE_NUMBER`,
  `IOCTL_STORAGE_QUERY_PROPERTY` (vendor/model such as "Samsung PSSD T7 Shield",
  bus type such as USB), `IOCTL_DISK_GET_LENGTH_INFO` (capacity),
  `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` (partitions and GPT type GUIDs).
- Partition recognition:

  | Partition marker | Meaning | Shown as |
  |---|---|---|
  | GPT `48465300-0000-11AA-AA11-00306543ECAC` | HFS+ / HFSX | probe → Supported |
  | MBR type `0xAF` | HFS+ (old) | probe → Supported |
  | APM entry `Apple_HFS` (parsed by us, Windows doesn't understand APM) | HFS+ on PowerPC-era drives | probe → Supported |
  | GPT `7C3457EF-0000-11AA-AA11-00306543ECAC` | APFS | Unsupported (APFS) |
  | GPT `53746F72-6167-11AA-AA11-00306543ECAC` | Core Storage (often encrypted HFS+) | Unsupported (Core Storage) |
  | HFS signature `BD` without embedded HFS+ | Mac OS Standard (HFS) | Unsupported (HFS Standard) |

- Probe = read the volume header at partition offset + 1024, check `H+`/`HX`,
  version, journal state, block size, and read the volume name from the
  catalog. Nothing is mounted unless the probe passes.
- Mount manager: starts a mount host for a volume, waits for Windows to report
  the new drive letter (`GetLogicalDrives` / `GetVolumeInformation`) before
  reporting "Mounted", and stops it on unmount, removal or service stop.
- Surprise removal: the mount host stops its dispatcher, outstanding reads fail
  with "device not connected", the drive letter is removed, and the UI shows
  "Disconnected".

### 4. Desktop app (`app/`, C#, `Eastape HFS.exe`)

- .NET with WinUI 3 (Windows App SDK), unpackaged and self-contained, so no
  separate runtime install is needed. Dark theme first, Mica on Windows 11.
- Talks only to the service over the named pipe, using small JSON messages:
  `ListDrives`, `Mount{driveId}`, `Unmount{driveId}`, `GetLog`, `Settings`,
  plus pushed `DriveChanged` events.
- `driveId` values are issued by the service. The app can never pass a raw
  disk path, so it can't ask the service to open an arbitrary device.

### 5. Installer (`installer/`)

- One `Eastape HFS Setup.exe`: checks for WinFsp and installs the bundled,
  unmodified official WinFsp MSI silently if missing or too old, installs the
  app, registers and starts the service, adds Start Menu and optional desktop
  shortcuts, and registers the uninstaller in Settings → Installed apps.
- The uninstaller asks the service to unmount everything, stops and deletes
  the service, and removes program files. It never touches external drives.
  WinFsp stays installed (other software may use it) unless the user removes it.

## Drive states

`Detected` → `Supported` | `Unsupported (reason)` → `Mounting` → `Mounted (E:)`
→ `Unmounting` → `Supported`; any state → `Disconnected`; mount errors →
`Failed (reason, log reference)`.

The UI never shows `Mounted` until Windows itself reports the drive letter.

## Read-only safety model

Writes are blocked by three independent layers, so a bug in one cannot reach
the disk:

1. **Operating system**: the disk handle is opened with `GENERIC_READ` only.
   Windows rejects any write through it.
2. **Engine**: libhfs has no write path (its callback table only has open,
   read and close), and the journal overlay keeps replayed blocks in memory.
3. **WinFsp**: `ReadOnlyVolume` makes the kernel driver reject every write,
   rename, delete and attribute change before it reaches our code.

The service never issues disk IOCTLs that change state (no
`IOCTL_DISK_SET_*`, no format, no partition-table changes, no `chkdsk`
equivalent), and never "repairs" a volume.

## Repository layout (planned)

```
engine/            C engine + vendored hfsfuse libs (third_party/hfsfuse)
mount/             WinFsp mount host
service/           Windows service
app/               WinUI 3 desktop application
installer/         NSIS script, icons, WinFsp MSI (downloaded at build time)
tests/             engine tests, image fixtures, checksum tools
docs/              architecture, phase reports, licenses, user guide
.github/workflows/ Windows and Linux CI
```
