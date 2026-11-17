// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

// Opt in through the module's compiler flags. Normal socket progress is quiet.
#ifndef CONFIG_NETSERVER_DEBUG_TRACE
#    define CONFIG_NETSERVER_DEBUG_TRACE 0
#endif

struct NetserverLogLimiter
{
    uint64_t next_ns;
    uint64_t suppressed;
};

// A single CAS admits at most one caller per interval, without waiting for a
// metadata owner. Formatting and publication always happen after this helper.
static inline int netserver_log_admit(struct NetserverLogLimiter *limit, uint64_t now, uint64_t *suppressed)
{
    const uint64_t interval = 1000000000ULL;
    const uint64_t maximum  = ~(uint64_t)0;
    uint64_t       next     = __atomic_load_n(&limit->next_ns, __ATOMIC_RELAXED);
    uint64_t       deadline = now > maximum - interval ? maximum : now + interval;
    if (now < next || next == maximum ||
        !__atomic_compare_exchange_n(&limit->next_ns, &next, deadline, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    {
        __atomic_add_fetch(&limit->suppressed, 1ULL, __ATOMIC_RELAXED);
        return 0;
    }
    *suppressed = __atomic_exchange_n(&limit->suppressed, 0ULL, __ATOMIC_RELAXED);
    return 1;
}

#ifdef __cplusplus
extern "C" {
#endif
int netserver_lwip_diag(const char *format, ...);
#ifdef __cplusplus
}
#endif
