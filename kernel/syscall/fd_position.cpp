// SPDX-License-Identifier: Apache-2.0
#include <syscall/fd_position.h>
#include <task/scheduler.h>
#include <fs/vfs/vfs.h>
#include <errno.h>
#include <krlibc.h>

struct FdSharedPosition
{
    size_t offset;
    size_t references;
    tcb_t owner;
    fd_file_handle *active;
    unsigned depth;
    bool commit;
    FdSharedPosition *next;
};

namespace fd_positions {
struct Link
{
    fd_file_handle *handle;
    FdSharedPosition *position;
    Link *next;
};

constexpr unsigned kBuckets = 256;
spin_t lock = SPIN_INIT;
Link *links[kBuckets];
FdSharedPosition *positions;

unsigned bucket(fd_file_handle *handle)
{
    return unsigned((uintptr_t(handle) >> 4) ^ (uintptr_t(handle) >> 16)) & (kBuckets - 1);
}

bool seekable(fd_file_handle *handle)
{
    return handle && handle->node && handle->node->size != uint64_t(-1) &&
           !(handle->node->type & (file_stream | file_pipe | file_socket | file_ptmx | file_pts));
}

FdSharedPosition *find_locked(fd_file_handle *handle)
{
    for (Link *link = links[bucket(handle)]; link; link = link->next)
        if (link->handle == handle) return link->position;
    return NULL;
}

int ensure(fd_file_handle *handle, FdSharedPosition *&position)
{
    position = NULL;
    if (!seekable(handle)) return 0;
    spin_lock(&lock);
    position = find_locked(handle);
    spin_unlock(&lock);
    if (position) return 0; // The caller's FD operation retains this association.

    auto *prepared = static_cast<FdSharedPosition *>(calloc(1, sizeof(FdSharedPosition)));
    auto *link = static_cast<Link *>(malloc(sizeof(Link)));
    if (!prepared || !link) { free(prepared); free(link); return -ENOMEM; }
    prepared->references = 1;
    spin_lock(&lock);
    position = find_locked(handle);
    if (!position)
    {
        prepared->offset = handle->offset;
        position = prepared;
        prepared->next = positions;
        positions = prepared;
        *link = {handle, position, links[bucket(handle)]};
        links[bucket(handle)] = link;
        prepared = NULL;
        link = NULL;
    }
    spin_unlock(&lock);
    free(prepared);
    free(link);
    return 0;
}
}

int fd_position_share(fd_file_handle *source, fd_file_handle *copy)
{
    FdSharedPosition *position = NULL;
    int error = fd_positions::ensure(source, position);
    if (error) return error;
    if (!position) { copy->offset = source->offset; return 0; }
    auto *link = static_cast<fd_positions::Link *>(malloc(sizeof(fd_positions::Link)));
    if (!link) return -ENOMEM;
    spin_lock(&fd_positions::lock);
    if (position->references == size_t(-1)) error = -EOVERFLOW;
    else
    {
        ++position->references;
        copy->offset = position->offset;
        unsigned index = fd_positions::bucket(copy);
        *link = {copy, position, fd_positions::links[index]};
        fd_positions::links[index] = link;
    }
    spin_unlock(&fd_positions::lock);
    if (error) free(link);
    return error;
}

void fd_position_forget(fd_file_handle *handle)
{
    fd_positions::Link *removed = NULL;
    FdSharedPosition *unused = NULL;
    spin_lock(&fd_positions::lock);
    auto **entry = &fd_positions::links[fd_positions::bucket(handle)];
    while (*entry && (*entry)->handle != handle) entry = &(*entry)->next;
    if (*entry)
    {
        removed = *entry;
        *entry = removed->next;
        if (--removed->position->references == 0)
        {
            unused = removed->position;
            FdSharedPosition **state = &fd_positions::positions;
            while (*state && *state != unused) state = &(*state)->next;
            if (*state) *state = unused->next;
        }
    }
    spin_unlock(&fd_positions::lock);
    free(removed);
    free(unused);
}

FdPositionScope::FdPositionScope(fd_file_handle *handle, bool commit)
    : FdPositionScope(handle, commit, NULL, false)
{
}

FdPositionScope::FdPositionScope(fd_file_handle *first, bool first_commit,
                               fd_file_handle *second, bool second_commit)
    : handles_{first, second}, commit_{first_commit, second_commit}, error_(0)
{
    for (unsigned i = 0; i < 2; ++i)
    {
        error_ = fd_positions::ensure(handles_[i], positions_[i]);
        if (error_) return;
    }
    if (positions_[0] && positions_[0] == positions_[1])
    {
        // Two implicit offsets necessarily overlap. Explicit positional I/O
        // can retain the single position used by the other endpoint instead.
        if (first_commit && second_commit) { error_ = -EINVAL; return; }
        if (second_commit) positions_[0] = NULL;
        else positions_[1] = NULL;
    }
    if (!positions_[0] && !positions_[1]) return;
    tcb_t owner = get_current_task();
    if (!owner) { error_ = -ESRCH; return; }
    for (;;)
    {
        spin_lock(&fd_positions::lock);
        bool busy = false;
        for (unsigned i = 0; i < 2; ++i)
        {
            FdSharedPosition *position = positions_[i];
            if (!position) continue;
            if (position->owner && position->owner != owner) busy = true;
            if (position->owner == owner &&
                (position->active != handles_[i] || position->commit != commit_[i])) error_ = -EDEADLK;
        }
        if (owner->status == DEATH || owner->status == OUT) error_ = -EINTR;
        if (!busy && !error_)
        {
            // Acquire both positions in one metadata transaction. Crossed
            // copy/sendfile calls never wait while owning just one position.
            for (unsigned i = 0; i < 2; ++i)
            {
                FdSharedPosition *position = positions_[i];
                if (!position) continue;
                if (!position->owner)
                {
                    position->owner = owner;
                    position->active = handles_[i];
                    position->commit = commit_[i];
                    handles_[i]->offset = position->offset;
                }
                ++position->depth;
            }
        }
        spin_unlock(&fd_positions::lock);
        if (!busy || error_) return;
        // Only metadata is spin protected. Slow VFS/user copies run under
        // this preemptible ownership, which the quiescent FD reaper releases.
        scheduler_yield();
    }
}

FdPositionScope::~FdPositionScope()
{
    if (error_) return;
    spin_lock(&fd_positions::lock);
    for (unsigned i = 0; i < 2; ++i)
    {
        FdSharedPosition *position = positions_[i];
        if (!position || --position->depth) continue;
        if (position->commit) position->offset = handles_[i]->offset;
        position->owner = NULL;
        position->active = NULL;
    }
    spin_unlock(&fd_positions::lock);
}

void fd_position_abandon(tcb_t owner)
{
    if (!owner || (owner->status != DEATH && owner->status != OUT) || scheduler_task_referenced(owner)) return;
    spin_lock(&fd_positions::lock);
    for (FdSharedPosition *position = fd_positions::positions; position; position = position->next)
        if (position->owner == owner)
        {
            if (position->commit) position->offset = position->active->offset;
            position->owner = NULL;
            position->active = NULL;
            position->depth = 0;
        }
    spin_unlock(&fd_positions::lock);
}
