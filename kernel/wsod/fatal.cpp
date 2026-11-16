// SPDX-License-Identifier: Apache-2.0
#include <apic/apic.h>
#include <cpu/msr.h>
#include <krlibc.h>
#include <mm/hhdm.h>
#include <proto.hpp>
#include <task/scheduler.h>
#include <wsod.h>

extern APIC_INFO     ApicInfo;
extern XSK_SMP_INFO *xsi;
namespace {
alignas(64) WsodCpuState states[MAX_CPU_NUM];
unsigned owner = MAX_CPU_NUM;
alignas(16) unsigned char nmi_stacks[MAX_CPU_NUM][8192];
alignas(16) unsigned char double_fault_stacks[MAX_CPU_NUM][16384];
unsigned cpu_index()
{
    PROCESSOR_INFO *cpu = get_current_cpu();
    if (xsi && cpu)
        for (unsigned i = 0; i < MAX_CPU_NUM; ++i)
            if (cpu == &xsi->pcr_inf[i]) return i;
    return 0;
}

// A lockless walk of the stopped CPU's current page tables. Never uses the
// normal page-table lock (the interrupted context may own it), or faults in a
// lazy page. Hardware/physical corruption remains outside software recovery.
bool readable(uint64_t address)
{
    if (get_physical_memory_offset() == 0) return false;
    if ((address >> 47) != 0 && (address >> 47) != 0x1ffff) return false;
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t physical = cr3 & 0x000ffffffffff000ULL;
    for (int shift = 39; shift >= 12; shift -= 9)
    {
        if (!physical) return false;
        volatile uint64_t *table = (volatile uint64_t *)phys_to_virt(physical);
        uint64_t           entry = table[(address >> shift) & 511];
        if (!(entry & 1)) return false;
        if (shift == 12 || ((shift == 30 || shift == 21) && (entry & 128))) return true;
        physical = entry & 0x000ffffffffff000ULL;
    }
    return false;
}

void text(char *out, const char *in, unsigned capacity)
{
    unsigned i = 0;
    for (; i + 1 < capacity; ++i)
    {
        char c;
        if (!in || !wsod_read_memory((uint64_t)in + i, &c, 1) || !c) break;
        out[i] = c;
    }
    out[i] = 0;
}

void capture(unsigned index, X64_REGS *regs)
{
    WsodCpuState &s = states[index];
    if (__atomic_exchange_n(&s.captured, 1U, __ATOMIC_ACQ_REL)) return;
    if (regs)
    {
        s.regs     = *regs;
        s.has_regs = true;
        // Err_Code keeps the original FXSAVE64 image immediately below the
        // unchanged register frame, rounded down to a 16-byte boundary.
        uint64_t image = ((uint64_t)regs - 512) & ~15ULL;
        s.has_fxsave   = wsod_read_memory(image, s.fxsave, sizeof(s.fxsave));
    }
    asm volatile("mov %%cr0, %0" : "=r"(s.cr0));
    asm volatile("mov %%cr2, %0" : "=r"(s.cr2));
    asm volatile("mov %%cr3, %0" : "=r"(s.cr3));
    asm volatile("mov %%cr4, %0" : "=r"(s.cr4));
    asm volatile("mov %%fs, %0" : "=r"(s.fs));
    asm volatile("mov %%gs, %0" : "=r"(s.gs));
    asm volatile("str %0" : "=r"(s.tr));
    s.fs_base        = rdmsr(0xc0000100);
    s.gs_base        = rdmsr(0xc0000101);
    s.kernel_gs_base = rdmsr(0xc0000102);
    s.efer           = rdmsr(0xc0000080);
    // DR access can itself trap when GD is armed. Record debug registers only
    // once DR7 is available in the ordinary (non-debugger) kernel policy.
    // Do not touch DRs here: a #DB reentry would destroy the original report.
    PROCESSOR_INFO *cpu = xsi ? &xsi->pcr_inf[index] : nullptr;
    s.lapic             = cpu ? cpu->lapic_id : 0;
    s.preemption_depth  = cpu ? cpu->scheduler_disable_depth : 0;
    tcb_t task          = cpu ? cpu->current_task : nullptr;
    s.task_address      = (uint64_t)task;
    if (task && readable((uint64_t)task))
    {
        wsod_read_memory((uint64_t)&task->tid, &s.tid, sizeof(s.tid));
        text(s.thread_name, task->name, sizeof(s.thread_name));
        pcb_t process = nullptr;
        wsod_read_memory((uint64_t)&task->parent_group, &process, sizeof(process));
        if (process && readable((uint64_t)process))
        {
            wsod_read_memory((uint64_t)&process->pid, &s.pid, sizeof(s.pid));
            text(s.process_name, process->name, sizeof(s.process_name));
        }
    }
    __atomic_store_n(&s.captured, 2U, __ATOMIC_RELEASE);
}
} // namespace

