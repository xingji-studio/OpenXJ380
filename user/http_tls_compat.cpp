#include "krlibc.h"
#include "libsys.h"
#include "xposix/stdio.h"
#include "liballoc/alloc.h"
#include "mbed_compat/time.h"
#include <psa/crypto.h>
#include <psa/crypto_platform.h>

struct tls_timespec
{
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct tls_stat
{
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
};

static FILE g_tls_fake_file;
__attribute__((weak)) FILE *stderr = &g_tls_fake_file;
char __libc_single_threaded = 1;
extern "C" {
void *__dso_handle = NULL;
}
static unsigned char g_tls_runtime_storage[64 * 1024];

typedef unsigned int wint_t;
typedef void *iconv_t;

struct __locale_struct
{
    int dummy;
};

static constexpr double TLS_PI = 3.14159265358979323846;
static constexpr double TLS_HALF_PI = TLS_PI * 0.5;

static int tls_is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

static double tls_parse_double(const char *text, char **endptr)
{
    if (text == NULL) {
        if (endptr != NULL) *endptr = NULL;
        return 0.0;
    }

    const char *p = text;
    while (tls_is_space(*p)) ++p;

    int sign = 1;
    if (*p == '-') {
        sign = -1;
        ++p;
    } else if (*p == '+') {
        ++p;
    }

    double value = 0.0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10.0 + (double)(*p - '0');
        ++p;
    }

    if (*p == '.') {
        ++p;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') {
            value += (double)(*p - '0') * scale;
            scale *= 0.1;
            ++p;
        }
    }

    if (*p == 'e' || *p == 'E') {
        ++p;
        int exp_sign = 1;
        if (*p == '-') {
            exp_sign = -1;
            ++p;
        } else if (*p == '+') {
            ++p;
        }

        int exponent = 0;
        while (*p >= '0' && *p <= '9') {
            exponent = exponent * 10 + (*p - '0');
            ++p;
        }

        double factor = 1.0;
        for (int i = 0; i < exponent; ++i) factor *= 10.0;
        if (exp_sign < 0) value /= factor;
        else value *= factor;
    }

    if (endptr != NULL) *endptr = (char *)p;
    return value * (double)sign;
}

static double tls_abs_double(double value)
{
    return value < 0.0 ? -value : value;
}

static double tls_sqrt_double(double value)
{
    if (value <= 0.0) return 0.0;
    double estimate = value > 1.0 ? value : 1.0;
    for (int i = 0; i < 16; ++i) {
        estimate = 0.5 * (estimate + value / estimate);
    }
    return estimate;
}

static double tls_wrap_angle(double value)
{
    while (value > TLS_PI) value -= TLS_PI * 2.0;
    while (value < -TLS_PI) value += TLS_PI * 2.0;
    return value;
}

static double tls_tan_approx(double value)
{
    value = tls_wrap_angle(value);
    if (value > TLS_HALF_PI - 0.001) value = TLS_HALF_PI - 0.001;
    if (value < -TLS_HALF_PI + 0.001) value = -TLS_HALF_PI + 0.001;

    const double x2 = value * value;
    return value + (value * x2) / 3.0 + (2.0 * value * x2 * x2) / 15.0;
}

static double tls_atan_approx(double value)
{
    if (value > 1.0) return TLS_HALF_PI - tls_atan_approx(1.0 / value);
    if (value < -1.0) return -TLS_HALF_PI - tls_atan_approx(1.0 / value);

    const double x2 = value * value;
    return value / (1.0 + 0.280872 * x2);
}

static wchar_t *tls_wmemcpy(wchar_t *dest, const wchar_t *src, size_t count)
{
    for (size_t i = 0; i < count; ++i) dest[i] = src[i];
    return dest;
}

static wchar_t *tls_wmemmove(wchar_t *dest, const wchar_t *src, size_t count)
{
    if (dest == src || count == 0) return dest;
    if (dest < src) {
        for (size_t i = 0; i < count; ++i) dest[i] = src[i];
    } else {
        for (size_t i = count; i > 0; --i) dest[i - 1] = src[i - 1];
    }
    return dest;
}

