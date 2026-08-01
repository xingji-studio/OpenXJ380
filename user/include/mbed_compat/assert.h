#pragma once

#include "../../xapi/include/krlibc.h"

#ifdef __cplusplus
extern "C" {
#endif

void abort(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#ifndef assert
#define assert(expr)                        \
    do {                                    \
        if (!(expr)) {                      \
            abort();                        \
        }                                   \
    } while (0)
#endif
