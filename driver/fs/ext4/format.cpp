// SPDX-License-Identifier: Apache-2.0
// Original implementation from public ext4 on-disk facts, without imports.
#include "format.h"
#include <errno.h>

namespace xjext4 {
namespace {
constexpr uint32_t kBlock = 4096, kGroup = 32768, kInodes = 8192, kTable = kInodes / 16;
constexpr uint32_t kCompat = 0x2c, kIncompat = 0x2c2, kReadonly = 0x46b;

void zero(void *buffer, Size count)
{
    auto *bytes = static_cast<uint8_t *>(buffer);
    for (Size i = 0; i < count; ++i) bytes[i] = 0;
}

void copy(void *output, const void *input, Size count)
{
    auto *to = static_cast<uint8_t *>(output);
    const auto *from = static_cast<const uint8_t *>(input);
    for (Size i = 0; i < count; ++i) to[i] = from[i];
}

void le16(uint8_t *bytes, uint16_t value) { bytes[0] = uint8_t(value); bytes[1] = uint8_t(value >> 8); }
void le32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (8 * i));
}
void be32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (24 - 8 * i));
}
uint32_t crc(uint32_t state, const void *input, Size count)
{
    const auto *bytes = static_cast<const uint8_t *>(input);
    for (Size i = 0; i < count; ++i)
    {
        state ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            state = (state >> 1) ^ ((0U - (state & 1U)) & 0x82f63b78U);
    }
    return state;
}

bool power(uint32_t value, uint32_t base)
{
    if (!value) return false;
    while (value % base == 0) value /= base;
    return value == 1;
}

bool backup(uint32_t group)
{
    return group < 2 || power(group, 3) || power(group, 5) || power(group, 7);
}

struct LayoutStorage
{
    FormatGroupLocation *locations;
    uint8_t *descriptors;
};

int shape(uint64_t bytes, uint32_t &blocks, uint32_t &groups, uint32_t &gdt_blocks)
{
    if (bytes % kBlock || bytes < 32ULL * 1024 * 1024 || bytes / kBlock > 0xffffffffULL) return -EINVAL;
    blocks = uint32_t(bytes / kBlock);
    // Subtraction/division avoids overflowing at the last 32-bit block.
    groups = (blocks - 1) / kGroup + 1;
    gdt_blocks = uint32_t((uint64_t(groups) * 64 + kBlock - 1) / kBlock);
    if (uint64_t(groups) * kInodes > 0xffffffffULL) return -EOVERFLOW;
    return 0;
}

bool overlaps(const void *a, Size as, const void *b, Size bs)
{
    uintptr_t x = reinterpret_cast<uintptr_t>(a), y = reinterpret_cast<uintptr_t>(b);
    return x <= y ? y - x < as : x - y < bs;
}

int arena(const FormatGeometry &geometry, const FormatStorage *storage, LayoutStorage &result)
{
    result = {};
    if (geometry.groups <= 128) return 0;
    Size locations = Size(geometry.groups) * sizeof(FormatGroupLocation);
    Size required = locations + Size(geometry.gdt_blocks) * kBlock;
    if (!storage || !storage->data || storage->bytes < required) return -ENOMEM;
    uintptr_t base = reinterpret_cast<uintptr_t>(storage->data);
    if ((base & (alignof(FormatGroupLocation) - 1)) || required > uintptr_t(-1) - base ||
        overlaps(storage->data, required, &geometry, sizeof(geometry)) ||
        overlaps(storage->data, required, storage, sizeof(*storage))) return -EINVAL;
    result.locations = static_cast<FormatGroupLocation *>(storage->data);
    result.descriptors = static_cast<uint8_t *>(storage->data) + locations;
    return 0;
}

