#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

import hashlib
import importlib.util
import os
import json
import subprocess
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
SPEC = importlib.util.spec_from_file_location(
    "prepare_playden_bottle", ROOT / "tools/prepare_playden_bottle.py")
assert SPEC and SPEC.loader
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)


def fake_pe(machine: int, pe32_plus: bool) -> bytes:
    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, machine)
    struct.pack_into("<H", data, 0x98, 0x20B if pe32_plus else 0x10B)
    return bytes(data)


class Case(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="playden-bottle-")
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def make_prefix(self) -> Path:
        prefix = self.root / "prefix"
        for directory in ("system32", "syswow64"):
            (prefix / "drive_c/windows" / directory).mkdir(parents=True)
        for name in PREPARE.CORE64:
            (prefix / "drive_c/windows/system32" / name).write_bytes(
                fake_pe(PREPARE.PE_AMD64, True))
        for name in PREPARE.CORE32:
            (prefix / "drive_c/windows/syswow64" / name).write_bytes(
                fake_pe(PREPARE.PE_I386, False))
        (prefix / "drive_c/users/prospero").mkdir(parents=True)
        (prefix / "drive_c/users/Public").mkdir()
        dosdevices = prefix / "dosdevices"
        dosdevices.mkdir()
        os.symlink("../drive_c", dosdevices / "c:")
        os.symlink("/", dosdevices / "z:")
        (prefix / "system.reg").write_text(
            "\n".join(PREPARE.REQUIRED_32_REGISTRY) + "\n", encoding="utf-8")
        (prefix / "user.reg").write_text(
            '[Volatile Environment]\n"USERNAME"="prospero"\n"USERPROFILE"="C:\\\\users\\\\prospero"\n',
            encoding="utf-8")
        return prefix

    def test_validate_prefix_requires_both_architectures_and_32bit_registry(self) -> None:
        prefix = self.make_prefix()
        result = PREPARE.validate_prefix(prefix, "prospero")
        self.assertEqual(result["system32_machine"], "amd64")
        self.assertEqual(result["syswow64_machine"], "i386")
        self.assertEqual(result["user"], "prospero")
        (prefix / "system.reg").write_text("", encoding="utf-8")
        with self.assertRaisesRegex(SystemExit, "32-bit registry registration"):
            PREPARE.validate_prefix(prefix, "prospero")

    def test_stage_runtime_preserves_retained_pe_bytes_and_only_adds_unix_overlay(self) -> None:
        runtime = self.root / "runtime"
        host_runtime = self.root / "host-runtime"
        overlay = self.root / "overlay"
        staged = self.root / "staged"
        for relative, content in (
            ("lib/wine/i386-windows/kernel32.dll", b"retained-i386"),
            ("lib/wine/x86_64-windows/kernel32.dll", b"retained-amd64"),
            ("share/wine/wine.inf", b"retained-inf"),
            ("bin/wine", b"wine"),
        ):
            path = runtime / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        unix = overlay / "lib/wine/i386-unix"
        unix.mkdir(parents=True)
        (unix / "wine").write_bytes(b"legacy-loader")
        (unix / "ntdll.so").write_bytes(b"legacy-ntdll")
        for relative, content in (
            ("bin/wine", b"host-wine"),
            ("bin/wineserver", b"host-server"),
            ("lib/wine/x86_64-unix/ntdll.so", b"host-ntdll"),
        ):
            path = host_runtime / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        before = PREPARE.retained_inventory(runtime)
        PREPARE.stage_runtime(runtime, host_runtime, overlay, staged)
        self.assertEqual(PREPARE.retained_inventory(runtime), before)
        self.assertEqual(PREPARE.retained_inventory(staged), before)
        self.assertEqual((staged / "lib/wine/i386-unix/wine").read_bytes(),
                         b"legacy-loader")
        self.assertEqual((staged / "bin/wine").read_bytes(), b"host-wine")

    def test_stage_runtime_refuses_existing_destination(self) -> None:
        runtime = self.root / "runtime"
        host_runtime = self.root / "host-runtime"
        overlay = self.root / "overlay"
        staged = self.root / "staged"
        (runtime / "lib/wine/i386-windows").mkdir(parents=True)
        (runtime / "lib/wine/x86_64-windows").mkdir(parents=True)
        (runtime / "share/wine").mkdir(parents=True)
        (overlay / "lib/wine/i386-unix").mkdir(parents=True)
        staged.mkdir()
        with self.assertRaisesRegex(SystemExit, "already exists"):
            PREPARE.stage_runtime(runtime, host_runtime, overlay, staged)

    def test_build_commands_pin_legacy_split_wow64_inputs(self) -> None:
        command, environment = PREPARE.legacy_configure_command(
            Path("/wine"), Path("/wine64-build"), Path("/overlay"), "clang")
        self.assertEqual(command[0], "/wine/configure")
        self.assertIn("--with-wine64=/wine64-build", command)
        self.assertIn("--prefix=/overlay", command)
        self.assertEqual(environment["CC"], "clang -m32")
        guest = PREPARE.guest_command(
            Path("/dispatcher"), Path("/staged/bin/wine"),
            "wineboot.exe", "--init")
        self.assertEqual(guest, ["proot", "-q", "/dispatcher",
                                 "/staged/bin/wine", "wineboot.exe", "--init"])
        self.assertEqual(
            PREPARE.guest_command(None, Path("/staged/bin/wine"), "--version"),
            ["/staged/bin/wine", "--version"])

    def test_fixtures_exercise_both_architectures_and_32bit_networking(self) -> None:
        fixtures = PREPARE.bottle_fixtures()
        self.assertEqual(fixtures[0][0], r"C:\windows\system32\cmd.exe")
        self.assertEqual(fixtures[1][0], r"C:\windows\syswow64\cmd.exe")
        self.assertEqual(fixtures[2],
                         (r"C:\windows\syswow64\ipconfig.exe", "/all"))

    def test_loader_provenance_binds_source_patch_and_build_inputs(self) -> None:
        source = self.root / "wine-source"
        source.mkdir()
        (source / "VERSION").write_text("Wine version 11.17\n", encoding="utf-8")
        subprocess.run(["git", "init", "--quiet", str(source)], check=True)
        subprocess.run(["git", "-C", str(source), "add", "VERSION"], check=True)
        subprocess.run([
            "git", "-C", str(source), "-c", "user.name=test",
            "-c", "user.email=test.invalid", "-c", "commit.gpgsign=false",
            "commit", "--quiet", "-m", "source",
        ], check=True)
        head = subprocess.run(
            ["git", "-C", str(source), "rev-parse", "HEAD"], check=True,
            capture_output=True, text=True).stdout.strip()
        (source / "VERSION").write_text("Wine version 11.17 patched\n",
                                         encoding="utf-8")
        wine64 = self.root / "wine64"
        wine64.mkdir()
        (wine64 / "config.log").write_text("configured\n", encoding="utf-8")
        overlay = self.root / "overlay"
        unix = overlay / "lib/wine/i386-unix"
        unix.mkdir(parents=True)
        (unix / "wine").write_bytes(b"loader")
        provenance = self.root / "provenance.json"
        source_binding = self.root / "source-binding.json"
        diff = subprocess.run(
            ["git", "-C", str(source), "diff", "HEAD", "--binary"],
            check=True, capture_output=True).stdout
        staged_diff = subprocess.run(
            ["git", "-C", str(source), "diff", "--cached", "--binary"],
            check=True, capture_output=True).stdout
        unstaged_diff = subprocess.run(
            ["git", "-C", str(source), "diff", "--binary"],
            check=True, capture_output=True).stdout
        status = subprocess.run(
            ["git", "-C", str(source), "status", "--porcelain"],
            check=True, capture_output=True).stdout
        source_binding.write_text(json.dumps({
            "schema": "prospero-win-wine-source-binding/1",
            "retained_runtime_commit":
                "e16ec8f3a19dec4a1162e7554ab73ce41f83bdb9",
            "wine_source_head": head,
            "wine_source_diff_sha256": hashlib.sha256(diff).hexdigest(),
            "wine_source_staged_diff_sha256": hashlib.sha256(staged_diff).hexdigest(),
            "wine_source_unstaged_diff_sha256": hashlib.sha256(unstaged_diff).hexdigest(),
            "wine_source_status_sha256": hashlib.sha256(status).hexdigest(),
            "wine_source_changed_files": {
                "VERSION": PREPARE.file_sha256(source / "VERSION")},
            "wine64_config_log_sha256": PREPARE.file_sha256(
                wine64 / "config.log"),
        }), encoding="utf-8")
        PREPARE.write_loader_provenance(
            provenance, source, wine64, overlay,
            ["/wine/configure", "--with-wine64=/wine64"], "gcc -m32",
            "e16ec8f3a19dec4a1162e7554ab73ce41f83bdb9", source_binding)
        document = json.loads(provenance.read_text(encoding="utf-8"))
        self.assertEqual(document["wine_source_head"], head)
        self.assertTrue(document["wine_source_dirty"])
        self.assertEqual(len(document["wine_source_diff_sha256"]), 64)
        self.assertEqual(document["retained_runtime_commit"],
                         "e16ec8f3a19dec4a1162e7554ab73ce41f83bdb9")
        self.assertEqual(document["outputs"]["lib/wine/i386-unix/wine"],
                         hashlib.sha256(b"loader").hexdigest())
        self.assertEqual(document["source_binding_sha256"],
                         PREPARE.file_sha256(source_binding))
        PREPARE.validate_loader_provenance(provenance, overlay)
        (unix / "wine").write_bytes(b"drifted-loader")
        with self.assertRaisesRegex(SystemExit, "differs from loader provenance"):
            PREPARE.validate_loader_provenance(provenance, overlay)
        (source / "untracked-input.c").write_text("input\n", encoding="utf-8")
        with self.assertRaisesRegex(SystemExit, "untracked inputs"):
            PREPARE.validate_source_binding(
                source_binding, source, wine64,
                "e16ec8f3a19dec4a1162e7554ab73ce41f83bdb9")

    def test_rejects_material_initialization_diagnostics(self) -> None:
        PREPARE.reject_material_diagnostics("ordinary optional warning")
        for diagnostic in (
            "assertion failed [xstate]", "load_wow64_ntdll then crash",
            'loader_init "ws2_32.dll" failed to initialize',
            "Got unexpected trap -1",
        ):
            with self.subTest(diagnostic=diagnostic):
                with self.assertRaisesRegex(SystemExit, "material Wine diagnostic"):
                    PREPARE.reject_material_diagnostics(diagnostic)

    def test_stage_rejects_missing_host_launchers(self) -> None:
        runtime = self.root / "runtime"
        for relative in PREPARE.RETAINED_TREES:
            (runtime / relative).mkdir(parents=True)
        overlay = self.root / "overlay/lib/wine/i386-unix"
        overlay.mkdir(parents=True)
        (overlay / "wine").write_bytes(b"loader")
        (overlay / "ntdll.so").write_bytes(b"ntdll")
        with self.assertRaisesRegex(SystemExit, "host runtime lacks launcher"):
            PREPARE.stage_runtime(
                runtime, self.root / "host", self.root / "overlay",
                self.root / "staged")

    def test_logged_runner_preserves_failure_and_timeout_output(self) -> None:
        transcripts: list[str] = []
        with self.assertRaisesRegex(SystemExit, r"command failed \(7\)"):
            PREPARE.run_logged(
                [sys.executable, "-c", "print('decisive fault');raise SystemExit(7)"],
                transcripts, label="failure", timeout=2, environment=os.environ.copy())
        self.assertIn("decisive fault", "".join(transcripts))
        transcripts.clear()
        with self.assertRaisesRegex(SystemExit, "timed out"):
            PREPARE.run_logged(
                [sys.executable, "-c",
                 "import time;print('before timeout',flush=True);time.sleep(2)"],
                transcripts, label="timeout", timeout=0.05,
                environment=os.environ.copy())
        self.assertIn("before timeout", "".join(transcripts))

    def test_init_failure_is_logged_and_stops_exact_wineserver_before_mutation(self) -> None:
        arguments = SimpleNamespace(
            prefix=self.root / "prefix", runtime=self.root / "runtime",
            host_runtime=self.root / "host", overlay=self.root / "overlay",
            staged_runtime=self.root / "staged", user="prospero", display=":97",
            log=self.root / "init.log", xvfb_server="Xvfb",
            xvfb_ready_delay=0, timeout=2, shutdown_timeout=2,
            dispatcher=None, host_root=self.root, manifest=self.root / "manifest")
        arguments.loader_provenance = self.root / "loader-provenance.json"
        arguments.initialization_provenance = self.root / "init-provenance.json"
        (arguments.staged_runtime / "bin").mkdir(parents=True)
        labels: list[str] = []

        def run(command, transcripts, *, label, timeout, environment,
                allowed_returncodes=(0,)):
            labels.append(label)
            if label == "wineboot --init":
                transcripts.append("decisive init fault\n")
                raise PREPARE.Failure("init failed")
            return subprocess.CompletedProcess(command, 0, "")

        xvfb = mock.Mock()
        xvfb.poll.return_value = None
        with mock.patch.object(PREPARE, "retained_inventory", return_value={}), \
                mock.patch.object(PREPARE, "stage_runtime"), \
                mock.patch.object(PREPARE, "validate_loader_provenance",
                                  return_value={"source_binding_sha256": "binding"}), \
                mock.patch.object(PREPARE, "run_logged", side_effect=run), \
                mock.patch.object(PREPARE, "exact_prefix_processes", return_value=[]), \
                mock.patch.object(PREPARE.subprocess, "Popen", return_value=xvfb), \
                mock.patch.object(PREPARE, "pin_registered_fonts") as pin:
            with self.assertRaisesRegex(SystemExit, "init failed"):
                PREPARE.init_bottle(arguments)
        self.assertEqual(labels,
                         ["wineboot --init", "wineserver -k", "wineserver -w"])
        pin.assert_not_called()
        self.assertIn("decisive init fault", arguments.log.read_text())

    def test_pins_registered_fonts_inside_the_bottle(self) -> None:
        prefix = self.root / "prefix"
        fonts = prefix / "drive_c/windows/Fonts"
        fonts.mkdir(parents=True)
        runtime = self.root / "runtime"
        wine_fonts = runtime / "share/wine/fonts"
        wine_fonts.mkdir(parents=True)
        wine_font = wine_fonts / "tahoma.ttf"
        wine_font.write_bytes(b"wine-font")
        host_root = self.root / "host"
        host_font = host_root / "usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
        host_font.parent.mkdir(parents=True)
        host_font.write_bytes(b"dejavu-font")
        wine_path = "Z:" + str(wine_font).replace("/", "\\\\")
        host_path = r"Z:\\usr\\share\\fonts\\truetype\\dejavu\\DejaVuSans.ttf"
        nt_host_path = r"\\??\\unix\\usr\\share\\fonts\\truetype\\dejavu\\DejaVuSans.ttf"
        for index, registry in enumerate(("system.reg", "user.reg")):
            (prefix / registry).write_text(
                f'"Tahoma (TrueType)"="{wine_path}"\n'
                f'"DejaVu Sans (TrueType)"="{host_path if index else nt_host_path}"\n',
                encoding="utf-8")
        result = PREPARE.pin_registered_fonts(prefix, runtime, host_root)
        self.assertEqual(result, ["DejaVuSans.ttf", "tahoma.ttf"])
        self.assertEqual((fonts / "tahoma.ttf").read_bytes(), b"wine-font")
        self.assertEqual((fonts / "DejaVuSans.ttf").read_bytes(), b"dejavu-font")
        for registry in ("system.reg", "user.reg"):
            text = (prefix / registry).read_text(encoding="utf-8")
            self.assertNotIn('="Z:', text)
            self.assertIn(r'="C:\\windows\\Fonts\\tahoma.ttf"', text)

    def test_manifest_is_stable_and_detects_byte_drift(self) -> None:
        prefix = self.make_prefix()
        manifest = self.root / "prefix.sha256"
        digest = PREPARE.write_manifest(prefix, manifest)
        expected = hashlib.sha256(manifest.read_bytes()).hexdigest()
        self.assertEqual(digest, expected)
        PREPARE.verify_manifest(prefix, manifest)
        (prefix / "user.reg").write_text("changed", encoding="utf-8")
        with self.assertRaisesRegex(SystemExit, "hash drift"):
            PREPARE.verify_manifest(prefix, manifest)


if __name__ == "__main__":
    unittest.main()
