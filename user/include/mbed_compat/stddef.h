#pragma once

#include "../../xapi/include/stdint.h"

#ifndef NULL
#define NULL 0
#endif

typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
