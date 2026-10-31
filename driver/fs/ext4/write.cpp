// SPDX-License-Identifier: Apache-2.0
// Original bounded writer, based on on-disk format facts, not upstream code.
#include "write.h"
#include <errno.h>

namespace xjext4 {
namespace {

uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void put16(uint8_t *p, uint16_t value) { p[0] = uint8_t(value); p[1] = uint8_t(value >> 8); }
void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
}

void copy(void *out, const void *in, Size length)
{
    for (Size i = 0; i < length; ++i) static_cast<uint8_t *>(out)[i] = static_cast<const uint8_t *>(in)[i];
}

uint32_t crc(uint32_t seed, const uint8_t *bytes, Size length)
{
    for (Size i = 0; i < length; ++i)
    {
        seed ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) seed = (seed >> 1) ^ ((seed & 1) ? 0x82f63b78U : 0);
    }
    return seed;
}

bool is_power(uint32_t value, uint32_t base)
{
    if (!value) return false;
    while (value % base == 0) value /= base;
    return value == 1;
}

uint64_t descriptor_block(const uint8_t *descriptor, unsigned low, unsigned high, bool wide)
{
    return le32(descriptor + low) | (wide ? uint64_t(le32(descriptor + high)) << 32 : 0);
}

constexpr Size kStorageBudget = Size(256) * 1024 * 1024;
constexpr Size kBitmapPage = 4096;
struct StoragePlan
{
    Size metadata, groups, dirty, mapping, xattrs, holds;
    Size mapping_generations, xattr_generations, hold_generations;
    Size mapping_bytes, xattr_bytes, hold_bytes, total;
};

int plan_storage(uint64_t blocks, uint32_t inodes, uint32_t groups,
                 Size metadata_size, Size group_size, StoragePlan &plan)
{
    plan = {};
    if (blocks < 2 || inodes < 2 || !groups || groups > blocks) return -EINVAL;
    Size next = 0;
    auto append = [&](uint64_t count, Size width, Size &offset) -> int
    {
        if (count > Size(-1) / width || next > Size(-1) - 7) return -EOVERFLOW;
        next = (next + 7) & ~Size(7);
        Size bytes = Size(count) * width;
        if (bytes > Size(-1) - next) return -EOVERFLOW;
        if (bytes > kStorageBudget || next > kStorageBudget - bytes) return -E2BIG;
        offset = next;
        next += bytes;
        return 0;
    };
    uint64_t mapping_bits = blocks > inodes ? blocks : inodes;
    uint64_t mapping_bytes = mapping_bits / 8 + (mapping_bits % 8 != 0);
    uint64_t xattr_bytes = blocks / 8 + (blocks % 8 != 0);
    uint64_t hold_bytes = uint64_t(inodes) / 8 + (inodes % 8 != 0);
    int error = append(uint64_t(groups) * 4, metadata_size, plan.metadata);
    if (!error) error = append(groups, group_size, plan.groups);
    if (!error) error = append(groups, 1, plan.dirty);
    if (!error) error = append(mapping_bytes, 1, plan.mapping);
    if (!error) error = append(xattr_bytes, 1, plan.xattrs);
    if (!error) error = append(hold_bytes, 1, plan.holds);
    if (!error) error = append((mapping_bytes + kBitmapPage - 1) / kBitmapPage, 4, plan.mapping_generations);
    if (!error) error = append((xattr_bytes + kBitmapPage - 1) / kBitmapPage, 4, plan.xattr_generations);
    if (!error) error = append((hold_bytes + kBitmapPage - 1) / kBitmapPage, 4, plan.hold_generations);
    if (error) return error;
    plan.mapping_bytes = Size(mapping_bytes);
    plan.xattr_bytes = Size(xattr_bytes);
    plan.hold_bytes = Size(hold_bytes);
    plan.total = (next + 7) & ~Size(7);
    return 0;
}

} // namespace

int WriteSession::storage_size_for(uint64_t blocks, uint32_t inodes, uint32_t groups, Size &bytes)
{
    bytes = 0;
    StoragePlan plan;
    int error = plan_storage(blocks, inodes, groups, sizeof(MetadataRange), sizeof(GroupLayout), plan);
    if (!error) bytes = plan.total;
    return error;
}

int WriteSession::storage_size(const WriteDevice &device, Size &bytes)
{
    bytes = 0;
    if (!device.read || !device.write || !device.flush) return -EINVAL;
    uint8_t super[1024];
    if (!device.read(device.context, 1024, super, sizeof(super))) return -EIO;
    Volume volume = {};
    int error = volume.mount_internal(device.read, device.context, device.bytes, true,
                                       (le32(super + 0x60) & 4U) != 0);
    if (error) return error;
    if ((le32(super + 0x5c) & ~0x3cU) || (volume.readonly_ & ~0x47bU) ||
        !(le32(super + 0x5c) & 4U) || le32(super + 0xe4) || le32(super + 0xe0) != 8 ||
        volume.blocks_per_group_ % 8 || le32(super + 0x54) < 11 ||
        le32(super + 0x54) > volume.inodes_ ||
        uint64_t(volume.groups_ - 1) * volume.inodes_per_group_ >= volume.inodes_)
        return -EOPNOTSUPP;
    return storage_size_for(volume.blocks_, volume.inodes_, volume.groups_, bytes);
}

int WriteSession::configure_storage(const WriteStorage *storage)
{
    StoragePlan plan;
    int error = plan_storage(volume_.blocks_, volume_.inodes_, volume_.groups_,
                              sizeof(MetadataRange), sizeof(GroupLayout), plan);
    if (error) return error;
    if (!storage)
    {
        if (volume_.groups_ > 128 || volume_.blocks_ > uint64_t(sizeof(inline_mapping_)) * 8 ||
            volume_.inodes_ > uint64_t(sizeof(inline_holds_)) * 8) return -EOPNOTSUPP;
        metadata_ = inline_metadata_;
        metadata_capacity_ = 512;
        groups_ = inline_groups_;
        tx_dirty_ = inline_dirty_;
        mapping_bits_ = {inline_mapping_, inline_generation_[0], sizeof(inline_mapping_), 128,
                         volume_.blocks_ > volume_.inodes_ ? volume_.blocks_ : volume_.inodes_, 1};
        xattr_bits_ = {inline_xattrs_, inline_generation_[1], sizeof(inline_xattrs_), 128, volume_.blocks_, 1};
        held_bits_ = {inline_holds_, inline_generation_[2], sizeof(inline_holds_), 128, volume_.inodes_, 1};
    }
    else
    {
        uintptr_t start = reinterpret_cast<uintptr_t>(storage->data);
        if (!start || (start & 7) || storage->bytes > uintptr_t(-1) - start) return -EINVAL;
        if (storage->bytes < plan.total) return -ENOSPC;
        uintptr_t end = start + storage->bytes;
        auto overlaps = [&](const void *pointer, Size bytes) -> bool
        {
            uintptr_t first = reinterpret_cast<uintptr_t>(pointer);
            return first > uintptr_t(-1) - bytes || (start < first + bytes && first < end);
        };
        if (overlaps(this, sizeof(*this)) || overlaps(workspace_, sizeof(*workspace_))) return -EINVAL;
        auto *data = static_cast<uint8_t *>(storage->data);
        metadata_ = reinterpret_cast<MetadataRange *>(data + plan.metadata);
        metadata_capacity_ = Size(volume_.groups_) * 4;
        groups_ = reinterpret_cast<GroupLayout *>(data + plan.groups);
        tx_dirty_ = data + plan.dirty;
        mapping_bits_ = {data + plan.mapping, reinterpret_cast<uint32_t *>(data + plan.mapping_generations),
                         plan.mapping_bytes, (plan.mapping_bytes + 4095) / 4096,
                         volume_.blocks_ > volume_.inodes_ ? volume_.blocks_ : volume_.inodes_, 1};
        xattr_bits_ = {data + plan.xattrs, reinterpret_cast<uint32_t *>(data + plan.xattr_generations),
                       plan.xattr_bytes, (plan.xattr_bytes + 4095) / 4096, volume_.blocks_, 1};
        held_bits_ = {data + plan.holds, reinterpret_cast<uint32_t *>(data + plan.hold_generations),
                      plan.hold_bytes, (plan.hold_bytes + 4095) / 4096, volume_.inodes_, 1};
    }
    Bitmap *bitmaps[] = {&mapping_bits_, &xattr_bits_, &held_bits_};
    for (Bitmap *bitmap : bitmaps)
        for (Size page = 0; page < bitmap->pages; ++page) bitmap->generation[page] = 0;
    return 0;
}

bool WriteSession::bit_test(const Bitmap &bitmap, uint64_t bit)
{
    if (bit >= bitmap.bits) return false;
    Size byte = Size(bit / 8), page = byte / kBitmapPage;
    return bitmap.generation[page] == bitmap.epoch && (bitmap.data[byte] & (1U << (bit % 8)));
}

bool WriteSession::bit_set(Bitmap &bitmap, uint64_t bit, bool value)
{
    if (bit >= bitmap.bits) return false;
    Size byte = Size(bit / 8), page = byte / kBitmapPage;
    if (bitmap.generation[page] != bitmap.epoch)
    {
        Size first = page * kBitmapPage;
        Size end = bitmap.bytes - first < kBitmapPage ? bitmap.bytes : first + kBitmapPage;
        for (Size i = first; i < end; ++i) bitmap.data[i] = 0;
        bitmap.generation[page] = bitmap.epoch;
    }
    if (value) bitmap.data[byte] |= uint8_t(1U << (bit % 8));
    else bitmap.data[byte] &= uint8_t(~(1U << (bit % 8)));
    return true;
}

void WriteSession::bit_clear(Bitmap &bitmap)
{
    if (++bitmap.epoch) return;
    for (Size page = 0; page < bitmap.pages; ++page) bitmap.generation[page] = 0;
    bitmap.epoch = 1;
}

