// SPDX-License-Identifier: Apache-2.0
// First-party operation pins and logical descriptor retirement. Registry
// records live on the heap so a killed thread never leaves a stack pointer.
#include <syscall/fd_lifetime.h>
#include <fs/vfs/vfs.h>
#include <task/scheduler.h>
#include <errno.h>

struct FdOperationRecord
{
    fd_file_handle *handle;
    tcb_t owner;
    FdOperationRecord *next;
    bool retired;
};

namespace {
constexpr unsigned kBuckets = 256;
spin_t registry_lock = SPIN_INIT;
FdOperationRecord *operations[kBuckets];

unsigned bucket(fd_file_handle *handle)
{
    return unsigned((uintptr_t(handle) >> 4) ^ (uintptr_t(handle) >> 16)) & (kBuckets - 1);
}

int finish_handle(fd_file_handle *handle)
{
    int error = vfs_close(handle->node);
    free(handle);
    return error;
}

// All callers hold registry_lock before a descriptor-table lock. Detach only
// edits links while locked; allocator and VFS operations occur after unlock.
lock_node *detach_locked(lock_queue *table, size_t first, size_t last, bool cloexec)
{
    for (lock_node *node = table->head; node; node = node->next)
    {
        auto *handle = static_cast<fd_file_handle *>(node->data);
        if (node->index < first || node->index > last || (cloexec && (!handle || !(handle->flags & O_CLOEXEC))))
            continue;
        if (node->prev) node->prev->next = node->next;
        else table->head = node->next;
        if (node->next) node->next->prev = node->prev;
        else table->tail = node->prev;
        if (table->rr_cursor == node) table->rr_cursor = node->next ? node->next : table->head;
        if (!table->head) table->tail = table->rr_cursor = NULL;
        if (table->size) --table->size;
        return node;
    }
    return NULL;
}

fd_file_handle *detached_handle(lock_node *node)
{
    if (!node) return NULL;
    auto *handle = static_cast<fd_file_handle *>(node->data);
    free(node);
    return handle;
}

// Remove exactly one record. A retired handle is released by the last pin;
// live handles remain owned by their descriptor table.
fd_file_handle *remove_record_locked(FdOperationRecord *record)
{
    unsigned index = bucket(record->handle);
    FdOperationRecord **entry = &operations[index];
    while (*entry && *entry != record) entry = &(*entry)->next;
    if (!*entry) return NULL;
    *entry = record->next;
    if (!record->retired) return NULL;
    for (FdOperationRecord *other = operations[index]; other; other = other->next)
        if (other->handle == record->handle) return NULL;
    return record->handle;
}
}

FdOperationRecord *fd_operation_acquire_process(pcb_t process, int fd, int &error)
{
    error = -EBADF;
    tcb_t task = get_current_task();
    if (fd < 0 || !process || !task) return NULL;
    auto *record = static_cast<FdOperationRecord *>(malloc(sizeof(FdOperationRecord)));
    if (!record) { error = -ENOMEM; return NULL; }
    record->handle = NULL;
    record->owner = task;
    record->retired = false;
    spin_lock(&registry_lock);
    lock_queue *table = process->file_open;
    if (task->status == DEATH || task->status == OUT || process->status == DEATH ||
        process->status == ZOMBIE || process->status == OUT) table = NULL;
    if (table)
    {
        spin_lock(&table->lock);
        for (lock_node *entry = table->head; entry; entry = entry->next)
        {
            if (entry->index != size_t(fd)) continue;
            auto *handle = static_cast<fd_file_handle *>(entry->data);
            if (!handle || !handle->node) break;
            record->handle = handle;
            unsigned index = bucket(handle);
            record->next = operations[index];
            operations[index] = record;
            error = 0;
            break;
        }
        spin_unlock(&table->lock);
    }
    spin_unlock(&registry_lock);
    if (error) { free(record); return NULL; }
    return record;
}

