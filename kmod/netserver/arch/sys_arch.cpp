extern "C" {
#include "../../../include/proto.hpp"
#include "../../../include/rtc.h"
#include "../../../include/task/pcb.h"
#include "../../../include/task/scheduler.h"
#include "lwip/arch.h"
#include "lwipopts.h"
#include "sys_arch.h"
#include <lwip/arch.h>
#include <lwip/debug.h>
#include <lwip/opt.h>
#include <lwip/stats.h>
#include <lwip/sys.h>
}

int errno_ = 0;

namespace {

static volatile long g_protect_lock = 0;
static void *g_protect_owner = nullptr;
static u32_t g_protect_depth = 0;
static sys_prot_t g_protect_flags = 0;

static inline void *sys_current_task()
{
    return reinterpret_cast<void *>(get_current_task());
}

static inline sys_prot_t sys_read_rflags()
{
    sys_prot_t flags = 0;
    asm volatile("pushfq\n\t"
                 "pop %0"
                 : "=r"(flags)
                 :
                 : "memory");
    return flags;
}

static inline void sys_restore_rflags(sys_prot_t flags)
{
    asm volatile("push %0\n\t"
                 "popfq"
                 :
                 : "r"(flags)
                 : "memory", "cc");
}

static inline void sys_cpu_relax()
{
    __asm__ volatile("pause" ::: "memory");
}

static inline u32_t sys_elapsed_since(u32_t start)
{
    return static_cast<u32_t>(sys_now() - start);
}

static inline bool sys_timeout_expired(u32_t start, u32_t timeout)
{
    return timeout != 0 && sys_elapsed_since(start) >= timeout;
}

static inline bool sys_mbox_is_empty(const sys_mbox_t *mbox)
{
    return mbox->ptrRead == mbox->ptrWrite;
}

static inline bool sys_mbox_is_full(const sys_mbox_t *mbox)
{
    return ((mbox->ptrWrite + 1U) % static_cast<u32_t>(mbox->size)) == mbox->ptrRead;
}

static inline void sys_mbox_post_locked(sys_mbox_t *mbox, void *msg)
{
    mbox->msges[mbox->ptrWrite] = msg;
    mbox->ptrWrite = (mbox->ptrWrite + 1U) % static_cast<u32_t>(mbox->size);
}

static inline bool sys_object_is_alive(bool invalid_flag)
{
    return !invalid_flag;
}

} // namespace

extern "C" {

void sys_init(void)
{
}

sys_prot_t sys_arch_protect(void)
{
    void *owner = sys_current_task();
    if (g_protect_owner == owner && g_protect_depth != 0) {
        g_protect_depth++;
        return g_protect_flags;
    }

    for (;;) {
        sys_prot_t flags = sys_read_rflags();
        __asm__ volatile("cli" ::: "memory");

        if (__sync_bool_compare_and_swap(&g_protect_lock, 0, 1)) {
            g_protect_owner = owner;
            g_protect_depth = 1;
            g_protect_flags = flags;
            return flags;
        }

        sys_restore_rflags(flags);
        scheduler_yield();
        sys_cpu_relax();
    }
}

void sys_arch_unprotect(sys_prot_t pval)
{
    (void)pval;

    void *owner = sys_current_task();
    if (g_protect_owner != owner || g_protect_depth == 0) {
        return;
    }

    g_protect_depth--;
    if (g_protect_depth != 0) {
        return;
    }

    sys_prot_t flags = g_protect_flags;
    g_protect_owner = nullptr;
    g_protect_flags = 0;
    __sync_lock_release(&g_protect_lock);
    sys_restore_rflags(flags);
}

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    if (mutex == nullptr) {
        return ERR_ARG;
    }

    mutex->state = 0;
    mutex->owner = nullptr;
    mutex->depth = 0;
    mutex->valid = true;
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    if (mutex == nullptr) {
        return;
    }

    void *current = sys_current_task();
    u32_t start = sys_now();
    bool waiting_logged = false;
    for (;;) {
        if (!mutex->valid) {
            return;
        }

        if (mutex->owner == current) {
            mutex->depth++;
            return;
        }

        if (__sync_bool_compare_and_swap(&mutex->state, 0, 1)) {
            mutex->owner = current;
            mutex->depth = 1;
            return;
        }

        if (!waiting_logged && sys_elapsed_since(start) >= 1000) {
            waiting_logged = true;
            printk("netserver: sys_mutex_lock waiting mutex=%p owner=%p current=%p state=%ld depth=%u\n",
                   mutex, mutex->owner, current, mutex->state, (unsigned int)mutex->depth);
        }
        scheduler_yield();
        sys_cpu_relax();
    }
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    if (mutex == nullptr || !mutex->valid) {
        return;
    }

    if (mutex->owner != sys_current_task() || mutex->depth == 0) {
        return;
    }

    mutex->depth--;
    if (mutex->depth != 0) {
        return;
    }

    mutex->owner = nullptr;
    __sync_lock_release(&mutex->state);
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    if (mutex == nullptr) {
        return;
    }

    mutex->valid = false;
    mutex->owner = nullptr;
    mutex->depth = 0;
    __sync_lock_release(&mutex->state);
}

