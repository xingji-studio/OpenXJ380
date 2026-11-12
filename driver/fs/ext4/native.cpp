// SPDX-License-Identifier: Apache-2.0
// Original implementation from the public ext4 on-disk format description:
// https://www.kernel.org/doc/html/latest/filesystems/ext4/
// No implementation code from Linux, lwext4, or another filesystem is used.
#include "native.h"
#include <errno.h>

namespace xjext4 {
namespace {

constexpr uint32_t kFileType = 0x2;
constexpr uint32_t k64Bit = 0x80;
constexpr uint32_t kChecksumSeed = 0x2000;
constexpr uint32_t kMetadataChecksum = 0x400;
constexpr uint32_t kGroupChecksum = 0x10;
constexpr uint32_t kExtents = 0x80000;
constexpr uint32_t kIndexed = 0x1000;
constexpr uint32_t kHugeFile = 0x40000;
constexpr uint16_t kTypeMask = 0170000;
constexpr uint16_t kRegular = 0100000;
constexpr uint16_t kDirectory = 0040000;
constexpr uint16_t kSymlink = 0120000;

uint16_t little16(const uint8_t *p)
{
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

uint32_t little32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void encode32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
}

void copy_bytes(void *destination, const void *source, Size length)
{
    auto *out = static_cast<uint8_t *>(destination);
    const auto *in = static_cast<const uint8_t *>(source);
    for (Size i = 0; i < length; ++i) out[i] = in[i];
}

void zero_bytes(void *destination, Size length)
{
    auto *out = static_cast<uint8_t *>(destination);
    for (Size i = 0; i < length; ++i) out[i] = 0;
}

bool power_of_two(uint32_t value)
{
    return value && !(value & (value - 1));
}

// Reflected Castagnoli CRC, without a final complement, as stored by ext4.
// A bitwise implementation keeps this small and independent of host runtimes.
uint32_t crc32c(uint32_t crc, const uint8_t *data, Size length)
{
    for (Size i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0);
    }
    return crc;
}

uint16_t crc16(uint16_t crc, const uint8_t *data, Size length)
{
    for (Size i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = uint16_t((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0));
    }
    return crc;
}

bool same_name(const char *a, const char *b)
{
    for (Size i = 0; i <= 255; ++i)
    {
        if (a[i] != b[i]) return false;
        if (!a[i]) return true;
    }
    return false;
}

} // namespace

void Volume::reset()
{
    reader_ = nullptr;
    context_ = nullptr;
    bytes_ = blocks_ = free_blocks_ = 0;
    block_size_ = inodes_ = free_inodes_ = 0;
    blocks_per_group_ = inodes_per_group_ = first_data_block_ = groups_ = 0;
    incompat_ = readonly_ = checksum_seed_ = 0;
    inode_size_ = descriptor_size_ = 0;
    zero_bytes(uuid_, sizeof(uuid_));
    ready_ = bootstrap_ = false;
}

int Volume::fetch(uint64_t offset, void *output, Size length) const
{
    if (!reader_ || !output || offset > bytes_ || length > bytes_ - offset) return -EIO;
    return reader_(context_, offset, output, length) ? 0 : -EIO;
}

int Volume::mount(Reader reader, void *context, uint64_t bytes)
{
    return mount_internal(reader, context, bytes, false);
}