uint32_t location(const FormatGeometry &geometry, const LayoutStorage &storage, uint32_t group, unsigned kind)
{
    if (storage.locations)
    {
        const auto &item = storage.locations[group];
        return kind == 0 ? item.block_bitmap : kind == 1 ? item.inode_bitmap : item.inode_table;
    }
    return kind == 0 ? geometry.block_bitmap[group] : kind == 1 ? geometry.inode_bitmap[group]
                                                                   : geometry.inode_table[group];
}

void set_location(FormatGeometry &geometry, const LayoutStorage &storage, uint32_t group, unsigned kind,
                  uint32_t block)
{
    if (group < 128)
    {
        if (kind == 0) geometry.block_bitmap[group] = block;
        else if (kind == 1) geometry.inode_bitmap[group] = block;
        else geometry.inode_table[group] = block;
    }
    if (storage.locations)
    {
        auto &item = storage.locations[group];
        if (kind == 0) item.block_bitmap = block;
        else if (kind == 1) item.inode_bitmap = block;
        else item.inode_table = block;
    }
}

// Metadata allocations move monotonically. Previous allocations therefore lie
// before cursor; only fixed super/GDT copies can intersect the next candidate.
// Each requested run is shorter than one group, so at most two groups need
// checking. No bitmap proportional to filesystem capacity is needed.
bool allocate(const FormatGeometry &geometry, uint32_t count, uint64_t &cursor, uint32_t &first)
{
    while (cursor < geometry.blocks && count <= uint64_t(geometry.blocks) - cursor)
    {
        uint64_t end = cursor + count, jump = cursor;
        uint32_t first_group = uint32_t(cursor / kGroup), last_group = uint32_t((end - 1) / kGroup);
        for (uint32_t group = first_group; group <= last_group; ++group)
        {
            uint64_t begin = uint64_t(group) * kGroup, reserved_end = begin + 1 + geometry.gdt_blocks;
            if (backup(group) && cursor < reserved_end && begin < end) jump = reserved_end;
        }
        if (jump != cursor) { cursor = jump; continue; }
        first = uint32_t(cursor);
        cursor = end;
        return true;
    }
    return false;
}

int plan(const Ext4FormatDevice &device, const FormatOptions &options, FormatWorkspace &work,
         FormatGeometry &geometry, const FormatStorage *external)
{
    zero(&geometry, sizeof(geometry));
    if (!device.read || !device.write || !device.flush || options.journal_mib < 4 || options.journal_mib > 32 ||
        options.time_seconds > 0x37fffffffULL) return -EINVAL;
    int error = shape(device.bytes, geometry.blocks, geometry.groups, geometry.gdt_blocks);
    if (error) return error;
    uint8_t uuid = 0;
    for (unsigned i = 0; i < 16; ++i) uuid |= options.uuid[i];
    if (!uuid) return -EINVAL;
    LayoutStorage storage;
    error = arena(geometry, external, storage);
    if (error) return error;
    if (storage.locations)
    {
        Size bytes = Size(geometry.groups) * sizeof(FormatGroupLocation) + Size(geometry.gdt_blocks) * kBlock;
        if (overlaps(external->data, bytes, &work, sizeof(work)) ||
            overlaps(external->data, bytes, &options, sizeof(options)) ||
            overlaps(external->data, bytes, &device, sizeof(device))) return -EINVAL;
    }
    else storage.descriptors = work.descriptors;
    geometry.inodes = geometry.groups * kInodes;
    geometry.journal_blocks = options.journal_mib * 256;
    zero(storage.descriptors, Size(geometry.gdt_blocks) * kBlock);
    uint64_t allocated = 0;
    for (uint32_t group = 0; group < geometry.groups; ++group)
    {
        if (!backup(group)) continue;
        uint64_t first = uint64_t(group) * kGroup;
        if (geometry.blocks - first < 1 + geometry.gdt_blocks) return -ENOSPC;
        allocated += 1 + geometry.gdt_blocks;
    }
    uint64_t cursor = 0;
    for (unsigned kind = 0; kind < 3; ++kind)
    {
        uint32_t count = kind == 2 ? kTable : 1;
        for (uint32_t group = 0; group < geometry.groups; ++group)
        {
            uint32_t first = 0;
            if (!allocate(geometry, count, cursor, first)) return -ENOSPC;
            set_location(geometry, storage, group, kind, first);
            allocated += count;
        }
    }
    if (!allocate(geometry, geometry.journal_blocks, cursor, geometry.journal_first) ||
        !allocate(geometry, 1, cursor, geometry.root_block) ||
        !allocate(geometry, 1, cursor, geometry.lost_found_block)) return -ENOSPC;
    allocated += geometry.journal_blocks + 2;
    if (allocated >= geometry.blocks) return -ENOSPC;
    geometry.free_blocks = geometry.blocks - uint32_t(allocated);
    return 0;
}

