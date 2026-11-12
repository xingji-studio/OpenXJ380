// SPDX-License-Identifier: Apache-2.0
// Original read-only package/namespace readback using first-party ext4 parsing.
#include <installer/ext4_verify.h>
#include "../../driver/fs/ext4/native.h"
#include <errno.h>

namespace xjinstaller {
namespace {

constexpr uint32_t kBlock = 4096, kGroup = 32768, kInodes = 8192, kMissing = uint32_t(-1);

uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t le32(const uint8_t *p) { return uint32_t(le16(p)) | uint32_t(le16(p + 2)) << 16; }
uint32_t be32(const uint8_t *p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t checksum(uint32_t state, const uint8_t *bytes, Size size)
{
    for (Size i = 0; i < size; ++i)
    {
        state ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) state = (state >> 1) ^ ((state & 1) ? 0x82f63b78U : 0);
    }
    return state;
}
void copy(void *to, const void *from, Size size)
{
    auto *a = static_cast<uint8_t *>(to);
    const auto *b = static_cast<const uint8_t *>(from);
    for (Size i = 0; i < size; ++i) a[i] = b[i];
}
bool equal(const void *a, const void *b, Size size)
{
    const auto *left = static_cast<const uint8_t *>(a), *right = static_cast<const uint8_t *>(b);
    for (Size i = 0; i < size; ++i) if (left[i] != right[i]) return false;
    return true;
}
Size length(const char *text)
{
    for (Size i = 0; i <= 255; ++i) if (!text[i]) return i;
    return 256;
}
bool named(xjpak2::Span path, const char *text, Size bytes)
{
    return path.size == bytes && equal(path.data, text, bytes);
}
int compare(xjpak2::Span a, xjpak2::Span b)
{
    Size size = a.size < b.size ? a.size : b.size;
    for (Size i = 0; i < size; ++i) if (a.data[i] != b.data[i]) return a.data[i] < b.data[i] ? -1 : 1;
    return a.size < b.size ? -1 : a.size != b.size;
}
uint32_t find(const xjpak2::Plan &plan, xjpak2::Span path, Size count)
{
    Size low = 0, high = count;
    while (low < high)
    {
        Size middle = low + (high - low) / 2;
        int order = compare(plan.records[middle].path, path);
        if (!order) return uint32_t(middle);
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    return kMissing;
}
struct Region { const void *pointer; Size size, alignment; };
bool separate(const Region *regions, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
    {
        uintptr_t a = uintptr_t(regions[i].pointer);
        Size bytes = regions[i].size;
        if (!a || bytes > uintptr_t(-1) - a || a % regions[i].alignment) return false;
        for (unsigned j = 0; j < i; ++j)
            if (a < uintptr_t(regions[j].pointer) + regions[j].size &&
                uintptr_t(regions[j].pointer) < a + bytes) return false;
    }
    return true;
}
bool regions_valid(const uint8_t *package, Size bytes, Size count, Ext4VerifyWorkspace &work,
                    Ext4VerifyStatus *status)
{
    if (count > Size(-1) / sizeof(xjpak2::Record) || count > Size(-1) / sizeof(Ext4VerifyEntry)) return false;
    Region regions[] = {{package, bytes, 1}, {&work, sizeof(work), alignof(Ext4VerifyWorkspace)},
        {work.records, count * sizeof(xjpak2::Record), alignof(xjpak2::Record)},
        {work.entries, count * sizeof(Ext4VerifyEntry), alignof(Ext4VerifyEntry)},
        {work.volume, sizeof(xjext4::Volume), alignof(xjext4::Volume)},
        {status, sizeof(*status), alignof(Ext4VerifyStatus)}};
    unsigned total = !status || status == &work.status ? 5 : 6;
    return separate(regions, total);
}
int finish(Ext4VerifyWorkspace &work, int error, Ext4VerifyStatus *status)
{
    work.status.error = error;
    if (work.used && error) work.stopped = true;
    if (status) *status = work.status;
    return error;
}
bool read(void *opaque, uint64_t offset, void *output, Size bytes)
{
    auto &work = *static_cast<Ext4VerifyWorkspace *>(opaque);
    if (work.stopped || work.status.io_failed) return false;
    if (offset > work.io.bytes || bytes > work.io.bytes - offset ||
        !work.io.read(work.io.context, offset, output, bytes))
    {
        work.status.io_failed = true;
        return false;
    }
    return true;
}
const char *filename(Ext4VerifyWorkspace &work, const xjpak2::Record &record)
{
    Size first = record.path.size;
    while (first && record.path.data[first - 1] != '/') --first;
    Size bytes = record.path.size - first;
    copy(work.name, record.path.data + first, bytes);
    work.name[bytes] = 0;
    return work.name;
}
int prepare(Ext4VerifyWorkspace &work)
{
    work.missing_lost_found = true;
    for (Size i = 0; i < work.plan.count; ++i) work.entries[i] = {};
    for (Size i = 0; i < work.plan.count; ++i)
    {
        const auto &record = work.records[i];
        auto &entry = work.entries[i];
        entry.order = uint32_t(i);
        if (record.kind != xjpak2::Kind::Hardlink) entry.references = 1;
        if (record.kind == xjpak2::Kind::Symlink && record.xattr_count) return -EOPNOTSUPP;
        if (named(record.path, "/lost+found", 11))
        {
            if (record.kind != xjpak2::Kind::Directory) return -EEXIST;
            work.missing_lost_found = false;
        }
        if (!i) continue;
        Size parent_size = record.path.size;
        while (parent_size > 1 && record.path.data[parent_size - 1] != '/') --parent_size;
        if (parent_size > 1) --parent_size;
        uint32_t parent = find(work.plan, {record.path.data, parent_size}, i);
        if (parent == kMissing) return -EINVAL;
        entry.parent_index = parent;
        if (record.kind == xjpak2::Kind::Directory) ++work.entries[parent].subdirectories;
    }
    if (work.missing_lost_found) ++work.entries[0].subdirectories;
    for (Size i = 0; i < work.plan.count; ++i)
        if (work.records[i].kind == xjpak2::Kind::Hardlink)
        {
            auto &target = work.entries[work.records[i].object_id - 1];
            if (target.references >= 65000) return -EMLINK;
            ++target.references;
        }
    for (Size i = 0; i < work.plan.count; ++i)
        if (work.entries[i].subdirectories > 64998) return -EMLINK;
    return 0;
}
int profile(Ext4VerifyWorkspace &work)
{
    const auto &volume = *work.volume;
    if (!read(&work, 1024, work.super, sizeof(work.super))) return -EIO;
    const uint8_t *s = work.super;
    uint32_t incompatible = le32(s + 0x60);
    if (volume.block_size() != kBlock || le16(s + 0x58) != 256 || le16(s + 0xfe) != 64 ||
        le32(s + 0x5c) != 0x2c || (incompatible != 0x2c2 && incompatible != 0x42c2) ||
        le32(s + 0x64) != 0x46b || le32(s + 0x14) || le32(s + 0x20) != kGroup ||
        le32(s + 0x28) != kInodes || le32(s + 0x54) != 11 || le32(s + 0xe0) != 8 ||
        le32(s + 0xe8) || le16(s + 0x3a) != 1 || s[0x174] != 7 ||
        !equal(volume.uuid(), work.uuid, 16) || !equal(s + 0x68, work.uuid, 16)) return -EUCLEAN;
    uint64_t blocks = volume.blocks_count();
    uint32_t groups = uint32_t((blocks + kGroup - 1) / kGroup);
    uint64_t allocated = 10 + work.plan.inode_count - !work.missing_lost_found;
    if (!groups || blocks > 0xffffffffULL || blocks * kBlock != work.io.bytes ||
        volume.inodes_count() != groups * kInodes || allocated > volume.inodes_count() ||
        volume.free_inodes() != volume.inodes_count() - allocated ||
        checksum(0xffffffffU, s, 0x3fc) != le32(s + 0x3fc)) return -EUCLEAN;
    return 0;
}
int no_xattrs(void *, uint8_t, const char *, const void *, Size) { return -EUCLEAN; }
int journal(Ext4VerifyWorkspace &work)
{
    xjext4::Inode inode;
    int error = work.volume->read_inode(8, inode);
    if (error) return error;
    if (inode.mode != 0100600 || inode.links != 1 || inode.uid || inode.gid || inode.flags != 0x80000 ||
        inode.xattr_block || inode.size % (1024 * 1024) || inode.size < 4 * 1024 * 1024 ||
        inode.size > 32 * 1024 * 1024 || inode.blocks_512 != inode.size / 512) return -EUCLEAN;
    error = work.volume->list_xattr_raw(8, no_xattrs, nullptr);
    Size done = 0;
    if (!error) error = work.volume->read(inode, 0, work.scratch, kBlock, done);
    if (error) return error;
    uint8_t *s = work.scratch;
    if (done != kBlock || be32(s) != 0xc03b3998 || be32(s + 4) != 4 || be32(s + 0xc) != kBlock ||
        be32(s + 0x10) != inode.size / kBlock || be32(s + 0x14) != 1 || be32(s + 0x1c) ||
        be32(s + 0x20) || be32(s + 0x24) || be32(s + 0x28) != 0x11 || be32(s + 0x2c) ||
        !equal(s + 0x30, work.uuid, 16) || be32(s + 0x40) != 1 || s[0x50] != 4) return -EUCLEAN;
    uint32_t stored = be32(s + 0xfc);
    for (unsigned i = 0; i < 4; ++i) s[0xfc + i] = 0;
    return checksum(0xffffffffU, s, 1024) == stored ? 0 : -EUCLEAN;
}
int attribute(void *opaque, uint8_t index, const char *name, const void *value, Size bytes)
{
    auto &work = *static_cast<Ext4VerifyWorkspace *>(opaque);
    const auto &record = work.records[work.current_record];
    Size name_size = length(name), offset = 0;
    if (index != 1 || name_size > 250) return -EUCLEAN;
    for (unsigned i = 0; i < record.xattr_count; ++i)
    {
        const uint8_t *raw = record.xattrs.data + offset;
        uint16_t full_size = le16(raw);
        uint32_t value_size = le32(raw + 4);
        if (full_size == name_size + 5 && equal(raw + 8, "user.", 5) &&
            equal(raw + 13, name, name_size))
        {
            if ((work.xattr_entries & (1U << i)) || value_size != bytes ||
                !equal(raw + 8 + full_size, value, bytes)) return -EUCLEAN;
            work.xattr_entries |= 1U << i;
            return 0;
        }
        offset += 8 + full_size + value_size;
    }
    return -EUCLEAN;
}
uint16_t inode_type(const xjpak2::Record &record)
{
    return record.kind == xjpak2::Kind::Directory ? 0040000 :
           record.kind == xjpak2::Kind::Symlink ? 0120000 : 0100000;
}
uint8_t directory_type(const xjpak2::Record &record)
{
    return record.kind == xjpak2::Kind::Regular ? 1 : record.kind == xjpak2::Kind::Directory ? 2 : 7;
}
int object(Ext4VerifyWorkspace &work, uint32_t index, const xjext4::Inode &inode)
{
    const auto &record = work.records[index];
    if (record.kind == xjpak2::Kind::Hardlink) return 0;
    auto &entry = work.entries[index];
    uint32_t expected_links = record.kind == xjpak2::Kind::Directory ? entry.subdirectories + 2 : entry.references;
    uint32_t expected_flags = record.kind == xjpak2::Kind::Symlink && record.payload.size < 60 ? 0 : 0x80000;
    uint32_t actual_flags = inode.flags;
    if (record.kind == xjpak2::Kind::Directory) actual_flags &= ~0x1000U;
    if (inode.mode != (inode_type(record) | record.mode) || inode.uid != record.uid || inode.gid != record.gid ||
        inode.mtime != uint64_t(record.mtime_seconds) || inode.mtime_nsec != record.mtime_nsec ||
        inode.links != expected_links || actual_flags != expected_flags) return -EUCLEAN;
    if (record.kind == xjpak2::Kind::Directory)
    {
        if (inode.size < kBlock || inode.size % kBlock) return -EUCLEAN;
    }
    else
    {
        if (inode.size != record.payload.size) return -EUCLEAN;
        Size offset = 0;
        while (offset < record.payload.size)
        {
            Size amount = record.payload.size - offset;
            if (amount > sizeof(work.scratch)) amount = sizeof(work.scratch);
            Size done = 0;
            int error = work.volume->read(inode, offset, work.scratch, amount, done);
            if (error) return error;
            if (done != amount || !equal(work.scratch, record.payload.data + offset, amount)) return -EUCLEAN;
            if (record.kind == xjpak2::Kind::Regular) work.status.data_bytes_verified += amount;
            offset += amount;
        }
    }
    work.current_record = index;
    work.xattr_entries = 0;
    int error = work.volume->list_xattr_raw(inode.number, attribute, &work);
    uint32_t expected = record.xattr_count == 32 ? uint32_t(-1) : (1U << record.xattr_count) - 1;
    return error ? error : work.xattr_entries == expected ? 0 : -EUCLEAN;
}
int directory_entry(void *opaque, const char *name, uint32_t inode, uint8_t type)
{
    auto &work = *static_cast<Ext4VerifyWorkspace *>(opaque);
    Size bytes = length(name);
    if (!bytes || bytes > 255) return -EUCLEAN;
    if ((bytes == 1 && name[0] == '.') || (bytes == 2 && name[0] == '.' && name[1] == '.'))
    {
        unsigned bit = bytes == 1 ? 1 : 2;
        if ((work.dot_entries & bit) || type != 2 ||
            inode != (bytes == 1 ? work.current_inode : work.current_parent)) return -EUCLEAN;
        work.dot_entries |= bit;
        return 0;
    }
    if (work.current_directory == kMissing) return -EUCLEAN; // Factory-only lost+found must be empty.
    const auto &parent = work.records[work.current_directory];
    Size prefix = parent.path.size == 1 ? 0 : parent.path.size;
    if (prefix + 1 + bytes > 4095) return -EUCLEAN;
    copy(work.path, parent.path.data, prefix);
    work.path[prefix] = '/';
    copy(work.path + prefix + 1, name, bytes);
    uint32_t found = find(work.plan, {reinterpret_cast<const uint8_t *>(work.path), prefix + 1 + bytes},
                           work.plan.count);
    if (found == kMissing)
    {
        if (work.current_directory || !work.missing_lost_found || bytes != 10 ||
            !equal(name, "lost+found", 10) || inode != 11 || type != 2 || work.lost_found_seen) return -EUCLEAN;
        work.lost_found_seen = true;
        return 0;
    }
    auto &entry = work.entries[found];
    const auto &record = work.records[found];
    const auto &canonical = record.kind == xjpak2::Kind::Hardlink ? work.records[record.object_id - 1] : record;
    if (entry.parent_index != work.current_directory || entry.inode != inode || entry.seen ||
        directory_type(canonical) != type) return -EUCLEAN;
    entry.seen = 1;
    ++work.status.names_verified;
    return 0;
}
int directories(Ext4VerifyWorkspace &work)
{
    for (Size i = 0; i < work.plan.count; ++i)
    {
        if (work.records[i].kind != xjpak2::Kind::Directory) continue;
        xjext4::Inode inode;
        int error = work.volume->read_inode(work.entries[i].inode, inode);
        if (error) return error;
        work.current_directory = uint32_t(i);
        work.current_inode = inode.number;
        work.current_parent = i ? work.entries[work.entries[i].parent_index].inode : 2;
        work.dot_entries = 0;
        error = work.volume->list_directory(inode, directory_entry, &work);
        if (error || work.dot_entries != 3) return error ? error : -EUCLEAN;
    }
    for (Size i = 1; i < work.plan.count; ++i) if (work.entries[i].seen != 1) return -EUCLEAN;
    if (!work.missing_lost_found) return 0;
    if (!work.lost_found_seen) return -EUCLEAN;
    xjext4::Inode lost;
    int error = work.volume->read_inode(11, lost);
    if (error) return error;
    if (lost.mode != 0040700 || lost.uid || lost.gid || lost.links != 2 || lost.flags != 0x80000 ||
        lost.size != kBlock || lost.blocks_512 != 8 || lost.xattr_block) return -EUCLEAN;
    error = work.volume->list_xattr_raw(11, no_xattrs, nullptr);
    work.current_directory = kMissing; work.current_inode = 11; work.current_parent = 2; work.dot_entries = 0;
    if (!error) error = work.volume->list_directory(lost, directory_entry, &work);
    if (error || work.dot_entries != 3) return error ? error : -EUCLEAN;
    return 0;
}
bool less(const Ext4VerifyWorkspace &work, Size a, Size b)
{
    uint32_t left = work.entries[a].order, right = work.entries[b].order;
    uint32_t x = work.entries[left].inode, y = work.entries[right].inode;
    return x < y || (x == y && left < right);
}
void swap(Ext4VerifyWorkspace &work, Size a, Size b)
{
    uint32_t value = work.entries[a].order;
    work.entries[a].order = work.entries[b].order; work.entries[b].order = value;
}
void sift(Ext4VerifyWorkspace &work, Size root, Size count)
{
    while (root < count / 2)
    {
        Size child = root * 2 + 1;
        if (child + 1 < count && less(work, child, child + 1)) ++child;
        if (!less(work, root, child)) return;
        swap(work, root, child); root = child;
    }
}
int identities(Ext4VerifyWorkspace &work)
{
    // Sort only the embedded index permutation; ordinal maps remain stable.
    Size count = work.plan.count;
    for (Size i = count / 2; i; --i) sift(work, i - 1, count);
    for (Size end = count; end > 1; --end) { swap(work, 0, end - 1); sift(work, 0, end - 1); }
    for (Size first = 0; first < count;)
    {
        Size last = first;
        uint32_t inode = work.entries[work.entries[first].order].inode, canonical = kMissing;
        while (last < count && work.entries[work.entries[last].order].inode == inode)
        {
            uint32_t index = work.entries[last].order;
            if (work.records[index].kind != xjpak2::Kind::Hardlink)
            {
                if (canonical != kMissing) return -EUCLEAN;
                canonical = index;
            }
            ++last;
        }
        if (canonical == kMissing) return -EUCLEAN;
        for (Size i = first; i < last; ++i)
        {
            uint32_t index = work.entries[i].order;
            if (index != canonical && work.records[index].object_id - 1 != canonical) return -EUCLEAN;
        }
        first = last;
    }
    return 0;
}
int allocation(Ext4VerifyWorkspace &work)
{
    uint32_t seed = checksum(0xffffffffU, work.uuid, 16);
    uint64_t blocks = work.volume->blocks_count(), free_blocks = 0, free_inodes = 0;
    uint32_t groups = uint32_t((blocks + kGroup - 1) / kGroup);
    Size next = 0;
    for (uint32_t group = 0; group < groups; ++group)
    {
        if (!read(&work, uint64_t(kBlock) + uint64_t(group) * 64, work.descriptor, 64)) return -EIO;
        uint8_t *d = work.descriptor;
        uint16_t expected_checksum = le16(d + 0x1e);
        d[0x1e] = d[0x1f] = 0;
        uint8_t encoded[] = {uint8_t(group), uint8_t(group >> 8), uint8_t(group >> 16), uint8_t(group >> 24)};
        if (uint16_t(checksum(checksum(seed, encoded, 4), d, 64)) != expected_checksum || le16(d + 18) != 4)
            return -EUCLEAN;
        uint64_t bitmap = le32(d + 4) | uint64_t(le32(d + 36)) << 32;
        uint64_t block_bitmap = le32(d) | uint64_t(le32(d + 32)) << 32;
        uint64_t table = le32(d + 8) | uint64_t(le32(d + 40)) << 32;
        if (!bitmap || bitmap >= blocks || !block_bitmap || block_bitmap >= blocks ||
            !table || table >= blocks || kInodes / 16 > blocks - table) return -EUCLEAN;
        if (!read(&work, bitmap * kBlock, work.scratch, kBlock)) return -EIO;
        uint32_t expected_bitmap = le16(d + 26) | uint32_t(le16(d + 58)) << 16;
        if (checksum(seed, work.scratch, kInodes / 8) != expected_bitmap) return -EUCLEAN;
        uint32_t group_free = 0, last_used = 0, group_directories = group ? 0 : uint32_t(work.missing_lost_found);
        for (uint32_t bit = 0; bit < kGroup; ++bit)
        {
            uint32_t number = group * kInodes + bit + 1;
            bool expected = true;
            if (bit < kInodes)
            {
                while (next < work.plan.count && work.entries[work.entries[next].order].inode < number) ++next;
                expected = number <= 11 || (next < work.plan.count &&
                    work.entries[work.entries[next].order].inode == number);
                if (!expected) ++group_free;
                else
                {
                    last_used = bit + 1;
                    if (next < work.plan.count && work.entries[work.entries[next].order].inode == number &&
                        work.records[work.entries[next].order].kind == xjpak2::Kind::Directory)
                        ++group_directories;
                }
            }
            if (bool(work.scratch[bit / 8] & (1U << (bit % 8))) != expected) return -EUCLEAN;
        }
        if (group_free != (le16(d + 14) | uint32_t(le16(d + 46)) << 16) ||
            group_directories != (le16(d + 16) | uint32_t(le16(d + 48)) << 16) ||
            kInodes - last_used != (le16(d + 28) | uint32_t(le16(d + 50)) << 16)) return -EUCLEAN;
        free_inodes += group_free;
        if (!read(&work, block_bitmap * kBlock, work.scratch, kBlock)) return -EIO;
        expected_bitmap = le16(d + 24) | uint32_t(le16(d + 56)) << 16;
        if (checksum(seed, work.scratch, kBlock) != expected_bitmap) return -EUCLEAN;
        uint32_t valid = uint32_t(blocks - uint64_t(group) * kGroup);
        if (valid > kGroup) valid = kGroup;
        group_free = 0;
        for (uint32_t bit = 0; bit < kGroup; ++bit)
        {
            bool used = (work.scratch[bit / 8] & (1U << (bit % 8))) != 0;
            if (bit >= valid && !used) return -EUCLEAN;
            if (bit < valid && !used) ++group_free;
        }
        if (group_free != (le16(d + 12) | uint32_t(le16(d + 44)) << 16)) return -EUCLEAN;
        free_blocks += group_free;
        ++work.status.groups_verified;
    }
    return free_blocks == work.volume->free_blocks() && free_inodes == work.volume->free_inodes() ? 0 : -EUCLEAN;
}

} // namespace

int ext4_verify_package(const uint8_t *package, Size bytes, const xjpak2::Limits &requested,
                         const Ext4VerifyDevice &device, const uint8_t expected_uuid[16],
                         Ext4VerifyWorkspace &work, Ext4VerifyStatus *status)
{
    if (!expected_uuid || !device.read || device.bytes < 32ULL * 1024 * 1024 || device.bytes % kBlock ||
        device.bytes / kBlock > 0xffffffffULL) return -EINVAL;
    uint8_t uuid[16], nonzero = 0;
    copy(uuid, expected_uuid, 16);
    for (uint8_t byte : uuid) nonzero |= byte;
    if (!nonzero) return -EINVAL;
    Ext4VerifyDevice snapshot = device;
    xjpak2::Limits limits = requested;
    limits.allow_v1 = limits.allow_trusted = limits.allow_security = false;
    if (limits.max_entries > 262144) limits.max_entries = 262144;
    if (limits.max_inodes > 262144) limits.max_inodes = 262144;
    if (limits.max_bytes > (8ULL << 30)) limits.max_bytes = 8ULL << 30;
    if (limits.max_file_bytes > (4ULL << 30)) limits.max_file_bytes = 4ULL << 30;
    if (limits.max_data_bytes > (8ULL << 30)) limits.max_data_bytes = 8ULL << 30;
    if (limits.max_path > 4095) limits.max_path = 4095;
    if (limits.max_depth > 128) limits.max_depth = 128;
    if (limits.max_xattrs > 32) limits.max_xattrs = 32;
    if (limits.max_xattr_storage > 3968) limits.max_xattr_storage = 3968;
    xjpak2::HeaderInfo header;
    int error = xjpak2::inspect_header(package, bytes, limits, header);
    if (error) return error;
    if (header.record_count > work.capacity || !regions_valid(package, bytes, header.record_count, work, status))
        return -EINVAL;
    if (work.used)
    {
        if (status) *status = work.status;
        return work.status.error ? work.status.error : -EBUSY;
    }
    work.status = {}; work.plan = {}; work.lost_found_seen = false;
    error = xjpak2::validate(package, bytes, limits, {work.records, work.capacity}, work.plan);
    if (!error && work.plan.legacy_v1) error = -EOPNOTSUPP;
    if (!error) error = prepare(work);
    if (error) return finish(work, error, status);
    work.io = snapshot;
    copy(work.uuid, uuid, 16);
    work.used = true;
    work.volume->reset();
    error = work.volume->mount(read, &work, snapshot.bytes);
    if (!error) error = profile(work);
    if (!error) error = journal(work);
    for (Size i = 0; !error && i < work.plan.count; ++i)
    {
        xjext4::Inode inode;
        if (!i) error = work.volume->read_inode(2, inode);
        else
        {
            xjext4::Inode parent;
            error = work.volume->read_inode(work.entries[work.entries[i].parent_index].inode, parent);
            if (!error) error = work.volume->lookup(parent, filename(work, work.records[i]), inode);
        }
        if (!error && i && (inode.number < 12 &&
            !(inode.number == 11 && named(work.records[i].path, "/lost+found", 11)))) error = -EUCLEAN;
        if (!error)
        {
            work.entries[i].inode = inode.number;
            error = object(work, uint32_t(i), inode);
        }
        if (!error) ++work.status.records_verified;
    }
    if (!error) error = directories(work);
    if (!error) error = identities(work);
    if (!error) error = allocation(work);
    if (work.status.io_failed) error = -EIO;
    if (!error && (work.status.data_bytes_verified != work.plan.data_bytes ||
                   work.status.names_verified != work.plan.count - 1)) error = -EUCLEAN;
    if (!error) work.status.complete = true;
    return finish(work, error, status);
}

} // namespace xjinstaller
