// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

// Bootstrap code must work before a libc, heap, TLS or relocated GOT exists.
static inline long rtld_syscall(long number, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "+a"(number) : "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return number;
}

static inline int rtld_failed(long result)
{
    return (unsigned long)result >= (unsigned long)-4095;
}

static inline size_t rtld_length(const char *text)
{
    size_t length = 0;
    while (text[length]) ++length;
    return length;
}

static inline void rtld_write(long fd, const char *text)
{
    size_t remaining = rtld_length(text);
    while (remaining)
    {
        long written = rtld_syscall(1, fd, (long)text, (long)remaining, 0, 0, 0);
        if (written == -4) continue;
        if (written <= 0) break;
        text += written;
        remaining -= (size_t)written;
    }
}

__attribute__((noreturn)) static inline void rtld_exit(int status)
{
    rtld_syscall(231, status, 0, 0, 0, 0, 0);
    for (;;) __asm__ volatile("ud2");
}
