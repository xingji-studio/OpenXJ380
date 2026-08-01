#include <dlinker.h>
#include <mm/bitmap.h>
#include <mm/buddy.h>
#include <mm/frame.h>
#include <proto.hpp>
//Update Buddy!
extern FrameAllocator frame_allocator;

Bitmap using_regions;

const char *zone_names[__MAX_NR_ZONES] = {
    "DMA",
    "DMA32", "Normal"};

zone_t *zones[__MAX_NR_ZONES] = {NULL};
int     nr_zones              = 0;

extern uint64_t memory_size;
extern void    *early_alloc(size_t size);

static inline bool order_valid(size_t order) // 判断 order 是否在合法范围内
{
    return order >= MIN_ORDER && order <= MAX_ORDER;//+1
}

static inline size_t log2_floor_u64(uint64_t x)//math 不敢用啊 /gt
{
    if (x == 0) return 0;
    return 63U - __builtin_clzll(x);
}

static inline size_t next_power_of_2(size_t x)
{
    if (x <= 1) return 1;
    if ((x & (x - 1)) == 0) return x;
    return 1ULL << (log2_floor_u64(x) + 1);
}

static inline uint64_t order_to_block_size(size_t order)//order转块大小
{
    return 1ULL << order;//其实就是2^n
}

static inline size_t order_to_pages(size_t order)//order转页数
{
    return 1ULL << (order - MIN_ORDER);
}

static inline size_t count_to_block_pages(size_t count)
{
    return next_power_of_2(count);
}

static inline size_t count_to_order(size_t count)//普通的向上取整，因为order只能是2^n
{
    return log2_floor_u64(count_to_block_pages(count)) + MIN_ORDER;
}

static inline size_t order_to_index(size_t order)
{
    return order - MIN_ORDER;
}

static inline uint64_t *block_next_ptr(uint64_t base)
{
    return (uint64_t *)phys_to_virt(base);
}

static inline uint64_t block_list_next(uint64_t base)
{
    return *block_next_ptr(base);
}

static inline void block_list_set_next(uint64_t base, uint64_t next)
{
    *block_next_ptr(base) = next;
}

static inline bool zone_contains_block(zone_t *zone, uint64_t base, size_t order)
{
    if (!zone || !order_valid(order)) return false;

    uint64_t zone_start = zone->zone_start_pfn * PAGE_SIZE;//起始位置
    uint64_t zone_end   = zone->zone_end_pfn * PAGE_SIZE;//结束位置
    uint64_t block_size = order_to_block_size(order);

    if (base < zone_start) return false;
    if (base > zone_end) return false;
    if (block_size > zone_end - base) return false;

    return true;
}

static inline void push_block(zone_t *zone, size_t order, uint64_t base)
{
    size_t index = order_to_index(order);
    block_list_set_next(base, zone->allocator.free_area[index]);
    zone->allocator.free_area[index] = base;
}

static inline uint64_t pop_block(zone_t *zone, size_t order)
{
    size_t   index = order_to_index(order);
    uint64_t head  = zone->allocator.free_area[index];

    if (head == 0) return 0;

    zone->allocator.free_area[index] = block_list_next(head);
    block_list_set_next(head, 0);
    return head;
}

static bool remove_block(zone_t *zone, size_t order, uint64_t base)
{
    size_t   index = order_to_index(order);
    uint64_t prev  = 0;
    uint64_t curr  = zone->allocator.free_area[index];

    while (curr != 0)
    {
        uint64_t next = block_list_next(curr);

        if (curr == base)
        {
            if (prev == 0) { zone->allocator.free_area[index] = next; }
            else { block_list_set_next(prev, next); }

            block_list_set_next(curr, 0);
            return true;
        }

        prev = curr;
        curr = next;//太恶心了
    }

    return false;
}

enum zone_type phys_to_zone_type(uint64_t phys)
{
    if (phys < ZONE_DMA_END) return ZONE_DMA;
    if (phys < ZONE_DMA32_END) return ZONE_DMA32;
    return ZONE_NORMAL;
}

zone_t *get_zone(enum zone_type type)
{
    if (type >= __MAX_NR_ZONES) return NULL;
    return zones[type];
}

bool zone_has_memory(zone_t *zone)
{
    return zone && zone->managed_pages > 0;
}

