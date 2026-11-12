// SPDX-License-Identifier: Apache-2.0
// Original directory mutations, derived from documented on-disk field facts.
// Each namespace operation builds one bounded JBD2 transaction before any write.
#include "write.h"
#include <errno.h>

namespace xjext4 {
namespace {

constexpr uint16_t kRegular = 0100000;
constexpr uint16_t kDirectory = 0040000;
constexpr uint16_t kSymlink = 0120000;
constexpr uint32_t kMaximumDirectoryBlocks = 128;

uint16_t get16(const uint8_t *p)
{
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}

uint32_t get32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void set16(uint8_t *p, uint16_t value)
{
    p[0] = uint8_t(value);
    p[1] = uint8_t(value >> 8);
}

void set32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
}

void clear(void *output, Size bytes)
{
    for (Size i = 0; i < bytes; ++i) static_cast<uint8_t *>(output)[i] = 0;
}

bool same_name(const char *left, const char *right)
{
    for (Size i = 0;; ++i)
    {
        if (left[i] != right[i]) return false;
        if (!left[i]) return true;
    }
}

int name_length(const char *name)
{
    if (!name || !*name) return -EINVAL;
    for (unsigned i = 0; i <= 255; ++i)
    {
        if (!name[i])
        {
            if (same_name(name, ".") || same_name(name, "..")) return -EINVAL;
            return int(i);
        }
        if (name[i] == '/') return -EINVAL;
    }
    return -ENAMETOOLONG;
}

uint16_t record_size(uint16_t name)
{
    return uint16_t((8U + name + 3U) & ~3U);
}

uint8_t entry_type(uint16_t mode)
{
    return (mode & 0170000) == kDirectory ? 2 : (mode & 0170000) == kSymlink ? 7 : 1;
}

} // namespace

int WriteSession::link(uint32_t parent, const char *name, uint32_t target, uint64_t seconds)
{
    int length = name_length(name);
    if (length < 0) return length;
    if (target < first_inode_) return -EPERM;
    int error = tx_begin();
    if (error) return error;
    uint8_t *directory = nullptr, *raw = nullptr;
    error = ns_directory(parent, directory);
    if (!error) error = tx_inode(target, raw);
    if (!error && (get16(raw) & 0170000) == kDirectory) error = -EPERM;
    if (!error && (get16(raw) & 0170000) != kRegular && (get16(raw) & 0170000) != kSymlink)
        error = -EOPNOTSUPP;
    if (!error && (get32(raw + 0x20) & ~(0x80000U | 0xc8U | 0x10000U))) error = -EOPNOTSUPP;
    if (!error && (!get16(raw + 0x1a) || get16(raw + 0x1a) >= 65000)) error = -EMLINK;
    DirectoryPosition existing = {};
    if (!error)
    {
        error = ns_find(parent, directory, name, existing);
        if (!error) error = -EEXIST;
        else if (error == -ENOENT) error = 0;
    }
    if (!error) error = ns_insert(parent, directory, name, target, entry_type(get16(raw)));
    if (!error)
    {
        set16(raw + 0x1a, uint16_t(get16(raw + 0x1a) + 1));
        error = tx_finish_inode(target, raw, seconds, false);
    }
    if (!error) error = tx_finish_inode(parent, directory, seconds);
    if (!error) return tx_commit();
    tx_cancel();
    return stop(error);
}

int WriteSession::symlink(uint32_t parent, const char *name, const char *target, uint32_t uid, uint32_t gid,
                          uint64_t seconds, Inode &created)
{
    int length = name_length(name);
    if (length < 0) return length;
    if (!target) return -EINVAL;
    Size bytes = 0;
    while (bytes < 4096 && target[bytes]) ++bytes;
    // Linux reads mapped symlink targets from one NUL-terminated data block.
    // Reject longer text before allocating an inode or publishing a name.
    if (!bytes || bytes >= volume_.block_size_) return -ENAMETOOLONG;
    int error = tx_begin();
    if (error) return error;
    uint8_t *directory = nullptr, *raw = nullptr;
    error = ns_directory(parent, directory);
    DirectoryPosition existing = {};
    if (!error)
    {
        error = ns_find(parent, directory, name, existing);
        if (!error) error = -EEXIST;
        else if (error == -ENOENT) error = 0;
    }
    if (!error && (get16(directory) & 02000))
        gid = get16(directory + 0x18) | uint32_t(get16(directory + 0x7a)) << 16;
    uint32_t number = 0;
    if (!error) error = tx_allocate_inode(kSymlink | 0777, uid, gid, seconds, number, raw);
    if (!error && bytes < 60)
    {
        set32(raw + 0x20, 0);
        clear(raw + 0x28, 60);
        for (Size i = 0; i < bytes; ++i) raw[0x28 + i] = uint8_t(target[i]);
        set32(raw + 4, uint32_t(bytes));
    }
    else if (!error)
    {
        // A new classic-map inode with no sectors is also the fast-link
        // representation. Allocate its data block before resize so the latter
        // can distinguish this intentional transition from resizing an inline
        // target as though its bytes were block pointers.
        uint64_t first = 0;
        if (!(get32(raw + 0x20) & 0x80000U)) error = tx_map(raw, 0, first, true);
        if (!error) error = tx_resize(number, raw, bytes);
        for (Size position = 0; !error && position < bytes;)
        {
            uint64_t physical = 0;
            error = tx_map(raw, uint32_t(position / volume_.block_size_), physical, true);
            uint8_t *block = nullptr;
            if (!error) error = tx_edit(physical, block);
            Size amount = bytes - position;
            if (amount > volume_.block_size_) amount = volume_.block_size_;
            if (!error)
                for (Size i = 0; i < amount; ++i) block[i] = uint8_t(target[position + i]);
            position += amount;
        }
    }
    if (!error) error = tx_finish_inode(number, raw, seconds);
    if (!error) error = ns_insert(parent, directory, name, number, 7);
    if (!error) error = tx_finish_inode(parent, directory, seconds);
    if (!error) error = tx_commit();
    else tx_cancel();
    if (!error) error = volume_.read_inode(number, created);
    return error ? stop(error) : 0;
}