int Volume::mount_internal(Reader reader, void *context, uint64_t bytes, bool recovery, bool bootstrap)
{
    reset();
    if (!reader || bytes < 2048) return -EINVAL;
    reader_ = reader;
    context_ = context;
    bytes_ = bytes;
    uint8_t super[1024];
    int result = fetch(1024, super, sizeof(super));
    if (result) return result;
    if (little16(super + 0x38) != 0xef53) return -EINVAL;
    // Recovery and concurrent-mount prevention require writes, which this
    // reader can never issue. Explicitly reject those volumes rather than
    // interpreting potentially uncommitted metadata as a consistent snapshot.
    uint32_t compatible = little32(super + 0x5c);
    incompat_ = little32(super + 0x60);
    readonly_ = little32(super + 0x64);
    if (bootstrap && (!recovery || !(incompat_ & 4U))) return -EUCLEAN;
    bootstrap_ = bootstrap;
    if ((compatible & ~0x23cU) || (incompat_ & ~(0x62c2U | (recovery ? 4U : 0U))) || (readonly_ & ~0x147bU))
        return -EOPNOTSUPP;
    if ((little32(super + 0x160) & 3U) == 3U) return -EUCLEAN;
    if ((little16(super + 0x3a) != 1 &&
         !(recovery && little16(super + 0x3a) == 0 && (incompat_ & 4U))) ||
        (!recovery && little32(super + 0xe8))) return -EUCLEAN;
    if (little32(super + 0x4c) != 1 || little32(super + 0x48) != 0) return -EOPNOTSUPP;
    uint32_t shift = little32(super + 0x18);
    if (shift > 2 || little32(super + 0x1c) != shift) return -EOPNOTSUPP;
    block_size_ = 1024U << shift;
    inodes_ = little32(super);
    free_inodes_ = little32(super + 0x10);
    blocks_ = little32(super + 4);
    free_blocks_ = little32(super + 0xc);
    if (incompat_ & k64Bit)
    {
        blocks_ |= uint64_t(little32(super + 0x150)) << 32;
        free_blocks_ |= uint64_t(little32(super + 0x158)) << 32;
    }
    first_data_block_ = little32(super + 0x14);
    blocks_per_group_ = little32(super + 0x20);
    inodes_per_group_ = little32(super + 0x28);
    inode_size_ = little16(super + 0x58);
    descriptor_size_ = incompat_ & k64Bit ? little16(super + 0xfe) : 32;
    if (first_data_block_ != (block_size_ == 1024 ? 1U : 0U) ||
        blocks_ <= first_data_block_ || blocks_ > bytes_ / block_size_ || inodes_ < 2 ||
        (!bootstrap_ && (free_blocks_ > blocks_ || free_inodes_ > inodes_)) || !blocks_per_group_ ||
        blocks_per_group_ > block_size_ * 8 || little32(super + 0x24) != blocks_per_group_ ||
        !inodes_per_group_ || inodes_per_group_ > block_size_ * 8 || inodes_per_group_ % 8 ||
        inode_size_ < 128 || inode_size_ > block_size_ || !power_of_two(inode_size_) ||
        descriptor_size_ < (incompat_ & k64Bit ? 64 : 32) || descriptor_size_ > block_size_ ||
        !power_of_two(descriptor_size_)) return -EINVAL;
    uint64_t groups = (blocks_ - first_data_block_ - 1) / blocks_per_group_ + 1;
    if (groups > 0xffffffffU || uint64_t(inodes_) > groups * inodes_per_group_) return -EINVAL;
    groups_ = uint32_t(groups);
    uint64_t descriptor_offset = uint64_t(first_data_block_ + 1) * block_size_;
    if (groups * descriptor_size_ > blocks_ * block_size_ - descriptor_offset) return -EINVAL;
    copy_bytes(uuid_, super + 0x68, sizeof(uuid_));
    checksum_seed_ = incompat_ & kChecksumSeed ? little32(super + 0x270)
                                             : crc32c(0xffffffffU, uuid_, sizeof(uuid_));
    if (readonly_ & kMetadataChecksum)
    {
        if (super[0x175] != 1) return -EOPNOTSUPP;
        if (!bootstrap_ && crc32c(0xffffffffU, super, 0x3fc) != little32(super + 0x3fc)) return -EUCLEAN;
    }
    else if (incompat_ & kChecksumSeed) return -EINVAL;
    ready_ = true;
    if (bootstrap_) return 0;
    Inode root;
    result = read_inode(2, root);
    if (!result && (root.mode & kTypeMask) != kDirectory) result = -EUCLEAN;
    if (result) ready_ = false;
    return result;
}

