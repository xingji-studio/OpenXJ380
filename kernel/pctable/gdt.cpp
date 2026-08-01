#if defined(__x86_64__) || defined(__i386__)

#    include "smp/smp.h"
#    include <mm/memory.h>
#    include <pctable/gdt.h>
#    include <proto.hpp>
#    include <stdint.h>

uint64_t gdt_entries[7];
__attribute__((__aligned__(PAGE_SIZE)));
struct gdt_register gdt_pointer;
tss_t               tss0;
tss_stack_t         tss_stack;

void tss_setup()
{
    uint64_t address     = ((uint64_t)(&tss0));
    uint64_t low_base    = (((address & 0xffffffU)) << 16U);
    uint64_t mid_base    = (((((address >> 24U)) & 0xffU)) << 56U);
    uint64_t high_base   = (address >> 32U);
    uint64_t access_byte = (((uint64_t)(0x89U)) << 40U);
    uint64_t limit       = ((uint64_t)((uint32_t)(sizeof(tss_t) - 1U)));
    gdt_entries[5]       = (((low_base | mid_base) | limit) | access_byte);
    gdt_entries[6]       = high_base;

    tss0.ist[0] = ((uint64_t)&tss_stack) + sizeof(tss_stack_t);

    __asm__ volatile("ltr %[offset];" : : [offset] "rm"(0x28U) : "memory");
}

void init_gdt()
{
    gdt_entries[0] = 0x0000000000000000U;
    gdt_entries[1] = 0x00a09a0000000000U;
    gdt_entries[2] = 0x00c0920000000000U;
    gdt_entries[3] = 0x00c0f20000000000U;
    gdt_entries[4] = 0x00a0fa0000000000U;

    gdt_pointer = ((struct gdt_register){
        .size = ((uint16_t)((uint32_t)sizeof(gdt_entries_tT) - 1U)),
        .ptr  = &gdt_entries,
    });

    __asm__ volatile("lgdt %[ptr]\n\t"
                     "call *%%rax\n\t"
                     "mov %[dseg], %%ds\n\t"
                     "mov %[dseg], %%fs\n\t"
                     "mov %[dseg], %%gs\n\t"
                     "mov %[dseg], %%es\n\t"
                     "mov %[dseg], %%ss\n\t"
                     :
                     : [ptr] "m"(gdt_pointer), [dseg] "rm"((uint16_t)0x10U), "a"(&_setcs_helper), "b"((uint16_t)0x8U)
                     : "memory");
    tss_setup();
    write_serial_string("GDT Initialize Success.\n");
}

#endif