from __future__ import annotations

import json
import importlib
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class LicenseComplianceTests(unittest.TestCase):
    def test_elf_header_uses_declared_permissive_upstream(self) -> None:
        header = (ROOT / "include/elf.h").read_text(encoding="utf-8")

        self.assertIn("SPDX-License-Identifier: MIT", header)
        self.assertNotIn("GNU Lesser General Public", header)
        self.assertNotIn("GNU C Library", header)

    def test_disallowed_gpl_imports_are_absent(self) -> None:
        self.assertFalse((ROOT / "third_party/lwext4").exists())
        self.assertFalse((ROOT / "seabios.bin").exists())

    def test_seabios_has_no_build_or_source_reference(self) -> None:
        needle = "sea" + "bios.bin"
        referenced_by: list[str] = []
        candidates = subprocess.check_output(
            ["git", "ls-files", "-co", "--exclude-standard", "-z"], cwd=ROOT
        ).decode().split("\0")
        for relative in candidates:
            path = ROOT / relative
            if not relative or path == Path(__file__) or not path.is_file():
                continue
            if path.stat().st_size > 4 * 1024 * 1024:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            if needle in text.lower():
                referenced_by.append(relative)

        self.assertEqual([], referenced_by)

    def test_root_ovmf_firmware_is_not_distributed(self) -> None:
        self.assertFalse((ROOT / "OVMF.fd").exists())
        tracked_paths = subprocess.check_output(
            ["git", "ls-files", "OVMF.fd"], cwd=ROOT, text=True
        ).splitlines()
        self.assertEqual([], tracked_paths)

    def test_mbedtls_apache_selection_is_machine_readable(self) -> None:
        selection = json.loads(
            (ROOT / "third_party/mbedtls-license-selection.json").read_text(encoding="utf-8")
        )

        self.assertEqual("Apache-2.0", selection["selected_license"])
        self.assertEqual(
            "Apache-2.0 OR GPL-2.0-or-later", selection["upstream_license_expression"]
        )

    def test_busybox_corresponding_source_bundle_is_complete(self) -> None:
        source_dir = ROOT / "third_party/busybox-source"
        archive = source_dir / "busybox-1.31.1.tar.bz2"
        expected_source_sha256 = (
            "d0f940a72f648943c1f2211e0e3117387c31d765137d92bd8284a3fb9752a998"
        )
        expected_binary_sha256 = (
            "a08214e46cafb238685f694a2ff4e4b038b5fed83f884354127ba08d68498066"
        )
        staged_binary = ROOT / "resources/apps/busybox"
        preserved_binary = ROOT / "third_party/busybox-prebuilt/busybox_amd64"

        self.assertTrue(archive.is_file())
        self.assertTrue((source_dir / "LICENSE").is_file())
        self.assertTrue((source_dir / "busybox-1.31.1.config").is_file())
        self.assertEqual(
            expected_source_sha256,
            subprocess.check_output(["sha256sum", str(archive)], text=True).split()[0],
        )
        for binary in (staged_binary, preserved_binary):
            self.assertEqual(
                expected_binary_sha256,
                subprocess.check_output(["sha256sum", str(binary)], text=True).split()[0],
            )
        self.assertEqual(staged_binary.read_bytes(), preserved_binary.read_bytes())
        subprocess.run(["bzip2", "-t", str(archive)], check=True)

    def test_busybox_build_disables_volatile_timestamp(self) -> None:
        build_script = (
            ROOT / "third_party/busybox-source/build.sh"
        ).read_text(encoding="utf-8")

        self.assertIn("KCONFIG_NOTIMESTAMP=1", build_script)

    def test_mikanos_hankaku_source_material_is_complete(self) -> None:
        source_root = ROOT / "third_party/mikanos-hankaku"
        self.assertTrue((source_root / "LICENSE").is_file())
        self.assertTrue((source_root / "SOURCE.md").is_file())
        self.assertTrue((source_root / "hankaku.txt").is_file())

        manifest = json.loads(
            (ROOT / "third_party/compliance-manifest.json").read_text(encoding="utf-8")
        )
        components = {component["slug"]: component for component in manifest["components"]}

        self.assertEqual("MikanOS hankaku font", components["mikanos-hankaku"]["name"])
        self.assertEqual(
            ["third_party/mikanos-hankaku/LICENSE", "third_party/mikanos-hankaku/SOURCE.md"],
            components["mikanos-hankaku"]["license_files"],
        )
        self.assertIn(
            "third_party/mikanos-hankaku/hankaku.txt", components["mikanos-hankaku"]["source_files"]
        )
        self.assertTrue(components["mikanos-hankaku"]["bundle_source"])

    def test_third_party_manifest_references_existing_materials(self) -> None:
        manifest = json.loads(
            (ROOT / "third_party/compliance-manifest.json").read_text(encoding="utf-8")
        )
        components = manifest["components"]

        self.assertGreaterEqual(len(components), 12)
        for component in components:
            for key in ("license_files", "source_files"):
                for relative_path in component[key]:
                    self.assertTrue(
                        (ROOT / relative_path).exists(),
                        f"{component['name']}: missing {relative_path}",
                    )

    def test_compliance_bundle_command_produces_required_tree(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            destination = Path(tmp) / "third-party"

            subprocess.run(
                [
                    "python3",
                    str(ROOT / "tools/package_third_party.py"),
                    "--output",
                    str(destination),
                ],
                cwd=ROOT,
                check=True,
            )

            self.assertTrue((destination / "MANIFEST.json").is_file())
            self.assertTrue((destination / "notices/THIRD_PARTY_NOTICES.md").is_file())
            self.assertTrue(
                (destination / "sources/busybox/busybox-1.31.1.tar.bz2").is_file()
            )

    def test_compliance_build_edge_tracks_packaged_materials(self) -> None:
        build_graph = (ROOT / "build.ninja").read_text(encoding="utf-8")
        edge = next(
            line
            for line in build_graph.splitlines()
            if line.startswith("build out/compliance/third-party/MANIFEST.json:")
        )

        self.assertIn("third_party/busybox-source/build.sh", edge)
        self.assertIn("third_party/busybox-source/LICENSE", edge)

    def test_system_staging_replaces_legacy_module_extension(self) -> None:
        ninja_build = importlib.import_module("tools.ninja_build")
        original_root = ninja_build.ROOT
        with tempfile.TemporaryDirectory() as tmp:
            fixture = Path(tmp)
            project = fixture / "project"
            stage = fixture / "stage"
            (project / "out").mkdir(parents=True)
            (stage / "mod").mkdir(parents=True)
            (project / "out/netserver.sys").write_bytes(b"current")
            (stage / "mod/netserver.km").write_bytes(b"legacy")

            try:
                ninja_build.ROOT = project
                ninja_build.copy_modules(stage)
            finally:
                ninja_build.ROOT = original_root

            self.assertFalse((stage / "mod/netserver.km").exists())
            self.assertEqual(b"current", (stage / "mod/netserver.sys").read_bytes())


if __name__ == "__main__":
    unittest.main()
