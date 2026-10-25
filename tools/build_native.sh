#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build the prospero-win PS5 title: the launcher and Wine's own Unix side
# (ntdll.prx) started in-process (native/wine64_main.c). The Wine runtime
# itself comes from tools/build_wine_ps5.sh and is staged beside the title.
#
# Environment:
#   PS5_NATIVE_FOUNDATION  boilerplate checkout (default .deps/, pinned)
#   PS5LOG_DEV_CONF        private dev.conf copied into the title
#   PW_FOUNDATION_READY    1 trusts an already prepared foundation checkout
#                          and verifies its artifacts instead of rebuilding
#                          its dependencies, which would mutate a tree the
#                          laboratory's other projects share (default 0)
#   PW_NATIVE_MODE         wine64, the only mode (accepted for older scripts)
#   PW_TITLE_ID            four uppercase letters and five digits
#                          (default PPSA99995); the shell asks the kernel
#                          for its ID at startup and uses this as fallback
#   PW_ANY_TITLE           1 builds one shell for every title a launcher
#                          makes on the console: PW_TITLE_ID is then the
#                          template ID in sce_sys/param.json, and the helper
#                          must be a title-rule helper whose range holds it
#   PW_LAPY_HELPER_ELF     local title-specific Lapy helper
#   PW_LAPY_HELPER_MANIFEST matching manifest; required with the helper and
#                          required for every non-default title
#   PW_OUTPUT_SUFFIX       isolated build/dist suffix, e.g. -wine-smoke
#   PW_WINE64_SCRIPT       1 drives the launcher unattended (validation)
#   PW_WINE64_SECONDS      close each game after this many seconds; 0 never
#   PW_WINE64_SCRIPT_CYCLES games the unattended launcher opens (default 2)
#   PW_WINE64_WAIT_WATCHDOG 1 turns on Wine's wait watchdog (patch 0680) in
#                          every game: a snapshot of the server's waits every
#                          two seconds, for diagnosing stalls (default 0)
#   PW_TITLE_PROBE         1 builds a probe that records how the title can
#                          learn its own ID into /app0/title-probe.txt and
#                          exits (step-0 console probe, default 0)
#   Lapy helper             PPSA99995 fetches the GitHub release pinned below
#                          (lapy_release, lapy_elf_sha256); per-game titles
#                          take local pinned inputs (PW_LAPY_HELPER_ELF and
#                          PW_LAPY_HELPER_MANIFEST)
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
pin=9c0b994a048521af6fb84c73ded364504fe250e9
url=https://github.com/mpereiraesaa/ps5-native-app-boilerplate.git
# The one-shot /data helper bundled with the title: a published release of
# mpereiraesaa/PS5-Lapy-JB-Daemon and the SHA-256 of its lapy.elf. Bump both
# together, after the new helper has run on a console.
lapy_release=v0.3.2-experimental
lapy_elf_sha256=43fab6d8045b525403f025a7b15e313aeaeac174bba6bff19a32b0000293c647
foundation=${PS5_NATIVE_FOUNDATION:-$root/.deps/ps5-native-app-boilerplate}
dev_conf=${PS5LOG_DEV_CONF:-$root/dev.conf}
native_mode=${PW_NATIVE_MODE:-wine64}
output_suffix=${PW_OUTPUT_SUFFIX:-}
wine64_script=${PW_WINE64_SCRIPT:-0}
wine64_seconds=${PW_WINE64_SECONDS:-0}
wine64_cycles=${PW_WINE64_SCRIPT_CYCLES:-2}
wine64_watchdog=${PW_WINE64_WAIT_WATCHDOG:-0}
title_probe=${PW_TITLE_PROBE:-0}
any_title=${PW_ANY_TITLE:-0}
title_id=${PW_TITLE_ID:-PPSA99995}
lapy_helper_elf=${PW_LAPY_HELPER_ELF:-}
lapy_helper_manifest=${PW_LAPY_HELPER_MANIFEST:-}
managed_descriptor_required=0
[[ $title_id == PPSA99995 && ${PW_ANY_TITLE:-0} == 0 ]] || managed_descriptor_required=1

