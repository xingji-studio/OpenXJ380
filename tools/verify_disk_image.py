#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Read back required boot/system files from GPT ext4 or legacy FAT32 images."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import uuid

if __package__:
    from . import make_disk_image as images
else:
    import make_disk_image as images


ROOT_FILES = (
    "system/kernel.krl",
    "apps/system/shell/shell.elf",
    "system/mod/e1000.sys",
    "system/mod/netserver.sys",
    "system/licenses/third-party/MANIFEST.json",
)


def read_partitions(image: Path) -> dict:
    """Locate filesystems by GPT type, then check both copies with the formatter's verifier."""
    size = image.stat().st_size
    if size < 68 * images.SECTOR_SIZE or size % images.SECTOR_SIZE:
        raise ValueError("image is empty, truncated, or not sector aligned")
    with image.open("rb") as stream:
        stream.seek(images.SECTOR_SIZE)
        header = stream.read(images.SECTOR_SIZE)
        if header[:8] != b"EFI PART":
            raise ValueError("missing GPT header")
        table_lba, count, width = struct.unpack_from("<QII", header, 72)
        if not 1 <= count <= 128 or not 128 <= width <= 512 or width % 128:
            raise ValueError("unsupported GPT entry geometry")
        if table_lba * images.SECTOR_SIZE + count * width > size:
            raise ValueError("GPT entries extend beyond disk")
        stream.seek(table_lba * images.SECTOR_SIZE)
        table = stream.read(count * width)
    first_usable, last_usable = struct.unpack_from("<QQ", header, 40)
    identities = {"disk": uuid.UUID(bytes_le=header[56:72])}
    partitions, layout = {}, []
    kinds = {images.ESP_TYPE_GUID: ("esp", "ef00"), images.ROOT_TYPE_GUID: ("root", "8300")}
    for offset in range(0, len(table), width):
        entry = table[offset:offset + width]
        if not any(entry[:16]):
            continue
        kind = uuid.UUID(bytes_le=entry[:16])
        if kind not in kinds:
            raise ValueError(f"unsupported image partition type: {kind}")
        name, code = kinds[kind]
        if name in partitions:
            raise ValueError(f"multiple {name} partitions in image")
        first, last = struct.unpack_from("<QQ", entry, 32)
        if not 2 <= first_usable <= first <= last <= last_usable < size // images.SECTOR_SIZE - 1:
            raise ValueError(f"invalid {name} partition range")
        partitions[name] = (first, last)
        identities[name] = uuid.UUID(bytes_le=entry[16:32])
        layout.append((name, first, last, code))
    if "esp" not in partitions:
        raise ValueError("image has no EFI system partition")
    if "root" in partitions:
        root, esp = partitions["root"], partitions["esp"]
        if max(root[0], esp[0]) <= min(root[1], esp[1]):
            raise ValueError("root and EFI partitions overlap")
    images.verify_gpt(image, layout, identities)
    return partitions


def require_file(path: Path, name: str) -> None:
    if not path.is_file() or not path.stat().st_size:
        raise ValueError(f"missing or empty image file: /{name}")


def fat_file(image: Path, first: int, name: str, destination: Path) -> None:
    subprocess.run(["mcopy", "-i", f"{image}@@{first * images.SECTOR_SIZE}", f"::/{name}", str(destination)],
                   check=True, capture_output=True, text=True)
    require_file(destination, name)


def extract_partition(image: Path, partition: tuple[int, int], destination: Path) -> None:
    """Copy only the root partition and preserve zero ranges as sparse host-file holes."""
    first, last = partition
    remaining = (last - first + 1) * images.SECTOR_SIZE
    with image.open("rb") as source, destination.open("wb") as target:
        source.seek(first * images.SECTOR_SIZE)
        target.truncate(remaining)
        while remaining:
            chunk = source.read(min(4 * images.MIB, remaining))
            if not chunk:
                raise ValueError("truncated root partition")
            if chunk.count(0) == len(chunk):
                target.seek(len(chunk), 1)
            else:
                target.write(chunk)
            remaining -= len(chunk)


def file_digest(path: Path) -> bytes:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(4 * images.MIB):
            digest.update(chunk)
    return digest.digest()


def verify_image(image: Path) -> None:
    image = image.resolve()
    partitions = read_partitions(image)
    with tempfile.TemporaryDirectory(prefix="xj380-verify-image-") as directory:
        work = Path(directory)
        for index, name in enumerate(images.ESP_BOOT_FILES):
            destination = work / f"esp-{index}"
            fat_file(image, partitions["esp"][0], name, destination)
            if name == "system/kernel.krl":
                esp_kernel = file_digest(destination)
        if "root" in partitions:
            root = work / "root.img"
            extract_partition(image, partitions["root"], root)
        for index, name in enumerate(ROOT_FILES):
            destination = work / f"root-{index}"
            if "root" in partitions:
                # debugfs can exit successfully when a file is missing. Check
                # that each fresh destination actually exists and has content.
                quoted_output = str(destination).replace("\\", "\\\\").replace('"', '\\"')
                subprocess.run(["debugfs", "-R", f'dump "/{name}" "{quoted_output}"', str(root)],
                               check=True, capture_output=True, text=True)
                require_file(destination, name)
            else:
                fat_file(image, partitions["esp"][0], name, destination)
            if name == "system/kernel.krl" and file_digest(destination) != esp_kernel:
                raise ValueError("root and EFI partition contain different kernels")
    filesystem = "ext4 root + FAT32 ESP" if "root" in partitions else "FAT32"
    print(f"Verified {image}: {filesystem}, boot files and required system files")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    try:
        verify_image(args.image)
    except subprocess.CalledProcessError as error:
        parser.exit(1, f"image verification failed: {error}\n{error.stderr or ''}")
    except (OSError, ValueError) as error:
        parser.exit(1, f"image verification failed: {error}\n")


if __name__ == "__main__":
    main()
