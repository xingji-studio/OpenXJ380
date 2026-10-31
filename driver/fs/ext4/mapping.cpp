// SPDX-License-Identifier: Apache-2.0
// Original extent-tree and indirect-pointer mutation routines. Storage field
// facts come from the ext4 format descriptions; no upstream driver is imported.
#include "write.h"
#include <errno.h>

namespace xjext4 {
namespace {

uint16_t get16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t get32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void set16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void set32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void clear(void *p, Size n) { for (Size i = 0; i < n; ++i) static_cast<uint8_t *>(p)[i] = 0; }
void copy(void *p, const void *q, Size n)
{
    for (Size i = 0; i < n; ++i) static_cast<uint8_t *>(p)[i] = static_cast<const uint8_t *>(q)[i];
}
uint32_t checksum(uint32_t seed, const uint8_t *bytes, Size length)
{
    for (Size i = 0; i < length; ++i)
    {
        seed ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) seed = (seed >> 1) ^ ((seed & 1) ? 0x82f63b78U : 0);
    }
    return seed;
}
uint64_t extent_block(const uint8_t *p) { return get32(p + 8) | uint64_t(get16(p + 6)) << 32; }
uint64_t index_block(const uint8_t *p) { return get32(p + 4) | uint64_t(get16(p + 8)) << 32; }
uint32_t extent_length(const uint8_t *p)
{
    uint32_t n = get16(p + 4);
    return n > 32768 ? n - 32768 : n;
}
bool unwritten(const uint8_t *p) { return get16(p + 4) > 32768; }
void set_extent(uint8_t *p, uint32_t logical, uint32_t length, uint64_t block, bool unused = false)
{
    set32(p, logical); set16(p + 4, uint16_t(length + (unused ? 32768 : 0)));
    set16(p + 6, uint16_t(block >> 32)); set32(p + 8, uint32_t(block));
}
void set_index(uint8_t *p, uint32_t logical, uint64_t block)
{
    set32(p, logical); set32(p + 4, uint32_t(block)); set16(p + 8, uint16_t(block >> 32)); set16(p + 10, 0);
}
struct ExtentPath
{
    uint64_t block[6];
    uint16_t index[6];
    unsigned levels;
};

// Descend through verified immutable node images. For insertion before the
// first key we choose child zero; its ancestors will receive the new lower key.
template<class Read, class Check>
int find_extent_path(const uint8_t *raw, uint32_t logical, uint32_t block_size, uint32_t seed, bool crc,
                     ExtentPath &path, uint8_t *leaf, Read read, Check check)
{
    clear(&path, sizeof(path));
    clear(leaf, 4096);
    copy(leaf, raw + 0x28, 60);
    uint16_t depth = get16(leaf + 6);
    if (depth > 5) return -EUCLEAN;
    uint64_t lower = 0, upper = uint64_t(1) << 32;
    for (unsigned level = 0; level <= 5; ++level)
    {
        uint16_t maximum = get16(leaf + 4), entries = get16(leaf + 2);
        unsigned capacity = level ? (block_size - 12) / 12 : 4;
        if (get16(leaf) != 0xf30a || get16(leaf + 6) != depth || !maximum || maximum > capacity ||
            entries > maximum || (depth && !entries)) return -EUCLEAN;
        if (level && crc)
        {
            unsigned tail = 12 + maximum * 12;
            if (tail + 4 > block_size || checksum(seed, leaf, tail) != get32(leaf + tail)) return -EUCLEAN;
        }
        int chosen = 0;
        uint64_t previous = lower;
        for (unsigned i = 0; i < entries; ++i)
        {
            const uint8_t *entry = leaf + 12 + i * 12;
            uint64_t start = get32(entry);
            if (start < lower || start >= upper || (i && start < previous)) return -EUCLEAN;
            if (depth)
            {
                if (!index_block(entry)) return -EUCLEAN;
                previous = start + 1;
                if (logical >= start) chosen = int(i);
            }
            else
            {
                uint32_t length = extent_length(entry);
                if (!length || start + length > upper || !extent_block(entry)) return -EUCLEAN;
                previous = start + length;
            }
        }
        path.levels = level + 1;
        path.index[level] = uint16_t(chosen);
        if (!depth) return 0;
        const uint8_t *entry = leaf + 12 + chosen * 12;
        uint64_t child = index_block(entry);
        for (unsigned previous_level = 1; previous_level <= level; ++previous_level)
            if (path.block[previous_level] == child) return -EUCLEAN;
        int error = check(child);
        if (error) return error;
        lower = get32(entry);
        if (unsigned(chosen + 1) < entries) upper = get32(entry + 12);
        path.block[level + 1] = child;
        error = read(child, leaf);
        if (error) return error;
        --depth;
    }
    return -EUCLEAN;
}

void seal_node(uint8_t *node, uint32_t size, uint32_t seed, bool crc, unsigned depth,
               unsigned maximum, const uint8_t *records, unsigned count, bool root)
{
    uint32_t generation = get32(node + 8);
    clear(node, root ? 60 : size);
    set16(node, 0xf30a); set16(node + 2, uint16_t(count)); set16(node + 4, uint16_t(maximum));
    set16(node + 6, uint16_t(depth)); set32(node + 8, generation);
    copy(node + 12, records, count * 12);
    if (!root && crc) set32(node + 12 + maximum * 12, checksum(seed, node, 12 + maximum * 12));
}

} // namespace

