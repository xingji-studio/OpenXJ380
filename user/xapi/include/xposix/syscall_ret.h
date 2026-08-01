#pragma once

#include "errno.h"
#include "../stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

long __xposix_ret(uint64_t ret);
void *__xposix_ptr(uint64_t ret);

#ifdef __cplusplus
}
#endif
