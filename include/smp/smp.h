#ifndef _SMP_H_
#define _SMP_H_

#ifndef CONFIG_MAX_CPU_NUM
#define CONFIG_MAX_CPU_NUM 256
#endif

#define MAX_CPU_NUM CONFIG_MAX_CPU_NUM

#include "lock_queue.h"
#include "task/pcb.h"
#include <stdint.h>

struct gdt_register
{
    uint16_t size;
    void    *ptr;
} __attribute__((packed));

struct tss
{
    uint32_t unused0;
    uint64_t rsp[3];
    uint64_t unused1;
    uint64_t ist[7];
    uint64_t unused2;
    uint16_t unused3;
    uint16_t iopb;
} __attribute__((packed));

typedef struct tss tss_t;
typedef uint64_t   gdt_entries_tT[7];
typedef uint8_t    tss_stack_t[1024];

struct PROCESSOR_INFO
{
    uint64_t            processor_id;
    uint64_t            lapic_id;         // 字面意思。
    uint64_t            gdt_entries_t[7]; // GDT条目
    struct gdt_register gdt_pointer;
    tss_t               tss0;
    tss_stack_t         tss_stack;
    tcb_t               current_task;    // 当前核心运行的任务
    lock_queue         *scheduler_queue; // 该核心的调度队列
    lock_node          *iter_node;       // 该核心当前调度迭代的节点
    uint64_t            scheduler_ticks; // 当前时间片已运行的 tick 数
    uint64_t            syscall_user_rsp;
    uint64_t            syscall_user_rax;
};

struct XSK_SMP_INFO
{
    size_t                cpu_count;            // CPU 核心总数
    uint32_t              bsp_lapic_id;         // BSP 的lapic id
    struct PROCESSOR_INFO pcr_inf[MAX_CPU_NUM]; // 各个核心的数据
};

void _setcs_helper();

uint64_t        get_cpu_num();
PROCESSOR_INFO *get_cpu(uint64_t id);
uint64_t        get_bsp();

#endif