static wchar_t *tls_wmemset(wchar_t *dest, wchar_t ch, size_t count)
{
    for (size_t i = 0; i < count; ++i) dest[i] = ch;
    return dest;
}

static const unsigned short *tls_ctype_table()
{
    static unsigned short table[384];
    static bool           initialized = false;
    if (!initialized) {
        initialized = true;
        unsigned short *base = table + 128;

        const unsigned short _ISupper = (1 << 0) << 8;
        const unsigned short _ISlower = (1 << 1) << 8;
        const unsigned short _ISalpha = (1 << 2) << 8;
        const unsigned short _ISdigit = (1 << 3) << 8;
        const unsigned short _ISxdigit = (1 << 4) << 8;
        const unsigned short _ISspace = (1 << 5) << 8;
        const unsigned short _ISprint = (1 << 6) << 8;
        const unsigned short _ISgraph = (1 << 7) << 8;
        const unsigned short _ISblank = (1 << 8) >> 8;
        const unsigned short _IScntrl = (1 << 9) >> 8;
        const unsigned short _ISpunct = (1 << 10) >> 8;
        const unsigned short _ISalnum = (1 << 11) >> 8;

        for (int c = 0; c < 256; ++c) {
            unsigned short flags = 0;
            if (c < 32 || c == 127) flags |= _IScntrl;
            if (c >= 32 && c <= 126) flags |= _ISprint;
            if (c >= 33 && c <= 126) flags |= _ISgraph;
            if (c == ' ' || c == '\t') flags |= _ISblank;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') flags |= _ISspace;
            if (c >= '0' && c <= '9') flags |= _ISdigit | _ISxdigit | _ISalnum;
            if (c >= 'A' && c <= 'Z') flags |= _ISupper | _ISalpha | _ISalnum;
            if (c >= 'a' && c <= 'z') flags |= _ISlower | _ISalpha | _ISalnum;
            if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) flags |= _ISxdigit;
            if ((flags & (_ISgraph | _ISalnum)) == _ISgraph) flags |= _ISpunct;
            base[c] = flags;
        }
    }
    return table + 128;
}

extern "C" __attribute__((weak)) void *memmove(void *dest, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0) return dest;

    if (d < s) {
        for (size_t i = 0; i < n; ++i) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; --i) d[i - 1] = s[i - 1];
    }

    return dest;
}

extern "C" __attribute__((weak)) void abort(void)
{
    for (;;) {}
}

extern "C" void __assert_fail(const char *, const char *, unsigned int, const char *)
{
    abort();
}

extern "C" int atexit(void (*)(void))
{
    return 0;
}

extern "C" int __cxa_atexit(void (*)(void *), void *, void *)
{
    return 0;
}

extern "C" __attribute__((weak)) void *memchr(const void *src, int ch, size_t len)
{
    const unsigned char *bytes = (const unsigned char *)src;
    for (size_t i = 0; i < len; ++i) {
        if (bytes[i] == (unsigned char)ch) {
            return (void *)(bytes + i);
        }
    }
    return NULL;
}

extern "C" __attribute__((weak)) int bcmp(const void *lhs, const void *rhs, size_t len)
{
    return memcmp(lhs, rhs, len);
}

extern "C" __attribute__((weak)) int tolower(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 'a';
    return ch;
}

extern "C" __attribute__((weak)) int toupper(int ch)
{
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 'A';
    return ch;
}

extern "C" __attribute__((weak)) int strcasecmp(const char *lhs, const char *rhs)
{
    while (*lhs != '\0' && *rhs != '\0') {
        const int diff = tolower((unsigned char)*lhs) - tolower((unsigned char)*rhs);
        if (diff != 0) return diff;
        ++lhs;
        ++rhs;
    }
    return tolower((unsigned char)*lhs) - tolower((unsigned char)*rhs);
}

extern "C" __attribute__((weak)) int strncasecmp(const char *lhs, const char *rhs, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        const int lch = tolower((unsigned char)lhs[i]);
        const int rch = tolower((unsigned char)rhs[i]);
        if (lch != rch || lhs[i] == '\0' || rhs[i] == '\0') {
            return lch - rch;
        }
    }
    return 0;
}