int sys_mutex_valid(sys_mutex_t *mutex)
{
    return mutex != nullptr && mutex->valid;
}

void sys_mutex_set_invalid(sys_mutex_t *mutex)
{
    sys_mutex_free(mutex);
}

err_t sys_sem_new(sys_sem_t *sem, uint8_t cnt)
{
    if (sem == nullptr) {
        return ERR_ARG;
    }

    sem->invalid = false;
    sem->lock = SPIN_INIT;
    sem->cnt = cnt;
    return ERR_OK;
}

void sys_sem_signal(sys_sem_t *sem)
{
    if (sem == nullptr || sem->invalid) {
        return;
    }

    spin_lock(&sem->lock);
    sem->cnt++;
    spin_unlock(&sem->lock);
}

uint32_t sys_arch_sem_wait(sys_sem_t *sem, uint32_t timeout)
{
    if (sem == nullptr) {
        return SYS_ARCH_TIMEOUT;
    }

    u32_t start = sys_now();
    for (;;) {
        spin_lock(&sem->lock);
        if (!sem->invalid && sem->cnt > 0) {
            sem->cnt--;
            spin_unlock(&sem->lock);
            return sys_elapsed_since(start);
        }

        bool invalid = sem->invalid;
        spin_unlock(&sem->lock);
        if (invalid || sys_timeout_expired(start, timeout)) {
            return SYS_ARCH_TIMEOUT;
        }

        scheduler_yield();
    }
}

void sys_sem_free(sys_sem_t *sem)
{
    if (sem == nullptr) {
        return;
    }

    spin_lock(&sem->lock);
    sem->invalid = true;
    sem->cnt = 0;
    spin_unlock(&sem->lock);
}

void sys_sem_set_invalid(sys_sem_t *sem)
{
    if (sem == nullptr) {
        return;
    }

    sem->lock = SPIN_INIT;
    sem->cnt = 0;
    sem->invalid = true;
}

int sys_sem_valid(sys_sem_t *sem)
{
    return sem != nullptr && sys_object_is_alive(sem->invalid);
}

uint32_t sys_now(void)
{
    return static_cast<u32_t>(nanoTime() / 1000000ULL);
}

sys_thread_t sys_thread_new(const char *pcName,
                            void (*pxThread)(void *pvParameters), void *pvArg,
                            int iStackSize, int iPriority)
{
    (void)iStackSize;
    (void)iPriority;
    return create_kernel_thread(reinterpret_cast<void *>(pxThread), pvArg,
                                const_cast<char *>(pcName), nullptr);
}

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    if (mbox == nullptr) {
        return ERR_ARG;
    }

    if (size <= 0) {
        size = TCPIP_MBOX_SIZE;
    }

    memset(mbox, 0, sizeof(*mbox));
    mbox->invalid = false;
    mbox->lock = SPIN_INIT;
    mbox->size = size + 1;
    mbox->msges = static_cast<void **>(calloc(static_cast<size_t>(mbox->size),
                                              sizeof(mbox->msges[0])));
    return mbox->msges != nullptr ? ERR_OK : ERR_MEM;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    if (mbox == nullptr) {
        return;
    }

    spin_lock(&mbox->lock);
    mbox->invalid = true;
    free(mbox->msges);
    mbox->msges = nullptr;
    mbox->ptrRead = 0;
    mbox->ptrWrite = 0;
    spin_unlock(&mbox->lock);
}

