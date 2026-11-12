// SPDX-License-Identifier: Apache-2.0
#include "platform.h"

extern int native_demo_data;
extern int native_demo_fd;
extern int native_demo_initialized;
extern int *native_demo_pointer;
extern int native_demo_add(int value);
extern int native_demo_missing(void) __attribute__((weak));

static int local_data = 7;
static int *volatile local_pointer = &local_data;

int main(int argc, char **argv, char **envp)
{
    if (argc < 0 || !argv || argv[argc] || !envp || *local_pointer != 7 ||
        !native_demo_initialized || native_demo_data != 41 || native_demo_pointer != &native_demo_data ||
        native_demo_add(1) != 56 || native_demo_missing) return 1;
    if (argc > 1)
    {
        long fd = rtld_syscall(2, (long)argv[1], 0x241, 0600, 0, 0, 0);
        if (fd < 0) return 2;
        native_demo_fd = (int)fd;
    }
    int private_value = (int)rtld_syscall(39, 0, 0, 0, 0, 0, 0);
    native_demo_data = private_value;
    // Concurrent guest instances deliberately retain distinct values across
    // scheduling. Both must have observed the library's initial value of 41.
    for (unsigned i = 0; i < 30; ++i)
    {
        int64_t delay[2] = {0, 10000000};
        if (rtld_syscall(35, (long)delay, 0, 0, 0, 0, 0) != 0 || native_demo_data != private_value ||
            native_demo_add(1) != private_value + 15) return 3;
    }
    rtld_write(native_demo_fd >= 0 ? native_demo_fd : 1,
               "RTLD_DEMO PASS initial=41 relocations=ok bss=ok private=ok\n");
    return 0;
}
