#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <syscall/syscall.h>

// Linux inotify ABI values; event names are NUL-terminated and padded to four bytes.
constexpr uint32_t IN_ACCESS = 0x00000001, IN_MODIFY = 0x00000002, IN_ATTRIB = 0x00000004;
constexpr uint32_t IN_CLOSE_WRITE = 0x00000008, IN_CLOSE_NOWRITE = 0x00000010, IN_OPEN = 0x00000020;
constexpr uint32_t IN_MOVED_FROM = 0x00000040, IN_MOVED_TO = 0x00000080, IN_CREATE = 0x00000100;
constexpr uint32_t IN_DELETE = 0x00000200, IN_DELETE_SELF = 0x00000400, IN_MOVE_SELF = 0x00000800;
constexpr uint32_t IN_UNMOUNT = 0x00002000, IN_Q_OVERFLOW = 0x00004000, IN_IGNORED = 0x00008000;
constexpr uint32_t IN_ONLYDIR = 0x01000000, IN_DONT_FOLLOW = 0x02000000, IN_EXCL_UNLINK = 0x04000000;
constexpr uint32_t IN_MASK_CREATE = 0x10000000, IN_MASK_ADD = 0x20000000;
constexpr uint32_t IN_ISDIR = 0x40000000, IN_ONESHOT = 0x80000000;

struct InotifyEvent
{
    int32_t wd;
    uint32_t mask, cookie, len;
};
static_assert(sizeof(InotifyEvent) == 16, "Linux inotify event header");

int inotify_create(int flags);
bool inotify_is_node(vfs_node_t node);
int inotify_add(fd_file_handle *handle, vfs_node_t node, uint32_t mask);
int inotify_remove(fd_file_handle *handle, int wd);
int64_t inotify_read_user(fd_file_handle *handle, void *buffer, size_t size);

// Normal-context notification captures identity/name under the VFS child lock.
void inotify_vfs_event(vfs_node_t node, uint32_t mask);
// These bounded publication helpers require the VFS child lock. They never
// allocate, free, enter VFS, touch user memory, or wait. Lock order is child -> registry.
void inotify_vfs_child_locked(vfs_node_t parent, vfs_node_t child, const char *name, uint32_t mask);
void inotify_vfs_unlink_locked(vfs_node_t node, vfs_node_t parent, const char *name);
void inotify_vfs_move_locked(vfs_node_t node, vfs_node_t old_parent, const char *old_name);
