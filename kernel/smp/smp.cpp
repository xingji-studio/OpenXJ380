#include "../build_settings.h"
#include "cpu/lock.h"
#include "cpu/regio.h"
#include "pctable/gdt.h"
#include "pctable/idt.h"
#include "proto.hpp"
#include <apic/apic.h>
#include <apic/madt.h>
#include <cpu/msr.h>
#include <cpu/fsgsbase.h>
#include <krlibc.h>
#include <mm/alloc/alloc.h>
#include <syscall/syscall.h>
#include <task/scheduler.h>

XSK_SMP_INFO *xsi;

extern APIC_INFO ApicInfo;
extern void     *temp_stack[MAX_CPU_NUM];
extern uint32_t  smp_trampoline_rs;
extern size_t    now_tid;

extern pcb_t kernel_group;

extern bool smp_scheduler_lock;
int         scheduler_is_ready = 0;

static tcb_t alloc_zeroed_tcb(void)
{
    const size_t alloc_size = (sizeof(struct thread_control_block) + 15ULL) & ~15ULL;
    tcb_t        task = (tcb_t)aligned_alloc(16, alloc_size);
    if (task != NULL) memset(task, 0, sizeof(struct thread_control_block));
    return task;
}

typedef char symbol[];

extern uint8_t _APU_boot_start;
extern uint8_t _APU_boot_end;

extern IDTR_TYPE idt_ptr;

#define APU_BASE_ADDR 0x10000

#define smp_lock_read(var)                                                                                             \
    ({                                                                                                                 \
        typeof(*var) locked_read__ret = 0;                                                                             \
        asm volatile("lock xadd %0, %1" : "+r"(locked_read__ret) : "m"(*(var)) : "memory");                            \
        locked_read__ret;                                                                                              \
    })

spin_t apu_lock = SPIN_INIT;

#define ltr(n)                                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        __asm__ __volatile__("ltr	%%ax" : : "a"(n << 3) : "memory");                                                 \
    } while (0)

__attribute__((naked)) void _setcs_helper()
{
    __asm__ volatile("pop %%rax\n\t"
                     "push %%rbx\n\t"
                     "push %%rax\n\t"
                     "lretq\n\t" ::
                         : "memory");
}

void init_cpu();

extern vfs_node_t rootdir;

