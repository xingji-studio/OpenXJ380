// SPDX-License-Identifier: Apache-2.0
// Original transactional htree implementation using public storage-format facts.
#include "write.h"
#include <errno.h>

namespace xjext4 {
namespace {

constexpr uint32_t kMaximumDirectoryBlocks = 128;
constexpr uint16_t kDirectory = 0040000;

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
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
}

void zero(void *output, DirectorySize bytes)
{
    for (DirectorySize i = 0; i < bytes; ++i) static_cast<uint8_t *>(output)[i] = 0;
}

void copy(void *output, const void *input, DirectorySize bytes)
{
    for (DirectorySize i = 0; i < bytes; ++i)
        static_cast<uint8_t *>(output)[i] = static_cast<const uint8_t *>(input)[i];
}

uint32_t crc(uint32_t seed, const uint8_t *bytes, DirectorySize length)
{
    for (DirectorySize i = 0; i < length; ++i)
    {
        seed ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) seed = (seed >> 1) ^ ((seed & 1) ? 0x82f63b78U : 0);
    }
    return seed;
}

uint32_t leaf_limit(const DirectoryIndexIO &io)
{
    return io.block_size - (io.metadata_checksum ? 12 : 0);
}

unsigned node_offset(bool root)
{
    return root ? 32 : 8;
}

unsigned node_capacity(const DirectoryIndexIO &io, bool root)
{
    return (io.block_size - node_offset(root) - (io.metadata_checksum ? 8 : 0)) / 8;
}

uint16_t name_size(const DirectoryIndexIO &io, const uint8_t *entry)
{
    return io.file_type ? entry[6] : get16(entry + 6);
}

uint16_t record_size(DirectorySize length)
{
    return uint16_t((8U + length + 3U) & ~3U);
}

bool name_equals(const uint8_t *entry, const char *name, DirectorySize length)
{
    for (DirectorySize i = 0; i < length; ++i)
        if (entry[8 + i] != uint8_t(name[i])) return false;
    return true;
}

int ordinary_name(const char *name, DirectorySize length)
{
    if (!name || !length || length > 255) return -EINVAL;
    if ((length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.')) return -EINVAL;
    for (DirectorySize i = 0; i < length; ++i) if (!name[i] || name[i] == '/') return -EINVAL;
    return 0;
}

int name_length(const char *name)
{
    if (!name) return -EINVAL;
    DirectorySize length = 0;
    while (length <= 255 && name[length]) ++length;
    if (length > 255) return -ENAMETOOLONG;
    int error = ordinary_name(name, length);
    return error ? error : int(length);
}

DirectoryIndexIO basic_format(uint32_t size, uint32_t blocks, uint32_t inode, uint32_t inodes,
                               uint32_t seed, bool checksum, bool typed)
{
    DirectoryIndexIO io = {};
    io.block_size = size;
    io.block_count = blocks;
    io.inode = inode;
    io.inode_count = inodes;
    io.inode_seed = seed;
    io.metadata_checksum = checksum;
    io.file_type = typed;
    return io;
}

uint64_t raw_size(const uint8_t *raw)
{
    return get32(raw + 4) | uint64_t(get32(raw + 0x6c)) << 32;
}

bool root_record(const DirectoryIndexIO &io, const uint8_t *block)
{
    return get32(block) == io.inode && get16(block + 4) == 12 && name_size(io, block) == 1 && block[8] == '.';
}

} // namespace

int directory_leaf_check(const DirectoryIndexIO &io, const uint8_t *block)
{
    uint32_t limit = leaf_limit(io);
    if (io.metadata_checksum)
    {
        const uint8_t *tail = block + limit;
        if (get32(tail) || get16(tail + 4) != 12 || tail[6] || tail[7] != 0xde ||
            crc(io.inode_seed, block, limit) != get32(tail + 8)) return -EUCLEAN;
    }
    for (uint32_t offset = 0; offset < limit;)
    {
        if (limit - offset < 8) return -EUCLEAN;
        const uint8_t *entry = block + offset;
        uint16_t length = get16(entry + 4), name = name_size(io, entry);
        if (length < 8 || length % 4 || length > limit - offset || name > 255 || name > length - 8)
            return -EUCLEAN;
        if (get32(entry))
        {
            if (get32(entry) > io.inode_count || !name || (io.file_type && entry[7] > 7)) return -EUCLEAN;
            for (uint16_t i = 0; i < name; ++i)
                if (!entry[8 + i] || entry[8 + i] == '/') return -EUCLEAN;
        }
        offset += length;
    }
    return 0;
}

void directory_leaf_seal(const DirectoryIndexIO &io, uint8_t *block)
{
    if (!io.metadata_checksum) return;
    uint8_t *tail = block + leaf_limit(io);
    zero(tail, 12);
    set16(tail + 4, 12);
    tail[7] = 0xde;
    set32(tail + 8, crc(io.inode_seed, block, leaf_limit(io)));
}

int directory_node_check(const DirectoryIndexIO &io, bool root, const uint8_t *block)
{
    if (root)
    {
        if (!root_record(io, block) || get16(block + 16) != io.block_size - 12 ||
            name_size(io, block + 12) != 2 || block[20] != '.' || block[21] != '.' ||
            !get32(block + 12) || get32(block + 12) > io.inode_count ||
            (io.file_type && (block[7] != 2 || block[19] != 2)) || get32(block + 24) ||
            block[28] > 2 || block[29] != 8 || block[30] > 2 || block[31] ||
            (block[30] == 2 && !io.large_directory)) return -EUCLEAN;
    }
    else if (get32(block) || get16(block + 4) != io.block_size || get16(block + 6)) return -EUCLEAN;
    unsigned offset = node_offset(root);
    unsigned limit = get16(block + offset), count = get16(block + offset + 2);
    if (limit != node_capacity(io, root) || !count || count > limit) return -EUCLEAN;
    uint32_t previous = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        uint32_t child = get32(block + offset + i * 8 + 4);
        if (!child || child >= io.block_count) return -EUCLEAN;
        if (i)
        {
            uint32_t hash = get32(block + offset + i * 8);
            if (hash < previous) return -EUCLEAN;
            previous = hash;
        }
    }
    if (io.metadata_checksum)
    {
        unsigned tail = offset + limit * 8;
        if (get32(block + tail)) return -EUCLEAN;
        uint32_t sum = crc(io.inode_seed, block, offset + count * 8);
        sum = crc(sum, block + tail, 4);
        const uint8_t zeros[4] = {};
        sum = crc(sum, zeros, 4);
        if (sum != get32(block + tail + 4)) return -EUCLEAN;
    }
    return 0;
}

