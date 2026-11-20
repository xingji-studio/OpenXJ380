// SPDX-License-Identifier: Apache-2.0
// First-party operation pins and logical descriptor retirement. Registry
// records live on the heap so a killed thread never leaves a stack pointer.
#include <syscall/fd_lifetime.h>
#include <fs/vfs/vfs.h>
#include <task/scheduler.h>
#include <errno.h>

struct FdDescription;
struct FdPositionLease
{
    FdDescription *description;
    fd_file_handle *handle;
    bool update;
};

struct FdOperationRecord
{
    fd_file_handle *handle;
    tcb_t owner;
    FdOperationRecord *next;
    bool retired;
    unsigned positions;
    FdPositionLease position[2];
};

struct FdDescription
{
    size_t offset;
    uint64_t flags;
    size_t references;
    FdOperationRecord *owner;
    bool failed;
    FdDescription *next_dead;
};

struct FdBinding
{
    fd_file_handle *handle;
    FdDescription *description;
    FdBinding *next;
};

namespace {
constexpr unsigned kBuckets = 256;
spin_t registry_lock = SPIN_INIT;
FdOperationRecord *operations[kBuckets];
FdBinding *bindings[kBuckets];
FdDescription *dead_descriptions;

unsigned bucket(fd_file_handle *handle)
{
    return unsigned((uintptr_t(handle) >> 4) ^ (uintptr_t(handle) >> 16)) & (kBuckets - 1);
}

FdBinding *binding_locked(fd_file_handle *handle)
{
    for (FdBinding *entry = bindings[bucket(handle)]; entry; entry = entry->next)
        if (entry->handle == handle) return entry;
    return NULL;
}

void unref_description_locked(FdDescription *description)
{
    if (--description->references) return;
    description->next_dead = dead_descriptions;
    dead_descriptions = description;
}

void reap_descriptions()
{
    spin_lock(&registry_lock);
    FdDescription *dead = dead_descriptions;
    dead_descriptions = NULL;
    spin_unlock(&registry_lock);
    while (dead)
    {
        FdDescription *next = dead->next_dead;
        free(dead);
        dead = next;
    }
}

// The caller pins handle, or owns it before publication. Allocation and
// reclamation stay outside registry/table locks. Racing initializers share
// whichever binding was published first.
int ensure_description(fd_file_handle *handle)
{
    if (!handle || !handle->node) return -EBADF;
    spin_lock(&registry_lock);
    bool present = binding_locked(handle) != NULL;
    spin_unlock(&registry_lock);
    if (present) return 0;
    auto *binding = static_cast<FdBinding *>(malloc(sizeof(FdBinding)));
    auto *description = static_cast<FdDescription *>(malloc(sizeof(FdDescription)));
    if (!binding || !description) { free(binding); free(description); return -ENOMEM; }
    spin_lock(&registry_lock);
    if (!binding_locked(handle))
    {
        *description = {handle->offset, __atomic_load_n(&handle->flags, __ATOMIC_RELAXED) & ~uint64_t(O_CLOEXEC),
                        1, NULL, false, NULL};
        unsigned index = bucket(handle);
        *binding = {handle, description, bindings[index]};
        bindings[index] = binding;
        binding = NULL;
        description = NULL;
    }
    spin_unlock(&registry_lock);
    free(binding);
    free(description);
    return 0;
}

void release_positions_locked(FdOperationRecord *record, bool abandoned)
{
    for (unsigned index = 0; index < record->positions; ++index)
    {
        FdPositionLease &lease = record->position[index];
        if (abandoned) lease.description->failed = true;
        else if (lease.update) lease.description->offset = lease.handle->offset;
        if (lease.description->owner == record) lease.description->owner = NULL;
        unref_description_locked(lease.description);
    }
    record->positions = 0;
}

bool has_file_position(vfs_node_t node)
{
    // Unknown endpoint types must not serialize a blocking read against its
    // writer. Directory cookies, regular files, and block files do share a
    // position; namespace/lifetime flags do not change that classification.
    uint32_t allowed = file_none | file_dir | file_block | file_symlink | file_proxy | file_delete | file_unlinked;
    return node->size != uint64_t(-1) && !(node->type & ~allowed);
}

int finish_handle(fd_file_handle *handle)
{
    spin_lock(&registry_lock);
    FdBinding **entry = &bindings[bucket(handle)];
    while (*entry && (*entry)->handle != handle) entry = &(*entry)->next;
    FdBinding *removed = *entry;
    if (removed)
    {
        *entry = removed->next;
        unref_description_locked(removed->description);
    }
    spin_unlock(&registry_lock);
    free(removed);
    reap_descriptions();
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
    // Normal scopes commit before releasing their operation pin. A killed
    // owner may have stopped in a VFS write: its position cannot be guessed.
    // Fail that description closed, while releasing all ownership and refs.
    release_positions_locked(record, true);
    if (!record->retired) return NULL;
    for (FdOperationRecord *other = operations[index]; other; other = other->next)
        if (other->handle == record->handle) return NULL;
    return record->handle;
}
}

