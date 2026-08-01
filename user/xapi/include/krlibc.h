#pragma once

#include "./stdint.h"
#include "./xposix/stdarg.h"

#if !defined(__cplusplus) && !defined(__bool_true_false_are_defined)
typedef unsigned char bool;
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif
#endif

#ifdef __cplusplus // Fuck you C++
extern "C" {
#endif
#define waitif(cond)                                                                                                   \
    ((void)({                                                                                                          \
        while (cond) {}                                                                                                \
    }))
static inline void empty() {};
#define ABS(x)    ((x) > 0 ? (x) : -(x))
#define MAX(x, y) ((x > y) ? (x) : (y))
#define MIN(x, y) ((x < y) ? (x) : (y))
#define streq(s1, s2)                                                                                                  \
    ({                                                                                                                 \
        const char *_s1 = (s1), *_s2 = (s2);                                                                           \
        (_s1 && _s2) ? strcmp(_s1, _s2) == 0 : _s1 == _s2;                                                             \
    })
#undef CHAR_BIT
#define CHAR_BIT __CHAR_BIT__

#ifndef MB_LEN_MAX
#    define MB_LEN_MAX 1
#endif

#undef SCHAR_MAX
#define SCHAR_MAX __SCHAR_MAX__
#undef SCHAR_MIN
#define SCHAR_MIN (-SCHAR_MAX - 1)

#undef UCHAR_MAX
#if __SCHAR_MAX__ == __INT_MAX__
#    define UCHAR_MAX (SCHAR_MAX * 2U + 1U)
#else
#    define UCHAR_MAX (SCHAR_MAX * 2 + 1)
#endif

#ifdef __CHAR_UNSIGNED__
#    undef CHAR_MAX
#    define CHAR_MAX UCHAR_MAX
#    undef CHAR_MIN
#    if __SCHAR_MAX__ == __INT_MAX__
#        define CHAR_MIN 0U
#    else
#        define CHAR_MIN 0
#    endif
#else
#    undef CHAR_MAX
#    define CHAR_MAX SCHAR_MAX
#    undef CHAR_MIN
#    define CHAR_MIN SCHAR_MIN
#endif

#undef SHRT_MAX
#define SHRT_MAX __SHRT_MAX__
#undef SHRT_MIN
#define SHRT_MIN (-SHRT_MAX - 1)

#undef USHRT_MAX
#if __SHRT_MAX__ == __INT_MAX__
#    define USHRT_MAX (SHRT_MAX * 2U + 1U)
#else
#    define USHRT_MAX (SHRT_MAX * 2 + 1)
#endif

#undef INT_MAX
#define INT_MAX __INT_MAX__
#undef INT_MIN
#define INT_MIN (-INT_MAX - 1)

#undef UINT_MAX
#define UINT_MAX (INT_MAX * 2U + 1U)

#undef LONG_MAX
#define LONG_MAX __LONG_MAX__
#undef LONG_MIN
#define LONG_MIN (-LONG_MAX - 1L)
#undef ULONG_MAX
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)
int     memcmp(const void *a_, const void *b_, size_t size);
void   *memcpy(void *dest, const void *src, size_t n);
void   *memset(void *dst, int val, size_t size);
char   *strcpy(char *dest, const char *src);
size_t  strlen(const char *str);
char   *strcat(char *dest, const char *src);
char   *strchrnul(const char *s, int c);
int     strncmp(const char *s1, const char *s2, size_t n);
char   *strchr(const char *s, int c);
int     strcmp(const char *s1, const char *s2);
char   *strcpy(char *dest, const char *src);
int     isspace(int c);
char   *strtok(char *str, const char *delim);
int64_t strtol(const char *str, char **endptr, int base);
char   *strdup(const char *str);
char   *pathacat(char *p1, char *p2);
int     fls(unsigned int x);
int     isdigit(int c);
char   *strrchr(const char *s, int c);
char   *strncpy(char *dest, const char *src, size_t n);
char   *strstr(const char *str1, const char *str2);

typedef struct num_fmt_type
{
    bool zeropad : 1; // 补0
    bool sign    : 1; // 是否带符号 (用于区分signed和unsigned)
    bool plus    : 1; // 是否显示正号
    bool space   : 1; // 以空格代替正号
    bool left    : 1; // 左对齐
    bool special : 1; // 特殊前缀（比如：0x）
    bool small   : 1; // 使用小写字母代替大写字母（0X -> 0x, 1F -> 1f）
} num_fmt_type;

typedef struct num_formatter
{
    size_t num;       // 整数
    size_t base;      // 进制
    size_t size;      // 输出的字符串的最小大小
    size_t precision; // 精度（整数情况下约等于补0）
} num_formatter_t;

/* Placeholder ... */
typedef struct Writer Writer;



/**
 * A handle of writing a char
 * `uint8_t` is a bool, if != 0 means write success, if == 0 means write failure
 */
typedef uint8_t (*WriteHandler)(Writer *writer, char ch);

/**
 * A interface of writing a char (May extended in the future)
 */
typedef struct Writer
{
    void        *data; // Any data
    WriteHandler handler;
} Writer;

/**
 * Write a number to a writer
 * @param writer Writer
 * @param fmter Number formatter
 * @param type Number format type
 */
size_t wnumber(Writer *writer, num_formatter_t fmter, num_fmt_type type);

/**
 * 把字符串数字转成整数
 * @param pstr 字符串
 * @return 整数
 */
int atoi(const char *pstr);

/**
 * 跳过字符串中的数字，并返回连续的数字
 * @param s 指向字符串的指针（用于跳过）
 * @return 整数
 */
int skip_atoi(const char **s);

static inline bool are_interrupts_enabled()
{
    uint64_t rflags = 0;
    __asm__ volatile("pushfq\n\t"
                     "pop %0"
                     : "=r"(rflags));
    return (rflags & (1 << 9)) != 0;
}

#ifdef __cplusplus
} // extern "C"
#endif
typedef enum num_size
{
    HALF_2 = 0, // char
    HALF_1 = 1, // short
    INT    = 2, // int
    LONG_1 = 3, // long
    LONG_2 = 4, // long long
    SIZE_T = 5, // size_t
} num_size_t;
#define PADDING_DOWN(size, to) ((size_t)(size) / (size_t)(to) * (size_t)(to))
#define PADDING_UP(size, to)   PADDING_DOWN((size_t)(size) + (size_t)(to) - (size_t)1, to)
#define IS_DIGIT(c)            ((c) >= '0' && (c) <= '9')
#define IS_ALPHA(a)            (((a) >= 'A' && (a) <= 'Z') || ((a) >= 'a' && (a) <= 'z'))

// 格式化输出函数
#ifdef __cplusplus
extern "C" {
#endif
int snprintf(char *str, size_t size, const char *format, ...);
int sprintf(char *str, const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int simple_snprintf(char *str, size_t size, const char *format, ...);
size_t vwprintf(Writer *writer, const char *fmt, va_list args);
size_t wfmt_arg(Writer *writer, const char **fmt_ptr, va_list args);
size_t wnumber(Writer *writer, num_formatter_t fmter, num_fmt_type type);

void uint64_to_string(uint64_t num, char *buffer, int buffer_size);
void int_to_string(int num, char *buffer, int buffer_size);
#ifdef __cplusplus
}
#endif

// static inline void *memmove(void *dest, const void *src, size_t n);
