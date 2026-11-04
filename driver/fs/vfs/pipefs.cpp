#define ALL_IMPLEMENTATION
#include "pipe.h"
#include <proto.hpp>
#include <task/poll.h>
#include <fs/vfs/vfs.h>
#include <errno.h>

vfs_node_t pipefs_root = NULL;
int        pipefs_id   = 0;
int        pipefd_id   = 0;

int pipefs_create_pair(vfs_node_t *reader, vfs_node_t *writer)
{
    if (!reader || !writer) return -EINVAL;
    *reader = *writer = NULL;
    if (!pipefs_root) return -ENOSYS;
    static uint64_t next_inode = 1;
    vfs_node_t input = vfs_node_alloc(NULL, "pipe-read");
    vfs_node_t output = vfs_node_alloc(NULL, "pipe-write");
    pipe_info_t *info = (pipe_info_t *)calloc(1, sizeof(pipe_info_t));
    pipe_specific_t *read_spec = (pipe_specific_t *)calloc(1, sizeof(pipe_specific_t));
    pipe_specific_t *write_spec = (pipe_specific_t *)calloc(1, sizeof(pipe_specific_t));
    char *buffer = (char *)calloc(1, PIPE_BUFF);
    if (!input || !output || !info || !read_spec || !write_spec || !buffer)
    {
        // These nodes have not acquired filesystem handles yet.
        if (input) { free(input->name); free(input); }
        if (output) { free(output->name); free(output); }
        free(info);
        free(read_spec);
        free(write_spec);
        free(buffer);
        return -ENOMEM;
    }
    info->buf = buffer;
    info->lock = SPIN_INIT;
    info->read_fds = info->write_fds = 1;
    info->read_spec = read_spec;
    info->write_spec = write_spec;
    read_spec->info = write_spec->info = info;
    read_spec->node = input;
    write_spec->node = output;
    write_spec->write = true;
    input->type = output->type = file_pipe;
    input->fsid = output->fsid = pipefs_id;
    input->root = output->root = pipefs_root;
    input->dev = output->dev = PIEFS_REGISTER_ID;
    input->inode = output->inode = __atomic_fetch_add(&next_inode, 1ULL, __ATOMIC_RELAXED);
    input->handle = read_spec;
    output->handle = write_spec;
    *reader = input;
    *writer = output;
    return 0;
}

// A detached wake list keeps numeric identity, never an unpinned TCB to use
// after unlocking the pipe. The public list-node prefix remains unchanged.
struct PipeWaiter
{
    task_block_list_t node;
    size_t tid;
};
extern lock_queue *pcb_group_queue;

static void pipe_wake_live_waiter(tcb_t identity, size_t tid)
{
    if (!pcb_group_queue) return;
    spin_lock(&pcb_group_queue->lock);
    for (lock_node *entry = pcb_group_queue->head; entry; entry = entry->next)
    {
        auto *process = static_cast<pcb_t>(entry->data);
        if (!process || !process->thread_queue) continue;
        spin_lock(&process->thread_queue->lock);
        bool found = false;
        for (lock_node *thread = process->thread_queue->head; thread; thread = thread->next)
        {
            if (thread->data != identity) continue;
            auto *task = static_cast<tcb_t>(thread->data);
            if (task->tid == tid) scheduler_wake_task(task);
            found = true;
            break;
        }
        spin_unlock(&process->thread_queue->lock);
        if (found) break;
    }
    spin_unlock(&pcb_group_queue->lock);
}

static task_block_list_t *pipe_detach_waiters(task_block_list_t *head)
{
    task_block_list_t *list = head->next;
    head->next = NULL;
    return list;
}

static void pipe_wake_waiters(task_block_list_t *list)
{
    while (list)
    {
        task_block_list_t *next = list->next;
        auto *waiter = reinterpret_cast<PipeWaiter *>(list);
        pipe_wake_live_waiter(waiter->node.thread, waiter->tid);
        free(waiter);
        list = next;
    }
}

