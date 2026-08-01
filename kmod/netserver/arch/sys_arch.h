#pragma once

#include <arch/cc.h>
#include <task/scheduler.h>

#ifndef SPIN_INIT
typedef struct spinlock {
    volatile long lock;
    long          rflags;
} spin_t;

#define SPIN_INIT (spin_t){0, 0}

static inline void spin_init(spin_t *lock)
{
    lock->lock   = 0;
    lock->rflags = 0;
}

static inline void spin_lock(spin_t *lock)
{
    for (;;) {
        unsigned char busy;
        asm volatile("lock btsq $0, %1\n\t"
                     "setc %0\n\t"
                     : "=q"(busy), "+m"(lock->lock)
                     :
                     : "memory", "cc");
        if (!busy) {
            break;
        }
        scheduler_yield();
        asm volatile("pause" ::: "memory");
    }

    asm volatile("mfence" ::: "memory");

    long flags;
    asm volatile("pushfq\n\t"
                 "pop %0\n\t"
                 : "=r"(flags)
                 :
                 : "memory");

    asm volatile("cli\n\t");
    lock->rflags = flags;
}

static inline void spin_unlock(spin_t *lock)
{
    asm volatile("lock btrq $0, %0\n\t"
                 : "+m"(lock->lock)
                 :
                 : "memory", "cc");

    asm volatile("push %0\n\t"
                 "popfq"
                 :
                 : "r"(lock->rflags)
                 : "memory");

    asm volatile("sfence" ::: "memory");
}
#endif

typedef unsigned long sys_prot_t;
typedef struct sys_mutex {
    volatile long state;
    void    *owner;
    u32_t    depth;
    bool     valid;
} sys_mutex_t;

typedef struct sys_sem {
    bool   invalid;
    spin_t lock;
    u32_t  cnt;
} sys_sem_t;

typedef struct mbox_block {
    struct mbox_block *next;
    void              *task;
    bool               write;
} mboxBlock;

typedef struct sys_mbox {
    bool       invalid;
    spin_t     lock;
    int        size;
    void     **msges;
    u32_t      ptrWrite;
    u32_t      ptrRead;
} sys_mbox_t;

typedef size_t sys_thread_t;
