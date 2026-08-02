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


if __name__ == "__main__":
    unittest.main()
