// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression) ((expression) ? (void)0 : __builtin_trap())
#endif
