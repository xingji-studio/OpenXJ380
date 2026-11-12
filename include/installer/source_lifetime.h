#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <efi/installer_handoff.h>
#include <fs/vfs/devfs.h>

namespace installer
{
// Kernel-private static/heap storage, zero-initialized before its first call.
// The boot thread owns retention and rollback; user requests must never supply
// this object. A published or quarantined lifetime remains reserved to reboot.
// Unpublished retain/release is boot-serialized. After publication metadata is
// immutable; cross-thread validation and atomic quarantine are permitted.
struct SourceLifetime
{
    XJ_INSTALLER_SOURCE identity;
    DevfsBlockReference reference;
    BlockAccessToken lease;
    const void *owner;
    uint64_t owner_tid;
    unsigned state;
};

int retain_source(const XJ_INSTALLER_SOURCE &, SourceLifetime *);
// Cross-thread metadata/lifetime reauthentication; never transfers I/O rights.
int validate_source(const SourceLifetime &);
// Publish only source retention. This does not make an installer session Ready.
int publish_source(SourceLifetime *);
// Only the original boot TCB/TID may undo an unpublished clean retention.
// Cleanup errors quarantine the entire remaining lifetime instead of retrying.
int release_source_boot_only(SourceLifetime *);
void quarantine_source(SourceLifetime *);
vfs_node_t source_node(const SourceLifetime &);
} // namespace installer