extern "C" void apu_entry(uint64_t cpu_id, uint64_t tr)
{
    disable_intr();
    spin_lock(&apu_lock);

    uint64_t ia32_apic_base  = rdmsr(0x1b);
    ia32_apic_base          |= 1 << 11;
    if (ApicInfo.x2Apic) { ia32_apic_base |= 1 << 10; }
    wrmsr(0x1b, ia32_apic_base);

    PROCESSOR_INFO *info = NULL;
    for (size_t i = 0; i < MAX_CPU_NUM; i++)
    {
        PROCESSOR_INFO *inter_info = &xsi->pcr_inf[i];
        if (inter_info->lapic_id == lapic_id())
        {
            info = inter_info;
            break;
        }
    }
    if (info == NULL)
    {
        write_serial_string("APU Entry Failed: CPU not found.\n");
        while (true)
        {
            asm volatile("hlt");
        }
    }
    write_kgsbase((uint64_t)info);

    init_cpu();

    info->gdt_entries_t[0] = 0x0000000000000000U;
    info->gdt_entries_t[1] = 0x00a09a0000000000U;
    info->gdt_entries_t[2] = 0x00c0920000000000U;
    info->gdt_entries_t[3] = 0x00c0f20000000000U;
    info->gdt_entries_t[4] = 0x00a0fa0000000000U;
    info->gdt_pointer      = ((struct gdt_register){
             .size = ((uint16_t)((uint32_t)sizeof(gdt_entries_tT) - 1U)),
             .ptr  = &info->gdt_entries_t,
    });

    __asm__ volatile("lgdt %[ptr]\n\t"
                     "call *%%rax\n\t"
                     "mov %[dseg], %%ds\n\t"
                     "mov %[dseg], %%fs\n\t"
                     "mov %[dseg], %%gs\n\t"
                     "mov %[dseg], %%es\n\t"
                     "mov %[dseg], %%ss\n\t"
                     :
                     : [ptr] "m"(info->gdt_pointer), [dseg] "rm"((uint16_t)0x10U), "a"(&_setcs_helper),
                       "b"((uint16_t)0x8U)
                     : "memory");

    __asm__ volatile("lidt %0" : : "m"(idt_ptr));

    uint64_t address     = (uint64_t)&(info->tss0);
    uint64_t low_base    = (((address & 0xffffffU)) << 16U);
    uint64_t mid_base    = (((((address >> 24U)) & 0xffU)) << 56U);
    uint64_t high_base   = (address >> 32U);
    uint64_t access_byte = (((uint64_t)(0x89U)) << 40U);
    uint64_t limit       = ((uint64_t)(uint32_t)(sizeof(tss_t) - 1U));

    info->gdt_entries_t[5] = (((low_base | mid_base) | limit) | access_byte);
    info->gdt_entries_t[6] = high_base;

    info->tss0.ist[0] = ((uint64_t)&(info->tss_stack)) + sizeof(tss_stack_t);

    __asm__ volatile("ltr %[offset]\n\t" : : [offset] "rm"(0x28U) : "memory");

    init_lApic();

    init_syscall();

    while (true)
    {
        if (!smp_scheduler_lock) break;
    }

    tcb_t apu_idle         = alloc_zeroed_tcb();
    if (apu_idle == NULL) while (true) { asm volatile("hlt"); }
    apu_idle->task_level   = TASK_IDLE_LEVEL;
    apu_idle->tid          = __atomic_fetch_add(&now_tid, 1, __ATOMIC_SEQ_CST);
    apu_idle->kernel_stack = apu_idle->context0.rsp = get_rsp();
    set_kernel_stack(apu_idle->kernel_stack);
    apu_idle->context0.rflags = get_rflags() | 0x200;
    apu_idle->cpu_id          = lapic_id();
    apu_idle->status          = RUNNING;
    strcpy(apu_idle->name, "XSK_IDLE");
    apu_idle->parent_group = kernel_group;
    apu_idle->str_cwd      = "/";
    apu_idle->cwd          = rootdir;
    apu_idle->fs           = 0x10;
    apu_idle->fs_base      = 0;
    scheduler_init_task(apu_idle);

    info->current_task    = apu_idle;
    apu_idle->queue_index = queue_enqueue_ref(info->scheduler_queue, apu_idle, &apu_idle->sched_node);

    get_current_cpu()->current_task = apu_idle;

    scheduler_is_ready++;
    spin_unlock(&apu_lock);
    enable_intr();

    while (true)
    {
        asm volatile("pause");
    }

    asm volatile("hlt"); // 你来这干什么？？？
}

