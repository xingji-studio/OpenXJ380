#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Repack native XJ380 x86-64 ELF images as EPF v1.0.

This is a post-link format converter, not a linker or Linux ABI translator.
Dynamic output uses the EPF eager binding ABI, with no ELF interposition or lazy PLT binding.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import stat
import struct
import tempfile

PAGE = 4096
MAX_FILE = 64 << 20
MAX_IMAGE = 1 << 30
MAX_SEGMENTS = 256
MAX_RELOCATIONS = 65536
USER_MMAP_START = 0x400000000000
RESERVED = (0x11717450000, 0x300000000000)
REQUIRED = 0x80000000


class EpfError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EpfError(message)


def bounded(offset: int, size: int, limit: int) -> bool:
    return 0 <= offset <= limit and 0 <= size <= limit - offset


def power_of_two(value: int) -> bool:
    return value > 0 and value & (value - 1) == 0


def aligned(value: int, align: int = PAGE) -> int:
    return (value + align - 1) & -align


@dataclass
class Load:
    address: int
    data: bytes
    memory_size: int
    flags: int
    align: int


def lower_static_relro(loads: list[Load], ranges: list[tuple[int, int]]) -> list[Load]:
    """Already-resolved fixed-address RELRO becomes ordinary read-only LOADs.

    Runtime RELRO is deliberately not emitted or approximated. GNU RELRO's
    leading page prefix is protected too; supported ranges end on a full page.
    """
    for start, end in ranges:
        require(end % PAGE == 0, "partial-page GNU_RELRO endpoint cannot be represented; relink with -z norelro")
        # Linkers may extend RELRO through the zero-filled tail of a LOAD's
        # final mapped page, even when p_memsz ends before that page boundary.
        require(any(load.flags & 2 and not load.flags & 1 and load.address <= start and
                    end <= aligned(load.address + load.memory_size) for load in loads),
                "GNU_RELRO is outside writable LOAD")
    result = []
    for load in loads:
        end = load.address + load.memory_size
        cuts = sorted({load.address, end} |
                      {point for region in ranges for point in region if load.address < point < end})
        for first, last in zip(cuts, cuts[1:]):
            offset = first - load.address
            flags = load.flags
            if any(start <= first and last <= stop for start, stop in ranges):
                flags &= ~2
            data = load.data[offset:min(last - load.address, len(load.data))] if offset < len(load.data) else b""
            result.append(Load(first, data, last - first, flags, PAGE))
    return result


