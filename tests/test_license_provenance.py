from __future__ import annotations

import json
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class LicenseProvenanceTests(unittest.TestCase):
    def test_parson_json_parser_provenance_is_declared(self) -> None:
        license_path = ROOT / "third_party/parson/LICENSE"
        source_record = ROOT / "third_party/parson/SOURCE.md"
        implementation = (ROOT / "lib/xapi_json_impl.inc").read_text(encoding="utf-8")
        notices = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        root_licenses = (ROOT / "LICENSES.md").read_text(encoding="utf-8")
        manifest = json.loads(
            (ROOT / "third_party/compliance-manifest.json").read_text(encoding="utf-8")
        )
        components = {component["slug"]: component for component in manifest["components"]}
        license_text = license_path.read_text(encoding="utf-8")
        source_text = source_record.read_text(encoding="utf-8")
        implementation_sha = subprocess.check_output(
            ["sha256sum", str(ROOT / "lib/xapi_json_impl.inc")],
            text=True,
        ).split()[0]

        self.assertIn("MIT License", license_text)
        self.assertIn("Krzysztof Gabis", license_text)
        self.assertIn("kgabis/parson", source_text)
        self.assertIn("ba29f4eda9ea7703a9f6a9cf2b0532a2605723c3", source_text)
        self.assertIn(implementation_sha, source_text)
        self.assertIn("SPDX-License-Identifier: MIT", implementation)
        self.assertIn("third_party/parson/SOURCE.md", implementation)
        self.assertEqual("parson JSON parser", components["parson"]["name"])
        self.assertEqual("MIT", components["parson"]["license"])
        self.assertEqual(
            ["third_party/parson/LICENSE", "third_party/parson/SOURCE.md"],
            components["parson"]["license_files"],
        )
        self.assertEqual(["lib/xapi_json_impl.inc"], components["parson"]["source_files"])
        self.assertIn("parson JSON parser", notices)
        self.assertIn("parson JSON parser", root_licenses)

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


if __name__ == "__main__":
    unittest.main()
