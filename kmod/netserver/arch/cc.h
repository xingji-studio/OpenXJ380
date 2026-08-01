#pragma once

#include "../../../include/errno.h"
#include "../../../include/stdint.h"

#define SSIZE_MAX 0x7fffffff

typedef intptr_t ptrdiff_t;

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_NO_STDDEF_H 1
#define LWIP_NO_LIMITS_H 1
#define LWIP_NO_UNISTD_H 1
#define LWIP_NO_CTYPE_H 1
#define LWIP_TIMEVAL_PRIVATE 0

#ifdef __cplusplus
extern "C" {
#endif

int      printk(const char *format, ...);
void    *malloc(size_t size);
void    *calloc(size_t count, size_t size);
void     free(void *pointer);
int      memcmp(const void *left, const void *right, size_t size);
void    *memcpy(void *destination, const void *source, size_t size);
void    *memset(void *destination, int value, size_t size);
size_t   strlen(const char *text);
char    *strcat(char *destination, const char *source);
int      strncmp(const char *left, const char *right, size_t size);
char    *strchr(const char *text, int character);
char    *strstr(const char *text, const char *needle);
int      strcmp(const char *left, const char *right);
int      atoi(const char *text);
int      isspace(int character);
int64_t  strtol(const char *text, char **end, int base);
char    *strdup(const char *text);
char    *strncpy(char *destination, const char *source, size_t size);
char    *strrchr(const char *text, int character);
int      isdigit(int character);
uint64_t nanoTime(void);
void     delay_ms_hp(uint64_t milliseconds);

#ifdef __cplusplus
}
#endif

#ifndef __cplusplus
static inline void *memmove(void *destination, const void *source, size_t size)
{
    uint8_t       *output = destination;
    const uint8_t *input = source;
    if (output < input) {
        for (size_t index = 0; index < size; index++) {
            output[index] = input[index];
        }
    } else if (output > input) {
        for (size_t index = size; index != 0; index--) {
            output[index - 1] = input[index - 1];
        }
    }
    return destination;
}
#endif
