#include "mutex.h"
#include "errno.h"
#include "task/scheduler.h"
#include "cpu/lock.h"
#include "mm/heap.h"
#include "krlibc.h"
#include <proto.hpp>
#include <smp/smp.h>

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

/*
 * The caller holds the mutex metadata lock, and its process-exit hook must
 * clear this owner before freeing the TCB. A killed thread may still be
 * finishing a kernel call on another CPU; never steal its lock until it has
 * actually stopped executing. Terminal tasks cannot become runnable again.
 */
static bool mutex_owner_has_stopped_locked(tcb_t owner)
{
    if (owner == NULL) return false;
    TaskStatus status = __atomic_load_n(&owner->status, __ATOMIC_ACQUIRE);
    if (status != DEATH && status != OUT) return false;

    return !scheduler_task_referenced(owner);
}

int mutex_lock_recoverable(mutex_t *mutex, bool *recovered)
{
    if (recovered != NULL) *recovered = false;
    for (;;)
    {
        int result = mutex_trylock(mutex);
        if (result != -EBUSY) return result;

        bool abandoned = false;
        spin_lock(&mutex->lock);
        if (mutex->state == MUTEX_LOCKED && mutex_owner_has_stopped_locked(mutex->owner))
        {
            // Transfer ownership atomically. Publishing UNLOCKED first would
            // let a second waiter enter without learning that state recovery
            // is required, before this recoverer can rebuild or poison it.
            mutex->owner = get_current_task();
            mutex->rcc = 1;
            abandoned = true;
            if (recovered != NULL) *recovered = true;
        }
        spin_unlock(&mutex->lock);

        if (abandoned) return 0;
        else
        {
            scheduler_yield();
            cpu_relax();
        }
    }
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

/*
 * Process teardown happens before a TCB is freed, which gives subsystems that
 * own process-scoped mutexes one last safe point to release an abandoned lock.
 * Do not release a lock held by a live thread: doing so would let two threads
 * mutate the protected state concurrently.
 */
bool mutex_abort_for_process(mutex_t *mutex, pcb_t process)
{
    if (mutex == NULL || process == NULL) return false;

    bool released = false;
    spin_lock(&mutex->lock);

    tcb_t owner = mutex->owner;
    if (mutex->state == MUTEX_LOCKED && owner != NULL && owner->parent_group == process &&
        mutex_owner_has_stopped_locked(owner))
    {
        mutex->owner = NULL;
        mutex->rcc = 0;
        mutex->state = MUTEX_UNLOCKED;
        released = true;
    }

    spin_unlock(&mutex->lock);
    return released;
}
