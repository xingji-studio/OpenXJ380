// SPDX-License-Identifier: Apache-2.0
// Original bounded private installer-root preparation; no root is published.
#include <errno.h>
#include <fs/vfs/tmpfs.h>
#include <installer_mode.h>
#include <installer/context.h>
#include <installer/package.h>
#include <krlibc.h>
#include <proto.hpp>
#include <task/pcb.h>
#include <user/permissions.h>
#include <user/user.h>

int installer_validate_legacy_pak(const uint8_t *pak, size_t size)
{
    // The bootloader owns this immutable, reserved kernel span. Header checks
    // bound arena allocation; only the complete validation permits legacy apply.
    xjpak2::Limits limits;
    limits.max_path = 511;
    // This legacy destination is FAT32, whose on-disk size field is 32 bits.
    limits.max_file_bytes = 0xffffffffULL;
    xjpak2::HeaderInfo header;
    int result = xjpak2::inspect_header(pak, size, limits, header);
    if (result < 0) return result;
    // Richer metadata/link records need an inode-relative importer. Never feed
    // PAK2 to the old pathname/FAT copy path and silently discard those records.
    if (!header.legacy_v1) return -EOPNOTSUPP;
    xjpak2::Record *records = NULL;
    if (header.record_count != 0)
    {
        records = (xjpak2::Record *)malloc(header.record_count * sizeof(xjpak2::Record));
        if (records == NULL) return -ENOMEM;
    }
    xjpak2::Plan plan;
    result = xjpak2::validate(pak, size, limits, {records, header.record_count}, plan);
    free(records);
    return result;
}

bool installer_boot_active(const BOOT_CONFIG &boot_config)
{
    return (boot_config.boot_flags & BOOT_FLAG_INSTALLER) != 0 &&
           boot_config.installer_root_pak != 0 &&
           boot_config.installer_root_pak_size >= sizeof(xj380_pak_header) &&
           boot_config.system_payload_pak != 0 &&
           boot_config.system_payload_pak_size >= sizeof(xj380_pak_header);
}

int installer_prepare_root(const BOOT_CONFIG &boot_config)
{
    (void)boot_config;
    // A legacy physical pointer/boot flag cannot bypass the session's mapped
    // ownership proof, complete package validation and retained source lease.
    return -EOPNOTSUPP;
}

bool installer_root_is_tmpfs_ready()
{
    installer::RootStageSnapshot snapshot = {};
    return installer::root_stage_snapshot(&snapshot) && snapshot.published &&
           snapshot.state == installer::RootStageState::published;
}

int installer_launch_app_checked()
{
    // Private preparation/sealing alone never creates an executable mount.
    if (!installer_root_is_tmpfs_ready()) return -ENODEV;
    if (!task_effective_user() || !permissions_have(user_type_permission_mask(XUT_Admin))) return -EACCES;
    return create_user_process_from_file((char *)"/apps/installer/installer.elf", NULL, NULL);
}

void installer_launch_app()
{
    int result = installer_launch_app_checked();
    write_serial_fmt("installer: launch /apps/installer/installer.elf result=%d\n", result);
}

const BOOT_CONFIG *installer_current_boot_config()
{
    return NULL;
}

vfs_node_t installer_base_root()
{
    return NULL;
}