int WriteSession::create(uint32_t parent, const char *name, uint16_t mode, uint32_t uid, uint32_t gid,
                         uint64_t seconds, Inode &result)
{
    int length = name_length(name);
    if (length < 0) return length;
    if (seconds > 0x37fffffffULL) return -EOVERFLOW;
    uint16_t type = mode & 0170000;
    if (!type) { type = kRegular; mode |= kRegular; }
    if (type != kRegular && type != kDirectory) return -EOPNOTSUPP;
    int error = tx_begin();
    if (error) return error;
    uint8_t *directory = nullptr;
    error = ns_directory(parent, directory);
    DirectoryPosition existing = {};
    if (!error)
    {
        error = ns_find(parent, directory, name, existing);
        if (!error) error = -EEXIST;
        else if (error == -ENOENT) error = 0;
    }
    if (!error && type == kDirectory && get16(directory + 0x1a) >= 65000) error = -EMLINK;
    if (!error && (get16(directory) & 02000))
    {
        gid = get16(directory + 0x18) | uint32_t(get16(directory + 0x7a)) << 16;
        if (type == kDirectory) mode |= 02000;
    }
    uint32_t number = 0;
    uint8_t *raw = nullptr;
    if (!error) error = tx_allocate_inode(mode, uid, gid, seconds, number, raw);
    if (!error && type == kDirectory)
    {
        uint64_t physical = 0;
        error = tx_map(raw, 0, physical, true);
        if (!error) error = tx_resize(number, raw, volume_.block_size_);
        uint8_t *block = nullptr;
        if (!error) error = tx_edit(physical, block, true);
        if (!error)
        {
            uint32_t limit = volume_.block_size_ - ((volume_.readonly_ & 0x400U) ? 12U : 0U);
            set32(block, number);
            set16(block + 4, 12);
            set32(block + 12, parent);
            set16(block + 16, uint16_t(limit - 12));
            if (volume_.incompat_ & 2U)
            {
                block[6] = 1; block[7] = 2;
                block[18] = 2; block[19] = 2;
            }
            else { set16(block + 6, 1); set16(block + 18, 2); }
            block[8] = '.';
            block[20] = block[21] = '.';
            set16(raw + 0x1a, 2);
            ns_seal_block(number, raw, block);
            set16(directory + 0x1a, uint16_t(get16(directory + 0x1a) + 1));
        }
    }
    if (!error) error = ns_insert(parent, directory, name, number, entry_type(mode));
    if (!error) error = tx_finish_inode(number, raw, seconds);
    if (!error) error = tx_finish_inode(parent, directory, seconds);
    if (!error) error = tx_commit();
    else { tx_cancel(); return stop(error); }
    if (!error) error = volume_.read_inode(number, result);
    return stop(error);
}

