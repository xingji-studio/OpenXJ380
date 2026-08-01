#pragma once

#include <stdint.h>

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