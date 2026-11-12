#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <installer/package.h>
#include <installer/esp.h>
#include <installer/progress.h>

namespace xjinstaller
{
inline uint32_t fat32_cluster_bytes(uint64_t volume_bytes)
{
    uint32_t bytes = volume_bytes < 512ULL * 1048576 ? 512 : 4096;
    while (volume_bytes / bytes > 0x0fffff00ULL && bytes < 65536) bytes *= 2;
    return bytes;
}

struct Fat32Name
{
    uint64_t hash;
    xjpak2::Size record;
};

struct Fat32Requirements
{
    uint64_t required_bytes, data_bytes, records;
    uint32_t cluster_bytes;
};

// Validated PAK1 or indexed flat FAT32 profile. Names use FatFs Unicode folding;
// case collisions and the conservative allocation budget are refused without I/O.
int fat32_check_plan(const xjpak2::Plan &, Fat32Name *, xjpak2::Size capacity,
                      uint64_t volume_bytes, Fat32Requirements &);

// Uses the same private, lease-authenticated disk window as the ESP builder.
// The trusted coordinator must first pass fat32_check_plan and retain the
// immutable package/plan and parent lease for the complete synchronous call.
// Import and independent remount/readback never publish the boot binding.
int fatfs_build_root_window(const BlockAccessWindow *, const xjpak2::Plan *, uint32_t volume_id,
                             EspBuildStatus *, const ProgressObserver & = {});
int fatfs_verify_root_window(const BlockAccessWindow *, const xjpak2::Plan *, uint32_t volume_id,
                              EspBuildStatus *, const ProgressObserver & = {});
} // namespace xjinstaller

struct XJ_ROOT_BINDING;
int fatfs_mount_bound_root(const char *source, const XJ_ROOT_BINDING *binding);
