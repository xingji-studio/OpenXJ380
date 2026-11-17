// SPDX-License-Identifier: Apache-2.0
// First-party event backend. Watches own vnode pins; producers only publish to
// preallocated rings. Final FD retirement and detached watch disposal run in
// ordinary task context, outside both registry and VFS namespace spin locks.
#include <task/inotify.h>
#include <syscall/fd_lifetime.h>
#include <syscall/signal.h>
#include <task/poll.h>
#include <task/scheduler.h>
#include <mm/uaccess.h>
#include <mutex.h>
#include <ioctl.h>
#include <input_method.h>

namespace {
constexpr uint32_t kEvents = 0x00000fff;
constexpr uint32_t kOptions = IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK | IN_MASK_CREATE | IN_MASK_ADD | IN_ONESHOT;
constexpr unsigned kBuckets = 256, kNormalEvents = 256, kSlots = kNormalEvents + 1, kMaxWatches = 4096;
constexpr size_t kReadBytes = 4096;
spin_t notification_lock = SPIN_INIT;
mutex_t registration_lock = {SPIN_INIT, MUTEX_UNLOCKED, NULL, NULL, 0, true};
int inotify_fsid;
uint32_t move_cookie;

struct Identity
{
    vfs_node_t mount, fallback;
    uint64_t device, inode;
};
struct QueuedEvent
{
    InotifyEvent header;
    char name[256];
};
struct Instance;
struct Watch
{
    Identity key;
    vfs_node_t pin;
    Instance *instance;
    Watch *hash_next, *next, *retired_next;
    uint32_t mask;
    int wd;
};
struct Instance
{
    Watch *watches, *retired;
    unsigned watch_count, head, count;
    uint32_t next_wd;
    uint64_t read_generation;
    bool overflow;
    QueuedEvent events[kSlots];
};
Watch *watch_index[kBuckets];

Identity identity(vfs_node_t node)
{
    return {node->root, node->inode ? NULL : node, node->dev, node->inode};
}

bool equal(const Identity &a, const Identity &b)
{
    return a.mount == b.mount && a.fallback == b.fallback && a.device == b.device && a.inode == b.inode;
}

unsigned bucket(const Identity &key)
{
    uint64_t hash = uintptr_t(key.mount) ^ uintptr_t(key.fallback) ^ key.device ^ key.inode;
    return unsigned(hash ^ (hash >> 16) ^ (hash >> 32)) & (kBuckets - 1);
}

void enqueue(Instance *instance, int wd, uint32_t mask, uint32_t cookie, const char *name)
{
    unsigned length = 0;
    if (name) while (length < 255 && name[length]) ++length;
    unsigned padded = name ? (length + 4) & ~3U : 0;
    if (instance->count)
    {
        QueuedEvent &tail = instance->events[(instance->head + instance->count - 1) % kSlots];
        if (tail.header.wd == wd && tail.header.mask == mask && tail.header.cookie == cookie &&
            tail.header.len == padded && (!padded || !memcmp(tail.name, name, length + 1))) return;
    }
    if (instance->count >= kNormalEvents)
    {
        if (instance->overflow) return;
        instance->overflow = true;
        wd = -1;
        mask = IN_Q_OVERFLOW;
        cookie = padded = length = 0;
    }
    QueuedEvent &event = instance->events[(instance->head + instance->count++) % kSlots];
    event.header = {wd, mask, cookie, padded};
    if (padded)
    {
        memcpy(event.name, name, length);
        memset(event.name + length, 0, padded - length);
    }
}

void detach(Watch *watch)
{
    Watch **hashed = &watch_index[bucket(watch->key)];
    while (*hashed && *hashed != watch) hashed = &(*hashed)->hash_next;
    if (*hashed) *hashed = watch->hash_next;
    Instance *instance = watch->instance;
    Watch **entry = &instance->watches;
    while (*entry && *entry != watch) entry = &(*entry)->next;
    if (*entry) *entry = watch->next;
    --instance->watch_count;
    enqueue(instance, watch->wd, IN_IGNORED, 0, NULL);
    watch->retired_next = instance->retired;
    instance->retired = watch;
}

void publish(const Identity &key, uint32_t mask, uint32_t cookie, const char *name, bool unlinked = false)
{
    for (Watch *watch = watch_index[bucket(key)], *next; watch; watch = next)
    {
        next = watch->hash_next;
        if (!equal(watch->key, key) || !(watch->mask & mask & kEvents) ||
            (unlinked && (watch->mask & IN_EXCL_UNLINK))) continue;
        enqueue(watch->instance, watch->wd, mask, cookie, name);
        if (watch->mask & IN_ONESHOT) detach(watch);
    }
}

void destroy_watches(Watch *retired)
{
    while (retired)
    {
        Watch *next = retired->retired_next;
        vfs_close(retired->pin);
        free(retired);
        retired = next;
    }
}

void reap(Instance *instance)
{
    spin_lock(&notification_lock);
    Watch *retired = instance->retired;
    instance->retired = NULL;
    spin_unlock(&notification_lock);
    destroy_watches(retired);
}

void instance_close(void *file)
{
    auto *instance = static_cast<Instance *>(file);
    spin_lock(&notification_lock);
    while (instance->watches) detach(instance->watches);
    Watch *retired = instance->retired;
    spin_unlock(&notification_lock);
    destroy_watches(retired);
    free(instance);
}

errno_t instance_stat(void *, vfs_node_t node)
{
    node->size = ~uint64_t(0);
    return EOK;
}

errno_t instance_poll(void *file, size_t events)
{
    spin_lock(&notification_lock);
    bool ready = static_cast<Instance *>(file)->count != 0;
    spin_unlock(&notification_lock);
    return ready ? events & EPOLLIN : 0;
}

errno_t instance_ioctl(void *file, size_t request, void *buffer)
{
    if (request != FIONREAD) return -ENOTTY;
    if (!buffer) return -EFAULT;
    auto *instance = static_cast<Instance *>(file);
    int bytes = 0;
    spin_lock(&notification_lock);
    for (unsigned index = 0; index < instance->count; ++index)
        bytes += sizeof(InotifyEvent) + instance->events[(instance->head + index) % kSlots].header.len;
    spin_unlock(&notification_lock);
    // The ioctl syscall supplies a kernel bounce buffer.
    *static_cast<int *>(buffer) = bytes;
    return 0;
}

size_t invalid_read(void *, void *, size_t, size_t) { return size_t(-EINVAL); }
size_t invalid_write(void *, const void *, size_t, size_t) { return size_t(-EBADF); }
errno_t invalid_mount(const char *, vfs_node_t) { return -ENODEV; }
size_t invalid_link(vfs_node_t, void *, size_t, size_t) { return size_t(-EINVAL); }
errno_t invalid_mk(void *, const char *, vfs_node_t) { return -EINVAL; }
errno_t invalid_rename(void *, const char *) { return -EINVAL; }
vfs_callback inotify_callbacks = {
    .mount = invalid_mount, .unmount = (vfs_unmount_t)empty, .open = (vfs_open_t)empty, .close = instance_close,
    .read = invalid_read, .write = invalid_write, .readlink = invalid_link,
    .mkdir = invalid_mk, .mkfile = invalid_mk, .link = invalid_mk, .symlink = invalid_mk,
    .stat = instance_stat, .ioctl = instance_ioctl, .dup = (vfs_dup_t)empty, .poll = instance_poll,
    .map = (vfs_mapfile_t)empty, .resize = (vfs_resize_t)empty, .del = (vfs_del_t)empty, .rename = invalid_rename,
};
}

