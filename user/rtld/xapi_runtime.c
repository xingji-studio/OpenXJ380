// SPDX-License-Identifier: Apache-2.0
#include "platform.h"
#include "xapi_runtime.h"

#ifdef XAPI_MINIMAL_RUNTIME
char **environ;
#else
extern char **environ;
#endif

static xapi_rtld_fini runtime_finalizer;
static int initialized, exiting;
extern void exit(int status);

void __xapi_rtld_init_v1(int argc, char **argv, char **envp, xapi_rtld_fini finalizer)
{
    (void)argc;
    (void)argv;
    if (!initialized)
    {
        environ = envp;
        runtime_finalizer = finalizer;
        initialized = 1;
    }
    else if (runtime_finalizer != finalizer)
    {
        rtld_write(2, "libxapi: inconsistent interpreter finalizer\n");
        rtld_exit(127);
    }
}

void __xapi_shared_finalize(void)
{
    // Mark the callback consumed before calling user destructors: exit() from
    // a destructor must not recurse into the loader or finalize an object twice.
    if (exiting) return;
    exiting = 1;
    xapi_rtld_fini finalizer = runtime_finalizer;
    runtime_finalizer = NULL;
    if (finalizer) finalizer();
}

void __xapi_start_main(int argc, char **argv, char **envp, xapi_rtld_fini finalizer,
                       int (*main_function)(int, char **, char **))
{
    if (!initialized)
    {
        rtld_write(2, "libxapi: interpreter bootstrap v1 was not called\n");
        rtld_exit(127);
    }
    __xapi_rtld_init_v1(argc, argv, envp, finalizer);
    // Initialization is idempotent: setenv() in a constructor may already have
    // replaced environ, which must not be reset to the original stack table.
    exit(main_function(argc, argv, envp));
    rtld_exit(127);
}

#ifdef XAPI_MINIMAL_RUNTIME
// The public tree deliberately supplies only its existing syscall API. The
// product links its real libc/POSIX objects and their ordinary exit() instead.
void exit(int status)
{
    __xapi_shared_finalize();
    rtld_exit(status);
}
#endif
