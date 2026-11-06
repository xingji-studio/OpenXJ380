// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *calloc(size_t count, size_t bytes);
void free(void *allocation);
#ifdef __cplusplus
}
#endif
