// SPDX-License-Identifier: Apache-2.0
// Original pure GPT planner/serializer, expressed from GPT field definitions.
#include <installer/disk_layout.h>

namespace xjinstaller {
namespace {

constexpr int Invalid = -22;
constexpr uint64_t SectorsPerMiB = DiskMiB / DiskSectorBytes;
constexpr uint64_t TableSectors = DiskTableBytes / DiskSectorBytes;
constexpr uint32_t HeaderBytes = 92;
constexpr uint8_t RootType[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
};
constexpr uint8_t EspType[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

uint32_t get32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint64_t get64(const uint8_t *p)
{
    return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32;
}

void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
}

void put64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = uint8_t(value >> (i * 8));
}

void zero(uint8_t *p, DiskLayoutSize size)
{
    for (DiskLayoutSize i = 0; i < size; ++i) p[i] = 0;
}

void copy(uint8_t *target, const uint8_t *source, DiskLayoutSize size)
{
    for (DiskLayoutSize i = 0; i < size; ++i) target[i] = source[i];
}

bool equal(const uint8_t *a, const uint8_t *b, DiskLayoutSize size)
{
    for (DiskLayoutSize i = 0; i < size; ++i) if (a[i] != b[i]) return false;
    return true;
}

bool zeros(const uint8_t *p, DiskLayoutSize size)
{
    for (DiskLayoutSize i = 0; i < size; ++i) if (p[i]) return false;
    return true;
}

bool valid_range(const void *p, DiskLayoutSize size)
{
    uintptr_t begin = reinterpret_cast<uintptr_t>(p);
    return begin && size <= uintptr_t(-1) - begin;
}

bool overlaps(const void *a, DiskLayoutSize a_size, const void *b, DiskLayoutSize b_size)
{
    uintptr_t first = reinterpret_cast<uintptr_t>(a), second = reinterpret_cast<uintptr_t>(b);
    return first <= second ? second - first < a_size : first - second < b_size;
}

// IEEE reflected CRC32. Header checksum bytes are treated as zero without
// altering the immutable readback buffer or borrowing a 512-byte stack copy.
uint32_t crc32(const uint8_t *data, DiskLayoutSize size, bool header = false)
{
    uint32_t state = 0xffffffffU;
    for (DiskLayoutSize i = 0; i < size; ++i)
    {
        state ^= header && i >= 16 && i < 20 ? 0 : data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            state = (state >> 1) ^ ((0U - (state & 1U)) & 0xedb88320U);
    }
    return state ^ 0xffffffffU;
}

bool valid_identities(const DiskIdentities &ids)
{
    return !zeros(ids.disk, 16) && !zeros(ids.root, 16) && !zeros(ids.esp, 16) &&
           !equal(ids.disk, ids.root, 16) && !equal(ids.disk, ids.esp, 16) && !equal(ids.root, ids.esp, 16);
}

// Build/validate never treat caller-supplied geometry as authority. Recompute
// from fixed-size options and compare every field without struct padding.
bool valid_layout(const DiskLayout &layout)
{
    if (layout.esp_first_lba > layout.esp_last_lba ||
        layout.esp_last_lba - layout.esp_first_lba >= uint64_t(-1) / DiskSectorBytes) return false;
    DiskLayoutOptions options;
    options.disk_bytes = layout.disk_bytes;
    options.esp_bytes = (layout.esp_last_lba - layout.esp_first_lba + 1) * DiskSectorBytes;
    copy(options.identities.disk, layout.identities.disk, 16);
    copy(options.identities.root, layout.identities.root, 16);
    copy(options.identities.esp, layout.identities.esp, 16);
    DiskLayout expected;
    if (disk_layout_plan(options, expected)) return false;
    return layout.total_sectors == expected.total_sectors &&
           layout.primary_entries_lba == expected.primary_entries_lba &&
           layout.backup_entries_lba == expected.backup_entries_lba &&
           layout.first_usable_lba == expected.first_usable_lba &&
           layout.last_usable_lba == expected.last_usable_lba &&
           layout.root_first_lba == expected.root_first_lba && layout.root_last_lba == expected.root_last_lba &&
           layout.esp_first_lba == expected.esp_first_lba && layout.esp_last_lba == expected.esp_last_lba;
}

uint32_t protective_size(const DiskLayout &layout)
{
    return layout.total_sectors - 1 > 0xffffffffU ? 0xffffffffU : uint32_t(layout.total_sectors - 1);
}

void make_mbr(const DiskLayout &layout, uint8_t *mbr)
{
    zero(mbr, DiskSectorBytes);
    mbr[448] = 2;
    mbr[450] = 0xee;
    mbr[451] = 0xff;
    mbr[452] = 0xff;
    mbr[453] = 0xff;
    put32(mbr + 454, 1);
    put32(mbr + 458, protective_size(layout));
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
}

void make_entry(uint8_t *entry, const uint8_t *type, const uint8_t *identity, uint64_t first, uint64_t last,
                const char *label)
{
    copy(entry, type, 16);
    copy(entry + 16, identity, 16);
    put64(entry + 32, first);
    put64(entry + 40, last);
    for (unsigned i = 0; label[i]; ++i) entry[56 + i * 2] = uint8_t(label[i]);
}

void make_header(const DiskLayout &layout, uint8_t *header, uint32_t table_crc, bool backup)
{
    static const uint8_t signature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    zero(header, DiskSectorBytes);
    copy(header, signature, sizeof(signature));
    put32(header + 8, 0x00010000U);
    put32(header + 12, HeaderBytes);
    put64(header + 24, backup ? layout.total_sectors - 1 : 1);
    put64(header + 32, backup ? 1 : layout.total_sectors - 1);
    put64(header + 40, layout.first_usable_lba);
    put64(header + 48, layout.last_usable_lba);
    copy(header + 56, layout.identities.disk, 16);
    put64(header + 72, backup ? layout.backup_entries_lba : layout.primary_entries_lba);
    put32(header + 80, DiskEntryCount);
    put32(header + 84, DiskEntryBytes);
    put32(header + 88, table_crc);
    put32(header + 16, crc32(header, HeaderBytes, true));
}

bool check_mbr(const DiskLayout &layout, const uint8_t *mbr)
{
    return zeros(mbr, 448) && mbr[448] == 2 && mbr[449] == 0 && mbr[450] == 0xee &&
           mbr[451] == 0xff && mbr[452] == 0xff && mbr[453] == 0xff && get32(mbr + 454) == 1 &&
           get32(mbr + 458) == protective_size(layout) && zeros(mbr + 462, 48) &&
           mbr[510] == 0x55 && mbr[511] == 0xaa;
}

bool check_header(const DiskLayout &layout, const uint8_t *header, uint32_t table_crc, bool backup)
{
    static const uint8_t signature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    return equal(header, signature, sizeof(signature)) && get32(header + 8) == 0x00010000U &&
           get32(header + 12) == HeaderBytes && get32(header + 16) == crc32(header, HeaderBytes, true) &&
           get32(header + 20) == 0 && get64(header + 24) == (backup ? layout.total_sectors - 1 : 1) &&
           get64(header + 32) == (backup ? 1 : layout.total_sectors - 1) &&
           get64(header + 40) == layout.first_usable_lba && get64(header + 48) == layout.last_usable_lba &&
           equal(header + 56, layout.identities.disk, 16) &&
           get64(header + 72) == (backup ? layout.backup_entries_lba : layout.primary_entries_lba) &&
           get32(header + 80) == DiskEntryCount && get32(header + 84) == DiskEntryBytes &&
           get32(header + 88) == table_crc && zeros(header + HeaderBytes, DiskSectorBytes - HeaderBytes);
}

bool check_entry(const uint8_t *entry, const uint8_t *type, const uint8_t *identity, uint64_t first, uint64_t last,
                 const char *label)
{
    if (!equal(entry, type, 16) || !equal(entry + 16, identity, 16) || get64(entry + 32) != first ||
        get64(entry + 40) != last || get64(entry + 48) != 0) return false;
    unsigned length = 0;
    for (; label[length]; ++length)
        if (entry[56 + length * 2] != uint8_t(label[length]) || entry[57 + length * 2] != 0) return false;
    return zeros(entry + 56 + length * 2, 72 - length * 2);
}

} // namespace

