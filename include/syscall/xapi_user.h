#pragma once

#include <errno.h>
#include <krlibc.h>
#include <mm/uaccess.h>
#include <task/pcb.h>

static constexpr size_t XAPI_USER_STRING_MAX = 4096UL;
static constexpr size_t XAPI_USER_PATH_MAX   = 4096UL;
static constexpr size_t XAPI_IO_BOUNCE_BYTES = 0x10000UL;
static constexpr size_t XAPI_RUN_ARG_MAX     = 128UL;

static inline page_directory_t *xapi_current_pagedir()
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL) return NULL;
    return task->parent_group->pagedir;
}

static inline bool xapi_checked_add_size(size_t a, size_t b, size_t *out)
{
    if (out == NULL) return false;
    if (a > (size_t)-1 - b) return false;
    *out = a + b;
    return true;
}

static inline bool xapi_checked_mul_size(size_t a, size_t b, size_t *out)
{
    if (out == NULL) return false;
    if (a != 0 && b > (size_t)-1 / a) return false;
    *out = a * b;
    return true;
}

static inline size_t xapi_strnlen(const char *str, size_t max_len)
{
    if (str == NULL) return 0;
    size_t len = 0;
    while (len < max_len && str[len] != '\0') len++;
    return len;
}

static inline int xapi_copy_string_from_user(char **out, const char *src, size_t max_len)
{
    if (out == NULL) return -EINVAL;
    *out = NULL;
    if (src == NULL || max_len == 0) return -EINVAL;

    page_directory_t *pagedir = xapi_current_pagedir();
    if (pagedir == NULL) return -EFAULT;

    char *tmp = (char *)malloc(max_len);
    if (tmp == NULL) return -ENOMEM;

    for (size_t i = 0; i < max_len; i++)
    {
        char c = 0;
        if (!copy_from_user_pagedir(pagedir, &c, src + i, sizeof(c)))
        {
            free(tmp);
            return -EFAULT;
        }
        tmp[i] = c;
        if (c == '\0')
        {
            *out = tmp;
            return 0;
        }
    }

    free(tmp);
    return -ENAMETOOLONG;
}
