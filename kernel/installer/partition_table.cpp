// SPDX-License-Identifier: Apache-2.0
#include <installer/partition_table.h>

namespace xjinstaller::partition {
namespace {
constexpr int     Invalid = -22, Io = -5, Unsupported = -95, NoSpace = -28, Stale = -116;
constexpr uint8_t EspType[16]   = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                   0xba, 0x4b, 0,    0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
constexpr uint8_t LinuxType[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                   0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
constexpr uint8_t DataType[16]  = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                   0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};

void zero(void *p, __SIZE_TYPE__ n)
{
    auto *b = static_cast<uint8_t *>(p);
    while (n--)
        *b++ = 0;
}

void copy(void *to, const void *from, __SIZE_TYPE__ n)
{
    auto *a = static_cast<uint8_t *>(to);
    auto *b = static_cast<const uint8_t *>(from);
    while (n--)
        *a++ = *b++;
}

bool equal(const void *a, const void *b, __SIZE_TYPE__ n)
{
    auto *x = static_cast<const uint8_t *>(a);
    auto *y = static_cast<const uint8_t *>(b);
    while (n--)
        if (*x++ != *y++) return false;
    return true;
}

bool empty(const void *p, __SIZE_TYPE__ n)
{
    auto *b = static_cast<const uint8_t *>(p);
    while (n--)
        if (*b++) return false;
    return true;
}

uint32_t get32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint64_t get64(const uint8_t *p)
{
    return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32;
}

void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = uint8_t(v >> (i * 8));
}

void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i)
        p[i] = uint8_t(v >> (i * 8));
}

uint64_t fingerprint(const Table &t)
{
    // Presentation correlation only. commit() compares the complete metadata.
    uint64_t value = 14695981039346656037ULL;
    auto     add   = [&](const uint8_t *p, unsigned bytes) {
        while (bytes--)
        {
            value ^= *p++;
            value *= 1099511628211ULL;
        }
    };
    add(t.mbr, 512);
    add(t.primary, 512);
    add(t.backup, 512);
    add(t.entries, sizeof(t.entries));
    value ^= t.bytes;
    return value ? value : 1;
}

bool header(const uint8_t *h, uint64_t sectors, bool backup)
{
    if (!equal(h, "EFI PART", 8) || get32(h + 8) != 0x10000 || get32(h + 12) < 92 || get32(h + 12) > 512 ||
        get32(h + 20) || get64(h + 24) != (backup ? sectors - 1 : 1) || get64(h + 32) != (backup ? 1 : sectors - 1) ||
        empty(h + 56, 16))
        return false;
    uint8_t saved[512];
    copy(saved, h, 512);
    zero(saved + 16, 4);
    return crc32(saved, get32(h + 12)) == get32(h + 16);
}

void refresh(Table &t)
{
    uint32_t crc = crc32(t.entries, t.count * 128);
    for (unsigned i = 0; i < 2; ++i)
    {
        auto *h = i ? t.backup : t.primary;
        put32(h + 88, crc);
        put32(h + 16, 0);
        put32(h + 16, crc32(h, get32(h + 12)));
    }
    t.fingerprint = fingerprint(t);
}

bool valid_entries(const Table &t)
{
    for (unsigned i = 0; i < t.count; ++i)
    {
        const auto *a = t.entries + i * 128;
        if (empty(a, 16)) continue;
        uint64_t first = get64(a + 32), last = get64(a + 40);
        if (empty(a + 16, 16) || first < t.first || last > t.last || first > last) return false;
        for (unsigned j = 0; j < i; ++j)
        {
            const auto *b = t.entries + j * 128;
            if (empty(b, 16)) continue;
            if (equal(a + 16, b + 16, 16) || (first <= get64(b + 40) && get64(b + 32) <= last)) return false;
        }
    }
    return true;
}

int check(const Table &t)
{
    uint64_t sectors = t.bytes / 512;
    if (t.scheme != Scheme::Gpt || t.bytes % 512 || sectors < 4096 || !t.count || t.count > 128 ||
        t.entry_bytes != 128 || !header(t.primary, sectors, false) || !header(t.backup, sectors, true))
        return Invalid;
    uint64_t blocks = (t.count * 128 + 511) / 512;
    if (t.first != get64(t.primary + 40) || t.last != get64(t.primary + 48) || t.first > t.last ||
        t.primary_lba != get64(t.primary + 72) || t.backup_lba != get64(t.backup + 72) || t.primary_lba < 2 ||
        t.primary_lba > t.first || blocks > t.first - t.primary_lba || t.backup_lba <= t.last ||
        t.backup_lba >= sectors - 1 || blocks > sectors - 1 - t.backup_lba ||
        !equal(t.primary + 40, t.backup + 40, 32) || !equal(t.primary + 80, t.backup + 80, 12) ||
        get32(t.primary + 80) != t.count || get32(t.primary + 84) != 128 ||
        get32(t.primary + 88) != crc32(t.entries, t.count * 128) || !valid_entries(t))
        return Invalid;
    return 0;
}
} // namespace