extern "C" __attribute__((weak)) char *strdup(const char *str)
{
    if (str == NULL) return NULL;
    const size_t len = strlen(str) + 1;
    char        *out = (char *)malloc(len);
    if (out == NULL) return NULL;
    memcpy(out, str, len);
    return out;
}

extern "C" __attribute__((weak)) const unsigned short int **__ctype_b_loc(void)
{
    static const unsigned short int *ptr = NULL;
    if (ptr == NULL) ptr = tls_ctype_table();
    return &ptr;
}

extern "C" void *__memcpy_chk(void *dest, const void *src, size_t len, size_t destlen)
{
    if (len > destlen) len = destlen;
    return memcpy(dest, src, len);
}

extern "C" void *__memset_chk(void *dest, int ch, size_t len, size_t destlen)
{
    if (len > destlen) len = destlen;
    return memset(dest, ch, len);
}

extern "C" int __snprintf_chk(char *str, size_t maxlen, int, size_t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(str, maxlen, fmt, ap);
    va_end(ap);
    return ret;
}

extern "C" int __vsnprintf_chk(char *str, size_t maxlen, int, size_t, const char *fmt, va_list ap)
{
    return vsnprintf(str, maxlen, fmt, ap);
}

extern "C" int __printf_chk(int, const char *fmt, ...)
{
    char    buffer[512];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    if (ret > 0) {
        int write_len = ret;
        if (write_len >= (int)sizeof(buffer)) write_len = (int)sizeof(buffer) - 1;
        write(1, buffer, (uint64_t)write_len);
    }

    return ret;
}

extern "C" int __vfprintf_chk(FILE *stream, int, const char *fmt, va_list ap)
{
    char buffer[512];
    int  ret = vsnprintf(buffer, sizeof(buffer), fmt, ap);

    if (ret > 0) {
        int write_len = ret;
        if (write_len >= (int)sizeof(buffer)) write_len = (int)sizeof(buffer) - 1;
        write(stream == stderr ? 2 : 1, buffer, (uint64_t)write_len);
    }
    return ret;
}

extern "C" int __fprintf_chk(FILE *stream, int flag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = __vfprintf_chk(stream, flag, fmt, ap);
    va_end(ap);
    return ret;
}

extern "C" int __sprintf_chk(char *str, int, size_t maxlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(str, maxlen, fmt, ap);
    va_end(ap);
    return ret;
}

extern "C" __attribute__((weak)) int printf(const char *fmt, ...)
{
    char    buffer[512];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    if (ret > 0) {
        int write_len = ret;
        if (write_len >= (int)sizeof(buffer)) write_len = (int)sizeof(buffer) - 1;
        write(1, buffer, (uint64_t)write_len);
    }

    return ret;
}

extern "C" __attribute__((weak)) int *__errno_location(void)
{
    static int g_errno = 0;
    return &g_errno;
}

extern "C" char *gettext(const char *msgid)
{
    return (char *)(msgid == NULL ? "" : msgid);
}

extern "C" char *secure_getenv(const char *)
{
    return NULL;
}

extern "C" unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base)
{
    return (unsigned long)strtol(nptr, endptr, base);
}

extern "C" long __isoc23_strtol(const char *nptr, char **endptr, int base)
{
    return (long)strtol(nptr, endptr, base);
}

static inline uint64_t tls_rdtsc()
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t tls_mix64(uint64_t x)
{
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return x * 2685821657736338717ULL;
}

extern "C" time_t time(time_t *timer)
{
    time_t now = (time_t)(tls_rdtsc() / 1000000ULL);
    if (timer != NULL) *timer = now;
    return now;
}

extern "C" struct tm *gmtime_r(const time_t *timer, struct tm *result)
{
    if (result == NULL) return NULL;
    memset(result, 0, sizeof(*result));
    if (timer != NULL) result->tm_sec = (int)(*timer % 60);
    return result;
}

extern "C" void setbuf(FILE *, char *)
{
}

extern "C" __attribute__((weak)) size_t fwrite(const void *, size_t size, size_t count, FILE *)
{
    return size * count;
}

