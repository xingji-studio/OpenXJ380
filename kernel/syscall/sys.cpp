#include <syscall/syscall.h>
#include <syscall/perm.h>
#include <proto.hpp>
#include <fs/vfs/vfs.h>
#include <fs/fatfs/fatfs.h>
#include <ioctl.h>
#include <cpu/fsgsbase.h>
#include <task/poll.h>
#include <mm/lazyalloc.h>
#include <mm/frame.h>
#include <mm/uaccess.h>
#include <mm/hhdm.h>
#include "../build_settings.h"
#include <net/socket.h>
#include <net/unixsock.h>
#include <pipe.h>
#include <rtc.h>
#include <tty.h>
#include <task/ipc.h>
#include <mutex.h>
#include <math.hpp>
#include <user/user.h>
#include <rng.h>

extern lock_queue *pcb_group_queue;

static constexpr size_t USER_IO_BOUNCE_BYTES = 0x10000UL;
static constexpr size_t USER_PATH_MAX        = 4096UL;
static constexpr size_t USER_IOV_MAX         = 1024UL;
static constexpr size_t USER_POLL_MAX        = FD_SETSIZE;

static void epoll_close_node(vfs_node_t node);

static bool checked_add_size(size_t a, size_t b, size_t *out)
{
    if (out == NULL) return false;
    if (a > (size_t)-1 - b) return false;
    *out = a + b;
    return true;
}

static bool checked_mul_size(size_t a, size_t b, size_t *out)
{
    if (out == NULL) return false;
    if (a != 0 && b > (size_t)-1 / a) return false;
    *out = a * b;
    return true;
}

static bool align_up_u64(uint64_t value, uint64_t align, uint64_t *out)
{
    if (out == NULL || align == 0) return false;
    uint64_t addend = align - 1;
    if (value > UINT64_MAX - addend) return false;
    *out = (value + addend) & ~(align - 1);
    return true;
}

static bool add_u64_i64(uint64_t base, int64_t offset, uint64_t *out)
{
    if (out == NULL) return false;
    if (offset >= 0)
    {
        uint64_t addend = (uint64_t)offset;
        if (base > UINT64_MAX - addend) return false;
        *out = base + addend;
        return true;
    }

    uint64_t subtrahend = (uint64_t)(-(offset + 1)) + 1;
    if (base < subtrahend) return false;
    *out = base - subtrahend;
    return true;
}

static size_t bounded_strlen(const char *str, size_t max_len)
{
    if (str == NULL) return 0;
    size_t len = 0;
    while (len < max_len && str[len] != '\0')
        len++;
    return len;
}

static bool user_bitmap_bytes(uint64_t nfds, size_t *out)
{
    if (out == NULL) return false;
    if (nfds > FD_SETSIZE) return false;
    *out = ((size_t)nfds + 7) / 8;
    return true;
}

static bool process_has_other_live_thread(pcb_t process, tcb_t exiting_thread)
{
    if (process == NULL || process->thread_queue == NULL) return false;

    bool has_live_thread = false;
    spin_lock(&process->thread_queue->lock);
    queue_foreach(process->thread_queue, node)
    {
        tcb_t thread = (tcb_t)node->data;
        if (thread == NULL || thread == exiting_thread) continue;
        if (thread->status == DEATH || thread->status == OUT) continue;
        if (strcmp(thread->name, "Window Message Thread") == 0) continue;

        has_live_thread = true;
        break;
    }
    spin_unlock(&process->thread_queue->lock);
    return has_live_thread;
}

static uint8_t dirent_type_from_node(vfs_node_t node)
{
    if (node->type & file_symlink) return DT_LNK;
    if (node->type & file_none) return DT_REG;
    if (node->type & file_block) return DT_BLK;
    if (node->type & file_stream) return DT_CHR;
    if (node->type & file_socket) return DT_SOCK;
    if (node->type & file_dir) return DT_DIR;
    return DT_UNKNOWN;
}

static page_directory_t *current_user_pagedir()
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL) return NULL;
    return task->parent_group->pagedir;
}

static int copy_string_from_user(char **out, const char *src, size_t max_len)
{
    if (out == NULL) return -EINVAL;
    *out = NULL;
    if (src == NULL || max_len == 0) return -EINVAL;

    page_directory_t *pagedir = current_user_pagedir();
    if (pagedir == NULL) return -EFAULT;

    char *tmp = (char *)malloc(max_len);
    if (tmp == NULL) return -ENOMEM;

    for (size_t i = 0; i < max_len; i++)
    {
        char c = 0;
        if (!copy_from_user_pagedir(pagedir, &c, src + i, sizeof(c)))
        {
            free(tmp);
            return -EFAULT;
        }
        tmp[i] = c;
        if (c == '\0')
        {
            *out = tmp;
            return 0;
        }
    }

    free(tmp);
    return -ENAMETOOLONG;
}

static uint64_t fill_stat_from_node(vfs_node_t node, struct stat *out)
{
    if (node == NULL || out == NULL) return SYSCALL_FAULT_(EINVAL);
    memset(out, 0, sizeof(*out));
    uint64_t ctime_ns = node->createtime;
    uint64_t mtime_ns = node->writetime;
    uint64_t atime_ns = node->readtime;
    if (ctime_ns == 0) ctime_ns = mtime_ns;
    if (mtime_ns == 0) mtime_ns = ctime_ns;
    if (atime_ns == 0) atime_ns = mtime_ns;
    if (ctime_ns == 0 || mtime_ns == 0 || atime_ns == 0)
    {
        uint64_t now = realtime_ns();
        if (ctime_ns == 0) ctime_ns = now;
        if (mtime_ns == 0) mtime_ns = now;
        if (atime_ns == 0) atime_ns = now;
    }
    out->st_gid     = (int)node->group;
    out->st_uid     = (int)node->owner;
    out->st_ino     = node->inode;
    out->st_size    = node->size == (uint64_t)-1 ? 0 : (long long int)node->size;
    out->st_mode    = node->mode | ((node->type & file_symlink)  ? S_IFLNK
                                   : (node->type & file_dir)    ? S_IFDIR
                                   : (node->type & file_block)  ? S_IFBLK
                                   : (node->type & file_socket) ? S_IFSOCK
                                   : (node->type & file_pipe)   ? S_IFIFO
                                   : (node->type & file_none)   ? S_IFREG
                                   : (node->type & file_stream) ? S_IFCHR
                                                                : S_IFREG);
    out->st_nlink   = 1;
    out->st_dev     = (long)node->dev;
    out->st_rdev    = (long)node->rdev;
    out->st_blksize = PAGE_SIZE;
    out->st_blocks  = (node->size == (uint64_t)-1) ? 0 : (node->size + PAGE_SIZE - 1) / PAGE_SIZE;
    out->st_atim = (struct timespec){.tv_sec = atime_ns / 1000000000ULL, .tv_nsec = atime_ns % 1000000000ULL};
    out->st_mtim = (struct timespec){.tv_sec = mtime_ns / 1000000000ULL, .tv_nsec = mtime_ns % 1000000000ULL};
    out->st_ctim = (struct timespec){.tv_sec = ctime_ns / 1000000000ULL, .tv_nsec = ctime_ns % 1000000000ULL};
    return 0;
}

static uint32_t current_uid()
{
    return user_uid(task_effective_user());
}

static uint32_t current_gid()
{
    return user_gid(task_effective_user());
}

static bool current_is_root()
{
    return current_uid() == 0;
}

static bool node_change_permitted(vfs_node_t node, bool owner_change, bool group_change)
{
    if (node == NULL) return false;
    /* Root bypasses the check entirely so the kernel can manage system files.
     * Anything else requires the caller to be the file owner. There is no
     * separate "no-op" path that lets an unprivileged, non-owning caller
     * succeed; without that guard chmod/fchmod could rewrite the mode of any
     * inode in the filesystem. The host-friendly implementation lives in
     * include/syscall/perm.h so it can be unit-tested directly. */
    return xj_node_change_permitted((xj_uid_t)current_uid(),
                                    current_is_root() ? 1 : 0,
                                    (xj_uid_t)node->owner,
                                    owner_change ? 1 : 0,
                                    group_change ? 1 : 0) != 0;
}

static void stamp_node_owner(vfs_node_t node)
{
    if (node == NULL) return;
    node->owner = current_uid();
    node->group = current_gid();
}

static uint64_t stat_kernel_path(const char *path, struct stat *out)
{
    if (path == NULL || out == NULL) return SYSCALL_FAULT_(EINVAL);
    vfs_node_t node = vfs_open(path);
    if (node == NULL) return SYSCALL_FAULT_(ENOENT);
    uint64_t ret = fill_stat_from_node(node, out);
    vfs_close(node);
    return ret;
}

static uint64_t lstat_kernel_path(const char *path, struct stat *out)
{
    if (path == NULL || out == NULL) return SYSCALL_FAULT_(EINVAL);
    vfs_node_t node = vfs_open_no_follow(path);
    if (node == NULL) return SYSCALL_FAULT_(ENOENT);
    uint64_t ret = fill_stat_from_node(node, out);
    vfs_close(node);
    return ret;
}

static uint64_t copy_stat_to_user(struct stat *user_stat, const struct stat *kstat)
{
    if (user_stat == NULL || kstat == NULL) return SYSCALL_FAULT_(EINVAL);
    page_directory_t *pagedir = current_user_pagedir();
    if (!copy_to_user_pagedir(pagedir, user_stat, kstat, sizeof(*kstat))) return SYSCALL_FAULT_(EFAULT);
    return 0;
}

static uint64_t statfs_kernel_node(vfs_node_t node, struct statfs *buf)
{
    if (node == NULL || buf == NULL) return SYSCALL_FAULT_(EINVAL);
    vfs_filesystem_t filesystem = get_filesystem_node(node);
    if (filesystem == NULL) return SYSCALL_FAULT_(EINVAL);

    static constexpr uint64_t kDefaultFsBlockSize = PAGE_SIZE;
    static constexpr uint64_t kDefaultFsBlocks    = (128ULL * 1024ULL * 1024ULL) / kDefaultFsBlockSize;
    static constexpr uint64_t kDefaultFsBavail    = (96ULL * 1024ULL * 1024ULL) / kDefaultFsBlockSize;

    struct statfs kbuf;
    memset(&kbuf, 0, sizeof(kbuf));
    kbuf.f_type    = filesystem->magic;
    kbuf.f_bsize   = node->blksz != 0 ? node->blksz : kDefaultFsBlockSize;
    kbuf.f_frsize  = kbuf.f_bsize;
    kbuf.f_blocks  = kDefaultFsBlocks;
    kbuf.f_bfree   = kDefaultFsBavail;
    kbuf.f_bavail  = kDefaultFsBavail;
    kbuf.f_files   = 1024 * 1024;
    kbuf.f_ffree   = 1024 * 1024;
    kbuf.f_namelen = 255;

    if (strcmp(filesystem->name, "fatfs") == 0)
    {
        struct statfs fatfs_buf;
        if (fatfs_statfs(node, &fatfs_buf) == EOK) kbuf = fatfs_buf;
    }

    if (!copy_to_user_pagedir(current_user_pagedir(), buf, &kbuf, sizeof(kbuf))) return SYSCALL_FAULT_(EFAULT);
    return 0;
}

static int resolve_user_path_at(int dirfd, char *pathname, char **resolved)
{
    if (resolved == NULL) return -EINVAL;
    *resolved = NULL;

    char *kpath = NULL;
    int ret = copy_string_from_user(&kpath, pathname, USER_PATH_MAX);
    if (ret < 0) return ret;

    char *path = at_resolve_pathname(dirfd, kpath);
    free(kpath);
    if (path == NULL) return -ENOENT;

    *resolved = path;
    return 0;
}

static int64_t write_from_user_buffer(fd_file_handle *handle, const uint8_t *buffer, size_t size, bool socket_mode)
{
    if (handle == NULL || buffer == NULL) return -EINVAL;
    if (size == 0) return 0;

    pcb_t process = get_current_task()->parent_group;
    size_t bounce_bytes = MIN(size, USER_IO_BOUNCE_BYTES);
    uint8_t *bounce = (uint8_t *)malloc(bounce_bytes);
    if (bounce == NULL) return -ENOMEM;

    size_t total = 0;
    while (total < size)
    {
        size_t chunk = MIN(size - total, bounce_bytes);
        if (!copy_from_user_pagedir(process->pagedir, bounce, buffer + total, chunk))
        {
            free(bounce);
            return total > 0 ? (int64_t)total : -EFAULT;
        }

        if (socket_mode)
        {
            ssize_t wrote = socket_write(handle->node, bounce, chunk);
            if (wrote < 0)
            {
                free(bounce);
                return total > 0 ? (int64_t)total : wrote;
            }
            total += (size_t)wrote;
            if ((size_t)wrote < chunk) break;
            continue;
        }

        size_t write_offset = (handle->node->type & file_pipe) ? 0 : handle->offset;
        size_t wrote        = vfs_write(handle->node, bounce, write_offset, chunk);
        if ((handle->node->type & file_pipe) && wrote == (size_t)-EPIPE)
        {
            free(bounce);
            signal_send_process(process, SIGPIPE);
            return total > 0 ? (int64_t)total : -EPIPE;
        }
        if (wrote == (size_t)VFS_STATUS_FAILED)
        {
            free(bounce);
            return total > 0 ? (int64_t)total : -EIO;
        }
        if (!(handle->node->type & file_pipe) && handle->node->size != (uint64_t)-1) handle->offset += wrote;
        total += wrote;
        if (wrote < chunk) break;
    }

    free(bounce);
    if (!socket_mode) vfs_update(handle->node);
    return (int64_t)total;
}

static size_t ioctl_arg_size(int request)
{
    switch (request)
    {
    case TCGETS:
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        return sizeof(struct termios);
    case TIOCGWINSZ:
    case TIOCSWINSZ:
        return sizeof(struct winsize);
    case TIOCGPTN:
    case TIOCSPTLCK:
    case TIOCGPTLCK:
    case TIOCGPGRP:
    case TIOCSPGRP:
    case FIONBIO:
    case KDGETMODE:
    case KDSETMODE:
    case KDGKBMODE:
    case KDSKBMODE:
    case VT_OPENQRY:
        return sizeof(int);
    case VT_GETMODE:
    case VT_SETMODE:
        return sizeof(struct vt_mode);
    case FBIOGET_FSCREENINFO:
        return sizeof(struct fb_fix_screeninfo);
    case FBIOGET_VSCREENINFO:
    case FBIOPUT_VSCREENINFO:
    case FBIOPAN_DISPLAY:
        return sizeof(struct fb_var_screeninfo);
    default:
        return 0;
    }
}

static bool ioctl_writes_user(int request)
{
    switch (request)
    {
    case TCGETS:
    case TIOCGWINSZ:
    case TIOCGPTN:
    case TIOCGPTLCK:
    case TIOCGPGRP:
    case KDGETMODE:
    case KDGKBMODE:
    case VT_OPENQRY:
    case VT_GETMODE:
    case FBIOGET_FSCREENINFO:
    case FBIOGET_VSCREENINFO:
    case FBIOPUT_VSCREENINFO:
    case FBIOPAN_DISPLAY:
        return true;
    default:
        return false;
    }
}

static bool ioctl_reads_user(int request)
{
    switch (request)
    {
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
    case TIOCSWINSZ:
    case TIOCSPTLCK:
    case TIOCSPGRP:
    case FIONBIO:
    case KDSETMODE:
    case KDSKBMODE:
    case VT_SETMODE:
    case FBIOPUT_VSCREENINFO:
    case FBIOPAN_DISPLAY:
        return true;
    default:
        return false;
    }
}

static int64_t read_to_user_buffer(fd_file_handle *handle, uint8_t *buffer, size_t size, bool socket_mode,
                                   size_t offset, bool update_offset)
{
    if (handle == NULL || buffer == NULL) return -EINVAL;
    if (size == 0) return 0;

    page_directory_t *pagedir = current_user_pagedir();
    size_t bounce_bytes = MIN(size, USER_IO_BOUNCE_BYTES);
    uint8_t *bounce = (uint8_t *)malloc(bounce_bytes);
    if (bounce == NULL) return -ENOMEM;

    size_t total = 0;
    while (total < size)
    {
        size_t chunk = MIN(size - total, bounce_bytes);
        ssize_t read_now;
        if (socket_mode)
        {
            read_now = socket_read(handle->node, bounce, chunk);
            if (read_now < 0)
            {
                free(bounce);
                return total > 0 ? (int64_t)total : read_now;
            }
        }
        else
        {
            size_t got = vfs_read(handle->node, bounce, offset + total, chunk);
            if (got == (size_t)VFS_STATUS_FAILED)
            {
                free(bounce);
                return total > 0 ? (int64_t)total : -EIO;
            }
            read_now = (ssize_t)got;
        }

        if (read_now == 0) break;
        if (!copy_to_user_pagedir(pagedir, buffer + total, bounce, (size_t)read_now))
        {
            free(bounce);
            return total > 0 ? (int64_t)total : -EFAULT;
        }

        total += (size_t)read_now;
        if ((size_t)read_now < chunk) break;
    }

    free(bounce);
    if (update_offset && handle->node->size != (uint64_t)-1) handle->offset += total;
    return (int64_t)total;
}

static int64_t pipe_read_to_user_buffer(fd_file_handle *handle, uint8_t *buffer, size_t size)
{
    if (handle == NULL || handle->node == NULL || buffer == NULL) return -EINVAL;
    if (size == 0) return 0;

    size_t bounce_bytes = MIN(size, USER_IO_BOUNCE_BYTES);
    uint8_t *bounce = (uint8_t *)malloc(bounce_bytes);
    if (bounce == NULL) return -ENOMEM;

    size_t got = vfs_read(handle->node, bounce, 0, bounce_bytes);
    if (got == (size_t)VFS_STATUS_FAILED)
    {
        free(bounce);
        return -EIO;
    }

    if (got != 0 && !copy_to_user_pagedir(current_user_pagedir(), buffer, bounce, got))
    {
        free(bounce);
        return -EFAULT;
    }

    free(bounce);
    return (int64_t)got;
}

static bool wait4_pid_matches(pcb_t child, long pid)
{
    if (child == NULL) return false;
    return pid == -1 ? true : (long)child->pid == pid;
}

static pcb_t wait4_find_child(pcb_t parent, long pid)
{
    if (parent == NULL || parent->child_pcb == NULL) return NULL;

    pcb_t found = NULL;
    spin_lock(&parent->child_pcb->lock);
    queue_foreach(parent->child_pcb, node)
    {
        pcb_t child = (pcb_t)node->data;
        if (wait4_pid_matches(child, pid))
        {
            found = child;
            break;
        }
    }
    spin_unlock(&parent->child_pcb->lock);

    return found;
}

static pcb_t wait4_find_zombie_child(pcb_t parent, long pid)
{
    if (parent == NULL || parent->child_pcb == NULL) return NULL;

    pcb_t found = NULL;
    spin_lock(&parent->child_pcb->lock);
    queue_foreach(parent->child_pcb, node)
    {
        pcb_t child = (pcb_t)node->data;
        if (child != NULL && child->status == ZOMBIE && wait4_pid_matches(child, pid))
        {
            found = child;
            break;
        }
    }
    spin_unlock(&parent->child_pcb->lock);

    return found;
}

static ipc_message_t wait4_pop_exit_message(pcb_t parent, long pid, bool wait)
{
    if (parent == NULL || parent->ipc_queue == NULL) return NULL;

    while (true)
    {
        size_t remaining = parent->ipc_queue->size;
        for (size_t i = 0; i < remaining; ++i)
        {
            ipc_message_t message = (ipc_message_t)queue_dequeue(parent->ipc_queue);
            if (message == NULL) break;

            if (message->type == IPC_MSG_TYPE_EPID && (pid == -1 || message->pid == pid)) return message;

            message->index = lock_queue_enqueue(parent->ipc_queue, message);
        }

        if (!wait) return NULL;

        scheduler_sleep_ns(1000000ULL);
    }
}

static void wait4_discard_exit_message(pcb_t parent, int pid)
{
    ipc_message_t message = wait4_pop_exit_message(parent, pid, false);
    if (message != NULL) free(message);
}

static uint64_t wait4_reap_child(pcb_t parent, pcb_t child, int *status)
{
    if (parent == NULL || child == NULL) return SYSCALL_FAULT_(ECHILD);

    uint64_t child_pid = child->pid;
    if (status != NULL)
    {
        int wait_status = (child->exit_code & 0xff) << 8;
        if (!copy_to_user_pagedir(parent->pagedir, status, &wait_status, sizeof(wait_status)))
        {
            return SYSCALL_FAULT_(EFAULT);
        }
    }

    wait4_discard_exit_message(parent, child_pid);
    kill_proc(child, child->exit_code, false);
    return child_pid;
}

sys_(exit, int exit_code)
{
    tcb_t exit_thread = get_current_task();
    pcb_t  exit_process = exit_thread != NULL ? exit_thread->parent_group : NULL;
    if (exit_thread == NULL || exit_process == NULL)
    {
        open_interrupt;
        cpu_hlt;
    }

    write_serial_fmt("sys_exit: Thread %s exit with code %d.\n", exit_thread->name, exit_code);

    close_interrupt;
    if (process_has_other_live_thread(exit_process, exit_thread))
    {
        kill_thread(exit_thread);
    }
    else
    {
        exit_process->abnormal_exit = false;
        kill_proc(exit_process, exit_code, true);
    }
    open_interrupt;
    scheduler_yield();
    cpu_hlt;
    return 0;
}

static uint64_t open_kernel_path(char *path0, uint64_t flags, uint64_t mode)
{
    char *normalized_path = strdup(path0);
    if (normalized_path == NULL) return SYSCALL_FAULT_(ENOMEM);

    vfs_node_t node = vfs_open(path0);
    if (node == NULL)
    {
        if (flags & O_CREAT)
        {
            if (mode & O_DIRECTORY) { vfs_mkdir(normalized_path); }
            else
                vfs_mkfile(normalized_path);
            node = vfs_open(normalized_path);
            if (node == NULL)
                goto err;
            else {
                stamp_node_owner(node);
                pcb_t process = get_current_task()->parent_group;
                uint64_t create_mode = mode & ~(process != NULL ? process->umask : 0);
                if ((create_mode & 07777) != 0) node->mode = (uint16_t)(create_mode & 07777);
                goto next;
            }
        }
        else
        err:
            free(normalized_path);
        return SYSCALL_FAULT_(ENOENT);
    }
next:
    vfs_update(node);
    if (!(node->type & (file_dir | file_symlink)) && node->handle == NULL)
    {
        vfs_close(node);
        free(normalized_path);
        return SYSCALL_FAULT_(ENODEV);
    }

    fd_file_handle *fd_handle = (fd_file_handle *)calloc(1, sizeof(fd_file_handle));
    if (fd_handle == NULL)
    {
        vfs_close(node);
        free(normalized_path);
        return SYSCALL_FAULT_(ENOMEM);
    }
    fd_handle->offset = flags & O_APPEND ? node->size : 0;
    fd_handle->node   = node;
    fd_handle->flags  = flags;
    fd_handle->node->flags = flags;
    int index         = (int)queue_enqueue_lowest(get_current_task()->parent_group->file_open, fd_handle);
    fd_handle->fd     = index;
    if (index == -1)
    {
        write_serial_fmt("sys_open: open %s failed.\n", normalized_path);
        vfs_close(node);
        free(fd_handle);
        free(normalized_path);
        return SYSCALL_FAULT_(ENOENT);
    }
    /* Truncate only after the fd has been published, so a queue-full failure
     * does not silently lose the existing file contents. */
    if ((flags & O_TRUNC) && !(node->type & file_dir))
    {
        if (vfs_resize(node, 0) != VFS_STATUS_SUCCESS)
        {
            fd_file_handle *dropped =
                (fd_file_handle *)queue_remove_at(get_current_task()->parent_group->file_open, index);
            if (dropped != NULL) free(dropped);
            vfs_close(node);
            free(normalized_path);
            return SYSCALL_FAULT_(EIO);
        }
    }
    free(normalized_path);
    return index;
}

sys_(open, char *path0, uint64_t flags, uint64_t mode)
{
    if (unlikely(path0 == NULL)) return SYSCALL_FAULT_(EINVAL);

    char *kpath = NULL;
    int ret = copy_string_from_user(&kpath, path0, USER_PATH_MAX);
    if (ret < 0) return SYSCALL_FAULT_((int)-ret);

    char *resolved = vfs_cwd_path_build(kpath);
    free(kpath);
    if (resolved == NULL) return SYSCALL_FAULT_(ENOMEM);

    uint64_t open_ret = open_kernel_path(resolved, flags, mode);
    free(resolved);
    return open_ret;
}

