// SPDX-License-Identifier: Apache-2.0
#include "rock_ridge.h"
#include <errno.h>

namespace xjiso {
namespace {
constexpr uint64_t BlockSize = 2048;

bool both32(const uint8_t *p, uint32_t &value)
{
    value = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    return value == (uint32_t(p[7]) | uint32_t(p[6]) << 8 | uint32_t(p[5]) << 16 | uint32_t(p[4]) << 24);
}

bool signature(const uint8_t *p, char a, char b)
{
    return p[0] == uint8_t(a) && p[1] == uint8_t(b);
}

bool text_equal(const uint8_t *p, unsigned size, const char *text)
{
    unsigned i = 0;
    while (i < size && text[i])
    {
        if (p[i] != uint8_t(text[i])) return false;
        ++i;
    }
    return i == size && !text[i];
}

bool leap(unsigned year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

uint64_t date(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second,
              unsigned centiseconds, int zone)
{
    static constexpr unsigned Days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1970 || year > 9999 || !month || month > 12 || !day ||
        day > Days[month - 1] + unsigned(month == 2 && leap(year)) || hour > 23 || minute > 59 || second > 59 ||
        centiseconds > 99 || zone < -48 || zone > 52)
        return 0;
    auto     leap_days = [](unsigned y) { return y / 4 - y / 100 + y / 400; };
    uint64_t days      = uint64_t(year - 1970) * 365 + leap_days(year - 1) - leap_days(1969);
    for (unsigned m = 1; m < month; ++m)
        days += Days[m - 1] + unsigned(m == 2 && leap(year));
    int64_t  seconds  = int64_t((days + day - 1) * 86400 + hour * 3600 + minute * 60 + second) - zone * 900;
    uint64_t fraction = uint64_t(centiseconds) * 10000000;
    if (seconds < 0 || uint64_t(seconds) > (uint64_t(-1) - fraction) / 1000000000ULL) return 0;
    return uint64_t(seconds) * 1000000000ULL + fraction;
}

uint64_t long_time(const uint8_t *p)
{
    unsigned values[7] = {};
    unsigned position  = 0;
    for (unsigned field = 0; field < 7; ++field)
        for (unsigned digit = 0; digit < (field ? 2U : 4U); ++digit)
        {
            unsigned c = p[position++];
            if (c < '0' || c > '9') return 0;
            values[field] = values[field] * 10 + c - '0';
        }
    return date(values[0], values[1], values[2], values[3], values[4], values[5], values[6], int8_t(p[16]));
}

using Visitor = int (*)(void *, const uint8_t *);

// A continuation is appended after its containing area. Traverse this linear
// chain iteratively, retaining neither pointers into input media nor unbounded
// recursion. CE offsets, total work and repeated locations are all checked.
int scan(Reader reader, void *context, uint64_t bytes, const uint8_t *record, const RockRidgeState &state, bool root,
         bool probe, Visitor visit, void *visitor, SuspWorkspace &work)
{
    unsigned begin = 33U + record[32] + unsigned(!(record[32] & 1)) + (root ? state.root_skip : state.skip);
    if (begin > record[0]) return -EIO;
    uint64_t area = 0;
    uint32_t size = record[0] - begin, position = 0;
    unsigned total = size, followed = 0, entries = 0, selected = 1;
    bool     initial = true;
    for (;;)
    {
        uint64_t continuation      = 0;
        uint32_t continuation_size = 0;
        auto     read              = [&](uint32_t offset, unsigned count) -> int {
            if (offset > size || count > size - offset) return -EIO;
            if (initial)
                for (unsigned i = 0; i < count; ++i)
                    work.entry[i] = record[begin + offset + i];
            else if (!reader(context, area + offset, work.entry, count))
                return -EIO;
            return 0;
        };
        while (position < size)
        {
            unsigned remaining = size - position;
            int      error     = read(position, remaining < 4 ? remaining : 4);
            if (error) return error;
            if (remaining < 4 || (!work.entry[0] && !work.entry[1]))
            {
                // Only zero alignment padding may follow the final entry.
                while (position < size)
                {
                    unsigned count = size - position < 256 ? size - position : 256;
                    if ((error = read(position, count))) return error;
                    for (unsigned i = 0; i < count; ++i)
                        if (work.entry[i]) return -EIO;
                    position += count;
                }
                break;
            }
            unsigned length = work.entry[2];
            if (length < 4 || length > remaining || ++entries > 4096) return -EIO;
            if ((error = read(position, length))) return error;
            const uint8_t *p = work.entry;
            if (p[3] != 1) return -EOPNOTSUPP;
            position += length;
            if (signature(p, 'C', 'E'))
            {
                uint32_t block, offset, count;
                if (length != 28 || continuation_size || !both32(p + 4, block) || !both32(p + 12, offset) ||
                    !both32(p + 20, count) || !count)
                    return -EIO;
                continuation = uint64_t(block) * BlockSize + offset;
                if (continuation > bytes || count > bytes - continuation) return -EIO;
                if (count > MaxSystemUseBytes - total) return -E2BIG;
                total             += count;
                continuation_size  = count;
            }
            else if (signature(p, 'S', 'T'))
            {
                if (length != 4) return -EIO;
                break;
            }
            else if (signature(p, 'S', 'P'))
            {
                if (!root || !initial || position != 7 || length != 7 || p[4] != 0xbe || p[5] != 0xef ||
                    p[6] != state.skip)
                    return -EIO;
            }
            else if (signature(p, 'E', 'S'))
            {
                if (length != 5 || !p[4] || (!probe && p[4] > state.extensions)) return -EIO;
                selected = p[4];
            }
            else if (probe || selected == state.sequence)
            {
                if ((error = visit(visitor, p))) return error;
            }
        }
        if (!continuation_size) return 0;
        if (followed == MaxContinuationAreas) return -E2BIG;
        for (unsigned i = 0; i < followed; ++i)
            if (work.visited[i] == continuation) return -ELOOP;
        work.visited[followed++] = continuation;
        area                     = continuation;
        size                     = continuation_size;
        position                 = 0;
        initial                  = false;
    }
}

int extension(void *context, const uint8_t *p)
{
    if (!signature(p, 'E', 'R')) return 0;
    auto &state = *static_cast<RockRidgeState *>(context);
    if (p[2] < 8 || unsigned(p[5]) + p[6] + p[4] + 8 != p[2] || ++state.extensions > 255) return -EIO;
    bool recognized = text_equal(p + 8, p[4], "RRIP_1991A") || text_equal(p + 8, p[4], "IEEE_P1282") ||
                      text_equal(p + 8, p[4], "IEEE_1282");
    if (recognized)
    {
        if (p[7] != 1) return -EOPNOTSUPP;
        if (state.sequence) return -EIO;
        state.sequence = state.extensions;
    }
    return 0;
}

struct Parser
{
    RockRidgeInfo *info;
    bool           name_continues, link_continues, component_continues, have_child, have_parent, have_device;
    unsigned       times;
};

int append(char *output, unsigned &used, unsigned capacity, const uint8_t *input, unsigned size)
{
    if (size > capacity - used) return -ENAMETOOLONG;
    for (unsigned i = 0; i < size; ++i)
    {
        if (!input[i] || input[i] == '/') return -EIO;
        output[used++] = char(input[i]);
    }
    output[used] = 0;
    return 0;
}

int alternate_name(Parser &parser, const uint8_t *p)
{
    auto &info = *parser.info;
    if (p[2] < 5 || (p[4] & ~7U) || (info.name_present && !parser.name_continues)) return -EIO;
    unsigned flags = p[4];
    if (flags && (flags & (flags - 1))) return -EIO;
    info.name_present     = true;
    parser.name_continues = (flags & 1) != 0;
    if (flags & 6)
    {
        if (p[2] != 5 || info.name_size) return -EIO;
        info.name[info.name_size++] = '.';
        if (flags & 4) info.name[info.name_size++] = '.';
        info.name[info.name_size] = 0;
        return 0;
    }
    return append(info.name, info.name_size, 255, p + 5, p[2] - 5);
}

int symbolic_link(Parser &parser, const uint8_t *p)
{
    auto &info = *parser.info;
    if (p[2] < 5 || (p[4] & ~1U) || (info.symlink && !parser.link_continues)) return -EIO;
    info.symlink          = true;
    parser.link_continues = (p[4] & 1) != 0;
    for (unsigned offset = 5; offset < p[2];)
    {
        if (p[2] - offset < 2) return -EIO;
        unsigned flags = p[offset], length = p[offset + 1];
        offset += 2;
        if (length > p[2] - offset) return -EIO;
        if (flags & ~15U) return -EOPNOTSUPP;
        if (flags && (flags & (flags - 1))) return -EIO;
        if ((flags & 14) && (length || parser.component_continues)) return -EIO;
        if (flags & 8)
        {
            if (info.target_size) return -EIO;
            info.target[info.target_size++] = '/';
        }
        else
        {
            if (!parser.component_continues && info.target_size && info.target[info.target_size - 1] != '/')
            {
                if (info.target_size == MaxLinkBytes) return -ENAMETOOLONG;
                info.target[info.target_size++] = '/';
            }
            int                      error;
            static constexpr uint8_t Dot[] = {'.', '.'};
            if (flags & 6)
                error = append(info.target, info.target_size, MaxLinkBytes, Dot, flags & 4 ? 2 : 1);
            else
            {
                if (!length) return -EIO;
                error = append(info.target, info.target_size, MaxLinkBytes, p + offset, length);
            }
            if (error) return error;
        }
        info.target[info.target_size]  = 0;
        parser.component_continues     = (flags & 1) != 0;
        offset                        += length;
    }
    return !parser.link_continues && parser.component_continues ? -EIO : 0;
}

int parse_entry(void *context, const uint8_t *p)
{
    auto &parser = *static_cast<Parser *>(context);
    auto &info   = *parser.info;
    if (signature(p, 'P', 'X'))
    {
        if (info.attributes || (p[2] != 36 && p[2] != 44) || !both32(p + 4, info.mode) || !both32(p + 12, info.links) ||
            !both32(p + 20, info.uid) || !both32(p + 28, info.gid) || !info.links || (info.mode & ~0177777U))
            return -EIO;
        if (p[2] == 44 && !both32(p + 36, info.serial)) return -EIO;
        info.attributes = true;
    }
    else if (signature(p, 'N', 'M'))
        return alternate_name(parser, p);
    else if (signature(p, 'S', 'L'))
        return symbolic_link(parser, p);
    else if (signature(p, 'R', 'E'))
    {
        if (p[2] != 4 || info.relocated) return -EIO;
        info.relocated = true;
    }
    else if (signature(p, 'C', 'L') || signature(p, 'P', 'L'))
    {
        bool      child = p[0] == 'C';
        bool     &seen  = child ? parser.have_child : parser.have_parent;
        uint32_t &value = child ? info.child : info.parent;
        if (p[2] != 12 || seen || !both32(p + 4, value) || !value) return -EIO;
        seen = true;
    }
    else if (signature(p, 'P', 'N'))
    {
        if (p[2] != 20 || parser.have_device || !both32(p + 4, info.major) || !both32(p + 12, info.minor)) return -EIO;
        parser.have_device = true;
    }
    else if (signature(p, 'T', 'F'))
    {
        if (p[2] < 5 || (parser.times & (p[4] & 127))) return -EIO;
        parser.times  |= p[4] & 127;
        unsigned size = p[4] & 128 ? 17 : 7, offset = 5;
        for (unsigned bit = 0; bit < 7; ++bit)
            if (p[4] & (1U << bit))
            {
                if (size > p[2] - offset) return -EIO;
                uint64_t value  = size == 7 ? iso_recording_time(p + offset) : long_time(p + offset);
                offset         += size;
                if (bit == 0)
                {
                    info.created      = value;
                    info.have_created = true;
                }
                if (bit == 1)
                {
                    info.modified      = value;
                    info.have_modified = true;
                }
                if (bit == 2)
                {
                    info.accessed      = value;
                    info.have_accessed = true;
                }
                if (bit == 3)
                {
                    info.changed      = value;
                    info.have_changed = true;
                }
            }
        if (offset != p[2]) return -EIO;
    }
    else if (signature(p, 'S', 'F') || signature(p, 'Z', 'F'))
        info.unsupported = true; // Never return encoded sparse/compressed bytes as an ordinary file.
    return 0;
}
} // namespace

uint64_t iso_recording_time(const uint8_t *p)
{
    return date(1900 + p[0], p[1], p[2], p[3], p[4], p[5], 0, int8_t(p[6]));
}

int rock_ridge_probe(Reader reader, void *context, uint64_t bytes, const uint8_t *record, RockRidgeState &state,
                     SuspWorkspace &work)
{
    state          = {};
    unsigned begin = 33U + record[32] + unsigned(!(record[32] & 1));
    // Locate SP at the start, or after the advertised application-use prefix
    // (including CD-ROM XA's 14-byte prefix). No media code is executed.
    for (unsigned skip = 0; begin + skip + 7 <= record[0]; ++skip)
    {
        const uint8_t *p = record + begin + skip;
        if (!signature(p, 'S', 'P') || p[2] != 7 || p[3] != 1 || p[4] != 0xbe || p[5] != 0xef || (skip && p[6] != skip))
            continue;
        state.root_skip = skip;
        state.skip      = p[6];
        int error       = scan(reader, context, bytes, record, state, true, true, extension, &state, work);
        if (error) return error;
        state.enabled = state.sequence != 0;
        return 0;
    }
    return 0;
}

int rock_ridge_parse(Reader reader, void *context, uint64_t bytes, const uint8_t *record, const RockRidgeState &state,
                     RockRidgeInfo &info, SuspWorkspace &work, bool root_record)
{
    info = {};
    if (!state.enabled) return 0;
    Parser parser = {&info, false, false, false, false, false, false, 0};
    int    error  = scan(reader, context, bytes, record, state, root_record, false, parse_entry, &parser, work);
    if (error) return error;
    if (parser.name_continues || parser.link_continues || parser.component_continues ||
        (info.name_present && !info.name_size) || (info.symlink && !info.target_size))
        return -EIO;
    if (info.attributes && info.symlink && (info.mode & 0170000) != 0120000) return -EIO;
    if (info.attributes && (info.mode & 0170000) == 0120000 && !info.symlink) return -EIO;
    if (info.child && (info.relocated || info.symlink || info.parent)) return -EIO;
    return 0;
}
} // namespace xjiso
