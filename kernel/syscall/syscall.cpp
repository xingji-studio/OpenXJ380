#include "../build_settings.h"
#include <cpu/fsgsbase.h>
#include <cpu/msr.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <installer_protocol.h>
#include <math.hpp>
#include <mm/lazyalloc.h>
#include <mm/vma.h>
#include <pctable/gdt.h>
#include <pctable/idt.h>
#include <pipe.h>
#include <proto.hpp>
#include <rtc.h>
#include <syscall/signal.h>
#include <syscall/syscall.h>
#include <task/poll.h>

// MSR寄存器地址定义
#define MSR_EFER         0xC0000080 // EFER MSR寄存器
#define MSR_STAR         0xC0000081 // STAR MSR寄存器
#define MSR_LSTAR        0xC0000082 // LSTAR MSR寄存器
#define MSR_SYSCALL_MASK 0xC0000084 // SYSCALL_MASK MSR寄存器
#define EFER_NXE         (1 << 11)

sys_(chmod, char *path, uint64_t mode);
sys_(fchmod, int fd, uint64_t mode);
sys_(chown, char *path, uint32_t owner, uint32_t group);
sys_(fchown, int fd, uint32_t owner, uint32_t group);
sys_(fchownat, int dirfd, char *pathname, uint32_t owner, uint32_t group, int flags);
sys_(fchmodat, int dirfd, char *pathname, uint64_t mode, int flags);
sys_(utimensat, int dirfd, char *pathname, const struct timespec *times, int flags);
sys_(utimes, char *filename, const struct timeval *times);
sys_(close_range, uint32_t first, uint32_t last, uint32_t flags);

static const char *debug_syscall_name(uint64_t nr)
{
    switch (nr)
    {
    case SYS_READ: return "read";
    case SYS_WRITE: return "write";
    case SYS_OPEN: return "open";
    case SYS_CLOSE: return "close";
    case SYS_FSTAT: return "fstat";
    case SYS_IOCTL: return "ioctl";
    case SYS_PREAD64: return "pread64";
    case SYS_PWRITE64: return "pwrite64";
    case SYS_POLL: return "poll";
    case SYS_LSEEK: return "lseek";
    case SYS_MMAP: return "mmap";
    case SYS_MPROTECT: return "mprotect";
    case SYS_MUNMAP: return "munmap";
    case SYS_BRK: return "brk";
    case SYS_MADVISE: return "madvise";
    case SYS_RT_SIGACTION: return "rt_sigaction";
    case SYS_RT_SIGPROCMASK: return "rt_sigprocmask";
    case SYS_ACCESS: return "access";
    case SYS_PIPE: return "pipe";
    case SYS_DUP3: return "dup3";
    case SYS_SELECT: return "select";
    case SYS_NANOSLEEP: return "nanosleep";
    case SYS_GETPID: return "getpid";
    case SYS_SOCKET: return "socket";
    case SYS_CONNECT: return "connect";
    case SYS_SENDTO: return "sendto";
    case SYS_RECVFROM: return "recvfrom";
    case SYS_SENDMSG: return "sendmsg";
    case SYS_RECVMSG: return "recvmsg";
    case SYS_GETSOCKNAME: return "getsockname";
    case SYS_GETPEERNAME: return "getpeername";
    case SYS_SETSOCKOPT: return "setsockopt";
    case SYS_GETSOCKOPT: return "getsockopt";
    case SYS_SOCKETPAIR: return "socketpair";
    case SYS_CLONE: return "clone";
    case SYS_FORK: return "fork";
    case SYS_VFORK: return "vfork";
    case SYS_EXECVE: return "execve";
    case SYS_EXIT: return "exit";
    case SYS_WAIT4: return "wait4";
    case SYS_UNAME: return "uname";
    case SYS_FCNTL: return "fcntl";
    case SYS_FLOCK: return "flock";
    case SYS_SYNC: return "sync";
    case SYS_FSYNC: return "fsync";
    case SYS_FDATASYNC: return "fdatasync";
    case SYS_SYNC_FILE_RANGE: return "sync_file_range";
    case SYS_SYSINFO: return "sysinfo";
    case SYS_SETUID: return "setuid";
    case SYS_SETGID: return "setgid";
    case SYS_SETGROUPS: return "setgroups";
    case SYS_SETRESUID: return "setresuid";
    case SYS_GETRESUID: return "getresuid";
    case SYS_SETRESGID: return "setresgid";
    case SYS_GETRESGID: return "getresgid";
    case SYS_SETFSUID: return "setfsuid";
    case SYS_SETFSGID: return "setfsgid";
    case SYS_CAPGET: return "capget";
    case SYS_CAPSET: return "capset";
    case SYS_GETPGRP: return "getpgrp";
    case SYS_SETSID: return "setsid";
    case SYS_GETSID: return "getsid";
    case SYS_GETCWD: return "getcwd";
    case SYS_CHDIR: return "chdir";
    case SYS_FCHDIR: return "fchdir";
    case SYS_GETTIMEOFDAY: return "gettimeofday";
    case SYS_GETDENTS64: return "getdents64";
    case SYS_RENAME: return "rename";
    case SYS_RMDIR: return "rmdir";
    case SYS_UNLINK: return "unlink";
    case SYS_STATFS: return "statfs";
    case SYS_MKDIRAT: return "mkdirat";
    case SYS_RENAMEAT: return "renameat";
    case SYS_LINKAT: return "linkat";
    case SYS_SYMLINKAT: return "symlinkat";
    case SYS_CLOCK_GETTIME: return "clock_gettime";
    case SYS_CLOCK_NANOSLEEP: return "clock_nanosleep";
    case SYS_EXIT_GROUP: return "exit_group";
    case SYS_UTIMES: return "utimes";
    case SYS_OPENAT: return "openat";
    case SYS_NEWFSTATAT: return "newfstatat";
    case SXAH_INSTALLER_ENUM_DISKS: return "sxah_installer_enum_disks";
    case SXAH_INSTALLER_START: return "sxah_installer_start";
    case SXAH_INSTALLER_PROGRESS: return "sxah_installer_progress";
    case SXAH_INSTALLER_PRECHECK: return "sxah_installer_precheck";
    case SXAH_INSTALLER_START_EX: return "sxah_installer_start_ex";
    case SXAH_INSTALLER_RESCUE: return "sxah_installer_rescue";
    case SXAH_INSTALLER_LOG: return "sxah_installer_log";
    case SXAH_INSTALLER_START_OPTIONS: return "sxah_installer_start_options";
    case SXAH_INSTALLER_PRECHECK_OPTIONS: return "sxah_installer_precheck_options";
    case SYS_READLINKAT: return "readlinkat";
    case SYS_PSELECT6: return "pselect6";
    case SYS_PIPE2: return "pipe2";
    case SYS_FTRUNCATE: return "ftruncate";
    case SYS_FSTATFS: return "fstatfs";
    case SYS_PRLIMIT64: return "prlimit64";
    case SYS_SENDMMSG: return "sendmmsg";
    case SYS_GETRANDOM: return "getrandom";
    case SYS_FUTEX: return "futex";
    case SYS_EPOLL_CREATE: return "epoll_create";
    case SYS_EPOLL_WAIT: return "epoll_wait";
    case SYS_EPOLL_CTL: return "epoll_ctl";
    case SYS_EPOLL_PWAIT: return "epoll_pwait";
    case SYS_EPOLL_CREATE1: return "epoll_create1";
    case SYS_PRCTL: return "prctl";
    case SYS_ARCH_PRCTL: return "arch_prctl";
    case SYS_CHROOT: return "chroot";
    case SYS_FLISTXATTR: return "flistxattr";
    case SYS_SET_TID_ADDRESS: return "set_tid_address";
    case SYS_FADVISE64: return "fadvise64";
    case SYS_COPY_FILE_RANGE: return "copy_file_range";
    case SYS_SET_ROBUST_LIST: return "set_robust_list";
    case SYS_RSEQ: return "rseq";
    case SYS_CLONE3: return "clone3";
    case SYS_CLOSE_RANGE: return "close_range";
    default: return "?";
    }
}

