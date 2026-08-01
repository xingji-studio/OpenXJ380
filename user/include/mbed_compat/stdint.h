#pragma once

#include "../../xapi/include/stdint.h"

typedef int64_t  intptr_t;
typedef uint64_t uintptr_t;

#ifndef SIZE_MAX
#define SIZE_MAX __SIZE_MAX__
#endif

#ifndef INT8_MAX
#define INT8_MAX __INT8_MAX__
#endif

#ifndef INT16_MAX
#define INT16_MAX __INT16_MAX__
#endif

#ifndef INT32_MAX
#define INT32_MAX __INT32_MAX__
#endif

#ifndef INT64_MAX
#define INT64_MAX __INT64_MAX__
#endif

#ifndef INT8_MIN
#define INT8_MIN (-INT8_MAX - 1)
#endif

#ifndef INT16_MIN
#define INT16_MIN (-INT16_MAX - 1)
#endif

#ifndef INT32_MIN
#define INT32_MIN (-INT32_MAX - 1)
#endif

#ifndef INT64_MIN
#define INT64_MIN (-INT64_MAX - 1)
#endif

#ifndef UINT8_MAX
#define UINT8_MAX __UINT8_MAX__
#endif

#ifndef UINT16_MAX
#define UINT16_MAX __UINT16_MAX__
#endif

#ifndef UINT32_MAX
#define UINT32_MAX __UINT32_MAX__
#endif

#ifndef UINT64_MAX
#define UINT64_MAX __UINT64_MAX__
#endif

#ifndef INTPTR_MAX
#define INTPTR_MAX __INTPTR_MAX__
#endif

#ifndef INTPTR_MIN
#define INTPTR_MIN (-INTPTR_MAX - 1)
#endif

#ifndef UINTPTR_MAX
#define UINTPTR_MAX __UINTPTR_MAX__
#endif
