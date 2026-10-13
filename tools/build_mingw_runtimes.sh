#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Rebuild the pinned compiler's static C++ runtimes with neutral diagnostics.
# --source is llvm-mingw 20260922 with llvm-project 23.1.2 checked out inside it.
# --toolchain must be a private installation of the pinned compiler archive.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
MINGW_COMMIT=0eca5ac93da14a74bc81c249e841356ececc5d95
LLVM_COMMIT=85ac560262434c9ccfc0c183ec22d4138ed647fb
source= work= toolchain= jobs=1
fail() { echo "build_mingw_runtimes: $*" >&2; exit 2; }
while [ $# -gt 0 ]; do
 case $1 in
 --source) source=$2; shift 2;;
 --work) work=$2; shift 2;;
 --toolchain) toolchain=$2; shift 2;;
 --jobs) jobs=$2; shift 2;;
 *) fail "unknown argument $1";;
 esac
done
[[ $jobs =~ ^[1-9][0-9]*$ ]] || fail 'jobs must be positive'
[ -n "$source" ] && [ -n "$work" ] && [ -n "$toolchain" ] || fail 'requires --source --work --toolchain'
git -C "$source" cat-file -e "$MINGW_COMMIT^{commit}" 2>/dev/null || fail 'missing pinned llvm-mingw source'
git -C "$source/llvm-project" cat-file -e "$LLVM_COMMIT^{commit}" 2>/dev/null || fail 'missing pinned LLVM source'
[ -x "$toolchain/bin/clang" ] || fail 'missing compiler installation'
mkdir -p "$work"
work=$(realpath "$work"); toolchain=$(realpath "$toolchain")
# Build immutable exports; never modify the user's source checkouts.
if [ ! -d "$work/source" ]; then
 staging=$(mktemp -d "$work/source.XXXXXX")
 git -C "$source" archive "$MINGW_COMMIT" | tar -x -C "$staging"
 mkdir "$staging/llvm-project"
 git -C "$source/llvm-project" archive "$LLVM_COMMIT" | tar -x -C "$staging/llvm-project"
 mv "$staging" "$work/source"
fi
python3 "$root/tools/mesa_zink_manifest.py" --verify-source "$work/source" "$source" "$MINGW_COMMIT"
python3 "$root/tools/mesa_zink_manifest.py" --verify-source "$work/source/llvm-project" "$source/llvm-project" "$LLVM_COMMIT"
flags="-ffile-prefix-map=$work=/src/llvm-mingw -fmacro-prefix-map=$work=/src/llvm-mingw"
(cd "$work/source" && ARCHS='i686 x86_64' CORES="$jobs" CFLAGS="$flags" CXXFLAGS="$flags" \
 sh ./build-libcxx.sh --disable-shared "$toolchain")
python3 - "$work" "$toolchain" "$MINGW_COMMIT" "$LLVM_COMMIT" "$0" <<'PY'
import hashlib, json, sys
from pathlib import Path
work, compiler = map(Path, sys.argv[1:3])
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
archives = {}
for arch in ('i686', 'x86_64'):
 for name in ('libc++.a', 'libc++abi.a', 'libunwind.a'):
  path = compiler / (arch + '-w64-mingw32/lib') / name
  data = path.read_bytes()
  if b'/home/' in data or b'/Users/' in data:
   raise SystemExit('compiler runtime retains host path: ' + str(path))
  archives[str(path.relative_to(compiler))] = sha(path)
manifest = {'llvm_mingw_commit': sys.argv[3], 'llvm_commit': sys.argv[4],
            'build_script_sha256': sha(Path(sys.argv[5])), 'archives': archives,
            'diagnostic_source_prefix': '/src/llvm-mingw'}
(work / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
PY