bool WriteSession::bit_next(const Bitmap &bitmap, uint64_t first, uint64_t &found)
{
    if (first >= bitmap.bits) return false;
    Size byte = Size(first / 8);
    while (byte < bitmap.bytes)
    {
        Size page = byte / kBitmapPage;
        if (bitmap.generation[page] != bitmap.epoch)
        {
            byte = (page + 1) * kBitmapPage;
            continue;
        }
        uint8_t value = bitmap.data[byte];
        if (byte == first / 8) value &= uint8_t(0xffU << (first % 8));
        if (value)
            for (unsigned bit = 0; bit < 8; ++bit)
                if (value & (1U << bit))
                {
                    found = uint64_t(byte) * 8 + bit;
                    return found < bitmap.bits;
                }
        ++byte;
    }
    return false;
}

int WriteSession::stop(int error)
{
    if (error == -EIO || error == -EUCLEAN || journal_.failed()) failed_ = true;
    return error;
}

bool WriteSession::home_read(void *context, uint64_t offset, void *output, Size bytes)
{
    return static_cast<WriteSession *>(context)->read_home(offset, output, bytes);
}

bool WriteSession::read_home(uint64_t offset, void *output, Size bytes)
{
    const uint32_t size = volume_.block_size_;
    if (volume_.ready_ && size && bytes <= size - offset % size)
    {
        uint64_t physical = offset / size;
        uint64_t first = uint64_t(volume_.first_data_block_) + 1;
        uint64_t count = (uint64_t(volume_.groups_) * volume_.descriptor_size_ + size - 1) / size;
        if (physical >= first && physical - first < count)
        {
            if (!gdt_cache_valid_ || gdt_cache_block_ != physical)
            {
                gdt_cache_valid_ = false;
                if (!device_.read(device_.context, physical * size, gdt_cache_, size)) return false;
                gdt_cache_block_ = physical;
                gdt_cache_valid_ = true;
            }
            copy(output, gdt_cache_ + offset % size, bytes);
            return true;
        }
    }
    return device_.read(device_.context, offset, output, bytes);
}

bool WriteSession::journal_read(void *context, uint64_t block, void *data, uint32_t bytes)
{
    auto *session = static_cast<WriteSession *>(context);
    return block < session->volume_.blocks_ && bytes == session->volume_.block_size_ &&
           session->device_.read(session->device_.context, block * bytes, data, bytes);
}

bool WriteSession::journal_write(void *context, uint64_t block, const void *data, uint32_t bytes)
{
    auto *session = static_cast<WriteSession *>(context);
    if (session->failed_ || block >= session->volume_.blocks_ || bytes != session->volume_.block_size_) return false;
    // Checkpoint/replay may replace the primary descriptor table. The cache
    // never survives a writer request, including a request that fails late.
    session->gdt_cache_valid_ = false;
    bool ok = session->device_.write(session->device_.context, block * bytes, data, bytes);
    if (!ok) session->failed_ = true;
    return ok;
}

bool WriteSession::journal_flush(void *context)
{
    auto *session = static_cast<WriteSession *>(context);
    if (session->failed_) return false;
    bool ok = session->device_.flush(session->device_.context);
    if (!ok) session->failed_ = true;
    return ok;
}

int WriteSession::collect_journal_metadata(void *context, uint64_t physical)
{
    auto *session = static_cast<WriteSession *>(context);
    for (uint32_t i = 0; i < session->journal_metadata_count_; ++i)
        if (session->journal_metadata_[i] == physical) return 0;
    if (session->verifying_journal_ || session->journal_metadata_count_ == 512) return -EOPNOTSUPP;
    int error = session->check_target_block(physical);
    if (error) return error;
    for (uint32_t i = 0; i < session->journal_blocks_; ++i)
        if (session->journal_data_[i] == physical) return -EUCLEAN;
    session->journal_metadata_[session->journal_metadata_count_++] = physical;
    return 0;
}

bool WriteSession::journal_map(void *context, uint32_t logical, uint64_t &physical)
{
    auto *session = static_cast<WriteSession *>(context);
    if (logical >= session->journal_inode_.size / session->volume_.block_size_ || logical >= 32768) return false;
    bool unwritten = false;
    int error = session->volume_.map_block(session->journal_inode_, logical, physical, unwritten,
                                           collect_journal_metadata, session);
    if (error || !physical || unwritten || session->check_target_block(physical)) return false;
    if (session->journal_inode_.xattr_block == physical) return false;
    if (session->verifying_journal_)
        return logical < session->journal_blocks_ && session->journal_data_[logical] == physical;
    // Journal::open calls mappings in logical order exactly once.
    if (logical != session->journal_blocks_) return false;
    session->journal_data_[session->journal_blocks_++] = physical;
    if (physical < session->journal_first_) session->journal_first_ = physical;
    if (physical > session->journal_last_) session->journal_last_ = physical;
    return true;
}

int WriteSession::check_writable_super()
{
    if ((le32(super_ + 0x5c) & ~0x3cU) ||
        (volume_.readonly_ & ~0x47bU) || !(le32(super_ + 0x5c) & 4U) ||
        le32(super_ + 0xe4) || le32(super_ + 0xe0) != 8) return -EOPNOTSUPP;
    if (volume_.blocks_per_group_ % 8 ||
        uint64_t(volume_.groups_ - 1) * volume_.inodes_per_group_ >= volume_.inodes_) return -EUCLEAN;
    uint32_t first = le32(super_ + 0x54);
    if (first < 11 || first > volume_.inodes_) return -EUCLEAN;
    if (first_inode_ && first_inode_ != first) return -EUCLEAN;
    first_inode_ = first;
    return 0;
}

int WriteSession::add_metadata(uint64_t first, uint64_t count, uint32_t inode_group)
{
    if (!count || first >= volume_.blocks_ || count > volume_.blocks_ - first) return -EUCLEAN;
    if (metadata_count_ == metadata_capacity_) return -EOPNOTSUPP;
    metadata_[metadata_count_++] = {first, count, inode_group};
    return 0;
}

int WriteSession::build_layout()
{
    metadata_count_ = 0;
    bitmap_group_ = 0xffffffffU;
    uint32_t size = volume_.block_size_;
    bool wide = (volume_.incompat_ & 0x80U) != 0;
    uint64_t table_blocks = (uint64_t(volume_.inodes_per_group_) * volume_.inode_size_ + size - 1) / size;
    uint64_t descriptors = (uint64_t(volume_.groups_) * volume_.descriptor_size_ + size - 1) / size;
    uint8_t descriptor[4096];
    for (uint32_t group = 0; group < volume_.groups_; ++group)
    {
        int error = volume_.read_group(group, descriptor);
        if (error) return error;
        GroupLayout &layout = groups_[group];
        layout.bitmap = descriptor_block(descriptor, 0, 0x20, wide);
        layout.inode_bitmap = descriptor_block(descriptor, 4, 0x24, wide);
        layout.inode_table = descriptor_block(descriptor, 8, 0x28, wide);
        layout.flags = le16(descriptor + 0x12);
        layout.free_blocks = le16(descriptor + 0xc) | (wide ? uint32_t(le16(descriptor + 0x2c)) << 16 : 0);
        layout.free_inodes = le16(descriptor + 0xe) | (wide ? uint32_t(le16(descriptor + 0x2e)) << 16 : 0);
        layout.bitmap_checksum = le16(descriptor + 0x18) |
                                 (wide ? uint32_t(le16(descriptor + 0x38)) << 16 : 0);
        error = add_metadata(layout.bitmap, 1);
        if (!error) error = add_metadata(layout.inode_bitmap, 1);
        if (!error) error = add_metadata(layout.inode_table, table_blocks, group);
        if (error) return error;
        bool backup = group == 0 || group == 1 || !(volume_.readonly_ & 1U) ||
                      is_power(group, 3) || is_power(group, 5) || is_power(group, 7);
        if (backup)
        {
            uint64_t first = volume_.first_data_block_ + uint64_t(group) * volume_.blocks_per_group_;
            error = add_metadata(first, 1 + descriptors + le16(super_ + 0xce));
            if (error) return error;
        }
    }
    // Heapsort followed by adjacent validation makes overlap checking O(G log G)
    // even for a volume with thousands of allocation groups.
    auto sift = [&](Size start, Size count)
    {
        while (start < count / 2)
        {
            Size child = start * 2 + 1;
            if (child + 1 < count && metadata_[child].first < metadata_[child + 1].first) ++child;
            if (metadata_[child].first <= metadata_[start].first) break;
            MetadataRange temporary = metadata_[start];
            metadata_[start] = metadata_[child];
            metadata_[child] = temporary;
            start = child;
        }
    };
    for (Size index = metadata_count_ / 2; index; --index) sift(index - 1, metadata_count_);
    for (Size count = metadata_count_; count > 1; --count)
    {
        MetadataRange temporary = metadata_[0];
        metadata_[0] = metadata_[count - 1];
        metadata_[count - 1] = temporary;
        sift(0, count - 1);
    }
    for (Size index = 1; index < metadata_count_; ++index)
        if (metadata_[index].first < metadata_[index - 1].first + metadata_[index - 1].count) return -EUCLEAN;
    return 0;
}

const WriteSession::MetadataRange *WriteSession::metadata_range(uint64_t block) const
{
    Size low = 0, high = metadata_count_;
    while (low < high)
    {
        Size middle = low + (high - low) / 2;
        if (metadata_[middle].first <= block) low = middle + 1;
        else high = middle;
    }
    if (!low) return nullptr;
    const MetadataRange *range = &metadata_[low - 1];
    return block - range->first < range->count ? range : nullptr;
}

