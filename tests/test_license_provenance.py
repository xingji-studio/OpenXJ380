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


if __name__ == "__main__":
    unittest.main()
