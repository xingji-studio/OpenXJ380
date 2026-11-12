#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <fs/vfs/vfs.h>
#include <efi/root_binding.h>
#include <block_access.h>

struct stat;
struct statfs;
struct timespec;

// Advisory pure preflight for known factory blockers. No allocation/I/O/guard
// or reservation: later acquisition still rechecks all ordinary gates. Reports
// uninitialized ENODEV, poisoned/finished EIO, same-current mutex owner EBUSY.
int ext4fs_factory_ready();

// Trusted kernel API, never a user-pointer/syscall interface. The caller keeps
// its device reference throughout the call. Formatting requires an offline
// physical extent and an enabled scheduler/interrupts in ordinary task context.
struct ext4_format_options
{
    uint8_t uuid[16];
    uint64_t time_seconds;
    uint32_t journal_mib;
    char label[16];
};

struct ext4_format_status
{
    uint64_t bytes;
    uint32_t blocks, groups, inodes, free_blocks;
    bool metadata_durable;
    bool publication_started;
    bool primary_attempted;
    bool io_failed;
};

// UUID uniqueness belongs to the caller. A backend error poisons the extent
// until reboot; never retry through another alias. VALID metadata can already
// be durable when a final publication write/flush reports failure. Inspect
// status for completed phases, not as a guarantee that failed I/O did not land.
int ext4fs_format_node(vfs_node_t device, const ext4_format_options *options,
                       ext4_format_status *status = nullptr);

struct ext4_build_status
{
    ext4_format_status format;
    uint64_t required_blocks, required_inodes;
    // Confirmed native write prefixes only: an ambiguous failed I/O may have
    // persisted more data than written_bytes reports. This is not rollback.
    uint64_t imported_objects, written_bytes;
    bool import_started;
    bool import_complete;
    bool quarantined;
    bool import_io_failed;
};

// Trusted offline factory build, not an update or a user-pointer interface.
// Validate a complete immutable PAK2 and its resource budget before formatting,
// then import by parent/target inode while retaining one exclusive device lease.
// The caller must retain its kernel-owned package storage until success, or
// until reboot after quarantine/abandonment. Partial installation is never boot
// activation or a package-wide rollback. No cleanup I/O follows backend failure.
int ext4fs_build_node(vfs_node_t device, const ext4_format_options *options,
                      const void *package, size_t package_bytes, ext4_build_status *status = nullptr);

// Synchronous same-TCB build inside an authenticated whole-device lease. The
// caller retains the parent token/node/immutable package throughout completion
// or reboot after quarantine. The child never releases that token. No aliases
// are published, and every request is confined to the window; flush covers the
// physical whole device. Only real 512-byte logical-sector devices are accepted
// by this fresh-installer profile. This is a trusted API, not a user interface.
int ext4fs_build_window(const BlockAccessWindow *window, const ext4_format_options *options,
                        const void *package, size_t package_bytes, ext4_build_status *status = nullptr);

void ext4fs_init();
int ext4fs_mount_bound_root(const char *device, const XJ_ROOT_BINDING *binding);
// Task context only; permanently ends ext4 access before firmware poweroff.
int ext4fs_finish_all();
// Task-context hook before quiescent FD/VFS cleanup and before terminal TCB
// destruction. Clears abandoned mutex owners without resuming their WS state.
void ext4fs_quiescent_exit();
bool ext4fs_owns_node(vfs_node_t node);
int ext4fs_stat(vfs_node_t node, struct stat *result);
int ext4fs_statfs(vfs_node_t node, struct statfs *result);
int ext4fs_sync(vfs_node_t node);
int ext4fs_sync_all();
int ext4fs_chmod(vfs_node_t node, uint16_t mode);
int ext4fs_chown(vfs_node_t node, uint32_t uid, uint32_t gid);
int ext4fs_utimens(vfs_node_t node, const struct timespec *times);
// Compatibility API restricted to an existing byte range. Ordinary writable
// mounts use normal VFS operations. Caller retains an open node reference.
int ext4fs_pwrite_existing(vfs_node_t node, uint64_t offset, const void *bytes, size_t length,
                          uint64_t timestamp_seconds, size_t *completed);
