#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path
from typing import TypedDict


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "third_party/compliance-manifest.json"


class Component(TypedDict):
    name: str
    slug: str
    license: str
    license_files: list[str]
    source_files: list[str]
    bundle_source: bool


class ComplianceManifest(TypedDict):
    schema_version: int
    components: list[Component]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy_material(relative: str, destination: Path) -> list[dict[str, str]]:
    source = ROOT / relative
    target = destination / source.name
    if source.is_dir():
        shutil.copytree(source, target, dirs_exist_ok=True, ignore=shutil.ignore_patterns(".git", "*.o", "*.d"))
        return [{"path": relative, "sha256": "directory"}]
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    return [{"path": relative, "sha256": sha256(source)}]


def package(output: Path) -> None:
    subprocess.run(["python3", str(ROOT / "tools/generate_lwip_notice.py"), "--write-repository"], cwd=ROOT, check=True)
    manifest: ComplianceManifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    records: list[dict[str, object]] = []
    for component in manifest["components"]:
        record: dict[str, object] = {
            "name": component["name"],
            "license": component["license"],
            "licenses": [],
            "sources": [],
        }
        for relative in component["license_files"]:
            record["licenses"].extend(copy_material(relative, output / "licenses" / component["slug"]))
        if component["bundle_source"]:
            for relative in component["source_files"]:
                record["sources"].extend(copy_material(relative, output / "sources" / component["slug"]))
        records.append(record)
    notices = output / "notices"
    notices.mkdir()
    shutil.copy2(ROOT / "LICENSE", notices / "LICENSE")
    shutil.copy2(ROOT / "THIRD_PARTY_NOTICES.md", notices / "THIRD_PARTY_NOTICES.md")
    (output / "MANIFEST.json").write_text(
        json.dumps({"schema_version": manifest["schema_version"], "components": records}, indent=2) + "\n",
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "out/compliance/third-party")
    arguments = parser.parse_args()
    package(arguments.output.resolve())


if __name__ == "__main__":
    main()
