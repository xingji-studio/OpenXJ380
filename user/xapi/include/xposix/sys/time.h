#pragma once

#include "../time.h"

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

#ifdef __cplusplus
extern "C" {
#endif

int gettimeofday(struct timeval *tv, void *tz);

#ifdef __cplusplus
}
#endif
