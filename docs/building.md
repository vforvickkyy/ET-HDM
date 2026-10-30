# Building and testing

## Engine and `ehfs` tool

Requirements: CMake 3.16+, a C11 compiler (GCC or Clang), zlib.

### Linux / macOS

```bash
cmake -S . -B build
cmake --build build -j
```

The tool is `build/tools/ehfs-cli/ehfs`.

### Windows (MSYS2 MinGW64 shell)

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-zlib
cmake -S . -B build -G Ninja -DEHFS_BUILD_TESTS=OFF
cmake --build build
```

### Cross-compiling for Windows from Linux

```bash
sudo apt-get install mingw-w64 libz-mingw-w64-dev
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DEHFS_BUILD_TESTS=OFF
cmake --build build-win -j
```

## Using `ehfs`

Everything is read-only. On Windows, reading a physical drive needs an
administrator prompt.

```bash
ehfs scan  <disk>               # partitions and what is on them
ehfs info  <disk>               # details of the first usable HFS+ volume
ehfs ls    <disk> -R /Footage   # list a folder recursively
ehfs cat   <disk> /path/file    # write a file to standard output
ehfs hash  <disk>               # SHA-256 of every file (for verification)
```

`<disk>` is an image file, `/dev/sdX` on Linux, or `\\.\PhysicalDriveN` on
Windows (find N in Disk Management). Add `-p N` to pick a partition number
shown by `scan`. `--allow-dirty` opens a volume whose journal has unsaved
changes. The view may then be missing the most recent changes made on the Mac.

## Tests

```bash
sudo apt-get install hfsprogs xorriso
bash tests/engine/run_tests.sh build/tools/ehfs-cli/ehfs /tmp/ehfs-tests
```

or `ctest --test-dir build`. Set `EHFS_TEST_LARGE=1` to add a file larger
than 4 GiB (needs about 9 GB of free disk).

The suite generates HFS+ images with known contents and checks that:
- every file reads back byte-for-byte, through APM, GPT (512 and 4096-byte
  sectors), MBR, and no partition table,
- HFS+, HFSX, the HFS wrapper, HFS Standard, APFS, EFI and unknown volumes are
  classified correctly,
- dirty journals (Intel and PowerPC byte order) are detected and refused,
- damaged volumes fail with an error, never a crash or hang,
- no image is modified.

CI (`.github/workflows/engine.yml`) runs the suite on Linux, then builds
`ehfs.exe` natively on Windows and checks the same images with it
(`tests/engine/check_fixtures.py`).
