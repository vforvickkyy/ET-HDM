#!/usr/bin/env bash
# Eastape HFS engine tests.
#
# Builds HFS+ images with known content, runs the ehfs tool against them and
# checks that every file reads back byte-for-byte, that every kind of volume
# is classified correctly, that damaged images fail cleanly (never crash), and
# that no image is modified by the engine.
#
# usage: run_tests.sh <ehfs-binary> <work-dir>
# env:   EHFS_TEST_LARGE=1   also test a file larger than 4 GiB (needs ~9 GB disk)
#        EHFS_DFVFS_DIR=dir  use already-downloaded dfvfs images instead of fetching them
#
# Exit status: 0 all passed, 1 failures, 77 skipped (tools missing).

set -u
export LC_ALL=C.UTF-8 LANG=C.UTF-8

EHFS_BIN=$(realpath "${1:?path to ehfs}")
mkdir -p "${2:?work directory}" && WORK=$(cd "$2" && pwd)
FIX=$(cd "$(dirname "$0")/../fixtures" && pwd)

# every engine call is time-limited: a hang is a failure, never a stuck test run
EHFS="$WORK.ehfs-timeout"
mkdir -p "$(dirname "$EHFS")"
printf '#!/bin/sh\nexec timeout 120 "%s" "$@"\n' "$EHFS_BIN" >"$EHFS"
chmod +x "$EHFS"

for tool in mkfs.hfsplus xorriso python3 sha256sum; do
	command -v "$tool" >/dev/null || { echo "skipping engine tests: $tool not installed"; exit 77; }
done

PASS=0 FAIL=0 SKIP=0
pass() { PASS=$((PASS + 1)); echo "  ok    $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1"; }
skip() { SKIP=$((SKIP + 1)); echo "  skip  $1"; }
check() { local name=$1; shift; if "$@"; then pass "$name"; else fail "$name"; fi; }

rm -rf "$WORK" && mkdir -p "$WORK" && cd "$WORK" || exit 1

# field N of the tab-separated scan line for partition number P
scan_field() { "$EHFS" scan -t "$1" | awk -F'\t' -v p="$2" -v n="$3" '$3 == p { print $n }'; }
# does any partition line match "fs<TAB>status"?
scan_has() { "$EHFS" scan -t "$1" | awk -F'\t' -v fs="$2" -v st="$3" '$7 == fs && $8 == st { f = 1 } END { exit !f }'; }

# hash every file and compare with an expected manifest (names compared in NFC)
compare_manifest() {
	local img=$1 expected=$2
	shift 2
	"$EHFS" hash "$img" "$@" >got.raw 2>hash.err || { sed 's/^/    /' hash.err | head -5; return 1; }
	python3 - "$expected" got.raw <<'EOF'
import sys, unicodedata
exp = [l.rstrip("\n") for l in open(sys.argv[1], encoding="utf-8")]
got = sorted(unicodedata.normalize("NFC", l.rstrip("\n")) for l in open(sys.argv[2], encoding="utf-8"))
if got != exp:
    diff = sorted(set(got) ^ set(exp))
    print(f"    read {len(got)} files, expected {len(exp)}; differences: {diff[:4]}")
    sys.exit(1)
EOF
}

# a command must fail with a normal exit code (1), never crash on a signal
fails_cleanly() {
	"$@" >/dev/null 2>&1
	local rc=$?
	[ "$rc" -ne 0 ] && [ "$rc" -lt 128 ]
}

echo "== building fixtures"
python3 "$FIX/make_tree.py" tree manifest.txt || exit 1
echo "   $(wc -l <manifest.txt) files in source tree"
xorriso -as mkisofs -input-charset utf-8 -hfsplus -V "Eastape Café 🎬" \
	-o tree.iso tree >xorriso.log 2>&1 || { cat xorriso.log; exit 1; }

# raw HFS+ volume = the Apple_HFS partition of the hybrid ISO
read -r hoff hlen < <("$EHFS" scan -t tree.iso | awk -F'\t' '$6 == "Apple_HFS" { print $4, $5 }')
dd if=tree.iso of=volume.img bs=512 skip=$((hoff / 512)) count=$((hlen / 512)) status=none

python3 "$FIX/mkdisk.py" --scheme gpt --efi --out gpt.img volume.img
python3 "$FIX/mkdisk.py" --scheme gpt --lbs 4096 --efi --out gpt4k.img volume.img
python3 "$FIX/mkdisk.py" --scheme mbr --out mbr.img volume.img
cp gpt.img gpt_badprimary.img
printf '\xff' | dd of=gpt_badprimary.img bs=1 seek=$((512 + 100)) conv=notrunc status=none   # breaks primary GPT CRC