uint64_t buddy_alloc_zone(zone_t *zone, size_t count)
{
    if (!zone || count == 0) return 0;

    size_t order = count_to_order(count);//请转阶 :(|)
    if (!order_valid(order)) return 0;

    spin_lock(&zone->allocator.lock);

    uint64_t addr          = 0;
    size_t   current_order = order;

    for (; current_order < MAX_ORDER; current_order++)
    {
        addr = pop_block(zone, current_order);//往上,然后拆
        if (addr != 0) break;//闲的嘞
    }

    if (addr == 0)
    {
        spin_unlock(&zone->allocator.lock);
        return 0;//没有足够大的块了
    }
    //现在是一个2^current_roder的大块，地址是addr 要把它拆成 2^(current_order-1)的半块
    //左半块是addr
    //右边就是addr + 2^(current_order-1)   
    while (current_order > order)
    {
        current_order--;//左边继续
        push_block(zone, current_order, addr + order_to_block_size(current_order));//右边滚回去
    }

    zone->free_pages -= order_to_pages(order);

    spin_unlock(&zone->allocator.lock);
    return addr;
}
//其实，alloc和free严格对称/gt
void buddy_free_zone(zone_t *zone, uint64_t base, size_t order)
{
    //base对齐
    if (!zone || base == 0 || !order_valid(order)) return;//ccb
    if ((base & (order_to_block_size(order) - 1)) != 0) return;//ccb
    if (!zone_contains_block(zone, base, order)) return;//ccb

    size_t released_pages = order_to_pages(order);

    while (order + 1 < MAX_ORDER)
    {
        uint64_t buddy = base ^ order_to_block_size(order);//只是第order位不同
        //e.g. 8kib order13 bksize = 8192  
        //如果base = 0x100000 那就是 0x100000 ^ 0x2000 = 0x102000
        //明显buddy
        
        if (!zone_contains_block(zone, buddy, order)) break;//crash crash boom
        if (!remove_block(zone, order, buddy)) break;//没有buddy了，或者buddy不在空闲链表里了，那就不能合并了

        base = MIN(base, buddy);//取小的那个地址作为新的块地址
        order++;//合并并且继续往上合
    }

    push_block(zone, order, base);//把合并后的块放回链表里
    zone->free_pages += released_pages;//更新空闲页数
}

static void init_zone(zone_t *zone, enum zone_type type, uint64_t start_pfn, uint64_t end_pfn)
{
    memset(zone, 0, sizeof(zone_t));

    zone->type           = type;
    zone->name           = zone_names[type];
    zone->zone_start_pfn = start_pfn;
    zone->zone_end_pfn   = end_pfn;
    zone->allocator.lock = SPIN_INIT;
}

void buddy_init(void)
{
    uint64_t max_pfn = memory_size / PAGE_SIZE;

    uint64_t dma_end_pfn   = MIN(max_pfn, ZONE_DMA_END / PAGE_SIZE);
    uint64_t dma32_end_pfn = MIN(max_pfn, ZONE_DMA32_END / PAGE_SIZE);

    zones[ZONE_DMA] = (zone_t *)early_alloc(sizeof(zone_t));
    init_zone(zones[ZONE_DMA], ZONE_DMA, 0, dma_end_pfn);
    nr_zones++;

    if (dma_end_pfn < dma32_end_pfn)
    {
        zones[ZONE_DMA32] = (zone_t *)early_alloc(sizeof(zone_t));
        init_zone(zones[ZONE_DMA32], ZONE_DMA32, dma_end_pfn, dma32_end_pfn);
        nr_zones++;
    }

    if (dma32_end_pfn < max_pfn)
    {
        zones[ZONE_NORMAL] = (zone_t *)early_alloc(sizeof(zone_t));
        init_zone(zones[ZONE_NORMAL], ZONE_NORMAL, dma32_end_pfn, max_pfn);
        nr_zones++;
    }

    void *ptr = early_alloc((max_pfn + 7) / 8);
    bitmap_init(&using_regions, (uint8_t *)ptr, (max_pfn + 7) / 8);
}

