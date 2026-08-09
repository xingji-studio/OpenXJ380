import os
import subprocess
import sys
import tempfile
import unittest
from functools import cache
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


@cache
def generated_ninja_text() -> str:
    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        out = temp_path / "build.ninja"
        rust_libdir = temp_path / "rustlib"
        rust_libdir.mkdir()
        (rust_libdir / "libcore-test.rlib").touch()
        (rust_libdir / "libcompiler_builtins-test.rlib").touch()

        env = os.environ.copy()
        env["RUST_TARGET"] = "x86_64-unknown-linux-gnu"
        env["RUST_TARGET_LIBDIR"] = str(rust_libdir)
        env.pop("DIAGNOSTICS_COLOR", None)
        env.pop("RUST_DIAGNOSTICS_COLOR", None)
        subprocess.run(
            [sys.executable, "tools/gen_ninja.py", "--out", str(out)],
            cwd=ROOT,
            env=env,
            check=True,
        )
        return out.read_text(encoding="utf-8")


class GenNinjaDependencyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = generated_ninja_text()

    def test_xapi_objects_use_compiler_depfiles(self) -> None:
        self.assertIn("rule xapi_cxx\n", self.text)
        self.assertIn("depfile = $out.d\n", self.text)
        self.assertIn("deps = gcc\n", self.text)

    def test_user_objects_use_compiler_depfiles(self) -> None:
        self.assertIn("rule user_cxx_custom\n", self.text)
        self.assertIn("build out/cli_shell.o: user_cxx_custom user/cli_shell.cpp", self.text)
        self.assertIn("build out/shell.elf: user_ld", self.text)

    def test_user_tree_keeps_only_xapi_and_the_cli_example(self) -> None:
        user_entries = {path.name for path in (ROOT / "user").iterdir()}
        self.assertEqual(user_entries, {"AGENTS.md", "cli_shell.cpp", "xapi"})

    def test_gui_sources_are_absent_from_build_graph(self) -> None:
        source_section = self.text.split("CPP_FILES =", 1)[1].split("HEADER_FILES =", 1)[0]
        self.assertNotIn("graphics/", source_section)
        self.assertNotIn("user/browser/", source_section)
        self.assertNotIn("user/shell/", source_section)
        self.assertNotIn("font/ttf/", source_section)
        self.assertIn("font/hankaku.bin", self.text)


class GenNinjaLogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = generated_ninja_text()

    def test_compile_rules_use_bracketed_concise_descriptions(self) -> None:
        self.assertIn("description = \x1b[1;35m[ASM]\x1b[0m $in -> $out\n", self.text)
        self.assertIn("description = \x1b[1;32m[CC]\x1b[0m $in -> $out\n", self.text)
        self.assertIn("description = \x1b[1;36m[CXX]\x1b[0m $in -> $out\n", self.text)
        self.assertNotIn("description = CPP $in -> $out\n", self.text)

    def test_link_and_utility_rules_keep_short_action_labels(self) -> None:
        self.assertIn("description = \x1b[1;33m[LD]\x1b[0m $out\n", self.text)
        self.assertIn("description = \x1b[1;34m[CP]\x1b[0m $in -> $out\n", self.text)
        self.assertIn("description = \x1b[1;35m[GEN]\x1b[0m build.ninja\n", self.text)
        self.assertNotIn("description = LINK $out\n", self.text)
        self.assertNotIn("LINK browser objects", self.text)


class GenNinjaDiagnosticColorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = generated_ninja_text()

    def test_compiler_diagnostics_force_color_under_ninja(self) -> None:
        self.assertIn("DIAGNOSTIC_COLOR = -fdiagnostics-color=always\n", self.text)
        self.assertIn("diagnostic_color = $DIAGNOSTIC_COLOR\n", self.text)
        self.assertIn("C_FLAGS = $DIAGNOSTIC_COLOR $OPT_FLAG", self.text)
        self.assertIn("CPP_FLAGS = $DIAGNOSTIC_COLOR $OPT_FLAG", self.text)
        self.assertIn("c_flags = $C_FLAGS\n", self.text)
        self.assertIn("cpp_flags = $CPP_FLAGS\n", self.text)
        self.assertIn("$user_cxx $diagnostic_color $opt", self.text)
        self.assertIn("$user_cc $diagnostic_color $opt", self.text)

    def test_rust_diagnostics_force_color_under_ninja(self) -> None:
        self.assertIn("RUST_DIAGNOSTIC_COLOR = --color always\n", self.text)
        self.assertIn("rust_diagnostic_color = $RUST_DIAGNOSTIC_COLOR\n", self.text)
        self.assertIn("RUST_FLAGS = $RUST_DIAGNOSTIC_COLOR --edition=2024", self.text)
        self.assertIn("rust_user_flags = $RUST_FLAGS\n", self.text)


if __name__ == "__main__":
    unittest.main()
