#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Create FAT32 or GPT-bound ext4/FAT32 images using separate host-tool processes.

This formatter does not change the staged tree or import filesystem implementations.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import uuid
import zlib


SECTOR_SIZE = 512
MIB = 1024 * 1024
SECTORS_PER_MIB = MIB // SECTOR_SIZE
BOOT_FILES = ("EFI/BOOT/BOOTX64.EFI", "system/kernel.krl", "system/activept.cfk")
ESP_BOOT_FILES = BOOT_FILES[:2]
ROOT_BINDING_PATH = "EFI/XJ380/root.bin"
ROOT_BINDING_SIZE = 96
# Explicit tested host-image profile. At 4 KiB/32768-block groups this admits
# 1 TiB while leaving the runtime writer's separate 256 MiB workspace budget
# ample headroom. Runtime storage admission still checks the actual geometry.
NATIVE_MAX_ROOT_BYTES = 1 << 40
NATIVE_MAX_GROUPS = NATIVE_MAX_ROOT_BYTES // (32768 * 4096)
ESP_TYPE_GUID = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
ROOT_TYPE_GUID = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")

# Keep the already-tested modern feature set independent of host defaults.
EXT4_FEATURES = (
    "none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,64bit,"
    "flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize,metadata_csum"
)


def _crc32c_table() -> tuple[int, ...]:
    values = []
    for value in range(256):
        for _ in range(8):
            value = (value >> 1) ^ (0x82f63b78 if value & 1 else 0)
        values.append(value)
    return tuple(values)


CRC32C_TABLE = _crc32c_table()


def crc32c(data: bytes) -> int:
    """Standard reflected CRC32C, including initial and final all-ones XOR."""
    value = 0xffffffff
    for byte in data:
        value = CRC32C_TABLE[(value ^ byte) & 255] ^ (value >> 8)
    return value ^ 0xffffffff


def file_fingerprint(path: Path) -> tuple[int, int]:
    size, value = 0, 0xffffffff
    with path.open("rb") as stream:
        while chunk := stream.read(4 * MIB):
            size += len(chunk)
            for byte in chunk:
                value = CRC32C_TABLE[(value ^ byte) & 255] ^ (value >> 8)
    return size, value ^ 0xffffffff


def root_binding(disk_guid: uuid.UUID, root_guid: uuid.UUID, esp_guid: uuid.UUID,
                 filesystem_uuid: uuid.UUID, kernel_size: int, kernel_crc32c: int) -> bytes:
    identities = (disk_guid, root_guid, esp_guid, filesystem_uuid)
    if any(not isinstance(value, uuid.UUID) or not value.int for value in identities) or root_guid == esp_guid:
        raise ValueError("root binding needs nonzero disk, root, ESP and filesystem identities")
    if not 0 < kernel_size <= 0xffffffffffffffff or not 0 <= kernel_crc32c <= 0xffffffff:
        raise ValueError("root binding kernel size/checksum is outside its on-disk range")
    prefix = struct.pack("<8sII16s16s16s16sQI", b"XJROOT01", 1, ROOT_BINDING_SIZE,
                         disk_guid.bytes_le, root_guid.bytes_le, esp_guid.bytes_le,
                         filesystem_uuid.bytes, kernel_size, kernel_crc32c)
    return prefix + struct.pack("<I", crc32c(prefix))


def parse_root_binding(data: bytes) -> dict:
    if len(data) != ROOT_BINDING_SIZE:
        raise ValueError("root binding must contain exactly 96 bytes")
    magic, version, size, disk, root, esp, filesystem, kernel_size, kernel_crc, checksum = struct.unpack(
        "<8sII16s16s16s16sQII", data)
    if magic != b"XJROOT01" or version != 1 or size != ROOT_BINDING_SIZE or crc32c(data[:92]) != checksum:
        raise ValueError("invalid root binding header/checksum")
    result = {"disk_guid": uuid.UUID(bytes_le=disk), "root_guid": uuid.UUID(bytes_le=root),
              "esp_guid": uuid.UUID(bytes_le=esp), "filesystem_uuid": uuid.UUID(bytes=filesystem),
              "kernel_size": kernel_size, "kernel_crc32c": kernel_crc}
    # Reuse the field-level checks without assuming UUID generation versions.
    root_binding(result["disk_guid"], result["root_guid"], result["esp_guid"], result["filesystem_uuid"],
                 kernel_size, kernel_crc)
    return result