extern "C" __attribute__((weak)) int fputs(const char *str, FILE *stream)
{
    if (str == NULL) return -1;
    int len = (int)strlen(str);
    return write(stream == stderr ? 2 : 1, (char *)str, (uint64_t)len);
}

extern "C" __attribute__((weak)) int fputc(int ch, FILE *stream)
{
    char value = (char)ch;
    return write(stream == stderr ? 2 : 1, &value, 1);
}

extern "C" __attribute__((weak)) char *fgets(char *, int, FILE *)
{
    return NULL;
}

extern "C" int remove(const char *)
{
    return -1;
}

extern "C" void *__newlocale(int, const char *, void *base)
{
    return base;
}

extern "C" void *__duplocale(void *loc)
{
    return loc;
}

extern "C" void __freelocale(void *)
{
}

extern "C" void *__uselocale(void *loc)
{
    return loc;
}

extern "C" char *__nl_langinfo_l(int, void *)
{
    return (char *)"";
}

extern "C" char *bind_textdomain_codeset(const char *, const char *codeset)
{
    return (char *)(codeset == NULL ? "" : codeset);
}

extern "C" char *dgettext(const char *, const char *msgid)
{
    return (char *)(msgid == NULL ? "" : msgid);
}

extern "C" int __read_chk(int fd, void *buf, size_t nbytes, size_t bufsize)
{
    if (nbytes > bufsize) nbytes = bufsize;
    return read(fd, (char *)buf, (uint64_t)nbytes);
}

extern "C" int inet_pton(int af, const char *src, void *dst)
{
    if (src == NULL || dst == NULL) return 0;

    auto parse_ipv4_bytes = [](const char *text, uint8_t out[4]) -> int {
        uint32_t parts[4] = {0, 0, 0, 0};
        int      part = 0;
        uint32_t value = 0;

        for (const char *p = text;; ++p) {
            char ch = *p;
            if (ch >= '0' && ch <= '9') {
                value = value * 10 + (uint32_t)(ch - '0');
                if (value > 255) return 0;
            } else if (ch == '.' || ch == '\0') {
                if (part >= 4) return 0;
                parts[part++] = value;
                value = 0;
                if (ch == '\0') break;
            } else {
                return 0;
            }
        }

        if (part != 4) return 0;
        out[0] = (uint8_t)parts[0];
        out[1] = (uint8_t)parts[1];
        out[2] = (uint8_t)parts[2];
        out[3] = (uint8_t)parts[3];
        return 1;
    };

    auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };

    if (af == 2) {
        return parse_ipv4_bytes(src, (uint8_t *)dst);
    }
    if (af != 10) {
        return 0;
    }

    uint16_t    words[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int         word_count = 0;
    int         compress_at = -1;
    const char *p = src;
    const char *end = src + strlen(src);

    if ((end - p) >= 2 && p[0] == '[' && end[-1] == ']') {
        ++p;
        --end;
    }
    if (p == end) {
        return 0;
    }

    if (*p == ':') {
        if (p + 1 >= end || p[1] != ':') {
            return 0;
        }
        compress_at = 0;
        p += 2;
    }

    while (p < end) {
        if (word_count >= 8) {
            return 0;
        }

        const char *segment_start = p;
        while (p < end && *p != ':') {
            ++p;
        }

        const char *dot = NULL;
        for (const char *cursor = segment_start; cursor < p; ++cursor) {
            if (*cursor == '.') {
                dot = cursor;
                break;
            }
        }

        if (dot != NULL) {
            if (word_count > 6) {
                return 0;
            }

            char tail[32];
            size_t tail_len = (size_t)(end - segment_start);
            if (tail_len == 0 || tail_len >= sizeof(tail)) {
                return 0;
            }
            memcpy(tail, segment_start, tail_len);
            tail[tail_len] = '\0';

            uint8_t ipv4_bytes[4];
            if (!parse_ipv4_bytes(tail, ipv4_bytes)) {
                return 0;
            }
            words[word_count++] = (uint16_t)(((uint16_t)ipv4_bytes[0] << 8) | ipv4_bytes[1]);
            words[word_count++] = (uint16_t)(((uint16_t)ipv4_bytes[2] << 8) | ipv4_bytes[3]);
            p = end;
            break;
        }

        if (segment_start == p) {
            return 0;
        }

        uint32_t value = 0;
        int      digits = 0;
        for (const char *cursor = segment_start; cursor < p; ++cursor) {
            int hex = hex_value(*cursor);
            if (hex < 0) {
                return 0;
            }
            value = (value << 4) | (uint32_t)hex;
            if (++digits > 4) {
                return 0;
            }
        }
        words[word_count++] = (uint16_t)value;

        if (p >= end) {
            break;
        }

        ++p;
        if (p < end && *p == ':') {
            if (compress_at >= 0) {
                return 0;
            }
            compress_at = word_count;
            ++p;
            if (p >= end) {
                break;
            }
        }
    }

    if (compress_at < 0) {
        if (word_count != 8) {
            return 0;
        }
    } else {
        int zeros = 8 - word_count;
        if (zeros < 0) {
            return 0;
        }
        for (int i = word_count - 1; i >= compress_at; --i) {
            words[i + zeros] = words[i];
        }
        for (int i = 0; i < zeros; ++i) {
            words[compress_at + i] = 0;
        }
    }

    uint8_t *out = (uint8_t *)dst;
    for (int i = 0; i < 8; ++i) {
        out[i * 2]     = (uint8_t)(words[i] >> 8);
        out[i * 2 + 1] = (uint8_t)(words[i] & 0xffU);
    }
    return 1;
}

