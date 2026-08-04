#include "proto.hpp"
#include "stdint.h"
#include <krlibc.h>
#include <mm/bitmap.h>
#include <mm/frame.h>
#include <mm/heap.h>
#include <mm/hhdm.h>
#include <mm/memory.h>
#include <dlinker.h>
#include <mm/page.h>

int memcmp(const void *a_, const void *b_, size_t size)
{
    const char *a = (const char *)(a_);
    const char *b = (const char *)(b_);
    while (size-- > 0)
    {
        if (*a != *b) return *a > *b ? 1 : -1;
        a++, b++;
    }
    return 0;
}

void *memchr(const void *buffer, int value, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)buffer;
    unsigned char needle = (unsigned char)value;
    for (size_t i = 0; i < size; ++i)
        if (bytes[i] == needle) return (void *)(bytes + i);
    return NULL;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    char       *d = (char *)(dest);
    const char *s = (const char *)(src);

    void *ret = dest;

    if (n < 8)
    {
        while (n--)
        {
            *d++ = *s++;
        }
        return ret;
    }

    size_t align = (size_t)d & (sizeof(size_t) - 1);
    if (align)
    {
        align  = sizeof(size_t) - align;
        n     -= align;
        while (align--)
        {
            *d++ = *s++;
        }
    }

    size_t       *dw = (size_t *)d;
    const size_t *sw = (const size_t *)s;
    for (size_t i = 0; i < n / sizeof(size_t); i++)
    {
        *dw++ = *sw++;
    }

    d             = (char *)dw;
    s             = (const char *)sw;
    size_t remain = n & (sizeof(size_t) - 1);
    while (remain--)
    {
        *d++ = *s++;
    }

    return ret;
}

void *memset(void *dst, int val, size_t size)
{
    unsigned char *d = (unsigned char *)(dst);
    unsigned char  v = (unsigned char)(val);

    while (size && ((size_t)d & 7))
    {
        *d++ = v;
        size--;
    }

    size_t v8 = v * 0x0101010101010101ULL;
    while (size >= 8)
    {
        *(size_t *)d  = v8;
        d            += 8;
        size         -= 8;
    }

    while (size--)
        *d++ = v;

    return dst;
}

EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(memchr);

char *strcpy(char *dest, const char *src)
{
    char *ret = dest;
    do
    {
        *dest++ = *src++;
    } while (*src != 0);
    *dest = 0;
    return ret;
}

EXPORT_SYMBOL(strcpy);
EXPORT_SYMBOL(memcpy);


size_t strlen(const char *str)
{
    const char *s = str;
    while (*s)
    {
        s++;
    }
    return s - str;
}

size_t strnlen(const char *str, size_t maxlen)
{
    size_t length = 0;
    if (str == NULL) return 0;
    while (length < maxlen && str[length] != '\0') ++length;
    return length;
}
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strnlen);

/**
 * 把字符串数字转成整数
 * @param pstr 字符串
 * @return 整数
 */
int atoi(const char *pstr)
{
    int ret_integer  = 0;
    int integer_sign = 1;

    if (*pstr == '-') integer_sign = -1;
    if (*pstr == '-' || *pstr == '+') pstr++;

    while (*pstr >= '0' && *pstr <= '9')
    {
        ret_integer = ret_integer * 10 + *pstr - '0';
        pstr++;
    }
    ret_integer = integer_sign * ret_integer;
    return ret_integer;
}
EXPORT_SYMBOL(atoi);

char *strcat(char *dest, const char *src)
{
    char *ret = dest;
    while (*dest)
        dest++;
    while ((*dest++ = *src++))
        ;
    return ret;
}
EXPORT_SYMBOL(strcat);

char *strchrnul(const char *s, int c)
{
    while (*s)
    {
        if ((*s++) == c) break;
    }
    return (char *)s;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    const unsigned char *p1 = (const unsigned char *)s1, *p2 = (const unsigned char *)s2;
    while (n-- > 0)
    {
        if (*p1 != *p2) return *p1 - *p2;
        if (*p1 == '\0') return 0;
        p1++, p2++;
    }
    return 0;
}
EXPORT_SYMBOL(strncmp);

char *strchr(const char *s, int c)
{
    while (*s)
    {
        if (*s == (char)c) { return (char *)s; }
        s++;
    }
    return (*s == (char)c) ? (char *)s : NULL;
}
EXPORT_SYMBOL(strchr);

