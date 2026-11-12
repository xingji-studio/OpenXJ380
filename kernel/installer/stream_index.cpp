// SPDX-License-Identifier: Apache-2.0
#include <installer/stream_index.h>

namespace xjinstaller
{
namespace
{
uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint64_t le64(const uint8_t *p)
{
    return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32;
}
} // namespace

int stream_index_validate(const uint8_t *data, xjpak2::Size bytes, uint64_t volume_bytes, StreamIndex &output)
{
    output = {};
    if (!data || bytes < StreamIndexHeaderBytes || bytes > StreamIndexMaximumBytes || !volume_bytes ||
        uintptr_t(data) > uintptr_t(-1) - bytes) return -22;
    const char magic[] = "XJSTRM01";
    for (unsigned i = 0; i < 8; ++i) if (data[i] != uint8_t(magic[i])) return -22;
    if (le32(data + 8) != 1 || le32(data + 12) != StreamIndexHeaderBytes || le64(data + 16) != bytes ||
        xjpak2::crc32c(data, 124) != le32(data + 124) ||
        xjpak2::crc32c(data + 128, bytes - 128) != le32(data + 120)) return -22;
    for (unsigned i = 80; i < 120; ++i) if (data[i]) return -22;
    uint64_t total = le64(data + 24), count = le64(data + 32);
    uint64_t ext4 = le64(data + 40), ext4_bytes = le64(data + 48);
    uint64_t fat = le64(data + 56), fat_bytes = le64(data + 64), extents = le64(data + 72);
    if (!total || total > 0xffffffffULL || !count || count > StreamExtentMaximum || ext4 != 128 ||
        ext4_bytes < 64 || ext4_bytes > bytes - 128 || fat != ext4 + ext4_bytes ||
        fat_bytes < 64 || fat_bytes > bytes - fat || extents != fat + fat_bytes ||
        count > (bytes - extents) / 24 || count * 24 != bytes - extents) return -22;
    uint64_t logical = 0, previous_end = 0;
    for (uint64_t i = 0; i < count; ++i)
    {
        const auto *entry = data + extents + i * 24;
        uint64_t start = le64(entry), physical = le64(entry + 8), length = le64(entry + 16);
        if (start != logical || !length || length > total - logical || physical % 512 ||
            physical < previous_end || physical > volume_bytes || length > volume_bytes - physical ||
            (i + 1 < count && length % 512)) return -22;
        logical += length;
        previous_end = physical + length;
    }
    if (logical != total) return -22;
    output = {{data + ext4, xjpak2::Size(ext4_bytes)}, {data + fat, xjpak2::Size(fat_bytes)},
              {data + extents, xjpak2::Size(count * 24)}, total, count};
    return 0;
}

int stream_index_location(const StreamIndex &index, uint64_t logical, xjpak2::Size requested,
                           uint64_t &volume_offset, xjpak2::Size &available)
{
    volume_offset = 0;
    available = 0;
    if (!requested || logical >= index.data_bytes || requested > index.data_bytes - logical ||
        !index.extents.data || index.extent_count > index.extents.size / 24) return -22;
    uint64_t low = 0, high = index.extent_count;
    while (low < high)
    {
        uint64_t middle = low + (high - low) / 2;
        const uint8_t *entry = index.extents.data + middle * 24;
        uint64_t start = le64(entry), length = le64(entry + 16);
        if (logical < start) high = middle;
        else if (logical - start >= length) low = middle + 1;
        else
        {
            uint64_t within = logical - start, remaining = length - within;
            volume_offset = le64(entry + 8) + within;
            available = xjpak2::Size(remaining < requested ? remaining : requested);
            return 0;
        }
    }
    return -22;
}
} // namespace xjinstaller