int WriteSession::unlink(uint32_t parent, const char *name, uint64_t seconds, bool defer_cleanup)
{
    int length = name_length(name);
    if (length < 0) return length;
    if (seconds > 0x37fffffffULL) return -EOVERFLOW;
    int error = tx_begin();
    if (error) return error;
    uint8_t *directory = nullptr;
    error = ns_directory(parent, directory);
    DirectoryPosition position = {};
    if (!error) error = ns_find(parent, directory, name, position);
    if (!error && position.number < first_inode_) error = -EPERM;
    uint8_t *raw = nullptr;
    if (!error) error = tx_inode(position.number, raw);
    uint16_t type = !error ? get16(raw) & 0170000 : 0;
    if (!error && type != kRegular && type != kDirectory && type != kSymlink) error = -EOPNOTSUPP;
    if (!error && type == kDirectory)
    {
        error = ns_directory(position.number, raw);
        if (!error) error = ns_empty(position.number, raw, parent);
        if (!error && (get16(raw + 0x1a) != 2 || get16(directory + 0x1a) < 3)) error = -EUCLEAN;
    }
    if (!error && (get32(raw + 0x20) & ~(0x80000U | 0xc8U | 0x10000U | (type == kDirectory ? 0x1000U : 0U))))
        error = -EOPNOTSUPP;
    bool release = !error && (type == kDirectory || get16(raw + 0x1a) == 1);
    if (!error && release)
    {
        Inode inode;
        error = volume_.read_inode(position.number, inode);
        if (!error) error = preflight_inode_mapping(inode);
    }
    if (!error) error = ns_remove(parent, directory, position);
    if (!error && release)
    {
        set16(raw + 0x1a, 0);
        error = tx_add_orphan(position.number, raw);
        if (!error) error = tx_finish_inode(position.number, raw, seconds, false);
        if (!error && type == kDirectory)
            set16(directory + 0x1a, uint16_t(get16(directory + 0x1a) - 1));
    }
    else if (!error)
    {
        if (!get16(raw + 0x1a)) error = -EUCLEAN;
        else set16(raw + 0x1a, uint16_t(get16(raw + 0x1a) - 1));
        if (!error) error = tx_finish_inode(position.number, raw, seconds, false);
    }
    if (!error) error = tx_finish_inode(parent, directory, seconds);
    if (!error) error = tx_commit();
    else { tx_cancel(); return stop(error); }
    if (error) return stop(error);
    if (!release) return 0;
    if (defer_cleanup) { hold_orphan(position.number, true); return 0; }
    return reclaim_orphan(position.number);
}