namespace installer
{
namespace
{
constexpr uint64_t root_entry_limit = 32768;
constexpr uint64_t root_memory_limit = 64ULL << 20;
TmpfsPrivateTree private_root;
RootStageSnapshot root_snapshot;
const void *root_owner;
uint64_t root_owner_tid;
unsigned char root_lock;

class RootLock
{
    uint64_t flags;
public:
    RootLock()
    {
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
        while (__atomic_test_and_set(&root_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    }
    ~RootLock()
    {
        __atomic_clear(&root_lock, __ATOMIC_RELEASE);
        __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
    }
};

int stage_failed(int error)
{
    TmpfsPrivateSnapshot snapshot = {};
    int cleanup = 0;
    if (private_root.state)
    {
        tmpfs_private_snapshot(&private_root, &snapshot);
        cleanup = tmpfs_private_discard(&private_root);
    }
    RootLock lock;
    root_snapshot.error = error;
    root_snapshot.entries = cleanup ? snapshot.entries : 0;
    root_snapshot.data_bytes = cleanup ? snapshot.data_bytes : 0;
    root_snapshot.state = cleanup ? RootStageState::quarantined : RootStageState::empty;
    root_snapshot.published = false;
    if (!cleanup)
    {
        root_snapshot.epoch = 0;
        root_owner = NULL;
        root_owner_tid = 0;
    }
    return error;
}

int verify_records(const xjpak2::Plan &plan)
{
    for (size_t index = 0; index < plan.count; ++index)
    {
        const xjpak2::Record &record = plan.records[index];
        char path[512];
        memcpy(path, record.path.data, record.path.size);
        path[record.path.size] = '\0';
        TmpfsPrivateFile file = {};
        int error = tmpfs_private_file(&private_root, path, &file);
        if (error) return error;
        bool directory = record.kind == xjpak2::Kind::Directory;
        if (file.directory != directory || file.mode != record.mode || file.uid || file.gid ||
            file.bytes != record.payload.size ||
            (file.bytes && memcmp(file.data, record.payload.data, file.bytes))) return -EIO;
    }
    TmpfsPrivateFile executable = {}, guide = {};
    int error = tmpfs_private_file(&private_root, "/apps/installer/installer.elf", &executable);
    if (error) return error;
    error = tmpfs_private_file(&private_root, "/config/README.md", &guide);
    if (error) return error;
    if (executable.directory || !executable.bytes || !(executable.mode & 0111) || guide.directory || !guide.bytes)
        return -EINVAL;
    return 0;
}
} // namespace

int root_prepare_private()
{
    tcb_t task = get_current_task();
    if (!task) return -EPERM;
    {
        RootLock lock;
        if (root_snapshot.state != RootStageState::empty) return -EALREADY;
        root_snapshot = {};
        root_snapshot.state = RootStageState::building;
        root_owner = task;
        root_owner_tid = task->tid;
    }
    xjpak2::Plan plan;
    uint64_t epoch = 0;
    int error = session_root_package_view(&plan, &epoch);
    if (error) return stage_failed(error);
    if (!epoch || !plan.validated || !plan.legacy_v1 || !plan.records || !plan.count || !plan.inode_count)
        return stage_failed(-EINVAL);
    // The session also compares this conservative estimate with its RAM-derived
    // budget before yielding the view. Refuse the supported fixed profile here
    // before allocating a vnode, entry, path or copied file byte.
    if (plan.inode_count > root_entry_limit || plan.count > root_entry_limit ||
        plan.data_bytes > root_memory_limit - 4096 ||
        plan.inode_count > (root_memory_limit - 4096 - plan.data_bytes) / 1024) return stage_failed(-E2BIG);
    for (size_t index = 0; index < plan.count; ++index)
    {
        const auto &record = plan.records[index];
        if (!record.path.data || !record.path.size || record.path.size >= 512 ||
            (record.kind != xjpak2::Kind::Directory && record.kind != xjpak2::Kind::Regular))
            return stage_failed(-EINVAL);
    }
    error = tmpfs_private_begin(&private_root, {plan.inode_count, plan.data_bytes});
    if (error) return stage_failed(error);
    for (size_t index = 0; index < plan.count; ++index)
    {
        const auto &record = plan.records[index];
        char path[512];
        memcpy(path, record.path.data, record.path.size);
        path[record.path.size] = '\0';
        error = tmpfs_private_add(&private_root, path, record.kind == xjpak2::Kind::Directory,
                                   record.payload.data, record.payload.size, record.mode, 0, 0);
        if (error) return stage_failed(error);
    }
    if ((error = verify_records(plan))) return stage_failed(error);
    if ((error = tmpfs_private_seal(&private_root))) return stage_failed(error);
    TmpfsPrivateSnapshot snapshot = {};
    if ((error = tmpfs_private_snapshot(&private_root, &snapshot))) return stage_failed(error);
    if (!snapshot.sealed || snapshot.quarantined || snapshot.entries != plan.inode_count ||
        snapshot.data_bytes != plan.data_bytes) return stage_failed(-EIO);
    {
        RootLock lock;
        root_snapshot = {epoch, snapshot.entries, snapshot.data_bytes, 0, RootStageState::sealed, false};
    }
    return 0;
}

int root_discard_private()
{
    tcb_t task = get_current_task();
    {
        RootLock lock;
        if (root_snapshot.state != RootStageState::sealed) return -ENODEV;
        if (!task || root_owner != task || root_owner_tid != task->tid) return -EPERM;
        root_snapshot.state = RootStageState::discarding;
    }
    int error = tmpfs_private_discard(&private_root);
    RootLock lock;
    if (error)
    {
        root_snapshot.error = error;
        root_snapshot.state = RootStageState::quarantined;
    }
    else
    {
        root_snapshot = {};
        root_owner = NULL;
        root_owner_tid = 0;
    }
    return error;
}

bool root_stage_snapshot(RootStageSnapshot *output)
{
    if (!output) return false;
    RootLock lock;
    *output = root_snapshot;
    return true;
}

int root_publish_private()
{
    tcb_t task = get_current_task();
    uint64_t expected_epoch = 0;
    {
        RootLock lock;
        if (root_snapshot.state != RootStageState::sealed || root_snapshot.published) return -EBUSY;
        if (!task || root_owner != task || root_owner_tid != task->tid) return -EPERM;
        expected_epoch = root_snapshot.epoch;
        root_snapshot.state = RootStageState::publishing;
    }
    // Source lifetime and the original session epoch are reauthenticated before
    // the irreversible namespace commit. No callback runs with root_lock held.
    xjpak2::Plan plan;
    uint64_t epoch = 0;
    int error = session_root_package_view(&plan, &epoch);
    if (!error && (epoch != expected_epoch || !plan.validated || !plan.legacy_v1)) error = -ESTALE;
    if (!error) error = tmpfs_private_publish(&private_root);
    RootLock lock;
    root_snapshot.error = error;
    root_snapshot.state = error ? RootStageState::sealed : RootStageState::published;
    root_snapshot.published = !error;
    return error;
}

int root_prepare_mountpoint(const char *path)
{
    tcb_t task = get_current_task();
    {
        RootLock lock;
        if (root_snapshot.state != RootStageState::published || !root_snapshot.published) return -EBUSY;
        if (!task || root_owner != task || root_owner_tid != task->tid) return -EPERM;
    }
    return tmpfs_private_mountpoint(&private_root, path);
}
} // namespace installer
