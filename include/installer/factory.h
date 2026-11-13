#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <installer/disk_layout.h>
#include <installer/progress.h>
#include <installer/package.h>

struct vfs_node;

namespace xjinstaller {

struct FactoryOptions
{
    DiskIdentities identities;
    uint8_t filesystem_uuid[16];
    uint64_t time_seconds;
    uint32_t journal_mib;
    uint64_t esp_bytes;
    uint32_t filesystem = 2;
    ProgressObserver progress = {};
    xjpak2::DataReader source_reader = {};
};

enum class FactoryPhase : uint32_t
{
    None, Preflight, Published, InitialFlush, EspInvalidated, GptWritten, GptVerified,
    RootBuilt, EspBuilt, RootVerified, EspVerified, ActivationAttempted,
    ActivationDurable, Complete
};

struct FactoryStatus
{
    FactoryPhase phase;
    int error;
    uint64_t read_bytes, written_bytes; // Coordinator raw I/O; excludes child session I/O.
    uint64_t package_records, package_data_bytes;
    bool io_started, io_failed, quarantined;
    bool activation_attempted, activation_durable, complete;
};

} // namespace xjinstaller

// Trusted synchronous fresh-image build, not a user-pointer syscall. The
// caller retains real source/target block nodes and a mapped immutable package
// until success, or until reboot after quarantine/forced task termination.
// Source physical identity is excluded. The caller must identify the actual
// boot/source medium; an arbitrary supplied source is not authentication.
// Uses one exclusive whole-target lease through new GPT, native ext4/PAK2 or
// FAT32/PAK1 root import, private FAT32 ESP construction, complete readback and
// final binding sector. The historical function name is kept for callers;
// FactoryOptions::filesystem selects the root profile (1 FAT32, 2 ext4).
// All geometry uses 512-byte logical sectors. No repair/component filtering,
// keep-users, live-root replacement, partition publication or upgrade rollback.
// Before the first activation attempt both filesystems and all referenced
// bytes are already durable. A failed final request can still persist a valid
// binding: status never promises nonactivation after an ambiguous failure.
// Once any backend I/O begins, an error retains/poisons the entire lifetime
// instead of attempting cleanup I/O, retry or partial transaction unwind.
// Installer boot routing, source-RAM handoff and request authorization remain
// separate; this API alone does not enable installation from user space.
int installer_build_ext4_disk(struct vfs_node *target, struct vfs_node *source,
                              const xjinstaller::FactoryOptions *, const void *package,
                              __SIZE_TYPE__ package_bytes, xjinstaller::FactoryStatus *status = nullptr);
