#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original offline ext4 formatter; all serialized fields use explicit byte order.
#include <stdint.h>

namespace xjext4 {

using Size = __SIZE_TYPE__;

struct Ext4FormatDevice
{
    void *context;
    bool (*read)(void *, uint64_t offset, void *, Size bytes);
    bool (*write)(void *, uint64_t offset, const void *, Size bytes);
    bool (*flush)(void *);
    uint64_t bytes;
};

struct FormatOptions
{
    // Fully initialize this value, including any unused label bytes. UUID
    // uniqueness/randomness belongs to the caller; an all-zero UUID is invalid.
    uint8_t uuid[16];
    uint64_t time_seconds;
    uint32_t journal_mib;
    char label[16];
};

struct FormatGeometry
{
    uint32_t blocks, groups, gdt_blocks, inodes, free_blocks;
    uint32_t block_bitmap[128], inode_bitmap[128], inode_table[128];
    uint32_t journal_first, journal_blocks, root_block, lost_found_block;
    // Reset on entry, including invalid arguments. These report completed or
    // attempted phases, never a claim that a failing write could not persist.
    bool metadata_durable;    // The complete metadata flush succeeded.
    bool publication_started; // Entered the VALID backup/publication phase.
    bool primary_attempted;  // The final VALID primary write was attempted.
};

// Optional caller-owned arena for volumes with more than 128 groups. The
// formatter never allocates or frees it. Retain it throughout planning and
// formatting, including a failed I/O lifetime. Small volumes use the original
// inline storage and do not inspect this optional arena.
struct FormatStorage
{
    void *data;
    Size bytes;
};

struct FormatGroupLocation
{
    uint32_t block_bitmap, inode_bitmap, inode_table;
};

// Caller-owned heap storage (537600 bytes), never a kernel/interrupt stack.
// No host/kernel allocator or third-party archive is needed.
// allocated remains scratch storage for existing factory callers. Geometry
// planning no longer uses a bit per physical filesystem block.
struct FormatWorkspace
{
    uint8_t allocated[524288];
    uint8_t descriptors[8192];
    uint8_t block[4096];
    uint8_t super[1024];
};

// Pure geometry/storage preflight. Zero means the inline workspace suffices.
// The current serialized formatter profile uses fewer than 2^32 4-KiB blocks.
// A nonzero arena must be four-byte aligned and disjoint from other arguments.
int format_storage_size(uint64_t device_bytes, Size &required);

// Pure access to a successfully planned group's physical metadata locations.
// Large geometries require the same arena supplied to plan_format/format.
int format_group_location(const FormatGeometry &, const FormatStorage *, uint32_t group,
                          FormatGroupLocation &);

// Pure preflight: validate options and compute the exact factory layout/free
// counts without invoking any device callback. Resets geometry phase flags and
// modifies the caller workspace. Callback presence is still validated. A plan
// does not reserve or pin a device: retain exclusive ownership through format.
int plan_format(const Ext4FormatDevice &, const FormatOptions &, FormatWorkspace &, FormatGeometry &,
                const FormatStorage *storage = nullptr);

// Exclusive offline ownership is mandatory. Invalid arguments/geometry cause
// zero I/O. Stop at the first backend error: do not reuse an ambiguous lifetime.
// A flush preflight follows planning and precedes every destructive write.
// VALID supers are written only after complete metadata durability. A failed
// final write/flush can still persist VALID, but its metadata is already whole.
// The installer must publish partition/root binding only after success.
// This initializes the new profile's metadata, not every historic filesystem
// signature or unallocated data block. It is not a secure erase.
int format(const Ext4FormatDevice &, const FormatOptions &, FormatWorkspace &, FormatGeometry &,
           const FormatStorage *storage = nullptr);

} // namespace xjext4
