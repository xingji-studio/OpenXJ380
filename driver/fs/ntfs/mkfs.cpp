// SPDX-License-Identifier: Apache-2.0
// Original encoder from public NTFS structure definitions and independently
// generated disposable-volume observations. See PROVENANCE.md for references.
#include "mkfs.h"
#include <errno.h>

namespace xjntfs::mkfs {
namespace {
constexpr uint64_t Cluster = 4096, MftClusters = 16, LogClusters = 512;
constexpr unsigned RecordBytes = 1024;
const char *const  Names[12]   = {"$MFT",    "$MFTMirr", "$LogFile", "$Volume", "$AttrDef", ".",
                                  "$Bitmap", "$Boot",    "$BadClus", "$Secure", "$UpCase",  "$Extend"};

void clear(void *p, __SIZE_TYPE__ n)
{
    auto *v = static_cast<uint8_t *>(p);
    while (n--)
        *v++ = 0;
}

void copy(void *p, const void *q, __SIZE_TYPE__ n)
{
    auto *a = static_cast<uint8_t *>(p);
    auto *b = static_cast<const uint8_t *>(q);
    while (n--)
        *a++ = *b++;
}

bool equal(const void *p, const void *q, __SIZE_TYPE__ n)
{
    auto *a = static_cast<const uint8_t *>(p);
    auto *b = static_cast<const uint8_t *>(q);
    while (n--)
        if (*a++ != *b++) return false;
    return true;
}

unsigned length(const char *s)
{
    unsigned n = 0;
    if (s)
        while (s[n])
            ++n;
    return n;
}

void put16(uint8_t *p, uint16_t value)
{
    p[0] = uint8_t(value);
    p[1] = uint8_t(value >> 8);
}
void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = uint8_t(value >> (8 * i));
}
void put64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i)
        p[i] = uint8_t(value >> (8 * i));
}
unsigned align8(unsigned n)
{
    return (n + 7) & ~7U;
}
uint64_t ref(unsigned n)
{
    return uint64_t(n) | uint64_t(n < 2 ? 1 : n) << 48;
}
void utf16(uint8_t *p, const char *s)
{
    for (unsigned i = 0; s && s[i]; ++i)
        put16(p + i * 2, uint8_t(s[i]));
}

struct Layout
{
    uint64_t clusters, bitmap_bytes, bitmap_clusters, mft_bitmap, mft, mirror, log, definitions, upcase, index,
        metadata_end;
    uint64_t file_lcn[12], file_bytes[12], file_allocated[12];
};

bool plan(uint64_t bytes, Layout &l)
{
    if (bytes < 32ULL * 1048576 || bytes % 512 || (bytes / 512 - 1) / 8 > 0xffffffffULL) return false;
    l                 = {};
    l.clusters        = (bytes / 512 - 1) / 8;
    l.bitmap_bytes    = ((l.clusters + 7) / 8 + 7) & ~7ULL;
    l.bitmap_clusters = (l.bitmap_bytes + Cluster - 1) / Cluster;
    l.mft_bitmap      = 2 + l.bitmap_clusters;
    l.mft             = l.mft_bitmap + 1;
    if (l.mft < 4) l.mft = 4;
    l.mirror       = l.mft + MftClusters;
    l.log          = l.mirror + 1;
    l.definitions  = l.log + LogClusters;
    l.upcase       = l.definitions + 1;
    l.index        = l.upcase + 32;
    l.metadata_end = l.index + 1;
    if (l.metadata_end >= l.clusters) return false;
    l.file_lcn[0]   = l.mft;
    l.file_bytes[0] = l.file_allocated[0] = MftClusters * Cluster;
    l.file_lcn[1]                         = l.mirror;
    l.file_bytes[1] = l.file_allocated[1] = Cluster;
    l.file_lcn[2]                         = l.log;
    l.file_bytes[2] = l.file_allocated[2] = LogClusters * Cluster;
    l.file_lcn[4]                         = l.definitions;
    l.file_bytes[4]                       = 16 * 160;
    l.file_allocated[4]                   = Cluster;
    l.file_lcn[6]                         = 2;
    l.file_bytes[6]                       = l.bitmap_bytes;
    l.file_allocated[6]                   = l.bitmap_clusters * Cluster;
    l.file_lcn[7]                         = 0;
    l.file_bytes[7] = l.file_allocated[7] = 8192;
    l.file_lcn[10]                        = l.upcase;
    l.file_bytes[10] = l.file_allocated[10] = 131072;
    return true;
}