uint32_t mark(uint8_t *bitmap, uint64_t group_first, uint32_t valid, uint64_t first, uint64_t count)
{
    uint64_t last = first + count, group_last = group_first + valid;
    if (last <= group_first || first >= group_last) return 0;
    uint32_t begin = uint32_t((first > group_first ? first : group_first) - group_first);
    uint32_t end = uint32_t((last < group_last ? last : group_last) - group_first), length = end - begin;
    while (begin < end && (begin & 7)) { bitmap[begin / 8] |= uint8_t(1U << (begin & 7)); ++begin; }
    while (end - begin >= 8) { bitmap[begin / 8] = 0xff; begin += 8; }
    while (begin < end) { bitmap[begin / 8] |= uint8_t(1U << (begin & 7)); ++begin; }
    return length;
}

uint32_t group_bitmap(const FormatGeometry &geometry, const LayoutStorage &storage, uint32_t group,
                      uint8_t *bitmap)
{
    zero(bitmap, kBlock);
    uint64_t first = uint64_t(group) * kGroup;
    uint32_t valid = uint32_t(geometry.blocks - first);
    if (valid > kGroup) valid = kGroup;
    uint32_t allocated = backup(group) ? mark(bitmap, first, valid, first, 1 + geometry.gdt_blocks) : 0;
    for (unsigned kind = 0; kind < 3; ++kind)
    {
        uint32_t count = kind == 2 ? kTable : 1, low = 0, high = geometry.groups;
        // Each of the three location lists is sorted. Locate the first range
        // intersecting this group and visit only its actual overlapping runs.
        while (low < high)
        {
            uint32_t middle = low + (high - low) / 2;
            if (uint64_t(location(geometry, storage, middle, kind)) + count <= first) low = middle + 1;
            else high = middle;
        }
        for (uint32_t index = low; index < geometry.groups; ++index)
        {
            uint32_t block = location(geometry, storage, index, kind);
            if (uint64_t(block) >= first + valid) break;
            allocated += mark(bitmap, first, valid, block, count);
        }
    }
    allocated += mark(bitmap, first, valid, geometry.journal_first, geometry.journal_blocks);
    allocated += mark(bitmap, first, valid, geometry.root_block, 1);
    allocated += mark(bitmap, first, valid, geometry.lost_found_block, 1);
    // Out-of-volume bits are reserved but do not contribute to used/free counts.
    mark(bitmap, first, kGroup, first + valid, kGroup - valid);
    return valid - allocated;
}

uint32_t inode_seed(uint32_t seed, uint32_t number)
{
    uint8_t fields[8] = {};
    le32(fields, number);
    return crc(seed, fields, sizeof(fields));
}