char *strstr(const char *str1, const char *str2)
{
    const char *s1, *s2, *p = str1;
    while (*p != '\0')
    {
        s1 = p;
        s2 = str2;
        while (*s1 != '\0' && *s2 != '\0' && *s1 == *s2)
        {
            s1++;
            s2++;
        }
        if (*s2 == '\0') { return (char *)p; }
        p++;
    }
    return NULL;
}
EXPORT_SYMBOL(strstr);

int strcmp(const char *s1, const char *s2)
{
    char is_equal = 1;

    for (; (*s1 != '\0') && (*s2 != '\0'); s1++, s2++)
    {
        if (*s1 != *s2)
        {
            is_equal = 0;
            break;
        }
    }

    if (is_equal)
    {
        if (*s1 != '\0') { return 1; }
        else if (*s2 != '\0') { return -1; }
        else { return 0; }
    }
    else { return (int)(*s1 - *s2); }
}
EXPORT_SYMBOL(strcmp);

int isspace(int c)
{
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
}
EXPORT_SYMBOL(isspace);

char *strtok(char *str, const char *delim)
{
    static char *last = NULL;
    if (str) { last = str; }
    else if (!last) { return NULL; }

    char *start = last;
    while (*start && strchr(delim, *start))
    {
        start++;
    }

    if (*start == '\0')
    {
        last = NULL;
        return NULL;
    }

    char *end = start;
    while (*end && !strchr(delim, *end))
    {
        end++;
    }

    if (*end)
    {
        *end = '\0';
        last = end + 1;
    }
    else { last = NULL; }

    return start;
}

int64_t strtol(const char *str, char **endptr, int base)
{
    if (!str)
    {
        if (endptr) { *endptr = NULL; }
        return 0;
    }

    const char *cursor = str;
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
        if (endptr) { *endptr = (char *)str; }
        return 0;
    }

    if ((base == 0 || base == 16) && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X'))
    {
        int first_hex = cursor[2];
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
    uint64_t    limit        = negative ? ((uint64_t)LONG_MAX + 1ULL) : (uint64_t)LONG_MAX;
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
        if (endptr) { *endptr = (char *)str; }
        return 0;
    }

    if (endptr) { *endptr = (char *)cursor; }
    if (overflow) { return negative ? LONG_MIN : LONG_MAX; }
    if (!negative) { return (int64_t)value; }
    if (value == (uint64_t)LONG_MAX + 1ULL) { return LONG_MIN; }
    return -(int64_t)value;
}
EXPORT_SYMBOL(strtol);

int cmd_parse_limit(const char *cmd_str, char **argv, char token, int max_argc)
{
    int         argc = 0;
    const char *next = cmd_str;
    if (max_argc <= 0)
    {
        if (argv != NULL) argv[0] = NULL;
        return 0;
    }

    while (*next)
    {
        while (*next == token)
            next++;
        if (*next == '\0') break;

        char *arg = (char *)malloc(strlen(next) + 1);
        if (!arg)
        {
            for (int i = 0; i < argc; i++)
                free(argv[i]);
            return -1;
        }

        char  quote = 0;
        size_t len  = 0;
        while (*next)
        {
            char c = *next;
            if (quote)
            {
                if (c == quote)
                {
                    quote = 0;
                    next++;
                    continue;
                }
            }
            else
            {
                if (c == token) break;
                if (c == '\'' || c == '"')
                {
                    quote = c;
                    next++;
                    continue;
                }
            }

            if (c == '\\' && next[1] != '\0')
            {
                next++;
                c = *next;
            }
            arg[len++] = c;
            next++;
        }

        argv[argc] = arg;
        if (!argv[argc])
        {
            for (int i = 0; i < argc; i++)
                free(argv[i]);
            return -1;
        }
        argv[argc][len] = '\0';

        argc++;
        if (argc >= max_argc) break;
    }
    argv[argc] = NULL;
    return argc;
}
EXPORT_SYMBOL(cmd_parse_limit);

int cmd_parse(const char *cmd_str, char **argv, char token)
{
    return cmd_parse_limit(cmd_str, argv, token, 50);
}

void cmd_free(char **argv, int argc)
{
    for (int i = 0; i < argc; i++)
    {
        free(argv[i]);
    }
}