bool start_ap(uint32_t cs_number, uint32_t lapic_id, PROCESSOR_INFO *cpuinf)
{
    write_serial_string("Initializing CPU ");
    write_serial_dec(lapic_id);
    write_serial_string(".\n");

    // 准备跳板
    static void *trampoline = (void *)(APU_BASE_ADDR);
    memcpy((void *)(APU_BASE_ADDR + 0xFFFF800000000000), &_APU_boot_start,
           (uint64_t)&_APU_boot_end - (uint64_t)&_APU_boot_start);

    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *page_table = (uint64_t *)(APU_BASE_ADDR + 8 + 0xFFFF800000000000);
    uint64_t *ap_entry   = page_table + 1;
    uint64_t *rsp        = ap_entry + 1;
    uint64_t *cpu_id     = rsp + 1;
    uint64_t *tr         = cpu_id + 1;
    uint64_t *ready      = tr + 1;

    *page_table = cr3;
    *ap_entry   = (uint64_t)apu_entry;

    *rsp    = (uint64_t)((uint64_t)(temp_stack[cs_number]) + STACK_SIZE);
    *cpu_id = cs_number;

    asm volatile("" ::: "memory");

    // Send the INIT IPI
    if (ApicInfo.x2Apic) { lapic_write(LAPIC_REG_ICR0, ((uint64_t)lapic_id << 32) | 0x4500); }
    else
    {
        lapic_write(LAPIC_REG_ICR1, lapic_id << 24);
        lapic_write(LAPIC_REG_ICR0, 0x4500);
    }
    //一群250
    for (int i = 0; i < 250; i++)
        cpu_relax(); //睡吧宝贝

    // Send the Startup IPI
    if (ApicInfo.x2Apic)
    {
        lapic_write(LAPIC_REG_ICR0, ((uint64_t)lapic_id << 32) | ((size_t)trampoline >> 12) | 0x4600);
    }
    else
    {
        lapic_write(LAPIC_REG_ICR1, lapic_id << 24);
        lapic_write(LAPIC_REG_ICR0, ((size_t)trampoline >> 12) | 0x4600);
    }

    // 有的CPU（如275HX）会跑太快。。。
    while (1)
    // for (int i = 0; i < 250; i++)
    {
        if (*ready == 1)
        {
            write_serial_string("CPU ");
            write_serial_dec(lapic_id);
            write_serial_string(" Initialize Success.\n");
            return true;
        }
        for (int i = 0; i < 250; i++)
        {
            cpu_relax();
        }
    }

    return false;
}

PROCESSOR_INFO *get_current_cpu()
{
    PROCESSOR_INFO *info = NULL;
    for (size_t i = 0; i < MAX_CPU_NUM; i++)
    {
        PROCESSOR_INFO *inter_info = &xsi->pcr_inf[i];
        if (inter_info->lapic_id == lapic_id())
        {
            info = inter_info;
            break;
        }
    }
    return info;
}

uint64_t get_cpu_num()
{
    return xsi->cpu_count;
}

uint64_t get_bsp()
{
    return xsi->bsp_lapic_id;
}

PROCESSOR_INFO *get_cpu(uint64_t id)
{
    PROCESSOR_INFO *cpu = &xsi->pcr_inf[id];
    return cpu;
}

void set_kernel_stack(uint64_t rsp)
{
    if (lapic_id() == xsi->bsp_lapic_id)
    {
        extern tss_t tss0;
        tss0.rsp[0] = rsp;
        return;
    }
    PROCESSOR_INFO *info = NULL;
    for (size_t i = 0; i < MAX_CPU_NUM; i++)
    {
        PROCESSOR_INFO *inter_info = &xsi->pcr_inf[i];
        if (inter_info->lapic_id == lapic_id())
        {
            info = inter_info;
            break;
        }
    }
    if (info == NULL)
    {
        write_serial_string("Set Kernel Stack Failed: CPU not found.\n");
        while (true)
        {
            asm volatile("hlt");
        }
    }
    info->tss0.rsp[0] = rsp;
}

