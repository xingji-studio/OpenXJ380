// SPDX-License-Identifier: Apache-2.0
#include <mm/boot_memory.h>

namespace boot_memory
{
namespace
{
constexpr uint32_t conventional_memory = 7;
constexpr uint64_t minimum_allocatable = 0x100000;

bool structural_bounds(const MapView &map)
{
    if (!map.bytes || map.descriptor_bytes < descriptor_prefix_bytes || !map.map_bytes ||
        map.map_bytes > map.buffer_bytes || map.map_bytes % map.descriptor_bytes != 0 ||
        map.map_bytes / map.descriptor_bytes > maximum_descriptors)
    {
        return false;
    }
    uint64_t address = reinterpret_cast<uintptr_t>(map.bytes);
    return map.buffer_bytes <= __UINTPTR_MAX__ && address <= __UINTPTR_MAX__ - map.buffer_bytes;
}

template <typename T> T copy_field(const uint8_t *source)
{
    // Firmware storage and its stride need not satisfy C++ struct alignment.
    T     value       = 0;
    auto *destination = reinterpret_cast<uint8_t *>(&value);
    for (uint64_t i = 0; i < sizeof(T); ++i) destination[i] = source[i];
    return value;
}

bool read_descriptor(const MapView &map, uint64_t index, Descriptor *output)
{
    const auto *record = static_cast<const uint8_t *>(map.bytes) + index * map.descriptor_bytes;
    Descriptor descriptor = {};
    descriptor.type           = copy_field<uint32_t>(record);
    descriptor.physical_start = copy_field<uint64_t>(record + 8);
    descriptor.virtual_start  = copy_field<uint64_t>(record + 16);
    descriptor.pages          = copy_field<uint64_t>(record + 24);
    descriptor.attributes     = copy_field<uint64_t>(record + 32);
    if (!descriptor.pages || descriptor.physical_start % page_bytes != 0 ||
        descriptor.virtual_start % page_bytes != 0 || descriptor.pages > __UINT64_MAX__ / page_bytes ||
        descriptor.physical_start > __UINT64_MAX__ - descriptor.pages * page_bytes)
    {
        return false;
    }
    *output = descriptor;
    return true;
}

uint64_t region_end(const Descriptor &descriptor)
{
    return descriptor.physical_start + descriptor.pages * page_bytes;
}

bool permitted(uint32_t type, SpanPolicy policy)
{
    if (policy == SpanPolicy::loader_data_only) return type == 2;
    if (policy == SpanPolicy::loader_and_boot_services) return type >= 1 && type <= 4;
    return false;
}
} // namespace

bool descriptor_at(const MapView &map, uint64_t index, Descriptor *output)
{
    if (!output) return false;
    *output = {};
    if (!structural_bounds(map) || index >= map.map_bytes / map.descriptor_bytes) return false;
    return read_descriptor(map, index, output);
}

bool validate_map(const MapView &map, Summary *output)
{
    if (!output) return false;
    *output = {};
    if (!structural_bounds(map)) return false;
    Summary summary = {};
    summary.descriptor_count = map.map_bytes / map.descriptor_bytes;
    for (uint64_t index = 0; index < summary.descriptor_count; ++index)
    {
        Descriptor descriptor = {};
        if (!read_descriptor(map, index, &descriptor)) return false;
        uint64_t bytes = descriptor.pages * page_bytes;
        if (summary.total_bytes > __UINT64_MAX__ - bytes) return false;
        summary.total_bytes += bytes;
        for (uint64_t previous = 0; previous < index; ++previous)
        {
            Descriptor other = {};
            if (!read_descriptor(map, previous, &other)) return false;
            if (descriptor.physical_start < region_end(other) && other.physical_start < region_end(descriptor))
            {
                return false;
            }
        }
        if (descriptor.type != conventional_memory) continue;
        uint64_t end = region_end(descriptor);
        if (end > summary.conventional_top) summary.conventional_top = end;
        if (descriptor.physical_start >= minimum_allocatable) summary.allocatable_pages += descriptor.pages;
    }
    *output = summary;
    return true;
}

bool find_region(const MapView &map, uint64_t physical, Descriptor *output)
{
    if (!output) return false;
    *output = {};
    Summary summary = {};
    if (!validate_map(map, &summary)) return false;
    for (uint64_t index = 0; index < summary.descriptor_count; ++index)
    {
        Descriptor descriptor = {};
        if (!read_descriptor(map, index, &descriptor)) return false;
        if (physical >= descriptor.physical_start && physical < region_end(descriptor))
        {
            *output = descriptor;
            return true;
        }
    }
    return false;
}

bool covered_span(const MapView &map, uint64_t physical, uint64_t bytes, SpanPolicy policy, PhysicalSpan *output)
{
    if (!output) return false;
    *output = {};
    Summary summary = {};
    if (!validate_map(map, &summary) || !bytes || physical > __UINT64_MAX__ - bytes) return false;
    uint64_t end = physical + bytes;
    if (end > __UINT64_MAX__ - (page_bytes - 1)) return false;
    end = (end + page_bytes - 1) & ~(page_bytes - 1);
    uint64_t start   = physical & ~(page_bytes - 1);
    uint64_t current = start;
    while (current < end)
    {
        bool found = false;
        for (uint64_t index = 0; index < summary.descriptor_count; ++index)
        {
            Descriptor descriptor = {};
            if (!read_descriptor(map, index, &descriptor)) return false;
            uint64_t descriptor_end = region_end(descriptor);
            if (current < descriptor.physical_start || current >= descriptor_end) continue;
            if (!permitted(descriptor.type, policy)) return false;
            current = descriptor_end < end ? descriptor_end : end;
            found = true;
            break;
        }
        if (!found) return false;
    }
    *output = {start, end};
    return true;
}

bool plan_frame_bitmaps(const MapView &map, FrameBitmapLayout *output)
{
    if (!output) return false;
    *output = {};
    Summary summary = {};
    if (!validate_map(map, &summary) || !summary.conventional_top) return false;
    FrameBitmapLayout layout = {};
    uint64_t frames = summary.conventional_top / page_bytes;
    layout.bitmap_bytes = (frames + 7) / 8;
    if (layout.bitmap_bytes > __UINT64_MAX__ - (page_bytes - 1)) return false;
    layout.storage_bytes = (layout.bitmap_bytes + page_bytes - 1) & ~(page_bytes - 1);
    if (layout.storage_bytes > __UINT64_MAX__ / 2) return false;
    uint64_t reserved_bytes = layout.storage_bytes * 2;
    for (uint64_t index = 0; index < summary.descriptor_count; ++index)
    {
        Descriptor descriptor = {};
        if (!read_descriptor(map, index, &descriptor)) return false;
        if (descriptor.type != conventional_memory || descriptor.physical_start < minimum_allocatable ||
            descriptor.pages * page_bytes < reserved_bytes)
        {
            continue;
        }
        layout.first        = descriptor.physical_start;
        layout.second       = layout.first + layout.storage_bytes;
        layout.reserved_end = layout.second + layout.storage_bytes;
        *output = layout;
        return true;
    }
    return false;
}
} // namespace boot_memory
