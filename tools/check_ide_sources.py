#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the reviewed, first-party IDE implementation inventory."""
import hashlib
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "driver/ide"
MANIFEST = DRIVER / "SOURCE.json"


def fail(message):
    print("IDE source check failed:", message, file=sys.stderr)
    raise SystemExit(1)


def main():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != 1 or manifest.get("implementation") != "first-party-native-ide":
        fail("unexpected implementation declaration")
    if manifest.get("license") != "Apache-2.0":
        fail("implementation license must remain Apache-2.0")
    if manifest.get("new_external_implementations") != [] or manifest.get("new_link_archives") != []:
        fail("new external implementation or link archive requires a separate review")

    entries = manifest.get("sources")
    if not isinstance(entries, list) or {entry.get("path") for entry in entries} != {"ide.cpp"}:
        fail("the production implementation inventory must explicitly list ide.cpp only")
    actual = {path.name for path in DRIVER.iterdir() if path.suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}}
    if actual != {"ide.cpp"}:
        fail("unreviewed implementation input(s): " + ", ".join(sorted(actual - {"ide.cpp"})))

    source = DRIVER / "ide.cpp"
    content = source.read_bytes()
    entry = entries[0]
    if entry.get("spdx") != "Apache-2.0" or not content.startswith(b"// SPDX-License-Identifier: Apache-2.0"):
        fail("ide.cpp is missing its Apache-2.0 source notice")
    if entry.get("sha256") != hashlib.sha256(content).hexdigest():
        fail("ide.cpp changed without a reviewed inventory update")
    print("IDE source inventory passed: ide.cpp (Apache-2.0, SHA-256 verified)")


if __name__ == "__main__":
    main()