void init_smp(uint64_t MADT0)
{
    xsi = (XSK_SMP_INFO *)malloc(sizeof(XSK_SMP_INFO));
    write_serial_string("Initializing SMP...\n");

    uint8_t  bsp_lapic_id;
    uint32_t bsp_x2apic_id;

    if (ApicInfo.x2Apic)
    {
        bsp_x2apic_id = lapic_read(LAPIC_REG_ID);
        bsp_lapic_id  = bsp_x2apic_id;
    }
    else
    {
        bsp_lapic_id  = lapic_read(LAPIC_REG_ID) >> 24;
        bsp_x2apic_id = bsp_lapic_id;
    }

    xsi->bsp_lapic_id = bsp_x2apic_id;

    xsi->cpu_count = 0;

    write_serial_string("Searching CPUs...\n");
    size_t   max_cpus = 0;
    MADT    *madt     = (MADT *)(MADT0 + 0xffff800000000000);
    uint64_t current  = 0;
    for (;;)
    {
        if (current + ((uint32_t)sizeof(MADT) - 1) >= madt->h.Length) { break; }
        MadtHeader *header = (MadtHeader *)((uint64_t)(&madt->entries) + current);
        if (header->entry_type == MADT_APIC_LOCAL)
        {
            madt_local_apic *lapic = (madt_local_apic *)((uint64_t)(&madt->entries) + current);

            // 让我看看你发育正不正常呀
            if ((lapic->flags & 1) ^ ((lapic->flags >> 1) & 1)) max_cpus++;
        }
        else if (header->entry_type == MADT_X2APIC_LOCAL)
        {
            if (!ApicInfo.x2Apic) continue;

            madt_x2_localapic *x2lapic = (madt_x2_localapic *)((uint64_t)(&madt->entries) + current);

            if ((x2lapic->flags & 1) ^ ((x2lapic->flags >> 1) & 1)) max_cpus++;
        }
        current += (uint64_t)header->length;
    }

    xsi->cpu_count = 0;

    mtrr_save();

    write_serial_string("Start Up APs...\n");
    // 尝试启动所有AP
    current = 0;
    for (;;)
    {
        if (current + ((uint32_t)sizeof(MADT) - 1) >= madt->h.Length) { break; }
        MadtHeader *header = (MadtHeader *)((uint64_t)(&madt->entries) + current);
        if (header->entry_type == MADT_APIC_LOCAL)
        {
            madt_local_apic *lapic = (madt_local_apic *)((uint64_t)(&madt->entries) + current);

            // 让我看看你发育正不正常呀
            if (!((lapic->flags & 1) ^ ((lapic->flags >> 1) & 1)))
            {
                current += (uint64_t)header->length;
                continue;
            }

            PROCESSOR_INFO *cpuinf  = &xsi->pcr_inf[xsi->cpu_count];
            cpuinf->processor_id    = lapic->ACPI_Processor_UID;
            cpuinf->lapic_id        = lapic->local_apic_id;
            cpuinf->scheduler_queue = queue_init();

            // 如果是BSP就跑路
            if (lapic->local_apic_id == bsp_lapic_id)
            {
                (xsi->cpu_count)++;
                current += (uint64_t)header->length;
                continue;
            }

            if (!start_ap(xsi->cpu_count, lapic->local_apic_id, cpuinf))
            {
                pr_err("Failed to Bring Up AP.\n");
                current += (uint64_t)header->length;
                continue;
            }

            (xsi->cpu_count)++;
        }
        else if (header->entry_type == MADT_X2APIC_LOCAL)
        {
            madt_x2_localapic *x2lapic = (madt_x2_localapic *)((uint64_t)(&madt->entries) + current);

            // 让我看看你发育正不正常呀
            if (!((x2lapic->flags & 1) ^ ((x2lapic->flags >> 1) & 1)))
            {
                current += (uint64_t)header->length;
                continue;
            }

            PROCESSOR_INFO *cpuinf  = &xsi->pcr_inf[xsi->cpu_count];
            cpuinf->processor_id    = x2lapic->acpi_processor_uid;
            cpuinf->lapic_id        = x2lapic->x2apic_id;
            cpuinf->scheduler_queue = queue_init();

            // 如果是BSP就跑路
            if (x2lapic->x2apic_id == bsp_lapic_id)
            {
                (xsi->cpu_count)++;
                current += (uint64_t)header->length;
                continue;
            }

            if (!start_ap(xsi->cpu_count, x2lapic->x2apic_id, cpuinf))
            {
                pr_err("Failed to Bring Up AP.\n");
                current += (uint64_t)header->length;
                continue;
            }

            (xsi->cpu_count)++;
        }
        current += (uint64_t)header->length;
    }
    write_serial_fmt("SMP init with %d cpus\n", xsi->cpu_count);
}
