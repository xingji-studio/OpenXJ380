// SPDX-License-Identifier: Apache-2.0
// Original traditional orphan-list mutations. Namespace removal and zero-link
// intent commit together; bounded cleanup and runtime FD holds share the list.
#include "write.h"
#include <errno.h>

namespace xjext4 {
namespace {
uint16_t get16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t get32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void set32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
}
bool supported(const Inode &inode)
{
    uint16_t type = inode.mode & 0170000;
    return (type == 0100000 || (!inode.links && (type == 0040000 || type == 0120000))) &&
           !(inode.flags & ~(0x80000U | 0x10000U | 0xc8U | (type == 0040000 ? 0x1000U : 0U)));
}
}

bool WriteSession::orphan_held(uint32_t number) const
{
    return number && number <= volume_.inodes_ &&
           bit_test(held_bits_, number - 1);
}

void WriteSession::hold_orphan(uint32_t number, bool held)
{
    if (!number || number > volume_.inodes_ || orphan_held(number) == held) return;
    uint32_t bit = number - 1;
    if (held) { bit_set(held_bits_, bit); ++held_orphan_count_; }
    else { bit_set(held_bits_, bit, false); --held_orphan_count_; }
}

int WriteSession::find_orphan(uint32_t number, uint32_t &previous, uint32_t &next, bool &found) const
{
    previous = next = 0;
    found = false;
    uint32_t current = get32(super_ + 0xe8);
    for (uint32_t visited = 0; current; ++visited)
    {
        if (visited >= volume_.inodes_ || current < first_inode_ || current > volume_.inodes_) return -EUCLEAN;
        uint8_t raw[4096];
        int error = volume_.read_raw_inode(current, raw);
        if (error) return error;
        next = get32(raw + 0x14);
        if (current == number) { found = true; return 0; }
        previous = current;
        current = next;
    }
    previous = next = 0;
    return 0;
}

int WriteSession::tx_add_orphan(uint32_t number, uint8_t *raw)
{
    uint32_t previous, next;
    bool found;
    int error = find_orphan(number, previous, next, found);
    if (error) return error;
    if (found) return -EEXIST;
    uint8_t *super_block;
    error = tx_edit(1024 / volume_.block_size_, super_block);
    if (!error)
    {
        set32(raw + 0x14, get32(super_ + 0xe8));
        set32(super_block + 1024 % volume_.block_size_ + 0xe8, number);
    }
    return error;
}

int WriteSession::tx_remove_orphan(uint32_t number, uint8_t *raw)
{
    uint32_t previous, next;
    bool found;
    int error = find_orphan(number, previous, next, found);
    if (error) return error;
    if (!found || next != get32(raw + 0x14)) return -EUCLEAN;
    if (previous)
    {
        uint8_t *before;
        error = tx_inode(previous, before);
        if (!error)
        {
            set32(before + 0x14, next);
            tx_seal_inode(previous, before);
        }
    }
    else
    {
        uint8_t *super_block;
        error = tx_edit(1024 / volume_.block_size_, super_block);
        if (!error) set32(super_block + 1024 % volume_.block_size_ + 0xe8, next);
    }
    if (!error) set32(raw + 0x14, 0);
    return error;
}

