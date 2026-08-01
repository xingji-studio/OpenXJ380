from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class DmaPlanTests(unittest.TestCase):
    def test_isa_dma_transfer_plans_cover_8_and_16_bit_channels(self) -> None:
        source = (
            '#include "driver/dma_plan.h"\n'
            "\n"
            "int main()\n"
            "{\n"
            "    DmaTransferPlan plan = {};\n"
            "    if (!dma_plan_transfer(1, 0x1234, 16, &plan)) return 1;\n"
            "    if (plan.page != 0 || plan.offset != 0x1234 || plan.count != 15) return 2;\n"
            "\n"
            "    if (!dma_plan_transfer(5, 0x12340, 16, &plan)) return 3;\n"
            "    if (plan.page != 1 || plan.offset != 0x91a0 || plan.count != 7) return 4;\n"
            "    return 0;\n"
            "}\n"
        )

        self._compile_and_run(source)

    def test_isa_dma_transfer_plans_reject_invalid_ranges(self) -> None:
        source = (
            '#include "driver/dma_plan.h"\n'
            "\n"
            "int main()\n"
            "{\n"
            "    DmaTransferPlan plan = {};\n"
            "    if (dma_plan_transfer(4, 0x1000, 16, &plan)) return 1;\n"
            "    if (dma_plan_transfer(5, 0x1001, 16, &plan)) return 2;\n"
            "    if (dma_plan_transfer(1, 0xfff0, 32, &plan)) return 3;\n"
            "    if (dma_plan_transfer(5, 0x1fff0, 32, &plan)) return 4;\n"
            "    if (dma_plan_transfer(1, 0x1000000, 1, &plan)) return 5;\n"
            "    if (dma_plan_transfer(1, 0x1000, 0, &plan)) return 6;\n"
            "    return 0;\n"
            "}\n"
        )

        self._compile_and_run(source)

    def _compile_and_run(self, source: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source_path = Path(tmp) / "dma_plan_test.cpp"
            binary_path = Path(tmp) / "dma_plan_test"
            source_path.write_text(source, encoding="utf-8")
            subprocess.run(
                [
                    "g++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT),
                    str(source_path),
                    "-o",
                    str(binary_path),
                ],
                check=True,
            )
            subprocess.run([str(binary_path)], check=True)


if __name__ == "__main__":
    unittest.main()
