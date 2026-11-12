// SPDX-License-Identifier: Apache-2.0
// The format-independent runtime is compiled into each interpreter/DSO.
#include "../rtld/tls.c"
#include <epf_runtime.h>

PUBLIC int64_t epf_thread_create(EpfThread *thread, EpfThreadEntry entry, void *argument)
{
    return xj_thread_create(thread, entry, argument);
}

PUBLIC int64_t epf_thread_join(EpfThread *thread, int64_t *result)
{
    return xj_thread_join(thread, result);
}
