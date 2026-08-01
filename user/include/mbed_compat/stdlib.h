#pragma once

#include "../../xapi/include/liballoc/alloc.h"
#include "../../xapi/include/krlibc.h"

#ifdef __cplusplus
extern "C" {
#endif

void abort(void) __attribute__((noreturn));
int rand(void);

#ifdef __cplusplus
}
#endif