uint32_t WriteSession::tx_inode_number(const uint8_t *raw) const
{
    const unsigned size = volume_.block_size_;
    for (unsigned i = 0; i < tx_count_; ++i)
    {
        if (tx_blocks_[i] == ~uint64_t(0)) continue;
        uintptr_t offset = uintptr_t(raw) - uintptr_t(workspace_->blocks[i]);
        if (offset >= size || offset % volume_.inode_size_) continue;
        for (uint32_t group = 0; group < volume_.groups_; ++group)
        {
            uint64_t first = groups_[group].inode_table;
            uint64_t count = (uint64_t(volume_.inodes_per_group_) * volume_.inode_size_ + size - 1) / size;
            if (tx_blocks_[i] < first || tx_blocks_[i] - first >= count) continue;
            return uint32_t(uint64_t(group) * volume_.inodes_per_group_ +
                            ((tx_blocks_[i] - first) * size + offset) / volume_.inode_size_ + 1);
        }
    }
    return 0;
}

int WriteSession::tx_mapping_block(uint64_t block, uint64_t xattr)
{
    if (protected_block(block) || block == xattr) return stop(-EUCLEAN);
    uint32_t group = uint32_t((block - volume_.first_data_block_) / volume_.blocks_per_group_);
    if (!(tx_dirty_[group] & 1U)) return check_target_block(block);
    uint8_t bitmap[4096];
    int error = tx_read(groups_[group].bitmap, bitmap);
    if (error) return error;
    uint32_t bit = uint32_t((block - volume_.first_data_block_) % volume_.blocks_per_group_);
    return bitmap[bit / 8] & (1U << (bit % 8)) ? 0 : stop(-EUCLEAN);
}

int WriteSession::tx_change_sectors(uint8_t *raw, int64_t blocks)
{
    uint64_t sectors = get32(raw + 0x1c) | uint64_t(get16(raw + 0x74)) << 32;
    int64_t next = int64_t(sectors) + blocks * int64_t(volume_.block_size_ / 512);
    if (next < 0) return stop(-EUCLEAN);
    if (uint64_t(next) >= (uint64_t(1) << 48)) return -EFBIG;
    set32(raw + 0x1c, uint32_t(next)); set16(raw + 0x74, uint16_t(uint64_t(next) >> 32));
    return 0;
}

int WriteSession::tx_mapping_witness(uint64_t block)
{
    uint32_t group = uint32_t((block - volume_.first_data_block_) / volume_.blocks_per_group_);
    uint8_t *bitmap, *descriptor;
    int error = tx_bitmap(group, false, bitmap, descriptor);
    return error ? error : tx_counts(group, 0, 0, 0);
}

int WriteSession::tx_map(uint8_t *raw, uint32_t logical, uint64_t &physical, bool allocate)
{
    physical = 0;
    uint32_t allowed = 0x80000U | 0x10000U | 0xc8U;
    if ((get16(raw) & 0170000) == 0040000) allowed |= 0x1000U;
    if (get32(raw + 0x20) & ~allowed) return -EOPNOTSUPP;
    uint32_t number = tx_inode_number(raw);
    if (!number) return -EINVAL;
    return (get32(raw + 0x20) & 0x80000U) ? tx_extent_map(number, raw, logical, physical, allocate) :
                                                         tx_indirect_map(raw, logical, physical, allocate);
}