sys_(close, int fd)
{
    if (unlikely(fd < 0)) return SYSCALL_FAULT_(EINVAL);
    fd_file_handle *handle = (fd_file_handle *)queue_remove_at(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL) return SYSCALL_FAULT_(EBADF);
    vfs_close(handle->node);
    free(handle);
    return 0;
}

sys_(close_range, uint32_t first, uint32_t last, uint32_t flags)
{
    if ((flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC)) != 0) return SYSCALL_FAULT_(EINVAL);
    if ((flags & CLOSE_RANGE_UNSHARE) != 0) return SYSCALL_FAULT_(ENOSYS);
    if (first > last) return SYSCALL_FAULT_(EINVAL);

    pcb_t process = get_current_task()->parent_group;
    if (process == NULL || process->file_open == NULL) return SYSCALL_FAULT_(EBADF);

    const bool cloexec = (flags & CLOSE_RANGE_CLOEXEC) != 0;
    lock_queue *queue = process->file_open;
    size_t count = 0;

    spin_lock(&queue->lock);
    for (lock_node *node = queue->head; node != NULL; node = node->next)
    {
        if (node->index >= first && node->index <= last) count++;
    }

    size_t *fds = NULL;
    if (count != 0)
    {
        fds = (size_t *)malloc(sizeof(size_t) * count);
        if (fds == NULL)
        {
            spin_unlock(&queue->lock);
            return SYSCALL_FAULT_(ENOMEM);
        }

        size_t i = 0;
        for (lock_node *node = queue->head; node != NULL; node = node->next)
        {
            if (node->index >= first && node->index <= last) fds[i++] = node->index;
        }
    }
    spin_unlock(&queue->lock);

    for (size_t i = 0; i < count; i++)
    {
        fd_file_handle *handle = (fd_file_handle *)queue_get(queue, fds[i]);
        if (handle == NULL) continue;

        if (cloexec)
        {
            handle->flags |= O_CLOEXEC;
            continue;
        }

        handle = (fd_file_handle *)queue_remove_at(queue, fds[i]);
        if (handle != NULL)
        {
            vfs_close(handle->node);
            free(handle);
        }
    }

    free(fds);
    return 0;
}

sys_(write, int fd, uint8_t *buffer, size_t size)
{
    if (unlikely(fd < 0 || buffer == NULL)) return SYSCALL_FAULT_(EINVAL);
    if (unlikely(size == 0)) return 0;
    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (!handle) return SYSCALL_FAULT_(EBADF);
    if (handle->node->type & file_socket)
    {
        int64_t ret = write_from_user_buffer(handle, buffer, size, true);
        if (ret < 0) return SYSCALL_FAULT_((int)-ret);
        return (uint64_t)ret;
    }
    int64_t ret = write_from_user_buffer(handle, buffer, size, false);
    if (ret < 0) return SYSCALL_FAULT_((int)-ret);
    return (uint64_t)ret;
}

sys_(read, int fd, uint8_t *buffer, size_t size)
{
    if (unlikely(fd < 0 || buffer == NULL)) return SYSCALL_FAULT_(EINVAL);
    if (unlikely(size == 0)) return 0;
    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (!handle) return SYSCALL_FAULT_(EBADF);
    if (handle->node->type & file_socket)
    {
        int64_t ret = read_to_user_buffer(handle, buffer, size, true, 0, false);
        return ret < 0 ? SYSCALL_FAULT_((int)-ret) : (uint64_t)ret;
    }
    if (handle->node->type & file_pipe)
    {
        vfs_update(handle->node);
        if (handle->node->size == 0 && handle->flags & O_NONBLOCK)
        {
            pipe_specific_t *spec = (pipe_specific_t *)handle->node->handle;
            pipe_info_t *pipe = spec != NULL ? spec->info : NULL;
            if (pipe == NULL || pipe->write_fds != 0) return SYSCALL_FAULT_(EWOULDBLOCK);
        }
        int64_t ret = pipe_read_to_user_buffer(handle, buffer, size);
        return ret < 0 ? SYSCALL_FAULT_((int)-ret) : (uint64_t)ret;
    }
    if (handle->node->type & file_pipe && handle->node->size == 0 && handle->flags & O_NONBLOCK)
    {
        return SYSCALL_FAULT_(EWOULDBLOCK);
    }
    if (handle->node->size != (uint64_t)-1)
    {
        if (handle->offset >= handle->node->size)
        {
            if (handle->node->type & file_pipe) { goto pipe; }
            return 0;
        }
    }
read:;
    {
        int64_t read_ret = read_to_user_buffer(handle, buffer, size, false, handle->offset, true);
        return read_ret < 0 ? SYSCALL_FAULT_((int)-read_ret) : (uint64_t)read_ret;
    }
pipe:;
    while (handle->offset >= handle->node->size)
    {
        vfs_update(handle->node);
        scheduler_yield();
    }
    goto read;
}

sys_(sendto, int fd, const void *buffer, size_t size, uint64_t flags, const struct sockaddr *addr, socklen_t addrlen)
{
    if (fd < 0 || (buffer == NULL && size > 0)) return SYSCALL_FAULT_(EINVAL);
    if (addr != NULL && (addrlen < sizeof(struct sockaddr) || addrlen > sizeof(struct sockaddr_in6)))
        return SYSCALL_FAULT_(EINVAL);

    page_directory_t *pagedir = current_user_pagedir();
    if (pagedir == NULL) return SYSCALL_FAULT_(EFAULT);

    struct sockaddr_in6 kaddr;
    const struct sockaddr *kaddr_ptr = NULL;
    if (addr != NULL)
    {
        memset(&kaddr, 0, sizeof(kaddr));
        if (!copy_from_user_pagedir(pagedir, &kaddr, addr, addrlen)) return SYSCALL_FAULT_(EFAULT);
        kaddr_ptr = (const struct sockaddr *)&kaddr;
    }

    if (size == 0) return 0;

    size_t bounce_bytes = MIN(size, USER_IO_BOUNCE_BYTES);
    uint8_t *bounce = (uint8_t *)malloc(bounce_bytes);
    if (bounce == NULL) return SYSCALL_FAULT_(ENOMEM);

    size_t total = 0;
    while (total < size)
    {
        size_t chunk = MIN(size - total, bounce_bytes);
        if (!copy_from_user_pagedir(pagedir, bounce, (const uint8_t *)buffer + total, chunk))
        {
            free(bounce);
            return total > 0 ? total : SYSCALL_FAULT_(EFAULT);
        }

        ssize_t ret = socket_sendto(fd, bounce, chunk, flags, kaddr_ptr, addrlen);
        if (ret < 0)
        {
            free(bounce);
            return total > 0 ? total : SYSCALL_FAULT_((int)-ret);
        }
        total += (size_t)ret;
        if ((size_t)ret < chunk) break;
    }

    free(bounce);
    return total;
}

sys_(recvfrom, int fd, void *buffer, size_t size, uint64_t flags, struct sockaddr *addr, socklen_t *addrlen)
{
    if (fd < 0 || (buffer == NULL && size > 0)) return SYSCALL_FAULT_(EINVAL);
    if (addr != NULL && addrlen == NULL) return SYSCALL_FAULT_(EINVAL);

    page_directory_t *pagedir = current_user_pagedir();
    if (pagedir == NULL) return SYSCALL_FAULT_(EFAULT);

    struct sockaddr_in6 kaddr;
    socklen_t kaddrlen = sizeof(kaddr);
    socklen_t user_addr_capacity = 0;
    if (addrlen != NULL && !copy_from_user_pagedir(pagedir, &kaddrlen, addrlen, sizeof(kaddrlen)))
        return SYSCALL_FAULT_(EFAULT);
    user_addr_capacity = kaddrlen;
    if (kaddrlen > sizeof(kaddr)) kaddrlen = sizeof(kaddr);
    memset(&kaddr, 0, sizeof(kaddr));

    if (size == 0) return 0;

    size_t bounce_bytes = MIN(size, USER_IO_BOUNCE_BYTES);
    uint8_t *bounce = (uint8_t *)malloc(bounce_bytes);
    if (bounce == NULL) return SYSCALL_FAULT_(ENOMEM);

    ssize_t ret = socket_recvfrom(fd, bounce, bounce_bytes, flags,
                                  addr != NULL ? (struct sockaddr *)&kaddr : NULL,
                                  addrlen != NULL ? &kaddrlen : NULL);
    if (ret < 0)
    {
        free(bounce);
        return SYSCALL_FAULT_((int)-ret);
    }

    if (!copy_to_user_pagedir(pagedir, buffer, bounce, (size_t)ret))
    {
        free(bounce);
        return SYSCALL_FAULT_(EFAULT);
    }
    free(bounce);

    if (addr != NULL)
    {
        socklen_t copy_len = MIN(user_addr_capacity, kaddrlen);
        if (copy_len > 0 && !copy_to_user_pagedir(pagedir, addr, &kaddr, copy_len)) return SYSCALL_FAULT_(EFAULT);
    }
    if (addrlen != NULL && !copy_to_user_pagedir(pagedir, addrlen, &kaddrlen, sizeof(kaddrlen)))
        return SYSCALL_FAULT_(EFAULT);
    return (uint64_t)ret;
}

sys_(sendmsg, int fd, const struct msghdr *msg, uint64_t flags)
{
    if (fd < 0 || msg == NULL) return SYSCALL_FAULT_(EINVAL);

    page_directory_t *pagedir = current_user_pagedir();
    if (pagedir == NULL) return SYSCALL_FAULT_(EFAULT);

    struct msghdr kmsg;
    if (!copy_from_user_pagedir(pagedir, &kmsg, msg, sizeof(kmsg))) return SYSCALL_FAULT_(EFAULT);
    if (kmsg.msg_iovlen > USER_IOV_MAX) return SYSCALL_FAULT_(EINVAL);
    if (kmsg.msg_iovlen > 0 && kmsg.msg_iov == NULL) return SYSCALL_FAULT_(EINVAL);
    if (kmsg.msg_name != NULL &&
        (kmsg.msg_namelen < sizeof(struct sockaddr) || kmsg.msg_namelen > sizeof(struct sockaddr_in6)))
        return SYSCALL_FAULT_(EINVAL);

    struct sockaddr_in6 kaddr;
    const struct sockaddr *kaddr_ptr = NULL;
    if (kmsg.msg_name != NULL)
    {
        memset(&kaddr, 0, sizeof(kaddr));
        if (!copy_from_user_pagedir(pagedir, &kaddr, kmsg.msg_name, kmsg.msg_namelen))
            return SYSCALL_FAULT_(EFAULT);
        kaddr_ptr = (const struct sockaddr *)&kaddr;
    }

    size_t iov_bytes = 0;
    if (!checked_mul_size(kmsg.msg_iovlen, sizeof(struct iovec), &iov_bytes)) return SYSCALL_FAULT_(EINVAL);
    struct iovec *kiov = NULL;
    if (iov_bytes > 0)
    {
        kiov = (struct iovec *)malloc(iov_bytes);
        if (kiov == NULL) return SYSCALL_FAULT_(ENOMEM);
        if (!copy_from_user_pagedir(pagedir, kiov, kmsg.msg_iov, iov_bytes))
        {
            free(kiov);
            return SYSCALL_FAULT_(EFAULT);
        }
    }

    uint8_t *bounce = (uint8_t *)malloc(USER_IO_BOUNCE_BYTES);
    if (bounce == NULL && kmsg.msg_iovlen > 0)
    {
        free(kiov);
        return SYSCALL_FAULT_(ENOMEM);
    }

    size_t total = 0;
    for (size_t i = 0; i < kmsg.msg_iovlen; i++)
    {
        const uint8_t *base = (const uint8_t *)kiov[i].iov_base;
        size_t len = kiov[i].iov_len;
        if (base == NULL && len > 0)
        {
            free(bounce);
            free(kiov);
            return total > 0 ? total : SYSCALL_FAULT_(EFAULT);
        }
        size_t done = 0;
        while (done < len)
        {
            size_t chunk = MIN(len - done, USER_IO_BOUNCE_BYTES);
            if (!copy_from_user_pagedir(pagedir, bounce, base + done, chunk))
            {
                free(bounce);
                free(kiov);
                return total > 0 ? total : SYSCALL_FAULT_(EFAULT);
            }
            ssize_t ret = socket_sendto(fd, bounce, chunk, flags, kaddr_ptr, kmsg.msg_namelen);
            if (ret < 0)
            {
                free(bounce);
                free(kiov);
                return total > 0 ? total : SYSCALL_FAULT_((int)-ret);
            }
            total += (size_t)ret;
            done += (size_t)ret;
            if ((size_t)ret < chunk) goto out;
        }
    }

out:
    free(bounce);
    free(kiov);
    return total;
}

sys_(sendmmsg, int fd, struct mmsghdr *mmsg, uint32_t vlen, uint64_t flags)
{
    if (fd < 0 || mmsg == NULL) return SYSCALL_FAULT_(EINVAL);
    if (vlen == 0) return 0;
    if (vlen > USER_IOV_MAX) return SYSCALL_FAULT_(EINVAL);

    page_directory_t *pagedir = current_user_pagedir();
    if (pagedir == NULL) return SYSCALL_FAULT_(EFAULT);

    uint32_t sent = 0;
    for (uint32_t i = 0; i < vlen; i++)
    {
        struct mmsghdr *user_entry = mmsg + i;
        struct mmsghdr kentry;
        if (!copy_from_user_pagedir(pagedir, &kentry, user_entry, sizeof(kentry)))
            return sent > 0 ? sent : SYSCALL_FAULT_(EFAULT);

        uint64_t ret = sys_sendmsg(fd, &user_entry->msg_hdr, flags, 0, 0, 0, regs);
        if ((int64_t)ret < 0) return sent > 0 ? sent : ret;
        if (ret > UINT32_MAX) ret = UINT32_MAX;

        kentry.msg_len = (uint32_t)ret;
        if (!copy_to_user_pagedir(pagedir, &user_entry->msg_len, &kentry.msg_len, sizeof(kentry.msg_len)))
            return sent > 0 ? sent : SYSCALL_FAULT_(EFAULT);

        sent++;
    }

    return sent;
}

sys_(recvmsg, int fd, struct msghdr *msg, uint64_t flags)
{
    if (fd < 0 || msg == NULL) return SYSCALL_FAULT_(EINVAL);

    page_directory_t *pagedir = current_user_pagedir();
    if (pagedir == NULL) return SYSCALL_FAULT_(EFAULT);

    struct msghdr kmsg;
    if (!copy_from_user_pagedir(pagedir, &kmsg, msg, sizeof(kmsg))) return SYSCALL_FAULT_(EFAULT);
    if (kmsg.msg_iovlen > USER_IOV_MAX) return SYSCALL_FAULT_(EINVAL);
    if (kmsg.msg_iovlen > 0 && kmsg.msg_iov == NULL) return SYSCALL_FAULT_(EINVAL);

    size_t iov_bytes = 0;
    if (!checked_mul_size(kmsg.msg_iovlen, sizeof(struct iovec), &iov_bytes)) return SYSCALL_FAULT_(EINVAL);
    struct iovec *kiov = NULL;
    if (iov_bytes > 0)
    {
        kiov = (struct iovec *)malloc(iov_bytes);
        if (kiov == NULL) return SYSCALL_FAULT_(ENOMEM);
        if (!copy_from_user_pagedir(pagedir, kiov, kmsg.msg_iov, iov_bytes))
        {
            free(kiov);
            return SYSCALL_FAULT_(EFAULT);
        }
    }

    struct sockaddr_in6 kaddr;
    socklen_t kaddrlen = sizeof(kaddr);
    socklen_t user_addr_capacity = kmsg.msg_namelen;
    memset(&kaddr, 0, sizeof(kaddr));

    uint8_t *bounce = (uint8_t *)malloc(USER_IO_BOUNCE_BYTES);
    if (bounce == NULL && kmsg.msg_iovlen > 0)
    {
        free(kiov);
        return SYSCALL_FAULT_(ENOMEM);
    }

    size_t total = 0;
    bool got_packet = false;
    for (size_t i = 0; i < kmsg.msg_iovlen; i++)
    {
        uint8_t *base = (uint8_t *)kiov[i].iov_base;
        size_t len = kiov[i].iov_len;
        if (base == NULL && len > 0)
        {
            free(bounce);
            free(kiov);
            return total > 0 ? total : SYSCALL_FAULT_(EFAULT);
        }
        size_t done = 0;
        while (done < len)
        {
            size_t chunk = MIN(len - done, USER_IO_BOUNCE_BYTES);
            socklen_t recv_addrlen = sizeof(kaddr);
            ssize_t ret = socket_recvfrom(fd, bounce, chunk, flags,
                                          kmsg.msg_name != NULL ? (struct sockaddr *)&kaddr : NULL,
                                          kmsg.msg_name != NULL ? &recv_addrlen : NULL);
            if (ret < 0)
            {
                free(bounce);
                free(kiov);
                return total > 0 ? total : SYSCALL_FAULT_((int)-ret);
            }
            got_packet = true;
            if (kmsg.msg_name != NULL) kaddrlen = recv_addrlen;
            if (!copy_to_user_pagedir(pagedir, base + done, bounce, (size_t)ret))
            {
                free(bounce);
                free(kiov);
                return SYSCALL_FAULT_(EFAULT);
            }
            total += (size_t)ret;
            done += (size_t)ret;
            if ((size_t)ret < chunk) goto recv_out;
        }
    }

recv_out:
    free(bounce);
    free(kiov);

    if (kmsg.msg_name != NULL && got_packet)
    {
        socklen_t copy_len = MIN(user_addr_capacity, kaddrlen);
        if (copy_len > 0 && !copy_to_user_pagedir(pagedir, kmsg.msg_name, &kaddr, copy_len))
            return SYSCALL_FAULT_(EFAULT);
        kmsg.msg_namelen = kaddrlen;
    }
    else
    {
        kmsg.msg_namelen = 0;
    }
    kmsg.msg_controllen = 0;
    kmsg.msg_flags = 0;
    if (!copy_to_user_pagedir(pagedir, msg, &kmsg, sizeof(kmsg))) return SYSCALL_FAULT_(EFAULT);
    return total;
}

sys_(setsockopt, int fd, int level, int optname, const void *optval, socklen_t optlen)
{
    if (fd < 0) return SYSCALL_FAULT_(EBADF);
    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL) return SYSCALL_FAULT_(EBADF);
    if (!(handle->node->type & file_socket)) return SYSCALL_FAULT_(ENOTSOCK);
    if (optlen != 0 && optval == NULL) return SYSCALL_FAULT_(EFAULT);
    if (optlen > 4096) return SYSCALL_FAULT_(EINVAL);
    if (optlen != 0 && !user_range_mapped(current_user_pagedir(), optval, optlen)) return SYSCALL_FAULT_(EFAULT);

    (void)level;
    (void)optname;
    return 0;
}

sys_(getsockopt, int fd, int level, int optname, void *optval, socklen_t *optlen)
{
    if (fd < 0) return SYSCALL_FAULT_(EBADF);
    if (optval == NULL || optlen == NULL) return SYSCALL_FAULT_(EFAULT);

    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL) return SYSCALL_FAULT_(EBADF);
    if (!(handle->node->type & file_socket)) return SYSCALL_FAULT_(ENOTSOCK);

    page_directory_t *pagedir = current_user_pagedir();
    socklen_t koptlen = 0;
    if (!copy_from_user_pagedir(pagedir, &koptlen, optlen, sizeof(koptlen))) return SYSCALL_FAULT_(EFAULT);

    int value = 0;
    if (koptlen < sizeof(value)) return SYSCALL_FAULT_(EINVAL);
    int sock_ret = socket_getsockopt(fd, level, optname, &value, &koptlen);
    if (sock_ret < 0) return SYSCALL_FAULT_(-sock_ret);
    if (!copy_to_user_pagedir(pagedir, optval, &value, sizeof(value))) return SYSCALL_FAULT_(EFAULT);
    if (!copy_to_user_pagedir(pagedir, optlen, &koptlen, sizeof(koptlen))) return SYSCALL_FAULT_(EFAULT);
    return 0;
}

static uint64_t socket_name_to_user(int fd, struct sockaddr *addr, socklen_t *addrlen, bool peer)
{
    if (fd < 0) return SYSCALL_FAULT_(EBADF);
    if (addrlen == NULL) return SYSCALL_FAULT_(EFAULT);

    page_directory_t *pagedir = current_user_pagedir();
    socklen_t user_addr_capacity = 0;
    if (!copy_from_user_pagedir(pagedir, &user_addr_capacity, addrlen, sizeof(user_addr_capacity)))
        return SYSCALL_FAULT_(EFAULT);

    struct sockaddr_in6 kaddr;
    memset(&kaddr, 0, sizeof(kaddr));
    socklen_t kaddrlen = MIN(user_addr_capacity, (socklen_t)sizeof(kaddr));

    int ret = peer ? socket_getpeername(fd, (struct sockaddr *)&kaddr, &kaddrlen)
                   : socket_getsockname(fd, (struct sockaddr *)&kaddr, &kaddrlen);
    if (ret < 0) return SYSCALL_FAULT_(-ret);

    if (addr != NULL)
    {
        socklen_t copy_len = MIN(user_addr_capacity, kaddrlen);
        if (copy_len > 0 && !copy_to_user_pagedir(pagedir, addr, &kaddr, copy_len)) return SYSCALL_FAULT_(EFAULT);
    }
    if (!copy_to_user_pagedir(pagedir, addrlen, &kaddrlen, sizeof(kaddrlen))) return SYSCALL_FAULT_(EFAULT);
    return 0;
}

sys_(getsockname, int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    return socket_name_to_user(fd, addr, addrlen, false);
}

sys_(getpeername, int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    return socket_name_to_user(fd, addr, addrlen, true);
}

//要IPC

// sys_(waitpid, int pid, int *status, uint64_t options)

// {
//     pcb_t wait_p;
//     if (get_current_task()->parent_group->child_pcb->size == 0) return SYSCALL_FAULT_(ECHILD);
//     if (pid == -1) goto wait;
//     wait_p = found_pcb(pid);
//     if (wait_p == NULL) return SYSCALL_FAULT_(ECHILD);
// wait:
//     int ret_pid = 0;
//     int   status0 = waitpid(pid, &ret_pid);
//     if (status) *status = status0;
//     return ret_pid;
// }

sys_(wait4, unsigned long pid_value, int *status, int options, void *rusage)
{
    static constexpr int LINUX_WNOHANG    = 1;
    static constexpr int LINUX_WUNTRACED  = 2;
    static constexpr int LINUX_WCONTINUED = 8;

    pcb_t parent = get_current_task()->parent_group;
    long  pid    = (long)(int32_t)(uint32_t)pid_value;
    (void)rusage;
    if (parent == NULL || parent->child_pcb == NULL) return SYSCALL_FAULT_(ECHILD);
    if ((options & ~(LINUX_WNOHANG | LINUX_WUNTRACED | LINUX_WCONTINUED)) != 0) return SYSCALL_FAULT_(EINVAL);
    if (pid < -1 || pid == 0) return SYSCALL_FAULT_(EINVAL);
    if (wait4_find_child(parent, pid) == NULL) return SYSCALL_FAULT_(ECHILD);

    pcb_t zombie = wait4_find_zombie_child(parent, pid);
    if (zombie != NULL) return wait4_reap_child(parent, zombie, status);
    if (options & LINUX_WNOHANG) return 0;

    while (true)
    {
        zombie = wait4_find_zombie_child(parent, pid);
        if (zombie != NULL) return wait4_reap_child(parent, zombie, status);

        if (wait4_find_child(parent, pid) == NULL) return SYSCALL_FAULT_(ECHILD);

        ipc_message_t message = wait4_pop_exit_message(parent, pid, false);
        if (message == NULL)
        {
            __asm__ volatile("pause");
            scheduler_yield();
            continue;
        }

        int exit_code = (int)message->data[0] | ((int)message->data[1] << 8) | ((int)message->data[2] << 16) |
                        ((int)message->data[3] << 24);
        int child_pid = message->pid;
        free(message);

        pcb_t child = wait4_find_child(parent, child_pid);
        if (child == NULL)
        {
            if (wait4_find_child(parent, pid) == NULL) return SYSCALL_FAULT_(ECHILD);
            continue;
        }

        child->exit_code = exit_code;
        child->status    = ZOMBIE;
        return wait4_reap_child(parent, child, status);
    }
}

