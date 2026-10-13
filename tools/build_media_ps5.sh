#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
exec python3 "$(dirname -- "$0")/build_media_ps5.py" "$@"
