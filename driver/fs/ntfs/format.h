#pragma once
// SPDX-License-Identifier: Apache-2.0
// Shared checked NTFS format primitives. USA and mapping-pairs portions:
// Copyright (C) 2020 ViudiraTech. 2026/7/26 By MicroFish & JiTianYu391.
// Adapted for XJ380; see PROVENANCE.md and retained Uinxed LICENSE/NOTICE.
#include "native.h"
#include <errno.h>

namespace xjntfs::format {

constexpr uint64_t NumberMask = 0x0000ffffffffffffULL;
constexpr uint32_t Data = 0x80, IndexRoot = 0x90, IndexAllocation = 0xa0, Bitmap = 0xb0;
constexpr uint32_t Unsupported = 0x800 | 0x4000 | 0x400 | 0x1000 | 0x400000; // compressed, EFS, reparse, offline

inline uint16_t le16(const uint8_t *p)
{
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}

inline uint32_t le32(const uint8_t *p)
{
    return uint32_t(le16(p)) | uint32_t(le16(p + 2)) << 16;
}

inline uint64_t le64(const uint8_t *p)
{
    return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32;
}

inline void copy(void *destination, const void *source, Size length)
{
    auto *out = static_cast<uint8_t *>(destination);
    auto *in  = static_cast<const uint8_t *>(source);
    for (Size i = 0; i < length; ++i)
        out[i] = in[i];
}

inline void clear(void *destination, Size length)
{
    auto *out = static_cast<uint8_t *>(destination);
    for (Size i = 0; i < length; ++i)
        out[i] = 0;
}

inline bool equal(const char *left, const char *right, bool ascii_fold = false)
{
    for (;; ++left, ++right)
    {
        unsigned a = uint8_t(*left), b = uint8_t(*right);
        if (ascii_fold)
        {
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        }
        if (a != b) return false;
        if (!a) return true;
    }
}

inline bool power_of_two(uint64_t value)
{
    return value && !(value & (value - 1));
}

// USA protects 512-byte strides, including on 4 KiB logical-sector media.
// Validate every trailer before restoring any, as in the upstream reader.
inline int unpack(uint8_t *record, unsigned bytes, uint32_t signature)
{
    if (bytes < 512 || bytes % 512 || le32(record) != signature) return -EIO;
    unsigned offset = le16(record + 4), count = le16(record + 6);
    if (offset < 8 || offset % 2 || count != bytes / 512 + 1 || offset > 510 || count * 2 > 510 - offset) return -EIO;
    uint16_t sequence = le16(record + offset);
    if (!sequence) return -EIO;
    for (unsigned i = 1; i < count; ++i)
        if (le16(record + i * 512 - 2) != sequence) return -EIO;
    for (unsigned i = 1; i < count; ++i)
        copy(record + i * 512 - 2, record + offset + i * 2, 2);
    return 0;
}

struct Attribute
{
    const uint8_t *data;
    unsigned       length;
    uint32_t       type;
};

// A single checked attribute iterator is shared by all metadata consumers.
inline int next_attribute(const uint8_t *record, unsigned bytes, unsigned &offset, Attribute &attribute)
{
    unsigned used = le32(record + 24);
    if (offset > used || used > bytes || used - offset < 4) return -EIO;
    uint32_t type = le32(record + offset);
    if (type == 0xffffffffU) return 1;
    if (!type || type % 16 || used - offset < 24) return -EIO;
    const uint8_t *p      = record + offset;
    unsigned       length = le32(p + 4), form = p[8], name = le16(p + 10), names = unsigned(p[9]) * 2;
    unsigned       header = form ? 64 : 24;
    if (form > 1 || length < header || length % 8 || length > used - offset) return -EIO;
    if (names && (name < header || name > length || names > length - name)) return -EIO;
    if (!form)
    {
        unsigned value = le16(p + 20), value_length = le32(p + 16);
        if (value < header || value > length || value_length > length - value || (names && name + names > value))
            return -EIO;
    }
    else
    {
        unsigned mapping = le16(p + 32);
        if (mapping < header || mapping >= length || (names && name + names > mapping)) return -EIO;
    }
    attribute  = {p, length, type};
    offset    += length;
    return 0;
}

inline bool named_i30(const uint8_t *attribute)
{
    if (attribute[9] != 4) return false;
    const uint8_t *name = attribute + le16(attribute + 10);
    return le16(name) == '$' && le16(name + 2) == 'I' && le16(name + 4) == '3' && le16(name + 6) == '0';
}

inline int record_layout(uint8_t *record, unsigned bytes, uint64_t number, uint16_t sequence, uint64_t owner = 0)
{
    int error = unpack(record, bytes, 0x454c4946); // FILE
    if (error) return error;
    unsigned attributes = le16(record + 20), used = le32(record + 24);
    unsigned usa_end = le16(record + 4) + le16(record + 6) * 2;
    if (!(le16(record + 22) & 1) || (le16(record + 22) & ~15U) || !le16(record + 16) ||
        (sequence && sequence != le16(record + 16)) || le64(record + 32) != owner || attributes < 48 ||
        attributes % 8 || attributes < usa_end || used > bytes || attributes > used || used - attributes < 4 ||
        le32(record + 28) != bytes || (uint64_t(le32(record + 44)) | uint64_t(le16(record + 42)) << 32) != number)
        return -EIO;
    Attribute attribute;
    int       status;
    while (!(status = next_attribute(record, bytes, attributes, attribute))) {}
    return status == 1 ? 0 : status;
}

inline int decode_size(uint8_t encoded, uint32_t cluster, unsigned maximum, uint32_t &result)
{
    if (!encoded) return -EIO;
    uint64_t size;
    if (encoded & 0x80)
    {
        unsigned shift = 256 - unsigned(encoded);
        if (shift > 31) return -EOPNOTSUPP;
        size = uint64_t(1) << shift;
    }
    else
        size = uint64_t(encoded) * cluster;
    if (!power_of_two(size) || size < 512) return -EIO;
    if (size > maximum) return -EOPNOTSUPP;
    result = uint32_t(size);
    return 0;
}

// Adapted signed-delta decoding from Uinxed ntfs_runlist_parse. In addition
// to arithmetic checks, every physical run is confined to this volume and a
// terminator plus exact highest-VCN coverage is required. Limits never truncate.
inline int decode_runs(const uint8_t *mapping, unsigned length, uint64_t clusters, uint64_t first, uint64_t highest,
                       bool sparse, Stream &stream)
{
    uint64_t vcn    = first;
    int64_t  lcn    = 0;
    unsigned offset = 0;
    while (offset < length)
    {
        unsigned header = mapping[offset++];
        if (!header) return stream.count && vcn && vcn - 1 == highest ? 0 : -EIO;
        unsigned lb = header & 15, ob = header >> 4;
        if (!lb || lb > 8 || ob > 8 || lb + ob > length - offset) return -EIO;
        if (stream.count == MaxRuns) return -EOPNOTSUPP;
        uint64_t run = 0, raw = 0;
        for (unsigned i = 0; i < lb; ++i)
            run |= uint64_t(mapping[offset++]) << (i * 8);
        if (!run || run > uint64_t(__INT64_MAX__) - vcn) return -EIO;
        int64_t physical = -1;
        if (ob)
        {
            for (unsigned i = 0; i < ob; ++i)
                raw |= uint64_t(mapping[offset++]) << (i * 8);
            if (ob < 8 && (mapping[offset - 1] & 0x80)) raw |= ~((uint64_t(1) << (ob * 8)) - 1);
            int64_t delta = int64_t(raw);
            if (delta < 0)
            {
                uint64_t magnitude = uint64_t(-(delta + 1)) + 1;
                if (magnitude > uint64_t(lcn)) return -EIO;
                lcn -= int64_t(magnitude);
            }
            else
            {
                if (uint64_t(delta) > uint64_t(__INT64_MAX__ - lcn)) return -EIO;
                lcn += delta;
            }
            if (uint64_t(lcn) >= clusters || run > clusters - uint64_t(lcn)) return -EIO;
            physical = lcn;
        }
        else if (!sparse)
            return -EIO;
        stream.runs[stream.count++]  = {vcn, run, physical};
        vcn                         += run;
    }
    return -EIO;
}

inline int utf8_name(const uint8_t *input, unsigned units, char *output)
{
    unsigned size = 0;
    for (unsigned i = 0; i < units; ++i)
    {
        uint32_t code = le16(input + i * 2);
        if (code >= 0xd800 && code <= 0xdbff)
        {
            if (++i == units) return -EILSEQ;
            uint32_t low = le16(input + i * 2);
            if (low < 0xdc00 || low > 0xdfff) return -EILSEQ;
            code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
        }
        else if (code >= 0xdc00 && code <= 0xdfff)
            return -EILSEQ;
        if (!code || code == '/' || code == '\\') return -EOPNOTSUPP;
        if (code < 0x80)
            output[size++] = char(code);
        else if (code < 0x800)
        {
            output[size++] = char(0xc0 | code >> 6);
            output[size++] = char(0x80 | (code & 63));
        }
        else if (code < 0x10000)
        {
            output[size++] = char(0xe0 | code >> 12);
            output[size++] = char(0x80 | ((code >> 6) & 63));
            output[size++] = char(0x80 | (code & 63));
        }
        else
        {
            output[size++] = char(0xf0 | code >> 18);
            output[size++] = char(0x80 | ((code >> 12) & 63));
            output[size++] = char(0x80 | ((code >> 6) & 63));
            output[size++] = char(0x80 | (code & 63));
        }
    }
    if (!size) return -EIO;
    output[size] = 0;
    return 0;
}

inline int index_layout(const uint8_t *buffer, unsigned bytes, unsigned header, unsigned &first, unsigned &end)
{
    if (header > bytes || bytes - header < 16) return -EIO;
    unsigned entries = le32(buffer + header), used = le32(buffer + header + 4);
    unsigned allocated = le32(buffer + header + 8);
    if (entries < 16 || entries % 8 || used < entries || used - entries < 16 || allocated < used ||
        allocated > bytes - header || (buffer[header + 12] & ~1U))
        return -EIO;
    first = header + entries;
    end   = header + used;
    return 0;
}

inline uint64_t record_reference(const uint8_t *record)
{
    return uint64_t(le32(record + 44)) | uint64_t(le16(record + 42)) << 32 | uint64_t(le16(record + 16)) << 48;
}

inline bool same_attribute_name(const uint8_t *attribute, const uint8_t *entry)
{
    if (attribute[9] != entry[6]) return false;
    if (!entry[6]) return true; // Unnamed attributes have no meaningful name offset.
    const uint8_t *left = attribute + le16(attribute + 10), *right = entry + entry[7];
    for (unsigned i = 0; i < unsigned(entry[6]) * 2; ++i)
        if (left[i] != right[i]) return false;
    return true;
}

// The first extent owns stream sizes. Continuations own only their VCN range;
// their relative LCN accumulator restarts at zero for each mapping-pairs array.
inline int append_fragment(const uint8_t *p, unsigned length, Stream &stream, uint64_t clusters, uint32_t cluster,
                           uint64_t bytes, bool &found)
{
    uint16_t flags = le16(p + 12);
    if (flags & ~0x8000U) return -EOPNOTSUPP;
    if (!p[8])
    {
        if (found || flags) return -EIO;
        stream.resident = true;
        stream.size = stream.initialized = stream.allocated = stream.resident_length = le32(p + 16);
        if (stream.size > MaxRecord) return -EOPNOTSUPP;
        copy(stream.value, p + le16(p + 20), stream.resident_length);
        found = true;
        return 0;
    }
    if (stream.resident || le16(p + 34)) return -EOPNOTSUPP;
    unsigned mapping = le16(p + 32);
    if (flags && mapping < 72) return -EIO;
    uint64_t first = le64(p + 16), highest = le64(p + 24);
    if (!found && !first && highest == __UINT64_MAX__ && !le64(p + 40) && !le64(p + 48) && !le64(p + 56) && !p[mapping])
    {
        stream.flags = flags;
        found        = true;
        return 0;
    }
    uint64_t expected = stream.count ? stream.runs[stream.count - 1].vcn + stream.runs[stream.count - 1].length : 0;
    // Bootstrap may read a partial MFT map before complete_stream runs. Check
    // byte coverage now as well, so corrupt run lengths cannot wrap its reads.
    if (first != expected || highest < first || highest >= uint64_t(__INT64_MAX__) / cluster) return -EIO;
    if (!found)
    {
        stream.flags       = flags;
        stream.allocated   = le64(p + 40);
        stream.size        = le64(p + 48);
        stream.initialized = le64(p + 56);
        if (stream.size > uint64_t(__INT64_MAX__) || stream.initialized > stream.size || stream.allocated > bytes ||
            stream.allocated % cluster)
            return -EIO;
    }
    else if (flags != stream.flags || (le64(p + 40) && le64(p + 40) != stream.allocated) ||
             (le64(p + 48) && le64(p + 48) != stream.size) || (le64(p + 56) && le64(p + 56) != stream.initialized))
        return -EIO;
    int error = decode_runs(p + mapping, length - mapping, clusters, first, highest, flags == 0x8000, stream);
    if (!error) found = true;
    return error;
}

inline int complete_stream(const Stream &stream, uint32_t cluster)
{
    if (stream.resident) return 0;
    if (!stream.count) return !stream.size && !stream.allocated ? 0 : -EIO;
    const Run &last    = stream.runs[stream.count - 1];
    uint64_t   logical = last.vcn + last.length;
    if (logical > uint64_t(__INT64_MAX__) / cluster || stream.size > logical * cluster ||
        (!stream.flags && stream.allocated != logical * cluster))
        return -EIO;
    return 0;
}

inline void put16(uint8_t *p, uint16_t value)
{
    p[0] = uint8_t(value);
    p[1] = uint8_t(value >> 8);
}

inline void put32(uint8_t *p, uint32_t value)
{
    put16(p, uint16_t(value));
    put16(p + 2, uint16_t(value >> 16));
}

inline void put64(uint8_t *p, uint64_t value)
{
    put32(p, uint32_t(value));
    put32(p + 4, uint32_t(value >> 32));
}

// Pack a complete logical FILE/INDX/RSTR/RCRD image. The logical source retains
// restored trailer words; the physical destination gets the next USA sequence.
inline int pack(const uint8_t *logical, unsigned bytes, uint8_t *physical)
{
    unsigned offset = le16(logical + 4), count = le16(logical + 6);
    if (bytes < 512 || bytes % 512 || count != bytes / 512 + 1 || offset < 8 || offset % 2 || offset > 510 ||
        count * 2 > 510 - offset)
        return -EIO;
    copy(physical, logical, bytes);
    uint16_t sequence = uint16_t(le16(logical + offset) + 1);
    if (!sequence) sequence = 1;
    put16(physical + offset, sequence);
    for (unsigned i = 1; i < count; ++i)
    {
        copy(physical + offset + i * 2, logical + i * 512 - 2, 2);
        put16(physical + i * 512 - 2, sequence);
    }
    return 0;
}

} // namespace xjntfs::format