sys_(sigret)
{
    return signal_return(regs);
}

sys_(getpid)
{
    if (unlikely(arg0 == UINT64_MAX || arg1 == UINT64_MAX || arg2 == UINT64_MAX || arg3 == UINT64_MAX ||
                 arg4 == UINT64_MAX))
        return 1;
    return get_current_task()->parent_group->pid;
}

sys_(socket, int domain, int type, int protocol)
{
    int sock_type = type & SOCK_TYPE_MASK;
    uint64_t fd_flags = 0;
    if (type & SOCK_CLOEXEC) fd_flags |= O_CLOEXEC;
    if (type & SOCK_NONBLOCK) fd_flags |= O_NONBLOCK;

    int ret = socket_open(domain, sock_type, protocol);
    if (ret < 0) return SYSCALL_FAULT_(-ret);
    if (fd_flags != 0)
    {
        fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, ret);
        if (handle != NULL)
        {
            handle->flags |= fd_flags;
            handle->node->flags |= fd_flags;
            if (handle->node->type & file_socket)
            {
                int flags_ret = socket_apply_flags(handle->node, handle->node->flags);
                if (flags_ret < 0) return SYSCALL_FAULT_(-flags_ret);
            }
        }
    }
    return ret;
}

sys_(socketpair, int domain, int type, int protocol, int *sv)
{
    if (sv == NULL) return SYSCALL_FAULT_(EFAULT);
    int sock_type = type & SOCK_TYPE_MASK;
    if (domain == AF_UNIX || domain == AF_LOCAL)
    {
        int fds[2] = {-1, -1};
        int ret = unix_socketpair(sock_type, protocol, fds);
        if (ret < 0) return SYSCALL_FAULT_(-ret);
        page_directory_t *pagedir = current_user_pagedir();
        if (!copy_to_user_pagedir(pagedir, sv, fds, sizeof(fds))) return SYSCALL_FAULT_(EFAULT);
        return 0;
    }
    return SYSCALL_FAULT_(EAFNOSUPPORT);
}

sys_(connect, int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    if (addr == NULL) return SYSCALL_FAULT_(EINVAL);
    if (addrlen < sizeof(struct sockaddr) || addrlen > sizeof(struct sockaddr_in6)) return SYSCALL_FAULT_(EINVAL);
    struct sockaddr_in6 kaddr;
    memset(&kaddr, 0, sizeof(kaddr));
    if (!copy_from_user_pagedir(current_user_pagedir(), &kaddr, addr, addrlen)) return SYSCALL_FAULT_(EFAULT);
    int ret = socket_connect(fd, (const struct sockaddr *)&kaddr, addrlen);
    if (ret < 0) return SYSCALL_FAULT_(-ret);
    return ret;
}

sys_(accept, int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    socklen_t kaddrlen = sizeof(struct sockaddr_in6);
    if (addrlen != NULL && !copy_from_user_pagedir(current_user_pagedir(), &kaddrlen, addrlen, sizeof(kaddrlen)))
        return SYSCALL_FAULT_(EFAULT);
    if (addr != NULL && addrlen == NULL) return SYSCALL_FAULT_(EINVAL);
    if (kaddrlen > sizeof(struct sockaddr_in6)) kaddrlen = sizeof(struct sockaddr_in6);

    struct sockaddr_in6 kaddr;
    memset(&kaddr, 0, sizeof(kaddr));
    int ret = socket_accept(fd, addr ? (struct sockaddr *)&kaddr : NULL, addr ? &kaddrlen : NULL);
    if (ret < 0) return SYSCALL_FAULT_(-ret);
    if (addr != NULL && !copy_to_user_pagedir(current_user_pagedir(), addr, &kaddr, kaddrlen))
        return SYSCALL_FAULT_(EFAULT);
    if (addrlen != NULL && !copy_to_user_pagedir(current_user_pagedir(), addrlen, &kaddrlen, sizeof(kaddrlen)))
        return SYSCALL_FAULT_(EFAULT);
    return ret;
}


sys_(stat, char *fn, struct stat *buf)
{
    if (unlikely(fn == NULL || buf == NULL)) return SYSCALL_FAULT_(EINVAL);

    char *user_path = NULL;
    int ret = copy_string_from_user(&user_path, fn, USER_PATH_MAX);
    if (ret < 0) return SYSCALL_FAULT_((int)-ret);

    char *path = vfs_cwd_path_build(user_path);
    free(user_path);
    if (path == NULL) return SYSCALL_FAULT_(ENOMEM);

    struct stat kstat;
    uint64_t stat_ret = stat_kernel_path(path, &kstat);
    free(path);
    if ((int64_t)stat_ret < 0) return stat_ret;
    return copy_stat_to_user(buf, &kstat);
}


sys_(arch_prctl, uint64_t code, uint64_t addr)
{
    tcb_t thread = get_current_task();
    if (thread == NULL) return SYSCALL_FAULT_(ESRCH);
    switch (code)
    {
    case ARCH_SET_FS:
        /* FS base is consumed by user-mode instructions. Reject non-canonical
         * and kernel addresses before preserving it in the task context. */
        if (addr > 0x00007fffffffffffULL || check_user_overflow(addr, 1))
            return SYSCALL_FAULT_(EINVAL);
        thread->fs_base = addr;
        write_fsbase(thread->fs_base);
        break;
    case ARCH_GET_FS:
        if (addr == 0 || !copy_to_user_pagedir(current_user_pagedir(), (void *)addr, &thread->fs_base,
                                                sizeof(thread->fs_base)))
            return SYSCALL_FAULT_(EFAULT);
        break;
    // case ARCH_SET_GS:
    //     thread->gs_base = addr;
    //     write_gsbase(thread->gs_base);
    //     break;
    // case ARCH_GET_GS: return thread->gs_base;
    default: return SYSCALL_FAULT;
    }
    return 0;
}

#define PR_SET_NAME 15
#define PR_GET_NAME 16

sys_(prctl, int option, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    tcb_t task = get_current_task();
    switch (option) {
    case PR_SET_NAME:
        if (task == NULL) return SYSCALL_FAULT_(ESRCH);
        if (a2 == 0) return SYSCALL_FAULT_(EFAULT);
        {
            page_directory_t *pagedir = current_user_pagedir();
            char name[32];
            if (!copy_from_user_pagedir(pagedir, name, (void *)a2, sizeof(name) - 1))
                return SYSCALL_FAULT_(EFAULT);
            name[sizeof(name) - 1] = '\0';
            strncpy(task->name, name, sizeof(task->name) - 1);
            if (task->parent_group != NULL)
                strncpy(task->parent_group->name, name, sizeof(task->parent_group->name) - 1);
        }
        return 0;
    case PR_GET_NAME:
        if (task == NULL) return SYSCALL_FAULT_(ESRCH);
        if (a2 == 0) return SYSCALL_FAULT_(EFAULT);
        {
            page_directory_t *pagedir = current_user_pagedir();
            if (!copy_to_user_pagedir(pagedir, (void *)a2, task->name, sizeof(task->name)))
                return SYSCALL_FAULT_(EFAULT);
        }
        return 0;
    default:
        return 0;
    }
}

sys_(yield)
{
    scheduler_yield();
    return 0;
}

sys_(uname, struct utsname *utsname)
{
    if (unlikely(utsname == NULL)) return SYSCALL_FAULT_(EINVAL);
    struct utsname kname;
    memset(&kname, 0, sizeof(kname));
    char sysname[] = KN_VERSION;
    char machine[] = "x86_64";
    char release[] = "6.6.30"; // byd 咱就非要检查吗
    char version[] = OS_VERSION;
    memcpy(kname.sysname, sysname, sizeof(sysname));
    memcpy(kname.nodename, "localhost", 10);
    memcpy(kname.release, release, sizeof(release));
    memcpy(kname.version, version, sizeof(version));
    memcpy(kname.machine, machine, sizeof(machine));
    if (!copy_to_user_pagedir(current_user_pagedir(), utsname, &kname, sizeof(kname))) return SYSCALL_FAULT_(EFAULT);
    return 0;
}
void scheduler_nano_sleep(uint64_t nano) {
    scheduler_sleep_ns(nano);
}

static uint64_t clock_now_ns_for_sleep(int clockid)
{
    switch (clockid)
    {
    case 0:
        return realtime_ns();
    case 1:
    case 4:
    case 6:
    case 7:
        return bootNanoTime();
    default:
        return UINT64_MAX;
    }
}

static uint64_t timespec_to_ns_checked(const struct timespec *ts, bool *ok)
{
    if (ok != NULL) *ok = false;
    if (ts == NULL) return 0;
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L) return 0;
    if ((uint64_t)ts->tv_sec > (UINT64_MAX - (uint64_t)ts->tv_nsec) / 1000000000ULL) return 0;
    if (ok != NULL) *ok = true;
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

sys_(nano_sleep, void *time_handle)
{
    struct timespec k_req;
    if (unlikely(time_handle == NULL)) return SYSCALL_FAULT_(EINVAL);
    if (!copy_from_user_pagedir(get_current_task()->parent_group->pagedir, &k_req, time_handle, sizeof(k_req)))
    {
        return SYSCALL_FAULT_(EFAULT);
    }
    bool ok = false;
    uint64_t nsec = timespec_to_ns_checked(&k_req, &ok);
    if (!ok) return SYSCALL_FAULT_(EINVAL);
    scheduler_nano_sleep(nsec);
    return 0;
}

sys_(clock_nanosleep, int clockid, int flags, const struct timespec *request, struct timespec *remain)
{
    (void)remain;
    constexpr int TIMER_ABSTIME = 1;
    if (request == NULL) return SYSCALL_FAULT_(EFAULT);
    if ((flags & ~TIMER_ABSTIME) != 0) return SYSCALL_FAULT_(EINVAL);

    uint64_t now = clock_now_ns_for_sleep(clockid);
    if (now == UINT64_MAX) return SYSCALL_FAULT_(EINVAL);

    struct timespec k_req;
    if (!copy_from_user_pagedir(current_user_pagedir(), &k_req, request, sizeof(k_req)))
        return SYSCALL_FAULT_(EFAULT);

    bool ok = false;
    uint64_t target_or_delta = timespec_to_ns_checked(&k_req, &ok);
    if (!ok) return SYSCALL_FAULT_(EINVAL);

    uint64_t sleep_ns = target_or_delta;
    if (flags & TIMER_ABSTIME) sleep_ns = target_or_delta > now ? target_or_delta - now : 0;
    scheduler_nano_sleep(sleep_ns);
    return 0;
}

sys_(ioctl, int fd, int options, void *arg2)
{
    if (unlikely(fd < 0)) return SYSCALL_FAULT_(EINVAL);
    fd_file_handle *handle =  (fd_file_handle*)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL) return SYSCALL_FAULT_(EBADF);

    size_t arg_size = ioctl_arg_size(options);
    void *karg = arg2;
    if (arg_size > 0)
    {
        if (arg2 == NULL) return SYSCALL_FAULT_(EFAULT);
        karg = calloc(1, arg_size);
        if (karg == NULL) return SYSCALL_FAULT_(ENOMEM);
        if (ioctl_reads_user(options) && !copy_from_user_pagedir(current_user_pagedir(), karg, arg2, arg_size))
        {
            free(karg);
            return SYSCALL_FAULT_(EFAULT);
        }
    }
    else if (arg2 == NULL)
    {
        karg = NULL;
    }

    if ((handle->node->type & file_socket) && options == FIONBIO)
    {
        int nonblock = 0;
        if (karg == NULL)
        {
            return SYSCALL_FAULT_(EFAULT);
        }
        nonblock = *(int *)karg;
        if (nonblock) {
            handle->flags |= O_NONBLOCK;
            handle->node->flags |= O_NONBLOCK;
        } else {
            handle->flags &= ~O_NONBLOCK;
            handle->node->flags &= ~O_NONBLOCK;
        }
        int ret = socket_apply_flags(handle->node, handle->node->flags);
        if (arg_size > 0) free(karg);
        return ret < 0 ? SYSCALL_FAULT_(-ret) : 0;
    }

    int ret = vfs_ioctl(handle->node, options, karg);
    if (ret == EOK && arg_size > 0 && ioctl_writes_user(options) &&
        !copy_to_user_pagedir(current_user_pagedir(), arg2, karg, arg_size))
    {
        free(karg);
        return SYSCALL_FAULT_(EFAULT);
    }
    if (arg_size > 0) free(karg);
    return ret;
}

sys_(writev, int fd, struct iovec *iov, int iovcnt)
{
    if (unlikely(fd < 0 || iov == NULL)) return SYSCALL_FAULT_(EINVAL);
    if (unlikely(iovcnt < 0 || (size_t)iovcnt > USER_IOV_MAX)) return SYSCALL_FAULT_(EINVAL);
    if (iovcnt == 0) return 0;
    size_t iov_bytes = 0;
    if (!checked_mul_size((size_t)iovcnt, sizeof(struct iovec), &iov_bytes) ||
        !user_range_mapped(current_user_pagedir(), iov, iov_bytes))
    {
        return SYSCALL_FAULT_(EFAULT);
    }
    page_directory_t *pagedir = get_current_task()->parent_group->pagedir;
    fd_file_handle *handle =  (fd_file_handle*)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL) return SYSCALL_FAULT_(EBADF);
    if (handle->node->type & file_socket)
    {
        size_t total = 0;
        for (int i = 0; i < iovcnt; i++)
        {
            struct iovec kiov;
            if (!copy_from_user_pagedir(pagedir, &kiov, &iov[i], sizeof(kiov)))
            {
                return total > 0 ? total : SYSCALL_FAULT_(EFAULT);
            }
            int64_t ret = write_from_user_buffer(handle, (const uint8_t *)kiov.iov_base, kiov.iov_len, true);
            if (ret < 0)
            {
                if (total > 0) return total;
                return SYSCALL_FAULT_((int)-ret);
            }
            if (!checked_add_size(total, (size_t)ret, &total)) return SYSCALL_FAULT_(EINVAL);
            if ((size_t)ret < kiov.iov_len) break;
        }
        return total;
    }
    size_t          total  = 0;
    for (int i = 0; i < iovcnt; i++)
    {
        struct iovec kiov;
        if (!copy_from_user_pagedir(pagedir, &kiov, &iov[i], sizeof(kiov)))
        {
            return total > 0 ? total : SYSCALL_FAULT_(EFAULT);
        }
        int64_t status = write_from_user_buffer(handle, (const uint8_t *)kiov.iov_base, kiov.iov_len, false);
        if (status < 0) return total > 0 ? total : SYSCALL_FAULT_((int)-status);
        if (!checked_add_size(total, (size_t)status, &total)) return SYSCALL_FAULT_(EINVAL);
        if ((size_t)status < kiov.iov_len) break;
    }
    return total;
}

sys_(readv, int fd, struct iovec *iov, int iovcnt0)
{
    if (unlikely(fd < 0 || iov == NULL)) return SYSCALL_FAULT_(EINVAL);
    if (unlikely(iovcnt0 < 0 || (size_t)iovcnt0 > USER_IOV_MAX)) return SYSCALL_FAULT_(EINVAL);
    if (iovcnt0 == 0) return 0;
    size_t          iovcnt = (size_t)iovcnt0;
    fd_file_handle *handle =  (fd_file_handle*)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL) return SYSCALL_FAULT_(EBADF);

    size_t iov_bytes = 0;
    if (!checked_mul_size(iovcnt, sizeof(struct iovec), &iov_bytes)) return SYSCALL_FAULT_(EFAULT);
    struct iovec *kiov = (struct iovec *)malloc(iov_bytes);
    if (kiov == NULL) return SYSCALL_FAULT_(ENOMEM);
    if (!copy_from_user_pagedir(current_user_pagedir(), kiov, iov, iov_bytes))
    {
        free(kiov);
        return SYSCALL_FAULT_(EFAULT);
    }

    size_t total = 0;
    for (size_t i = 0; i < iovcnt; i++)
    {
        if (kiov[i].iov_len == 0) continue;
        uint64_t ret = sys_read(fd, (uint8_t *)kiov[i].iov_base, kiov[i].iov_len, 0, 0, 0, regs);
        if ((int64_t)ret < 0)
        {
            free(kiov);
            return total > 0 ? total : ret;
        }
        if (!checked_add_size(total, (size_t)ret, &total))
        {
            free(kiov);
            return SYSCALL_FAULT_(EINVAL);
        }
        if ((size_t)ret < kiov[i].iov_len) break;
    }

    free(kiov);
    return total;
}
static mutex_t mm_op_lock = {SPIN_INIT, MUTEX_UNLOCKED, NULL, NULL, 0, false};

static void mm_op_lock_acquire()
{
    mutex_lock(&mm_op_lock);
}

static void mm_op_lock_release()
{
    mutex_unlock(&mm_op_lock);
}

static bool mapped_page_intersects(page_directory_t *pagedir, uint64_t start, uint64_t length)
{
    if (pagedir == NULL || length == 0) return true;

    for (uint64_t offset = 0; offset < length; offset += PAGE_SIZE)
    {
        if (translate_address(pagedir, start + offset) != 0) return true;
    }
    return false;
}

static bool map_zero_user_range(page_directory_t *pagedir, uint64_t start, uint64_t length, uint64_t flags)
{
    if (pagedir == NULL) return false;

    for (uint64_t page = start; page < start + length; page += PAGE_SIZE) {
        uint64_t frame = alloc_frames(1);
        if (frame == 0) {
            unmap_page_range(pagedir, start, page - start);
            return false;
        }
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        page_map_to(pagedir, page, frame, flags | PTE_FRAME_ALLOCATED);
    }
    return true;
}

static bool user_range_available(pcb_t process, uint64_t start, uint64_t length)
{
    if (process == NULL || process->pagedir == NULL) return false;
    if (check_user_overflow(start, length)) return false;
    if (vma_find_intersection(&process->vma_manager, start, start + length)) return false;
    if (mapped_page_intersects(process->pagedir, start, length)) return false;
    return true;
}

static uint64_t find_user_mmap_range(pcb_t process, uint64_t hint, uint64_t length)
{
    if (process == NULL || length == 0) return 0;

    uint64_t start_addr = hint >= USER_MMAP_START ? (hint & PAGE_MASK) : USER_MMAP_START;
    if (start_addr >= USER_BRK_START || length > USER_BRK_START - start_addr) return 0;

    uint64_t limit = USER_BRK_START - length;
    while (start_addr <= limit)
    {
        if (user_range_available(process, start_addr, length)) return start_addr;
        if (start_addr > limit - PAGE_SIZE) return 0;
        start_addr += PAGE_SIZE;
    }

    return 0;
}

static uint64_t prot_to_pte_flags(uint64_t prot)
{
    uint64_t flags = PTE_USER;
    if (prot != PROT_NONE) {
        flags |= PTE_PRESENT;
        if (prot & PROT_WRITE) flags |= PTE_WRITEABLE;
        if (!(prot & PROT_EXEC)) flags |= PTE_NO_EXECUTE;
    }
    return flags;
}

static void update_lazy_pte_flags(pcb_t process, uint64_t start, uint64_t end, uint64_t pte_flags)
{
    if (process == NULL || process->virt_queue == NULL) return;

    while (true) {
        mm_virtual_page_t old;
        memset(&old, 0, sizeof(old));
        bool found = false;
        spin_lock(&process->virt_queue->lock);
        queue_foreach(process->virt_queue, node)
        {
            mm_virtual_page_t *candidate = (mm_virtual_page_t *)node->data;
            if (candidate == NULL) continue;

            uint64_t vpage_start = candidate->start;
            uint64_t vpage_end   = candidate->start + candidate->count * PAGE_SIZE;
            if (vpage_end > start && vpage_start < end && candidate->pte_flags != pte_flags) {
                old = *candidate;
                found = true;
                break;
            }
        }
        spin_unlock(&process->virt_queue->lock);

        if (!found) break;

        uint64_t vpage_start = old.start;
        uint64_t vpage_end   = old.start + old.count * PAGE_SIZE;

        uint64_t overlap_start = max(vpage_start, start);
        uint64_t overlap_end   = min(vpage_end, end);
        if (overlap_start == vpage_start && overlap_end == vpage_end) {
            mm_virtual_page_t *removed = (mm_virtual_page_t *)queue_remove_at(process->virt_queue, old.index);
            if (removed == NULL) continue;
            removed->pte_flags = pte_flags;
            removed->index = queue_enqueue(process->virt_queue, removed);
            continue;
        }

        mm_virtual_page_t *removed = (mm_virtual_page_t *)queue_remove_at(process->virt_queue, old.index);
        if (removed == NULL) continue;
        free(removed);

        if (vpage_start < overlap_start) {
            mm_virtual_page_t *left = (mm_virtual_page_t *)malloc(sizeof(mm_virtual_page_t));
            if (left != NULL) {
                *left = old;
                left->start = vpage_start;
                left->count = (overlap_start - vpage_start) / PAGE_SIZE;
                left->index = queue_enqueue(process->virt_queue, left);
            }
        }

        mm_virtual_page_t *middle = (mm_virtual_page_t *)malloc(sizeof(mm_virtual_page_t));
        if (middle != NULL) {
            *middle = old;
            middle->start = overlap_start;
            middle->count = (overlap_end - overlap_start) / PAGE_SIZE;
            middle->pte_flags = pte_flags;
            middle->index = queue_enqueue(process->virt_queue, middle);
        }

        if (overlap_end < vpage_end) {
            mm_virtual_page_t *right = (mm_virtual_page_t *)malloc(sizeof(mm_virtual_page_t));
            if (right != NULL) {
                *right = old;
                right->start = overlap_end;
                right->count = (vpage_end - overlap_end) / PAGE_SIZE;
                right->index = queue_enqueue(process->virt_queue, right);
            }
        }
    }
}

static bool heap_growth_range_available(pcb_t process, uint64_t start, uint64_t end)
{
    if (process == NULL || start >= end) return true;

    vma_t *vma = process->vma_manager.vma_list;
    while (vma != NULL)
    {
        if (!(end <= vma->vm_start || start >= vma->vm_end)) return false;
        vma = vma->vm_next;
    }
    return !mapped_page_intersects(process->pagedir, start, end - start);
}

static vma_t *heap_vma_at_end(pcb_t process, uint64_t end)
{
    if (process == NULL) return NULL;

    vma_t *vma = process->vma_manager.vma_list;
    while (vma != NULL)
    {
        if (vma->vm_end == end && vma->vm_type == VMA_TYPE_ANON &&
            vma->vm_name != NULL && strcmp(vma->vm_name, "[heap]") == 0 &&
            (vma->vm_flags & (VMA_READ | VMA_WRITE | VMA_ANON)) == (VMA_READ | VMA_WRITE | VMA_ANON))
        {
            return vma;
        }
        vma = vma->vm_next;
    }

    return NULL;
}

static bool heap_insert_range(pcb_t process, uint64_t start, uint64_t end)
{
    if (process == NULL || start >= end) return true;

    vma_t *tail = heap_vma_at_end(process, start);
    if (tail != NULL)
    {
        tail->vm_end = end;
        process->vma_manager.vm_used += end - start;
        return true;
    }

    vma_t *heap_vma = vma_alloc();
    if (heap_vma == NULL) return false;

    heap_vma->vm_start = start;
    heap_vma->vm_end = end;
    heap_vma->vm_flags = VMA_READ | VMA_WRITE | VMA_ANON;
    heap_vma->vm_type = VMA_TYPE_ANON;
    heap_vma->vm_name = strdup("[heap]");
    if (heap_vma->vm_name == NULL || vma_insert(&process->vma_manager, heap_vma) != 0) {
        vma_free(heap_vma);
        return false;
    }
    return true;
}

