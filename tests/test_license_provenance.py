from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class LicenseProvenanceTests(unittest.TestCase):
    def test_musl_elf_license_record_replaces_stale_glibc_record(self) -> None:
        root_licenses = (ROOT / "LICENSES.md").read_text(encoding="utf-8")
        source_record = (ROOT / "third_party/musl-elf/SOURCE.md").read_text(encoding="utf-8")
        manifest = json.loads(
            (ROOT / "third_party/compliance-manifest.json").read_text(encoding="utf-8")
        )
        components = {component["slug"]: component for component in manifest["components"]}

        self.assertIn("musl ELF definitions", root_licenses)
        self.assertIn("MIT", root_licenses)
        self.assertNotIn("GNU libc `elf.h`", root_licenses)
        self.assertFalse((ROOT / "licenses/glibc-elf-h.txt").exists())
        self.assertTrue((ROOT / "licenses/musl-elf.txt").is_file())
        self.assertIn(
            "c0ec617f48fb3e28a808c7d27e442496cbc51e0cf7d2e669440645d9a786b9e6",
            source_record,
        )
        self.assertEqual(
            ["third_party/musl-elf/COPYRIGHT", "third_party/musl-elf/SOURCE.md"],
            components["musl-elf"]["license_files"],
        )

    def test_libutf_license_material_is_complete(self) -> None:
        license_path = ROOT / "third_party/libutf/LICENSE"
        source_record = ROOT / "third_party/libutf/SOURCE.md"
        implementation = (ROOT / "kernel/utflib.cpp").read_text(encoding="utf-8")
        manifest = json.loads(
            (ROOT / "third_party/compliance-manifest.json").read_text(encoding="utf-8")
        )
        components = {component["slug"]: component for component in manifest["components"]}

        self.assertIn("MIT/X Consortium License", license_path.read_text(encoding="utf-8"))
        self.assertIn("Connor Lane Smith", source_record.read_text(encoding="utf-8"))
        self.assertIn("SPDX-License-Identifier: MIT", implementation)
        self.assertIn("third_party/libutf/SOURCE.md", implementation)
        self.assertEqual("libutf", components["libutf"]["name"])
        self.assertEqual(
            ["third_party/libutf/LICENSE", "third_party/libutf/SOURCE.md"],
            components["libutf"]["license_files"],
        )
        self.assertEqual(
            ["kernel/utflib.cpp", "include/proto.hpp"],
            components["libutf"]["source_files"],
        )

    def test_linux_uapi_and_llist_provenance_are_clean(self) -> None:
        ioctl_header = (ROOT / "include/ioctl.h").read_text(encoding="utf-8")
        llist_header = (ROOT / "include/llist.h").read_text(encoding="utf-8")
        manifest = json.loads(
            (ROOT / "third_party/compliance-manifest.json").read_text(encoding="utf-8")
        )
        components = {component["slug"]: component for component in manifest["components"]}

        self.assertIn("GPL-2.0 WITH Linux-syscall-note", ioctl_header)
        self.assertIn("third_party/linux-uapi/SOURCE.md", ioctl_header)
        self.assertTrue((ROOT / "third_party/linux-uapi/COPYING").is_file())
        self.assertTrue((ROOT / "third_party/linux-uapi/GPL-2.0").is_file())
        self.assertTrue((ROOT / "third_party/linux-uapi/Linux-syscall-note").is_file())
        self.assertEqual("GPL-2.0 WITH Linux-syscall-note", components["linux-uapi"]["license"])
        self.assertNotIn("list_entry - get the struct for this entry", llist_header)
        self.assertNotIn("@ptr:", llist_header)
        self.assertNotIn("list_for_each_entry", llist_header)

    def test_liballoc_record_matches_rust_runtime_and_talc_archive(self) -> None:
        liballoc_record = (ROOT / "licenses/liballoc.txt").read_text(encoding="utf-8")
        rust_source = (ROOT / "third_party/rust-runtime/SOURCE.md").read_text(encoding="utf-8")
        talc_source = (ROOT / "third_party/talc/SOURCE.md").read_text(encoding="utf-8")
        talc_license = (ROOT / "third_party/talc/LICENSE.md").read_text(encoding="utf-8")
        symbols = subprocess.check_output(["nm", "-a", str(ROOT / "liballoc-x86_64.a")], text=True)
        manifest = json.loads(
            (ROOT / "third_party/compliance-manifest.json").read_text(encoding="utf-8")
        )
        components = {component["slug"]: component for component in manifest["components"]}

        self.assertNotIn("plos-clan/liballoc", liballoc_record)
        self.assertIn("Rust alloc", liballoc_record)
        self.assertIn("compiler_builtins", liballoc_record)
        self.assertIn("talc", liballoc_record)
        self.assertIn("_RNvMs0_NtCs1PPGXRNTyC3_4talc4talc", symbols)
        self.assertIn("Shaun Beautement", talc_license)
        self.assertIn("SFBdragon/talc", talc_source)
        self.assertIn("ad7bae44cec09e810d531b558499bef47ca4d17b", talc_source)
        self.assertIn("2137fa65410bfecc22371769f3557e8f342216254afe67ad72e76117d1446d08", rust_source)
        self.assertIn("third_party/rust-runtime/SOURCE.md", components["rust-runtime"]["license_files"])
        self.assertIn("liballoc-x86_64.a", components["rust-runtime"]["source_files"])
        self.assertEqual("MIT", components["talc"]["license"])
        self.assertEqual(
            ["third_party/talc/LICENSE.md", "third_party/talc/SOURCE.md"],
            components["talc"]["license_files"],
        )
        self.assertEqual(["liballoc-x86_64.a"], components["talc"]["source_files"])

    def test_compliance_bundle_contains_provenance_materials(self) -> None:
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

            for relative_path in (
                "licenses/musl-elf/COPYRIGHT",
                "licenses/musl-elf/SOURCE.md",
                "licenses/libutf/LICENSE",
                "licenses/libutf/SOURCE.md",
                "licenses/linux-uapi/COPYING",
                "licenses/linux-uapi/GPL-2.0",
                "licenses/linux-uapi/Linux-syscall-note",
                "licenses/linux-uapi/SOURCE.md",
                "licenses/rust-runtime/SOURCE.md",
                "licenses/talc/LICENSE.md",
                "licenses/talc/SOURCE.md",
            ):
                self.assertTrue((destination / relative_path).is_file(), relative_path)

            self.assertFalse((destination / "licenses/glibc-elf-h.txt").exists())


if __name__ == "__main__":
    unittest.main()
