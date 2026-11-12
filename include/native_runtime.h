// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize to zero. The caller serializes create/join on a given handle;
// A completed join (also -ECANCELED) releases TLS, stack and control record.
typedef struct { uint64_t handle; } XjThread;
typedef int64_t (*XjThreadEntry)(void *argument);
int64_t xj_thread_create(XjThread *thread, XjThreadEntry entry, void *argument);
int64_t xj_thread_join(XjThread *thread, int64_t *result);

#ifdef __cplusplus
}
#endif
