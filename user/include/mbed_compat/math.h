#pragma once

#ifdef __cplusplus
extern "C" {
#endif

double ceil(double x);
double floor(double x);
double fabs(double x);
double sqrt(double x);
double round(double x);

#ifndef signbit
#define signbit(x) __builtin_signbit(x)
#endif

#ifndef INFINITY
#define INFINITY (__builtin_inf())
#endif

#ifndef NAN
#define NAN (__builtin_nanf(""))
#endif

#ifndef HUGE_VAL
#define HUGE_VAL (__builtin_huge_val())
#endif

#ifdef __cplusplus
}
#endif