// Per-file self-relative descriptor avoids sharing uninitialized security IDs.
// SYSTEM owns the file; SYSTEM and Administrators have full control.
void security(uint8_t *s)
{
    clear(s, 100);
    s[0] = 1;
    put16(s + 2, 0x8004);
    put32(s + 4, 72);
    put32(s + 8, 84);
    put32(s + 16, 20);
    s[20] = 2;
    put16(s + 22, 52);
    put16(s + 24, 2);
    put16(s + 30, 20);
    put32(s + 32, 0x001f01ff);
    s[36] = 1;
    s[37] = 1;
    s[43] = 5;
    put32(s + 44, 18);
    put16(s + 50, 24);
    put32(s + 52, 0x001f01ff);
    s[56] = 1;
    s[57] = 2;
    s[63] = 5;
    put32(s + 64, 32);
    put32(s + 68, 544);
    copy(s + 72, s + 36, 12);
    copy(s + 84, s + 56, 16);
}

unsigned filename(uint8_t *p, unsigned n, const Layout &l, uint64_t time)
{
    unsigned count = length(Names[n]);
    clear(p, 66 + count * 2);
    put64(p, ref(5));
    for (unsigned i = 0; i < 4; ++i)
        put64(p + 8 + i * 8, time);
    put64(p + 40, l.file_allocated[n]);
    put64(p + 48, l.file_bytes[n]);
    put32(p + 56, n == 5 || n == 11 ? 0x10000016 : n == 8 ? 0x206 : 6);
    p[64] = uint8_t(count);
    p[65] = 3;
    utf16(p + 66, Names[n]);
    return 66 + count * 2;
}

struct Record
{
    uint8_t *data;
    unsigned at = 56, instance = 0;
    bool     ok = true;

    Record(uint8_t *p, unsigned number) : data(p)
    {
        clear(p, RecordBytes);
        copy(p, "FILE", 4);
        put16(p + 4, 48);
        put16(p + 6, 3);
        put16(p + 16, uint16_t(number < 2 ? 1 : number));
        put16(p + 18, number < 12 ? 1 : 0);
        put16(p + 20, 56);
        put16(p + 22, number < 16 ? (number == 5 || number == 11 ? 3 : 1) : 0);
        put32(p + 28, RecordBytes);
        put32(p + 44, number);
    }

    void resident(unsigned type, const void *value, unsigned bytes, const char *name = nullptr)
    {
        unsigned names = length(name), offset = align8(24 + names * 2), total = align8(offset + bytes);
        if (at + total + 8 > RecordBytes)
        {
            ok = false;
            return;
        }
        auto *a = data + at;
        put32(a, type);
        put32(a + 4, total);
        a[9] = uint8_t(names);
        put16(a + 10, names ? 24 : 0);
        put16(a + 14, uint16_t(instance++));
        put32(a + 16, bytes);
        put16(a + 20, uint16_t(offset));
        if (type == 0x30) a[22] = 1;
        utf16(a + 24, name);
        copy(a + offset, value, bytes);
        at += total;
    }

