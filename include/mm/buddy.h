#pragma once

#include <mm/hhdm.h>

#define MAX_ORDER 31
#define MIN_ORDER 12 // 4KB = 2^12
#define ORDER_COUNT (MAX_ORDER - MIN_ORDER + 1)

typedef struct buddy_allocator {
    uint64_t free_area[ORDER_COUNT]; // 每个 order 的空闲块链表头
    spin_t lock;
} buddy_allocator_t;

// Zone 类型
enum zone_type {
    ZONE_DMA, // 0-16MB
    ZONE_DMA32,  // 0-4GB
    ZONE_NORMAL, // 4GB+
    __MAX_NR_ZONES
};

// Zone 边界
#define ZONE_DMA_END (16UL << 20)  // 16MB
#define ZONE_DMA32_END (4UL << 30) // 4GB

// GFP 标志
#define GFP_DMA (1 << 0)
#define GFP_DMA32 (1 << 1)
#define GFP_KERNEL (1 << 2)
#define GFP_ATOMIC (1 << 3)
#define GFP_NOWAIT (1 << 4)

#define GFP_KERNEL_NORMAL (GFP_KERNEL)
#define GFP_KERNEL_DMA (GFP_KERNEL | GFP_DMA)
#define GFP_KERNEL_DMA32 (GFP_KERNEL | GFP_DMA32)

// Zone 结构
typedef struct zone {
    buddy_allocator_t allocator;
    uint64_t zone_start_pfn;
    uint64_t zone_end_pfn;
    uint64_t managed_pages;
    uint64_t free_pages;
    enum zone_type type;
    const char *name;
} zone_t;

extern zone_t *zones[__MAX_NR_ZONES];
extern int nr_zones;
extern const char *zone_names[__MAX_NR_ZONES];

// 初始化
void buddy_init(void);
void add_memory_region(uint64_t start, uint64_t end, enum zone_type type);

// 分配/释放（底层接口）
uint64_t buddy_alloc_zone(zone_t *zone, size_t count);
void buddy_free_zone(zone_t *zone, uint64_t addr, size_t order);

// 辅助函数
zone_t *get_zone(enum zone_type type);
bool zone_has_memory(zone_t *zone);
enum zone_type phys_to_zone_type(uint64_t phys);