int WriteSession::check_target_block(uint64_t block, uint32_t inode_group)
{
    if (!block || block < volume_.first_data_block_ || block >= volume_.blocks_) return -EUCLEAN;
    for (uint32_t i = 0; i < journal_metadata_count_; ++i)
        if (journal_metadata_[i] == block) return -EUCLEAN;
    const MetadataRange *range = metadata_range(block);
    bool inode_table = range && range->inode_group == inode_group && inode_group != 0xffffffffU;
    if ((range && !inode_table) || (inode_group != 0xffffffffU && !inode_table)) return -EUCLEAN;
    uint32_t group = uint32_t((block - volume_.first_data_block_) / volume_.blocks_per_group_);
    if (group >= volume_.groups_ || (groups_[group].flags & 2U)) return -EUCLEAN;
    if (bitmap_group_ != group)
    {
        int error = volume_.fetch(groups_[group].bitmap * volume_.block_size_, bitmap_cache_, volume_.block_size_);
        if (error) return error;
        if (!volume_.bootstrap_ && (volume_.readonly_ & 0x400U))
        {
            uint32_t actual = crc(volume_.checksum_seed_, bitmap_cache_, volume_.blocks_per_group_ / 8);
            if (volume_.descriptor_size_ == 32) actual &= 0xffffU;
            if (actual != groups_[group].bitmap_checksum) return -EUCLEAN;
        }
        bitmap_group_ = group;
    }
    uint64_t bit = (block - volume_.first_data_block_) % volume_.blocks_per_group_;
    return bitmap_cache_[bit / 8] & (1U << (bit % 8)) ? 0 : -EUCLEAN;
}

int WriteSession::check_inode_update(uint64_t block, uint32_t group, const void *after)
{
    int error = check_target_block(block, group);
    if (error) return error;
    uint8_t before[4096];
    uint32_t size = volume_.block_size_;
    error = volume_.fetch(block * size, before, size);
    if (error) return error;
    const auto *replacement = static_cast<const uint8_t *>(after);
    uint64_t first = uint64_t(group) * volume_.inodes_per_group_ +
                     (block - groups_[group].inode_table) * (size / volume_.inode_size_) + 1;
    for (uint32_t offset = 0; offset < size; offset += volume_.inode_size_)
    {
        uint64_t number = first + offset / volume_.inode_size_;
        bool changed = false;
        for (uint32_t i = 0; i < volume_.inode_size_; ++i)
            changed |= before[offset + i] != replacement[offset + i];
        if (!changed) continue;
        if (number < first_inode_ || number > volume_.inodes_) return -EUCLEAN;
        const uint8_t *old = before + offset;
        const uint8_t *image = replacement + offset;
        uint8_t raw[4096];
        uint64_t location = 0;
        // A failed checkpoint may have persisted the new timestamps but only
        // part of their checksum. Validate allocation and the complete logged
        // after-image without requiring the torn home checksum to be valid.
        error = volume_.read_raw_inode(uint32_t(number), raw, &location);
        if (error) return error;
        if (location != block * size + offset) return -EUCLEAN;
        for (uint32_t i = 0; i < volume_.inode_size_; ++i)
            if (raw[i] != old[i]) return -EUCLEAN;
        uint16_t extra_size = volume_.inode_size_ > 128 ? le16(old + 0x80) : 0;
        if (extra_size % 4 || uint32_t(extra_size) + 128 > volume_.inode_size_) return -EUCLEAN;
        Inode inode = {};
        inode.number = uint32_t(number);
        inode.generation = le32(old + 0x64);
        inode.mode = le16(old);
        inode.flags = le32(old + 0x20);
        if ((inode.mode & 0170000) != 0100000 || (inode.flags & ~(0x80000U | 0xc8U))) return -EOPNOTSUPP;
        if (le32(old + 0x14) || !le16(old + 0x1a)) return -ENOENT;
        bool extra = volume_.inode_size_ >= 160 && le16(old + 0x80) >= 16;
        bool high = volume_.inode_size_ >= 132 && le16(old + 0x80) >= 4;
        for (uint32_t i = 0; i < volume_.inode_size_; ++i)
        {
            bool allowed = (i >= 0xc && i < 0x14) || (extra && i >= 0x84 && i < 0x8c) ||
                           ((volume_.readonly_ & 0x400U) &&
                            ((i >= 0x7c && i < 0x7e) || (high && i >= 0x82 && i < 0x84)));
            if (!allowed && old[i] != image[i]) return -EUCLEAN;
        }
        if (le32(image + 0xc) > 0x7fffffffU || le32(image + 0x10) > 0x7fffffffU ||
            (extra && (le32(image + 0x84) || le32(image + 0x88)))) return -EOPNOTSUPP;
        if (volume_.readonly_ & 0x400U)
        {
            copy(raw, image, volume_.inode_size_);
            uint32_t expected = le16(raw + 0x7c);
            put16(raw + 0x7c, 0);
            if (high) { expected |= uint32_t(le16(raw + 0x82)) << 16; put16(raw + 0x82, 0); }
            uint32_t actual = crc(volume_.inode_seed(inode), raw, volume_.inode_size_);
            if (!high) actual &= 0xffffU;
            if (actual != expected) return -EUCLEAN;
        }
    }
    return 0;
}

bool WriteSession::validate_update(void *context, uint64_t block, const void *after)
{
    auto *session = static_cast<WriteSession *>(context);
    if (!after) return false;
    for (uint32_t i = 0; i < session->journal_blocks_; ++i)
    {
        if (session->journal_data_[i] == block) { session->stop(-EUCLEAN); return false; }
    }
    if (const MetadataRange *range = session->metadata_range(block))
    {
        int error = range->inode_group == 0xffffffffU ? -EUCLEAN :
                    session->check_inode_update(block, range->inode_group, after);
        if (error) session->stop(error);
        return error == 0;
    }
    int error = session->check_target_block(block);
    if (error) session->stop(error);
    return error == 0;
}

bool WriteSession::validate_transaction(void *context, const JournalTransaction &transaction)
{
    auto *session = static_cast<WriteSession *>(context);
    if ((!transaction.count && !session->initial_validator_) ||
        (transaction.count && (!transaction.read || !transaction.blocks)) ||
        transaction.count > JournalWorkspace::kMaximumBlocks ||
        transaction.block_size != session->volume_.block_size_) return false;
    session->validating_ = true;
    session->replay_view_ = &transaction;
    int error = session->validate_afterstate();
    if (!error && session->initial_validator_ && !session->initial_validated_)
    {
        Volume after = {};
        error = after.mount_internal(overlay_read, session, session->device_.bytes, true);
        if (!error) error = session->initial_validator_(session->initial_validator_context_, after);
        if (!error) session->initial_validated_ = true;
    }
    session->replay_view_ = nullptr;
    session->validating_ = false;
    session->bitmap_group_ = 0xffffffffU;
    if (error) session->stop(error);
    return error == 0;
}

unsigned WriteSession::tx_image_count() const
{
    return validating_ && replay_view_ ? replay_view_->count : tx_count_;
}

uint64_t WriteSession::tx_image_block(unsigned index) const
{
    return validating_ && replay_view_ ? replay_view_->blocks[index] : tx_blocks_[index];
}

bool WriteSession::tx_image_read(unsigned index, void *output) const
{
    if (index >= tx_image_count()) return false;
    if (validating_ && replay_view_) return replay_view_->read(replay_view_->context, index, output);
    copy(output, workspace_->blocks[index], volume_.block_size_);
    return true;
}

bool WriteSession::tx_contains(uint64_t block) const
{
    if (validating_ && replay_view_ && replay_view_->find)
    {
        unsigned index;
        return replay_view_->find(replay_view_->context, block, index);
    }
    for (unsigned i = 0; i < tx_image_count(); ++i) if (tx_image_block(i) == block) return true;
    return false;
}

