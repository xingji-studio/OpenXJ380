// SPDX-License-Identifier: Apache-2.0
// Public-tree example: use its existing exported syscall bridge via libxapi.
#include <stdint.h>

extern uint64_t enter_syscall(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern char **environ;

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    if (environ != envp || !envp) return 1;
    const char message[] = "XAPI_SHARED_DEMO PASS environment=ok imported_syscall=ok\n";
    return enter_syscall(1, 1, (uint64_t)message, sizeof(message) - 1, 0, 0, 0) == sizeof(message) - 1 ? 0 : 1;
}
