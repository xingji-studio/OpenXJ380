#include "cpu/fsgsbase.h"
#include "proto.hpp"

uint64_t (*read_fsbase)()            = read_fsbase_msr;
void (*write_fsbase)(uint64_t value) = write_fsbase_msr;
uint64_t (*read_gsbase)()            = read_gsbase_msr;
void (*write_gsbase)(uint64_t value) = write_gsbase_msr;

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t eax, edx;
    __asm__ volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(msr));
    return ((uint64_t)edx << 32) | eax;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t eax = (uint32_t)value;
    uint32_t edx = value >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(eax), "d"(edx));
}

uint64_t read_fsbase_msr()
{
    return rdmsr(IA32_FS_BASE);
}

void write_fsbase_msr(uint64_t value)
{
    wrmsr(IA32_FS_BASE, value);
}

uint64_t read_gsbase_msr()
{
    return rdmsr(IA32_GS_BASE);
}

void write_gsbase_msr(uint64_t value)
{
    wrmsr(IA32_GS_BASE, value);
}

uint64_t rdfsbase()
{
    uint64_t ret;
    __asm__ __volatile__("rdfsbase %0" : "=r"(ret));
    return ret;
}

void wrfsbase(uint64_t value)
{
    __asm__ __volatile__("wrfsbase %0" ::"r"(value));
}

uint64_t rdgsbase()
{
    uint64_t ret;
    __asm__ __volatile__("rdgsbase %0" : "=r"(ret));
    return ret;
}

void wrgsbase(uint64_t value)
{
    __asm__ __volatile__("wrgsbase %0" ::"r"(value));
}

uint64_t read_kgsbase()
{
    return rdmsr(IA32_KERNEL_GS_BASE);
}

void write_kgsbase(uint64_t value)
{
    wrmsr(IA32_KERNEL_GS_BASE, value);
}

uint32_t has_fsgsbase()
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x07), "c"(0x00));
    return ebx & (1 << 0);
}

uint64_t fsgsbase_init()
{
    uint32_t support = has_fsgsbase();
    if (support) {
        uint64_t cr4 = 0;
        __asm__ __volatile__("movq %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 16);
        __asm__ __volatile__("movq %0, %%cr4" ::"r"(cr4) : "memory");

        read_fsbase  = rdfsbase;
        write_fsbase = wrfsbase;
        read_gsbase  = rdgsbase;
        write_gsbase = wrgsbase;
    }

    write_serial_string("fsgsbase support:");
    if (support)
        write_serial_string("cpu");
    else
        write_serial_string("msr");
    write_serial_string("\n");
    return 0;
}
