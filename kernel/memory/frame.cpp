#include <dlinker.h>
#include <mm/boot_memory.h>
#include <mm/buddy.h>
#include <mm/frame.h>
#include <mm/memory.h>
#include <proto.hpp>
extern void *early_alloc(size_t size);
#define ENABLE_BUDDY 0

FrameAllocator frame_allocator;
Bitmap         usable_regions;
uint64_t       memory_size       = 0;
uint64_t       memory_total_size = 0;

namespace
{
[[noreturn]] void frame_init_failed(const char *reason)
{
    write_serial_string("Fatal boot memory map: ");
    write_serial_string(reason);
    write_serial_string("\n");
    for (;;) asm volatile("cli; hlt" ::: "memory");
}

boot_memory::MapView frame_map_view(MEMORY_MAP map)
{
    uint64_t physical = reinterpret_cast<uintptr_t>(map.Buffer);
    uint64_t offset   = get_physical_memory_offset();
    if (!physical || !map.MapSize || physical > __UINT64_MAX__ - map.MapSize ||
        physical > __UINTPTR_MAX__ - offset || physical + offset > __UINTPTR_MAX__ - map.MapSize)
    {
        frame_init_failed("invalid map buffer range");
    }
    // Boot already provides this mapped HHDM buffer. These arithmetic checks do
    // not prove mappings; callers retaining boot blobs must validate those separately.
    return {phys_to_virt(physical), map.MapSize, map.MapSize, map.DescriptorSize};
}

boot_memory::Summary checked_memory_map(const boot_memory::MapView &view, MEMORY_MAP map)
{
    boot_memory::Summary      summary     = {};
    boot_memory::PhysicalSpan map_storage = {};
    if (!boot_memory::validate_map(view, &summary) ||
        !boot_memory::covered_span(view, reinterpret_cast<uintptr_t>(map.Buffer), map.MapSize,
                                   boot_memory::SpanPolicy::loader_and_boot_services, &map_storage))
    {
        frame_init_failed("invalid or allocatable map storage");
    }
    return summary;
}
} // namespace

uint64_t get_memory_all_size(MEMORY_MAP map)
{
    boot_memory::MapView view = frame_map_view(map);
    return checked_memory_map(view, map).total_bytes;
}

static uint64_t get_zone_boundary(enum zone_type type)
{
    switch (type)
    {
#if defined(__x86_64__)
    case ZONE_DMA: return ZONE_DMA_END;
#endif
    case ZONE_DMA32: return ZONE_DMA32_END;
    case ZONE_NORMAL: return __UINT64_MAX__;
    default: return 0;
    }
}

// 处理单个内存区域，正确处理不连续的可用帧
static void process_memory_region(uintptr_t start, uintptr_t end)
{
    // 页对齐
    start = PADDING_UP(start, PAGE_SIZE);
    end   = PADDING_DOWN(end, PAGE_SIZE);

    if (start >= end) return;

    uintptr_t current = start;

    while (current < end)
    {
        // 确定当前位置所属的 zone
        enum zone_type type = phys_to_zone_type(current);

        // 找到同一 zone 的边界
        uintptr_t zone_boundary = get_zone_boundary(type);
        uintptr_t zone_end      = MIN(zone_boundary, end);

        // 在当前 zone 内查找连续的可用区域
        uintptr_t region_current = current;

        while (region_current < zone_end)
        {
            size_t frame = region_current / PAGE_SIZE;

            // 跳过不可用的帧
            while (region_current < zone_end && !bitmap_get(&frame_allocator.bitmap, region_current / PAGE_SIZE))
            {
                region_current += PAGE_SIZE;
            }

            if (region_current >= zone_end) break;

            // 找到连续可用区域的起始
            uintptr_t usable_start = region_current;

            // 找到连续可用区域的结束
            while (region_current < zone_end && bitmap_get(&frame_allocator.bitmap, region_current / PAGE_SIZE))
            {
                region_current += PAGE_SIZE;
            }

            uintptr_t usable_end = region_current;

            // 添加这段连续可用的区域到 buddy 分配器
            if (usable_end > usable_start) { add_memory_region(usable_start, usable_end, type); }
        }

        current = zone_end;
    }
}