void inode(uint8_t *raw, uint32_t number, uint16_t mode, uint16_t links, uint32_t start, uint32_t blocks,
           uint64_t seconds, uint32_t seed)
{
    zero(raw, 256);
    le16(raw, mode);
    le16(raw + 0x1a, links);
    if (mode)
    {
        le32(raw + 4, blocks * kBlock);
        le32(raw + 0x1c, blocks * 8);
        le32(raw + 0x20, 0x80000);
        le16(raw + 0x28, 0xf30a);
        le16(raw + 0x2a, blocks ? 1 : 0);
        le16(raw + 0x2c, 4);
        if (blocks)
        {
            le16(raw + 0x38, uint16_t(blocks));
            le32(raw + 0x3c, start);
        }
    }
    le32(raw + 8, uint32_t(seconds)); le32(raw + 0xc, uint32_t(seconds));
    le32(raw + 0x10, uint32_t(seconds)); le32(raw + 0x90, uint32_t(seconds));
    le16(raw + 0x80, 32);
    uint32_t extra = uint32_t((seconds - uint64_t(int64_t(int32_t(seconds)))) >> 32) & 3;
    le32(raw + 0x84, extra); le32(raw + 0x88, extra);
    le32(raw + 0x8c, extra); le32(raw + 0x94, extra);
    uint32_t checksum = crc(inode_seed(seed, number), raw, 256);
    le16(raw + 0x7c, uint16_t(checksum));
    le16(raw + 0x82, uint16_t(checksum >> 16));
}

void directory(uint8_t *block, uint32_t number, uint32_t parent, bool root, uint32_t seed)
{
    zero(block, kBlock);
    le32(block, number); le16(block + 4, 12); block[6] = 1; block[7] = 2; block[8] = '.';
    le32(block + 12, parent); le16(block + 16, root ? 12 : kBlock - 24);
    block[18] = 2; block[19] = 2; block[20] = '.'; block[21] = '.';
    if (root)
    {
        le32(block + 24, 11); le16(block + 28, kBlock - 36); block[30] = 10; block[31] = 2;
        static const char name[] = "lost+found";
        copy(block + 32, name, 10);
    }
    le16(block + kBlock - 8, 12);
    block[kBlock - 5] = 0xde;
    le32(block + kBlock - 4, crc(inode_seed(seed, number), block, kBlock - 12));
}

void superblock(FormatWorkspace &work, const FormatGeometry &geometry, const FormatOptions &options, uint32_t group)
{
    uint8_t *super = work.super;
    zero(super, 1024);
    le32(super, geometry.inodes); le32(super + 4, geometry.blocks);
    le32(super + 0xc, geometry.free_blocks); le32(super + 0x10, geometry.inodes - 11);
    le32(super + 0x18, 2); le32(super + 0x1c, 2);
    le32(super + 0x20, kGroup); le32(super + 0x24, kGroup); le32(super + 0x28, kInodes);
    le32(super + 0x30, uint32_t(options.time_seconds));
    le16(super + 0x36, 0xffff); le16(super + 0x38, 0xef53); le16(super + 0x3a, 1);
    le16(super + 0x3c, 1); le32(super + 0x40, uint32_t(options.time_seconds));
    le32(super + 0x4c, 1); le32(super + 0x54, 11); le16(super + 0x58, 256); le16(super + 0x5a, group);
    le32(super + 0x5c, kCompat); le32(super + 0x60, kIncompat); le32(super + 0x64, kReadonly);
    copy(super + 0x68, options.uuid, 16); copy(super + 0x78, options.label, 16);
    le32(super + 0xe0, 8); copy(super + 0xec, options.uuid, 16);
    super[0xfc] = 1; super[0xfd] = 1; le16(super + 0xfe, 64);
    le32(super + 0x100, 4); le32(super + 0x108, uint32_t(options.time_seconds));
    uint8_t journal[256];
    uint32_t seed = crc(0xffffffffU, options.uuid, 16);
    inode(journal, 8, 0100600, 1, geometry.journal_first, geometry.journal_blocks,
          options.time_seconds, seed);
    copy(super + 0x10c, journal + 0x28, 60);
    le32(super + 0x14c, geometry.journal_blocks * kBlock);
    le16(super + 0x15c, 32); le16(super + 0x15e, 32); le32(super + 0x160, 1);
    super[0x174] = 7; super[0x175] = 1;
    super[0x274] = uint8_t(options.time_seconds >> 32);
    super[0x276] = uint8_t(options.time_seconds >> 32);
    super[0x277] = uint8_t(options.time_seconds >> 32);
    le32(super + 0x3fc, crc(0xffffffffU, super, 0x3fc));
}