# Verify the independently reviewed inventory before compiling the provider.
publication_provider_sha256=$(python3 - "$root" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
pin = "f14d5e3ecb31951e3cf5bfdc41972181a9be1d96ed0dc7cb3aeebb8e4aab844a"
raw = (root / "third_party/mbedtls-publication/provenance.json").read_bytes()
if hashlib.sha256(raw).hexdigest() != pin:
    raise SystemExit("publication provider provenance differs from reviewed pin")
for entry in json.loads(raw)["files"]:
    path = root / entry["path"]
    if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
        raise SystemExit("publication provider inventory mismatch: " + entry["path"])
print(pin)
PY
)

[[ $native_mode == wine64 ]] || {
    echo "PW_NATIVE_MODE must be wine64: the direct Win32 runtime was removed" >&2; exit 2; }
[[ $any_title == 0 || $any_title == 1 ]] || {
    echo "PW_ANY_TITLE must be 0 or 1" >&2; exit 2; }
[[ $title_probe == 0 || $title_probe == 1 ]] || {
    echo "PW_TITLE_PROBE must be 0 or 1" >&2; exit 2; }
[[ $wine64_watchdog == 0 || $wine64_watchdog == 1 ]] || {
    echo "PW_WINE64_WAIT_WATCHDOG must be 0 or 1" >&2; exit 2; }
[[ $wine64_script == 0 || $wine64_script == 1 ]] && [[ $wine64_seconds =~ ^[0-9]+$ ]] &&
    [[ $wine64_cycles =~ ^[1-9][0-9]{0,3}$ ]] || {
    echo "PW_WINE64_SCRIPT must be 0 or 1, PW_WINE64_SECONDS a number and" \
         "PW_WINE64_SCRIPT_CYCLES 1-9999" >&2; exit 2; }
[[ $output_suffix =~ ^[A-Za-z0-9_-]*$ ]] || {
    echo "PW_OUTPUT_SUFFIX must contain only letters, digits, '_' or '-'" >&2
    exit 2
}
[[ $title_id =~ ^[A-Z]{4}[0-9]{5}$ ]] || {
    echo "PW_TITLE_ID must be four uppercase letters followed by five digits" >&2
    exit 2
}
[[ -z $lapy_helper_elf && -z $lapy_helper_manifest ]] ||
    [[ -n $lapy_helper_elf && -n $lapy_helper_manifest ]] || {
        echo "PW_LAPY_HELPER_ELF and PW_LAPY_HELPER_MANIFEST must be set together" >&2
        exit 2
    }
if [[ ($title_id != PPSA99995 || $any_title == 1) && -z $lapy_helper_elf ]]; then
    echo "per-game PW_TITLE_ID=$title_id requires PW_LAPY_HELPER_ELF and PW_LAPY_HELPER_MANIFEST" >&2
    exit 2
fi

helper_download=
if [[ -z $lapy_helper_elf ]]; then
    helper_download=$(mktemp -d "${TMPDIR:-/tmp}/prospero-lapy-helper.XXXXXX")
    trap 'rm -rf -- "$helper_download"' EXIT
    python3 "$root/tools/fetch_lapy_helper.py" \
        --repo mpereiraesaa/PS5-Lapy-JB-Daemon --tag "$lapy_release" \
        --sha256 "$lapy_elf_sha256" --out "$helper_download"
    lapy_helper_elf="$helper_download/lapy.elf"
    lapy_helper_manifest="$helper_download/lapy-manifest.json"
else
    [[ -f $lapy_helper_elf ]] || {
        echo "PW_LAPY_HELPER_ELF is not a file: $lapy_helper_elf" >&2; exit 2; }
    [[ -f $lapy_helper_manifest ]] || {
        echo "PW_LAPY_HELPER_MANIFEST is not a file: $lapy_helper_manifest" >&2; exit 2; }
fi
helper_magic=$(od -An -tx1 -N4 "$lapy_helper_elf" | tr -d ' \n')
[[ $helper_magic == 7f454c46 ]] || {
    echo "Lapy helper is not an ELF file" >&2
    exit 2
}
if ! python3 - "$root" "$lapy_helper_elf" "$lapy_helper_manifest" \
    "$title_id" "$root/native/lapy_elevation_protocol.h" "$any_title" <<'PY'
import json, pathlib, sys
sys.path.insert(0, sys.argv[1])
from tools.fetch_lapy_helper import validate_helper_manifest
elf = pathlib.Path(sys.argv[2])
manifest = json.loads(pathlib.Path(sys.argv[3]).read_text())
protocol = pathlib.Path(sys.argv[5])
try:
    validate_helper_manifest(manifest, elf, protocol, sys.argv[4], sys.argv[6] == "1")