int WriteSession::validate_afterstate()
{
    uint8_t super[1024];
    if (!overlay_read(this, 1024, super, sizeof(super))) return -EIO;
    // Allocation counters, write time, the orphan head and their checksum are mutable.
    // EXT_ATTR and LARGEDIR may only be enabled by a fully validated transaction;
    // layout, journal identity and all other feature/reserved bits stay fixed.
    for (unsigned i = 0; i < 1024; ++i)
    {
        bool mutable_byte = (i >= 0xc && i < 0x14) || (i >= 0x30 && i < 0x34) || i == 0x274 ||
                            (i >= 0xe8 && i < 0xec) ||
                            (i >= 0x158 && i < 0x15c) || i >= 0x3fc || i == 0x5c || i == 0x61;
        if (!mutable_byte && super[i] != super_[i]) return -EUCLEAN;
    }
    if (((le32(super + 0x5c) ^ le32(super_ + 0x5c)) & ~8U) ||
        (le32(super_ + 0x5c) & ~le32(super + 0x5c))) return -EUCLEAN;
    uint32_t previous_incompat = le32(super_ + 0x60), next_incompat = le32(super + 0x60);
    if (((previous_incompat ^ next_incompat) & ~0x4000U) || (previous_incompat & ~next_incompat))
        return -EUCLEAN;
    bool large_upgrade = !(previous_incompat & 0x4000U) && (next_incompat & 0x4000U);
    bool large_witness = false;
    Volume after = {};
    int error = after.mount_internal(overlay_read, this, device_.bytes, true);
    if (error) return error;
    error = validate_xattrs_afterstate(after);
    if (error) return error;
    error = validate_orphans_afterstate(after);
    if (error) return error;
    if (!active_)
    {
        error = validate_orphan_references(after);
        if (error) return error;
    }
    bit_clear(mapping_bits_);
    bitmap_group_ = 0xffffffffU;
    uint64_t free_blocks = 0, free_inodes = 0;
    uint32_t size = volume_.block_size_;
    bool wide = (volume_.incompat_ & 0x80U) != 0;
    uint8_t descriptor[4096], bitmap[4096], old[4096];
    for (uint32_t group = 0; group < volume_.groups_; ++group)
    {
        error = after.read_group(group, descriptor);
        if (error) return error;
        if (descriptor_block(descriptor, 0, 0x20, wide) != groups_[group].bitmap ||
            descriptor_block(descriptor, 4, 0x24, wide) != groups_[group].inode_bitmap ||
            descriptor_block(descriptor, 8, 0x28, wide) != groups_[group].inode_table ||
            ((le16(descriptor + 0x12) ^ groups_[group].flags) & ~3U) ||
            (le16(descriptor + 0x12) & ~groups_[group].flags & 3U)) return -EUCLEAN;
        uint64_t descriptor_offset = uint64_t(volume_.first_data_block_ + 1) * size +
                                     uint64_t(group) * volume_.descriptor_size_;
        if (!read_home(descriptor_offset, old, volume_.descriptor_size_)) return -EIO;
        for (unsigned i = 0; i < volume_.descriptor_size_; ++i)
        {
            bool mutable_byte = (i >= 0xc && i < 0x12) || (i >= 0x18 && i < 0x20) ||
                                (i >= 0x2c && i < 0x34) || (i >= 0x38 && i < 0x3c) || i == 0x12;
            if (!mutable_byte && descriptor[i] != old[i]) return -EUCLEAN;
        }
        uint32_t group_free_blocks = le16(descriptor + 0xc) |
                                    (wide ? uint32_t(le16(descriptor + 0x2c)) << 16 : 0);
        uint32_t group_free_inodes = le16(descriptor + 0xe) |
                                    (wide ? uint32_t(le16(descriptor + 0x2e)) << 16 : 0);
        if (group_free_blocks > volume_.blocks_per_group_ || group_free_inodes > volume_.inodes_per_group_)
            return -EUCLEAN;
        free_blocks += group_free_blocks;
        free_inodes += group_free_inodes;
        for (unsigned inode = 0; inode < 2; ++inode)
        {
            uint64_t physical = inode ? groups_[group].inode_bitmap : groups_[group].bitmap;
            bool changed = false;
            changed = tx_contains(physical);
            if (!changed) continue;
            if (le16(descriptor + 0x12) & (inode ? 1U : 2U)) return -EOPNOTSUPP;
            if (!overlay_read(this, physical * size, bitmap, size)) return -EIO;
            uint32_t bits = inode ? volume_.inodes_per_group_ : volume_.blocks_per_group_;
            uint64_t actual_bits = inode ? uint64_t(volume_.inodes_) - uint64_t(group) * bits :
                                   volume_.blocks_ - volume_.first_data_block_ - uint64_t(group) * bits;
            if (actual_bits > bits) actual_bits = bits;
            uint32_t available = 0;
            for (uint32_t bit = 0; bit < size * 8; ++bit)
            {
                bool set = (bitmap[bit / 8] & (1U << (bit % 8))) != 0;
                if (bit < actual_bits) available += !set;
                else if (!set) return -EUCLEAN;
            }
            if (available != (inode ? group_free_inodes : group_free_blocks)) return -EUCLEAN;
            if (volume_.readonly_ & 0x400U)
            {
                uint32_t expected = le16(descriptor + (inode ? 0x1a : 0x18));
                if (wide) expected |= uint32_t(le16(descriptor + (inode ? 0x3a : 0x38))) << 16;
                uint32_t calculated = crc(volume_.checksum_seed_, bitmap, bits / 8);
                if (volume_.descriptor_size_ == 32) calculated &= 0xffffU;
                if (calculated != expected) return -EUCLEAN;
            }
        }
    }
    if (free_blocks != after.free_blocks_ || free_inodes != after.free_inodes_) return -EUCLEAN;
    // Every journal and journal-index block stays allocated, irrespective of
    // which allocation group happens to be modified by the transaction.
    uint32_t cached_group = 0xffffffffU;
    auto allocated = [&](uint64_t physical) -> bool
    {
        if (physical < volume_.first_data_block_ || physical >= volume_.blocks_) return false;
        uint32_t group = uint32_t((physical - volume_.first_data_block_) / volume_.blocks_per_group_);
        if (group != cached_group)
        {
            if (!overlay_read(this, groups_[group].bitmap * size, bitmap, size)) return false;
            cached_group = group;
        }
        uint32_t bit = uint32_t((physical - volume_.first_data_block_) % volume_.blocks_per_group_);
        return (bitmap[bit / 8] & (1U << (bit % 8))) != 0;
    };
    for (uint32_t i = 0; i < journal_blocks_; ++i) if (!allocated(journal_data_[i])) return -EUCLEAN;
    for (uint32_t i = 0; i < journal_metadata_count_; ++i) if (!allocated(journal_metadata_[i])) return -EUCLEAN;
    uint64_t gdt_first = volume_.first_data_block_ + 1;
    uint64_t gdt_count = (uint64_t(volume_.groups_) * volume_.descriptor_size_ + size - 1) / size;
    bool has_super = false, has_descriptor = false, has_bitmap = false;
    for (unsigned i = 0; i < tx_image_count(); ++i)
    {
        uint64_t target = tx_image_block(i);
        has_super |= target == 1024 / size;
        has_descriptor |= target >= gdt_first && target - gdt_first < gdt_count;
        for (uint32_t group = 0; group < volume_.groups_; ++group)
            has_bitmap |= target == groups_[group].bitmap || target == groups_[group].inode_bitmap;
    }
    bool allocation_transaction = has_super && has_descriptor && has_bitmap;
    for (unsigned update = 0; update < tx_image_count(); ++update)
    {
        uint64_t physical = tx_image_block(update);
        uint8_t image_bytes[4096];
        if (!tx_image_read(update, image_bytes)) return -EIO;
        const uint8_t *image = image_bytes;
        if (!device_.read(device_.context, physical * size, old, size)) return -EIO;
        if (physical == 1024 / size)
        {
            for (uint32_t i = 0; i < size; ++i)
                if ((i < 1024 % size || i >= 1024 % size + 1024) && image[i] != old[i]) return -EUCLEAN;
            continue;
        }
        if (physical >= gdt_first && physical - gdt_first < gdt_count) continue;
        bool metadata = false;
        for (uint32_t group = 0; group < volume_.groups_; ++group)
        {
            if (physical == groups_[group].bitmap || physical == groups_[group].inode_bitmap)
            {
                metadata = true;
                break;
            }
            uint64_t table_count = (uint64_t(volume_.inodes_per_group_) * volume_.inode_size_ + size - 1) / size;
            if (physical < groups_[group].inode_table || physical - groups_[group].inode_table >= table_count)
                continue;
            metadata = true;
            uint64_t first_number = uint64_t(group) * volume_.inodes_per_group_ +
                                    (physical - groups_[group].inode_table) * (size / volume_.inode_size_) + 1;
            for (uint32_t offset = 0; offset < size; offset += volume_.inode_size_)
            {
                bool changed = false;
                for (uint32_t i = 0; i < volume_.inode_size_; ++i) changed |= image[offset + i] != old[offset + i];
                uint32_t number = uint32_t(first_number + offset / volume_.inode_size_);
                if ((number < first_inode_ && number != 2) || number > volume_.inodes_)
                {
                    if (changed) return -EUCLEAN;
                    continue;
                }
                const uint8_t *raw = image + offset;
                if (changed && !allocation_transaction)
                    for (unsigned i = 0; i < 60; ++i)
                        if (raw[0x28 + i] != old[offset + 0x28 + i]) return -EUCLEAN;
                uint32_t bit = (number - 1) % volume_.inodes_per_group_;
                uint8_t inode_bitmap[4096];
                if (!overlay_read(this, groups_[group].inode_bitmap * size, inode_bitmap, size)) return -EIO;
                if (!(inode_bitmap[bit / 8] & (1U << (bit % 8))))
                {
                    if (!changed) continue;
                    if (le16(raw + 0x1a) || !le32(raw + 0x14) || le32(raw + 4) || le32(raw + 0x6c) ||
                        le32(raw + 0x1c) || le16(raw + 0x74) || le32(raw + 0x68) || le16(raw + 0x76)) return -EUCLEAN;
                    continue;
                }
                Inode inode;
                error = after.read_inode(number, inode);
                if (error) return error;
                uint16_t type = inode.mode & 0170000;
                if (type != 0100000 && type != 0040000 && type != 0120000) return -EOPNOTSUPP;
                if (inode.flags & ~(0x80000U | 0x10000U | 0x40000U | 0x1000U | 0xc8U)) return -EOPNOTSUPP;
                if (type == 0120000 && inode.links &&
                    !(allocation_transaction && tx_contains(groups_[group].inode_bitmap)))
                {
                    // Existing symlink payload/type/mapping stay fixed while
                    // ordinary metadata and independently validated EA change.
                    if ((le16(old + offset) & 0170000) != type) return -EUCLEAN;
                    for (uint32_t i = 0x28; i < 0x64; ++i)
                        if (raw[i] != old[offset + i]) return -EUCLEAN;
                    if (le32(raw + 4) != le32(old + offset + 4) ||
                        le32(raw + 0x6c) != le32(old + offset + 0x6c) ||
                        le32(raw + 0x20) != le32(old + offset + 0x20) ||
                        le32(raw + 0x64) != le32(old + offset + 0x64)) return -EUCLEAN;
                }
                if (inode.xattr_block && (!allocated(inode.xattr_block) || protected_block(inode.xattr_block)))
                    return -EUCLEAN;
                uint64_t mapped_blocks = 0;
                error = validate_inode_mapping(after, inode, mapped_blocks);
                if (error) return error;
                if ((mapped_blocks + (inode.xattr_block ? 1 : 0)) * (size / 512) != inode.blocks_512) return -EUCLEAN;
                if (type == 0040000 && (inode.links || inode.size))
                {
                    if (inode.flags & 0x1000U)
                    {
                        if (!(le32(super + 0x5c) & 0x20U)) return -EUCLEAN;
                        struct IndexRead
                        {
                            Volume *volume;
                            const Inode *inode;
                        } context = {&after, &inode};
                        DirectoryIndexIO io = {};
                        io.context = &context;
                        io.read = [](void *opaque, uint32_t logical, uint8_t *bytes) -> int
                        {
                            auto *context = static_cast<IndexRead *>(opaque);
                            Size completed;
                            uint32_t block_size = context->volume->block_size();
                            int error = context->volume->read(*context->inode, uint64_t(logical) * block_size,
                                                               bytes, block_size, completed);
                            return error ? error : (completed == block_size ? 0 : -EUCLEAN);
                        };
                        io.block_size = size;
                        io.block_count = uint32_t(inode.size / size);
                        io.inode = inode.number;
                        io.inode_count = after.inodes_;
                        io.inode_seed = after.inode_seed(inode);
                        for (unsigned i = 0; i < 4; ++i) io.hash_seed[i] = le32(super + 0xec + i * 4);
                        io.default_hash_version = super[0xfc];
                        io.unsigned_hash = (le32(super + 0x160) & 2U) != 0;
                        io.metadata_checksum = (after.readonly_ & 0x400U) != 0;
                        io.file_type = (after.incompat_ & 2U) != 0;
                        io.large_directory = (after.incompat_ & 0x4000U) != 0;
                        error = IndexedDirectory(io, workspace_->directory).validate();
                        if (error) return error;
                        if (large_upgrade)
                        {
                            // A promotion always changes the directory inode
                            // and root block in this transaction. Recovery may
                            // already have the new root in a partial checkpoint,
                            // so use only its complete validated final image.
                            uint8_t root[4096];
                            error = io.read(io.context, 0, root);
                            if (error) return error;
                            if (root[30] == 2)
                            {
                                if (active_)
                                {
                                    Inode before;
                                    error = volume_.read_inode(inode.number, before);
                                    if (error) return error;
                                    if ((before.mode & 0170000) != 0040000 || !(before.flags & 0x1000U))
                                        return -EUCLEAN;
                                    Size completed = 0;
                                    error = volume_.read(before, 0, root, size, completed);
                                    if (error) return error;
                                    if (completed != size || root[30] != 1) return -EUCLEAN;
                                    uint64_t physical = 0;
                                    bool unwritten = false;
                                    error = after.map_block(inode, 0, physical, unwritten);
                                    if (error) return error;
                                    if (unwritten || !physical || !tx_contains(physical)) return -EUCLEAN;
                                }
                                large_witness = true;
                            }
                        }
                    }
                    struct DirectoryCheck
                    {
                        Volume *volume;
                        bool unlinked;
                    } check = {&after, !inode.links};
                    error = after.list_directory(inode,
                        [](void *context, const char *name, uint32_t child, uint8_t) -> int
                        {
                            auto *check = static_cast<DirectoryCheck *>(context);
                            Inode target;
                            int result = check->volume->read_inode(child, target);
                            if (result) return result;
                            bool dot = name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]));
                            return !target.links && !(check->unlinked && dot) ? -EUCLEAN : 0;
                        }, &check);
                    if (error) return error;
                }
            }
            break;
        }
        if (!metadata && (protected_block(physical) || !allocated(physical))) return -EUCLEAN;
    }
    // Every nonstructural afterimage needs an inode owner in the same final
    // view. Merely being allocated never authorizes a journal target.
    for (unsigned update = 0; update < tx_image_count(); ++update)
    {
        uint64_t physical = tx_image_block(update);
        bool structural = physical == 1024 / size ||
                          (physical >= gdt_first && physical - gdt_first < gdt_count);
        structural |= metadata_range(physical) != nullptr;
        if (!structural && !tx_is_xattr_block(physical) &&
            !bit_test(mapping_bits_, physical)) return -EUCLEAN;
    }
    if (large_upgrade && !large_witness) return -EUCLEAN;
    return 0;
}

