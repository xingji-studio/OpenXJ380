// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <syscall/syscall.h>

int eventfd_create(uint32_t initial, int flags);
bool eventfd_is_node(vfs_node_t node);
int64_t eventfd_read_user(fd_file_handle *handle, void *buffer, size_t size);
int64_t eventfd_write_user(fd_file_handle *handle, const void *buffer, size_t size);
