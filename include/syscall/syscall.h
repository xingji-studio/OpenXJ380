#pragma once
#include <proto.hpp>
#include <net/socket.h>
#include "pxapi.h"
#include <syscall/signal.h>
#include <cpu/longm.h>

#define XJ380_PRIVATE_MESSAGE_REVERT_ADDRESS 0x000000011717450000
#define XPSR_OFFEST                         0x927

#define SYSCALL_SUCCESS      EOK
#define SYSCALL_FAULT        ((uint64_t)-(ENOSYS))
#define SYSCALL_FAULT_(name) ((uint64_t)-(name))
#define FD_SETSIZE           1024
#define SENDFILE_BUF_SIZE    4096

// 一个非常取巧的宏魔法, 可以简化 syscall 函数的定义
#define __EXPAND_PARAMS(...) __VA_ARGS__
#define __CONCAT_IMPL(a, b)  a##b
#define __CONCAT(a, b)       __CONCAT_IMPL(a, b)

#define __ARGS_COUNT_IMPL(_0, _1, _2, _3, _4, _5, _6, N, ...) N

#define __ARGS_COUNT(...) __EXPAND_PARAMS(__ARGS_COUNT_IMPL(__VA_ARGS__, 6, 5, 4, 3, 2, 1, 0))

#define __SYSCALL_IMPL_0(NAME)                                                                     \
    uint64_t sys_##NAME(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3,            \
                            uint64_t arg4, uint64_t arg5, struct X64_REGS *regs)

#define __SYSCALL_IMPL_1(NAME, P1)                                                                 \
    uint64_t sys_##NAME(P1, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4,        \
                            uint64_t arg5, struct X64_REGS *regs)

#define __SYSCALL_IMPL_2(NAME, P1, P2)                                                             \
    uint64_t sys_##NAME(P1, P2, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,    \
                            struct X64_REGS *regs)

#define __SYSCALL_IMPL_3(NAME, P1, P2, P3)                                                         \
    uint64_t sys_##NAME(P1, P2, P3, uint64_t arg3, uint64_t arg4, uint64_t arg5,               \
                            struct X64_REGS *regs)

#define __SYSCALL_IMPL_4(NAME, P1, P2, P3, P4)                                                     \
    uint64_t sys_##NAME(P1, P2, P3, P4, uint64_t arg4, uint64_t arg5, struct X64_REGS *regs)

#define __SYSCALL_IMPL_5(NAME, P1, P2, P3, P4, P5)                                                 \
    uint64_t sys_##NAME(P1, P2, P3, P4, P5, uint64_t arg5, struct X64_REGS *regs)

#define __SYSCALL_IMPL_6(NAME, P1, P2, P3, P4, P5, P6)                                             \
    uint64_t sys_##NAME(P1, P2, P3, P4, P5, P6, struct X64_REGS *regs)

#define __SYSCALL_DISPATCH(N, NAME, ...) __CONCAT(__SYSCALL_IMPL_, N)(NAME, ##__VA_ARGS__)

#define sys_(NAME, ...) __SYSCALL_DISPATCH(__ARGS_COUNT(0, ##__VA_ARGS__), NAME, ##__VA_ARGS__)

#define sys_def_(name)                                                                         \
    uint64_t sys_##name(                                                                       \
        uint64_t arg0 __attribute__((unused)), uint64_t arg1 __attribute__((unused)),              \
        uint64_t arg2 __attribute__((unused)), uint64_t arg3 __attribute__((unused)),              \
        uint64_t arg4 __attribute__((unused)), uint64_t arg5 __attribute__((unused)),              \
        struct X64_REGS *regs __attribute__((unused)))

// arch_prctl 系统调用 code
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_SET_GS 0x1004
#define ARCH_GET_GS 0x1005

// futex 系统调用操作码
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_FD          2
#define FUTEX_REQUEUE     3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP     5
#define FUTEX_LOCK_PI     6
#define FUTEX_UNLOCK_PI   7
#define FUTEX_TRYLOCK_PI  8
#define FUTEX_WAIT_BITSET 9

