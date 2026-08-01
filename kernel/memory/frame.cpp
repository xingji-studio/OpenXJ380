#include <dlinker.h>
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

static uint64_t get_memory_top(MEMORY_MAP map)
{
    uint64_t       top         = 0;
    EfiMemoryDesc *EfiMemory   = (EfiMemoryDesc *)((uint64_t)(map.Buffer) + 0xFFFF800000000000);

    for (int i = 0; i < (map.MapSize / map.DescriptorSize); i++)
    {
        if (EfiMemory->Type == EfiConventionalMemory)
        {
            uint64_t region_end = EfiMemory->PhysicalStart + EfiMemory->NumberOfPages * PAGE_SIZE;
            if (region_end > top) top = region_end;
        }
        EfiMemory++;
    }

    return top;
}

uint64_t get_memory_all_size(MEMORY_MAP map)
{
    uint64_t       total_pages = 0;
    EfiMemoryDesc *EfiMemory   = (EfiMemoryDesc *)((uint64_t)(map.Buffer) + 0xFFFF800000000000);

    for (int i = 0; i < (map.MapSize / map.DescriptorSize); i++)
    {
        total_pages += EfiMemory->NumberOfPages;
        EfiMemory++;
    }

    return total_pages * PAGE_SIZE;
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
    // `memory_size` is used to size frame bitmaps and page metadata, so it must
    // cover the highest usable PFN rather than only the sum of usable pages.
    memory_size       = get_memory_top(map);
    memory_total_size = get_memory_all_size(map);

    size_t         bitmap_size    = (memory_size / 4096 + 7) / 8;
    uint64_t       bitmap_address = 0;
    EfiMemoryDesc *EfiMemory      = (EfiMemoryDesc *)((uint64_t)(map.Buffer) + 0xFFFF800000000000);

    for (uint64_t i = 0; i < (map.MapSize / map.DescriptorSize); i++)
    {
        if (EfiMemory->PhysicalStart >= 0x100000 && (EfiMemory->Type == EfiConventionalMemory))
        {
            if (EfiMemory->NumberOfPages * PAGE_SIZE > ((bitmap_size + PAGE_SIZE - 1) & (~PAGE_SIZE - 1)) * 2)
            {
                bitmap_address = EfiMemory->PhysicalStart;
                break;
            }
        }
        EfiMemory++;
    }

    // if (!bitmap_address) return;

    Bitmap *bitmap = &frame_allocator.bitmap;
    bitmap_init(bitmap, (uint8_t *)(bitmap_address + 0xFFFF800000000000), bitmap_size);
    bitmap_init(&usable_regions,
                (uint8_t *)(bitmap_address + ((bitmap_size + PAGE_SIZE - 1) & (~PAGE_SIZE - 1)) + 0xFFFF800000000000),
                bitmap_size);
    EfiMemory            = (EfiMemoryDesc *)((uint64_t)(map.Buffer) + 0xFFFF800000000000);
    size_t origin_frames = 0;
    for (uint64_t i = 0; i < (map.MapSize / map.DescriptorSize); i++)
    {
        if (EfiMemory->PhysicalStart >= 0x100000 && (EfiMemory->Type == EfiConventionalMemory))
        {
            size_t start_frame = EfiMemory->PhysicalStart / PAGE_SIZE;
            size_t frame_count = EfiMemory->NumberOfPages;
            serial_wprintf("%p~%p\n", start_frame * PAGE_SIZE, (start_frame + frame_count) * PAGE_SIZE);
            origin_frames += frame_count;
            bitmap_set_range(bitmap, start_frame, start_frame + frame_count, true);
            bitmap_set_range(&usable_regions, start_frame, start_frame + frame_count, true);
        }
        EfiMemory++;
    }

    size_t bitmap_frame_start = bitmap_address / PAGE_SIZE;
    size_t bitmap_frame_count = (((bitmap_size + PAGE_SIZE - 1) & (~PAGE_SIZE - 1)) * 2) / PAGE_SIZE;
    size_t bitmap_frame_end   = bitmap_frame_start + bitmap_frame_count;
    bitmap_set_range(bitmap, bitmap_frame_start, bitmap_frame_end, false);
    bitmap_set_range(&usable_regions, bitmap_frame_start, bitmap_frame_end, false);

    frame_allocator.origin_frames = origin_frames;
    frame_allocator.usable_frames = origin_frames - bitmap_frame_count;

    write_serial_string("Available memory: ");
    write_serial_dec((origin_frames / 256));
    write_serial_string(" MiB\n");

    page_setup();
    page_init();
    buddy_init();

    EfiMemory = (EfiMemoryDesc *)((uint64_t)(map.Buffer) + 0xFFFF800000000000);
    for (uint64_t i = 0; i < (map.MapSize / map.DescriptorSize); i++)
    {
        if (EfiMemory->PhysicalStart >= 0x100000 && (EfiMemory->Type == EfiConventionalMemory))
        {
            size_t addr       = EfiMemory->PhysicalStart;
            size_t region_end = EfiMemory->PhysicalStart + EfiMemory->NumberOfPages * PAGE_SIZE;

            // 跳过 bitmap 占用的部分
            if (addr <= bitmap_address && bitmap_address < region_end)
            {
                // bitmap 在这个区域内
                uintptr_t bitmap_end = PADDING_UP(bitmap_address + bitmap_size, PAGE_SIZE);

                // 处理 bitmap 之前的部分
                if (addr < bitmap_address) { process_memory_region(addr, bitmap_address); }

                // 处理 bitmap 之后的部分
                if (bitmap_end < region_end) { process_memory_region(bitmap_end, region_end); }
            }
            else
            {
                process_memory_region(addr, region_end);
            }
        }
        EfiMemory++;
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