bool inotify_is_node(vfs_node_t node)
{
    int id = __atomic_load_n(&inotify_fsid, __ATOMIC_ACQUIRE);
    return id && node && node->fsid == id;
}

int inotify_create(int flags)
{
    if (flags & ~(O_NONBLOCK | O_CLOEXEC)) return -EINVAL;
    mutex_lock(&registration_lock);
    int id = inotify_fsid;
    int error = id ? 0 : vfs_regist_checked("inotify", &inotify_callbacks, -1, 0, &id);
    if (!error) __atomic_store_n(&inotify_fsid, id, __ATOMIC_RELEASE);
    mutex_unlock(&registration_lock);
    if (error) return error;
    vfs_node_t node = vfs_node_alloc(NULL, "anon_inotify");
    auto *instance = static_cast<Instance *>(calloc(1, sizeof(Instance)));
    auto *handle = static_cast<fd_file_handle *>(calloc(1, sizeof(fd_file_handle)));
    if (!node || !instance || !handle)
    {
        if (node) { free(node->name); free(node); }
        free(instance);
        free(handle);
        return -ENOMEM;
    }
    instance->next_wd = 1;
    node->type = file_stream | file_delete;
    node->fsid = id;
    node->handle = instance;
    node->size = ~uint64_t(0);
    node->flags = handle->flags = flags;
    handle->node = node;
    int fd = fd_install_handle(get_current_task()->parent_group, handle, 0, false);
    if (fd < 0) fd_retire_handle(handle);
    return fd;
}