uint32_t crc32(const void *data, __SIZE_TYPE__ bytes)
{
    auto    *p     = static_cast<const uint8_t *>(data);
    uint32_t state = ~0U;
    while (bytes--)
    {
        state ^= *p++;
        for (unsigned i = 0; i < 8; ++i)
            state = (state >> 1) ^ ((0U - (state & 1)) & 0xedb88320U);
    }
    return ~state;
}

bool same(const Table &a, const Table &b)
{
    return a.scheme == b.scheme && a.bytes == b.bytes && a.count == b.count && a.primary_lba == b.primary_lba &&
           a.backup_lba == b.backup_lba && equal(a.mbr, b.mbr, 512) && equal(a.primary, b.primary, 512) &&
           equal(a.backup, b.backup, 512) && equal(a.entries, b.entries, sizeof(a.entries));
}

int load(const Device &d, Table &t)
{
    zero(&t, sizeof(t));
    if (!d.read || d.bytes % 512 || d.bytes < 4096 * 512) return Invalid;
    t.bytes = d.bytes;
    if (!d.read(d.context, 0, t.mbr, 512) || !d.read(d.context, 512, t.primary, 512) ||
        !d.read(d.context, d.bytes - 512, t.backup, 512))
        return Io;
    if (!equal(t.primary, "EFI PART", 8))
    {
        if (equal(t.backup, "EFI PART", 8)) return Invalid;
        if (empty(t.mbr, 512) && empty(t.primary, 512) && empty(t.backup, 512))
        {
            t.scheme = Scheme::Empty;
            t.first  = Alignment;
            t.last   = d.bytes / 512 - Alignment - 1;
        }
        else if (t.mbr[510] == 0x55 && t.mbr[511] == 0xaa)
        {
            t.scheme = Scheme::Mbr;
            t.count  = 4;
            // A protective/hybrid MBR with a missing GPT is damaged, not MBR.
            for (unsigned i = 0; i < 4; ++i)
                if (t.mbr[446 + i * 16 + 4] == 0xee) return Invalid;
        }
        t.fingerprint = fingerprint(t);
        return 0;
    }
    if (!header(t.primary, d.bytes / 512, false) || !header(t.backup, d.bytes / 512, true)) return Invalid;
    if (t.mbr[510] != 0x55 || t.mbr[511] != 0xaa) return Invalid;
    unsigned protective = 0;
    for (unsigned i = 0; i < 4; ++i)
    {
        const auto *p = t.mbr + 446 + i * 16;
        if (!p[4])
        {
            if (get32(p + 8) || get32(p + 12)) return Invalid;
            continue;
        }
        // Hybrid MBR edits require coordinated legacy-table changes. Keep
        // them outside the writable GPT-only profile rather than stale them.
        if (p[4] != 0xee) return Unsupported;
        uint64_t sectors = d.bytes / 512 - 1;
        if (get32(p + 8) != 1 || get32(p + 12) != (sectors > 0xffffffffULL ? 0xffffffffU : uint32_t(sectors)))
            return Invalid;
        ++protective;
    }
    if (protective != 1) return Invalid;
    t.scheme      = Scheme::Gpt;
    t.count       = get32(t.primary + 80);
    t.entry_bytes = get32(t.primary + 84);
    if (!t.count || t.count > 128 || t.entry_bytes != 128) return Unsupported;
    t.first         = get64(t.primary + 40);
    t.last          = get64(t.primary + 48);
    t.primary_lba   = get64(t.primary + 72);
    t.backup_lba    = get64(t.backup + 72);
    uint64_t blocks = (t.count * 128 + 511) / 512;
    if (t.primary_lba >= d.bytes / 512 || blocks > d.bytes / 512 - t.primary_lba || t.backup_lba >= d.bytes / 512 ||
        blocks > d.bytes / 512 - t.backup_lba)
        return Invalid;
    if (!d.read(d.context, t.primary_lba * 512, t.entries, blocks * 512)) return Io;
    uint8_t sector[512];
    for (uint64_t i = 0; i < blocks; ++i)
    {
        if (!d.read(d.context, (t.backup_lba + i) * 512, sector, 512)) return Io;
        if (!equal(sector, t.entries + i * 512, 512)) return Invalid;
    }
    int error = check(t);
    if (!error) t.fingerprint = fingerprint(t);
    return error;
}

