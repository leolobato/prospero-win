#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
[ "$#" -eq 1 ] || { echo "usage: $0 OUTPUT_DIR" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$1"
# Compile from a local snapshot; shared-host filesystem readers can otherwise
# retain a previous file size after an editor replaces the source.
probe_tmp=$(mktemp -d)
trap 'rm -rf "$probe_tmp"' EXIT HUP INT TERM
cp "$root/tools/pw_mf_decode_probe.c" "$probe_tmp/probe.c"
for arch in i686 x86_64; do
    "$arch-w64-mingw32-gcc" -std=c11 -O2 -Wall -Wextra -Werror -municode \
        "$probe_tmp/probe.c" -o "$1/pw_mf_decode_probe-$arch.exe" \
        -lmfreadwrite -lmfplat -lmfuuid -lole32 -lpropsys -luuid -lpsapi
done
