#include "./include/stdint.h"

extern "C" uint64_t enter_syscall(uint64_t syscall_number, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4,
                                  uint64_t arg5, uint64_t arg6)
{
    uint64_t      ret;
    register long r10 __asm__("r10") = arg4;
    register long r8 __asm__("r8")   = arg5;
    register long r9 __asm__("r9")   = arg6;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(syscall_number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
                         : "rcx", "r11", "memory");
    return ret;
}

extern "C" uint64_t __stack_chk_guard = 0x5858333830475541ULL;

extern "C" __attribute__((noreturn)) void __stack_chk_fail(void)
{
    for (;;) {}
}