void wsod_setup_stacks(tss_t *tss, unsigned index)
{
    if (!tss || index >= MAX_CPU_NUM) return;
    tss->ist[1] = (uint64_t)&nmi_stacks[index][sizeof(nmi_stacks[index])];
    tss->ist[2] = (uint64_t)&double_fault_stacks[index][sizeof(double_fault_stacks[index])];
}

bool wsod_read_memory(uint64_t address, void *destination, unsigned bytes)
{
    if (address + bytes < address) return false;
    unsigned char *out = (unsigned char *)destination;
    for (unsigned i = 0; i < bytes;)
    {
        if (!readable(address + i)) return false;
        unsigned count = 4096 - ((address + i) & 4095);
        if (count > bytes - i) count = bytes - i;
        const volatile unsigned char *in = (const volatile unsigned char *)(address + i);
        for (unsigned j = 0; j < count; ++j)
            out[i + j] = in[j];
        i += count;
    }
    return true;
}

[[noreturn]] void wsod_halt()
{
    for (;;)
        asm volatile("cli; hlt" : : : "memory");
}

bool wsod_begin(X64_REGS *regs)
{
    asm volatile("cli" : : : "memory");
    unsigned index = cpu_index(), expected = MAX_CPU_NUM;
    if (!__atomic_compare_exchange_n(&owner, &expected, index, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        if (expected != index) capture(index, regs);
        return false;
    }
    // Closing the boot gate is an irreversible fatal transition; do not touch
    // any per-CPU nested preemption depth or TCB/list lifetime state.
    scheduler_stop();
    capture(index, regs);
    return true;
}

bool wsod_nmi_stop(X64_REGS *regs)
{
    unsigned first = __atomic_load_n(&owner, __ATOMIC_ACQUIRE);
    if (first == MAX_CPU_NUM) return false;
    if (first != cpu_index()) capture(cpu_index(), regs);
    wsod_halt();
}

bool wsod_stop_other_cpus()
{
    if (!xsi || !ApicInfo.LocalApicAddr) return true;
    // Destination shorthand: all excluding self, delivery mode NMI. This
    // interrupts even CPUs stopped inside CLI/spin-locked critical sections.
    if (ApicInfo.x2Apic)
        wrmsr(0x830, (3U << 18) | (4U << 8));
    else
    {
        unsigned polls = 0;
        while ((lapic_read(LAPIC_REG_ICR0) & (1U << 12)) && ++polls < 1000000)
            asm volatile("pause");
        if (polls == 1000000) return false;
        lapic_write(LAPIC_REG_ICR0, (3U << 18) | (4U << 8));
    }
    // Bounded even if the clock or an NMI target is broken. Never enter disk
    // or framebuffer work until all initialized CPUs acknowledge quiescence.
    for (unsigned polls = 0; polls < 10000000; ++polls)
    {
        bool done = true;
        for (unsigned i = 0; i < MAX_CPU_NUM; ++i)
            if (xsi->pcr_inf[i].current_task && __atomic_load_n(&states[i].captured, __ATOMIC_ACQUIRE) != 2)
                done = false;
        if (done) return true;
        asm volatile("pause");
    }
    return false;
}

const WsodCpuState *wsod_cpu_states()
{
    return states;
}
unsigned wsod_owner_index()
{
    return __atomic_load_n(&owner, __ATOMIC_ACQUIRE);
}
unsigned wsod_online_cpus()
{
    unsigned count = 0;
    if (xsi)
        for (unsigned i = 0; i < MAX_CPU_NUM; ++i)
            if (xsi->pcr_inf[i].current_task) ++count;
    return count ? count : 1;
}