static bool debug_trace_xbps_task()
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL || !task->parent_group->linux_abi) return false;
    return strstr(task->name, "xbps") != NULL || strstr(task->parent_group->name, "xbps") != NULL;
}

static bool debug_trace_grep_task()
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL || !task->parent_group->linux_abi) return false;
    return strstr(task->name, "grep") != NULL || strstr(task->parent_group->name, "grep") != NULL;
}

void init_syscall()
{
    uint64_t efer;

    // 1. 启用 EFER.SCE (System Call Extensions)
    efer  = rdmsr(MSR_EFER);
    efer |= 1; // 设置 SCE 位
    wrmsr(MSR_EFER, efer);

    uint16_t cs_sysret_cmp  = SELECTOR_USER_CS - 16;
    uint16_t ss_sysret_cmp  = SELECTOR_USER_DS - 8;
    uint16_t cs_syscall_cmp = SELECTOR_KERNEL_CS;
    uint16_t ss_syscall_cmp = SELECTOR_KERNEL_DS - 8;

    if (cs_sysret_cmp != ss_sysret_cmp)
    {
        write_serial_string("Sysret offset is not valid (1)\n");
        return;
    }

    if (cs_syscall_cmp != ss_syscall_cmp)
    {
        write_serial_string("Syscall offset is not valid (2)\n");
        return;
    }

    // 2. 设置 STAR MSR
    uint64_t star = 0;
    star          = ((uint64_t)(SELECTOR_USER_DS - 8) << 48) | // SYSRET 的基础 CS
                    ((uint64_t)SELECTOR_KERNEL_CS << 32);      // SYSCALL 的 CS
    wrmsr(MSR_STAR, star);

    // 3. 设置 LSTAR MSR (系统调用入口点)
    wrmsr(MSR_LSTAR, (uint64_t)syscall_handler);

    // 4. 设置 SYSCALL_MASK MSR (RFLAGS 掩码)
    wrmsr(MSR_SYSCALL_MASK, (1 << 9));

    efer |= EFER_NXE;
    wrmsr(MSR_EFER, efer);
}

// regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8, regs->r9
extern "C" uint64_t c_syscall_handler(struct X64_REGS *regs, uint64_t user_rsp)
{
    regs->rip    = regs->rcx;
    regs->cs     = SELECTOR_USER_CS;
    regs->rflags = regs->r11;
    regs->rsp    = user_rsp;
    regs->ss     = SELECTOR_USER_DS;

    tcb_t syscall_task = get_current_task();
    if (syscall_task != NULL && syscall_task->task_level == TASK_APPLICATION_LEVEL)
    {
        uint64_t hw_fs_base = read_fsbase();
        if (hw_fs_base != 0 || syscall_task->fs_base == 0)
            syscall_task->fs_base = hw_fs_base;
        else
            write_fsbase(syscall_task->fs_base);
    }

    uint64_t syscall_number = regs->rax;
    bool trace_grep = debug_trace_grep_task();
    if (trace_grep)
    {
        write_serial_fmt("[DEBUG-grep-syscall] enter task=%s nr=%llu(%s) args=%llx,%llx,%llx,%llx,%llx rip=%llx\n",
                         syscall_task != NULL ? syscall_task->name : "?",
                         syscall_number,
                         debug_syscall_name(syscall_number),
                         regs->rdi,
                         regs->rsi,
                         regs->rdx,
                         regs->r10,
                         regs->r8,
                         regs->rip);
    }

    switch (syscall_number)
    {

    case SYS_WRITE: regs->rax = (uint64_t)sys_write(regs->rdi, (uint8_t *)regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_READ: regs->rax = (uint64_t)sys_read(regs->rdi, (uint8_t *)regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_PREAD64:
        regs->rax = (uint64_t)sys_pread(regs->rdi, (uint8_t *)regs->rsi, regs->rdx, regs->r10, 0, 0, regs);
        break;
    case SYS_PWRITE64:
        regs->rax = (uint64_t)sys_pwrite(regs->rdi, (uint8_t *)regs->rsi, regs->rdx, (int64_t)regs->r10, 0, 0, regs);
        break;
    case SYS_OPEN: regs->rax = (uint64_t)sys_open((char *)regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_CLOSE: regs->rax = (uint64_t)sys_close(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_CLOSE_RANGE: regs->rax = (uint64_t)sys_close_range(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_POLL:
        regs->rax = (uint64_t)sys_poll((struct pollfd *)regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_EPOLL_CREATE:
        regs->rax = (uint64_t)sys_epoll_create(regs->rdi, 0, 0, 0, 0, 0, regs);
        break;
    case SYS_EPOLL_CREATE1:
        regs->rax = (uint64_t)sys_epoll_create1(regs->rdi, 0, 0, 0, 0, 0, regs);
        break;
    case SYS_EPOLL_CTL:
        regs->rax = (uint64_t)sys_epoll_ctl(regs->rdi, regs->rsi, regs->rdx,
                                            (struct epoll_event *)regs->r10, 0, 0, regs);
        break;
    case SYS_EPOLL_WAIT:
        regs->rax = (uint64_t)sys_epoll_wait(regs->rdi, (struct epoll_event *)regs->rsi, regs->rdx,
                                             regs->r10, 0, 0, regs);
        break;
    case SYS_EPOLL_PWAIT:
        regs->rax = (uint64_t)sys_epoll_pwait(regs->rdi, (struct epoll_event *)regs->rsi, regs->rdx,
                                              regs->r10, (sigset_t *)regs->r8, regs->r9, regs);
        break;
    case SYS_SOCKET: regs->rax = (uint64_t)sys_socket(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_CONNECT:
        regs->rax = (uint64_t)sys_connect(regs->rdi, (const struct sockaddr *)regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_ACCEPT:
        regs->rax =
            (uint64_t)sys_accept(regs->rdi, (struct sockaddr *)regs->rsi, (socklen_t *)regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_SENDTO:
        regs->rax = (uint64_t)sys_sendto(regs->rdi, (const void *)regs->rsi, regs->rdx, regs->r10,
                                         (const struct sockaddr *)regs->r8, regs->r9, regs);
        break;
    case SYS_RECVFROM:
        regs->rax = (uint64_t)sys_recvfrom(regs->rdi, (void *)regs->rsi, regs->rdx, regs->r10,
                                           (struct sockaddr *)regs->r8, (socklen_t *)regs->r9, regs);
        break;
    case SYS_SENDMSG:
        regs->rax = (uint64_t)sys_sendmsg(regs->rdi, (const struct msghdr *)regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_RECVMSG:
        regs->rax = (uint64_t)sys_recvmsg(regs->rdi, (struct msghdr *)regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_SENDMMSG:
        regs->rax = (uint64_t)sys_sendmmsg(regs->rdi, (struct mmsghdr *)regs->rsi, regs->rdx, regs->r10, 0, 0, regs);
        break;
    case SYS_SETSOCKOPT:
        regs->rax = (uint64_t)sys_setsockopt(regs->rdi, regs->rsi, regs->rdx, (const void *)regs->r10,
                                             regs->r8, 0, regs);
        break;
    case SYS_GETSOCKOPT:
        regs->rax = (uint64_t)sys_getsockopt(regs->rdi, regs->rsi, regs->rdx, (void *)regs->r10,
                                             (socklen_t *)regs->r8, 0, regs);
        break;
    case SYS_GETSOCKNAME:
        regs->rax = (uint64_t)sys_getsockname(regs->rdi, (struct sockaddr *)regs->rsi,
                                              (socklen_t *)regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_GETPEERNAME:
        regs->rax = (uint64_t)sys_getpeername(regs->rdi, (struct sockaddr *)regs->rsi,
                                             (socklen_t *)regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_SOCKETPAIR:
        regs->rax = (uint64_t)sys_socketpair(regs->rdi, regs->rsi, regs->rdx, (int *)regs->r10, 0, 0, regs);
        break;
    case SYS_STAT: regs->rax = (uint64_t)sys_stat((char *)regs->rdi, (struct stat *)regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_LSTAT:
        regs->rax =
            (uint64_t)sys_newfstatat(AT_FDCWD, (char *)regs->rdi, (struct stat *)regs->rsi,
                                     AT_SYMLINK_NOFOLLOW, 0, 0, regs);
        break;
    case SYS_FSTAT: regs->rax = (uint64_t)sys_fstat(regs->rdi, (struct stat *)regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_IOCTL: regs->rax = (uint64_t)sys_ioctl(regs->rdi, regs->rsi, (void *)regs->rdx, 0, 0, 0, regs); break;
    case SYS_READV:
        regs->rax = (uint64_t)sys_readv(regs->rdi, (struct iovec *)regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_WRITEV:
        regs->rax = (uint64_t)sys_writev(regs->rdi, (struct iovec *)regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_CREAT:
        regs->rax = (uint64_t)sys_open((char *)regs->rdi, O_CREAT | O_WRONLY | O_TRUNC, regs->rsi, 0, 0, 0, regs);
        break;
    case SYS_FCNTL: regs->rax = (uint64_t)sys_fcntl(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_FLOCK: regs->rax = (uint64_t)sys_flock(regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_SYNC: regs->rax = (uint64_t)sys_sync(0, 0, 0, 0, 0, 0, regs); break;
    case SYS_FSYNC: regs->rax = (uint64_t)sys_fsync(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_FDATASYNC: regs->rax = (uint64_t)sys_fdatasync(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_FADVISE64:
        regs->rax = (uint64_t)sys_fadvise64(regs->rdi, regs->rsi, regs->rdx, regs->r10, 0, 0, regs);
        break;
    case SYS_SYNC_FILE_RANGE:
        regs->rax = (uint64_t)sys_sync_file_range(regs->rdi, (int64_t)regs->rsi, (int64_t)regs->rdx, regs->r10, 0, 0, regs);
        break;
    case SYS_MKDIR: regs->rax = (uint64_t)sys_mkdir((char *)regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_RMDIR: regs->rax = (uint64_t)sys_rmdir(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_UNLINK: regs->rax = (uint64_t)sys_unlink(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_CHDIR: regs->rax = (uint64_t)sys_chdir((char *)regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_FCHDIR: regs->rax = (uint64_t)sys_fchdir(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_GETCWD: regs->rax = (uint64_t)sys_getcwd((char *)regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_LSEEK: regs->rax = (uint64_t)sys_lseek(regs->rdi, (int64_t)regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_UMASK: regs->rax = (uint64_t)sys_umask(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_GETDENTS:
        regs->rax = (uint64_t)sys_getdents(regs->rdi, (dirent *)regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_PIPE: regs->rax = (uint64_t)sys_pipe((int *)regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_PIPE2: regs->rax = (uint64_t)sys_pipe2((int *)regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_SELECT:
        regs->rax = (uint64_t)sys_select(regs->rdi, (uint8_t *)regs->rsi, (uint8_t *)regs->rdx, (uint8_t *)regs->r10,
                                         (struct timeval *)regs->r8, 0, regs);
        break;
    case SYS_PSELECT6:
        regs->rax = (uint64_t)sys_pselect6(regs->rdi, (fd_set *)regs->rsi, (fd_set *)regs->rdx, (fd_set *)regs->r10,
                                           (struct timespec *)regs->r8, (WeirdPselect6 *)regs->r9, regs);
        break;
    case SYS_SCHED_GETAFFINITY:
        regs->rax = (uint64_t)sys_sched_getaffinity(regs->rdi, regs->rsi, (void *)regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_DUP: regs->rax = (uint64_t)sys_dup(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_DUP2: regs->rax = (uint64_t)sys_dup2(regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_DUP3: regs->rax = (uint64_t)sys_dup3(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_SHUTDOWN: regs->rax = (uint64_t)sys_shutdown(regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_BIND:
        regs->rax = (uint64_t)sys_bind(regs->rdi, (const struct sockaddr *)regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_LISTEN: regs->rax = (uint64_t)sys_listen(regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;

    case SYS_RT_SIGACTION:
        regs->rax = (uint64_t)sys_rt_sigaction(regs->rdi, (sigaction_t *)regs->rsi, (sigaction_t *)regs->rdx, regs->r10,
                                               0, 0, regs);
        break;
    case SYS_RT_SIGPROCMASK:
        regs->rax =
            (uint64_t)sys_rt_sigprocmask(regs->rdi, (sigset_t *)regs->rsi, (sigset_t *)regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_RT_SIGRETURN: regs->rax = (uint64_t)sys_sigret(0, 0, 0, 0, 0, 0, regs); break;
    case SYS_SIGALTSTACK:
        regs->rax = (uint64_t)syscall_sigaltstack((stack_t *)regs->rdi, (stack_t *)regs->rsi);
        break;

    case SYS_CLONE:
        regs->rax = (uint64_t)sys_clone(regs->rdi, regs->rsi, (int *)regs->rdx, (int *)regs->r10, regs->r8, 0, regs);
        break;
    case SYS_CLONE3:
        regs->rax = (uint64_t)sys_clone3((struct clone_args *)regs->rdi, regs->rsi, 0, 0, 0, 0, regs);
        break;
    case SYS_FORK: regs->rax = (uint64_t)process_fork(regs, false, 0); break;
    case SYS_VFORK: regs->rax = (uint64_t)process_fork(regs, true, 0); break;
    case SYS_FUTEX:
        regs->rax = (uint64_t)sys_futex((uint32_t *)regs->rdi, regs->rsi, regs->rdx,
                                        (const struct timespec *)regs->r10, (uint32_t *)regs->r8, regs->r9, regs);
        break;
    case SYS_EXECVE:
        regs->rax = (uint64_t)process_execve((char *)regs->rdi, (char **)regs->rsi, (char **)regs->rdx);
        break;
    case SYS_EXIT: sys_exit(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_EXIT_GROUP: sys_exit_group(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_TGKILL: regs->rax = (uint64_t)sys_tgkill(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_GETPID: regs->rax = get_current_task()->parent_group->pid; break;
    case SYS_GETTID: regs->rax = get_current_task()->tid; break;
    case SYS_TIME: regs->rax = (uint64_t)sys_time((int64_t *)regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_FLISTXATTR:
        regs->rax = (uint64_t)sys_flistxattr(regs->rdi, (char *)regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_GETTIMEOFDAY:
        regs->rax = (uint64_t)sys_gettimeofday((struct timeval *)regs->rdi, (void *)regs->rsi, 0, 0, 0, 0, regs);
        break;
    case SYS_GETDENTS64:
        regs->rax = (uint64_t)sys_getdents64(regs->rdi, (void *)regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_CLOCK_GETTIME:
        regs->rax = (uint64_t)sys_clock_gettime(regs->rdi, (struct timespec *)regs->rsi, 0, 0, 0, 0, regs);
        break;
    case SYS_WAIT4:
        regs->rax = (uint64_t)sys_wait4(regs->rdi, (int *)regs->rsi, regs->rdx, (void *)regs->r10, 0, 0, regs);
        break;
    case SYS_KILL: regs->rax = (uint64_t)sys_kill(regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_UNAME: regs->rax = (uint64_t)sys_uname((struct utsname *)regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_CHMOD: regs->rax = (uint64_t)sys_chmod((char *)regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_FCHMOD: regs->rax = (uint64_t)sys_fchmod(regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_CHOWN:
        regs->rax = (uint64_t)sys_chown((char *)regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_LCHOWN:
        regs->rax =
            (uint64_t)sys_fchownat(AT_FDCWD, (char *)regs->rdi, regs->rsi, regs->rdx, AT_SYMLINK_NOFOLLOW, 0, regs);
        break;
    case SYS_FCHOWN:
        regs->rax = (uint64_t)sys_fchown(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_BRK: regs->rax = (uint64_t)sys_brk(regs->rdi); break;
    case SYS_MMAP:
        regs->rax = (uint64_t)sys_mmap(regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8, regs->r9, regs);
        break;
    case SYS_MPROTECT: regs->rax = (uint64_t)sys_mprotect(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_MUNMAP: regs->rax = (uint64_t)sys_munmap(regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_MREMAP:
        regs->rax = (uint64_t)sys_mremap(regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8, 0, regs);
        break;
    case SYS_MADVISE: regs->rax = (uint64_t)sys_madvise(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_NANOSLEEP: regs->rax = (uint64_t)sys_nano_sleep((void *)regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_CLOCK_GETRES:
        regs->rax = (uint64_t)sys_clock_getres(regs->rdi, (uint64_t)regs->rsi, 0, 0, 0, 0, regs);
        break;
    case SYS_CLOCK_NANOSLEEP:
        regs->rax = (uint64_t)sys_clock_nanosleep(regs->rdi, regs->rsi, (const struct timespec *)regs->rdx,
                                                  (struct timespec *)regs->r10, 0, 0, regs);
        break;
    case SYS_GETPPID: regs->rax = (uint64_t)sys_getppid(0, 0, 0, 0, 0, 0, regs); break;
    case SYS_SYSINFO: regs->rax = (uint64_t)sys_sysinfo((struct sysinfo *)regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_GETUID: regs->rax = (uint64_t)sys_getuid(0, 0, 0, 0, 0, 0, regs); break;
    case SYS_GETGID: regs->rax = (uint64_t)sys_getgid(0, 0, 0, 0, 0, 0, regs); break;
    case SYS_SETUID: regs->rax = (uint64_t)sys_setuid(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_SETGID: regs->rax = (uint64_t)sys_setgid(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_GETEUID: regs->rax = (uint64_t)sys_geteuid(0, 0, 0, 0, 0, 0, regs); break;
    case SYS_GETEGID: regs->rax = (uint64_t)sys_getegid(0, 0, 0, 0, 0, 0, regs); break;
    case SYS_GETGROUPS: regs->rax = (uint64_t)sys_getgroups(regs->rdi, (int *)regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_SETGROUPS:
        regs->rax = (uint64_t)sys_setgroups(regs->rdi, (const int *)regs->rsi, 0, 0, 0, 0, regs);
        break;
    case SYS_SETRESUID:
        regs->rax = (uint64_t)sys_setresuid(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_GETRESUID:
        regs->rax = (uint64_t)sys_getresuid((uint32_t *)regs->rdi, (uint32_t *)regs->rsi,
                                            (uint32_t *)regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_SETRESGID:
        regs->rax = (uint64_t)sys_setresgid(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_GETRESGID:
        regs->rax = (uint64_t)sys_getresgid((uint32_t *)regs->rdi, (uint32_t *)regs->rsi,
                                            (uint32_t *)regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_SETFSUID:
        regs->rax = (uint64_t)sys_setfsuid(regs->rdi, 0, 0, 0, 0, 0, regs);
        break;
    case SYS_SETFSGID:
        regs->rax = (uint64_t)sys_setfsgid(regs->rdi, 0, 0, 0, 0, 0, regs);
        break;
    case SYS_CAPGET:
        regs->rax = (uint64_t)sys_capget((struct user_cap_header *)regs->rdi,
                                         (struct user_cap_data *)regs->rsi, 0, 0, 0, 0, regs);
        break;
    case SYS_CAPSET:
        regs->rax = (uint64_t)sys_capset((struct user_cap_header *)regs->rdi,
                                         (struct user_cap_data *)regs->rsi, 0, 0, 0, 0, regs);
        break;
    case SYS_GETPGRP: regs->rax = (uint64_t)sys_getpgid(0, 0, 0, 0, 0, 0, regs); break;
    case SYS_SETSID: regs->rax = (uint64_t)sys_setsid(0, 0, 0, 0, 0, 0, regs); break;
    case SYS_GETSID: regs->rax = (uint64_t)sys_getsid(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_GETPGID: regs->rax = (uint64_t)sys_getpgid(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_SETPGID: regs->rax = (uint64_t)sys_setpgid(regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_SENDFILE:
        regs->rax = (uint64_t)sys_sendfile(regs->rdi, regs->rsi, (uint64_t *)regs->rdx, regs->r10, 0, 0, regs);
        break;
    case SYS_STATFS:
        regs->rax = (uint64_t)sys_statfs((char *)regs->rdi, (struct statfs *)regs->rsi, 0, 0, 0, 0, regs);
        break;
    case SYS_FSTATFS:
        regs->rax = (uint64_t)sys_fstatfs(regs->rdi, (struct statfs *)regs->rsi, 0, 0, 0, 0, regs);
        break;
    case SYS_COPY_FILE_RANGE:
        regs->rax = (uint64_t)sys_copy_file_range(regs->rdi,
                                                  (uint64_t *)regs->rsi,
                                                  regs->rdx,
                                                  (uint64_t *)regs->r10,
                                                  regs->r8,
                                                  regs->r9,
                                                  regs);
        break;
    case SYS_MINCORE: regs->rax = (uint64_t)sys_mincore(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_OPENAT: regs->rax = (uint64_t)sys_openat(regs->rdi, regs->rsi, regs->rdx, regs->r10, 0, 0, regs); break;
    case SYS_MKDIRAT:
        regs->rax = (uint64_t)sys_mkdirat(regs->rdi, (char *)regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_NEWFSTATAT:
        regs->rax =
            (uint64_t)sys_newfstatat(regs->rdi, (char *)regs->rsi, (struct stat *)regs->rdx, regs->r10, 0, 0, regs);
        break;
    case SYS_FCHOWNAT:
        regs->rax = (uint64_t)sys_fchownat(regs->rdi, (char *)regs->rsi, regs->rdx, regs->r10, regs->r8, 0, regs);
        break;
    case SYS_FCHMODAT:
        regs->rax = (uint64_t)sys_fchmodat(regs->rdi, (char *)regs->rsi, regs->rdx, regs->r10, 0, 0, regs);
        break;
    case SYS_FCHMODAT2:
        regs->rax = (uint64_t)sys_fchmodat(regs->rdi, (char *)regs->rsi, regs->rdx, regs->r10, 0, 0, regs);
        break;
    case SYS_UTIMENSAT:
        regs->rax =
            (uint64_t)sys_utimensat(regs->rdi, (char *)regs->rsi, (const struct timespec *)regs->rdx, regs->r10, 0, 0, regs);
        break;
    case SYS_UTIMES:
        regs->rax = (uint64_t)sys_utimes((char *)regs->rdi, (const struct timeval *)regs->rsi, 0, 0, 0, 0, regs);
        break;
    case SYS_STATX:
        regs->rax =
            (uint64_t)sys_statx(regs->rdi, (char *)regs->rsi, regs->rdx, regs->r10, (struct statx *)regs->r8, 0, regs);
        break;
    case SYS_ACCESS: regs->rax = (uint64_t)sys_access((char *)regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_FACCESSAT: regs->rax = (uint64_t)sys_faccessat(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_FACCESSAT2:
        regs->rax = (uint64_t)sys_faccessat2(regs->rdi, regs->rsi, regs->rdx, regs->r10, 0, 0, regs);
        break;
    case SYS_UNLINKAT: regs->rax = (uint64_t)sys_unlinkat(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_RENAMEAT:
        regs->rax = (uint64_t)sys_renameat(regs->rdi, (char *)regs->rsi, regs->rdx, (char *)regs->r10, 0, 0, regs);
        break;
    case SYS_LINKAT:
        regs->rax = (uint64_t)sys_linkat(regs->rdi, (char *)regs->rsi, regs->rdx, (char *)regs->r10, regs->r8, 0, regs);
        break;
    case SYS_SYMLINKAT:
        regs->rax = (uint64_t)sys_symlinkat((char *)regs->rdi, regs->rsi, (char *)regs->rdx, 0, 0, 0, regs);
        break;
    case SYS_READLINK: regs->rax = (uint64_t)sys_readlink(regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs); break;
    case SYS_READLINKAT:
        regs->rax = (uint64_t)sys_readlinkat(regs->rdi, (char *)regs->rsi, (char *)regs->rdx, regs->r10, 0, 0, regs);
        break;
    case SYS_LINK: regs->rax = (uint64_t)sys_link((char *)regs->rdi, (char *)regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_SYMLINK: regs->rax = (uint64_t)sys_symlink((char *)regs->rdi, (char *)regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_RENAME: regs->rax = (uint64_t)sys_rename((char *)regs->rdi, (char *)regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_FTRUNCATE: regs->rax = (uint64_t)sys_ftruncate(regs->rdi, regs->rsi, 0, 0, 0, 0, regs); break;
    case SYS_PRLIMIT64:
        regs->rax =
            (uint64_t)sys_prlimit64(regs->rdi, regs->rsi, (struct rlimit *)regs->rdx, (struct rlimit *)regs->r10, 0, 0, regs);
        break;
    case SYS_GETRANDOM:
        regs->rax = (uint64_t)sys_getrandom((void *)regs->rdi, regs->rsi, regs->rdx, 0, 0, 0, regs);
        break;

    case SYS_PRCTL:
        regs->rax = (uint64_t)sys_prctl((int)regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8, 0, regs);
        break;
    case SYS_ARCH_PRCTL:
        regs->rax = (uint64_t)sys_arch_prctl(regs->rdi, regs->rsi, 0, 0, 0, 0, regs);
        break;
    case SYS_CHROOT:
        regs->rax = (uint64_t)sys_chroot((char *)regs->rdi, 0, 0, 0, 0, 0, regs);
        break;
        // case SYS_SET_TID_ADDRESS:
        //     // todo
        //     regs->rax = get_current_task()->parent_group->pid;
        //     break;
    case SYS_SET_TID_ADDRESS: regs->rax = (uint64_t)sys_set_tid_address((int *)regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case SYS_SET_ROBUST_LIST: regs->rax = (uint64_t)-ENOSYS; break;
    case SYS_RSEQ: regs->rax = (uint64_t)-ENOSYS; break;











    /* XJ380API XAPI Edition */
    case XAPI_OUTPUT: do_xapi_Output((char *)regs->rdi); break;
    case XAPI_INPUT: do_xapi_Input((char *)regs->rdi); break;
    case XAPI_GETCH: regs->rax = (uint64_t)do_xapi_Getch(); break;
    case XAPI_ENDLINE: do_xapi_Endline(); break;
    case XAPI_PRINTLINE: do_xapi_Printline((char *)regs->rdi); break;
    case XAPI_PRINTF: do_xapi_Printf((char *)regs->rdi); break;
    case XAPI_OUTPUT_SERIAL: do_xapi_OutputSerial((char *)regs->rdi); break;
    case XAPI_OPEN_FILE: regs->rax = do_xapi_OpenFile(regs->rdi); break;
    case XAPI_CLOSE_FILE: do_xapi_CloseFile(regs->rdi); break;
    case XAPI_FILE_DIALOG: regs->rax = (uint64_t)-ENOSYS; break;
    case XAPI_SEARCH_FILE: do_xapi_SearchFile(regs->rdi, regs->rsi, regs->rdx); break;
    case XAPI_MAKEDIR: regs->rax = (uint64_t)vfs_mkdir((char *)regs->rdi); break;
    case XAPI_CREATE_FILE: regs->rax = (uint64_t)sys_open((char *)regs->rdi, O_CREAT, 0644, 0, 0, 0, regs); break;
    case XAPI_DELETE_FILE: regs->rax = (uint64_t)sys_unlink(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case XAPI_RENAME_FILE:
        regs->rax = (uint64_t)sys_rename((char *)regs->rdi, (char *)regs->rsi, 0, 0, 0, 0, regs);
        break;
    case XAPI_READ_FILE: regs->rax = (uint64_t)do_xapi_ReadFile(regs); break;
    case XAPI_WRITE_FILE: regs->rax = (uint64_t)do_xapi_WriteFile(regs); break;
    case XAPI_FORK: regs->rax = (uint64_t)process_fork(regs, false, 0); break;
    case XAPI_EXECVE:
        regs->rax = (uint64_t)process_execve((char *)regs->rdi, (char **)regs->rsi, (char **)regs->rdx);
        break;
    case XAPI_CREATE_WINDOW: case XAPI_SET_WINDOW_TITLE: case XAPI_CLOSE_WINDOW: case XAPI_SET_ICON:
    case XAPI_GET_WIN_SZIE: case XAPI_DRAW_POINT: case XAPI_DRAW_LINE: case XAPI_DRAW_RECT:
    case XAPI_DRAW_RECT_FILL: case XAPI_DRAW_TEXT: case XAPI_DRAW_TEXT_L: case XAPI_DRAW_TEXT_SW:
    case XAPI_DRAW_BMP: case XAPI_DRAW_PNG: case XAPI_DRAW_PICTURE: case XAPI_LOAD_PICTURE:
    case XAPI_DRAW_SVG: case XAPI_DRAW_FA: case XAPI_SET_MSH_PROCOR: case XAPI_READBUFFER:
    case XAPI_WRITEBUFFER: case XAPI_READBUFFERA: case XAPI_WRITEBUFFERA: case XAPI_REFRESH_WINDOW:
    case XAPI_BUTTON: case XAPI_BUTTON_EMP: case XAPI_DELETE_BUTTON: case XAPI_PUT_SWITCH:
    case XAPI_SET_SWITCH: case XAPI_DEL_SWITCH: case XAPI_PUT_VERTICAL_SCROLL_BAR:
    case XAPI_PUT_HORIZONTAL_SCROLL_BAR: case XAPI_DELETE_SCROLL_BAR: case XAPI_SET_SCROLL_BAR_POSITION:
    case XAPI_PUT_TEXT_INBOX: case XAPI_GET_TEXT_INBOX: case XAPI_DEL_TEXT_INBOX:
    case XAPI_REG_RB_MENU: case XAPI_URG_RB_MENU: case XAPI_REFRESH_PART_WINDOW:
    case XAPI_GET_PIC_SIZE: case XAPI_CALC_TEXT_WIDTH:
        regs->rax = (uint64_t)-ENOSYS;
        break;
    case XAPI_GET_VERSION: do_xapi_GetSystemVersion(regs->rdi); break;
    case XAPI_GET_TIME: regs->rax = (uint64_t)do_xapi_GetTime(); break;
    case XAPI_GET_CURRENT_USER: do_xapi_GetCurrentUser(regs->rdi); break;
    case XAPI_SLEEP: do_xapi_Sleep(regs->rdi); break;
    case XAPI_BROKEN: do_xapi_Broken((char *)regs->rdi); break;
    case XAPI_GET_TIME_X: do_xapi_GetTimeX(regs->rdi); break;
    case XAPI_GET_CPU_MODEL: get_cpu_name((char *)regs->rdi); break;
    case XAPI_GET_MEMORY_SIZE: regs->rax = do_xapi_GetMemorySize(); break;
    case XAPI_SEND_APP_MSG: regs->rax = do_xapi_SendAppMessage((char *)regs->rdi, (char *)regs->rsi); break;
    case XAPI_RUN: do_xapi_Run((char *)regs->rdi); break;
    case XAPI_RUN_ARGS: regs->rax = do_xapi_RunArgs((char *)regs->rdi, (char **)regs->rsi); break;
    case XAPI_MAP_MEMORY: regs->rax = do_xapi_MapMemory(regs->rdi, regs->rsi, regs->rdx); break;
    case XAPI_REMOVEDIR: sys_rmdir(regs->rdi, 0, 0, 0, 0, 0, regs); break;
    case XAPI_FLUSH_TIME: regs->rax = (uint64_t)-ENOSYS; break;
    case XAPI_GET_TASK_LIST: regs->rax = (uint64_t)-ENOSYS; break;
    case XAPI_KILL_PROCESS: regs->rax = do_xapi_KillProcess(regs->rdi); break;
    case XAPI_USER_OOBE_REQUIRED: regs->rax = do_xapi_UserOobeRequired(); break;
    case XAPI_USER_LIST: regs->rax = do_xapi_UserList(regs->rdi, regs->rsi); break;
    case XAPI_USER_LOGIN: regs->rax = do_xapi_UserLogin(regs->rdi, regs->rsi); break;
    case XAPI_USER_CREATE_FIRST: regs->rax = do_xapi_UserCreateFirst(regs->rdi, regs->rsi); break;
    case XAPI_GET_TIME_NANO: regs->rax = nanoTime(); break;
    case XAPI_POWER_ACTION: regs->rax = (uint64_t)-ENOSYS; break;
    case XAPI_NOTIFY_SEND: case XAPI_NOTIFY_SET_PROCOR: regs->rax = (uint64_t)-ENOSYS; break;

    /* XJ380API 隐藏版（严禁泄露） */
    case SXAH_CHECK_TERMINAL_INIT_STATUS: regs->rax = (uint64_t)check_terminal_init_status(); break;
    case SXAH_SYSCALL_RETURN:
        // get_current_task()->parent_group->msgprci = false;
        // message_end();
        break;
    case SXAH_MARK_IS_TERMINAL: mark_process_is_terminal(); break;
    case SXAH_READ_OUTPUT_BUFFER: read_terminal_app_output_buffer((char *)regs->rdi); break;
    case SXAH_WRITE_INPUT_BUFFER: write_terminal_app_output_buffer((char *)regs->rdi); break;
    case SXAH_CHECK_INPUT_BUFFER: regs->rax = check_input_waiting_status(); break;
    case SXAH_UNLOCK_OUTPUT_LOCK: terminal_finish_app_output(); break;
    case SXAH_MESSAGE_ASK: regs->rax = message_ask(regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8); break;
    case SXAH_INSTALLER_ENUM_DISKS: regs->rax = do_xapi_InstallerEnumDisks(regs->rdi); break;
    case SXAH_INSTALLER_START: regs->rax = do_xapi_InstallerStart(regs->rdi); break;
    case SXAH_INSTALLER_START_EX: regs->rax = do_xapi_InstallerStartEx(regs->rdi, regs->rsi); break;
    case SXAH_INSTALLER_START_OPTIONS: regs->rax = do_xapi_InstallerStartOptions(regs->rdi); break;
    case SXAH_INSTALLER_PRECHECK: regs->rax = do_xapi_InstallerPrecheck(regs->rdi, regs->rsi, regs->rdx); break;
    case SXAH_INSTALLER_PRECHECK_OPTIONS: regs->rax = do_xapi_InstallerPrecheckOptions(regs->rdi, regs->rsi); break;
    case SXAH_INSTALLER_PROGRESS: regs->rax = do_xapi_InstallerProgress(regs->rdi); break;
    case SXAH_INSTALLER_RESCUE: regs->rax = do_xapi_InstallerRescue(regs->rdi, regs->rsi, regs->rdx); break;
    case SXAH_INSTALLER_LOG: regs->rax = do_xapi_InstallerLog(regs->rdi); break;

    default:
        regs->rax = (uint64_t)-ENOSYS;
        write_serial_string("Unknown syscall index: ");
        write_serial_dec(syscall_number);
        write_serial_string("\n");
        break;
    }
    if (debug_trace_xbps_task() && (int64_t)regs->rax < 0)
    {
        tcb_t task = get_current_task();
        write_serial_fmt("[DEBUG-xbps-syscall] task=%s nr=%llu(%s) args=%llx,%llx,%llx,%llx,%llx ret=%lld\n",
                         task != NULL ? task->name : "?",
                         syscall_number,
                         debug_syscall_name(syscall_number),
                         regs->rdi,
                         regs->rsi,
                         regs->rdx,
                         regs->r10,
                         regs->r8,
                         (long long)regs->rax);
    }
    if (trace_grep)
    {
        tcb_t task = get_current_task();
        write_serial_fmt("[DEBUG-grep-syscall] ret task=%s nr=%llu(%s) ret=%lld\n",
                         task != NULL ? task->name : "?",
                         syscall_number,
                         debug_syscall_name(syscall_number),
                         (long long)regs->rax);
    }
    signal_deliver_pending(regs);
    tcb_t return_task = get_current_task();
    if (return_task != NULL && return_task->task_level == TASK_APPLICATION_LEVEL)
    {
        uint64_t fs_selector = task_user_fs_selector(return_task);
        __asm__ __volatile__("movq %0, %%fs\n\t" ::"r"(fs_selector));
        write_fsbase(return_task->fs_base);
    }
    return (uint64_t)regs;
}
