#pragma once
// SPDX-License-Identifier: Apache-2.0
// Trusted fresh native-ext4 importer; no pathname/VFS or archive dependency.
#include <installer/package.h>

namespace xjext4 {
struct WriteDevice;
struct WriteWorkspace;
struct WriteStorage;
class WriteSession;
class Volume;
}

namespace xjinstaller {

using Size = __SIZE_TYPE__;

struct Ext4ImportEntry
{
    uint32_t inode;
    uint32_t parent_index;
    uint32_t references;
    uint32_t children;
    uint32_t subdirectories;
};

struct Ext4ImportRequirements
{
    uint64_t blocks = 0;
    uint32_t new_inodes = 0;
    uint32_t canonical_inodes = 0;
    uint32_t names = 0;
    uint32_t block_size = 4096;
};

enum class Ext4ImportPhase : uint32_t
{
    Empty, Prepared, Preflight, Opening, Directories, RegularData,
    Symlinks, Hardlinks, Metadata, Times, Closing, Complete
};

struct Ext4ImportStatus
{
    Ext4ImportPhase phase = Ext4ImportPhase::Empty;
    int error = 0;
    uint64_t data_bytes_committed = 0;
    uint32_t last_record = 0;
    uint32_t inodes_created = 0;
    uint32_t names_created = 0;
    bool prepared = false;
    bool preflight_complete = false;
    bool writer_started = false;
    bool writer_open = false;
    bool complete = false;
    bool io_failed = false;
};

struct Ext4ImportIO
{
    bool (*read)(void *, uint64_t, void *, Size);
    bool (*write)(void *, uint64_t, const void *, Size);
    bool (*flush)(void *);
    void *context;
    uint64_t bytes;
};

// Caller owns every object/arena and zero-initializes this workspace. None may
// overlap the immutable package or each other. The first six fields are inputs;
// the rest are persistent state, not kernel-stack scratch. The Volume/session/
// WriteWorkspace must also be heap/task-lifetime objects, not stack objects.
struct Ext4ImportWorkspace
{
    xjpak2::Record *records;
    Ext4ImportEntry *entries;
    Size capacity;
    xjext4::WriteSession *writer;
    xjext4::WriteWorkspace *writer_workspace;
    xjext4::Volume *read_volume;

    const uint8_t *package;
    Size package_bytes;
    xjpak2::Limits limits;
    xjpak2::Plan plan;
    Ext4ImportRequirements requirements;
    Ext4ImportStatus status;
    Ext4ImportIO io;
    uint8_t uuid[16];
    uint8_t super[1024];
    uint8_t scratch[4096];
    char name[256];
    char attribute_name[256];
    bool used;
    bool stopped;
};

// Pure memory preparation: PAK2 only, complete package, default user.* policy.
// Records and ordinal maps are supplied by the caller. Native factory
// lost+found inode 11 is retained; an explicit /lost+found must be a DIRECTORY
// and reuses it. Symlink user xattrs are rejected before any target operation.
int ext4_import_prepare(const uint8_t *, Size, const xjpak2::Limits &,
                        Ext4ImportWorkspace &, Ext4ImportRequirements &);

// Conservative fresh-volume upper budget, not a reservation or space guarantee.
// The native formatter's free blocks and inodes-11 may be supplied before format.
int ext4_import_check_budget(const Ext4ImportRequirements &, uint32_t block_size,
                             uint64_t free_blocks, uint32_t free_inodes);

// Caller retains an exclusive pinned offline device and immutable package.
// Requires a freshly formatted native 4KiB/256-byte-inode profile; no recovery,
// repair, component selection, existing-tree overwrite or boot publication.
// Revalidates package/maps and fresh target before WS's first flush/write, and
// repeats fresh preflight in InitialValidator. Uses inode-relative mutations.
// One-shot: after any backend failure no more callbacks run. Every error after
// writer start leaves its lifetime retained, without close/retry/RECOVER clear.
// Success alone closes the writer. Partial committed trees are not rolled back.
int ext4_import_apply_fresh(const xjext4::WriteDevice &, uint64_t operation_seconds,
                            Ext4ImportWorkspace &, Ext4ImportStatus &,
                            const xjext4::WriteStorage *storage = nullptr);

} // namespace xjinstaller