int initialize(uint64_t bytes, const uint8_t guid[16], Table &t)
{
    if (bytes % 512 || bytes < 4 * MiB || !guid || empty(guid, 16)) return Invalid;
    zero(&t, sizeof(t));
    t.scheme      = Scheme::Gpt;
    t.bytes       = bytes;
    t.count       = 128;
    t.entry_bytes = 128;
    t.first       = 34;
    t.last        = bytes / 512 - 34;
    t.primary_lba = 2;
    t.backup_lba  = bytes / 512 - 33;
    t.mbr[448]    = 2;
    t.mbr[450]    = 0xee;
    t.mbr[451] = t.mbr[452] = t.mbr[453] = 0xff;
    put32(t.mbr + 454, 1);
    put32(t.mbr + 458, bytes / 512 - 1 > 0xffffffffULL ? 0xffffffffU : uint32_t(bytes / 512 - 1));
    t.mbr[510] = 0x55;
    t.mbr[511] = 0xaa;
    for (unsigned i = 0; i < 2; ++i)
    {
        auto *h = i ? t.backup : t.primary;
        copy(h, "EFI PART", 8);
        put32(h + 8, 0x10000);
        put32(h + 12, 92);
        put64(h + 24, i ? bytes / 512 - 1 : 1);
        put64(h + 32, i ? 1 : bytes / 512 - 1);
        put64(h + 40, t.first);
        put64(h + 48, t.last);
        copy(h + 56, guid, 16);
        put64(h + 72, i ? t.backup_lba : t.primary_lba);
        put32(h + 80, 128);
        put32(h + 84, 128);
    }
    refresh(t);
    return 0;
}

int entry(const Table &t, unsigned number, Entry &e)
{
    e = {};
    if (!number || number > t.count) return Invalid;
    e.number = number;
    if (t.scheme == Scheme::Mbr)
    {
        const auto *p = t.mbr + 446 + (number - 1) * 16;
        if (!p[4] || !get32(p + 12)) return 0;
        e.occupied = true;
        e.first    = get32(p + 8);
        e.last     = e.first + get32(p + 12) - 1;
        e.type[0]  = p[4];
        e.esp      = p[4] == 0xef;
        return e.last < t.bytes / 512 ? 0 : Invalid;
    }
    if (t.scheme != Scheme::Gpt) return Unsupported;
    const auto *p = t.entries + (number - 1) * 128;
    if (empty(p, 16)) return 0;
    e.occupied = true;
    copy(e.type, p, 16);
    copy(e.guid, p + 16, 16);
    e.first      = get64(p + 32);
    e.last       = get64(p + 40);
    e.attributes = get64(p + 48);
    copy(e.name, p + 56, 72);
    e.esp = equal(e.type, EspType, 16);
    return 0;
}

bool free_range(const Table &t, uint64_t first, uint64_t last)
{
    if (t.scheme != Scheme::Gpt || first < t.first || last > t.last || first > last) return false;
    for (unsigned i = 1; i <= t.count; ++i)
    {
        Entry e;
        if (entry(t, i, e)) return false;
        if (e.occupied && first <= e.last && e.first <= last) return false;
    }
    return true;
}

int create(Table &t, uint64_t first, uint64_t sectors, const uint8_t guid[16], Filesystem fs, bool esp,
           unsigned &number)
{
    number = 0;
    if (!guid || empty(guid, 16) || !sectors || first % Alignment || sectors % Alignment ||
        sectors - 1 > uint64_t(-1) - first || !free_range(t, first, first + sectors - 1) || fs < Filesystem::Fat32 ||
        fs > Filesystem::Ntfs || (esp && fs != Filesystem::Fat32))
        return Invalid;
    for (unsigned i = 0; i < t.count; ++i)
    {
        const auto *p = t.entries + i * 128;
        if (!empty(p, 16) && equal(p + 16, guid, 16)) return Invalid;
    }
    for (unsigned i = 0; i < t.count; ++i)
    {
        auto *p = t.entries + i * 128;
        if (!empty(p, 16)) continue;
        zero(p, 128);
        copy(p, esp ? EspType : fs == Filesystem::Ext4 ? LinuxType : DataType, 16);
        copy(p + 16, guid, 16);
        put64(p + 32, first);
        put64(p + 40, first + sectors - 1);
        const char *label = esp ? "EFI System Partition" : "XJ380";
        for (unsigned j = 0; label[j]; ++j)
            p[56 + j * 2] = uint8_t(label[j]);
        refresh(t);
        number = i + 1;
        return 0;
    }
    return NoSpace;
}

