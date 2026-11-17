// SPDX-License-Identifier: Apache-2.0
// Anonymous event counters use the existing VFS/FD retirement contracts. The
// counter mutex may fault user pages; it is never held by poll or across sleep.
#include <task/eventfd.h>
#include <syscall/fd_lifetime.h>
#include <syscall/signal.h>
#include <task/poll.h>
#include <task/scheduler.h>
#include <mm/uaccess.h>
#include <mutex.h>

namespace {
constexpr int kSemaphore = 1;
constexpr uint64_t kMaximum = ~uint64_t(0) - 1;
int eventfd_fsid = 0;
mutex_t registration_lock = {SPIN_INIT, MUTEX_UNLOCKED, NULL, NULL, 0, true};

struct EventCounter
{
    mutex_t lock;
    uint64_t value;
    bool semaphore;
};

void counter_close(void *file)
{
    auto *counter = static_cast<EventCounter *>(file);
    mutex_destroy(&counter->lock);
    free(file);
}

errno_t counter_stat(void *, vfs_node_t node)
{
    node->size = ~uint64_t(0);
    return EOK;
}

errno_t counter_poll(void *file, size_t events)
{
    auto *counter = static_cast<EventCounter *>(file);
    uint64_t value = __atomic_load_n(&counter->value, __ATOMIC_ACQUIRE);
    return ((value ? EPOLLIN : 0) | (value < kMaximum ? EPOLLOUT : 0)) & events;
}

// The syscall boundary performs the exact eight-byte user copy transaction.
// Generic filesystem reads must not consume a counter before an EFAULT.
size_t counter_invalid_read(void *, void *, size_t, size_t)
{
    return size_t(-EINVAL);
}

size_t counter_invalid_write(void *, const void *, size_t, size_t)
{
    return size_t(-EINVAL);
}

errno_t counter_invalid_mount(const char *, vfs_node_t)
{
    return -ENODEV;
}

size_t counter_invalid_link(vfs_node_t, void *, size_t, size_t)
{
    return size_t(-EINVAL);
}

errno_t counter_invalid_mk(void *, const char *, vfs_node_t)
{
    return -EINVAL;
}

errno_t counter_invalid_ioctl(void *, size_t, void *)
{
    return -ENOTTY;
}

errno_t counter_invalid_rename(void *, const char *)
{
    return -EINVAL;
}

vfs_callback counter_callbacks = {
    .mount = counter_invalid_mount,
    .unmount = (vfs_unmount_t)empty,
    .open = (vfs_open_t)empty,
    .close = counter_close,
    .read = counter_invalid_read,
    .write = counter_invalid_write,
    .readlink = counter_invalid_link,
    .mkdir = counter_invalid_mk,
    .mkfile = counter_invalid_mk,
    .link = counter_invalid_mk,
    .symlink = counter_invalid_mk,
    .stat = counter_stat,
    .ioctl = counter_invalid_ioctl,
    .dup = (vfs_dup_t)empty,
    .poll = counter_poll,
    .map = (vfs_mapfile_t)empty,
    .resize = (vfs_resize_t)empty,
    .del = (vfs_del_t)empty,
    .rename = counter_invalid_rename,
};
}

bool eventfd_is_node(vfs_node_t node)
{
    int id = __atomic_load_n(&eventfd_fsid, __ATOMIC_ACQUIRE);
    return id && node && node->fsid == id;
}

int eventfd_create(uint32_t initial, int flags)
{
    if (flags & ~(kSemaphore | O_NONBLOCK | O_CLOEXEC)) return -EINVAL;
    mutex_lock(&registration_lock);
    int id = eventfd_fsid;
    int error = id ? 0 : vfs_regist_checked("eventfd", &counter_callbacks, -1, 0, &id);
    if (!error) __atomic_store_n(&eventfd_fsid, id, __ATOMIC_RELEASE);
    mutex_unlock(&registration_lock);
    if (error) return error;

    vfs_node_t node = vfs_node_alloc(NULL, "anon_eventfd");
    auto *counter = static_cast<EventCounter *>(calloc(1, sizeof(EventCounter)));
    auto *handle = static_cast<fd_file_handle *>(calloc(1, sizeof(fd_file_handle)));
    if (!node || !counter || !handle)
    {
        if (node) { free(node->name); free(node); }
        free(counter);
        free(handle);
        return -ENOMEM;
    }
    counter->lock = {SPIN_INIT, MUTEX_UNLOCKED, NULL, NULL, 0, true};
    counter->value = initial;
    counter->semaphore = flags & kSemaphore;
    // Detached deleted nodes already have final-reference VFS destruction.
    // No new public vnode type or module ABI field is required.
    node->type = file_stream | file_delete;
    node->fsid = id;
    node->handle = counter;
    node->size = ~uint64_t(0);
    node->flags = handle->flags = O_RDWR | (flags & (O_NONBLOCK | O_CLOEXEC));
    handle->node = node;
    int fd = fd_install_handle(get_current_task()->parent_group, handle, 0, false);
    if (fd < 0) fd_retire_handle(handle);
    return fd;
}

int64_t eventfd_read_user(fd_file_handle *handle, void *buffer, size_t size)
{
    if (size < sizeof(uint64_t)) return -EINVAL;
    auto *counter = static_cast<EventCounter *>(handle->node->handle);
    while (true)
    {
        mutex_lock(&counter->lock);
        uint64_t value = __atomic_load_n(&counter->value, __ATOMIC_RELAXED);
        if (value)
        {
            uint64_t result = counter->semaphore ? 1 : value;
            if (!copy_to_user_pagedir(get_current_task()->parent_group->pagedir, buffer, &result, sizeof(result)))
            {
                mutex_unlock(&counter->lock);
                return -EFAULT;
            }
            __atomic_store_n(&counter->value, value - result, __ATOMIC_RELEASE);
            mutex_unlock(&counter->lock);
            return sizeof(result);
        }
        mutex_unlock(&counter->lock);
        if (__atomic_load_n(&handle->node->flags, __ATOMIC_RELAXED) & O_NONBLOCK) return -EAGAIN;
        if (signals_pending_quick(get_current_task())) return -EINTR;
        scheduler_sleep_ns(1000000ULL);
    }
}

int64_t eventfd_write_user(fd_file_handle *handle, const void *buffer, size_t size)
{
    if (size < sizeof(uint64_t)) return -EINVAL;
    uint64_t increment;
    if (!copy_from_user_pagedir(get_current_task()->parent_group->pagedir, &increment, buffer, sizeof(increment)))
        return -EFAULT;
    if (increment == ~uint64_t(0)) return -EINVAL;
    auto *counter = static_cast<EventCounter *>(handle->node->handle);
    while (true)
    {
        mutex_lock(&counter->lock);
        uint64_t value = __atomic_load_n(&counter->value, __ATOMIC_RELAXED);
        if (increment <= kMaximum - value)
        {
            __atomic_store_n(&counter->value, value + increment, __ATOMIC_RELEASE);
            mutex_unlock(&counter->lock);
            return sizeof(increment);
        }
        mutex_unlock(&counter->lock);
        if (__atomic_load_n(&handle->node->flags, __ATOMIC_RELAXED) & O_NONBLOCK) return -EAGAIN;
        if (signals_pending_quick(get_current_task())) return -EINTR;
        scheduler_sleep_ns(1000000ULL);
    }
}
