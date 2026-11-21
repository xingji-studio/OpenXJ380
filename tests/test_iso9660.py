# SPDX-License-Identifier: Apache-2.0
"""Exercise the production reader under sanitizers, including sparse Level 3 media.

xorriso is invoked only as a separate host fixture generator. No generator
implementation or library is imported, linked, or installed in the OS.
"""
import hashlib
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "driver/fs/iso9660"
SECTOR = 2048


def both16(value):
    return struct.pack("<H", value) + struct.pack(">H", value)


def both32(value):
    return struct.pack("<I", value) + struct.pack(">I", value)


def record(name, block, size, flags=0):
    if isinstance(name, str):
        name = name.encode("ascii")
    result = bytearray(33 + len(name) + (len(name) % 2 == 0))
    result[0] = len(result)
    result[2:10] = both32(block)
    result[10:18] = both32(size)
    result[18:25] = bytes((126, 11, 21, 12, 34, 56, 32))
    result[25] = flags
    result[28:32] = both16(1)
    result[32] = len(name)
    result[33:33 + len(name)] = name
    return result


def descriptor(root=20, sectors=64, joliet=False):
    result = bytearray(SECTOR)
    result[:7] = bytes((2 if joliet else 1,)) + b"CD001\x01"
    result[80:88] = both32(sectors)
    result[120:124] = both16(1)
    result[124:128] = both16(1)
    result[128:132] = both16(SECTOR)
    result[156:190] = record(b"\0", root, SECTOR, 2)
    result[881] = 1
    if joliet:
        result[88:91] = b"%/E"
    return result


def directory(entries, block=20, parent=20):
    data = bytearray(record(b"\0", block, SECTOR, 2) + record(b"\1", parent, SECTOR, 2))
    offsets = []
    for entry in entries:
        if len(data) % SECTOR + len(entry) > SECTOR:
            data.extend(bytes(SECTOR - len(data) % SECTOR))
        offsets.append(block * SECTOR + len(data))
        data.extend(entry)
    data.extend(bytes((-len(data)) % SECTOR))
    return data, offsets


def fixture(joliet=False):
    # Model two real, separate namespaces: Joliet descriptors must not make
    # the primary ASCII tree invalid when a reader falls back to that tree.
    data = fixture()[0] if joliet else bytearray(64 * SECTOR)
    data[16 * SECTOR:17 * SECTOR] = descriptor()
    end = 17
    if joliet:
        data[17 * SECTOR:18 * SECTOR] = descriptor(root=22, joliet=True)
        end = 18
    data[end * SECTOR:end * SECTOR + 7] = b"\xffCD001\x01"
    names = ["hello.txt;1", "nested", "empty.;1", "fragmented.bin;1", "version.txt;1", "version.txt;2"]
    if joliet:
        names[0] = "中文文件.txt;1"
    encode = lambda name: name.encode("utf-16-be") if joliet else name.upper().encode("ascii")
    root_block, nested_block = (22, 23) if joliet else (20, 21)
    entries = [record(encode(names[0]), 24, 12000), record(encode(names[1]), nested_block, SECTOR, 2),
               record(encode(names[2]), 0, 0), record(encode(names[3]), 32, 4096, 0x80),
               record(encode(names[3]), 36, 99), record(encode(names[4]), 38, 3),
               record(encode(names[5]), 39, 3)]
    root, offsets = directory(entries, root_block, root_block)
    data[root_block * SECTOR:(root_block + 1) * SECTOR] = root
    nested, _ = directory([record(encode("inside.txt;1"), 40, 7)], nested_block, root_block)
    data[nested_block * SECTOR:(nested_block + 1) * SECTOR] = nested
    payload = bytes((i * 79 + 17) % 256 for i in range(12000))
    data[24 * SECTOR:24 * SECTOR + len(payload)] = payload
    data[32 * SECTOR:34 * SECTOR] = b"A" * 4096
    data[36 * SECTOR:36 * SECTOR + 99] = b"B" * 99
    data[38 * SECTOR:38 * SECTOR + 3] = b"old"
    data[39 * SECTOR:39 * SECTOR + 3] = b"new"
    data[40 * SECTOR:40 * SECTOR + 7] = b"nested\n"
    return data, offsets, payload