uint64_t sys_brk(uint64_t brk) {
    pcb_t process = get_current_task()->parent_group;

    // Linux 语义里 brk(0) 用来查询当前 program break，不能返回堆起点。
    if (!brk) {
        return process->brk_current;
    }

    // 用户堆只能在装载器预留的 brk 区间里伸缩，越界时保持原 break 不变。
    if (brk < process->brk_start || brk > process->brk_end) {
        return process->brk_current;
    }

    if (brk == process->brk_current) {
        return process->brk_current;
    }

    uint64_t old_map_end = process->brk_start;
    uint64_t new_map_end = process->brk_start;
    if (process->brk_current > process->brk_start &&
        !align_up_u64(process->brk_current, PAGE_SIZE, &old_map_end))
        return process->brk_current;
    if (brk > process->brk_start && !align_up_u64(brk, PAGE_SIZE, &new_map_end)) return process->brk_current;

    mm_op_lock_acquire();

    if (new_map_end > old_map_end) {
        if (!heap_growth_range_available(process, old_map_end, new_map_end)) {
            mm_op_lock_release();
            return process->brk_current;
        }

        if (!heap_insert_range(process, old_map_end, new_map_end)) {
            mm_op_lock_release();
            return process->brk_current;
        }

        if (!lazy_infoalloc(process, old_map_end, new_map_end - old_map_end,
                            PTE_USER | PTE_PRESENT | PTE_WRITEABLE | PTE_NO_EXECUTE, 0))
        {
            vma_unmap_range(&process->vma_manager, old_map_end, new_map_end);
            mm_op_lock_release();
            return process->brk_current;
        }
    }
    else if (new_map_end < old_map_end) {
        vma_unmap_range(&process->vma_manager, new_map_end, old_map_end);
        unmap_virtual_page(process, new_map_end, old_map_end - new_map_end);
        unmap_page_range(get_current_directory(), new_map_end, old_map_end - new_map_end);
    }

    process->brk_current = brk;

    mm_op_lock_release();

    return process->brk_current;
}

sys_(mmap, uint64_t addr, size_t length, uint64_t prot, uint64_t flags, int fd,
         uint64_t offset) {

    addr = addr & (~(PAGE_SIZE - 1));

    uint64_t aligned_len = 0;
    if (!align_up_u64(length, PAGE_SIZE, &aligned_len)) { return SYSCALL_FAULT_(EINVAL); }

    if (check_user_overflow(addr, aligned_len)) { return -EFAULT; }

    if (aligned_len == 0) { return SYSCALL_FAULT_(EINVAL); }
    pcb_t process = get_current_task()->parent_group;

    page_directory_t *pagedir = process->pagedir;
    vma_manager_t *mgr        = &process->vma_manager;
    uint64_t       start_addr = 0;
    uint64_t       end_addr   = 0;
    if (flags & MAP_FIXED) {
        if (!addr) return SYSCALL_FAULT_(EINVAL);

        start_addr = addr;
        if (check_user_overflow(start_addr, aligned_len)) return SYSCALL_FAULT_(EINVAL);
        end_addr = start_addr + aligned_len;
    } else {
        if (process->mmap_start < USER_MMAP_START || process->mmap_start >= USER_BRK_START)
            process->mmap_start = USER_MMAP_START;
        uint64_t search_start = addr ? addr : process->mmap_start;
        if (addr) {
            start_addr = addr;
            if (start_addr < USER_MMAP_START || start_addr >= USER_BRK_START ||
                aligned_len > USER_BRK_START - start_addr) {
                return SYSCALL_FAULT_(ENOMEM);
            }
            end_addr = start_addr + aligned_len;
            if (!user_range_available(process, start_addr, aligned_len)) {
                return SYSCALL_FAULT_(ENOMEM);
            }
        } else {
            start_addr = find_user_mmap_range(process, search_start, aligned_len);
            if (start_addr == 0 && search_start != USER_MMAP_START)
                start_addr = find_user_mmap_range(process, USER_MMAP_START, aligned_len);
            if (start_addr == 0) return SYSCALL_FAULT_(ENOMEM);
            end_addr = start_addr + aligned_len;
        }
    }

    fd_file_handle *file_handle = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        file_handle = (fd_file_handle *)queue_get(process->file_open, fd);
        if (file_handle == NULL || file_handle->node == NULL) return SYSCALL_FAULT_(EBADF);
        if (file_handle->node->type == file_dir) return SYSCALL_FAULT_(EACCES);
    }

    mm_op_lock_acquire();

    vma_t *vma = vma_alloc();
    if (!vma) {
        mm_op_lock_release();
        return SYSCALL_FAULT_(ENOMEM);
    }

    vma->vm_start = start_addr;
    vma->vm_end   = end_addr;
    vma->vm_flags = 0;

    if (prot & PROT_READ) vma->vm_flags |= VMA_READ;
    if (prot & PROT_WRITE) vma->vm_flags |= VMA_WRITE;
    if (prot & PROT_EXEC) vma->vm_flags |= VMA_EXEC;
    if (flags & MAP_SHARED) vma->vm_flags |= VMA_SHARED;

    if (flags & MAP_ANONYMOUS) {
        vma->vm_type   = VMA_TYPE_ANON;
        vma->vm_flags |= VMA_ANON;
        vma->vm_fd     = -1;
    } else {
        vma->vm_type   = VMA_TYPE_FILE;
        vma->vm_fd     = fd;
        vma->vm_offset = (int64_t)offset;
    }

    if (flags & MAP_FIXED) {
        vma_unmap_range(mgr, start_addr, end_addr);
        unmap_virtual_page(process, start_addr, aligned_len);
        unmap_page_range(pagedir, start_addr, aligned_len);
    }

    if (vma_insert(mgr, vma) != 0) {
        vma_free(vma);
        mm_op_lock_release();
        return SYSCALL_FAULT_(ENOMEM);
    }

    if (!(flags & MAP_FIXED) && process->mmap_start < end_addr) process->mmap_start = end_addr;

    if (!(flags & MAP_ANONYMOUS)) {
        uint64_t ret = (uint64_t)vfs_map(file_handle->node, start_addr, aligned_len, prot, flags, offset);
        mm_op_lock_release();
        return ret;
    }

    uint64_t pt_flags = PTE_USER | PTE_PRESENT | PTE_WRITEABLE;

    if (prot != PROT_NONE) {
        if (prot & PROT_READ) pt_flags |= PTE_PRESENT;
        if (prot & PROT_WRITE) pt_flags |= PTE_WRITEABLE;
        if (!(prot & PROT_EXEC)) pt_flags |= PTE_NO_EXECUTE;
    }

    if (!lazy_infoalloc(process, start_addr, aligned_len, pt_flags, flags)) {
        vma_unmap_range(mgr, start_addr, end_addr);
        mm_op_lock_release();
        return SYSCALL_FAULT_(ENOMEM);
    }
    mm_op_lock_release();

    return start_addr;

    return addr;
}

sys_(madvise, uint64_t addr, size_t length, int advice)//掩饰一下啦
{
    (void)advice;
    if (length == 0) return 0;
    if ((addr & (PAGE_SIZE - 1)) != 0) return SYSCALL_FAULT_(EINVAL);

    uint64_t aligned_len = 0;
    if (!align_up_u64(length, PAGE_SIZE, &aligned_len)) return SYSCALL_FAULT_(EINVAL);
    if (check_user_overflow(addr, aligned_len)) return SYSCALL_FAULT_(EINVAL);

    return 0;
}

sys_(mprotect, uint64_t addr, size_t length, uint64_t prot)
{
    if (length == 0) return 0;

    addr = addr & (~(PAGE_SIZE - 1));
    uint64_t aligned_len = 0;
    if (!align_up_u64(length, PAGE_SIZE, &aligned_len)) return SYSCALL_FAULT_(EINVAL);
    if (check_user_overflow(addr, aligned_len)) return SYSCALL_FAULT_(EFAULT);

    uint64_t start = addr;
    uint64_t end   = addr + aligned_len;
    pcb_t process = get_current_task()->parent_group;
    vma_manager_t *mgr = &process->vma_manager;
    uint64_t pte_flags = prot_to_pte_flags(prot);

    mm_op_lock_acquire();

    uint64_t cursor = start;
    while (cursor < end) {
        vma_t *vma = vma_find(mgr, cursor);
        if (vma == NULL) {
            mm_op_lock_release();
            return SYSCALL_FAULT_(ENOMEM);
        }
        cursor = vma->vm_end < end ? vma->vm_end : end;
    }

    cursor = start;
    while (cursor < end) {
        vma_t *vma = vma_find(mgr, cursor);
        if (vma == NULL) break;

        if (vma->vm_start < start) {
            vma_split(vma, start);
            vma = vma_find(mgr, start);
        }
        if (vma != NULL && vma->vm_end > end) {
            vma_split(vma, end);
        }
        if (vma != NULL) {
            vma->vm_flags &= ~(VMA_READ | VMA_WRITE | VMA_EXEC);
            if (prot & PROT_READ) vma->vm_flags |= VMA_READ;
            if (prot & PROT_WRITE) vma->vm_flags |= VMA_WRITE;
            if (prot & PROT_EXEC) vma->vm_flags |= VMA_EXEC;
            cursor = vma->vm_end;
        }
    }

    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        if (translate_address(process->pagedir, page) != 0)
            page_table_update_flags(process->pagedir, page, pte_flags);
    }
    update_lazy_pte_flags(process, start, end, pte_flags);

    mm_op_lock_release();
    return 0;
}

sys_(munmap, uint64_t addr, size_t size) {
    if (size == 0) return 0;

    addr = addr & (~(PAGE_SIZE - 1));
    uint64_t aligned_size = 0;
    if (!align_up_u64(size, PAGE_SIZE, &aligned_size)) return SYSCALL_FAULT_(EINVAL);
    size = (size_t)aligned_size;

    if (check_user_overflow(addr, size)) { return -EFAULT; }

    uint64_t start = addr;
    uint64_t end   = addr + size;

    vma_manager_t *mgr = &get_current_task()->parent_group->vma_manager;
    if (vma_unmap_range(mgr, start, end) != 0) return SYSCALL_FAULT_(ENOMEM);

    unmap_virtual_page(get_current_task()->parent_group, addr, size);
    unmap_page_range(get_current_directory(), addr, size);
    return 0;
}

sys_(mremap, uint64_t old_addr, uint64_t old_size, uint64_t new_size, uint64_t flags,
         uint64_t new_addr) {
    old_addr = old_addr & (~(PAGE_SIZE - 1));
    new_addr = new_addr & (~(PAGE_SIZE - 1));
    if (!align_up_u64(old_size, PAGE_SIZE, &old_size) ||
        !align_up_u64(new_size, PAGE_SIZE, &new_size))
    {
        return SYSCALL_FAULT_(EINVAL);
    }
    if (check_user_overflow(old_addr, old_size) || check_user_overflow(new_addr, new_size))
        return SYSCALL_FAULT_(EFAULT);

    vma_manager_t *mgr = &get_current_task()->parent_group->vma_manager;

    vma_t *vma = vma_find(mgr, (unsigned long)old_addr);
    if (!vma || vma->vm_start != (unsigned long)old_addr) { return SYSCALL_FAULT_(EINVAL); }

    // 如果新大小更小，直接截断
    if (new_size <= vma->vm_end - vma->vm_start) {
        uint64_t new_end = vma->vm_start + new_size;
        if (new_end < vma->vm_end)
            unmap_page_range(get_current_directory(), new_end, vma->vm_end - new_end);
        vma->vm_end = vma->vm_start + new_size;
        return old_addr;
    }

    // 如果需要扩大，检查是否有足够空间
    if (check_user_overflow(vma->vm_start, new_size)) return SYSCALL_FAULT_(ENOMEM);
    uint64_t new_end = vma->vm_start + new_size;
    if (!vma_find_intersection(mgr, vma->vm_end, new_end)) {
        uint64_t pt_flags = PTE_USER | PTE_PRESENT | PTE_WRITEABLE;

        if (vma->vm_flags & VMA_READ) pt_flags |= PTE_PRESENT;
        if (vma->vm_flags & VMA_WRITE) pt_flags |= PTE_WRITEABLE;
        if (!(vma->vm_flags & VMA_EXEC)) pt_flags |= PTE_NO_EXECUTE;

        uint64_t old_end = vma->vm_end;
        for (uint64_t page = old_end; page < new_end; page += PAGE_SIZE) {
            uint64_t frame = alloc_frames(1);
            if (frame == 0) {
                unmap_page_range(get_current_directory(), old_end, page - old_end);
                return SYSCALL_FAULT_(ENOMEM);
            }
            memset(phys_to_virt(frame), 0, PAGE_SIZE);
            page_map_to(get_current_directory(), page, frame, pt_flags | PTE_FRAME_ALLOCATED);
        }

        vma->vm_end = new_end;
        mgr->vm_used += new_end - old_end;
        return old_addr;
    }

    if (flags & MREMAP_MAYMOVE) {
        if (flags & MREMAP_FIXED) {
            if (new_addr == 0 || check_user_overflow(new_addr, new_size)) return SYSCALL_FAULT_(EINVAL);
        }

        // 简单的地址分配策略：从高地址开始
        uint64_t start_addr = (flags & MREMAP_FIXED) ? new_addr : 0;
        if (flags & MREMAP_FIXED) {
            if (start_addr < USER_MMAP_START || start_addr >= USER_BRK_START ||
                new_size > USER_BRK_START - start_addr) {
                return SYSCALL_FAULT_(ENOMEM);
            }
            if (vma_find_intersection(mgr, start_addr, start_addr + new_size))
                return SYSCALL_FAULT_(ENOMEM);
        } else {
            start_addr = find_user_mmap_range(get_current_task()->parent_group, USER_MMAP_START, new_size);
            if (start_addr == 0) return SYSCALL_FAULT_(ENOMEM);
        }

        vma_t *new_vma = vma_alloc();
        if (!new_vma) return SYSCALL_FAULT_(ENOMEM);

        new_vma->vm_flags = vma->vm_flags;
        new_vma->vm_type = vma->vm_type;
        new_vma->vm_fd = vma->vm_fd;
        new_vma->vm_offset = vma->vm_offset;
        new_vma->shm_id = vma->shm_id;
        new_vma->vm_name = vma->vm_name ? strdup(vma->vm_name) : NULL;
        if (vma->vm_name != NULL && new_vma->vm_name == NULL) {
            vma_free(new_vma);
            return SYSCALL_FAULT_(ENOMEM);
        }
        new_vma->vm_start = start_addr;
        new_vma->vm_end   = start_addr + new_size;

        if (vma_insert(mgr, new_vma) != 0) {
            vma_free(new_vma);
            return SYSCALL_FAULT_(ENOMEM);
        }

        uint64_t pt_flags = PTE_USER | PTE_PRESENT | PTE_WRITEABLE;

        if (new_vma->vm_flags & VMA_READ) pt_flags |= PTE_PRESENT;
        if (new_vma->vm_flags & VMA_WRITE) pt_flags |= PTE_WRITEABLE;
        if (!(new_vma->vm_flags & VMA_EXEC)) pt_flags |= PTE_NO_EXECUTE;

        uint64_t copy_size = MIN(old_size, new_size);
        for (uint64_t offset = 0; offset < new_size; offset += PAGE_SIZE) {
            uint64_t frame = alloc_frames(1);
            if (frame == 0) {
                vma_remove(mgr, new_vma);
                vma_free(new_vma);
                unmap_page_range(get_current_directory(), start_addr, offset);
                return SYSCALL_FAULT_(ENOMEM);
            }
            memset(phys_to_virt(frame), 0, PAGE_SIZE);

            if (offset < copy_size) {
                uint64_t old_phys = translate_address(get_current_directory(), old_addr + offset);
                if (old_phys != 0) {
                    size_t bytes = (size_t)MIN(PAGE_SIZE, copy_size - offset);
                    memcpy(phys_to_virt(frame), phys_to_virt(old_phys & PTE_ADDR_MASK), bytes);
                }
            }
            page_map_to(get_current_directory(), start_addr + offset, frame, pt_flags | PTE_FRAME_ALLOCATED);
        }

        sys_munmap(old_addr, old_size, 0, 0, 0, 0, regs);
        return start_addr;
    }

    return (uint64_t)-ENOMEM;
}

sys_(getcwd, char *buffer, size_t length)
{
    if (unlikely(buffer == NULL)) return SYSCALL_FAULT_(EINVAL);
    if (unlikely(length == 0)) return 0;
    if (!user_range_mapped(current_user_pagedir(), buffer, length)) return SYSCALL_FAULT_(EFAULT);

    tcb_t  process  = get_current_task();
    char  *cwd      = vfs_get_fullpath(process->cwd);
    if (unlikely(cwd == NULL)) return SYSCALL_FAULT_(ENOENT);

    size_t cwd_leng = strlen(cwd);
    size_t copy_len = cwd_leng;
    if (copy_len >= length) copy_len = length - 1;
    if (!copy_to_user_pagedir(current_user_pagedir(), buffer, cwd, copy_len) ||
        !copy_to_user_pagedir(current_user_pagedir(), buffer + copy_len, "", 1))
    {
        free(cwd);
        return SYSCALL_FAULT_(EFAULT);
    }
    free(cwd);
    return copy_len;
}

static uint64_t set_current_cwd(vfs_node_t node, char *normalized_path)
{
    if (node == NULL || normalized_path == NULL) return SYSCALL_FAULT_(EINVAL);
    if (!(node->type & file_dir)) return SYSCALL_FAULT_(ENOTDIR);

    tcb_t current = get_current_task();
    if (current->cwd != NULL) vfs_close(current->cwd);
    current->cwd = node;

    if (current->str_cwd != NULL) free(current->str_cwd);
    current->str_cwd = normalized_path;
    return 0;
}

sys_(chdir, char *s)
{
    if (unlikely(s == NULL)) return SYSCALL_FAULT_(EINVAL);
    tcb_t current = get_current_task();

    char *user_path = NULL;
    int copy_ret = copy_string_from_user(&user_path, s, USER_PATH_MAX);
    if (copy_ret < 0) return SYSCALL_FAULT_((int)-copy_ret);

    char *path;
    char *bpath = NULL;
    if (user_path[0] == '/') { path = strdup(user_path); }
    else
    {
        bpath = vfs_get_fullpath(current->cwd);
        path  = pathacat(bpath, user_path);
    }
    free(user_path);

    if (unlikely(path == NULL))
    {
        free(bpath);
        return SYSCALL_FAULT_(ENOMEM);
    }

    char *normalized_path = normalize_path(path);
    free(path);
    free(bpath);

    if (unlikely(normalized_path == NULL)) { return SYSCALL_FAULT_(ENOMEM); }

    vfs_node_t node;
    if ((node = vfs_open(normalized_path)) == NULL)
    {
        free(normalized_path);
        return SYSCALL_FAULT_(ENOENT);
    }

    uint64_t ret = set_current_cwd(node, normalized_path);
    if ((int64_t)ret < 0)
    {
        vfs_close(node);
        free(normalized_path);
    }
    return ret;
}

sys_(fchdir, int fd)
{
    if (unlikely(fd < 0)) return SYSCALL_FAULT_(EBADF);

    pcb_t process = get_current_task()->parent_group;
    if (process == NULL || process->file_open == NULL) return SYSCALL_FAULT_(EBADF);

    fd_file_handle *handle = (fd_file_handle *)queue_get(process->file_open, fd);
    if (handle == NULL || handle->node == NULL) return SYSCALL_FAULT_(EBADF);
    if (!(handle->node->type & file_dir)) return SYSCALL_FAULT_(ENOTDIR);

    char *normalized_path = vfs_get_fullpath(handle->node);
    if (unlikely(normalized_path == NULL)) return SYSCALL_FAULT_(ENOENT);

    handle->node->refcount++;
    return set_current_cwd(handle->node, normalized_path);
}

sys_(exit_group, int exit_code)
{
    pcb_t exit_process = get_current_task()->parent_group;
    write_serial_fmt("task: Process %s exit with code %d.\n", exit_process->name, exit_code);
    close_interrupt;//applets 已经走到这个
    exit_process->abnormal_exit = false;
    kill_proc(exit_process, exit_code,
              true); // 子进程调用，is_zombie = true，不能在child_pcb中删除当前进程
    open_interrupt;
    scheduler_yield();
    cpu_hlt;
}

static uint64_t poll_kernel_fds(struct pollfd *fds_user, size_t nfds, size_t timeout)
{
    int      ready      = 0;
    uint64_t start_time = nanoTime();
    uint64_t timeout_ns = timeout == (size_t)-1 ? (uint64_t)-1 : timeout * 1000000ULL;
    bool     sigexit    = false;

    extern vfs_callback_t fs_callbacks[256];

    do
    {
        ready = 0;
        for (size_t i = 0; i < nfds; i++)
        {
            fds_user[i].revents = 0;
            if (fds_user[i].fd < 0) continue;
            fd_file_handle *handle =
                (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fds_user[i].fd);
            if (handle == NULL)
            {
                fds_user[i].revents = POLLNVAL;
                ready++;
                continue;
            }
            vfs_node_t      node   = handle->node;
            if (fs_callbacks[node->fsid]->poll == (void *)empty)
            {
                if (fds_user[i].events & POLLIN || fds_user[i].events & POLLOUT)
                {
                    fds_user[i].revents = fds_user[i].events & POLLIN ? POLLIN : POLLOUT;
                    ready++;
                }
                continue;
            }
            int revents = (int)epoll_to_poll_comp(vfs_poll(node, poll_to_epoll_comp(fds_user[i].events)));
            if (revents > 0)
            {
                fds_user[i].revents = (short)revents;
                ready++;
            }
        }

        // sigexit = signals_pending_quick(get_current_task());

        if (ready > 0 || sigexit) break;

        open_interrupt;
        scheduler_yield();
        __asm__ volatile("pause");
    } while (timeout != 0 && ((int)timeout == -1 || (nanoTime() - start_time) < timeout_ns));

    close_interrupt;

    if (!ready && sigexit) return (size_t)-EINTR;
    return ready;
}

static epoll_file_t *epoll_from_fd(int epfd)
{
    if (epfd < 0) return NULL;
    pcb_t process = get_current_task()->parent_group;
    if (process == NULL || process->file_open == NULL) return NULL;

    fd_file_handle *handle = (fd_file_handle *)queue_get(process->file_open, epfd);
    if (handle == NULL || handle->node == NULL || !(handle->node->type & file_epoll)) return NULL;
    return (epoll_file_t *)handle->node->handle;
}

static epoll_watch_t *epoll_find_watch(epoll_file_t *ep, int fd)
{
    if (ep == NULL || ep->watches == NULL) return NULL;

    epoll_watch_t *found = NULL;
    spin_lock(&ep->watches->lock);
    queue_foreach(ep->watches, node)
    {
        epoll_watch_t *watch = (epoll_watch_t *)node->data;
        if (watch != NULL && watch->fd == fd)
        {
            found = watch;
            break;
        }
    }
    spin_unlock(&ep->watches->lock);
    return found;
}

static int epoll_poll_fd(int fd, uint32_t events, uint32_t *revents)
{
    if (revents == NULL) return -EINVAL;
    *revents = 0;
    if (fd < 0) return -EBADF;

    pcb_t process = get_current_task()->parent_group;
    if (process == NULL || process->file_open == NULL) return -EBADF;

    fd_file_handle *handle = (fd_file_handle *)queue_get(process->file_open, fd);
    if (handle == NULL || handle->node == NULL) return -EBADF;

    extern vfs_callback_t fs_callbacks[256];
    vfs_node_t node = handle->node;
    if (fs_callbacks[node->fsid]->poll == (void *)empty)
    {
        if (events & (EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM))
        {
            if (events & (EPOLLIN | EPOLLRDNORM)) *revents |= EPOLLIN;
            if (events & (EPOLLOUT | EPOLLWRNORM)) *revents |= EPOLLOUT;
        }
        return 0;
    }

    uint32_t wanted = events;
    if (events & EPOLLRDNORM) wanted |= EPOLLIN;
    if (events & EPOLLWRNORM) wanted |= EPOLLOUT;
    *revents = vfs_poll(node, wanted);
    return 0;
}

static void epoll_free_file(epoll_file_t *ep)
{
    if (ep == NULL) return;
    if (ep->watches != NULL)
    {
        while (true)
        {
            epoll_watch_t *watch = (epoll_watch_t *)queue_dequeue(ep->watches);
            if (watch == NULL) break;
            free(watch);
        }
        queue_destroy(ep->watches);
    }
    free(ep);
}

static void epoll_close_node(vfs_node_t node)
{
    if (node == NULL || !(node->type & file_epoll)) return;
    epoll_free_file((epoll_file_t *)node->handle);
    node->handle = NULL;
}