int Volume::read_group(uint32_t group, uint8_t *descriptor) const
{
    if (!ready_ || group >= groups_) return -EINVAL;
    uint64_t offset = uint64_t(first_data_block_ + 1) * block_size_ + uint64_t(group) * descriptor_size_;
    int result = fetch(offset, descriptor, descriptor_size_);
    if (result) return result;
    uint8_t encoded_group[4];
    encode32(encoded_group, group);
    uint16_t expected = little16(descriptor + 0x1e);
    if (readonly_ & kMetadataChecksum)
    {
        descriptor[0x1e] = descriptor[0x1f] = 0;
        uint32_t sum = crc32c(checksum_seed_, encoded_group, sizeof(encoded_group));
        if (!bootstrap_ && uint16_t(crc32c(sum, descriptor, descriptor_size_)) != expected) return -EUCLEAN;
    }
    else if (readonly_ & kGroupChecksum)
    {
        uint16_t sum = crc16(0xffff, uuid_, sizeof(uuid_));
        sum = crc16(sum, encoded_group, sizeof(encoded_group));
        sum = crc16(sum, descriptor, 0x1e);
        if (descriptor_size_ > 32) sum = crc16(sum, descriptor + 32, descriptor_size_ - 32);
        if (!bootstrap_ && sum != expected) return -EUCLEAN;
    }
    return 0;
}

int Volume::read_raw_inode(uint32_t number, uint8_t *raw, uint64_t *location) const
{
    if (!ready_) return -EINVAL;
    if (!number || number > inodes_) return -ENOENT;
    uint32_t group = (number - 1) / inodes_per_group_;
    uint32_t index = (number - 1) % inodes_per_group_;
    if (group >= groups_) return -EUCLEAN;
    uint8_t scratch[4096];
    int result = read_group(group, scratch);
    if (result) return result;
    if (little16(scratch + 0x12) & 1) return -ENOENT; // Uninitialized inode table/bitmap.
    uint64_t table = little32(scratch + 8);
    uint64_t bitmap = little32(scratch + 4);
    uint32_t bitmap_checksum = little16(scratch + 0x1a);
    if (incompat_ & k64Bit)
    {
        table |= uint64_t(little32(scratch + 0x28)) << 32;
        bitmap |= uint64_t(little32(scratch + 0x24)) << 32;
        bitmap_checksum |= uint32_t(little16(scratch + 0x3a)) << 16;
    }
    uint64_t table_blocks = (uint64_t(inodes_per_group_) * inode_size_ + block_size_ - 1) / block_size_;
    if (!bitmap || bitmap >= blocks_ || !table || table >= blocks_ || table_blocks > blocks_ - table)
        return -EUCLEAN;
    result = fetch(bitmap * block_size_, scratch, block_size_);
    if (result) return result;
    if (readonly_ & kMetadataChecksum)
    {
        uint32_t calculated = crc32c(checksum_seed_, scratch, inodes_per_group_ / 8);
        if (descriptor_size_ == 32) calculated &= 0xffffU;
        if (!bootstrap_ && calculated != bitmap_checksum) return -EUCLEAN;
    }
    if (!(scratch[index / 8] & (1U << (index % 8)))) return -ENOENT;
    uint64_t offset = table * block_size_ + uint64_t(index) * inode_size_;
    result = fetch(offset, raw, inode_size_);
    if (!result && location) *location = offset;
    return result;
}

uint32_t Volume::inode_seed(const Inode &inode) const
{
    uint8_t numbers[8];
    encode32(numbers, inode.number);
    encode32(numbers + 4, inode.generation);
    return crc32c(checksum_seed_, numbers, sizeof(numbers));
}

int Volume::orphan_member(uint32_t number, bool &member) const
{
    member = false;
    uint8_t encoded_head[4];
    int result = fetch(1024 + 0xe8, encoded_head, sizeof(encoded_head));
    if (result) return result;
    uint32_t current = little32(encoded_head);
    for (uint32_t visited = 0; current && visited < inodes_; ++visited)
    {
        if (current > inodes_) return -EUCLEAN;
        if (current == number) { member = true; return 0; }
        uint8_t link[4096];
        result = read_raw_inode(current, link);
        if (result) return result;
        current = little32(link + 0x14);
    }
    return current ? -EUCLEAN : 0;
}

