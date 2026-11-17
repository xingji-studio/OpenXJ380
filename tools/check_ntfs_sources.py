#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the explicitly reviewed NTFS implementation and attribution inventory.

This is a source drift guard, not an authorship, complete link-closure or patent
clearance audit. New linked inputs require review even if a wrapper is permissive.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


DIRECTORY = Path("driver/fs/ntfs")
IMPLEMENTATION = {"native.cpp", "native.h", "names.cpp", "ntfs.cpp", "format.h", "journal.h", "journal.cpp", "write.h", "write.cpp", "recovery.h", "recovery.cpp", "recovery_actions.cpp", "mkfs.cpp", "mkfs.h"}
UPSTREAM_COMMIT = "ba4c809852a357378ab6058da5f514cceb8819c3"
INCLUDES = {
    "mkfs.cpp": {"mkfs.h", "errno.h"},
    "mkfs.h": {"stdint.h"},
    "native.cpp": {"native.h", "format.h", "errno.h"},
    "names.cpp": {"native.h", "errno.h"},
    "native.h": {"stdint.h"},
    "journal.h": {"native.h"},
    "journal.cpp": {"journal.h", "format.h", "recovery.h", "errno.h"},
    "write.h": {"journal.h", "recovery.h"},
    "write.cpp": {"write.h", "format.h", "errno.h"},
    "recovery.h": {"journal.h"},
    "recovery.cpp": {"recovery.h", "format.h", "errno.h"},
    "recovery_actions.cpp": {"recovery.h", "format.h", "errno.h"},
    "format.h": {"native.h", "errno.h"},
    "ntfs.cpp": {"native.h", "write.h", "rtc.h", "block_access.h", "device.h", "errno.h", "fs/ntfs/ntfs.h", "input_method.h",
                 "krlibc.h", "mutex.h", "task/scheduler.h", "user/permissions.h"},
}
UNREVIEWED = {".a", ".o", ".so", ".lib", ".dll", ".rlib", ".bc", ".c", ".cc", ".cxx",
              ".hpp", ".hh", ".inc", ".s", ".asm", ".rs", ".pdb", ".exe", ".sys", ".etl",
              ".iso", ".wim", ".img", ".vmdk"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    require(path.is_file() and not path.is_symlink(), f"missing or symlinked input: {path}")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check(root):
    directory = root / DIRECTORY
    manifest = json.loads((directory / "SOURCE.json").read_text())
    require(manifest["schema_version"] == 1 and manifest["license"] == "Apache-2.0", "unsupported manifest")
    require(manifest["upstream"]["commit"] == UPSTREAM_COMMIT, "upstream commit needs review")
    require(manifest["upstream"]["repository"] == "https://github.com/ViudiraTech/Uinxed-Kernel",
            "unreviewed upstream repository")
    require(not manifest["new_link_archives"], "new linked archive is not approved")
    profile = manifest["experimental_write_profile"]
    require(profile["metadata_mutation"] is True and profile["journal_replay"] is True and
            profile["allocation_and_directory_mutation"] is True and
            profile["profile_id"] == "bounded-lfs-windows-interop-v5", "unreviewed write/recovery scope")
    require(profile["windows_control_records"] == {
        "minimum_client_header": 40, "empty_redo_flag": 2, "empty_undo_flag": 4,
        "compensation_undo_exempt": True, "first_transaction_previous_lsn": 0,
        "terminal_undo_next_lsn": 0, "open_lsn": "preceding global LSN", "metadata_hint_unit": 512},
        "unreviewed Windows control-record scope")
    require(profile["native_profile_corruption_fallback"] is False, "native corruption may not bypass strict replay")
    require(profile["passive_root_initializer"] ==
            "Only the observed empty resident root $TXF_DATA initializer is retained byte for byte; "
            "active or unknown utility state remains unsupported", "unreviewed root utility mutation scope")
    require(profile["geometry"] == {
        "logical_sector": [512, 4096], "cluster": 4096, "mft_record": [1024, 4096], "index_buffer": 4096,
        "record_must_cover_logical_sector": True, "mirrored_records": 4, "usa_stride": 512},
        "unreviewed metadata geometry")
    require(set(manifest["sources"]) == IMPLEMENTATION, "unexpected implementation inventory")
    discovered = set()
    for path in directory.rglob("*"):
        require(not path.is_symlink(), f"symlink in NTFS inputs: {path}")
        if not path.is_file():
            continue
        if path.suffix in {".cpp", ".h"}:
            discovered.add(path.relative_to(directory).as_posix())
        require(path.suffix.lower() not in UNREVIEWED, f"unreviewed linked/source input: {path}")
    require(discovered == IMPLEMENTATION, "extra or missing NTFS source file")
    for name, expected in manifest["sources"].items():
        path = directory / name
        require(digest(path) == expected, f"source changed without review: {name}")
        source = path.read_text()
        require("SPDX-License-Identifier: Apache-2.0" in source[:512], f"missing Apache SPDX: {name}")
        require(not re.search(r"GNU\s+(?:(?:Lesser|Affero)\s+)?General\s+Public\s+License|"
                              r"\b(?:A|L)?GPL-[123]", source), f"copyleft source notice: {name}")
        source = re.sub(r"\\\r?\n", "", source)
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
        source = source.replace("%:", "#").replace("??=", "#")
        for match in re.finditer(r"(?m)^\s*#\s*(include(?:_next)?|import)\b([^\n]*)", source):
            require(match[1] == "include", f"unsupported include/import: {name}")
            literal = re.fullmatch(r'\s*[<"]([^>"\n]+)[>"]\s*', match[2])
            require(literal and literal[1] in INCLUDES[name], f"unreviewed include: {name}: {match[2]}")
    require(set(manifest["notices"]) == {"UINXED_LICENSE.txt", "UINXED_NOTICE.txt", "DISCUTILS_LICENSE.txt", "DISCUTILS_NOTICE.txt"},
            "missing or unreviewed upstream notice")
    for name, expected in manifest["notices"].items():
        require(digest(directory / name) == expected, f"upstream notice changed: {name}")
    require(digest(directory / "REFERENCES.json") == manifest["references_sha256"],
            "format reference inventory changed without review")
    references = json.loads((directory / "REFERENCES.json").read_text())
    reviewed = references["permissive_implementation_references"]
    require(manifest["additional_licenses"] == ["MIT"] and len(reviewed) == 1 and
            reviewed[0]["repository"] == "https://github.com/LTRData/DiscUtils" and
            reviewed[0]["commit"] == "b8c77f834ad0c43baf667b1fa7a8ff42c9e360b8" and
            reviewed[0]["license"] == "MIT", "unreviewed journal reference")
    public_header = root / "include/fs/ntfs/ntfs.h"
    require(digest(public_header) == manifest["public_header_sha256"], "public header changed without review")
    require(set(manifest["upstream"]["reviewed_files"]) == {
        "fs/ntfs/ntfs_runlist.c", "fs/ntfs/ntfs_mft.c"}, "upstream fragment inventory changed")
    require(re.search(r"using\s+Reader\s*=\s*bool\s*\(\s*\*\s*\)\s*\(",
                      (directory / "native.h").read_text()), "native reader interface changed")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        check(args.root)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"NTFS source policy: {error}\n")
    print("NTFS source policy: reviewed Apache-2.0/MIT P3 inputs and attribution match")


if __name__ == "__main__":
    main()