int erase(Table &t, unsigned number)
{
    Entry e;
    if (t.scheme != Scheme::Gpt || entry(t, number, e) || !e.occupied) return Invalid;
    zero(t.entries + (number - 1) * 128, 128);
    refresh(t);
    return 0;
}

int set_filesystem(Table &t, unsigned number, Filesystem fs)
{
    Entry e;
    if (t.scheme != Scheme::Gpt || entry(t, number, e) || !e.occupied || fs < Filesystem::Fat32 ||
        fs > Filesystem::Ntfs || (e.esp && fs != Filesystem::Fat32))
        return Invalid;
    if (!e.esp) copy(t.entries + (number - 1) * 128, fs == Filesystem::Ext4 ? LinuxType : DataType, 16);
    refresh(t);
    return 0;
}

int split_for_install(Table &t, unsigned number, const uint8_t guid[16], unsigned &esp_number)
{
    esp_number = 0;
    Entry e;
    if (entry(t, number, e) || !e.occupied || e.esp || e.first % Alignment) return Invalid;
    constexpr uint64_t esp_sectors = DefaultEspBytes / 512;
    if (e.last - e.first + 1 < esp_sectors + 32 * Alignment) return NoSpace;
    // Reserve a free slot before modifying the selected entry.
    unsigned slot = 0;
    for (unsigned i = 0; i < t.count; ++i)
        if (empty(t.entries + i * 128, 16))
        {
            slot = i + 1;
            break;
        }
    if (!slot || !guid || empty(guid, 16)) return NoSpace;
    for (unsigned i = 0; i < t.count; ++i)
        if (!empty(t.entries + i * 128, 16) && equal(t.entries + i * 128 + 16, guid, 16)) return Invalid;
    auto *root = t.entries + (number - 1) * 128;
    copy(root, LinuxType, 16);
    put64(root + 32, e.first + esp_sectors);
    refresh(t);
    return create(t, e.first, esp_sectors, guid, Filesystem::Fat32, true, esp_number);
}

int commit(const Device &d, const Table &before, const Table &after, Table &scratch)
{
    if (&scratch == &before || &scratch == &after || !d.write || !d.flush || d.bytes != before.bytes ||
        after.bytes != before.bytes || (before.scheme != Scheme::Gpt && before.scheme != Scheme::Empty) || check(after))
        return Invalid;
    int error = load(d, scratch);
    if (error) return error;
    if (!same(before, scratch)) return Stale;
    if (before.scheme == Scheme::Gpt &&
        (before.count != after.count || before.first != after.first || before.last != after.last ||
         before.primary_lba != after.primary_lba || before.backup_lba != after.backup_lba ||
         !equal(before.primary + 56, after.primary + 56, 16) || !equal(before.mbr, after.mbr, 512)))
        return Invalid;
    if (!d.flush(d.context)) return Io;
    const auto blocks = (after.count * 128 + 511) / 512;
    if (!d.write(d.context, after.backup_lba * 512, after.entries, blocks * 512) ||
        !d.write(d.context, d.bytes - 512, after.backup, 512) || !d.flush(d.context))
        return Io;
    uint8_t sector[512];
    if (!d.read(d.context, d.bytes - 512, sector, 512) || !equal(sector, after.backup, 512)) return Io;
    for (unsigned i = 0; i < blocks; ++i)
        if (!d.read(d.context, (after.backup_lba + i) * 512, sector, 512) ||
            !equal(sector, after.entries + i * 512, 512))
            return Io;
    if (!d.write(d.context, after.primary_lba * 512, after.entries, blocks * 512) ||
        !d.write(d.context, 512, after.primary, 512) ||
        (before.scheme == Scheme::Empty && !d.write(d.context, 0, after.mbr, 512)) || !d.flush(d.context))
        return Io;
    error = load(d, scratch);
    return error ? error : same(after, scratch) ? 0 : Io;
}

Filesystem identify(const uint8_t *p, __SIZE_TYPE__ bytes)
{
    if (!p || bytes < 512 || p[510] != 0x55 || p[511] != 0xaa)
    {
        if (p && bytes >= 1082 && p[1080] == 0x53 && p[1081] == 0xef) return Filesystem::Ext4;
        return Filesystem::Unknown;
    }
    if (equal(p + 3, "EXFAT   ", 8)) return Filesystem::ExFat;
    if (equal(p + 3, "NTFS    ", 8)) return Filesystem::Ntfs;
    if (equal(p + 82, "FAT32   ", 8)) return Filesystem::Fat32;
    if (bytes >= 1082 && p[1080] == 0x53 && p[1081] == 0xef) return Filesystem::Ext4;
    return Filesystem::Unknown;
}
} // namespace xjinstaller::partition
