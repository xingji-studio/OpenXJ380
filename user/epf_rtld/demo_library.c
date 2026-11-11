// SPDX-License-Identifier: Apache-2.0
#include <stdint.h>
#include "../rtld/platform.h"

int epf_demo_data = 41;
int epf_demo_fd = -1;
int epf_demo_initialized;
int *epf_demo_pointer = &epf_demo_data;
int *const epf_demo_relro_pointer = &epf_demo_data;
static unsigned char zero_fill[6001];

__attribute__((constructor)) static void initialize(void)
{
    for (size_t i = 0; i < sizeof(zero_fill); ++i) if (zero_fill[i]) rtld_exit(90);
    zero_fill[6000] = 9;
    epf_demo_initialized = 1;
}

int epf_demo_add(int value)
{
    return *epf_demo_pointer + value + zero_fill[6000];
}

__attribute__((destructor)) static void finalize(void)
{
    rtld_write(epf_demo_fd >= 0 ? epf_demo_fd : 1, "EPF_DYNAMIC FINI\n");
    if (epf_demo_fd >= 0)
    {
        rtld_syscall(74, epf_demo_fd, 0, 0, 0, 0, 0);
        rtld_syscall(3, epf_demo_fd, 0, 0, 0, 0, 0);
    }
}