int WriteSession::verify_recovered_layout()
{
    uint8_t recovered[1024];
    if (!device_.read(device_.context, 1024, recovered, sizeof(recovered))) return -EIO;
    for (Size i = 0; i < sizeof(recovered); ++i)
    {
        bool mutable_byte = (i >= 0xc && i < 0x14) || (i >= 0x30 && i < 0x34) || i == 0x274 ||
                            (i >= 0xe8 && i < 0xec) ||
                            (i >= 0x158 && i < 0x15c) || i >= 0x3fc || i == 0x5c || i == 0x61;
        if (!mutable_byte && recovered[i] != super_[i]) return -EUCLEAN;
    }
    if (((le32(recovered + 0x5c) ^ le32(super_ + 0x5c)) & ~8U) ||
        (le32(super_ + 0x5c) & ~le32(recovered + 0x5c))) return -EUCLEAN;
    // Journal commit/replay checked the complete index transition before any
    // write. Adopt its one-way capability upgrade into the mounted view.
    if (((le32(recovered + 0x60) ^ le32(super_ + 0x60)) & ~0x4000U) ||
        (le32(super_ + 0x60) & ~le32(recovered + 0x60))) return -EUCLEAN;
    copy(super_, recovered, sizeof(super_));
    int error = volume_.mount_internal(home_read, this, device_.bytes, true);
    if (error) return error;
    error = check_writable_super();
    if (error) return error;
    Inode journal;
    error = volume_.read_inode(8, journal);
    if (error) return error;
    if (journal.size != journal_inode_.size || journal.generation != journal_inode_.generation ||
        journal.flags != journal_inode_.flags || journal.mode != journal_inode_.mode ||
        journal.xattr_block != journal_inode_.xattr_block) return -EUCLEAN;
    for (Size i = 0; i < sizeof(journal.block_map); ++i)
        if (journal.block_map[i] != journal_inode_.block_map[i]) return -EUCLEAN;
    error = build_layout();
    if (error) return error;
    bitmap_group_ = 0xffffffffU;
    verifying_journal_ = true;
    for (uint32_t logical = 0; logical < journal_blocks_; ++logical)
    {
        uint64_t physical;
        if (!journal_map(this, logical, physical)) { verifying_journal_ = false; return -EUCLEAN; }
    }
    verifying_journal_ = false;
    return 0;
}

int WriteSession::set_recovery(bool enabled)
{
    if (failed_) return -EIO;
    uint32_t features = le32(super_ + 0x60);
    put32(super_ + 0x60, enabled ? features | 4U : features & ~4U);
    put16(super_ + 0x3a, 1); // VALID; RECOVER records the active journaled session.
    if (volume_.readonly_ & 0x400U) put32(super_ + 0x3fc, crc(0xffffffffU, super_, 0x3fc));
    uint32_t size = volume_.block_size_;
    uint64_t offset = 1024 / size * uint64_t(size);
    uint8_t *block = workspace_->blocks[0];
    if (!device_.read(device_.context, offset, block, size)) return stop(-EIO);
    copy(block + 1024 % size, super_, sizeof(super_));
    if (!device_.write(device_.context, offset, block, size) || !device_.flush(device_.context)) return stop(-EIO);
    volume_.incompat_ = enabled ? features | 4U : features & ~4U;
    return 0;
}

int WriteSession::open(const WriteDevice &device, WriteWorkspace &workspace,
                       InitialValidator validator, void *validator_context, const WriteStorage *storage)
{
    // Open on a freshly zero-initialized session, or after successful close.
    // Reopening an active/failed object would silently abandon a dirty mount.
    if (active_ || failed_) return -EBUSY;
    if (!device.read || !device.write || !device.flush) return -EINVAL;
    device_ = device;
    workspace_ = &workspace;
    gdt_cache_valid_ = false;
    initial_validator_ = validator;
    initial_validator_context_ = validator_context;
    initial_validated_ = false;
    tx_count_ = 0;
    tx_active_ = validating_ = false;
    replay_view_ = nullptr;
    held_orphan_count_ = 0;

    if (!device.read(device.context, 1024, super_, sizeof(super_))) return stop(-EIO);
    bool bootstrap = (le32(super_ + 0x60) & 4U) != 0;
    int error = volume_.mount_internal(home_read, this, device.bytes, true, bootstrap);
    if (error) return stop(error);
    first_inode_ = 0;
    metadata_count_ = journal_blocks_ = journal_metadata_count_ = 0;
    journal_first_ = ~uint64_t(0);
    journal_last_ = 0;
    bitmap_group_ = 0xffffffffU;
    verifying_journal_ = false;
    int layout_error = check_writable_super();
    if (layout_error) return stop(layout_error);
    layout_error = configure_storage(storage);
    if (layout_error) return layout_error;
    layout_error = build_layout();
    if (layout_error) return stop(layout_error);
    uint32_t journal_number = le32(super_ + 0xe0);
    error = volume_.read_inode(journal_number, journal_inode_);
    if (error) return stop(error);
    if ((journal_inode_.mode & 0170000) != 0100000 ||
        journal_inode_.size % volume_.block_size_ ||
        journal_inode_.size / volume_.block_size_ > 32768 || journal_inode_.xattr_block) return stop(-EUCLEAN);
    JournalDevice journal_device = {};
    journal_device.context = this;
    journal_device.workspace = &workspace.journal;
    journal_device.read = journal_read;
    journal_device.write = journal_write;
    journal_device.flush = journal_flush;
    journal_device.map = journal_map;
    journal_device.validate = nullptr;
    journal_device.validate_transaction = validate_transaction;
    journal_device.filesystem_blocks = volume_.blocks_;
    journal_device.block_size = volume_.block_size_;
    journal_device.mapped_blocks = uint32_t(journal_inode_.size / volume_.block_size_);
    journal_device.journal_bytes = journal_inode_.size;
    copy(journal_device.uuid, volume_.uuid_, 16);
    error = journal_.open(journal_device);
    if (error) return stop(error);
    bool recovery = (le32(super_ + 0x60) & 4U) != 0;
    if (!recovery && le32(super_ + 0xe8)) return stop(-EUCLEAN);
    if (!recovery && journal_.has_pending()) return stop(-EUCLEAN);
    if (recovery)
    {
        bool replayed = false;
        error = journal_.recover(replayed, initial_validator_ != nullptr);
        if (error) return stop(error);
        error = verify_recovered_layout();
        if (error) return stop(error);
    }
    error = validate_orphans_afterstate(volume_);
    if (error) return stop(error);
    error = validate_orphan_references(volume_);
    if (error) return stop(error);
    if (initial_validator_ && !initial_validated_)
    {
        error = initial_validator_(initial_validator_context_, volume_);
        if (error) return stop(error);
        initial_validated_ = true;
    }
    initial_validator_ = nullptr;
    initial_validator_context_ = nullptr;
    // Reject backends without durable flush before marking a clean volume
    // active. Unsupported recovery records above still cause no I/O writes.
    if (!device_.flush(device_.context)) return stop(-EIO);
    error = set_recovery(true);
    if (error) return error;
    active_ = true;
    error = recover_orphans();
    return error ? stop(error) : 0;
}

