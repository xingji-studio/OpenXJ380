extern "C" {
#include "../../../include/proto.hpp"
#include "../../../include/task/pcb.h"
#include "../../../include/task/scheduler.h"
#include "lwip/err.h"
#include "lwip/opt.h"
#include "lwip/sio.h"
#include "lwip/sys.h"
}

int errno_ = 0;

namespace {

volatile int g_core_guard = 0;
void        *g_core_owner = nullptr;
u32_t        g_core_depth = 0;
sys_prot_t   g_core_flags = 0;

static inline void *current_thread()
{
    return reinterpret_cast<void *>(get_current_task());
}

static inline sys_prot_t read_flags()
{
    sys_prot_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags) : : "memory");
    return flags;
}

static inline void restore_flags(sys_prot_t flags)
{
    asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

static inline void acquire(volatile int *guard)
{
    while (!__sync_bool_compare_and_swap(guard, 0, 1)) {
        scheduler_yield();
        asm volatile("pause" ::: "memory");
    }
}

static inline bool try_acquire(volatile int *guard)
{
    return __sync_bool_compare_and_swap(guard, 0, 1);
}

static inline void release(volatile int *guard)
{
    __sync_lock_release(guard);
}

static inline u32_t elapsed(u32_t since)
{
    return static_cast<u32_t>(sys_now() - since);
}

static bool take_message(sys_mbox_t *mbox, void **message)
{
    bool available = false;
    acquire(&mbox->guard);
    if (mbox->valid && mbox->count != 0) {
        if (message != nullptr) {
            *message = mbox->entries[mbox->head];
        }
        mbox->head = (mbox->head + 1U) % mbox->capacity;
        mbox->count--;
        available = true;
    }
    release(&mbox->guard);
    return available;
}

}

extern "C" {

void sys_init(void)
{
}

sys_prot_t sys_arch_protect(void)
{
    void *owner = current_thread();
    if (g_core_owner == owner && g_core_depth != 0) {
        g_core_depth++;
        return g_core_flags;
    }
    for (;;) {
        sys_prot_t flags = read_flags();
        asm volatile("cli" ::: "memory");
        if (try_acquire(&g_core_guard)) {
            g_core_owner = owner;
            g_core_depth = 1;
            g_core_flags = flags;
            return flags;
        }
        restore_flags(flags);
        scheduler_yield();
    }
}

void sys_arch_unprotect(sys_prot_t value)
{
    if (g_core_owner != current_thread() || g_core_depth == 0) {
        return;
    }
    if (--g_core_depth == 0) {
        g_core_owner = nullptr;
        g_core_flags = 0;
        release(&g_core_guard);
        restore_flags(value);
    }
}

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    if (mutex == nullptr) {
        return ERR_ARG;
    }
    *mutex = {};
    mutex->valid = true;
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    if (mutex == nullptr || !mutex->valid) {
        return;
    }
    void *owner = current_thread();
    if (mutex->owner == owner) {
        mutex->depth++;
        return;
    }
    acquire(&mutex->held);
    if (mutex->valid) {
        mutex->owner = owner;
        mutex->depth = 1;
    } else {
        release(&mutex->held);
    }
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    if (mutex == nullptr || !mutex->valid || mutex->owner != current_thread() || mutex->depth == 0) {
        return;
    }
    if (--mutex->depth == 0) {
        mutex->owner = nullptr;
        release(&mutex->held);
    }
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    if (mutex != nullptr) {
        mutex->valid = false;
        mutex->owner = nullptr;
        mutex->depth = 0;
        release(&mutex->held);
    }
}

int sys_mutex_valid(sys_mutex_t *mutex)
{
    return mutex != nullptr && mutex->valid;
}

void sys_mutex_set_invalid(sys_mutex_t *mutex)
{
    if (mutex != nullptr) {
        *mutex = {};
    }
}

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    if (sem == nullptr) {
        return ERR_ARG;
    }
    *sem = {};
    sem->count = count;
    sem->valid = true;
    return ERR_OK;
}

void sys_sem_signal(sys_sem_t *sem)
{
    if (sem == nullptr || !sem->valid) {
        return;
    }
    acquire(&sem->guard);
    sem->count++;
    release(&sem->guard);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    if (sem == nullptr) {
        return SYS_ARCH_TIMEOUT;
    }
    u32_t start = sys_now();
    for (;;) {
        acquire(&sem->guard);
        bool taken = sem->valid && sem->count != 0;
        if (taken) {
            sem->count--;
        }
        bool valid = sem->valid;
        release(&sem->guard);
        if (taken) {
            return elapsed(start);
        }
        if (!valid || (timeout != 0 && elapsed(start) >= timeout)) {
            return SYS_ARCH_TIMEOUT;
        }
        scheduler_yield();
    }
}

