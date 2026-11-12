// SPDX-License-Identifier: Apache-2.0
#include "platform.h"

int native_demo_data = 41;
int native_demo_fd = -1;
int native_demo_initialized;
int *native_demo_pointer = &native_demo_data;
static int local_data = 5;
static int *volatile local_pointer = &local_data;
static unsigned char zero_fill[6001];

__attribute__((constructor)) static void initialize(void)
{
    for (size_t i = 0; i < sizeof(zero_fill); ++i) if (zero_fill[i]) rtld_exit(90);
    zero_fill[6000] = 9;
    native_demo_initialized = 1;
}

int native_demo_add(int value)
{
    return *native_demo_pointer + value + *local_pointer + zero_fill[6000];
}

__attribute__((destructor)) static void finalize(void)
{
    const char message[] = "RTLD_DEMO FINI\n";
    rtld_write(native_demo_fd >= 0 ? native_demo_fd : 1, message);
    if (native_demo_fd >= 0)
    {
        rtld_syscall(74, native_demo_fd, 0, 0, 0, 0, 0);
        rtld_syscall(3, native_demo_fd, 0, 0, 0, 0, 0);
    }
}