static PipeWaiter *pipe_prepare_waiter()
{
    tcb_t task = get_current_task();
    if (!task || task->task_level == TASK_IDLE_LEVEL) return NULL;
    auto *waiter = static_cast<PipeWaiter *>(malloc(sizeof(PipeWaiter)));
    if (waiter) *waiter = {{NULL, task}, task->tid};
    return waiter;
}

// The caller allocates before taking the pipe lock and rechecks readiness
// under that lock. Publication and WAIT therefore cannot lose a reader wake.
static bool pipe_wait_on(pipe_info_t *pipe, task_block_list_t *head, PipeWaiter *prepared)
{
    if (!prepared)
    {
        spin_unlock(&pipe->lock);
        scheduler_sleep_ns(1000000ULL);
        return true;
    }
    tcb_t current = prepared->node.thread;
    TaskStatus expected = __atomic_load_n(&current->status, __ATOMIC_ACQUIRE);
    prepared->node.next = head->next;
    head->next = &prepared->node;
    current->wakeup_time = 0;
    if ((expected != RUNNING && expected != START) ||
        !__atomic_compare_exchange_n(&current->status, &expected, WAIT, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        head->next = prepared->node.next;
        spin_unlock(&pipe->lock);
        free(prepared);
        return false;
    }
    spin_unlock(&pipe->lock);
    while (__atomic_load_n(&current->status, __ATOMIC_ACQUIRE) == WAIT) scheduler_yield();
    expected = START;
    (void)__atomic_compare_exchange_n(&current->status, &expected, RUNNING, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    TaskStatus status = __atomic_load_n(&current->status, __ATOMIC_ACQUIRE);
    return status != DEATH && status != OUT;
}

// An all-or-nothing message packet write. The descriptor operation ticket
// retained by the caller pins spec/pipe across the unlocked wake phase.
int pipefs_try_write_packet(vfs_node_t node, const void *data, size_t size)
{
    if (!node || !(node->type & file_pipe) || !data || !size || size > PIPE_BUFF) return -EINVAL;
    auto *spec = static_cast<pipe_specific_t *>(node->handle);
    if (!spec || !spec->write || !spec->info) return -EBADF;
    pipe_info_t *pipe = spec->info;
    task_block_list_t *waiters = NULL;
    int result = -EAGAIN;
    spin_lock(&pipe->lock);
    if (!pipe->buf || pipe->read_fds == 0 || pipe->write_fds == 0) result = -EPIPE;
    else if (pipe->ptr <= PIPE_BUFF && PIPE_BUFF - pipe->ptr >= size)
    {
        memcpy(pipe->buf + pipe->ptr, data, size);
        pipe->ptr += size;
        waiters = pipe_detach_waiters(&pipe->blocking_read);
        result = int(size);
    }
    spin_unlock(&pipe->lock);
    pipe_wake_waiters(waiters);
    return result;
}

void pipefs_open(void *parent, const char *name, vfs_node_t node) {
    (void)parent;
    (void)name;
    node->type = file_pipe;
}

size_t pipefs_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)offset;
    if (!size) return 0;
    if (size > PIPE_BUFF) size = PIPE_BUFF;
    auto *spec = static_cast<pipe_specific_t *>(file);
    if (!spec || !spec->info) return (size_t)-EINVAL;
    pipe_info_t *pipe = spec->info;
    PipeWaiter *prepared = NULL;
    for (;;)
    {
        spin_lock(&pipe->lock);
        if (pipe->ptr)
        {
            uint32_t count = MIN(size, pipe->ptr);
            memcpy(addr, pipe->buf, count);
            if (pipe->ptr > count) memmove(pipe->buf, pipe->buf + count, pipe->ptr - count);
            pipe->ptr -= count;
            task_block_list_t *waiters = pipe_detach_waiters(&pipe->blocking_write);
            spin_unlock(&pipe->lock);
            pipe_wake_waiters(waiters);
            free(prepared);
            return count;
        }
        if (!pipe->write_fds)
        {
            spin_unlock(&pipe->lock);
            free(prepared);
            return 0;
        }
        if (!prepared)
        {
            spin_unlock(&pipe->lock);
            prepared = pipe_prepare_waiter();
            if (!prepared) scheduler_sleep_ns(1000000ULL);
            continue;
        }
        bool alive = pipe_wait_on(pipe, &pipe->blocking_read, prepared);
        prepared = NULL;
        if (!alive) return (size_t)-EINTR;
    }
}

