// SPDX-License-Identifier: Apache-2.0
// Native xattr syscall boundary: retain objects, bound buffers, copy user data.
#include <syscall/syscall.h>
#include <syscall/fd_lifetime.h>
#include <fs/vfs/vfs.h>
#include <mm/uaccess.h>
#include <task/pcb.h>
#include <user/permissions.h>
#include <errno.h>

namespace {

constexpr size_t kMaximumValue = 65536;

page_directory_t *user_directory()
{
    tcb_t task = get_current_task();
    return task && task->parent_group ? task->parent_group->pagedir : NULL;
}

int copy_text(const char *input, char *&output, size_t maximum)
{
    output = NULL;
    if (!input) return -EFAULT;
    char *text = (char *)malloc(maximum);
    if (!text) return -ENOMEM;
    for (size_t i = 0; i < maximum; ++i)
    {
        if (!copy_from_user_pagedir(user_directory(), text + i, input + i, 1))
        {
            free(text);
            return -EFAULT;
        }
        if (!text[i]) { output = text; return 0; }
    }
    free(text);
    return -ERANGE;
}

struct Object
{
    vfs_node_t node = NULL;
    bool retained = false;
    FdOperation operation;
    ~Object() { if (retained) vfs_close(node); }

    int path(const char *input, bool follow)
    {
        char *copied = NULL;
        int error = copy_text(input, copied, 4096);
        if (error) return error;
        if (!*copied) { free(copied); return -ENOENT; }
        char *absolute = vfs_cwd_path_build(copied);
        free(copied);
        if (!absolute) return -ENOMEM;
        node = vfs_open_metadata(absolute, follow);
        free(absolute);
        retained = node != NULL;
        return node ? 0 : permissions_file_error() ? -permissions_file_error() : -ENOENT;
    }

    int descriptor(int fd)
    {
        int error = operation.acquire(fd);
        if (error) return error;
        fd_file_handle *handle = operation.get();
        if (handle->flags & O_PATH) return -EBADF;
        if (handle->node->type & (file_pipe | file_ptmx | file_pts)) return -EOPNOTSUPP;
        node = handle->node;
        return 0;
    }

};

enum Operation { kGet, kList, kSet, kRemove };

uint64_t perform(Object &object, Operation operation, const char *user_name, void *user_value,
                 size_t size, int flags)
{
    if (size > kMaximumValue) return SYSCALL_FAULT_(E2BIG);
    if (operation == kSet && ((flags & ~3) || flags == 3)) return SYSCALL_FAULT_(EINVAL);
    char *name = NULL;
    int error = operation == kList ? 0 : copy_text(user_name, name, 256);
    if (error) return SYSCALL_FAULT_(-error);
    void *buffer = size ? malloc(size) : NULL;
    if (size && !buffer) { free(name); return SYSCALL_FAULT_(ENOMEM); }
    if (size && !user_value) error = -EFAULT;
    if (!error && operation == kSet && size &&
        !copy_from_user_pagedir(user_directory(), buffer, user_value, size)) error = -EFAULT;
    size_t required = 0;
    if (!error)
    {
        if (operation == kGet) error = vfs_getxattr(object.node, name, buffer, size, &required);
        if (operation == kList) error = vfs_listxattr(object.node, (char *)buffer, size, &required);
        if (operation == kSet) error = vfs_setxattr(object.node, name, buffer, size, unsigned(flags));
        if (operation == kRemove) error = vfs_removexattr(object.node, name);
    }
    if (!error && (operation == kGet || operation == kList) && size && required &&
        required > size) error = -ERANGE;
    if (!error && (operation == kGet || operation == kList) && size && required &&
        !copy_to_user_pagedir(user_directory(), user_value, buffer, required)) error = -EFAULT;
    free(buffer);
    free(name);
    if (error) return SYSCALL_FAULT_(-error);
    return operation == kGet || operation == kList ? required : 0;
}

uint64_t path_operation(const char *path, bool follow, Operation operation, const char *name,
                        void *value, size_t size, int flags = 0)
{
    Object object;
    int error = object.path(path, follow);
    return error ? SYSCALL_FAULT_(-error) : perform(object, operation, name, value, size, flags);
}

uint64_t fd_operation(int fd, Operation operation, const char *name, void *value, size_t size, int flags = 0)
{
    Object object;
    int error = object.descriptor(fd);
    return error ? SYSCALL_FAULT_(-error) : perform(object, operation, name, value, size, flags);
}

} // namespace

sys_(getxattr, const char *path, const char *name, void *value, size_t size)
{ return path_operation(path, true, kGet, name, value, size); }
sys_(lgetxattr, const char *path, const char *name, void *value, size_t size)
{ return path_operation(path, false, kGet, name, value, size); }
sys_(fgetxattr, int fd, const char *name, void *value, size_t size)
{ return fd_operation(fd, kGet, name, value, size); }
sys_(listxattr, const char *path, char *list, size_t size)
{ return path_operation(path, true, kList, NULL, list, size); }
sys_(llistxattr, const char *path, char *list, size_t size)
{ return path_operation(path, false, kList, NULL, list, size); }
sys_(flistxattr, int fd, char *list, size_t size)
{ return fd_operation(fd, kList, NULL, list, size); }
sys_(setxattr, const char *path, const char *name, const void *value, size_t size, int flags)
{ return path_operation(path, true, kSet, name, (void *)value, size, flags); }
sys_(lsetxattr, const char *path, const char *name, const void *value, size_t size, int flags)
{ return path_operation(path, false, kSet, name, (void *)value, size, flags); }
sys_(fsetxattr, int fd, const char *name, const void *value, size_t size, int flags)
{ return fd_operation(fd, kSet, name, (void *)value, size, flags); }
sys_(removexattr, const char *path, const char *name)
{ return path_operation(path, true, kRemove, name, NULL, 0); }
sys_(lremovexattr, const char *path, const char *name)
{ return path_operation(path, false, kRemove, name, NULL, 0); }
sys_(fremovexattr, int fd, const char *name)
{ return fd_operation(fd, kRemove, name, NULL, 0); }