// stat 文件类型标志
#define S_IFMT   00170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

// reboot 校验码
#define REBOOT_MAGIC1  0xfee1dead
#define REBOOT_MAGIC2  672274793
#define REBOOT_MAGIC2A 85072278
#define REBOOT_MAGIC2B 369367448
#define REBOOT_MAGIC2C 537993216

#define REBOOT_CMD_RESTART    0x01234567
#define REBOOT_CMD_HALT       0xCDEF0123
#define REBOOT_CMD_CAD_ON     0x89ABCDEF
#define REBOOT_CMD_CAD_OFF    0x00000000
#define REBOOT_CMD_POWER_OFF  0x4321FEDC
#define REBOOT_CMD_RESTART2   0xA1B2C3D4
#define REBOOT_CMD_SW_SUSPEND 0xD000FCE2
#define REBOOT_CMD_KEXEC      0x45584543

#define MS_RDONLY      1  /* 只读挂载 */
#define MS_NOSUID      2  /* 忽略 SUID/SGID */
#define MS_NODEV       4  /* 禁止访问设备文件 */
#define MS_NOEXEC      8  /* 禁止执行 */
#define MS_SYNCHRONOUS 16 /* 同步写入 */
#define MS_REMOUNT     32 /* 重新挂载已挂载点 */
#define MS_MANDLOCK    64
#define MS_DIRSYNC     128
#define MS_NOATIME     1024
#define MS_NODIRATIME  2048
#define MS_BIND        4096  /* 绑定挂载 */
#define MS_MOVE        8192  /* 挂载点移动 */
#define MS_REC         16384 /* 递归 */
#define MS_PRIVATE     (1 << 18)
#define MS_SHARED      (1 << 20)
#define MS_SLAVE       (1 << 19)
#define MS_UNBINDABLE  (1 << 17)
//用不太着
// // Linux 兼容层系统调用编号定义
// #define SYSCALL_READ        0
// #define SYSCALL_WRITE       1
// #define SYSCALL_OPEN        2
// #define SYSCALL_CLOSE       3
// #define SYSCALL_STAT        4
// #define SYSCALL_FSTAT       5
// #define SYSCALL_LSTAT       6
// #define SYSCALL_POLL        7
// #define SYSCALL_LSEEK       8
// #define SYSCALL_MMAP        9
// #define SYSCALL_MPROTECT    10
// #define SYSCALL_MUNMAP      11
// /* #define SYSCALL_BRK  12  brk 系统调用不实现*/
// #define SYSCALL_SIGACTION   13
// #define SYSCALL_RT_SIGMASK  14
// #define SYSCALL_SIGRET      15
// #define SYSCALL_IOCTL       16
// #define SYSCALL_PREAD       17
// #define SYSCALL_PWRITE      18
// #define SYSCALL_READV       19
// #define SYSCALL_WRITEV      20
// #define SYSCALL_ACCESS      21
// #define SYSCALL_PIPE        22
// #define SYSCALL_SELECT      23
// #define SYSCALL_YIELD       24
// #define SYSCALL_MREMAP      25
// #define SYSCALL_MINCORE     27
// #define SYSCALL_DUP         32
// #define SYSCALL_DUP2        33
// #define SYSCALL_NANO_SLEEP  35
// #define SYSCALL_GETPID      39
// #define SYSCALL_SENDFILE    40
// #define SYSCALL_SOCKET      41
// #define SYSCALL_CONNECT     42
// #define SYSCALL_ACCEPT      43
// #define SYSCALL_BIND        49
// #define SYSCALL_LISTEN      50
// #define SYSCALL_CLONE       56
// #define SYSCALL_FORK        57
// #define SYSCALL_VFORK       58
// #define SYSCALL_EXECVE      59
// #define SYSCALL_EXIT        60
// #define SYSCALL_WAITPID     61
// #define SYSCALL_UNAME       63
// #define SYSCALL_FCNTL       72
// #define SYSCALL_FTRUNCATE   77
// #define SYSCALL_GETCWD      79
// #define SYSCALL_CHDIR       80
// #define SYSCALL_RENAME      82
// #define SYSCALL_MKDIR       83
// #define SYSCALL_RMDIR       84
// #define SYSCALL_LINK        86
// #define SYSCALL_UNLINK      87
// #define SYSCALL_SYMLINK     88
// #define SYSCALL_READLINK    89
// #define SYSCALL_SYSINFO     99
// #define SYSCALL_GETUID      102
// #define SYSCALL_GETGID      104
// #define SYSCALL_SETUID      105
// #define SYSCALL_SETGID      106
// #define SYSCALL_GETEUID     107
// #define SYSCALL_GETEGID     108
// #define SYSCALL_SETPGID     109
// #define SYSCALL_GETPPID     110
// #define SYSCALL_GETGROUPS   115
// #define SYScall_GETPGID     121
// #define SYSCALL_SIGSUSPEND  130
// #define SYSCALL_SIGALTSTACK 131
// #define SYSCALL_STATFS      137
// #define SYSCALL_PRCTL       157
// #define SYSCALL_ARCH_PRCTL  158
// #define SYSCALL_PIVOT_ROOT  155
// #define SYSCALL_G_AFFINITY  160
// #define SYSCALL_CHROOT      161
// #define SYSCALL_MOUNT       165
// #define SYSCALL_UMOUNT2     166
// #define SYSCALL_REBOOT      169
// #define SYSCALL_GET_TID     186
// #define SYSCALL_FUTEX       202
// #define SYSCALL_GETDENTS64  217
// #define SYSCALL_SETID_ADDR  218
// #define SYSCALL_EXIT_GROUP  231
// #define SYSCALL_C_SETTIME   227
// #define SYSCALL_C_GETTIME   228
// #define SYSCALL_C_GETRES    229
// #define SYSCALL_C_NANOSLEEP 230
// #define SYSCALL_OPENAT      257
// #define SYSCALL_NEWFSTATAT  262
// #define SYSCALL_UNLINKAT    263
// #define SYSCALL_FACCESSAT   269
// #define SYSCALL_PSELECT6    270
// #define SYSCALL_PIPE2       293
// #define SYSCALL_CP_F_RANGE  326
// #define SYSCALL_STATX       332
// #define SYSCALL_FSOPEN      430
// #define SYSCALL_FACCESSAT2  439