bool WriteSession::protected_block(uint64_t block) const
{
    if (!block || block < volume_.first_data_block_ || block >= volume_.blocks_) return true;
    if (metadata_range(block)) return true;
    if (block >= journal_first_ && block <= journal_last_)
        for (uint32_t i = 0; i < journal_blocks_; ++i) if (journal_data_[i] == block) return true;
    for (uint32_t i = 0; i < journal_metadata_count_; ++i) if (journal_metadata_[i] == block) return true;
    return false;
}

bool WriteSession::overlay_read(void *context, uint64_t offset, void *output, Size bytes)
{
    auto *session = static_cast<WriteSession *>(context);
    if (offset > session->device_.bytes || bytes > session->device_.bytes - offset) return false;
    uint32_t size = session->volume_.block_size_;
    auto *destination = static_cast<uint8_t *>(output);
    uint8_t image[4096];
    while (bytes)
    {
        uint64_t physical = offset / size;
        Size part = size - offset % size;
        if (part > bytes) part = bytes;
        unsigned index = 0;
        bool found = false;
        if (session->validating_ && session->replay_view_ && session->replay_view_->find)
            found = session->replay_view_->find(session->replay_view_->context, physical, index);
        else
            for (; index < session->tx_image_count(); ++index)
                if (session->tx_image_block(index) == physical) { found = true; break; }
        if (found)
        {
            if (!session->tx_image_read(index, image)) return false;
            copy(destination, image + offset % size, part);
        }
        else if (!session->read_home(offset, destination, part)) return false;
        destination += part; offset += part; bytes -= part;
    }
    return true;
}

int WriteSession::tx_begin()
{
    if (!active_) return -EINVAL;
    if (failed_ || journal_.failed()) return stop(-EIO);
    if (tx_active_) return -EBUSY;
    tx_count_ = 0;
    for (uint32_t i = 0; i < volume_.groups_; ++i) tx_dirty_[i] = 0;
    tx_active_ = true;
    return 0;
}

void WriteSession::tx_cancel()
{
    tx_count_ = 0;
    tx_active_ = false;
    bitmap_group_ = 0xffffffffU;
}

int WriteSession::tx_read(uint64_t block, uint8_t *output)
{
    if (block >= volume_.blocks_) return -EUCLEAN;
    return overlay_read(this, block * volume_.block_size_, output, volume_.block_size_) ? 0 : stop(-EIO);
}

void WriteSession::tx_forget(uint64_t block)
{
    for (unsigned i = 0; i < tx_count_; ++i)
        if (tx_blocks_[i] == block) tx_blocks_[i] = ~uint64_t(0);
}

int WriteSession::tx_edit(uint64_t block, uint8_t *&data, bool zero)
{
    if (!tx_active_ || block >= volume_.blocks_) return -EINVAL;
    unsigned index = 0;
    while (index < tx_count_ && tx_blocks_[index] != block) ++index;
    if (index == tx_count_)
    {
        for (index = 0; index < tx_count_ && tx_blocks_[index] != ~uint64_t(0); ++index) {}
        if (index == 16) return -E2BIG;
        data = workspace_->blocks[index];
        if (!zero && !device_.read(device_.context, block * volume_.block_size_, data, volume_.block_size_))
            return stop(-EIO);
        tx_blocks_[index] = block;
        if (index == tx_count_) ++tx_count_;
    }
    else data = workspace_->blocks[index];
    if (zero) for (uint32_t i = 0; i < volume_.block_size_; ++i) data[i] = 0;
    return 0;
}

int WriteSession::tx_inode(uint32_t number, uint8_t *&raw)
{
    if ((number < first_inode_ && number != 2) || number > volume_.inodes_) return -EINVAL;
    Inode checked;
    int error = volume_.read_inode(number, checked);
    if (error) return stop(error);
    uint32_t group = (number - 1) / volume_.inodes_per_group_;
    uint64_t location = groups_[group].inode_table * volume_.block_size_ +
                        uint64_t((number - 1) % volume_.inodes_per_group_) * volume_.inode_size_;
    uint8_t *block;
    error = tx_edit(location / volume_.block_size_, block);
    if (!error) raw = block + location % volume_.block_size_;
    return error;
}

int WriteSession::tx_descriptor(uint32_t group, uint8_t *&descriptor)
{
    if (group >= volume_.groups_) return -EINVAL;
    uint64_t position = uint64_t(volume_.first_data_block_ + 1) * volume_.block_size_ +
                        uint64_t(group) * volume_.descriptor_size_;
    uint8_t *block;
    int error = tx_edit(position / volume_.block_size_, block);
    if (!error) descriptor = block + position % volume_.block_size_;
    return error;
}

int WriteSession::tx_bitmap(uint32_t group, bool inode, uint8_t *&bitmap, uint8_t *&descriptor)
{
    int error = tx_descriptor(group, descriptor);
    if (error) return error;
    uint16_t flags = le16(descriptor + 0x12);
    bool uninitialized = (flags & (inode ? 1U : 2U)) != 0;
    error = tx_edit(inode ? groups_[group].inode_bitmap : groups_[group].bitmap, bitmap);
    if (error) return error;
    unsigned dirty = inode ? 2 : 1;
    if (uninitialized)
    {
        for (uint32_t i = 0; i < volume_.block_size_; ++i) bitmap[i] = 0;
        uint64_t first = inode ? uint64_t(group) * volume_.inodes_per_group_ :
                         volume_.first_data_block_ + uint64_t(group) * volume_.blocks_per_group_;
        uint64_t count = inode ? uint64_t(volume_.inodes_) - first : volume_.blocks_ - first;
        uint32_t maximum = inode ? volume_.inodes_per_group_ : volume_.blocks_per_group_;
        if (count > maximum) count = maximum;
        auto reserve = [&](uint64_t block)
        {
            if (block >= first && block - first < count)
            {
                uint32_t bit = uint32_t(block - first);
                bitmap[bit / 8] |= uint8_t(1U << (bit % 8));
            }
        };
        if (inode)
        {
            for (uint64_t bit = 0; bit < count && first + bit + 1 < first_inode_; ++bit)
                bitmap[bit / 8] |= uint8_t(1U << (bit % 8));
        }
        else
        {
            reserve(0);
            for (uint32_t range = 0; range < metadata_count_; ++range)
            {
                uint64_t begin = metadata_[range].first > first ? metadata_[range].first : first;
                uint64_t end = metadata_[range].first + metadata_[range].count;
                if (end > first + count) end = first + count;
                for (uint64_t block = begin; block < end; ++block) reserve(block);
            }
            for (uint32_t i = 0; i < journal_blocks_; ++i) reserve(journal_data_[i]);
            for (uint32_t i = 0; i < journal_metadata_count_; ++i) reserve(journal_metadata_[i]);
        }
        for (uint32_t bit = uint32_t(count); bit < volume_.block_size_ * 8; ++bit)
            bitmap[bit / 8] |= uint8_t(1U << (bit % 8));
        put16(descriptor + 0x12, uint16_t(flags & ~(inode ? 1U : 2U)));
    }
    if (!uninitialized && !(tx_dirty_[group] & dirty) && (volume_.readonly_ & 0x400U))
    {
        unsigned low = inode ? 0x1a : 0x18;
        unsigned high = inode ? 0x3a : 0x38;
        uint32_t expected = le16(descriptor + low);
        if (volume_.descriptor_size_ >= 64) expected |= uint32_t(le16(descriptor + high)) << 16;
        uint32_t actual = crc(volume_.checksum_seed_, bitmap,
                              (inode ? volume_.inodes_per_group_ : volume_.blocks_per_group_) / 8);
        if (volume_.descriptor_size_ == 32) actual &= 0xffffU;
        if (actual != expected) return stop(-EUCLEAN);
    }
    tx_dirty_[group] |= uint8_t(dirty);
    return 0;
}

