#!/usr/bin/env python3
"""Create a deterministic folder tree for HFS+ test images, plus a SHA-256
manifest ("<hash>  <relative path>", sorted) to compare the engine's output
against. Content is pseudo-random from a fixed seed so runs are reproducible.

usage: make_tree.py <output-dir> <manifest-file> [--large]

--large creates a tree holding only one file larger than 4 GiB.
"""

import hashlib
import os
import random
import sys
import unicodedata

BLOCK = 4096


def write(root, rel, data):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def write_streamed(root, rel, size, seed):
    """Large file written in chunks so it never sits in memory."""
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    rng = random.Random(seed)
    chunk = rng.randbytes(1 << 20)
    with open(path, "wb") as f:
        left = size
        while left:
            n = min(left, len(chunk))
            f.write(chunk[:n])
            left -= n
            chunk = chunk[1:] + chunk[:1]   # vary content between chunks


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    root, manifest = sys.argv[1], sys.argv[2]
    large = "--large" in sys.argv[3:]
    rng = random.Random(20261030)
    os.makedirs(root, exist_ok=True)

    if large:
        # beyond 4 GiB: exercises 64-bit offsets end to end
        write_streamed(root, "Large/over_4GiB.bin", (4 << 30) + 12345, 7)
    else:
        make_main_tree(root, rng)
    write_manifest(root, manifest)


def make_main_tree(root, rng):
    # sizes around allocation-block boundaries
    for size in (0, 1, BLOCK - 1, BLOCK, BLOCK + 1, 3 * BLOCK + 7, (1 << 20) + 1):
        write(root, f"sizes/file_{size}.bin", rng.randbytes(size))

    # a medium file that spans many blocks, like a short clip
    write(root, "Footage/Day 01/A001_C002.mov", rng.randbytes(24 * (1 << 20) + 123))

    # an image-sequence folder with many files
    for frame in range(1, 2001):
        write(root, f"Footage/Day 01/VFX_shot_010/shot_010.{frame:06d}.exr", rng.randbytes(512 + frame % 97))

    # names that need Unicode handling (stored decomposed on HFS+). Greek is left out because
    # xorriso decomposes the tonos accent to U+030D instead of U+0301 as macOS does, so its
    # names cannot be compared exactly; Mac-made fixtures cover Greek instead.
    for name in ("Café.txt", "Ñandú – résumé.txt", "फ़ाइल.txt", "テスト動画.txt", "🎬 clip notes.txt",
                 "naïve file.txt"):
        write(root, f"Unicode/{name}", name.encode("utf-8") * 3)

    # deep nesting and spaces/dots in names
    write(root, "Projects/2026/Client A/Edit v3/final.final.prproj", rng.randbytes(9000))
    write(root, "Projects/2026/Client A/.hidden_settings", b"hidden=true\n")
    write(root, "Projects/" + "/".join(f"level{i}" for i in range(12)) + "/deep.txt", b"deep\n")


def write_manifest(root, manifest):
    lines = []
    for dirpath, _, files in os.walk(root):
        for fn in files:
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            h = hashlib.sha256()
            with open(full, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    h.update(block)
            # HFS+ returns names in its decomposed form; compare in NFC
            lines.append(f"{h.hexdigest()}  {unicodedata.normalize('NFC', rel)}")
    lines.sort()
    with open(manifest, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
