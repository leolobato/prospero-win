#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline Playden kit packaging and validation contract."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.package_playden_kit import (
    KitError,
    REQUIRED_HELPER_SHA256,
    REQUIRED_PROVENANCE,
    package_kit,
    validate_kit,
)


WRAPPER_REVISION = "54caac176e86c60c7c407972583b491f67838ed9"
WINE_REVISION = "70bfc92b423b3f6ca82b863223239d1d9dbca4a6"
HELPER_REVISION = "a8f391b269aa2394f291cd1e670afd4744e35590"
RULE_HELPER_REVISION = "66ab4f10f9a6c6a5a42d05f1d165230c13ae2ab7"
TITLE_RULE = {"prefix": "FAKE78", "allow_list": "/data/playden/title-rule/allow-list"}
HELPER_BASE_REVISION = "b48b7d7236eca25c7b9dfd6c040c763ae05dde80"
FOUNDATION_REVISION = "9c0b994a048521af6fb84c73ded364504fe250e9"
DXVK_DLLS = ("d3d8", "d3d9", "d3d10core", "d3d11", "dxgi")
CORE_DLLS = ("ntdll", "kernel32", "kernelbase", "mmdevapi", "dinput8", "rsaenh")
NATIVE_PRXS = ("ntdll", "wineserver", "win32u", "wineps5", "libvulkan", "wowprospero")


def write(path: Path, data: bytes | str = b"x", mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, str):
        path.write_text(data, encoding="utf-8")
    else:
        path.write_bytes(data)
    path.chmod(mode)


def pe(architecture: str, payload: bytes) -> bytes:
    data = bytearray(0x100)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, 0x14C if architecture == "pe32" else 0x8664)
    struct.pack_into("<H", data, 0x94, 0x60)
    struct.pack_into("<H", data, 0x98, 0x10B if architecture == "pe32" else 0x20B)
    return bytes(data) + payload


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Fixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.runtime = root / "runtime"
        self.title = root / "title"
        self.prefix = root / "prefix"
        self.recipe = root / "recipe.json"
        self.protocol = root / "lapy_elevation_protocol.h"
        self.helper_manifest = root / "lapy-manifest.json"
        self.runtime_inventory = root / "runtime-inventory.json"
        self.spec = root / "kit-spec.json"
        self.out = root / "kit"
        self.make()

    def make(self) -> None:
        for architecture, folder in (("pe32", "i386-windows"),
                                     ("pe64", "x86_64-windows")):
            for name in CORE_DLLS:
                write(self.runtime / f"lib/wine/{folder}/{name}.dll",
                      pe(architecture, f"{folder}/{name}".encode()))
        write(self.runtime / "lib/wine/x86_64-windows/wowprospero.dll",
              pe("pe64", b"wowprospero"))
        for name in NATIVE_PRXS:
            write(self.runtime / f"lib/wine/x86_64-unix/{name}.prx", b"prx", 0o755)
        self.runtime_inventory.write_text(json.dumps({
            path.relative_to(self.runtime).as_posix(): digest(path)
            for path in sorted(self.runtime.rglob("*")) if path.is_file()
        }))

        write(self.title / "eboot.bin", b"wrapper", 0o755)
        write(self.title / "lapy.elf", b"\x7fELFhelper", 0o755)
        write(self.title / "sce_module/libc.prx", b"libc", 0o755)
        write(self.title / "sce_sys/param.json", json.dumps({"titleId": "FAKE78001"}))
        write(self.title / "native-title-build.json", json.dumps({
            "schema": "prospero-win-native-title/1",
            "title_id": "FAKE78001",
            "source_revision": WRAPPER_REVISION,
            "source_dirty": False,
            "eboot_sha256": digest(self.title / "eboot.bin"),
        }))
        write(self.protocol, b"protocol-v1")
        write(self.helper_manifest, json.dumps({
            "mode": "elf-helper",
            "target_title": "FAKE78001",
            "elf_sha256": digest(self.title / "lapy.elf"),
            "protocol_sha256": digest(self.protocol),
            "features": ["root_layout_probe_retry"],
        }))

        system_reg = r'''WINE REGISTRY Version 2

[Software\\Microsoft\\Wow64\\x86]
@="wow64cpu.dll"

[Software\\Microsoft\\Cryptography\\Defaults\\Provider Types\\Type 001]
"Name"="Microsoft Base Cryptographic Provider v1.0"

[Software\\Microsoft\\Cryptography\\Defaults\\Provider\\Microsoft Base Cryptographic Provider v1.0]
"Image Path"="C:\\windows\\system32\\rsaenh.dll"
"Type"=dword:00000001

[Software\\Wow6432Node\\Microsoft\\Cryptography\\Defaults\\Provider Types\\Type 001]
"Name"="Microsoft Base Cryptographic Provider v1.0"
"TypeName"="RSA Full (Signature and Key Exchange)"

[Software\\Wow6432Node\\Microsoft\\Cryptography\\Defaults\\Provider\\Microsoft Base Cryptographic Provider v1.0]
"Image Path"="C:\\windows\\system32\\rsaenh.dll"
"Type"=dword:00000001

[Software\\Classes\\Wow6432Node\\CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}]
@="MMDeviceEnumerator"
[Software\\Classes\\Wow6432Node\\CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}\\InprocServer32]
@="C:\\windows\\system32\\mmdevapi.dll"
"ThreadingModel"="Both"

[Software\\Classes\\Wow6432Node\\CLSID\\{25E609E4-B259-11CF-BFC7-444553540000}]
@="DirectInput8"
[Software\\Classes\\Wow6432Node\\CLSID\\{25E609E4-B259-11CF-BFC7-444553540000}\\InprocServer32]
@="C:\\windows\\system32\\dinput8.dll"
"ThreadingModel"="Both"

'''
        write(self.prefix / "system.reg", system_reg)
        write(self.prefix / "user.reg", r'''WINE REGISTRY Version 2

[Software\\Wine\\Drivers]
"Audio"="ps5"

[Volatile Environment]
"USERNAME"="prospero"
"USERPROFILE"="C:\\users\\prospero"
''')
        write(self.prefix / "userdef.reg", "WINE REGISTRY Version 2\n")
        write(self.prefix / "drive_c/windows/system32/wowprospero.dll",
              pe("pe64", b"cpu"))
        for folder in ("AppData/Local/Temp", "AppData/LocalLow", "AppData/Roaming",
                       "Desktop", "Documents", "Saved Games"):
            (self.prefix / "drive_c/users/prospero" / folder).mkdir(parents=True, exist_ok=True)
        dosdevices = self.prefix / "dosdevices"
        dosdevices.mkdir(parents=True)
        os.symlink("../drive_c", dosdevices / "c:")
        os.symlink("/", dosdevices / "z:")

        dxvk_hashes: dict[str, dict[str, str]] = {"x64": {}, "x32": {}}
        for architecture, folder, label in (
                ("pe64", "system32", "x64"), ("pe32", "syswow64", "x32")):
            for name in DXVK_DLLS:
                path = self.prefix / f"drive_c/windows/{folder}/{name}.dll"
                write(path, pe(architecture, f"{label}/{name}".encode()))
                dxvk_hashes[label][f"{name}.dll"] = digest(path)

        for name in CORE_DLLS:
            path = self.prefix / f"drive_c/windows/syswow64/{name}.dll"
            if not path.exists():
                write(path, pe("pe32", f"prefix/{name}".encode()))
            write(self.prefix / f"drive_c/windows/system32/{name}.dll",
                  pe("pe64", f"prefix64/{name}".encode()))
        write(self.recipe, json.dumps({
            "schema": 1, "id": "steam-220-hl2-v1", "version": "1",
            "requirements": {"audio": "ps5"},
        }))
        self.write_spec(dxvk_hashes)

    def write_spec(self, hashes: dict[str, dict[str, str]] | None = None) -> None:
        if hashes is None:
            hashes = json.loads(self.spec.read_text())["dxvk"]["files"]
        document = {
            "schema": "playden-prospero-kit-spec/1",
            "kit_id": "hl2-fw1360-v1",
            "firmware": ["13.60"],
            "architecture": "x86_64+wow64-i386",
            "runtime": {
                "path": str(self.runtime),
                "bundle_id": "prospero-win-wine-ps5",
                "artifact_revision": "e16ec8f3-ps5-r1",
                "wine_revision": WINE_REVISION,
                "inventory_manifest": str(self.runtime_inventory),
            },
            "template": {"path": str(self.prefix), "user": "prospero"},
            "recipe": {"id": "steam-220-hl2-v1", "version": "1", "path": str(self.recipe)},
            "variants": [{
                "id": "hl2-fa78001-v1",
                "title_id": "FAKE78001",
                "path": str(self.title),
                "wrapper_revision": WRAPPER_REVISION,
                "helper_revision": HELPER_REVISION,
                "helper_manifest": str(self.helper_manifest),
                "helper_protocol": str(self.protocol),
                "native_build_manifest": str(self.title / "native-title-build.json"),
            }],
            "dxvk": {"version": "2.6.2", "files": hashes},
            "sources": {
                "wrapper": {"revision": WRAPPER_REVISION, "baseline_revision": WRAPPER_REVISION,
                            "url": "https://example/wrapper"},
                "wine": {"revision": WINE_REVISION, "url": "https://example/wine"},
                "helper": {"revision": HELPER_REVISION, "base_revision": HELPER_BASE_REVISION,
                           "url": "https://example/helper"},
                "foundation": {"revision": FOUNDATION_REVISION,
                               "url": "https://example/foundation"},
                "sdk": {"version": "0.42", "url": "https://example/sdk"},
                "dxvk": {"version": "2.6.2", "url": "https://example/dxvk",
                         "archive_sha256": "17761876556afd55736cb895d184f5a1c55d43350f1b1e3b129f8d28706d7992"},
            },
            "capabilities": ["per_game_title", "offline_bottle", "wow64_i386", "dxvk"],
        }
        write(self.spec, json.dumps(document))


class PlaydenKitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.fixture = Fixture(Path(self.temporary.name))
        self.helper_pin = mock.patch.dict(
            REQUIRED_HELPER_SHA256,
            {"FAKE78001": digest(self.fixture.title / "lapy.elf")}, clear=True)
        self.helper_pin.start()

    def tearDown(self) -> None:
        self.helper_pin.stop()
        self.temporary.cleanup()

    def package(self) -> dict:
        return package_kit(self.fixture.spec, self.fixture.out)

    def make_any_title(self, *, capability: bool = True) -> None:
        """Turn the fixture's only variant into the one-shell-for-every-title kind."""
        fixture = self.fixture
        write(fixture.title / "sce_sys/param.json", json.dumps({"titleId": "FAKE78000"}))
        build = json.loads((fixture.title / "native-title-build.json").read_text())
        build.update(title_id="FAKE78000", any_title=True, title_rule=TITLE_RULE)
        write(fixture.title / "native-title-build.json", json.dumps(build))
        helper = json.loads(fixture.helper_manifest.read_text())
        helper.update(target_title="FAKE78*", title_rule=TITLE_RULE,
                      features=["root_layout_probe_retry", "title_rule"])
        write(fixture.helper_manifest, json.dumps(helper))
        spec = json.loads(fixture.spec.read_text())
        spec["variants"][0].update(id="any-title", title_id="FAKE78000", any_title=True,
                                   helper_revision=RULE_HELPER_REVISION)
        spec["sources"]["helper"]["revision"] = RULE_HELPER_REVISION
        if capability:
            spec["capabilities"].append("title-any-id")
        write(fixture.spec, json.dumps(spec))
        REQUIRED_HELPER_SHA256["FAKE78*"] = digest(fixture.title / "lapy.elf")
        del REQUIRED_HELPER_SHA256["FAKE78001"]

    def test_packages_one_shell_for_every_title(self) -> None:
        self.make_any_title()
        manifest = self.package()
        self.assertEqual(validate_kit(self.fixture.out), manifest)
        self.assertEqual(manifest["variants"][0]["any_title"], True)
        self.assertEqual(manifest["variants"][0]["title_id"], "FAKE78000")
        self.assertIn("title-any-id", manifest["capabilities"])

    def test_fixed_variants_carry_no_any_title_field(self) -> None:
        manifest = self.package()
        self.assertNotIn("any_title", manifest["variants"][0])

    def add_mono_runtime_path(self) -> None:
        user = self.fixture.prefix / "user.reg"
        write(user, user.read_text() + "\n[Software\\\\Wine\\\\Mono]\n"
              '"RuntimePath"="Z:\\\\data\\\\playden\\\\kits\\\\hl2-fw1360-v1\\\\mono\\\\wine-mono-11.3.0"\n')

    def add_wine_mono(self, *, capability: bool = True, registry: bool = True) -> Path:
        """Give the fixture Wine Mono, its provenance, and the template's RuntimePath."""
        mono = self.fixture.root / "inputs/wine-mono-11.3.0"
        write(mono / "bin/libmono-2.0-x86.dll", pe("pe32", b"mono32"))
        write(mono / "bin/libmono-2.0-x86_64.dll", pe("pe64", b"mono64"))
        write(mono / "lib/mono/4.5/mscorlib.dll", pe("pe32", b"corlib"))
        if registry:
            self.add_mono_runtime_path()
        spec = json.loads(self.fixture.spec.read_text())
        spec["mono"] = {"version": "11.3.0", "path": str(mono)}
        spec["sources"]["mono"] = {"version": "11.3.0", "url": "https://example/wine-mono",
                                   "archive_sha256": REQUIRED_PROVENANCE["mono"]["archive_sha256"]}
        if capability:
            spec["capabilities"].append("wine-mono")
        write(self.fixture.spec, json.dumps(spec))
        return mono

    def test_packages_wine_mono_once_at_the_kit_root(self) -> None:
        self.add_wine_mono()
        manifest = self.package()
        self.assertEqual(validate_kit(self.fixture.out), manifest)
        self.assertEqual(manifest["mono"], {"version": "11.3.0", "path": "mono/wine-mono-11.3.0"})
        self.assertIn("wine-mono", manifest["capabilities"])
        paths = [entry["path"] for entry in manifest["files"]]
        self.assertIn("mono/wine-mono-11.3.0/bin/libmono-2.0-x86.dll", paths)
        self.assertFalse(any(path.startswith("template/") and "mono-2.0" in path for path in paths))

    def test_a_kit_without_wine_mono_names_none(self) -> None:
        manifest = self.package()
        self.assertNotIn("mono", manifest)
        self.assertNotIn("wine-mono", manifest["capabilities"])

    def test_wine_mono_capability_provenance_and_files_go_together(self) -> None:
        self.add_wine_mono(capability=False)
        with self.assertRaisesRegex(KitError, "wine-mono"):
            self.package()
        spec = json.loads(self.fixture.spec.read_text())
        spec["capabilities"].append("wine-mono")
        del spec["mono"]
        write(self.fixture.spec, json.dumps(spec))
        with self.assertRaisesRegex(KitError, "needs Wine Mono 11.3.0"):
            self.package()
        spec["mono"] = {"version": "11.3.0", "path": str(self.fixture.root / "inputs/wine-mono-11.3.0")}
        spec["sources"]["mono"]["archive_sha256"] = "0" * 64
        write(self.fixture.spec, json.dumps(spec))
        with self.assertRaisesRegex(KitError, "pin Wine Mono"):
            self.package()
        self.assertFalse(self.fixture.out.exists())

    def test_wine_mono_needs_both_architectures_and_the_template_runtime_path(self) -> None:
        mono = self.add_wine_mono(registry=False)
        with self.assertRaisesRegex(KitError, "RuntimePath"):
            self.package()
        self.add_mono_runtime_path()
        write(mono / "bin/libmono-2.0-x86_64.dll", pe("pe32", b"wrong"))
        with self.assertRaisesRegex(KitError, r"libmono-2.0-x86_64.dll must be PE32\+"):
            self.package()
        (mono / "bin/libmono-2.0-x86_64.dll").unlink()
        os.symlink("libmono-2.0-x86.dll", mono / "bin/libmono-2.0-x86_64.dll")
        with self.assertRaisesRegex(KitError, "missing bin/libmono-2.0-x86_64.dll"):
            self.package()
        self.assertFalse(self.fixture.out.exists())

    def test_standalone_validator_checks_wine_mono(self) -> None:
        self.add_wine_mono()
        self.package()
        manifest_path = self.fixture.out / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["capabilities"].remove("wine-mono")
        del manifest["sources"]["mono"]
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(KitError, "wine-mono"):
            validate_kit(self.fixture.out)

    def test_any_title_needs_its_capability_and_the_capability_needs_it(self) -> None:
        self.make_any_title(capability=False)
        with self.assertRaisesRegex(KitError, "title-any-id"):
            self.package()
        spec = json.loads(self.fixture.spec.read_text())
        spec["capabilities"].append("title-any-id")
        spec["variants"][0]["any_title"] = "yes"
        write(self.fixture.spec, json.dumps(spec))
        with self.assertRaisesRegex(KitError, "any_title must be true or false"):
            self.package()
        self.assertFalse(self.fixture.out.exists())

    def test_capability_without_an_any_title_variant_is_refused(self) -> None:
        spec = json.loads(self.fixture.spec.read_text())
        spec["capabilities"].append("title-any-id")
        write(self.fixture.spec, json.dumps(spec))
        with self.assertRaisesRegex(KitError, "title-any-id"):
            self.package()

    def test_any_title_needs_a_rule_helper_and_a_matching_shell(self) -> None:
        self.make_any_title()
        build_path = self.fixture.title / "native-title-build.json"
        original = build_path.read_text()
        for change in ({"any_title": False}, {"title_rule": {**TITLE_RULE, "prefix": "FAKE7"}}):
            with self.subTest(change=change):
                write(build_path, json.dumps({**json.loads(original), **change}))
                with self.assertRaisesRegex(KitError, "any-title shell"):
                    self.package()
        write(build_path, original)
        # An exact helper can't serve an any-title variant.
        helper = json.loads(self.fixture.helper_manifest.read_text())
        write(self.fixture.helper_manifest, json.dumps({**helper, "title_rule": None}))
        with self.assertRaisesRegex(KitError, "any-title shell"):
            self.package()
        write(self.fixture.helper_manifest, json.dumps(helper))
        # The rule helper's pin is its own.
        REQUIRED_HELPER_SHA256["FAKE78*"] = "0" * 64
        with self.assertRaisesRegex(KitError, "pinned helper digest"):
            self.package()
        self.assertFalse(self.fixture.out.exists())

    def test_an_any_title_shell_is_refused_as_a_fixed_variant(self) -> None:
        build_path = self.fixture.title / "native-title-build.json"
        write(build_path, json.dumps({**json.loads(build_path.read_text()), "any_title": True}))
        with self.assertRaisesRegex(KitError, "not marked any_title"):
            self.package()

    def test_standalone_validator_checks_the_any_title_variant(self) -> None:
        self.make_any_title()
        self.package()
        manifest_path = self.fixture.out / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["capabilities"].remove("title-any-id")
        manifest_path.chmod(0o644)
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(KitError, "title-any-id"):
            validate_kit(self.fixture.out)

    def test_playden_recipe_identity_is_supported(self) -> None:
        self.fixture.recipe.write_text(json.dumps({
            "recipe_id": "steam-220-hl2-v1", "recipe_version": 1,
            "requirements": {"audio": "ps5"},
        }))
        manifest = self.package()
        self.assertEqual(validate_kit(self.fixture.out), manifest)

    def test_recipe_identity_aliases_must_be_complete_and_agree(self) -> None:
        for document in (
                {"recipe_id": "steam-220-hl2-v1", "recipe_version": 1, "id": "different", "version": "1"},
                {"recipe_id": "steam-220-hl2-v1", "recipe_version": 1, "version": "1"},
                {"recipe_id": "steam-220-hl2-v1", "recipe_version": True},
                {"recipe_id": "steam-220-hl2-v1", "recipe_version": 1.0}):
            with self.subTest(document=document):
                self.fixture.recipe.write_text(json.dumps(document))
                with self.assertRaisesRegex(KitError, "recipe identity"):
                    self.package()
                self.assertFalse(self.fixture.out.exists())

    def test_matching_recipe_aliases_are_supported(self) -> None:
        self.fixture.recipe.write_text(json.dumps({
            "recipe_id": "steam-220-hl2-v1", "recipe_version": 1,
            "id": "steam-220-hl2-v1", "version": "1",
        }))
        manifest = self.package()
        self.assertEqual(validate_kit(self.fixture.out), manifest)

    def test_packages_and_validates_complete_offline_kit(self) -> None:
        manifest = self.package()
        validated = validate_kit(self.fixture.out)

        self.assertEqual(validated, manifest)
        self.assertEqual(manifest["schema"], "playden-prospero-kit/1")
        self.assertEqual(manifest["runtime"]["wine_revision"], WINE_REVISION)
        self.assertEqual(manifest["runtime"]["bundle_id"], "prospero-win-wine-ps5")
        self.assertEqual(manifest["runtime"]["module_counts"], {
            "i386-windows": 6, "x86_64-windows": 7, "x86_64-unix": 6,
        })
        self.assertNotEqual(manifest["runtime"]["artifact_revision"], WRAPPER_REVISION)
        self.assertEqual(manifest["recipe"]["version"], "1")
        self.assertEqual(REQUIRED_PROVENANCE["wine"]["revision"], WINE_REVISION)
        self.assertEqual(manifest["variants"][0]["title_id"], "FAKE78001")
        self.assertEqual(manifest["template"]["user"], "prospero")
        paths = [entry["path"] for entry in manifest["files"]]
        self.assertEqual(paths, sorted(paths))
        self.assertIn("template/bottle/dosdevices/.pw-symlinks", paths)
        self.assertTrue(all(set(entry) == {"path", "size", "sha256", "mode"}
                            for entry in manifest["files"]))
        serialized = json.dumps(manifest)
        self.assertNotIn(str(self.fixture.root), serialized)
        self.assertFalse(any(path.is_symlink() for path in self.fixture.out.rglob("*")))

    def test_rejects_incomplete_wow64_and_wrong_cpu_registry_view(self) -> None:
        saved_games = self.fixture.prefix / "drive_c/users/prospero/Saved Games"
        saved_games.rmdir()
        with self.assertRaisesRegex(KitError, "Saved Games"):
            self.package()
        saved_games.mkdir()

        syswow64 = self.fixture.prefix / "drive_c/windows/syswow64"
        moved = self.fixture.root / "syswow64.saved"
        syswow64.rename(moved)
        syswow64.mkdir()
        with self.assertRaisesRegex(KitError, "syswow64"):
            self.package()
        syswow64.rmdir()
        moved.rename(syswow64)

        cpu = self.fixture.prefix / "drive_c/windows/system32/wowprospero.dll"
        cpu.unlink()
        with self.assertRaisesRegex(KitError, "wowprospero"):
            self.package()
        write(cpu, pe("pe64", b"cpu"))

        write(cpu, pe("pe32", b"wrong cpu architecture"))
        with self.assertRaisesRegex(KitError, "wowprospero.*pe64"):
            self.package()
        write(cpu, pe("pe64", b"cpu"))

        rsaenh = self.fixture.prefix / "drive_c/windows/syswow64/rsaenh.dll"
        rsaenh.unlink()
        with self.assertRaisesRegex(KitError, "syswow64.*rsaenh.dll"):
            self.package()
        write(rsaenh, pe("pe32", b"prefix/rsaenh"))

        registry = self.fixture.prefix / "system.reg"
        registry.write_bytes(registry.read_bytes().replace(b'wow64cpu.dll', b'wrongcpu.dll'))
        with self.assertRaisesRegex(KitError, "wowprospero"):
            self.package()

    def test_rejects_missing_64_bit_core_module(self) -> None:
        required = self.fixture.prefix / "drive_c/windows/system32/kernelbase.dll"
        required.unlink()
        with self.assertRaisesRegex(KitError, "system32.*kernelbase.dll"):
            self.package()

    def test_preserves_only_known_wine_16bit_placeholders(self) -> None:
        placeholder = bytearray(256)
        placeholder[:2] = b"MZ"
        placeholder[0x3c:0x40] = (0x60).to_bytes(4, "little")
        marker = b"Wine placeholder DLL\x00"
        placeholder[0x40:0x40 + len(marker)] = marker
        placeholder[0x60:0x62] = b"NE"
        folder = self.fixture.prefix / "drive_c/windows/syswow64"
        module = folder / "msvideo.dll"
        write(module, bytes(placeholder))
        self.package()
        module.unlink()
        for name, payload in (("kernel32.dll", bytes(placeholder)),
                              ("unknown.dll", bytes(placeholder)),
                              ("msvideo.dll", b"MZ" + bytes(254))):
            target = folder / name
            original = target.read_bytes() if target.exists() else None
            write(target, payload)
            with self.assertRaisesRegex(KitError, "syswow64 module"):
                self.package()
            if original is None:
                target.unlink()
            else:
                write(target, original)

    def test_registry_bindings_require_exact_values(self) -> None:
        registry = self.fixture.prefix / "system.reg"
        original = registry.read_text()
        cases = (
            ('@="C:\\\\windows\\\\system32\\\\mmdevapi.dll"',
             '@="C:\\\\windows\\\\system32\\\\wrong.dll"\n'
             '"Decoy"="C:\\\\windows\\\\system32\\\\mmdevapi.dll"',
             "MMDeviceEnumerator"),
            ('"Image Path"="C:\\\\windows\\\\system32\\\\rsaenh.dll"',
             '"Image Path"="C:\\\\windows\\\\system32\\\\wrong.dll"\n'
             '"Decoy"="C:\\\\windows\\\\system32\\\\rsaenh.dll"',
             "cryptography"),
        )
        for expected, replacement, message in cases:
            registry.write_text(original.replace(expected, replacement), encoding="utf-8")
            with self.assertRaisesRegex(KitError, message):
                self.package()
        registry.write_text(original, encoding="utf-8")

    def test_validates_the_selected_rsa_provider(self) -> None:
        registry = self.fixture.prefix / "system.reg"
        registry.write_text(registry.read_text().replace(
            "Microsoft Base Cryptographic Provider v1.0",
            "Microsoft Enhanced Cryptographic Provider v1.0"))
        self.package()

    def test_rejects_repeated_required_registry_sections(self) -> None:
        cases = (
            ("system.reg", r"Software\\Microsoft\\Wow64\\x86", '@="wrongcpu.dll"'),
            ("user.reg", r"Software\\Wine\\Drivers", '"Audio"="none"'),
            ("system.reg", r"Software\\Microsoft\\Cryptography\\Defaults\\Provider\\Microsoft Base Cryptographic Provider v1.0", '"Type"=dword:00000002'),
            ("system.reg", r"Software\\Classes\\Wow6432Node\\CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}\\InprocServer32", '"ThreadingModel"="Apartment"'),
        )
        for filename, section, assignment in cases:
            path = self.fixture.prefix / filename
            original = path.read_text()
            for name in (section, section.lower()):
                path.write_text(original + f"\n[{name}]\n{assignment}\n")
                with self.assertRaisesRegex(KitError, "duplicate registry section"):
                    self.package()
            path.write_text(original)

    def test_rejects_malformed_required_registry_headers(self) -> None:
        cases = (
            ("system.reg", r"Software\\Microsoft\\Wow64\\x86", "wowprospero"),
            ("user.reg", r"Software\\Wine\\Drivers", "Audio"),
            ("system.reg", r"Software\\Microsoft\\Cryptography\\Defaults\\Provider Types\\Type 001", "cryptography"),
        )
        for filename, section, _message in cases:
            path = self.fixture.prefix / filename
            original = path.read_text()
            header = f"[{section}]"
            for suffix in ("suffix", " 1791225270suffix", " -1"):
                path.write_text(original.replace(header, header + suffix))
                with self.assertRaisesRegex(KitError, "malformed registry section"):
                    self.package()
                # Wine still loads this key even when its timestamp is invalid.
                # A later malformed duplicate must not bypass uniqueness checks.
                path.write_text(original + f"\n{header}{suffix}\n@=\"wrong\"\n")
                with self.assertRaisesRegex(KitError, "duplicate registry section"):
                    self.package()
            path.write_text(original)

    def test_accepts_wine_timestamped_registry_headers(self) -> None:
        for filename in ("system.reg", "user.reg"):
            path = self.fixture.prefix / filename
            path.write_text("\n".join(
                line + " 1791225270" if line.startswith("[") else line
                for line in path.read_text().splitlines()) + "\n")
        self.package()

    def test_registry_rejects_contradictory_effective_values(self) -> None:
        system_registry = self.fixture.prefix / "system.reg"
        original_system = system_registry.read_text()
        system_cases = (
            ('@="wow64cpu.dll"',
             '@="wow64cpu.dll"\n@="wrongcpu.dll"', "wowprospero"),
            ('"Name"="Microsoft Base Cryptographic Provider v1.0"',
             '"Name"="Microsoft Base Cryptographic Provider v1.0"\n'
             '"Name"="Wrong Provider"', "cryptography"),
            ('"Name"="Microsoft Base Cryptographic Provider v1.0"',
             '"Name"="Microsoft Base Cryptographic Provider v1.0"\n'
             '"Name"=dword:00000000', "cryptography"),
            ('"Type"=dword:00000001',
             '"Type"=dword:00000001\n"Type"=dword:00000002', "cryptography"),
            ('"Type"=dword:00000001',
             '"Type"=dword:00000001\n"Type"="one"', "cryptography"),
            ('"ThreadingModel"="Both"',
             '"ThreadingModel"="Both"\n"ThreadingModel"="Apartment"',
             "MMDeviceEnumerator"),
            ('"ThreadingModel"="Both"',
             '"ThreadingModel"="Both"\n"ThreadingModel"=dword:00000000',
             "MMDeviceEnumerator"),
        )
        for expected, replacement, message in system_cases:
            with self.subTest(value=expected):
                system_registry.write_text(
                    original_system.replace(expected, replacement), encoding="utf-8")
                with self.assertRaisesRegex(KitError, message):
                    self.package()
        system_registry.write_text(original_system, encoding="utf-8")

        user_registry = self.fixture.prefix / "user.reg"
        original_user = user_registry.read_text()
        user_registry.write_text(original_user.replace(
            '"Audio"="ps5"', '"Audio"="ps5"\n"Audio"="none"'), encoding="utf-8")
        with self.assertRaisesRegex(KitError, "Audio.*ps5"):
            self.package()

    def test_rejects_incomplete_runtime_inventory_and_wrong_architectures(self) -> None:
        required = self.fixture.runtime / "lib/wine/i386-windows/mmdevapi.dll"
        required.unlink()
        with self.assertRaisesRegex(KitError, "inventory|mmdevapi"):
            self.package()

        write(required, pe("pe64", b"wrong"))
        inventory = {
            path.relative_to(self.fixture.runtime).as_posix(): digest(path)
            for path in sorted(self.fixture.runtime.rglob("*")) if path.is_file()
        }
        self.fixture.runtime_inventory.write_text(json.dumps(inventory))
        with self.assertRaisesRegex(KitError, "i386-windows/mmdevapi.dll.*pe32"):
            self.package()

        write(required, pe("pe32", b"fixed"))
        prx = self.fixture.runtime / "lib/wine/x86_64-unix/wineserver.prx"
        prx.unlink()
        self.fixture.runtime_inventory.write_text(json.dumps({
            path.relative_to(self.fixture.runtime).as_posix(): digest(path)
            for path in sorted(self.fixture.runtime.rglob("*")) if path.is_file()
        }))
        with self.assertRaisesRegex(KitError, "wineserver.prx"):
            self.package()

    def test_rejects_missing_32_bit_registry_repairs(self) -> None:
        registry = self.fixture.prefix / "system.reg"
        original = registry.read_text()
        cases = (
            ("Software\\\\Wow6432Node\\\\Microsoft\\\\Cryptography", "cryptography"),
            ("{BCDE0395-E52F-467C-8E3D-C4579291692E}", "MMDeviceEnumerator"),
            ("{25E609E4-B259-11CF-BFC7-444553540000}", "DirectInput8"),
        )
        for marker, message in cases:
            registry.write_text(original.replace(marker, "missing"), encoding="utf-8")
            with self.assertRaisesRegex(KitError, message):
                self.package()
        registry.write_text(original, encoding="utf-8")

    def test_rejects_missing_64_bit_crypto_or_recipe_audio_default(self) -> None:
        registry = self.fixture.prefix / "system.reg"
        original = registry.read_text()
        registry.write_text(original.replace(
            "Software\\\\Microsoft\\\\Cryptography\\\\Defaults\\\\Provider Types",
            "Software\\\\Missing64\\\\Provider Types"))
        with self.assertRaisesRegex(KitError, "64-bit cryptography"):
            self.package()

        registry.write_text(original)
        user_registry = self.fixture.prefix / "user.reg"
        user_registry.write_text(
            user_registry.read_text().replace('"Audio"="ps5"', '"Audio"="none"'))
        with self.assertRaisesRegex(KitError, "Audio.*ps5"):
            self.package()

    def test_rejects_native_title_build_manifest_drift(self) -> None:
        build_manifest = self.fixture.title / "native-title-build.json"
        original = json.loads(build_manifest.read_text())
        build_manifest.unlink()
        with self.assertRaisesRegex(KitError, "native build manifest.*missing"):
            self.package()
        build_manifest.write_text(json.dumps(original))
        cases = (
            ({**original, "source_dirty": True}, "dirty"),
            ({**original, "source_revision": "unknown"}, "source_revision"),
            ({**original, "source_revision": "0" * 40}, "wrapper revision"),
            ({**original, "schema": "prospero-win-native-title/2"}, "schema"),
            ({**original, "title_id": "FAKE78002"}, "title"),
            ({**original, "eboot_sha256": "0" * 64}, "eboot"),
        )
        for document, message in cases:
            build_manifest.write_text(json.dumps(document))
            with self.assertRaisesRegex(KitError, message):
                self.package()

        build_manifest.write_text(json.dumps(original))
        (self.fixture.title / "eboot.bin").write_bytes(b"mutated signed executable")
        with self.assertRaisesRegex(KitError, "eboot"):
            self.package()

    def test_publication_guard_claim_requires_compiled_variant_markers(self) -> None:
        spec = json.loads(self.fixture.spec.read_text())
        spec["capabilities"].append("playden-publication-guard-v1")
        self.fixture.spec.write_text(json.dumps(spec))
        build = self.fixture.title / "native-title-build.json"
        original = json.loads(build.read_text())
        qualified = {
            **original,
            "managed_publication_guard": "playden-publication-guard-v1",
            "managed_descriptor_required": True,
            "publication_provider_sha256": "f14d5e3ecb31951e3cf5bfdc41972181a9be1d96ed0dc7cb3aeebb8e4aab844a",
        }
        for change in (
                original,
                {**qualified, "managed_publication_guard": "other"},
                {**qualified, "managed_publication_guard": True},
                {**qualified, "managed_descriptor_required": False},
                {**qualified, "managed_descriptor_required": 1},
                {**qualified, "publication_provider_sha256": "0" * 64},
                {**qualified, "publication_provider_sha256": "F" * 64}):
            with self.subTest(change=change):
                build.write_text(json.dumps(change))
                with self.assertRaisesRegex(KitError, "publication guard"):
                    self.package()
                self.assertFalse(self.fixture.out.exists())
        build.write_text(json.dumps(qualified))
        manifest = self.package()
        self.assertIn("playden-publication-guard-v1", manifest["capabilities"])
        self.assertEqual(validate_kit(self.fixture.out), manifest)

    def test_standalone_validator_refuses_guard_claim_on_legacy_variant(self) -> None:
        self.package()
        path = self.fixture.out / "manifest.json"
        manifest = json.loads(path.read_text())
        manifest["capabilities"] = sorted(manifest["capabilities"] + ["playden-publication-guard-v1"])
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(KitError, "publication guard"):
            validate_kit(self.fixture.out)

    def test_rejects_wrong_dxvk_architecture_or_hash(self) -> None:
        dll = self.fixture.prefix / "drive_c/windows/syswow64/d3d9.dll"
        dll.write_bytes(pe("pe64", b"wrong architecture"))
        with self.assertRaisesRegex(KitError, "d3d9.dll.*pe32"):
            self.package()

        dll.write_bytes(pe("pe32", b"wrong hash"))
        with self.assertRaisesRegex(KitError, "DXVK.*hash"):
            self.package()

    def test_rejects_helper_mismatch_and_unavailable_title_id(self) -> None:
        manifest = json.loads(self.fixture.helper_manifest.read_text())
        manifest["target_title"] = "FAKE78002"
        self.fixture.helper_manifest.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(KitError, "helper.*title|title.*digest"):
            self.package()

        manifest["target_title"] = "FAKE78001"
        self.fixture.helper_manifest.write_text(json.dumps(manifest))
        spec = json.loads(self.fixture.spec.read_text())
        spec["variants"][0]["title_id"] = "PPSA99995"
        self.fixture.spec.write_text(json.dumps(spec))
        with self.assertRaisesRegex(KitError, "FAKE78000"):
            self.package()

    def test_rejects_recipe_identity_and_pinned_provenance_drift(self) -> None:
        recipe = json.loads(self.fixture.recipe.read_text())
        recipe["version"] = "2"
        self.fixture.recipe.write_text(json.dumps(recipe))
        with self.assertRaisesRegex(KitError, "recipe.*version"):
            self.package()

        recipe["version"] = "1"
        self.fixture.recipe.write_text(json.dumps(recipe))
        spec = json.loads(self.fixture.spec.read_text())
        spec["sources"]["wine"]["revision"] = "0" * 40
        self.fixture.spec.write_text(json.dumps(spec))
        with self.assertRaisesRegex(KitError, "wine.*pinned revision"):
            self.package()

        spec["sources"]["wine"]["revision"] = WINE_REVISION
        spec["sources"]["helper"]["base_revision"] = HELPER_BASE_REVISION[:7] + "0" * 33
        self.fixture.spec.write_text(json.dumps(spec))
        with self.assertRaisesRegex(KitError, "helper.*base revision"):
            self.package()

    def test_rejects_escaping_symlink_personal_state_and_missing_execute_mode(self) -> None:
        os.symlink("../../outside", self.fixture.prefix / "escape")
        with self.assertRaisesRegex(KitError, "symbolic link.*escapes"):
            self.package()
        (self.fixture.prefix / "escape").unlink()

        write(self.fixture.prefix / "drive_c/users/prospero/loginusers.vdf", "account")
        with self.assertRaisesRegex(KitError, "personal or account state"):
            self.package()
        (self.fixture.prefix / "drive_c/users/prospero/loginusers.vdf").unlink()

        write(self.fixture.title / "dev.conf", "DEV_SERVER=builder-workstation")
        with self.assertRaisesRegex(KitError, "personal or account state"):
            self.package()
        (self.fixture.title / "dev.conf").unlink()

        (self.fixture.title / "eboot.bin").chmod(0o644)
        with self.assertRaisesRegex(KitError, "executable permission"):
            self.package()

    def test_rejects_runtime_prx_without_source_execute_permission(self) -> None:
        module = self.fixture.runtime / "lib/wine/x86_64-unix/ntdll.prx"
        module.chmod(0o644)
        with self.assertRaisesRegex(KitError, "executable permission"):
            self.package()
        self.assertFalse(self.fixture.out.exists())
        self.assertEqual(module.stat().st_mode & 0o777, 0o644)

    def test_rejects_duplicate_firmware_at_package_and_standalone_validation(self) -> None:
        spec = json.loads(self.fixture.spec.read_text())
        spec["firmware"] = ["13.60", "13.60"]
        self.fixture.spec.write_text(json.dumps(spec))
        with self.assertRaisesRegex(KitError, "duplicate firmware"):
            self.package()
        self.assertFalse(self.fixture.out.exists())
        spec["firmware"] = ["13.60"]
        self.fixture.spec.write_text(json.dumps(spec))
        self.package()
        manifest = self.fixture.out / "manifest.json"
        document = json.loads(manifest.read_text())
        document["firmware"] = ["13.60", "13.60"]
        manifest.write_text(json.dumps(document))
        with self.assertRaisesRegex(KitError, "duplicate firmware"):
            validate_kit(self.fixture.out)

    def test_rejects_host_personal_path_in_registry_payload(self) -> None:
        user_registry = self.fixture.prefix / "user.reg"
        original = user_registry.read_text()
        for host_path in ("/Users/alice/private", r"Z:\\Users\\alice\\private"):
            user_registry.write_text(
                original + '\n[Software\\\\Example]\n'
                f'"Cache"="{host_path}"\n', encoding="utf-8")
            with self.assertRaisesRegex(KitError, "host personal path.*user.reg"):
                self.package()
        user_registry.write_text(original, encoding="utf-8")

        config = self.fixture.title / "settings.conf"
        for host_path in (r"Z:\Users\alice\private", r"Z:\home\alice"):
            write(config, f"cache={host_path}\n")
            with self.assertRaisesRegex(KitError, "host personal path.*settings.conf"):
                self.package()
        config.unlink()

    def test_rejects_host_personal_paths_in_binary_payloads(self) -> None:
        for folder, encoding in (("runtime", "utf-8"), ("title", "utf-16-le")):
            root = self.fixture.runtime if folder == "runtime" else self.fixture.title
            payload = root / "embedded.bin"
            write(payload, b"\x7fELF\x00" + "/home/alice/build/private.c".encode(encoding) + b"\x00")
            if folder == "runtime":
                inventory = json.loads(self.fixture.runtime_inventory.read_text())
                inventory["embedded.bin"] = digest(payload)
                self.fixture.runtime_inventory.write_text(json.dumps(inventory))
            with self.assertRaisesRegex(KitError, "host personal path.*embedded.bin"):
                self.package()
            payload.unlink()
            if folder == "runtime":
                inventory.pop("embedded.bin")
                self.fixture.runtime_inventory.write_text(json.dumps(inventory))

    def test_console_form_template_keeps_its_drive_table(self) -> None:
        dosdevices = self.fixture.prefix / "dosdevices"
        for link in dosdevices.iterdir():
            link.unlink()
        write(dosdevices / ".pw-symlinks", "c:\t../drive_c\nz:\t/\n")
        manifest = self.package()
        table = self.fixture.out / manifest["template"]["path"] / "dosdevices/.pw-symlinks"
        self.assertEqual(table.read_text(), "c:\t../drive_c\nz:\t/\n")
        write(dosdevices / ".pw-symlinks", "c:\t../drive_c\nz\n")
        shutil.rmtree(self.fixture.out)
        with self.assertRaisesRegex(KitError, "link table is malformed"):
            self.package()

    def test_manifest_orders_sibling_names_by_full_path(self) -> None:
        # "Fonts-extra" sorts before "Fonts/" as a string but after it as path parts.
        write(self.fixture.prefix / "drive_c/windows/Fonts/a.ttf", b"font")
        write(self.fixture.prefix / "drive_c/windows/Fonts-extra/b.ttf", b"font")
        manifest = self.package()
        paths = [entry["path"] for entry in manifest["files"]]
        self.assertEqual(paths, sorted(paths))

    def test_development_kit_permits_only_runtime_build_paths(self) -> None:
        payload = self.fixture.runtime / "embedded.bin"
        write(payload, b"\x54\x14\xf5\xee/home/alice/build/source/dlls/x.c\x00")
        inventory = json.loads(self.fixture.runtime_inventory.read_text())
        inventory["embedded.bin"] = digest(payload)
        self.fixture.runtime_inventory.write_text(json.dumps(inventory))
        result = package_kit(self.fixture.spec, self.fixture.out, development_build_paths=True)
        self.assertEqual(len(result["development_exceptions"]), 1)
        self.assertTrue(result["development_exceptions"][0].endswith("embedded.bin"))
        manifest = json.loads((self.fixture.out / "manifest.json").read_text())
        self.assertNotIn("development_exceptions", manifest)
        # The default (release-capable) validation still refuses the same kit.
        with self.assertRaisesRegex(KitError, "host personal path.*embedded.bin"):
            validate_kit(self.fixture.out)
        self.assertEqual(len(validate_kit(self.fixture.out, development_build_paths=True)
                             ["development_exceptions"]), 1)

    def test_development_kit_exempts_template_binaries_but_not_text(self) -> None:
        module = self.fixture.prefix / "drive_c/windows/system32/built.dll"
        write(module, b"MZ\x90\x00/home/alice/build/dlls/built/main.c\x00")
        result = package_kit(self.fixture.spec, self.fixture.out, development_build_paths=True)
        self.assertTrue(any(path.endswith("built.dll") for path in result["development_exceptions"]))
        shutil.rmtree(self.fixture.out)
        settings = self.fixture.prefix / "drive_c/windows/settings.conf"
        write(settings, "cache=/home/alice/private\n")
        with self.assertRaisesRegex(KitError, "host personal path.*settings.conf"):
            package_kit(self.fixture.spec, self.fixture.out, development_build_paths=True)

    def test_development_kit_still_refuses_title_build_paths(self) -> None:
        payload = self.fixture.title / "embedded.bin"
        write(payload, b"\x7fELF\x00/home/alice/build/private.c\x00")
        with self.assertRaisesRegex(KitError, "host personal path.*embedded.bin"):
            package_kit(self.fixture.spec, self.fixture.out, development_build_paths=True)

    def test_development_kit_exempts_the_title_mesa_zink_binaries(self) -> None:
        module = self.fixture.title / "win/mesa-zink/i386-windows/libgallium_wgl.dll"
        write(module, b"MZ\x90\x00/home/runner/work/llvm-mingw/libcxxabi/src/cxa_personality.cpp\x00")
        result = package_kit(self.fixture.spec, self.fixture.out, development_build_paths=True)
        self.assertTrue(any(path.endswith("libgallium_wgl.dll") for path in result["development_exceptions"]))
        with self.assertRaisesRegex(KitError, "host personal path.*libgallium_wgl.dll"):
            validate_kit(self.fixture.out)
        shutil.rmtree(self.fixture.out)
        write(self.fixture.title / "win/mesa-zink/notes.txt", "built in /home/alice/mesa\n")
        with self.assertRaisesRegex(KitError, "host personal path.*notes.txt"):
            package_kit(self.fixture.spec, self.fixture.out, development_build_paths=True)

    def test_rejects_wrong_or_ambiguous_windows_user_registry_binding(self) -> None:
        path = self.fixture.prefix / "user.reg"
        pristine = path.read_text()
        wrong = pristine.replace(r'C:\\users\\prospero', r'C:\\users\\other')
        path.write_text(wrong + '\n; expected C:\\users\\prospero\n')
        with self.assertRaisesRegex(KitError, "Windows profile"):
            self.package()

    def test_standalone_validator_rejects_invalid_firmware_and_capabilities(self) -> None:
        self.package()
        manifest_path = self.fixture.out / "manifest.json"
        pristine = json.loads(manifest_path.read_text())
        cases = (("firmware", [], "supported firmware"),
                 ("capabilities", ["dxvk", 42], "capabilities"))
        for field, value, message in cases:
            damaged = json.loads(json.dumps(pristine))
            damaged[field] = value
            manifest_path.write_text(json.dumps(damaged))
            with self.assertRaisesRegex(KitError, message):
                validate_kit(self.fixture.out)
        manifest_path.write_text(json.dumps(pristine))

    def test_rejects_helper_without_independent_title_hash_pin(self) -> None:
        with mock.patch.dict(REQUIRED_HELPER_SHA256, {}, clear=True):
            with self.assertRaisesRegex(KitError, "reviewed helper hash pin"):
                self.package()

    def test_unpatched_fixture_helper_does_not_match_production_pin(self) -> None:
        self.helper_pin.stop()
        try:
            with self.assertRaisesRegex(KitError, "pinned helper digest"):
                self.package()
        finally:
            self.helper_pin.start()

    def test_rejects_helper_whose_self_reported_hash_is_not_independently_pinned(self) -> None:
        write(self.fixture.title / "lapy.elf", b"\x7fELFdifferent-helper", 0o755)
        manifest = json.loads(self.fixture.helper_manifest.read_text())
        manifest["elf_sha256"] = digest(self.fixture.title / "lapy.elf")
        self.fixture.helper_manifest.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(KitError, "pinned helper digest"):
            self.package()

    def test_standalone_validator_rejects_rehashed_unpinned_helper(self) -> None:
        self.package()
        helper = self.fixture.out / "titles/hl2-fa78001-v1/lapy.elf"
        helper_manifest = self.fixture.out / "titles/hl2-fa78001-v1/lapy-manifest.json"
        helper.write_bytes(b"\x7fELFdifferent-helper")
        reported = json.loads(helper_manifest.read_text())
        reported["elf_sha256"] = digest(helper)
        helper_manifest.write_text(json.dumps(reported))

        manifest_path = self.fixture.out / "manifest.json"
        kit_manifest = json.loads(manifest_path.read_text())
        by_path = {entry["path"]: entry for entry in kit_manifest["files"]}
        for path in (helper, helper_manifest):
            relative = path.relative_to(self.fixture.out).as_posix()
            by_path[relative]["size"] = path.stat().st_size
            by_path[relative]["sha256"] = digest(path)
        lines = [f"{entry['path']}\t{entry['size']}\t{entry['sha256']}\t{entry['mode']}\n"
                 for entry in kit_manifest["files"]]
        kit_manifest["files_sha256"] = hashlib.sha256(
            "".join(lines).encode()).hexdigest()
        manifest_path.write_text(json.dumps(kit_manifest))

        with self.assertRaisesRegex(KitError, "pinned helper digest"):
            validate_kit(self.fixture.out)

    def test_validator_rejects_traversal_hash_mode_and_symlink_tampering(self) -> None:
        self.package()
        manifest_path = self.fixture.out / "manifest.json"
        pristine = json.loads(manifest_path.read_text())

        damaged = json.loads(json.dumps(pristine))
        damaged["files"][0]["path"] = "../escape"
        manifest_path.write_text(json.dumps(damaged))
        with self.assertRaisesRegex(KitError, "unsafe manifest path"):
            validate_kit(self.fixture.out)

        manifest_path.write_text(json.dumps(pristine))
        payload = self.fixture.out / pristine["files"][0]["path"]
        payload.write_bytes(payload.read_bytes() + b"tampered")
        with self.assertRaisesRegex(KitError, "hash|size"):
            validate_kit(self.fixture.out)

        self.fixture.out.rename(self.fixture.root / "damaged")
        self.fixture.out = self.fixture.root / "kit2"
        self.package()
        executable = self.fixture.out / "titles/hl2-fa78001-v1/eboot.bin"
        executable.chmod(0o644)
        with self.assertRaisesRegex(KitError, "mode"):
            validate_kit(self.fixture.out)

        executable.chmod(0o755)
        target = self.fixture.root / "outside"
        write(target)
        victim = (self.fixture.out /
                  "runtime/prospero-win-wine-ps5/e16ec8f3-ps5-r1/lib/wine/x86_64-unix/wineserver.prx")
        victim.unlink()
        os.symlink(target, victim)
        with self.assertRaisesRegex(KitError, "symbolic link"):
            validate_kit(self.fixture.out)


if __name__ == "__main__":
    unittest.main()