def runtime_relocations(elf: bytes, programs: list[tuple[int, ...]], base: int) -> list[tuple[int, int]]:
    directories = [p for p in programs if p[0] == 2]
    require(len(directories) <= 1, "duplicate ELF PT_DYNAMIC")
    if not directories:
        return []

    def initialized(address: int, size: int) -> int:
        for kind, flags, offset, va, _, file_size, _, _ in programs:
            if kind == 1 and flags & 4 and address >= va and bounded(address - va, size, file_size):
                return offset + address - va
        raise EpfError("ELF runtime table is outside initialized readable LOAD content")

    _, _, offset, address, _, size, _, _ = directories[0]
    require(size > 0 and size % 16 == 0 and size // 16 <= 4096, "invalid ELF dynamic directory size")
    require(initialized(address, size) == offset, "ELF PT_DYNAMIC file/VA mismatch")
    tags = {}
    terminated = False
    # Tables used only for ELF symbol lookup may be stripped because every
    # accepted relocation is symbol-free. Other execution semantics are rejected.
    passive = {3, 4, 5, 6, 10, 11, 21, 0x6ffffef5}
    must_be_zero = {1, 2, 12, 13, 14, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29,
                    32, 33, 35, 36, 37, 0x6ffffff0, 0x6ffffffc, 0x6ffffffd, 0x6ffffffe, 0x6fffffff}
    for position in range(offset, offset + size, 16):
        tag, value = struct.unpack_from("<QQ", elf, position)
        if tag == 0:
            require(value == 0 and not any(elf[position:offset + size]), "malformed ELF dynamic terminator")
            terminated = True
            break
        require(tag not in tags, "duplicate ELF dynamic tag")
        tags[tag] = value
        if tag in must_be_zero:
            require(value == 0, f"ELF dynamic tag {tag:#x} needs unsupported binding, lifecycle, TLS or relocation")
        elif tag == 30:
            require(value & ~8 == 0, "unsupported ELF DT_FLAGS")
        elif tag == 0x6ffffffb:
            require(value & ~0x08000001 == 0, "unsupported ELF DT_FLAGS_1")
        else:
            require(tag in passive | {7, 8, 9, 0x6ffffff9}, f"unsupported ELF dynamic tag {tag:#x}")
    require(terminated, "unterminated ELF dynamic directory")
    group = {7, 8, 9} & tags.keys()
    require(not group or group == {7, 8, 9}, "incomplete ELF RELA directory")
    if not group:
        require(tags.get(0x6ffffff9, 0) == 0, "ELF RELACOUNT without RELA table")
        return []
    require(tags[9] == 24 and tags[8] % 24 == 0, "unsupported ELF RELA stride or size")
    count = tags[8] // 24
    require(count <= MAX_RELOCATIONS and tags.get(0x6ffffff9, 0) <= count, "ELF relocation count limit")
    table = initialized(tags[7], tags[8])
    result = []
    writable_loads = sorted((p for p in programs if p[0] == 1 and p[1] & 2 and not p[1] & 1), key=lambda p: p[3])
    for i in range(count):
        target, info, addend = struct.unpack_from("<QQq", elf, table + i * 24)
        kind, symbol = info & 0xffffffff, info >> 32
        if kind == 0:
            require(i >= tags.get(0x6ffffff9, 0), "ELF RELACOUNT prefix contains NONE")
            continue
        require(kind in (8, 38) and symbol == 0, "only symbol-free x86-64 RELATIVE relocations can be converted")
        require(i >= tags.get(0x6ffffff9, 0) or kind == 8, "ELF RELACOUNT prefix is not RELATIVE")
        checked = target
        for program in writable_loads:
            if program[3] <= checked < program[3] + program[6]:
                checked = min(target + 8, program[3] + program[6])
                if checked == target + 8:
                    break
        require(checked == target + 8, "ELF relocation write is not writable data")
        require(-(1 << 63) <= addend - base < 1 << 63, "EPF signed addend overflow")
        result.append((target - base, addend - base))
    result.sort()
    require(all(a[0] + 8 <= b[0] for a, b in zip(result, result[1:])), "overlapping ELF relocation writes")
    return result



def tls_program(programs: list[tuple[int, ...]], file_size: int):
    templates = [p for p in programs if p[0] == 7]
    require(len(templates) <= 1, "duplicate ELF PT_TLS")
    if not templates:
        return None
    p = templates[0]
    _, flags, offset, address, _, initialized_size, memory_size, align = p
    require(flags & ~4 == 0 and 0 < memory_size <= 1 << 20 and initialized_size <= memory_size and
            bounded(offset, initialized_size, file_size) and power_of_two(align) and align <= 2 << 20 and
            address % align == 0, "invalid ELF TLS range, size or alignment")
    require(not initialized_size or any(load[0] == 1 and load[1] & 4 and address >= load[3] and
            bounded(address - load[3], initialized_size, load[5]) and
            offset == load[2] + address - load[3] for load in programs),
            "ELF TLS template must be initialized readable LOAD content")
    return p


def dynamic_tables(elf: bytes, programs: list[tuple[int, ...]], base: int):
    """Translate native ELF dynamic metadata into section-independent EPF tables."""
    def initialized(address, size):
        for p in programs:
            if p[0] == 1 and p[1] & 4 and address >= p[3] and bounded(address - p[3], size, p[5]):
                return p[2] + address - p[3]
        raise EpfError("ELF runtime table is outside initialized readable LOAD content")

    tls = tls_program(programs, len(elf))
    directories = [p for p in programs if p[0] == 2]
    require(len(directories) == 1, "dynamic EPF needs exactly one ELF PT_DYNAMIC")
    directory = directories[0]
    require(directory[5] and directory[5] % 16 == 0 and directory[5] // 16 <= 4096,
            "invalid ELF dynamic directory size")
    require(initialized(directory[3], directory[5]) == directory[2], "ELF PT_DYNAMIC file/VA mismatch")
    tags, needed = {}, []
    for at in range(directory[2], directory[2] + directory[5], 16):
        tag, value = struct.unpack_from("<QQ", elf, at)
        if tag == 0:
            require(not any(elf[at:directory[2] + directory[5]]), "malformed ELF dynamic terminator")
            break
        if tag == 1:
            needed.append(value)
            require(len(needed) <= 32, "dependency count limit")
            continue
        require(tag not in tags, "duplicate ELF dynamic tag")
        tags[tag] = value
        if tag == 30:
            require(value & ~26 == 0, "unsupported ELF DT_FLAGS")
        elif tag == 0x6ffffffb:
            require(value & ~0x08000001 == 0, "unsupported ELF DT_FLAGS_1")
        else:
            require(tag in {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 20, 21, 23, 24,
                            25, 26, 27, 28, 0x6ffffef5, 0x6ffffff9},
                    f"unsupported dynamic ELF tag {tag:#x} (versions, RELR and search paths are excluded)")
    else:
        raise EpfError("unterminated ELF dynamic directory")
    require(all(tag in tags for tag in (4, 5, 6, 10, 11)) and tags[11] == 24,
            "dynamic conversion requires SysV DT_HASH and complete ELF symbol/string tables")
    strings_at = initialized(tags[5], tags[10])
    strings = elf[strings_at:strings_at + tags[10]]
    require(strings and len(strings) <= 4 << 20 and strings[0] == 0, "invalid dynamic string table")

    string_work = 0

    def string_end(offset):
        nonlocal string_work
        require(0 <= offset < len(strings), "invalid dynamic string offset")
        end = strings.find(b"\0", offset)
        require(end >= offset, "unterminated dynamic string")
        string_work += end - offset + 1
        require(string_work <= 100000000, "dynamic string validation work limit")
        return end

    def library_name(offset):
        end = string_end(offset)
        require(0 < end - offset <= 127, "invalid EPF library basename length")
        name = strings[offset:end]
        require(name not in (b".", b"..") and
                all(32 < c < 127 and c not in b"/\\:" for c in name), "invalid EPF library basename")

    for offset in needed:
        library_name(offset)
    if 14 in tags:
        library_name(tags[14])
    hash_at = initialized(tags[4], 8)
    buckets, count = struct.unpack_from("<II", elf, hash_at)
    require(0 < count <= 65536 and 0 < buckets <= 65536, "invalid ELF symbol count")
    initialized(tags[4], (2 + buckets + count) * 4)
    symbols_at = initialized(tags[6], count * 24)
    symbols = bytearray()
    for i in range(count):
        name, info, visibility, section, value, size = struct.unpack_from("<IBBHQQ", elf, symbols_at + i * 24)
        if i == 0:
            require(not any(elf[symbols_at:symbols_at + 24]), "nonzero ELF null symbol")
            symbols.extend(bytes(32))
            continue
        binding, kind = info >> 4, info & 15
        require(binding <= 2 and kind in (0, 1, 2, 6) and visibility in (0, 2), "unsupported ELF symbol attributes")
        string_end(name)
        require(section < 0xff00 or section == 0xfff1, "unsupported ELF symbol definition")
        domain = 0 if section == 0 else 3 if section == 0xfff1 else 2
        if domain == 0:
            require(value == 0 and name != 0 and binding != 0, "invalid undefined ELF symbol")
        elif kind == 6:
            require(domain == 2 and tls is not None and bounded(value, size or 1, tls[6]),
                    "TLS symbol outside its module template")
            domain = 4
        elif domain == 2:
            require(any(p[0] == 1 and p[3] <= value and bounded(value - p[3], size or 1, p[6])
                        for p in programs), "defined symbol outside ELF LOAD")
            if kind == 2:
                require(any(p[0] == 1 and p[1] & 1 and p[3] <= value < p[3] + p[5] for p in programs),
                        "function symbol must be initialized executable code")
            value -= base
        symbols.extend(struct.pack("<IIQQBBBBI", name, 0, value, size, binding, 4 if kind == 6 else kind,
                                   int(visibility == 2), domain, 0))
    tables = []
    group = {7, 8, 9} & tags.keys()
    require(not group or group == {7, 8, 9}, "incomplete ELF RELA group")
    if group:
        require(tags[9] == 24 and tags[8] % 24 == 0, "invalid ELF RELA stride")
        tables.append((tags[7], tags[8]))
    require(tags.get(0x6ffffff9, 0) <= tags.get(8, 0) // 24, "invalid ELF RELACOUNT")
    for i in range(tags.get(0x6ffffff9, 0)):
        at = initialized(tags[7] + i * 24, 24)
        require(struct.unpack_from("<Q", elf, at + 8)[0] == 8, "invalid ELF RELACOUNT prefix")
    group = {2, 20, 23} & tags.keys()
    require(not group or group == {2, 20, 23}, "incomplete ELF PLT relocation group")
    if group:
        require(tags[20] == 7 and tags[2] % 24 == 0, "only ELF RELA PLT is supported")
        tables.append((tags[23], tags[2]))
    result = []
    count_rela = 0
    writable_loads = sorted((p for p in programs if p[0] == 1 and p[1] & 2 and not p[1] & 1), key=lambda p: p[3])
    for address, size in tables:
        table_at = initialized(address, size)
        count_rela += size // 24
        require(count_rela <= MAX_RELOCATIONS, "runtime relocation count limit")
        for at in range(table_at, table_at + size, 24):
            target, info, addend = struct.unpack_from("<QQq", elf, at)
            symbol, kind = info >> 32, info & 0xffffffff
            if kind == 0:
                continue
            mapping = {1: (2, 8), 2: (3, 4), 6: (2, 8), 7: (2, 8), 8: (5, 8), 10: (1, 4), 38: (5, 8),
                       16: (0x003e0001, 8), 17: (0x003e0002, 8), 18: (0x003e0003, 8)}
            require(kind in mapping and symbol < count, "unsupported ELF runtime relocation")
            epf_kind, width = mapping[kind]
            if kind in (16, 17, 18):
                require((symbol and symbols[symbol * 32 + 25] == 4) or (not symbol and tls is not None),
                        "ELF TLS relocation needs a TLS symbol or local TLS storage")
                require(kind != 16 or addend == 0, "ELF DTPMOD addend must be zero")
            else:
                require(not symbol or symbols[symbol * 32 + 25] != 4, "non-TLS relocation references TLS")
            if kind in (6, 7):
                require(addend == 0, "ELF GOT/PLT addend must be zero")
            if kind in (8, 38):
                require(symbol == 0, "relative ELF relocation has a symbol")
                addend -= base
            checked = target
            for p in writable_loads:
                if p[3] <= checked < p[3] + p[6]:
                    checked = min(target + width, p[3] + p[6])
            require(checked == target + width, "ELF relocation target outside writable data")
            require(-(1 << 63) <= addend < 1 << 63, "EPF signed addend overflow")
            result.append((target - base, addend, symbol, epf_kind))
    result.sort()
    require(all(a[0] + (4 if a[3] in (1, 3, 4) else 8) <= b[0] for a, b in zip(result, result[1:])),
            "overlapping ELF relocation writes")

    relocation_by_target = {record[0]: record for record in result}

    def lifecycle(address_tag, size_tag, function_tag):
        require((address_tag in tags) == (size_tag in tags), "incomplete ELF lifecycle array")
        values = []
        function = tags.get(function_tag, 0)
        if function:
            values.append(function - base)
        if address_tag in tags:
            require(tags[size_tag] % 8 == 0 and tags[size_tag] // 8 <= 1024, "invalid lifecycle array size")
            at = initialized(tags[address_tag], tags[size_tag])
            for offset in range(0, tags[size_tag], 8):
                rva = tags[address_tag] + offset - base
                record = relocation_by_target.pop(rva, None)
                if record:
                    require(record[2] == 0 and record[3] == 5, "lifecycle entry needs a local relative relocation")
                    value = record[1]
                else:
                    value = struct.unpack_from("<Q", elf, at + offset)[0] - base
                values.append(value)
        require(len(values) <= 1024 and all(any(p[0] == 1 and p[1] & 1 and
                p[3] <= base + value < p[3] + p[5] for p in programs) for value in values),
                "lifecycle target must be initialized executable code")
        return values

    init = lifecycle(25, 27, 12)
    fini = lifecycle(26, 28, 13)
    result = [record for record in result if record[0] in relocation_by_target]
    return strings, bytes(symbols), needed, tags.get(14), result, init, fini


def dynamic_payload(tables, rva):
    strings, symbols, needed, soname, relocations, init, fini = tables
    entries = 5 + 3 + len(needed) + (soname is not None) + 2 * bool(init) + 2 * bool(fini) + 1
    offset = entries * 16
    tags = []
    payload = bytearray(offset)

    def table(tag, count_tag, data, count, stride_tag=None):
        nonlocal offset
        padding = aligned(offset, 8) - offset
        payload.extend(bytes(padding))
        offset += padding
        tags.extend(((tag, rva + offset), (count_tag, count)))
        if stride_tag:
            tags.append((stride_tag, 32))
        payload.extend(data)
        offset += len(data)

    table(1, 2, strings, len(strings))
    table(3, 4, symbols, len(symbols) // 32, 5)
    table(6, 7, b"".join(struct.pack("<QqIIQ", *record, 0) for record in relocations), len(relocations), 8)
    tags += [(9, name) for name in needed]
    if soname is not None:
        tags.append((10, soname))
    for data, tag in ((init, 11), (fini, 13)):
        if data:
            table(tag, tag + 1, b"".join(struct.pack("<Q", value) for value in data), len(data))
    require(len(tags) + 1 == entries and entries <= 256, "EPF dynamic directory count limit")
    for i, (tag, value) in enumerate(tags):
        struct.pack_into("<IIQ", payload, i * 16, tag, 1, value)
    return bytes(payload), entries * 16

def convert(elf: bytes, *, shared: bool = False, interpreter: str | None = None,
            require_tls: bool = False, abi_version: int | None = None) -> bytes:
    require(64 <= len(elf) <= MAX_FILE, "ELF file must be between 64 bytes and 64 MiB")
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", elf)
    ident, kind, machine, version, entry, table, _, arch_flags, size, stride, count, *_ = header
    require(ident[:7] == b"\x7fELF\x02\x01\x01" and machine == 62 and version == 1,
            "input must be little-endian ELF64 x86-64")
    require(kind in (2, 3) and arch_flags == 0 and size == 64, "input must be a native ELF EXEC or DYN")
    require(stride == 56 and 0 < count <= MAX_SEGMENTS and bounded(table, count * stride, len(elf)),
            "invalid ELF program header table")
    programs = [struct.unpack_from("<IIQQQQQQ", elf, table + i * stride) for i in range(count)]
    pie = kind == 3
    dynamic = shared or interpreter is not None
    require(not shared or pie, "SHARED input must be ELF ET_DYN")
    require(not (shared and interpreter), "SHARED must not contain INTERP")
    require(interpreter is None or interpreter == "/system/lib/ld-epf-x86_64.epf", "unsupported EPF INTERP path")
    require(sum(p[0] == 3 for p in programs) <= 1, "duplicate ELF INTERP")
    tls = tls_program(programs, len(elf))
    require(not (tls or require_tls) or dynamic, "TLS requires --interpreter or --shared")
    loads = []
    relro = []
    entry_valid = False
    expanded_bytes = 128 + (count + 2) * 64
    for ptype, flags, offset, va, _, file_size, memory_size, align in programs:
        require(bounded(offset, file_size, len(elf)) and bounded(va, memory_size, 1 << 64),
                "ELF segment range overflow or truncation")
        require(ptype in (0, 1, 2, 3, 4, 6, 7, 0x6474e551, 0x6474e552),
                f"unsupported ELF segment {ptype:#x} (unwind and architecture extensions are excluded)")
        if ptype == 3:
            require(dynamic and not shared and interpreter is not None, "ELF INTERP needs explicit dynamic EPF output")
            require(elf[offset:offset + file_size] == interpreter.encode("ascii") + b"\0",
                    "ELF INTERP must match the requested native EPF interpreter")
        if ptype == 0x6474e551:
            require(not flags & 1, "executable user stacks are not supported")
        if ptype == 0x6474e552 and memory_size:
            relro.append((va & -PAGE, va + memory_size))
        if ptype != 1 or (memory_size == 0 and file_size == 0):
            continue
        require(flags & 4 and flags & 3 != 3 and flags & ~7 == 0, "ELF LOAD must be readable and obey W^X")
        require(0 < memory_size and file_size <= memory_size and (align in (0, 1) or power_of_two(align)),
                "invalid ELF LOAD size or alignment")
        require(align in (0, 1) or offset % align == va % align, "incongruent ELF LOAD alignment")
        alignment = max(PAGE, align) if pie else PAGE
        require(alignment <= MAX_IMAGE, "ELF LOAD alignment exceeds native EPF profile")
        require(not dynamic or alignment <= 2 << 20, "dynamic EPF LOAD alignment exceeds 2 MiB")
        start = va & -alignment
        prefix = va - start
        require(prefix + file_size <= MAX_FILE, "ELF page-prefix expansion exceeds EPF file limit")
        expanded_bytes += prefix + file_size
        require(expanded_bytes <= MAX_FILE, "combined ELF LOAD expansion exceeds EPF file limit")
        data = bytes(prefix) + elf[offset:offset + file_size]
        loads.append(Load(start, data, prefix + memory_size, flags, alignment))
        entry_valid |= bool(flags & 1 and va <= entry < va + file_size)
    require(loads and (shared or entry_valid), "ELF entry must lie in executable file content")
    loads.sort(key=lambda load: load.address)
    require(all(aligned(a.address + a.memory_size) <= b.address for a, b in zip(loads, loads[1:])),
            "ELF LOADs share a page after EPF alignment; relink with separate code/data pages")
    base = 0 if pie else loads[0].address
    tables = dynamic_tables(elf, programs, base) if dynamic else None
    relocations = tables[4] if dynamic else runtime_relocations(elf, programs, base)
    runtime_relro = bool(relro and (dynamic or pie or relocations))
    if relro:
        # Validate all protection ranges before constructing metadata.
        lowered = lower_static_relro(loads, relro)
        if not runtime_relro:
            loads = lowered
    image_end = aligned(max(load.address + load.memory_size for load in loads))
    image_size = image_end - base
    meta_rva = image_size
    directory_size = 0
    if dynamic:
        payload, directory_size = dynamic_payload(tables, meta_rva)
        loads.append(Load(image_end, payload, len(payload), 4, PAGE))
        image_size = aligned(image_size + len(payload))
    elif relocations:
        directory_size = 64
        payload = b"".join(struct.pack("<QQ", (1 << 32) | tag, value) for tag, value in
                           ((6, meta_rva + 64), (7, len(relocations)), (8, 32))) + bytes(16)
        payload += b"".join(struct.pack("<QqIIQ", target, addend, 0, 5, 0) for target, addend in relocations)
        loads.append(Load(image_end, payload, len(payload), 4, PAGE))
        image_size = aligned(image_size + len(payload))
    require(0 < image_size <= MAX_IMAGE, "EPF image exceeds 1 GiB reservation limit")
    require(not shared or image_size <= 64 << 20, "SHARED image exceeds 64 MiB runtime limit")
    segment_count = len(loads) + bool(directory_size) + bool(interpreter) + bool(tls) + len(relro) * runtime_relro
    require(segment_count <= MAX_SEGMENTS, "EPF segment count limit")
    require(shared or 0 <= entry - base < image_size, "EPF entry RVA out of range")
    if pie:
        low, high = 0x10000000, 0x100000000 - image_size
        for record in relocations:
            if dynamic and record[3] != 5:
                continue
            addend = record[1]
            if addend < 0:
                low = max(low, -addend)
            else:
                high = min(high, (1 << 64) - 1 - addend)
        alignment = max(load.align for load in loads)
        require(aligned(low, alignment) <= high, "no representable native PIE base")
    else:
        require(PAGE <= base and base + image_size <= USER_MMAP_START, "EPF fixed image outside native image space")
        require(all(base + image_size <= address or address + PAGE <= base for address in RESERVED),
                "EPF fixed image overlaps a native reserved address")
        require(all(0 <= base + record[1] < 1 << 64 for record in relocations if not dynamic or record[3] == 5),
                "relative relocation overflow")

    output = bytearray(128 + segment_count * 64)
    meta_offset = None
    for index, load in enumerate(loads):
        position = aligned(len(output), load.align)
        require(position + len(load.data) <= MAX_FILE, "converted EPF exceeds 64 MiB file limit")
        output.extend(bytes(position - len(output)))
        output.extend(load.data)
        flags = REQUIRED | 1 | (2 if load.flags & 2 else 0) | (4 if load.flags & 1 else 0)
        struct.pack_into("<IIQQQQQQQ", output, 128 + index * 64, 1, flags, position,
                         load.address - base, len(load.data), load.memory_size, load.align, 0, 0)
        if directory_size and index == len(loads) - 1:
            meta_offset = position
    descriptors = []
    if directory_size:
        descriptors.append((2, REQUIRED, meta_offset, meta_rva, directory_size, directory_size, 8, 0, 0))
    if runtime_relro:
        descriptors += [(5, REQUIRED, 0, start - base, 0, end - start, PAGE, 0, 0) for start, end in relro]
    if tls:
        _, _, offset, address, _, initialized_size, memory_size, align = tls
        file_offset = 0
        if initialized_size:
            load_index = next(i for i, load in enumerate(loads) if load.address <= address and
                              bounded(address - load.address, initialized_size, len(load.data)))
            file_offset = struct.unpack_from("<Q", output, 128 + load_index * 64 + 8)[0]
            file_offset += address - loads[load_index].address
        descriptors.append((4, REQUIRED, file_offset, address - base if initialized_size else 0,
                            initialized_size, memory_size, align, 0, 0))
    if interpreter:
        data = interpreter.encode("ascii") + b"\0"
        position = len(output)
        output.extend(data)
        descriptors.append((3, REQUIRED, position, 0, len(data), len(data), 1, 0, 0))
    for i, descriptor in enumerate(descriptors):
        struct.pack_into("<IIQQQQQQQ", output, 128 + (len(loads) + i) * 64, *descriptor)
    require(len(output) <= MAX_FILE, "converted EPF exceeds file limit")
    thread_exit_symbols = (b"__cxa_thread_atexit\0", b"__cxa_thread_atexit_impl\0")
    uses_cxx_tls = bool(tables and any(tables[0].startswith(name, struct.unpack_from("<I", tables[1], i)[0])
                        for i in range(0, len(tables[1]), 32) for name in thread_exit_symbols))
    uses_tls = bool(tls or require_tls or uses_cxx_tls or (dynamic and
                    (any(record[3] >= 0x003e0001 for record in relocations) or
                     any(tables[1][i + 25] == 4 for i in range(0, len(tables[1]), 32)))))
    minimum_abi = 3 if uses_cxx_tls else 2 if uses_tls else 1
    selected_abi = minimum_abi if abi_version is None else abi_version
    require(selected_abi in (1, 2, 3) and selected_abi >= minimum_abi,
            f"this input requires native ABI {minimum_abi}")
    output[:8] = b"$EPF\x01\x00\x08\x01"
    struct.pack_into("<HHHHII", output, 8, 3 if shared else 2, 62, 0x0380, selected_abi, int(pie and not shared), 128)
    features = int(bool(relocations)) | (2 if dynamic else 0) | (4 if uses_tls else 0) | (8 if runtime_relro else 0)
    struct.pack_into("<QQQQQQQ", output, 24, features, base, (1 << 64) - 1 if shared else entry - base,
                     image_size, 128, 0, len(output))
    struct.pack_into("<IIIHHQI20s", output, 80, segment_count, 0, 0, 64, 64, PAGE, 0, bytes(20))
    return bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="already-linked native ELF EXEC, PIE or shared library")
    parser.add_argument("-o", "--output", type=Path, required=True, help="destination EPF file")
    parser.add_argument("--shared", action="store_true", help="produce EPF_SHARED (.eps) with no entry")
    parser.add_argument("--interpreter", help="produce dynamic EXEC using the native EPF interpreter")
    parser.add_argument("--require-tls", action="store_true", help="declare TLS ABI use by assembly/runtime code")
    parser.add_argument("--abi-version", type=int, choices=(1, 2, 3),
                        help="native ABI (C++ TLS destruction: 3, TLS: 2, otherwise: 1)")
    args = parser.parse_args()
    temporary = None
    try:
        require(args.input.resolve() != args.output.resolve(), "input and output must be different files")
        source_stat = args.input.stat()
        require(stat.S_ISREG(source_stat.st_mode), "input must be a regular ELF file")
        require(source_stat.st_size <= MAX_FILE, "input ELF exceeds 64 MiB file limit")
        with args.input.open("rb") as source:
            # Bound the actual read too: the file may grow after stat().
            output = convert(source.read(MAX_FILE + 1), shared=args.shared, interpreter=args.interpreter,
                             require_tls=args.require_tls, abi_version=args.abi_version)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(prefix=args.output.name + ".", suffix=".tmp",
                                         dir=args.output.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(output)
        temporary.chmod(0o755)
        os.replace(temporary, args.output)
        temporary = None
    except (OSError, EpfError, struct.error) as error:
        parser.exit(1, f"elf_to_epf: {error}\n")
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