void sys_mbox_set_invalid(sys_mbox_t *mbox)
{
    if (mbox == nullptr) {
        return;
    }

    mbox->lock = SPIN_INIT;
    mbox->msges = nullptr;
    mbox->size = 0;
    mbox->ptrRead = 0;
    mbox->ptrWrite = 0;
    mbox->invalid = true;
}

int sys_mbox_valid(sys_mbox_t *mbox)
{
    if (mbox == nullptr) {
        return 0;
    }

    spin_lock(&mbox->lock);
    int valid = !mbox->invalid && mbox->msges != nullptr;
    spin_unlock(&mbox->lock);
    return valid;
}

void sys_mbox_post_unsafe(sys_mbox_t *mbox, void *msg)
{
    if (mbox == nullptr || mbox->msges == nullptr) {
        return;
    }

    sys_mbox_post_locked(mbox, msg);
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    if (mbox == nullptr) {
        return;
    }

    for (;;) {
        spin_lock(&mbox->lock);
        if (mbox->invalid || mbox->msges == nullptr) {
            spin_unlock(&mbox->lock);
            return;
        }

        if (!sys_mbox_is_full(mbox)) {
            sys_mbox_post_locked(mbox, msg);
            spin_unlock(&mbox->lock);
            return;
        }

        spin_unlock(&mbox->lock);
        scheduler_yield();
    }
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    if (mbox == nullptr) {
        return ERR_ARG;
    }

    spin_lock(&mbox->lock);
    if (mbox->invalid || mbox->msges == nullptr) {
        spin_unlock(&mbox->lock);
        return ERR_ARG;
    }

    if (sys_mbox_is_full(mbox)) {
        spin_unlock(&mbox->lock);
        return ERR_MEM;
    }

    sys_mbox_post_locked(mbox, msg);
    spin_unlock(&mbox->lock);
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    return sys_mbox_trypost(mbox, msg);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    if (mbox == nullptr) {
        if (msg != nullptr) {
            *msg = nullptr;
        }
        return SYS_ARCH_TIMEOUT;
    }

    u32_t start = sys_now();
    for (;;) {
        spin_lock(&mbox->lock);
        if (!sys_mbox_is_empty(mbox)) {
            if (msg != nullptr) {
                *msg = mbox->msges[mbox->ptrRead];
            }
            mbox->ptrRead = (mbox->ptrRead + 1U) % static_cast<u32_t>(mbox->size);
            spin_unlock(&mbox->lock);
            return sys_elapsed_since(start);
        }

        bool invalid = mbox->invalid || mbox->msges == nullptr;
        spin_unlock(&mbox->lock);
        if (invalid || sys_timeout_expired(start, timeout)) {
            if (msg != nullptr) {
                *msg = nullptr;
            }
            return SYS_ARCH_TIMEOUT;
        }

        scheduler_yield();
    }
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    if (mbox == nullptr) {
        if (msg != nullptr) {
            *msg = nullptr;
        }
        return SYS_MBOX_EMPTY;
    }

    spin_lock(&mbox->lock);
    if (mbox->invalid || mbox->msges == nullptr || sys_mbox_is_empty(mbox)) {
        spin_unlock(&mbox->lock);
        if (msg != nullptr) {
            *msg = nullptr;
        }
        return SYS_MBOX_EMPTY;
    }

    if (msg != nullptr) {
        *msg = mbox->msges[mbox->ptrRead];
    }
    mbox->ptrRead = (mbox->ptrRead + 1U) % static_cast<u32_t>(mbox->size);
    spin_unlock(&mbox->lock);
    return ERR_OK;
}

void *sio_open(u8_t devnum)
{
    (void)devnum;
    return nullptr;
}

u32_t sio_write(void *fd, const u8_t *data, u32_t len)
{
    (void)fd;
    (void)data;
    (void)len;
    return 0;
}

void sio_send(u8_t c, void *fd)
{
    (void)c;
    (void)fd;
}

u8_t sio_recv(void *fd)
{
    (void)fd;
    return 0;
}

u32_t sio_read(void *fd, u8_t *data, u32_t len)
{
    (void)fd;
    (void)data;
    (void)len;
    return 0;
}

u32_t sio_tryread(void *fd, u8_t *data, u32_t len)
{
    (void)fd;
    (void)data;
    (void)len;
    return 0;
}

}
