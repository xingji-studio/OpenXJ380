#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <block_access.h>

struct vfs_node;
constexpr unsigned WSOD_REPORT_BYTES = 256 * 1024;
constexpr unsigned WSOD_FILE_SECTORS = WSOD_REPORT_BYTES / 512 + 1;
struct WsodFileMap
{
    BlockExtent extent;
    uint64_t    sectors[WSOD_FILE_SECTORS]; // Absolute 512-byte LBAs on physical_device.
};
// Preparation only, in a normal schedulable task. No mapping API is exposed
// through user ioctl. The retained file must be immutable before publication.
int  fatfs_wsod_map(struct vfs_node *node, WsodFileMap *map);
int  ext4fs_wsod_map(struct vfs_node *node, WsodFileMap *map);
int  vfs_wsod_pin(struct vfs_node *node);
void vfs_wsod_seal();

struct WsodDiskTarget
{
    unsigned transport;
    void    *disk;
    void    *buffer;
    uint64_t physical_buffer;
};
// A preallocated polling target. Unsupported geometries fail explicitly. Caller
// must have permanently stopped all CPUs before using it; never resumes tasks
// after any command submission, including a timeout (DMA storage stays pinned).
int ahci_wsod_prepare(const BlockExtent &extent, WsodDiskTarget *target);
int ahci_wsod_write(WsodDiskTarget *target, uint64_t lba, const void *data, unsigned sectors);
int ahci_wsod_flush(WsodDiskTarget *target);

int nvme_wsod_prepare(const BlockExtent &extent, WsodDiskTarget *target);
int nvme_wsod_write(WsodDiskTarget *target, uint64_t lba, const void *data, unsigned sectors);
int nvme_wsod_flush(WsodDiskTarget *target);
int ide_wsod_prepare(const BlockExtent &extent, WsodDiskTarget *target);
int ide_wsod_write(WsodDiskTarget *target, uint64_t lba, const void *data, unsigned sectors);
int ide_wsod_flush(WsodDiskTarget *target);
int wsod_disk_prepare(const BlockExtent &extent, WsodDiskTarget *target);
int wsod_disk_write(WsodDiskTarget *target, uint64_t lba, const void *data, unsigned sectors);
int wsod_disk_flush(WsodDiskTarget *target);
