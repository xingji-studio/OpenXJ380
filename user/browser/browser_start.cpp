#include "../xapi/include/x3api.h"
#include "../xapi/include/libsys.h"

extern "C" int browser_cpp_main(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int xtls_prepare_runtime(uint64_t *saved_fs, void **tls_block);
extern "C" void xtls_restore_runtime(uint64_t saved_fs, void *tls_block);
extern "C" void (*__preinit_array_start[])(void);
extern "C" void (*__preinit_array_end[])(void);
extern "C" void (*__init_array_start[])(void);
extern "C" void (*__init_array_end[])(void);
extern "C" void *g_browser_heap_base = NULL;
extern "C" unsigned long long g_browser_heap_size = 0;

static void run_init_array(void (**begin)(void), void (**end)(void))
{
    for (void (**fn)(void) = begin; fn != end; ++fn) {
        if (*fn != 0) {
            (*fn)();
        }
    }
}

extern "C" __attribute__((force_align_arg_pointer)) void xapi_start(int argc, char *argv[], char *envp[])
{
    // browser 使用自定义启动器，也需要显式挂接 libc environ。
    environ = envp;

    uint64_t runtime_saved_fs = 0;
    void    *runtime_tls_block = NULL;
    if (xtls_prepare_runtime(&runtime_saved_fs, &runtime_tls_block) != 0) {
        xapi_OutputSerial((char *)"browser: prepare runtime failed\n");
        exit(-1);
    }

    run_init_array(__preinit_array_start, __preinit_array_end);
    run_init_array(__init_array_start, __init_array_end);

    int ret = browser_cpp_main(argc, argv, envp);
    xtls_restore_runtime(runtime_saved_fs, runtime_tls_block);
    exit(ret);
}
