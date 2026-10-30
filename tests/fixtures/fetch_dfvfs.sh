#!/usr/bin/env bash
# Download small macOS-made test images from the dfvfs project (Apache-2.0),
# pinned to one commit and verified by SHA-256.
#
# usage: fetch_dfvfs.sh <output-dir>
set -euo pipefail

OUT=${1:?output directory}
COMMIT=e86f63c2f0a704cbcdd621f4e4fa022e653e74fa
BASE=https://raw.githubusercontent.com/log2timeline/dfvfs/$COMMIT/test_data

FILES="
03cfaa73e1bc61ee19d285252ae6919afc9990506ad1c2919249d1e11d289b08 hfsplus.raw
897f487f4971e72c762e00460935e3b25176cda35620b508b490f65a7e0118ff apm.dmg
88f1f131409eda86b86b31bb75f11f8bda5f87968ace2cda5c9e9ab1e4c12379 hfs.raw
e3e3adcbbf189403d892b013d6cba155f2e58e42ff5eb541ec681c37a91a3f29 apfs.raw
33fe6f183aeb1a95fec68efdab17d59aedbad3d8ef1a117d411117376d9d8485 apfs_encrypted.dmg
"

mkdir -p "$OUT"
echo "$FILES" | while read -r sum name; do
	[ -n "$name" ] || continue
	if [ ! -f "$OUT/$name" ] || ! echo "$sum  $OUT/$name" | sha256sum -c --quiet - 2>/dev/null; then
		curl -fsSL -o "$OUT/$name.tmp" "$BASE/$name"
		mv "$OUT/$name.tmp" "$OUT/$name"
	fi
	echo "$sum  $OUT/$name" | sha256sum -c --quiet -
done
