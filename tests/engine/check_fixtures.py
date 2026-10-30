#!/usr/bin/env python3
"""Cross-platform check of an ehfs binary against prebuilt fixture images.

CI uses this on Windows: the Linux job builds the HFS+ images (the tools to
make them only exist there) and the Windows job verifies them with the native
ehfs.exe, so the Windows disk backend is tested on a real Windows system.

usage: check_fixtures.py <ehfs> <fixtures-dir>
"""

import os
import subprocess
import sys
import unicodedata


def run(ehfs, *args):
    p = subprocess.run([ehfs, *args], capture_output=True, timeout=300)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def main():
    ehfs, fix = sys.argv[1], sys.argv[2]
    expected = [l.rstrip("\n") for l in open(os.path.join(fix, "manifest.txt"), encoding="utf-8")]
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        print(("  ok    " if ok else "  FAIL  ") + name + ("" if ok else f"  {detail}"))
        failures += not ok

    for img in ("tree.iso", "gpt.img", "gpt4k.img", "mbr.img"):
        rc, out, err = run(ehfs, "hash", os.path.join(fix, img))
        got = sorted(unicodedata.normalize("NFC", l.rstrip("\r")) for l in out.splitlines() if l)
        check(f"{img}: all {len(expected)} files match", rc == 0 and got == expected,
              f"rc={rc} read={len(got)} {err.strip()[:200]}")

    rc, out, _ = run(ehfs, "cat", os.path.join(fix, "gpt.img"), "/Unicode/Café.txt")
    check("non-English path argument", rc == 0 and out == "Café.txt" * 3, repr(out[:40]))

    rc, out, _ = run(ehfs, "scan", "-t", os.path.join(fix, "dirty.img"))
    check("dirty journal detected", "\tneeds-mac\t1\t1\t" in out, out.strip())
    rc, _, _ = run(ehfs, "ls", os.path.join(fix, "dirty.img"), "/")
    check("dirty journal refused", rc == 1, f"rc={rc}")

    print("failed" if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
