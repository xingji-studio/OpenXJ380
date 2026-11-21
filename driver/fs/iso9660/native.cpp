// SPDX-License-Identifier: Apache-2.0
#include "native.h"
#include <errno.h>

namespace xjiso {
namespace {
bool equal(const uint8_t *a, const uint8_t *b, unsigned size)
{
    for (unsigned i = 0; i < size; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

bool both16(const uint8_t *p, uint16_t &value)
{
    value = uint16_t(p[0]) | uint16_t(p[1]) << 8;
    return value == (uint16_t(p[3]) | uint16_t(p[2]) << 8);
}

bool both32(const uint8_t *p, uint32_t &value)
{
    value = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    return value == (uint32_t(p[7]) | uint32_t(p[6]) << 8 | uint32_t(p[5]) << 16 | uint32_t(p[4]) << 24);
}

bool leap(unsigned year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

uint64_t timestamp(const uint8_t *p)
{
    unsigned                  year = 1900 + p[0], month = p[1], day = p[2];
    static constexpr unsigned Days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int                       zone   = int(int8_t(p[6]));
    if (year < 1970 || !month || month > 12 || !day || day > Days[month - 1] + unsigned(month == 2 && leap(year)) ||
        p[3] > 23 || p[4] > 59 || p[5] > 59 || zone < -48 || zone > 52)
        return 0;
    uint64_t days = 0;
    for (unsigned y = 1970; y < year; ++y)
        days += leap(y) ? 366 : 365;
    for (unsigned m = 1; m < month; ++m)
        days += Days[m - 1] + unsigned(m == 2 && leap(year));
    int64_t seconds = int64_t((days + day - 1) * 86400 + p[3] * 3600 + p[4] * 60 + p[5]) - zone * 900;
    return seconds < 0 ? 0 : uint64_t(seconds) * 1000000000ULL;
}

int parse_record(const uint8_t *p, unsigned available, uint64_t volume_bytes, Inode &inode)
{
    unsigned length = p[0];
    if (length < 34 || length > available || !p[32] || 33U + p[32] > length ||
        ((p[32] & 1) == 0 && 34U + p[32] > length) || (p[25] & 0x60))
        return -EIO;
    uint32_t block, size;
    uint16_t sequence;
    if (!both32(p + 2, block) || !both32(p + 10, size) || !both16(p + 28, sequence)) return -EIO;
    if (sequence != 1 || p[26] || p[27] || (p[25] & 0x18)) return -EOPNOTSUPP;
    uint64_t data = (uint64_t(block) + p[1]) * Sector;
    if (data > volume_bytes || size > volume_bytes - data) return -EIO;
    bool directory = (p[25] & 2) != 0;
    if (directory && (!size || (p[25] & 0x80) || size > MaxDirectoryBytes || size % Sector)) return -EOPNOTSUPP;
    inode      = {};
    inode.size = inode.first_bytes = size;
    inode.data                     = data;
    inode.modified                 = timestamp(p + 18);
    inode.directory                = directory;
    inode.sections                 = 1;
    return 0;
}

int parse_descriptor(const uint8_t *p, uint64_t device_bytes, Inode &root, uint64_t &bytes)
{
    uint32_t blocks;
    uint16_t block_size, volumes, sequence;
    if (!both32(p + 80, blocks) || !both16(p + 120, volumes) || !both16(p + 124, sequence) ||
        !both16(p + 128, block_size) || !blocks || p[881] != 1)
        return -EIO;
    if (block_size != Sector || volumes != 1 || sequence != 1) return -EOPNOTSUPP;
    bytes = uint64_t(blocks) * Sector;
    if (bytes > device_bytes || bytes < 18U * Sector) return -EIO;
    int error = parse_record(p + 156, 34, bytes, root);
    if (error) return error;
    if (!root.directory || p[188] != 1 || p[189] != 0 || !root.size) return -EIO;
    root.identifier  = root.data;
    root.records_end = root.data + root.size;
    return 0;
}

unsigned lower(unsigned c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}
} // namespace

int Volume::read_exact(uint64_t offset, void *output, Size length) const
{
    if (offset > bytes || length > bytes - offset) return -EIO;
    return !length || reader(context, offset, output, length) ? 0 : -EIO;
}

int Volume::mount(Reader input, void *opaque, uint64_t device_bytes, Workspace &work)
{
    *this = {};
    if (!input || device_bytes < 18U * Sector) return -EINVAL;
    reader                           = input;
    context                          = opaque;
    bytes                            = device_bytes;
    Inode                    primary = {}, supplementary = {};
    uint64_t                 primary_bytes = 0, supplementary_bytes = 0;
    bool                     terminated = false, identified = false;
    unsigned                 best_level = 0, descriptor_end = 0;
    static constexpr uint8_t Signature[] = {'C', 'D', '0', '0', '1'};
    for (unsigned sector = 16; sector < 16 + 256; ++sector)
    {
        int error = read_exact(uint64_t(sector) * Sector, work.descriptor, Sector);
        if (error) return error;
        const uint8_t *p = work.descriptor;
        if (!equal(p + 1, Signature, 5)) return identified ? -EIO : -EINVAL;
        identified = true;
        if (p[6] != 1) return -EOPNOTSUPP;
        if (p[0] == 255)
        {
            terminated     = true;
            descriptor_end = sector + 1;
            break;
        }
        if (p[0] == 1 && !primary_bytes)
        {
            error = parse_descriptor(p, device_bytes, primary, primary_bytes);
            if (error) return error;
        }
        else if (p[0] == 2 && !(p[7] & 1) && p[88] == '%' && p[89] == '/')
        {
            unsigned level = p[90] == '@' ? 1 : p[90] == 'C' ? 2 : p[90] == 'E' ? 3 : 0;
            if (level > best_level)
            {
                error = parse_descriptor(p, device_bytes, supplementary, supplementary_bytes);
                if (error) return error;
                best_level = level;
            }
        }
    }
    if (!terminated || !primary_bytes || uint64_t(descriptor_end) * Sector > primary_bytes) return -EIO;
    if (best_level && supplementary_bytes != primary_bytes) return -EIO;
    unicode    = best_level != 0;
    bytes      = primary_bytes;
    root_inode = unicode ? supplementary : primary;
    // Read the actual '.' record too; the descriptor alone cannot establish a
    // usable directory, and no partially validated tree may be published.
    uint64_t position = root_inode.data;
    Inode    dot;
    int      error = record_at(position, root_inode.records_end, dot, work);
    if (error != 1) return error < 0 ? error : -EIO;
    if (work.record[32] != 1 || work.record[33] || !dot.directory || dot.data != root_inode.data ||
        dot.size != root_inode.size)
        return -EIO;
    return 0;
}

int Volume::record_at(uint64_t &position, uint64_t end, Inode &inode, Workspace &work) const
{
    if (end > bytes || position > end) return -EIO;
    while (position < end)
    {
        int error = read_exact(position, work.record, 1);
        if (error) return error;
        unsigned length = work.record[0];
        if (!length)
        {
            uint64_t padding  = Sector - position % Sector;
            position         += padding < end - position ? padding : end - position;
            continue;
        }
        if (length > end - position || length > Sector - position % Sector) return -EIO;
        error = read_exact(position, work.record, length);
        if (error) return error;
        error = parse_record(work.record, length, bytes, inode);
        if (error) return error;
        inode.identifier   = position;
        inode.records_end  = end;
        position          += length;
        return 1;
    }
    return 0;
}

int Volume::decode_name(const uint8_t *name, unsigned length, bool directory, Entry &entry) const
{
    unsigned used = 0;
    if (unicode && length % 2) return -EIO;
    for (unsigned i = 0; i < length;)
    {
        unsigned c = name[i++];
        if (unicode) c = (c << 8) | name[i++];
        if (!c || c == '/' || c == '\\' || (c >= 0xd800 && c <= 0xdfff) || c == 0xfffe || c == 0xffff) return -EIO;
        if (!unicode)
        {
            if (c < 32 || c > 126) return -EOPNOTSUPP;
            c = lower(c);
        }
        unsigned need = c < 128 ? 1 : c < 2048 ? 2 : 3;
        if (need > 255 - used) return -ENAMETOOLONG;
        if (need == 3) entry.name[used++] = char(0xe0 | (c >> 12));
        if (need >= 2) entry.name[used++] = char((need == 3 ? 0x80 : 0xc0) | ((c >> 6) & (need == 3 ? 63 : 31)));
        entry.name[used++] = char(need == 1 ? c : 0x80 | (c & 63));
    }
    entry.inode.version = 0;
    if (!directory)
    {
        unsigned digits = used;
        while (digits && entry.name[digits - 1] >= '0' && entry.name[digits - 1] <= '9')
            --digits;
        if (digits && digits < used && entry.name[digits - 1] == ';')
        {
            unsigned version = 0;
            for (unsigned i = digits; i < used; ++i)
            {
                version = version * 10 + unsigned(entry.name[i] - '0');
                if (version > 32767) return -EIO;
            }
            if (!version) return -EIO;
            entry.inode.version = uint16_t(version);
            used                = digits - 1;
        }
        if (used && entry.name[used - 1] == '.') --used;
    }
    entry.name[used] = 0;
    if (!used || (used == 1 && entry.name[0] == '.') || (used == 2 && entry.name[0] == '.' && entry.name[1] == '.'))
        return -EIO;
    return 0;
}

bool Volume::names_equal(const char *a, const char *b) const
{
    if (!a || !b) return false;
    while (*a && *b)
    {
        unsigned left = uint8_t(*a++), right = uint8_t(*b++);
        if ((unicode ? left : lower(left)) != (unicode ? right : lower(right))) return false;
    }
    return *a == *b;
}

int Volume::cursor(const Inode &directory, Cursor &cursor) const
{
    if (!directory.directory) return -ENOTDIR;
    if (directory.sections != 1 || directory.size > MaxDirectoryBytes || directory.data > bytes ||
        directory.size > bytes - directory.data)
        return -EIO;
    cursor = {directory.data, directory.data + directory.size, 0};
    return 0;
}

int Volume::next(Cursor &cursor, Entry &entry, Workspace &work) const
{
    for (;;)
    {
        int result = record_at(cursor.position, cursor.end, entry.inode, work);
        if (result != 1) return result;
        if (++cursor.entries > MaxEntries) return -E2BIG;
        unsigned length = work.record[32], flags = work.record[25];
        if (length == 1 && work.record[33] <= 1)
        {
            if (!entry.inode.directory) return -EIO;
            continue;
        }
        for (unsigned i = 0; i < length; ++i)
            work.original_name[i] = work.record[33 + i];
        result = decode_name(work.original_name, length, entry.inode.directory, entry);
        if (result) return result;
        bool     more           = (flags & 0x80) != 0;
        uint32_t previous_bytes = entry.inode.first_bytes;
        while (more)
        {
            if (!previous_bytes || previous_bytes % Sector || entry.inode.sections == MaxSections) return -EIO;
            Inode section;
            result = record_at(cursor.position, cursor.end, section, work);
            if (result != 1) return result < 0 ? result : -EIO;
            if (work.record[32] != length || !equal(work.original_name, work.record + 33, length) ||
                (work.record[25] & 0x7f) != (flags & 0x7f) || !section.size)
                return -EIO;
            entry.inode.size += section.size;
            ++entry.inode.sections;
            previous_bytes = section.first_bytes;
            more           = (work.record[25] & 0x80) != 0;
        }
        // Associated files are a separate namespace, not duplicate ordinary
        // directory names. Hidden ordinary files remain visible to the VFS.
        if (!(flags & 4)) return 1;
    }
}

int Volume::lookup(const Inode &directory, const char *name, Entry &entry, Workspace &work) const
{
    Cursor walk;
    int    error = cursor(directory, walk);
    if (error) return error;
    Entry candidate;
    bool  found = false;
    while ((error = next(walk, candidate, work)) == 1)
    {
        if (!names_equal(candidate.name, name)) continue;
        if (found && candidate.inode.version == entry.inode.version) return -EIO;
        if (!found || candidate.inode.version > entry.inode.version) entry = candidate;
        found = true;
    }
    return error < 0 ? error : found ? 0 : -ENOENT;
}

int Volume::read(const Inode &inode, uint64_t offset, void *output, Size length, Size &completed, Workspace &work) const
{
    completed = 0;
    if (inode.directory) return -EISDIR;
    if (!length || offset >= inode.size) return 0;
    if (!output || !inode.sections || inode.sections > MaxSections) return -EINVAL;
    if (length > inode.size - offset) length = Size(inode.size - offset);
    auto *destination = static_cast<uint8_t *>(output);
    if (inode.sections == 1)
    {
        if (inode.size != inode.first_bytes || inode.data > bytes || inode.size > bytes - inode.data) return -EIO;
        int error = read_exact(inode.data + offset, output, length);
        if (!error) completed = length;
        return error;
    }
    uint64_t position = inode.identifier, logical = 0;
    unsigned original_length = 0, original_flags = 0;
    // Extents stay on disk instead of allocating a fixed array per VFS node.
    // Revalidate the entire bounded chain, including sections past the read.
    for (unsigned index = 0; index < inode.sections; ++index)
    {
        Inode section;
        int   error = record_at(position, inode.records_end, section, work);
        if (error != 1) return error < 0 ? error : -EIO;
        unsigned name_length = work.record[32], flags = work.record[25];
        if (!index)
        {
            if (section.identifier != inode.identifier || section.data != inode.data ||
                section.first_bytes != inode.first_bytes)
                return -EIO;
            original_length = name_length;
            original_flags  = flags;
            for (unsigned i = 0; i < name_length; ++i)
                work.original_name[i] = work.record[33 + i];
        }
        if (section.directory || name_length != original_length ||
            !equal(work.original_name, work.record + 33, name_length) || (flags & 0x7f) != (original_flags & 0x7f) ||
            bool(flags & 0x80) != (index + 1 < inode.sections) || !section.size ||
            (index + 1 < inode.sections && section.size % Sector) || logical > inode.size ||
            section.size > inode.size - logical)
            return -EIO;
        if (offset < logical + section.size && completed < length)
        {
            uint64_t within = offset > logical ? offset - logical : 0;
            Size     take   = length - completed;
            if (take > section.size - within) take = Size(section.size - within);
            error = read_exact(section.data + within, destination + completed, take);
            if (error) return error;
            completed += take;
            offset    += take;
        }
        logical += section.size;
    }
    return logical == inode.size && completed == length ? 0 : -EIO;
}
} // namespace xjiso
