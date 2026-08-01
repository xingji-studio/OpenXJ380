import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class NinjaFileLogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = (ROOT / "build.ninja").read_text(encoding="utf-8")

    def test_direct_compile_rules_use_bracketed_labels(self) -> None:
        self.assertIn("rule nasm\n", self.text)
        self.assertIn("rule root_as\n", self.text)
        self.assertIn("rule root_cc\n", self.text)
        self.assertIn("rule root_cxx\n", self.text)
        self.assertIn("description = \x1b[1;35m[ASM]\x1b[0m $in -> $out\n", self.text)
        self.assertIn("description = \x1b[1;32m[CC]\x1b[0m $in -> $out\n", self.text)
        self.assertIn("description = \x1b[1;36m[CXX]\x1b[0m $in -> $out\n", self.text)


if __name__ == "__main__":
    unittest.main()