// XJ380API POSIX Edition
#define SYS_READ     0
#define SYS_WRITE    1
#define SYS_OPEN     2
#define SYS_CLOSE    3
#define SYS_STAT     4
#define SYS_FSTAT    5
#define SYS_LSTAT    6
#define SYS_POLL     7
#define SYS_LSEEK    8
#define SYS_MMAP     9
#define SYS_MPROTECT 10
#define SYS_MUNMAP   11
#define SYS_BRK      12

#define SYS_RT_SIGACTION  13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN  15

#define SYS_IOCTL 16
#define SYS_PREAD64 17
#define SYS_PWRITE64 18

#define SYS_READV  19
#define SYS_WRITEV 20
#define SYS_ACCESS 21

#define SYS_PIPE 22
#define SYS_SELECT 23
#define SYS_SCHED_YIELD 24

#define SYS_MREMAP 25
#define SYS_MSYNC  26
#define SYS_MINCORE 27
#define SYS_MADVISE 28

#define SYS_DUP       32
#define SYS_DUP2      33
#define SYS_DUP3      292
#define SYS_PAUSE     34
#define SYS_NANOSLEEP 35

#define SYS_ALARM 37

#define SYS_GETPID 39
#define SYS_SENDFILE 40

#define SYS_SOCKET 41
#define SYS_CONNECT 42
#define SYS_ACCEPT 43
#define SYS_SENDTO 44
#define SYS_RECVFROM 45
#define SYS_SENDMSG 46
#define SYS_RECVMSG 47
#define SYS_SOCKETPAIR 53