void directory_node_seal(const DirectoryIndexIO &io, bool root, uint8_t *block)
{
    if (!io.metadata_checksum) return;
    unsigned offset = node_offset(root);
    unsigned tail = offset + unsigned(get16(block + offset)) * 8;
    unsigned count = get16(block + offset + 2);
    if (get16(block + offset) != node_capacity(io, root) || !count || count > get16(block + offset)) return;
    zero(block + tail, 8);
    uint32_t sum = crc(io.inode_seed, block, offset + count * 8);
    set32(block + tail + 4, crc(sum, block + tail, 8));
}

IndexedDirectory::IndexedDirectory(const DirectoryIndexIO &io, DirectoryIndexWorkspace &workspace)
    : io_(io), workspace_(workspace), version_(0), stored_version_(0), depth_(0), ready_(false)
{
}

int IndexedDirectory::claim(uint32_t logical)
{
    if (logical >= io_.block_count || logical >= sizeof(workspace_.visited) * 8) return -EUCLEAN;
    uint8_t mask = uint8_t(1U << (logical % 8));
    if (workspace_.visited[logical / 8] & mask) return -EUCLEAN;
    workspace_.visited[logical / 8] |= mask;
    return 0;
}

int IndexedDirectory::read_node(unsigned level, uint32_t logical)
{
    if (level > 2 || logical >= io_.block_count) return -EUCLEAN;
    int error = io_.read(io_.context, logical, workspace_.nodes[level]);
    return error ? error : directory_node_check(io_, level == 0, workspace_.nodes[level]);
}

int IndexedDirectory::check_leaf(uint32_t logical, uint32_t lower, uint64_t upper)
{
    int error = claim(logical);
    if (!error) error = io_.read(io_.context, logical, workspace_.leaf);
    if (!error) error = directory_leaf_check(io_, workspace_.leaf);
    if (error) return error;
    for (uint32_t offset = 0; offset < leaf_limit(io_);)
    {
        const uint8_t *entry = workspace_.leaf + offset;
        if (get32(entry))
        {
            uint16_t length = name_size(io_, entry);
            if ((length == 1 && entry[8] == '.') || (length == 2 && entry[8] == '.' && entry[9] == '.'))
                return -EUCLEAN;
            DirectoryHash hash;
            error = directory_hash(version_, io_.hash_seed, reinterpret_cast<const char *>(entry + 8), length, hash);
            if (error) return error;
            if (hash.major < (lower & ~1U)) return -EUCLEAN;
            if (upper < (1ULL << 32) &&
                (hash.major > (uint32_t(upper) & ~1U) ||
                 (hash.major == (uint32_t(upper) & ~1U) && !(upper & 1U)))) return -EUCLEAN;
        }
        offset += get16(entry + 4);
    }
    return 0;
}

int IndexedDirectory::walk_index(unsigned level, uint32_t logical, unsigned remaining,
                                 uint32_t lower, uint64_t upper)
{
    int error = claim(logical);
    if (!error) error = read_node(level, logical);
    if (error) return error;
    const uint8_t *node = workspace_.nodes[level];
    unsigned offset = node_offset(level == 0), count = get16(node + offset + 2);
    for (unsigned i = 0; i < count; ++i)
    {
        uint32_t first = i ? get32(node + offset + i * 8) : lower;
        uint64_t end = i + 1 < count ? get32(node + offset + (i + 1) * 8) : upper;
        if (first < lower || uint64_t(first) > end || end > upper) return -EUCLEAN;
        uint32_t child = get32(node + offset + i * 8 + 4);
        error = remaining ? walk_index(level + 1, child, remaining - 1, first, end) : check_leaf(child, first, end);
        if (error) return error;
    }
    return 0;
}

int IndexedDirectory::validate()
{
    ready_ = false;
    if (!io_.read || (io_.block_size != 1024 && io_.block_size != 2048 && io_.block_size != 4096) ||
        io_.block_count < 2 || io_.block_count > sizeof(workspace_.visited) * 8 ||
        !io_.inode || io_.inode > io_.inode_count) return -EUCLEAN;
    zero(workspace_.visited, (io_.block_count + 7) / 8);
    int error = read_node(0, 0);
    if (error) return error;
    stored_version_ = workspace_.nodes[0][28];
    version_ = uint8_t(stored_version_ + (stored_version_ <= 2 && io_.unsigned_hash ? 3 : 0));
    depth_ = workspace_.nodes[0][30];
    if ((io_.inode == 2 && get32(workspace_.nodes[0] + 12) != 2) ||
        (io_.inode != 2 && get32(workspace_.nodes[0] + 12) == io_.inode)) return -EUCLEAN;
    error = walk_index(0, 0, depth_, 0, 1ULL << 32);
    if (error) return error;
    for (uint32_t logical = 0; logical < io_.block_count; ++logical)
        if (!(workspace_.visited[logical / 8] & (1U << (logical % 8)))) return -EUCLEAN;
    ready_ = true;
    return 0;
}

