from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class KrlibcFormatterTests(unittest.TestCase):
    def test_strtol_has_no_bsd_or_musl_markers(self) -> None:
        source = (ROOT / "kernel/krlibc.cpp").read_text(encoding="utf-8")

        self.assertNotIn("goto noconv", source)
        self.assertNotIn("LONG_MIN + LONG_MAX", source)
        self.assertNotIn("any < 0", source)
        self.assertNotIn("cutlim", source)
    def test_number_formatter_has_no_linux_vsprintf_markers(self) -> None:
        source = (ROOT / "kernel/krlibc.cpp").read_text(encoding="utf-8")

        self.assertNotIn("digits[33]", source)
        self.assertNotIn("If type no include LEFT or ZEROPAD", source)
        self.assertNotIn("LEFT adjust", source)
        self.assertNotIn("Write the zero padding", source)


if __name__ == "__main__":
    unittest.main()