int inotify_add(fd_file_handle *handle, vfs_node_t node, uint32_t mask)
{
    if (!handle || !inotify_is_node(handle->node)) return -EINVAL;
    if (!node) return -ENOENT;
    if (!(mask & kEvents) || (mask & ~(kEvents | kOptions)) ||
        ((mask & IN_MASK_CREATE) && (mask & IN_MASK_ADD))) return -EINVAL;
    if ((mask & IN_ONLYDIR) && !(node->type & file_dir)) return -ENOTDIR;
    if (!vfs_check_access(node, NULL, IME_FILE_READ)) return -vfs_last_error();
    auto *instance = static_cast<Instance *>(handle->node->handle);
    reap(instance);
    auto *fresh = static_cast<Watch *>(calloc(1, sizeof(Watch)));
    if (!fresh) return -ENOMEM;
    int result = -ENOENT;
    vfs_child_lock();
    Identity key = identity(node);
    spin_lock(&notification_lock);
    Watch *existing = watch_index[bucket(key)];
    while (existing && (!equal(existing->key, key) || existing->instance != instance)) existing = existing->hash_next;
    if (vfs_node_retiring_locked(node) || (node->type & (file_delete | file_unlinked))) result = -ENOENT;
    else if (existing)
    {
        result = mask & IN_MASK_CREATE ? -EEXIST : existing->wd;
        if (result >= 0) existing->mask = mask & IN_MASK_ADD ? existing->mask | mask : mask;
    }
    else if (instance->watch_count >= kMaxWatches || instance->next_wd > 0x7fffffffU) result = -ENOSPC;
    else if (node != rootdir && node->refcount == uint32_t(-1)) result = -EOVERFLOW;
    else
    {
        if (node != rootdir) ++node->refcount;
        fresh->key = key;
        fresh->pin = node;
        fresh->instance = instance;
        fresh->mask = mask;
        result = fresh->wd = instance->next_wd++;
        fresh->hash_next = watch_index[bucket(key)];
        watch_index[bucket(key)] = fresh;
        fresh->next = instance->watches;
        instance->watches = fresh;
        ++instance->watch_count;
        fresh = NULL;
    }
    spin_unlock(&notification_lock);
    vfs_child_unlock();
    free(fresh);
    return result;
}

int inotify_remove(fd_file_handle *handle, int wd)
{
    if (!handle || !inotify_is_node(handle->node)) return -EINVAL;
    auto *instance = static_cast<Instance *>(handle->node->handle);
    spin_lock(&notification_lock);
    Watch *watch = instance->watches;
    while (watch && watch->wd != wd) watch = watch->next;
    if (watch) detach(watch);
    spin_unlock(&notification_lock);
    reap(instance);
    return watch ? 0 : -EINVAL;
}