int WriteSession::tx_extent_map(uint32_t number, uint8_t *raw, uint32_t logical,
                                 uint64_t &physical, bool allocate)
{
    Inode identity = {};
    identity.number = number; identity.generation = get32(raw + 0x64);
    const uint32_t seed = volume_.inode_seed(identity), size = volume_.block_size_;
    const bool crc = (volume_.readonly_ & 0x400U) != 0;
    const uint64_t xattr = get32(raw + 0x68) | uint64_t(get16(raw + 0x76)) << 32;
    ExtentPath path;
    uint8_t node[4096], records[4128];
    int error = find_extent_path(raw, logical, size, seed, crc, path, node,
        [&](uint64_t block, uint8_t *bytes) { return tx_read(block, bytes); },
        [&](uint64_t block) { return tx_mapping_block(block, xattr); });
    if (error) return error;
    unsigned count = get16(node + 2);
    copy(records, node + 12, count * 12);
    unsigned insertion = 0;
    while (insertion < count && get32(records + insertion * 12) <= logical)
    {
        uint8_t *entry = records + insertion * 12;
        uint32_t length = extent_length(entry);
        if (uint64_t(logical) - get32(entry) < length)
        {
            physical = extent_block(entry) + logical - get32(entry);
            error = tx_mapping_block(physical, xattr);
            if (error) return error;
            for (unsigned level = 1; level < path.levels; ++level)
                if (physical == path.block[level]) return stop(-EUCLEAN);
            if (!unwritten(entry)) return 0;
            if (!allocate) { physical = 0; return 0; }
            uint32_t before = logical - get32(entry), after = length - before - 1;
            uint32_t start = get32(entry); uint64_t disk = extent_block(entry);
            unsigned extra = (before ? 1U : 0U) + (after ? 1U : 0U);
            for (unsigned i = count; i > insertion + 1; --i)
                copy(records + (i + extra - 1) * 12, records + (i - 1) * 12, 12);
            unsigned out = insertion;
            if (before) set_extent(records + out++ * 12, start, before, disk, true);
            set_extent(records + out++ * 12, logical, 1, physical);
            if (after) set_extent(records + out++ * 12, logical + 1, after, physical + 1, true);
            count += extra;
            uint8_t *data;
            error = tx_edit(physical, data, true);
            if (!error) error = tx_mapping_witness(physical);
            if (error) return error;
            goto store;
        }
        ++insertion;
    }
    if (!allocate) return 0;
    error = tx_allocate_block(physical);
    if (error) return error;
    if (physical == xattr) return stop(-EUCLEAN);
    error = tx_change_sectors(raw, 1);
    if (error) return error;
    for (unsigned i = count; i > insertion; --i) copy(records + i * 12, records + (i - 1) * 12, 12);
    set_extent(records + insertion * 12, logical, 1, physical);
    ++count;
store:
    // Neighboring records with identical initialization state may coalesce.
    for (unsigned i = 1; i < count;)
    {
        uint8_t *left = records + (i - 1) * 12, *right = records + i * 12;
        uint32_t combined = extent_length(left) + extent_length(right);
        if (unwritten(left) == unwritten(right) && combined <= (unwritten(left) ? 32767U : 32768U) &&
            uint64_t(get32(left)) + extent_length(left) == get32(right) &&
            extent_block(left) + extent_length(left) == extent_block(right))
        {
            set16(left + 4, uint16_t(combined + (unwritten(left) ? 32768 : 0)));
            for (unsigned j = i; j + 1 < count; ++j) copy(records + j * 12, records + (j + 1) * 12, 12);
            --count;
        }
        else ++i;
    }
    {
        unsigned level = path.levels - 1, depth = 0;
        bool carry = false;
        uint64_t right_block = 0;
        uint32_t right_key = 0;
        for (;;)
        {
            uint8_t *edited;
            if (!level) edited = raw + 0x28;
            else { error = tx_edit(path.block[level], edited); if (error) return error; }
            unsigned maximum = level ? get16(edited + 4) : 4;
            uint32_t first_key = count ? get32(records) : 0;
            if (count > maximum)
            {
                if (!level)
                {
                    if (depth == 5) return -EFBIG;
                    uint64_t child;
                    error = tx_allocate_block(child);
                    if (!error) error = tx_change_sectors(raw, 1);
                    uint8_t *bytes;
                    if (!error) error = tx_edit(child, bytes);
                    if (error) return error;
                    seal_node(bytes, size, seed, crc, depth, (size - 12) / 12, records, count, false);
                    uint8_t index[12]; set_index(index, first_key, child);
                    seal_node(edited, size, seed, false, depth + 1, 4, index, 1, true);
                    return 0;
                }
                unsigned left_count = count / 2;
                error = tx_allocate_block(right_block);
                if (!error) error = tx_change_sectors(raw, 1);
                uint8_t *right;
                if (!error) error = tx_edit(right_block, right);
                if (error) return error;
                right_key = get32(records + left_count * 12);
                seal_node(right, size, seed, crc, depth, maximum, records + left_count * 12, count - left_count, false);
                seal_node(edited, size, seed, crc, depth, maximum, records, left_count, false);
                carry = true;
            }
            else
            {
                seal_node(edited, size, seed, crc, depth, maximum, records, count, level == 0);
                carry = false;
            }
            if (!level) return 0;
            --level; ++depth;
            uint8_t parent[4096];
            if (!level) { clear(parent, sizeof(parent)); copy(parent, raw + 0x28, 60); }
            else { error = tx_read(path.block[level], parent); if (error) return error; }
            count = get16(parent + 2);
            copy(records, parent + 12, count * 12);
            unsigned position = path.index[level];
            if (position >= count) return stop(-EUCLEAN);
            bool first_changed = get32(records + position * 12) != first_key;
            set32(records + position * 12, first_key);
            if (carry)
            {
                for (unsigned i = count; i > position + 1; --i)
                    copy(records + i * 12, records + (i - 1) * 12, 12);
                set_index(records + (position + 1) * 12, right_key, right_block);
                ++count;
            }
            else if (!first_changed) return 0;
        }
    }
}

