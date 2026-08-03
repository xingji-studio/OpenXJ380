from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import ninja_build


ROOT = Path(__file__).resolve().parents[1]


class NinjaBuildSourceToolTests(unittest.TestCase):
    def test_check_sources_only_checks_translation_units_with_matching_language(self) -> None:
        # Given: one source and one header for each supported language.
        sources = (
            [ROOT / "kernel/example.c"],
            [ROOT / "driver/example.cpp"],
            [ROOT / "include/example.h", ROOT / "include/example.hpp"],
        )
        commands: list[list[str]] = []

        # When: the source checker builds its clang-tidy commands.
        with (
            mock.patch.object(ninja_build, "root_sources", return_value=sources),
            mock.patch.object(ninja_build, "run", side_effect=commands.append),
        ):
            ninja_build.check_sources()

        # Then: only real translation units are checked with their matching standard.
        self.assertEqual(2, len(commands))
        self.assertEqual(str(sources[0][0]), commands[0][1])
        self.assertIn("-std=c11", commands[0])
        self.assertEqual(str(sources[1][0]), commands[1][1])
        self.assertIn("-std=gnu++17", commands[1])

    def test_format_sources_still_formats_headers(self) -> None:
        # Given: one source and one header for each supported language.
        sources = (
            [ROOT / "kernel/example.c"],
            [ROOT / "driver/example.cpp"],
            [ROOT / "include/example.h", ROOT / "include/example.hpp"],
        )
        commands: list[list[str]] = []

        # When: the formatter builds its clang-format commands.
        with (
            mock.patch.object(ninja_build, "root_sources", return_value=sources),
            mock.patch.object(ninja_build, "run", side_effect=commands.append),
        ):
            ninja_build.format_sources()

        # Then: source files and headers are all formatted.
        self.assertEqual(
            [
                ["clang-format", "-i", str(path)]
                for path in sources[0] + sources[1] + sources[2]
            ],
            commands,
        )



class NinjaBuildQemuFirmwareTests(unittest.TestCase):
    def test_qemu_uses_explicit_ovmf_firmware_override(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            firmware = Path(tmp) / "OVMF test.fd"
            firmware.write_bytes(b"firmware")

            with mock.patch.dict(
                os.environ,
                {"OVMF_FIRMWARE": str(firmware), "SUDO": "0"},
                clear=False,
            ):
                command = ninja_build.qemu_cmd()

        self.assertIn(f"-bios '{firmware}'", command)
        self.assertNotIn("-bios OVMF.fd", command)

    def test_qemu_falls_back_to_system_ovmf_firmware(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            firmware = Path(tmp) / "OVMF.fd"
            firmware.write_bytes(b"firmware")

            with (
                mock.patch.dict(os.environ, {"SUDO": "0"}, clear=False),
                mock.patch.object(ninja_build, "OVMF_FIRMWARE_CANDIDATES", (firmware,)),
            ):
                os.environ.pop("OVMF_FIRMWARE", None)
                command = ninja_build.qemu_cmd()

        self.assertIn(f"-bios {firmware}", command)
        self.assertNotIn("-bios OVMF.fd", command)


if __name__ == "__main__":
    unittest.main()
