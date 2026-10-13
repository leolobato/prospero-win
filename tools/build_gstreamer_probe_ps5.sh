#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
[ "$#" -eq 3 ] || { echo "usage: $0 SDK MEDIA_ROOT OUTPUT_ELF" >&2; exit 2; }
sdk=$1
media_root=$2
output=$3
source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
export PKG_CONFIG_LIBDIR="$media_root/lib/pkgconfig"
# pkg-config emits compiler/linker arguments, intentionally split here.
"$sdk/bin/prospero-clang" -O2 -Wall -Wextra -Werror \
    "$source_root/tools/pw_gstreamer_decode_probe.c" \
    -Wl,--start-group -L "$media_root/lib/gstreamer-1.0" \
    -lgstcoreelements -lgstapp -lgstplayback -lgsttypefindfunctions \
    -lgstaudioconvert -lgstaudioresample -lgstvideoconvertscale \
    -lgstisomp4 -lgstavi -lgstvideoparsersbad -lgstlibav \
    $(pkg-config --static --cflags --libs gstreamer-app-1.0 gstreamer-video-1.0 \
        gstreamer-audio-1.0 gstreamer-pbutils-1.0 gstreamer-riff-1.0 \
        gstreamer-rtp-1.0 gstreamer-codecparsers-1.0 gstreamer-net-1.0 \
        gstreamer-fft-1.0 gio-2.0 libavfilter libavformat libavcodec \
        libavutil libswscale libswresample) \
    -Wl,--end-group -o "$output"
