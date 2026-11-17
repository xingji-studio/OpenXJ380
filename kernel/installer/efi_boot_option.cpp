// SPDX-License-Identifier: Apache-2.0
#include <installer/efi_boot.h>

namespace xjinstaller {
namespace {
void put16(uint8_t *p, uint16_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i)
        p[i] = uint8_t(v >> (i * 8));
}
} // namespace

int efi_boot_option(unsigned partition, uint64_t first, uint64_t sectors, const uint8_t guid[16], uint8_t *output,
                    __SIZE_TYPE__ capacity, __SIZE_TYPE__ &bytes)
{
    bytes                            = 0;
    const char         description[] = "XJ380";
    const char         path[]        = "\\EFI\\XJ380\\BOOTX64.EFI";
    constexpr unsigned file_bytes    = 4 + sizeof(path) * 2;
    constexpr unsigned path_bytes    = 42 + file_bytes + 4;
    constexpr unsigned total         = 6 + sizeof(description) * 2 + path_bytes;
    if (!partition || !first || !sectors || sectors > uint64_t(-1) - first || !guid || !output || capacity < total)
        return -22;
    uint8_t present = 0;
    for (unsigned i = 0; i < 16; ++i)
        present |= guid[i];
    if (!present) return -22;
    for (unsigned i = 0; i < total; ++i)
        output[i] = 0;
    output[0] = 1;
    put16(output + 4, path_bytes);
    for (unsigned i = 0; i < sizeof(description); ++i)
        put16(output + 6 + i * 2, uint8_t(description[i]));
    uint8_t *node = output + 6 + sizeof(description) * 2;
    node[0]       = 4;
    node[1]       = 1;
    put16(node + 2, 42);
    for (unsigned i = 0; i < 4; ++i)
        node[4 + i] = uint8_t(partition >> (8 * i));
    put64(node + 8, first);
    put64(node + 16, sectors);
    for (unsigned i = 0; i < 16; ++i)
        node[24 + i] = guid[i];
    node[40] = node[41]  = 2;
    node                += 42;
    node[0]              = 4;
    node[1]              = 4;
    put16(node + 2, file_bytes);
    for (unsigned i = 0; i < sizeof(path); ++i)
        put16(node + 4 + i * 2, uint8_t(path[i]));
    node    += file_bytes;
    node[0]  = 0x7f;
    node[1]  = 0xff;
    put16(node + 2, 4);
    bytes = total;
    return 0;
}

int efi_boot_order(const uint8_t *before, __SIZE_TYPE__ before_bytes, uint16_t slot, uint8_t *output,
                   __SIZE_TYPE__ capacity, __SIZE_TYPE__ &bytes)
{
    bytes = 0;
    if ((!before && before_bytes) || !output || before_bytes % 2 || before_bytes > capacity ||
        capacity - before_bytes < 2 || before == output)
        return -22;
    put16(output, slot);
    bytes = 2;
    for (__SIZE_TYPE__ i = 0; i < before_bytes; i += 2)
    {
        uint16_t value = uint16_t(before[i]) | uint16_t(before[i + 1]) << 8;
        if (value == slot) continue;
        put16(output + bytes, value);
        bytes += 2;
    }
    return 0;
}
} // namespace xjinstaller
