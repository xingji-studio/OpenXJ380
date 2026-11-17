// SPDX-License-Identifier: Apache-2.0 AND MIT
// Copyright (c) 2026, DiscUtils contributors. Adapted for bounded C++ recovery.
// Exact upstream and original MIT grants: REFERENCES.json / DISCUTILS_NOTICE.txt.
#include "format.h"
#include "recovery.h"
#include <errno.h>
namespace xjntfs {
using namespace format;
namespace {
unsigned align8(unsigned n)
{
    return (n + 7) & ~7U;
}

void move(uint8_t *p, unsigned to, unsigned from, unsigned length)
{
    if (to > from)
        for (unsigned i = length; i; --i)
            p[to + i - 1] = p[from + i - 1];
    else
        copy(p + to, p + from, length);
}

int attribute_at(uint8_t *p, unsigned size, unsigned target, bool end = false)
{
    unsigned used = le32(p + 24), at = le16(p + 20);
    if (used > size || at < 32 || at > used || used - at < 4) return -EIO;
    while (at < target)
    {
        if (at + 8 > used || le32(p + at) == 0xffffffffU) return -EIO;
        unsigned length = le32(p + at + 4);
        if (length < 24 || length % 8 || length > used - at) return -EIO;
        at += length;
    }
    if (at != target || target > used - 4) return -EIO;
    if (le32(p + target) == 0xffffffffU) return end ? 0 : -EIO;
    return target + 24 <= used && le32(p + target + 4) >= 24 && le32(p + target + 4) <= used - target ? 0 : -EIO;
}

int resize(uint8_t *p, unsigned size, unsigned at, unsigned old, unsigned length)
{
    unsigned used = le32(p + 24);
    if (at > used || old > used - at || length > size - (used - old)) return -EIO;
    move(p, at + length, at + old, used - at - old);
    put32(p + at + 4, length);
    put32(p + 24, used - old + length);
    return 0;
}

int entry_at(uint8_t *p, unsigned size, unsigned header, unsigned capacity, unsigned target, bool end)
{
    if (header > size || capacity > size - header || capacity < 16) return -EIO;
    unsigned entries = le32(p + header), used = le32(p + header + 4), total = le32(p + header + 8);
    if (entries < 16 || entries > used || used > total || total > capacity) return -EIO;
    unsigned at = header + entries, limit = header + used;
    while (at < target && at < limit)
    {
        if (limit - at < 16) return -EIO;
        unsigned n = le16(p + at + 8);
        if (n < 16 || n % 8 || n > limit - at) return -EIO;
        at += n;
    }
    if (at != target || target > limit) return -EIO;
    if (!end && (limit - target < 16 || le16(p + target + 8) < 16 || le16(p + target + 8) > limit - target))
        return -EIO;
    return 0;
}

int index_entry(uint16_t op, const RecoveryOperation &r, uint8_t *p, unsigned size, const uint8_t *data,
                unsigned length, bool index)
{
    unsigned roff = r.record_offset, header = 24, target = r.attribute_offset, capacity = size - 24;
    if (!index)
    {
        if (attribute_at(p, size, roff) || le32(p + roff) != IndexRoot || p[roff + 8]) return -EIO;
        unsigned attr = le32(p + roff + 4), value = le16(p + roff + 20), bytes = le32(p + roff + 16);
        if (value < 24 || value > attr || bytes < 32 || bytes > attr - value) return -EIO;
        header   = roff + value + 16;
        target   = roff + r.attribute_offset;
        capacity = bytes - 16;
    }
    bool add = op == 0xc || op == 0xe, remove = op == 0xd || op == 0xf;
    if (entry_at(p, size, header, capacity, target, add || op == 0x10)) return -EIO;
    unsigned used = le32(p + header + 4), total = le32(p + header + 8), end = header + used;
    if (add)
    {
        if (length < 16) return -EIO;
        unsigned n = le16(data + 8);
        if (n < 16 || n % 8 || n > length) return -EIO;
        if (index)
        {
            if (n > total - used || n > size - end) return -EIO;
            move(p, target + n, target, end - target);
            copy(p + target, data, n);
            put32(p + header + 4, used + n);
        }
        else
        {
            unsigned fr = le32(p + 24);
            if (n > size - fr) return -EIO;
            move(p, target + n, target, fr - target);
            copy(p + target, data, n);
            put32(p + 24, fr + n);
            put32(p + roff + 4, le32(p + roff + 4) + n);
            put32(p + roff + 16, le32(p + roff + 16) + n);
            put32(p + header + 4, used + n);
            put32(p + header + 8, total + n);
        }
    }
    else if (remove)
    {
        unsigned n = le16(p + target + 8);
        if (!n || n > end - target) return -EIO;
        if (index)
        {
            move(p, target, target + n, end - target - n);
            put32(p + header + 4, used - n);
        }
        else
        {
            unsigned fr = le32(p + 24);
            move(p, target, target + n, fr - target - n);
            put32(p + 24, fr - n);
            put32(p + roff + 4, le32(p + roff + 4) - n);
            put32(p + roff + 16, le32(p + roff + 16) - n);
            put32(p + header + 4, used - n);
            put32(p + header + 8, total - n);
        }
    }
    else if (op == 0x10)
    {
        if (length > header + total - target || length > size - target) return -EIO;
        copy(p + target, data, length);
        put32(p + header + 4, target - header + length);
    }
    else
    {
        unsigned n = le16(p + target + 8);
        if (n > size - target) return -EIO;
        if (op == 0x11 || op == 0x12)
        {
            if (length < 8 || n < 24) return -EIO;
            copy(p + target + n - 8, data, 8);
        }
        else if (op == 0x13 || op == 0x14)
        {
            if (n < 24 + 56) return -EIO;
            unsigned amount = length < 56 ? length : 56;
            copy(p + target + 24, data, amount);
        }
        else if (op == 0x21 || op == 0x22)
        {
            unsigned off = le16(p + target);
            if (off > n || length > n - off) return -EIO;
            copy(p + target + off, data, length);
        }
        else
            return -EOPNOTSUPP;
    }
    return 0;
}
} // namespace
int recovery_target(uint16_t op)
{
    switch (op)
    {
    case 0:
    case 1:
    case 0xa:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1a:
    case 0x1b:
    case 0x1c:
    case 0x1d:
    case 0x1e:
    case 0x1f:
    case 0x20: return 0;
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 9:
    case 0xb:
    case 0xc:
    case 0xd:
    case 0x11:
    case 0x13:
    case 0x21:
    case 0x25: return 1;
    case 0xe:
    case 0xf:
    case 0x10:
    case 0x12:
    case 0x14:
    case 0x22: return 2;
    case 8:
    case 0x15:
    case 0x16: return 3;
    default: return -1;
    }
}

int recovery_action(uint16_t op, const RecoveryOperation &r, uint8_t *p, unsigned size, const uint8_t *data,
                    unsigned length, bool index)
{
    if (index || op == 0xc || op == 0xd || op == 0x11 || op == 0x13 || op == 0x21)
        return index_entry(op, r, p, size, data, length, index);
    unsigned at = r.record_offset, aoff = r.attribute_offset;
    if (op == 2 || op == 0x25)
    {
        if (at > size || length > size - at) return -EIO;
        if (op == 2)
            copy(p + at, data, length);
        else
            clear(p + at, length);
        return 0;
    }
    if (op == 3)
    {
        put16(p + 22, uint16_t(le16(p + 22) & ~1U));
        put16(p + 16, uint16_t(le16(p + 16) + 1));
        return 0;
    }
    if (attribute_at(p, size, at, op == 4 || op == 5)) return -EIO;
    unsigned used = le32(p + 24);
    if (op == 4)
    {
        if (length > size - at) return -EIO;
        copy(p + at, data, length);
        put32(p + 24, align8(at + length));
        return 0;
    }
    if (op == 5)
    {
        if (length < 24) return -EIO;
        unsigned n = le32(data + 4);
        if (n < 24 || n % 8 || n > length || n > size - used) return -EIO;
        move(p, at + n, at, used - at);
        copy(p + at, data, n);
        put32(p + 24, used + n);
        unsigned id = le16(p + at + 14);
        if (le16(p + 40) <= id) put16(p + 40, uint16_t(id + 1));
        if (!p[at + 8] && (p[at + 22] & 1)) put16(p + 18, uint16_t(le16(p + 18) + 1));
        return 0;
    }
    unsigned old = le32(p + at + 4);
    if (op == 6)
    {
        if (!p[at + 8] && (p[at + 22] & 1))
        {
            if (!le16(p + 18)) return -EIO;
            put16(p + 18, uint16_t(le16(p + 18) - 1));
        }
        move(p, at, at + old, used - at - old);
        put32(p + 24, used - old);
        return 0;
    }
    if (op == 7)
    {
        if (r.redo_size == r.undo_size)
        {
            if (aoff > old || length > old - aoff) return -EIO;
            copy(p + at + aoff, data, length);
            return 0;
        }
        unsigned value = le16(p + at + 20);
        if (p[at + 8] || value < 24 || value > old || aoff < value) return -EIO;
        unsigned n = align8(aoff + length);
        if (resize(p, size, at, old, n)) return -EIO;
        put32(p + at + 16, aoff + length - value);
        copy(p + at + aoff, data, length);
        return 0;
    }
    if (op == 9)
    {
        if (!p[at + 8] || old < 64) return -EIO;
        unsigned mapping = le16(p + at + 32);
        if (mapping < 64 || aoff < mapping || aoff > old) return -EIO;
        unsigned n = align8(aoff + length);
        if (n <= mapping || resize(p, size, at, old, n)) return -EIO;
        copy(p + at + aoff, data, length);
        uint64_t vcn      = le64(p + at + 16);
        unsigned position = mapping;
        while (position < n && p[at + position])
        {
            unsigned h = p[at + position], lb = h & 15, ob = h >> 4;
            if (!lb || lb > 8 || ob > 8 || 1 + lb + ob > n - position) return -EIO;
            uint64_t run = 0;
            for (unsigned i = 0; i < lb; ++i)
                run |= uint64_t(p[at + position + 1 + i]) << (i * 8);
            if (!run || vcn > uint64_t(__INT64_MAX__) - run) return -EIO;
            vcn      += run;
            position += 1 + lb + ob;
        }
        if (position >= n || !vcn) return -EIO;
        put64(p + at + 24, vcn - 1);
        return 0;
    }
    if (op == 0xb)
    {
        if (!p[at + 8] || old < 64 || length < 24) return -EIO;
        unsigned mapping = le16(p + at + 32);
        if (mapping < 64 || mapping > old) return -EIO;
        put64(p + at + 40, le64(data));
        put64(p + at + 56, le64(data + 8));
        put64(p + at + 48, le64(data + 16));
        if (length >= 32 && mapping >= 72) put64(p + at + 64, le64(data + 24));
        return 0;
    }
    return -EOPNOTSUPP;
}
} // namespace xjntfs
