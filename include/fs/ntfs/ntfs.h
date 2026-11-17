#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <fs/vfs/vfs.h>

// Data volumes: ntfsro:/dev/<partition> or explicit experimental ntfsrw:.
// P3 metadata transactions are bounded; see docs/NTFS_WRITE_DESIGN.md.
void ntfsfs_init();
bool ntfsfs_owns_node(vfs_node_t node);
// Task-context cleanup before FD/VFS teardown and before recycling a TCB.
void ntfsfs_quiescent_exit();
