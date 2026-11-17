#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <installer/partition_table.h>

struct vfs_node;
namespace xjinstaller {
// Kernel-owned immutable plan. Source user fields have been resolved through
// same-owner opaque disk handles. Nodes and tables survive the entire job,
// including quarantine. No pointers or device numbers cross the user ABI.
struct PartitionInstall
{
    partition::Table before, after, esp_before;
    struct vfs_node *esp_node;
    unsigned         root_number, esp_number;
    unsigned         tool_filesystem;
    bool             reuse_esp, separate_disk;
};
} // namespace xjinstaller
