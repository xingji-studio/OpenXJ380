#pragma once

#include "sys/types.h"

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *dst, int val, size_t size);
void *memchr(const void *s, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t size);
int   bcmp(const void *s1, const void *s2, size_t n);
void  bcopy(const void *src, void *dest, size_t n);
void  bzero(void *s, size_t n);

size_t strlen(const char *str);
size_t strnlen(const char *str, size_t maxlen);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
int    strcasecmp(const char *s1, const char *s2);
int    strncasecmp(const char *s1, const char *s2, size_t n);
char  *strcpy(char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);
char  *strcat(char *dest, const char *src);
char  *strchr(const char *s, int c);
char  *strchrnul(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strdup(const char *str);
char  *strndup(const char *str, size_t n);
char  *strtok(char *str, const char *delim);
char  *strtok_r(char *str, const char *delim, char **saveptr);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
char  *index(const char *s, int c);
char  *rindex(const char *s, int c);

#ifdef __cplusplus
}
#endif
