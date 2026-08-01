/*
 *
 *
 *      XJ380 终端应用程序启动框架
 *      2025/11/5 - GuoqiFish
 *      Copyright(C) XINGJI Interactive Software 2017 - 2026 All rights reserved.
 * 
 * 
 */

#include "./include/libsys.h"

extern "C" int main(int argc, char *argv[], char *envp[]) __attribute__((weak));
extern "C" int xapi_legacy_cpp_main(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_") __attribute__((weak));
// extern void _init();
// extern void __preinit_array_start();
// extern void __preinit_array_end();
// extern void __init_array_start();
// extern void __init_array_end();
// extern void __fini_array_start();
// extern void __fini_array_end();

extern "C" __attribute__((force_align_arg_pointer)) void xapi_start(int argc, char *argv[], char *envp[])
{
    //{
    //    void (*fn)() = __preinit_array_start;
    //    while (fn != __preinit_array_end)
    //    {
    //        (*fn)();
    //        fn++;
    //    }
    //}

    //_init();

    //{
    //    void (*fn)() = __init_array_start;
    //    while (fn != __init_array_end)
    //    {
    //        (*fn)();
    //        fn++;
    //    }
    //}

    if (main != NULL)
    {
        enter_syscall(SYS_EXIT, (uint64_t)main(argc, argv, envp), 0, 0, 0, 0, 0);
    }

    if (xapi_legacy_cpp_main != NULL)
    {
        enter_syscall(SYS_EXIT, (uint64_t)xapi_legacy_cpp_main(argc, argv, envp), 0, 0, 0, 0, 0);
    }

    enter_syscall(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    for (;;) {}
}