extern "C" int rand(void)
{
    return (int)(tls_rdtsc() & 0x7fffffff);
}

extern "C" wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t count)
{
    return tls_wmemcpy(dest, src, count);
}

extern "C" wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t count)
{
    return tls_wmemmove(dest, src, count);
}

extern "C" wchar_t *wmemset(wchar_t *dest, wchar_t ch, size_t count)
{
    return tls_wmemset(dest, ch, count);
}

extern "C" wchar_t *wmemchr(const wchar_t *src, wchar_t ch, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (src[i] == ch) return (wchar_t *)(src + i);
    }
    return NULL;
}

extern "C" int wmemcmp(const wchar_t *lhs, const wchar_t *rhs, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (lhs[i] != rhs[i]) return lhs[i] < rhs[i] ? -1 : 1;
    }
    return 0;
}

extern "C" size_t wcslen(const wchar_t *str)
{
    size_t len = 0;
    while (str[len] != 0) ++len;
    return len;
}

extern "C" int wcscmp(const wchar_t *lhs, const wchar_t *rhs)
{
    while (*lhs != 0 && *rhs != 0) {
        if (*lhs != *rhs) return *lhs < *rhs ? -1 : 1;
        ++lhs;
        ++rhs;
    }
    if (*lhs == *rhs) return 0;
    return *lhs == 0 ? -1 : 1;
}

extern "C" wchar_t *__wmemcpy_chk(wchar_t *dest, const wchar_t *src, size_t count, size_t destlen)
{
    size_t max_count = destlen / sizeof(wchar_t);
    if (count > max_count) count = max_count;
    return tls_wmemcpy(dest, src, count);
}

extern "C" wchar_t *__wmemset_chk(wchar_t *dest, wchar_t ch, size_t count, size_t destlen)
{
    size_t max_count = destlen / sizeof(wchar_t);
    if (count > max_count) count = max_count;
    return tls_wmemset(dest, ch, count);
}

extern "C" int __strcoll_l(const char *lhs, const char *rhs, void *)
{
    return strcmp(lhs, rhs);
}

extern "C" size_t __strxfrm_l(char *dest, const char *src, size_t count, void *)
{
    const size_t len = strlen(src);
    if (count > 0) {
        size_t copy_len = len < count - 1 ? len : count - 1;
        memcpy(dest, src, copy_len);
        dest[copy_len] = '\0';
    }
    return len;
}