int fd_share_description(fd_file_handle *source, fd_file_handle *copy)
{
    if (!copy || source == copy) return -EINVAL;
    int error = ensure_description(source);
    if (error) return error;
    auto *prepared = static_cast<FdBinding *>(malloc(sizeof(FdBinding)));
    if (!prepared) return -ENOMEM;
    spin_lock(&registry_lock);
    FdDescription *description = binding_locked(source)->description;
    ++description->references;
    unsigned index = bucket(copy);
    *prepared = {copy, description, bindings[index]};
    bindings[index] = prepared;
    copy->offset = description->offset;
    copy->flags = description->flags | (__atomic_load_n(&source->flags, __ATOMIC_RELAXED) & O_CLOEXEC);
    spin_unlock(&registry_lock);
    return 0;
}

int fd_get_status_flags(FdOperationRecord *record, uint64_t &flags)
{
    if (!record) return -EBADF;
    int error = ensure_description(record->handle);
    if (error) return error;
    spin_lock(&registry_lock);
    flags = binding_locked(record->handle)->description->flags;
    spin_unlock(&registry_lock);
    return 0;
}

int fd_set_status_flags(FdOperationRecord *record, uint64_t mask, uint64_t flags)
{
    if (!record) return -EBADF;
    spin_lock(&registry_lock);
    mask &= ~uint64_t(O_CLOEXEC);
    FdBinding *binding = binding_locked(record->handle);
    if (!binding)
    {
        // A handle with no binding has never been duplicated or used with a
        // cursor scope. Its first initializer will observe this atomic cache.
        uint64_t previous = __atomic_load_n(&record->handle->flags, __ATOMIC_RELAXED);
        uint64_t updated;
        do { updated = (previous & ~mask) | (flags & mask); }
        while (!__atomic_compare_exchange_n(&record->handle->flags, &previous, updated, false,
                                            __ATOMIC_RELEASE, __ATOMIC_RELAXED));
        spin_unlock(&registry_lock);
        return 0;
    }
    FdDescription *description = binding->description;
    description->flags = (description->flags & ~mask) | (flags & mask);
    for (unsigned index = 0; index < kBuckets; ++index)
        for (FdBinding *entry = bindings[index]; entry; entry = entry->next)
            if (entry->description == description)
            {
                uint64_t previous = __atomic_load_n(&entry->handle->flags, __ATOMIC_RELAXED);
                uint64_t updated;
                do { updated = description->flags | (previous & O_CLOEXEC); }
                while (!__atomic_compare_exchange_n(&entry->handle->flags, &previous, updated, false,
                                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED));
            }
    spin_unlock(&registry_lock);
    return 0;
}