void sys_sem_free(sys_sem_t *sem)
{
    if (sem != nullptr) {
        acquire(&sem->guard);
        sem->valid = false;
        sem->count = 0;
        release(&sem->guard);
    }
}

void sys_sem_set_invalid(sys_sem_t *sem)
{
    if (sem != nullptr) {
        *sem = {};
    }
}

int sys_sem_valid(sys_sem_t *sem)
{
    return sem != nullptr && sem->valid;
}

u32_t sys_now(void)
{
    return static_cast<u32_t>(nanoTime() / 1000000ULL);
}

sys_thread_t sys_thread_new(const char *name, void (*entry)(void *), void *argument, int stack_size, int priority)
{
    (void)stack_size;
    (void)priority;
    return create_kernel_thread(reinterpret_cast<void *>(entry), argument, const_cast<char *>(name), nullptr);
}

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    if (mbox == nullptr) {
        return ERR_ARG;
    }
    *mbox = {};
    mbox->capacity = static_cast<u32_t>(size > 0 ? size : TCPIP_MBOX_SIZE);
    mbox->entries = static_cast<void **>(calloc(mbox->capacity, sizeof(void *)));
    if (mbox->entries == nullptr) {
        return ERR_MEM;
    }
    mbox->valid = true;
    return ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    if (mbox == nullptr) {
        return;
    }
    acquire(&mbox->guard);
    mbox->valid = false;
    void **entries = mbox->entries;
    mbox->entries = nullptr;
    mbox->count = 0;
    release(&mbox->guard);
    free(entries);
}

void sys_mbox_set_invalid(sys_mbox_t *mbox)
{
    if (mbox != nullptr) {
        *mbox = {};
    }
}

int sys_mbox_valid(sys_mbox_t *mbox)
{
    return mbox != nullptr && mbox->valid;
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *message)
{
    if (mbox == nullptr || !mbox->valid || !try_acquire(&mbox->guard)) {
        return ERR_MEM;
    }
    if (!mbox->valid || mbox->count == mbox->capacity) {
        release(&mbox->guard);
        return ERR_MEM;
    }
    mbox->entries[mbox->tail] = message;
    mbox->tail = (mbox->tail + 1U) % mbox->capacity;
    mbox->count++;
    release(&mbox->guard);
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *message)
{
    return sys_mbox_trypost(mbox, message);
}

void sys_mbox_post(sys_mbox_t *mbox, void *message)
{
    while (sys_mbox_trypost(mbox, message) != ERR_OK) {
        scheduler_yield();
    }
}

void sys_mbox_post_unsafe(sys_mbox_t *mbox, void *message)
{
    sys_mbox_post(mbox, message);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **message, u32_t timeout)
{
    if (mbox == nullptr) {
        return SYS_ARCH_TIMEOUT;
    }
    u32_t start = sys_now();
    while (mbox->valid) {
        if (take_message(mbox, message)) {
            return elapsed(start);
        }
        if (timeout != 0 && elapsed(start) >= timeout) {
            return SYS_ARCH_TIMEOUT;
        }
        scheduler_yield();
    }
    return SYS_ARCH_TIMEOUT;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **message)
{
    return mbox != nullptr && take_message(mbox, message) ? 0 : SYS_MBOX_EMPTY;
}

sio_fd_t sio_open(u8_t device)
{
    (void)device;
    return nullptr;
}

u32_t sio_write(sio_fd_t fd, const u8_t *data, u32_t size)
{
    (void)fd;
    (void)data;
    return size;
}

u32_t sio_read(sio_fd_t fd, u8_t *data, u32_t size)
{
    (void)fd;
    (void)data;
    (void)size;
    return 0;
}

void sio_send(u8_t byte, sio_fd_t fd)
{
    (void)byte;
    (void)fd;
}

u8_t sio_recv(sio_fd_t fd)
{
    (void)fd;
    return 0;
}

u32_t sio_tryread(sio_fd_t fd, u8_t *data, u32_t size)
{
    return sio_read(fd, data, size);
}

}
