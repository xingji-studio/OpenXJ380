#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <installer/package.h>

namespace xjinstaller {
constexpr uint64_t StreamIndexMaximumBytes = 64ULL << 20;
constexpr uint64_t StreamExtentMaximum     = 65536;
constexpr uint32_t StreamIndexHeaderBytes  = 128;

struct StreamIndex
{
    xjpak2::Span ext4, fat32, extents;
    uint64_t     data_bytes   = 0;
    uint64_t     extent_count = 0;
    uint32_t     version      = 1;
    xjpak2::Span catalog;
};

// Original bounded source catalog. Extents describe the on-media system PAK
// file relative to the authenticated loaded EFI volume, never another disk.
// Version 3 entries are {logical offset, fixed file ID, file bytes}; they do
// not encode carrier sectors. All spans remain immutable for source lifetime.
int stream_index_validate(const uint8_t *, xjpak2::Size bytes, uint64_t volume_bytes, StreamIndex &);
int stream_index_location(const StreamIndex &, uint64_t logical, xjpak2::Size requested, uint64_t &volume_offset,
                          xjpak2::Size &available);
int stream_index_file_location(const StreamIndex &, uint64_t logical, xjpak2::Size requested, unsigned &file,
                               uint64_t &offset, xjpak2::Size &available);
} // namespace xjinstaller
