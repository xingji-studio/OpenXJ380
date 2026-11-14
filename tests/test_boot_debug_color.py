"""Check actual framebuffer stores and console preservation with guarded host buffers."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BootDebugColorTests(unittest.TestCase):
    def test_enabled_disabled_and_non_one(self):
        with tempfile.TemporaryDirectory(prefix="xj380-boot-debug-host-") as directory:
            work = Path(directory)
            (work / "cpu").mkdir()
            (work / "cpu/lock.h").write_text(
                "#pragma once\n"
                "struct spin_t { int value; };\n"
                "#define SPIN_INIT {0}\n"
                "inline void spin_init(spin_t *) {}\n"
                "inline void spin_lock(spin_t *) {}\n"
                "inline void spin_unlock(spin_t *) {}\n"
            )
            for value in (1, 0, 2):
                with self.subTest(macro=value):
                    binary = work / f"boot-debug-{value}"
                    subprocess.run([
                        "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", "-O2",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        f"-DDEVELOP_MODE_BOOT_DEBUG_COLOR={value}",
                        "-I", str(work), "-iquote", str(ROOT / "kernel"),
                        "-idirafter", str(ROOT / "include"),
                        str(ROOT / "tests/boot_debug_color_harness.cpp"),
                        str(ROOT / "kernel/boot_debug_color.cpp"), str(ROOT / "kernel/console.cpp"),
                        "-o", str(binary),
                    ], check=True)
                    subprocess.run([str(binary)], check=True, timeout=30,
                                   env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                                            UBSAN_OPTIONS="halt_on_error=1"))


if __name__ == "__main__":
    unittest.main()
