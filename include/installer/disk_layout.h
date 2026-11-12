#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original bounded fresh-install GPT profile; no disk I/O or runtime dependency.
#include <stdint.h>

namespace xjinstaller {

using DiskLayoutSize = __SIZE_TYPE__;

constexpr uint32_t DiskSectorBytes = 512;
constexpr uint32_t DiskEntryCount = 128;
constexpr uint32_t DiskEntryBytes = 128;
constexpr uint32_t DiskTableBytes = DiskEntryCount * DiskEntryBytes;
constexpr uint64_t DiskMiB = 1048576;

struct DiskIdentities
{
    // Exact GPT on-disk GUID bytes (the first three fields are little endian),
    // not UUID network-order bytes. All three identities must be nonzero/distinct.
    uint8_t disk[16];
    uint8_t root[16];
    uint8_t esp[16];
};

struct DiskLayoutOptions
{
    uint64_t disk_bytes;
    uint64_t esp_bytes = 64 * DiskMiB;
    DiskIdentities identities;
};

struct DiskLayout
{
    uint64_t disk_bytes;
    uint64_t total_sectors;
    uint64_t primary_entries_lba;
    uint64_t backup_entries_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint64_t root_first_lba;
    uint64_t root_last_lba;
    uint64_t esp_first_lba;
    uint64_t esp_last_lba;
    DiskIdentities identities;
};

struct DiskLayoutWorkspace
{
    uint8_t mbr[DiskSectorBytes];
    uint8_t primary_header[DiskSectorBytes];
    uint8_t backup_header[DiskSectorBytes];
    uint8_t entries[DiskTableBytes];
};

struct DiskLayoutSpan
{
    const uint8_t *data;
    DiskLayoutSize size;
};

struct DiskLayoutReadback
{
    // Supply separately retained buffers read at the PLAN's fixed positions:
    // 0, 1, 2, total_sectors-1 and total_sectors-33 respectively. Never follow
    // offsets obtained from an unvalidated disk header to fill these buffers.
    DiskLayoutSpan mbr;
    DiskLayoutSpan primary_header;
    DiskLayoutSpan primary_entries;
    DiskLayoutSpan backup_header;
    DiskLayoutSpan backup_entries;
};

// Pure memory operations, returning 0 or -EINVAL. Device and ESP sizes are
// whole MiB, with >=32 MiB root, >=64 MiB ESP and one MiB at each disk end.
// Plans/buffers remain unchanged on refusal. Check every return value; a
// failed plan never authorizes using a previous plan against another device.
int disk_layout_plan(const DiskLayoutOptions &, DiskLayout &);

// Caller retains an immutable plan and owns the disjoint 17920-byte workspace
// in heap/task-lifetime storage. Writes only these buffers, not a block device.
// Both headers are built independently; entries serialize once for both copies.
int disk_layout_build(const DiskLayout &, DiskLayoutWorkspace &);

// Strict readback of this generated profile, not a general GPT import parser.
// The expected immutable plan is revalidated. All five exact-sized, disjoint
// buffers must remain mapped/readable; pointer arithmetic checks do not prove
// mapping or the identity of the device used by the caller's I/O callbacks.
// Checks IEEE CRC32 (not descriptor CRC32C), MBR, all header fields/reserved
// bytes, complete tables, identities, ranges, attributes and fixed labels.
int disk_layout_validate(const DiskLayout &, const DiskLayoutReadback &);

} // namespace xjinstaller
