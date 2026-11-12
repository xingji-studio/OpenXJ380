#pragma once

#include <efi/boot.h>
#include <fs/vfs/vfs.h>
#include <installer_protocol.h>

bool installer_boot_active(const BOOT_CONFIG &boot_config);
int  installer_prepare_root(const BOOT_CONFIG &boot_config);
bool installer_root_is_tmpfs_ready();
void installer_launch_app();
int installer_launch_app_checked();
const BOOT_CONFIG *installer_current_boot_config();
vfs_node_t installer_base_root();

// Full bounded validation for the existing PAK1 copy paths. Requires a mapped,
// immutable kernel-owned span; PAK2 is refused until its importer is connected.
int installer_validate_legacy_pak(const uint8_t *pak, size_t size);

namespace installer
{
enum class RootStageState : uint32_t
{
    empty = 0,
    building = 1,
    sealed = 2,
    discarding = 3,
    quarantined = 4,
    publishing = 5,
    published = 6
};

struct RootStageSnapshot
{
    uint64_t epoch, entries, data_bytes;
    int error;
    RootStageState state;
    bool published;
};

int root_prepare_private();
int root_discard_private();
int root_publish_private();
int root_prepare_mountpoint(const char *path);
bool root_stage_snapshot(RootStageSnapshot *);

// Boot-thread staging only. Does not publish a root, a Ready session, or disk
// write authority. The original config/map/page tables are trusted bootstrap
// storage; both package spans receive explicit ownership/mapping checks.
int session_boot_prepare(const BOOT_CONFIG &, const void *original_config);
int session_packages_prepare();
int session_source_prepare();
}
