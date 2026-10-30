# Third-Party Components and Licenses

Status: hfsfuse libraries **vendored** at upstream commit `eed780c` in
`engine/third_party/hfsfuse/` (license files included, changes listed in
`EASTAPE-PATCHES.md`). Other entries are still planned and are re-checked
against the exact files when added.

This is an engineering review, not legal advice.

## Key point: internal use vs. distribution

Eastape HFS is an internal tool. Under the GPL and similar licenses, installing
copies on an organisation's own PCs is **not** "distribution". The obligations
below only matter if Eastape ever gives the software to someone outside the
company (clients, freelancers, a public download). Before doing that, re-read
this file, especially the WinFsp row.

## Runtime components (shipped in Setup.exe)

| Component | Used for | License | Obligations for Eastape |
|---|---|---|---|
| [WinFsp](https://github.com/winfsp/winfsp) (official MSI, unmodified) | Drive letter / Explorer integration, signed kernel driver | GPLv3 with a FLOSS exception; commercial license available | Internal use: none. External distribution: either release Eastape HFS under an open-source license and show "WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos" plus a repo link in the UI and docs, **or** buy a commercial license. We show the notice in the About screen regardless. |
| hfsfuse `libhfsuser` (hfsfuse project code) | HFS+ engine support layer | MIT | Keep the copyright notice. |
| NetBSD `libhfs` (via hfsfuse) | HFS+ on-disk structures, B-trees, catalog | BSD-2-Clause (NetBSD Foundation) | Keep the copyright notice. |
| utf8proc (via hfsfuse) | Unicode normalization of file names | MIT | Keep the notice. |
| ublio (via hfsfuse) | Block read cache | BSD-style (per file) | Keep the notices. |
| uthash (via hfsfuse) | Hash tables | BSD (1-clause, revised) | Keep the notice. |
| LZFSE (via hfsfuse) | Decompress LZFSE-compressed files | BSD-3-Clause (Apple) | Keep the notice. |
| LZVN (via hfsfuse) | Decompress LZVN-compressed files | Apple Public Source License 2.0 | Internal use: none. External distribution: source of any *modifications* to these files must be published. We don't plan to modify them. LZFSE's BSD-licensed LZVN decoder is a possible replacement if this ever matters. |
| zlib | Decompress zlib-compressed files | zlib license | None beyond not misrepresenting origin. |
| .NET runtime (self-contained) | Desktop app runtime | MIT | Keep notices. |
| Windows App SDK / WinUI 3 (self-contained) | Desktop UI framework | MIT (source); Microsoft redistributable terms for runtime binaries | Free to redistribute. |

## Build and test tools (not shipped)

| Tool | License / cost | Note |
|---|---|---|
| NSIS (recommended installer builder) | zlib/libpng, free for any use | Recommended because it has no payment conditions at all. |
| Inno Setup | Free license, but since v6.5 the authors ask commercial organisations, including in-house use, to buy a commercial license (they say it isn't strictly required) | Doesn't meet the "no paid tools" rule cleanly. |
| WiX Toolset v6/v7 | Open Source Maintenance Fee. v7's EULA asks for a fee once a project earns US$10,000+/year | Probably free for an internal tool that earns nothing, but needs EULA acceptance and a judgement call. |
| Visual Studio Community / Build Tools | Free for small organisations; larger enterprises need paid editions (check Microsoft's terms) | Alternative: build in GitHub Actions Windows runners. |
| HFSExplorer | GPL-3.0 | Only used as an independent cross-check during testing. Not linked or shipped. |
| libfshfs (libyal) | LGPL-3.0-or-later | Possible second cross-check reader in tests. Not shipped. |
| dfvfs test images | Apache-2.0 | Small macOS-made HFS+/APM/APFS images used as test fixtures. Downloaded at test time (pinned commit, SHA-256 checked), not stored in the repo. |
| hfsprogs (`mkfs.hfsplus`, `fsck.hfsplus`), xorriso | APSL-2.0 / GPL-3.0 | Linux tools that generate test images in CI. Not linked or shipped. |
| Apple `hfs` / `livefiles_hfs_plugin` | APSL-2.0 | Candidate base for the read/write engine (spike pending). Internal use: no obligations. External distribution: publish source of modified Apple files. |

## Explicitly not used

- Paragon or any other commercial filesystem SDK (project rule 3 and 4).
- Apple Boot Camp HFS+ drivers: licensed for Apple hardware only.
- Linux kernel `hfsplus` (GPL-2.0, kernel code): reference reading only.
- Apple `hfs` kernel-extension sources (APSL-2.0): format reference only. The
  separate user-space plugin from the same project is the read/write candidate
  listed above.
