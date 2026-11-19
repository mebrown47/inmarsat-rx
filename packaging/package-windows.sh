#!/usr/bin/env bash
# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
#
# package-windows.sh — build the self-contained Windows .exe and zip it with the sample recording,
# README-Windows.txt, LICENSE and NOTICE. Run from an MSYS2 UCRT64 shell in the repository:
#     packaging/package-windows.sh            -> dist/inmarsat-rx-windows-x64-<version>.zip
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
[ "${MSYSTEM:-}" = UCRT64 ] || { echo "run this from an MSYS2 UCRT64 shell (MSYSTEM=$MSYSTEM)"; exit 1; }
command -v zip >/dev/null || { echo "needs zip: pacman -S zip"; exit 1; }
VER="$(git describe --always --dirty --tags 2>/dev/null || echo unknown)"
B=build-package
rm -rf "$B"; mkdir -p "$B"
# NATIVE=OFF: -march=native would tune for this machine's CPU and could crash on an older one.
cmake -S . -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNATIVE=OFF -DSTATIC=ON > "$B/package.log"
cmake --build "$B" >> "$B/package.log"
# It must stand alone: no DLLs beyond Windows' own.
extra="$(ldd "$B/inmarsat-rx.exe" | grep -viE '/c/windows|system32' || true)"
[ -z "$extra" ] || { echo "the .exe needs DLLs beyond Windows':"; echo "$extra"; exit 1; }
tests/run_tests.sh "$B" | tail -1
NAME="inmarsat-rx-windows-x64-$VER"
S="dist/$NAME"
rm -rf "$S" "$S.zip"; mkdir -p "$S/sample"
cp "$B/inmarsat-rx.exe" "$S/"
cp tests/data/aerol600-i4f3-12k5.cs8 tests/data/aerol600-i4f3-12k5.sigmf-meta "$S/sample/"
cp LICENSE NOTICE "$S/"
# Windows line ends for the text files, so Notepad shows them properly.
for f in packaging/README-Windows.txt; do sed 's/$/\r/' "$f" > "$S/$(basename "$f")"; done
for f in LICENSE NOTICE; do sed -i 's/$/\r/' "$S/$f"; done
printf 'inmarsat-rx %s\r\nBuilt %s with %s (MSYS2 UCRT64), statically linked, no CPU-specific optimisation.\r\n' \
  "$VER" "$(date -u +%Y-%m-%dT%H:%MZ)" "$(gcc --version | head -1)" > "$S/VERSION.txt"
(cd dist && zip -qr "$NAME.zip" "$NAME")
echo "dist/$NAME.zip ($(du -k "dist/$NAME.zip" | cut -f1) KB)"
