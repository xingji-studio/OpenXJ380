// XJ380 User-space C Library - Miscellaneous Functions
// This file provides errno, ctype, and other utility functions

#include "./include/stdint.h"
#include "./include/xposix/errno.h"
#include "./include/xposix/string.h"
#include "./include/xposix/stdio.h"
#include "./include/xposix/unistd.h"
#include "./include/libsys.h"

extern "C" {
int snprintf(char *str, size_t size, const char *format, ...);

// errno is defined in libc_unistd.cpp
extern int errno;

int isalpha(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

int isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

int isupper(int c)
{
    return c >= 'A' && c <= 'Z';
}

int islower(int c)
{
    return c >= 'a' && c <= 'z';
}

int toupper(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

int tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

int isprint(int c)
{
    return c >= 32 && c <= 126;
}

int ispunct(int c)
{
    return isprint(c) && !isalnum(c) && !isspace(c);
}

int iscntrl(int c)
{
    return (c >= 0 && c < 32) || c == 127;
}

int isxdigit(int c)
{
    return isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

int isgraph(int c)
{
    return c > 32 && c <= 126;
}

int isblank(int c)
{
    return c == ' ' || c == '\t';
}

// Additional utility functions
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size > 0)
    {
        size_t copy_len = (len >= size) ? size - 1 : len;
        memcpy(dst, src, copy_len);
        dst[copy_len] = '\0';
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dst_len = strlen(dst);
    size_t src_len = strlen(src);

    if (dst_len >= size) return size + src_len;

    size_t copy_len = size - dst_len - 1;
    if (copy_len > src_len) copy_len = src_len;

    memcpy(dst + dst_len, src, copy_len);
    dst[dst_len + copy_len] = '\0';

    return dst_len + src_len;
}

char *strsignal(int sig)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), "Signal %d", sig);
    return buf;
}

// Simple bump allocator using SYS_BRK
// Header: [size: 8 bytes][data: size bytes]
// free() just marks the block as dead (no coalescing)

#define MALLOC_ALIGN 16
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

typedef struct malloc_block
{
    size_t size;  // data size (not including header)
    int    freed; // 0 = in use, 1 = freed
} malloc_block_t;

static void *heap_break = NULL;

static void *brk_request(void *addr)
{
    return (void *)enter_syscall(SYS_BRK, (uint64_t)addr, 0, 0, 0, 0, 0);
}

void *malloc(size_t size)
{
    if (size == 0) return NULL;

    size_t aligned = ALIGN_UP(size, MALLOC_ALIGN);
    size_t total = sizeof(malloc_block_t) + aligned;

    // First call: initialize heap break
    if (heap_break == NULL)
    {
        void *cur = brk_request(NULL);
        if (cur == NULL) return NULL;
        heap_break = cur;
    }

    void *new_break = (void *)((uintptr_t)heap_break + total);
    void *result = brk_request(new_break);
    if (result != new_break) return NULL;

    malloc_block_t *block = (malloc_block_t *)heap_break;
    block->size = aligned;
    block->freed = 0;

    heap_break = new_break;
    return (void *)((uintptr_t)block + sizeof(malloc_block_t));
}

void free(void *ptr)
{
    if (ptr == NULL) return;
    malloc_block_t *block = (malloc_block_t *)((uintptr_t)ptr - sizeof(malloc_block_t));
    block->freed = 1;
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void  *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    if (ptr == NULL) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    malloc_block_t *block = (malloc_block_t *)((uintptr_t)ptr - sizeof(malloc_block_t));
    size_t old_size = block->size;

    if (size <= old_size) return ptr;

    void *new_ptr = malloc(size);
    if (new_ptr == NULL) return NULL;
    memcpy(new_ptr, ptr, old_size);
    free(ptr);
    return new_ptr;
}

} // extern "C"