except ValueError as error:
    raise SystemExit(str(error))
PY
then
    exit 2
fi

if [[ ! -e $foundation/.git ]]; then
    mkdir -p -- "$(dirname -- "$foundation")"
    git clone --filter=blob:none "$url" "$foundation"
fi
actual=$(git -C "$foundation" rev-parse HEAD)
if [[ $actual != "$pin" ]]; then
    git -C "$foundation" fetch origin "$pin"
    git -C "$foundation" checkout --detach "$pin"
fi
[[ $(git -C "$foundation" rev-parse HEAD) == "$pin" ]] || {
    echo "native foundation pin verification failed" >&2; exit 2; }

sdk="$foundation/.deps/native/ps5-payload-sdk"
native="$foundation/tooling/native"
tool="$foundation/build/host/ps5-native-tool-$pin"
# Keep the low PE32 address range free for fixed-base games such as OpenArena.
layout="$native/ps5-pie-high.ld"

# The pinned foundation is often a checkout shared with the laboratory's
# other projects. Rebuilding its dependencies mutates that tree and reaches
# the network, so when it is already complete, verify it instead.
if [[ ${PW_FOUNDATION_READY:-0} == 1 ]]; then
    for artifact in "$sdk/bin/prospero-lld" "$sdk/target/lib/libkernel.so" \
                    "$foundation/runtime/libc.prx" \
                    "$layout" "$native/app_crt.cpp"; do
        [[ -e $artifact ]] || {
            echo "PW_FOUNDATION_READY=1 but $artifact is missing" >&2
            exit 2
        }
    done
    echo "using the prepared foundation at $foundation (deps not rebuilt)"
else
    make -C "$foundation" deps libc >/dev/null
fi
if [[ ! -x $tool ]]; then
    zlib_root="$foundation/.deps/native/zlib/root"
    zlib_archive=$(find "$zlib_root" -type f -name libz.a -print -quit)
    cxx=$(command -v clang++-18 || command -v clang++ || true)
    [[ -n $cxx && -n $zlib_archive ]] || {
        echo "native foundation host-tool dependencies are unavailable" >&2
        exit 2
    }
    mkdir -p "$foundation/build/host"
    "$cxx" -std=c++20 -O2 -Wall -Wextra -Werror \
        -I "$zlib_root/usr/include" \
        "$native/native_app_builder.cpp" "$native/self_container.cpp" \
        "$native/elf_object.cpp" "$native/sce_module_writer.cpp" \
        "$zlib_archive" -o "$tool"
fi
[[ -x $tool && -d $sdk && -f $foundation/runtime/libc.prx ]] || {
    echo "native foundation did not produce its SDK, tool and runtime" >&2
    exit 2
}

build="$root/build/native$output_suffix"
dist="$root/dist/$title_id$output_suffix"
rm -rf -- "$build" "$dist"
mkdir -p "$build/obj" "$build/import-stubs" "$dist/sce_sys" "$dist/sce_module"
if [[ -n $helper_download ]]; then
    cp "$helper_download/release.json" "$build/lapy-helper-release.json"
else
    printf '{"source":"local"}\n' > "$build/lapy-helper-release.json"
fi
cp "$lapy_helper_manifest" "$build/lapy-helper-manifest.json"

cc=(env PS5_PAYLOAD_SDK="$sdk" sh "$foundation/tooling/prospero-clang18")
# A source archive has no Git metadata. Keep revision and dirtiness separate
# in the build manifest so release-kit validation can reject either explicitly.
if source_revision=$(git -C "$root" rev-parse HEAD 2>/dev/null); then
    source_dirty=false
    if [[ -n $(git -C "$root" status --porcelain --untracked-files=normal) ]]; then
        source_dirty=true
    fi
else
    source_revision=unknown
    source_dirty=false
