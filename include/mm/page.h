#pragma once

#define PTE_PRESENT         (0x1 << 0)
#define PTE_WRITEABLE       (0x1 << 1)
#define PTE_USER            (0x1 << 2)
#define PTE_FLAG_U          (0x1 << 3)
#define PTE_HUGE            (0x1 << 7)
#define PTE_FRAME_ALLOCATED (((uint64_t)0x1) << 62)
#define PTE_NO_EXECUTE      (((uint64_t)0x1) << 63)
#define PTE_ADDR_MASK       0x000ffffffffff000ULL

#define ARCH_PT_IS_TABLE(x) (((x) & (PTE_PRESENT | PTE_WRITEABLE)) == (PTE_PRESENT | PTE_WRITEABLE))
#define ARCH_PT_IS_LARGE(x) (((x) & (PTE_PRESENT | PTE_HUGE)) == (PTE_PRESENT | PTE_HUGE))

#define MAP_ANONYMOUS    32
#define MAP_FIXED        16
#define MREMAP_MAYMOVE   1
#define MREMAP_FIXED     2
#define MREMAP_DONTUNMAP 4

#define PROT_NONE  0x00
#define PROT_READ  0x01
#define PROT_WRITE 0x02
#define PROT_EXEC  0x04

#define KERNEL_PTE_FLAGS (PTE_PRESENT | PTE_WRITEABLE)

#define PAGE_MASK  (~(PAGE_SIZE - 1))
#define ENTRY_MASK 0x1FF

#include <stdint.h>
#include <mm/memory.h>

typedef struct page
{
    int refcount;
} page_t;

extern page_t *page_maps;

void page_init();

page_t *get_page(uint64_t addr);

void page_ref(page_t *page);
void page_unref(page_t *page);
bool page_can_free(page_t *page);

void address_ref(uint64_t addr);
void address_unref(uint64_t addr);
bool address_can_free(uint64_t addr);

typedef struct page_table_entry
{
    uint64_t value;
} __attribute__((packed)) page_table_entry_t;

typedef struct
{
    page_table_entry_t entries[512];
} __attribute__((packed)) page_table_t;

typedef struct page_directory
{
    page_table_t *table;
} page_directory_t;

/**
 * 获取内核用页表
 * @return 页表指针
 */
page_directory_t *get_kernel_pagedir();

/**
 * 页错误处理
 * @param frame 帧指针
 * @param error_code 错误码
 */
#ifdef __cplusplus
extern "C" {
#endif
void handle_page_fault(struct X64_REGS *frame, uint64_t error_code);
#ifdef __cplusplus
}
#endif

/**
 * 清空页表
 * @param table 页表
 */
void page_table_clear(page_table_t *table);

/**
 * 映射一页地址到指定物理地址
 * @param directory 页表
 * @param addr 虚拟地址
 * @param frame 物理地址
 * @param flags 页表项标志位
 */
void page_map_to(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags);

/**
 * 映射一组物理地址 (对应的虚拟地址用hhdm计算)
 * @param directory 页表
 * @param frame 物理地址
 * @param length 长度
 * @param flags 页表项标志位
 */
void page_map_range_to(page_directory_t *directory, uint64_t frame, uint64_t length, uint64_t flags);

/**
 * 映射一组物理地址并指定物理地址基址
 * @param directory 页表
 * @param addr 虚拟地址
 * @param frame 物理地址
 * @param length 长度
 * @param flags 页表项标志位
 */
void page_map_range(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t length, uint64_t flags);

/**
 * 将指定虚拟地址随机映射到物理地址上(物理地址由页框分配器决定)
 * @param directory 页表
 * @param addr 虚拟地址
 * @param length 长度
 * @param flags 页表项标志位
 */
void page_map_range_to_random(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags);
bool page_map_range_to_random_checked(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags);

/**
 * 克隆指定页表
 * @param dir 源页表
 * @param all_copy 是否深拷贝 (false不会拷贝内核部分)
 * @return == NULL ? 未分配成功 : 新页表
 */
page_directory_t *clone_page_directory(page_directory_t *dir, bool all_copy);

/**
 * 释放指定页表 (不得为内核页, 内核页由引导程序提供,不遵循页框分配器的规则)
 * @param dir 待释放页表
 */
void free_page_directory(page_directory_t *dir);

/**
 * 多核页切换(不会切换进程的页表, 一般用于进程上下文切换)
 * @param dir 目标页表
 */
void switch_page_directory(page_directory_t *dir);

/**
 * 进程页切换(一般用于内核主动性页表切换)
 * @param dir 目标页表
 */
void switch_process_page_directory(page_directory_t *dir);

/**
 * 分配一页大小的内存
 * @param directory 页目录
 * @param length 长度(字节为单位)
 * @param flags 页标志
 * @return == -1 ? 未分配成功 : 地址
 */
uint64_t page_alloc_random(page_directory_t *directory, uint64_t length, uint64_t flags);
uint64_t page_reserve_user_range(page_directory_t *directory, uint64_t length);
struct lazy_address_space_owner;
uint64_t page_reserve_user_range_owner(const struct lazy_address_space_owner *owner, uint64_t length);

/**
 * 释放一段页映射 (未使用 alloc_frames 的页不可使用此方法取消映射)
 * @param directory 页表
 * @param vaddr 虚拟地址 (4k对齐)
 * @param size 大小
 */
void unmap_page_range(page_directory_t *directory, uint64_t vaddr, uint64_t size);

/**
 * 释放指定地址的映射 (未使用 alloc_frames 的页不可使用此方法取消映射)
 * @param directory 页表
 * @param vaddr 虚拟地址 (4k对齐)
 */
void unmap_page(page_directory_t *directory, uint64_t vaddr);

/**
 * 获取指定地址的页表项标志
 * @param directory 页表
 * @param addr 地址 (需要 4k 对齐)
 * @param out_flags 输出的标志
 * @return 是否成功获取
 */
bool page_table_get_flags(page_directory_t *directory, uint64_t addr, uint64_t *out_flags);

/**
 * 设置指定地址的页表项标志
 * @param root 页表
 * @param addr 地址 (需要 4k 对齐)
 * @param new_flags 标志
 * @return 是否成功设置
 */
bool page_table_update_flags(page_directory_t *directory, uint64_t addr, uint64_t new_flags);

/**
 * 获取当前CPU核心的页表
 * @return 页表指针
 */
page_directory_t *get_current_directory();

/*
 * 将引导器提供的页表置换成 XSK 自己构建的页表
 * 为解决引导器页表占用内存未被标记容易被覆写问题
 */
void switch_xsk_page_directory();
void page_setup();

uint64_t translate_address(page_directory_t *page_dir, uint64_t virtual_address);
