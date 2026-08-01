#pragma once

#include "sys/types.h"

#include "../krlibc.h"

#ifdef __cplusplus
extern "C" {
#endif

pid_t fork();
pid_t vfork();
pid_t getpid(void);
pid_t getppid(void);
pid_t setsid(void);
pid_t getsid(pid_t pid);
pid_t gettid(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int   getgroups(int size, gid_t list[]);

int close(int fd);
ssize_t read(int fd, void *buf, size_t len);
ssize_t write(int fd, const void *buf, size_t len);
int dup(int fd);
int dup2(int fd, int newfd);
int pipe(int pipefd[2]);
int pipe2(int pipefd[2], int flags);
int execve(const char *filename, char *const argv[], char *const envp[]);
int brk(void *addr);
void *sbrk(intptr_t increment);
int chdir(const char *path);
int fchdir(int fd);
char *getcwd(char *buffer, size_t length);
int access(const char *path, int mode);
int unlink(const char *path);
int rmdir(const char *path);
int rename(const char *oldpath, const char *newpath);
int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
int link(const char *oldpath, const char *newpath);
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
int symlink(const char *target, const char *linkpath);
int symlinkat(const char *target, int newdirfd, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
off_t lseek(int fd, off_t offset, int whence);
int fsync(int fd);
int fdatasync(int fd);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned long usec);
int isatty(int fd);
char *ttyname(int fd);

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define SEEK_DATA 3
#define SEEK_HOLE 4

#ifdef __cplusplus
}
#endif