int WriteSession::tx_indirect_map(uint8_t *raw, uint32_t logical, uint64_t &physical, bool allocate)
{
    const uint64_t xattr = get32(raw + 0x68) | uint64_t(get16(raw + 0x76)) << 32;
    uint32_t pointers = volume_.block_size_ / 4;
    if (logical < 12)
    {
        physical = get32(raw + 0x28 + logical * 4);
        if (physical) return tx_mapping_block(physical, xattr);
        if (!allocate) return 0;
        int error = tx_allocate_block(physical);
        if (!error && physical > 0xffffffffULL) error = -EFBIG;
        if (!error) error = tx_change_sectors(raw, 1);
        if (!error) set32(raw + 0x28 + logical * 4, uint32_t(physical));
        return error;
    }
    uint64_t remaining = uint64_t(logical) - 12, span = pointers;
    unsigned levels = 1;
    while (levels <= 3 && remaining >= span) { remaining -= span; span *= pointers; ++levels; }
    if (levels > 3) return -EFBIG;
    uint8_t *parent = raw + 0x28 + (11 + levels) * 4;
    uint64_t ancestors[3] = {};
    for (unsigned depth = 0; depth < levels; ++depth)
    {
        uint64_t block = get32(parent);
        int error = 0;
        if (!block)
        {
            if (!allocate) { physical = 0; return 0; }
            error = tx_allocate_block(block);
            if (!error && block > 0xffffffffULL) error = -EFBIG;
            if (!error) error = tx_change_sectors(raw, 1);
            if (error) return error;
            set32(parent, uint32_t(block));
        }
        error = tx_mapping_block(block, xattr);
        if (error) return error;
        for (unsigned i = 0; i < depth; ++i) if (ancestors[i] == block) return stop(-EUCLEAN);
        ancestors[depth] = block;
        span /= pointers;
        unsigned index = unsigned(remaining / span);
        remaining %= span;
        if (index >= pointers) return stop(-EUCLEAN);
        if (allocate)
        {
            uint8_t *bytes;
            error = tx_edit(block, bytes);
            if (error) return error;
            parent = bytes + index * 4;
        }
        else
        {
            uint8_t bytes[4096];
            error = tx_read(block, bytes);
            if (error) return error;
            physical = get32(bytes + index * 4);
            if (depth + 1 == levels)
            {
                if (physical) return tx_mapping_block(physical, xattr);
                return 0;
            }
            // The next pointer must remain valid after the scratch block ends.
            // Store it in a local word while continuing the read-only path.
            uint64_t next = physical;
            for (unsigned later = depth + 1; later < levels; ++later)
            {
                if (!next) { physical = 0; return 0; }
                error = tx_mapping_block(next, xattr);
                if (error) return error;
                for (unsigned i = 0; i < later; ++i) if (ancestors[i] == next) return stop(-EUCLEAN);
                ancestors[later] = next;
                span /= pointers;
                index = unsigned(remaining / span); remaining %= span;
                error = tx_read(next, bytes);
                if (error) return error;
                next = get32(bytes + index * 4);
            }
            physical = next;
            if (physical)
            {
                for (unsigned i = 0; i < levels; ++i) if (ancestors[i] == physical) return stop(-EUCLEAN);
                return tx_mapping_block(physical, xattr);
            }
            return 0;
        }
    }
    physical = get32(parent);
    if (!physical && allocate)
    {
        int error = tx_allocate_block(physical);
        if (!error && physical > 0xffffffffULL) error = -EFBIG;
        if (!error) error = tx_change_sectors(raw, 1);
        if (error) return error;
        set32(parent, uint32_t(physical));
    }
    if (physical)
    {
        for (unsigned i = 0; i < levels; ++i) if (ancestors[i] == physical) return stop(-EUCLEAN);
        return tx_mapping_block(physical, xattr);
    }
    return 0;
}