class Iso9660Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("g++"):
            raise unittest.SkipTest("g++ is required")
        cls.temporary = tempfile.TemporaryDirectory(prefix="xj380-iso9660-")
        cls.work = Path(cls.temporary.name)
        cls.binary = cls.work / "reader"
        subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-I", str(CORE),
                        str(CORE / "native.cpp"), str(ROOT / "tests/iso9660_harness.cpp"), "-o", str(cls.binary)],
                       check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def probe(self, data=None, operation="list", path="/", *arguments, image=None):
        if image is None:
            image = self.work / "synthetic.iso"
            image.write_bytes(data)
        return subprocess.run([str(self.binary), str(image), operation, path, *map(str, arguments)],
                              capture_output=True, timeout=15,
                              # The parser allocates nothing. LeakSanitizer's
                              # ptrace scan is unavailable in sandboxed hosts.
                              env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
                                   "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})

    def success(self, result):
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        return result.stdout

    def rejected(self, result, error=None):
        self.assertEqual(result.returncode, 2, result.stderr.decode(errors="replace"))
        self.assertIn(b"ERROR", result.stderr)
        if error is not None:
            self.assertIn(f" {error}\n".encode(), result.stderr)

    def test_plain_nested_empty_versions_and_ascii_case(self):
        data, _, payload = fixture()
        before = hashlib.sha256(data).digest()
        names = self.success(self.probe(data)).decode().splitlines()
        self.assertIn("hello.txt", names)
        self.assertNotIn(".", names)
        for path, expected in (("/HELLO.TXT", payload), ("/nested/inside.txt", b"nested\n"),
                               ("/empty", b""), ("/version.txt", b"new")):
            with self.subTest(path=path):
                self.assertEqual(self.success(self.probe(data, "read", path)), expected)
        self.assertEqual(hashlib.sha256((self.work / "synthetic.iso").read_bytes()).digest(), before)
        self.rejected(self.probe(data, "read", "/missing"), -2)
        self.rejected(self.probe(data, "read", "/nested"), -21)

    def test_random_ranges_eof_and_noncontiguous_sections(self):
        data, _, payload = fixture()
        files = {"hello.txt": payload, "fragmented.bin": b"A" * 4096 + b"B" * 99}
        for name, expected in files.items():
            for offset, count in ((0, 1), (511, 4096), (2043, 80), (4090, 70),
                                  (len(expected) - 3, 100), (len(expected), 100), (2**64 - 1, 8), (0, 0)):
                with self.subTest(name=name, offset=offset):
                    self.assertEqual(self.success(self.probe(data, "read", "/" + name, offset, count)),
                                     expected[offset:offset + count])

    def test_joliet_chinese_case_and_primary_fallback(self):
        data, _, payload = fixture(joliet=True)
        self.assertIn("中文文件.txt", self.success(self.probe(data)).decode().splitlines())
        for escape in (b"%/@", b"%/C", b"%/E"):
            data[17 * SECTOR + 88:17 * SECTOR + 91] = escape
            self.assertEqual(self.success(self.probe(data, "read", "/中文文件.txt")), payload)
        self.rejected(self.probe(data, "read", "/中文文件.TXT"), -2)
        data[17 * SECTOR + 88:17 * SECTOR + 91] = b"???"
        self.success(self.probe(data, "info"))
        self.assertTrue(self.success(self.probe(data, "info")).endswith(b" 0\n"))
        self.assertEqual(self.success(self.probe(data, "read", "/HELLO.TXT")), payload)

    def test_bad_descriptors_geometry_and_root_records(self):
        original, _, _ = fixture()
        for offset, replacement in ((16 * SECTOR + 1, b"XXXXX"), (16 * SECTOR + 84, b"\0\0\0\0"),
                                    (16 * SECTOR + 128, both16(512)), (16 * SECTOR + 120, both16(2)),
                                    (16 * SECTOR + 156 + 25, b"\0"), (20 * SECTOR + 33, b"\1"),
                                    (17 * SECTOR, b"\0"), (16 * SECTOR + 80, both32(1000))):
            with self.subTest(offset=offset):
                data = bytearray(original)
                data[offset:offset + len(replacement)] = replacement
                self.rejected(self.probe(data))
        for length in (0, 16 * SECTOR, 17 * SECTOR, 20 * SECTOR + 8):
            self.rejected(self.probe(original[:length]))

    def test_bad_directory_bounds_names_flags_and_interleave(self):
        original, offsets, _ = fixture()
        mutations = [(offsets[0], b"\1"), (offsets[0] + 32, b"\xff"),
                     (offsets[0] + 6, b"\0\0\0\0"), (offsets[0] + 2, both32(1000)),
                     (offsets[0] + 26, b"\1"), (offsets[0] + 25, b"\x60"),
                     (offsets[0] + 28, both16(2)), (offsets[0] + 33, b"/"),
                     (offsets[0] + 33, b"\0"), (offsets[0] + 33, b"\\")]
        for offset, replacement in mutations:
            with self.subTest(offset=offset, replacement=replacement):
                data = bytearray(original)
                data[offset:offset + len(replacement)] = replacement
                self.rejected(self.probe(data))

    def test_bad_joliet_lengths_and_surrogates(self):
        original, offsets, _ = fixture(joliet=True)
        for offset, replacement in ((offsets[0] + 32, b"\x03"), (offsets[0] + 33, b"\xd8\0"),
                                    (offsets[0] + 33, b"\0/")):
            data = bytearray(original)
            data[offset:offset + len(replacement)] = replacement
            self.rejected(self.probe(data))

    def test_descriptors_must_be_inside_declared_volume(self):
        data, _, _ = fixture()
        data[63 * SECTOR:63 * SECTOR + 7] = b"\xffCD001\x01"
        for sector in range(17, 63):
            data[sector * SECTOR:sector * SECTOR + 7] = b"\0CD001\x01"
        data[16 * SECTOR + 80:16 * SECTOR + 88] = both32(40)
        self.rejected(self.probe(data))

    def test_incomplete_mismatched_and_unaligned_section_chains(self):
        original, offsets, _ = fixture()
        for offset, replacement in ((offsets[3] + 10, both32(4095)), (offsets[4] + 33, b"X"),
                                    (offsets[4] + 25, b"\x80"), (offsets[4] + 10, both32(0))):
            data = bytearray(original)
            data[offset:offset + len(replacement)] = replacement
            self.rejected(self.probe(data))

    def test_associated_files_do_not_shadow_ordinary_names(self):
        data, offsets, payload = fixture()
        data[offsets[0] + 25] = 4
        self.assertNotIn("hello.txt", self.success(self.probe(data)).decode().splitlines())
        self.rejected(self.probe(data, "read", "/hello.txt"), -2)

    def test_sparse_file_over_four_gib(self):
        image = self.work / "large.iso"
        first = 0xfffff800
        second_block = 24 + first // SECTOR + 2
        sectors = second_block + 4
        root, _ = directory([record("LARGE.BIN;1", 24, first, 0x80), record("LARGE.BIN;1", second_block, 4096)])
        with image.open("wb") as stream:
            stream.truncate(sectors * SECTOR)
            stream.seek(16 * SECTOR)
            stream.write(descriptor(sectors=sectors))
            stream.write(b"\xffCD001\x01")
            stream.seek(20 * SECTOR)
            stream.write(root)
            stream.seek(24 * SECTOR + first - 8)
            stream.write(b"A" * 8)
            stream.seek(second_block * SECTOR)
            stream.write(b"B" * 4096)
        info = self.success(self.probe(operation="info", path="/large.bin", image=image)).split()
        self.assertEqual(int(info[1]), first + 4096)
        self.assertEqual(self.success(self.probe(None, "read", "/large.bin", first - 8, 32, image=image)),
                         b"A" * 8 + b"B" * 24)
        self.assertEqual(self.success(self.probe(None, "read", "/large.bin", 2**32 + 100, 32, image=image)), b"B" * 32)

    @unittest.skipUnless(shutil.which("xorriso"), "separate xorriso fixture generator missing")
    def test_real_xorriso_levels_joliet_and_large_directory(self):
        files = self.work / "files"
        (files / "nested").mkdir(parents=True, exist_ok=True)
        (files / "nested/readme.txt").write_bytes(b"ISO 9660 real media\n")
        (files / "中文长文件名.txt").write_text("光盘读取\n", encoding="utf-8")
        for index in range(1200):
            (files / f"entry-{index:04d}.txt").write_bytes(str(index).encode())
        for level in (1, 2, 3):
            image = self.work / f"level-{level}.iso"
            subprocess.run(["xorriso", "-as", "mkisofs", "-quiet", "-iso-level", str(level), "-J",
                            "-o", str(image), str(files)], check=True, capture_output=True)
            before = hashlib.sha256(image.read_bytes()).digest()
            names = self.success(self.probe(image=image)).decode().splitlines()
            self.assertEqual(len(names), 1202)
            for name, data in (("nested/readme.txt", b"ISO 9660 real media\n"),
                               ("中文长文件名.txt", "光盘读取\n".encode()), ("entry-1199.txt", b"1199")):
                self.assertEqual(self.success(self.probe(operation="read", path="/" + name, image=image)), data)
            self.assertEqual(hashlib.sha256(image.read_bytes()).digest(), before)

    def test_deterministic_malformed_media_never_crashes(self):
        original, offsets, _ = fixture(joliet=True)
        generator = random.Random(9660)
        for case in range(80):
            data = bytearray(original)
            start = generator.choice([16 * SECTOR, 17 * SECTOR, 20 * SECTOR, *offsets])
            for _ in range(generator.randrange(1, 8)):
                data[start + generator.randrange(34)] = generator.randrange(256)
            result = self.probe(data)
            self.assertIn(result.returncode, (0, 2), f"case {case}: {result.stderr.decode(errors='replace')}")


if __name__ == "__main__":
    unittest.main()
