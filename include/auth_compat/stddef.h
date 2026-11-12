// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