FdOperationRecord *fd_operation_acquire(int fd, int &error)
{
    tcb_t task = get_current_task();
    return fd_operation_acquire_process(task ? task->parent_group : NULL, fd, error);
}

int fd_snapshot_numbers(pcb_t process, size_t *numbers, size_t capacity, size_t &count)
{
    count = 0;
    if (!process || (!numbers && capacity)) return -EINVAL;
    spin_lock(&registry_lock);
    lock_queue *table = process->file_open;
    if (table)
    {
        spin_lock(&table->lock);
        for (lock_node *entry = table->head; entry; entry = entry->next)
        {
            auto *handle = static_cast<fd_file_handle *>(entry->data);
            if (!handle || !handle->node || entry->index > 0x7fffffffU) continue;
            if (count < capacity) numbers[count] = entry->index;
            ++count;
        }
        spin_unlock(&table->lock);
    }
    spin_unlock(&registry_lock);
    return numbers && count > capacity ? -ENOSPC : 0;
}

fd_file_handle *fd_operation_handle(FdOperationRecord *record) { return record ? record->handle : NULL; }

void fd_operation_release(FdOperationRecord *record)
{
    if (!record) return;
    spin_lock(&registry_lock);
    fd_file_handle *retired = remove_record_locked(record);
    spin_unlock(&registry_lock);
    free(record);
    if (retired) finish_handle(retired);
}

int fd_retire_handle(fd_file_handle *handle)
{
    if (!handle) return -EBADF;
    bool pinned = false;
    spin_lock(&registry_lock);
    for (FdOperationRecord *record = operations[bucket(handle)]; record; record = record->next)
    {
        if (record->handle != handle) continue;
        record->retired = true;
        pinned = true;
    }
    spin_unlock(&registry_lock);
    return pinned ? 0 : finish_handle(handle);
}

int fd_install_handle(pcb_t process, fd_file_handle *handle, int minimum, bool exact,
                      FdOperationRecord **publisher)
{
    if (publisher) *publisher = NULL;
    if (!process || !handle || minimum < 0) return -EINVAL;
    auto *node = static_cast<lock_node *>(malloc(sizeof(lock_node)));
    if (!node) return -ENOMEM;
    auto *creator = publisher ? static_cast<FdOperationRecord *>(malloc(sizeof(FdOperationRecord))) : NULL;
    if (publisher && !creator) { free(node); return -ENOMEM; }
    lock_node *replaced = NULL;
    int result = -EBADF;
    spin_lock(&registry_lock);
    lock_queue *table = process->file_open;
    if (table && process->status != DEATH && process->status != ZOMBIE && process->status != OUT)
    {
        spin_lock(&table->lock);
        size_t id = size_t(minimum);
        if (!exact)
        {
            for (;;)
            {
                bool used = false;
                for (lock_node *entry = table->head; entry; entry = entry->next)
                    if (entry->index == id) { used = true; break; }
                if (!used || id == 0x7fffffffU) break;
                ++id;
            }
        }
        bool used = false;
        if (!exact)
            for (lock_node *entry = table->head; entry; entry = entry->next) used |= entry->index == id;
        if (!used)
        {
            if (exact) replaced = detach_locked(table, id, id, false);
            handle->fd = id;
            node->data = handle;
            node->index = id;
            node->next = NULL;
            node->prev = table->tail;
            if (table->tail) table->tail->next = node;
            else table->head = node;
            table->tail = node;
            if (!table->rr_cursor) table->rr_cursor = node;
            ++table->size;
            if (id >= table->next_index) table->next_index = id + 1;
            if (creator)
            {
                unsigned index = bucket(handle);
                *creator = {handle, get_current_task(), operations[index], false};
                operations[index] = creator;
                *publisher = creator;
            }
            result = int(id);
        }
        else result = -EMFILE;
        spin_unlock(&table->lock);
    }
    spin_unlock(&registry_lock);
    if (result < 0) { free(node); free(creator); }
    if (replaced) fd_retire_handle(detached_handle(replaced));
    return result;
}