extern "C" int __wcscoll_l(const wchar_t *lhs, const wchar_t *rhs, void *)
{
    while (*lhs != 0 && *rhs != 0) {
        if (*lhs != *rhs) return *lhs < *rhs ? -1 : 1;
        ++lhs;
        ++rhs;
    }
    if (*lhs == *rhs) return 0;
    return *lhs == 0 ? -1 : 1;
}

extern "C" size_t __wcsxfrm_l(wchar_t *dest, const wchar_t *src, size_t count, void *)
{
    const size_t len = wcslen(src);
    if (count > 0) {
        size_t copy_len = len < count - 1 ? len : count - 1;
        tls_wmemcpy(dest, src, copy_len);
        dest[copy_len] = 0;
    }
    return len;
}

extern "C" float __strtof_l(const char *nptr, char **endptr, void *)
{
    return (float)tls_parse_double(nptr, endptr);
}

extern "C" double __strtod_l(const char *nptr, char **endptr, void *)
{
    return tls_parse_double(nptr, endptr);
}

extern "C" long double strtold_l(const char *nptr, char **endptr, void *)
{
    return (long double)tls_parse_double(nptr, endptr);
}

extern "C" size_t __ctype_get_mb_cur_max(void)
{
    return 1;
}

extern "C" wint_t btowc(int ch)
{
    if (ch == EOF) return (wint_t)-1;
    return (wint_t)(unsigned char)ch;
}

extern "C" int wctob(wint_t ch)
{
    return ch <= 0xFF ? (int)ch : EOF;
}

extern "C" wint_t __towupper_l(wint_t ch, void *)
{
    return (wint_t)toupper((int)ch);
}

extern "C" wint_t __towlower_l(wint_t ch, void *)
{
    return (wint_t)tolower((int)ch);
}

extern "C" unsigned long __wctype_l(const char *, void *)
{
    return 0;
}

extern "C" int __iswctype_l(wint_t ch, unsigned long, void *)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

extern "C" size_t wcsnrtombs(char *dest, const wchar_t **src, size_t nwc, size_t len, void *)
{
    size_t copied = 0;
    if (src == NULL || *src == NULL) return 0;
    while (copied < nwc && copied < len && (*src)[copied] != 0) {
        if (dest != NULL) dest[copied] = (char)(*src)[copied];
        ++copied;
    }
    *src += copied;
    return copied;
}

extern "C" size_t mbsnrtowcs(wchar_t *dest, const char **src, size_t nms, size_t len, void *)
{
    size_t copied = 0;
    if (src == NULL || *src == NULL) return 0;
    while (copied < nms && copied < len && (*src)[copied] != '\0') {
        if (dest != NULL) dest[copied] = (unsigned char)(*src)[copied];
        ++copied;
    }
    *src += copied;
    return copied;
}

extern "C" size_t __mbsrtowcs_chk(wchar_t *dest, const char **src, size_t len, size_t dstlen, void *ps)
{
    if (len > dstlen) len = dstlen;
    return mbsnrtowcs(dest, src, len, len, ps);
}

extern "C" size_t __mbsnrtowcs_chk(wchar_t *dest, const char **src, size_t nms, size_t len,
                                   size_t dstlen, void *ps)
{
    if (len > dstlen) len = dstlen;
    return mbsnrtowcs(dest, src, nms, len, ps);
}

extern "C" size_t mbrtowc(wchar_t *pwc, const char *src, size_t len, void *)
{
    if (src == NULL) return 0;
    if (len == 0) return (size_t)-2;
    if (*src == '\0') {
        if (pwc != NULL) *pwc = 0;
        return 0;
    }
    if (pwc != NULL) *pwc = (unsigned char)*src;
    return 1;
}

extern "C" size_t wcrtomb(char *dest, wchar_t wc, void *)
{
    if (dest == NULL) return 1;
    if ((unsigned int)wc > 0xFFU) return (size_t)-1;
    *dest = (char)wc;
    return 1;
}

extern "C" size_t __strftime_l(char *dest, size_t max, const char *, const struct tm *, void *)
{
    if (dest != NULL && max > 0) dest[0] = '\0';
    return 0;
}

extern "C" size_t __wcsftime_l(wchar_t *dest, size_t max, const wchar_t *, const struct tm *, void *)
{
    if (dest != NULL && max > 0) dest[0] = 0;
    return 0;
}

