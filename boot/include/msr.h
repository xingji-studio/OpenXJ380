#pragma once

#include <efi.h>

static inline UINT64 rdmsr(UINT32 msr)
{
    UINT32 eax, edx;
    __asm__ volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(msr));
    return ((UINT64)edx << 32) | eax;
}

static inline void wrmsr(UINT32 msr, UINT64 value)
{
    UINT32 eax = (UINT32)value;
    UINT32 edx = value >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(eax), "d"(edx));
}