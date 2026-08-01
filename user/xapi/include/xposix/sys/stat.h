#pragma once

#include "types.h"
#include "../time.h"

// 文件类型掩码：用于从 st_mode 中取出“这是普通文件/目录/设备”等类型位。
#define S_IFMT   00170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000

// 文件类型判断宏：toybox 这类 POSIX 工具会大量用它们分流 ls/cp/rm 的行为。
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

// POSIX 权限位：保留完整的用户/组/其他权限，便于 chmod/stat/ls 共享同一套语义。
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IRWXU 0700
#define S_IRWXG 0070
#define S_IRWXO 0007
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IREAD  S_IRUSR
#define S_IWRITE S_IWUSR
#define S_IEXEC  S_IXUSR
#define ACCESSPERMS (S_IRWXU | S_IRWXG | S_IRWXO)
#define ALLPERMS    (S_ISUID | S_ISGID | S_ISVTX | ACCESSPERMS)
#define DEFFILEMODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)

struct stat {
    long            st_dev;
    unsigned long   st_ino;
    unsigned long   st_nlink;
    int             st_mode;
    int             st_uid;
    int             st_gid;
    long            st_rdev;
    long long       st_size;
    long            st_blksize;
    unsigned long   st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    char            _pad[24];
};

// 传统 BSD/GNU 字段别名：很多小工具仍写 st_mtime，而不是直接访问 st_mtim.tv_sec。
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

#ifdef __cplusplus
extern "C" {
#endif

int stat(const char *path, struct stat *buf);
int lstat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int fstatat(int dirfd, const char *path, struct stat *buf, int flags);
int mkdir(const char *path, mode_t mode);
int mkdirat(int dirfd, const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);
int fchmod(int fd, mode_t mode);
int chown(const char *path, uid_t owner, gid_t group);
int lchown(const char *path, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
int truncate(const char *path, off_t length);
int ftruncate(int fd, off_t length);
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);

#ifdef __cplusplus
}
#endif