static uint64_t epoll_create_common(int flags)
{
    if ((flags & ~EPOLL_CLOEXEC) != 0) return SYSCALL_FAULT_(EINVAL);

    epoll_file_t *ep = (epoll_file_t *)calloc(1, sizeof(epoll_file_t));
    if (ep == NULL) return SYSCALL_FAULT_(ENOMEM);
    ep->watches = queue_init();
    if (ep->watches == NULL)
    {
        free(ep);
        return SYSCALL_FAULT_(ENOMEM);
    }

    vfs_node_t node = vfs_node_alloc(NULL, "anon_epoll");
    if (node == NULL)
    {
        epoll_free_file(ep);
        return SYSCALL_FAULT_(ENOMEM);
    }
    node->type = file_epoll;
    node->handle = ep;
    node->fsid = 0;
    node->size = 0;

    fd_file_handle *fd_handle = (fd_file_handle *)calloc(1, sizeof(fd_file_handle));
    if (fd_handle == NULL)
    {
        epoll_close_node(node);
        free(node->name);
        free(node);
        return SYSCALL_FAULT_(ENOMEM);
    }
    fd_handle->node = node;
    fd_handle->flags = flags & EPOLL_CLOEXEC ? O_CLOEXEC : 0;
    node->flags = fd_handle->flags;

    int fd = (int)queue_enqueue_lowest(get_current_task()->parent_group->file_open, fd_handle);
    if (fd < 0)
    {
        epoll_close_node(node);
        free(node->name);
        free(node);
        free(fd_handle);
        return SYSCALL_FAULT_(EMFILE);
    }

    fd_handle->fd = fd;
    return fd;
}

sys_(epoll_create, int size)
{
    if (size <= 0) return SYSCALL_FAULT_(EINVAL);
    return epoll_create_common(0);
}

sys_(epoll_create1, int flags)
{
    return epoll_create_common(flags);
}

sys_(epoll_ctl, int epfd, int op, int fd, struct epoll_event *event)
{
    epoll_file_t *ep = epoll_from_fd(epfd);
    if (ep == NULL) return SYSCALL_FAULT_(EBADF);
    if (fd < 0 || fd == epfd) return SYSCALL_FAULT_(EINVAL);

    pcb_t process = get_current_task()->parent_group;
    fd_file_handle *target = (fd_file_handle *)queue_get(process->file_open, fd);
    if (target == NULL || target->node == NULL) return SYSCALL_FAULT_(EBADF);

    struct epoll_event kev = {};
    if (op != EPOLL_CTL_DEL)
    {
        if (event == NULL) return SYSCALL_FAULT_(EFAULT);
        if (!copy_from_user_pagedir(current_user_pagedir(), &kev, event, sizeof(kev)))
            return SYSCALL_FAULT_(EFAULT);
    }

    epoll_watch_t *watch = epoll_find_watch(ep, fd);
    switch (op)
    {
    case EPOLL_CTL_ADD:
        if (watch != NULL) return SYSCALL_FAULT_(EEXIST);
        watch = (epoll_watch_t *)calloc(1, sizeof(epoll_watch_t));
        if (watch == NULL) return SYSCALL_FAULT_(ENOMEM);
        watch->fd = fd;
        watch->event = kev;
        if (lock_queue_enqueue(ep->watches, watch) == (size_t)-1)
        {
            free(watch);
            return SYSCALL_FAULT_(ENOMEM);
        }
        return 0;
    case EPOLL_CTL_MOD:
        if (watch == NULL) return SYSCALL_FAULT_(ENOENT);
        watch->event = kev;
        return 0;
    case EPOLL_CTL_DEL:
        if (watch == NULL) return SYSCALL_FAULT_(ENOENT);
        {
            spin_lock(&ep->watches->lock);
            lock_node *found_node = NULL;
            queue_foreach(ep->watches, node)
            {
                epoll_watch_t *candidate = (epoll_watch_t *)node->data;
                if (candidate != NULL && candidate->fd == fd)
                {
                    found_node = node;
                    break;
                }
            }
            spin_unlock(&ep->watches->lock);
            epoll_watch_t *removed = (epoll_watch_t *)queue_remove_node(ep->watches, found_node);
            free(removed);
        }
        return 0;
    default:
        return SYSCALL_FAULT_(EINVAL);
    }
}

sys_(epoll_wait, int epfd, struct epoll_event *events, int maxevents, int timeout)
{
    epoll_file_t *ep = epoll_from_fd(epfd);
    if (ep == NULL) return SYSCALL_FAULT_(EBADF);
    if (events == NULL || maxevents <= 0) return SYSCALL_FAULT_(EINVAL);

    uint64_t start_time = nanoTime();
    uint64_t timeout_ns = timeout < 0 ? (uint64_t)-1 : (uint64_t)timeout * 1000000ULL;
    int ready = 0;

    do
    {
        ready = 0;
        spin_lock(&ep->watches->lock);
        queue_foreach(ep->watches, node)
        {
            if (ready >= maxevents) break;
            epoll_watch_t *watch = (epoll_watch_t *)node->data;
            if (watch == NULL) continue;

            uint32_t revents = 0;
            int poll_ret = epoll_poll_fd(watch->fd, watch->event.events, &revents);
            if (poll_ret < 0) revents = EPOLLERR;
            revents &= watch->event.events | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
            if (revents == 0) continue;

            struct epoll_event out = watch->event;
            out.events = revents;
            if (!copy_to_user_pagedir(current_user_pagedir(), &events[ready], &out, sizeof(out)))
            {
                spin_unlock(&ep->watches->lock);
                return SYSCALL_FAULT_(EFAULT);
            }
            ready++;
        }
        spin_unlock(&ep->watches->lock);

        if (ready > 0 || timeout == 0) break;

        open_interrupt;
        scheduler_yield();
        __asm__ volatile("pause");
        close_interrupt;
    } while (timeout < 0 || (nanoTime() - start_time) < timeout_ns);

    return ready;
}

sys_(epoll_pwait, int epfd, struct epoll_event *events, int maxevents, int timeout, sigset_t *sigmask, size_t sigsetsize)
{
    (void)sigmask;
    (void)sigsetsize;
    return sys_epoll_wait(epfd, events, maxevents, timeout, 0, 0, regs);
}

sys_(poll, struct pollfd *fds_user, size_t nfds, size_t timeout)
{
    if (nfds == 0) return 0;
    if (fds_user == NULL) return SYSCALL_FAULT_(EINVAL);
    if (nfds > USER_POLL_MAX) return SYSCALL_FAULT_(EINVAL);

    size_t fds_bytes = 0;
    if (!checked_mul_size(nfds, sizeof(struct pollfd), &fds_bytes)) return SYSCALL_FAULT_(EFAULT);
    struct pollfd *fds = (struct pollfd *)malloc(fds_bytes);
    if (fds == NULL) return SYSCALL_FAULT_(ENOMEM);
    if (!copy_from_user_pagedir(current_user_pagedir(), fds, fds_user, fds_bytes))
    {
        free(fds);
        return SYSCALL_FAULT_(EFAULT);
    }

    uint64_t ret = poll_kernel_fds(fds, nfds, timeout);
    if ((int64_t)ret >= 0 && !copy_to_user_pagedir(current_user_pagedir(), fds_user, fds, fds_bytes))
    {
        free(fds);
        return SYSCALL_FAULT_(EFAULT);
    }

    free(fds);
    return ret;
}

sys_(sched_getaffinity, int pid, size_t cpusetsize, void *mask)
{
    if (cpusetsize == 0 || mask == NULL) return SYSCALL_FAULT_(EINVAL);
    if (pid < 0) return SYSCALL_FAULT_(ESRCH);

    uint8_t *kbuf = (uint8_t *)calloc(1, cpusetsize);
    if (kbuf == NULL) return SYSCALL_FAULT_(ENOMEM);
    kbuf[0] = 1;
    bool ok = copy_to_user_pagedir(current_user_pagedir(), mask, kbuf, cpusetsize);
    free(kbuf);
    return ok ? 0 : SYSCALL_FAULT_(EFAULT);
}

sys_(rt_sigprocmask, int how, sigset_t *set, sigset_t *oldset)
{
    return syscall_ssetmask(how, set, oldset);
}

sys_(rt_sigaction, int sig, sigaction_t *action, sigaction_t *oldaction, size_t sigsetsize)
{
    if (sigsetsize < sizeof(sigset_t)) return SYSCALL_FAULT_(EINVAL);
    int ret = syscall_sig_action(sig, action, oldaction);
    return ret < 0 ? (uint64_t)ret : (uint64_t)ret;
}

sys_(kill, int pid, int sig)
{
    pcb_t target = NULL;
    if (pid > 0) target = found_pcb(pid);
    else if (pid == 0) target = get_current_task()->parent_group;
    else return SYSCALL_FAULT_(ENOSYS);

    if (target == NULL) return SYSCALL_FAULT_(ESRCH);
    int ret = signal_send_process(target, sig);
    return ret < 0 ? (uint64_t)ret : (uint64_t)ret;
}

sys_(tgkill, int tgid, int tid, int sig)
{
    pcb_t target = found_pcb(tgid);
    if (target == NULL) return SYSCALL_FAULT_(ESRCH);
    if (tid <= 0 || found_thread(target, tid) == NULL) return SYSCALL_FAULT_(ESRCH);

    int ret = signal_send_process(target, sig);
    return ret < 0 ? (uint64_t)ret : (uint64_t)ret;
}

extern fd_file_handle *fd_dup(fd_file_handle *src);

sys_(dup2, int fd, int newfd)
{
    if (unlikely(fd < 0 || newfd < 0)) return SYSCALL_FAULT_(EBADF);
    if (fd == newfd) return newfd;

    fd_file_handle *handle =  (fd_file_handle*)queue_get(get_current_task()->parent_group->file_open, fd);
    if (unlikely(handle == NULL)) return SYSCALL_FAULT_(EBADF);

    fd_file_handle *old_handle =  (fd_file_handle*)queue_get(get_current_task()->parent_group->file_open, newfd);
    if (old_handle != NULL)
    {
        queue_remove_at(get_current_task()->parent_group->file_open, newfd);
        vfs_close(old_handle->node);
        free(old_handle);
    }

    fd_file_handle *new_handle = fd_dup(handle);
    if (new_handle == NULL) return SYSCALL_FAULT_(ENOMEM);
    new_handle->flags &= ~O_CLOEXEC;
    new_handle->fd = newfd;
    if (queue_enqueue_id(get_current_task()->parent_group->file_open, new_handle, newfd) == (size_t)-1)
    {
        vfs_close(new_handle->node);
        free(new_handle);
        return SYSCALL_FAULT_(EBADF);
    }
    return newfd;
}

sys_(dup3, int fd, int newfd, int flags)
{
    if (fd == newfd) return SYSCALL_FAULT_(EINVAL);
    if ((flags & ~O_CLOEXEC) != 0) return SYSCALL_FAULT_(EINVAL);

    int ret = (int)sys_dup2(fd, newfd, 0, 0, 0, 0, regs);
    if (ret < 0) return ret;

    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, newfd);
    if (handle != NULL)
    {
        if (flags & O_CLOEXEC) handle->flags |= O_CLOEXEC;
        else handle->flags &= ~O_CLOEXEC;
    }
    return ret;
}

sys_(dup, int fd)
{
    if (unlikely(fd < 0)) return SYSCALL_FAULT_(EINVAL);
    fd_file_handle *handle =  (fd_file_handle*)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL) return SYSCALL_FAULT_(EBADF);
    fd_file_handle *new_handle = fd_dup(handle);
    if (new_handle == NULL) return SYSCALL_FAULT_(ENOMEM);
    new_handle->flags &= ~O_CLOEXEC;
    return new_handle->fd      = queue_enqueue_lowest(get_current_task()->parent_group->file_open, new_handle);
}

static int fd_dup_from(fd_file_handle *handle, int minfd, bool cloexec)
{
    if (handle == NULL || minfd < 0) return -EINVAL;

    fd_file_handle *new_handle = fd_dup(handle);
    if (new_handle == NULL) return -ENOMEM;
    if (cloexec) new_handle->flags |= O_CLOEXEC;
    else new_handle->flags &= ~O_CLOEXEC;

    lock_queue *queue = get_current_task()->parent_group->file_open;
    int newfd = minfd;
    while (queue_get(queue, (size_t)newfd) != NULL) newfd++;

    new_handle->fd = queue_enqueue_id(queue, new_handle, (size_t)newfd);
    if (new_handle->fd == (size_t)-1)
    {
        vfs_close(new_handle->node);
        free(new_handle);
        return -EBADF;
    }
    return newfd;
}

sys_(fcntl, int fd, int cmd, uint64_t arg)
{
    if (fd < 0 || cmd < 0) return SYSCALL_FAULT_(EINVAL);
    fd_file_handle *handle =  (fd_file_handle*)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL) return SYSCALL_FAULT_(EBADF);

    switch (cmd)
    {
    case F_GETFD: return (handle->flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    case F_SETFD:
        if (arg & FD_CLOEXEC) handle->flags |= O_CLOEXEC;
        else handle->flags &= ~O_CLOEXEC;
        return 0;
    case F_DUPFD_CLOEXEC:
        return fd_dup_from(handle, (int)arg, true);
    case F_DUPFD:
        return fd_dup_from(handle, (int)arg, false);
    case F_GETFL: return handle->flags;
    case F_SETFL:
        {uint32_t valid_flags  = O_APPEND | O_DIRECT | O_NOATIME | O_NONBLOCK;
        handle->node->flags  &= ~valid_flags;
        handle->node->flags  |= arg & valid_flags;
        handle->flags        &= ~valid_flags;
        handle->flags        |= arg & valid_flags;
        if (handle->node->type & file_socket) {
            int ret = socket_apply_flags(handle->node, handle->node->flags);
            if (ret < 0) return SYSCALL_FAULT_(-ret);
        }}
        return 0;
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        return 0;
    default: break;
    }
    return 0;
}

sys_(fork)
{
    return process_fork(regs, false, 0);
}

sys_(clone, uint64_t flags, uint64_t stack, int *parent_tid, int *child_tid, uint64_t tls)
{
    return thread_clone(regs, flags, stack, parent_tid, child_tid, tls);
}

sys_(clone3, struct clone_args *cl_args, size_t size)
{
    if (size < sizeof(uint64_t)) return SYSCALL_FAULT_(EINVAL);
    if (size > sizeof(struct clone_args)) size = sizeof(struct clone_args);

    struct clone_args args;
    page_directory_t *pagedir = current_user_pagedir();
    if (pagedir == NULL) return SYSCALL_FAULT_(EFAULT);
    memset(&args, 0, sizeof(args));
    if (!copy_from_user_pagedir(pagedir, &args, cl_args, size)) return SYSCALL_FAULT_(EFAULT);

    if (args.pidfd != 0) return SYSCALL_FAULT_(EINVAL);

    uint64_t flags = args.flags;
    if (args.exit_signal != 0 && (flags & 0xFF) == 0)
        flags |= (args.exit_signal & 0xFF);

    uint64_t stack      = args.stack;
    int     *parent_tid = (args.parent_tid != 0) ? (int *)(uintptr_t)args.parent_tid : NULL;
    int     *child_tid  = (args.child_tid != 0)  ? (int *)(uintptr_t)args.child_tid  : NULL;
    uint64_t tls        = args.tls;

    return thread_clone(regs, flags, stack, parent_tid, child_tid, tls);
}

sys_(get_tid)
{
    return get_current_task()->tid;
}

sys_(set_tid_address, int *tidptr)
{
    tcb_t task = get_current_task();
    if (task == NULL) return SYSCALL_FAULT_(ESRCH);

    task->clear_child_tid = (uint64_t)tidptr;
    task->tid_directory   = task->parent_group != NULL ? task->parent_group->pagedir : NULL;
    return task->tid;
}

sys_(futex, uint32_t *uaddr, int op, uint32_t val, const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3)
{
    (void)timeout;
    (void)uaddr2;
    (void)val3;

    if (uaddr == NULL) return SYSCALL_FAULT_(EFAULT);

    uint32_t current = 0;
    int cmd = op & 0x7f;

    switch (cmd)
    {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET:
        if (!copy_from_user_pagedir(current_user_pagedir(), &current, uaddr, sizeof(current)))
            return SYSCALL_FAULT_(EFAULT);
        if (current != val) return SYSCALL_FAULT_(EAGAIN);
        scheduler_yield();
        return SYSCALL_FAULT_(ETIMEDOUT);
    case FUTEX_WAKE:
        return 0;
    default:
        return SYSCALL_FAULT_(ENOSYS);
    }
}

sys_(vfork)
{
    return process_fork(regs, true, 0);
}

sys_(execve, char *path, char **argv, char **envp)
{
    if (unlikely(path == NULL)) return SYSCALL_FAULT_(EINVAL);
    return process_execve(path, argv, envp);
}

sys_(fstat, int fd, struct stat *buf)
{
    if (unlikely(buf == NULL)) return SYSCALL_FAULT_(EINVAL);

    fd_file_handle *handle =  (fd_file_handle*)queue_get(get_current_task()->parent_group->file_open, fd);
    if (unlikely(handle == NULL)) return SYSCALL_FAULT_(EBADF);
    struct stat kstat;
    uint64_t ret = fill_stat_from_node(handle->node, &kstat);
    if ((int64_t)ret < 0) return ret;
    return copy_stat_to_user(buf, &kstat);
}

sys_(time, int64_t *timer)
{
    int64_t timestamp = (int64_t)(realtime_ns() / 1000000000ULL);
    if (timer != NULL && !copy_to_user_pagedir(current_user_pagedir(), timer, &timestamp, sizeof(timestamp)))
        return SYSCALL_FAULT_(EFAULT);
    return (uint64_t)timestamp;
}

sys_(gettimeofday, struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv == NULL) return SYSCALL_FAULT_(EINVAL);

    struct timeval ktv;
    uint64_t now_ns = realtime_ns();
    ktv.tv_sec = (long)(now_ns / 1000000000ULL);
    ktv.tv_usec = (long)((now_ns % 1000000000ULL) / 1000ULL);
    if (!copy_to_user_pagedir(current_user_pagedir(), tv, &ktv, sizeof(ktv))) return SYSCALL_FAULT_(EFAULT);
    return 0;
}

sys_(clock_gettime, uint64_t arg0, struct timespec *ts)
{
    struct timespec kts;
    memset(&kts, 0, sizeof(kts));

    switch (arg0)
    {
    case 1:
    case 6:
    case 4: {
        uint64_t nano = bootNanoTime();
        kts.tv_sec    = nano / 1000000000ULL;
        kts.tv_nsec   = nano % 1000000000ULL;
        if (ts != NULL && !copy_to_user_pagedir(current_user_pagedir(), ts, &kts, sizeof(kts)))
            return SYSCALL_FAULT_(EFAULT);
        return 0;
    }
    case 0: {
        uint64_t now_ns = realtime_ns();
        kts.tv_sec  = now_ns / 1000000000ULL;
        kts.tv_nsec = now_ns % 1000000000ULL;
        if (ts != NULL && !copy_to_user_pagedir(current_user_pagedir(), ts, &kts, sizeof(kts)))
            return SYSCALL_FAULT_(EFAULT);
        return 0;
    }
    case 7: {
        uint64_t nano = bootNanoTime();
        kts.tv_sec    = nano / 1000000000ULL;
        kts.tv_nsec   = nano % 1000000000ULL;
        if (ts != NULL && !copy_to_user_pagedir(current_user_pagedir(), ts, &kts, sizeof(kts)))
            return SYSCALL_FAULT_(EFAULT);
        return 0;
    }
    default: write_serial_fmt("clock not supported(%d)\n", arg0); return SYSCALL_FAULT_(EINVAL);
    }
}

sys_(clock_getres)
{
    struct timespec *ts = (struct timespec *)arg1;
    if (ts == NULL) return SYSCALL_FAULT_(EINVAL);
    struct timespec kts;
    memset(&kts, 0, sizeof(kts));
    kts.tv_nsec = 1000000;
    if (!copy_to_user_pagedir(current_user_pagedir(), ts, &kts, sizeof(kts))) return SYSCALL_FAULT_(EFAULT);
    return 0;
}

sys_(shutdown, int fd, int how)
{
    int ret = socket_shutdown(fd, how);
    if (ret < 0) return SYSCALL_FAULT_(-ret);
    return ret;
}


sys_(mkdir, char *name, uint64_t mode)
{
    if (name == NULL) return SYSCALL_FAULT_(EINVAL);
    char *user_path = NULL;
    int copy_ret = copy_string_from_user(&user_path, name, USER_PATH_MAX);
    if (copy_ret < 0) return SYSCALL_FAULT_((int)-copy_ret);

    char  *npath = vfs_cwd_path_build(user_path);
    free(user_path);
    if (npath == NULL) return SYSCALL_FAULT_(ENOMEM);
    size_t ret   = vfs_mkdir(npath) == VFS_STATUS_FAILED ? SYSCALL_FAULT_(EIO) : 0;
    if (ret == 0)
    {
        vfs_node_t node = vfs_open(npath);
        if (node != NULL)
        {
            stamp_node_owner(node);
            if ((mode & 07777) != 0) node->mode = (uint16_t)(mode & 07777);
            vfs_close(node);
        }
    }
    free(npath);
    return ret;
}

sys_(bind, int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    if (addr == NULL) return SYSCALL_FAULT_(EINVAL);
    if (addrlen < sizeof(struct sockaddr) || addrlen > sizeof(struct sockaddr_in6)) return SYSCALL_FAULT_(EINVAL);
    struct sockaddr_in6 kaddr;
    memset(&kaddr, 0, sizeof(kaddr));
    if (!copy_from_user_pagedir(current_user_pagedir(), &kaddr, addr, addrlen)) return SYSCALL_FAULT_(EFAULT);
    int ret = socket_bind(fd, (const struct sockaddr *)&kaddr, addrlen);
    if (ret < 0) return SYSCALL_FAULT_(-ret);
    return ret;
}

sys_(listen, int fd, int backlog)
{
    int ret = socket_listen(fd, backlog);
    if (ret < 0) return SYSCALL_FAULT_(-ret);
    return ret;
}

sys_(lseek, int fd, int64_t offset, size_t whence)
{
    fd_file_handle *handle =  (fd_file_handle*)queue_get(get_current_task()->parent_group->file_open, fd);
    if (unlikely(handle == NULL)) return SYSCALL_FAULT_(EBADF);
    if (handle->node->type & (file_socket | file_pipe)) return SYSCALL_FAULT_(ESPIPE);

    uint64_t new_offset = 0;
    switch (whence)
    {
    case SEEK_SET:
        if (offset < 0) return SYSCALL_FAULT_(EINVAL);
        new_offset = (uint64_t)offset;
        break;
    case SEEK_CUR:
        if (!add_u64_i64(handle->offset, offset, &new_offset)) return SYSCALL_FAULT_(EINVAL);
        break;
    case SEEK_END:
        if (!add_u64_i64(handle->node->size, offset, &new_offset)) return SYSCALL_FAULT_(EINVAL);
        break;
    case SEEK_DATA:
        if (offset < 0) return SYSCALL_FAULT_(EINVAL);
        if (offset >= handle->node->size) return SYSCALL_FAULT_(ENXIO);
        new_offset = (uint64_t)offset;
        break;
    case SEEK_HOLE:
        if (offset < 0) return SYSCALL_FAULT_(EINVAL);
        if (offset >= handle->node->size) return SYSCALL_FAULT_(ENXIO);
        return handle->node->size;
    default: return SYSCALL_FAULT_(EINVAL);
    }

    handle->offset = (size_t)new_offset;
    return handle->offset;
}

sys_(umask, uint64_t mask)
{
    pcb_t process = get_current_task()->parent_group;
    if (process == NULL) return 0;

    uint16_t old_mask = process->umask & 0777;
    process->umask = (uint16_t)(mask & 0777);
    return old_mask;
}

