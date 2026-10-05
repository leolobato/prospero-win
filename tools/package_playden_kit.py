#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build and validate an offline, immutable Playden prospero-win kit.

The packager only copies already-built inputs.  It never downloads or builds
Wine, DXVK, the native title, or its helper.  Its JSON spec names the finite
set of title/helper variants that Playden may allocate at install time.
Each title's helper bytes must also have a reviewed hash in
``REQUIRED_HELPER_SHA256``; extending title coverage requires adding that pin.

Usage:
    package_playden_kit.py package --spec kit-spec.json --out KIT
    package_playden_kit.py validate KIT
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import sys
import tempfile
from typing import Any

try:
    from tools.pw_registry import windows_user_bound
    from tools.fetch_lapy_helper import validate_helper_manifest
    from tools.pw_install import InstallError, pe_architecture
    from tools.pw_prefix import (CONSOLE_USER_DIRS, LINK_TABLE, REGISTRY,
                                 local_tree, to_console)
    from tools.validate_wine_runtime import (PE_MACHINE_I386, PE_MAGIC_PE32,
                                             pe_identity)
except ModuleNotFoundError:  # Direct execution from tools/.
    from pw_registry import windows_user_bound
    from fetch_lapy_helper import validate_helper_manifest
    from pw_install import InstallError, pe_architecture
    from pw_prefix import CONSOLE_USER_DIRS, LINK_TABLE, REGISTRY, local_tree, to_console
    from validate_wine_runtime import PE_MACHINE_I386, PE_MAGIC_PE32, pe_identity


SPEC_SCHEMA = "playden-prospero-kit-spec/1"
MANIFEST_SCHEMA = "playden-prospero-kit/1"
TITLE_RE = re.compile(r"FAKE78\d{3}\Z")
ID_RE = re.compile(r"[a-z0-9][a-z0-9._-]{0,95}\Z")
REVISION_RE = re.compile(r"[0-9a-f]{40}\Z")
DXVK_VERSION = "2.6.2"
DXVK_DLLS = ("d3d8.dll", "d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll")
CORE_DLLS = ("ntdll.dll", "kernel32.dll", "kernelbase.dll", "mmdevapi.dll",
             "dinput8.dll", "rsaenh.dll")
# Wineboot also materializes these legacy *.dll16 aliases as NE placeholders.
# Preserve them without advertising 16-bit execution; core modules remain PE32.
WINE_NE_PLACEHOLDERS = frozenset("""
avifile.dll commdlg.dll compobj.dll ctl3d.dll ctl3dv2.dll dispdib.dll imm.dll
lzexpand.dll msacm.dll msvideo.dll ole2.dll ole2conv.dll ole2disp.dll ole2nls.dll
ole2prox.dll ole2thk.dll olecli.dll olesvr.dll rasapi16.dll setupx.dll shell.dll
storage.dll stress.dll toolhelp.dll typelib.dll ver.dll w32sys.dll win32s16.dll
win87em.dll winaspi.dll windebug.dll wing.dll winnls.dll winsock.dll wintab.dll
""".split())
NATIVE_PRXS = ("ntdll.prx", "wineserver.prx", "win32u.prx", "wineps5.prx",
               "libvulkan.prx", "wowprospero.prx")
PE_MACHINE_AMD64 = 0x8664
PE_MAGIC_PE32_PLUS = 0x020B
MMDEVICE_CLSID = "{BCDE0395-E52F-467C-8E3D-C4579291692E}"
DIRECTINPUT_CLSID = "{25E609E4-B259-11CF-BFC7-444553540000}"
PERSONAL_NAMES = {
    "loginusers.vdf", "config.vdf", "localconfig.vdf", "cookies", "userdata",
    "steamapps", "ssfn", "save", "saves", "dev.conf",
    "credentials.json", "token.json",
}
TEXT_PAYLOAD_SUFFIXES = {".cfg", ".conf", ".ini", ".json", ".reg", ".txt"}
REQUIRED_SOURCES = ("wrapper", "wine", "helper", "foundation", "sdk", "dxvk")
REQUIRED_PROVENANCE: dict[str, dict[str, str]] = {
    "wrapper": {"baseline_revision": "54caac176e86c60c7c407972583b491f67838ed9"},
    "wine": {"revision": "e16ec8f3a19dec4a1162e7554ab73ce41f83bdb9"},
    "helper": {
        "revision": "e9f2cfc2c3913e42fdf83d74291e270b041ff0d9",
        "base_revision": "611fa5f7ed46cdde7094b764168390ab5c4e89f8",
    },
    "foundation": {"revision": "9c0b994a048521af6fb84c73ded364504fe250e9"},
    "sdk": {"version": "0.42"},
    "dxvk": {
        "version": DXVK_VERSION,
        "archive_sha256": "17761876556afd55736cb895d184f5a1c55d43350f1b1e3b129f8d28706d7992",
    },
}
# Extend this source-owned allowlist only after reviewing the exact per-title
# helper artifact.  Helper manifests cannot add or override trust anchors.
REQUIRED_HELPER_SHA256: dict[str, str] = {
    "FAKE78001": "5608915201204dfcd1846b01dd2a9e60d03aa390c69b1dc61f3b150566bf1935",
}


class KitError(Exception):
    """The proposed kit is incomplete, unsafe, or does not match its identity."""


