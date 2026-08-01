#pragma once

#include <stdint.h>

static inline uint64_t get_cr0()
{
    uint64_t cr0 = 0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}

static inline uint64_t get_cr3()
{
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline uint64_t get_rsp()
{
    uint64_t rsp = 0;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

static inline uint64_t get_rflags()
{
    uint64_t rflags = 0;
    __asm__ volatile("pushfq\n"
                     "pop %0\n"
                     : "=r"(rflags)
                     :
                     : "memory");
    return rflags;
}

static inline void set_cr0(uint64_t cr0)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
}

static inline void flush_tlb(uint64_t addr)
{
    __asm__ volatile("invlpg (%0)" ::"r"(addr) : "memory");
}