static uint64_t select_common(int nfds, uint8_t *read, uint8_t *write, uint8_t *except,
                              const struct timeval *ktimeout)
{
    if (nfds < 0) return SYSCALL_FAULT_(EINVAL);
    size_t bitmap_bytes = 0;
    if (!user_bitmap_bytes((uint64_t)nfds, &bitmap_bytes)) return SYSCALL_FAULT_(EINVAL);

    page_directory_t *pagedir = current_user_pagedir();
    uint8_t *kread = NULL;
    uint8_t *kwrite = NULL;
    uint8_t *kexcept = NULL;
    if (bitmap_bytes > 0)
    {
        if (read)
        {
            kread = (uint8_t *)malloc(bitmap_bytes);
            if (kread == NULL) return SYSCALL_FAULT_(ENOMEM);
            if (!copy_from_user_pagedir(pagedir, kread, read, bitmap_bytes)) { free(kread); return SYSCALL_FAULT_(EFAULT); }
        }
        if (write)
        {
            kwrite = (uint8_t *)malloc(bitmap_bytes);
            if (kwrite == NULL) { free(kread); return SYSCALL_FAULT_(ENOMEM); }
            if (!copy_from_user_pagedir(pagedir, kwrite, write, bitmap_bytes)) { free(kread); free(kwrite); return SYSCALL_FAULT_(EFAULT); }
        }
        if (except)
        {
            kexcept = (uint8_t *)malloc(bitmap_bytes);
            if (kexcept == NULL) { free(kread); free(kwrite); return SYSCALL_FAULT_(ENOMEM); }
            if (!copy_from_user_pagedir(pagedir, kexcept, except, bitmap_bytes)) { free(kread); free(kwrite); free(kexcept); return SYSCALL_FAULT_(EFAULT); }
        }
    }

    size_t         complength = sizeof(struct pollfd);
    struct pollfd *comp       = (struct pollfd *)malloc(complength);
    if (comp == NULL) { free(kread); free(kwrite); free(kexcept); return SYSCALL_FAULT_(ENOMEM); }
    memset(comp, 0, complength);
    size_t compIndex = 0;
    if (kread)
    {
        for (int i = 0; i < nfds; i++)
        {
            if (select_bitmap(kread, i)) select_add(&comp, &compIndex, &complength, i, POLLIN);
        }
    }
    if (kwrite)
    {
        for (int i = 0; i < nfds; i++)
        {
            if (select_bitmap(kwrite, i)) select_add(&comp, &compIndex, &complength, i, POLLOUT);
        }
    }
    if (kexcept)
    {
        for (int i = 0; i < nfds; i++)
        {
            if (select_bitmap(kexcept, i)) select_add(&comp, &compIndex, &complength, i, POLLPRI | POLLERR);
        }
    }

    if (kread) memset(kread, 0, bitmap_bytes);
    if (kwrite) memset(kwrite, 0, bitmap_bytes);
    if (kexcept) memset(kexcept, 0, bitmap_bytes);

    size_t time = 0;
    if (ktimeout == NULL) { time = -1; }
    else if (ktimeout->tv_sec == -1 || ktimeout->tv_usec == -1) { time = -1; }
    else
    {
        if (ktimeout->tv_sec < 0 || ktimeout->tv_usec < 0 || ktimeout->tv_usec >= 1000000)
        {
            free(kread); free(kwrite); free(kexcept); free(comp);
            return SYSCALL_FAULT_(EINVAL);
        }
        size_t sec_ms = 0;
        size_t usec_with_rounding = 0;
        size_t usec_ms = 0;
        if (!checked_mul_size((size_t)ktimeout->tv_sec, 1000, &sec_ms) ||
            !checked_add_size((size_t)ktimeout->tv_usec, 999, &usec_with_rounding) ||
            !checked_add_size(sec_ms, usec_with_rounding / 1000, &usec_ms))
        {
            free(kread); free(kwrite); free(kexcept); free(comp);
            return SYSCALL_FAULT_(EINVAL);
        }
        time = usec_ms;
    }

    size_t res = poll_kernel_fds(comp, compIndex, time);

    if ((int64_t)res < 0)
    {
        free(kread); free(kwrite); free(kexcept); free(comp);
        return res;
    }

    size_t verify = 0;
    for (size_t i = 0; i < compIndex; i++)
    {
        if (!comp[i].revents) continue;
        if (comp[i].events & POLLIN && comp[i].revents & POLLIN)
        {
            if (kread) select_bitmap_set(kread, comp[i].fd);
            verify++;
        }
        if (comp[i].events & POLLOUT && comp[i].revents & POLLOUT)
        {
            if (kwrite) select_bitmap_set(kwrite, comp[i].fd);
            verify++;
        }
        if ((comp[i].events & POLLPRI && comp[i].revents & POLLPRI))
        {
            if (kexcept) select_bitmap_set(kexcept, comp[i].fd);
            verify++;
        }
    }

    if (kread && !copy_to_user_pagedir(pagedir, read, kread, bitmap_bytes))
    {
        free(kread); free(kwrite); free(kexcept); free(comp);
        return SYSCALL_FAULT_(EFAULT);
    }
    if (kwrite && !copy_to_user_pagedir(pagedir, write, kwrite, bitmap_bytes))
    {
        free(kread); free(kwrite); free(kexcept); free(comp);
        return SYSCALL_FAULT_(EFAULT);
    }
    if (kexcept && !copy_to_user_pagedir(pagedir, except, kexcept, bitmap_bytes))
    {
        free(kread); free(kwrite); free(kexcept); free(comp);
        return SYSCALL_FAULT_(EFAULT);
    }

    free(kread);
    free(kwrite);
    free(kexcept);
    free(comp);
    return verify;
}

sys_(select, int nfds, uint8_t *read, uint8_t *write, uint8_t *except, struct timeval *timeout)
{
    struct timeval ktimeout;
    const struct timeval *ktimeout_ptr = NULL;
    if (timeout != NULL)
    {
        if (!copy_from_user_pagedir(current_user_pagedir(), &ktimeout, timeout, sizeof(ktimeout)))
            return SYSCALL_FAULT_(EFAULT);
        ktimeout_ptr = &ktimeout;
    }

    return select_common(nfds, read, write, except, ktimeout_ptr);
}

sys_(pselect6, uint64_t nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timespec *timeout,
     WeirdPselect6 *weirdPselect6)
{
    size_t bitmap_bytes = 0;
    if (!user_bitmap_bytes(nfds, &bitmap_bytes)) return SYSCALL_FAULT_(EINVAL);
    page_directory_t *pagedir = current_user_pagedir();
    if (readfds && !user_range_mapped(pagedir, readfds, bitmap_bytes)) { return SYSCALL_FAULT_(EFAULT); }
    if (writefds && !user_range_mapped(pagedir, writefds, bitmap_bytes)) { return SYSCALL_FAULT_(EFAULT); }
    if (exceptfds && !user_range_mapped(pagedir, exceptfds, bitmap_bytes)) { return SYSCALL_FAULT_(EFAULT); }

    WeirdPselect6 kweird;
    memset(&kweird, 0, sizeof(kweird));
    if (weirdPselect6 != NULL &&
        !copy_from_user_pagedir(pagedir, &kweird, weirdPselect6, sizeof(kweird)))
    {
        return SYSCALL_FAULT_(EFAULT);
    }
    size_t    sigsetsize = kweird.ss_len;
    sigset_t *sigmask    = kweird.ss;
    if (sigmask != NULL && sigsetsize < sizeof(sigset_t)) { return SYSCALL_FAULT_(EINVAL); }
    sigset_t origmask = 0;
    if (sigmask)
    {
        sigset_t requested_mask = 0;
        if (!copy_from_user_pagedir(pagedir, &requested_mask, sigmask, sizeof(requested_mask)))
        {
            return SYSCALL_FAULT_(EFAULT);
        }
        signal_setmask(SIG_SETMASK, &requested_mask, &origmask);
    }
    struct timeval timeoutConv;
    const struct timeval *timeoutPtr = NULL;
    if (timeout)
    {
        struct timespec ktimeout;
        if (!copy_from_user_pagedir(pagedir, &ktimeout, timeout, sizeof(ktimeout)))
        {
            if (sigmask) { signal_setmask(SIG_SETMASK, &origmask, NULL); }
            return SYSCALL_FAULT_(EFAULT);
        }
        if (ktimeout.tv_sec < 0 || ktimeout.tv_nsec < 0 || ktimeout.tv_nsec >= 1000000000L)
        {
            if (sigmask) { signal_setmask(SIG_SETMASK, &origmask, NULL); }
            return SYSCALL_FAULT_(EINVAL);
        }
        timeoutConv =
            (struct timeval){.tv_sec = (long)ktimeout.tv_sec, .tv_usec = (long)(ktimeout.tv_nsec / 1000)};
        timeoutPtr = &timeoutConv;
    }

    size_t ret = select_common((int)nfds, (uint8_t *)readfds, (uint8_t *)writefds, (uint8_t *)exceptfds,
                               timeoutPtr);
    if (sigmask) { signal_setmask(SIG_SETMASK, &origmask, NULL); }
    return ret;
}


sys_(getdents, int fd, struct dirent *dents, size_t size)
{
    if (size == 0) return 0;
    if (dents == NULL) return SYSCALL_FAULT_(EINVAL);
    page_directory_t *pagedir = current_user_pagedir();
    if (!user_range_mapped(pagedir, dents, size)) return SYSCALL_FAULT_(EFAULT);

    fd_file_handle *handle =  (fd_file_handle*)queue_get(get_current_task()->parent_group->file_open, fd);
    if (unlikely(handle == NULL)) { return SYSCALL_FAULT_(EBADF); }
    if (handle->node->type != file_dir) { return SYSCALL_FAULT_(ENOTDIR); }
    size_t   child_count   = (uint64_t)list_length(handle->node->child);
    size_t   max_dents_num = size / sizeof(struct dirent);
    size_t   read_count    = 0;
    uint64_t offset        = 0;
    vfs_node_t child_node;
    vfs_child_lock();
    list_foreach(handle->node->child, i)
    {
        if (offset < handle->offset) { goto next; }
        if (handle->offset >= (child_count * sizeof(struct dirent))) { break; }
        if (read_count >= max_dents_num) { break; }
        child_node = (vfs_node_t)i->data;
        struct dirent kd;
        memset(&kd, 0, sizeof(kd));
        kd.d_ino    = (long)child_node->inode;
        kd.d_off    = (long)handle->offset;
        kd.d_reclen = sizeof(struct dirent);
        kd.d_type = dirent_type_from_node(child_node);
        strncpy(kd.d_name, child_node->name, sizeof(kd.d_name) - 1);
        if (!copy_to_user_pagedir(pagedir, &dents[read_count], &kd, sizeof(kd)))
        {
            vfs_child_unlock();
            return read_count > 0 ? read_count * sizeof(struct dirent) : SYSCALL_FAULT_(EFAULT);
        }
        handle->offset += sizeof(struct dirent);
        read_count++;
    next:
        offset += sizeof(struct dirent);
    }
    vfs_child_unlock();
    return read_count * sizeof(struct dirent);
}

struct linux_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[1];
} __attribute__((packed));

sys_(getdents64, int fd, void *dents, size_t size)
{
    if (size == 0) return 0;
    if (dents == NULL) return SYSCALL_FAULT_(EINVAL);
    page_directory_t *pagedir = current_user_pagedir();
    if (!user_range_mapped(pagedir, dents, size)) return SYSCALL_FAULT_(EFAULT);

    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (unlikely(handle == NULL)) { return SYSCALL_FAULT_(EBADF); }
    if (handle->node->type != file_dir) { return SYSCALL_FAULT_(ENOTDIR); }

    size_t written = 0;
    size_t offset = 0;
    vfs_child_lock();
    list_foreach(handle->node->child, i)
    {
        vfs_node_t child_node = (vfs_node_t)i->data;
        size_t name_len = strlen(child_node->name);
        if (name_len > 255) name_len = 255;
        size_t reclen = offsetof(struct linux_dirent64, d_name) + name_len + 1;
        uint64_t aligned_reclen = 0;
        if (!align_up_u64(reclen, 8, &aligned_reclen) || aligned_reclen > sizeof(struct dirent))
        {
            vfs_child_unlock();
            return written > 0 ? written : SYSCALL_FAULT_(EINVAL);
        }
        reclen = (size_t)aligned_reclen;

        if (offset < handle->offset)
        {
            offset += reclen;
            continue;
        }
        if (written + reclen > size)
        {
            vfs_child_unlock();
            return written > 0 ? written : SYSCALL_FAULT_(EINVAL);
        }

        char record[sizeof(struct dirent)];
        memset(record, 0, sizeof(record));
        struct linux_dirent64 *kd = (struct linux_dirent64 *)record;
        kd->d_ino = (uint64_t)child_node->inode;
        kd->d_off = (int64_t)(offset + reclen);
        kd->d_reclen = (unsigned short)reclen;
        kd->d_type = dirent_type_from_node(child_node);
        strncpy(kd->d_name, child_node->name, name_len);

        if (!copy_to_user_pagedir(pagedir, (uint8_t *)dents + written, record, reclen))
        {
            vfs_child_unlock();
            return written > 0 ? written : SYSCALL_FAULT_(EFAULT);
        }
        written += reclen;
        offset += reclen;
        handle->offset = offset;
    }
    vfs_child_unlock();
    return written;
}

sys_(newfstatat, int dirfd, char *pathname, struct stat *buf, uint64_t flags)
{
    if (buf == NULL) return SYSCALL_FAULT_(EINVAL);
    char *resolved = NULL;
    int path_ret = resolve_user_path_at(dirfd, pathname, &resolved);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);

    struct stat kstat;
    uint64_t ret = (flags & AT_SYMLINK_NOFOLLOW) ?
                   lstat_kernel_path(resolved, &kstat) :
                   stat_kernel_path(resolved, &kstat);

    free(resolved);
    if ((int64_t)ret < 0) return ret;
    return copy_stat_to_user(buf, &kstat);
}

sys_(statx, int dirfd, char *pathname, uint64_t flags, uint64_t mask, struct statx *buff)
{
    if (pathname == NULL || buff == NULL) return SYSCALL_FAULT_(EINVAL);

    struct stat simple;
    char *resolved = NULL;
    int path_ret = resolve_user_path_at(dirfd, pathname, &resolved);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);

    uint64_t ret = stat_kernel_path(resolved, &simple);
    free(resolved);
    if ((int64_t)ret < 0) return ret;

    struct statx kstatx;
    memset(&kstatx, 0, sizeof(kstatx));
    kstatx.stx_mask            = mask;
    kstatx.stx_blksize         = simple.st_blksize;
    kstatx.stx_attributes      = 0;
    kstatx.stx_nlink           = simple.st_nlink;
    kstatx.stx_uid             = simple.st_uid;
    kstatx.stx_gid             = simple.st_gid;
    kstatx.stx_mode            = simple.st_mode;
    kstatx.stx_ino             = simple.st_ino;
    kstatx.stx_size            = simple.st_size;
    kstatx.stx_blocks          = simple.st_blocks;
    kstatx.stx_attributes_mask = 0;

    kstatx.stx_atime.tv_sec  = (long)simple.st_atim.tv_sec;
    kstatx.stx_atime.tv_nsec = simple.st_atim.tv_nsec;

    kstatx.stx_btime.tv_sec  = (long)simple.st_ctim.tv_sec;
    kstatx.stx_btime.tv_nsec = simple.st_ctim.tv_nsec;

    kstatx.stx_ctime.tv_sec  = (long)simple.st_ctim.tv_sec;
    kstatx.stx_ctime.tv_nsec = simple.st_ctim.tv_nsec;

    kstatx.stx_mtime.tv_sec  = (long)simple.st_mtim.tv_sec;
    kstatx.stx_mtime.tv_nsec = simple.st_mtim.tv_nsec;

    if (!copy_to_user_pagedir(current_user_pagedir(), buff, &kstatx, sizeof(kstatx))) return SYSCALL_FAULT_(EFAULT);
    return 0;
}

sys_(pipe2, int *pipefd, uint64_t flags)
{
    /* fs/pipefs.c */
    extern vfs_node_t pipefs_root;
    extern int        pipefd_id;
    extern int        pipefs_id;
    static uint64_t   pipe_inode_next = 1;

    if ((flags & ~(O_CLOEXEC | O_NONBLOCK)) != 0) return SYSCALL_FAULT_(EINVAL);
    if (pipefs_root == NULL) return SYSCALL_FAULT_(ENOSYS);
    if (pipefd == NULL) return SYSCALL_FAULT_(EINVAL);
    bool kernel_pipefd = regs == NULL;
    if (!kernel_pipefd && !user_range_mapped(current_user_pagedir(), pipefd, sizeof(int) * 2))
        return SYSCALL_FAULT_(EFAULT);

    char buf[16];
    sprintf(buf, "pipe%d", pipefd_id++);

    vfs_node_t node_input = vfs_node_alloc(pipefs_root, buf);
    node_input->type      = file_pipe;
    node_input->fsid      = pipefs_id;
    pipefs_root->mode = 0700;

    sprintf(buf, "pipe%d", pipefd_id++);
    vfs_node_t node_output = vfs_node_alloc(pipefs_root, buf);
    node_output->type      = file_pipe;
    node_output->fsid      = pipefs_id;
    pipefs_root->mode = 0700;

    uint64_t pipe_inode = pipe_inode_next++;
    node_input->inode   = pipe_inode;
    node_output->inode  = pipe_inode;
    node_input->dev     = PIEFS_REGISTER_ID;
    node_output->dev    = PIEFS_REGISTER_ID;

    pipe_info_t *info = (pipe_info_t *)malloc(sizeof(pipe_info_t));
    memset(info, 0, sizeof(pipe_info_t));
    info->buf       = (char*)calloc(1, PIPE_BUFF);
    info->read_fds  = 1;
    info->write_fds = 1;
    info->ptr       = 0;
    info->lock      = SPIN_INIT;

    pipe_specific_t *read_spec = (pipe_specific_t *)malloc(sizeof(pipe_specific_t));
    read_spec->write           = false;
    read_spec->info            = info;
    read_spec->node            = node_input;

    pipe_specific_t *write_spec = (pipe_specific_t *)malloc(sizeof(pipe_specific_t));
    write_spec->write           = true;
    write_spec->info            = info;
    write_spec->node            = node_output;

    info->read_spec  = read_spec;
    info->write_spec = write_spec;

    node_input->handle  = read_spec;
    node_output->handle = write_spec;

    lock_queue     *queue     = get_current_task()->parent_group->file_open;
    fd_file_handle *handle_in = (fd_file_handle*)malloc(sizeof(fd_file_handle));
    handle_in->node           = node_input;
    handle_in->offset         = 0;
    handle_in->flags          = (flags & ~O_ACCMODE) | O_RDONLY;
    handle_in->node->flags    = handle_in->flags;
    handle_in->fd             = queue_enqueue_lowest(queue, handle_in);

    fd_file_handle *handle_out = (fd_file_handle*)malloc(sizeof(fd_file_handle));
    handle_out->node           = node_output;
    handle_out->offset         = 0;
    handle_out->flags          = (flags & ~O_ACCMODE) | O_WRONLY;
    handle_out->node->flags    = handle_out->flags;
    handle_out->fd             = queue_enqueue_lowest(queue, handle_out);

    int kpipefd[2] = {(int)handle_in->fd, (int)handle_out->fd};
    if (kernel_pipefd)
    {
        memcpy(pipefd, kpipefd, sizeof(kpipefd));
    }
    else if (!copy_to_user_pagedir(current_user_pagedir(), pipefd, kpipefd, sizeof(kpipefd)))
    {
        return SYSCALL_FAULT_(EFAULT);
    }

    return 0;
}

sys_(pipe, int *pipefd)
{
    return sys_pipe2(pipefd, 0, 0, 0, 0, 0, regs);
}

sys_(unlink)
{
    char *name = (char *)arg0;
    if (name == NULL) return SYSCALL_FAULT_(EINVAL);
    char *user_path = NULL;
    int copy_ret = copy_string_from_user(&user_path, name, USER_PATH_MAX);
    if (copy_ret < 0) return SYSCALL_FAULT_((int)-copy_ret);

    char      *npath = vfs_cwd_path_build(user_path);
    free(user_path);
    if (npath == NULL) return SYSCALL_FAULT_(ENOMEM);
    vfs_node_t node  = vfs_open_no_follow(npath);
    if (node == NULL) { free(npath); return SYSCALL_FAULT_(ENOENT); }
    if (node->type != file_none && node->type != file_symlink)
    {
        vfs_close(node);
        free(npath);
        return SYSCALL_FAULT_(ENOTDIR);
    }
    size_t ret = vfs_delete(node) == VFS_STATUS_SUCCESS ? 0 : SYSCALL_FAULT_(ENOENT);
    free(npath);
    return ret;
}

sys_(rmdir)
{
    char *name = (char *)arg0;
    if (name == NULL) return SYSCALL_FAULT_(EINVAL);
    char *user_path = NULL;
    int copy_ret = copy_string_from_user(&user_path, name, USER_PATH_MAX);
    if (copy_ret < 0) return SYSCALL_FAULT_((int)-copy_ret);

    char      *n_name = vfs_cwd_path_build(user_path);
    free(user_path);
    if (n_name == NULL) return SYSCALL_FAULT_(ENOMEM);
    vfs_node_t node   = vfs_open_no_follow(n_name);
    if (node == NULL) { free(n_name); return SYSCALL_FAULT_(ENOENT); }
    if (node->type != file_dir)
    {
        vfs_close(node);
        free(n_name);
        return SYSCALL_FAULT_(ENOTDIR);
    }
    size_t ret = vfs_delete(node);
    free(n_name);
    return ret;
}

sys_(unlinkat)
{
    int   dirfd = (int)arg0;
    char *name  = (char *)arg1;
    if (name == NULL) return SYSCALL_FAULT_(EINVAL);
    char *path = NULL;
    int path_ret = resolve_user_path_at(dirfd, name, &path);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);
    if (!path) return -ENOENT;

    vfs_node_t node = vfs_open_no_follow(path);
    if (node == NULL)
    {
        free(path);
        return SYSCALL_FAULT_(ENOENT);
    }
    if (node->type != file_none && node->type != file_symlink)
    {
        vfs_close(node);
        free(path);
        return SYSCALL_FAULT_(ENOTDIR);
    }

    uint64_t ret;
    if (node->refcount > 1)
    {
        node->refcount--;
        ret = 0;
    }
    else
    {
        ret = vfs_delete(node) == VFS_STATUS_SUCCESS ? 0 : SYSCALL_FAULT_(ENOENT);
        node = NULL;
    }

    if (node != NULL) vfs_close(node);
    free(path);
    return ret;
}

sys_(access, char *filename)
{
    if (filename == NULL) return SYSCALL_FAULT_(EINVAL);
    char *user_path = NULL;
    int ret = copy_string_from_user(&user_path, filename, USER_PATH_MAX);
    if (ret < 0) return SYSCALL_FAULT_((int)-ret);

    char *path = vfs_cwd_path_build(user_path);
    free(user_path);
    if (path == NULL) return SYSCALL_FAULT_(ENOMEM);

    struct stat buf;
    uint64_t stat_ret = stat_kernel_path(path, &buf);
    free(path);
    return stat_ret;
}

sys_(faccessat)
{
    int      dirfd    = arg0;
    char    *pathname = (char *)arg1;
    uint64_t mode     = arg2;
    (void)mode;
    if (pathname == NULL) return SYSCALL_FAULT_(EINVAL);

    char *resolved = NULL;
    int path_ret = resolve_user_path_at(dirfd, pathname, &resolved);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);

    struct stat buf;
    uint64_t ret = stat_kernel_path(resolved, &buf);
    free(resolved);

    return ret;
}

sys_(faccessat2)
{
    uint64_t dirfd    = arg0;
    char    *pathname = (char *)arg1;
    uint64_t mode     = arg2;
    uint64_t flag     = arg3;
    (void)mode;
    (void)flag;
    if (pathname == NULL) return SYSCALL_FAULT_(EINVAL);

    char *resolved = NULL;
    int path_ret = resolve_user_path_at(dirfd, pathname, &resolved);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);

    struct stat buf;
    uint64_t ret = stat_kernel_path(resolved, &buf);
    free(resolved);

    return ret;
}

sys_(openat)
{
    int      dirfd = arg0;
    char    *name  = (char *)arg1;
    uint64_t flags = arg2;
    uint64_t mode  = arg3;

    char *path = NULL;
    int path_ret = resolve_user_path_at(dirfd, name, &path);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);

    uint64_t ret = open_kernel_path(path, flags, mode);

    free(path);

    return ret;
}

sys_(mkdirat, int dirfd, char *pathname, uint64_t mode)
{
    (void)mode;
    if (pathname == NULL) return SYSCALL_FAULT_(EINVAL);

    char *resolved = NULL;
    int path_ret = resolve_user_path_at(dirfd, pathname, &resolved);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);

    errno_t ret = vfs_mkdir(resolved);
    free(resolved);
    return ret == VFS_STATUS_SUCCESS ? 0 : SYSCALL_FAULT_(EIO);
}