int64_t inotify_read_user(fd_file_handle *handle, void *buffer, size_t size)
{
    if (size < sizeof(InotifyEvent)) return -EINVAL;
    auto *instance = static_cast<Instance *>(handle->node->handle);
    reap(instance);
    // Snapshot complete events, fault user pages with no locks held, then claim
    // that head generation. Racing readers retry; EFAULT consumes no events.
    uint8_t bounce[kReadBytes];
    while (true)
    {
        size_t bytes = 0;
        unsigned records = 0;
        spin_lock(&notification_lock);
        uint64_t generation = instance->read_generation;
        while (records < instance->count)
        {
            const QueuedEvent &event = instance->events[(instance->head + records) % kSlots];
            size_t length = sizeof(event.header) + event.header.len;
            if (length > size - bytes || length > sizeof(bounce) - bytes) break;
            memcpy(bounce + bytes, &event, length);
            bytes += length;
            ++records;
        }
        bool too_small = instance->count && !records;
        spin_unlock(&notification_lock);
        if (too_small) return -EINVAL;
        if (bytes)
        {
            if (!copy_to_user_pagedir(get_current_task()->parent_group->pagedir, buffer, bounce, bytes)) return -EFAULT;
            spin_lock(&notification_lock);
            bool claimed = instance->read_generation == generation;
            if (claimed)
            {
                for (unsigned index = 0; index < records; ++index)
                    if (instance->events[(instance->head + index) % kSlots].header.mask == IN_Q_OVERFLOW)
                        instance->overflow = false;
                instance->head = (instance->head + records) % kSlots;
                instance->count -= records;
                ++instance->read_generation;
            }
            spin_unlock(&notification_lock);
            if (claimed) return bytes;
            continue;
        }
        if (__atomic_load_n(&handle->node->flags, __ATOMIC_RELAXED) & O_NONBLOCK) return -EAGAIN;
        if (signals_pending_quick(get_current_task())) return -EINTR;
        scheduler_sleep_ns(1000000ULL);
    }
}

void inotify_vfs_child_locked(vfs_node_t parent, vfs_node_t child, const char *name, uint32_t mask)
{
    if (!parent) return;
    spin_lock(&notification_lock);
    publish(identity(parent), mask | (child->type & file_dir ? IN_ISDIR : 0), 0, name,
            child->type & file_unlinked);
    spin_unlock(&notification_lock);
}

void inotify_vfs_event(vfs_node_t node, uint32_t mask)
{
    if (!node) return;
    vfs_child_lock();
    mask |= node->type & file_dir ? IN_ISDIR : 0;
    spin_lock(&notification_lock);
    publish(identity(node), mask, 0, NULL);
    if (node->parent) publish(identity(node->parent), mask, 0, node->name, node->type & file_unlinked);
    spin_unlock(&notification_lock);
    vfs_child_unlock();
}

void inotify_vfs_unlink_locked(vfs_node_t node, vfs_node_t parent, const char *name)
{
    uint32_t directory = node->type & file_dir ? IN_ISDIR : 0;
    spin_lock(&notification_lock);
    if (parent) publish(identity(parent), IN_DELETE | directory, 0, name);
    Identity key = identity(node);
    publish(key, IN_DELETE_SELF | directory, 0, NULL);
    for (Watch *watch = watch_index[bucket(key)], *next; watch; watch = next)
    {
        next = watch->hash_next;
        if (equal(watch->key, key)) detach(watch);
    }
    spin_unlock(&notification_lock);
}

void inotify_vfs_move_locked(vfs_node_t node, vfs_node_t old_parent, const char *old_name)
{
    if (old_parent == node->parent && !strcmp(old_name, node->name)) return;
    uint32_t directory = node->type & file_dir ? IN_ISDIR : 0;
    spin_lock(&notification_lock);
    if (!++move_cookie) ++move_cookie;
    if (old_parent) publish(identity(old_parent), IN_MOVED_FROM | directory, move_cookie, old_name);
    if (node->parent) publish(identity(node->parent), IN_MOVED_TO | directory, move_cookie, node->name);
    publish(identity(node), IN_MOVE_SELF | directory, 0, NULL);
    spin_unlock(&notification_lock);
}
