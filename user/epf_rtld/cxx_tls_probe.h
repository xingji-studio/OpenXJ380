// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

struct CxxTlsResult
{
    uint64_t order;
    uint32_t seed, completed, constructors[6], destructors[6];
};

struct CxxTlsState
{
    CxxTlsResult *sink;
    uint64_t order;
    uint32_t seed, completed, constructors[6], destructors[6];
};

extern "C" CxxTlsState *epf_cxx_state(void);
extern "C" int epf_cxx_construct(unsigned id);
extern "C" void epf_cxx_destroy(unsigned id, int value);
extern "C" void epf_cxx_touch_library(int **first, int **last);
extern "C" void *epf_cxx_dso_identity(void);