int Volume::read_inode(uint32_t number, Inode &inode) const
{
    uint8_t raw[4096];
    int result = read_raw_inode(number, raw);
    if (result) return result;
    uint16_t extra = inode_size_ > 128 ? little16(raw + 0x80) : 0;
    if (extra % 4 || uint32_t(extra) + 128 > inode_size_) return -EUCLEAN;
    inode.number = number;
    inode.generation = little32(raw + 0x64);
    if (readonly_ & kMetadataChecksum)
    {
        uint32_t expected = little16(raw + 0x7c);
        raw[0x7c] = raw[0x7d] = 0;
        if (extra >= 4)
        {
            expected |= uint32_t(little16(raw + 0x82)) << 16;
            raw[0x82] = raw[0x83] = 0;
        }
        uint32_t calculated = crc32c(inode_seed(inode), raw, inode_size_);
        if (extra < 4) calculated &= 0xffffU;
        if (calculated != expected) return -EUCLEAN;
    }
    inode.mode = little16(raw);
    inode.links = little16(raw + 0x1a);
    inode.flags = little32(raw + 0x20);
    if (!inode.links || little32(raw + 0x14))
    {
        // The traditional orphan list temporarily uses i_dtime as its next
        // pointer while a linked file is being truncated. Public read-only
        // mounts reject such volumes at mount time. An internal recovery
        // view may read a zero-link inode only after proving list membership.
        bool member = false;
        result = orphan_member(number, member);
        if (result) return result;
        if (!member) return -ENOENT;
    }
    uint16_t type = inode.mode & kTypeMask;
    if (type != kRegular && type != kDirectory && type != kSymlink) return -EOPNOTSUPP;
    if (inode.flags & (0x4U | 0x100U | 0x200U | 0x800U | 0x100000U | 0x200000U | 0x10000000U |
                       0x40000000U)) return -EOPNOTSUPP;
    inode.uid = little16(raw + 2) | (uint32_t(little16(raw + 0x78)) << 16);
    inode.gid = little16(raw + 0x18) | (uint32_t(little16(raw + 0x7a)) << 16);
    inode.size = little32(raw + 4) | (uint64_t(little32(raw + 0x6c)) << 32);
    inode.blocks_512 = little32(raw + 0x1c);
    if (readonly_ & 8) inode.blocks_512 |= uint64_t(little16(raw + 0x74)) << 32;
    if (inode.flags & kHugeFile)
    {
        if (!(readonly_ & 8)) return -EUCLEAN;
        inode.blocks_512 *= block_size_ / 512;
    }
    inode.atime = little32(raw + 8);
    inode.ctime = little32(raw + 0xc);
    inode.mtime = little32(raw + 0x10);
    inode.atime_nsec = inode.ctime_nsec = inode.mtime_nsec = 0;
    // Extended time stores a signed low word plus two epoch bits; adding the
    // epoch to an unsigned low word would misdecode dates starting in 2038.
    auto decode_time = [](uint32_t low, uint32_t extended, uint64_t &seconds, uint32_t &nanos) -> bool
    {
        int64_t value = int64_t(int32_t(low)) + (int64_t(extended & 3U) << 32);
        seconds = uint64_t(value);
        nanos = extended >> 2;
        return nanos < 1000000000U;
    };
    bool extended_time = inode_size_ >= 160 && extra >= 16;
    if (!decode_time(little32(raw + 8), extended_time ? little32(raw + 0x8c) : 0,
                     inode.atime, inode.atime_nsec) ||
        !decode_time(little32(raw + 0xc), extended_time ? little32(raw + 0x84) : 0,
                     inode.ctime, inode.ctime_nsec) ||
        !decode_time(little32(raw + 0x10), extended_time ? little32(raw + 0x88) : 0,
                     inode.mtime, inode.mtime_nsec)) return -EUCLEAN;
    inode.xattr_block = little32(raw + 0x68) | (uint64_t(little16(raw + 0x76)) << 32);
    if (inode.xattr_block >= blocks_) return -EUCLEAN;
    if ((inode.flags & kExtents) && !(incompat_ & 0x40)) return -EUCLEAN;
    uint64_t logical_blocks = uint64_t(1) << 32;
    if (!(inode.flags & kExtents))
    {
        uint64_t pointers = block_size_ / 4;
        logical_blocks = 12 + pointers + pointers * pointers + pointers * pointers * pointers;
    }
    if (inode.size > logical_blocks * block_size_) return -EFBIG;
    // Directories cannot be sparse. These bounds also prevent a tiny corrupt
    // volume from describing terabytes of repeated directory blocks and
    // consuming unbounded traversal time despite individually valid pointers.
    if (inode.blocks_512 > blocks_ * (block_size_ / 512)) return -EUCLEAN;
    if (type == kDirectory && (inode.size > blocks_ * block_size_ ||
                              inode.size > inode.blocks_512 * 512)) return -EUCLEAN;
    copy_bytes(inode.block_map, raw + 0x28, sizeof(inode.block_map));
    return 0;
}

