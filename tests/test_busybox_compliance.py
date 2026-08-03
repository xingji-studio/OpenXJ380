from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BusyBoxComplianceTests(unittest.TestCase):
    def test_corresponding_source_bundle_is_complete(self) -> None:
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

    def test_vulnerable_applets_are_disabled(self) -> None:
        config = (ROOT / "third_party/busybox-source/busybox-1.31.1.config").read_text(encoding="utf-8")

        disabled_options = (
            "CONFIG_FEATURE_SEAMLESS_XZ",
            "CONFIG_FEATURE_SEAMLESS_LZMA",
            "CONFIG_UNLZMA",
            "CONFIG_LZMA",
            "CONFIG_UNXZ",
            "CONFIG_XZCAT",
            "CONFIG_XZ",
            "CONFIG_FEATURE_UNZIP_LZMA",
            "CONFIG_FEATURE_UNZIP_XZ",
            "CONFIG_WGET",
        )
        for option in disabled_options:
            self.assertIn(f"# {option} is not set", config)

        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "busybox"
            executable.write_bytes((ROOT / "resources/apps/busybox").read_bytes())
            executable.chmod(0o755)
            applets = subprocess.check_output([str(executable), "--list"], text=True).splitlines()

        kernel_main = (ROOT / "kernel/main.cpp").read_text(encoding="utf-8")
        alias_table = kernel_main.split("busybox_alias_applets[] = {", 1)[1].split("};", 1)[0]
        for applet in ("wget", "lzma", "unlzma", "unxz", "xz", "xzcat"):
            self.assertNotIn(applet, applets)
            self.assertNotIn(f'"{applet}"', alias_table)

    def test_build_disables_volatile_timestamp(self) -> None:
        build_script = (ROOT / "third_party/busybox-source/build.sh").read_text(encoding="utf-8")

        self.assertIn("KCONFIG_NOTIMESTAMP=1", build_script)


if __name__ == "__main__":
    unittest.main()