size_t pipe_write_inner(void *file, const void *addr, size_t size)
{
    auto *spec = static_cast<pipe_specific_t *>(file);
    if (!spec || !spec->info) return (size_t)-EINVAL;
    pipe_info_t *pipe = spec->info;
    PipeWaiter *prepared = NULL;
    for (;;)
    {
        spin_lock(&pipe->lock);
        if (!pipe->read_fds)
        {
            spin_unlock(&pipe->lock);
            free(prepared);
            return (size_t)-EPIPE;
        }
        if (pipe->ptr <= PIPE_BUFF && PIPE_BUFF - pipe->ptr >= size)
        {
            memcpy(pipe->buf + pipe->ptr, addr, size);
            pipe->ptr += size;
            task_block_list_t *waiters = pipe_detach_waiters(&pipe->blocking_read);
            spin_unlock(&pipe->lock);
            pipe_wake_waiters(waiters);
            free(prepared);
            return size;
        }
        if (!prepared)
        {
            spin_unlock(&pipe->lock);
            prepared = pipe_prepare_waiter();
            if (!prepared) scheduler_sleep_ns(1000000ULL);
            continue;
        }
        bool alive = pipe_wait_on(pipe, &pipe->blocking_write, prepared);
        prepared = NULL;
        if (!alive) return (size_t)-EINTR;
    }
}

size_t pipefs_write(void *file, const void *addr, size_t offset, size_t size) {
    (void)offset;
    size_t ret       = 0;
    size_t chunks    = size / PIPE_BUFF;
    size_t remainder = size % PIPE_BUFF;
    if (chunks)
        for (size_t i = 0; i < chunks; i++) {
            size_t cycle = 0;
            while (cycle != PIPE_BUFF)
            {
                size_t wrote = pipe_write_inner(file, (const char *)addr + i * PIPE_BUFF + cycle, PIPE_BUFF - cycle);
                if ((int64_t)wrote < 0) return ret ? ret : wrote;
                cycle += wrote;
            }
            ret += cycle;
        }

    if (remainder) {
        size_t cycle = 0;
        while (cycle != remainder)
        {
            size_t wrote = pipe_write_inner(file, (const char *)addr + chunks * PIPE_BUFF + cycle, remainder - cycle);
            if ((int64_t)wrote < 0) return ret ? ret : wrote;
            cycle += wrote;
        }
        ret += cycle;
    }

    return ret;
}

int pipefs_ioctl(void *file, ssize_t cmd, ssize_t arg) {
    switch (cmd) {
    default: return -ENOSYS;
    }
}