int Volume::map_extents(const Inode &inode, uint32_t logical, uint64_t &physical, bool &unwritten,
                       BlockVisitor metadata, void *context) const
{
    uint8_t scratch[4096];
    const uint8_t *node = inode.block_map;
    uint32_t capacity = 4;
    uint16_t expected_depth = little16(node + 6);
    if (expected_depth > 5) return -EUCLEAN;
    uint64_t lower = 0, upper = uint64_t(1) << 32;
    // Depth decreases on every step. Corrupt child pointers therefore cannot
    // create a cycle or grow the call stack; only this single block is used.
    for (unsigned level = 0; level <= 5; ++level)
    {
        uint16_t entries = little16(node + 2);
        uint16_t maximum = little16(node + 4);
        if (little16(node) != 0xf30a || little16(node + 6) != expected_depth ||
            !maximum || maximum > capacity || entries > maximum || (expected_depth && !entries))
            return -EUCLEAN;
        if (level && (readonly_ & kMetadataChecksum))
        {
            uint32_t tail = 12 + uint32_t(maximum) * 12;
            if (tail + 4 > block_size_ || crc32c(inode_seed(inode), node, tail) != little32(node + tail))
                return -EUCLEAN;
        }
        uint64_t previous_end = lower;
        int chosen = -1;
        for (uint16_t i = 0; i < entries; ++i)
        {
            const uint8_t *entry = node + 12 + Size(i) * 12;
            uint64_t start = little32(entry);
            if (start < lower || start >= upper || (i && start < previous_end)) return -EUCLEAN;
            if (!expected_depth)
            {
                uint16_t encoded_length = little16(entry + 4);
                uint32_t length = encoded_length > 32768 ? encoded_length - 32768U : encoded_length;
                uint64_t disk = little32(entry + 8) | (uint64_t(little16(entry + 6)) << 32);
                if (!length || start + length > upper || !disk || disk >= blocks_ || length > blocks_ - disk)
                    return -EUCLEAN;
                previous_end = start + length;
                if (logical >= start && uint64_t(logical) < previous_end) chosen = i;
            }
            else
            {
                uint64_t disk = little32(entry + 4) | (uint64_t(little16(entry + 8)) << 32);
                if (!disk || disk >= blocks_) return -EUCLEAN;
                previous_end = start + 1;
                if (logical >= start) chosen = i;
            }
        }
        if (chosen < 0) return 0; // Sparse range before the first entry.
        const uint8_t *entry = node + 12 + Size(chosen) * 12;
        uint64_t start = little32(entry);
        if (!expected_depth)
        {
            physical = (little32(entry + 8) | (uint64_t(little16(entry + 6)) << 32)) + logical - start;
            unwritten = little16(entry + 4) > 32768;
            return 0;
        }
        uint64_t next = little32(entry + 4) | (uint64_t(little16(entry + 8)) << 32);
        if (metadata)
        {
            int error = metadata(context, next);
            if (error) return error;
        }
        lower = start;
        if (chosen + 1 < entries) upper = little32(entry + 12);
        int result = fetch(next * block_size_, scratch, block_size_);
        if (result) return result;
        node = scratch;
        capacity = (block_size_ - 12) / 12;
        --expected_depth;
    }
    return -EUCLEAN;
}

