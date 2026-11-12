#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <installer/package.h>

namespace xjinstaller
{
constexpr uint64_t StreamIndexMaximumBytes = 64ULL << 20;
constexpr uint64_t StreamExtentMaximum = 65536;
constexpr uint32_t StreamIndexHeaderBytes = 128;

struct StreamIndex
{
    xjpak2::Span ext4, fat32, extents;
    uint64_t data_bytes = 0;
    uint64_t extent_count = 0;
};

// Original bounded source catalog. Extents describe the on-media system PAK
// file relative to the authenticated loaded EFI volume, never another disk.
// Metadata and extents must remain immutable for the entire source lifetime.
int stream_index_validate(const uint8_t *, xjpak2::Size bytes, uint64_t volume_bytes, StreamIndex &);
int stream_index_location(const StreamIndex &, uint64_t logical, xjpak2::Size requested,
                           uint64_t &volume_offset, xjpak2::Size &available);
} // namespace xjinstaller
