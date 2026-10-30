# Eastape HFS

Mac drive support for Windows. Eastape HFS is a free, internal Windows desktop
utility that detects Mac-formatted **HFS+** external drives and mounts them as a
normal drive letter in **This PC**, read-only, so files can be browsed in
File Explorer and opened directly in Premiere Pro, DaVinci Resolve, After
Effects and other Windows applications.

It replaces Eastape's dependency on Paragon HFS+ for Windows.

## Status

| Phase | Name | Status |
|---|---|---|
| 1 | Research and source inspection | Complete |
| 2 | HFS+ filesystem engine | **In progress:** read path working and tested ([progress report](docs/phase-2-progress-report.md)) |
| 3 | Windows mounting | Not started |
| 4 | Drive management | Not started |
| 5 | Desktop application | Not started |
| 6 | Installer | Not started |
| 7 | Testing and release | Not started |

Read/write support (Paragon parity) is planned as a second track after the
read-only v1. See the progress report, section 5.

## Quick start (engine tool)

```bash
cmake -S . -B build && cmake --build build -j
build/tools/ehfs-cli/ehfs scan drive.img
build/tools/ehfs-cli/ehfs ls -R drive.img /
```

On Windows, `ehfs.exe scan \\.\PhysicalDrive2` reads a real drive (run as
administrator). Details: [docs/building.md](docs/building.md).

## Documents

- [Phase 1 feasibility report](docs/phase-1-feasibility-report.md): what was
  inspected, what was selected and why, risks, and the Phase 2 plan
- [Phase 2 progress report](docs/phase-2-progress-report.md): engine status,
  test results, bugs found, read/write track
- [Building and testing](docs/building.md)
- [Architecture](docs/architecture.md): components, interfaces and the
  read-only safety model
- [Third-party licenses](docs/third-party-licenses.md): every planned
  dependency, its license, and what that means for Eastape

## Planned stack (pending approval)

| Layer | Choice |
|---|---|
| HFS+ engine | hfsfuse's `libhfs` + `libhfsuser` (MIT / BSD), C, vendored at a pinned commit |
| Drive letter / Explorer integration | WinFsp (official signed installer, native API) |
| Drive service and mount host | C/C++ Windows service |
| Desktop app | C# / .NET / WinUI 3, dark theme |
| Installer | NSIS `Setup.exe` that also installs WinFsp (decision pending, see report) |

Targets Windows 11 and Windows 10 22H2, x64.