int WriteSession::rename(uint32_t source_parent, const char *source, uint32_t target_parent, const char *target,
                         uint64_t seconds, bool defer_target_cleanup)
{
    int source_length = name_length(source), target_length = name_length(target);
    if (source_length < 0) return source_length;
    if (target_length < 0) return target_length;
    if (seconds > 0x37fffffffULL) return -EOVERFLOW;
    int error = tx_begin();
    if (error) return error;
    uint8_t *from = nullptr, *to = nullptr;
    error = ns_directory(source_parent, from);
    if (!error) error = ns_directory(target_parent, to);
    DirectoryPosition source_position = {}, target_position = {};
    if (!error) error = ns_find(source_parent, from, source, source_position);
    if (!error && source_position.number < first_inode_) error = -EPERM;
    bool target_exists = false;
    if (!error)
    {
        error = ns_find(target_parent, to, target, target_position);
        if (!error) target_exists = true;
        else if (error == -ENOENT) error = 0;
    }
    if (!error && target_exists && target_position.number == source_position.number)
    {
        tx_cancel();
        return 0; // Renaming two hard links of the same inode is a no-op.
    }
    uint8_t *moving = nullptr, *replaced = nullptr;
    if (!error) error = tx_inode(source_position.number, moving);
    uint16_t type = !error ? get16(moving) & 0170000 : 0;
    if (!error && type != kRegular && type != kDirectory && type != kSymlink) error = -EOPNOTSUPP;
    if (!error && (get32(moving + 0x20) & ~(0x80000U | 0xc8U | 0x10000U | (type == kDirectory ? 0x1000U : 0U))))
        error = -EOPNOTSUPP;
    if (!error && type == kDirectory) error = ns_directory(source_position.number, moving);
    if (!error && type == kDirectory && source_parent != target_parent)
    {
        // Follow verified dotdot records before any directory edit. This also
        // bounds a malformed ancestor cycle and prevents moving into oneself.
        uint32_t ancestor = target_parent;
        for (unsigned depth = 0;; ++depth)
        {
            if (ancestor == source_position.number) { error = -EINVAL; break; }
            if (ancestor == 2) break;
            if (depth == kMaximumDirectoryBlocks) { error = -ELOOP; break; }
            uint8_t *ancestor_raw = nullptr;
            error = ns_directory(ancestor, ancestor_raw);
            DirectoryPosition parent_position = {};
            if (!error) error = ns_find(ancestor, ancestor_raw, "..", parent_position);
            if (error) break;
            ancestor = parent_position.number;
        }
        if (!error && (get16(from + 0x1a) < 3 || get16(to + 0x1a) >= 65000)) error = -EMLINK;
    }
    uint16_t replaced_type = 0;
    bool release_target = false;
    bool renamed_in_place = false;
    if (!error && !target_exists && source_parent == target_parent && !(get32(from + 0x20) & 0x1000U))
    {
        uint8_t *block = nullptr;
        error = tx_edit(source_position.block, block);
        if (!error)
        {
            uint8_t *entry = block + source_position.offset;
            uint16_t record = get16(entry + 4);
            if (record_size(uint16_t(target_length)) <= record)
            {
                // Reuse the existing record before considering allocation. A
                // shorter rename must still succeed in a full directory/disk.
                clear(entry + 8, record - 8);
                if (volume_.incompat_ & 2U)
                {
                    entry[6] = uint8_t(target_length);
                    entry[7] = entry_type(get16(moving));
                }
                else set16(entry + 6, uint16_t(target_length));
                for (int i = 0; i < target_length; ++i) entry[8 + i] = uint8_t(target[i]);
                ns_seal_block(source_parent, from, block);
                renamed_in_place = true;
            }
        }
    }
    if (!error && target_exists)
    {
        if (target_position.number < first_inode_) error = -EPERM;
        if (!error) error = tx_inode(target_position.number, replaced);
        if (!error) replaced_type = get16(replaced) & 0170000;
        if (!error && (type == kDirectory) != (replaced_type == kDirectory))
            error = type == kDirectory ? -ENOTDIR : -EISDIR;
        if (!error && replaced_type != kRegular && replaced_type != kDirectory && replaced_type != kSymlink)
            error = -EOPNOTSUPP;
        if (!error && (get32(replaced + 0x20) &
            ~(0x80000U | 0xc8U | 0x10000U | (replaced_type == kDirectory ? 0x1000U : 0U)))) error = -EOPNOTSUPP;
        if (!error && replaced_type == kDirectory)
        {
            error = ns_directory(target_position.number, replaced);
            if (!error) error = ns_empty(target_position.number, replaced, target_parent);
            if (!error && (get16(replaced + 0x1a) != 2 || get16(to + 0x1a) < 3)) error = -EUCLEAN;
        }
        release_target = !error && (replaced_type == kDirectory || get16(replaced + 0x1a) == 1);
        if (!error && release_target)
        {
            Inode inode;
            error = volume_.read_inode(target_position.number, inode);
            if (!error) error = preflight_inode_mapping(inode);
        }
    }
    if (!error && target_exists)
    {
        uint8_t *block = nullptr;
        error = tx_edit(target_position.block, block);
        if (!error)
        {
            set32(block + target_position.offset, source_position.number);
            if (volume_.incompat_ & 2U) block[target_position.offset + 7] = entry_type(get16(moving));
            ns_seal_block(target_parent, to, block);
        }
    }
    else if (!error && !renamed_in_place)
        error = ns_insert(target_parent, to, target, source_position.number, entry_type(get16(moving)));
    // Insertion can split the preceding record in this same directory block.
    // Refresh the location so removing the source merges the correct neighbor.
    if (!error && !renamed_in_place) error = ns_find(source_parent, from, source, source_position);
    if (!error && !renamed_in_place) error = ns_remove(source_parent, from, source_position);
    if (!error && target_exists)
    {
        if (release_target)
        {
            set16(replaced + 0x1a, 0);
            error = tx_add_orphan(target_position.number, replaced);
            if (!error) error = tx_finish_inode(target_position.number, replaced, seconds, false);
            if (!error && replaced_type == kDirectory)
                set16(to + 0x1a, uint16_t(get16(to + 0x1a) - 1));
        }
        else
        {
            if (!get16(replaced + 0x1a)) error = -EUCLEAN;
            else set16(replaced + 0x1a, uint16_t(get16(replaced + 0x1a) - 1));
            if (!error) error = tx_finish_inode(target_position.number, replaced, seconds, false);
        }
    }
    if (!error && type == kDirectory && source_parent != target_parent)
    {
        DirectoryPosition dotdot = {};
        error = ns_find(source_position.number, moving, "..", dotdot);
        if (!error && dotdot.number != source_parent) error = -EUCLEAN;
        uint8_t *block = nullptr;
        if (!error) error = tx_edit(dotdot.block, block);
        if (!error)
        {
            set32(block + dotdot.offset, target_parent);
            ns_seal_block(source_position.number, moving, block);
            set16(from + 0x1a, uint16_t(get16(from + 0x1a) - 1));
            set16(to + 0x1a, uint16_t(get16(to + 0x1a) + 1));
        }
    }
    if (!error) error = tx_finish_inode(source_position.number, moving, seconds, false);
    if (!error) error = tx_finish_inode(source_parent, from, seconds);
    if (!error && target_parent != source_parent) error = tx_finish_inode(target_parent, to, seconds);
    if (!error) error = tx_commit();
    else { tx_cancel(); return stop(error); }
    if (error) return stop(error);
    if (!release_target) return 0;
    if (defer_target_cleanup) { hold_orphan(target_position.number, true); return 0; }
    return reclaim_orphan(target_position.number);
}

} // namespace xjext4
