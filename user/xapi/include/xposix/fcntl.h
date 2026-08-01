#pragma once

#include "../stdint.h"

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

#define O_CREAT     0100
#define O_EXCL      0200
#define O_NOCTTY    0400
#define O_TRUNC     01000
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030

#define O_APPEND   02000
#define O_NONBLOCK 04000
#define O_NDELAY   O_NONBLOCK
#define O_DSYNC     010000
#define O_ASYNC     020000
#define O_DIRECT    040000
#define O_LARGEFILE 0100000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW  0400000
#define O_NOATIME   01000000
#define O_CLOEXEC   02000000
#define O_SYNC      04010000
#define O_RSYNC     O_SYNC
#define O_PATH      010000000

#ifdef __cplusplus
extern "C" {
#endif

int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int creat(const char *path, unsigned int mode);
int fcntl(int fd, int cmd, uint64_t arg);
int posix_openpt(int flags);

#ifdef __cplusplus
}
#endif
