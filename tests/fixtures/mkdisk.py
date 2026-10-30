#!/usr/bin/env python3
"""Build a partitioned disk image from volume images, the way a Mac or
Windows formats an external drive.

usage: mkdisk.py --scheme gpt|mbr [--lbs 512|4096] [--efi] --out disk.img vol1.img [vol2.img ...]

GPT disks get a protective MBR, primary and backup headers with valid CRCs,
and (with --efi) a 200 MB-style EFI partition first, like macOS Disk Utility.
HFS+ partitions use the Apple HFS+ type (GPT) or 0xAF (MBR).
"""

import argparse
import os
import struct
import uuid
import zlib

HFS_GUID = "48465300-0000-11AA-AA11-00306543ECAC"
EFI_GUID = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
ALIGN = 1 << 20
EFI_SIZE = 4 << 20   # kept small for tests


def align(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


def gpt_entry(type_guid, first, last, name):
    return (uuid.UUID(type_guid).bytes_le + uuid.uuid4().bytes_le +
            struct.pack("<QQQ", first, last, 0) + name.encode("utf-16-le")[:72].ljust(72, b"\0"))


def gpt_header(lbs, current, backup, first_usable, last_usable, disk_guid, entries_lba, entries_crc):
    fields = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92, 0, 0, current, backup,
                         first_usable, last_usable, disk_guid, entries_lba, 128, 128, entries_crc)
    crc = zlib.crc32(fields)
    fields = fields[:16] + struct.pack("<I", crc) + fields[20:]
    return fields.ljust(lbs, b"\0")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scheme", choices=("gpt", "mbr"), required=True)
    ap.add_argument("--lbs", type=int, default=512)
    ap.add_argument("--efi", action="store_true")
    ap.add_argument("--out", required=True)
    ap.add_argument("volumes", nargs="+")
    a = ap.parse_args()
    lbs = a.lbs

    parts = []   # (type, offset, size, source or None, name)
    pos = ALIGN
    if a.efi and a.scheme == "gpt":
        parts.append((EFI_GUID, pos, EFI_SIZE, None, "EFI System Partition"))
        pos += EFI_SIZE
    for i, vol in enumerate(a.volumes):
        size = os.path.getsize(vol)
        size += -size % lbs   # pad the partition to whole logical blocks
        parts.append((HFS_GUID if a.scheme == "gpt" else 0xAF, pos, size, vol, f"Volume {i + 1}"))
        pos = align(pos + size)
    total = pos + ALIGN
    lbas = total // lbs

    with open(a.out, "wb") as out:
        out.truncate(total)

        if a.scheme == "mbr":
            mbr = bytearray(512)
            for i, (ptype, off, size, _, _) in enumerate(parts[:4]):
                mbr[446 + 16 * i: 446 + 16 * (i + 1)] = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", ptype,
                                                                   b"\xfe\xff\xff", off // lbs, size // lbs)
            mbr[510:512] = b"\x55\xaa"
            out.seek(0)
            out.write(mbr)
        else:
            entries = b"".join(gpt_entry(t, off // lbs, (off + size) // lbs - 1, name) for t, off, size, _, name in parts)
            entries = entries.ljust(128 * 128, b"\0")
            entries_lbas = len(entries) // lbs
            ecrc = zlib.crc32(entries)
            disk_guid = uuid.uuid4().bytes_le
            first_usable, last_usable = 2 + entries_lbas, lbas - entries_lbas - 2

            mbr = bytearray(512)
            mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", 1, min(lbas - 1, 0xFFFFFFFF))
            mbr[510:512] = b"\x55\xaa"
            out.seek(0)
            out.write(mbr)

            out.seek(lbs)
            out.write(gpt_header(lbs, 1, lbas - 1, first_usable, last_usable, disk_guid, 2, ecrc))
            out.seek(2 * lbs)
            out.write(entries)
            out.seek((lbas - 1 - entries_lbas) * lbs)
            out.write(entries)
            out.seek((lbas - 1) * lbs)
            out.write(gpt_header(lbs, lbas - 1, 1, first_usable, last_usable, disk_guid, lbas - 1 - entries_lbas, ecrc))

        for _, off, size, src, _ in parts:
            if not src:
                continue
            with open(src, "rb") as f:
                out.seek(off)
                for block in iter(lambda: f.read(1 << 20), b""):
                    out.write(block)


if __name__ == "__main__":
    main()
