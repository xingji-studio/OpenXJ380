#include "mutex.h"
#include "errno.h"
#include "task/scheduler.h"
#include "cpu/lock.h"
#include "mm/heap.h"
#include "krlibc.h"
#include <proto.hpp>

// 创建互斥锁
void mutex_create(mutex_t* mtx,bool recursive) {
    if (mtx == NULL) return;

    mtx->lock = SPIN_INIT;
    mtx->state = MUTEX_UNLOCKED;
    mtx->owner = NULL;
    mtx->wait_queue = queue_init();
    mtx->rcc = 0;
    mtx->rec = recursive;
}

// 当前调度器里的 WAIT 语义并不适合通用互斥，这里采用 yield 型互斥：
// 获取失败时主动让出时间片，但不把线程切到 WAIT，避免线程在未持锁时继续执行。
int mutex_lock(mutex_t *mutex) {
    if (mutex == NULL) return -EINVAL;

    tcb_t current = get_current_task();

    for (;;)
    {
        spin_lock(&mutex->lock);

        if (mutex->state == MUTEX_DESTROYED)
        {
            spin_unlock(&mutex->lock);
            return -EINVAL;
        }

        if (mutex->owner == current)
        {
            if (mutex->rec)
            {
                mutex->rcc++;
                spin_unlock(&mutex->lock);
                return 0;
            }

            spin_unlock(&mutex->lock);
            return -EDEADLK;
        }

        if (mutex->state == MUTEX_UNLOCKED)
        {
            mutex->state = MUTEX_LOCKED;
            mutex->owner = current;
            mutex->rcc   = 1;
            spin_unlock(&mutex->lock);
            return 0;
        }

        spin_unlock(&mutex->lock);
        scheduler_yield();
        cpu_relax();
    }
}

// 尝试获取互斥锁（非阻塞）
int mutex_trylock(mutex_t *mutex) {
    if (mutex == NULL) return -EINVAL;

    tcb_t current = get_current_task();

    spin_lock(&mutex->lock);

    if (mutex->state == MUTEX_DESTROYED)
    {
        spin_unlock(&mutex->lock);
        return -EINVAL;
    }

    if (mutex->owner == current)
    {
        if (mutex->rec)
        {
            mutex->rcc++;
            spin_unlock(&mutex->lock);
            return 0;
        }

        spin_unlock(&mutex->lock);
        return -EDEADLK;
    }

    if (mutex->state == MUTEX_UNLOCKED)
    {
        mutex->state = MUTEX_LOCKED;
        mutex->owner = current;
        mutex->rcc   = 1;
        spin_unlock(&mutex->lock);
        return 0;
    }

    spin_unlock(&mutex->lock);
    return -EBUSY;
}

// 释放互斥锁
int mutex_unlock(mutex_t *mutex) {
    if (mutex == NULL) return -EINVAL;

    tcb_t current = get_current_task();

    spin_lock(&mutex->lock);

    if (mutex->state == MUTEX_DESTROYED)
    {
        spin_unlock(&mutex->lock);
        return -EINVAL;
    }

    if (mutex->owner != current || mutex->state != MUTEX_LOCKED || mutex->rcc == 0)
    {
        spin_unlock(&mutex->lock);
        return -EPERM;
    }

    mutex->rcc--;

    if (mutex->rcc == 0)
    {
        mutex->owner = NULL;
        if (mutex->state != MUTEX_DESTROYED)
        {
            mutex->state = MUTEX_UNLOCKED;
        }
    }

    spin_unlock(&mutex->lock);
    return 0;
}

// 销毁互斥锁
int mutex_destroy(mutex_t *mutex) {
    if (mutex == NULL) return -EINVAL;

    spin_lock(&mutex->lock);

    if (mutex->state == MUTEX_LOCKED)
    {
        spin_unlock(&mutex->lock);
        return -EBUSY;
    }

    mutex->state = MUTEX_DESTROYED;
    lock_queue *wait_queue = mutex->wait_queue;
    mutex->wait_queue = NULL;
    mutex->owner = NULL;
    mutex->rcc   = 0;

    spin_unlock(&mutex->lock);

    if (wait_queue != NULL) queue_destroy(wait_queue);
    return 0;
}

// 检查锁是否被锁定
bool mutex_is_locked(mutex_t *mutex) {
    if (mutex == NULL) {
        return false;
    }
    
    spin_lock(&mutex->lock);
    bool locked = (mutex->state == MUTEX_LOCKED);
    spin_unlock(&mutex->lock);
    
    return locked;
}

// 获取锁的当前持有者
tcb_t mutex_get_owner(mutex_t *mutex) {
    if (mutex == NULL) {
        return NULL;
    }
    
    spin_lock(&mutex->lock);
    tcb_t owner = mutex->owner;
    spin_unlock(&mutex->lock);
    
    return owner;
}
