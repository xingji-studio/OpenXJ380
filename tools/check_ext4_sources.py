#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the explicitly scoped first-party ext4 source inventory.

This checks driver source paths, hashes, SPDX notices, declared new external
inputs, and the isolated native parser's literal includes. It does not audit
the complete kernel compile/link closure or establish original authorship or
legal clearance. Existing kernel platform dependencies remain within their
previous review boundary; new linked dependencies require a separate review.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re


SOURCE_DIRECTORY = Path("driver/fs/ext4")
SOURCE_EXTENSIONS = {".c", ".cpp", ".h"}
UNREVIEWED_INPUT_EXTENSIONS = {
    ".a", ".o", ".obj", ".lib", ".so", ".dll", ".rlib", ".bc",
    ".cc", ".cxx", ".hh", ".hpp", ".inc", ".S", ".s", ".asm", ".rs", ".C",
}
NATIVE_SOURCES = {"native.cpp", "native.h"}
NATIVE_INCLUDES = {"native.h", "stdint.h", "errno.h"}
MANIFEST_KEYS = {
    "schema_version", "implementation", "license", "sources",
    "new_external_implementations", "new_link_archives",
}
SPDX = re.compile(r"(?m)^\s*(?://|/\*|\*)\s*SPDX-License-Identifier:\s*([^\r\n]+)")
COPYLEFT_HEADER = re.compile(
    r"GNU\s+(?:(?:Affero|Lesser)\s+)?General\s+Public\s+License|"
    r"(?:license[d]?\s+(?:under\s+)?|license\s*:\s*)(?:the\s+)?(?:A|L)?GPL\b|"
    r"\b(?:A|L)?GPL-[123](?:\.0)?(?:-or-later|-only)?\b",
    re.IGNORECASE,
)


class PolicyError(ValueError):
    """The source tree does not match the reviewed manifest and narrow policy."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PolicyError(message)


def unique_fields(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for name, value in pairs:
        require(name not in result, f"duplicate JSON field: {name}")
        result[name] = value
    return result


def check_native_includes(name: str, source: str) -> None:
    # Handle line splicing and C comments before examining preprocessor lines.
    # Include macros, include_next, import, and alternative directive spellings
    # are rejected instead of pretending to resolve a preprocessor dependency
    # closure. Even includes in inactive branches must obey this narrow policy.
    source = re.sub(r"\\[ \t]*(?:\r\n|\n)", "", source)
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.DOTALL)
    source = re.sub(r"//[^\n]*", "", source)
    source = source.replace("%:", "#").replace("??=", "#")
    directives = re.finditer(r"(?m)^\s*#\s*(include(?:_next)?|import)\b([^\n]*)", source)
    for match in directives:
        require(match[1] == "include", f"{name}: unsupported include/import directive")
        payload = match[2].strip()
        include = re.fullmatch(r'"([^"\n]+)"|<([^>\n]+)>', payload)
        require(include is not None, f"{name}: include must be one literal approved header")
        header = include[1] or include[2]
        require(header in NATIVE_INCLUDES, f"{name}: unapproved native include {header!r}")


def audit(root: Path) -> list[str]:
    directory = root / SOURCE_DIRECTORY
    require(directory.is_dir() and not directory.is_symlink(), "missing or symlinked ext4 source directory")
    manifest_path = directory / "SOURCE.json"
    require(not manifest_path.is_symlink(), "SOURCE.json must not be a symlink")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"), object_pairs_hook=unique_fields)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PolicyError(f"cannot read SOURCE.json: {error}") from error
    require(isinstance(manifest, dict) and set(manifest) == MANIFEST_KEYS,
            "SOURCE.json has missing or unknown fields")
    require(type(manifest["schema_version"]) is int and manifest["schema_version"] == 1,
            "unsupported source manifest schema_version")
    require(manifest["implementation"] == "first-party-native-ext4", "unapproved implementation identity")
    require(manifest["license"] == "Apache-2.0", "source manifest license must be Apache-2.0")
    require(manifest["new_external_implementations"] == [], "new external implementations are not approved")
    require(manifest["new_link_archives"] == [], "new link archives are not approved")
    require(isinstance(manifest["sources"], list) and manifest["sources"], "sources must be a nonempty list")

    entries = {}
    for entry in manifest["sources"]:
        require(isinstance(entry, dict) and set(entry) == {"path", "sha256", "spdx"},
                "each source needs exactly path, sha256, and spdx")
        name = entry["path"]
        require(isinstance(name, str) and bool(name) and "\\" not in name, "unsafe source path")
        relative = PurePosixPath(name)
        require(not relative.is_absolute() and ".." not in relative.parts and relative.as_posix() == name,
                f"unsafe source path: {name!r}")
        require(relative.suffix in SOURCE_EXTENSIONS, f"unsupported inventory source type: {name}")
        require(name not in entries, f"duplicate source entry: {name}")
        require(entry["spdx"] == "Apache-2.0", f"{name}: source SPDX must be Apache-2.0")
        digest = entry["sha256"]
        require(isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{64}", digest) is not None,
                f"{name}: invalid SHA-256")
        entries[name] = entry

    actual = {}
    for path in directory.rglob("*"):
        require(not path.is_symlink(), f"symlinked driver input: {path.relative_to(directory)}")
        if not path.is_file():
            continue
        name = path.relative_to(directory).as_posix()
        require(path.suffix not in UNREVIEWED_INPUT_EXTENSIONS,
                f"unreviewed implementation or binary input: {name}")
        if path.suffix in SOURCE_EXTENSIONS:
            actual[name] = path
    require(set(actual) == set(entries),
            f"source inventory differs: unlisted={sorted(set(actual) - set(entries))}, "
            f"missing={sorted(set(entries) - set(actual))}")
    require(NATIVE_SOURCES.issubset(actual), "native.cpp and native.h must remain explicitly inventoried")

    for name, path in sorted(actual.items()):
        data = path.read_bytes()
        require(hashlib.sha256(data).hexdigest() == entries[name]["sha256"], f"{name}: source hash changed")
        try:
            source = data.decode("utf-8")
        except UnicodeError as error:
            raise PolicyError(f"{name}: source must be UTF-8") from error
        identifiers = [value.removesuffix("*/").strip() for value in SPDX.findall(source)]
        require(identifiers == ["Apache-2.0"], f"{name}: expected one Apache-2.0 SPDX source notice")
        require(not COPYLEFT_HEADER.search(source[:8192]), f"{name}: copyleft notice in source header")
        if name in NATIVE_SOURCES:
            check_native_includes(name, source)
    return sorted(actual)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1],
                        help="OpenXJ380 provider root (default: this script's repository)")
    arguments = parser.parse_args()
    try:
        sources = audit(arguments.root)
    except (PolicyError, OSError) as error:
        parser.exit(1, f"ext4 source policy failed: {error}\n")
    print(f"ext4 source policy: {len(sources)} inventoried Apache-2.0 sources; "
          "no declared new external implementations or archives (scoped source check)")


if __name__ == "__main__":
    main()
