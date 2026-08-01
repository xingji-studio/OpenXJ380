#pragma once

#include "task/pcb.h"
#include "lock_queue.h"

// 互斥锁状态
typedef enum {
    MUTEX_UNLOCKED = 0,  // 未锁定
    MUTEX_LOCKED = 1,    // 已锁定
    MUTEX_DESTROYED = 2  // 已销毁
} mutex_state_t;

// 互斥锁结构
typedef struct mutex {
    spin_t lock;          // 保护互斥锁内部结构的自旋锁
    mutex_state_t state;  // 锁状态
    tcb_t owner;          // 当前持有者（如果被锁定）
    lock_queue *wait_queue; // 等待队列
    size_t rcc; // 递归计数（用于可重入锁）
    bool rec;    // 是否为递归锁
} mutex_t;

void mutex_create(mutex_t* mtx,bool recursive);
int mutex_lock(mutex_t *mutex);
int mutex_trylock(mutex_t *mutex);
int mutex_unlock(mutex_t *mutex);
int mutex_destroy(mutex_t *mutex);
bool mutex_is_locked(mutex_t *mutex);
tcb_t mutex_get_owner(mutex_t *mutex);