char *strdup(const char *str)
{
    if (str == NULL) return NULL;

    char *strat = (char *)str;
    int   len   = 0;
    while (*str++ != '\0')
        len++;
    char *ret = (char *)malloc(len + 1);

    while ((*ret++ = *strat++) != '\0') {}

    return ret - (len + 1);
}
EXPORT_SYMBOL(strdup);

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
    {
        dest[i] = src[i];
    }
    for (; i < n; i++)
    {
        dest[i] = '\0';
    }
    return dest;
}
EXPORT_SYMBOL(strncpy);

char *strrchr(const char *s, int c)
{
    char *last = NULL;
    while (*s)
    {
        if (*s == (char)c) { last = (char *)s; }
        s++;
    }
    return (c == '\0') ? (char *)s : last;
}
EXPORT_SYMBOL(strrchr);

int isdigit(int c)
{
    return (c >= '0' && c <= '9');
}
EXPORT_SYMBOL(isdigit);

int fls(unsigned int x)
{
    if (x == 0) return 0;
    return 32 - __builtin_clz(x);
}

char *pathacat(char *p1, char *p2)
{
    char *p = (char *)malloc(strlen(p1) + strlen(p2) + 2);
    if (p1[strlen(p1) - 1] == '/') { sprintf(p, "%s%s", p1, p2); }
    else { sprintf(p, "%s/%s", p1, p2); }
    return p;
}
/**
 * 跳过字符串中的数字，并返回连续的数字
 * @param s 指向字符串的指针（用于跳过）
 * @return 整数
 */
int skip_atoi(const char **s)
{
    int i = 0;
    while (IS_DIGIT(**s))
        i = i * 10 + *((*s)++) - '0';
    return i;
}

/**
 * 把整数转成字符串数字，并输出到`Writer`
 * @param str 字符串
 * @param fmter 整数格式化器
 * @param type 
 */
size_t wnumber(Writer *writer, num_formatter_t fmter, num_fmt_type type) // NOLINT
{
    if (!writer || !writer->handler || fmter.base < 2 || fmter.base > 36) { return 0; }

    char         tmp[65];
    const char  *digits  = type.small ? "0123456789abcdefghijklmnopqrstuvwxyz"
                                      : "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    WriteHandler write   = writer->handler;
    size_t       result  = 0;
    uint64_t     value   = (uint64_t)fmter.num;
    char         sign    = 0;
    char         prefix0 = 0;
    char         prefix1 = 0;

    if (type.sign && (int64_t)fmter.num < 0)
    {
        int64_t signed_value = (int64_t)fmter.num;
        sign                 = '-';
        value                = (uint64_t)(-(signed_value + 1)) + 1;
    }
    else if (type.plus) { sign = '+'; }
    else if (type.space) { sign = ' '; }

    if (type.special)
    {
        if (fmter.base == 8) { prefix0 = '0'; }
        else if (fmter.base == 16)
        {
            prefix0 = '0';
            prefix1 = type.small ? 'x' : 'X';
        }
    }

    size_t digit_count = 0;
    do
    {
        tmp[digit_count++] = digits[value % (uint64_t)fmter.base];
        value /= (uint64_t)fmter.base;
    } while (value != 0);

    size_t prefix_width      = (prefix0 != 0 ? 1 : 0) + (prefix1 != 0 ? 1 : 0);
    size_t precision_width   = fmter.precision > digit_count ? fmter.precision : digit_count;
    size_t precision_padding = precision_width - digit_count;
    size_t content_width     = precision_width + prefix_width + (sign != 0 ? 1 : 0);
    size_t field_padding     = fmter.size > content_width ? fmter.size - content_width : 0;
    bool   zero_field        = type.zeropad && !type.left;

    while (!type.left && !zero_field && field_padding > 0)
    {
        field_padding--;
        write(writer, ' '), result++;
    }

    if (sign != 0) write(writer, sign), result++;
    if (prefix0 != 0) write(writer, prefix0), result++;
    if (prefix1 != 0) write(writer, prefix1), result++;

    while (zero_field && field_padding > 0)
    {
        field_padding--;
        write(writer, '0'), result++;
    }
    while (precision_padding > 0)
    {
        precision_padding--;
        write(writer, '0'), result++;
    }
    while (digit_count-- > 0)
        write(writer, tmp[digit_count]), result++;
    while (type.left && field_padding > 0)
    {
        field_padding--;
        write(writer, ' '), result++;
    }

    return result;
}
