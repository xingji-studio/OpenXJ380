#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the scoped ISO 9660 implementation inventory before generating a build.

Hashes detect unreviewed source drift; they do not prove authorship or audit
the licenses of the entire pre-existing kernel/dependency graph.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


DIRECTORY = Path("driver/fs/iso9660")
INCLUDES = {
    "native.h": {"rock_ridge.h"},
    "native.cpp": {"native.h", "errno.h"},
    "rock_ridge.h": {"stdint.h"},
    "rock_ridge.cpp": {"rock_ridge.h", "errno.h"},
    "iso9660.cpp": {"native.h", "block_access.h", "device.h", "efi/boot.h", "errno.h",
                    "fs/iso9660/iso9660.h", "input_method.h", "krlibc.h", "mutex.h",
                    "syscall/syscall.h", "task/scheduler.h", "user/permissions.h"},
}
PUBLIC_HEADER = Path("include/fs/iso9660/iso9660.h")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check(root):
    directory = root / DIRECTORY
    manifest = json.loads((directory / "SOURCE.json").read_text())
    require(manifest["license"] == "Apache-2.0" and manifest["imported_implementations"] == [],
            "unreviewed license or imported implementation")
    actual = {path.name for path in directory.iterdir()
              if path.suffix in {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inc", ".S", ".asm", ".a", ".o"}}
    require(actual == set(INCLUDES), "unreviewed source or link input")
    expected = {str(DIRECTORY / name) for name in INCLUDES} | {str(PUBLIC_HEADER)}
    require(set(manifest["files"]) == expected, "implementation inventory changed")
    includes = {**INCLUDES, str(PUBLIC_HEADER): {"fs/vfs/vfs.h"}}
    for relative, checksum in manifest["files"].items():
        path = root / relative
        require(digest(path) == checksum, "source changed without inventory review: " + relative)
        source = path.read_text()
        require(re.findall(r"SPDX-License-Identifier:\s*([^\r\n]+)", source) == ["Apache-2.0"],
                "unreviewed source license: " + relative)
        source = re.sub(r"\\\r?\n", "", source)
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
        allowed = includes[str(PUBLIC_HEADER) if relative == str(PUBLIC_HEADER) else path.name]
        for match in re.finditer(r"(?m)^\s*#\s*(include(?:_next)?|import)\b([^\n]*)", source):
            literal = re.fullmatch(r'\s*[<"]([^>"\n]+)[>"]\s*', match[2])
            require(match[1] == "include" and literal and literal[1] in allowed,
                    "unreviewed include: " + relative)
    for name in ("REFERENCES.json", "PROVENANCE.md"):
        require(digest(directory / name) == manifest["documentation"][name], "provenance changed: " + name)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        check(args.root)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"ISO9660 source policy: {error}\n")
    print("ISO9660 source policy: original Apache-2.0 inputs and attribution match")


if __name__ == "__main__":
    main()
