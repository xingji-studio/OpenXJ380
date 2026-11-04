// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../errno.h"
#include "../xnet.h"

// Pure address value codec shared by the SDK and persistent/native adapters.
// No allocation, syscalls, provider or lwIP types enter this header.
namespace xnet_address {
inline bool ipv4(const char *start, const char *end, uint8_t *bytes)
{
    for (unsigned part = 0; part < 4; ++part)
    {
        unsigned value = 0, digits = 0;
        while (start != end && *start >= '0' && *start <= '9')
        {
            value = value * 10 + unsigned(*start++ - '0');
            if (++digits > 3 || value > 255) return false;
        }
        if (!digits) return false;
        bytes[part] = uint8_t(value);
        if (part != 3)
        {
            if (start == end || *start++ != '.') return false;
        }
    }
    return start == end;
}

inline int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline char *decimal(char *out, uint32_t value)
{
    char     digits[10];
    unsigned count = 0;
    do
    {
        digits[count++]  = char('0' + value % 10);
        value           /= 10;
    } while (value);
    while (count)
        *out++ = digits[--count];
    return out;
}

inline XNetResult parse(const char *text, XNetAddress *out)
{
    if (!text || !out) return -EINVAL;
    const char *end = text;
    while (*end && end - text < 80)
        ++end;
    if (*end || end == text) return -EINVAL;
    XNetAddress address = {};
    if (ipv4(text, end, address.bytes))
    {
        address.family = XNET_IPV4;
        *out           = address;
        return 0;
    }
    const char *scope = text;
    while (scope != end && *scope != '%')
        ++scope;
    if (scope != end)
    {
        if (scope + 1 == end) return -EINVAL;
        uint64_t value = 0;
        for (const char *p = scope + 1; p != end; ++p)
        {
            if (*p < '0' || *p > '9') return -EINVAL;
            value = value * 10 + unsigned(*p - '0');
            if (value > 0xffffffffULL) return -EINVAL;
        }
        if (!value) return -EINVAL;
        address.scope_id = uint32_t(value);
        end              = scope;
    }
    uint16_t    words[8] = {};
    unsigned    count    = 0;
    int         gap      = -1;
    const char *p        = text;
    if (p != end && *p == ':')
    {
        if (p + 1 == end || p[1] != ':') return -EINVAL;
        gap  = 0;
        p   += 2;
    }
    while (p != end)
    {
        if (count == 8) return -EINVAL;
        const char *segment = p;
        bool        dotted  = false;
        while (p != end && *p != ':')
        {
            dotted |= *p == '.';
            ++p;
        }
        if (dotted)
        {
            uint8_t tail[4];
            if (p != end || count > 6 || !ipv4(segment, end, tail)) return -EINVAL;
            words[count++] = uint16_t(uint16_t(tail[0]) * 256 + tail[1]);
            words[count++] = uint16_t(uint16_t(tail[2]) * 256 + tail[3]);
            break;
        }
        if (p == segment || p - segment > 4) return -EINVAL;
        unsigned value = 0;
        for (const char *digit = segment; digit != p; ++digit)
        {
            int nibble = hex(*digit);
            if (nibble < 0) return -EINVAL;
            value = value * 16 + unsigned(nibble);
        }
        words[count++] = uint16_t(value);
        if (p == end) break;
        if (++p == end) return -EINVAL;
        if (*p == ':')
        {
            if (gap >= 0) return -EINVAL;
            gap = int(count);
            ++p;
        }
    }
    if (gap < 0 && count != 8) return -EINVAL;
    if (gap >= 0)
    {
        if (count >= 8) return -EINVAL;
        unsigned zeros = 8 - count;
        for (int i = int(count) - 1; i >= gap; --i)
            words[i + zeros] = words[i];
        for (unsigned i = unsigned(gap); i < unsigned(gap) + zeros; ++i)
            words[i] = 0;
    }
    address.family = XNET_IPV6;
    for (unsigned i = 0; i < 8; ++i)
    {
        address.bytes[i * 2]     = uint8_t(words[i] >> 8);
        address.bytes[i * 2 + 1] = uint8_t(words[i]);
    }
    *out = address;
    return 0;
}

inline XNetResult format(const XNetAddress *address, char *text, uint32_t capacity)
{
    if (!address || !text || !capacity) return -EINVAL;
    char buffer[64], *out = buffer;
    if (address->family == XNET_IPV4)
    {
        if (address->scope_id) return -EINVAL;
        for (unsigned i = 4; i < 16; ++i)
            if (address->bytes[i]) return -EINVAL;
        for (unsigned i = 0; i < 4; ++i)
        {
            if (i) *out++ = '.';
            out = decimal(out, address->bytes[i]);
        }
    }
    else if (address->family == XNET_IPV6)
    {
        uint16_t words[8];
        for (unsigned i = 0; i < 8; ++i)
            words[i] = uint16_t(address->bytes[i * 2] * 256U + address->bytes[i * 2 + 1]);
        unsigned best = 8, length = 1;
        for (unsigned i = 0; i < 8;)
        {
            if (words[i])
            {
                ++i;
                continue;
            }
            unsigned start = i;
            while (i < 8 && !words[i])
                ++i;
            if (i - start > length)
            {
                best   = start;
                length = i - start;
            }
        }
        static const char digits[] = "0123456789abcdef";
        for (unsigned i = 0; i < 8;)
        {
            if (i == best)
            {
                *out++  = ':';
                *out++  = ':';
                i      += length;
                continue;
            }
            if (out != buffer && out[-1] != ':') *out++ = ':';
            bool emitted = false;
            for (int shift = 12; shift >= 0; shift -= 4)
            {
                unsigned digit = (words[i] >> shift) & 15U;
                if (digit || emitted || !shift)
                {
                    *out++  = digits[digit];
                    emitted = true;
                }
            }
            ++i;
        }
        if (address->scope_id)
        {
            *out++ = '%';
            out    = decimal(out, address->scope_id);
        }
    }
    else
        return -EAFNOSUPPORT;
    uint32_t bytes = uint32_t(out - buffer);
    if (capacity <= bytes) return -ENOBUFS;
    for (uint32_t i = 0; i < bytes; ++i)
        text[i] = buffer[i];
    text[bytes] = 0;
    return 0;
}

} // namespace xnet_address
