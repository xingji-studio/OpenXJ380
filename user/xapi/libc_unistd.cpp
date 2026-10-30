// XJ380 User-space C Library - POSIX System Call Wrappers
// This file provides POSIX-compatible system call wrappers

#include "./include/libsys.h"
#include "./include/stdint.h"
#include "./include/xposix/unistd.h"
#include "./include/xposix/fcntl.h"
#include "./include/xposix/sys/stat.h"
#include "./include/xposix/errno.h"
#include "./include/xposix/stdarg.h"

extern "C" {

int errno = 0;

int *__errno_location(void)
{
    return &errno;
}

char *strerror(int errnum)
{
    switch (errnum)
    {
    case 0: return (char *)"Success";
    case EPERM: return (char *)"Operation not permitted";
    case ENOENT: return (char *)"No such file or directory";
    case ESRCH: return (char *)"No such process";
    case EINTR: return (char *)"Interrupted system call";
    case EIO: return (char *)"I/O error";
    case ENOMEM: return (char *)"Out of memory";
    case EACCES: return (char *)"Permission denied";
    case EFAULT: return (char *)"Bad address";
    case EBUSY: return (char *)"Device or resource busy";
    case EEXIST: return (char *)"File exists";
    case ENOTDIR: return (char *)"Not a directory";
    case EISDIR: return (char *)"Is a directory";
    case EINVAL: return (char *)"Invalid argument";
    case ENOSPC: return (char *)"No space left on device";
    case EROFS: return (char *)"Read-only file system";
    case EPIPE: return (char *)"Broken pipe";
    case EDOM: return (char *)"Math argument out of domain";
    case ERANGE: return (char *)"Math result not representable";
    default: return (char *)"Unknown error";
    }
}

void perror(const char *prefix)
{
    if (prefix && *prefix)
    {
        write(2, prefix, strlen(prefix));
        write(2, ": ", 2);
    }
    write(2, strerror(errno), strlen(strerror(errno)));
    write(2, "\n", 1);
}

ssize_t read(int fd, void *buf, size_t len)
{
    return (ssize_t)enter_syscall(SYS_READ, fd, (uint64_t)buf, len, 0, 0, 0);
}

ssize_t write(int fd, const void *buf, size_t len)
{
    return (ssize_t)enter_syscall(SYS_WRITE, fd, (uint64_t)buf, len, 0, 0, 0);
}

int open(const char *pathname, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
    {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    return (int)enter_syscall(SYS_OPEN, (uint64_t)pathname, flags, mode, 0, 0, 0);
}

int close(int fd)
{
    return (int)enter_syscall(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
}

off_t lseek(int fd, off_t offset, int whence)
{
    return (off_t)enter_syscall(SYS_LSEEK, fd, offset, whence, 0, 0, 0);
}

int mkdir(const char *pathname, mode_t mode)
{
    return (int)enter_syscall(SYS_MKDIR, (uint64_t)pathname, mode, 0, 0, 0, 0);
}

int rmdir(const char *pathname)
{
    return (int)enter_syscall(SYS_RMDIR, (uint64_t)pathname, 0, 0, 0, 0, 0);
}

int unlink(const char *pathname)
{
    return (int)enter_syscall(SYS_UNLINK, (uint64_t)pathname, 0, 0, 0, 0, 0);
}

int rename(const char *oldpath, const char *newpath)
{
    return (int)enter_syscall(SYS_RENAME, (uint64_t)oldpath, (uint64_t)newpath, 0, 0, 0, 0);
}

int access(const char *pathname, int mode)
{
    return (int)enter_syscall(SYS_ACCESS, (uint64_t)pathname, mode, 0, 0, 0, 0);
}

int stat(const char *pathname, struct stat *statbuf)
{
    return (int)enter_syscall(SYS_STAT, (uint64_t)pathname, (uint64_t)statbuf, 0, 0, 0, 0);
}

int fstat(int fd, struct stat *statbuf)
{
    return (int)enter_syscall(SYS_FSTAT, fd, (uint64_t)statbuf, 0, 0, 0, 0);
}

int lstat(const char *pathname, struct stat *statbuf)
{
    return stat(pathname, statbuf); // No symlinks in XJ380
}

pid_t getpid(void)
{
    return (pid_t)enter_syscall(SYS_GETPID, 0, 0, 0, 0, 0, 0);
}

pid_t getppid(void)
{
    return (pid_t)enter_syscall(SYS_GETPPID, 0, 0, 0, 0, 0, 0);
}

uid_t getuid(void)
{
    return 0; // Root user
}

uid_t geteuid(void)
{
    return 0;
}

gid_t getgid(void)
{
    return 0;
}

gid_t getegid(void)
{
    return 0;
}

int chdir(const char *path)
{
    return (int)enter_syscall(SYS_CHDIR, (uint64_t)path, 0, 0, 0, 0, 0);
}

char *getcwd(char *buf, size_t size)
{
    if (buf == NULL || size == 0) return NULL;

    int result = (int)enter_syscall(SYS_GETCWD, (uint64_t)buf, size, 0, 0, 0, 0);
    if (result < 0) return NULL;
    return buf;
}

int dup(int oldfd)
{
    return (int)enter_syscall(SYS_DUP, oldfd, 0, 0, 0, 0, 0);
}

int dup2(int oldfd, int newfd)
{
    return (int)enter_syscall(SYS_DUP2, oldfd, newfd, 0, 0, 0, 0);
}

int pipe(int pipefd[2])
{
    return (int)enter_syscall(SYS_PIPE, (uint64_t)pipefd, 0, 0, 0, 0, 0);
}

unsigned int sleep(unsigned int seconds)
{
    struct timespec req;
    req.tv_sec = seconds;
    req.tv_nsec = 0;
    enter_syscall(SYS_NANOSLEEP, (uint64_t)&req, 0, 0, 0, 0, 0);
    return 0;
}

int usleep(unsigned long usec)
{
    struct timespec req;
    req.tv_sec = usec / 1000000;
    req.tv_nsec = (usec % 1000000) * 1000;
    enter_syscall(SYS_NANOSLEEP, (uint64_t)&req, 0, 0, 0, 0, 0);
    return 0;
}

int execve(const char *pathname, char *const argv[], char *const envp[])
{
    return (int)enter_syscall(SYS_EXECVE, (uint64_t)pathname, (uint64_t)argv, (uint64_t)envp, 0, 0, 0);
}

int execv(const char *pathname, char *const argv[])
{
    return execve(pathname, argv, NULL);
}

int execvp(const char *file, char *const argv[])
{
    // Simple implementation - just try to execute directly
    // TODO: Implement PATH search
    return execv(file, argv);
}

pid_t fork(void)
{
    return (pid_t)enter_syscall(SYS_FORK, 0, 0, 0, 0, 0, 0);
}

pid_t vfork(void)
{
    return fork();
}

int wait(int *wstatus)
{
    return (int)enter_syscall(SYS_WAIT4, -1, (uint64_t)wstatus, 0, 0, 0, 0);
}

pid_t waitpid(pid_t pid, int *wstatus, int options)
{
    return (pid_t)enter_syscall(SYS_WAIT4, (uint64_t)pid, (uint64_t)wstatus, options, 0, 0, 0);
}

void exit(int status)
{
    enter_syscall(SYS_EXIT, status, 0, 0, 0, 0, 0);
    while (1); // Should not reach here
}

void _exit(int status)
{
    exit(status);
}

void abort(void)
{
    exit(127);
}

int isatty(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;
    return (st.st_mode & 0170000) == 0020000; // S_IFCHR
}

int fcntl(int fd, int cmd, uint64_t arg)
{
    return (int)enter_syscall(SYS_FCNTL, fd, cmd, arg, 0, 0, 0);
}

int ioctl(int fd, unsigned long request, ...)
{
    // Simplified - ignore additional arguments
    return (int)enter_syscall(SYS_IOCTL, fd, request, 0, 0, 0, 0);
}

int getdents(int fd, struct dirent *dents, size_t size)
{
    return (int)enter_syscall(SYS_GETDENTS, fd, (uint64_t)dents, size, 0, 0, 0);
}

} // extern "C"
