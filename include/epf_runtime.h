// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <native_runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize to zero. The caller serializes create/join on a given handle;
// A completed join (also -ECANCELED) releases TLS, stack and control record.
typedef XjThread EpfThread;
typedef XjThreadEntry EpfThreadEntry;
int64_t epf_thread_create(EpfThread *thread, EpfThreadEntry entry, void *argument);
int64_t epf_thread_join(EpfThread *thread, int64_t *result);

#ifdef __cplusplus
}
#endif