int WriteSession::tx_counts(uint32_t group, int blocks, int inodes, int directories)
{
    uint8_t *descriptor, *super_block;
    int error = tx_descriptor(group, descriptor);
    if (!error) error = tx_edit(1024 / volume_.block_size_, super_block);
    if (error) return error;
    uint8_t *super = super_block + 1024 % volume_.block_size_;
    const unsigned low_fields[] = {0xc, 0xe, 0x10};
    const unsigned high_fields[] = {0x2c, 0x2e, 0x30};
    const int deltas[] = {blocks, inodes, directories};
    for (unsigned i = 0; i < 3; ++i)
    {
        uint64_t value = le16(descriptor + low_fields[i]);
        if (volume_.descriptor_size_ >= 64) value |= uint64_t(le16(descriptor + high_fields[i])) << 16;
        int64_t next = int64_t(value) + deltas[i];
        if (next < 0 || uint64_t(next) > (i ? volume_.inodes_per_group_ : volume_.blocks_per_group_))
            return stop(-EUCLEAN);
        put16(descriptor + low_fields[i], uint16_t(next));
        if (volume_.descriptor_size_ >= 64) put16(descriptor + high_fields[i], uint16_t(uint64_t(next) >> 16));
    }
    uint64_t free_blocks = le32(super + 0xc) |
                          ((volume_.incompat_ & 0x80U) ? uint64_t(le32(super + 0x158)) << 32 : 0);
    int64_t next_blocks = int64_t(free_blocks) + blocks;
    int64_t next_inodes = int64_t(le32(super + 0x10)) + inodes;
    if (next_blocks < 0 || uint64_t(next_blocks) > volume_.blocks_ ||
        next_inodes < 0 || uint64_t(next_inodes) > volume_.inodes_) return stop(-EUCLEAN);
    put32(super + 0xc, uint32_t(next_blocks));
    if (volume_.incompat_ & 0x80U) put32(super + 0x158, uint32_t(uint64_t(next_blocks) >> 32));
    put32(super + 0x10, uint32_t(next_inodes));
    return 0;
}

void WriteSession::tx_seal_group(uint32_t group, uint8_t *descriptor)
{
    uint8_t encoded[4];
    put32(encoded, group);
    put16(descriptor + 0x1e, 0);
    if (volume_.readonly_ & 0x400U)
    {
        uint32_t checksum = crc(volume_.checksum_seed_, encoded, 4);
        checksum = crc(checksum, descriptor, volume_.descriptor_size_);
        put16(descriptor + 0x1e, uint16_t(checksum));
    }
    else if (volume_.readonly_ & 0x10U)
    {
        uint16_t checksum = 0xffff;
        auto feed = [&checksum](const uint8_t *data, Size bytes)
        {
            for (Size i = 0; i < bytes; ++i)
            {
                checksum ^= data[i];
                for (unsigned bit = 0; bit < 8; ++bit)
                    checksum = uint16_t((checksum >> 1) ^ ((checksum & 1) ? 0xa001U : 0));
            }
        };
        feed(volume_.uuid_, 16);
        feed(encoded, 4);
        feed(descriptor, 0x1e);
        if (volume_.descriptor_size_ > 32) feed(descriptor + 32, volume_.descriptor_size_ - 32);
        put16(descriptor + 0x1e, checksum);
    }
}

int WriteSession::tx_allocate_block(uint64_t &block)
{
    for (uint32_t group = 0; group < volume_.groups_; ++group)
    {
        if (!groups_[group].free_blocks && !(tx_dirty_[group] & 1U)) continue;
        uint8_t *bitmap, *descriptor;
        int error = tx_bitmap(group, false, bitmap, descriptor);
        if (error) return error;
        uint64_t first = volume_.first_data_block_ + uint64_t(group) * volume_.blocks_per_group_;
        uint64_t count = volume_.blocks_ - first;
        if (count > volume_.blocks_per_group_) count = volume_.blocks_per_group_;
        for (uint32_t index = 0; index < count; ++index)
        {
            if (bitmap[index / 8] & (1U << (index % 8))) continue;
            block = first + index;
            if (protected_block(block)) return stop(-EUCLEAN);
            bitmap[index / 8] |= uint8_t(1U << (index % 8));
            error = tx_counts(group, -1, 0, 0);
            uint8_t *data;
            if (!error) error = tx_edit(block, data, true);
            return error;
        }
    }
    return -ENOSPC;
}

int WriteSession::tx_release_block(uint64_t block)
{
    if (protected_block(block)) return stop(-EUCLEAN);
    uint32_t group = uint32_t((block - volume_.first_data_block_) / volume_.blocks_per_group_);
    uint32_t index = uint32_t((block - volume_.first_data_block_) % volume_.blocks_per_group_);
    uint8_t *bitmap, *descriptor;
    int error = tx_bitmap(group, false, bitmap, descriptor);
    if (error) return error;
    if (!(bitmap[index / 8] & (1U << (index % 8)))) return stop(-EUCLEAN);
    bitmap[index / 8] &= uint8_t(~(1U << (index % 8)));
    return tx_counts(group, 1, 0, 0);
}

int WriteSession::tx_allocate_inode(uint16_t mode, uint32_t uid, uint32_t gid, uint64_t seconds,
                                    uint32_t &number, uint8_t *&raw)
{
    if (seconds > 0x37fffffffULL || ((mode & 0170000) != 0100000 && (mode & 0170000) != 0040000 &&
                                    (mode & 0170000) != 0120000))
        return -EINVAL;
    if (volume_.inode_size_ < 160 && seconds > 0x7fffffffULL) return -EOVERFLOW;
    for (uint32_t group = 0; group < volume_.groups_; ++group)
    {
        if (!groups_[group].free_inodes && !(tx_dirty_[group] & 2U)) continue;
        uint8_t *bitmap, *descriptor;
        int error = tx_bitmap(group, true, bitmap, descriptor);
        if (error) return error;
        uint64_t first_number = uint64_t(group) * volume_.inodes_per_group_;
        uint32_t count = first_number >= volume_.inodes_ ? 0 :
                         uint32_t(volume_.inodes_ - first_number);
        if (count > volume_.inodes_per_group_) count = volume_.inodes_per_group_;
        for (uint32_t index = 0; index < count; ++index)
        {
            number = uint32_t(first_number + index + 1);
            if (number < first_inode_ || (bitmap[index / 8] & (1U << (index % 8)))) continue;
            uint64_t offset = groups_[group].inode_table * volume_.block_size_ + uint64_t(index) * volume_.inode_size_;
            uint8_t *block;
            error = tx_edit(offset / volume_.block_size_, block);
            if (error) return error;
            raw = block + offset % volume_.block_size_;
            uint32_t generation = le32(raw + 0x64) + 1;
            for (uint32_t i = 0; i < volume_.inode_size_; ++i) raw[i] = 0;
            put16(raw, mode);
            put16(raw + 2, uint16_t(uid));
            put16(raw + 0x78, uint16_t(uid >> 16));
            put16(raw + 0x18, uint16_t(gid));
            put16(raw + 0x7a, uint16_t(gid >> 16));
            put16(raw + 0x1a, (mode & 0170000) == 0040000 ? 2 : 1);
            put32(raw + 8, uint32_t(seconds));
            put32(raw + 0x64, generation);
            if (volume_.inode_size_ >= 160)
            {
                put16(raw + 0x80, 32);
                put32(raw + 0x90, uint32_t(seconds));
                uint32_t epoch = uint32_t((int64_t(seconds) - int64_t(int32_t(seconds))) >> 32);
                put32(raw + 0x8c, epoch);
                put32(raw + 0x94, epoch);
            }
            if (volume_.incompat_ & 0x40U)
            {
                put32(raw + 0x20, 0x80000U);
                put16(raw + 0x28, 0xf30a);
                put16(raw + 0x2c, 4);
            }
            bitmap[index / 8] |= uint8_t(1U << (index % 8));
            uint32_t unused = le16(descriptor + 0x1c);
            if (volume_.descriptor_size_ >= 64) unused |= uint32_t(le16(descriptor + 0x32)) << 16;
            uint32_t remaining = volume_.inodes_per_group_ - index - 1;
            if (remaining < unused)
            {
                put16(descriptor + 0x1c, uint16_t(remaining));
                if (volume_.descriptor_size_ >= 64) put16(descriptor + 0x32, uint16_t(remaining >> 16));
            }
            error = tx_counts(group, 0, -1, (mode & 0170000) == 0040000 ? 1 : 0);
            if (!error) error = tx_finish_inode(number, raw, seconds);
            return error;
        }
    }
    return -ENOSPC;
}

int WriteSession::tx_release_inode(uint32_t number, uint8_t *raw)
{
    if (number < first_inode_ || number > volume_.inodes_) return -EINVAL;
    int xattr_error = tx_release_xattrs(number, raw);
    if (xattr_error) return xattr_error;
    if (le32(raw + 4) || le32(raw + 0x6c) || le32(raw + 0x1c) || le16(raw + 0x74)) return -EUCLEAN;
    uint32_t group = (number - 1) / volume_.inodes_per_group_;
    uint32_t index = (number - 1) % volume_.inodes_per_group_;
    uint8_t *bitmap, *descriptor;
    int error = tx_bitmap(group, true, bitmap, descriptor);
    if (error) return error;
    if (!(bitmap[index / 8] & (1U << (index % 8)))) return stop(-EUCLEAN);
    bitmap[index / 8] &= uint8_t(~(1U << (index % 8)));
    put16(raw + 0x1a, 0);
    uint32_t low_seconds = le32(raw + 0xc);
    uint64_t seconds = uint64_t(int64_t(int32_t(low_seconds)));
    if (volume_.inode_size_ >= 160 && le16(raw + 0x80) >= 16)
        seconds += uint64_t(le32(raw + 0x84) & 3U) << 32;
    put32(raw + 0x14, low_seconds ? low_seconds : 1);
    error = tx_counts(group, 0, 1, (le16(raw) & 0170000) == 0040000 ? -1 : 0);
    if (!error) error = tx_finish_inode(number, raw, seconds, false);
    return error;
}

