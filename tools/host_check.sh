#!/usr/bin/env bash
# host_check.sh — validate fat16::build() on a Linux host using fsck.fat + mtools.
#
# Requires: g++, dosfstools (fsck.fat), mtools. Debian: sudo apt-get install dosfstools mtools
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "== compile =="
g++ -O2 -std=c++17 -I include src/fat16.cpp tools/dump_image.cpp -o "$WORK/dump_image"

echo "== build sample image =="
mkdir -p "$WORK/expected"
"$WORK/dump_image" "$WORK/gotek.img" "$WORK/expected"

echo "== fsck.fat (structural integrity) =="
fsck.fat -n "$WORK/gotek.img"

echo "== mdir (LFN names roundtrip) =="
mdir -i "$WORK/gotek.img" ::

echo "== mcopy data roundtrip =="
fail=0
for f in "$WORK"/expected/*; do
  base="$(basename "$f")"
  if ! mcopy -i "$WORK/gotek.img" "::$base" "$WORK/out.bin" 2>/dev/null; then
    # mtools sometimes wants the short name; fall back to it via mdir listing
    echo "  FAILED to read '$base' by long name" >&2
    fail=1
    continue
  fi
  if cmp -s "$f" "$WORK/out.bin"; then
    echo "  OK   $base"
  else
    echo "  MISMATCH $base" >&2
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "FAILED" >&2
  exit 1
fi
echo "ALL CHECKS PASSED"