sys_(flistxattr, int fd, char *list, size_t size)
{
    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL || handle->node == NULL) return SYSCALL_FAULT_(EBADF);
    if (size != 0 && list == NULL) return SYSCALL_FAULT_(EFAULT);
    if (size != 0 && !user_range_mapped(current_user_pagedir(), list, size)) return SYSCALL_FAULT_(EFAULT);
    return 0;
}



sys_(copy_file_range, int fd_in, uint64_t *off_in, int fd_out, uint64_t *off_out, size_t len, uint64_t flags)
{
    if (flags != 0) { return SYSCALL_FAULT_(EINVAL); }
    if (len == 0) return 0;

    pcb_t process = get_current_task()->parent_group;
    page_directory_t *pagedir = process->pagedir;
    fd_file_handle *src_handle =  (fd_file_handle*)queue_get(process->file_open, fd_in);
    fd_file_handle *dst_handle =  (fd_file_handle*)queue_get(process->file_open, fd_out);
    if (src_handle == NULL || dst_handle == NULL) { return SYSCALL_FAULT_(EBADF); }
    if (src_handle->node == NULL || dst_handle->node == NULL) return SYSCALL_FAULT_(EBADF);
    if (src_handle->node->type & (file_socket | file_pipe) || dst_handle->node->type & (file_socket | file_pipe))
        return SYSCALL_FAULT_(EINVAL);

    uint64_t src_offset = src_handle->offset;
    uint64_t dst_offset = dst_handle->offset;
    if (off_in != NULL && !copy_from_user_pagedir(pagedir, &src_offset, off_in, sizeof(src_offset)))
        return SYSCALL_FAULT_(EFAULT);
    if (off_out != NULL && !copy_from_user_pagedir(pagedir, &dst_offset, off_out, sizeof(dst_offset)))
        return SYSCALL_FAULT_(EFAULT);

    if (src_handle->node->size != (uint64_t)-1 && src_offset >= src_handle->node->size) return 0;

    size_t remaining = len;
    if (src_handle->node->size != (uint64_t)-1)
    {
        uint64_t available = src_handle->node->size - src_offset;
        if ((uint64_t)remaining > available) remaining = (size_t)available;
    }

    uint8_t *buffer = (uint8_t *)malloc(SENDFILE_BUF_SIZE);
    if (buffer == NULL) return SYSCALL_FAULT_(ENOMEM);

    size_t copy_total = 0;
    while (remaining > 0)
    {
        size_t chunk = remaining < SENDFILE_BUF_SIZE ? remaining : SENDFILE_BUF_SIZE;
        size_t got = vfs_read(src_handle->node, buffer, src_offset, chunk);
        if (got == (size_t)VFS_STATUS_FAILED)
        {
            free(buffer);
            return copy_total > 0 ? copy_total : SYSCALL_FAULT_(EIO);
        }
        if (got == 0) break;

        size_t wrote = vfs_write(dst_handle->node, buffer, dst_offset, got);
        if (wrote == (size_t)VFS_STATUS_FAILED)
        {
            free(buffer);
            return copy_total > 0 ? copy_total : SYSCALL_FAULT_(EIO);
        }
        if (wrote == 0) break;

        src_offset += wrote;
        dst_offset += wrote;
        copy_total += wrote;
        remaining -= wrote;
        if (wrote < got) break;
    }

    vfs_update(dst_handle->node);
    free(buffer);

    if (off_in != NULL)
    {
        if (!copy_to_user_pagedir(pagedir, off_in, &src_offset, sizeof(src_offset))) return SYSCALL_FAULT_(EFAULT);
    }
    else
    {
        src_handle->offset = (size_t)src_offset;
    }

    if (off_out != NULL)
    {
        if (!copy_to_user_pagedir(pagedir, off_out, &dst_offset, sizeof(dst_offset))) return SYSCALL_FAULT_(EFAULT);
    }
    else
    {
        dst_handle->offset = (size_t)dst_offset;
    }

    return copy_total;
}

sys_(pread, int fd, uint8_t *buffer, size_t size, uint64_t offset)
{
    if (unlikely(fd < 0 || buffer == NULL)) return SYSCALL_FAULT_(EINVAL);
    if (unlikely(size == 0)) return 0;

    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL || handle->node == NULL) return SYSCALL_FAULT_(EBADF);
    if (handle->node->type & file_socket) return SYSCALL_FAULT_(ESPIPE);
    if (handle->node->type & file_pipe) return SYSCALL_FAULT_(ESPIPE);

    if (handle->node->size != (uint64_t)-1 && offset >= handle->node->size) return 0;

    int64_t ret = read_to_user_buffer(handle, buffer, size, false, (size_t)offset, false);
    return ret < 0 ? SYSCALL_FAULT_((int)-ret) : (uint64_t)ret;
}

sys_(pwrite, int fd, uint8_t *buffer, size_t size, int64_t offset)
{
    if (unlikely(fd < 0 || buffer == NULL)) return SYSCALL_FAULT_(EINVAL);
    if (unlikely(size == 0)) return 0;
    if (unlikely(offset < 0)) return SYSCALL_FAULT_(EINVAL);

    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL || handle->node == NULL) return SYSCALL_FAULT_(EBADF);
    if (handle->node->type & file_socket) return SYSCALL_FAULT_(ESPIPE);
    if (handle->node->type & file_pipe) return SYSCALL_FAULT_(ESPIPE);

    size_t old_offset = handle->offset;
    handle->offset = (size_t)offset;
    int64_t ret = write_from_user_buffer(handle, buffer, size, false);
    handle->offset = old_offset;
    return ret < 0 ? SYSCALL_FAULT_((int)-ret) : (uint64_t)ret;
}

sys_(mount, char *dev_name, char *dir_name, char *type, uint64_t flags, void *data)
{
    if (dir_name == NULL) return SYSCALL_FAULT_(EINVAL);

    char *udir_name = NULL;
    int copy_ret = copy_string_from_user(&udir_name, dir_name, USER_PATH_MAX);
    if (copy_ret < 0) return SYSCALL_FAULT_((int)-copy_ret);

    char      *ndir_name = vfs_cwd_path_build(udir_name);
    free(udir_name);
    if (ndir_name == NULL) return SYSCALL_FAULT_(ENOMEM);
    vfs_node_t dir       = vfs_open((const char *)ndir_name);
    if (!dir)
    {
        free(ndir_name);
        return SYSCALL_FAULT_(ENOENT);
    }

    if (flags & MS_MOVE)
    {
        if (flags & (MS_REMOUNT | MS_BIND))
        {
            free(ndir_name);
            return SYSCALL_FAULT_(EINVAL);
        }
        if (dev_name == NULL) { free(ndir_name); return SYSCALL_FAULT_(EINVAL); }
        char *udev_name = NULL;
        copy_ret = copy_string_from_user(&udev_name, dev_name, USER_PATH_MAX);
        if (copy_ret < 0) { free(ndir_name); return SYSCALL_FAULT_((int)-copy_ret); }
        char      *old_root_p = vfs_cwd_path_build(udev_name);
        free(udev_name);
        if (old_root_p == NULL) { free(ndir_name); return SYSCALL_FAULT_(ENOMEM); }
        vfs_node_t old_root   = vfs_open(old_root_p);
        free(old_root_p);
        if (old_root == NULL || !old_root->is_mount) return SYSCALL_FAULT_(EINVAL);
        if (dir != rootdir) list_append(dir->parent->child, old_root);
        char *nb       = old_root->name;
        old_root->name = dir->name;
        dir->name      = nb;
        list_append(old_root->parent->child, dir);

        list_delete(old_root->parent->child, old_root);
        if (dir != rootdir)
            list_delete(dir->parent->child, dir);
        else
            rootdir = old_root;

        vfs_node_t parent = dir->parent;
        dir->parent       = old_root->parent;
        old_root->parent  = parent;

        vfs_close(old_root);
        vfs_close(dir);
        return 0;
    }

    if (type == NULL) return SYSCALL_FAULT_(EINVAL);

    char            *ndev_name = NULL;
    char            *udev_name = NULL;
    char            *ktype = NULL;
    copy_ret = copy_string_from_user(&ktype, type, USER_PATH_MAX);
    if (copy_ret < 0) { free(ndir_name); return SYSCALL_FAULT_((int)-copy_ret); }
    vfs_filesystem_t filesystem = get_filesystem(ktype);
    free(ktype);
    if (filesystem == NULL) { free(ndir_name); return SYSCALL_FAULT_(EINVAL); }
    if (filesystem->id != 0)
    {
        ndev_name = (char *)filesystem->id;
        goto mount;
    }

    if (dev_name == NULL) { free(ndir_name); return SYSCALL_FAULT_(EINVAL); }
    copy_ret = copy_string_from_user(&udev_name, dev_name, USER_PATH_MAX);
    if (copy_ret < 0) { free(ndir_name); return SYSCALL_FAULT_((int)-copy_ret); }
    ndev_name = vfs_cwd_path_build(udev_name);
    free(udev_name);
    if (ndev_name == NULL) { free(ndir_name); return SYSCALL_FAULT_(ENOMEM); }
mount:
    if (vfs_mount((const char *)ndev_name, dir) == VFS_STATUS_FAILED)
    {
        free(ndir_name);
        free(ndev_name);
        return SYSCALL_FAULT_(ENOENT);
    }
    free(ndir_name);
    if (filesystem->id == 0) free(ndev_name);
    return 0;
}

sys_(setpgid, int pid, int pgid)
{
    pcb_t process = pid == 0 ? get_current_task()->parent_group : found_pcb(pid);
    if (process == NULL || process->status == DEATH) { return SYSCALL_FAULT_(ESRCH); }
    if (pgid == 0) { pgid = process->pid; }
    process->pid = pgid;

    // Keep the controlling tty foreground pgrp in sync for Linux userland job control.
    tcb_t current = get_current_task();
    if (current != NULL && current->parent_group == process)
    {
        for (int fd = 0; fd <= 2; fd++)
        {
            fd_file_handle *handle = (fd_file_handle *)queue_get(process->file_open, fd);
            if (handle == NULL || handle->node == NULL) continue;
            vfs_ioctl(handle->node, TIOCSPGRP, &pgid);
        }
    }
    return 0;
}

sys_(getpgid)
{
    size_t pid     = arg0;
    pcb_t  process = pid == 0 ? get_current_task()->parent_group : found_pcb(pid);
    if (process == NULL || process->status == DEATH) { return SYSCALL_FAULT_(ESRCH); }
    return process->pid;
}

sys_(setsid)
{
    pcb_t process = get_current_task()->parent_group;
    if (process == NULL) return SYSCALL_FAULT_(ESRCH);
    return process->pid;
}

sys_(getsid, int pid)
{
    pcb_t process = pid == 0 ? get_current_task()->parent_group : found_pcb(pid);
    if (process == NULL || process->status == DEATH) return SYSCALL_FAULT_(ESRCH);
    return process->pid;
}


sys_(getgroups, int count, int *gid_list)
{
    if (count < 0) return SYSCALL_FAULT_(EINVAL);
    if (count > 0)
    {
        if (gid_list == NULL) return SYSCALL_FAULT_(EINVAL);
        int gid = (int)current_gid();
        if (!copy_to_user_pagedir(current_user_pagedir(), gid_list, &gid, sizeof(gid)))
            return SYSCALL_FAULT_(EFAULT);
        return 1;
    }
    return 1;
}

sys_(setgroups, int count, const int *gid_list)
{
    if (count < 0) return SYSCALL_FAULT_(EINVAL);
    if (count > 65536) return SYSCALL_FAULT_(EINVAL);
    if (count > 0 && gid_list == NULL) return SYSCALL_FAULT_(EINVAL);
    if (count > 0 && !user_range_mapped(current_user_pagedir(), gid_list, sizeof(int) * (size_t)count))
        return SYSCALL_FAULT_(EFAULT);
    return current_uid() == 0 ? 0 : SYSCALL_FAULT_(EPERM);
}

sys_(flock, int fd, int operation)
{
    if ((operation & ~(1 | 2 | 4 | 8)) != 0) return SYSCALL_FAULT_(EINVAL);
    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL || handle->node == NULL) return SYSCALL_FAULT_(EBADF);
    return 0;
}

sys_(chroot, char *path)
{
    if (path == NULL) return SYSCALL_FAULT_(EINVAL);

    char *kpath = NULL;
    int ret = copy_string_from_user(&kpath, path, USER_PATH_MAX);
    if (ret < 0) return SYSCALL_FAULT_((int)-ret);

    char *resolved = vfs_cwd_path_build(kpath);
    free(kpath);
    if (resolved == NULL) return SYSCALL_FAULT_(ENOMEM);

    vfs_node_t node = vfs_open(resolved);
    free(resolved);
    if (node == NULL) return SYSCALL_FAULT_(ENOENT);

    bool is_dir = (node->type & file_dir) != 0;
    vfs_close(node);
    if (!is_dir) return SYSCALL_FAULT_(ENOTDIR);
    return current_uid() == 0 ? 0 : SYSCALL_FAULT_(EPERM);
}

char *get_parent_path(const char *path) {
    if (!path || !*path) return strdup(".");

    char *copy = strdup(path);
    if (!copy) return NULL;

    char *last_slash = strrchr(copy, '/');

    if (last_slash && last_slash != copy) {
        *last_slash = '\0';
    } else if (last_slash == copy) {
        copy[1] = '\0';
    } else {
        free(copy);
        return strdup(".");
    }

    return copy;
}


sys_(rename, char *oldpath, char *newpath)
{
    if (!oldpath || !newpath) return SYSCALL_FAULT_(EINVAL);
    char *uoldpath = NULL;
    char *unewpath = NULL;
    int copy_ret = copy_string_from_user(&uoldpath, oldpath, USER_PATH_MAX);
    if (copy_ret < 0) return SYSCALL_FAULT_((int)-copy_ret);
    copy_ret = copy_string_from_user(&unewpath, newpath, USER_PATH_MAX);
    if (copy_ret < 0) { free(uoldpath); return SYSCALL_FAULT_((int)-copy_ret); }

    char      *noldpath = vfs_cwd_path_build(uoldpath);
    char      *nnewpath = vfs_cwd_path_build(unewpath);
    free(uoldpath);
    free(unewpath);
    if (noldpath == NULL || nnewpath == NULL)
    {
        free(noldpath);
        free(nnewpath);
        return SYSCALL_FAULT_(ENOMEM);
    }
    vfs_node_t oldnode  = vfs_open_no_follow(noldpath);
    if (!oldnode)
    {
        free(noldpath);
        free(nnewpath);
        return SYSCALL_FAULT_(ENOENT);
    }
    vfs_node_t newnode = vfs_open_no_follow(nnewpath);
    if (newnode == oldnode)
    {
        free(noldpath);
        free(nnewpath);
        return 0;
    }
    if (newnode) { vfs_delete(newnode); }
    size_t     ret    = vfs_rename(oldnode, nnewpath) == VFS_STATUS_SUCCESS ? 0 : SYSCALL_FAULT_(ENOENT);
    char      *parent = get_parent_path(nnewpath);
    vfs_node_t parent_dir = vfs_open(parent);
    if (!parent_dir)
    {
        free(parent);
        ret = SYSCALL_FAULT_(ENOENT);
        goto end_rename;
    }
    vfs_close(parent_dir); // 更新父目录的信息
    free(parent);
end_rename:
    free(noldpath);
    free(nnewpath);
    return ret;
}

sys_(renameat, int olddirfd, char *oldpath, int newdirfd, char *newpath)
{
    if (oldpath == NULL || newpath == NULL) return SYSCALL_FAULT_(EINVAL);

    char *resolved_old = NULL;
    char *resolved_new = NULL;
    int path_ret = resolve_user_path_at(olddirfd, oldpath, &resolved_old);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);
    path_ret = resolve_user_path_at(newdirfd, newpath, &resolved_new);
    if (path_ret < 0)
    {
        free(resolved_old);
        return SYSCALL_FAULT_((int)-path_ret);
    }

    vfs_node_t oldnode = vfs_open_no_follow(resolved_old);
    if (!oldnode)
    {
        free(resolved_old);
        free(resolved_new);
        return SYSCALL_FAULT_(ENOENT);
    }

    vfs_node_t newnode = vfs_open_no_follow(resolved_new);
    if (newnode == oldnode)
    {
        free(resolved_old);
        free(resolved_new);
        return 0;
    }

    /* Defer the destination deletion until the rename has actually succeeded,
     * otherwise a failed rename would silently lose the destination node. */
    uint64_t ret = vfs_rename(oldnode, resolved_new) == VFS_STATUS_SUCCESS ? 0 : SYSCALL_FAULT_(ENOENT);
    if (ret == 0 && newnode) vfs_delete(newnode);
    vfs_close(oldnode);
    free(resolved_old);
    free(resolved_new);
    return ret;
}

sys_(symlink, char *name, char *nw)
{
    if (name == NULL || nw == NULL) return SYSCALL_FAULT_(EINVAL);
    char *kname = NULL;
    int copy_ret = copy_string_from_user(&kname, name, USER_PATH_MAX);
    if (copy_ret < 0) return SYSCALL_FAULT_((int)-copy_ret);

    char *knw = NULL;
    int path_ret = resolve_user_path_at(AT_FDCWD, nw, &knw);
    if (path_ret < 0)
    {
        free(kname);
        return SYSCALL_FAULT_((int)-path_ret);
    }

    errno_t ret = vfs_symlink(knw, kname);
    free(kname);
    free(knw);
    return ret;
}

sys_(symlinkat, char *target, int newdirfd, char *linkpath)
{
    if (target == NULL || linkpath == NULL) return SYSCALL_FAULT_(EINVAL);

    char *ktarget = NULL;
    int copy_ret = copy_string_from_user(&ktarget, target, USER_PATH_MAX);
    if (copy_ret < 0) return SYSCALL_FAULT_((int)-copy_ret);

    char *klinkpath = NULL;
    int path_ret = resolve_user_path_at(newdirfd, linkpath, &klinkpath);
    if (path_ret < 0)
    {
        free(ktarget);
        return SYSCALL_FAULT_((int)-path_ret);
    }

    errno_t ret = vfs_symlink(klinkpath, ktarget);
    free(ktarget);
    free(klinkpath);
    return ret;
}

sys_(link, char *name, char *nw)
{
    if (name == NULL || nw == NULL) return SYSCALL_FAULT_(EINVAL);
    char *kname = NULL;
    char *knw = NULL;
    int path_ret = resolve_user_path_at(AT_FDCWD, name, &kname);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);
    path_ret = resolve_user_path_at(AT_FDCWD, nw, &knw);
    if (path_ret < 0) { free(kname); return SYSCALL_FAULT_((int)-path_ret); }
    errno_t ret = vfs_link(kname, knw);
    free(kname);
    free(knw);

    return ret;
}

sys_(linkat, int olddirfd, char *oldpath, int newdirfd, char *newpath, int flags)
{
    (void)flags;
    if (oldpath == NULL || newpath == NULL) return SYSCALL_FAULT_(EINVAL);

    char *resolved_old = NULL;
    char *resolved_new = NULL;
    int path_ret = resolve_user_path_at(olddirfd, oldpath, &resolved_old);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);
    path_ret = resolve_user_path_at(newdirfd, newpath, &resolved_new);
    if (path_ret < 0)
    {
        free(resolved_old);
        return SYSCALL_FAULT_((int)-path_ret);
    }

    errno_t ret = vfs_link(resolved_old, resolved_new);
    free(resolved_old);
    free(resolved_new);
    return ret;
}

sys_(umount2)
{
    char *user_path = NULL;
    int ret = copy_string_from_user(&user_path, (char *)arg0, USER_PATH_MAX);
    if (ret < 0) return SYSCALL_FAULT_((int)-ret);

    char *path = normalize_path(user_path);
    free(user_path);
    int flags = arg1;
    (void)flags;
    if (path == NULL) return SYSCALL_FAULT_(ENOMEM);

    uint64_t status = vfs_unmount(path);
    if (status == VFS_STATUS_FAILED) status = SYSCALL_FAULT_(EBUSY);
    free(path);
    return status;
}

sys_(readlink)
{
    char    *path = (char *)arg0;
    char    *buf  = (char *)arg1;
    uint64_t size = arg2;
    if (path == NULL || buf == NULL || size == 0) { return SYSCALL_FAULT_(EINVAL); }

    char *kpath = NULL;
    int ret = copy_string_from_user(&kpath, path, USER_PATH_MAX);
    if (ret < 0) return SYSCALL_FAULT_((int)-ret);

    char *resolved = vfs_cwd_path_build(kpath);
    free(kpath);
    if (resolved == NULL) return SYSCALL_FAULT_(ENOMEM);

    vfs_node_t node = vfs_open_no_follow(resolved);
    free(resolved);
    if (node == NULL) { return SYSCALL_FAULT_(ENOENT); }
    if (!(node->type & file_symlink))
    {
        vfs_close(node);
        return SYSCALL_FAULT_(EINVAL);
    }

    size_t copy_size = MIN((size_t)size, USER_PATH_MAX);
    char *kbuf = (char *)malloc(copy_size);
    if (kbuf == NULL)
    {
        vfs_close(node);
        return SYSCALL_FAULT_(ENOMEM);
    }

    size_t read = vfs_readlink(node, kbuf, copy_size);
    vfs_close(node);
    if (read == (size_t)VFS_STATUS_FAILED)
    {
        free(kbuf);
        return SYSCALL_FAULT_(EIO);
    }
    if (!copy_to_user_pagedir(current_user_pagedir(), buf, kbuf, read))
    {
        free(kbuf);
        return SYSCALL_FAULT_(EFAULT);
    }
    free(kbuf);
    return read;
}

sys_(readlinkat, int dirfd, char *path, char *buf, size_t size)
{
    if (path == NULL || buf == NULL || size == 0) return SYSCALL_FAULT_(EINVAL);

    char *resolved = NULL;
    int path_ret = resolve_user_path_at(dirfd, path, &resolved);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);

    vfs_node_t node = vfs_open_no_follow(resolved);
    free(resolved);
    if (node == NULL) return SYSCALL_FAULT_(ENOENT);
    if (!(node->type & file_symlink))
    {
        vfs_close(node);
        return SYSCALL_FAULT_(EINVAL);
    }

    size_t copy_size = MIN(size, USER_PATH_MAX);
    char *kbuf = (char *)malloc(copy_size);
    if (kbuf == NULL)
    {
        vfs_close(node);
        return SYSCALL_FAULT_(ENOMEM);
    }

    size_t read = vfs_readlink(node, kbuf, copy_size);
    vfs_close(node);
    if (read == (size_t)VFS_STATUS_FAILED)
    {
        free(kbuf);
        return SYSCALL_FAULT_(EIO);
    }
    if (!copy_to_user_pagedir(current_user_pagedir(), buf, kbuf, read))
    {
        free(kbuf);
        return SYSCALL_FAULT_(EFAULT);
    }
    free(kbuf);
    return read;
}

sys_(prlimit64, int pid, uint64_t resource, struct rlimit *new_limit, struct rlimit *old_limit)
{
    (void)resource;
    if (pid != 0)
    {
        pcb_t process = found_pcb(pid);
        if (process == NULL || process->status == DEATH) return SYSCALL_FAULT_(ESRCH);
        if (process != get_current_task()->parent_group) return SYSCALL_FAULT_(EPERM);
    }

    if (new_limit != NULL)
    {
        struct rlimit ignored;
        if (!copy_from_user_pagedir(current_user_pagedir(), &ignored, new_limit, sizeof(ignored)))
            return SYSCALL_FAULT_(EFAULT);
    }

    if (old_limit != NULL)
    {
        struct rlimit current;
        current.rlim_cur = 1024ULL * 1024ULL * 1024ULL;
        current.rlim_max = 1024ULL * 1024ULL * 1024ULL;
        if (!copy_to_user_pagedir(current_user_pagedir(), old_limit, &current, sizeof(current)))
            return SYSCALL_FAULT_(EFAULT);
    }

    return 0;
}

sys_(ftruncate, int fd, uint64_t length)
{
    if (fd < 0) return SYSCALL_FAULT_(EBADF);
    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL || handle->node == NULL) return SYSCALL_FAULT_(EBADF);
    if (handle->node->type == file_dir) return SYSCALL_FAULT_(EISDIR);
    if (vfs_resize(handle->node, length) != VFS_STATUS_SUCCESS) return SYSCALL_FAULT_(EIO);
    if (handle->offset > length) handle->offset = length;
    return 0;
}

sys_(fsync, int fd)
{
    if (fd < 0) return SYSCALL_FAULT_(EBADF);
    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL || handle->node == NULL) return SYSCALL_FAULT_(EBADF);
    vfs_update(handle->node);
    return 0;
}

