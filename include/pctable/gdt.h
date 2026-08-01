#ifndef _INCLUDE_GDT_H_
#define _INCLUDE_GDT_H_

#include <cpu/longm.h>
#include <stdint.h>

#define GDT_TYPE_ZERO      0
#define GDT_TYPE_KERNEL_CS 1
#define GDT_TYPE_KERNEL_DS 2
#define GDT_TYPE_USER_DS   3
#define GDT_TYPE_USER_CS   4
#define GDT_TYPE_TSS       6

#define SELECTOR_KERNEL_CS (GDT_TYPE_KERNEL_CS << 3)
#define SELECTOR_KERNEL_DS (GDT_TYPE_KERNEL_DS << 3)
#define SELECTOR_USER_CS   ((GDT_TYPE_USER_CS << 3) | RING3)
#define SELECTOR_USER_DS   ((GDT_TYPE_USER_DS << 3) | RING3)

#include <smp/smp.h>

// 全局描述符表长度
#define GDT_LENGTH GDT_TYPE_TSS + MAX_CPU_NUM * 2 + 1

// GDT 段描述符结构体 (64位模式)
struct GDT_ENTRY
{
    uint16_t limit;       // 段界限 [0:15]
    uint16_t base_low;    // 基地址 [0:15]
    uint8_t  base_mid;    // 基地址 [16:23]
    uint8_t  access;      // P(1) | DPL(2) | S(1) | Type(4)
    uint8_t  granularity; // G(1) | D/B(1) | L(1) | AVL(1) | Limit[16:19](4)
    uint8_t  base_hi;     // 基地址 [24:31]
} __attribute__((packed));

// GDTR
typedef struct
{
    uint16_t limit; // 全局描述符表限长
    uint64_t base;  // 全局描述符表 32 位基地址
} __attribute__((packed)) GDT_PTR;

extern GDT_PTR gdt_ptr;

#endif