int WriteSession::validate_orphans_afterstate(const Volume &after)
{
    uint8_t encoded_head[4], raw[4096];
    int error = after.fetch(1024 + 0xe8, encoded_head, sizeof(encoded_head));
    if (error) return error;
    uint32_t head = get32(encoded_head), old_head = get32(super_ + 0xe8);
    if (!head && (!old_head || !active_)) return 0;
    if (after.inodes_ > mapping_bits_.bits) return -EOPNOTSUPP;
    bit_clear(mapping_bits_);
    uint32_t current = head;
    for (uint32_t visited = 0; current; ++visited)
    {
        if (visited >= after.inodes_ || current < first_inode_ || current > after.inodes_) return -EUCLEAN;
        uint32_t bit = current - 1;
        if (bit_test(mapping_bits_, bit)) return -EUCLEAN;
        bit_set(mapping_bits_, bit);
        error = after.read_raw_inode(current, raw);
        if (error) return error;
        Inode inode;
        error = after.read_inode(current, inode);
        if (error) return error;
        if (!supported(inode)) return -EOPNOTSUPP;
        current = get32(raw + 0x14);
    }
    // Mutable home images, including their chain links, may be torn during
    // replay. The complete committed afterstate is checked independently.
    if (!active_) return 0;
    auto inode_image = [&](uint32_t number) -> bool
    {
        uint32_t group = (number - 1) / after.inodes_per_group_;
        uint32_t index = (number - 1) % after.inodes_per_group_;
        return tx_contains(groups_[group].inode_table + uint64_t(index) * after.inode_size_ / after.block_size_);
    };
    // Clear members shared by the old and new lists. What remains identifies
    // additions without allocating another set or imposing a small list cap.
    current = old_head;
    unsigned removed = 0;
    for (uint32_t visited = 0; current; ++visited)
    {
        if (visited >= volume_.inodes_ || current < first_inode_ || current > volume_.inodes_) return -EUCLEAN;
        uint8_t before[4096];
        error = volume_.read_raw_inode(current, before);
        if (error) return error;
        uint32_t bit = current - 1;
        if (bit_test(mapping_bits_, bit))
            bit_set(mapping_bits_, bit, false);
        else
        {
            if (!inode_image(current) || ++removed > 1) return -EUCLEAN;
            uint32_t group = bit / after.inodes_per_group_, index = bit % after.inodes_per_group_;
            uint8_t bitmap[4096];
            error = after.fetch(groups_[group].inode_bitmap * after.block_size_, bitmap, after.block_size_);
            if (error) return error;
            uint64_t location = groups_[group].inode_table * after.block_size_ + uint64_t(index) * after.inode_size_;
            error = after.fetch(location, raw, after.inode_size_);
            if (error) return error;
            if (!(bitmap[index / 8] & (1U << (index % 8))))
            {
                if (get16(raw + 0x1a) || !get32(raw + 0x14) || get32(raw + 4) || get32(raw + 0x6c) ||
                    get32(raw + 0x1c) || get16(raw + 0x74) || get32(raw + 0x68) || get16(raw + 0x76))
                    return -EUCLEAN;
            }
            else
            {
                Inode finished;
                error = after.read_inode(current, finished);
                if (error) return error;
                if ((finished.mode & 0170000) != 0100000 || !finished.links || get32(raw + 0x14)) return -EUCLEAN;
                uint64_t end;
                error = inode_mapping_end(after, finished, end);
                if (error) return error;
                if (end > (finished.size + after.block_size_ - 1) / after.block_size_) return -EUCLEAN;
            }
        }
        current = get32(before + 0x14);
    }
    current = head;
    unsigned added = 0;
    for (uint32_t visited = 0; current; ++visited)
    {
        if (visited >= after.inodes_) return -EUCLEAN;
        error = after.read_raw_inode(current, raw);
        if (error) return error;
        uint32_t bit = current - 1;
        if (bit_test(mapping_bits_, bit))
        {
            if (++added > 1 || removed || current != head || get32(raw + 0x14) != old_head || !inode_image(current))
                return -EUCLEAN;
            uint8_t before[4096];
            error = volume_.read_raw_inode(current, before);
            if (error) return error;
            uint16_t old_links = get16(before + 0x1a), links = get16(raw + 0x1a);
            uint16_t type = get16(raw) & 0170000;
            uint64_t old_size = get32(before + 4) | uint64_t(get32(before + 0x6c)) << 32;
            uint64_t size = get32(raw + 4) | uint64_t(get32(raw + 0x6c)) << 32;
            if (!old_links || size > old_size || get32(raw + 0x64) != get32(before + 0x64) ||
                type != (get16(before) & 0170000) || (links && links != old_links) ||
                (!links && old_links != (type == 0040000 ? 2 : 1))) return -EUCLEAN;
            for (unsigned i = 0; i < 60; ++i)
                if (raw[0x28 + i] != before[0x28 + i]) return -EUCLEAN;
        }
        current = get32(raw + 0x14);
    }
    return 0;
}

int WriteSession::preflight_inode_mapping(const Inode &inode)
{
    bit_clear(mapping_bits_);
    bit_clear(xattr_bits_);
    if (inode.xattr_block && !bit_set(xattr_bits_, inode.xattr_block)) return stop(-EUCLEAN);
    bitmap_group_ = 0xffffffffU;
    uint64_t allocated = 0;
    int error = validate_inode_mapping(volume_, inode, allocated);
    if (error) return stop(error);
    if ((allocated + (inode.xattr_block ? 1 : 0)) * (volume_.block_size_ / 512) != inode.blocks_512)
        return stop(-EUCLEAN);
    return 0;
}

