#!/usr/bin/env python3
"""Generate exact redistribution notices for the compiled lwIP subset."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def compiled_sources() -> list[str]:
    generator = (ROOT / "tools/gen_ninja.py").read_text(encoding="utf-8")
    block = generator.split("lwip_c = [", 1)[1].split("]", 1)[0]
    return [line.strip().strip('\",') for line in block.splitlines() if '"lwip/' in line]


def license_blocks(source: str) -> list[str]:
    blocks = re.findall(r"/\*.*?\*/", source, re.DOTALL)
    return [
        block.strip()
        for block in blocks
        if re.search(r"Copyright|Redistribution and use|All rights reserved|following disclaimer", block, re.I)
    ]


def generate() -> str:
    output = [
        "lwIP redistribution notices for the netserver.sys compiled source subset.",
        "",
        "The notices below are copied verbatim from the source files listed in the",
        "compiled-source index. Duplicate notice blocks are included once.",
        "",
    ]
    seen: set[str] = set()
    for relative in compiled_sources():
        source_path = ROOT / "kmod/netserver" / relative
        if not source_path.is_file():
            raise FileNotFoundError(source_path)
        output.extend([f"===== kmod/netserver/{relative} =====", ""])
        blocks = license_blocks(source_path.read_text(encoding="utf-8"))
        if not blocks:
            raise RuntimeError(f"no license block found in {source_path}")
        for block in blocks:
            if block in seen:
                output.extend(["[duplicate notice block shown above]", ""])
                continue
            seen.add(block)
            output.extend([block, ""])
    return "\n".join(output).rstrip() + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    parser.add_argument("--write-repository", action="store_true")
    args = parser.parse_args()
    if args.write_repository:
        (ROOT / "licenses/lwip-notice-full.txt").write_text(generate(), encoding="utf-8")
        return
    if args.output is None:
        raise SystemExit("--output is required unless --write-repository is set")
    output = args.output if args.output.is_absolute() else ROOT / args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(generate(), encoding="utf-8")


if __name__ == "__main__":
    main()
