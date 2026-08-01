#pragma once

#include "../../../include/stdint.h"
#include "../../../include/errno.h"

#ifndef SSIZE_MAX
#define SSIZE_MAX 0x7fffffffffffffffL
#endif

typedef intptr_t ptrdiff_t;

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#ifndef LWIP_NO_STDDEF_H
#define LWIP_NO_STDDEF_H 1
#endif

#ifndef LWIP_NO_LIMITS_H
#define LWIP_NO_LIMITS_H 1
#endif

#ifndef LWIP_NO_UNISTD_H
#define LWIP_NO_UNISTD_H 1
#endif

#ifndef LWIP_NO_CTYPE_H
#define LWIP_NO_CTYPE_H 1
#endif

#ifndef LWIP_TIMEVAL_PRIVATE
#define LWIP_TIMEVAL_PRIVATE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

int printk(const char *fmt, ...);
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void free(void *ptr);
int memcmp(const void *lhs, const void *rhs, size_t size);
void *memcpy(void *dst, const void *src, size_t len);
void *memset(void *dst, int value, size_t len);
size_t strlen(const char *str);
char *strcat(char *dest, const char *src);
int strncmp(const char *lhs, const char *rhs, size_t len);
char *strchr(const char *str, int ch);
char *strstr(const char *haystack, const char *needle);
int strcmp(const char *lhs, const char *rhs);
int atoi(const char *str);
int isspace(int ch);
int64_t strtol(const char *str, char **endptr, int base);
char *strdup(const char *str);
char *strncpy(char *dest, const char *src, size_t len);
char *strrchr(const char *str, int ch);
int isdigit(int ch);
uint64_t nanoTime(void);
void delay_ms_hp(uint64_t ms);

#ifndef __cplusplus
static inline void *memmove(void *dest, const void *src, size_t len) {
    uint8_t *dst = (uint8_t *)dest;
    const uint8_t *source = (const uint8_t *)src;

    if (dst == source || len == 0) {
        return dest;
    }

    if (dst < source) {
        for (size_t i = 0; i < len; ++i) {
            dst[i] = source[i];
        }
    } else {
        for (size_t i = len; i > 0; --i) {
            dst[i - 1] = source[i - 1];
        }
    }
    return dest;
}
#endif

#ifdef __cplusplus
}
#endif
