#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build a validated installer package from a frozen staging tree.

The historical two positional arguments still emit PAK1. Use --format pak2
explicitly for metadata/link preservation; current installers reject PAK2 until
an importer with those semantics is available. No archive library is linked.
"""
from dataclasses import dataclass, field
import argparse
import os
from pathlib import Path
import stat
import struct
import tempfile
import sys

MAGIC = b"XJPAK20\0"
LEGACY_MAGIC = b"XJPAK10\0"
MAX_ENTRIES = 262144
LEGACY_MAX_PATH = 511
REGULAR, DIRECTORY, SYMLINK, HARDLINK = 1, 2, 3, 4
MAX_BYTES = 8 << 30
MAX_PATH = 4095


def crc32c(data):
    table = []
    for value in range(256):
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
        table.append(value)
    value = 0xFFFFFFFF
    for byte in data:
        value = table[(value ^ byte) & 255] ^ (value >> 8)
    return value ^ 0xFFFFFFFF


def canonical(path, maximum=MAX_PATH):
    if not path or len(path) > maximum or not path.startswith(b"/") or b"\0" in path:
        raise ValueError("invalid absolute entry path")
    if path != b"/":
        components = path[1:].split(b"/")
        if len(components) > 128 or any(not p or p in (b".", b"..") or len(p) > 255 for p in components):
            raise ValueError("entry path is not canonical")


def encode_xattrs(attributes, allow_trusted=False, allow_security=False):
    output, storage, previous = bytearray(), 0, None
    if len(attributes) > 32:
        raise ValueError("xattr count exceeds installer profile")
    for name, value in sorted(attributes):
        if not name or len(name) > 255 or b"\0" in name or name == previous:
            raise ValueError("invalid/duplicate xattr name")
        previous = name
        if not (name.startswith(b"user.") and len(name) > 5 or
                allow_trusted and name.startswith(b"trusted.") and len(name) > 8 or
                allow_security and name.startswith(b"security.") and len(name) > 9):
            raise ValueError("xattr namespace rejected by explicit import policy: " + repr(name))
        storage += 16 + ((len(name) + 3) & ~3) + ((len(value) + 3) & ~3)
        if storage > 3968:
            raise ValueError("xattr storage exceeds conservative target block budget")
        output += struct.pack("<HHI", len(name), 0, len(value)) + name + value
    return bytes(output)


@dataclass
class Entry:
    path: bytes
    kind: int
    payload: bytes = b""
    mode: int = 0o644
    uid: int = 0
    gid: int = 0
    seconds: int = 0
    nanoseconds: int = 0
    xattrs: tuple = field(default_factory=tuple)
    link: bytes = b""


def encode(entries, allow_trusted=False, allow_security=False):
    entries = sorted(entries, key=lambda e: e.path)
    if not entries or len(entries) > 262144:
        raise ValueError("missing root or excessive record count")
    objects = {}
    for index, entry in enumerate(entries, 1):
        canonical(entry.path)
        if entry.path in objects:
            raise ValueError("duplicate entry path")
        objects[entry.path] = (index, entry.kind)
    if entries[0].path != b"/" or entries[0].kind != DIRECTORY:
        raise ValueError("first record must describe root directory")
    body, inode_count, data_bytes = bytearray(), 0, 0
    for index, entry in enumerate(entries, 1):
        if entry.path != b"/":
            parent = entry.path.rsplit(b"/", 1)[0] or b"/"
            if objects.get(parent, (0, 0))[1] != DIRECTORY:
                raise ValueError("missing/non-directory parent")
        if entry.kind not in (REGULAR, DIRECTORY, SYMLINK, HARDLINK):
            raise ValueError("unsupported inode kind")
        if entry.kind == HARDLINK:
            identity, kind = objects.get(entry.link, (0, 0))
            if kind not in (REGULAR, SYMLINK):
                raise ValueError("hardlink must reference a canonical regular/symlink inode")
            if (entry.payload or entry.xattrs or entry.mode or entry.uid or entry.gid or
                    entry.seconds or entry.nanoseconds):
                raise ValueError("hardlink alias cannot carry independent data/metadata")
            mode = uid = gid = seconds = nanoseconds = 0
            attrs = b""
        else:
            identity = index
            inode_count += 1
            mode, uid, gid = entry.mode, entry.uid, entry.gid
            seconds, nanoseconds = entry.seconds, entry.nanoseconds
            attrs = encode_xattrs(entry.xattrs, allow_trusted, allow_security)
            if mode & ~0o7777 or not 0 <= uid <= 0xFFFFFFFF or not 0 <= gid <= 0xFFFFFFFF:
                raise ValueError("invalid mode/owner")
            if not -0x80000000 <= seconds <= 0x37FFFFFFF or not 0 <= nanoseconds < 1000000000:
                raise ValueError("timestamp outside current native target profile")
        if entry.kind == DIRECTORY and entry.payload:
            raise ValueError("directory cannot carry payload")
        if entry.kind == SYMLINK and (not entry.payload or len(entry.payload) > MAX_PATH or b"\0" in entry.payload):
            raise ValueError("invalid opaque symlink target bytes")
        if len(entry.payload) > 4 << 30:
            raise ValueError("file exceeds profile limit")
        if entry.kind == REGULAR:
            data_bytes += len(entry.payload)
        record_size = 64 + len(entry.path) + len(entry.payload) + len(attrs)
        if record_size > MAX_BYTES - 64 - len(body) or data_bytes > MAX_BYTES:
            raise ValueError("package exceeds profile limit before output growth")
        body += struct.pack("<QIHHQQIIIIqII", record_size, len(entry.path), entry.kind,
                            len(entry.xattrs), identity, len(entry.payload), mode, uid, gid,
                            nanoseconds, seconds, len(attrs), 0)
        body += entry.path + entry.payload + attrs
    if len(body) + 64 > MAX_BYTES or data_bytes > MAX_BYTES:
        raise ValueError("package exceeds profile limit")
    header = struct.pack("<8sIIQQQQIII", MAGIC, 2, 64, len(body) + 64, len(entries), inode_count,
                         data_bytes, 0, 0, crc32c(body))
    assert len(header) == 60
    return header + struct.pack("<I", crc32c(header)) + body


def _read_regular(path, info, remaining):
    if info.st_size > 4 << 30 or info.st_size > remaining:
        raise ValueError("source staging-byte budget exceeded before reading file")
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        fields = lambda st: (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns,
                             st.st_mode, st.st_uid, st.st_gid)
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or fields(before) != fields(info):
            raise ValueError("source changed before collecting")
        chunks, collected = [], 0
        while True:
            chunk = os.read(descriptor, 1 << 20)
            if not chunk:
                break
            collected += len(chunk)
            if collected > 4 << 30 or collected > remaining:
                raise ValueError("source file exceeds profile limit")
            chunks.append(chunk)
        if collected != info.st_size or fields(os.fstat(descriptor)) != fields(info):
            raise ValueError("source changed while collecting")
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def collect(root, package_format="pak2"):
    """Requires a frozen staging tree; returned links are never dereferenced.

    This is not a snapshot of a concurrently mutated directory hierarchy. PAK1
    deliberately retains its old link-omission behavior, with a stderr count.
    PAK2 retains link text and non-directory inode identity.
    """
    legacy = package_format in ("pak1", "1")
    entries, identities = [], {}
    resident_bytes, discovered, omitted_links = 0, 1, 0
    root = os.fsencode(os.path.abspath(root))
    if not stat.S_ISDIR(os.lstat(root).st_mode):
        raise ValueError("root is not a directory")

    def visit(path, relative):
        nonlocal resident_bytes, discovered, omitted_links
        if len(entries) == MAX_ENTRIES:
            raise ValueError("source entry count exceeds profile limit")
        info = os.lstat(path)
        canonical(relative, LEGACY_MAX_PATH if legacy else MAX_PATH)
        if stat.S_ISDIR(info.st_mode):
            kind, payload = DIRECTORY, b""
        elif stat.S_ISLNK(info.st_mode):
            if legacy:
                omitted_links += 1
                return
            kind, payload = SYMLINK, os.readlink(path)
        elif stat.S_ISREG(info.st_mode):
            kind = REGULAR
            maximum = 0xffffffff if legacy else 4 << 30
            payload = _read_regular(path, info, min(MAX_BYTES - resident_bytes, maximum))
            resident_bytes += len(payload)
        else:
            raise ValueError("unsupported source inode kind: " + repr(relative))
        attributes = ()
        if not legacy:
            attributes = tuple(sorted((os.fsencode(name), os.getxattr(path, name, follow_symlinks=False))
                                      for name in os.listxattr(path, follow_symlinks=False)))
        seconds, nanoseconds = divmod(info.st_mtime_ns, 1000000000)
        entry = Entry(relative, kind, payload, stat.S_IMODE(info.st_mode), info.st_uid, info.st_gid,
                      seconds, nanoseconds, attributes)
        if not legacy or relative != b"/":
            entries.append(entry)
        if kind != DIRECTORY:
            identities[relative] = (info.st_dev, info.st_ino)
        if kind == DIRECTORY:
            children = []
            with os.scandir(path) as directory:
                for child in directory:
                    if child.name in (b".git", b"__pycache__") and child.is_dir(follow_symlinks=False):
                        continue
                    if discovered == MAX_ENTRIES:
                        raise ValueError("source directory exceeds remaining entry budget")
                    discovered += 1
                    children.append(child)
            for child in sorted(children, key=lambda item: item.name):
                visit(child.path, relative.rstrip(b"/") + b"/" + child.name)

    visit(root, b"/")
    if legacy:
        if omitted_links:
            print(f"make_pak.py: PAK1 omitted {omitted_links} symbolic link(s); "
                  "use --format pak2 to preserve links (requires a PAK2 importer)", file=sys.stderr)
        return entries
    canonical_objects = {}
    for entry in sorted(entries, key=lambda e: e.path):
        if entry.kind == DIRECTORY:
            continue
        identity = identities[entry.path]
        if identity not in canonical_objects:
            canonical_objects[identity] = entry
            continue
        first = canonical_objects[identity]
        properties = lambda e: (e.kind, e.mode, e.uid, e.gid, e.seconds, e.nanoseconds, e.xattrs, e.payload)
        if properties(entry) != properties(first):
            raise ValueError("hardlink source changed or conflicting inode metadata")
        entry.kind, entry.link = HARDLINK, first.path
        entry.payload, entry.xattrs = b"", ()
        entry.mode = entry.uid = entry.gid = entry.seconds = entry.nanoseconds = 0
    return entries


def encode_legacy(entries):
    # Validate every entry before publishing any output bytes. PAK1 cannot carry
    # symlinks, hardlink identity, owner, timestamps or extended attributes.
    entries = sorted(entries, key=lambda entry: entry.path)
    if len(entries) >= MAX_ENTRIES:
        raise ValueError("legacy record/inode count exceeds implicit-root profile")
    objects = {b"/": DIRECTORY}
    total = 16
    for entry in entries:
        canonical(entry.path, LEGACY_MAX_PATH)
        if entry.path in objects:
            raise ValueError("duplicate or explicit legacy root entry")
        parent = entry.path.rsplit(b"/", 1)[0] or b"/"
        if objects.get(parent) != DIRECTORY:
            raise ValueError("missing/non-directory parent")
        if entry.kind not in (REGULAR, DIRECTORY) or entry.mode & ~0o7777:
            raise ValueError("unsupported legacy inode kind/mode")
        if entry.kind == DIRECTORY and entry.payload:
            raise ValueError("directory cannot carry payload")
        if len(entry.payload) > 0xffffffff:
            raise ValueError("legacy file exceeds profile limit")
        total += 16 + len(entry.path) + len(entry.payload)
        if total > MAX_BYTES:
            raise ValueError("legacy package exceeds byte budget")
        objects[entry.path] = entry.kind
    output = bytearray(struct.pack("<8sQ", LEGACY_MAGIC, len(entries)))
    for entry in entries:
        output += struct.pack("<HHIQ", len(entry.path), entry.kind, entry.mode, len(entry.payload))
        output += entry.path + entry.payload
    return bytes(output)


def write_atomic(output, data):
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix="." + output.name + ".", dir=output.parent,
                                         delete=False) as stream:
            temporary = stream.name
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
        temporary = None
    finally:
        if temporary:
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root")
    parser.add_argument("output", type=Path)
    parser.add_argument("--format", choices=("pak1", "pak2", "1", "2"), default="pak1",
                        help="pak1 remains the compatible default; pak2 requires a new importer")
    parser.add_argument("--allow-trusted", action="store_true")
    parser.add_argument("--allow-security", action="store_true")
    args = parser.parse_args()
    root, output = Path(args.root).resolve(), args.output.resolve()
    if root == output or root in output.parents:
        raise ValueError("output must be outside the frozen staging tree")
    legacy = args.format in ("pak1", "1")
    if legacy and (args.allow_trusted or args.allow_security):
        raise ValueError("protected-xattr policy requires explicit --format pak2")
    entries = collect(root, args.format)
    data = encode_legacy(entries) if legacy else encode(entries, args.allow_trusted, args.allow_security)
    write_atomic(output, data)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print("make_pak.py: " + str(error), file=sys.stderr)
        raise SystemExit(1)
