// SPDX-License-Identifier: Apache-2.0
// Optional xattr operations without changing the kernel-module callback layout.
#include <fs/vfs/vfs.h>
#include <input_method.h>
#include <user/permissions.h>
#include <errno.h>

static const vfs_xattr_ops *attribute_ops[256];

void vfs_set_xattr_ops(int fsid, const vfs_xattr_ops *ops)
{
    if (fsid > 0 && fsid < 256) __atomic_store_n(&attribute_ops[fsid], ops, __ATOMIC_RELEASE);
}

static int fail_attribute(int error)
{
    permissions_set_file_error(-error);
    return error;
}

static const vfs_xattr_ops *operations(vfs_node_t node)
{
    return node && node->fsid < 256 ? __atomic_load_n(&attribute_ops[node->fsid], __ATOMIC_ACQUIRE) : NULL;
}

int vfs_getxattr(vfs_node_t node, const char *name, void *value, size_t capacity, size_t *required)
{
    if (required) *required = 0;
    if (!node || !name || !required || (!value && capacity)) return fail_attribute(-EINVAL);
    if (!vfs_check_access(node, NULL, IME_FILE_READ)) return -vfs_last_error();
    const vfs_xattr_ops *ops = operations(node);
    if (!ops || !ops->get) return fail_attribute(-EOPNOTSUPP);
    int error = ops->get(node, name, value, capacity, required);
    return error ? fail_attribute(error) : 0;
}

int vfs_listxattr(vfs_node_t node, char *names, size_t capacity, size_t *required)
{
    if (required) *required = 0;
    if (!node || !required || (!names && capacity)) return fail_attribute(-EINVAL);
    if (!vfs_check_access(node, NULL, IME_FILE_READ)) return -vfs_last_error();
    const vfs_xattr_ops *ops = operations(node);
    if (!ops || !ops->list) return fail_attribute(-EOPNOTSUPP);
    int error = ops->list(node, names, capacity, required);
    return error ? fail_attribute(error) : 0;
}

int vfs_setxattr(vfs_node_t node, const char *name, const void *value, size_t length, unsigned flags)
{
    if (!node || !name || (!value && length) || (flags & ~3U) || flags == 3U) return fail_attribute(-EINVAL);
    if (!vfs_check_access(node, NULL, IME_FILE_WRITE)) return -vfs_last_error();
    const vfs_xattr_ops *ops = operations(node);
    if (!ops || !ops->set) return fail_attribute(-EOPNOTSUPP);
    int error = ops->set(node, name, value, length, flags);
    return error ? fail_attribute(error) : 0;
}

int vfs_removexattr(vfs_node_t node, const char *name)
{
    if (!node || !name) return fail_attribute(-EINVAL);
    if (!vfs_check_access(node, NULL, IME_FILE_WRITE)) return -vfs_last_error();
    const vfs_xattr_ops *ops = operations(node);
    if (!ops || !ops->remove) return fail_attribute(-EOPNOTSUPP);
    int error = ops->remove(node, name);
    return error ? fail_attribute(error) : 0;
}