int WriteSession::tx_mapping_last(uint8_t *raw, uint32_t &logical, bool &found)
{
    uint32_t number = tx_inode_number(raw);
    if (!number) return -EINVAL;
    return get32(raw + 0x20) & 0x80000U ? tx_extent_last(number, raw, logical, found, false) :
                                       tx_indirect_last(raw, logical, found, false);
}

int WriteSession::tx_mapping_remove_last(uint8_t *raw, uint32_t logical)
{
    uint32_t number = tx_inode_number(raw), actual = logical;
    if (!number) return -EINVAL;
    bool found = false;
    int error = get32(raw + 0x20) & 0x80000U ? tx_extent_last(number, raw, actual, found, true) :
                                                            tx_indirect_last(raw, actual, found, true);
    return !error && (!found || actual != logical) ? stop(-EUCLEAN) : error;
}

int WriteSession::tx_extent_last(uint32_t number, uint8_t *raw, uint32_t &logical, bool &found, bool remove)
{
    Inode identity = {}; identity.number = number; identity.generation = get32(raw + 0x64);
    const uint32_t seed = volume_.inode_seed(identity), size = volume_.block_size_;
    const bool crc = (volume_.readonly_ & 0x400U) != 0;
    const uint64_t xattr = get32(raw + 0x68) | uint64_t(get16(raw + 0x76)) << 32;
    ExtentPath path; uint8_t node[4096], records[4096];
    int error = find_extent_path(raw, 0xffffffffU, size, seed, crc, path, node,
        [&](uint64_t block, uint8_t *bytes) { return tx_read(block, bytes); },
        [&](uint64_t block) { return tx_mapping_block(block, xattr); });
    if (error) return error;
    unsigned count = get16(node + 2);
    found = count != 0;
    if (!found) return 0;
    uint8_t *last = node + 12 + (count - 1) * 12;
    uint32_t length = extent_length(last);
    logical = get32(last) + length - 1;
    if (!remove) return 0;
    uint64_t data = extent_block(last) + length - 1;
    error = tx_mapping_block(data, xattr);
    if (!error) error = tx_release_block(data);
    if (!error) error = tx_change_sectors(raw, -1);
    if (error) return error;
    tx_forget(data);
    if (length > 1) set16(last + 4, uint16_t(length - 1 + (unwritten(last) ? 32768 : 0)));
    else --count;
    copy(records, node + 12, count * 12);
    unsigned level = path.levels - 1, depth = 0;
    for (;;)
    {
        uint8_t *edited;
        if (!level) edited = raw + 0x28;
        else { error = tx_edit(path.block[level], edited); if (error) return error; }
        if (count || !level)
        {
            seal_node(edited, size, seed, crc, depth, level ? get16(edited + 4) : 4, records, count, !level);
            break;
        }
        uint64_t retired = path.block[level];
        error = tx_release_block(retired);
        if (!error) error = tx_change_sectors(raw, -1);
        if (error) return error;
        tx_forget(retired);
        --level; ++depth;
        if (!level) { clear(node, sizeof(node)); copy(node, raw + 0x28, 60); }
        else { error = tx_read(path.block[level], node); if (error) return error; }
        count = get16(node + 2);
        unsigned position = path.index[level];
        if (!count || position >= count) return stop(-EUCLEAN);
        copy(records, node + 12, count * 12);
        for (unsigned i = position; i + 1 < count; ++i) copy(records + i * 12, records + (i + 1) * 12, 12);
        --count;
        if (!count && !level) depth = 0;
    }
    // Shrinking a root with one sufficiently small child avoids permanently
    // retaining unnecessary metadata levels after a fragmented file is freed.
    while (get16(raw + 0x2e) && get16(raw + 0x2a) == 1)
    {
        uint64_t child = index_block(raw + 0x34);
        error = tx_read(child, node);
        if (error) return error;
        count = get16(node + 2);
        if (count > 4) break;
        copy(records, node + 12, count * 12);
        unsigned child_depth = get16(node + 6);
        seal_node(raw + 0x28, size, seed, false, child_depth, 4, records, count, true);
        error = tx_release_block(child);
        if (!error) error = tx_change_sectors(raw, -1);
        if (error) return error;
        tx_forget(child);
    }
    return 0;
}

