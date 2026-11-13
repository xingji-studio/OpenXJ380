# SPDX-License-Identifier: Apache-2.0
"""Exercise CI readback with real ext4/FAT32 images and missing system files."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from tools import verify_disk_image as verifier


ROOT = Path(__file__).resolve().parents[1]


class VerifyDiskImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("sgdisk", "mkfs.vfat", "mcopy", "mdir", "mke2fs", "e2fsck", "debugfs"):
            if not shutil.which(tool):
                raise unittest.SkipTest(f"{tool} is needed for image verification tests")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xj380-ci-image-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.staged = self.work / "staged root"
        self.image = self.work / "test disk.img"
        self.files = {
            **{name: f"fixture for {name}\n".encode() for name in verifier.ROOT_FILES},
            "EFI/BOOT/BOOTX64.EFI": b"MZ EFI fixture",
            "system/activept.cfk": b"",
            "config/README.md": b"configuration guide\n",
        }
        for name, contents in self.files.items():
            path = self.staged / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)

    def build_image(self, filesystem="ext4"):
        subprocess.run([sys.executable, str(ROOT / "tools/make_disk_image.py"), "--root", str(self.staged),
                        "--image", str(self.image), "--size-mib", "128", "--fs", filesystem],
                       check=True, capture_output=True, text=True)

    def test_default_layout_reproduces_old_error_and_verifies_without_modifying_image(self):
        self.build_image()
        old_check = subprocess.run(["mdir", "-i", f"{self.image}@@1048576", "::/EFI/BOOT/BOOTX64.efi"],
                                   capture_output=True, text=True)
        self.assertNotEqual(old_check.returncode, 0)
        self.assertIn("non DOS media", old_check.stderr)
        before = verifier.file_digest(self.image)
        subprocess.run([sys.executable, str(ROOT / "tools/verify_disk_image.py"), str(self.image)],
                       check=True, capture_output=True, text=True)
        self.assertEqual(verifier.file_digest(self.image), before)
        # Partition numbers are not filesystem identities.
        subprocess.run(["sgdisk", "--transpose=1:2", str(self.image)], check=True, capture_output=True)
        verifier.verify_image(self.image)

    def test_legacy_fat32_layout(self):
        self.build_image("fat32")
        verifier.verify_image(self.image)

    def test_missing_ext4_file_is_rejected_even_if_present_on_esp(self):
        name = "system/mod/netserver.sys"
        (self.staged / name).unlink()
        self.build_image()
        esp = verifier.read_partitions(self.image)["esp"][0]
        decoy_directory = self.work / "mod"
        decoy_directory.mkdir()
        decoy = decoy_directory / "netserver.sys"
        decoy.write_bytes(self.files[name])
        # A same-named file on the ESP must not hide missing root contents.
        subprocess.run(["mcopy", "-s", "-i", f"{self.image}@@{esp * 512}", str(decoy_directory), "::/system"],
                       check=True, capture_output=True)
        with self.assertRaisesRegex(ValueError, "missing or empty image file: /system/mod/netserver.sys"):
            verifier.verify_image(self.image)

    def test_missing_fat32_file_is_rejected(self):
        (self.staged / "apps/system/shell/shell.elf").unlink()
        self.build_image("fat32")
        with self.assertRaises(subprocess.CalledProcessError):
            verifier.verify_image(self.image)

    def test_different_esp_and_root_kernels_are_rejected(self):
        self.build_image()
        esp = verifier.read_partitions(self.image)["esp"][0]
        replacement = self.work / "kernel.krl"
        replacement.write_bytes(b"different kernel")
        subprocess.run(["mcopy", "-o", "-i", f"{self.image}@@{esp * 512}", str(replacement),
                        "::/system/kernel.krl"], check=True, capture_output=True)
        with self.assertRaisesRegex(ValueError, "different kernels"):
            verifier.verify_image(self.image)


if __name__ == "__main__":
    unittest.main()
