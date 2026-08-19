#pragma once

#include <stdint.h>
#define MAX_WAIT_INDEX 1000000 // 阻塞最大循环数

#ifdef __cplusplus // Fuck you C++
extern "C" {
#endif

#define container_of(ptr, type, member)                                                            \
    ({                                                                                             \
        uint64_t __mptr = ((uint64_t)(ptr));                                                       \
        (type *)((char *)__mptr - offsetof(type, member));                                         \
    })

// 分支预测优化: x 很可能为假
#define unlikely(x) __builtin_expect(!!(x), 0)

// 分支预测优化: x 很可能为真
#define likely(x) __builtin_expect(!!(x), 1)

typedef typeof(nullptr) nullptr_t;
#define offsetof(s, m) __builtin_offsetof(s, m)

#define cpu_hlt while(1) __asm__("hlt")

typedef int errno_t;

#define KERNEL_AREA_MEM        0xf000000000000000       // 内核地址空间起始
static inline bool check_user_overflow(uint64_t addr, uint64_t size) {
    if (size == 0) return false;
    if (addr >= KERNEL_AREA_MEM) return true;
    return size > (KERNEL_AREA_MEM - addr);
}

#define UNUSED(...)                                                                                \
    do {                                                                                           \
        (void)(0, ##__VA_ARGS__);                                                                  \
    } while (0)
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
void   *memchr(const void *buffer, int value, size_t size);
void   *memcpy(void *dest, const void *src, size_t n);
void   *memset(void *dst, int val, size_t size);
char   *strcpy(char *dest, const char *src);
size_t  strlen(const char *str);
size_t  strnlen(const char *str, size_t maxlen);
char   *strcat(char *dest, const char *src);
char   *strchrnul(const char *s, int c);
int     strncmp(const char *s1, const char *s2, size_t n);
char   *strchr(const char *s, int c);
int     strcmp(const char *s1, const char *s2);
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
static inline char *LeadingWhitespace(char *beg, char *end)
{
    while (end > beg && *--end <= 0x20)
    {
        *end = 0;
    }
    while (beg < end && *beg <= 0x20)
    {
        beg++;
    }
    return beg;
}
static inline void *memmove(void *dest, const void *src, size_t n)
{
    uint8_t       *pdest = (uint8_t *)dest;
    const uint8_t *psrc  = (const uint8_t *)src;

    if (src > dest)
    {
        for (size_t i = 0; i < n; i++)
        {
            pdest[i] = psrc[i];
        }
    }
    else if (src < dest)
    {
        for (size_t i = n; i > 0; i--)
        {
            pdest[i - 1] = psrc[i - 1];
        }
    }

    return dest;
}

void cmd_free(char **argv, int argc);
int cmd_parse(const char *cmd_str, char **argv, char token);
int cmd_parse_limit(const char *cmd_str, char **argv, char token, int max_argc);

#ifdef __cplusplus
} // extern "C"
#endif

#define PADDING_DOWN(size, to) ((size_t)(size) / (size_t)(to) * (size_t)(to))
#define PADDING_UP(size, to)   PADDING_DOWN((size_t)(size) + (size_t)(to) - (size_t)1, to)
#define PADDING_REQ(size, to)  ((size + (to) - 1) & ~((to) - 1) / (to))
#define IS_DIGIT(c)            ((c) >= '0' && (c) <= '9')
#define IS_ALPHA(a)            (((a) >= 'A' && (a) <= 'Z') || ((a) >= 'a' && (a) <= 'z'))
