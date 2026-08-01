#pragma once

#include "../../xapi/include/krlibc.h"

#ifdef __cplusplus
extern "C" {
#endif

void *memchr(const void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);

#ifdef __cplusplus
}
#endif
