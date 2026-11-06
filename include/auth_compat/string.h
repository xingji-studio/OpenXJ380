// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memcpy(void *destination, const void *source, size_t bytes);
void *memset(void *destination, int value, size_t bytes);
void *memmove(void *destination, const void *source, size_t bytes);
int memcmp(const void *a, const void *b, size_t bytes);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t bytes);
size_t strlen(const char *text);
#ifdef __cplusplus
}
#endif
