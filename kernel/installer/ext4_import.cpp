// SPDX-License-Identifier: Apache-2.0
// Original fresh native-ext4 package application using inode-relative APIs.
#include <installer/ext4_import.h>
#include "../../driver/fs/ext4/write.h"
#include <errno.h>

namespace xjinstaller {
namespace {

constexpr uint32_t kBlock = 4096, kGroup = 32768, kInodes = 8192, kLinks = 65000;
constexpr Size kMissing = Size(-1);

uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t le32(const uint8_t *p) { return uint32_t(le16(p)) | uint32_t(le16(p + 2)) << 16; }
void copy(void *to, const void *from, Size bytes)
{
    auto *out = static_cast<uint8_t *>(to);
    const auto *in = static_cast<const uint8_t *>(from);
    for (Size i = 0; i < bytes; ++i) out[i] = in[i];
}
bool same(const uint8_t *a, const uint8_t *b, Size bytes)
{
    for (Size i = 0; i < bytes; ++i) if (a[i] != b[i]) return false;
    return true;
}
bool text(const char *a, const char *b)
{
    for (Size i = 0;; ++i)
    {
        if (a[i] != b[i]) return false;
        if (!a[i]) return true;
    }
}
bool named(xjpak2::Span span, const char *name, Size length)
{
    if (span.size != length) return false;
    for (Size i = 0; i < length; ++i) if (span.data[i] != uint8_t(name[i])) return false;
    return true;
}
int compare(xjpak2::Span a, xjpak2::Span b)
{
    Size length = a.size < b.size ? a.size : b.size;
    for (Size i = 0; i < length; ++i)
        if (a.data[i] != b.data[i]) return a.data[i] < b.data[i] ? -1 : 1;
    return a.size < b.size ? -1 : a.size != b.size;
}
Size find(const xjpak2::Plan &plan, xjpak2::Span path, Size count)
{
    Size low = 0, high = count;
    while (low < high)
    {
        Size middle = low + (high - low) / 2;
        int order = compare(plan.records[middle].path, path);
        if (!order) return middle;
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    return kMissing;
}
struct Region { const void *pointer; Size bytes; Size alignment; };
bool separate(const Region *regions, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
    {
        const Region &a = regions[i];
        uintptr_t start = uintptr_t(a.pointer);
        if (!a.bytes) continue;
        if (!a.pointer || start > uintptr_t(-1) - a.bytes || start % a.alignment) return false;
        for (unsigned j = 0; j < i; ++j)
        {
            const Region &b = regions[j];
            if (b.bytes && start < uintptr_t(b.pointer) + b.bytes && uintptr_t(b.pointer) < start + a.bytes)
                return false;
        }
    }
    return true;
}
bool regions_valid(const uint8_t *package, Size bytes, Ext4ImportWorkspace &work, Size count, bool full,
                   const xjext4::WriteStorage *storage = nullptr)
{
    if (count > Size(-1) / sizeof(xjpak2::Record) || count > Size(-1) / sizeof(Ext4ImportEntry)) return false;
    Region regions[] = {
        {package, bytes, 1}, {&work, sizeof(work), alignof(Ext4ImportWorkspace)},
        {work.records, count * sizeof(xjpak2::Record), alignof(xjpak2::Record)},
        {work.entries, count * sizeof(Ext4ImportEntry), alignof(Ext4ImportEntry)},
        {work.writer, sizeof(xjext4::WriteSession), alignof(xjext4::WriteSession)},
        {work.writer_workspace, sizeof(xjext4::WriteWorkspace), alignof(xjext4::WriteWorkspace)},
        {work.read_volume, sizeof(xjext4::Volume), alignof(xjext4::Volume)},
        {storage ? storage->data : nullptr, storage ? storage->bytes : 0, alignof(uint64_t)}
    };
    return separate(regions, full ? 8 : 4);
}
bool add(uint64_t &value, uint64_t amount)
{
    if (amount > uint64_t(-1) - value) return false;
    value += amount;
    return true;
}
int finish(Ext4ImportWorkspace &work, int error, Ext4ImportStatus &status)
{
    work.status.error = error;
    if (error) work.stopped = true;
    status = work.status;
    return error;
}
bool io_failure(Ext4ImportWorkspace &work)
{
    work.status.io_failed = true;
    return false;
}
bool read(void *context, uint64_t offset, void *output, Size bytes)
{
    auto &work = *static_cast<Ext4ImportWorkspace *>(context);
    if (work.stopped || work.status.io_failed) return false;
    if (offset > work.io.bytes || bytes > work.io.bytes - offset) return io_failure(work);
    return work.io.read(work.io.context, offset, output, bytes) || io_failure(work);
}
bool write(void *context, uint64_t offset, const void *input, Size bytes)
{
    auto &work = *static_cast<Ext4ImportWorkspace *>(context);
    if (work.stopped || work.status.io_failed) return false;
    if (offset > work.io.bytes || bytes > work.io.bytes - offset) return io_failure(work);
    return work.io.write(work.io.context, offset, input, bytes) || io_failure(work);
}
bool flush(void *context)
{
    auto &work = *static_cast<Ext4ImportWorkspace *>(context);
    if (work.stopped || work.status.io_failed) return false;
    return work.io.flush(work.io.context) || io_failure(work);
}
bool sparse(uint32_t group)
{
    if (group <= 1) return true;
    constexpr uint32_t bases[] = {3, 5, 7};
    for (uint32_t base : bases)
    {
        uint32_t value = group;
        while (value % base == 0) value /= base;
        if (value == 1) return true;
    }
    return false;
}
int no_xattrs(void *, uint8_t, const char *, const void *, Size) { return -EEXIST; }
struct DirectoryCheck { uint32_t inode; unsigned seen; bool root; };
int check_directory(void *context, const char *name, uint32_t inode, uint8_t type)
{
    auto &check = *static_cast<DirectoryCheck *>(context);
    unsigned bit = 0;
    uint32_t expected = 0;
    if (text(name, ".")) { bit = 1; expected = check.inode; }
    else if (text(name, "..")) { bit = 2; expected = 2; }
    else if (check.root && text(name, "lost+found")) { bit = 4; expected = 11; }
    else return -EEXIST;
    if (inode != expected || type != 2 || (check.seen & bit)) return -EUCLEAN;
    check.seen |= bit;
    return 0;
}
int raw_inode(Ext4ImportWorkspace &work, uint32_t number)
{
    uint32_t group = (number - 1) / kInodes, index = (number - 1) % kInodes;
    if (!read(&work, uint64_t(kBlock) + uint64_t(group) * 64, work.scratch, 64)) return -EIO;
    uint64_t table = le32(work.scratch + 8) | uint64_t(le32(work.scratch + 40)) << 32;
    if (table >= work.io.bytes / kBlock) return -EUCLEAN;
    if (!read(&work, table * kBlock + uint64_t(index) * 256, work.scratch, 256)) return -EIO;
    return le16(work.scratch + 0x80) == 32 ? 0 : -EOPNOTSUPP;
}
int fresh(Ext4ImportWorkspace &work, const xjext4::Volume &volume)
{
    if (!read(&work, 1024, work.super, sizeof(work.super))) return -EIO;
    const uint8_t *super = work.super;
    if (volume.block_size() != kBlock || le16(super + 0x58) != 256 || le16(super + 0xfe) != 64 ||
        le32(super + 0x5c) != 0x2c || le32(super + 0x60) != 0x2c2 || le32(super + 0x64) != 0x46b ||
        le32(super + 0x14) || le32(super + 0x20) != kGroup || le32(super + 0x28) != kInodes ||
        le32(super + 0x54) != 11 || le32(super + 0xe0) != 8 || le32(super + 0xe8) ||
        le16(super + 0x3a) != 1 || super[0x174] != 7) return -EOPNOTSUPP;
    uint64_t blocks = volume.blocks_count();
    uint32_t groups = uint32_t(blocks / kGroup + (blocks % kGroup != 0));
    if (!groups || blocks > 0xffffffffULL || blocks > work.io.bytes / kBlock || blocks * kBlock != work.io.bytes ||
        volume.inodes_count() != groups * kInodes || volume.free_inodes() != groups * kInodes - 11)
        return -EEXIST;
    xjext4::Inode root, lost, journal;
    int error = volume.read_inode(2, root);
    if (!error) error = volume.read_inode(11, lost);
    if (!error) error = volume.read_inode(8, journal);
    if (error) return error;
    if (root.mode != 0040755 || root.links != 3 || root.uid || root.gid || root.flags != 0x80000 ||
        root.size != kBlock || root.blocks_512 != 8 || root.xattr_block ||
        lost.mode != 0040700 || lost.links != 2 || lost.uid || lost.gid || lost.flags != 0x80000 ||
        lost.size != kBlock || lost.blocks_512 != 8 || lost.xattr_block ||
        journal.mode != 0100600 || journal.links != 1 || journal.uid || journal.gid ||
        journal.flags != 0x80000 || journal.xattr_block || journal.size % (1024 * 1024) ||
        journal.size < 4 * 1024 * 1024 || journal.size > 32 * 1024 * 1024 ||
        journal.blocks_512 != journal.size / 512) return -EEXIST;
    error = raw_inode(work, 2);
    if (!error) error = raw_inode(work, 11);
    if (!error) error = volume.list_xattr_raw(2, no_xattrs, nullptr);
    if (!error) error = volume.list_xattr_raw(11, no_xattrs, nullptr);
    DirectoryCheck root_check = {2, 0, true}, lost_check = {11, 0, false};
    if (!error) error = volume.list_directory(root, check_directory, &root_check);
    if (!error) error = volume.list_directory(lost, check_directory, &lost_check);
    if (error) return error;
    if (root_check.seen != 7 || lost_check.seen != 3) return -EUCLEAN;
    uint64_t allocated = uint64_t(groups) * (2 + kInodes / 16) + journal.size / kBlock + 2;
    uint32_t descriptors = (groups * 64 + kBlock - 1) / kBlock;
    for (uint32_t group = 0; group < groups; ++group) if (sparse(group)) allocated += 1 + descriptors;
    if (allocated > blocks || volume.free_blocks() != blocks - allocated) return -EEXIST;
    uint64_t free_total = 0;
    for (uint32_t group = 0; group < groups; ++group)
    {
        if (!read(&work, uint64_t(kBlock) + uint64_t(group) * 64, work.scratch, 64)) return -EIO;
        uint64_t bitmap = le32(work.scratch) | uint64_t(le32(work.scratch + 32)) << 32;
        uint64_t inode_bitmap = le32(work.scratch + 4) | uint64_t(le32(work.scratch + 36)) << 32;
        uint32_t expected_free = le16(work.scratch + 12) | uint32_t(le16(work.scratch + 44)) << 16;
        uint32_t free_inodes = le16(work.scratch + 14) | uint32_t(le16(work.scratch + 46)) << 16;
        uint32_t directories = le16(work.scratch + 16) | uint32_t(le16(work.scratch + 48)) << 16;
        if (bitmap >= blocks || inode_bitmap >= blocks || le16(work.scratch + 18) != 4 ||
            free_inodes != kInodes - (group ? 0 : 11) || directories != (group ? 0U : 2U)) return -EEXIST;
        if (!read(&work, inode_bitmap * kBlock, work.scratch, kBlock)) return -EIO;
        for (uint32_t bit = 0; bit < kBlock * 8; ++bit)
        {
            bool set = (work.scratch[bit / 8] & (1U << (bit % 8))) != 0;
            bool expected = bit >= kInodes || (!group && bit < 11);
            if (set != expected) return -EEXIST;
        }
        if (!read(&work, bitmap * kBlock, work.scratch, kBlock)) return -EIO;
        uint32_t valid = uint32_t(blocks - uint64_t(group) * kGroup);
        if (valid > kGroup) valid = kGroup;
        uint32_t count = 0;
        for (uint32_t bit = 0; bit < kGroup; ++bit)
        {
            bool set = (work.scratch[bit / 8] & (1U << (bit % 8))) != 0;
            if (bit >= valid && !set) return -EUCLEAN;
            if (bit < valid && !set) ++count;
        }
        if (count != expected_free) return -EUCLEAN;
        free_total += count;
    }
    if (free_total != volume.free_blocks()) return -EUCLEAN;
    return ext4_import_check_budget(work.requirements, volume.block_size(), volume.free_blocks(),
                                    volume.free_inodes());
}
int initial(void *context, const xjext4::Volume &volume)
{
    auto &work = *static_cast<Ext4ImportWorkspace *>(context);
    if (!same(work.uuid, volume.uuid(), 16)) return -EUCLEAN;
    return fresh(work, volume);
}
const char *filename(Ext4ImportWorkspace &work, const xjpak2::Record &record)
{
    Size start = record.path.size;
    while (start && record.path.data[start - 1] != '/') --start;
    Size length = record.path.size - start;
    copy(work.name, record.path.data + start, length);
    work.name[length] = 0;
    return work.name;
}
int metadata(Ext4ImportWorkspace &work, Size index, uint64_t seconds)
{
    const auto &record = work.plan.records[index];
    xjext4::AttributeUpdate update = {};
    update.mask = xjext4::kAttributeUid | xjext4::kAttributeGid;
    update.uid = record.uid; update.gid = record.gid;
    int error = work.writer->set_attributes(work.entries[index].inode, update, seconds);
    update = {};
    update.mask = xjext4::kAttributeMode; update.mode = uint16_t(record.mode);
    if (!error) error = work.writer->set_attributes(work.entries[index].inode, update, seconds);
    Size offset = 0;
    for (unsigned i = 0; !error && i < record.xattr_count; ++i)
    {
        const uint8_t *raw = record.xattrs.data + offset;
        uint16_t length = le16(raw);
        uint32_t bytes = le32(raw + 4);
        copy(work.attribute_name, raw + 8, length); work.attribute_name[length] = 0;
        error = work.writer->set_xattr(work.entries[index].inode, work.attribute_name, raw + 8 + length,
                                       bytes, 0, seconds);
        offset += 8 + length + bytes;
    }
    return error;
}

} // namespace

int ext4_import_prepare(const uint8_t *package, Size bytes, const xjpak2::Limits &requested,
                        Ext4ImportWorkspace &work, Ext4ImportRequirements &requirements)
{
    if (work.used || work.stopped || work.status.io_failed)
        return work.status.error ? work.status.error : -EBUSY;
    xjpak2::Limits limits = requested;
    limits.allow_v1 = limits.allow_trusted = limits.allow_security = false;
    if (limits.max_entries > 262144) limits.max_entries = 262144;
    if (limits.max_inodes > 262144) limits.max_inodes = 262144;
    if (limits.max_bytes > (8ULL << 30)) limits.max_bytes = 8ULL << 30;
    if (limits.max_data_bytes > (8ULL << 30)) limits.max_data_bytes = 8ULL << 30;
    if (limits.max_file_bytes > (4ULL << 30)) limits.max_file_bytes = 4ULL << 30;
    if (limits.max_xattrs > 32) limits.max_xattrs = 32;
    if (limits.max_xattr_storage > 3968) limits.max_xattr_storage = 3968;
    xjpak2::HeaderInfo header;
    int error = xjpak2::inspect_header(package, bytes, limits, header);
    if (error) return error;
    if (header.record_count > work.capacity || !regions_valid(package, bytes, work, header.record_count, false))
        return -EINVAL;
    work.status = {}; work.plan = {}; work.requirements = {}; requirements = {};
    error = xjpak2::validate(package, bytes, limits, {work.records, work.capacity}, work.plan);
    if (error) return error;
    auto lost_path = xjpak2::Span{reinterpret_cast<const uint8_t *>("/lost+found"), 11};
    Size lost = find(work.plan, lost_path, work.plan.count);
    if (lost != kMissing && work.records[lost].kind != xjpak2::Kind::Directory) return -EEXIST;
    for (Size i = 0; i < work.plan.count; ++i) work.entries[i] = {};
    if (lost == kMissing) work.entries[0].children = work.entries[0].subdirectories = 1;
    Ext4ImportRequirements needed;
    needed.blocks = 32; // Explicit spare budget; does not reserve physical space.
    needed.canonical_inodes = uint32_t(work.plan.inode_count);
    needed.new_inodes = needed.canonical_inodes - 1 - (lost != kMissing);
    needed.names = uint32_t(work.plan.count - 1 - (lost != kMissing));
    for (Size i = 0; i < work.plan.count; ++i)
    {
        const auto &record = work.records[i];
        if (record.kind != xjpak2::Kind::Hardlink) work.entries[i].references = 1;
        if (!i) continue;
        Size parent_size = record.path.size;
        while (parent_size > 1 && record.path.data[parent_size - 1] != '/') --parent_size;
        if (parent_size > 1) --parent_size;
        Size parent = find(work.plan, {record.path.data, parent_size}, i);
        if (parent == kMissing) return -EINVAL;
        work.entries[i].parent_index = uint32_t(parent);
        ++work.entries[parent].children;
        if (record.kind == xjpak2::Kind::Directory) ++work.entries[parent].subdirectories;
    }
    for (Size i = 0; i < work.plan.count; ++i)
    {
        const auto &record = work.records[i];
        if (record.kind == xjpak2::Kind::Hardlink)
        {
            auto &target = work.entries[record.object_id - 1];
            if (target.references >= kLinks) return -EMLINK;
            ++target.references;
            continue;
        }
        uint64_t blocks = 0;
        if (record.kind == xjpak2::Kind::Directory)
        {
            if (work.entries[i].subdirectories > kLinks - 2) return -EMLINK;
            blocks = uint64_t(work.entries[i].children) * 2 + (i && i != lost);
        }
        else if (record.kind == xjpak2::Kind::Regular)
        {
            blocks = record.payload.size / kBlock + (record.payload.size % kBlock != 0);
            if (blocks > 4 && !add(blocks, 2 * ((blocks + 127) / 128) + 4)) return -EOVERFLOW;
        }
        else if (record.kind == xjpak2::Kind::Symlink)
        {
            if (record.xattr_count) return -EOPNOTSUPP;
            blocks = record.payload.size >= 60;
        }
        if (record.xattr_count && !add(blocks, 1)) return -EOVERFLOW;
        if (!add(needed.blocks, blocks)) return -EOVERFLOW;
    }
    work.package = package; work.package_bytes = bytes; work.limits = limits;
    work.requirements = needed; requirements = needed;
    work.status.prepared = true; work.status.phase = Ext4ImportPhase::Prepared;
    return 0;
}

int ext4_import_check_budget(const Ext4ImportRequirements &requirements, uint32_t block_size,
                             uint64_t free_blocks, uint32_t free_inodes)
{
    if (block_size != kBlock || requirements.block_size != kBlock) return -EOPNOTSUPP;
    if (requirements.blocks > free_blocks || requirements.new_inodes > free_inodes) return -ENOSPC;
    return 0;
}

int ext4_import_apply_fresh(const xjext4::WriteDevice &device, uint64_t seconds,
                            Ext4ImportWorkspace &work, Ext4ImportStatus &status,
                            const xjext4::WriteStorage *storage)
{
    if (work.used || work.stopped || work.status.io_failed)
    {
        status = work.status;
        return work.status.error ? work.status.error : -EBUSY;
    }
    if (!device.read || !device.write || !device.flush || !device.bytes || seconds > 0x37fffffffULL ||
        !work.status.prepared) return -EINVAL;
    Ext4ImportRequirements needed;
    int error = ext4_import_prepare(work.package, work.package_bytes, work.limits, work, needed);
    if (error) return error;
    if (!regions_valid(work.package, work.package_bytes, work, work.plan.count, true, storage)) return -EINVAL;
    work.io = {device.read, device.write, device.flush, device.context, device.bytes};
    work.used = true;
    work.status.phase = Ext4ImportPhase::Preflight;
    work.read_volume->reset();
    error = work.read_volume->mount(read, &work, device.bytes);
    if (!error) error = fresh(work, *work.read_volume);
    if (error) return finish(work, error, status);
    copy(work.uuid, work.read_volume->uuid(), 16);
    work.status.preflight_complete = true;
    work.status.phase = Ext4ImportPhase::Opening;
    work.status.writer_started = true;
    xjext4::WriteDevice wrapped = {read, write, flush, &work, device.bytes};
    error = work.writer->open(wrapped, *work.writer_workspace, initial, &work, storage);
    if (error) return finish(work, error, status);
    work.status.writer_open = true;
    work.entries[0].inode = 2;
    work.status.phase = Ext4ImportPhase::Directories;
    for (Size i = 1; !error && i < work.plan.count; ++i)
    {
        const auto &record = work.records[i];
        if (record.kind != xjpak2::Kind::Directory) continue;
        work.status.last_record = uint32_t(i + 1);
        if (named(record.path, "/lost+found", 11)) { work.entries[i].inode = 11; continue; }
        xjext4::Inode created;
        error = work.writer->create(work.entries[work.entries[i].parent_index].inode, filename(work, record),
                                     0040700, 0, 0, seconds, created);
        if (!error)
        {
            work.entries[i].inode = created.number;
            ++work.status.inodes_created; ++work.status.names_created;
        }
    }
    if (error) return finish(work, error, status);
    work.status.phase = Ext4ImportPhase::RegularData;
    for (Size i = 1; !error && i < work.plan.count; ++i)
    {
        const auto &record = work.records[i];
        if (record.kind != xjpak2::Kind::Regular) continue;
        work.status.last_record = uint32_t(i + 1);
        xjext4::Inode created;
        error = work.writer->create(work.entries[work.entries[i].parent_index].inode, filename(work, record),
                                     0100600, 0, 0, seconds, created);
        if (!error)
        {
            work.entries[i].inode = created.number;
            ++work.status.inodes_created; ++work.status.names_created;
        }
        for (Size offset = 0; !error && offset < record.payload.size;)
        {
            Size amount = record.payload.size - offset;
            if (amount > 65536) amount = 65536;
            Size completed = 0;
            error = work.writer->pwrite(created.number, offset, record.payload.data + offset, amount, seconds,
                                         completed);
            if (completed > amount) error = -EUCLEAN;
            else { offset += completed; work.status.data_bytes_committed += completed; }
            if (!error && completed != amount) error = -EIO;
        }
    }
    if (error) return finish(work, error, status);
    work.status.phase = Ext4ImportPhase::Symlinks;
    for (Size i = 1; !error && i < work.plan.count; ++i)
    {
        const auto &record = work.records[i];
        if (record.kind != xjpak2::Kind::Symlink) continue;
        work.status.last_record = uint32_t(i + 1);
        copy(work.scratch, record.payload.data, record.payload.size); work.scratch[record.payload.size] = 0;
        xjext4::Inode created;
        error = work.writer->symlink(work.entries[work.entries[i].parent_index].inode, filename(work, record),
                                      reinterpret_cast<const char *>(work.scratch), 0, 0, seconds, created);
        if (!error)
        {
            work.entries[i].inode = created.number;
            ++work.status.inodes_created; ++work.status.names_created;
        }
    }
    if (error) return finish(work, error, status);
    work.status.phase = Ext4ImportPhase::Hardlinks;
    for (Size i = 1; !error && i < work.plan.count; ++i)
    {
        const auto &record = work.records[i];
        if (record.kind != xjpak2::Kind::Hardlink) continue;
        work.status.last_record = uint32_t(i + 1);
        uint32_t target = work.entries[record.object_id - 1].inode;
        if (!target) { error = -EUCLEAN; break; }
        error = work.writer->link(work.entries[work.entries[i].parent_index].inode, filename(work, record),
                                   target, seconds);
        if (!error) { work.entries[i].inode = target; ++work.status.names_created; }
    }
    if (error) return finish(work, error, status);
    work.status.phase = Ext4ImportPhase::Metadata;
    for (Size i = 0; !error && i < work.plan.count; ++i)
    {
        if (work.records[i].kind == xjpak2::Kind::Hardlink) continue;
        work.status.last_record = uint32_t(i + 1);
        error = metadata(work, i, seconds);
    }
    if (error) return finish(work, error, status);
    work.status.phase = Ext4ImportPhase::Times;
    // Reverse path order places every directory after all of its descendants.
    for (Size i = work.plan.count; !error && i;)
    {
        --i;
        const auto &record = work.records[i];
        if (record.kind == xjpak2::Kind::Hardlink) continue;
        work.status.last_record = uint32_t(i + 1);
        xjext4::AttributeUpdate update = {};
        update.mask = xjext4::kAttributeMtime;
        update.mtime_seconds = record.mtime_seconds; update.mtime_nanoseconds = record.mtime_nsec;
        error = work.writer->set_attributes(work.entries[i].inode, update, seconds);
    }
    if (error) return finish(work, error, status);
    work.status.phase = Ext4ImportPhase::Closing;
    error = work.writer->close();
    if (error) return finish(work, error, status);
    work.status.writer_open = false; work.status.complete = true;
    work.status.phase = Ext4ImportPhase::Complete; work.stopped = true;
    status = work.status;
    return 0;
}

} // namespace xjinstaller