fi
build_id=$source_revision
[[ $source_dirty == false ]] || build_id="$build_id-dirty"
common=(-DPW_BUILD_ID=\""$build_id"\" -DPW_TITLE_ID=\""$title_id"\"
        -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
        -I"$root/include" -I"$root/src" -I"$root/native"
        -I"$root/native/ps5log"
        -I"$root/third_party/mbedtls-publication/include"
        -I"$root/third_party/mbedtls-publication"
        -DMBEDTLS_CONFIG_FILE=\"pw_publication_config.h\"
        -DPW_MANAGED_DESCRIPTOR_REQUIRED="$managed_descriptor_required"
        -DPW_WINE64_SCRIPT="$wine64_script" -DPW_WINE64_SECONDS="$wine64_seconds"
        -DPW_WINE64_SCRIPT_CYCLES="$wine64_cycles" -DPW_WINE64_WAIT_WATCHDOG="$wine64_watchdog"
        -DPW_TITLE_PROBE="$title_probe")

sources=(
    native/wine64_main.c native/pw_diagnostics.c native/pw_audio_ps5.c native/pw_pad_ps5.c native/pw_agc_ps5.c
    native/pw_agc_submit_lifecycle.c native/pw_videoout_ps5.c native/pw_data_mount.c
    native/pw_lapy_elevation.c
    native/pw_wine_display.c native/pw_wine_library.c native/pw_hid_ps5.c native/pw_wine_prefix.c
    src/pw_result.c src/pw_wine_start.c src/pw_wine_launch.c src/pw_game_profile.c src/pw_prefix_temp.c
    src/pw_title_launch.c src/pw_title_id.c src/pw_title_session.c
    native/pw_title_publication_native.c native/pw_title_publication_ps5_lock.c
    src/pw_title_publication.c src/pw_publication_sha256.c
    third_party/mbedtls-publication/library/sha256.c
    src/pw_app_profile.c src/pw_profile_catalog.c src/pw_launcher_render.c src/pw_present.c
    src/pw_pad.c src/pw_game_menu.c src/pw_hid.c src/pw_spinner.c src/pw_script_input.c src/pw_tsc_calibrate.c
    wine/ps5/pw_wine_prx.c
)
objects=()
for source in "${sources[@]}"; do
    object="$build/obj/${source//\//_}.o"
    "${cc[@]}" -std=c11 "${common[@]}" -c "$root/$source" -o "$object"
    objects+=("$object")
done
"${cc[@]}" -std=c11 "${common[@]}" \
    -include "$root/native/ps5log/ps5log_ps5_net.h" \
    -c "$root/native/ps5log/ps5log.c" -o "$build/obj/ps5log.o"
"${cc[@]}" -std=c11 "${common[@]}" \
    -c "$root/native/ps5log/ps5log_ps5_net.c" \
    -o "$build/obj/ps5log_ps5_net.o"
"${cc[@]}" -std=c++20 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -c "$native/app_crt.cpp" \
    -o "$build/obj/app_crt.o"
objects+=("$build/obj/ps5log.o" "$build/obj/ps5log_ps5_net.o")

# VideoOut flips through AGC; these link-only facades name its imports.
"${cc[@]}" -std=c11 -O2 -fPIC -c "$root/native/stubs/libSceAgc.c" \
    -o "$build/obj/agc-import.o"
"$sdk/bin/prospero-lld" --shared -soname libSceAgc.prx \
    -o "$build/import-stubs/libSceAgc.so" "$build/obj/agc-import.o"
"${cc[@]}" -std=c11 -O2 -fPIC -c "$root/native/stubs/libSceAgcDriver.c" \
    -o "$build/obj/agc-driver-import.o"
"$sdk/bin/prospero-lld" --shared -soname libSceAgcDriver.prx \
    -o "$build/import-stubs/libSceAgcDriver.so" "$build/obj/agc-driver-import.o"

# Payload kernel stubs describe different import libraries. Including all of
# them lets the weak stub select libkernel_web, which a native title does not
# load: clock_gettime then calls NULL before startup logging on FW 13.60.
# Keep the native provider first and omit the payload/weak variants.
native_stubs=("$sdk/target/lib/libkernel.so")
for stub in "$sdk"/target/lib/*.so; do
    case "${stub##*/}" in
        libkernel*.so) continue ;;
    esac
    native_stubs+=("$stub")
done
"$sdk/bin/prospero-lld" -T "$layout" --eh-frame-hdr -e _start \
    -o "$build/llvm-pie.elf" "$build/obj/app_crt.o" "${objects[@]}" \
    --as-needed "${native_stubs[@]}" "$build/import-stubs/libSceAgc.so" \
    "$build/import-stubs/libSceAgcDriver.so"
