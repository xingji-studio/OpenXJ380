// SPDX-License-Identifier: Apache-2.0
#pragma once

// The interpreter calls this versioned entry before any preinit/constructor.
// It belongs to libxapi.so.1, so environment and exit state have one owner.
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*xapi_rtld_fini)(void);
void __xapi_rtld_init_v1(int argc, char **argv, char **envp, xapi_rtld_fini finalizer);
void __xapi_shared_finalize(void) __attribute__((visibility("hidden")));
void __xapi_start_main(int argc, char **argv, char **envp, xapi_rtld_fini finalizer,
                       int (*main_function)(int, char **, char **)) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
