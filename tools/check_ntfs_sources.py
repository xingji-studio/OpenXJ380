#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the independently reviewed native NTFS formatter source inventory."""
import argparse
import hashlib
import json
from pathlib import Path
import re


INCLUDES = {"mkfs.cpp": {"mkfs.h", "errno.h"}, "mkfs.h": {"stdint.h"}}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check(root):
    directory = root / "driver/fs/ntfs"
    manifest = json.loads((directory / "SOURCE.json").read_text())
    require(manifest["schema_version"] == 1 and manifest["license"] == "Apache-2.0", "unsupported manifest")
    require(not manifest["new_link_archives"] and not manifest["imported_implementations"],
            "new formatter dependency needs review")
    require(set(manifest["sources"]) == set(INCLUDES), "unexpected formatter inventory")
    discovered = set()
    for path in directory.rglob("*"):
        require(not path.is_symlink(), f"symlinked formatter input: {path}")
        if not path.is_file():
            continue
        if path.suffix in {".cpp", ".h"}:
            discovered.add(path.relative_to(directory).as_posix())
        require(path.suffix.lower() not in {".a", ".o", ".so", ".lib", ".dll", ".bc", ".c", ".cc", ".cxx",
                                            ".hpp", ".hh", ".inc", ".s", ".asm", ".rs"},
                f"unreviewed formatter source/archive: {path}")
    require(discovered == set(INCLUDES), "extra or missing formatter input")
    for name, expected in manifest["sources"].items():
        path = directory / name
        require(path.is_file() and not path.is_symlink(), f"missing or symlinked formatter: {name}")
        raw = path.read_bytes()
        require(hashlib.sha256(raw).hexdigest() == expected, f"formatter changed without review: {name}")
        source = raw.decode()
        require("SPDX-License-Identifier: Apache-2.0" in source[:512], f"missing Apache SPDX: {name}")
        source = re.sub(r"\\\r?\n", "", source)
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
        source = source.replace("%:", "#").replace("??=", "#")
        for match in re.finditer(r"(?m)^\s*#\s*(include(?:_next)?|import)\b([^\n]*)", source):
            literal = re.fullmatch(r'\s*[<"]([^>"\n]+)[>"]\s*', match[2])
            require(match[1] == "include" and literal and literal[1] in INCLUDES[name],
                    f"unreviewed formatter include: {name}: {match[2]}")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        check(args.root)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"NTFS formatter source policy: {error}\n")
    print("NTFS formatter source policy: original Apache-2.0 inputs match")


if __name__ == "__main__":
    main()