int disk_layout_plan(const DiskLayoutOptions &options, DiskLayout &layout)
{
    if (!valid_range(&options, sizeof(options)) || !valid_range(&layout, sizeof(layout)) ||
        overlaps(&options, sizeof(options), &layout, sizeof(layout))) return Invalid;
    if (options.disk_bytes % DiskMiB || options.esp_bytes % DiskMiB ||
        options.disk_bytes < 98 * DiskMiB || options.esp_bytes < 64 * DiskMiB ||
        options.esp_bytes > options.disk_bytes - 2 * DiskMiB ||
        options.disk_bytes - 2 * DiskMiB - options.esp_bytes < 32 * DiskMiB ||
        !valid_identities(options.identities)) return Invalid;
    uint64_t total = options.disk_bytes / DiskSectorBytes;
    uint64_t esp_first = total - SectorsPerMiB - options.esp_bytes / DiskSectorBytes;
    layout.disk_bytes = options.disk_bytes;
    layout.total_sectors = total;
    layout.primary_entries_lba = 2;
    layout.backup_entries_lba = total - 1 - TableSectors;
    layout.first_usable_lba = 2 + TableSectors;
    layout.last_usable_lba = total - 2 - TableSectors;
    layout.root_first_lba = SectorsPerMiB;
    layout.root_last_lba = esp_first - 1;
    layout.esp_first_lba = esp_first;
    layout.esp_last_lba = total - SectorsPerMiB - 1;
    copy(layout.identities.disk, options.identities.disk, 16);
    copy(layout.identities.root, options.identities.root, 16);
    copy(layout.identities.esp, options.identities.esp, 16);
    return 0;
}