int Volume::map_block(const Inode &inode, uint32_t logical, uint64_t &physical, bool &unwritten,
                     BlockVisitor metadata, void *context) const
{
    physical = 0;
    unwritten = false;
    if (inode.flags & kExtents) return map_extents(inode, logical, physical, unwritten, metadata, context);
    if (logical < 12)
    {
        physical = little32(inode.block_map + logical * 4);
        return physical >= blocks_ ? -EUCLEAN : 0;
    }
    uint64_t remaining = logical - 12;
    uint64_t pointers = block_size_ / 4;
    uint64_t span = pointers;
    unsigned levels = 1;
    for (; levels <= 3; ++levels)
    {
        if (remaining < span) break;
        remaining -= span;
        span *= pointers;
    }
    if (levels > 3) return -EFBIG;
    physical = little32(inode.block_map + (11 + levels) * 4);
    while (levels--)
    {
        if (!physical) return 0;
        if (physical >= blocks_) return -EUCLEAN;
        if (metadata)
        {
            int error = metadata(context, physical);
            if (error) return error;
        }
        span /= pointers;
        uint64_t index = remaining / span;
        remaining %= span;
        uint8_t encoded[4];
        int result = fetch(physical * block_size_ + index * 4, encoded, sizeof(encoded));
        if (result) return result;
        physical = little32(encoded);
    }
    return physical >= blocks_ ? -EUCLEAN : 0;
}

int Volume::read(const Inode &inode, uint64_t offset, void *output, Size length, Size &completed) const
{
    completed = 0;
    if (!ready_ || (!output && length)) return -EINVAL;
    if (!length || offset >= inode.size) return 0;
    uint16_t type = inode.mode & kTypeMask;
    if (type != kRegular && type != kDirectory && type != kSymlink) return -EOPNOTSUPP;
    if (length > inode.size - offset) length = Size(inode.size - offset);
    uint64_t attribute_sectors = inode.xattr_block ? block_size_ / 512 : 0;
    if (type == kSymlink && !(inode.flags & kExtents) && inode.size <= 60 &&
        inode.blocks_512 == attribute_sectors)
    {
        copy_bytes(output, inode.block_map + Size(offset), length);
        completed = length;
        return 0;
    }
    auto *destination = static_cast<uint8_t *>(output);
    while (completed < length)
    {
        uint64_t logical = offset / block_size_;
        if (logical > 0xffffffffU) return -EFBIG;
        uint32_t within = uint32_t(offset % block_size_);
        Size take = block_size_ - within;
        if (take > length - completed) take = length - completed;
        uint64_t physical;
        bool unwritten;
        int result = map_block(inode, uint32_t(logical), physical, unwritten);
        if (result) return result;
        if (!physical || unwritten) zero_bytes(destination + completed, take);
        else
        {
            result = fetch(physical * block_size_ + within, destination + completed, take);
            if (result) return result;
        }
        offset += take;
        completed += take;
    }
    return 0;
}

