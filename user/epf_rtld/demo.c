// SPDX-License-Identifier: Apache-2.0
#include <stdint.h>
#include "../rtld/platform.h"

extern int epf_demo_data, epf_demo_fd, epf_demo_initialized;
extern int *epf_demo_pointer;
extern int *const epf_demo_relro_pointer;
extern int epf_demo_add(int);
extern int epf_demo_missing(void) __attribute__((weak));
static int local_data = 7;
static int *volatile local_pointer = &local_data;

int main(int argc, char **argv, char **envp)
{
    if (argc < 0 || !argv || argv[argc] || !envp || *local_pointer != 7 || !epf_demo_initialized ||
        epf_demo_data != 41 || epf_demo_pointer != &epf_demo_data || epf_demo_relro_pointer != &epf_demo_data ||
        epf_demo_add(1) != 51 || epf_demo_missing) return 1;
    if (argc > 1)
    {
        long fd = rtld_syscall(2, (long)argv[1], 0x242, 0600, 0, 0, 0);
        if (fd < 0) return 2;
        epf_demo_fd = (int)fd;
        // This reaches the kernel's user-copy path, proving that RELRO also
        // blocks syscall writes through HHDM, not only ordinary CPU stores.
        if (rtld_syscall(1, fd, (long)"X", 1, 0, 0, 0) != 1 ||
            rtld_syscall(8, fd, 0, 0, 0, 0, 0) ||
            rtld_syscall(0, fd, (long)&epf_demo_relro_pointer, 1, 0, 0, 0) != -14 ||
            rtld_syscall(8, fd, 0, 0, 0, 0, 0) || epf_demo_relro_pointer != &epf_demo_data) return 4;
    }
    int private_value = (int)rtld_syscall(39, 0, 0, 0, 0, 0, 0);
    epf_demo_data = private_value;
    for (unsigned i = 0; i < 20; ++i)
    {
        int64_t delay[2] = {0, 10000000};
        if (rtld_syscall(35, (long)delay, 0, 0, 0, 0, 0) || epf_demo_data != private_value ||
            epf_demo_add(1) != private_value + 10) return 3;
    }
    rtld_write(epf_demo_fd >= 0 ? epf_demo_fd : 1, "EPF_DYNAMIC PASS binding=ok bss=ok private=ok relro=ok\n");
    return 0;
}
