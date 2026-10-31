#pragma once

#include "krlibc.h"
#include "device.h"
#include "llist.h"
#include <block_access.h>

typedef struct device_handle *device_handle_t;

struct device_handle {
    device_t           *device; // 设备句柄
    char               *name;   // 节点名
    bool                is_dir; // 是否为目录
    struct llist_header curr;   // 当前节点
    struct llist_header child;  // 子节点链表
};

void   devfs_setup();
size_t devfs_read(void *file, void *addr, size_t offset, size_t size);
size_t devfs_write(void *file, const void *addr, size_t offset, size_t size);

// Original bounded registration capture (Apache-2.0). No path/name authority.
struct DevfsBlockReference
{
    vfs_node_t node;
    BlockExtent extent;
    uint64_t bytes;
    uint32_t sector_size;
    bool writable;
    bool quarantined;
};

// Retains one real cached devfs node for this exact nonzero registration.
// Device publication and VFS tree mutation are serialized independently; no
// allocator, backend callback or blocking work runs under the tree spin lock.
// Success owns one reference, released with vfs_close outside all locks. Error
// normally clears output; a failed close retains output with quarantined=true,
// which the caller must preserve until reboot rather than reuse/free.
int devfs_retain_block(uint32_t device, uint64_t expected_generation, DevfsBlockReference *output);