    void stream(unsigned type, uint64_t lcn, uint64_t allocated, uint64_t bytes, const char *name = nullptr,
                bool sparse = false)
    {
        unsigned names = length(name), base = sparse ? 72 : 64, offset = align8(base + names * 2);
        uint64_t clusters    = (bytes + Cluster - 1) / Cluster;
        unsigned count_bytes = 1, location_bytes = sparse ? 0 : 1;
        while (count_bytes < 8 && clusters >> (count_bytes * 8))
            ++count_bytes;
        while (location_bytes && location_bytes < 8 && lcn >> (location_bytes * 8 - 1))
            ++location_bytes;
        unsigned total = align8(offset + 2 + count_bytes + location_bytes);
        if (!clusters || at + total + 8 > RecordBytes)
        {
            ok = false;
            return;
        }
        auto *a = data + at;
        put32(a, type);
        put32(a + 4, total);
        a[8] = 1;
        a[9] = uint8_t(names);
        put16(a + 10, names ? uint16_t(base) : 0);
        put16(a + 12, sparse ? 0x8000 : 0);
        put16(a + 14, uint16_t(instance++));
        put64(a + 24, clusters - 1);
        put16(a + 32, uint16_t(offset));
        put64(a + 40, allocated);
        put64(a + 48, bytes);
        put64(a + 56, sparse ? 0 : bytes);
        utf16(a + base, name);
        a[offset] = uint8_t(count_bytes | location_bytes << 4);
        for (unsigned i = 0; i < count_bytes; ++i)
            a[offset + 1 + i] = uint8_t(clusters >> (8 * i));
        for (unsigned i = 0; i < location_bytes; ++i)
            a[offset + 1 + count_bytes + i] = uint8_t(lcn >> (8 * i));
        at += total;
    }

    bool finish()
    {
        if (!ok) return false;
        put32(data + at, 0xffffffffU);
        put32(data + 24, at + 8);
        put16(data + 40, uint16_t(instance));
        put16(data + 48, 0xa55a);
        for (unsigned i = 0; i < 2; ++i)
        {
            copy(data + 50 + i * 2, data + (i + 1) * 512 - 2, 2);
            put16(data + (i + 1) * 512 - 2, 0xa55a);
        }
        return true;
    }
};

unsigned index_root(uint8_t *p, unsigned rule, bool child)
{
    clear(p, 64);
    put32(p, rule == 1 ? 0x30 : 0);
    put32(p + 4, rule);
    put32(p + 8, 4096);
    p[12] = 1;
    put32(p + 16, 16);
    put32(p + 20, child ? 40 : 32);
    put32(p + 24, child ? 40 : 32);
    p[28] = child ? 1 : 0;
    put16(p + 40, child ? 24 : 16);
    put16(p + 44, child ? 3 : 2);
    return child ? 56 : 48;
}

