#pragma once

#include <krlibc.h>
#include <proto.hpp>
#include <syscall/syscall.h>
#include <task/scheduler.h>

extern bool no_interrupt;

// Limit the IRQ scope to existing filesystem and blocking descriptor entry
// points. Process/image, memory mapping, device ioctl and unrelated syscalls
// retain their own entry contracts. Native file APIs use the same boundary.
static inline bool syscall_file_io_may_block(uint64_t number)
{
    switch (number)
    {
    case SYS_READ:
    case SYS_PREAD64:
    case SYS_READV:
    case SYS_WRITE:
    case SYS_PWRITE64:
    case SYS_WRITEV:
    case SYS_OPEN:
    case SYS_OPENAT:
    case SYS_CREAT:
    case SYS_CLOSE:
    case SYS_CLOSE_RANGE:
    case SYS_LSEEK:
    case SYS_FCNTL:
    case SYS_FLOCK:
    case SYS_DUP:
    case SYS_DUP2:
    case SYS_DUP3:
    case SYS_STAT:
    case SYS_LSTAT:
    case SYS_FSTAT:
    case SYS_NEWFSTATAT:
    case SYS_STATX:
    case SYS_STATFS:
    case SYS_FSTATFS:
    case SYS_ACCESS:
    case SYS_FACCESSAT:
    case SYS_FACCESSAT2:
    case SYS_GETDENTS:
    case SYS_GETDENTS64:
    case SYS_READLINK:
    case SYS_READLINKAT:
    case SYS_GETCWD:
    case SYS_CHDIR:
    case SYS_FCHDIR:
    case SYS_MKDIR:
    case SYS_MKDIRAT:
    case SYS_RMDIR:
    case SYS_UNLINK:
    case SYS_UNLINKAT:
    case SYS_RENAME:
    case SYS_RENAMEAT:
    case SYS_LINK:
    case SYS_LINKAT:
    case SYS_SYMLINK:
    case SYS_SYMLINKAT:
    case SYS_FTRUNCATE:
    case SYS_CHMOD:
    case SYS_FCHMOD:
    case SYS_FCHMODAT:
    case SYS_FCHMODAT2:
    case SYS_CHOWN:
    case SYS_LCHOWN:
    case SYS_FCHOWN:
    case SYS_FCHOWNAT:
    case SYS_UTIMES:
    case SYS_UTIMENSAT:
    case SYS_FSYNC:
    case SYS_FDATASYNC:
    case SYS_SYNC:
    case SYS_SYNC_FILE_RANGE:
    case SYS_FADVISE64:
    case SYS_SENDFILE:
    case SYS_COPY_FILE_RANGE:
    case SYS_SETXATTR:
    case SYS_LSETXATTR:
    case SYS_FSETXATTR:
    case SYS_GETXATTR:
    case SYS_LGETXATTR:
    case SYS_FGETXATTR:
    case SYS_LISTXATTR:
    case SYS_LLISTXATTR:
    case SYS_FLISTXATTR:
    case SYS_REMOVEXATTR:
    case SYS_LREMOVEXATTR:
    case SYS_FREMOVEXATTR:
    case SYS_POLL:
    case SYS_SELECT:
    case SYS_PSELECT6:
    case SYS_EPOLL_WAIT:
    case SYS_EPOLL_PWAIT:
    case XAPI_OPEN_FILE:
    case XAPI_CLOSE_FILE:
    case XAPI_SEARCH_FILE:
    case XAPI_SEARCH_FILE_FREEM:
    case XAPI_READ_FILE:
    case XAPI_WRITE_FILE:
    case XAPI_MAKEDIR:
    case XAPI_CREATE_FILE:
    case XAPI_DELETE_FILE:
    case XAPI_RENAME_FILE:
    case XAPI_REMOVEDIR:
    case XAPI_FILE_DIALOG:
    case XAPI_QUERY_FILE_ACCESS:
    case XAPI_REQUEST_FILE_ACCESS:
        return true;
    default:
        return false;
    }
}

// Entry assembly has saved the user registers/FPU image, and the C dispatcher
// has initialized its return frame and FSBASE before constructing this scope.
// File helpers retain their existing FD/VFS references and explicit user-page
// copies; this guard changes only whether their blocking work can be preempted.
class FileIoSyscallScope
{
    bool restore_required;
    bool entry_interrupts_enabled;

public:
    explicit FileIoSyscallScope(uint64_t number)
        : restore_required(syscall_file_io_may_block(number)), entry_interrupts_enabled(false)
    {
        if (!restore_required) return;
        entry_interrupts_enabled = are_interrupts_enabled();
        if (!no_interrupt && scheduler_is_enabled()) enable_intr();
    }

    void finish()
    {
        if (!restore_required) return;
        restore_required = false;
        if (entry_interrupts_enabled && !no_interrupt) enable_intr();
        else disable_intr();
    }

    ~FileIoSyscallScope() { finish(); }

    FileIoSyscallScope(const FileIoSyscallScope &) = delete;
    FileIoSyscallScope &operator=(const FileIoSyscallScope &) = delete;
};