#define SYS_SHUTDOWN 48

#define SYS_BIND 49
#define SYS_LISTEN 50
#define SYS_GETSOCKNAME 51
#define SYS_GETPEERNAME 52
#define SYS_SETSOCKOPT 54
#define SYS_GETSOCKOPT 55

#define SYS_CLONE  56
#define SYS_FORK   57
#define SYS_VFORK  58
#define SYS_EXECVE 59
#define SYS_EXIT   60
#define SYS_WAIT4  61
#define SYS_KILL   62
#define SYS_UNAME  63
#define SYS_FCNTL  72
#define SYS_FLOCK  73
#define SYS_FTRUNCATE 77

#define SYS_GETDENTS 78
#define SYS_GETCWD   79
#define SYS_CHDIR    80
#define SYS_FCHDIR   81
#define SYS_RENAME   82

#define SYS_MKDIR    83
#define SYS_RMDIR    84
#define SYS_CREAT    85
#define SYS_LINK     86
#define SYS_UNLINK   87
#define SYS_SYMLINK  88
#define SYS_READLINK 89
#define SYS_CHMOD    90
#define SYS_FCHMOD   91
#define SYS_CHOWN    92
#define SYS_FCHOWN   93
#define SYS_LCHOWN   94
#define SYS_UMASK    95
#define SYS_GETTIMEOFDAY 96

#define SYS_SYSINFO 99
#define SYS_GETUID 102
#define SYS_GETGID 104
#define SYS_SETUID 105
#define SYS_SETGID 106
#define SYS_GETEUID 107
#define SYS_GETEGID 108
#define SYS_SETPGID 109
#define SYS_GETPPID 110
#define SYS_GETPGRP 111
#define SYS_SETSID 112
#define SYS_GETGROUPS 115
#define SYS_SETGROUPS 116
#define SYS_SETRESUID 117
#define SYS_GETRESUID 118
#define SYS_SETRESGID 119
#define SYS_GETRESGID 120
#define SYS_GETPGID 121
#define SYS_SETFSUID 122
#define SYS_SETFSGID 123
#define SYS_GETSID 124
#define SYS_CAPGET 125
#define SYS_CAPSET 126
#define SYS_SIGALTSTACK 131
#define SYS_MKNOD 133
#define SYS_STATFS 137
#define SYS_PIVOT_ROOT 155

#define SYS_PRCTL 157
#define SYS_ARCH_PRCTL 158
#define SYS_CHROOT 161
#define SYS_SYNC 162
#define SYS_MOUNT 165
#define SYS_UMOUNT2 166

#define SYS_GETTID 186
#define SYS_FLISTXATTR 196
#define SYS_TIME 201
#define SYS_FUTEX 202
#define SYS_SCHED_GETAFFINITY 204
#define SYS_EPOLL_CREATE 213
#define SYS_GETDENTS64 217

#define SYS_SET_TID_ADDRESS 218
#define SYS_FADVISE64 221

#define SYS_CLOCK_GETTIME 228
#define SYS_CLOCK_GETRES 229
#define SYS_CLOCK_NANOSLEEP 230

#define SYS_EXIT_GROUP 231
#define SYS_EPOLL_WAIT 232
#define SYS_EPOLL_CTL 233
#define SYS_TGKILL 234
#define SYS_UTIMES 235

#define SYS_OPENAT 257
#define SYS_MKDIRAT 258
#define SYS_FCHOWNAT 260
#define SYS_NEWFSTATAT 262
#define SYS_UNLINKAT 263
#define SYS_RENAMEAT 264
#define SYS_LINKAT 265
#define SYS_SYMLINKAT 266
#define SYS_READLINKAT 267
#define SYS_FCHMODAT 268
#define SYS_FACCESSAT 269
#define SYS_PSELECT6 270
#define SYS_SET_ROBUST_LIST 273
#define SYS_SYNC_FILE_RANGE 277
#define SYS_UTIMENSAT 280
#define SYS_EPOLL_PWAIT 281
#define SYS_EPOLL_CREATE1 291
#define SYS_PIPE2 293
#define SYS_PRLIMIT64 302
#define SYS_SENDMMSG 307
#define SYS_GETRANDOM 318
#define SYS_COPY_FILE_RANGE 326
#define SYS_STATX 332
#define SYS_FSTATFS 138
#define SYS_FSYNC 74
#define SYS_FDATASYNC 75