mk_empty() { local img=$1; shift; truncate -s 64M "$img" && mkfs.hfsplus "$@" "$img" >/dev/null; }
mk_empty plain.img -v "Plain Volume"
mk_empty journaled.img -J -v "Journaled Volume"
mk_empty hfsx.img -s -J -v "Case Sensitive"
mk_empty wrapper.img -w -v "Wrapped Volume"
mk_empty unicode_name.img -J -v "Fotos Café"
cp journaled.img journal_clean.img && python3 "$FIX/patch_journal.py" journal_clean.img --state clean
cp journaled.img dirty.img && python3 "$FIX/patch_journal.py" dirty.img --state dirty
cp journaled.img dirty_be.img && python3 "$FIX/patch_journal.py" dirty_be.img --state dirty --endian big

head -c $((1 << 20)) /dev/urandom >garbage.img
cp volume.img badheader.img
printf '\x00\x00\x00\x00' | dd of=badheader.img bs=1 seek=$((1024 + 40)) conv=notrunc status=none   # block size = 0
head -c $((hlen / 2)) volume.img >truncated.img
cp volume.img badcatalog.img
python3 - badcatalog.img <<'EOF'
import os, struct, sys
with open(sys.argv[1], "r+b") as f:
    f.seek(1024)
    vh = f.read(512)
    block_size, = struct.unpack(">I", vh[40:44])
    cat_start, = struct.unpack(">I", vh[272 + 16:272 + 20])   # first extent of the catalog file
    f.seek(cat_start * block_size)
    f.write(os.urandom(4 * block_size))
EOF

IMAGES="tree.iso volume.img gpt.img gpt4k.img mbr.img gpt_badprimary.img plain.img journaled.img hfsx.img wrapper.img
        unicode_name.img journal_clean.img dirty.img dirty_be.img garbage.img badheader.img truncated.img badcatalog.img"
sha256sum $IMAGES >images.sha256

echo "== reading files (byte-for-byte against the source tree)"
check "APM hybrid image: every file matches"           compare_manifest tree.iso manifest.txt
check "raw volume (no partition table): every file matches" compare_manifest volume.img manifest.txt
check "GPT disk with EFI partition: every file matches" compare_manifest gpt.img manifest.txt
check "GPT disk with 4096-byte sectors: every file matches" compare_manifest gpt4k.img manifest.txt
check "MBR disk (type 0xAF): every file matches"        compare_manifest mbr.img manifest.txt
check "GPT with damaged primary header: backup used, every file matches" compare_manifest gpt_badprimary.img manifest.txt
check "single file read (cat)" bash -c '[ "$("$0" cat volume.img "/Unicode/Café.txt")" = "Café.txtCafé.txtCafé.txt" ]' "$EHFS"
check "listing a 2000-file folder" bash -c '[ "$("$0" ls volume.img "/Footage/Day 01/VFX_shot_010" | wc -l)" = 2000 ]' "$EHFS"
check "hidden file flagged" bash -c '"$0" ls volume.img "/Projects/2026/Client A" | grep -q "\.hidden_settings  \[hidden\]"' "$EHFS"
check "missing path reports an error" fails_cleanly "$EHFS" cat volume.img /does/not/exist

echo "== detection and classification"
check "APM: scheme, HFS+ partition and emoji volume name" bash -c '"$0" scan -t tree.iso | grep -q "^APM.*Apple_HFS	HFS+	supported.*	Eastape Café 🎬$"' "$EHFS"
check "GPT: EFI partition is not a Mac volume" scan_has gpt.img EFI not-mac
check "GPT: HFS+ partition supported"          scan_has gpt.img HFS+ supported
check "GPT 4K: sector size 4096 detected" bash -c '[ "$("$0" scan -t gpt4k.img | head -1 | cut -f2)" = 4096 ]' "$EHFS"
check "MBR: HFS+ partition supported"          scan_has mbr.img HFS+ supported
check "plain volume: name and not journaled" bash -c '"$0" scan -t plain.img | grep -q "	HFS+	supported	0	0	0	Plain Volume$"' "$EHFS"
check "journaled volume: clean journal"      bash -c '"$0" scan -t journaled.img | grep -q "	HFS+	supported	1	0	0	Journaled Volume$"' "$EHFS"
check "HFSX: detected as case-sensitive"     bash -c '"$0" scan -t hfsx.img | grep -q "	HFSX	supported	1	0	1	Case Sensitive$"' "$EHFS"
check "HFS wrapper: embedded HFS+ found"     bash -c '"$0" scan -t wrapper.img | grep -q "	HFS+	supported.*Wrapped Volume$"' "$EHFS"
check "accented volume name"                 bash -c '"$0" scan -t unicode_name.img | grep -q "	Fotos Café$"' "$EHFS"
check "empty volumes list without error" bash -c 'for i in plain journaled hfsx wrapper; do "$0" ls $i.img / >/dev/null || exit 1; done' "$EHFS"
check "random data is not a Mac volume"      scan_has garbage.img unknown not-mac

