// SPDX-License-Identifier: Apache-2.0
#include "include/errno.h"
#include "include/xnet_runtime.h"

extern "C" int xapi_secure_random(void *output, uint32_t bytes)
{
    if (bytes && !output) return -EINVAL;
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    bool seed = false;
    if (a >= 7)
    {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
        seed = (b & (1U << 18)) != 0;
    }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    bool     random = (c & (1U << 30)) != 0;
    auto    *data   = static_cast<uint8_t *>(output);
    uint32_t offset = 0;
    while (offset < bytes)
    {
        uint64_t      word = 0;
        unsigned char ok   = 0;
        if (seed)
            for (unsigned retry = 0; retry < 10 && !ok; ++retry)
                __asm__ volatile("rdseed %0; setc %1" : "=r"(word), "=qm"(ok)::"cc");
        if (random && !ok)
            for (unsigned retry = 0; retry < 10 && !ok; ++retry)
                __asm__ volatile("rdrand %0; setc %1" : "=r"(word), "=qm"(ok)::"cc");
        if (!ok)
        {
            volatile uint8_t *erase = data;
            for (uint32_t i = 0; i < bytes; ++i)
                erase[i] = 0;
            return seed || random ? -EIO : -EOPNOTSUPP;
        }
        for (unsigned i = 0; i < 8 && offset < bytes; ++i, ++offset)
            data[offset] = uint8_t(word >> (i * 8));
        __asm__ volatile("" : "+r"(word)::"memory");
        word = 0;
    }
    return 0;
}