void add_memory_region(uint64_t start, uint64_t end, enum zone_type type)
{
    zone_t *zone = zones[type];
    if (!zone) return;

    start = PADDING_UP(start, PAGE_SIZE);
    end   = PADDING_DOWN(end, PAGE_SIZE);

    if (start >= end) return;

    uint64_t zone_start = zone->zone_start_pfn * PAGE_SIZE;
    uint64_t zone_end   = zone->zone_end_pfn * PAGE_SIZE;

    if (start < zone_start) start = zone_start;
    if (end > zone_end) end = zone_end;
    if (start >= end) return;

    spin_lock(&zone->allocator.lock);

    while (start < end)
    {
        size_t order = MAX_ORDER - 1;

        while (order > MIN_ORDER)
        {
            uint64_t block_size = order_to_block_size(order);
            if ((start & (block_size - 1)) == 0 && start + block_size <= end) break;
            order--;
        }

        buddy_free_zone(zone, start, order);
        zone->managed_pages += order_to_pages(order);
        start += order_to_block_size(order);
    }

    spin_unlock(&zone->allocator.lock);
}

uint64_t alloc_frames(size_t count)
{
    if (count == 0) return 0;

    size_t   required_pages = count_to_block_pages(count);
    uint64_t addr           = 0;

    if (zones[ZONE_NORMAL] && zone_has_memory(zones[ZONE_NORMAL]))
    {
        addr = buddy_alloc_zone(zones[ZONE_NORMAL], count);
        if (addr != 0) goto ret;
    }

    if (zones[ZONE_DMA32] && zone_has_memory(zones[ZONE_DMA32]))
    {
        addr = buddy_alloc_zone(zones[ZONE_DMA32], count);
        if (addr != 0) goto ret;
    }

    if (zones[ZONE_DMA] && zone_has_memory(zones[ZONE_DMA]))
    {
        addr = buddy_alloc_zone(zones[ZONE_DMA], count);
        if (addr != 0) goto ret;
    }

ret:
    if (addr)
    {
        bitmap_set_range(&using_regions, addr / PAGE_SIZE, addr / PAGE_SIZE + required_pages, true);
        for (uint64_t a = addr; a < addr + required_pages * PAGE_SIZE; a += PAGE_SIZE)
        {
            page_t *p = get_page(a);
            page_ref(p);
        }
    }
    else
    {
        serial_wprintf("Failed to allocate memory!!!");
    }

    return addr;
}

EXPORT_SYMBOL(alloc_frames);

void free_frames(uint64_t addr, size_t count)
{
    if (addr == 0 || count == 0) return;

    size_t required_pages = count_to_block_pages(count);
    size_t order          = count_to_order(count);
    if (!order_valid(order)) return;

    uint64_t idx = addr / PAGE_SIZE;
    for (size_t off = 0; off < required_pages; off++)
    {
        if (bitmap_get(&using_regions, idx + off) == false) return;
        if (bitmap_get(&frame_allocator.bitmap, idx + off) == false) return;
    }

    enum zone_type type = phys_to_zone_type(addr);
    zone_t        *zone = zones[type];
    if (!zone) return;

    for (uint64_t a = addr; a < addr + required_pages * PAGE_SIZE; a += PAGE_SIZE)
    {
        address_unref(a);
    }
    for (uint64_t a = addr; a < addr + required_pages * PAGE_SIZE; a += PAGE_SIZE)
    {
        if (!address_can_free(a)) return;
    }

    spin_lock(&zone->allocator.lock);
    buddy_free_zone(zone, addr, order);
    bitmap_set_range(&using_regions, idx, idx + required_pages, false);
    spin_unlock(&zone->allocator.lock);
}

EXPORT_SYMBOL(free_frames);

uint64_t alloc_frames_dma32(size_t count)
{
    if (count == 0) return 0;

    size_t   required_pages = count_to_block_pages(count);
    uint64_t addr           = 0;

    if (zones[ZONE_DMA32] && zone_has_memory(zones[ZONE_DMA32]))
    {
        addr = buddy_alloc_zone(zones[ZONE_DMA32], count);
        if (addr != 0) goto ret;
    }

    if (zones[ZONE_DMA] && zone_has_memory(zones[ZONE_DMA]))
    {
        addr = buddy_alloc_zone(zones[ZONE_DMA], count);
        if (addr != 0) goto ret;
    }

ret:
    if (addr == 0) return 0;

    bitmap_set_range(&using_regions, addr / PAGE_SIZE, addr / PAGE_SIZE + required_pages, true);
    for (uint64_t a = addr; a < addr + required_pages * PAGE_SIZE; a += PAGE_SIZE)
    {
        page_t *p = get_page(a);
        page_ref(p);
    }

    return addr;
}

EXPORT_SYMBOL(alloc_frames_dma32);

void free_frames_dma32(uint64_t addr, size_t count)
{
    free_frames(addr, count);
}

EXPORT_SYMBOL(free_frames_dma32);