int IndexedDirectory::probe(uint32_t hash, uint32_t &leaf)
{
    uint32_t logical = 0, lower = 0;
    uint64_t upper = 1ULL << 32;
    for (unsigned level = 0; level <= depth_; ++level)
    {
        int error = read_node(level, logical);
        if (error) return error;
        const uint8_t *node = workspace_.nodes[level];
        unsigned offset = node_offset(level == 0), count = get16(node + offset + 2), selected = 0;
        for (unsigned i = 1; i < count; ++i)
        {
            if (get32(node + offset + i * 8) > hash) break;
            selected = i;
        }
        path_[level] = {logical, uint16_t(selected), uint16_t(count), lower, upper};
        logical = get32(node + offset + selected * 8 + 4);
        if (selected) lower = get32(node + offset + selected * 8);
        if (selected + 1 < count) upper = get32(node + offset + (selected + 1) * 8);
    }
    leaf = logical;
    return 0;
}

int IndexedDirectory::next_leaf(uint32_t &leaf, uint32_t &boundary)
{
    for (int level = depth_; level >= 0; --level)
    {
        Frame &frame = path_[level];
        if (unsigned(frame.slot) + 1 >= frame.count) continue;
        int error = read_node(unsigned(level), frame.logical);
        if (error) return error;
        unsigned offset = node_offset(level == 0);
        ++frame.slot;
        const uint8_t *node = workspace_.nodes[level];
        boundary = get32(node + offset + unsigned(frame.slot) * 8);
        uint32_t logical = get32(node + offset + unsigned(frame.slot) * 8 + 4);
        uint32_t lower = boundary;
        uint64_t upper = unsigned(frame.slot) + 1 < frame.count ?
                         get32(node + offset + (unsigned(frame.slot) + 1) * 8) : frame.upper;
        for (unsigned child_level = unsigned(level) + 1; child_level <= depth_; ++child_level)
        {
            error = read_node(child_level, logical);
            if (error) return error;
            node = workspace_.nodes[child_level];
            unsigned count = get16(node + 10);
            path_[child_level] = {logical, 0, uint16_t(count), lower, upper};
            logical = get32(node + 12);
            if (count > 1) upper = get32(node + 16);
        }
        leaf = logical;
        return 0;
    }
    return -ENOENT;
}