bool encode(Workspace &w, const Layout &l, const Options &options, uint64_t bytes)
{
    clear(&w, sizeof(w));
    auto *b = w.boot;
    b[0]    = 0xeb;
    b[1]    = 0x52;
    b[2]    = 0x90;
    copy(b + 3, "NTFS    ", 8);
    put16(b + 11, 512);
    b[13] = 8;
    b[21] = 0xf8;
    put16(b + 24, 63);
    put16(b + 26, 255);
    put32(b + 28, uint32_t(options.partition_lba));
    b[36] = 0x80;
    b[38] = 0x80;
    put64(b + 40, bytes / 512 - 1);
    put64(b + 48, l.mft);
    put64(b + 56, l.mirror);
    b[64] = 0xf6;
    b[68] = 1;
    put64(b + 72, options.serial);
    b[84]         = 0xeb;
    b[85]         = 0xfe;
    b[510]        = 0x55;
    b[511]        = 0xaa;
    uint64_t time = 116444736000000000ULL + options.time_seconds * 10000000;
    for (unsigned n = 0; n < 64; ++n)
    {
        Record record(w.records[n], n);
        if (n >= 16)
        {
            if (!record.finish()) return false;
            continue;
        }
        uint8_t value[640] = {};
        for (unsigned i = 0; i < 4; ++i)
            put64(value + i * 8, time);
        put32(value + 32, n == 5 || n == 11 ? 0x16 : n == 8 ? 0x206 : 6);
        record.resident(0x10, value, 48);
        if (n < 12) record.resident(0x30, value, filename(value, n, l, time));
        security(value);
        record.resident(0x50, value, 100);
        if (n == 3)
        {
            utf16(value, "XJ380");
            record.resident(0x60, value, 10);
            clear(value, 12);
            value[8] = 3;
            value[9] = 1;
            record.resident(0x70, value, 12);
        }
        if (l.file_allocated[n < 12 ? n : 3])
            record.stream(0x80, l.file_lcn[n], l.file_allocated[n], l.file_bytes[n]);
        else if (n < 12 && n != 5 && n != 11)
            record.resident(0x80, nullptr, 0);
        if (n == 8) record.stream(0x80, 0, 0, l.clusters * Cluster, "$Bad", true);
        if (n == 9)
        {
            record.resident(0x80, nullptr, 0, "$SDS");
            record.resident(0x90, value, index_root(value, 0x12, false), "$SDH");
            record.resident(0x90, value, index_root(value, 0x10, false), "$SII");
        }
        if (n == 5 || n == 11)
        {
            record.resident(0x90, value, index_root(value, 1, n == 5), "$I30");
            if (n == 5)
            {
                record.stream(0xa0, l.index, Cluster, Cluster, "$I30");
                clear(value, 8);
                value[0] = 1;
                record.resident(0xb0, value, 8, "$I30");
            }
        }
        if (n == 0) { record.stream(0xb0, l.mft_bitmap, Cluster, 8); }
        if (!record.finish()) return false;
    }
    // All special filenames fit in a single leaf INDX, in $UpCase order.
    const unsigned order[] = {4, 8, 6, 7, 11, 2, 0, 1, 9, 10, 3, 5};
    copy(w.index, "INDX", 4);
    put16(w.index + 4, 40);
    put16(w.index + 6, 9);
    put32(w.index + 24, 40);
    put32(w.index + 32, 4096 - 24);
    unsigned at = 64;
    for (unsigned n : order)
    {
        auto    *p   = w.index + at;
        unsigned key = filename(p + 16, n, l, time), total = align8(16 + key);
        put64(p, ref(n));
        put16(p + 8, uint16_t(total));
        put16(p + 10, uint16_t(key));
        at += total;
    }
    put16(w.index + at + 8, 16);
    put16(w.index + at + 12, 2);
    at += 16;
    put32(w.index + 28, at - 24);
    put16(w.index + 40, 0xa55a);
    for (unsigned i = 0; i < 8; ++i)
    {
        copy(w.index + 42 + i * 2, w.index + (i + 1) * 512 - 2, 2);
        put16(w.index + (i + 1) * 512 - 2, 0xa55a);
    }
    const char *const attributes[] = {"$STANDARD_INFORMATION",
                                      "$ATTRIBUTE_LIST",
                                      "$FILE_NAME",
                                      "$OBJECT_ID",
                                      "$SECURITY_DESCRIPTOR",
                                      "$VOLUME_NAME",
                                      "$VOLUME_INFORMATION",
                                      "$DATA",
                                      "$INDEX_ROOT",
                                      "$INDEX_ALLOCATION",
                                      "$BITMAP",
                                      "$REPARSE_POINT",
                                      "$EA_INFORMATION",
                                      "$EA",
                                      "$LOGGED_UTILITY_STREAM"};
    for (unsigned i = 0; i < 15; ++i)
    {
        auto *p = w.definitions + i * 160;
        utf16(p, attributes[i]);
        put32(p + 128, i == 14 ? 0x100 : (i + 1) * 16);
        const uint32_t flags[] = {0x40, 0x80, 0x42, 0x40, 0x80, 0x40, 0x40, 0, 0x40, 0x80, 0x80, 0x80, 0x40, 0, 0x80};
        put32(p + 140, flags[i]);
        put64(p + 152, uint64_t(-1));
        if (i == 0)
        {
            put32(p + 140, 0x40);
            put64(p + 144, 48);
            put64(p + 152, 72);
        }
        if (i == 2)
        {
            put32(p + 140, 0x42);
            put64(p + 144, 68);
            put64(p + 152, 578);
        }
        if (i == 3) put64(p + 152, 256);
        if (i == 5)
        {
            put64(p + 144, 2);
            put64(p + 152, 256);
        }
        if (i == 11) put64(p + 152, 16384);
        if (i == 12)
        {
            put64(p + 144, 8);
            put64(p + 152, 8);
        }
        if (i == 13 || i == 14) put64(p + 152, 65536);
        if (i == 6)
        {
            put32(p + 140, 0x40);
            put64(p + 144, 12);
            put64(p + 152, 12);
        }
    }
    return true;
}

