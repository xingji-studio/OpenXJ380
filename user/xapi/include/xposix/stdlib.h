#pragma once

#include "sys/types.h"
#include "../stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

void exit(int status);
void _exit(int status);
void abort(void);

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

int atoi(const char *nptr);
int abs(int value);
long atol(const char *nptr);
long long atoll(const char *nptr);
int64_t strtol(const char *nptr, char **endptr, int base);
uint64_t strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

extern char **environ;
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int grantpt(int fd);
int unlockpt(int fd);
char *ptsname(int fd);

#ifdef __cplusplus
}
#endif