def _sha256(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            result.update(chunk)
    return result.hexdigest()


def _read_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise KitError(f"cannot read {label} {path}: {error}") from error
    if not isinstance(value, dict):
        raise KitError(f"{label} must contain a JSON object")
    return value


def _require_id(value: object, label: str) -> str:
    if not isinstance(value, str) or not ID_RE.fullmatch(value):
        raise KitError(f"{label} is not a safe versioned identifier: {value!r}")
    return value


def _source_path(spec_path: Path, value: object, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise KitError(f"{label} path is missing")
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = spec_path.parent / candidate
    try:
        return candidate.resolve(strict=True)
    except OSError as error:
        raise KitError(f"{label} path is missing: {candidate}") from error


def _safe_relative(value: object) -> str:
    if not isinstance(value, str) or not value or "\\" in value:
        raise KitError(f"unsafe manifest path: {value!r}")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise KitError(f"unsafe manifest path: {value!r}")
    return value


def _check_no_escaping_symlinks(root: Path, label: str,
                                allow_console_root: bool = False) -> None:
    resolved_root = root.resolve()
    for path in root.rglob("*"):
        if not path.is_symlink():
            continue
        target = os.readlink(path)
        if (allow_console_root and path.parent == root / "dosdevices" and
                path.name == "z:" and target == "/"):
            continue
        if Path(target).is_absolute():
            raise KitError(f"{label} symbolic link escapes its root: {path} -> {target}")
        resolved = (path.parent / target).resolve()
        if resolved != resolved_root and resolved_root not in resolved.parents:
            raise KitError(f"{label} symbolic link escapes its root: {path} -> {target}")


def _copy_plain_tree(source: Path, destination: Path, label: str) -> None:
    _check_no_escaping_symlinks(source, label)
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        target = destination / relative
        if path.is_symlink():
            raise KitError(f"{label} contains a symbolic link: {relative}")
        if path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif path.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            target.chmod(stat.S_IMODE(path.stat().st_mode))
        else:
            raise KitError(f"{label} contains a non-file entry: {relative}")


def _copy_console_prefix(source: Path, destination: Path) -> None:
    _check_no_escaping_symlinks(source, "template", allow_console_root=True)
    files, directories, links = local_tree(source)
    for relative in sorted(directories):
        (destination / relative).mkdir(parents=True, exist_ok=True)
    for relative, path in sorted(files.items()):
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if relative in REGISTRY:
            target.write_bytes(to_console(relative, path.read_bytes()))
        else:
            shutil.copyfile(path, target)
        target.chmod(stat.S_IMODE(path.stat().st_mode))
    for relative, entries in sorted(links.items()):
        table = destination / relative / LINK_TABLE
        table.parent.mkdir(parents=True, exist_ok=True)
        table.write_text("".join(f"{name}\t{target}\n"
                                 for name, target in sorted(entries.items())),
                         encoding="utf-8")
        table.chmod(0o644)


def _section(registry: str, name: str) -> str:
    # Wine loads a bracketed key even when its following timestamp is malformed.
    # Count all effective headers before checking the sole header's syntax.
    matches = list(re.finditer(rf"^\[{re.escape(name)}\]", registry,
                               re.MULTILINE | re.IGNORECASE))
    if not matches:
        return ""
    if len(matches) != 1:
        raise KitError(f"duplicate registry section: {name}")
    header_end = registry.find("\n", matches[0].end())
    suffix = registry[matches[0].end():] if header_end < 0 else registry[matches[0].end():header_end]
    if not re.fullmatch(r"(?: [0-9]+)?\r?", suffix):
        raise KitError(f"malformed registry section: {name}")
    start = matches[0].start()
    end = registry.find("\n[", matches[0].end())
    return registry[start:] if end < 0 else registry[start:end]


def _registry_string(section: str, name: str | None = None) -> str | None:
    key = "@" if name is None else f'"{name}"'
    prefix = f'{key}="'
    assignments = [line for line in section.splitlines()
                   if line.lower().startswith(f"{key}=".lower())]
    if len(assignments) != 1:
        return None
    assignment = assignments[0]
    if not assignment.lower().startswith(prefix.lower()) or not assignment.endswith('"'):
        return None
    return assignment[len(prefix):-1]


def _registry_dword(section: str, name: str) -> int | None:
    key = f'"{name}"'
    assignments = [line for line in section.splitlines()
                   if line.lower().startswith(f"{key}=".lower())]
    if len(assignments) != 1:
        return None
    match = re.fullmatch(
        rf'{re.escape(key)}=dword:([0-9a-fA-F]{{8}})', assignments[0], re.IGNORECASE)
    return int(match.group(1), 16) if match else None


def _validate_crypto_view(registry: str, prefix: str, label: str) -> None:
    base = prefix + r"Microsoft\\Cryptography\\Defaults\\"
    provider_type = _section(registry, base + r"Provider Types\\Type 001")
    selected = _registry_string(provider_type, "Name")
    if (not selected or any(c in selected for c in "\\[]\r\n") or
            len(selected) > 128):
        raise KitError(f"{label} cryptography provider Type 001 is not registered")
    provider = _section(
        registry, base + r"Provider\\" + selected)
    if (_registry_string(provider, "Image Path") !=
            r"C:\\windows\\system32\\rsaenh.dll" or
            _registry_dword(provider, "Type") != 1):
        raise KitError(f"{label} cryptography view does not bind the pinned RSA provider")


def _recipe_requires_ps5_audio(recipe: dict[str, Any]) -> bool:
    requirements = recipe.get("requirements")
    prospero = recipe.get("prospero")
    return (isinstance(requirements, dict) and requirements.get("audio") == "ps5") or (
        isinstance(prospero, dict) and prospero.get("audio") == "ps5") or recipe.get("audio") == "ps5"


def _validate_registry(prefix: Path, recipe: dict[str, Any]) -> None:
    system = prefix / "system.reg"
    if not system.is_file():
        raise KitError("template is missing system.reg")
    try:
        registry = to_console("system.reg", system.read_bytes()).decode("utf-8")
        user_registry = (prefix / "user.reg").read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raise KitError("template registry is not UTF-8 text") from error
    except OSError as error:
        raise KitError(f"cannot read template user.reg: {error}") from error

    cpu = _section(registry, r"Software\\Microsoft\\Wow64\\x86")
    if _registry_string(cpu) != "wowprospero.dll":
        raise KitError("32-bit CPU registry view must select wowprospero.dll")

    _validate_crypto_view(registry, r"Software\\", "64-bit")
    _validate_crypto_view(registry, r"Software\\Wow6432Node\\", "32-bit")

    for clsid, display, dll in (
            (MMDEVICE_CLSID, "MMDeviceEnumerator", "mmdevapi.dll"),
            (DIRECTINPUT_CLSID, "DirectInput8", "dinput8.dll")):
        root = rf"Software\\Classes\\Wow6432Node\\CLSID\\{clsid}"
        registration = _section(registry, root)
        server = _section(registry, root + r"\\InprocServer32")
        expected = rf"C:\\windows\\system32\\{dll}"
        if (not registration or _registry_string(server) != expected or
                _registry_string(server, "ThreadingModel") != "Both"):
            raise KitError(f"32-bit {display} COM registration is missing or incomplete")
    if _recipe_requires_ps5_audio(recipe):
        drivers = _section(user_registry, r"Software\\Wine\\Drivers")
        if _registry_string(drivers, "Audio") != "ps5":
            raise KitError("recipe requires the Wine Audio default to be ps5")


def _pe_identity(path: Path, label: str) -> dict[str, int]:
    try:
        return pe_identity(path)
    except SystemExit as error:
        raise KitError(f"{label} is not a valid PE image: {error}") from error


def _wine_ne_placeholder(path: Path) -> bool:
    if path.name not in WINE_NE_PLACEHOLDERS:
        return False
    with path.open("rb") as stream:
        header = stream.read(160)
    marker = b"Wine placeholder DLL\x00"
    return (len(header) == 160 and header[:2] == b"MZ" and
            int.from_bytes(header[0x3c:0x40], "little") == 0x60 and
            header[0x40:0x40 + len(marker)] == marker and
            header[0x60:0x62] == b"NE")


def _validate_pe(path: Path, relative: str, machine: int, magic: int,
                 architecture: str) -> None:
    if not path.is_file():
        raise KitError(f"runtime is missing required module {relative}")
    identity = _pe_identity(path, relative)
    if identity["machine"] != machine or identity["magic"] != magic:
        raise KitError(f"runtime {relative} must be {architecture}")


def _load_runtime_inventory(path: Path, prefix: str = "") -> dict[str, str]:
    raw = _read_json(path, "runtime inventory manifest")
    inventory: dict[str, str] = {}
    for source_name, digest in raw.items():
        if prefix and not source_name.startswith(prefix):
            continue
        relative = source_name[len(prefix):] if prefix else source_name
        relative = _safe_relative(relative)
        if (not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest) or
                relative in inventory):
            raise KitError(f"runtime inventory has an invalid entry: {source_name!r}")
        inventory[relative] = digest
    if not inventory:
        raise KitError("runtime inventory does not contain the selected runtime")
    return inventory


def _runtime_files(runtime: Path) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for path in runtime.rglob("*"):
        if path.is_file() and path.name != "runtime-inventory.json":
            result[path.relative_to(runtime).as_posix()] = path
    return result


def _validate_runtime(runtime: Path, inventory: dict[str, str]) -> dict[str, int]:
    actual = _runtime_files(runtime)
    if set(actual) != set(inventory):
        missing = sorted(set(inventory) - set(actual))
        extra = sorted(set(actual) - set(inventory))
        raise KitError(f"runtime inventory differs from files: missing={missing}, extra={extra}")
    for relative, expected in inventory.items():
        if _sha256(actual[relative]) != expected:
            raise KitError(f"runtime inventory hash mismatch: {relative}")

    libraries = runtime / "lib/wine"
    for folder, machine, magic, architecture in (
            ("i386-windows", PE_MACHINE_I386, PE_MAGIC_PE32, "pe32"),
            ("x86_64-windows", PE_MACHINE_AMD64, PE_MAGIC_PE32_PLUS, "pe64")):
        directory = libraries / folder
        if not directory.is_dir():
            raise KitError(f"runtime is missing full PE module tree {folder}")
        for name in CORE_DLLS:
            _validate_pe(directory / name, f"lib/wine/{folder}/{name}",
                         machine, magic, architecture)
    _validate_pe(libraries / "x86_64-windows/wowprospero.dll",
                 "lib/wine/x86_64-windows/wowprospero.dll",
                 PE_MACHINE_AMD64, PE_MAGIC_PE32_PLUS, "pe64")
    native = libraries / "x86_64-unix"
    for name in NATIVE_PRXS:
        if not (native / name).is_file():
            raise KitError(f"runtime is missing essential native module lib/wine/x86_64-unix/{name}")
    for module in native.glob("*.prx"):
        _required_executable(module, f"runtime native module {module.name}")
    return {
        folder: sum(1 for path in (libraries / folder).iterdir() if path.is_file())
        for folder in ("i386-windows", "x86_64-windows", "x86_64-unix")
    }


def _validate_native_title_build(path: Path, eboot: Path, title_id: str,
                                 wrapper_revision: str) -> None:
    manifest = _read_json(path, "native title build manifest")
    if manifest.get("schema") != "prospero-win-native-title/1":
        raise KitError("native title build manifest has unknown schema")
    if manifest.get("title_id") != title_id:
        raise KitError("native title build manifest title does not match the variant")
    revision = manifest.get("source_revision")
    if not isinstance(revision, str) or not REVISION_RE.fullmatch(revision):
        raise KitError("native title build manifest has invalid source_revision")
    if revision != wrapper_revision:
        raise KitError("native title build manifest does not match wrapper revision")
    if manifest.get("source_dirty") is not False:
        raise KitError("native title build manifest records a dirty source")
    recorded = manifest.get("eboot_sha256")
    if (not isinstance(recorded, str) or not re.fullmatch(r"[0-9a-f]{64}", recorded) or
            not eboot.is_file() or recorded != _sha256(eboot)):
        raise KitError("native title build manifest eboot hash does not match eboot.bin")


def _validate_prefix(prefix: Path, dxvk: dict[str, Any], recipe: dict[str, Any]) -> None:
    for registry in REGISTRY:
        if not (prefix / registry).is_file():
            raise KitError(f"template is missing {registry}")
    user = prefix / "drive_c/users/prospero"
    if not user.is_dir():
        raise KitError("template must contain Windows user prospero")
    for relative in CONSOLE_USER_DIRS:
        if not (user / relative).is_dir():
            raise KitError(f"template prospero user is missing {relative}")
    if not windows_user_bound((prefix / "user.reg").read_text(encoding="utf-8"), "prospero"):
        raise KitError("template registry does not bind the Windows profile to prospero")
    cpu = prefix / "drive_c/windows/system32/wowprospero.dll"
    if not cpu.is_file():
        raise KitError("template is missing drive_c/windows/system32/wowprospero.dll")
    identity = _pe_identity(cpu, "template wowprospero.dll")
    if identity["machine"] != PE_MACHINE_AMD64 or identity["magic"] != PE_MAGIC_PE32_PLUS:
        raise KitError("template wowprospero.dll must be a pe64 image")
    system32 = prefix / "drive_c/windows/system32"
    for name in CORE_DLLS:
        module = system32 / name
        if not module.is_file():
            raise KitError(f"template system32 is missing required 64-bit module {name}")
        try:
            architecture = pe_architecture(module)
        except InstallError as error:
            raise KitError(f"system32 module is invalid: {name}: {error}") from error
        if architecture != "pe64":
            raise KitError(f"system32 module {name} must be pe64, got {architecture}")
    syswow64 = prefix / "drive_c/windows/syswow64"
    if not syswow64.is_dir() or not any(path.is_file() for path in syswow64.iterdir()):
        raise KitError("template has an empty or missing 32-bit syswow64")
    for module in syswow64.glob("*.dll"):
        if _wine_ne_placeholder(module):
            continue
        try:
            architecture = pe_architecture(module)
        except InstallError as error:
            raise KitError(f"syswow64 module is invalid: {module.name}: {error}") from error
        if architecture != "pe32":
            raise KitError(f"syswow64 module {module.name} must be pe32, got {architecture}")
    for name in CORE_DLLS:
        if not (syswow64 / name).is_file():
            raise KitError(f"template syswow64 is missing required 32-bit module {name}")
    _validate_registry(prefix, recipe)

    if not isinstance(dxvk, dict) or dxvk.get("version") != DXVK_VERSION:
        raise KitError(f"DXVK must be pinned to {DXVK_VERSION}")
    hashes = dxvk.get("files")
    if not isinstance(hashes, dict):
        raise KitError("DXVK file hashes are missing")
    for label, folder, architecture in (
            ("x64", "system32", "pe64"), ("x32", "syswow64", "pe32")):
        expected = hashes.get(label)
        if not isinstance(expected, dict) or set(expected) != set(DXVK_DLLS):
            raise KitError(f"DXVK {label} hashes must list all {DXVK_DLLS}")
        for name in DXVK_DLLS:
            path = prefix / "drive_c/windows" / folder / name
            if not path.is_file():
                raise KitError(f"DXVK {DXVK_VERSION} is missing {label}/{name}")
            try:
                actual_architecture = pe_architecture(path)
            except InstallError as error:
                raise KitError(f"DXVK {label}/{name} is invalid: {error}") from error
            if actual_architecture != architecture:
                raise KitError(f"DXVK {label}/{name} must be {architecture}, got {actual_architecture}")
            if not isinstance(expected[name], str) or _sha256(path) != expected[name]:
                raise KitError(f"DXVK {label}/{name} hash does not match the pinned input")

    _validate_no_personal_state(prefix, "template")


def _validate_no_personal_state(root: Path, label: str) -> None:
    for path in root.rglob("*"):
        lowered = path.name.lower()
        if lowered in PERSONAL_NAMES or lowered.startswith("ssfn") or (
                path.is_file() and path.suffix.lower() in (".log", ".dmp", ".sav", ".save")):
            raise KitError(
                f"{label} contains personal or account state: {path.relative_to(root)}")
        relative_parts = [part.lower() for part in path.relative_to(root).parts]
        if "saved games" in relative_parts[:-1]:
            raise KitError(
                f"{label} contains personal or account state: {path.relative_to(root)}")
        for index, part in enumerate(relative_parts[:-1]):
            if part == "users" and relative_parts[index + 1] not in (
                    "prospero", "public", "default", "default user"):
                raise KitError(
                    f"{label} contains personal or account state: {path.relative_to(root)}")
        if path.is_file() and _contains_host_personal_path(path):
            raise KitError(
                f"{label} contains a host personal path in {path.relative_to(root)}")


def _contains_host_personal_path(path: Path) -> bool:
    # Scan text and binaries with bounded memory, including PE UTF-16 strings.
    # Overlap retains path prefixes split across read boundaries.
    pattern = re.compile(
        rb"(?i)(?:/(?:Users|home)/|Z:\\(?:Users|home)\\)[^/\\\s\"']+")
    carry = b""
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            payload = carry + chunk
            for candidate in (payload,
                              payload.decode("utf-16-le", errors="ignore").encode("utf-8"),
                              payload[1:].decode("utf-16-le", errors="ignore").encode("utf-8")):
                normalized = re.sub(rb"\\+", lambda _: b"\\", candidate)
                if pattern.search(normalized):
                    return True
            carry = payload[-512:]
    return False


def _validate_kit_contract(document: dict[str, Any]) -> None:
    firmware = document.get("firmware")
    if (not isinstance(firmware, list) or not firmware or
            not all(isinstance(item, str) and item for item in firmware)):
        raise KitError("kit must list supported firmware")
    if len(set(firmware)) != len(firmware):
        raise KitError("kit must not list duplicate firmware")
    capabilities = document.get("capabilities")
    if (not isinstance(capabilities, list) or
            not all(isinstance(item, str) for item in capabilities)):
        raise KitError("kit capabilities must be a list of strings")


def _validate_helper_pin(helper: Path, title_id: str, variant_id: str) -> None:
    expected = REQUIRED_HELPER_SHA256.get(title_id)
    if expected is None:
        raise KitError(
            f"variant {variant_id} title {title_id} has no reviewed helper hash pin; "
            "add its exact artifact digest to REQUIRED_HELPER_SHA256")
    if not helper.is_file() or _sha256(helper) != expected:
        raise KitError(f"variant {variant_id} does not match its pinned helper digest")


def _required_executable(path: Path, relative: str) -> None:
    if not path.is_file() or stat.S_IMODE(path.stat().st_mode) & 0o111 == 0:
        raise KitError(f"{relative} is missing executable permission")


def _validate_sources(spec: dict[str, Any]) -> dict[str, Any]:
    sources = spec.get("sources")
    if not isinstance(sources, dict) or any(name not in sources for name in REQUIRED_SOURCES):
        raise KitError(f"source provenance must include {', '.join(REQUIRED_SOURCES)}")
    for name in ("wrapper", "wine", "helper", "foundation"):
        entry = sources.get(name)
        if not isinstance(entry, dict) or not REVISION_RE.fullmatch(str(entry.get("revision", ""))):
            raise KitError(f"source provenance for {name} needs a full revision")
        if not isinstance(entry.get("url"), str) or not entry["url"]:
            raise KitError(f"source provenance for {name} needs a URL")
    wrapper = sources["wrapper"]
    helper = sources["helper"]
    if wrapper.get("baseline_revision") != REQUIRED_PROVENANCE["wrapper"]["baseline_revision"]:
        raise KitError("wrapper provenance does not name the tested baseline revision")
    for name in ("wine", "helper", "foundation"):
        if sources[name].get("revision") != REQUIRED_PROVENANCE[name]["revision"]:
            raise KitError(f"source provenance for {name} is not the pinned revision")
    if helper.get("base_revision") != REQUIRED_PROVENANCE["helper"]["base_revision"]:
        raise KitError("helper provenance does not match the exact tested base revision")
    sdk = sources["sdk"]
    dxvk = sources["dxvk"]
    if (not isinstance(sdk, dict) or
            sdk.get("version") != REQUIRED_PROVENANCE["sdk"]["version"] or not sdk.get("url")):
        raise KitError("source provenance must pin SDK 0.42")
    if (not isinstance(dxvk, dict) or
            dxvk.get("version") != REQUIRED_PROVENANCE["dxvk"]["version"] or
            dxvk.get("archive_sha256") != REQUIRED_PROVENANCE["dxvk"]["archive_sha256"] or
            not dxvk.get("url")):
        raise KitError(f"source provenance must pin DXVK {DXVK_VERSION} with archive hash")
    return json.loads(json.dumps(sources))


def _file_entries(root: Path) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    for path in sorted(root.rglob("*")):
        if path.name == "manifest.json" or path.is_dir():
            continue
        if path.is_symlink():
            raise KitError(f"kit payload contains a symbolic link: {path.relative_to(root)}")
        if not path.is_file():
            raise KitError(f"kit payload contains a non-file entry: {path.relative_to(root)}")
        entries.append({
            "path": path.relative_to(root).as_posix(),
            "size": path.stat().st_size,
            "sha256": _sha256(path),
            "mode": f"{stat.S_IMODE(path.stat().st_mode):04o}",
        })
    return entries


def _files_digest(entries: list[dict[str, Any]]) -> str:
    lines = [f"{entry['path']}\t{entry['size']}\t{entry['sha256']}\t{entry['mode']}\n"
             for entry in entries]
    return hashlib.sha256("".join(lines).encode("utf-8")).hexdigest()


def package_kit(spec_path: str | Path, out: str | Path) -> dict[str, Any]:
    """Package validated local inputs into a new immutable kit directory."""
    spec_path = Path(spec_path).resolve()
    spec = _read_json(spec_path, "kit specification")
    if spec.get("schema") != SPEC_SCHEMA:
        raise KitError(f"unknown kit specification schema: {spec.get('schema')!r}")
    kit_id = _require_id(spec.get("kit_id"), "kit_id")
    if spec.get("architecture") != "x86_64+wow64-i386":
        raise KitError("kit architecture must be x86_64+wow64-i386")
    _validate_kit_contract(spec)
    firmware = spec["firmware"]
    capabilities = spec["capabilities"]
    sources = _validate_sources(spec)

    runtime = spec.get("runtime")
    template = spec.get("template")
    recipe = spec.get("recipe")
    variants = spec.get("variants")
    if not all(isinstance(value, dict) for value in (runtime, template, recipe)):
        raise KitError("kit needs runtime, template, and recipe sections")
    if not isinstance(variants, list) or not variants:
        raise KitError("kit needs at least one finite title variant")
    bundle_id = _require_id(runtime.get("bundle_id"), "runtime bundle_id")
    artifact_revision = _require_id(runtime.get("artifact_revision"), "runtime artifact_revision")
    if artifact_revision == sources["wrapper"]["revision"]:
        raise KitError("runtime artifact revision must be distinct from wrapper revision")
    if runtime.get("wine_revision") != sources["wine"]["revision"]:
        raise KitError("runtime Wine revision does not match source provenance")
    runtime_source = _source_path(spec_path, runtime.get("path"), "runtime")
    runtime_inventory_path = _source_path(
        spec_path, runtime.get("inventory_manifest"), "runtime inventory manifest")
    prefix_source = _source_path(spec_path, template.get("path"), "template")
    recipe_source = _source_path(spec_path, recipe.get("path"), "recipe")
    if not runtime_source.is_dir() or not prefix_source.is_dir() or not recipe_source.is_file():
        raise KitError("runtime/template/recipe input has the wrong type")
    if template.get("user") != "prospero":
        raise KitError("template user must be prospero")
    recipe_id = _require_id(recipe.get("id"), "recipe id")
    recipe_version = recipe.get("version")
    if not isinstance(recipe_version, str) or not recipe_version or len(recipe_version) > 64:
        raise KitError("recipe version must be an explicit non-empty string")
    recipe_document = _read_json(recipe_source, "recipe")
    if (recipe_document.get("id") != recipe_id or
            str(recipe_document.get("version", "")) != recipe_version):
        raise KitError("recipe id or version does not match the kit specification")
    runtime_inventory = _load_runtime_inventory(
        runtime_inventory_path, str(runtime.get("inventory_prefix", "")))
    module_counts = _validate_runtime(runtime_source, runtime_inventory)
    _validate_prefix(prefix_source, spec.get("dxvk"), recipe_document)
    _validate_no_personal_state(runtime_source, "runtime")

    prepared_variants: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    seen_titles: set[str] = set()
    for raw in variants:
        if not isinstance(raw, dict):
            raise KitError("variant entry must be an object")
        variant_id = _require_id(raw.get("id"), "variant id")
        title_id = raw.get("title_id")
        if not isinstance(title_id, str) or not TITLE_RE.fullmatch(title_id):
            raise KitError(f"variant title ID {title_id!r} is outside FAKE78000-FAKE78999")
        if variant_id in seen_ids or title_id in seen_titles:
            raise KitError("variant IDs and title IDs must be unique")
        seen_ids.add(variant_id)
        seen_titles.add(title_id)
        if raw.get("wrapper_revision") != sources["wrapper"]["revision"]:
            raise KitError(f"variant {variant_id} wrapper revision does not match provenance")
        if raw.get("helper_revision") != sources["helper"]["revision"]:
            raise KitError(f"variant {variant_id} helper revision does not match provenance")
        title_source = _source_path(spec_path, raw.get("path"), f"variant {variant_id}")
        _validate_no_personal_state(title_source, f"variant {variant_id}")
        helper_manifest_path = _source_path(
            spec_path, raw.get("helper_manifest"), f"variant {variant_id} helper manifest")
        protocol_path = _source_path(
            spec_path, raw.get("helper_protocol"), f"variant {variant_id} helper protocol")
        native_build_path = _source_path(
            spec_path, raw.get("native_build_manifest"),
            f"variant {variant_id} native build manifest")
        if native_build_path != title_source / "native-title-build.json":
            raise KitError(f"variant {variant_id} native build manifest path is not canonical")
        helper = title_source / "lapy.elf"
        _validate_helper_pin(helper, title_id, variant_id)
        try:
            validate_helper_manifest(_read_json(helper_manifest_path, "helper manifest"),
                                     helper, protocol_path, title_id)
        except (OSError, ValueError) as error:
            raise KitError(f"variant {variant_id} helper validation failed: {error}") from error
        parameter = _read_json(title_source / "sce_sys/param.json", "title param.json")
        if parameter.get("titleId") != title_id:
            raise KitError(f"variant {variant_id} title metadata does not match {title_id}")
        _validate_native_title_build(native_build_path, title_source / "eboot.bin",
                                     title_id, raw["wrapper_revision"])
        _required_executable(title_source / "eboot.bin", f"variant {variant_id} eboot.bin")
        _required_executable(helper, f"variant {variant_id} lapy.elf")
        prepared_variants.append({
            "id": variant_id,
            "title_id": title_id,
            "source": title_source,
            "helper_manifest": helper_manifest_path,
            "helper_protocol": protocol_path,
            "native_build_manifest": native_build_path,
            "wrapper_revision": raw["wrapper_revision"],
            "helper_revision": raw["helper_revision"],
        })

    destination = Path(out).resolve()
    if destination.exists():
        raise KitError(f"refusing to overwrite immutable kit directory {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{destination.name}.", dir=destination.parent))
    try:
        runtime_destination = staging / "runtime" / bundle_id / artifact_revision
        _copy_plain_tree(runtime_source, runtime_destination, "runtime")
        normalized_inventory = runtime_destination / "runtime-inventory.json"
        normalized_inventory.write_text(
            json.dumps(runtime_inventory, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        normalized_inventory.chmod(0o644)
        template_destination = staging / "template/bottle"
        _copy_console_prefix(prefix_source, template_destination)
        recipe_destination = staging / "recipes" / f"{recipe_id}.json"
        recipe_destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(recipe_source, recipe_destination)
        recipe_destination.chmod(stat.S_IMODE(recipe_source.stat().st_mode))

        variant_manifest: list[dict[str, Any]] = []
        for variant in prepared_variants:
            title_destination = staging / "titles" / variant["id"]
            _copy_plain_tree(variant["source"], title_destination, f"variant {variant['id']}")
            shutil.copyfile(variant["helper_manifest"], title_destination / "lapy-manifest.json")
            shutil.copyfile(variant["helper_protocol"], title_destination / "lapy_elevation_protocol.h")
            (title_destination / "lapy-manifest.json").chmod(0o644)
            (title_destination / "lapy_elevation_protocol.h").chmod(0o644)
            variant_manifest.append({
                "id": variant["id"],
                "title_id": variant["title_id"],
                "path": f"titles/{variant['id']}",
                "wrapper_revision": variant["wrapper_revision"],
                "helper_revision": variant["helper_revision"],
                "helper_manifest": f"titles/{variant['id']}/lapy-manifest.json",
                "helper_protocol": f"titles/{variant['id']}/lapy_elevation_protocol.h",
                "native_build_manifest": f"titles/{variant['id']}/native-title-build.json",
            })

        entries = _file_entries(staging)
        manifest: dict[str, Any] = {
            "schema": MANIFEST_SCHEMA,
            "kit_id": kit_id,
            "firmware": firmware,
            "architecture": spec["architecture"],
            "runtime": {
                "bundle_id": bundle_id,
                "artifact_revision": artifact_revision,
                "wine_revision": runtime["wine_revision"],
                "path": f"runtime/{bundle_id}/{artifact_revision}",
                "inventory_manifest": (
                    f"runtime/{bundle_id}/{artifact_revision}/runtime-inventory.json"),
                "module_counts": module_counts,
            },
            "template": {"path": "template/bottle", "user": "prospero"},
            "recipe": {
                "id": recipe_id,
                "version": recipe_version,
                "path": f"recipes/{recipe_id}.json",
                "sha256": _sha256(recipe_destination),
            },
            "variants": variant_manifest,
            "dxvk": json.loads(json.dumps(spec["dxvk"])),
            "sources": sources,
            "capabilities": sorted(set(capabilities)),
            "files": entries,
            "files_sha256": _files_digest(entries),
        }
        (staging / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        (staging / "manifest.json").chmod(0o644)
        validate_kit(staging)
        os.replace(staging, destination)
        return manifest
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def _resolve_kit_path(root: Path, value: object) -> Path:
    relative = _safe_relative(value)
    path = root / relative
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise KitError(f"manifest file is missing: {relative}") from error
    resolved_root = root.resolve()
    if resolved != resolved_root and resolved_root not in resolved.parents:
        raise KitError(f"manifest path escapes the kit: {relative}")
    return path


def validate_kit(kit: str | Path, manifest_path: str | Path | None = None) -> dict[str, Any]:
    """Validate a packaged kit and return its manifest."""
    root = Path(kit).resolve()
    manifest_file = Path(manifest_path).resolve() if manifest_path else root / "manifest.json"
    for path in root.rglob("*"):
        if path.is_symlink():
            raise KitError(f"kit payload contains a symbolic link: {path.relative_to(root)}")
    manifest = _read_json(manifest_file, "kit manifest")
    if manifest.get("schema") != MANIFEST_SCHEMA:
        raise KitError(f"unknown kit manifest schema: {manifest.get('schema')!r}")
    _require_id(manifest.get("kit_id"), "kit_id")
    if manifest.get("architecture") != "x86_64+wow64-i386":
        raise KitError("kit manifest has the wrong architecture")
    _validate_kit_contract(manifest)
    _validate_sources(manifest)
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise KitError("kit manifest lists no files")
    declared: set[str] = set()
    normalized: list[dict[str, Any]] = []
    for entry in files:
        if not isinstance(entry, dict) or set(entry) != {"path", "size", "sha256", "mode"}:
            raise KitError("kit manifest has a malformed file entry")
        relative = _safe_relative(entry["path"])
        if relative in declared:
            raise KitError(f"kit manifest repeats file: {relative}")
        declared.add(relative)
        path = root / relative
        if path.is_symlink():
            raise KitError(f"kit payload contains a symbolic link: {relative}")
        resolved = _resolve_kit_path(root, relative)
        if not resolved.is_file():
            raise KitError(f"manifest entry is not a file: {relative}")
        if resolved.stat().st_size != entry["size"]:
            raise KitError(f"file size does not match manifest: {relative}")
        if _sha256(resolved) != entry["sha256"]:
            raise KitError(f"file hash does not match manifest: {relative}")
        actual_mode = f"{stat.S_IMODE(resolved.stat().st_mode):04o}"
        if actual_mode != entry["mode"]:
            raise KitError(f"file mode does not match manifest: {relative}")
        normalized.append(entry)
    if [entry["path"] for entry in normalized] != sorted(declared):
        raise KitError("kit manifest file entries are not sorted")
    actual = {path.relative_to(root).as_posix() for path in root.rglob("*")
              if path.is_file() and path != manifest_file}
    if actual != declared:
        missing = sorted(declared - actual)
        extra = sorted(actual - declared)
        raise KitError(f"kit file set differs from manifest: missing={missing}, extra={extra}")
    if _files_digest(normalized) != manifest.get("files_sha256"):
        raise KitError("kit aggregate file hash does not match")

    runtime = manifest.get("runtime")
    template = manifest.get("template")
    recipe = manifest.get("recipe")
    variants = manifest.get("variants")
    if not all(isinstance(value, dict) for value in (runtime, template, recipe)):
        raise KitError("manifest lacks runtime, template, or recipe identity")
    if not isinstance(variants, list) or not variants:
        raise KitError("manifest lists no title variants")
    runtime_root = _resolve_kit_path(root, runtime.get("path"))
    bundle_id = _require_id(runtime.get("bundle_id"), "runtime bundle_id")
    artifact_revision = _require_id(runtime.get("artifact_revision"), "runtime artifact_revision")
    if runtime.get("path") != f"runtime/{bundle_id}/{artifact_revision}":
        raise KitError("runtime path does not match its bundle identity")
    if runtime.get("artifact_revision") == manifest["sources"]["wrapper"]["revision"]:
        raise KitError("runtime artifact revision must be distinct from wrapper revision")
    if runtime.get("wine_revision") != manifest["sources"]["wine"]["revision"]:
        raise KitError("runtime Wine revision does not match provenance")
    inventory_relative = f"runtime/{bundle_id}/{artifact_revision}/runtime-inventory.json"
    if runtime.get("inventory_manifest") != inventory_relative:
        raise KitError("runtime inventory manifest path is not canonical")
    inventory_path = _resolve_kit_path(root, runtime.get("inventory_manifest"))
    runtime_inventory = _load_runtime_inventory(inventory_path)
    counts = _validate_runtime(runtime_root, runtime_inventory)
    if runtime.get("module_counts") != counts:
        raise KitError("runtime module counts do not match its inventory")
    recipe_path = _resolve_kit_path(root, recipe.get("path"))
    recipe_id = _require_id(recipe.get("id"), "recipe id")
    if recipe.get("path") != f"recipes/{recipe_id}.json":
        raise KitError("recipe path does not match its identity")
    if not isinstance(recipe.get("version"), str) or not recipe["version"]:
        raise KitError("recipe version must be an explicit non-empty string")
    if _sha256(recipe_path) != recipe.get("sha256"):
        raise KitError("recipe hash does not match its identity")
    recipe_document = _read_json(recipe_path, "recipe")
    if (recipe_document.get("id") != recipe_id or
            str(recipe_document.get("version", "")) != recipe["version"]):
        raise KitError("packaged recipe id or version does not match its identity")
    prefix = _resolve_kit_path(root, template.get("path"))
    if template.get("path") != "template/bottle":
        raise KitError("template path is not canonical")
    if template.get("user") != "prospero":
        raise KitError("template user must be prospero")
    _validate_prefix(prefix, manifest.get("dxvk"), recipe_document)
    drive_links = prefix / "dosdevices" / LINK_TABLE
    if not drive_links.is_file():
        raise KitError("template is missing console drive mappings")
    mappings = drive_links.read_text(encoding="utf-8")
    if "c:\t../drive_c\n" not in mappings or "z:\t/\n" not in mappings:
        raise KitError("template console drive mappings must include c: and z:")
    seen_ids: set[str] = set()
    seen_titles: set[str] = set()
    for variant in variants:
        if not isinstance(variant, dict):
            raise KitError("variant entry is not an object")
        variant_id = _require_id(variant.get("id"), "variant id")
        title_id = variant.get("title_id")
        if not isinstance(title_id, str) or not TITLE_RE.fullmatch(title_id):
            raise KitError(f"variant title ID {title_id!r} is outside FAKE78000-FAKE78999")
        if variant_id in seen_ids or title_id in seen_titles:
            raise KitError("variant IDs and title IDs must be unique")
        seen_ids.add(variant_id)
        seen_titles.add(title_id)
        if variant.get("wrapper_revision") != manifest["sources"]["wrapper"]["revision"]:
            raise KitError(f"variant {variant_id} wrapper provenance does not match")
        if variant.get("helper_revision") != manifest["sources"]["helper"]["revision"]:
            raise KitError(f"variant {variant_id} helper provenance does not match")
        title = _resolve_kit_path(root, variant.get("path"))
        if variant.get("path") != f"titles/{variant_id}":
            raise KitError(f"variant {variant_id} title path is not canonical")
        if variant.get("helper_manifest") != f"titles/{variant_id}/lapy-manifest.json":
            raise KitError(f"variant {variant_id} helper manifest path is not canonical")
        if variant.get("helper_protocol") != f"titles/{variant_id}/lapy_elevation_protocol.h":
            raise KitError(f"variant {variant_id} helper protocol path is not canonical")
        if variant.get("native_build_manifest") != f"titles/{variant_id}/native-title-build.json":
            raise KitError(f"variant {variant_id} native build manifest path is not canonical")
        helper_manifest = _resolve_kit_path(root, variant.get("helper_manifest"))
        protocol = _resolve_kit_path(root, variant.get("helper_protocol"))
        native_build = _resolve_kit_path(root, variant.get("native_build_manifest"))
        _validate_helper_pin(title / "lapy.elf", title_id, variant_id)
        try:
            validate_helper_manifest(_read_json(helper_manifest, "helper manifest"),
                                     title / "lapy.elf", protocol, title_id)
        except (OSError, ValueError) as error:
            raise KitError(f"variant {variant_id} helper validation failed: {error}") from error
        parameter = _read_json(title / "sce_sys/param.json", "title param.json")
        if parameter.get("titleId") != title_id:
            raise KitError(f"variant {variant_id} title metadata does not match")
        _validate_native_title_build(native_build, title / "eboot.bin", title_id,
                                     variant["wrapper_revision"])
        _required_executable(title / "eboot.bin", f"variant {variant_id} eboot.bin")
        _required_executable(title / "lapy.elf", f"variant {variant_id} lapy.elf")
        for module in title.rglob("*.prx"):
            _required_executable(module, module.relative_to(root).as_posix())
    for module in runtime_root.rglob("*.prx"):
        _required_executable(module, module.relative_to(root).as_posix())
    _validate_no_personal_state(root, "kit")
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    package = commands.add_parser("package", help="build a kit from local inputs")
    package.add_argument("--spec", required=True)
    package.add_argument("--out", required=True)
    validate = commands.add_parser("validate", help="validate an existing kit")
    validate.add_argument("kit")
    try:
        arguments = parser.parse_args(argv)
        if arguments.command == "package":
            result = package_kit(arguments.spec, arguments.out)
            print(f"Playden kit packaged: {result['kit_id']} files={len(result['files'])} "
                  f"sha256={result['files_sha256']}")
        else:
            result = validate_kit(arguments.kit)
            print(f"Playden kit valid: {result['kit_id']} variants={len(result['variants'])} "
                  f"files={len(result['files'])} sha256={result['files_sha256']}")
        return 0
    except KitError as error:
        print(f"Playden kit error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