int WriteSession::tx_indirect_last(uint8_t *raw, uint32_t &logical, bool &found, bool remove)
{
    const uint32_t pointers = volume_.block_size_ / 4;
    const uint64_t xattr = get32(raw + 0x68) | uint64_t(get16(raw + 0x76)) << 32;
    uint64_t path[3] = {}; unsigned positions[3] = {}, root_slot = 0, levels = 0;
    found = false;
    auto descend = [&](auto &&self, uint64_t block, unsigned depth, uint64_t base, unsigned level) -> int
    {
        if (level >= 3) return stop(-EUCLEAN);
        int error = tx_mapping_block(block, xattr);
        if (error) return error;
        for (unsigned i = 0; i < level; ++i) if (path[i] == block) return stop(-EUCLEAN);
        path[level] = block;
        uint8_t bytes[4096];
        error = tx_read(block, bytes);
        if (error) return error;
        uint64_t span = 1;
        for (unsigned i = 1; i < depth; ++i) span *= pointers;
        for (unsigned i = pointers; i-- > 0;)
        {
            uint64_t child = get32(bytes + i * 4);
            if (!child) continue;
            positions[level] = i;
            if (depth == 1)
            {
                uint64_t at = base + i;
                if (at > 0xffffffffULL) return -EFBIG;
                logical = uint32_t(at); found = true;
                return 0;
            }
            error = self(self, child, depth - 1, base + i * span, level + 1);
            if (error || found) return error;
        }
        return 0;
    };
    uint64_t roots[3] = {12, uint64_t(12) + pointers, uint64_t(12) + pointers + uint64_t(pointers) * pointers};
    for (unsigned depth = 3; depth > 0 && !found; --depth)
    {
        uint64_t block = get32(raw + 0x28 + (11 + depth) * 4);
        if (!block) continue;
        root_slot = 11 + depth; levels = depth;
        int error = descend(descend, block, depth, roots[depth - 1], 0);
        if (error) return error;
    }
    if (!found)
    {
        for (unsigned i = 12; i-- > 0;)
        {
            uint64_t block = get32(raw + 0x28 + i * 4);
            if (!block) continue;
            logical = i; found = true; levels = 0; root_slot = i; break;
        }
    }
    if (!found || !remove) return 0;
    uint8_t *parent = raw + 0x28 + root_slot * 4;
    for (unsigned level = 0; level < levels; ++level)
    {
        uint8_t *bytes;
        int error = tx_edit(path[level], bytes);
        if (error) return error;
        parent = bytes + positions[level] * 4;
    }
    uint64_t data = get32(parent);
    int error = tx_mapping_block(data, xattr);
    if (!error) error = tx_release_block(data);
    if (!error) error = tx_change_sectors(raw, -1);
    if (error) return error;
    set32(parent, 0); tx_forget(data);
    for (unsigned level = levels; level-- > 0;)
    {
        uint8_t *bytes;
        error = tx_edit(path[level], bytes);
        if (error) return error;
        bool any = false;
        for (unsigned i = 0; i < pointers; ++i) any |= get32(bytes + i * 4) != 0;
        if (any) break;
        error = tx_release_block(path[level]);
        if (!error) error = tx_change_sectors(raw, -1);
        if (error) return error;
        tx_forget(path[level]);
        if (!level) set32(raw + 0x28 + root_slot * 4, 0);
        else
        {
            uint8_t *ancestor;
            error = tx_edit(path[level - 1], ancestor);
            if (error) return error;
            set32(ancestor + positions[level - 1] * 4, 0);
        }
    }
    return 0;
}

