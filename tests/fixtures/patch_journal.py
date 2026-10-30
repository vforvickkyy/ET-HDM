#!/usr/bin/env python3
"""Give a journaled HFS+ volume image an initialised journal header, as macOS
does on first mount. Test-fixture tool only: it modifies the image it is given.

  --state clean   start == end: what a cleanly ejected drive looks like
  --state dirty   end > start: a transaction was never applied, as when a drive
                  is unplugged from a Mac without ejecting
  --endian        little (Intel / Apple silicon Macs) or big (PowerPC Macs)

usage: patch_journal.py <volume-image> --state clean|dirty [--endian little|big]
"""

import argparse
import struct
import sys

JOURNAL_MAGIC = 0x4A4E4C78
JOURNAL_ENDIAN = 0x12345678
JIB_NEED_INIT = 0x4
JHDR_SIZE = 512


def checksum(data):
    """calc_checksum() from Apple's vfs_journal.c"""
    c = 0
    for b in data:
        c = ((c << 8) ^ (c + b)) & 0xFFFFFFFF
    return ~c & 0xFFFFFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--state", choices=("clean", "dirty"), required=True)
    ap.add_argument("--endian", choices=("little", "big"), default="little")
    a = ap.parse_args()
    order = "<" if a.endian == "little" else ">"

    with open(a.image, "r+b") as f:
        f.seek(1024)
        vh = f.read(512)
        sig, = struct.unpack(">H", vh[0:2])
        attrs, = struct.unpack(">I", vh[4:8])
        if sig not in (0x482B, 0x4858) or not attrs & (1 << 13):
            sys.exit("not a journaled HFS+ volume")
        jib_block, = struct.unpack(">I", vh[12:16])
        block_size, = struct.unpack(">I", vh[40:44])

        f.seek(jib_block * block_size)
        jib = bytearray(f.read(52))
        flags, = struct.unpack(">I", jib[0:4])
        joff, jsize = struct.unpack(">QQ", jib[36:52])
        jib[0:4] = struct.pack(">I", flags & ~JIB_NEED_INIT)
        f.seek(jib_block * block_size)
        f.write(jib)

        start = JHDR_SIZE
        end = start if a.state == "clean" else start + 4096
        fmt = order + "IIQQQIII"
        hdr = struct.pack(fmt, JOURNAL_MAGIC, JOURNAL_ENDIAN, start, end, jsize, 4096, 0, JHDR_SIZE)
        hdr = struct.pack(fmt, JOURNAL_MAGIC, JOURNAL_ENDIAN, start, end, jsize, 4096, checksum(hdr), JHDR_SIZE)
        f.seek(joff)
        f.write(hdr)


if __name__ == "__main__":
    main()