bool store(const Ext4FormatDevice &device, uint32_t block, const void *bytes)
{
    return device.write(device.context, uint64_t(block) * kBlock, bytes, kBlock);
}

} // namespace

int format_storage_size(uint64_t bytes, Size &required)
{
    required = 0;
    uint32_t blocks = 0, groups = 0, gdt_blocks = 0;
    int error = shape(bytes, blocks, groups, gdt_blocks);
    if (error) return error;
    if (groups <= 128) return 0;
    uint64_t needed = uint64_t(groups) * sizeof(FormatGroupLocation) + uint64_t(gdt_blocks) * kBlock;
    if (needed > Size(-1)) return -EOVERFLOW;
    required = Size(needed);
    return 0;
}

int format_group_location(const FormatGeometry &geometry, const FormatStorage *external, uint32_t group,
                          FormatGroupLocation &result)
{
    result = {};
    uint32_t blocks = 0, groups = 0, gdt_blocks = 0;
    int error = shape(uint64_t(geometry.blocks) * kBlock, blocks, groups, gdt_blocks);
    if (error || groups != geometry.groups || gdt_blocks != geometry.gdt_blocks || group >= groups)
        return -EINVAL;
    LayoutStorage storage;
    error = arena(geometry, external, storage);
    if (error) return error;
    result = {location(geometry, storage, group, 0), location(geometry, storage, group, 1),
              location(geometry, storage, group, 2)};
    return 0;
}

int plan_format(const Ext4FormatDevice &device, const FormatOptions &options, FormatWorkspace &work,
                FormatGeometry &geometry, const FormatStorage *storage)
{
    return plan(device, options, work, geometry, storage);
}

