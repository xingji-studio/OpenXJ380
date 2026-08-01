#pragma once

#include "sys/types.h"

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define TIMER_ABSTIME            1

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

#ifdef __cplusplus
extern "C" {
#endif

int clock_gettime(int clockid, struct timespec *ts);
int clock_getres(int clockid, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
int clock_nanosleep(int clockid, int flags, const struct timespec *req, struct timespec *rem);
time_t time(time_t *timer);

struct tm {
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

struct tm *gmtime_r(const time_t *timer, struct tm *result);
struct tm *localtime_r(const time_t *timer, struct tm *result);
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);

#ifdef __cplusplus
}
#endif
