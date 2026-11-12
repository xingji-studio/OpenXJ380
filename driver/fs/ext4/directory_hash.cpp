// SPDX-License-Identifier: Apache-2.0
// Independently expressed ext directory hash arithmetic, tested against the
// separately invoked debugfs dx_hash oracle. No external implementation input.
#include "directory_index.h"
#include <errno.h>

namespace xjext4 {
namespace {

uint32_t rotate(uint32_t value, unsigned bits)
{
    return (value << bits) | (value >> (32 - bits));
}

uint32_t byte_value(char byte, bool unsigned_bytes)
{
    uint8_t value = static_cast<uint8_t>(byte);
    return unsigned_bytes || value < 128 ? uint32_t(value) : uint32_t(int32_t(value) - 256);
}

// The remaining name length supplies a repeated-byte padding word. Octets are
// folded from left to right; signed variants sign-extend before addition.
void words(const char *name, DirectorySize length, bool unsigned_bytes, uint32_t *output, unsigned count)
{
    uint32_t padding = uint32_t(length) * 0x01010101U;
    DirectorySize bytes = length < count * 4U ? length : count * 4U;
    for (unsigned word = 0; word < count; ++word)
    {
        uint32_t value = padding;
        for (unsigned octet = 0; octet < 4 && word * 4U + octet < bytes; ++octet)
            value = (value << 8) + byte_value(name[word * 4U + octet], unsigned_bytes);
        output[word] = value;
    }
}

void half_md4(uint32_t state[4], const uint32_t input[8])
{
    // Eight-word, three-round MD4 compression. Keeping the round schedules as
    // data makes the permutation and rotate counts independently auditable.
    constexpr uint8_t order[3][8] = {{0, 1, 2, 3, 4, 5, 6, 7},
                                    {1, 3, 5, 7, 0, 2, 4, 6},
                                    {3, 7, 2, 6, 1, 5, 0, 4}};
    constexpr uint8_t shifts[3][4] = {{3, 7, 11, 19}, {3, 5, 9, 13}, {3, 9, 11, 15}};
    constexpr uint32_t constant[3] = {0, 0x5a827999U, 0x6ed9eba1U};
    uint32_t value[4] = {state[0], state[1], state[2], state[3]};
    for (unsigned round = 0; round < 3; ++round)
        for (unsigned step = 0; step < 8; ++step)
        {
            unsigned destination = (4U - step % 4U) % 4U;
            uint32_t b = value[(destination + 1) % 4];
            uint32_t c = value[(destination + 2) % 4];
            uint32_t d = value[(destination + 3) % 4];
            uint32_t mix = round == 0 ? ((b & c) | (~b & d)) :
                           round == 1 ? ((b & c) | (b & d) | (c & d)) : (b ^ c ^ d);
            value[destination] = rotate(value[destination] + mix + input[order[round][step]] + constant[round],
                                        shifts[round][step % 4]);
        }
    for (unsigned i = 0; i < 4; ++i) state[i] += value[i];
}

void tea(uint32_t state[4], const uint32_t input[4])
{
    uint32_t left = state[0], right = state[1], sum = 0;
    for (unsigned round = 0; round < 16; ++round)
    {
        sum += 0x9e3779b9U;
        left += ((right << 4) + input[0]) ^ (right + sum) ^ ((right >> 5) + input[1]);
        right += ((left << 4) + input[2]) ^ (left + sum) ^ ((left >> 5) + input[3]);
    }
    state[0] += left;
    state[1] += right;
}

} // namespace

int directory_hash(uint8_t version, const uint32_t seed[4], const char *name,
                   DirectorySize length, DirectoryHash &result)
{
    if (version > 5) return -EOPNOTSUPP;
    if ((!name && length) || length > 255) return -EINVAL;
    bool unsigned_bytes = version >= 3;
    unsigned algorithm = version % 3;
    uint32_t major = 0, minor = 0;
    if (!algorithm)
    {
        uint32_t latest = 0x12a3fe2dU, previous = 0x37abe8f9U;
        for (DirectorySize i = 0; i < length; ++i)
        {
            uint32_t next = previous + (latest ^ (byte_value(name[i], unsigned_bytes) * 7152373U));
            if (next & 0x80000000U) next -= 0x7fffffffU;
            previous = latest;
            latest = next;
        }
        major = latest << 1;
    }
    else
    {
        uint32_t state[4] = {0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U};
        if (seed && (seed[0] || seed[1] || seed[2] || seed[3]))
            for (unsigned i = 0; i < 4; ++i) state[i] = seed[i];
        DirectorySize remaining = length;
        const char *cursor = name;
        unsigned stride = algorithm == 1 ? 32 : 16;
        while (remaining)
        {
            uint32_t input[8];
            words(cursor, remaining, unsigned_bytes, input, stride / 4);
            if (algorithm == 1) half_md4(state, input);
            else tea(state, input);
            DirectorySize consumed = remaining < stride ? remaining : stride;
            cursor += consumed;
            remaining -= consumed;
        }
        major = state[algorithm == 1 ? 1 : 0];
        minor = state[algorithm == 1 ? 2 : 1];
    }
    major &= ~1U;
    // 0xfffffffe is reserved for the end-of-directory cookie.
    if (major == 0xfffffffeU) major -= 2;
    result = {major, minor};
    return 0;
}

} // namespace xjext4