int WriteSession::claim_mapping_block(const Volume &after, uint64_t block)
{
    if (!block || block < after.first_data_block_ || block >= after.blocks_ || protected_block(block) ||
        tx_is_xattr_block(block) || block >= mapping_bits_.bits)
        return -EUCLEAN;
    if (bit_test(mapping_bits_, block)) return -EUCLEAN;
    uint32_t group = uint32_t((block - after.first_data_block_) / after.blocks_per_group_);
    if (group >= after.groups_) return -EUCLEAN;
    if (bitmap_group_ != group)
    {
        uint8_t *descriptor = descriptor_cache_;
        int error = after.read_group(group, descriptor);
        if (error) return error;
        if (get16(descriptor + 0x12) & 2U) return -EUCLEAN;
        error = after.fetch(groups_[group].bitmap * after.block_size_, bitmap_cache_, after.block_size_);
        if (error) return error;
        if (after.readonly_ & 0x400U)
        {
            uint32_t expected = get16(descriptor + 0x18);
            if (after.descriptor_size_ >= 64) expected |= uint32_t(get16(descriptor + 0x38)) << 16;
            uint32_t actual = checksum(after.checksum_seed_, bitmap_cache_, after.blocks_per_group_ / 8);
            if (after.descriptor_size_ == 32) actual &= 0xffffU;
            if (actual != expected) return -EUCLEAN;
        }
        bitmap_group_ = group;
    }
    uint32_t bit = uint32_t((block - after.first_data_block_) % after.blocks_per_group_);
    if (!(bitmap_cache_[bit / 8] & (1U << (bit % 8)))) return -EUCLEAN;
    if (!bit_set(mapping_bits_, block)) return -EUCLEAN;
    return 0;
}

int WriteSession::validate_extent_node(const Volume &after, const Inode &inode, const uint8_t *node,
                                      uint16_t depth, uint64_t lower, uint64_t upper, uint64_t &blocks)
{
    bool root = node == inode.block_map;
    unsigned capacity = root ? 4 : (after.block_size_ - 12) / 12;
    uint16_t count = get16(node + 2), maximum = get16(node + 4);
    if (depth > 5 || get16(node) != 0xf30a || get16(node + 6) != depth || !maximum ||
        maximum > capacity || count > maximum || (depth && !count) || (!root && !count)) return -EUCLEAN;
    if (!root && (after.readonly_ & 0x400U))
    {
        unsigned tail = 12 + maximum * 12;
        if (tail + 4 > after.block_size_ || checksum(after.inode_seed(inode), node, tail) != get32(node + tail))
            return -EUCLEAN;
    }
    uint64_t previous_end = lower;
    for (unsigned i = 0; i < count; ++i)
    {
        const uint8_t *entry = node + 12 + i * 12;
        uint64_t start = get32(entry);
        if (start < lower || start >= upper || (i && start < previous_end)) return -EUCLEAN;
        if (!depth)
        {
            uint32_t length = extent_length(entry);
            uint64_t physical = extent_block(entry);
            if (!length || start + length > upper || physical >= after.blocks_ || length > after.blocks_ - physical)
                return -EUCLEAN;
            for (uint32_t j = 0; j < length; ++j)
            {
                int error = claim_mapping_block(after, physical + j);
                if (error) return error;
            }
            blocks += length;
            previous_end = start + length;
        }
        else
        {
            uint64_t child = index_block(entry);
            int error = claim_mapping_block(after, child);
            if (error) return error;
            ++blocks;
            uint8_t bytes[4096];
            error = after.fetch(child * after.block_size_, bytes, after.block_size_);
            if (error) return error;
            uint64_t end = i + 1 < count ? get32(entry + 12) : upper;
            error = validate_extent_node(after, inode, bytes, depth - 1, start, end, blocks);
            if (error) return error;
            previous_end = start + 1;
        }
    }
    return 0;
}

