#!/usr/bin/env python3
"""Host-side FAT32/ext4 image formatter; this does not provide a kernel filesystem driver.

The OS build currently permits FAT32 only. ext4 output is retained for disposable
format tests and future driver development, and is not a bootable XJ380 system.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


SECTOR_SIZE = 512
MIB = 1024 * 1024
SECTORS_PER_MIB = MIB // SECTOR_SIZE
BOOT_FILES = ("EFI/BOOT/BOOTX64.EFI", "system/kernel.krl", "system/activept.cfk")

# Keep test images independent of host mke2fs.conf feature defaults. A future
# kernel driver must establish its own supported feature set before activation.
EXT4_FEATURES = (
    "none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,64bit,"
    "flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize,metadata_csum"
)


def positive_integer(value: str) -> int:
    try:
        number = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected a positive integer") from error
    if number <= 0:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return number


def partition_layout(filesystem: str, size_mib: int, esp_mib: int = 64,
                     fat_mib: int | None = None) -> list[tuple[str, int, int, str]]:
    """Return (name, first LBA, last LBA, GPT type), reserving both GPT copies."""
    if filesystem not in ("fat32", "ext4"):
        raise ValueError(f"unsupported filesystem: {filesystem}")
    if size_mib <= 2:
        raise ValueError("image size must leave space for partitions and GPT headers")
    first = SECTORS_PER_MIB
    # One MiB at either end keeps every partition MiB-aligned and leaves the
    # backup GPT outside the filesystem. No mkfs command can extend the disk.
    end = (size_mib - 1) * SECTORS_PER_MIB - 1
    if filesystem == "fat32":
        capacity = size_mib - 2
        length = capacity if fat_mib is None else fat_mib
        if length < 64 or length > capacity:
            raise ValueError(f"FAT32 size must be between 64 and {capacity} MiB")
        return [("ESP", first, first + length * SECTORS_PER_MIB - 1, "ef00")]
    if esp_mib < 64:
        raise ValueError("FAT32 EFI partition must be at least 64 MiB")
    if size_mib - esp_mib - 2 < 32:
        raise ValueError("ext4 root needs at least 32 MiB in addition to the EFI partition and GPT space")
    esp_first = (size_mib - esp_mib - 1) * SECTORS_PER_MIB
    # The kernel probes GPT entries in order. Root must precede the ESP because
    # both partitions need the kernel and marker for their respective loaders.
    return [("XJ380", first, esp_first - 1, "8300"), ("ESP", esp_first, end, "ef00")]


def run(arguments: list[str], sudo: list[str] | None = None) -> None:
    subprocess.run([*(sudo or []), *map(str, arguments)], check=True)


def create_file(path: Path, size: int) -> None:
    with path.open("wb") as stream:
        stream.truncate(size)


def copy_partition(source: Path, destination: Path, offset: int) -> None:
    """Copy bounded partition bytes while leaving zero ranges sparse on the host."""
    with source.open("rb") as incoming, destination.open("r+b") as outgoing:
        outgoing.seek(offset)
        while chunk := incoming.read(4 * MIB):
            if chunk.count(0) == len(chunk):
                outgoing.seek(len(chunk), os.SEEK_CUR)
            else:
                outgoing.write(chunk)


def build_image(root: Path, image: Path, *, filesystem: str = "fat32", size_mib: int = 3072,
                esp_mib: int = 64, fat_mib: int | None = None, sudo: str = "") -> None:
    root, image = root.resolve(), image.absolute()
    layout = partition_layout(filesystem, size_mib, esp_mib, fat_mib)
    if not root.is_dir():
        raise ValueError(f"staged system directory does not exist: {root}")
    if image.is_symlink() or (image.exists() and not image.is_file()):
        raise ValueError(f"output must be a regular image file: {image}")
    if image.is_relative_to(root):
        raise ValueError("image output must be outside the staged system directory")
    for name in BOOT_FILES:
        if not (root / name).is_file():
            raise ValueError(f"staged system is missing {name}")
    required = ["sgdisk", "mkfs.vfat", "mcopy"]
    if filesystem == "ext4":
        required += ["mke2fs", "e2fsck"]
    missing = [tool for tool in required if not shutil.which(tool)]
    if missing:
        raise ValueError("missing image tools: " + ", ".join(missing))
    sudo_command = shlex.split(sudo)
    image.parent.mkdir(parents=True, exist_ok=True)
    # Build beside the destination and replace it only after every format,
    # population, and filesystem check succeeds. A failure preserves old images.
    with tempfile.TemporaryDirectory(prefix=f".{image.name}-", dir=image.parent) as directory:
        work = Path(directory)
        disk = work / "disk.img"
        create_file(disk, size_mib * MIB)
        command = ["sgdisk", "--clear"]
        for index, (name, first, last, kind) in enumerate(layout, 1):
            command += [f"--new={index}:{first}:{last}", f"--typecode={index}:{kind}",
                        f"--change-name={index}:{name}"]
        run([*command, str(disk)], sudo_command)

        boot_root = root
        if filesystem == "ext4":
            boot_root = work / "esp-root"
            for name in BOOT_FILES:
                destination = boot_root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(root / name, destination)
            first, last = layout[0][1:3]
            partition = work / "root.img"
            create_file(partition, (last - first + 1) * SECTOR_SIZE)
            run(["mke2fs", "-q", "-F", "-t", "ext4", "-b", "4096", "-I", "256", "-L", "XJ380",
                 "-O", EXT4_FEATURES, "-E", "lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0",
                 "-d", str(root), str(partition)], sudo_command)
            run(["e2fsck", "-fn", str(partition)], sudo_command)
            copy_partition(partition, disk, first * SECTOR_SIZE)

        first, last = layout[-1][1:3]
        partition = work / "esp.img"
        create_file(partition, (last - first + 1) * SECTOR_SIZE)
        run(["mkfs.vfat", "-F", "32", "-S", "512", "-n", "XJ380BOOT", str(partition)], sudo_command)
        entries = [str(entry) for entry in sorted(boot_root.iterdir())]
        run(["mcopy", "-o", "-s", "-i", str(partition), *entries, "::"])
        copy_partition(partition, disk, first * SECTOR_SIZE)
        run(["sgdisk", "--verify", str(disk)], sudo_command)
        os.replace(disk, image)

    print(f"Created {image} ({filesystem}, {size_mib} MiB)", flush=True)
    for index, (name, first, last, _) in enumerate(layout, 1):
        print(f"  Partition {index}: {name}, {(last - first + 1) * SECTOR_SIZE // MIB} MiB", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--fs", choices=("fat32", "ext4"), default="fat32")
    parser.add_argument("--size-mib", type=positive_integer, default=3072)
    parser.add_argument("--esp-mib", type=positive_integer, default=64)
    parser.add_argument("--fat-mib", type=positive_integer)
    parser.add_argument("--sudo", default="")
    args = parser.parse_args()
    try:
        build_image(args.root, args.image, filesystem=args.fs, size_mib=args.size_mib,
                    esp_mib=args.esp_mib, fat_mib=args.fat_mib, sudo=args.sudo)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"image build failed: {error}\n")


if __name__ == "__main__":
    main()