int WriteSession::tx_resize(uint32_t number, uint8_t *raw, uint64_t size)
{
    if (number != tx_inode_number(raw)) return -EINVAL;
    uint16_t type = le16(raw) & 0170000;
    uint64_t previous = le32(raw + 4) | uint64_t(le32(raw + 0x6c)) << 32;
    uint32_t block_size = volume_.block_size_;
    uint64_t limit = uint64_t(1) << 32;
    if (!(le32(raw + 0x20) & 0x80000U))
    {
        uint64_t pointers = block_size / 4;
        limit = 12 + pointers + pointers * pointers + pointers * pointers * pointers;
    }
    if (size > limit * block_size) return -EFBIG;
    uint64_t xattr = le32(raw + 0x68) | uint64_t(le16(raw + 0x76)) << 32;
    uint64_t sectors = le32(raw + 0x1c) | uint64_t(le16(raw + 0x74)) << 32;
    bool inline_symlink = type == 0120000 && !(le32(raw + 0x20) & 0x80000U) && previous <= 60 &&
                          sectors == (xattr ? block_size / 512 : 0);
    if (inline_symlink)
    {
        if (size) return size == previous ? 0 : -EOPNOTSUPP;
        for (unsigned i = 0; i < 60; ++i) raw[0x28 + i] = 0;
        put32(raw + 4, 0); put32(raw + 0x6c, 0);
        return 0;
    }
    if (type != 0100000 && type != 0040000 && type != 0120000) return -EOPNOTSUPP;
    uint64_t keep = (size + block_size - 1) / block_size;
    if (size < previous)
    {
        for (;;)
        {
            uint32_t logical = 0; bool found = false;
            int error = tx_mapping_last(raw, logical, found);
            if (error) return error;
            if (!found || logical < keep) break;
            error = tx_mapping_remove_last(raw, logical);
            if (error) return error;
        }
    }
    uint64_t boundary = size < previous ? size : previous;
    if (size != previous && boundary % block_size)
    {
        uint64_t disk = 0;
        int error = tx_map(raw, uint32_t(boundary / block_size), disk);
        if (error) return error;
        if (disk)
        {
            uint8_t *bytes;
            error = tx_edit(disk, bytes);
            if (error) return error;
            for (uint32_t i = uint32_t(boundary % block_size); i < block_size; ++i) bytes[i] = 0;
        }
    }
    put32(raw + 4, uint32_t(size)); put32(raw + 0x6c, uint32_t(size >> 32));
    return 0;
}

int WriteSession::tx_finish_inode(uint32_t number, uint8_t *raw, uint64_t seconds, bool modify_mtime)
{
    bool extended_time = volume_.inode_size_ >= 160 && le16(raw + 0x80) >= 16;
    if (seconds > 0x37fffffffULL || (!extended_time && seconds > 0x7fffffffULL)) return -EOVERFLOW;
    put32(raw + 0xc, uint32_t(seconds));
    if (modify_mtime) put32(raw + 0x10, uint32_t(seconds));
    if (extended_time)
    {
        uint32_t epoch = uint32_t((int64_t(seconds) - int64_t(int32_t(seconds))) >> 32);
        put32(raw + 0x84, epoch);
        if (modify_mtime) put32(raw + 0x88, epoch);
    }
    tx_seal_inode(number, raw);
    return 0;
}

void WriteSession::tx_seal_inode(uint32_t number, uint8_t *raw)
{
    if (volume_.readonly_ & 0x400U)
    {
        Inode inode = {};
        inode.number = number;
        inode.generation = le32(raw + 0x64);
        put16(raw + 0x7c, 0);
        bool high = volume_.inode_size_ >= 132 && le16(raw + 0x80) >= 4;
        if (high) put16(raw + 0x82, 0);
        uint32_t checksum = crc(volume_.inode_seed(inode), raw, volume_.inode_size_);
        put16(raw + 0x7c, uint16_t(checksum));
        if (high) put16(raw + 0x82, uint16_t(checksum >> 16));
    }
}

int WriteSession::tx_commit()
{
    if (!tx_active_ || !tx_count_) return -EINVAL;
    for (uint32_t group = 0; group < volume_.groups_; ++group)
    {
        if (!tx_dirty_[group]) continue;
        uint8_t *descriptor;
        int error = tx_descriptor(group, descriptor);
        if (error) { tx_cancel(); return error; }
        for (unsigned inode = 0; inode < 2; ++inode)
        {
            if (!(tx_dirty_[group] & (inode ? 2U : 1U))) continue;
            uint8_t bitmap[4096];
            error = tx_read(inode ? groups_[group].inode_bitmap : groups_[group].bitmap, bitmap);
            if (error) { tx_cancel(); return error; }
            if (volume_.readonly_ & 0x400U)
            {
                uint32_t checksum = crc(volume_.checksum_seed_, bitmap,
                                       (inode ? volume_.inodes_per_group_ : volume_.blocks_per_group_) / 8);
                put16(descriptor + (inode ? 0x1a : 0x18), uint16_t(checksum));
                if (volume_.descriptor_size_ >= 64)
                    put16(descriptor + (inode ? 0x3a : 0x38), uint16_t(checksum >> 16));
            }
        }
        tx_seal_group(group, descriptor);
    }
    for (unsigned i = 0; i < tx_count_; ++i)
    {
        if (tx_blocks_[i] != 1024 / volume_.block_size_) continue;
        uint8_t *super = workspace_->blocks[i] + 1024 % volume_.block_size_;
        if (volume_.readonly_ & 0x400U) put32(super + 0x3fc, crc(0xffffffffU, super, 0x3fc));
    }
    // Freed mapping/data blocks have no home after-image. Compact only after
    // all callers have finished using their stable workspace pointers.
    unsigned count = 0;
    for (unsigned i = 0; i < tx_count_; ++i)
    {
        if (tx_blocks_[i] == ~uint64_t(0)) continue;
        if (count != i) copy(workspace_->blocks[count], workspace_->blocks[i], volume_.block_size_);
        tx_blocks_[count++] = tx_blocks_[i];
    }
    tx_count_ = count;
    BlockUpdate updates[16];
    for (unsigned i = 0; i < count; ++i) updates[i] = {tx_blocks_[i], workspace_->blocks[i]};
    int error = journal_.commit(updates, count);
    tx_cancel();
    if (error) return stop(error);
    error = verify_recovered_layout();
    return error ? stop(error) : 0;
}

int WriteSession::pwrite(uint32_t number, uint64_t offset, const void *data, Size length,
                        uint64_t seconds, Size &completed)
{
    completed = 0;
    if (!active_) return -EINVAL;
    if (failed_ || journal_.failed()) return stop(-EIO);
    if (!length) return 0;
    if (!data || number < first_inode_) return -EINVAL;
    if (seconds > 0x37fffffffULL) return -EOVERFLOW;
    uint32_t size = volume_.block_size_;
    if (offset > (1ULL << 32) * size || length > (1ULL << 32) * size - offset) return -EFBIG;
    while (completed < length)
    {
        Inode inode;
        int error = volume_.read_inode(number, inode);
        if (error) return stop(error);
        if ((inode.mode & 0170000) != 0100000 || (inode.flags & ~(0x80000U | 0xc8U))) return -EOPNOTSUPP;
        uint64_t position = offset + completed;
        Size amount = length - completed;
        uint64_t count = (position % size + amount - 1) / size + 1;
        bool old_overwrite = position <= inode.size && amount <= inode.size - position;
        if (!old_overwrite || count > 15)
        {
            Size maximum = Size(8) * size - position % size;
            if (amount > maximum) amount = maximum;
        }
        for (;;)
        {
            error = tx_begin();
            uint8_t *raw = nullptr;
            if (!error) error = tx_inode(number, raw);
            if (!error && position + amount > inode.size) error = tx_resize(number, raw, position + amount);
            Size copied = 0;
            while (!error && copied < amount)
            {
                uint64_t current = position + copied;
                uint64_t physical;
                error = tx_map(raw, uint32_t(current / size), physical, true);
                uint8_t *block = nullptr;
                if (!error) error = tx_edit(physical, block);
                Size part = size - current % size;
                if (part > amount - copied) part = amount - copied;
                if (!error) copy(block + current % size, static_cast<const uint8_t *>(data) + completed + copied, part);
                copied += part;
            }
            if (!error) error = tx_finish_inode(number, raw, seconds);
            if (!error) error = tx_commit();
            else tx_cancel();
            if (error == -E2BIG && amount > size - position % size)
            {
                amount = size - position % size;
                continue;
            }
            break;
        }
        if (error) return stop(error);
        completed += amount;
    }
    return 0;
}

int WriteSession::truncate(uint32_t number, uint64_t size, uint64_t seconds)
{
    if (number < first_inode_) return -EINVAL;
    if (seconds > 0x37fffffffULL) return -EOVERFLOW;
    int error = tx_begin();
    uint8_t *raw = nullptr;
    if (!error) error = tx_inode(number, raw);
    if (!error && (le16(raw) & 0170000) != 0100000) error = -EISDIR;
    if (!error) error = tx_resize(number, raw, size);
    if (!error) error = tx_finish_inode(number, raw, seconds);
    if (!error) return tx_commit();
    tx_cancel();
    if (error == -E2BIG) return begin_orphan_truncate(number, size, seconds);
    return stop(error);
}

int WriteSession::sync()
{
    if (!active_) return -EINVAL;
    if (failed_ || journal_.failed()) return stop(-EIO);
    return device_.flush(device_.context) ? 0 : stop(-EIO);
}

int WriteSession::close()
{
    if (held_orphan_count_) return -EBUSY;
    if (active_ && !failed_)
    {
        int cleanup = recover_orphans();
        if (cleanup) return cleanup;
    }
    int error = sync();
    if (error) return error;
    error = set_recovery(false);
    if (!error) { active_ = false; volume_.reset(); }
    return error;
}

int WriteSession::shutdown()
{
    if (!active_) return failed_ ? -EIO : -EINVAL;
    if (failed_) return -EIO;
    held_orphan_count_ = 0;
    bit_clear(held_bits_);
    return close();
}

} // namespace xjext4