void init_frame(MEMORY_MAP map)
{
    boot_memory::MapView           view    = frame_map_view(map);
    boot_memory::Summary           summary = checked_memory_map(view, map);
    boot_memory::FrameBitmapLayout layout = {};
    if (!boot_memory::plan_frame_bitmaps(view, &layout) ||
        layout.reserved_end > __UINTPTR_MAX__ - get_physical_memory_offset())
    {
        frame_init_failed("no suitable region for both frame bitmaps");
    }

    // Metadata indexes physical frames, including holes, rather than their sum.
    memory_size       = summary.conventional_top;
    memory_total_size = summary.total_bytes;
    Bitmap *bitmap = &frame_allocator.bitmap;
    bitmap_init(bitmap, static_cast<uint8_t *>(phys_to_virt(layout.first)), layout.bitmap_bytes);
    bitmap_init(&usable_regions, static_cast<uint8_t *>(phys_to_virt(layout.second)), layout.bitmap_bytes);

    size_t origin_frames = 0;
    for (uint64_t index = 0; index < summary.descriptor_count; ++index)
    {
        boot_memory::Descriptor descriptor = {};
        if (!boot_memory::descriptor_at(view, index, &descriptor)) frame_init_failed("map changed during frame init");
        if (descriptor.physical_start < 0x100000 || descriptor.type != EfiConventionalMemory) continue;
        size_t first = descriptor.physical_start / PAGE_SIZE;
        size_t end   = first + descriptor.pages;
        serial_wprintf("%p~%p\n", first * PAGE_SIZE, end * PAGE_SIZE);
        origin_frames += descriptor.pages;
        bitmap_set_range(bitmap, first, end, true);
        bitmap_set_range(&usable_regions, first, end, true);
    }

    // Both complete page-aligned allocations stay unavailable to early and buddy allocators.
    size_t bitmap_frame_start = layout.first / PAGE_SIZE;
    size_t bitmap_frame_end   = layout.reserved_end / PAGE_SIZE;
    size_t bitmap_frame_count = bitmap_frame_end - bitmap_frame_start;
    bitmap_set_range(bitmap, bitmap_frame_start, bitmap_frame_end, false);
    bitmap_set_range(&usable_regions, bitmap_frame_start, bitmap_frame_end, false);
    frame_allocator.origin_frames = origin_frames;
    frame_allocator.usable_frames = origin_frames - bitmap_frame_count;

    write_serial_string("Available memory: ");
    write_serial_dec(origin_frames / 256);
    write_serial_string(" MiB\n");
    page_setup();
    page_init();
    buddy_init();

    for (uint64_t index = 0; index < summary.descriptor_count; ++index)
    {
        boot_memory::Descriptor descriptor = {};
        if (!boot_memory::descriptor_at(view, index, &descriptor)) frame_init_failed("map changed during buddy init");
        if (descriptor.physical_start < 0x100000 || descriptor.type != EfiConventionalMemory) continue;
        // This routine scans the bitmap and therefore excludes both bitmaps as
        // well as any frames consumed by page_setup/page_init/buddy_init.
        process_memory_region(descriptor.physical_start, descriptor.physical_start + descriptor.pages * PAGE_SIZE);
    }
}

spin_t frame_op_lock = SPIN_INIT;

uint64_t alloc_frames_l(size_t count)
{
    spin_lock(&frame_op_lock);
    Bitmap *bitmap      = &frame_allocator.bitmap;
    size_t  frame_index = bitmap_find_range(bitmap, count, true);

    if (frame_index == (size_t)-1) {
        spin_unlock(&frame_op_lock);
        return 0;
    }
    bitmap_set_range(bitmap, frame_index, frame_index + count, false);

    frame_allocator.usable_frames -= count;
    spin_unlock(&frame_op_lock);

    return frame_index * PAGE_SIZE;
}
void *early_alloc(size_t size)
{
    void *ptr = (void *)phys_to_virt(alloc_frames_l((size + PAGE_SIZE - 1) / PAGE_SIZE));
    memset(ptr, 0, (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    return ptr;
}

// void free_frame(uint64_t addr)
// {
//     spin_lock(&frame_op_lock);
// #if ENABLE_BUDDY
//     buddy_free_pages(addr, 1);
// #else
//     if (addr == 0)
//     {
//         spin_unlock(&frame_op_lock);
//         return;
//     }
//     size_t frame_index = addr / PAGE_SIZE;
//     if (bitmap_get(&usable_regions, frame_index) == false)
//     {
//         spin_unlock(&frame_op_lock);
//         return;
//     }
//     Bitmap *bitmap = &frame_allocator.bitmap;
//     bitmap_set(bitmap, frame_index, true);
// #endif
//     frame_allocator.usable_frames++;
//     spin_unlock(&frame_op_lock);
// }

// EXPORT_SYMBOL(free_frame);

// EXPORT_SYMBOL(alloc_frames);

// void free_frames(uint64_t addr, uint64_t size)
// {
//     spin_lock(&frame_op_lock);
// #if ENABLE_BUDDY
//     buddy_free_pages(addr, size);
// #else

//     if (addr == 0)
//     {
//         spin_unlock(&frame_op_lock);
//         return;
//     }

//     size_t frame_index = addr / PAGE_SIZE;
//     for (size_t i = frame_index; i < size; i++)
//     {
//         if (bitmap_get(&usable_regions, i) == false)
//         {
//             spin_unlock(&frame_op_lock);
//             return;
//         }
//     }

//     bitmap_set_range(&frame_allocator.bitmap, frame_index, frame_index + size, true);
// #endif
//     frame_allocator.usable_frames += size;

//     spin_unlock(&frame_op_lock);
// }

// EXPORT_SYMBOL(free_frames);