int format(const Ext4FormatDevice &device, const FormatOptions &options, FormatWorkspace &work,
           FormatGeometry &geometry, const FormatStorage *external)
{
    int error = plan_format(device, options, work, geometry, external);
    LayoutStorage storage;
    if (!error) error = arena(geometry, external, storage);
    if (!error && !storage.locations) storage.descriptors = work.descriptors;
    if (error) return error;
    // Reject a backend that cannot flush before any destructive write. This
    // runs after the complete zero-I/O plan, so invalid arguments stay inert.
    if (!device.flush(device.context)) return -EIO;
    // Invalidate every copy before changing any data structure it describes.
    zero(work.block, kBlock);
    for (uint32_t group = 0; group < geometry.groups; ++group)
        if (backup(group) && !store(device, group * kGroup, work.block)) return -EIO;
    if (!device.flush(device.context)) return -EIO;
    uint32_t seed = crc(0xffffffffU, options.uuid, 16);
    for (uint32_t group = 0; group < geometry.groups; ++group)
    {
        uint8_t *descriptor = storage.descriptors + Size(group) * 64;
        uint32_t block_bitmap = location(geometry, storage, group, 0);
        uint32_t inode_bitmap = location(geometry, storage, group, 1);
        uint32_t inode_table = location(geometry, storage, group, 2);
        le32(descriptor, block_bitmap);
        le32(descriptor + 4, inode_bitmap);
        le32(descriptor + 8, inode_table);
        uint32_t free = group_bitmap(geometry, storage, group, work.block);
        uint32_t checksum = crc(seed, work.block, kBlock);
        le16(descriptor + 0x18, uint16_t(checksum)); le16(descriptor + 0x38, uint16_t(checksum >> 16));
        if (!store(device, block_bitmap, work.block)) return -EIO;
        le16(descriptor + 0xc, uint16_t(free));
        le16(descriptor + 0xe, uint16_t(kInodes - (group ? 0 : 11)));
        le16(descriptor + 0x10, group ? 0 : 2); le16(descriptor + 0x12, 4);
        le16(descriptor + 0x1c, uint16_t(kInodes - (group ? 0 : 11)));
        zero(work.block, kBlock);
        for (uint32_t bit = 0; bit < kGroup; ++bit)
            if (bit >= kInodes || (!group && bit < 11)) work.block[bit / 8] |= uint8_t(1U << (bit % 8));
        checksum = crc(seed, work.block, kInodes / 8);
        le16(descriptor + 0x1a, uint16_t(checksum)); le16(descriptor + 0x3a, uint16_t(checksum >> 16));
        if (!store(device, inode_bitmap, work.block)) return -EIO;
        zero(work.block, kBlock);
        if (!group)
        {
            for (uint32_t number = 1; number <= 11; ++number)
            {
                uint16_t mode = number == 2 ? 0040755 : number == 11 ? 0040700 : number == 8 ? 0100600 : 0;
                uint16_t links = number == 2 ? 3 : number == 11 ? 2 : number == 8 ? 1 : 0;
                uint32_t first = number == 2 ? geometry.root_block : number == 11 ? geometry.lost_found_block :
                                 number == 8 ? geometry.journal_first : 0;
                uint32_t count = number == 8 ? geometry.journal_blocks : mode ? 1 : 0;
                inode(work.block + (number - 1) * 256, number, mode, links, first, count, options.time_seconds, seed);
            }
        }
        for (uint32_t index = 0; index < kTable; ++index)
        {
            if (!store(device, inode_table + index, work.block)) return -EIO;
            if (!group && !index) zero(work.block, kBlock);
        }
        uint8_t encoded[4]; le32(encoded, group);
        checksum = crc(crc(seed, encoded, 4), descriptor, 64);
        le16(descriptor + 0x1e, uint16_t(checksum));
    }
    directory(work.block, 2, 2, true, seed);
    if (!store(device, geometry.root_block, work.block)) return -EIO;
    directory(work.block, 11, 2, false, seed);
    if (!store(device, geometry.lost_found_block, work.block)) return -EIO;
    zero(work.block, kBlock);
    be32(work.block, 0xc03b3998); be32(work.block + 4, 4);
    be32(work.block + 0xc, kBlock); be32(work.block + 0x10, geometry.journal_blocks);
    be32(work.block + 0x14, 1); be32(work.block + 0x18, 1);
    be32(work.block + 0x28, 0x11); copy(work.block + 0x30, options.uuid, 16);
    be32(work.block + 0x40, 1); work.block[0x50] = 4;
    be32(work.block + 0xfc, crc(0xffffffffU, work.block, 1024));
    for (uint32_t index = 0; index < geometry.journal_blocks; ++index)
    {
        if (!store(device, geometry.journal_first + index, work.block)) return -EIO;
        if (!index) zero(work.block, kBlock);
    }
    for (uint32_t group = 0; group < geometry.groups; ++group)
    {
        if (!backup(group)) continue;
        for (uint32_t index = 0; index < geometry.gdt_blocks; ++index)
            if (!store(device, group * kGroup + 1 + index, storage.descriptors + Size(index) * kBlock)) return -EIO;
    }
    if (!device.flush(device.context)) return -EIO;
    geometry.metadata_durable = true;
    geometry.publication_started = true;
    for (uint32_t group = 1; group < geometry.groups; ++group)
    {
        if (!backup(group)) continue;
        superblock(work, geometry, options, group);
        zero(work.block, kBlock); copy(work.block, work.super, 1024);
        if (!store(device, group * kGroup, work.block)) return -EIO;
    }
    if (!device.flush(device.context)) return -EIO;
    superblock(work, geometry, options, 0);
    zero(work.block, kBlock); copy(work.block + 1024, work.super, 1024);
    geometry.primary_attempted = true;
    if (!store(device, 0, work.block) || !device.flush(device.context)) return -EIO;
    return 0;
}

} // namespace xjext4
