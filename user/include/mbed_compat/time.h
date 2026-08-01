#pragma once

#include "../../xapi/include/stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t time_t;

struct tm
{
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t time(time_t *timer);

#ifdef __cplusplus
}
#endif