int fd_position_begin(FdOperationRecord *first, FdOperationRecord *second, bool update_first, bool update_second)
{
    if (!first || (second && second == first)) return -EINVAL;
    // A process exit can mark a thread DEATH while it is still unwinding a
    // blocking file syscall.  Do not let that thread remain in this wait
    // loop forever: the boundary/operation scope will finish the termination
    // path after the position and descriptor pins are released.
    tcb_t waiter = get_current_task();
    if (!waiter) return -ESRCH;
    FdOperationRecord *records[] = {first, second};
    bool updates[] = {update_first, update_second};
    unsigned count = second ? 2 : 1;
    for (unsigned index = 0; index < count; ++index)
    {
        int error = ensure_description(records[index]->handle);
        if (error) return error;
    }
    for (;;)
    {
        spin_lock(&registry_lock);
        bool busy = false;
        TaskStatus waiter_status = __atomic_load_n(&waiter->status, __ATOMIC_ACQUIRE);
        bool waiter_exiting = waiter_status == DEATH || waiter_status == OUT ||
                              __atomic_load_n(&waiter->kernel_exit_pending, __ATOMIC_ACQUIRE);
        int error = waiter_exiting ? -EINTR : ((first->positions || (second && second->positions)) ? -EBUSY : 0);
        for (unsigned index = 0; index < count && !error; ++index)
        {
            fd_file_handle *handle = records[index]->handle;
            FdDescription *description = binding_locked(handle)->description;
            if (!has_file_position(handle->node)) continue;
            if (description->failed) error = -EIO;
            busy |= description->owner != NULL;
        }
        if (!busy && !error)
        {
            // Acquire a pair in one metadata transaction: opposite-direction
            // copies cannot hold one description while waiting for the other.
            for (unsigned index = 0; index < count; ++index)
            {
                fd_file_handle *handle = records[index]->handle;
                FdDescription *description = binding_locked(handle)->description;
                if (!has_file_position(handle->node)) continue;
                description->owner = first;
                ++description->references;
                handle->offset = description->offset;
                first->position[first->positions++] = {description, handle, updates[index]};
            }
        }
        spin_unlock(&registry_lock);
        if (error) return error;
        if (!busy) return 0;
        scheduler_yield();
    }
}

void fd_position_end(FdOperationRecord *record)
{
    if (!record) return;
    spin_lock(&registry_lock);
    release_positions_locked(record, false);
    spin_unlock(&registry_lock);
    reap_descriptions();
}

static FdOperationRecord *acquire_prepared(pcb_t process, int fd, int &error, FdOperationRecord *record)
{
    error = -EBADF;
    tcb_t task = get_current_task();
    if (fd < 0 || !process || !task) return NULL;
    record->handle = NULL;
    record->owner = task;
    record->retired = false;
    record->positions = 0;
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
    return error ? NULL : record;
}


FdOperationRecord *fd_operation_acquire_process(pcb_t process, int fd, int &error)
{
    auto *prepared = static_cast<FdOperationRecord *>(malloc(sizeof(FdOperationRecord)));
    if (!prepared) { error = -ENOMEM; return NULL; }
    FdOperationRecord *record = acquire_prepared(process, fd, error, prepared);
    if (!record) free(prepared);
    return record;
}

// The global process list pins the PCB only during this bounded acquisition.
// The resulting descriptor ticket never retains or dereferences that PCB.
extern lock_queue *pcb_group_queue;
FdOperationRecord *fd_operation_acquire_notification(uint64_t pid, uint64_t &callback, int &error)
{
    callback = 0;
    error = -ESRCH;
    auto *prepared = static_cast<FdOperationRecord *>(malloc(sizeof(FdOperationRecord)));
    if (!prepared) { error = -ENOMEM; return NULL; }
    FdOperationRecord *record = NULL;
    if (pcb_group_queue)
    {
        spin_lock(&pcb_group_queue->lock);
        for (lock_node *entry = pcb_group_queue->head; entry; entry = entry->next)
        {
            auto *process = static_cast<pcb_t>(entry->data);
            if (!process || process->pid != pid) continue;
            if (process->notify_pcor_registered && process->notify_pcor_func)
            {
                record = acquire_prepared(process, process->notify_pcor_pipe_write_fd, error, prepared);
                if (record) callback = process->notify_pcor_func;
            }
            break;
        }
        spin_unlock(&pcb_group_queue->lock);
    }
    if (!record) free(prepared);
    return record;
}

bool fd_operation_transfer_to_kernel(FdOperationRecord *record)
{
    if (!record) return false;
    tcb_t owner = get_current_task();
    spin_lock(&registry_lock);
    bool valid = owner && record->owner == owner && !record->retired &&
                 owner->status != DEATH && owner->status != OUT;
    if (valid) record->owner = NULL;
    spin_unlock(&registry_lock);
    return valid;
}

bool fd_operation_is_retired(FdOperationRecord *record)
{
    if (!record) return true;
    spin_lock(&registry_lock);
    bool retired = record->retired;
    spin_unlock(&registry_lock);
    return retired;
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
    reap_descriptions();
}

void fd_discard_file_table(lock_queue *table)
{
    if (!table) return;
    while (fd_file_handle *handle = fd_detach_first(table)) fd_retire_handle(handle);
    queue_destroy(table);
}

