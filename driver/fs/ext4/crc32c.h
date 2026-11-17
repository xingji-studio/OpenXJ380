#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

namespace xjext4 {
namespace detail {

struct Crc32cTable
{
    uint32_t entries[256];

    constexpr Crc32cTable() : entries{}
    {
        for (unsigned index = 0; index < 256; ++index)
        {
            uint32_t value = index;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1U) ? 0x82f63b78U : 0);
            entries[index] = value;
        }
    }
};

// Generate the byte transitions from the existing reflected Castagnoli
// polynomial. This immutable table needs no runtime initialization or CPU
// feature. Keep the caller's seed and omit the final complement, as ext4 and
// JBD2 require; callers may feed successive fields into the same checksum.
inline constexpr Crc32cTable kCrc32cTable{};

inline uint32_t crc32c_update(uint32_t seed, const uint8_t *bytes, __SIZE_TYPE__ length)
{
    for (__SIZE_TYPE__ index = 0; index < length; ++index)
        seed = (seed >> 8) ^ kCrc32cTable.entries[(seed ^ bytes[index]) & 0xffU];
    return seed;
}

} // namespace detail
} // namespace xjext4