def journal_size_mib(root_mib: int) -> int:
    return min(32, max(4, root_mib // 64))


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
    # Retain the established physical layout. The binding selects identities,
    # independently of labels, partition order, and legacy marker discovery.
    return [("XJ380", first, esp_first - 1, "8300"), ("ESP", esp_first, end, "ef00")]


def run(arguments: list[str], sudo: list[str] | None = None) -> None:
    subprocess.run([*(sudo or []), *map(str, arguments)], check=True)


def create_file(path: Path, size: int) -> None:
    with path.open("wb") as stream:
        stream.truncate(size)


def copy_partition(source: Path, destination: Path, offset: int) -> None:
    """Copy bounded partition bytes while leaving zero ranges sparse on the host."""
    if offset < 0 or source.stat().st_size > destination.stat().st_size - offset:
        raise ValueError("partition extends beyond the disk image")
    with source.open("rb") as incoming, destination.open("r+b") as outgoing:
        outgoing.seek(offset)
        while chunk := incoming.read(4 * MIB):
            if chunk.count(0) == len(chunk):
                outgoing.seek(len(chunk), os.SEEK_CUR)
            else:
                outgoing.write(chunk)


def verify_gpt(path: Path, layout: list, identities: dict) -> None:
    """Check both GPT CRCs and the actual identities used by the descriptor."""
    sectors = path.stat().st_size // SECTOR_SIZE
    tables = []
    with path.open("rb") as stream:
        for lba in (1, sectors - 1):
            stream.seek(lba * SECTOR_SIZE)
            header = bytearray(stream.read(SECTOR_SIZE))
            if header[:8] != b"EFI PART":
                raise ValueError("missing GPT header")
            size, checksum = struct.unpack_from("<II", header, 12)
            if not 92 <= size <= SECTOR_SIZE:
                raise ValueError("invalid GPT header size")
            header[16:20] = bytes(4)
            if zlib.crc32(header[:size]) & 0xffffffff != checksum:
                raise ValueError("invalid GPT header checksum")
            current, alternate = struct.unpack_from("<QQ", header, 24)
            if current != lba or alternate != (sectors - 1 if lba == 1 else 1):
                raise ValueError("invalid GPT header locations")
            if header[56:72] != identities["disk"].bytes_le:
                raise ValueError("GPT disk GUID does not match root binding")
            table_lba, count, width, table_checksum = struct.unpack_from("<QIII", header, 72)
            if not 1 <= count <= 128 or not 128 <= width <= 512 or width % 128:
                raise ValueError("unsupported GPT entry geometry")
            if table_lba >= sectors or count * width > (sectors - table_lba) * SECTOR_SIZE:
                raise ValueError("GPT entries extend beyond disk")
            stream.seek(table_lba * SECTOR_SIZE)
            table = stream.read(count * width)
            if zlib.crc32(table) & 0xffffffff != table_checksum:
                raise ValueError("invalid GPT partition-table checksum")
            tables.append(table)
            active = [table[offset:offset + width] for offset in range(0, len(table), width)
                      if any(table[offset:offset + 16])]
            if len(active) != len(layout):
                raise ValueError("unexpected GPT partition count")
            for entry, (name, first, last, kind) in zip(active, layout):
                identity = identities["esp" if kind == "ef00" else "root"]
                expected_type = ESP_TYPE_GUID if kind == "ef00" else ROOT_TYPE_GUID
                if entry[:16] != expected_type.bytes_le or entry[16:32] != identity.bytes_le or \
                        struct.unpack_from("<QQ", entry, 32) != (first, last):
                    raise ValueError(f"GPT partition identity/range mismatch: {name}")
    if tables[0] != tables[1]:
        raise ValueError("primary and backup GPT entries disagree")


def verify_ext4(partition: Path, filesystem_uuid: uuid.UUID, journal_mib: int) -> None:
    with partition.open("rb") as stream:
        stream.seek(1024)
        superblock = stream.read(1024)
        u16 = lambda offset: struct.unpack_from("<H", superblock, offset)[0]
        u32 = lambda offset: struct.unpack_from("<I", superblock, offset)[0]
        if u16(56) != 0xef53 or u32(24) != 2 or superblock[104:120] != filesystem_uuid.bytes:
            raise ValueError("ext4 format/UUID does not match root binding")
        if (u32(92), u32(96), u32(100)) != (0x3c, 0x2c2, 0x46b):
            raise ValueError("host mkfs changed the reviewed ext4 feature set")
        blocks = u32(4) | (u32(0x150) << 32)
        per_group, inodes_per_group, inode_size, descriptor_size = u32(32), u32(40), u16(88), u16(254)
        if not per_group or per_group > 32768 or (blocks + per_group - 1) // per_group > NATIVE_MAX_GROUPS or \
                blocks * 4096 > NATIVE_MAX_ROOT_BYTES or blocks * 4096 != partition.stat().st_size or \
                not 1 <= inodes_per_group <= 32768 or inode_size != 256 or not 64 <= descriptor_size <= 4096:
            raise ValueError("ext4 geometry exceeds native driver bounds")
        # Journal inode 8 is reserved by the formatter. Read its declared size
        # directly; e2fsck separately validates its complete mapping and CRCs.
        group, index = divmod(7, inodes_per_group)
        stream.seek(4096 + group * descriptor_size)
        descriptor = stream.read(descriptor_size)
        table = struct.unpack_from("<I", descriptor, 8)[0] | (struct.unpack_from("<I", descriptor, 40)[0] << 32)
        stream.seek(table * 4096 + index * inode_size)
        inode = stream.read(inode_size)
        journal_size = struct.unpack_from("<I", inode, 4)[0] | (struct.unpack_from("<I", inode, 108)[0] << 32)
        if journal_size != journal_mib * MIB or journal_size % 4096 or not 1024 <= journal_size // 4096 <= 32768:
            raise ValueError("ext4 journal size exceeds native driver bounds")


def verify_fat32(partition: Path) -> None:
    size = partition.stat().st_size
    with partition.open("rb") as stream:
        boot = stream.read(512)
    if len(boot) != 512 or size % SECTOR_SIZE or boot[510:512] != b"\x55\xaa":
        raise ValueError("invalid FAT32 EFI partition geometry")
    sector_size = struct.unpack_from("<H", boot, 11)[0]
    cluster_sectors = boot[13]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fats = boot[16]
    total = struct.unpack_from("<I", boot, 32)[0]
    fat_sectors = struct.unpack_from("<I", boot, 36)[0]
    flags, version = struct.unpack_from("<HH", boot, 40)
    root_cluster = struct.unpack_from("<I", boot, 44)[0]
    fsinfo, backup_boot = struct.unpack_from("<HH", boot, 48)
    # BPB_TotSec32 describes the filesystem, not the containing partition.
    # mkfs.fat may leave a final partial track unused. Microsoft FAT32 format
    # facts allow a smaller filesystem, but never one extending past its media.
    if sector_size != SECTOR_SIZE or not cluster_sectors or cluster_sectors > 128 or \
            cluster_sectors & (cluster_sectors - 1) or not reserved or not fats or \
            struct.unpack_from("<H", boot, 17)[0] or struct.unpack_from("<H", boot, 19)[0] or \
            struct.unpack_from("<H", boot, 22)[0] or not total or total > size // SECTOR_SIZE or \
            not fat_sectors or version or flags & ~0x008f or (flags & 0x0080 and (flags & 15) >= fats):
        raise ValueError("invalid FAT32 EFI partition geometry")
    data_start = reserved + fats * fat_sectors
    if data_start >= total:
        raise ValueError("invalid FAT32 FAT/data region bounds")
    clusters = (total - data_start) // cluster_sectors
    # FAT32 reserves table entries zero and one. Every complete data cluster
    # must have an entry, and its number must stay below reserved 28-bit IDs.
    if clusters < 65525 or clusters + 1 >= 0x0ffffff0 or fat_sectors * SECTOR_SIZE // 4 < clusters + 2 or \
            not 2 <= root_cluster <= clusters + 1 or \
            data_start + (root_cluster - 1) * cluster_sectors > total:
        raise ValueError("invalid FAT32 cluster/table/root bounds")
    if (fsinfo != 0xffff and not 0 < fsinfo < reserved) or \
            (backup_boot not in (0, 0xffff) and not 0 < backup_boot < reserved):
        raise ValueError("invalid FAT32 reserved-sector references")


def require_staged_file(root: Path, name: str) -> None:
    path = root
    for component in Path(name).parts:
        path /= component
        if path.is_symlink():
            raise ValueError(f"boot/configuration input must not traverse symlinks: {name}")
    if not path.is_file():
        raise ValueError(f"staged system is missing {name}")


def build_image(root: Path, image: Path, *, filesystem: str = "fat32", size_mib: int = 3072,
                esp_mib: int = 64, fat_mib: int | None = None, sudo: str = "") -> None:
    root = root.resolve()
    if image.is_symlink():
        raise ValueError(f"output must be a regular image file: {image}")
    image = image.parent.resolve() / image.name
    layout = partition_layout(filesystem, size_mib, esp_mib, fat_mib)
    if not root.is_dir():
        raise ValueError(f"staged system directory does not exist: {root}")
    if image.is_symlink() or (image.exists() and not image.is_file()):
        raise ValueError(f"output must be a regular image file: {image}")
    if image.is_relative_to(root):
        raise ValueError("image output must be outside the staged system directory")
    for name in BOOT_FILES:
        require_staged_file(root, name)
    if filesystem == "ext4":
        require_staged_file(root, "config/README.md")
        if (layout[0][2] - layout[0][1] + 1) * SECTOR_SIZE > NATIVE_MAX_ROOT_BYTES:
            raise ValueError("ext4 root exceeds the reviewed 1 TiB host-image profile")
    required = ["sgdisk", "mkfs.vfat", "mcopy"]
    if filesystem == "ext4":
        required += ["mke2fs", "e2fsck", "debugfs"]
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
        identities = {name: uuid.uuid4() for name in ("disk", "root", "esp", "filesystem")}
        if len(set(identities.values())) != 4 or any(not value.int for value in identities.values()):
            raise ValueError("image identities must be distinct and nonzero")
        command = ["sgdisk", "--clear", f"--disk-guid={identities['disk']}"]
        for index, (name, first, last, kind) in enumerate(layout, 1):
            identity = identities["esp" if kind == "ef00" else "root"]
            command += [f"--new={index}:{first}:{last}", f"--typecode={index}:{kind}",
                        f"--change-name={index}:{name}", f"--partition-guid={index}:{identity}"]
        run([*command, str(disk)], sudo_command)

        boot_root = root
        if filesystem == "ext4":
            boot_root = work / "esp-root"
            for name in ESP_BOOT_FILES:
                destination = boot_root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(root / name, destination)
            kernel_size, kernel_crc = file_fingerprint(boot_root / "system/kernel.krl")
            binding = root_binding(identities["disk"], identities["root"], identities["esp"],
                                   identities["filesystem"], kernel_size, kernel_crc)
            descriptor = boot_root / ROOT_BINDING_PATH
            descriptor.parent.mkdir(parents=True, exist_ok=True)
            descriptor.write_bytes(binding)
            first, last = layout[0][1:3]
            partition = work / "root.img"
            create_file(partition, (last - first + 1) * SECTOR_SIZE)
            journal_mib = journal_size_mib((last - first + 1) * SECTOR_SIZE // MIB)
            run(["mke2fs", "-q", "-F", "-t", "ext4", "-b", "4096", "-I", "256", "-L", "XJ380",
                 "-U", str(identities["filesystem"]), "-J", f"size={journal_mib}",
                 "-O", EXT4_FEATURES, "-E", "lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0",
                 "-d", str(root), str(partition)], sudo_command)
            run(["e2fsck", "-fn", str(partition)], sudo_command)
            verify_ext4(partition, identities["filesystem"], journal_mib)
            # Verify what mkfs actually imported, rather than rereading a staged
            # file that could have changed between the ESP snapshot and mkfs.
            root_kernel = work / "root-kernel.bin"
            quoted_output = str(root_kernel).replace("\\", "\\\\").replace('"', '\\"')
            run(["debugfs", "-R", f'dump /system/kernel.krl "{quoted_output}"', str(partition)], sudo_command)
            if not root_kernel.is_file() or file_fingerprint(root_kernel) != (kernel_size, kernel_crc):
                raise ValueError("ext4 root and EFI partition contain different kernels")
            copy_partition(partition, disk, first * SECTOR_SIZE)

        first, last = layout[-1][1:3]
        partition = work / "esp.img"
        create_file(partition, (last - first + 1) * SECTOR_SIZE)
        run(["mkfs.vfat", "-F", "32", "-S", "512", "-n", "XJ380BOOT", str(partition)], sudo_command)
        verify_fat32(partition)
        entries = [str(entry) for entry in sorted(boot_root.iterdir())]
        run(["mcopy", "-o", "-s", "-i", str(partition), *entries, "::"])
        if filesystem == "ext4":
            esp_binding = work / "readback-binding.bin"
            esp_kernel = work / "readback-kernel.bin"
            run(["mcopy", "-i", str(partition), f"::/{ROOT_BINDING_PATH}", str(esp_binding)])
            run(["mcopy", "-i", str(partition), "::/system/kernel.krl", str(esp_kernel)])
            if esp_binding.read_bytes() != binding or file_fingerprint(esp_kernel) != (kernel_size, kernel_crc):
                raise ValueError("EFI partition root binding/kernel readback differs")
        copy_partition(partition, disk, first * SECTOR_SIZE)
        run(["sgdisk", "--verify", str(disk)], sudo_command)
        verify_gpt(disk, layout, identities)
        with disk.open("r+b") as stream:
            stream.flush()
            os.fsync(stream.fileno())
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
