#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

namespace boot_memory
{
constexpr uint64_t page_bytes = 4096;
constexpr uint64_t descriptor_prefix_bytes = 40;
// Bound the allocation-free overlap scan even for a corrupt handoff.
constexpr uint64_t maximum_descriptors = 16384;

struct MapView
{
    // Caller owns the lifetime and supplies already mapped, readable bytes.
    // Integer range checks here do not establish a virtual mapping.
    const void *bytes;
    uint64_t    buffer_bytes;
    uint64_t    map_bytes;
    uint64_t    descriptor_bytes;
};

struct Descriptor
{
    uint32_t type;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t pages;
    uint64_t attributes;
};

struct Summary
{
    uint64_t descriptor_count;
    uint64_t total_bytes;
    uint64_t conventional_top;
    uint64_t allocatable_pages;
};

struct PhysicalSpan
{
    uint64_t start;
    uint64_t end; // Exclusive; the complete page-rounded span.
};

enum class SpanPolicy
{
    loader_data_only,
    loader_and_boot_services // EFI types 1..4; never ConventionalMemory or MMIO.
};

struct FrameBitmapLayout
{
    uint64_t bitmap_bytes;
    uint64_t storage_bytes; // Each bitmap's complete page-rounded storage.
    uint64_t first;
    uint64_t second;
    uint64_t reserved_end;
};

// The map must remain immutable throughout validation and all subsequent uses.
// All output arguments are cleared on failure; no function allocates or does I/O.
bool validate_map(const MapView &map, Summary *output);
// This checks the record and structural bounds; validate_map additionally rejects overlap/total overflow.
bool descriptor_at(const MapView &map, uint64_t index, Descriptor *output);
bool find_region(const MapView &map, uint64_t physical, Descriptor *output);
bool covered_span(const MapView &map, uint64_t physical, uint64_t bytes, SpanPolicy policy, PhysicalSpan *output);
bool plan_frame_bitmaps(const MapView &map, FrameBitmapLayout *output);
} // namespace boot_memory