echo "== journal state (drive not ejected on a Mac)"
check "uninitialised journal (fresh volume) is clean" bash -c '"$0" scan -t journaled.img | grep -q "	HFS+	supported	1	0	"' "$EHFS"
check "initialised clean journal is clean"   bash -c '"$0" scan -t journal_clean.img | grep -q "	HFS+	supported	1	0	"' "$EHFS"
check "dirty journal (Intel byte order) reported as needs-mac" bash -c '"$0" scan -t dirty.img | grep -q "	HFS+	needs-mac	1	1	"' "$EHFS"
check "dirty journal (PowerPC byte order) reported as needs-mac" bash -c '"$0" scan -t dirty_be.img | grep -q "	HFS+	needs-mac	1	1	"' "$EHFS"
check "dirty journal refused by default"     fails_cleanly "$EHFS" ls dirty.img /
check "dirty journal opens with --allow-dirty" "$EHFS" ls dirty.img / --allow-dirty

echo "== damaged volumes fail cleanly"
check "invalid volume header reported damaged" scan_has badheader.img HFS+ damaged
check "invalid volume header: open fails cleanly" fails_cleanly "$EHFS" ls badheader.img /
check "truncated volume reported damaged"      scan_has truncated.img HFS+ damaged
check "truncated volume: open fails cleanly"   fails_cleanly "$EHFS" hash truncated.img /
check "corrupted catalog: scan does not crash" bash -c '"$0" scan badcatalog.img >/dev/null 2>&1; [ $? -lt 128 ]' "$EHFS"
check "corrupted catalog: reading fails cleanly" fails_cleanly "$EHFS" hash badcatalog.img /

echo "== macOS-made images (dfvfs)"
DFVFS=${EHFS_DFVFS_DIR:-$WORK/dfvfs}
if [ -n "${EHFS_DFVFS_DIR:-}" ] || bash "$FIX/fetch_dfvfs.sh" "$DFVFS" >/dev/null 2>&1; then
	check "hfsplus.raw: volume name"           bash -c '"$0" scan -t "$1/hfsplus.raw" | grep -q "	HFS+	supported	0	0	0	hfsplus_test$"' "$EHFS" "$DFVFS"
	check "hfsplus.raw: known file content"    bash -c '[ "$("$0" cat "$1/hfsplus.raw" /a_directory/another_file)" = "This is another file." ]' "$EHFS" "$DFVFS"
	check "hfsplus.raw: symlink target"        bash -c '"$0" ls "$1/hfsplus.raw" / | grep -q "^l .* 24 /a_link$"' "$EHFS" "$DFVFS"
	check "hfsplus.raw: all files readable"    "$EHFS" hash "$DFVFS/hfsplus.raw"
	check "apm.dmg: HFS+ inside Apple Partition Map" bash -c '"$0" scan -t "$1/apm.dmg" | grep -q "^APM.*Apple_HFS	HFS+	supported"' "$EHFS" "$DFVFS"
	check "hfs.raw: HFS Standard unsupported"  scan_has "$DFVFS/hfs.raw" HFS unsupported
	check "apfs.raw: APFS unsupported"         scan_has "$DFVFS/apfs.raw" APFS unsupported
	check "apfs_encrypted.dmg: APFS on GPT unsupported" scan_has "$DFVFS/apfs_encrypted.dmg" APFS unsupported
	check "APFS: open refused cleanly"         fails_cleanly "$EHFS" ls "$DFVFS/apfs.raw" /
else
	skip "dfvfs images could not be downloaded"
fi

echo "== read-only guarantee"
check "no test image was modified" sha256sum --quiet -c images.sha256

if [ -n "${EHFS_TEST_LARGE:-}" ]; then
	echo "== file larger than 4 GiB"
	rm -rf tree && python3 "$FIX/make_tree.py" large_tree large_manifest.txt --large &&
		xorriso -as mkisofs -input-charset utf-8 -hfsplus -iso-level 3 -V Large -o large.iso large_tree >xorriso_large.log 2>&1 &&
		rm -rf large_tree
	check "4 GiB+ file reads back byte-for-byte" compare_manifest large.iso large_manifest.txt
	rm -f large.iso
else
	skip "file larger than 4 GiB (set EHFS_TEST_LARGE=1)"
fi

echo
echo "passed $PASS, failed $FAIL, skipped $SKIP"
[ "$FAIL" -eq 0 ]