extern "C" iconv_t iconv_open(const char *, const char *)
{
    return (iconv_t)1;
}

extern "C" size_t iconv(iconv_t, char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft)
{
    if (inbuf == NULL || inbytesleft == NULL || outbuf == NULL || outbytesleft == NULL) {
        return (size_t)-1;
    }

    size_t converted = 0;
    while (*inbytesleft > 0 && *outbytesleft > 0) {
        **outbuf = **inbuf;
        ++(*inbuf);
        ++(*outbuf);
        --(*inbytesleft);
        --(*outbytesleft);
        ++converted;
    }

    return *inbytesleft == 0 ? 0 : (size_t)-1;
}

extern "C" int iconv_close(iconv_t)
{
    return 0;
}

extern "C" int getentropy(void *buffer, size_t length)
{
    if (buffer == NULL) return -1;

    uint8_t *out = (uint8_t *)buffer;
    uint64_t state = tls_rdtsc() ^ (uint64_t)(uintptr_t)buffer;
    for (size_t i = 0; i < length; ++i) {
        state = tls_mix64(state + 0x9e3779b97f4a7c15ULL + i);
        out[i] = (uint8_t)(state >> ((i & 7U) * 8U));
    }
    return 0;
}

extern "C" unsigned int arc4random(void)
{
    return (unsigned int)tls_mix64(tls_rdtsc());
}

#if defined(MBEDTLS_PSA_CRYPTO_C)
extern "C" psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t *context,
    uint8_t *output, size_t output_size, size_t *output_length)
{
    if (context == NULL || output == NULL || output_length == NULL) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    uint64_t *state = (uint64_t *)context;
    if (state[0] == 0 && state[1] == 0) {
        state[0] = tls_rdtsc() ^ (uint64_t)(uintptr_t)context ^ 0x5858333830475541ULL;
        state[1] = tls_mix64((uint64_t)(uintptr_t)output ^ 0x9e3779b97f4a7c15ULL);
    }

    for (size_t i = 0; i < output_size; ++i) {
        state[0] ^= tls_rdtsc() + ((uint64_t)(uintptr_t)&output[i] << 7);
        state[0] = tls_mix64(state[0] + state[1]);
        state[1] = tls_mix64(state[1] + 0x9e3779b97f4a7c15ULL + i);
        output[i] = (uint8_t)(state[(i & 1U)] >> ((i & 7U) * 8U));
    }

    *output_length = output_size;
    return PSA_SUCCESS;
}

extern "C" void psa_reset_key_attributes(psa_key_attributes_t *attributes)
{
    if (attributes != NULL) {
        memset(attributes, 0, sizeof(*attributes));
    }
}
#endif

extern "C" long syscall(long, ...)
{
    return -1;
}

extern "C" char *strerror_r(int errnum, char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0) return NULL;
    snprintf(buf, buflen, "errno %d", errnum);
    return buf;
}

extern "C" double fmod(double x, double y)
{
    if (y == 0.0) return 0.0;
    long long q = (long long)(x / y);
    return x - (double)q * y;
}

extern "C" float fmodf(float x, float y)
{
    if (y == 0.0f) return 0.0f;
    int q = (int)(x / y);
    return x - (float)q * y;
}

extern "C" double round(double x)
{
    long long value = (long long)(x >= 0.0 ? x + 0.5 : x - 0.5);
    return (double)value;
}

extern "C" float roundf(float x)
{
    int value = (int)(x >= 0.0f ? x + 0.5f : x - 0.5f);
    return (float)value;
}

extern "C" double nearbyint(double x)
{
    return round(x);
}

extern "C" double pow(double x, double y)
{
    long long exp = (long long)y;
    if ((double)exp != y) {
        return 0.0;
    }

    bool negative = exp < 0;
    if (negative) exp = -exp;

    double result = 1.0;
    double base = x;
    while (exp > 0) {
        if (exp & 1LL) result *= base;
        base *= base;
        exp >>= 1;
    }
    return negative ? (result == 0.0 ? 0.0 : 1.0 / result) : result;
}

