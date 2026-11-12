#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

struct vfs_node;
using BlockAccessToken = uint64_t;

struct BlockExtent
{
    uint32_t device;
    uint32_t physical_device;
    uint64_t generation;
    uint64_t physical_generation;
    uint64_t start;
    uint64_t bytes;
};

// Synchronous borrow of a live ordinary whole-device exclusive lease. The
// caller retains the real devfs node, parent token and immutable payload for
// the entire same-TCB call, or until reboot after quarantine/abandonment. This
// descriptor neither owns nor extends those lifetimes and is not a user ABI.
struct BlockAccessWindow
{
    struct vfs_node *node;
    BlockExtent whole;
    BlockAccessToken lease;
    uint64_t offset, bytes;
};

// All tables are bounded and fail closed. Tokens name static slots, never
// caller stack storage. Device registration must precede devfs publication.
int block_access_register_whole(uint32_t device, uint64_t bytes, bool flush_supported = false);
int block_access_register_slice(uint32_t device, uint32_t parent, uint64_t offset, uint64_t bytes);
int block_access_publish_device(uint32_t device, uint64_t generation);
// Nonblocking recursive publication/deletion claim; no spin lock remains held
// across device-manager allocation or devfs calls. Abandonment fails closed.
int block_access_management_begin(BlockAccessToken *token);
void block_access_management_end(BlockAccessToken token);
int block_access_unregister(uint32_t device);
uint64_t block_access_generation(uint32_t device);
int block_access_resolve(uint32_t device, uint64_t expected_generation, BlockExtent *extent);
// devfs authenticates the handle's captured registration generation.
int block_access_resolve_node(struct vfs_node *node, BlockExtent *extent);
// Immutable driver declaration, authenticated through the physical generation.
// A no-op or unknown ioctl returning zero is not a durability capability.
int block_access_require_flush(const BlockExtent &extent);

// Mounted leases pin a filesystem from its first backend I/O through final
// unmount, rejecting every overlapping mount alias while allowing ordinary I/O.
// Exclusive leases require a current task and reject overlap
// with any lease or in-flight I/O. The owning task alone may perform I/O inside
// its exclusive range; its monotonic TID also guards against TCB address reuse.
int block_access_claim(const BlockExtent &extent, bool exclusive, BlockAccessToken *token);
int block_access_release(BlockAccessToken token);
void block_access_poison(BlockAccessToken token);

// Authenticate a nonempty 512-byte-aligned window without taking a second
// lease. The token must name this exact current whole-device registration and
// belong to the current nonterminal TCB/TID; scan/shared/poisoned leases fail.
// This validates registry authority only. A backend must independently resolve
// its retained devfs node and match every captured whole-extent field before I/O.
int block_access_validate_owned_exclusive(BlockAccessToken token, const BlockExtent &whole,
                                          uint64_t offset, uint64_t bytes);

// Read-only lifetime query for a retained boot source. This neither grants I/O
// to the caller nor transfers the original owner's token. The exact ordinary
// whole-device exclusive lease and its nonterminal owner must still be live.
int block_access_validate_retained_exclusive(BlockAccessToken token, const BlockExtent &whole);

// expected_generation=0 is for current device-manager calls. Cached devfs
// handles must pass their nonzero captured generation. Zero-byte I/O is valid.
int block_access_io_begin(uint32_t device, uint64_t expected_generation, uint64_t offset, uint64_t bytes,
                          BlockAccessToken *token);
int block_access_flush_begin(uint32_t device, uint64_t expected_generation, BlockAccessToken *token);
void block_access_io_end(BlockAccessToken token);
// Call only after scheduler quiescence and before freeing/reusing the TCB.
// Abandoned tickets and exclusive leases become permanent poisoned tombstones.
void block_access_abandon_owner(const void *task, uint64_t tid);
// Reaper task context: terminal threads in a still-live process also abandon
// scopes. Every TCB free must still call abandon_owner before recycling it.
void block_access_reap_abandoned();

// A scan pins the complete physical disk exclusively. Slice publication from
// this owner is permitted; scans/deletion/re-registration otherwise cannot
// cross active leases. Release before automounting discovered partitions.
int block_access_scan_begin(uint32_t device, BlockAccessToken *token);

// Stack RAII owns only an opaque value; the lifetime record stays static even
// if a forcibly terminated caller never executes this destructor.
class BlockAccessIo
{
    BlockAccessToken token = 0;
    int result;
public:
    BlockAccessIo(uint32_t device, uint64_t generation, uint64_t offset, uint64_t bytes, bool enabled = true)
        : result(enabled ? block_access_io_begin(device, generation, offset, bytes, &token) : 0) {}
    ~BlockAccessIo() { if (token) block_access_io_end(token); }
    bool ready() const { return result == 0; }
    int error() const { return result; }
    BlockAccessIo(const BlockAccessIo &) = delete;
    BlockAccessIo &operator=(const BlockAccessIo &) = delete;
};
