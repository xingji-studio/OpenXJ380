// XJ380 User-space C Library - String and Memory Functions
// This file provides standard C string and memory manipulation functions

#include "./include/stdint.h"
#include "./include/xposix/string.h"
#include "./include/xposix/stdlib.h"

extern "C" {
void *malloc(size_t size);
}

extern "C" {

void *memcpy(void *dest, const void *src, size_t n)
{
    char       *d = (char *)dest;
    const char *s = (const char *)src;

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

void *memmove(void *dest, const void *src, size_t n)
{
    if (dest == src) return dest;

    char       *d = (char *)dest;
    const char *s = (const char *)src;

    if (d < s)
    {
        while (n--)
        {
            *d++ = *s++;
        }
    }
    else
    {
        d += n;
        s += n;
        while (n--)
        {
            *--d = *--s;
        }
    }

    return dest;
}

void *memset(void *dst, int val, size_t size)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char  v = (unsigned char)val;

    while (size && ((size_t)d & 7))
    {
        *d++ = v;
        size--;
    }

    size_t v8 = v * 0x0101010101010101ULL;
    while (size >= 8)
    {
        *(size_t *)d = v8;
        d           += 8;
        size        -= 8;
    }

    while (size--)
        *d++ = v;

    return dst;
}

void *memchr(const void *buffer, int value, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)buffer;
    unsigned char needle = (unsigned char)value;
    for (size_t i = 0; i < size; ++i)
        if (bytes[i] == needle) return (void *)(bytes + i);
    return NULL;
}

int memcmp(const void *a_, const void *b_, size_t size)
{
    const char *a = (const char *)a_;
    const char *b = (const char *)b_;
    while (size-- > 0)
    {
        if (*a != *b) return *a > *b ? 1 : -1;
        a++, b++;
    }
    return 0;
}

int bcmp(const void *s1, const void *s2, size_t n)
{
    return memcmp(s1, s2, n);
}

void bcopy(const void *src, void *dest, size_t n)
{
    memmove(dest, src, n);
}

void bzero(void *s, size_t n)
{
    memset(s, 0, n);
}

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

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2)
    {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n-- > 0)
    {
        if (*s1 != *s2) return (unsigned char)*s1 - (unsigned char)*s2;
        if (*s1 == '\0') return 0;
        s1++;
        s2++;
    }
    return 0;
}

int strcasecmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2)
    {
        s1++;
        s2++;
    }

    char c1 = *s1, c2 = *s2;
    if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
    if (c2 >= 'A' && c2 <= 'Z') c2 += 32;

    return (unsigned char)c1 - (unsigned char)c2;
}

int strncasecmp(const char *s1, const char *s2, size_t n)
{
    while (n-- > 0)
    {
        char c1 = *s1, c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;

        if (c1 != c2) return (unsigned char)c1 - (unsigned char)c2;
        if (c1 == '\0') return 0;
        s1++;
        s2++;
    }
    return 0;
}

char *strcpy(char *dest, const char *src)
{
    char *ret = dest;
    while ((*dest++ = *src++) != '\0')
        ;
    return ret;
}

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

char *strcat(char *dest, const char *src)
{
    char *ret = dest;
    while (*dest)
        dest++;
    while ((*dest++ = *src++))
        ;
    return ret;
}

char *strchr(const char *s, int c)
{
    while (*s)
    {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return ((char)c == '\0') ? (char *)s : NULL;
}

char *strchrnul(const char *s, int c)
{
    while (*s)
    {
        if (*s == (char)c) break;
        s++;
    }
    return (char *)s;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s)
    {
        if (*s == (char)c) last = s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;

    size_t needle_len = strlen(needle);

    while (*haystack)
    {
        if (strncmp(haystack, needle, needle_len) == 0)
        {
            return (char *)haystack;
        }
        haystack++;
    }

    return NULL;
}

char *strdup(const char *str)
{
    if (str == NULL) return NULL;
    size_t len = strlen(str) + 1;
    char  *copy = (char *)malloc(len);
    if (copy == NULL) return NULL;
    memcpy(copy, str, len);
    return copy;
}

char *strndup(const char *str, size_t n)
{
    if (str == NULL) return NULL;
    size_t len = strnlen(str, n);
    char  *copy = (char *)malloc(len + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, str, len);
    copy[len] = '\0';
    return copy;
}

static char *strtok_last = NULL;

char *strtok(char *str, const char *delim)
{
    return strtok_r(str, delim, &strtok_last);
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    if (str == NULL) str = *saveptr;

    // Skip leading delimiters
    while (*str && strchr(delim, *str))
    {
        str++;
    }

    if (*str == '\0')
    {
        *saveptr = str;
        return NULL;
    }

    // Find end of token
    char *end = str;
    while (*end && !strchr(delim, *end))
    {
        end++;
    }

    if (*end)
    {
        *end = '\0';
        *saveptr = end + 1;
    }
    else
    {
        *saveptr = end;
    }

    return str;
}

size_t strspn(const char *s, const char *accept)
{
    size_t count = 0;
    while (*s && strchr(accept, *s))
    {
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t count = 0;
    while (*s && !strchr(reject, *s))
    {
        count++;
        s++;
    }
    return count;
}

char *strpbrk(const char *s, const char *accept)
{
    while (*s)
    {
        if (strchr(accept, *s)) return (char *)s;
        s++;
    }
    return NULL;
}

char *index(const char *s, int c)
{
    return strchr(s, c);
}

char *rindex(const char *s, int c)
{
    return strrchr(s, c);
}

} // extern "C"
