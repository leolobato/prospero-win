#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Rebuild only Wine Mono 11.3.0's native WPF DLLs, preserving its source and
# default optimization while omitting host-local PDBs. Use a private compiler
# whose static C++ runtimes were rebuilt by build_mingw_runtimes.sh.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
MONO_COMMIT=17dd911a3ea04f21d7e4a8dcbff580ad520607eb
WPF_COMMIT=9178bf7cb4d5d33dfd36f0611153c9f95a337595
COMPILER_SHA256=bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21
source= work= toolchain= jobs=1
fail() { echo "build_wpf_ps5: $*" >&2; exit 2; }
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
git -C "$source" cat-file -e "$MONO_COMMIT^{commit}" 2>/dev/null || fail 'missing pinned Wine Mono source'
git -C "$source/wpf" cat-file -e "$WPF_COMMIT^{commit}" 2>/dev/null || fail 'missing pinned WPF source'
[ "$(cat "$toolchain/.archive-sha256" 2>/dev/null)" = "$COMPILER_SHA256" ] || fail 'compiler archive pin differs'
mkdir -p "$work"
work=$(realpath "$work"); toolchain=$(realpath "$toolchain")
if [ ! -d "$work/source" ]; then
 staging=$(mktemp -d "$work/source.XXXXXX")
 git -C "$source/wpf" archive "$WPF_COMMIT" | tar -x -C "$staging"
 mv "$staging" "$work/source"
fi
python3 "$root/tools/mesa_zink_manifest.py" --verify-source "$work/source" "$source/wpf" "$WPF_COMMIT"
flags="-ffile-prefix-map=$work=/src/wpf -fmacro-prefix-map=$work=/src/wpf"
for arch in x86 x86_64; do
 triplet=i686; [ "$arch" = x86 ] || triplet=x86_64
 make -j"$jobs" -C "$work/source/src/Microsoft.DotNet.Wpf/src/WpfGfx" \
  OBJDIR="$work/build-$arch" ARCH="$arch" MINGW="$toolchain/bin/$triplet-w64-mingw32" \
  CFLAGS="$flags" CXXFLAGS="$flags" LDFLAGS=-Wl,--strip-debug
 mkdir -p "$work/artifacts/lib/$arch"
 # Omit dependency CRT debug sections while linking; never postprocess DLLs.
 cp "$work/build-$arch/wpfgfx_cor3.dll" "$work/artifacts/lib/$arch/"
done
mkdir -p "$work/artifacts/LICENSES/wpf" "$work/artifacts/LICENSES/llvm-mingw"
cp "$work/source/LICENSE.TXT" "$work/artifacts/LICENSES/wpf/"
cp "$work/source/wine/LICENSE" "$work/artifacts/LICENSES/wpf/Wine-LICENSE"
cp "$toolchain/LICENSE.TXT" "$work/artifacts/LICENSES/llvm-mingw/"
cp -R "$toolchain/i686-w64-mingw32/share/mingw32" "$work/artifacts/LICENSES/llvm-mingw/"
python3 - "$work" "$toolchain" "$root/tools" "$MONO_COMMIT" "$WPF_COMMIT" "$COMPILER_SHA256" "$0" <<'PY'
import hashlib, json, sys
from pathlib import Path
work, compiler = map(Path, sys.argv[1:3])
sys.path.insert(0, sys.argv[3])
from mesa_zink_manifest import pe_machine, imports
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
files, archives = {}, {}
for arch, triplet, machine in (('x86', 'i686', 0x14c), ('x86_64', 'x86_64', 0x8664)):
 path = work / 'artifacts/lib' / arch / 'wpfgfx_cor3.dll'
 assert pe_machine(path) == machine, path
 data = path.read_bytes()
 if any(term in data for term in (b'/home/', b'/Users/', b'/builds/')):
  raise SystemExit('WPF retains host paths; rebuild the static compiler runtimes: ' + str(path))
 files[str(path.relative_to(work / 'artifacts'))] = {'sha256': sha(path), 'machine': machine,
                                                   'imports': imports(path, compiler / 'bin/llvm-readobj')}
 for name in ('libc++.a', 'libc++abi.a', 'libunwind.a'):
  path = compiler / (triplet + '-w64-mingw32/lib') / name
  archives[str(path.relative_to(compiler))] = sha(path)
record = {'wine_mono_commit': sys.argv[4], 'wpf_commit': sys.argv[5],
          'compiler_archive_sha256': sys.argv[6], 'compiler_runtimes': archives,
          'build_script_sha256': sha(Path(sys.argv[7])), 'files': files,
          'upstream_default_compiler': '20260616-ucrt', 'rebuild_compiler': '20260922-ucrt',
          'optimization': 'upstream default', 'pdb': False, 'console_validated': False,
          'license_sha256': {str(p.relative_to(work / 'artifacts')): sha(p)
                            for p in sorted((work / 'artifacts/LICENSES').rglob('*')) if p.is_file()}}
(work / 'artifacts/manifest.json').write_text(json.dumps(record, indent=2) + '\n')
PY
echo "$work/artifacts"
