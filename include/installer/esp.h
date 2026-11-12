#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <block_access.h>
#include <stdint.h>

namespace xjinstaller {

using EspSize = __SIZE_TYPE__;

struct EspPayload
{
    const void *loader;
    EspSize loader_bytes;
    const void *kernel;
    EspSize kernel_bytes;
};

constexpr uint64_t EspMinimumBytes = 64ULL * 1048576;
constexpr uint64_t EspMaximumSectors = 0x0ffffff0ULL;

struct EspRequirements
{
    uint64_t sectors, metadata_sectors, data_sectors;
};

// Pure arithmetic preflight, shared by the coordinator and private formatter.
// No allocation, data dereference or device callbacks. Inputs must be mapped
// kernel objects; range arithmetic cannot establish buffer mapping. Immutable
// payload buffers may overlap each other, but the output must be disjoint.
// Failure leaves requirements unchanged; success does not reserve disk space.
inline int esp_plan(uint64_t bytes, const EspPayload &payload, EspRequirements &requirements)
{
    auto valid = [](const void *pointer, EspSize count)
    {
        return pointer && count && uintptr_t(pointer) <= uintptr_t(-1) - count;
    };
    auto overlaps = [](const void *a, EspSize an, const void *b, EspSize bn)
    {
        return uintptr_t(a) <= uintptr_t(b) ? uintptr_t(b) - uintptr_t(a) < an :
                                             uintptr_t(a) - uintptr_t(b) < bn;
    };
    if (bytes < EspMinimumBytes || bytes % 512 || bytes / 512 > EspMaximumSectors ||
        payload.loader_bytes > 0xffffffffULL || payload.kernel_bytes > 0xffffffffULL ||
        !valid(&payload, sizeof(payload)) || !valid(&requirements, sizeof(requirements)) ||
        !valid(payload.loader, payload.loader_bytes) || !valid(payload.kernel, payload.kernel_bytes) ||
        overlaps(&requirements, sizeof(requirements), &payload, sizeof(payload)) ||
        overlaps(&requirements, sizeof(requirements), payload.loader, payload.loader_bytes) ||
        overlaps(&requirements, sizeof(requirements), payload.kernel, payload.kernel_bytes)) return -22;
    EspRequirements planned = {};
    planned.sectors = bytes / 512;
    planned.metadata_sectors = 32 + 2 * (((planned.sectors + 2) * 4 + 511) / 512) + 32;
    planned.data_sectors = (uint64_t(payload.loader_bytes) + 511) / 512 +
                           (uint64_t(payload.kernel_bytes) + 511) / 512 + 6;
    if (planned.metadata_sectors > planned.sectors ||
        planned.data_sectors > planned.sectors - planned.metadata_sectors) return -28;
    requirements = planned;
    return 0;
}

enum class EspBuildPhase : uint32_t
{
    None, Preflight, Published, InitialFlush, Formatted, Mounted,
    PlaceholderDurable, PayloadDurable, Readback, Closed, Complete
};

struct EspBuildStatus
{
    EspBuildPhase phase;
    // Accepted FatFs write/read prefixes, not an atomic or durable package
    // count after error. An ambiguous failed request may have persisted more.
    uint64_t written_bytes, verified_bytes;
    uint32_t fatfs_result;
    bool io_started, io_failed, quarantined, complete;
};

} // namespace xjinstaller

// Pure current subsystem-state observation: -ENODEV before initialization,
// -EIO after permanent FAT failure, -EBUSY for a current-TCB FAT guard. It does
// not acquire/retain a guard or reserve future availability. No I/O/allocation.
int fatfs_factory_ready();

// Trusted synchronous factory API. The caller retains its real whole-device
// node, exclusive parent lease, and immutable kernel-owned payload buffers.
// Entry while this TCB already owns a FAT operation guard is refused.
// Only a 512-byte-sector, 64 MiB or larger FAT32 window is supported. The ESP
// contains fixed directories, BOOTX64.EFI, kernel.krl, and a 96-byte all-zero
// root.bin placeholder. This call never publishes a valid boot binding.
// The parent lease is never released. Any error after session publication
// retains the complete session/device/slot and poisons that lease until reboot,
// even if the first I/O's authorization fails before any backend request.
// No cleanup I/O or retry follows a failed request. Success alone releases the
// private session after checked close/unmount, physical flush, and readback.
int fatfs_build_esp_window(const BlockAccessWindow *window, const xjinstaller::EspPayload *payload,
                           xjinstaller::EspBuildStatus *status = nullptr);