extern "C" double sqrt(double x)
{
    return tls_sqrt_double(x);
}

extern "C" float sqrtf(float x)
{
    return (float)tls_sqrt_double((double)x);
}

extern "C" double tan(double x)
{
    return tls_tan_approx(x);
}

extern "C" double atan2(double y, double x)
{
    if (x > 0.0) {
        return tls_atan_approx(y / x);
    }
    if (x < 0.0) {
        return y >= 0.0 ? tls_atan_approx(y / x) + TLS_PI
                         : tls_atan_approx(y / x) - TLS_PI;
    }
    if (tls_abs_double(y) < 1e-12) return 0.0;
    return y > 0.0 ? TLS_HALF_PI : -TLS_HALF_PI;
}

extern "C" void __explicit_bzero_chk(void *dest, size_t len, size_t destlen)
{
    if (len > destlen) len = destlen;
    memset(dest, 0, len);
}

extern "C" int pthread_mutex_init(int *mutex, const int *)
{
    if (mutex != NULL) *mutex = 0;
    return 0;
}

extern "C" int pthread_mutex_destroy(int *)
{
    return 0;
}

extern "C" int pthread_mutex_lock(int *)
{
    return 0;
}

extern "C" int pthread_mutex_unlock(int *)
{
    return 0;
}

extern "C" int pthread_once(int *, void (*init_routine)(void))
{
    if (init_routine != NULL) init_routine();
    return 0;
}

extern "C" int pthread_cond_wait(int *, int *)
{
    return 0;
}

extern "C" int pthread_cond_broadcast(int *)
{
    return 0;
}

extern "C" int _dl_find_object(void *, void *)
{
    return -1;
}

struct tls_index
{
    uint64_t module;
    uint64_t offset;
};

extern "C" void *__tls_get_addr(void *arg)
{
    if (arg == NULL) {
        return g_tls_runtime_storage;
    }

    tls_index *index = (tls_index *)arg;
    uint64_t   offset = index->offset;
    if (offset >= sizeof(g_tls_runtime_storage)) {
        offset %= (sizeof(g_tls_runtime_storage) - 64);
    }

    return g_tls_runtime_storage + offset;
}

extern "C" uint64_t __stack_chk_guard;

extern "C" int mbedtls_aesni_has_support(unsigned int)
{
    return 0;
}

extern "C" int mbedtls_aesni_crypt_ecb(...)
{
    return -1;
}

extern "C" int mbedtls_aesni_gcm_mult(...)
{
    return -1;
}

extern "C" int mbedtls_aesni_inverse_key(...)
{
    return -1;
}

extern "C" int mbedtls_aesni_setkey_enc(...)
{
    return -1;
}

extern "C" int xtls_prepare_runtime(uint64_t *saved_fs, void **tls_block)
{
    if (saved_fs == NULL || tls_block == NULL) return -1;

    uint8_t *block = (uint8_t *)malloc(0x60);
    if (block == NULL) return -1;

    memset(block, 0, 0x60);

    // libsupc++ expects __cxa_get_globals() to read a thread pointer from %fs:0
    // and then address exception globals at thread_pointer - 0x10.
    uint8_t *fs_base = block + 0x10;
    *(uint64_t *)(fs_base + 0x00) = (uint64_t)fs_base;

    uint64_t original_fs = enter_syscall(SYS_ARCH_PRCTL, ARCH_GET_FS, 0, 0, 0, 0, 0);

    *(uint64_t *)(fs_base + 0x28) = __stack_chk_guard;

    uint64_t set_ret = enter_syscall(SYS_ARCH_PRCTL, ARCH_SET_FS, (uint64_t)fs_base, 0, 0, 0, 0);
    if ((int64_t)set_ret < 0) {
        free(block);
        return -1;
    }

    *saved_fs = original_fs;
    *tls_block = block;
    return 0;
}

extern "C" void xtls_restore_runtime(uint64_t saved_fs, void *tls_block)
{
    enter_syscall(SYS_ARCH_PRCTL, ARCH_SET_FS, saved_fs, 0, 0, 0, 0);
    if (tls_block != NULL) free(tls_block);
}