struct Writer
{
    const Device &device;
    Status       &status;
    bool          io(bool result)
    {
        status.io_started = true;
        if (!result) status.io_failed = true;
        return result;
    }
    bool write(uint64_t offset, const void *p, __SIZE_TYPE__ n)
    {
        return !status.io_failed && offset <= device.bytes && n <= device.bytes - offset &&
               io(device.write(device.context, offset, p, n));
    }
    bool read(uint64_t offset, void *p, __SIZE_TYPE__ n)
    {
        return !status.io_failed && offset <= device.bytes && n <= device.bytes - offset &&
               io(device.read(device.context, offset, p, n));
    }
    bool flush()
    {
        return !status.io_failed && io(device.flush(device.context));
    }
};

void bitmap(uint8_t *data, uint64_t byte_offset, unsigned bytes, const Layout &l)
{
    clear(data, bytes);
    for (unsigned i = 0; i < bytes; ++i)
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            uint64_t cluster = (byte_offset + i) * 8 + bit;
            if (cluster < 3 + l.bitmap_clusters || (cluster >= l.mft && cluster < l.metadata_end) ||
                cluster >= l.clusters)
                data[i] |= uint8_t(1U << bit);
        }
}
} // namespace

int format(const Device &device, const Options &options, Workspace &work, Status &status)
{
    status = {};
    Layout layout;
    if (!device.read || !device.write || !device.flush || !options.serial ||
        options.time_seconds > (uint64_t(-1) - 116444736000000000ULL) / 10000000 || !plan(device.bytes, layout) ||
        !encode(work, layout, options, device.bytes))
        return -EINVAL;
    Writer io{device, status};
    clear(work.buffer, 512);
    if (!io.flush() || !io.write(0, work.buffer, 512) || !io.write(device.bytes - 512, work.buffer, 512) || !io.flush())
        return -EIO;
    if (!io.write(layout.mft * Cluster, work.records, sizeof(work.records)) ||
        !io.write(layout.mirror * Cluster, work.records, 4096) ||
        !io.write(layout.definitions * Cluster, work.definitions, 4096) ||
        !io.write(layout.index * Cluster, work.index, 4096))
        return -EIO;
    clear(work.buffer, 4096);
    work.buffer[0] = work.buffer[1] = 0xff;
    if (!io.write(layout.mft_bitmap * Cluster, work.buffer, 4096)) return -EIO;
    for (unsigned i = 0; i < sizeof(work.buffer); ++i)
        work.buffer[i] = 0xff;
    for (uint64_t at = 0; at < LogClusters * Cluster; at += sizeof(work.buffer))
        if (!io.write(layout.log * Cluster + at, work.buffer, sizeof(work.buffer))) return -EIO;
    for (uint64_t at = 0; at < layout.bitmap_clusters * Cluster;)
    {
        unsigned count = unsigned(layout.bitmap_clusters * Cluster - at);
        if (count > sizeof(work.buffer)) count = sizeof(work.buffer);
        bitmap(work.buffer, at, count, layout);
        if (!io.write(2 * Cluster + at, work.buffer, count)) return -EIO;
        at += count;
    }
    for (unsigned part = 0; part < 2; ++part)
    {
        for (unsigned i = 0; i < 32768; ++i)
        {
            uint32_t c     = part * 32768 + i;
            uint32_t upper = options.uppercase ? options.uppercase(c) : c >= 'a' && c <= 'z' ? c - 32 : c;
            put16(work.buffer + i * 2, uint16_t(upper <= 0xffff ? upper : c));
        }
        if (!io.write(layout.upcase * Cluster + part * 65536, work.buffer, 65536)) return -EIO;
    }
    if (!io.flush() || !io.read(layout.mft * Cluster, work.verify, 65536) || !equal(work.records, work.verify, 65536) ||
        !io.read(layout.mirror * Cluster, work.verify, 4096) || !equal(work.records, work.verify, 4096) ||
        !io.read(layout.index * Cluster, work.verify, 4096) || !equal(work.index, work.verify, 4096))
        return -EIO;
    if (!io.write(device.bytes - 512, work.boot, 512) || !io.flush() || !io.write(0, work.boot, 8192) || !io.flush() ||
        !io.read(0, work.verify, 8192) || !equal(work.boot, work.verify, 8192) ||
        !io.read(device.bytes - 512, work.verify, 512) || !equal(work.boot, work.verify, 512))
        return -EIO;
    status.complete = true;
    return 0;
}
} // namespace xjntfs::mkfs