int Volume::check_directory_block(const Inode &inode, uint8_t *block, uint64_t logical) const
{
    bool root_index = (inode.flags & kIndexed) && logical == 0;
    if (root_index && (little16(block + 4) != 12 || block[6] != 1 || block[8] != '.' ||
                       little16(block + 16) != block_size_ - 12 || block[18] != 2 ||
                       block[20] != '.' || block[21] != '.' || little32(block + 24) ||
                       block[28] > 2 || block[29] != 8 || block[30] > 2 || block[31] ||
                       (block[30] == 2 && !(incompat_ & 0x4000U)))) return -EUCLEAN;
    if (!(readonly_ & kMetadataChecksum)) return 0;
    bool inner_index = (inode.flags & kIndexed) && !little32(block) &&
                       little16(block + 4) == block_size_ && !little16(block + 6);
    if (root_index || inner_index)
    {
        uint32_t offset = root_index ? 32 : 8;
        uint16_t limit = little16(block + offset);
        uint16_t count = little16(block + offset + 2);
        if (limit != (block_size_ - offset - 8) / 8 || !count || count > limit) return -EUCLEAN;
        for (uint16_t i = 0; i < count; ++i)
        {
            uint32_t target = little32(block + offset + Size(i) * 8 + 4);
            if (!target || target >= inode.size / block_size_) return -EUCLEAN;
        }
        uint32_t tail = offset + uint32_t(limit) * 8;
        uint32_t expected = little32(block + tail + 4);
        zero_bytes(block + tail + 4, 4);
        uint32_t checksum = crc32c(inode_seed(inode), block, offset + uint32_t(count) * 8);
        checksum = crc32c(checksum, block + tail, 8);
        encode32(block + tail + 4, expected);
        return checksum == expected ? 0 : -EUCLEAN;
    }
    uint8_t *tail = block + block_size_ - 12;
    if (little32(tail) || little16(tail + 4) != 12 || tail[6] || tail[7] != 0xde) return -EUCLEAN;
    return crc32c(inode_seed(inode), block, block_size_ - 12) == little32(tail + 8) ? 0 : -EUCLEAN;
}

int Volume::list_directory(const Inode &inode, DirectoryVisitor visitor, void *context) const
{
    if (!visitor || !ready_) return -EINVAL;
    if ((inode.mode & kTypeMask) != kDirectory) return -ENOTDIR;
    if (!inode.size || inode.size % block_size_) return -EUCLEAN;
    uint8_t block[4096];
    for (uint64_t offset = 0; offset < inode.size; offset += block_size_)
    {
        Size got = 0;
        int result = read(inode, offset, block, block_size_, got);
        if (result) return result;
        if (got != block_size_) return -EIO;
        result = check_directory_block(inode, block, offset / block_size_);
        if (result) return result;
        bool index_block = (inode.flags & kIndexed) &&
                           (!offset || (!little32(block) && little16(block + 4) == block_size_ &&
                                        !little16(block + 6)));
        // The checksum tail's 0xde file_type marker is not a legacy 16-bit
        // name length. Its structure/checksum was checked above; enumerate
        // only real leaf records, including on volumes without FILETYPE.
        uint32_t limit = block_size_ - ((readonly_ & kMetadataChecksum) && !index_block ? 12 : 0);
        uint32_t position = 0;
        while (position < limit)
        {
            if (limit - position < 8) return -EUCLEAN;
            const uint8_t *entry = block + position;
            uint32_t number = little32(entry);
            uint16_t record = little16(entry + 4);
            uint16_t name_length = incompat_ & kFileType ? entry[6] : little16(entry + 6);
            uint8_t type = incompat_ & kFileType ? entry[7] : 0;
            if (record < 8 || record % 4 || record > limit - position ||
                name_length > 255 || name_length > record - 8) return -EUCLEAN;
            if (number)
            {
                if (number > inodes_ || !name_length || type > 7) return -EUCLEAN;
                char name[256];
                for (uint16_t i = 0; i < name_length; ++i)
                {
                    if (!entry[8 + i] || entry[8 + i] == '/') return -EUCLEAN;
                    name[i] = char(entry[8 + i]);
                }
                name[name_length] = 0;
                result = visitor(context, name, number, type);
                if (result) return result < 0 ? result : 0;
            }
            position += record;
        }
    }
    return 0;
}

int Volume::lookup(const Inode &directory, const char *name, Inode &inode) const
{
    if (!name || !*name) return -EINVAL;
    struct Search
    {
        const char *name;
        uint32_t found;
    } search = {name, 0};
    int result = list_directory(directory, [](void *context, const char *entry, uint32_t number, uint8_t) -> int {
        auto *search = static_cast<Search *>(context);
        if (!same_name(search->name, entry)) return 0;
        search->found = number;
        return 1;
    }, &search);
    if (result) return result;
    return search.found ? read_inode(search.found, inode) : -ENOENT;
}

} // namespace xjext4
