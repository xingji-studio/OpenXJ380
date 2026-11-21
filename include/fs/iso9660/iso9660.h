#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <fs/vfs/vfs.h>

struct statfs;
struct stat;

void iso9660fs_init();
bool iso9660fs_owns_node(vfs_node_t node);
int  iso9660fs_statfs(vfs_node_t node, struct statfs *result);
int  iso9660fs_stat(vfs_node_t node, struct stat *result);
// Task-context hook before descriptor cleanup and terminal TCB destruction.
void iso9660fs_quiescent_exit();
// Whole-media discovery after root mount, and after hotplug scan leases end.
// Installer mode deliberately retains its existing raw source/target policy.
void iso9660fs_automount();
void iso9660fs_try_automount(size_t device);