int disk_layout_build(const DiskLayout &layout, DiskLayoutWorkspace &workspace)
{
    if (!valid_range(&layout, sizeof(layout)) || !valid_range(&workspace, sizeof(workspace)) ||
        overlaps(&layout, sizeof(layout), &workspace, sizeof(workspace)) || !valid_layout(layout)) return Invalid;
    make_mbr(layout, workspace.mbr);
    zero(workspace.entries, DiskTableBytes);
    make_entry(workspace.entries, RootType, layout.identities.root, layout.root_first_lba,
               layout.root_last_lba, "XJ380");
    make_entry(workspace.entries + DiskEntryBytes, EspType, layout.identities.esp, layout.esp_first_lba,
               layout.esp_last_lba, "ESP");
    uint32_t table_crc = crc32(workspace.entries, DiskTableBytes);
    make_header(layout, workspace.primary_header, table_crc, false);
    make_header(layout, workspace.backup_header, table_crc, true);
    return 0;
}

int disk_layout_validate(const DiskLayout &layout, const DiskLayoutReadback &readback)
{
    if (!valid_range(&layout, sizeof(layout)) || !valid_range(&readback, sizeof(readback)) ||
        !valid_layout(layout)) return Invalid;
    const DiskLayoutSpan spans[5] = {
        readback.mbr, readback.primary_header, readback.primary_entries,
        readback.backup_header, readback.backup_entries
    };
    for (unsigned i = 0; i < 5; ++i)
    {
        DiskLayoutSize expected = i == 2 || i == 4 ? DiskTableBytes : DiskSectorBytes;
        if (spans[i].size != expected || !valid_range(spans[i].data, spans[i].size)) return Invalid;
        for (unsigned j = 0; j < i; ++j)
            if (overlaps(spans[i].data, spans[i].size, spans[j].data, spans[j].size)) return Invalid;
    }
    const uint8_t *entries = readback.primary_entries.data;
    if (!check_mbr(layout, readback.mbr.data) || !equal(entries, readback.backup_entries.data, DiskTableBytes) ||
        !check_entry(entries, RootType, layout.identities.root, layout.root_first_lba, layout.root_last_lba, "XJ380") ||
        !check_entry(entries + DiskEntryBytes, EspType, layout.identities.esp, layout.esp_first_lba,
                     layout.esp_last_lba, "ESP") ||
        !zeros(entries + 2 * DiskEntryBytes, DiskTableBytes - 2 * DiskEntryBytes))
        return Invalid;
    uint32_t checksum = crc32(entries, DiskTableBytes);
    return check_header(layout, readback.primary_header.data, checksum, false) &&
           check_header(layout, readback.backup_header.data, checksum, true) ? 0 : Invalid;
}

} // namespace xjinstaller
