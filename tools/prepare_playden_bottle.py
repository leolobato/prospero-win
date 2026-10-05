#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build and verify a fresh dual-architecture Playden Wine bottle.

The retained PS5 Wine runtime is immutable input.  A legacy i386 Unix Wine
loader, built from the exact corresponding Wine source, is the only runtime
overlay.  It lets QEMU run PE32 setup in a separate i386 process instead of
using Wine's in-process new-WoW64 CPU mode switch.
"""

from __future__ import annotations

try:
    from tools.pw_registry import windows_user_bound
except ModuleNotFoundError:
    from pw_registry import windows_user_bound

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import time
from pathlib import Path

PE_I386 = 0x014C
PE_AMD64 = 0x8664
CORE32 = (
    "ntdll.dll", "kernel32.dll", "kernelbase.dll", "setupapi.dll",
    "advapi32.dll", "crypt32.dll", "dinput8.dll", "mmdevapi.dll",
)
CORE64 = ("ntdll.dll", "kernel32.dll", "kernelbase.dll", "setupapi.dll")
REQUIRED_32_REGISTRY = (
    r"[Software\\Classes\\Wow6432Node\\CLSID\\{25E609E4-B259-11CF-BFC7-444553540000}]",
    r"[Software\\Classes\\Wow6432Node\\CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}]",
    r"[Software\\Wow6432Node\\Microsoft\\Cryptography\\Defaults\\Provider",
)
MATERIAL_DIAGNOSTICS = (
    "assertion failed", "xstate", "load_wow64_ntdll then crash",
    'loader_init "ws2_32.dll" failed to initialize', "got unexpected trap",
)
RETAINED_TREES = (
    Path("lib/wine/i386-windows"),
    Path("lib/wine/x86_64-windows"),
    Path("share/wine"),
)
FONT_VALUE_RE = re.compile(
    r'^(?P<head>"[^"]+"=)"(?P<path>[^"]+\.(?:fon|otf|ttc|ttf))"$',
    re.IGNORECASE)


class Failure(SystemExit):
    pass


def fail(message: str) -> None:
    raise Failure(f"Playden bottle preparation: {message}")


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def retained_inventory(runtime: Path) -> dict[str, str]:
    inventory: dict[str, str] = {}
    for relative_root in RETAINED_TREES:
        root = runtime / relative_root
        if not root.is_dir():
            fail(f"retained runtime lacks {relative_root}")
        for path in sorted(root.rglob("*")):
            if path.is_file():
                inventory[path.relative_to(runtime).as_posix()] = file_sha256(path)
    return inventory


def stage_runtime(runtime: Path, host_runtime: Path, overlay: Path,
                  staged: Path) -> None:
    if staged.exists():
        fail(f"staged runtime already exists: {staged}")
    source_inventory = retained_inventory(runtime)
    unix_overlay = overlay / "lib/wine/i386-unix"
    if not (unix_overlay / "wine").is_file() or not (unix_overlay / "ntdll.so").is_file():
        fail(f"i386 Unix overlay is incomplete: {unix_overlay}")
    shutil.copytree(runtime, staged, symlinks=True)
    for relative in (Path("bin/wine"), Path("bin/wineserver")):
        source = host_runtime / relative
        if not source.is_file():
            fail(f"host runtime lacks launcher: {relative}")
        destination_file = staged / relative
        destination_file.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination_file)
    host_unix = host_runtime / "lib/wine/x86_64-unix"
    if not host_unix.is_dir():
        fail("host runtime lacks x86_64 Unix Wine tree")
    destination_host_unix = staged / "lib/wine/x86_64-unix"
    if destination_host_unix.exists():
        shutil.rmtree(destination_host_unix)
    shutil.copytree(host_unix, destination_host_unix, symlinks=True)
    destination = staged / "lib/wine/i386-unix"
    if destination.exists():
        fail(f"retained runtime unexpectedly contains {destination.relative_to(staged)}")
    shutil.copytree(unix_overlay, destination, symlinks=True)
    if retained_inventory(runtime) != source_inventory:
        fail("source runtime changed while staging")
    if retained_inventory(staged) != source_inventory:
        fail("staging changed retained PE or Wine data bytes")


def pe_identity(path: Path) -> tuple[int, int]:
    raw = path.read_bytes()
    if len(raw) < 0x9A or raw[:2] != b"MZ":
        fail(f"not a PE image: {path}")
    nt_offset = struct.unpack_from("<I", raw, 0x3C)[0]
    if nt_offset + 26 > len(raw) or raw[nt_offset:nt_offset + 4] != b"PE\0\0":
        fail(f"not a PE image: {path}")
    return (struct.unpack_from("<H", raw, nt_offset + 4)[0],
            struct.unpack_from("<H", raw, nt_offset + 24)[0])


def validate_prefix(prefix: Path, user: str) -> dict[str, str]:
    for directory, names, machine, magic in (
        ("system32", CORE64, PE_AMD64, 0x20B),
        ("syswow64", CORE32, PE_I386, 0x10B),
    ):
        for name in names:
            path = prefix / "drive_c/windows" / directory / name
            if not path.is_file():
                fail(f"missing core {directory} file: {name}")
            actual_machine, actual_magic = pe_identity(path)
            if (actual_machine, actual_magic) != (machine, magic):
                fail(f"wrong PE architecture for {directory}/{name}")
    system_registry = (prefix / "system.reg").read_text(
        encoding="utf-8", errors="strict")
    for registration in REQUIRED_32_REGISTRY:
        if registration not in system_registry:
            fail(f"missing 32-bit registry registration: {registration}")
    profile = prefix / "drive_c/users" / user
    if not profile.is_dir():
        fail(f"missing Windows user profile: {user}")
    user_registry = (prefix / "user.reg").read_text(encoding="utf-8")
    if not windows_user_bound(user_registry, user):
        fail(f"registry does not bind the Windows profile to {user}")
    expected_links = {"c:": "../drive_c", "z:": "/"}
    for name, target in expected_links.items():
        link = prefix / "dosdevices" / name
        if not link.is_symlink() or os.readlink(link) != target:
            fail(f"unexpected drive mapping for {name}")
    return {"system32_machine": "amd64", "syswow64_machine": "i386",
            "user": user}


def legacy_configure_command(source: Path, wine64_build: Path,
                             overlay: Path, compiler: str) -> tuple[list[str], dict[str, str]]:
    command = [
        str(source / "configure"), f"--prefix={overlay}",
        f"--with-wine64={wine64_build}", "--disable-tests", "--without-x",
        "--without-wayland", "--without-freetype",
    ]
    environment = os.environ.copy()
    environment["CC"] = f"{compiler} -m32"
    return command, environment


def git_output(source: Path, *arguments: str, text: bool = True):
    completed = subprocess.run(
        ["git", "-C", str(source), *arguments], check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=text)
    if completed.returncode:
        fail(f"cannot inspect Wine source with git {' '.join(arguments)}")
    return completed.stdout


def validate_source_binding(binding_path: Path, source: Path,
                            wine64_build: Path,
                            runtime_commit: str) -> dict[str, object]:
    head = git_output(source, "rev-parse", "HEAD").strip()
    diff = git_output(source, "diff", "HEAD", "--binary", text=False)
    staged_diff = git_output(source, "diff", "--cached", "--binary", text=False)
    unstaged_diff = git_output(source, "diff", "--binary", text=False)
    status = git_output(source, "status", "--porcelain")
    untracked = git_output(source, "ls-files", "--others", "--exclude-standard")
    if untracked.strip():
        fail(f"Wine source has untracked inputs: {untracked.strip()}")
    config_log = wine64_build / "config.log"
    if not config_log.is_file():
        fail(f"64-bit Wine build has no config.log: {wine64_build}")
    try:
        binding = json.loads(binding_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(f"cannot read reviewed source binding: {error}")
    changed_names = git_output(
        source, "diff", "HEAD", "--name-only", "-z", text=False).split(b"\0")
    changed_files: dict[str, str | None] = {}
    for raw_name in changed_names:
        if not raw_name:
            continue
        name = raw_name.decode("utf-8", errors="strict")
        changed_path = source / name
        changed_files[name] = file_sha256(changed_path) if changed_path.is_file() else None
    actual = {
        "retained_runtime_commit": runtime_commit,
        "wine_source_head": head,
        "wine_source_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "wine_source_staged_diff_sha256": hashlib.sha256(staged_diff).hexdigest(),
        "wine_source_unstaged_diff_sha256": hashlib.sha256(unstaged_diff).hexdigest(),
        "wine_source_status_sha256": hashlib.sha256(
            status.encode("utf-8")).hexdigest(),
        "wine_source_changed_files": changed_files,
        "wine64_config_log_sha256": file_sha256(config_log),
    }
    if binding.get("schema") != "prospero-win-wine-source-binding/1":
        fail("reviewed source binding has unsupported schema")
    for name, value in actual.items():
        if binding.get(name) != value:
            fail(f"reviewed source binding mismatch: {name}")
    actual["wine_source_dirty"] = bool(status.strip())
    actual["source_binding_sha256"] = file_sha256(binding_path)
    return actual


def write_loader_provenance(path: Path, source: Path, wine64_build: Path,
                            overlay: Path, configure: list[str], compiler: str,
                            runtime_commit: str,
                            source_binding: Path) -> None:
    source_identity = validate_source_binding(
        source_binding, source, wine64_build, runtime_commit)
    unix = overlay / "lib/wine/i386-unix"
    outputs = {
        item.relative_to(overlay).as_posix(): file_sha256(item)
        for item in sorted(unix.rglob("*")) if item.is_file()
    }
    document = {
        "schema": "prospero-win-playden-loader-provenance/1",
        **source_identity,
        "configure": configure,
        "cc": compiler,
        "outputs": outputs,
    }
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def validate_loader_provenance(path: Path, overlay: Path) -> dict[str, object]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(f"cannot read loader provenance: {error}")
    if document.get("schema") != "prospero-win-playden-loader-provenance/1":
        fail("loader provenance has unsupported schema")
    if not document.get("source_binding_sha256"):
        fail("loader provenance lacks reviewed source binding")
    unix = overlay / "lib/wine/i386-unix"
    actual = {
        item.relative_to(overlay).as_posix(): file_sha256(item)
        for item in sorted(unix.rglob("*")) if item.is_file()
    }
    if document.get("outputs") != actual:
        fail("i386 Unix overlay differs from loader provenance")
    return document


def inventory_sha256(inventory: dict[str, str]) -> str:
    encoded = json.dumps(inventory, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def write_initialization_provenance(
        path: Path, arguments: argparse.Namespace,
        loader_document: dict[str, object], manifest_sha256: str) -> None:
    if path.exists():
        fail(f"initialization provenance already exists: {path}")
    host_inventory: dict[str, str] = {}
    for relative in (Path("bin/wine"), Path("bin/wineserver")):
        item = arguments.host_runtime / relative
        host_inventory[relative.as_posix()] = file_sha256(item)
    for item in sorted((arguments.host_runtime / "lib/wine/x86_64-unix").rglob("*")):
        if item.is_file():
            host_inventory[item.relative_to(arguments.host_runtime).as_posix()] = file_sha256(item)
    document = {
        "schema": "prospero-win-playden-bottle-initialization/1",
        "loader_provenance_sha256": file_sha256(arguments.loader_provenance),
        "source_binding_sha256": loader_document["source_binding_sha256"],
        "retained_runtime_inventory_sha256": inventory_sha256(
            retained_inventory(arguments.runtime)),
        "host_runtime_inventory_sha256": inventory_sha256(host_inventory),
        "prefix_manifest_sha256": manifest_sha256,
        "user": arguments.user,
        "dispatch": ("direct-binfmt" if arguments.dispatcher is None
                     else f"proot:{arguments.dispatcher}"),
        "display": arguments.display,
    }
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def guest_command(dispatcher: Path | None, executable: Path,
                  *arguments: str) -> list[str]:
    if dispatcher is None:
        return [str(executable), *arguments]
    return ["proot", "-q", str(dispatcher), str(executable), *arguments]


def bottle_fixtures() -> tuple[tuple[str, ...], ...]:
    return (
        (r"C:\windows\system32\cmd.exe", "/d", "/c", "ver"),
        (r"C:\windows\syswow64\cmd.exe", "/d", "/c", "ver"),
        (r"C:\windows\syswow64\ipconfig.exe", "/all"),
    )


def reject_material_diagnostics(output: str) -> None:
    lowered = output.lower()
    for marker in MATERIAL_DIAGNOSTICS:
        if marker in lowered:
            fail(f"material Wine diagnostic: {marker}")


def pin_registered_fonts(prefix: Path, runtime: Path, host_root: Path) -> list[str]:
    """Copy registered host fonts into C:\\windows\\Fonts and rewrite paths."""
    allowed_roots = (
        (runtime / "share/wine/fonts").resolve(),
        (host_root / "usr/share/fonts").resolve(),
    )
    destination_root = prefix / "drive_c/windows/Fonts"
    destination_root.mkdir(parents=True, exist_ok=True)
    copied: set[str] = set()
    for registry_name in ("system.reg", "user.reg", "userdef.reg"):
        registry = prefix / registry_name
        if not registry.exists():
            continue
        output: list[str] = []
        for line in registry.read_text(encoding="utf-8").splitlines(keepends=True):
            match = FONT_VALUE_RE.match(line.rstrip("\n"))
            if not match:
                output.append(line)
                continue
            encoded = match.group("path")
            normalized = encoded.replace("\\\\", "\\")
            if normalized.lower().startswith("z:\\"):
                unix_name = normalized[3:]
            elif normalized.startswith("\\??\\unix\\"):
                unix_name = normalized[len("\\??\\unix\\"):]
            else:
                output.append(line)
                continue
            decoded = Path("/" + unix_name.replace("\\", "/").lstrip("/"))
            runtime_fonts = allowed_roots[0]
            decoded_resolved = decoded.resolve()
            if (decoded_resolved == runtime_fonts or
                    runtime_fonts in decoded_resolved.parents):
                source = decoded_resolved
            else:
                source = (host_root / str(decoded).lstrip("/")).resolve()
            if not any(source == root or root in source.parents for root in allowed_roots):
                fail(f"registered font is outside audited roots: {source}")
            if not source.is_file():
                fail(f"registered font does not exist: {source}")
            destination = destination_root / source.name
            if destination.exists() and file_sha256(destination) != file_sha256(source):
                fail(f"font basename collision: {source.name}")
            if not destination.exists():
                shutil.copy2(source, destination)
            copied.add(source.name)
            replacement = f'C:\\\\windows\\\\Fonts\\\\{source.name}'
            ending = "\n" if line.endswith("\n") else ""
            output.append(f'{match.group("head")}"{replacement}"{ending}')
        registry.write_text("".join(output), encoding="utf-8")
    return sorted(copied)


def manifest_entries(prefix: Path) -> list[str]:
    entries: list[str] = []
    for path in sorted(prefix.rglob("*")):
        relative = path.relative_to(prefix).as_posix()
        if path.is_symlink():
            target = os.readlink(path)
            digest = hashlib.sha256(target.encode("utf-8")).hexdigest()
            entries.append(f"symlink\t{digest}\t{relative}\t{target}\n")
        elif path.is_file():
            entries.append(f"file\t{file_sha256(path)}\t{relative}\n")
    return entries


def write_manifest(prefix: Path, manifest: Path) -> str:
    if manifest.exists():
        fail(f"manifest already exists: {manifest}")
    manifest.write_text("".join(manifest_entries(prefix)), encoding="utf-8")
    return file_sha256(manifest)


def verify_manifest(prefix: Path, manifest: Path) -> None:
    expected = manifest.read_text(encoding="utf-8").splitlines(keepends=True)
    actual = manifest_entries(prefix)
    if actual != expected:
        fail("prefix hash drift")


def run_checked(command: list[str], *, cwd: Path | None = None,
                environment: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(command, cwd=cwd, env=environment, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode:
        fail(f"command failed ({completed.returncode}): {' '.join(command)}\n{completed.stdout}")
    return completed


def run_logged(command: list[str], transcripts: list[str], *, label: str,
               timeout: float,
               environment: dict[str, str],
               allowed_returncodes: tuple[int, ...] = (0,)
               ) -> subprocess.CompletedProcess[str]:
    try:
        completed = subprocess.run(
            command, env=environment, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        transcripts.append(f"\n## {label} (timeout)\n{output}")
        fail(f"command timed out after {timeout:g}s: {' '.join(command)}")
    transcripts.append(
        f"\n## {label} (exit {completed.returncode})\n{completed.stdout}")
    if completed.returncode not in allowed_returncodes:
        fail(f"command failed ({completed.returncode}): {' '.join(command)}")
    reject_material_diagnostics(completed.stdout)
    return completed


def exact_prefix_processes(prefix: Path, process_marker: Path,
                           proc_root: Path = Path("/proc")) -> list[int]:
    expected = f"WINEPREFIX={prefix}".encode("utf-8")
    marker = str(process_marker).encode("utf-8")
    matches: list[int] = []
    for process in proc_root.glob("[0-9]*"):
        try:
            if process.stat().st_uid != os.geteuid():
                continue
            command = (process / "cmdline").read_bytes()
            if marker not in command and b"wineserver" not in command:
                continue
            fields = (process / "environ").read_bytes().split(b"\0")
        except FileNotFoundError:
            continue
        except (OSError, PermissionError) as error:
            fail(f"cannot prove prefix liveness from {process}: {error}")
        if expected in fields:
            matches.append(int(process.name))
    return matches


def build_loader(arguments: argparse.Namespace) -> None:
    if arguments.build_dir.exists() or arguments.overlay.exists():
        fail("build directory and overlay must be new")
    arguments.build_dir.mkdir(parents=True)
    arguments.overlay.mkdir(parents=True)
    command, environment = legacy_configure_command(
        arguments.wine_source, arguments.wine64_build, arguments.overlay,
        arguments.compiler)
    run_checked(command, cwd=arguments.build_dir, environment=environment)
    run_checked(["make", f"-j{arguments.jobs}"], cwd=arguments.build_dir)
    run_checked(["make", "install"], cwd=arguments.build_dir)
    unix = arguments.overlay / "lib/wine/i386-unix"
    if not (unix / "wine").is_file() or not (unix / "ntdll.so").is_file():
        fail("legacy build produced no usable i386 Unix loader")
    write_loader_provenance(
        arguments.provenance, arguments.wine_source, arguments.wine64_build,
        arguments.overlay, command, environment["CC"], arguments.runtime_commit,
        arguments.source_binding)


def init_bottle(arguments: argparse.Namespace) -> None:
    if arguments.prefix.exists():
        fail(f"prefix already exists: {arguments.prefix}")
    loader_document = validate_loader_provenance(
        arguments.loader_provenance, arguments.overlay)
    original = retained_inventory(arguments.runtime)
    stage_runtime(arguments.runtime, arguments.host_runtime, arguments.overlay,
                  arguments.staged_runtime)
    wine = arguments.staged_runtime / "bin/wine"
    wineserver = arguments.staged_runtime / "bin/wineserver"
    environment = os.environ.copy()
    environment.update({
        "USER": arguments.user, "WINEARCH": "win64",
        "WINEPREFIX": str(arguments.prefix), "WINESERVER": str(wineserver),
        "WINEDLLOVERRIDES": "mscoree,mshtml,winemenubuilder.exe=d",
        "WINEDEBUG": "err+all",
        "DISPLAY": arguments.display,
    })
    transcripts: list[str] = []
    primary_error: BaseException | None = None
    xvfb_log = arguments.log.with_suffix(arguments.log.suffix + ".xvfb")
    xvfb_handle = xvfb_log.open("w", encoding="utf-8")
    xvfb = subprocess.Popen(
        [arguments.xvfb_server, arguments.display, "-screen", "0",
         "1024x768x24", "-nolisten", "tcp"],
        stdout=xvfb_handle, stderr=subprocess.STDOUT, text=True)
    try:
        time.sleep(arguments.xvfb_ready_delay)
        if xvfb.poll() is not None:
            fail(f"Xvfb exited before Wine initialization; see {xvfb_log}")
        run_logged(guest_command(
            arguments.dispatcher, wine, "wineboot.exe", "--init"),
            transcripts, label="wineboot --init", timeout=arguments.timeout,
            environment=environment)
        for fixture_arguments in bottle_fixtures():
            fixture = run_logged(guest_command(
                arguments.dispatcher, wine, *fixture_arguments),
                transcripts, label=" ".join(fixture_arguments),
                timeout=arguments.timeout, environment=environment)
            if fixture_arguments[0].endswith("cmd.exe"):
                if "Microsoft Windows" not in fixture.stdout:
                    fail(f"{fixture_arguments[0]} produced unexpected output")
            elif "IPv4 address" not in fixture.stdout:
                fail("32-bit network fixture produced unexpected output")
    except BaseException as error:
        primary_error = error
    finally:
        shutdown_error: BaseException | None = None
        try:
            run_logged(guest_command(arguments.dispatcher, wineserver, "-k"),
                       transcripts, label="wineserver -k",
                       timeout=arguments.shutdown_timeout,
                       environment=environment, allowed_returncodes=(0, 1))
            run_logged(guest_command(arguments.dispatcher, wineserver, "-w"),
                       transcripts, label="wineserver -w",
                       timeout=arguments.shutdown_timeout,
                       environment=environment)
            live = exact_prefix_processes(
                arguments.prefix, arguments.staged_runtime)
            if live:
                fail(f"prefix still has live processes: {live}")
        except BaseException as error:
            shutdown_error = error
        arguments.log.write_text("".join(transcripts), encoding="utf-8")
        xvfb.terminate()
        try:
            xvfb.wait(timeout=arguments.shutdown_timeout)
        except subprocess.TimeoutExpired:
            xvfb.kill()
            xvfb.wait(timeout=arguments.shutdown_timeout)
        xvfb_handle.close()
        if shutdown_error is not None:
            raise shutdown_error
    if primary_error is not None:
        raise primary_error
    pin_registered_fonts(arguments.prefix, arguments.staged_runtime,
                         arguments.host_root)
    validate_prefix(arguments.prefix, arguments.user)
    if retained_inventory(arguments.runtime) != original:
        fail("source runtime changed during bottle initialization")
    manifest_sha256 = write_manifest(arguments.prefix, arguments.manifest)
    verify_manifest(arguments.prefix, arguments.manifest)
    write_initialization_provenance(
        arguments.initialization_provenance, arguments, loader_document,
        manifest_sha256)


def path(value: str) -> Path:
    return Path(value).resolve()


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build-loader")
    build.add_argument("--wine-source", type=path, required=True)
    build.add_argument("--wine64-build", type=path, required=True)
    build.add_argument("--build-dir", type=path, required=True)
    build.add_argument("--overlay", type=path, required=True)
    build.add_argument("--compiler", default="gcc")
    build.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    build.add_argument("--runtime-commit", required=True)
    build.add_argument("--source-binding", type=path, required=True)
    build.add_argument("--provenance", type=path, required=True)
    build.set_defaults(function=build_loader)
    init = commands.add_parser("init")
    init.add_argument("--runtime", type=path, required=True)
    init.add_argument("--host-runtime", type=path, required=True)
    init.add_argument("--overlay", type=path, required=True)
    init.add_argument("--staged-runtime", type=path, required=True)
    init.add_argument("--prefix", type=path, required=True)
    init.add_argument("--dispatcher", type=path)
    init.add_argument("--manifest", type=path, required=True)
    init.add_argument("--loader-provenance", type=path, required=True)
    init.add_argument("--initialization-provenance", type=path, required=True)
    init.add_argument("--log", type=path, required=True)
    init.add_argument("--user", default="prospero")
    init.add_argument("--xvfb-server", default="Xvfb")
    init.add_argument("--display", default=":97")
    init.add_argument("--xvfb-ready-delay", type=float, default=1.0)
    init.add_argument("--timeout", type=float, default=120.0)
    init.add_argument("--shutdown-timeout", type=float, default=30.0)
    init.add_argument("--host-root", type=path, default=Path("/"))
    init.set_defaults(function=init_bottle)
    return result


def main(argv: list[str] | None = None) -> None:
    arguments = parser().parse_args(argv)
    arguments.function(arguments)


if __name__ == "__main__":
    main()