int WriteSession::validate_orphan_references(const Volume &after)
{
    uint8_t encoded_head[4];
    int error = after.fetch(1024 + 0xe8, encoded_head, sizeof(encoded_head));
    if (error || !get32(encoded_head)) return error;
    uint32_t current = get32(encoded_head);
    bool deleting = false;
    for (uint32_t visited = 0; current; ++visited)
    {
        if (visited >= after.inodes_) return -EUCLEAN;
        uint8_t raw[4096];
        error = after.read_raw_inode(current, raw);
        if (error) return error;
        if (!get16(raw + 0x1a)) { deleting = true; break; }
        current = get32(raw + 0x14);
    }
    if (!deleting) return 0;
    // Recovery must not reclaim a zero-link inode still named by an ordinary
    // directory. Scan once at mount/replay, rather than adding an inode-count
    // dependent scan to every normal unlink. Unlinked directories retain only
    // their FD-visible dot records and are outside the named namespace.
    uint8_t *descriptor = descriptor_cache_;
    uint8_t bits[4096], image[4096];
    for (uint32_t group = 0; group < after.groups_; ++group)
    {
        error = after.read_group(group, descriptor);
        if (error) return error;
        if (get16(descriptor + 0x12) & 1U) continue;
        error = after.fetch(groups_[group].inode_bitmap * after.block_size_, bits, after.block_size_);
        if (error) return error;
        uint64_t group_first = uint64_t(group) * after.inodes_per_group_;
        uint32_t count = group_first >= after.inodes_ ? 0 : uint32_t(after.inodes_ - group_first);
        if (count > after.inodes_per_group_) count = after.inodes_per_group_;
        uint32_t per_block = after.block_size_ / after.inode_size_;
        for (uint32_t first = 0; first < count; first += per_block)
        {
            bool used = false;
            for (uint32_t index = first; index < count && index - first < per_block; ++index)
                used |= (bits[index / 8] & (1U << (index % 8))) != 0;
            if (!used) continue;
            error = after.fetch(groups_[group].inode_table * after.block_size_ + uint64_t(first) * after.inode_size_,
                                image, after.block_size_);
            if (error) return error;
            for (uint32_t index = first; index < count && index - first < per_block; ++index)
            {
                if (!(bits[index / 8] & (1U << (index % 8)))) continue;
                uint32_t number = uint32_t(group_first + index + 1);
                if (number < first_inode_ && number != 2) continue;
                const uint8_t *raw = image + (index - first) * after.inode_size_;
                if ((get16(raw) & 0170000) != 0040000) continue;
                Inode directory;
                error = after.read_inode(number, directory);
                if (error) return error;
                if (!directory.links) continue;
                error = after.list_directory(directory,
                    [](void *context, const char *, uint32_t child, uint8_t) -> int
                    {
                        Inode target;
                        int result = static_cast<const Volume *>(context)->read_inode(child, target);
                        return result ? result : (!target.links ? -EUCLEAN : 0);
                    }, const_cast<Volume *>(&after));
                if (error) return error;
            }
        }
    }
    return 0;
}

int WriteSession::begin_orphan_truncate(uint32_t number, uint64_t size, uint64_t seconds)
{
    Inode inode;
    int error = volume_.read_inode(number, inode);
    if (error) return stop(error);
    if ((inode.mode & 0170000) != 0100000 || size >= inode.size || !supported(inode)) return -EOPNOTSUPP;
    error = preflight_inode_mapping(inode);
    if (error) return error;
    uint32_t previous, next;
    bool pending;
    error = find_orphan(number, previous, next, pending);
    if (error) return stop(error);
    error = tx_begin();
    uint8_t *raw = nullptr;
    if (!error) error = tx_inode(number, raw);
    if (!error && size % volume_.block_size_)
    {
        uint64_t physical;
        error = tx_map(raw, uint32_t(size / volume_.block_size_), physical);
        if (!error && physical)
        {
            uint8_t *bytes;
            error = tx_edit(physical, bytes);
            if (!error)
                for (uint32_t i = uint32_t(size % volume_.block_size_); i < volume_.block_size_; ++i) bytes[i] = 0;
        }
    }
    if (!error && !pending) error = tx_add_orphan(number, raw);
    if (!error)
    {
        set32(raw + 4, uint32_t(size)); set32(raw + 0x6c, uint32_t(size >> 32));
        error = tx_finish_inode(number, raw, seconds);
    }
    if (!error) error = tx_commit();
    else tx_cancel();
    if (error) return stop(error);
    return clean_orphan(number, false);
}

