#pragma once

#include "stdint.h"

// 哦，我们亲爱的派
#define PI                                                                                                             \
    (float64_t)3.1415926535 // 搞笑版:3.14159265358979323846264338327950288419716939937510582097494459230781640628620899
#define TAU                                                                                                            \
    (float64_t)6.2831853071 // 搞笑版:6.28318530717958647692528676655900576839433879875021164194988918553642212019584998
#define PI_DIV_2                                                                                                       \
    (float64_t)1.5707963267 // 搞笑版:1.570796326794896619231321691639751442098584699687552910487840454326648213393607260249141273724587006606315588174881520920962829254091715364367892590360011330530548820466521384146951941511609433057270365759591953092186117381932611793105118548074462379962749567351885752724891227938183011949128831426076896280457

#define FORCE_EVAL(x)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (sizeof(x) == sizeof(float))                                                                                \
        {                                                                                                              \
            volatile float __x __attribute__((unused));                                                                \
            __x = (x);                                                                                                 \
        }                                                                                                              \
        else if (sizeof(x) == sizeof(double))                                                                          \
        {                                                                                                              \
            volatile double __x __attribute__((unused));                                                               \
            __x = (x);                                                                                                 \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            volatile long double __x __attribute__((unused));                                                          \
            __x = (x);                                                                                                 \
        }                                                                                                              \
    } while (0)

int round(float64_t x)
{
    if (x >= 0.0)
        return (int)(x + 0.5);
    else
        return (int)(x - 0.5);
}

int32_t equal(float64_t elem1, float64_t elem2)
{
    if ((elem1 - elem2 < 0.0000001) && (elem1 - elem2 > -0.0000001))
        return 1; // 相等
    else
        return 0; // 不相等
}

static const double rounders[10 + 1] = {
    0.5,          // 0
    0.05,         // 1
    0.005,        // 2
    0.0005,       // 3
    0.00005,      // 4
    0.000005,     // 5
    0.0000005,    // 6
    0.00000005,   // 7
    0.000000005,  // 8
    0.0000000005, // 9
    0.00000000005 // 10
};

char *ftoa(double f, char *buf, int precision)
{
    char *ptr = buf;
    char *p   = ptr;
    char *p1;
    char  c;
    long  intPart;

    if (precision > 10) precision = 10;
    if (f < 0)
    {
        f      = -f;
        *ptr++ = '-';
    }
    if (precision < 0)
    {
        if (f < 1.0)
            precision = 6;
        else if (f < 10.0)
            precision = 5;
        else if (f < 100.0)
            precision = 4;
        else if (f < 1000.0)
            precision = 3;
        else if (f < 10000.0)
            precision = 2;
        else if (f < 100000.0)
            precision = 1;
        else
            precision = 0;
    }
    if (precision) f += rounders[precision];
    intPart  = f;
    f       -= intPart;
    if (!intPart)
        *ptr++ = '0';
    else
    {
        p = ptr;
        while (intPart)
        {
            *p++     = '0' + intPart % 10;
            intPart /= 10;
        }
        p1 = p;
        while (p > ptr)
        {
            c      = *--p;
            *p     = *ptr;
            *ptr++ = c;
        }
        ptr = p1;
    }
    if (precision)
    {
        *ptr++ = '.';
        while (precision--)
        {
            f      *= 10.0;
            c       = f;
            *ptr++  = '0' + c;
            f      -= c;
        }
    }
    *ptr = 0;
    return ptr;
}

float ceilf(float x)
{
    float fract = x - (int)x; // 获取小数部分
    if (fract > 0)
    {
        return (int)x + 1; // 如果有小数部分，向上取整
    }
    else
    {
        return (int)x; // 如果没有小数部分，直接返回整数部分
    }
}

float floorf(float x)
{
    float fract = x - (int)x; // 获取小数部分
    if (fract < 0)
    {
        return (int)x - 1; // 如果是负数且有小数部分，向下取整
    }
    else
    {
        return (int)x; // 如果是正数或没有小数部分，直接返回整数部分
    }
}

float roundf(float number)
{
    if (number < 0.0f) { return ceilf(number - 0.5f); }
    else { return floorf(number + 0.5f); }
}

double fabs(double x)
{
    if (x < 0) { return -x; }
    else { return x; }
}

double floor(double x)
{
    double fract = x - (int)x;
    if (fract < 0) { return (int)x - 1; }
    else { return (int)x; }
}

double ceil(double x)
{
    double fract = x - (int)x; // 获取小数部分
    if (fract > 0)
    {
        return (int)x + 1; // 如果有小数部分，向上取整
    }
    else
    {
        return (int)x; // 如果没有小数部分，直接返回整数部分
    }
}

double fmod(double x, double y)
{
    if (y == 0) { return __builtin_nanf(""); }
    double intPart   = x / y;
    double remainder = x - intPart * y;
    if (remainder < 0) { remainder += y; }
    else if (remainder > y) { remainder -= y; }
    return remainder;
}

double cos(double x)
{
    double sum  = 0.0;
    double term = 1.0;
    int    n    = 0;

    for (n = 0; term > 1e-15; n++)
    {
        term  = term * (-1) * (2 * n) * (2 * n - 1) / ((2 * n) * (2 * n - 1));
        sum  += term;
    }

    return sum;
}

double sqrt(double number)
{
    if (number < 0) { return __builtin_nanf(""); }

    double x       = number;
    double epsilon = 1e-15;
    double diff;

    do
    {
        x    = (x + number / x) / 2;
        diff = fabs(x - number / x);
    } while (diff > epsilon);

    return x;
}

double acos(double x)
{
    double x0 = x;
    double x1;
    double tolerance      = 1e-15;
    double max_iterations = 1000;
    int    iterations     = 0;

    while (iterations < max_iterations)
    {
        x1 = x0 - (-1 / sqrt(1 - x0 * x0)) / (1 / cos(x0));
        if (fabs(x1 - x0) < tolerance) { break; }
        x0 = x1;
        iterations++;
    }

    return x1;
}

double pow(double x, int y)
{
    double result = 1.0;
    for (int i = 0; i < y; i++)
    {
        result *= x;
    }
    return result;
}

double ldexp(double x, int exp)
{
    int n = 2;
    for (int i = 0; i < exp - 1; i++)
    {
        n *= 2;
    }
    return x * (double)(n);
}

int abs(int x)
{
    return (x < 0 ? -x : x);
}

double sin(double x)
{
    return sqrt(1.0 - cos(x) * cos(x));
}

double asin(double x)
{
    return 1.0 / sin(x);
}

double atan(double x)
{
    return 1.0 / sin(x) * cos(x);
}