#define SYS_RSEQ 334
#define SYS_FSOPEN 430
#define SYS_CLONE3 435
#define SYS_CLOSE_RANGE 436
#define SYS_FACCESSAT2 439
#define SYS_FCHMODAT2 452

#define CLOSE_RANGE_UNSHARE  (1U << 1)
#define CLOSE_RANGE_CLOEXEC  (1U << 2)

#define CLONE_ARGS_SIZE_VER0 80
#define CLONE_ARGS_SIZE_VER1 88

struct clone_args {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
};

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

struct msghdr {
    void         *msg_name;
    socklen_t    msg_namelen;
    struct iovec *msg_iov;
    size_t       msg_iovlen;
    void         *msg_control;
    size_t       msg_controllen;
    int          msg_flags;
};

struct mmsghdr {
    struct msghdr msg_hdr;
    uint32_t      msg_len;
};

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct stat {
    long              st_dev;
    unsigned long     st_ino;
    unsigned long     st_nlink;
    int               st_mode;
    int               st_uid;
    int               st_gid;
    long              st_rdev;
    long long         st_size;
    long              st_blksize;
    unsigned long int st_blocks;
    struct timespec   st_atim;
    struct timespec   st_mtim;
    struct timespec   st_ctim;
    char              _pad[24];
};

struct user_cap_header {
    uint32_t version;
    int      pid;
};

struct user_cap_data {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

typedef struct {
    vfs_node_t node;
    size_t     offset;
    size_t     fd;
    uint64_t   flags;
} fd_file_handle;

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / 8 / sizeof(long)];
} fd_set;

typedef struct {
    sigset_t *ss;
    size_t    ss_len;
} WeirdPselect6;

struct timeval {
    long tv_sec;
    long tv_usec;
};
struct rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

struct dirent {
    long           d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
};

struct statx_timestamp {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    int32_t  __reserved;
};

struct statx {
    /* 0x00 */
    uint32_t stx_mask;       /* What results were written [uncond] */
    uint32_t stx_blksize;    /* Preferred general I/O size [uncond] */
    uint64_t stx_attributes; /* Flags conveying information about the file [uncond] */
    /* 0x10 */
    uint32_t stx_nlink; /* Number of hard links */
    uint32_t stx_uid;   /* User ID of owner */
    uint32_t stx_gid;   /* Group ID of owner */
    uint16_t stx_mode;  /* File mode */
    uint16_t __spare0[1];
    /* 0x20 */
    uint64_t stx_ino;             /* Inode number */
    uint64_t stx_size;            /* File size */
    uint64_t stx_blocks;          /* Number of 512-byte blocks allocated */
    uint64_t stx_attributes_mask; /* Mask to show what's supported in stx_attributes */
    /* 0x40 */
    struct statx_timestamp stx_atime; /* Last access time */
    struct statx_timestamp stx_btime; /* File creation time */
    struct statx_timestamp stx_ctime; /* Last attribute change time */
    struct statx_timestamp stx_mtime; /* Last data modification time */
    /* 0x80 */
    uint32_t               stx_rdev_major; /* Device ID of special file [if bdev/cdev] */
    uint32_t               stx_rdev_minor;
    uint32_t               stx_dev_major; /* ID of device containing file [uncond] */
    uint32_t               stx_dev_minor;
    /* 0x90 */
    uint64_t               stx_mnt_id;
    uint32_t               stx_dio_mem_align;    /* Memory buffer alignment for direct I/O */
    uint32_t               stx_dio_offset_align; /* File offset alignment for direct I/O */
    /* 0xa0 */
    uint64_t               __spare3[12]; /* Spare space for future expansion */
                                         /* 0x100 */
};

