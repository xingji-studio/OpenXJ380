// SPDX-License-Identifier: Apache-2.0
// Original bounded validator; source is independent of archive libraries.
#include <installer/package.h>

namespace xjpak2 {
namespace {

uint16_t u16(const uint8_t *p)
{
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}

uint32_t u32(const uint8_t *p)
{
    return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16;
}

uint64_t u64(const uint8_t *p)
{
    return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32;
}

bool equal(const uint8_t *a, const char *b, Size count)
{
    for (Size i = 0; i < count; ++i) if (a[i] != uint8_t(b[i])) return false;
    return true;
}

int compare(Span a, Span b)
{
    Size length = a.size < b.size ? a.size : b.size;
    for (Size i = 0; i < length; ++i)
        if (a.data[i] != b.data[i]) return a.data[i] < b.data[i] ? -1 : 1;
    return a.size < b.size ? -1 : a.size != b.size;
}

bool prefix(Span value, const char *text, Size length)
{
    return value.size > length && equal(value.data, text, length);
}

bool canonical_path(Span path, const Limits &limits)
{
    if (!path.size || path.size > limits.max_path || path.data[0] != '/') return false;
    if (path.size == 1) return true;
    if (path.data[path.size - 1] == '/') return false;
    Size start = 1;
    unsigned depth = 0;
    for (Size i = 1; i <= path.size; ++i)
    {
        if (i < path.size && !path.data[i]) return false;
        if (i != path.size && path.data[i] != '/') continue;
        Size length = i - start;
        if (!length || length > 255 || (length == 1 && path.data[start] == '.') ||
            (length == 2 && path.data[start] == '.' && path.data[start + 1] == '.')) return false;
        if (++depth > limits.max_depth) return false;
        start = i + 1;
    }
    return true;
}

bool opaque_target(Span target, const Limits &limits)
{
    if (!target.size || target.size > limits.max_path) return false;
    for (Size i = 0; i < target.size; ++i) if (!target.data[i]) return false;
    return true;
}

int attributes(const Record &record, const Limits &limits)
{
    if (record.xattr_count > limits.max_xattrs) return TooBig;
    Size offset = 0;
    uint64_t storage = 0;
    Span previous = {};
    for (unsigned i = 0; i < record.xattr_count; ++i)
    {
        if (record.xattrs.size - offset < 8) return Invalid;
        const uint8_t *raw = record.xattrs.data + offset;
        unsigned name_size = u16(raw), flags = u16(raw + 2);
        uint32_t value_size = u32(raw + 4);
        offset += 8;
        if (!name_size || name_size > 255 || flags || name_size > record.xattrs.size - offset) return Invalid;
        Span name = {record.xattrs.data + offset, name_size};
        offset += name_size;
        if (value_size > record.xattrs.size - offset) return Invalid;
        offset += value_size;
        for (Size j = 0; j < name.size; ++j) if (!name.data[j]) return Invalid;
        if (i && compare(previous, name) >= 0) return Invalid;
        previous = name;
        // Storage permission is distinct from ACL/capability enforcement.
        // ACL and every unknown/system namespace remain unsupported imports.
        bool allowed = prefix(name, "user.", 5) ||
                       (limits.allow_trusted && prefix(name, "trusted.", 8)) ||
                       (limits.allow_security && prefix(name, "security.", 9));
        if (!allowed) return Unsupported;
        if (value_size > limits.max_xattr_storage) return TooBig;
        uint64_t weight = 16 + ((uint64_t(name_size) + 3) & ~uint64_t(3)) +
                          ((uint64_t(value_size) + 3) & ~uint64_t(3));
        if (weight > limits.max_xattr_storage || storage > limits.max_xattr_storage - weight) return TooBig;
        storage += weight;
    }
    return offset == record.xattrs.size ? 0 : Invalid;
}

void sift(Record *records, Size size, Size root)
{
    while (root < size / 2)
    {
        Size child = root * 2 + 1;
        if (child + 1 < size && compare(records[child].path, records[child + 1].path) < 0) ++child;
        if (compare(records[root].path, records[child].path) >= 0) return;
        Record swap = records[root];
        records[root] = records[child];
        records[child] = swap;
        root = child;
    }
}

void sort(Record *records, Size count)
{
    for (Size i = count / 2; i; --i) sift(records, count, i - 1);
    for (Size end = count; end > 1; --end)
    {
        Record swap = records[0];
        records[0] = records[end - 1];
        records[end - 1] = swap;
        sift(records, end - 1, 0);
    }
}

const Record *find_parent(const Record *records, Size count, Span key)
{
    Size low = 0, high = count;
    while (low < high)
    {
        Size middle = low + (high - low) / 2;
        int order = compare(records[middle].path, key);
        if (!order) return records + middle;
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    return nullptr;
}

struct CrcTable
{
    uint32_t values[256];
};

constexpr CrcTable make_crc_table()
{
    CrcTable table = {};
    for (unsigned i = 0; i < 256; ++i)
    {
        uint32_t value = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ (0x82f63b78U & (0U - (value & 1U)));
        table.values[i] = value;
    }
    return table;
}

constexpr CrcTable crc_table = make_crc_table();

bool address_range(const void *pointer, Size bytes)
{
    return pointer && uintptr_t(pointer) <= uintptr_t(-1) - bytes;
}

} // namespace

uint32_t crc32c(const uint8_t *bytes, Size size)
{
    uint32_t crc = 0xffffffffU;
    for (Size i = 0; i < size; ++i)
        crc = crc_table.values[(crc ^ bytes[i]) & 255U] ^ (crc >> 8);
    return ~crc;
}

int inspect_header(const uint8_t *package, Size bytes, const Limits &limits, HeaderInfo &info)
{
    info = {};
    if (!address_range(package, bytes) || bytes < 16 || bytes > limits.max_bytes ||
        !limits.max_path || limits.max_path > 4095) return Invalid;
    bool legacy = equal(package, "XJPAK10\0", 8);
    if ((!legacy && !equal(package, "XJPAK20\0", 8)) || (legacy && !limits.allow_v1)) return Unsupported;
    uint64_t count = legacy ? u64(package + 8) : 0;
    uint64_t declared_inodes = 0, declared_data = 0;
    Size offset = legacy ? 16 : 64;
    if (!legacy)
    {
        if (bytes < 64 || u32(package + 8) != 2 || u32(package + 12) != 64 || u64(package + 16) != bytes ||
            u32(package + 48) || u32(package + 52) || crc32c(package, 60) != u32(package + 60)) return Invalid;
        count = u64(package + 24);
        declared_inodes = u64(package + 32);
        declared_data = u64(package + 40);
        if (!count) return Invalid;
    }
    Size minimum_record = legacy ? 17 : 65;
    if (count > limits.max_entries || count > (bytes - offset) / minimum_record ||
        count > Size(-1) / sizeof(Record) || declared_data > limits.max_data_bytes) return TooBig;
    if (declared_inodes > limits.max_inodes) return NoSpace;
    info.record_count = Size(count);
    info.declared_inodes = declared_inodes;
    info.declared_data_bytes = declared_data;
    info.legacy_v1 = legacy;
    return 0;
}

int validate(const uint8_t *package, Size bytes, const Limits &limits, Workspace workspace, Plan &plan)
{
    plan = {};
    HeaderInfo info;
    int header_error = inspect_header(package, bytes, limits, info);
    if (header_error) return header_error;
    const Size count = info.record_count;
    const bool legacy = info.legacy_v1, version2 = !legacy;
    if (count > workspace.capacity) return TooBig;
    // The parser writes its arena before later records are validated. Reject
    // package overlap and wrapped/misaligned arenas before the first arena write.
    if (count)
    {
        Size arena_bytes = count * sizeof(Record);
        if (!address_range(workspace.records, arena_bytes) || uintptr_t(workspace.records) % alignof(Record))
            return Invalid;
        uintptr_t arena_start = uintptr_t(workspace.records), package_start = uintptr_t(package);
        if (arena_start < package_start + bytes && package_start < arena_start + arena_bytes) return Invalid;
    }
    Size offset = legacy ? 16 : 64;
    if (!legacy && crc32c(package + 64, bytes - 64) != u32(package + 56)) return Invalid;
    uint64_t data_total = 0;
    for (uint64_t i = 0; i < count; ++i)
    {
        Record record = {};
        Size header = legacy ? 16 : 64;
        if (bytes - offset < header) return Invalid;
        const uint8_t *raw = package + offset;
        uint64_t record_size = legacy ? 0 : u64(raw);
        uint32_t path_size = legacy ? u16(raw) : u32(raw + 8);
        uint64_t payload_size = legacy ? u64(raw + 8) : u64(raw + 24);
        uint32_t xattr_size = legacy ? 0 : u32(raw + 56);
        record.kind = Kind(legacy ? u16(raw + 2) : u16(raw + 12));
        record.mode = u32(raw + (legacy ? 4 : 32));
        record.metadata = legacy ? Mode : Mode | Owner | Mtime | Xattrs;
        record.object_id = legacy ? i + 1 : u64(raw + 16);
        record.xattr_count = legacy ? 0 : u16(raw + 14);
        if (!legacy)
        {
            record.uid = u32(raw + 36);
            record.gid = u32(raw + 40);
            record.mtime_nsec = u32(raw + 44);
            uint64_t bits = u64(raw + 48);
            // Decode signed LE seconds without an out-of-range unsigned to
            // signed conversion. Keep subsecond precision in its own field.
            int64_t seconds = bits & (1ULL << 63) ? -1 - int64_t(~bits) : int64_t(bits);
            if (seconds < -0x80000000LL || seconds > 0x37fffffffLL ||
                record.mtime_nsec >= 1000000000U || u32(raw + 60)) return Invalid;
            record.mtime_seconds = seconds;
        }
        if (record.mode & ~07777U) return Invalid;
        offset += header;
        if (path_size > bytes - offset) return Invalid;
        record.path = {package + offset, path_size};
        offset += path_size;
        if (!canonical_path(record.path, limits)) return Invalid;
        if (payload_size > bytes - offset ||
            (record.kind == Kind::Regular && payload_size > limits.max_file_bytes)) return TooBig;
        record.payload = {package + offset, Size(payload_size)};
        offset += Size(payload_size);
        if (xattr_size > bytes - offset) return Invalid;
        record.xattrs = {package + offset, xattr_size};
        offset += xattr_size;
        if (!legacy && (record_size < header || record_size != header + uint64_t(path_size) + payload_size +
                                                                     uint64_t(xattr_size))) return Invalid;
        if (record.kind == Kind::Regular)
        {
            if (payload_size > limits.max_data_bytes || data_total > limits.max_data_bytes - payload_size)
                return TooBig;
            data_total += payload_size;
        }
        else if (record.kind == Kind::Directory)
        {
            if (payload_size) return Invalid;
        }
        else if (!legacy && record.kind == Kind::Symlink)
        {
            // Never dereference, normalize or classify link target contents.
            if (!opaque_target(record.payload, limits)) return Invalid;
        }
        else if (!legacy && record.kind == Kind::Hardlink)
        {
            if (payload_size || record.mode || record.uid || record.gid || record.mtime_seconds ||
                record.mtime_nsec || record.xattr_count || xattr_size) return Invalid;
            record.metadata = 0;
        }
        else return Unsupported;
        if (version2 && record.kind != Kind::Hardlink && record.object_id != i + 1) return Invalid;
        int error = attributes(record, limits);
        if (error) return error;
        workspace.records[i] = record;
    }
    if (offset != bytes) return Invalid;
    if (legacy) sort(workspace.records, Size(count));
    uint64_t inodes = legacy ? 1 : 0;
    bool explicit_root = false;
    for (Size i = 0; i < count; ++i)
    {
        Record &record = workspace.records[i];
        if (i && compare(workspace.records[i - 1].path, record.path) >= 0) return Invalid;
        if (record.path.size == 1)
        {
            if (i || record.kind != Kind::Directory) return Invalid;
            explicit_root = true;
        }
        else
        {
            Size parent_size = record.path.size;
            while (parent_size > 1 && record.path.data[parent_size - 1] != '/') --parent_size;
            if (parent_size > 1) --parent_size;
            const Record *parent = find_parent(workspace.records, i, {record.path.data, parent_size});
            if (!parent && !(legacy && parent_size == 1)) return Invalid;
            if (parent && parent->kind != Kind::Directory) return Invalid;
        }
        if (record.kind == Kind::Hardlink)
        {
            if (!record.object_id || record.object_id > count) return Invalid;
            Kind target = workspace.records[record.object_id - 1].kind;
            if (target == Kind::Hardlink) return Loop;
            if (target != Kind::Regular && target != Kind::Symlink) return Invalid;
        }
        else if (!legacy || record.path.size != 1) ++inodes;
    }
    if (!legacy && (!explicit_root || inodes != info.declared_inodes || data_total != info.declared_data_bytes))
        return Invalid;
    if (inodes > limits.max_inodes) return NoSpace;
    plan.records = workspace.records;
    plan.count = Size(count);
    plan.inode_count = inodes;
    plan.data_bytes = data_total;
    plan.legacy_v1 = legacy;
    plan.validated = true;
    return 0;
}

} // namespace xjpak2
