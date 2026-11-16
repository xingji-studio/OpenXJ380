#include <task/futex.h>
#include <task/pcb.h>
#include <task/scheduler.h>
#include <cpu/lock.h>
#include <mm/uaccess.h>
#include <mm/hhdm.h>
#include <mm/lazyalloc.h>
#include <errno.h>
#include <proto.hpp>

namespace
{
struct Key
{
    page_directory_t *directory;
    uint64_t address;
};

struct Waiter
{
    Key key;
    tcb_t task;
    uint32_t bitset, notified;
    Waiter *next;
};

struct Bucket
{
    spin_t lock;
    Waiter *head;
};

static Bucket buckets[64];

static Bucket &bucket_for(const Key &key)
{
    return buckets[((key.address >> 2) ^ ((uint64_t)key.directory >> 12)) % 64];
}

static bool equal(const Key &a, const Key &b)
{
    return a.directory == b.directory && a.address == b.address;
}

// The memory-operation mutex pins the translation through comparison and
// publication. User-copy may allocate/fault, so it runs before bucket locks.
static bool prepare_key(page_directory_t *directory, uint32_t *address, bool private_key,
                         Key &key, uint32_t *&word)
{
    if (!directory || !address || ((uint64_t)address & 3) ||
        !user_range_mapped(directory, address, sizeof(*address))) return false;
    uint64_t physical = translate_address(directory, (uint64_t)address);
    if (!physical) return false;
    key = {private_key ? directory : NULL, private_key ? (uint64_t)address : physical};
    word = (uint32_t *)phys_to_virt(physical);
    return word != NULL;
}

static void remove_locked(Bucket &bucket, Waiter *waiter)
{
    Waiter **link = &bucket.head;
    while (*link && *link != waiter) link = &(*link)->next;
    if (*link) *link = waiter->next;
}
}

int futex_wait_user(page_directory_t *directory, uint32_t *address, uint32_t expected,
                    bool private_key, uint32_t bitset, uint64_t deadline)
{
    if (!bitset || ((uint64_t)address & 3)) return -EINVAL;
    if (!scheduler_is_enabled()) return -EWOULDBLOCK;
    Waiter *waiter = (Waiter *)calloc(1, sizeof(Waiter));
    if (!waiter) return -ENOMEM;
    waiter->task = get_current_task();
    waiter->bitset = bitset;
    uint32_t *word = NULL;
    mm_op_lock_acquire();
    if (!prepare_key(directory, address, private_key, waiter->key, word))
    {
        mm_op_lock_release();
        free(waiter);
        return -EFAULT;
    }
    Bucket &bucket = bucket_for(waiter->key);
    spin_lock(&bucket.lock);
    if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != expected)
    {
        spin_unlock(&bucket.lock);
        mm_op_lock_release();
        free(waiter);
        return -EAGAIN;
    }
    waiter->next = bucket.head;
    bucket.head = waiter;
    spin_unlock(&bucket.lock);
    mm_op_lock_release();

    int result = 0;
    for (;;)
    {
        if (__atomic_load_n(&waiter->notified, __ATOMIC_ACQUIRE)) break;
        if (signals_pending_quick(waiter->task)) { result = -EINTR; break; }
        uint64_t now = bootNanoTime();
        if (deadline != ~0ULL && now >= deadline) { result = -ETIMEDOUT; break; }
        scheduler_wait_event(&waiter->notified, deadline == ~0ULL ? ~0ULL : deadline - now);
    }
    spin_lock(&bucket.lock);
    if (waiter->notified) result = 0;
    remove_locked(bucket, waiter);
    spin_unlock(&bucket.lock);
    free(waiter);
    return result;
}

int futex_wake_user(page_directory_t *directory, uint32_t *address, bool private_key,
                    uint32_t count, uint32_t bitset)
{
    if (!bitset || ((uint64_t)address & 3)) return -EINVAL;
    Key key;
    uint32_t *word = NULL;
    mm_op_lock_acquire();
    if (!prepare_key(directory, address, private_key, key, word))
    {
        mm_op_lock_release();
        return -EFAULT;
    }
    Bucket &bucket = bucket_for(key);
    uint32_t woken = 0;
    spin_lock(&bucket.lock);
    for (Waiter *waiter = bucket.head; waiter && woken < count; waiter = waiter->next)
    {
        TaskStatus status = __atomic_load_n(&waiter->task->status, __ATOMIC_ACQUIRE);
        if (status != DEATH && status != OUT && equal(waiter->key, key) &&
            (waiter->bitset & bitset) && !waiter->notified)
        {
            __atomic_store_n(&waiter->notified, 1, __ATOMIC_RELEASE);
            // Retain the bucket lock across the short run-queue update. Exit
            // cleanup cannot retire this waiter/TCB until wake has finished.
            scheduler_wake_task(waiter->task);
            ++woken;
        }
    }
    spin_unlock(&bucket.lock);
    mm_op_lock_release();
    return woken;
}

void futex_thread_exit(tcb_t task)
{
    for (Bucket &bucket : buckets)
    {
        Waiter *retired = NULL;
        spin_lock(&bucket.lock);
        Waiter **link = &bucket.head;
        while (*link)
        {
            Waiter *waiter = *link;
            if (waiter->task != task) { link = &waiter->next; continue; }
            *link = waiter->next;
            waiter->next = retired;
            retired = waiter;
        }
        spin_unlock(&bucket.lock);
        while (retired)
        {
            Waiter *next = retired->next;
            free(retired);
            retired = next;
        }
    }
}