struct sysinfo {
    int64_t  uptime;    /* Seconds since boot */
    uint64_t loads[3];  /* 1, 5, and 15 minute load averages */
    uint64_t totalram;  /* Total usable main memory size */
    uint64_t freeram;   /* Available memory size */
    uint64_t sharedram; /* Amount of shared memory */
    uint64_t bufferram; /* Memory used by buffers */
    uint64_t totalswap; /* Total swap space size */
    uint64_t freeswap;  /* swap space still available */
    uint16_t procs;     /* Number of current processes */
    uint16_t pad;       /* Explicit padding for m68k */
    uint64_t totalhigh; /* Total high memory size */
    uint64_t freehigh;  /* Available high memory size */
    uint32_t mem_unit;  /* Memory unit size in bytes */
    char     _f[20 - 2 * sizeof(uint64_t) - sizeof(uint32_t)]; /* Padding: libc5 uses this.. */
};

typedef struct {
    int val[2];
} __kernel_fsid_t;

struct statfs {
    uint64_t        f_type;
    uint64_t        f_bsize;
    uint64_t        f_blocks;
    uint64_t        f_bfree;
    uint64_t        f_bavail;
    uint64_t        f_files;
    uint64_t        f_ffree;
    __kernel_fsid_t f_fsid;
    uint64_t        f_namelen;
    uint64_t        f_frsize;
    uint64_t        f_flags;
    uint64_t        f_spare[4];
};
fd_file_handle *fd_dup(fd_file_handle *src);
/*
// Proto (POSIX Edition)
int sys_read(uint32_t fd, uint8_t *buf, size_t count);
int sys_write(uint32_t fd, uint8_t *buf, size_t count);
int sys_readv(uint32_t fd, struct iovec *iovec, size_t count);
int sys_writev(uint32_t fd, struct iovec *iovec, size_t count);
int sys_open(const char *pathname, int flags, int mode);
int sys_close(uint32_t fd);
int sys_fstat(uint32_t fd, struct stat *stat);
int sys_execve(struct X64_REGS *regs);
int sys_exit(int status);
int sys_fork(struct X64_REGS *regs);
int sys_vfork(struct X64_REGS *regs);
uint64_t sys_wait4(unsigned long pid, long *status, int options, void *rusage);
uint64_t sys_brk(uint64_t addr);
*/

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED  0x1
#define MAP_PRIVATE 0x2
#define MAP_FIXED   0x10



// uint64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset);

// uint64_t sys_arch_prctl(uint64_t cmd, uint64_t arg);

