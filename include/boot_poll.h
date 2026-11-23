// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

// Optional bootstrap work, before normal workers can run. The callback may
// only perform bounded, allocation-free stores: no locks, VFS/device calls,
// waits or GUI calls. It may update a prevalidated bootstrap framebuffer and
// can run concurrently on the BSP and fully initialized idle APs.
using BootPollHook = void (*)(uint64_t now_ns);
extern BootPollHook boot_poll_hook;

inline void boot_poll(uint64_t now_ns)
{
    BootPollHook hook = __atomic_load_n(&boot_poll_hook, __ATOMIC_ACQUIRE);
    if (hook) hook(now_ns);
}

void boot_poll_idle();