namespace {
struct FdCloneItem
{
    FdOperationRecord *pin;
    lock_node *node;
    size_t number;
    uint64_t cloexec;
};

int snapshot_file_table(pcb_t source, FdCloneItem *items, size_t capacity, size_t &required)
{
    required = 0;
    int error = -EBADF;
    spin_lock(&registry_lock);
    lock_queue *table = source ? source->file_open : NULL;
    if (table && source->status != DEATH && source->status != OUT && source->status != ZOMBIE)
    {
        spin_lock(&table->lock);
        for (lock_node *node = table->head; node; node = node->next)
            if (node->data && static_cast<fd_file_handle *>(node->data)->node) ++required;
        error = required <= capacity ? 0 : -EAGAIN;
        if (!error)
        {
            size_t index = 0;
            for (lock_node *node = table->head; node; node = node->next)
            {
                auto *handle = static_cast<fd_file_handle *>(node->data);
                if (!handle || !handle->node) continue;
                FdCloneItem &item = items[index++];
                unsigned slot = bucket(handle);
                *item.pin = {handle, get_current_task(), operations[slot], false, 0, {}};
                operations[slot] = item.pin;
                item.number = node->index;
                item.cloexec = __atomic_load_n(&handle->flags, __ATOMIC_RELAXED) & O_CLOEXEC;
            }
        }
        spin_unlock(&table->lock);
    }
    spin_unlock(&registry_lock);
    return error;
}
}

int fd_clone_file_table(pcb_t source, lock_queue **copy)
{
    if (!copy) return -EINVAL;
    *copy = NULL;
    size_t capacity = 0, count = 0;
    FdCloneItem *items = NULL;
    int error;
    for (;;)
    {
        error = snapshot_file_table(source, items, capacity, count);
        if (error != -EAGAIN) break;
        for (size_t index = 0; index < capacity; ++index)
        {
            free(items[index].pin);
            free(items[index].node);
        }
        free(items);
        items = NULL;
        capacity = count;
        if (capacity > size_t(-1) / sizeof(FdCloneItem)) return -ENOMEM;
        items = static_cast<FdCloneItem *>(malloc(capacity * sizeof(FdCloneItem)));
        if (!items) return -ENOMEM;
        memset(items, 0, capacity * sizeof(FdCloneItem));
        bool complete = true;
        for (size_t index = 0; index < capacity; ++index)
        {
            items[index].pin = static_cast<FdOperationRecord *>(malloc(sizeof(FdOperationRecord)));
            items[index].node = static_cast<lock_node *>(malloc(sizeof(lock_node)));
            if (!items[index].pin || !items[index].node) { complete = false; break; }
        }
        if (!complete) { error = -ENOMEM; break; }
    }
    bool pinned = !error;
    lock_queue *table = pinned ? queue_init() : NULL;
    if (pinned && !table) error = -ENOMEM;
    for (size_t index = 0; !error && index < count; ++index)
    {
        fd_file_handle *handle = fd_dup(items[index].pin->handle);
        if (!handle) { error = -ENOMEM; break; }
        handle->fd = items[index].number;
        if (items[index].cloexec) __atomic_fetch_or(&handle->flags, uint64_t(O_CLOEXEC), __ATOMIC_RELEASE);
        else __atomic_fetch_and(&handle->flags, ~uint64_t(O_CLOEXEC), __ATOMIC_RELEASE);
        lock_node *node = items[index].node;
        node->data = handle;
        node->index = handle->fd;
        node->prev = table->tail;
        node->next = NULL;
        if (table->tail) table->tail->next = node;
        else table->head = node;
        table->tail = node;
        if (!table->rr_cursor) table->rr_cursor = node;
        ++table->size;
        if (node->index >= table->next_index) table->next_index = node->index + 1;
        items[index].node = NULL;
    }
    for (size_t index = 0; index < capacity; ++index)
    {
        if (pinned && index < count) fd_operation_release(items[index].pin);
        else free(items[index].pin);
        free(items[index].node);
    }
    free(items);
    if (error) fd_discard_file_table(table);
    else *copy = table;
    return error;
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
                *creator = {handle, get_current_task(), operations[index], false, 0, {}};
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
                __atomic_fetch_or(&static_cast<fd_file_handle *>(entry->data)->flags, uint64_t(O_CLOEXEC),
                                  __ATOMIC_RELEASE);
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
        reap_descriptions();
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
        reap_descriptions();
    }
    return true;
}