sys_(exit, int exit_code);
sys_(open, char *path0, uint64_t flags, uint64_t mode);
sys_(close, int fd);
sys_(write, int fd, uint8_t *buffer, size_t size);
sys_(read, int fd, uint8_t *buffer, size_t size);
sys_(sigret);
sys_(rt_sigaction, int sig, sigaction_t *action, sigaction_t *oldaction, size_t sigsetsize);
sys_(getpid);
sys_(socket, int domain, int type, int protocol);
sys_(connect, int fd, const struct sockaddr *addr, socklen_t addrlen);
sys_(accept, int fd, struct sockaddr *addr, socklen_t *addrlen);
sys_(sendto, int fd, const void *buffer, size_t size, uint64_t flags, const struct sockaddr *addr, socklen_t addrlen);
sys_(recvfrom, int fd, void *buffer, size_t size, uint64_t flags, struct sockaddr *addr, socklen_t *addrlen);
sys_(sendmsg, int fd, const struct msghdr *msg, uint64_t flags);
sys_(sendmmsg, int fd, struct mmsghdr *mmsg, uint32_t vlen, uint64_t flags);
sys_(recvmsg, int fd, struct msghdr *msg, uint64_t flags);
sys_(socketpair, int domain, int type, int protocol, int *sv);
sys_(setsockopt, int fd, int level, int optname, const void *optval, socklen_t optlen);
sys_(getsockopt, int fd, int level, int optname, void *optval, socklen_t *optlen);
sys_(getsockname, int fd, struct sockaddr *addr, socklen_t *addrlen);
sys_(getpeername, int fd, struct sockaddr *addr, socklen_t *addrlen);
sys_(stat, char *fn, struct stat *buf);
sys_(arch_prctl, uint64_t code, uint64_t addr);
sys_(prctl, int option, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
sys_(yield);
sys_(uname, struct utsname *utsname);
sys_(nano_sleep, void *time_handle);
sys_(clock_nanosleep, int clockid, int flags, const struct timespec *request, struct timespec *remain);
sys_(ioctl, int fd, int options, void *arg2);
sys_(writev, int fd, struct iovec *iov, int iovcnt);
sys_(readv, int fd, struct iovec *iov, int iovcnt0);
uint64_t sys_brk(uint64_t brk);//SB
sys_(mmap, uint64_t addr, size_t length, uint64_t prot, uint64_t flags, int fd,
         uint64_t offset);//sb
sys_(mprotect, uint64_t addr, size_t length, uint64_t prot);
sys_(munmap, uint64_t addr, size_t size);

sys_(mremap, uint64_t old_addr, uint64_t old_size, uint64_t new_size, uint64_t flags,
         uint64_t new_addr);//g

sys_(getcwd, char *buffer, size_t length);//nm

sys_(chdir, char *s);
sys_(fchdir, int fd);

sys_(wait4, unsigned long pid, int *status, int options, void *rusage);
sys_(exit_group, int exit_code);
sys_(poll, struct pollfd *fds_user, size_t nfds, size_t timeout);
sys_(epoll_create, int size);
sys_(epoll_create1, int flags);
sys_(epoll_ctl, int epfd, int op, int fd, struct epoll_event *event);
sys_(epoll_wait, int epfd, struct epoll_event *events, int maxevents, int timeout);
sys_(epoll_pwait, int epfd, struct epoll_event *events, int maxevents, int timeout, sigset_t *sigmask, size_t sigsetsize);
sys_(sched_getaffinity, int pid, size_t cpusetsize, void *mask);
sys_(rt_sigprocmask, int how, sigset_t *set, sigset_t *oldset);
sys_(kill, int pid, int sig);
sys_(tgkill, int tgid, int tid, int sig);
sys_(dup2, int fd, int newfd);
sys_(dup3, int fd, int newfd, int flags);
sys_(dup, int fd);
sys_(fcntl, int fd, int cmd, uint64_t arg);
sys_(fork);
sys_(clone, uint64_t flags, uint64_t stack, int *parent_tid, int *child_tid, uint64_t tls);
sys_(clone3, struct clone_args *cl_args, size_t size);
sys_(get_tid);
sys_(set_tid_address, int *tidptr);
sys_(futex, uint32_t *uaddr, int op, uint32_t val, const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3);
sys_(vfork);
sys_(execve, char *path, char **argv, char **envp);
sys_(fstat, int fd, struct stat *buf);
sys_(time, int64_t *timer);
sys_(gettimeofday, struct timeval *tv, void *tz);
sys_(clock_gettime, uint64_t arg0, struct timespec *ts);
sys_(sendfile, int out_fd, int in_fd, uint64_t *offset_ptr, size_t count);
sys_(statfs, char *path, struct statfs *buf);
sys_(fstatfs, int fd, struct statfs *buf);
sys_(pivot_root, char *new_root, char *put_old);
sys_(mincore, uint64_t addr, uint64_t size, uint64_t vec);
sys_(madvise, uint64_t addr, size_t length, int advice);
sys_(getppid);
sys_(setsid);
sys_(getsid, int pid);
sys_(fsopen, char *fsname, uint32_t flags);
sys_(getuid);
sys_(getgid);
sys_(setuid, uint32_t uid);
sys_(setgid, uint32_t gid);
sys_(setresuid, uint32_t ruid, uint32_t euid, uint32_t suid);
sys_(getresuid, uint32_t *ruid, uint32_t *euid, uint32_t *suid);
sys_(setresgid, uint32_t rgid, uint32_t egid, uint32_t sgid);
sys_(getresgid, uint32_t *rgid, uint32_t *egid, uint32_t *sgid);
sys_(setfsuid, uint32_t uid);
sys_(setfsgid, uint32_t gid);
sys_(geteuid);
sys_(getegid);
sys_(capget, struct user_cap_header *header, struct user_cap_data *data);
sys_(capset, struct user_cap_header *header, struct user_cap_data *data);
sys_(sysinfo, struct sysinfo *info);
sys_(readlink);
sys_(readlinkat, int dirfd, char *path, char *buf, size_t size);
sys_(mkdirat, int dirfd, char *pathname, uint64_t mode);
sys_(renameat, int olddirfd, char *oldpath, int newdirfd, char *newpath);
sys_(linkat, int olddirfd, char *oldpath, int newdirfd, char *newpath, int flags);
sys_(symlinkat, char *target, int newdirfd, char *linkpath);
sys_(ftruncate, int fd, uint64_t length);
sys_(fsync, int fd);
sys_(fdatasync, int fd);
sys_(fadvise64, int fd, uint64_t offset, uint64_t len, int advice);
sys_(sync_file_range, int fd, int64_t offset, int64_t nbytes, uint64_t flags);
sys_(sync);
sys_(prlimit64, int pid, uint64_t resource, struct rlimit *new_limit, struct rlimit *old_limit);
sys_(getrandom, void *buf, size_t buflen, uint64_t flags);
sys_(umount2);
sys_(link, char *name, char *nw);
sys_(symlink, char *name, char *nw);
sys_(rename, char *oldpath, char *newpath);
sys_(getgroups, int count, int *gid_list);
sys_(setgroups, int count, const int *gid_list);
sys_(getpgid);
sys_(setpgid, int pid, int pgid);
sys_(flock, int fd, int operation);
sys_(chroot, char *path);
sys_(mount, char *dev_name, char *dir_name, char *type, uint64_t flags, void *data);
sys_(pwrite, int fd, uint8_t *buffer, size_t size, int64_t offset);
sys_(pread, int fd, uint8_t *buffer, size_t size, uint64_t offset);
sys_(copy_file_range, int fd_in, uint64_t *off_in, int fd_out, uint64_t *off_out, size_t len, uint64_t flags);
sys_(flistxattr, int fd, char *list, size_t size);
sys_(openat);
sys_(faccessat2);
sys_(faccessat);
sys_(access, char *filename);
sys_(unlinkat);
sys_(rmdir);
sys_(unlink);
sys_(pipe, int *pipefd);
sys_(pipe2, int *pipefd, uint64_t flags);
sys_(statx, int dirfd, char *pathname, uint64_t flags, uint64_t mask, struct statx *buff);
sys_(newfstatat, int dirfd, char *pathname, struct stat *buf, uint64_t flags);
sys_(fchownat, int dirfd, char *pathname, uint32_t owner, uint32_t group, int flags);
sys_(fchmodat, int dirfd, char *pathname, uint64_t mode, int flags);
sys_(utimensat, int dirfd, char *pathname, const struct timespec *times, int flags);
sys_(utimes, char *filename, const struct timeval *times);
sys_(getdents, int fd, struct dirent *dents, size_t size);
sys_(getdents64, int fd, void *dents, size_t size);
sys_(pselect6, uint64_t nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timespec *timeout,
     WeirdPselect6 *weirdPselect6);
sys_(select, int nfds, uint8_t *read, uint8_t *write, uint8_t *except, struct timeval *timeout);
sys_(lseek, int fd, int64_t offset, size_t whence);
sys_(mkdir, char *name, uint64_t mode);
sys_(clock_getres);
sys_(umask, uint64_t mask);
sys_(shutdown, int fd, int how);
sys_(bind, int fd, const struct sockaddr *addr, socklen_t addrlen);
sys_(listen, int fd, int backlog);
