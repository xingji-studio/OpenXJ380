// SPDX-License-Identifier: Apache-2.0
// Original xattr codec and journaled mutations. Only on-disk format facts are
// used: https://www.kernel.org/doc/html/latest/filesystems/ext4/attributes.html
#include "write.h"
#include <errno.h>

namespace xjext4 {
namespace {

constexpr uint32_t kMagic = 0xea020000U;
constexpr unsigned kMaximumEntries = 512;
constexpr uint32_t kMaximumReferences = 1024;

uint16_t get16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t get32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void set16(uint8_t *p, uint16_t value) { p[0] = uint8_t(value); p[1] = uint8_t(value >> 8); }
void set32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
}

void clear(void *output, Size bytes)
{
    for (Size i = 0; i < bytes; ++i) static_cast<uint8_t *>(output)[i] = 0;
}

void copy(void *output, const void *input, Size bytes)
{
    for (Size i = 0; i < bytes; ++i)
        static_cast<uint8_t *>(output)[i] = static_cast<const uint8_t *>(input)[i];
}

bool equal(const void *first, const void *second, Size bytes)
{
    for (Size i = 0; i < bytes; ++i)
        if (static_cast<const uint8_t *>(first)[i] != static_cast<const uint8_t *>(second)[i]) return false;
    return true;
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

uint32_t rounded(uint32_t bytes) { return (bytes + 3U) & ~3U; }

struct Attribute
{
    const uint8_t *name;
    const uint8_t *value;
    uint32_t length;
    uint8_t index;
    uint8_t name_length;
    bool external;
};

struct Attributes
{
    Attribute entries[kMaximumEntries];
    unsigned count;
};

bool same_key(const Attribute &a, const Attribute &b)
{
    return a.index == b.index && a.name_length == b.name_length && equal(a.name, b.name, a.name_length);
}

int compare_key(const Attribute &a, const Attribute &b)
{
    if (a.index != b.index) return a.index < b.index ? -1 : 1;
    if (a.name_length != b.name_length) return a.name_length < b.name_length ? -1 : 1;
    for (unsigned i = 0; i < a.name_length; ++i)
        if (a.name[i] != b.name[i]) return a.name[i] < b.name[i] ? -1 : 1;
    return 0;
}

uint32_t entry_hash(const Attribute &entry)
{
    uint32_t result = 0;
    for (unsigned i = 0; i < entry.name_length; ++i)
        result = (result << 5) ^ (result >> 27) ^ entry.name[i];
    for (uint32_t offset = 0; offset < entry.length; offset += 4)
    {
        uint32_t word = 0;
        for (unsigned i = 0; i < 4 && offset + i < entry.length; ++i)
            word |= uint32_t(entry.value[offset + i]) << (8 * i);
        result = (result << 16) ^ (result >> 16) ^ word;
    }
    return result;
}

uint32_t block_checksum(uint32_t seed, uint64_t physical, const uint8_t *bytes, uint32_t size)
{
    uint8_t number[8];
    for (unsigned i = 0; i < 8; ++i) number[i] = uint8_t(physical >> (8 * i));
    uint32_t result = checksum(seed, number, sizeof(number));
    result = checksum(result, bytes, 16);
    const uint8_t zero[4] = {};
    result = checksum(result, zero, sizeof(zero));
    return checksum(result, bytes + 20, size - 20);
}

int body_position(const uint8_t *raw, uint32_t inode_size, uint32_t &position)
{
    position = inode_size;
    if (inode_size == 128) return 0;
    uint16_t extra = get16(raw + 0x80);
    if (extra % 4 || uint32_t(extra) + 128 > inode_size) return -EUCLEAN;
    position = 128 + extra;
    return 0;
}

// Offsets in an inode body are relative to its first entry, whereas offsets in
// an external block are relative to the block. No unaligned typed loads occur.
int parse_region(const uint8_t *bytes, uint32_t size, uint32_t entries_offset, uint32_t value_base,
                 bool external, Attributes &attributes)
{
    if (entries_offset > size || size - entries_offset < 4) return -EUCLEAN;
    unsigned first = attributes.count;
    uint32_t position = entries_offset;
    uint32_t aggregate_hash = 0;
    bool hashable = true;
    for (;;)
    {
        if (position > size || size - position < 4) return -EUCLEAN;
        const uint8_t *entry = bytes + position;
        if (!get32(entry)) break;
        if (size - position < 16) return -EUCLEAN;
        uint32_t record = rounded(16U + entry[0]);
        if (record > size - position || attributes.count == kMaximumEntries) return -EUCLEAN;
        if (!entry[0] && !entry[1]) return -EUCLEAN;
        if ((entry[1] == 2 || entry[1] == 3 || entry[1] == 8) && entry[0]) return -EUCLEAN;
        if (get32(entry + 4)) return -EOPNOTSUPP; // EA_INODE/value-inode storage.
        uint32_t offset = get16(entry + 2);
        uint32_t length = get32(entry + 8);
        if (offset % 4 || value_base > size || offset > size - value_base ||
            length > size - value_base - offset) return -EUCLEAN;
        Attribute attribute = {entry + 16, bytes + value_base + offset, length, entry[1], entry[0], external};
        for (unsigned i = 0; i < attribute.name_length; ++i)
            if (!attribute.name[i]) return -EUCLEAN;
        if (external && attributes.count != first &&
            compare_key(attributes.entries[attributes.count - 1], attribute) >= 0) return -EUCLEAN;
        for (unsigned i = 0; i < attributes.count; ++i)
            if (same_key(attributes.entries[i], attribute)) return -EUCLEAN;
        uint32_t hash = entry_hash(attribute);
        if (get32(entry + 12) && get32(entry + 12) != hash) return -EUCLEAN;
        uint32_t stored_hash = get32(entry + 12);
        if (!stored_hash) hashable = false;
        aggregate_hash = (aggregate_hash << 16) ^ (aggregate_hash >> 16) ^ stored_hash;
        attributes.entries[attributes.count++] = attribute;
        position += record;
    }
    uint32_t table_end = position + 4;
    for (unsigned i = first; i < attributes.count; ++i)
    {
        const Attribute &entry = attributes.entries[i];
        if (!entry.length) continue;
        uint32_t offset = uint32_t(entry.value - bytes);
        if (offset < table_end || rounded(entry.length) > size - offset) return -EUCLEAN;
        for (unsigned j = first; j < i; ++j)
        {
            const Attribute &other = attributes.entries[j];
            if (!other.length) continue;
            uint32_t other_offset = uint32_t(other.value - bytes);
            bool overlaps = offset < other_offset + rounded(other.length) &&
                            other_offset < offset + rounded(entry.length);
            // A block may deliberately share one identical value. Partial or
            // differently sized overlaps cannot be safely rebuilt.
            if (overlaps && (offset != other_offset || entry.length != other.length)) return -EUCLEAN;
        }
    }
    if (external && get32(bytes + 12) && get32(bytes + 12) != (hashable ? aggregate_hash : 0)) return -EUCLEAN;
    return 0;
}

int parse_body(const uint8_t *raw, uint32_t inode_size, Attributes &attributes)
{
    uint32_t start;
    int error = body_position(raw, inode_size, start);
    if (error || inode_size - start < 4) return error;
    if (!get32(raw + start)) return 0;
    if (get32(raw + start) != kMagic || inode_size - start < 8) return -EUCLEAN;
    return parse_region(raw, inode_size, start + 4, start + 4, false, attributes);
}

int parse_block(const uint8_t *bytes, uint32_t size, uint32_t seed, uint64_t physical,
                bool checksummed, Attributes &attributes)
{
    if (size < 36 || get32(bytes) != kMagic || !get32(bytes + 4) ||
        get32(bytes + 4) > kMaximumReferences || get32(bytes + 8) != 1) return -EUCLEAN;
    for (unsigned offset = 20; offset < 32; offset += 4)
        if (get32(bytes + offset)) return -EUCLEAN;
    if (checksummed && block_checksum(seed, physical, bytes, size) != get32(bytes + 16)) return -EUCLEAN;
    return parse_region(bytes, size, 32, 0, true, attributes);
}

int name_length(const char *name)
{
    if (!name) return -EINVAL;
    unsigned length = 0;
    while (length <= 255 && name[length]) ++length;
    return length > 255 ? -ERANGE : int(length);
}

struct Name
{
    uint8_t index;
    const char *short_name;
};

bool same_text(const char *a, const char *b)
{
    for (unsigned i = 0; i <= 255; ++i)
    {
        if (a[i] != b[i]) return false;
        if (!a[i]) return true;
    }
    return false;
}

bool starts_with(const char *name, const char *prefix)
{
    for (unsigned i = 0; prefix[i]; ++i) if (!name[i] || name[i] != prefix[i]) return false;
    return true;
}

int decode_name(const char *name, Name &decoded)
{
    int length = name_length(name);
    if (length < 0) return length;
    if (!length) return -EINVAL;
    const char *special[] = {"system.posix_acl_access", "system.posix_acl_default", "system.richacl"};
    const uint8_t special_index[] = {2, 3, 8};
    for (unsigned i = 0; i < 3; ++i)
        if (same_text(name, special[i])) { decoded = {special_index[i], name + length}; return 0; }
    const char *prefixes[] = {"user.", "trusted.", "security.", "system."};
    const uint8_t indices[] = {1, 4, 6, 7};
    for (unsigned i = 0; i < 4; ++i)
    {
        if (!starts_with(name, prefixes[i])) continue;
        unsigned prefix_length = 0;
        while (prefixes[i][prefix_length]) ++prefix_length;
        if (unsigned(length) == prefix_length) return -EINVAL;
        decoded = {indices[i], name + prefix_length};
        return 0;
    }
    decoded = {0, name};
    return 0;
}

const char *prefix_for(uint8_t index)
{
    switch (index)
    {
    case 0: return "";
    case 1: return "user.";
    case 2: return "system.posix_acl_access";
    case 3: return "system.posix_acl_default";
    case 4: return "trusted.";
    case 6: return "security.";
    case 7: return "system.";
    case 8: return "system.richacl";
    default: return nullptr;
    }
}

struct FindAttribute
{
    const char *name;
    uint8_t index;
    void *value;
    Size capacity;
    Size required;
    bool found;
};

int find_attribute(void *context, uint8_t index, const char *name, const void *value, Size length)
{
    auto &find = *static_cast<FindAttribute *>(context);
    if (index != find.index || !same_text(name, find.name)) return 0;
    find.found = true;
    find.required = length;
    if (find.capacity && find.capacity < length) return -ERANGE;
    if (find.value && find.capacity) copy(find.value, value, length);
    return 1;
}

struct ListAttributes
{
    char *output;
    Size capacity;
    Size required;
    bool copy_values;
};

int list_attribute(void *context, uint8_t index, const char *name, const void *, Size)
{
    auto &list = *static_cast<ListAttributes *>(context);
    const char *prefix = prefix_for(index);
    if (!prefix) return 0; // Unknown namespaces remain available through raw APIs.
    Size prefix_length = 0, short_length = 0;
    while (prefix[prefix_length]) ++prefix_length;
    while (name[short_length]) ++short_length;
    // Imported/raw keys can collide with another namespace's textual name
    // (for example index zero "user.foo"), or exceed the public name limit
    // after adding their prefix. Only expose names the standard getter can
    // decode back to this exact key; raw enumeration still exposes every key.
    if (prefix_length + short_length > 255) return 0;
    char full_name[256];
    copy(full_name, prefix, prefix_length);
    copy(full_name + prefix_length, name, short_length + 1);
    Name decoded;
    if (decode_name(full_name, decoded) || decoded.index != index || !same_text(decoded.short_name, name)) return 0;
    Size bytes = prefix_length + short_length + 1;
    if (bytes > ~Size(0) - list.required) return -EOVERFLOW;
    if (list.copy_values)
    {
        if (bytes > list.capacity - list.required) return -ERANGE;
        copy(list.output + list.required, full_name, bytes);
    }
    list.required += bytes;
    return 0;
}

uint32_t attribute_bytes(const Attribute &entry)
{
    return rounded(16U + entry.name_length) + rounded(entry.length);
}

// A small bounded subset-sum chooses which entries occupy the inode body.
// Greedy packing can incorrectly report ENOSPC when swapping one entry would
// let the complete attribute set fit across the body and external block.
int choose_body(const Attributes &attributes, uint32_t body_capacity, uint32_t block_size, bool *in_body)
{
    uint32_t available = body_capacity >= 8 ? body_capacity - 8 : 0;
    uint32_t external = block_size - 36;
    uint32_t total = 0;
    for (unsigned i = 0; i < attributes.count; ++i)
    {
        in_body[i] = false;
        uint32_t bytes = attribute_bytes(attributes.entries[i]);
        if (bytes > available && bytes > external) return -ENOSPC;
        if (bytes > available + external - total) return -ENOSPC;
        total += bytes;
    }
    unsigned capacity = available / 4;
    int16_t item[1025], previous[1025];
    if (capacity > 1024) return -EUCLEAN;
    for (unsigned i = 0; i <= capacity; ++i) item[i] = previous[i] = -1;
    item[0] = -2;
    for (unsigned i = 0; i < attributes.count; ++i)
    {
        unsigned weight = attribute_bytes(attributes.entries[i]) / 4;
        if (weight > capacity) continue;
        for (unsigned value = capacity + 1; value-- > weight;)
        {
            if (item[value] != -1 || item[value - weight] == -1) continue;
            item[value] = int16_t(i);
            previous[value] = int16_t(value - weight);
        }
    }
    unsigned selected = capacity;
    while (selected && item[selected] == -1) --selected;
    if (total - selected * 4 > external) return -ENOSPC;
    while (selected)
    {
        if (item[selected] < 0 || previous[selected] < 0 || unsigned(previous[selected]) >= selected)
            return -EUCLEAN;
        in_body[unsigned(item[selected])] = true;
        selected = unsigned(previous[selected]);
    }
    return 0;
}

unsigned region_count(const Attributes &attributes, const bool *in_body, bool external)
{
    unsigned count = 0;
    for (unsigned i = 0; i < attributes.count; ++i) count += in_body[i] != external;
    return count;
}

void pack_region(uint8_t *bytes, uint32_t size, uint32_t start, bool external,
                 const Attributes &attributes, const bool *in_body)
{
    uint32_t position = start + (external ? 32U : 4U);
    uint32_t end = size;
    set32(bytes + start, kMagic);
    uint32_t aggregate_hash = 0;
    bool hashable = true;
    for (unsigned i = 0; i < attributes.count; ++i)
    {
        if (in_body[i] == external) continue;
        const Attribute &attribute = attributes.entries[i];
        uint8_t *entry = bytes + position;
        entry[0] = attribute.name_length;
        entry[1] = attribute.index;
        if (attribute.length)
        {
            end -= rounded(attribute.length);
            copy(bytes + end, attribute.value, attribute.length);
            set16(entry + 2, uint16_t(external ? end : end - (start + 4)));
        }
        set32(entry + 8, attribute.length);
        copy(entry + 16, attribute.name, attribute.name_length);
        uint32_t hash = entry_hash(attribute);
        if (external) set32(entry + 12, hash);
        if (!hash) hashable = false;
        aggregate_hash = (aggregate_hash << 16) ^ (aggregate_hash >> 16) ^ hash;
        position += rounded(16U + attribute.name_length);
    }
    if (external)
    {
        set32(bytes + 4, 1);
        set32(bytes + 8, 1);
        set32(bytes + 12, hashable ? aggregate_hash : 0);
    }
}

bool unchanged_external(const Attributes &original, const Attributes &attributes, const bool *in_body)
{
    unsigned count = 0;
    for (unsigned i = 0; i < original.count; ++i) count += original.entries[i].external;
    if (count != region_count(attributes, in_body, true)) return false;
    for (unsigned i = 0; i < original.count; ++i)
    {
        const Attribute &old = original.entries[i];
        if (!old.external) continue;
        bool found = false;
        for (unsigned j = 0; j < attributes.count; ++j)
        {
            const Attribute &next = attributes.entries[j];
            if (in_body[j] || !same_key(old, next)) continue;
            found = old.length == next.length && equal(old.value, next.value, old.length);
            break;
        }
        if (!found) return false;
    }
    return true;
}

int adjust_inode_blocks(uint8_t *raw, int delta, uint32_t block_size, uint32_t readonly_features)
{
    uint64_t count = get32(raw + 0x1c);
    if (readonly_features & 8U) count |= uint64_t(get16(raw + 0x74)) << 32;
    int64_t change = int64_t(delta) * ((get32(raw + 0x20) & 0x40000U) ? 1 : block_size / 512);
    int64_t next = int64_t(count) + change;
    uint64_t maximum = readonly_features & 8U ? 0xffffffffffffULL : 0xffffffffULL;
    if (next < 0 || uint64_t(next) > maximum) return -EUCLEAN;
    set32(raw + 0x1c, uint32_t(next));
    if (readonly_features & 8U) set16(raw + 0x74, uint16_t(uint64_t(next) >> 32));
    return 0;
}

uint64_t xattr_pointer(const uint8_t *raw)
{
    return get32(raw + 0x68) | uint64_t(get16(raw + 0x76)) << 32;
}

void set_xattr_pointer(uint8_t *raw, uint64_t physical)
{
    set32(raw + 0x68, uint32_t(physical));
    set16(raw + 0x76, uint16_t(physical >> 32));
}

int inode_checksum(const uint8_t *raw, uint32_t bytes, uint32_t number, uint32_t seed, bool checksummed)
{
    uint32_t start;
    int error = body_position(raw, bytes, start);
    if (error || !checksummed) return error;
    uint8_t numbers[8];
    set32(numbers, number);
    copy(numbers + 4, raw + 0x64, 4);
    uint32_t result = checksum(seed, numbers, 8);
    result = checksum(result, raw, 0x7c);
    const uint8_t zero[2] = {};
    result = checksum(result, zero, 2);
    bool high = bytes >= 132 && get16(raw + 0x80) >= 4;
    uint32_t expected = get16(raw + 0x7c);
    if (high)
    {
        result = checksum(result, raw + 0x7e, 4);
        result = checksum(result, zero, 2);
        result = checksum(result, raw + 0x84, bytes - 0x84);
        expected |= uint32_t(get16(raw + 0x82)) << 16;
    }
    else result = checksum(result, raw + 0x7e, bytes - 0x7e) & 0xffffU;
    return result == expected ? 0 : -EUCLEAN;
}

struct RuntimeReference
{
    uint64_t block;
    int32_t delta;
};

} // namespace

int Volume::list_xattr_raw(uint32_t number, XattrVisitor visitor, void *context) const
{
    if (!visitor) return -EINVAL;
    Inode inode;
    int error = read_inode(number, inode);
    if (error) return error;
    uint8_t raw[4096], block[4096];
    error = read_raw_inode(number, raw);
    if (error) return error;
    Attributes attributes = {};
    error = parse_body(raw, inode_size_, attributes);
    if (!error && inode.xattr_block)
    {
        error = fetch(inode.xattr_block * block_size_, block, block_size_);
        if (!error) error = parse_block(block, block_size_, checksum_seed_, inode.xattr_block,
                                        (readonly_ & 0x400U) != 0, attributes);
    }
    if (error) return error;
    if (attributes.count)
    {
        uint8_t compatible[4];
        error = fetch(1024 + 0x5c, compatible, sizeof(compatible));
        if (error) return error;
        if (!(get32(compatible) & 8U)) return -EUCLEAN;
    }
    for (unsigned i = 0; i < attributes.count; ++i)
    {
        const Attribute &attribute = attributes.entries[i];
        char name[256];
        copy(name, attribute.name, attribute.name_length);
        name[attribute.name_length] = 0;
        error = visitor(context, attribute.index, name, attribute.value, attribute.length);
        if (error) return error > 0 ? 0 : error;
    }
    return 0;
}

int Volume::get_xattr_raw(uint32_t number, uint8_t index, const char *name, void *value,
                          Size capacity, Size &required) const
{
    required = 0;
    int length = name_length(name);
    if (length < 0) return length;
    if ((!length && !index) || ((index == 2 || index == 3 || index == 8) && length)) return -EINVAL;
    FindAttribute find = {name, index, value, capacity, 0, false};
    int error = list_xattr_raw(number, find_attribute, &find);
    required = find.required;
    return error ? error : find.found ? 0 : -ENODATA;
}

int Volume::get_xattr(uint32_t number, const char *name, void *value, Size capacity, Size &required) const
{
    required = 0;
    Name decoded;
    int error = decode_name(name, decoded);
    return error ? error : get_xattr_raw(number, decoded.index, decoded.short_name, value, capacity, required);
}

int Volume::list_xattr(uint32_t number, char *names, Size capacity, Size &required) const
{
    required = 0;
    ListAttributes list = {names, capacity, 0, false};
    int error = list_xattr_raw(number, list_attribute, &list);
    required = list.required;
    if (error || !capacity || !names) return error;
    if (capacity < required) return -ERANGE;
    list.required = 0;
    list.copy_values = true;
    return list_xattr_raw(number, list_attribute, &list);
}

int WriteSession::tx_release_xattrs(uint32_t, uint8_t *raw)
{
    uint64_t physical = xattr_pointer(raw);
    if (!physical) return 0;
    if (!(get32(super_ + 0x5c) & 8U)) return -EUCLEAN;
    if (protected_block(physical)) return -EUCLEAN;
    int error = check_target_block(physical);
    uint8_t previous[4096];
    Attributes attributes = {};
    if (!error) error = tx_read(physical, previous);
    if (!error) error = parse_block(previous, volume_.block_size_, volume_.checksum_seed_, physical,
                                    (volume_.readonly_ & 0x400U) != 0, attributes);
    if (error) return error;
    uint32_t references = get32(previous + 4);
    if (references == 1) error = tx_release_block(physical);
    else
    {
        uint8_t *edited = nullptr;
        error = tx_edit(physical, edited);
        if (!error)
        {
            set32(edited + 4, references - 1);
            if (volume_.readonly_ & 0x400U)
                set32(edited + 16, block_checksum(volume_.checksum_seed_, physical, edited, volume_.block_size_));
        }
    }
    if (!error) error = adjust_inode_blocks(raw, -1, volume_.block_size_, volume_.readonly_);
    if (!error) set_xattr_pointer(raw, 0);
    return error;
}

int WriteSession::mutate_xattr(uint32_t number, uint8_t index, const char *name, const void *value,
                                Size length, unsigned flags, bool remove, uint64_t seconds)
{
    if (!active_) return -EINVAL;
    if (failed_ || journal_.failed()) return stop(-EIO);
    if (number != 2 && number < first_inode_) return -EPERM;
    if (seconds > 0x37fffffffULL) return -EOVERFLOW;
    if ((flags & ~3U) || flags == 3 || (!value && length)) return -EINVAL;
    int namesize = name_length(name);
    if (namesize < 0) return namesize;
    if ((!namesize && !index) || ((index == 2 || index == 3 || index == 8) && namesize)) return -EINVAL;
    if (length > volume_.block_size_) return -ENOSPC;
    Inode inode;
    int error = volume_.read_inode(number, inode);
    if (error) return stop(error);
    if (inode.flags & (0x10U | 0x20U)) return -EPERM;
    error = tx_begin();
    uint8_t *raw = nullptr;
    if (!error) error = tx_inode(number, raw);
    uint8_t old_block[4096], body[4096], external[4096];
    Attributes attributes = {};
    if (!error) error = parse_body(raw, volume_.inode_size_, attributes);
    if (!error && inode.xattr_block)
    {
        error = protected_block(inode.xattr_block) ? -EUCLEAN : check_target_block(inode.xattr_block);
        if (!error) error = tx_read(inode.xattr_block, old_block);
        if (!error) error = parse_block(old_block, volume_.block_size_, volume_.checksum_seed_, inode.xattr_block,
                                        (volume_.readonly_ & 0x400U) != 0, attributes);
    }
    if (!error && (attributes.count || inode.xattr_block) && !(get32(super_ + 0x5c) & 8U)) error = -EUCLEAN;
    Attributes original = attributes;
    Attribute wanted = {reinterpret_cast<const uint8_t *>(name), static_cast<const uint8_t *>(value),
                        uint32_t(length), index, uint8_t(namesize), false};
    unsigned found = attributes.count;
    for (unsigned i = 0; i < attributes.count; ++i)
        if (same_key(attributes.entries[i], wanted)) { found = i; break; }
    if (!error && (remove || (flags & 2U)) && found == attributes.count) error = -ENODATA;
    if (!error && (flags & 1U) && found != attributes.count) error = -EEXIST;
    if (!error)
    {
        if (remove)
        {
            for (unsigned i = found + 1; i < attributes.count; ++i) attributes.entries[i - 1] = attributes.entries[i];
            --attributes.count;
        }
        else if (found < attributes.count) attributes.entries[found] = wanted;
        else if (attributes.count == kMaximumEntries) error = -ENOSPC;
        else attributes.entries[attributes.count++] = wanted;
    }
    // Canonical ordering also gives deterministic body/block placement. The
    // inode body need not be sorted, but one order simplifies comparison.
    if (!error)
        for (unsigned i = 1; i < attributes.count; ++i)
        {
            Attribute entry = attributes.entries[i];
            unsigned destination = i;
            while (destination && compare_key(entry, attributes.entries[destination - 1]) < 0)
            {
                attributes.entries[destination] = attributes.entries[destination - 1];
                --destination;
            }
            attributes.entries[destination] = entry;
        }
    uint32_t body_start = 0;
    bool in_body[kMaximumEntries] = {};
    if (!error) error = body_position(raw, volume_.inode_size_, body_start);
    if (!error) error = choose_body(attributes, volume_.inode_size_ - body_start, volume_.block_size_, in_body);
    unsigned external_count = !error ? region_count(attributes, in_body, true) : 0;
    if (!error)
    {
        copy(body, raw, volume_.inode_size_);
        clear(body + body_start, volume_.inode_size_ - body_start);
        clear(external, volume_.block_size_);
        if (region_count(attributes, in_body, false))
            pack_region(body, volume_.inode_size_, body_start, false, attributes, in_body);
        if (external_count) pack_region(external, volume_.block_size_, 0, true, attributes, in_body);
    }
    uint64_t next_block = inode.xattr_block;
    bool unchanged = !error && inode.xattr_block && external_count && unchanged_external(original, attributes, in_body);
    if (!error && !external_count && inode.xattr_block) error = tx_release_xattrs(number, raw);
    if (!error && external_count && !unchanged)
    {
        bool private_block = inode.xattr_block && get32(old_block + 4) == 1;
        if (!private_block) error = tx_allocate_block(next_block);
        if (!error && inode.xattr_block && !private_block) error = tx_release_xattrs(number, raw);
        if (!error && !private_block) error = adjust_inode_blocks(raw, 1, volume_.block_size_, volume_.readonly_);
        uint8_t *edited = nullptr;
        if (!error) error = tx_edit(next_block, edited);
        if (!error)
        {
            if (volume_.readonly_ & 0x400U)
                set32(external + 16, block_checksum(volume_.checksum_seed_, next_block, external, volume_.block_size_));
            copy(edited, external, volume_.block_size_);
            set_xattr_pointer(raw, next_block);
        }
    }
    if (!error)
    {
        // Preserve fields modified by block allocation/reclamation, then copy
        // only the independently packed inode-body region into the after-image.
        copy(raw + body_start, body + body_start, volume_.inode_size_ - body_start);
        if (attributes.count && !(get32(super_ + 0x5c) & 8U))
        {
            uint8_t *superblock = nullptr;
            error = tx_edit(1024 / volume_.block_size_, superblock);
            if (!error)
            {
                uint8_t *super = superblock + 1024 % volume_.block_size_;
                set32(super + 0x5c, get32(super + 0x5c) | 8U);
            }
        }
    }
    if (!error) error = tx_finish_inode(number, raw, seconds, false);
    if (!error) return tx_commit();
    tx_cancel();
    return stop(error);
}

int WriteSession::set_xattr_raw(uint32_t number, uint8_t index, const char *name, const void *value,
                                Size length, unsigned flags, uint64_t seconds)
{
    return mutate_xattr(number, index, name, value, length, flags, false, seconds);
}

int WriteSession::remove_xattr_raw(uint32_t number, uint8_t index, const char *name, uint64_t seconds)
{
    return mutate_xattr(number, index, name, nullptr, 0, 0, true, seconds);
}

int WriteSession::set_xattr(uint32_t number, const char *name, const void *value, Size length,
                            unsigned flags, uint64_t seconds)
{
    Name decoded;
    int error = decode_name(name, decoded);
    return error ? error : set_xattr_raw(number, decoded.index, decoded.short_name, value, length, flags, seconds);
}

int WriteSession::remove_xattr(uint32_t number, const char *name, uint64_t seconds)
{
    Name decoded;
    int error = decode_name(name, decoded);
    return error ? error : remove_xattr_raw(number, decoded.index, decoded.short_name, seconds);
}

bool WriteSession::tx_is_xattr_block(uint64_t physical) const
{
    return physical < volume_.blocks_ && bit_test(xattr_bits_, physical);
}

int WriteSession::validate_xattrs_afterstate(const Volume &after)
{
    const uint32_t size = volume_.block_size_;
    const bool checksummed = (volume_.readonly_ & 0x400U) != 0;
    const bool recovery = !active_;
    const unsigned image_count = tx_image_count();
    bit_clear(xattr_bits_);
    RuntimeReference references[1024];
    unsigned reference_count = 0;
    uint8_t image[4096], old[4096], external[4096], inode_bits[4096], old_inode_bits[4096], descriptor[4096];
    uint8_t compatible[4];
    int error = after.fetch(1024 + 0x5c, compatible, sizeof(compatible));
    if (error) return error;
    bool attributes_enabled = (get32(compatible) & 8U) != 0;
    bool inspect_owners = false;

    auto mark_reference = [&](uint64_t physical, int delta) -> int
    {
        if (!physical) return 0;
        if (protected_block(physical)) return -EUCLEAN;
        if (!bit_set(xattr_bits_, physical)) return -EUCLEAN;
        if (recovery) { inspect_owners = true; return 0; }
        unsigned index = 0;
        while (index < reference_count && references[index].block != physical) ++index;
        if (index == reference_count)
        {
            if (reference_count == 1024) return -E2BIG;
            references[reference_count++] = {physical, 0};
        }
        references[index].delta += delta;
        return 0;
    };

    auto check_inode_bits = [&](const Volume &volume, uint32_t group, uint8_t *bits) -> int
    {
        int result = volume.read_group(group, descriptor);
        if (result) return result;
        if (get16(descriptor + 0x12) & 1U) { clear(bits, size); return 0; }
        uint64_t physical = get32(descriptor + 4);
        uint32_t expected = get16(descriptor + 0x1a);
        if (volume.incompat_ & 0x80U)
        {
            physical |= uint64_t(get32(descriptor + 0x24)) << 32;
            expected |= uint32_t(get16(descriptor + 0x3a)) << 16;
        }
        result = volume.fetch(physical * size, bits, size);
        if (result) return result;
        if (checksummed)
        {
            uint32_t actual = checksum(volume.checksum_seed_, bits, volume.inodes_per_group_ / 8);
            if (volume.descriptor_size_ == 32) actual &= 0xffffU;
            if (actual != expected) return -EUCLEAN;
        }
        return 0;
    };

    // Identify EA header images even when the corresponding inode home copy
    // already contains its new pointer. This is only a trigger for recovery's
    // owner recount: arbitrary file data that resembles a header is not thereby
    // classified as attribute storage.
    if (recovery)
        for (unsigned i = 0; i < image_count; ++i)
        {
            if (!tx_image_read(i, image)) return -EIO;
            if (get32(image) == kMagic && get32(image + 8) == 1 &&
                get32(image + 4) && get32(image + 4) <= kMaximumReferences) inspect_owners = true;
        }

    for (uint32_t group = 0; group < volume_.groups_; ++group)
    {
        uint64_t table_blocks = (uint64_t(volume_.inodes_per_group_) * volume_.inode_size_ + size - 1) / size;
        bool updated = false;
        for (unsigned i = 0; i < image_count && !updated; ++i)
        {
            uint64_t physical = tx_image_block(i);
            updated = physical >= groups_[group].inode_table &&
                      physical - groups_[group].inode_table < table_blocks;
        }
        if (!updated) continue;
        error = check_inode_bits(after, group, inode_bits);
        if (!error && !recovery) error = check_inode_bits(volume_, group, old_inode_bits);
        if (error) return error;
        for (unsigned i = 0; i < image_count; ++i)
        {
            uint64_t physical = tx_image_block(i);
            if (physical < groups_[group].inode_table || physical - groups_[group].inode_table >= table_blocks)
                continue;
            if (!tx_image_read(i, image) ||
                !device_.read(device_.context, physical * size, old, size)) return -EIO;
            uint32_t first = uint32_t((physical - groups_[group].inode_table) * (size / volume_.inode_size_));
            for (uint32_t offset = 0; offset < size; offset += volume_.inode_size_)
            {
                // A private external block may change while its inode pointer,
                // body and second-resolution ctime remain byte-identical. Its
                // unchanged owner in this inode-table image is still evidence
                // for that EA journal target.
                uint32_t bit = first + offset / volume_.inode_size_;
                uint64_t number64 = uint64_t(group) * volume_.inodes_per_group_ + bit + 1;
                if (number64 > volume_.inodes_) continue;
                uint32_t number = uint32_t(number64);
                if (!recovery && (old_inode_bits[bit / 8] & (1U << (bit % 8))))
                {
                    error = mark_reference(xattr_pointer(old + offset), -1);
                    if (error) return error;
                }
                if (!(inode_bits[bit / 8] & (1U << (bit % 8)))) continue;
                const uint8_t *raw = image + offset;
                uint64_t pointer = xattr_pointer(raw);
                error = mark_reference(pointer, 1);
                if (error) return error;
                Attributes attributes = {};
                error = parse_body(raw, volume_.inode_size_, attributes);
                if (!error && pointer)
                {
                    error = after.fetch(pointer * size, external, size);
                    if (!error) error = parse_block(external, size, volume_.checksum_seed_, pointer,
                                                    checksummed, attributes);
                }
                if (error) return error;
                if (attributes.count || pointer)
                {
                    if (!attributes_enabled) return -EUCLEAN;
                    error = inode_checksum(raw, volume_.inode_size_, number, volume_.checksum_seed_, checksummed);
                    if (error) return error;
                    if (recovery) inspect_owners = true;
                }
            }
        }
    }

    uint32_t cached_group = 0xffffffffU;
    const Volume *cached_volume = nullptr;
    uint8_t block_bits[4096];
    auto allocated = [&](const Volume &volume, uint64_t physical, bool &present) -> int
    {
        present = false;
        if (physical < volume.first_data_block_ || physical >= volume.blocks_) return -EUCLEAN;
        uint32_t group = uint32_t((physical - volume.first_data_block_) / volume.blocks_per_group_);
        if (cached_group != group || cached_volume != &volume)
        {
            int result = volume.read_group(group, descriptor);
            if (result) return result;
            if (get16(descriptor + 0x12) & 2U) return 0;
            uint64_t bitmap = get32(descriptor);
            uint32_t expected = get16(descriptor + 0x18);
            if (volume.incompat_ & 0x80U)
            {
                bitmap |= uint64_t(get32(descriptor + 0x20)) << 32;
                expected |= uint32_t(get16(descriptor + 0x38)) << 16;
            }
            result = volume.fetch(bitmap * size, block_bits, size);
            if (result) return result;
            if (checksummed)
            {
                uint32_t actual = checksum(volume.checksum_seed_, block_bits, volume.blocks_per_group_ / 8);
                if (volume.descriptor_size_ == 32) actual &= 0xffffU;
                if (actual != expected) return -EUCLEAN;
            }
            cached_group = group;
            cached_volume = &volume;
        }
        uint32_t bit = uint32_t((physical - volume.first_data_block_) % volume.blocks_per_group_);
        present = (block_bits[bit / 8] & (1U << (bit % 8))) != 0;
        return 0;
    };

    if (!recovery)
    {
        for (unsigned i = 0; i < reference_count; ++i)
        {
            uint64_t physical = references[i].block;
            bool before_allocated = false, after_allocated = false;
            error = allocated(volume_, physical, before_allocated);
            if (!error) error = allocated(after, physical, after_allocated);
            if (error) return error;
            uint32_t before_references = 0;
            if (before_allocated)
            {
                if (!device_.read(device_.context, physical * size, old, size)) return -EIO;
                Attributes parsed = {};
                error = parse_block(old, size, volume_.checksum_seed_, physical, checksummed, parsed);
                if (error) return error;
                before_references = get32(old + 4);
            }
            int64_t expected = int64_t(before_references) + references[i].delta;
            if (expected < 0 || expected > kMaximumReferences) return -EUCLEAN;
            if (!expected)
            {
                if (after_allocated) return -EUCLEAN;
                if (!bit_set(xattr_bits_, physical, false)) return -EUCLEAN;
                continue;
            }
            if (!after_allocated) return -EUCLEAN;
            error = after.fetch(physical * size, external, size);
            Attributes parsed = {};
            if (!error) error = parse_block(external, size, volume_.checksum_seed_, physical, checksummed, parsed);
            if (error) return error;
            if (get32(external + 4) != uint32_t(expected)) return -EUCLEAN;
            if (before_references > 1 || (before_references && expected > 1))
                for (uint32_t byte = 0; byte < size; ++byte)
                    if (!(byte >= 4 && byte < 8) && !(byte >= 16 && byte < 20) && old[byte] != external[byte])
                        return -EUCLEAN;
        }
        return 0;
    }
    if (!inspect_owners) return 0;

    // Home copies may be any checkpoint prefix. Preserve the global census:
    // first collect every live EA block, then count exact owners in sorted
    // bounded batches. No allocation scales with physical block count.
    bit_clear(xattr_bits_);
    auto scan_owners = [&](unsigned batch, bool collect) -> int
    {
        for (uint32_t group = 0; group < volume_.groups_; ++group)
        {
            int result = check_inode_bits(after, group, inode_bits);
            if (result) return result;
            uint64_t group_first = uint64_t(group) * after.inodes_per_group_;
            uint32_t count = group_first >= after.inodes_ ? 0 : uint32_t(after.inodes_ - group_first);
            if (count > after.inodes_per_group_) count = after.inodes_per_group_;
            uint32_t inodes_per_block = size / after.inode_size_;
            for (uint32_t first = 0; first < count; first += inodes_per_block)
            {
                bool used = false;
                for (uint32_t bit = first; bit < count && bit - first < inodes_per_block; ++bit)
                    used |= (inode_bits[bit / 8] & (1U << (bit % 8))) != 0;
                if (!used) continue;
                result = after.fetch(groups_[group].inode_table * size + uint64_t(first) * after.inode_size_,
                                      image, size);
                if (result) return result;
                for (uint32_t index = 0; index < inodes_per_block && first + index < count; ++index)
                {
                    uint32_t bit = first + index;
                    if (!(inode_bits[bit / 8] & (1U << (bit % 8)))) continue;
                    const uint8_t *raw = image + index * after.inode_size_;
                    uint64_t physical = xattr_pointer(raw);
                    if (!physical) continue;
                    if (protected_block(physical) || !attributes_enabled) return -EUCLEAN;
                    uint32_t number = uint32_t(group_first + bit + 1);
                    result = inode_checksum(raw, after.inode_size_, number, after.checksum_seed_, checksummed);
                    if (result) return result;
                    if (collect)
                    {
                        if (!bit_set(xattr_bits_, physical)) return -EUCLEAN;
                        continue;
                    }
                    unsigned low = 0, high = batch;
                    while (low < high)
                    {
                        unsigned middle = low + (high - low) / 2;
                        if (recovery_references_[middle].block < physical) low = middle + 1;
                        else high = middle;
                    }
                    if (low == batch || recovery_references_[low].block != physical) continue;
                    if (recovery_references_[low].count == kMaximumReferences) return -EUCLEAN;
                    ++recovery_references_[low].count;
                }
            }
        }
        return 0;
    };
    error = scan_owners(0, true);
    if (error) return error;
    uint64_t next = 0, physical;
    while (bit_next(xattr_bits_, next, physical))
    {
        unsigned batch = 0;
        do
        {
            recovery_references_[batch++] = {physical, 0};
            next = physical + 1;
        } while (batch < 1024 && bit_next(xattr_bits_, next, physical));
        error = scan_owners(batch, false);
        if (error) return error;
        for (unsigned index = 0; index < batch; ++index)
        {
            physical = recovery_references_[index].block;
            bool present;
            error = allocated(after, physical, present);
            if (error) return error;
            if (!present) return -EUCLEAN;
            error = after.fetch(physical * size, external, size);
            Attributes parsed = {};
            if (!error) error = parse_block(external, size, after.checksum_seed_, physical, checksummed, parsed);
            if (error) return error;
            if (get32(external + 4) != recovery_references_[index].count) return -EUCLEAN;
        }
    }
    return 0;
}

} // namespace xjext4