int WriteSession::clean_orphan(uint32_t number, bool release_inode)
{
    Inode inode;
    int error = volume_.read_inode(number, inode);
    if (error) return stop(error);
    if (!supported(inode) || (release_inode && inode.links)) return -EOPNOTSUPP;
    error = preflight_inode_mapping(inode);
    if (error) return error;
    uint64_t limit = release_inode ? 0 : (inode.size + volume_.block_size_ - 1) / volume_.block_size_;
    bool fast_symlink = (inode.mode & 0170000) == 0120000 && !(inode.flags & 0x80000U) && inode.size <= 60 &&
                        inode.blocks_512 == (inode.xattr_block ? volume_.block_size_ / 512 : 0);
    if (release_inode && inode.size && !fast_symlink)
    {
        error = tx_begin();
        uint8_t *raw = nullptr;
        if (!error) error = tx_inode(number, raw);
        if (!error)
        {
            set32(raw + 4, 0); set32(raw + 0x6c, 0);
            tx_seal_inode(number, raw);
            error = tx_commit();
        }
        else tx_cancel();
        if (error) return stop(error);
    }
    unsigned budget = 64;
    for (;;)
    {
        error = tx_begin();
        uint8_t *raw = nullptr;
        if (!error) error = tx_inode(number, raw);
        bool finished = false;
        unsigned removed = 0;
        if (!error && fast_symlink)
        {
            error = tx_resize(number, raw, 0);
            finished = !error;
        }
        for (; !error && !fast_symlink; ++removed)
        {
            uint32_t logical = 0;
            bool found = false;
            error = tx_mapping_last(raw, logical, found);
            if (error) break;
            if (!found || logical < limit) { finished = true; break; }
            if (removed == budget) break;
            error = tx_mapping_remove_last(raw, logical);
        }
        // Inode/xattr release is a separate final transaction when blocks
        // were removed, keeping the minimum cleanup step within 16 images.
        bool finalize = finished && (!release_inode || !removed);
        if (!error && finalize && (release_inode || inode.links)) error = tx_remove_orphan(number, raw);
        if (!error && finalize && release_inode) error = tx_release_inode(number, raw);
        if (!error)
        {
            if (!(finalize && release_inode)) tx_seal_inode(number, raw);
            error = tx_commit();
        }
        else tx_cancel();
        if (error == -E2BIG && budget > 1) { budget /= 2; continue; }
        if (error) return stop(error);
        if (finalize) return 0;
        budget = 64;
    }
}

int WriteSession::recover_orphans()
{
    for (;;)
    {
        uint32_t current = get32(super_ + 0xe8), chosen = 0;
        for (uint32_t visited = 0; current; ++visited)
        {
            if (visited >= volume_.inodes_ || current < first_inode_ || current > volume_.inodes_) return stop(-EUCLEAN);
            uint8_t raw[4096];
            int error = volume_.read_raw_inode(current, raw);
            if (error) return stop(error);
            if (!orphan_held(current)) { chosen = current; break; }
            current = get32(raw + 0x14);
        }
        if (!chosen) return 0;
        Inode inode;
        int error = volume_.read_inode(chosen, inode);
        if (error) return stop(error);
        error = clean_orphan(chosen, !inode.links);
        if (error) return error;
    }
}

int WriteSession::reclaim_orphan(uint32_t number)
{
    if (!active_) return -EINVAL;
    if (failed_ || journal_.failed()) return stop(-EIO);
    uint32_t previous, next;
    bool pending;
    int error = find_orphan(number, previous, next, pending);
    if (error) return stop(error);
    if (!pending) return orphan_held(number) ? stop(-EUCLEAN) : 0;
    Inode inode;
    error = volume_.read_inode(number, inode);
    if (error) return stop(error);
    if (inode.links) return 0;
    hold_orphan(number, false);
    return clean_orphan(number, true);
}

} // namespace xjext4