fd_file_handle *fd_detach_matching(pcb_t process, size_t first, size_t last, bool cloexec)
{
    lock_node *removed = NULL;
    spin_lock(&registry_lock);
    lock_queue *table = process ? process->file_open : NULL;
    if (table)
    {
        spin_lock(&table->lock);
        removed = detach_locked(table, first, last, cloexec);
        spin_unlock(&table->lock);
    }
    spin_unlock(&registry_lock);
    return detached_handle(removed);
}

fd_file_handle *fd_detach_descriptor(pcb_t process, size_t fd)
{
    return fd_detach_matching(process, fd, fd, false);
}

fd_file_handle *fd_detach_descriptor_if(pcb_t process, size_t fd, fd_file_handle *expected)
{
    lock_node *removed = NULL;
    spin_lock(&registry_lock);
    lock_queue *table = process ? process->file_open : NULL;
    if (table)
    {
        spin_lock(&table->lock);
        for (lock_node *entry = table->head; entry; entry = entry->next)
            if (entry->index == fd && entry->data == expected)
            {
                removed = detach_locked(table, fd, fd, false);
                break;
            }
        spin_unlock(&table->lock);
    }
    spin_unlock(&registry_lock);
    return detached_handle(removed);
}

fd_file_handle *fd_detach_first(lock_queue *table)
{
    if (!table) return NULL;
    spin_lock(&table->lock);
    lock_node *removed = detach_locked(table, 0, size_t(-1), false);
    spin_unlock(&table->lock);
    return detached_handle(removed);
}

void fd_mark_cloexec(pcb_t process, size_t first, size_t last)
{
    spin_lock(&registry_lock);
    lock_queue *table = process ? process->file_open : NULL;
    if (table)
    {
        spin_lock(&table->lock);
        for (lock_node *entry = table->head; entry; entry = entry->next)
            if (entry->index >= first && entry->index <= last && entry->data)
                static_cast<fd_file_handle *>(entry->data)->flags |= O_CLOEXEC;
        spin_unlock(&table->lock);
    }
    spin_unlock(&registry_lock);
}

lock_queue *fd_detach_process_table(pcb_t process)
{
    spin_lock(&registry_lock);
    lock_queue *table = process->file_open;
    process->file_open = NULL;
    spin_unlock(&registry_lock);
    return table;
}

namespace {
bool owner_quiescent(tcb_t task)
{
    return task && (task->status == DEATH || task->status == OUT) && !scheduler_task_referenced(task);
}
}

// Individual thread exit need not destroy its still-live process. Its abandoned
// tickets must nevertheless stop retaining logically closed descriptors. The
// registry lock protects each owner pointer: final TCB destruction removes all
// its records through fd_operation_thread_exit before freeing that TCB.
void fd_reap_abandoned_operations()
{
    for (;;)
    {
        FdOperationRecord *abandoned = NULL;
        spin_lock(&registry_lock);
        for (unsigned index = 0; index < kBuckets && !abandoned; ++index)
            for (FdOperationRecord *record = operations[index]; record; record = record->next)
                if (owner_quiescent(record->owner)) { abandoned = record; break; }
        fd_file_handle *retired = abandoned ? remove_record_locked(abandoned) : NULL;
        spin_unlock(&registry_lock);
        if (!abandoned) return;
        free(abandoned);
        if (retired) finish_handle(retired);
    }
}

bool fd_operation_thread_exit(tcb_t task)
{
    if (!owner_quiescent(task)) return false;
    for (;;)
    {
        FdOperationRecord *abandoned = NULL;
        spin_lock(&registry_lock);
        for (unsigned index = 0; index < kBuckets && !abandoned; ++index)
            for (FdOperationRecord *record = operations[index]; record; record = record->next)
                if (record->owner == task) { abandoned = record; break; }
        fd_file_handle *retired = abandoned ? remove_record_locked(abandoned) : NULL;
        spin_unlock(&registry_lock);
        if (!abandoned) break;
        free(abandoned);
        if (retired) finish_handle(retired);
    }
    return true;
}
