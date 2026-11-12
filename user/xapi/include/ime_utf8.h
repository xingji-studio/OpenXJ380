#pragma once
#include <stdint.h>
#if __STDC_HOSTED__
#include <stddef.h>
#endif

// Strict UTF-8: reject overlong encodings, surrogates and out-of-range scalars.
static inline size_t ime_utf8_scalar_bytes(const char *text, size_t available)
{
    if (!text || !available) return 0;
    const uint8_t *s = (const uint8_t *)text;
    if (s[0] > 0 && s[0] < 0x80) return 1;
    size_t n = s[0] >= 0xc2 && s[0] <= 0xdf   ? 2
               : s[0] >= 0xe0 && s[0] <= 0xef ? 3
               : s[0] >= 0xf0 && s[0] <= 0xf4 ? 4
                                              : 0;
    if (!n || n > available) return 0;
    for (size_t i = 1; i < n; ++i)
        if ((s[i] & 0xc0) != 0x80) return 0;
    if ((s[0] == 0xe0 && s[1] < 0xa0) || (s[0] == 0xed && s[1] >= 0xa0) || (s[0] == 0xf0 && s[1] < 0x90) ||
        (s[0] == 0xf4 && s[1] >= 0x90))
        return 0;
    return n;
}

static inline bool ime_utf8_valid(const char *text, size_t capacity)
{
    size_t i = 0;
    while (i < capacity && text[i])
    {
        size_t n = ime_utf8_scalar_bytes(text + i, capacity - i);
        if (!n) return false;
        i += n;
    }
    return i < capacity;
}

static inline uint64_t ime_utf8_pack(const char *s, size_t n)
{
    uint64_t packed = 0;
    for (size_t i = 0; i < n && i < 4; ++i)
        packed |= (uint64_t)(uint8_t)s[i] << (i * 8);
    return packed;
}

static inline size_t ime_utf8_previous(const char *s, size_t end)
{
    if (!end) return 0;
    --end;
    while (end && ((uint8_t)s[end] & 0xc0) == 0x80)
        --end;
    return end;
}
