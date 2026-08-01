import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class MakefileLogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = (ROOT / "Makefile").read_text(encoding="utf-8")

    def test_direct_compile_rules_use_bracketed_labels(self) -> None:
        self.assertIn('printf "\\033[1;35m[ASM]\\033[0m %s -> %s\\n" $< $@', self.text)
        self.assertIn('printf "\\033[1;32m[CC]\\033[0m %s -> %s\\n" $< $@', self.text)
        self.assertIn('printf "\\033[1;36m[CXX]\\033[0m %s -> %s\\n" $< $@', self.text)
        self.assertNotIn('echo "NA $< -> $@"', self.text)
        self.assertNotIn('echo "AS $< -> $@"', self.text)
        self.assertNotIn('echo "CC $< -> $@"', self.text)
        self.assertNotIn('echo "CPP $< -> $@"', self.text)


if __name__ == "__main__":
    unittest.main()
