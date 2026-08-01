#pragma once

#include "cc.h"

typedef unsigned long sys_prot_t;
typedef size_t        sys_thread_t;

typedef struct sys_mutex {
    volatile int held;
    void        *owner;
    uint32_t     depth;
    bool         valid;
} sys_mutex_t;

typedef struct sys_sem {
    volatile int guard;
    uint32_t     count;
    bool         valid;
} sys_sem_t;

typedef struct sys_mbox {
    volatile int guard;
    void       **entries;
    uint32_t     capacity;
    uint32_t     head;
    uint32_t     tail;
    uint32_t     count;
    bool         valid;
} sys_mbox_t;
