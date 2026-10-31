#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original read-only fresh native-ext4 package readback; no archive dependency.
#include <installer/package.h>

namespace xjext4 { class Volume; }

namespace xjinstaller {

using Size = __SIZE_TYPE__;

struct Ext4VerifyDevice
{
    bool (*read)(void *, uint64_t, void *, Size);
    void *context;
    uint64_t bytes;
};

struct Ext4VerifyEntry
{
    uint32_t inode, parent_index, references, subdirectories, seen, order;
};

struct Ext4VerifyStatus
{
    uint64_t data_bytes_verified = 0; // Canonical regular bytes; equals Plan::data_bytes on success.
    uint32_t records_verified = 0, names_verified = 0, groups_verified = 0;
    int error = 0;
    bool complete = false, io_failed = false;
};

// Zero-initialize and retain every object for the complete synchronous call.
// Records/map/Volume are caller-owned heap/task-lifetime objects; they and this
// workspace must not overlap each other or the immutable mapped package. The
// first four fields are inputs; remaining fields are private persistent state.
// An existing record arena may be reused after import is completely closed.
struct Ext4VerifyWorkspace
{
    xjpak2::Record *records;
    Ext4VerifyEntry *entries;
    Size capacity;
    xjext4::Volume *volume;

    xjpak2::Plan plan;
    Ext4VerifyDevice io;
    Ext4VerifyStatus status;
    uint8_t uuid[16], super[1024], scratch[4096], descriptor[64];
    char name[256], path[4096];
    uint32_t current_directory, current_inode, current_parent, current_record;
    uint32_t dot_entries, xattr_entries;
    bool missing_lost_found, lost_found_seen, used, stopped;
};

// Revalidates complete PAK2 and independently rebuilds ordinal/parent maps
// before any target read. Reads each name by parent inode without following
// symlinks, checks exact data/metadata/user-EAs/hardlink identity and enumerates
// directories to reject extra/duplicate names. Only the omitted empty factory
// lost+found inode 11 is allowed. Checks expected UUID, native factory profile,
// clean journal/orphan state, allocation checksums/counters and exact inode set.
// This is a fresh-import readback gate, not recovery or a general fsck/block
// ownership audit. The caller owns device exclusivity/generation and real-flush
// policy; this API has no write/flush callback and publishes no boot metadata.
// One-shot after target reads start; first backend failure stops all reads.
int ext4_verify_package(const uint8_t *package, Size package_bytes, const xjpak2::Limits &,
                         const Ext4VerifyDevice &, const uint8_t expected_uuid[16],
                         Ext4VerifyWorkspace &, Ext4VerifyStatus *status = nullptr);

} // namespace xjinstaller