int IndexedDirectory::find(const char *name, DirectorySize length, DirectoryIndexPosition &position)
{
    int error = ready_ ? 0 : validate();
    if (error) return error;
    if (!name || !length || length > 255) return -EINVAL;
    if ((length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.'))
    {
        error = read_node(0, 0);
        if (error) return error;
        unsigned offset = length == 1 ? 0 : 12;
        position = {0, get32(workspace_.nodes[0] + offset), uint16_t(offset),
                    uint16_t(length == 1 ? 0xffff : 0), uint8_t(io_.file_type ? 2 : 0)};
        return 0;
    }
    error = ordinary_name(name, length);
    DirectoryHash hash;
    if (!error) error = directory_hash(version_, io_.hash_seed, name, length, hash);
    uint32_t logical = 0;
    if (!error) error = probe(hash.major, logical);
    if (error) return error;
    bool found = false;
    for (;;)
    {
        error = io_.read(io_.context, logical, workspace_.leaf);
        if (error) return error;
        uint16_t previous = 0xffff;
        for (uint32_t offset = 0; offset < leaf_limit(io_);)
        {
            const uint8_t *entry = workspace_.leaf + offset;
            if (get32(entry) && name_size(io_, entry) == length && name_equals(entry, name, length))
            {
                if (found) return -EUCLEAN;
                position = {logical, get32(entry), uint16_t(offset), previous,
                            uint8_t(io_.file_type ? entry[7] : 0)};
                found = true;
            }
            previous = uint16_t(offset);
            offset += get16(entry + 4);
        }
        uint32_t boundary = 0;
        error = next_leaf(logical, boundary);
        if (error == -ENOENT || (!error && (boundary & ~1U) != hash.major)) break;
        if (error) return error;
    }
    return found ? 0 : -ENOENT;
}

int IndexedDirectory::append(uint32_t &logical, uint8_t *&output)
{
    if (!io_.append || !io_.edit) return -EROFS;
    if (io_.block_count == sizeof(workspace_.visited) * 8) return -EFBIG;
    int error = io_.append(io_.context, logical, output);
    if (error) return error;
    if (logical != io_.block_count) return -EUCLEAN;
    ++io_.block_count;
    return 0;
}

int IndexedDirectory::collect_leaf(const uint8_t *block, bool skip_dots, unsigned &count, uint32_t &bytes)
{
    int error = directory_leaf_check(io_, block);
    if (error) return error;
    count = 0;
    bytes = 0;
    for (uint32_t offset = 0; offset < leaf_limit(io_);)
    {
        const uint8_t *entry = block + offset;
        uint16_t length = name_size(io_, entry);
        bool dot = skip_dots && ((offset == 0 && length == 1 && entry[8] == '.') ||
                                (offset == 12 && length == 2 && entry[8] == '.' && entry[9] == '.'));
        if (get32(entry) && !dot)
        {
            if (count == 342) return -EUCLEAN;
            DirectoryHash hash;
            error = directory_hash(version_, io_.hash_seed, reinterpret_cast<const char *>(entry + 8), length, hash);
            if (error) return error;
            workspace_.entries[count++] = {hash.major, hash.minor, get32(entry), uint16_t(offset), length,
                                            uint8_t(io_.file_type ? entry[7] : 0)};
            bytes += record_size(length);
        }
        offset += get16(entry + 4);
    }
    return 0;
}

int IndexedDirectory::add_entry(const char *name, DirectorySize length, uint32_t inode, uint8_t type,
                                 unsigned &count, uint32_t &bytes)
{
    if (count == 342) return -EUCLEAN;
    DirectoryHash hash;
    int error = directory_hash(version_, io_.hash_seed, name, length, hash);
    if (error) return error;
    workspace_.entries[count++] = {hash.major, hash.minor, inode, 0xffff, uint16_t(length), type};
    bytes += record_size(length);
    return 0;
}

void IndexedDirectory::sort_entries(unsigned count)
{
    for (unsigned i = 1; i < count; ++i)
    {
        auto entry = workspace_.entries[i];
        unsigned destination = i;
        while (destination)
        {
            const auto &previous = workspace_.entries[destination - 1];
            if (previous.hash < entry.hash || (previous.hash == entry.hash && previous.minor <= entry.minor)) break;
            workspace_.entries[destination] = previous;
            --destination;
        }
        workspace_.entries[destination] = entry;
    }
}

void IndexedDirectory::pack_leaf(uint8_t *output, unsigned first, unsigned end, const char *name)
{
    zero(output, io_.block_size);
    uint32_t offset = 0;
    for (unsigned i = first; i < end; ++i)
    {
        const auto &entry = workspace_.entries[i];
        uint8_t *record = output + offset;
        set32(record, entry.inode);
        uint16_t bytes = i + 1 == end ? uint16_t(leaf_limit(io_) - offset) : record_size(entry.length);
        set16(record + 4, bytes);
        if (io_.file_type) { record[6] = uint8_t(entry.length); record[7] = entry.type; }
        else set16(record + 6, entry.length);
        const void *source = entry.offset == 0xffff ? static_cast<const void *>(name) :
                             static_cast<const void *>(workspace_.leaf + entry.offset + 8);
        copy(record + 8, source, entry.length);
        offset += bytes;
    }
    if (first == end) set16(output + 4, uint16_t(leaf_limit(io_)));
    directory_leaf_seal(io_, output);
}

int IndexedDirectory::split_leaf(uint32_t logical, const char *name, DirectorySize length,
                                  uint32_t inode, uint8_t type, uint32_t &new_logical, uint32_t &boundary)
{
    int error = io_.read(io_.context, logical, workspace_.leaf);
    unsigned count = 0;
    uint32_t bytes = 0;
    if (!error) error = collect_leaf(workspace_.leaf, false, count, bytes);
    if (!error) error = add_entry(name, length, inode, type, count, bytes);
    if (error) return error;
    sort_entries(count);
    unsigned middle = 0;
    uint32_t left = 0, score = 0xffffffffU;
    for (unsigned i = 1; i < count; ++i)
    {
        left += record_size(workspace_.entries[i - 1].length);
        if (left > leaf_limit(io_) || bytes - left > leaf_limit(io_)) continue;
        uint32_t distance = left > bytes / 2 ? left - bytes / 2 : bytes / 2 - left;
        if (distance < score) { middle = i; score = distance; }
    }
    if (!middle) return -EUCLEAN;
    boundary = workspace_.entries[middle].hash;
    if (workspace_.entries[middle - 1].hash == boundary) boundary |= 1;
    uint8_t *second = nullptr, *first = nullptr;
    error = append(new_logical, second);
    if (!error) error = io_.edit(io_.context, logical, first);
    if (error) return error;
    pack_leaf(first, 0, middle, name);
    pack_leaf(second, middle, count, name);
    return 0;
}

void IndexedDirectory::pack_node(uint8_t *output, bool root, unsigned first, unsigned end)
{
    uint8_t dots[24];
    if (root) copy(dots, output, sizeof(dots));
    zero(output, io_.block_size);
    if (root)
    {
        copy(output, dots, sizeof(dots));
        set16(output + 16, uint16_t(io_.block_size - 12));
        output[28] = stored_version_;
        output[29] = 8;
        output[30] = depth_;
    }
    else set16(output + 4, uint16_t(io_.block_size));
    unsigned offset = node_offset(root), count = end - first;
    set16(output + offset, uint16_t(node_capacity(io_, root)));
    set16(output + offset + 2, uint16_t(count));
    for (unsigned i = 0; i < count; ++i)
    {
        const auto &entry = workspace_.indices[first + i];
        if (i) set32(output + offset + i * 8, entry.hash);
        set32(output + offset + i * 8 + 4, entry.logical);
    }
    directory_node_seal(io_, root, output);
}

int IndexedDirectory::insert_index(uint32_t logical, uint32_t boundary)
{
    for (int level = depth_; level >= 0; --level)
    {
        int error = read_node(unsigned(level), path_[level].logical);
        if (error) return error;
        const uint8_t *node = workspace_.nodes[level];
        bool root = level == 0;
        unsigned offset = node_offset(root), count = get16(node + offset + 2), insertion = path_[level].slot + 1;
        for (unsigned i = 0, source = 0; i < count + 1; ++i)
        {
            if (i == insertion) workspace_.indices[i] = {boundary, logical};
            else
            {
                workspace_.indices[i] = {source ? get32(node + offset + source * 8) : path_[level].lower,
                                          get32(node + offset + source * 8 + 4)};
                ++source;
            }
        }
        ++count;
        uint8_t *edited = nullptr;
        if (count <= node_capacity(io_, root))
        {
            error = io_.edit(io_.context, path_[level].logical, edited);
            if (!error) pack_node(edited, root, 0, count);
            return error;
        }
        if (root)
        {
            if (depth_ == 2) return -EFBIG;
            uint32_t child = 0;
            uint8_t *promoted = nullptr;
            error = append(child, promoted);
            if (error) return error;
            if (depth_ == 1 && !io_.large_directory)
            {
                if (!io_.enable_large_directory) return -EFBIG;
                error = io_.enable_large_directory(io_.context);
                if (error) return error;
                io_.large_directory = true;
            }
            pack_node(promoted, false, 0, count);
            error = io_.edit(io_.context, 0, edited);
            if (error) return error;
            ++depth_;
            workspace_.indices[0] = {0, child};
            pack_node(edited, true, 0, 1);
            return 0;
        }
        unsigned middle = count / 2;
        uint32_t next = 0;
        uint8_t *split = nullptr;
        error = append(next, split);
        if (!error) error = io_.edit(io_.context, path_[level].logical, edited);
        if (error) return error;
        uint32_t next_boundary = workspace_.indices[middle].hash;
        pack_node(edited, false, 0, middle);
        pack_node(split, false, middle, count);
        logical = next;
        boundary = next_boundary;
    }
    return -EUCLEAN;
}

int IndexedDirectory::insert(const char *name, DirectorySize length, uint32_t inode, uint8_t type)
{
    int error = ordinary_name(name, length);
    if (error) return error;
    if (!inode || inode > io_.inode_count || type > 7) return -EINVAL;
    if (!io_.edit || !io_.append) return -EROFS;
    error = ready_ ? 0 : validate();
    if (error) return error;
    DirectoryIndexPosition position;
    error = find(name, length, position);
    if (!error) return -EEXIST;
    if (error != -ENOENT) return error;
    DirectoryHash hash;
    error = directory_hash(version_, io_.hash_seed, name, length, hash);
    uint32_t logical = 0;
    if (!error) error = probe(hash.major, logical);
    if (error) return error;
    Frame last_path[3];
    uint32_t last = logical;
    for (;;)
    {
        error = io_.read(io_.context, logical, workspace_.leaf);
        unsigned count = 0;
        uint32_t bytes = 0;
        if (!error) error = collect_leaf(workspace_.leaf, false, count, bytes);
        if (error) return error;
        if (bytes + record_size(length) <= leaf_limit(io_))
        {
            error = add_entry(name, length, inode, type, count, bytes);
            if (error) return error;
            sort_entries(count);
            uint8_t *edited = nullptr;
            error = io_.edit(io_.context, logical, edited);
            if (!error) pack_leaf(edited, 0, count, name);
            return error;
        }
        for (unsigned level = 0; level <= depth_; ++level) last_path[level] = path_[level];
        last = logical;
        uint32_t boundary = 0;
        error = next_leaf(logical, boundary);
        if (error == -ENOENT || (!error && (boundary & ~1U) != hash.major)) break;
        if (error) return error;
    }
    for (unsigned level = 0; level <= depth_; ++level) path_[level] = last_path[level];
    uint32_t added = 0, boundary = 0;
    error = split_leaf(last, name, length, inode, type, added, boundary);
    return error ? error : insert_index(added, boundary);
}

int IndexedDirectory::remove(const DirectoryIndexPosition &position)
{
    int error = ready_ ? 0 : validate();
    if (error) return error;
    if (!io_.edit) return -EROFS;
    if (!position.logical || position.logical >= io_.block_count || position.offset >= leaf_limit(io_) ||
        !position.inode || position.inode > io_.inode_count) return -EINVAL;
    uint8_t *block = nullptr;
    error = io_.edit(io_.context, position.logical, block);
    if (!error) error = directory_leaf_check(io_, block);
    if (error) return error;
    uint32_t actual_offset = 0;
    uint16_t actual_previous = 0xffff;
    while (actual_offset < position.offset)
    {
        actual_previous = uint16_t(actual_offset);
        actual_offset += get16(block + actual_offset + 4);
    }
    if (actual_offset != position.offset || actual_previous != position.previous) return -EINVAL;
    uint16_t record = get16(block + position.offset + 4);
    if (get32(block + position.offset) != position.inode) return -EUCLEAN;
    if (position.previous == 0xffff)
    {
        zero(block + position.offset, record);
        set16(block + position.offset + 4, record);
    }
    else
    {
        if (position.previous >= position.offset ||
            unsigned(position.previous) + get16(block + position.previous + 4) != position.offset) return -EUCLEAN;
        set16(block + position.previous + 4, uint16_t(get16(block + position.previous + 4) + record));
        zero(block + position.offset, record);
    }
    directory_leaf_seal(io_, block);
    return 0;
}

int IndexedDirectory::empty(uint32_t expected_parent)
{
    int error = ready_ ? 0 : validate();
    if (error) return error;
    error = read_node(0, 0);
    if (error) return error;
    if (get32(workspace_.nodes[0] + 12) != expected_parent) return -EUCLEAN;
    for (uint32_t logical = 1; logical < io_.block_count; ++logical)
    {
        error = io_.read(io_.context, logical, workspace_.leaf);
        if (error) return error;
        if (!get32(workspace_.leaf) && get16(workspace_.leaf + 4) == io_.block_size &&
            !get16(workspace_.leaf + 6)) continue;
        for (uint32_t offset = 0; offset < leaf_limit(io_); offset += get16(workspace_.leaf + offset + 4))
            if (get32(workspace_.leaf + offset)) return -ENOTEMPTY;
    }
    return 0;
}

int IndexedDirectory::convert_and_insert(const char *name, DirectorySize length, uint32_t inode, uint8_t type)
{
    int error = ordinary_name(name, length);
    if (error) return error;
    if (!io_.edit || !io_.append || io_.block_count != 1 || !inode || inode > io_.inode_count || type > 7)
        return -EINVAL;
    if (io_.default_hash_version > 2) return -EOPNOTSUPP;
    stored_version_ = io_.default_hash_version;
    version_ = uint8_t(stored_version_ + (stored_version_ <= 2 && io_.unsigned_hash ? 3 : 0));
    depth_ = 0;
    error = io_.read(io_.context, 0, workspace_.leaf);
    if (!error) error = directory_leaf_check(io_, workspace_.leaf);
    if (error) return error;
    if (!root_record(io_, workspace_.leaf) || name_size(io_, workspace_.leaf + 12) != 2 ||
        workspace_.leaf[20] != '.' || workspace_.leaf[21] != '.' || !get32(workspace_.leaf + 12) ||
        get32(workspace_.leaf + 12) > io_.inode_count) return -EUCLEAN;
    for (uint32_t offset = 0; offset < leaf_limit(io_); offset += get16(workspace_.leaf + offset + 4))
    {
        const uint8_t *entry = workspace_.leaf + offset;
        if (get32(entry) && name_size(io_, entry) == length && name_equals(entry, name, length)) return -EEXIST;
    }
    uint8_t dots[24];
    copy(dots, workspace_.leaf, sizeof(dots));
    unsigned count = 0;
    uint32_t bytes = 0;
    error = collect_leaf(workspace_.leaf, true, count, bytes);
    if (!error) error = add_entry(name, length, inode, type, count, bytes);
    if (error) return error;
    sort_entries(count);
    uint32_t first_logical = 0, second_logical = 0, boundary = 0;
    uint8_t *first = nullptr, *second = nullptr;
    error = append(first_logical, first);
    if (error) return error;
    unsigned middle = count;
    if (bytes > leaf_limit(io_))
    {
        middle = 0;
        uint32_t left = 0, score = 0xffffffffU;
        for (unsigned i = 1; i < count; ++i)
        {
            left += record_size(workspace_.entries[i - 1].length);
            if (left > leaf_limit(io_) || bytes - left > leaf_limit(io_)) continue;
            uint32_t distance = left > bytes / 2 ? left - bytes / 2 : bytes / 2 - left;
            if (distance < score) { middle = i; score = distance; }
        }
        if (!middle) return -EUCLEAN;
        error = append(second_logical, second);
        if (error) return error;
        boundary = workspace_.entries[middle].hash;
        if (workspace_.entries[middle - 1].hash == boundary) boundary |= 1;
    }
    pack_leaf(first, 0, middle, name);
    if (second) pack_leaf(second, middle, count, name);
    uint8_t *root = nullptr;
    error = io_.edit(io_.context, 0, root);
    if (error) return error;
    copy(root, dots, sizeof(dots));
    workspace_.indices[0] = {0, first_logical};
    if (second) workspace_.indices[1] = {boundary, second_logical};
    pack_node(root, true, 0, second ? 2 : 1);
    ready_ = true;
    return 0;
}

DirectoryIndexIO WriteSession::ns_index_io(uint32_t number, uint8_t *raw, IndexContext &context)
{
    Inode inode = {};
    inode.number = number;
    inode.generation = get32(raw + 0x64);
    DirectoryIndexIO io = basic_format(volume_.block_size_, uint32_t(raw_size(raw) / volume_.block_size_),
                                       number, volume_.inodes_, volume_.inode_seed(inode),
                                       (volume_.readonly_ & 0x400U) != 0, (volume_.incompat_ & 2U) != 0);
    context = {this, raw};
    io.context = &context;
    io.read = [](void *opaque, uint32_t logical, uint8_t *output) -> int
    {
        auto *context = static_cast<IndexContext *>(opaque);
        uint64_t physical = 0;
        int error = context->session->tx_map(context->raw, logical, physical, false);
        if (!error && !physical) error = -EUCLEAN;
        return error ? error : context->session->tx_read(physical, output);
    };
    io.edit = [](void *opaque, uint32_t logical, uint8_t *&output) -> int
    {
        auto *context = static_cast<IndexContext *>(opaque);
        uint64_t physical = 0;
        int error = context->session->tx_map(context->raw, logical, physical, false);
        if (!error && !physical) error = -EUCLEAN;
        return error ? error : context->session->tx_edit(physical, output);
    };
    io.append = [](void *opaque, uint32_t &logical, uint8_t *&output) -> int
    {
        auto *context = static_cast<IndexContext *>(opaque);
        WriteSession *session = context->session;
        uint64_t bytes = raw_size(context->raw);
        if (bytes / session->volume_.block_size_ >= 0xffffffffU) return -EFBIG;
        logical = uint32_t(bytes / session->volume_.block_size_);
        uint64_t physical = 0;
        int error = session->tx_map(context->raw, logical, physical, true);
        if (!error) error = session->tx_resize(session->tx_inode_number(context->raw), context->raw,
                                               bytes + session->volume_.block_size_);
        return error ? error : session->tx_edit(physical, output, true);
    };
    for (unsigned i = 0; i < 4; ++i) io.hash_seed[i] = get32(super_ + 0xec + i * 4);
    io.default_hash_version = super_[0xfc];
    io.unsigned_hash = (get32(super_ + 0x160) & 2U) != 0;
    const uint8_t *current_super = super_;
    for (unsigned i = 0; i < tx_count_; ++i)
        if (tx_blocks_[i] == 1024 / volume_.block_size_)
            current_super = workspace_->blocks[i] + 1024 % volume_.block_size_;
    io.large_directory = (get32(current_super + 0x60) & 0x4000U) != 0;
    io.enable_large_directory = [](void *opaque) -> int
    {
        auto *context = static_cast<IndexContext *>(opaque);
        WriteSession *session = context->session;
        uint8_t *block = nullptr;
        int error = session->tx_edit(1024 / session->volume_.block_size_, block);
        if (!error)
        {
            uint8_t *super = block + 1024 % session->volume_.block_size_;
            set32(super + 0x60, get32(super + 0x60) | 0x4000U);
        }
        return error;
    };
    return io;
}

int WriteSession::ns_check_block(uint32_t number, const uint8_t *raw, uint8_t *block) const
{
    Inode inode = {};
    inode.number = number;
    inode.generation = get32(raw + 0x64);
    DirectoryIndexIO io = basic_format(volume_.block_size_, uint32_t(raw_size(raw) / volume_.block_size_),
                                       number, volume_.inodes_, volume_.inode_seed(inode),
                                       (volume_.readonly_ & 0x400U) != 0, (volume_.incompat_ & 2U) != 0);
    io.large_directory = (volume_.incompat_ & 0x4000U) != 0;
    for (unsigned i = 0; i < tx_count_; ++i)
        if (tx_blocks_[i] == 1024 / volume_.block_size_)
            io.large_directory = (get32(workspace_->blocks[i] + 1024 % volume_.block_size_ + 0x60) & 0x4000U) != 0;
    return (get32(raw + 0x20) & 0x1000U) && root_record(io, block) ?
           directory_node_check(io, true, block) : directory_leaf_check(io, block);
}

void WriteSession::ns_seal_block(uint32_t number, const uint8_t *raw, uint8_t *block) const
{
    Inode inode = {};
    inode.number = number;
    inode.generation = get32(raw + 0x64);
    DirectoryIndexIO io = basic_format(volume_.block_size_, uint32_t(raw_size(raw) / volume_.block_size_),
                                       number, volume_.inodes_, volume_.inode_seed(inode),
                                       (volume_.readonly_ & 0x400U) != 0, (volume_.incompat_ & 2U) != 0);
    if ((get32(raw + 0x20) & 0x1000U) && root_record(io, block)) directory_node_seal(io, true, block);
    else directory_leaf_seal(io, block);
}

int WriteSession::ns_directory(uint32_t number, uint8_t *&raw)
{
    int error = tx_inode(number, raw);
    if (error) return error;
    if ((get16(raw) & 0170000) != kDirectory) return -ENOTDIR;
    if (get32(raw + 0x20) & ~(0x80000U | 0xc8U | 0x10000U | 0x1000U)) return -EOPNOTSUPP;
    if (!get16(raw + 0x1a)) return -ENOENT;
    if (get16(raw + 0x1a) < 2) return -EOPNOTSUPP;
    uint64_t size = raw_size(raw);
    if (!size || size % volume_.block_size_) return -EUCLEAN;
    if (get32(raw + 0x20) & 0x1000U)
    {
        if (!(get32(super_ + 0x5c) & 0x20U)) return -EUCLEAN;
        IndexContext context;
        IndexedDirectory index(ns_index_io(number, raw, context), workspace_->directory);
        return index.validate();
    }
    if (size / volume_.block_size_ > kMaximumDirectoryBlocks) return -EFBIG;
    uint64_t physical = 0;
    error = tx_map(raw, 0, physical, false);
    if (error) return error;
    if (!physical) return -EUCLEAN;
    uint8_t block[4096];
    error = tx_read(physical, block);
    if (!error) error = ns_check_block(number, raw, block);
    if (error) return error;
    bool typed = (volume_.incompat_ & 2U) != 0;
    if (get32(block) != number || get16(block + 4) != 12 ||
        (typed ? block[6] != 1 : get16(block + 6) != 1) || block[8] != '.' ||
        (typed && block[7] != 2) || !get32(block + 12) || get32(block + 12) > volume_.inodes_ ||
        get16(block + 16) < 12 || (typed ? block[18] != 2 : get16(block + 18) != 2) ||
        block[20] != '.' || block[21] != '.' || (typed && block[19] != 2) ||
        (number == 2 && get32(block + 12) != 2) || (number != 2 && get32(block + 12) == number))
        return -EUCLEAN;
    return 0;
}

int WriteSession::ns_find(uint32_t number, uint8_t *raw, const char *name, DirectoryPosition &position)
{
    Size wanted = 0;
    while (wanted <= 255 && name[wanted]) ++wanted;
    if (!wanted || wanted > 255) return -EINVAL;
    if (get32(raw + 0x20) & 0x1000U)
    {
        IndexContext context;
        IndexedDirectory index(ns_index_io(number, raw, context), workspace_->directory);
        DirectoryIndexPosition found;
        int error = index.find(name, wanted, found);
        uint64_t physical = 0;
        if (!error) error = tx_map(raw, found.logical, physical, false);
        if (!error) position = {physical, found.inode, found.offset, found.previous, found.type};
        return error;
    }
    bool found = false;
    uint8_t block[4096];
    uint32_t limit = volume_.block_size_ - ((volume_.readonly_ & 0x400U) ? 12U : 0U);
    uint32_t blocks = uint32_t(raw_size(raw) / volume_.block_size_);
    for (uint32_t logical = 0; logical < blocks; ++logical)
    {
        uint64_t physical = 0;
        int error = tx_map(raw, logical, physical, false);
        if (error) return error;
        if (!physical) return -EUCLEAN;
        error = tx_read(physical, block);
        if (!error) error = ns_check_block(number, raw, block);
        if (error) return error;
        uint16_t previous = 0xffff;
        for (uint32_t offset = 0; offset < limit;)
        {
            const uint8_t *entry = block + offset;
            uint16_t length = volume_.incompat_ & 2U ? entry[6] : get16(entry + 6);
            bool matches = get32(entry) && length == wanted;
            for (uint16_t i = 0; matches && i < length; ++i) matches = entry[8 + i] == uint8_t(name[i]);
            if (matches)
            {
                if (found) return -EUCLEAN;
                position = {physical, get32(entry), uint16_t(offset), previous,
                            uint8_t(volume_.incompat_ & 2U ? entry[7] : 0)};
                found = true;
            }
            previous = uint16_t(offset);
            offset += get16(entry + 4);
        }
    }
    return found ? 0 : -ENOENT;
}

int WriteSession::ns_insert(uint32_t number, uint8_t *raw, const char *name, uint32_t child, uint8_t type)
{
    int length = name_length(name);
    if (length < 0) return length;
    if (get32(raw + 0x20) & 0x1000U)
    {
        IndexContext context;
        IndexedDirectory index(ns_index_io(number, raw, context), workspace_->directory);
        return index.insert(name, unsigned(length), child, type);
    }
    uint16_t needed = record_size(uint16_t(length));
    uint32_t limit = volume_.block_size_ - ((volume_.readonly_ & 0x400U) ? 12U : 0U);
    uint32_t blocks = uint32_t(raw_size(raw) / volume_.block_size_);
    uint8_t scratch[4096];
    uint64_t selected = 0;
    uint16_t target = 0, available = 0;
    for (uint32_t logical = 0; logical < blocks && !selected; ++logical)
    {
        uint64_t physical = 0;
        int error = tx_map(raw, logical, physical, false);
        if (error) return error;
        if (!physical) return -EUCLEAN;
        error = tx_read(physical, scratch);
        if (!error) error = ns_check_block(number, raw, scratch);
        if (error) return error;
        for (uint32_t offset = 0; offset < limit;)
        {
            const uint8_t *entry = scratch + offset;
            uint16_t record = get16(entry + 4);
            uint16_t length = volume_.incompat_ & 2U ? entry[6] : get16(entry + 6);
            uint16_t used = get32(entry) ? record_size(length) : 0;
            if (record - used >= needed)
            {
                selected = physical;
                target = uint16_t(offset + used);
                available = uint16_t(record - used);
                uint8_t *edited = nullptr;
                error = tx_edit(physical, edited);
                if (error) return error;
                if (used) set16(edited + offset + 4, used);
                break;
            }
            offset += record;
        }
    }
    if (!selected)
    {
        if (blocks == 1 && (get32(super_ + 0x5c) & 0x20U))
        {
            IndexContext context;
            IndexedDirectory index(ns_index_io(number, raw, context), workspace_->directory);
            int error = index.convert_and_insert(name, unsigned(length), child, type);
            if (!error) set32(raw + 0x20, get32(raw + 0x20) | 0x1000U);
            return error;
        }
        if (blocks == kMaximumDirectoryBlocks) return -EFBIG;
        int error = tx_map(raw, blocks, selected, true);
        if (error) return error;
        error = tx_resize(number, raw, uint64_t(blocks + 1) * volume_.block_size_);
        if (error) return error;
        target = 0;
        available = uint16_t(limit);
    }
    uint8_t *block = nullptr;
    int error = tx_edit(selected, block);
    if (error) return error;
    uint8_t *entry = block + target;
    zero(entry, available);
    set32(entry, child);
    set16(entry + 4, available);
    if (volume_.incompat_ & 2U) { entry[6] = uint8_t(length); entry[7] = type; }
    else set16(entry + 6, uint16_t(length));
    for (int i = 0; i < length; ++i) entry[8 + i] = uint8_t(name[i]);
    ns_seal_block(number, raw, block);
    return 0;
}

int WriteSession::ns_remove(uint32_t number, uint8_t *raw, const DirectoryPosition &position)
{
    uint8_t *block = nullptr;
    int error = tx_edit(position.block, block);
    if (error) return error;
    uint16_t record = get16(block + position.offset + 4);
    if (get32(block + position.offset) != position.number) return -EUCLEAN;
    if (position.previous != 0xffff)
    {
        uint16_t previous = get16(block + position.previous + 4);
        if (uint32_t(position.previous) + previous != position.offset) return -EUCLEAN;
        set16(block + position.previous + 4, uint16_t(previous + record));
        zero(block + position.offset, record);
    }
    else
    {
        zero(block + position.offset, record);
        set16(block + position.offset + 4, record);
    }
    ns_seal_block(number, raw, block);
    return 0;
}

int WriteSession::ns_empty(uint32_t number, uint8_t *raw, uint32_t expected_parent)
{
    if (get32(raw + 0x20) & 0x1000U)
    {
        IndexContext context;
        IndexedDirectory index(ns_index_io(number, raw, context), workspace_->directory);
        return index.empty(expected_parent);
    }
    uint8_t block[4096];
    uint32_t limit = volume_.block_size_ - ((volume_.readonly_ & 0x400U) ? 12U : 0U);
    uint32_t blocks = uint32_t(raw_size(raw) / volume_.block_size_);
    for (uint32_t logical = 0; logical < blocks; ++logical)
    {
        uint64_t physical = 0;
        int error = tx_map(raw, logical, physical, false);
        if (error) return error;
        if (!physical) return -EUCLEAN;
        error = tx_read(physical, block);
        if (!error) error = ns_check_block(number, raw, block);
        if (error) return error;
        for (uint32_t offset = 0; offset < limit;)
        {
            const uint8_t *entry = block + offset;
            uint32_t child = get32(entry);
            uint16_t length = volume_.incompat_ & 2U ? entry[6] : get16(entry + 6);
            if (child)
            {
                bool dot = logical == 0 && offset == 0 && length == 1 && entry[8] == '.' && child == number;
                bool dotdot = logical == 0 && offset == 12 && length == 2 && entry[8] == '.' &&
                              entry[9] == '.' && child == expected_parent;
                if (!dot && !dotdot) return -ENOTEMPTY;
            }
            offset += get16(entry + 4);
        }
    }
    return 0;
}

} // namespace xjext4
