#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise native-build title identity and pinned helper inputs."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "build_native.sh"
DEFAULT_TITLE_ID = "PPSA99995"
CUSTOM_TITLE_ID = "FAKE78001"
FOUNDATION_PIN = "9c0b994a048521af6fb84c73ded364504fe250e9"


def write(path: Path, data: bytes | str = "x", executable: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, bytes):
        path.write_bytes(data)
    else:
        path.write_text(data)
    if executable:
        path.chmod(path.stat().st_mode | stat.S_IXUSR)


def make_helper(root: Path, title_id: str) -> tuple[Path, Path]:
    helper = root / "lapy.elf"
    manifest = root / "lapy-manifest.json"
    write(helper, b"\x7fELFtested local helper")
    protocol = ROOT / "native" / "lapy_elevation_protocol.h"
    manifest.write_text(json.dumps({
        "mode": "elf-helper",
        "target_title": title_id,
        "elf_sha256": hashlib.sha256(helper.read_bytes()).hexdigest(),
        "protocol_sha256": hashlib.sha256(protocol.read_bytes()).hexdigest(),
        "features": ["root_layout_probe_retry"],
    }))
    return helper, manifest


def make_fake_toolchain(root: Path) -> tuple[Path, Path, Path]:
    foundation = root / "foundation"
    sdk = foundation / ".deps" / "native" / "ps5-payload-sdk"
    write(foundation / ".git" / "keep")
    write(sdk / "target" / "lib" / "libkernel.so")
    write(sdk / "bin" / "prospero-lld", """#!/bin/sh
out=
while [ "$#" -gt 0 ]; do
    if [ "$1" = -o ]; then out=$2; shift 2; else shift; fi
done
: > "$out"
""", executable=True)
    write(foundation / "runtime" / "libc.prx")
    write(foundation / "tooling" / "native" / "ps5-pie-high.ld")
    write(foundation / "tooling" / "native" / "app_crt.cpp")
    write(foundation / "tooling" / "prospero-clang18", """#!/bin/sh
printf '%s\\n' "$*" >> "$PW_TEST_COMPILE_LOG"
out=
while [ "$#" -gt 0 ]; do
    if [ "$1" = -o ]; then out=$2; shift 2; else shift; fi
done
: > "$out"
""", executable=True)
    tool = foundation / "build" / "host" / f"ps5-native-tool-{FOUNDATION_PIN}"
    write(tool, """#!/bin/sh
out=
while [ "$#" -gt 0 ]; do
    if [ "$1" = --out ]; then out=$2; shift 2; else shift; fi
done
: > "$out"
""", executable=True)

    fake_bin = root / "bin"
    write(fake_bin / "git", f"""#!/bin/sh
case "$*" in
  *'rev-parse HEAD'*) printf '%s\\n' {FOUNDATION_PIN} ;;
  *'diff --quiet'*) exit 0 ;;
  *) exit 0 ;;
esac
""", executable=True)
    compile_log = root / "compile.log"
    return foundation, fake_bin, compile_log


def run(env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(SCRIPT)], cwd=ROOT, env={**os.environ, **env},
        capture_output=True, text=True)


def main() -> int:
    param = json.loads((ROOT / "sce_sys" / "param.json").read_text())
    assert param["titleId"] == DEFAULT_TITLE_ID
    assert param["conceptId"] == "99995"
    assert param["contentId"] == "UP9000-PPSA99995_00-PROSPEROWIN00001"

    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        helper, manifest = make_helper(temp / "helper", CUSTOM_TITLE_ID)

        bad = run({"PW_TITLE_ID": "fake78001"})
        assert bad.returncode == 2 and "PW_TITLE_ID" in bad.stderr, bad.stderr

        bad = run({
            "PW_TITLE_ID": CUSTOM_TITLE_ID,
            "PW_LAPY_HELPER_ELF": str(helper),
        })
        assert bad.returncode == 2 and "must be set together" in bad.stderr, bad.stderr

        wrong_helper, wrong_manifest = make_helper(temp / "wrong", "FAKE78002")
        bad = run({
            "PW_TITLE_ID": CUSTOM_TITLE_ID,
            "PW_LAPY_HELPER_ELF": str(wrong_helper),
            "PW_LAPY_HELPER_MANIFEST": str(wrong_manifest),
        })
        assert bad.returncode == 2 and "title or digest does not match" in bad.stderr, bad.stderr

        foundation, fake_bin, compile_log = make_fake_toolchain(temp)
        suffix = "-identity-test"
        result = run({
            "PATH": f"{fake_bin}:{os.environ['PATH']}",
            "PW_TITLE_ID": CUSTOM_TITLE_ID,
            "PW_LAPY_HELPER_ELF": str(helper),
            "PW_LAPY_HELPER_MANIFEST": str(manifest),
            "PW_FOUNDATION_READY": "1",
            "PS5_NATIVE_FOUNDATION": str(foundation),
            "PW_OUTPUT_SUFFIX": suffix,
            "PW_TEST_COMPILE_LOG": str(compile_log),
            "LLVM_READELF": "/bin/true",
        })
        assert result.returncode == 0, result.stderr
        dist = ROOT / "dist" / f"{CUSTOM_TITLE_ID}{suffix}"
        built_param = json.loads((dist / "sce_sys" / "param.json").read_text())
        native_build = json.loads((dist / "native-title-build.json").read_text())
        assert built_param["titleId"] == CUSTOM_TITLE_ID
        assert built_param["conceptId"] == "78001"
        assert built_param["contentId"] == "UP9000-FAKE78001_00-PROSPEROWIN00001"
        assert (dist / "lapy.elf").read_bytes() == helper.read_bytes()
        assert (dist / "lapy-manifest.json").read_bytes() == manifest.read_bytes()
        assert native_build == {
            "schema": "prospero-win-native-title/1",
            "title_id": CUSTOM_TITLE_ID,
            "source_revision": FOUNDATION_PIN,
            "source_dirty": False,
            "eboot_sha256": hashlib.sha256((dist / "eboot.bin").read_bytes()).hexdigest(),
        }
        compile_commands = compile_log.read_text()
        assert '-DPW_TITLE_ID="FAKE78001"' in compile_commands, compile_commands
        assert "src/pw_title_launch.c" in compile_commands, compile_commands
        assert "src/pw_title_session.c" in compile_commands, compile_commands

        # Test artifacts are isolated by suffix and are not release inputs.
        subprocess.run(["rm", "-rf", str(ROOT / "build" / f"native{suffix}"), str(dist)],
                       check=True)

    print("title identity passed: validation, pinned helper, compile define and metadata")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
