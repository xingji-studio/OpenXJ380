// XJ380 User-space C Library - stdlib Functions
// This file provides standard library functions

#include "./include/stdint.h"
#include "./include/xposix/stdlib.h"
#include "./include/xposix/string.h"

extern "C" {

// Memory allocation - using kernel heap via syscall
extern void  *malloc(size_t size);
extern void   free(void *ptr);
extern void  *calloc(size_t nmemb, size_t size);
extern void  *realloc(void *ptr, size_t size);

int atoi(const char *nptr)
{
    int ret_integer  = 0;
    int integer_sign = 1;

    if (*nptr == '-') integer_sign = -1;
    if (*nptr == '-' || *nptr == '+') nptr++;

    while (*nptr >= '0' && *nptr <= '9')
    {
        ret_integer = ret_integer * 10 + *nptr - '0';
        nptr++;
    }
    ret_integer = integer_sign * ret_integer;
    return ret_integer;
}

long atol(const char *nptr)
{
    long ret = 0;
    int  sign = 1;

    if (*nptr == '-') sign = -1;
    if (*nptr == '-' || *nptr == '+') nptr++;

    while (*nptr >= '0' && *nptr <= '9')
    {
        ret = ret * 10 + *nptr - '0';
        nptr++;
    }
    return sign * ret;
}

long long atoll(const char *nptr)
{
    long long ret = 0;
    int       sign = 1;

    if (*nptr == '-') sign = -1;
    if (*nptr == '-' || *nptr == '+') nptr++;

    while (*nptr >= '0' && *nptr <= '9')
    {
        ret = ret * 10 + *nptr - '0';
        nptr++;
    }
    return sign * ret;
}

int abs(int value)
{
    return value < 0 ? -value : value;
}

static int isspace(int c)
{
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
}

int64_t strtol(const char *nptr, char **endptr, int base)
{
    if (!nptr)
    {
        if (endptr) { *endptr = NULL; }
        return 0;
    }

    const char *cursor = nptr;
    while (isspace((unsigned char)*cursor))
    {
        cursor++;
    }

    bool negative = false;
    if (*cursor == '-' || *cursor == '+')
    {
        negative = (*cursor == '-');
        cursor++;
    }

    if (base != 0 && (base < 2 || base > 36))
    {
        if (endptr) { *endptr = (char *)nptr; }
        return 0;
    }

    if ((base == 0 || base == 16) && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X'))
    {
        int  first_hex = cursor[2];
        bool has_hex_digit = (first_hex >= '0' && first_hex <= '9') || (first_hex >= 'A' && first_hex <= 'F') ||
                             (first_hex >= 'a' && first_hex <= 'f');
        if (has_hex_digit)
        {
            cursor += 2;
            base = 16;
        }
    }

    if (base == 0) { base = (cursor[0] == '0') ? 8 : 10; }

    const char *number_start = cursor;
    uint64_t    value        = 0;
    uint64_t    limit        = negative ? ((uint64_t)0x7FFFFFFFFFFFFFFFULL + 1ULL) : (uint64_t)0x7FFFFFFFFFFFFFFFULL;
    bool        overflow     = false;

    while (*cursor)
    {
        int ch    = *cursor;
        int digit = -1;

        if (ch >= '0' && ch <= '9') { digit = ch - '0'; }
        else if (ch >= 'A' && ch <= 'Z') { digit = ch - 'A' + 10; }
        else if (ch >= 'a' && ch <= 'z') { digit = ch - 'a' + 10; }
        else { break; }

        if (digit >= base) { break; }
        if (value > (limit - (uint64_t)digit) / (uint64_t)base) { overflow = true; }
        else { value = value * (uint64_t)base + (uint64_t)digit; }
        cursor++;
    }

    if (cursor == number_start)
    {
        if (endptr) { *endptr = (char *)nptr; }
        return 0;
    }

    if (endptr) { *endptr = (char *)cursor; }
    if (overflow) { return negative ? (int64_t)0x8000000000000000ULL : (int64_t)0x7FFFFFFFFFFFFFFFULL; }
    if (!negative) { return (int64_t)value; }
    if (value == (uint64_t)0x7FFFFFFFFFFFFFFFULL + 1ULL) { return (int64_t)0x8000000000000000ULL; }
    return -(int64_t)value;
}

uint64_t strtoul(const char *nptr, char **endptr, int base)
{
    if (!nptr)
    {
        if (endptr) { *endptr = NULL; }
        return 0;
    }

    const char *cursor = nptr;
    while (isspace((unsigned char)*cursor))
    {
        cursor++;
    }

    if (*cursor == '+') cursor++;

    if (base != 0 && (base < 2 || base > 36))
    {
        if (endptr) { *endptr = (char *)nptr; }
        return 0;
    }

    if ((base == 0 || base == 16) && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X'))
    {
        int  first_hex = cursor[2];
        bool has_hex_digit = (first_hex >= '0' && first_hex <= '9') || (first_hex >= 'A' && first_hex <= 'F') ||
                             (first_hex >= 'a' && first_hex <= 'f');
        if (has_hex_digit)
        {
            cursor += 2;
            base = 16;
        }
    }

    if (base == 0) { base = (cursor[0] == '0') ? 8 : 10; }

    const char *number_start = cursor;
    uint64_t    value        = 0;

    while (*cursor)
    {
        int ch    = *cursor;
        int digit = -1;

        if (ch >= '0' && ch <= '9') { digit = ch - '0'; }
        else if (ch >= 'A' && ch <= 'Z') { digit = ch - 'A' + 10; }
        else if (ch >= 'a' && ch <= 'z') { digit = ch - 'a' + 10; }
        else { break; }

        if (digit >= base) { break; }
        value = value * (uint64_t)base + (uint64_t)digit;
        cursor++;
    }

    if (cursor == number_start)
    {
        if (endptr) { *endptr = (char *)nptr; }
        return 0;
    }

    if (endptr) { *endptr = (char *)cursor; }
    return value;
}

long long strtoll(const char *nptr, char **endptr, int base)
{
    return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
    return (unsigned long long)strtoul(nptr, endptr, base);
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *))
{
    if (nmemb <= 1 || size == 0) return;

    char *arr = (char *)base;

    // Simple bubble sort for now (can be optimized later)
    for (size_t i = 0; i < nmemb - 1; i++)
    {
        for (size_t j = 0; j < nmemb - i - 1; j++)
        {
            if (compar(arr + j * size, arr + (j + 1) * size) > 0)
            {
                // Swap
                char temp[256];
                if (size <= sizeof(temp))
                {
                    memcpy(temp, arr + j * size, size);
                    memcpy(arr + j * size, arr + (j + 1) * size, size);
                    memcpy(arr + (j + 1) * size, temp, size);
                }
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *))
{
    if (nmemb == 0 || size == 0) return NULL;

    const char *arr = (const char *)base;
    size_t      low = 0;
    size_t      high = nmemb;

    while (low < high)
    {
        size_t mid = low + (high - low) / 2;
        int    cmp = compar(key, arr + mid * size);

        if (cmp == 0) return (void *)(arr + mid * size);
        else if (cmp < 0) high = mid;
        else low = mid + 1;
    }

    return NULL;
}

// Environment variables - minimal implementation
static char *empty_env[] = { NULL };
char       **environ = empty_env;

char *getenv(const char *name)
{
    (void)name;
    return NULL; // Not implemented yet
}

int setenv(const char *name, const char *value, int overwrite)
{
    (void)name;
    (void)value;
    (void)overwrite;
    return -1; // Not implemented yet
}

int unsetenv(const char *name)
{
    (void)name;
    return -1; // Not implemented yet
}

int grantpt(int fd)
{
    (void)fd;
    return 0;
}

int unlockpt(int fd)
{
    (void)fd;
    return 0;
}

char *ptsname(int fd)
{
    (void)fd;
    return NULL;
}

} // extern "C"
