// SPDX-License-Identifier: Apache-2.0
#include "include/sys/xattr.h"
#include "include/libsys.h"
#include "include/xposix/syscall_ret.h"

extern "C" int setxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
    return (int)__xposix_ret(enter_syscall(188, (uint64_t)path, (uint64_t)name, (uint64_t)value, size, flags, 0));
}

extern "C" int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
    return (int)__xposix_ret(enter_syscall(189, (uint64_t)path, (uint64_t)name, (uint64_t)value, size, flags, 0));
}

extern "C" int fsetxattr(int fd, const char *name, const void *value, size_t size, int flags)
{
    return (int)__xposix_ret(enter_syscall(190, fd, (uint64_t)name, (uint64_t)value, size, flags, 0));
}

extern "C" ssize_t getxattr(const char *path, const char *name, void *value, size_t size)
{
    return (ssize_t)__xposix_ret(enter_syscall(191, (uint64_t)path, (uint64_t)name, (uint64_t)value, size, 0, 0));
}

extern "C" ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size)
{
    return (ssize_t)__xposix_ret(enter_syscall(192, (uint64_t)path, (uint64_t)name, (uint64_t)value, size, 0, 0));
}

extern "C" ssize_t fgetxattr(int fd, const char *name, void *value, size_t size)
{
    return (ssize_t)__xposix_ret(enter_syscall(193, fd, (uint64_t)name, (uint64_t)value, size, 0, 0));
}

extern "C" ssize_t listxattr(const char *path, char *list, size_t size)
{
    return (ssize_t)__xposix_ret(enter_syscall(194, (uint64_t)path, (uint64_t)list, size, 0, 0, 0));
}

extern "C" ssize_t llistxattr(const char *path, char *list, size_t size)
{
    return (ssize_t)__xposix_ret(enter_syscall(195, (uint64_t)path, (uint64_t)list, size, 0, 0, 0));
}

extern "C" ssize_t flistxattr(int fd, char *list, size_t size)
{
    return (ssize_t)__xposix_ret(enter_syscall(196, fd, (uint64_t)list, size, 0, 0, 0));
}

extern "C" int removexattr(const char *path, const char *name)
{
    return (int)__xposix_ret(enter_syscall(197, (uint64_t)path, (uint64_t)name, 0, 0, 0, 0));
}

extern "C" int lremovexattr(const char *path, const char *name)
{
    return (int)__xposix_ret(enter_syscall(198, (uint64_t)path, (uint64_t)name, 0, 0, 0, 0));
}

extern "C" int fremovexattr(int fd, const char *name)
{
    return (int)__xposix_ret(enter_syscall(199, fd, (uint64_t)name, 0, 0, 0, 0));
}