sys_(fdatasync, int fd)
{
    return sys_fsync(fd, 0, 0, 0, 0, 0, regs);
}

sys_(fadvise64, int fd, uint64_t offset, uint64_t len, int advice)
{
    (void)offset;
    (void)len;

    if (fd < 0) return SYSCALL_FAULT_(EBADF);
    if (advice < 0 || advice > 5) return SYSCALL_FAULT_(EINVAL);

    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL || handle->node == NULL) return SYSCALL_FAULT_(EBADF);
    return 0;
}

sys_(sync_file_range, int fd, int64_t offset, int64_t nbytes, uint64_t flags)
{
    (void)offset;
    (void)nbytes;
    (void)flags;
    return sys_fsync(fd, 0, 0, 0, 0, 0, regs);
}

sys_(sync)
{
    if (rootdir != NULL) vfs_update(rootdir);
    return 0;
}

sys_(getrandom, void *buf, size_t buflen, uint64_t flags)
{
    (void)flags;
    if (buf == NULL) return SYSCALL_FAULT_(EINVAL);
    if (buflen == 0) return 0;
    if (!user_range_mapped(current_user_pagedir(), buf, buflen)) return SYSCALL_FAULT_(EFAULT);

    size_t bounce_len = MIN(buflen, USER_IO_BOUNCE_BYTES);
    uint8_t *bounce = (uint8_t *)malloc(bounce_len);
    if (bounce == NULL) return SYSCALL_FAULT_(ENOMEM);

    size_t written = 0;
    while (written < buflen)
    {
        size_t chunk = MIN(buflen - written, bounce_len);
        get_random_bytes(bounce, chunk);
        if (!copy_to_user_pagedir(current_user_pagedir(), (uint8_t *)buf + written, bounce, chunk))
        {
            free(bounce);
            return written > 0 ? written : SYSCALL_FAULT_(EFAULT);
        }
        written += chunk;
    }

    free(bounce);
    return written;
}


sys_(getuid)
{
    return current_uid();
}

sys_(getgid)
{
    return current_gid();
}

sys_(setuid, uint32_t uid)
{
    uint32_t euid = current_uid();
    if (uid == euid || euid == 0) return 0;
    return SYSCALL_FAULT_(EPERM);
}

sys_(setgid, uint32_t gid)
{
    uint32_t egid = current_gid();
    if (gid == egid || current_uid() == 0) return 0;
    return SYSCALL_FAULT_(EPERM);
}

static bool linux_id_arg_allowed(uint32_t id, uint32_t current)
{
    return id == (uint32_t)-1 || id == current || current_uid() == 0;
}

sys_(setresuid, uint32_t ruid, uint32_t euid, uint32_t suid)
{
    uint32_t uid = current_uid();
    if (linux_id_arg_allowed(ruid, uid) && linux_id_arg_allowed(euid, uid) && linux_id_arg_allowed(suid, uid))
        return 0;
    return SYSCALL_FAULT_(EPERM);
}

sys_(getresuid, uint32_t *ruid, uint32_t *euid, uint32_t *suid)
{
    uint32_t uid = current_uid();
    page_directory_t *pagedir = current_user_pagedir();
    if (ruid != NULL && !copy_to_user_pagedir(pagedir, ruid, &uid, sizeof(uid))) return SYSCALL_FAULT_(EFAULT);
    if (euid != NULL && !copy_to_user_pagedir(pagedir, euid, &uid, sizeof(uid))) return SYSCALL_FAULT_(EFAULT);
    if (suid != NULL && !copy_to_user_pagedir(pagedir, suid, &uid, sizeof(uid))) return SYSCALL_FAULT_(EFAULT);
    return 0;
}

sys_(setresgid, uint32_t rgid, uint32_t egid, uint32_t sgid)
{
    uint32_t gid = current_gid();
    if (linux_id_arg_allowed(rgid, gid) && linux_id_arg_allowed(egid, gid) && linux_id_arg_allowed(sgid, gid))
        return 0;
    return SYSCALL_FAULT_(EPERM);
}

sys_(getresgid, uint32_t *rgid, uint32_t *egid, uint32_t *sgid)
{
    uint32_t gid = current_gid();
    page_directory_t *pagedir = current_user_pagedir();
    if (rgid != NULL && !copy_to_user_pagedir(pagedir, rgid, &gid, sizeof(gid))) return SYSCALL_FAULT_(EFAULT);
    if (egid != NULL && !copy_to_user_pagedir(pagedir, egid, &gid, sizeof(gid))) return SYSCALL_FAULT_(EFAULT);
    if (sgid != NULL && !copy_to_user_pagedir(pagedir, sgid, &gid, sizeof(gid))) return SYSCALL_FAULT_(EFAULT);
    return 0;
}

sys_(setfsuid, uint32_t uid)
{
    uint32_t old_uid = current_uid();
    if (linux_id_arg_allowed(uid, old_uid)) return old_uid;
    return old_uid;
}

sys_(setfsgid, uint32_t gid)
{
    uint32_t old_gid = current_gid();
    if (linux_id_arg_allowed(gid, old_gid)) return old_gid;
    return old_gid;
}

sys_(geteuid)
{
    return current_uid();
}

sys_(getegid)
{
    return current_gid();
}

#define LINUX_CAPABILITY_VERSION_1 0x19980330U
#define LINUX_CAPABILITY_VERSION_2 0x20071026U
#define LINUX_CAPABILITY_VERSION_3 0x20080522U

static int linux_capability_u32s(uint32_t version)
{
    switch (version)
    {
    case LINUX_CAPABILITY_VERSION_1: return 1;
    case LINUX_CAPABILITY_VERSION_2:
    case LINUX_CAPABILITY_VERSION_3: return 2;
    default: return 0;
    }
}

sys_(capget, struct user_cap_header *header, struct user_cap_data *data)
{
    if (header == NULL) return SYSCALL_FAULT_(EINVAL);

    struct user_cap_header kheader;
    if (!copy_from_user_pagedir(current_user_pagedir(), &kheader, header, sizeof(kheader)))
        return SYSCALL_FAULT_(EFAULT);

    int u32s = linux_capability_u32s(kheader.version);
    if (u32s == 0)
    {
        kheader.version = LINUX_CAPABILITY_VERSION_3;
        if (!copy_to_user_pagedir(current_user_pagedir(), header, &kheader, sizeof(kheader)))
            return SYSCALL_FAULT_(EFAULT);
        return SYSCALL_FAULT_(EINVAL);
    }
    if (kheader.pid < 0) return SYSCALL_FAULT_(EINVAL);
    if (kheader.pid > 0)
    {
        pcb_t process = found_pcb(kheader.pid);
        if (process == NULL || process->status == DEATH) return SYSCALL_FAULT_(ESRCH);
    }
    if (data == NULL) return 0;

    struct user_cap_data kdata[2];
    memset(kdata, 0, sizeof(kdata));
    if (current_uid() == 0)
    {
        kdata[0].effective = 0xffffffffU;
        kdata[0].permitted = 0xffffffffU;
        if (u32s > 1)
        {
            kdata[1].effective = 0x000001ffU;
            kdata[1].permitted = 0x000001ffU;
        }
    }

    if (!copy_to_user_pagedir(current_user_pagedir(), data, kdata, sizeof(struct user_cap_data) * (size_t)u32s))
        return SYSCALL_FAULT_(EFAULT);
    return 0;
}

sys_(capset, struct user_cap_header *header, struct user_cap_data *data)
{
    if (header == NULL || data == NULL) return SYSCALL_FAULT_(EINVAL);

    struct user_cap_header kheader;
    if (!copy_from_user_pagedir(current_user_pagedir(), &kheader, header, sizeof(kheader)))
        return SYSCALL_FAULT_(EFAULT);
    if (linux_capability_u32s(kheader.version) == 0) return SYSCALL_FAULT_(EINVAL);
    if (kheader.pid < 0) return SYSCALL_FAULT_(EINVAL);
    if (kheader.pid > 0)
    {
        pcb_t process = found_pcb(kheader.pid);
        if (process == NULL || process->status == DEATH) return SYSCALL_FAULT_(ESRCH);
    }
    return current_uid() == 0 ? 0 : SYSCALL_FAULT_(EPERM);
}

sys_(sysinfo, struct sysinfo *info)
{
    if (info == NULL) return SYSCALL_FAULT_(EINVAL);

    struct sysinfo kinfo;
    memset(&kinfo, 0, sizeof(kinfo));
    kinfo.uptime   = (int64_t)(bootNanoTime() / 1000000000ULL);
    kinfo.totalram = frame_allocator.origin_frames * PAGE_SIZE;
    kinfo.freeram  = frame_allocator.usable_frames * PAGE_SIZE;
    kinfo.procs    = pcb_group_queue != NULL && pcb_group_queue->size > UINT16_MAX ? UINT16_MAX :
                     pcb_group_queue != NULL                                      ? (uint16_t)pcb_group_queue->size
                                                                                  : 0;
    kinfo.mem_unit = 1;

    if (!copy_to_user_pagedir(current_user_pagedir(), info, &kinfo, sizeof(kinfo)))
        return SYSCALL_FAULT_(EFAULT);
    return 0;
}

sys_(chmod, char *path, uint64_t mode)
{
    if (path == NULL) return SYSCALL_FAULT_(EINVAL);
    char *resolved = NULL;
    int path_ret = resolve_user_path_at(AT_FDCWD, path, &resolved);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);

    vfs_node_t node = vfs_open(resolved);
    free(resolved);
    if (node == NULL) return SYSCALL_FAULT_(ENOENT);

    if (!node_change_permitted(node, false, false)) {
        vfs_close(node);
        return SYSCALL_FAULT_(EPERM);
    }
    node->mode = (uint16_t)((node->mode & ~07777) | (mode & 07777));
    vfs_close(node);
    return 0;
}

sys_(fchmod, int fd, uint64_t mode)
{
    if (fd < 0) return SYSCALL_FAULT_(EBADF);
    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL || handle->node == NULL) return SYSCALL_FAULT_(EBADF);
    if (!node_change_permitted(handle->node, false, false)) return SYSCALL_FAULT_(EPERM);
    handle->node->mode = (uint16_t)((handle->node->mode & ~07777) | (mode & 07777));
    return 0;
}

static void apply_node_owner(vfs_node_t node, uint32_t owner, uint32_t group)
{
    if (node == NULL) return;
    if (owner != UINT32_MAX) node->owner = owner;
    if (group != UINT32_MAX) node->group = group;
}

sys_(chown, char *path, uint32_t owner, uint32_t group)
{
    return sys_fchownat(AT_FDCWD, path, owner, group, 0, 0, regs);
}

sys_(fchown, int fd, uint32_t owner, uint32_t group)
{
    if (fd < 0) return SYSCALL_FAULT_(EBADF);
    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL || handle->node == NULL) return SYSCALL_FAULT_(EBADF);
    if (!node_change_permitted(handle->node, owner != UINT32_MAX, group != UINT32_MAX)) {
        return SYSCALL_FAULT_(EPERM);
    }
    apply_node_owner(handle->node, owner, group);
    return 0;
}

sys_(fchownat, int dirfd, char *pathname, uint32_t owner, uint32_t group, int flags)
{
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0) return SYSCALL_FAULT_(EINVAL);
    if (pathname == NULL) return SYSCALL_FAULT_(EINVAL);
    char *resolved = NULL;
    int path_ret = resolve_user_path_at(dirfd, pathname, &resolved);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);

    vfs_node_t node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_open_no_follow(resolved) : vfs_open(resolved);
    free(resolved);
    if (node == NULL) return SYSCALL_FAULT_(ENOENT);

    if (!node_change_permitted(node, owner != UINT32_MAX, group != UINT32_MAX)) {
        vfs_close(node);
        return SYSCALL_FAULT_(EPERM);
    }
    apply_node_owner(node, owner, group);
    vfs_close(node);
    return 0;
}

sys_(fchmodat, int dirfd, char *pathname, uint64_t mode, int flags)
{
    (void)flags;
    if (pathname == NULL) return SYSCALL_FAULT_(EINVAL);
    char *resolved = NULL;
    int path_ret = resolve_user_path_at(dirfd, pathname, &resolved);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);

    vfs_node_t node = vfs_open(resolved);
    free(resolved);
    if (node == NULL) return SYSCALL_FAULT_(ENOENT);

    node->mode = (uint16_t)((node->mode & ~07777) | (mode & 07777));
    vfs_close(node);
    return 0;
}

sys_(utimensat, int dirfd, char *pathname, const struct timespec *times, int flags)
{
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0) return SYSCALL_FAULT_(EINVAL);
    if (times != NULL && !user_range_mapped(current_user_pagedir(), times, sizeof(struct timespec) * 2))
        return SYSCALL_FAULT_(EFAULT);

    char *kpath = NULL;
    bool target_fd = pathname == NULL;
    if (pathname != NULL)
    {
        int copy_ret = copy_string_from_user(&kpath, pathname, USER_PATH_MAX);
        if (copy_ret < 0) return SYSCALL_FAULT_((int)-copy_ret);
        target_fd = (flags & AT_EMPTY_PATH) && kpath[0] == '\0';
    }

    char *resolved = NULL;
    vfs_node_t node = NULL;
    bool close_node = true;
    if (target_fd)
    {
        if (dirfd == AT_FDCWD)
        {
            free(kpath);
            return SYSCALL_FAULT_(EINVAL);
        }
        fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, dirfd);
        if (handle == NULL || handle->node == NULL)
        {
            free(kpath);
            return SYSCALL_FAULT_(EBADF);
        }
        node = handle->node;
        close_node = false;
    }
    else
    {
        resolved = at_resolve_pathname(dirfd, kpath);
        free(kpath);
        kpath = NULL;
        if (resolved == NULL) return SYSCALL_FAULT_(ENOENT);
        node = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_open_no_follow(resolved) : vfs_open(resolved);
        free(resolved);
    }
    if (node == NULL) return SYSCALL_FAULT_(ENOENT);

    uint64_t now = realtime_ns();
    if (times == NULL)
    {
        node->readtime = now;
        node->writetime = now;
    }
    else
    {
        struct timespec ktimes[2];
        if (!copy_from_user_pagedir(current_user_pagedir(), ktimes, times, sizeof(ktimes)))
        {
            if (close_node) vfs_close(node);
            return SYSCALL_FAULT_(EFAULT);
        }
        for (int i = 0; i < 2; i++)
        {
            if (ktimes[i].tv_nsec != UTIME_NOW && ktimes[i].tv_nsec != UTIME_OMIT &&
                (ktimes[i].tv_nsec < 0 || ktimes[i].tv_nsec >= 1000000000L))
            {
                if (close_node) vfs_close(node);
                return SYSCALL_FAULT_(EINVAL);
            }
        }

        if (ktimes[0].tv_nsec == UTIME_NOW) node->readtime = now;
        else if (ktimes[0].tv_nsec != UTIME_OMIT)
        {
            if (ktimes[0].tv_sec < 0 ||
                (uint64_t)ktimes[0].tv_sec > (UINT64_MAX - (uint64_t)ktimes[0].tv_nsec) / 1000000000ULL)
            {
                if (close_node) vfs_close(node);
                return SYSCALL_FAULT_(EINVAL);
            }
            node->readtime = (uint64_t)ktimes[0].tv_sec * 1000000000ULL + (uint64_t)ktimes[0].tv_nsec;
        }

        if (ktimes[1].tv_nsec == UTIME_NOW) node->writetime = now;
        else if (ktimes[1].tv_nsec != UTIME_OMIT)
        {
            if (ktimes[1].tv_sec < 0 ||
                (uint64_t)ktimes[1].tv_sec > (UINT64_MAX - (uint64_t)ktimes[1].tv_nsec) / 1000000000ULL)
            {
                if (close_node) vfs_close(node);
                return SYSCALL_FAULT_(EINVAL);
            }
            node->writetime = (uint64_t)ktimes[1].tv_sec * 1000000000ULL + (uint64_t)ktimes[1].tv_nsec;
        }
    }

    if (close_node) vfs_close(node);
    return 0;
}

sys_(utimes, char *filename, const struct timeval *times)
{
    if (filename == NULL) return SYSCALL_FAULT_(EINVAL);
    if (times != NULL && !user_range_mapped(current_user_pagedir(), times, sizeof(struct timeval) * 2))
        return SYSCALL_FAULT_(EFAULT);

    char *resolved = NULL;
    int path_ret = resolve_user_path_at(AT_FDCWD, filename, &resolved);
    if (path_ret < 0) return SYSCALL_FAULT_((int)-path_ret);

    vfs_node_t node = vfs_open(resolved);
    free(resolved);
    if (node == NULL) return SYSCALL_FAULT_(ENOENT);

    if (times != NULL)
    {
        struct timeval ktv[2];
        if (!copy_from_user_pagedir(current_user_pagedir(), ktv, times, sizeof(ktv)))
        {
            vfs_close(node);
            return SYSCALL_FAULT_(EFAULT);
        }
        for (int i = 0; i < 2; i++)
        {
            if (ktv[i].tv_sec < 0 || ktv[i].tv_usec < 0 || ktv[i].tv_usec >= 1000000L)
            {
                vfs_close(node);
                return SYSCALL_FAULT_(EINVAL);
            }
        }
        node->readtime = (uint64_t)ktv[0].tv_sec * 1000000000ULL + (uint64_t)ktv[0].tv_usec * 1000ULL;
        node->writetime = (uint64_t)ktv[1].tv_sec * 1000000000ULL + (uint64_t)ktv[1].tv_usec * 1000ULL;
    }
    else
    {
        uint64_t now = realtime_ns();
        node->readtime = now;
        node->writetime = now;
    }

    vfs_close(node);
    return 0;
}

sys_(fsopen, char *fsname, uint32_t flags)
{

    vfs_filesystem_t filesystem = NULL;
    vfs_filesystem_t pos, n;
    llist_for_each(pos, n, &fs_metadata_list, node)
    {
        if (strcmp(pos->name, fsname) != 0)
        {
            filesystem = pos;
            break;
        }
    }

    if (filesystem == NULL) return SYSCALL_FAULT_(ENOENT);

    vfs_node_t node = vfs_node_alloc(NULL, NULL);
    node->type      = file_none;
    node->handle    = filesystem;
    node->size = 0;

    fd_file_handle *handle =  (fd_file_handle*)calloc(1, sizeof(fd_file_handle));
    handle->node           = node;
    handle->fd             = queue_enqueue_lowest(get_current_task()->parent_group->file_open, handle);
    handle->flags          = flags;
    return handle->fd;
}

sys_(getppid)
{
    return get_current_task()->parent_group->parent_task->pid;
}

sys_(mincore, uint64_t addr, uint64_t size, uint64_t vec)
{
    if (size == 0) { return 0; }

    if ((addr & (PAGE_SIZE - 1)) != 0) return SYSCALL_FAULT_(EINVAL);

    uint64_t aligned_size = 0;
    if (!align_up_u64(size, PAGE_SIZE, &aligned_size)) return SYSCALL_FAULT_(ENOMEM);
    if (check_user_overflow(addr, aligned_size)) return SYSCALL_FAULT_(ENOMEM);

    uint64_t num_pages = aligned_size / PAGE_SIZE;
    if (vec == 0 || check_user_overflow(vec, num_pages)) return SYSCALL_FAULT_(EFAULT);

    tcb_t task = get_current_task();
    pcb_t process = task != NULL ? task->parent_group : NULL;
    if (process == NULL || process->pagedir == NULL) return SYSCALL_FAULT_(EFAULT);

    uint8_t *kvec = (uint8_t *)malloc((size_t)num_pages);
    if (kvec == NULL) return SYSCALL_FAULT_(ENOMEM);

    mm_op_lock_acquire();
    for (uint64_t i = 0; i < num_pages; i++)
    {
        uint64_t page = addr + i * PAGE_SIZE;
        uint64_t flags = 0;
        bool present = page_table_get_flags(process->pagedir, page, &flags) &&
                       ((flags & (PTE_PRESENT | PTE_USER)) == (PTE_PRESENT | PTE_USER));
        if (vma_find(&process->vma_manager, page) == NULL && !present)
        {
            mm_op_lock_release();
            free(kvec);
            return SYSCALL_FAULT_(ENOMEM);
        }

        kvec[i] = present ? 1 : 0;
    }
    mm_op_lock_release();

    bool copied = copy_to_user_pagedir(process->pagedir, (void *)vec, kvec, (size_t)num_pages);
    free(kvec);
    if (!copied) return SYSCALL_FAULT_(EFAULT);
    return 0;
}

sys_(pivot_root, char *new_root, char *put_old)
{
    //TODO 未经测试和验证的实现
    vfs_node_t nroot = vfs_open(new_root);
    vfs_node_t src_root;
    if (nroot == NULL) return SYSCALL_FAULT_(ENOENT);
    vfs_node_t oroot;
    if (!nroot->is_mount)
    {
        vfs_close(nroot);
        goto error_pr;
    }
    oroot = vfs_open(put_old);
    if (oroot == NULL) return SYSCALL_FAULT_(ENOENT);
    if (!(oroot->type & file_dir)) goto error_pr;
    if (list_length(oroot->child) > 0) goto error_pr;
    if (oroot->root != nroot) goto error_pr;
    src_root = get_rootdir();
    set_rootdir(nroot);
    oroot->handle = src_root->handle;
    oroot->child  = src_root->child;
    return 0;
error_pr:
    return SYSCALL_FAULT_(EINVAL);
}


sys_(statfs, char *path, struct statfs *buf)
{
    if (path == NULL || buf == NULL) return SYSCALL_FAULT_(EINVAL);

    char *kpath = NULL;
    int ret = copy_string_from_user(&kpath, path, USER_PATH_MAX);
    if (ret < 0) return SYSCALL_FAULT_((int)-ret);

    vfs_node_t node = vfs_open(kpath);
    free(kpath);
    if (node == NULL) return SYSCALL_FAULT_(ENOENT);
    uint64_t status = statfs_kernel_node(node, buf);
    vfs_close(node);
    return status;
}

sys_(fstatfs, int fd, struct statfs *buf)
{
    if (fd < 0 || buf == NULL) return SYSCALL_FAULT_(EINVAL);
    fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, fd);
    if (handle == NULL || handle->node == NULL) return SYSCALL_FAULT_(EBADF);
    return statfs_kernel_node(handle->node, buf);
}


sys_(sendfile, int out_fd, int in_fd, uint64_t *offset_ptr, size_t count)
{
    pcb_t           process    = get_current_task()->parent_group;
    fd_file_handle *out_handle =  (fd_file_handle*)queue_get(process->file_open, out_fd);
    fd_file_handle *in_handle  =  (fd_file_handle*)queue_get(process->file_open, in_fd);
    if (out_handle == NULL || in_handle == NULL) return SYSCALL_FAULT_(EBADF);

    uint64_t current_offset = offset_ptr == NULL ? in_handle->offset : *offset_ptr;
    size_t   total_sent     = 0;

    size_t remaining = count;

    char *buffer = (char *)malloc(SENDFILE_BUF_SIZE);
    if (buffer == NULL) { return SYSCALL_FAULT_(ENOMEM); }

    while (remaining > 0)
    {
        size_t bytes_to_read = remaining < SENDFILE_BUF_SIZE ? remaining : SENDFILE_BUF_SIZE;
        size_t bytes_read;
        size_t bytes_written;
        bytes_read = vfs_read(in_handle->node, buffer, current_offset, bytes_to_read);
        if (bytes_read <= 0)
        {
            if (bytes_read == (size_t)VFS_STATUS_FAILED && total_sent == 0)
            {
                free(buffer);
                return SYSCALL_FAULT_(EIO);
            }
            break;
        }
        bytes_written = vfs_write(out_handle->node, buffer, out_handle->offset, bytes_read);
        if (bytes_written == (size_t)VFS_STATUS_FAILED)
        {
            if (total_sent == 0)
            {
                free(buffer);
                return SYSCALL_FAULT_(EIO);
            }
            break;
        }
        if (bytes_written < bytes_read) { bytes_read = bytes_written; }
        current_offset     += bytes_read;
        out_handle->offset += bytes_read;
        total_sent         += bytes_read;
        remaining          -= bytes_read;
    }
    free(buffer);
    if (offset_ptr != NULL) { *offset_ptr = current_offset; }
    else
    {
        in_handle->offset = current_offset;
    }
    return total_sent;
}