int WriteSession::validate_indirect_node(const Volume &after, uint64_t block, unsigned depth, uint64_t &blocks)
{
    if (!depth || depth > 3) return -EUCLEAN;
    int error = claim_mapping_block(after, block);
    if (error) return error;
    ++blocks;
    uint8_t bytes[4096];
    error = after.fetch(block * after.block_size_, bytes, after.block_size_);
    if (error) return error;
    bool any = false;
    for (unsigned i = 0; i < after.block_size_ / 4; ++i)
    {
        uint64_t child = get32(bytes + i * 4);
        if (!child) continue;
        any = true;
        if (depth > 1) error = validate_indirect_node(after, child, depth - 1, blocks);
        else { error = claim_mapping_block(after, child); if (!error) ++blocks; }
        if (error) return error;
    }
    return any ? 0 : -EUCLEAN;
}

int WriteSession::validate_inode_mapping(const Volume &after, const Inode &inode, uint64_t &blocks)
{
    blocks = 0;
    if ((inode.mode & 0170000) == 0120000 && !(inode.flags & 0x80000U) && inode.size <= 60 &&
        inode.blocks_512 == (inode.xattr_block ? after.block_size_ / 512 : 0)) return 0;
    if (inode.flags & 0x80000U)
        return validate_extent_node(after, inode, inode.block_map, get16(inode.block_map + 6), 0,
                                    uint64_t(1) << 32, blocks);
    for (unsigned i = 0; i < 15; ++i)
    {
        uint64_t block = get32(inode.block_map + i * 4);
        if (!block) continue;
        int error;
        if (i >= 12) error = validate_indirect_node(after, block, i - 11, blocks);
        else { error = claim_mapping_block(after, block); if (!error) ++blocks; }
        if (error) return error;
    }
    return 0;
}

int WriteSession::inode_mapping_end(const Volume &after, const Inode &inode, uint64_t &end)
{
    end = 0;
    if (inode.flags & 0x80000U)
    {
        uint8_t raw[128] = {}, leaf[4096];
        copy(raw + 0x28, inode.block_map, 60);
        ExtentPath path;
        int error = find_extent_path(raw, 0xffffffffU, after.block_size_, after.inode_seed(inode),
            (after.readonly_ & 0x400U) != 0, path, leaf,
            [&](uint64_t block, uint8_t *bytes) { return after.fetch(block * after.block_size_, bytes, after.block_size_); },
            [&](uint64_t block) { return protected_block(block) || tx_is_xattr_block(block) ? -EUCLEAN : 0; });
        if (error) return error;
        unsigned count = get16(leaf + 2);
        if (count)
        {
            const uint8_t *last = leaf + 12 + (count - 1) * 12;
            end = uint64_t(get32(last)) + extent_length(last);
        }
        return 0;
    }
    uint32_t pointers = after.block_size_ / 4;
    auto descend = [&](auto &&self, uint64_t block, unsigned depth, uint64_t base, bool &found) -> int
    {
        if (protected_block(block) || tx_is_xattr_block(block)) return -EUCLEAN;
        uint8_t bytes[4096];
        int error = after.fetch(block * after.block_size_, bytes, after.block_size_);
        if (error) return error;
        uint64_t span = 1;
        for (unsigned i = 1; i < depth; ++i) span *= pointers;
        for (unsigned i = pointers; i-- > 0;)
        {
            uint64_t child = get32(bytes + i * 4);
            if (!child) continue;
            if (depth == 1) { end = base + i + 1; found = true; return 0; }
            error = self(self, child, depth - 1, base + i * span, found);
            if (error || found) return error;
        }
        return 0;
    };
    uint64_t bases[] = {12, uint64_t(12) + pointers, uint64_t(12) + pointers + uint64_t(pointers) * pointers};
    for (unsigned depth = 3; depth; --depth)
    {
        uint64_t block = get32(inode.block_map + (11 + depth) * 4);
        if (!block) continue;
        bool found = false;
        int error = descend(descend, block, depth, bases[depth - 1], found);
        if (error || found) return error;
    }
    for (unsigned i = 12; i-- > 0;)
        if (get32(inode.block_map + i * 4)) { end = i + 1; break; }
    return 0;
}

} // namespace xjext4