"$tool" link --in "$build/llvm-pie.elf" --out "$build/eboot.elf" \
    --stub-dir "$sdk/target/lib" --module-sdk 0x02000009 \
    --stub "$build/import-stubs/libSceAgc.so" \
    --stub "$build/import-stubs/libSceAgcDriver.so" \
    --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$build/eboot.elf" --out "$dist/eboot.bin" \
    --magic 0x1D3D154F
python3 - "$dist/eboot.bin" "$dist/native-title-build.json" "$title_id" \
    "$source_revision" "$source_dirty" "$managed_descriptor_required" \
    "$publication_provider_sha256" "$any_title" "$lapy_helper_manifest" <<'PY'
import hashlib, json, pathlib, sys
eboot, destination = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
manifest = {
    "schema": "prospero-win-native-title/1",
    "title_id": sys.argv[3],
    "source_revision": sys.argv[4],
    "source_dirty": sys.argv[5] == "true",
    "eboot_sha256": hashlib.sha256(eboot.read_bytes()).hexdigest(),
}
if sys.argv[6] == "1":
    manifest.update({
        "managed_publication_guard": "playden-publication-guard-v1",
        "managed_descriptor_required": True,
        "publication_provider_sha256": sys.argv[7],
    })
if sys.argv[8] == "1":
    # One shell for every title in the helper's range; the title ID is read
    # at startup and title_id is only the template's.
    manifest["any_title"] = True
    manifest["title_rule"] = json.loads(pathlib.Path(sys.argv[9]).read_text())["title_rule"]
destination.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
PY
# The console installer copies icon0.png into /user/app/<title> and aborts
# the whole registration if it is absent, so it is not optional.
[[ -f $root/sce_sys/icon0.png ]] || python3 "$root/tools/make_icon.py"
python3 - "$root/sce_sys/param.json" "$dist/sce_sys/param.json" "$title_id" <<'PY'
import json, pathlib, sys
source, destination, title_id = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
param = json.loads(source.read_text())
param["titleId"] = title_id
param["conceptId"] = title_id[-5:]
param["contentId"] = f"UP9000-{title_id}_00-PROSPEROWIN00001"
destination.write_text(json.dumps(param, indent=2) + "\n")
PY
cp "$root/sce_sys/icon0.png" "$dist/sce_sys/"
cp "$foundation/runtime/libc.prx" "$dist/sce_module/libc.prx"
cp "$lapy_helper_elf" "$dist/lapy.elf"
cp "$lapy_helper_manifest" "$dist/lapy-manifest.json"
mkdir -p "$dist/licenses/mbedtls-publication"
cp "$root/third_party/mbedtls-publication/LICENSE" "$dist/licenses/mbedtls-publication/"
cp "$root/third_party/mbedtls-publication/provenance.json" "$dist/licenses/mbedtls-publication/"
# The console refuses to start a title whose eboot lacks execute permission
# (exec fails with EACCES) and its loader refuses a PRX without it ("mount
# flag / attribute error"), so mark both here. An FTP upload may still reset
# the modes; docs/GETTING_STARTED.md says what to do then.
chmod 755 "$dist/eboot.bin" "$dist/sce_module/libc.prx"
if [[ -f $dev_conf ]]; then
    cp "$dev_conf" "$dist/dev.conf"
fi

# Every dynamic import in the linked ELF is reviewed, per the porting
# playbook: an exported platform symbol is not a working one.
readelf=${LLVM_READELF:-$(command -v llvm-readelf-18 || command -v llvm-readelf || true)}
if [[ -n $readelf && -x $readelf ]]; then
    "$readelf" --dyn-syms "$build/llvm-pie.elf" \
        | awk '$7 == "UND" { print $8 }' | sort -u \
        > "$build/PW_DYNAMIC_IMPORTS.txt"
    if grep -qx 'strcasestr' "$build/PW_DYNAMIC_IMPORTS.txt"; then
        echo "strcasestr is banned: its provider is unusable on FW 12.02" >&2
        exit 2
    fi
    echo "dynamic imports recorded in $build/PW_DYNAMIC_IMPORTS.txt"
else
    echo "llvm-readelf unavailable: dynamic-import review skipped" >&2
fi

sha256sum "$build/eboot.elf" "$dist/eboot.bin"