bool pipefs_close(void *current) {
    pipe_specific_t *spec = (pipe_specific_t *)current;
    if (spec == NULL) return true;

    pipe_info_t *pipe = spec->info;
    if (pipe == NULL) {
        free(spec);
        return true;
    }

    bool free_spec  = false;
    bool free_pipe  = false;
    pipe_specific_t *other_spec = NULL;
    task_block_list_t *waiters = NULL;

    spin_lock(&pipe->lock);
    if (spec->write) {
        if (pipe->write_fds > 0) pipe->write_fds--;
        free_spec = (pipe->write_fds == 0);
        if (free_spec) {
            waiters = pipe_detach_waiters(&pipe->blocking_read);
            pipe->write_spec = NULL;
            other_spec = pipe->read_spec;
        }
    } else {
        if (pipe->read_fds > 0) pipe->read_fds--;
        free_spec = (pipe->read_fds == 0);
        if (free_spec) {
            waiters = pipe_detach_waiters(&pipe->blocking_write);
            pipe->read_spec = NULL;
            other_spec = pipe->write_spec;
        }
    }
    free_pipe = (pipe->write_fds == 0 && pipe->read_fds == 0);
    spin_unlock(&pipe->lock);

    pipe_wake_waiters(waiters);
    if (free_spec) {
        free(spec);
    }
    if (free_pipe) {
        if (other_spec) {
            other_spec->info = NULL;
            free(other_spec);
        }
        free(pipe->buf);
        free(pipe);
    }

    return true;
}

int pipefs_poll(void *file, size_t events) {
    pipe_specific_t *spec = (pipe_specific_t *)file;
    if (spec == NULL || spec->info == NULL) return EPOLLERR;

    pipe_info_t     *pipe = spec->info;

    int out = 0;

    spin_lock(&pipe->lock);
    if (spec->write) {
        if (pipe->read_fds == 0) out |= EPOLLERR | EPOLLHUP;
    } else if (pipe->write_fds == 0) {
        out |= EPOLLHUP;
    }

    if ((events & EPOLLIN) && pipe->ptr > 0) out |= EPOLLIN;

    if (events & EPOLLOUT) {
        if (pipe->ptr < PIPE_BUFF) out |= EPOLLOUT;
    }
    spin_unlock(&pipe->lock);
    return out;
}

int pipefs_mount(const char *handle, vfs_node_t node) {
    if ((uint64_t)handle != PIEFS_REGISTER_ID) return VFS_STATUS_FAILED;
    node->fsid  = pipefs_id;
    pipefs_root = node;
    return VFS_STATUS_SUCCESS;
}

static int dummy() {
    return -ENOSYS;
}

errno_t pipefs_stat(void *file, vfs_node_t node) {
    pipe_specific_t *spec = (pipe_specific_t *)file;
    pipe_info_t     *pipe = spec->info;
    if(pipe == NULL) return EOK;
    node->size = pipe->ptr;
    return EOK;
}

static struct vfs_callback pipefs_callbacks = {
    .mount    = pipefs_mount,
    .unmount  = (vfs_unmount_t)empty,
    .open     = (vfs_open_t)pipefs_open,
    .close    = (vfs_close_t)pipefs_close,
    .read     = pipefs_read,
    .write    = pipefs_write,
    .readlink = (vfs_readlink_t)dummy,
    .mkdir    = (vfs_mk_t)empty,
    .mkfile   = (vfs_mk_t)empty,
    .link     = (vfs_mk_t)dummy,
    .symlink  = (vfs_mk_t)dummy,
    .del   = (vfs_del_t)empty,
    .rename   = (vfs_rename_t)empty,
    .map      = (vfs_mapfile_t)empty,
    .stat     = pipefs_stat,
    .ioctl    = (vfs_ioctl_t)pipefs_ioctl,
    .poll     = pipefs_poll,
    .dup      = (vfs_dup_t)empty,
    .resize   = (vfs_resize_t)dummy,
};

void pipefs_setup() {
    pipefs_id = vfs_regist("pipefs", &pipefs_callbacks, PIEFS_REGISTER_ID, 0x50495045);
    vfs_mkdir("/pipe");
    vfs_node_t node = vfs_open("/pipe");
    if (node == NULL) {
        write_serial_string("pipefs: open /pipe failed\n");
        return;
    }
    if (vfs_mount((const char *)PIEFS_REGISTER_ID, node) == VFS_STATUS_FAILED) {
        write_serial_string("pipefs: mount failed\n");
        return;
    }
}
