#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

// Exclusive boot-thread staging followed by explicit managed publication.
// These objects never enter legacy tmpfs_entries or ordinary tmpfs callbacks.
// Sealing is validation; publish is a separate boot-only namespace commit.
// Retain and serialize this opaque handle until discard or reboot; copying it
// does not create another owner. File views/snapshots stop at publication.
struct TmpfsPrivateTree
{
    void *state;
};

struct TmpfsPrivateLimits
{
    uint64_t max_entries, max_data_bytes;
};

struct TmpfsPrivateSnapshot
{
    uint64_t entries, data_bytes;
    bool sealed, quarantined;
};

struct TmpfsPrivateFile
{
    const uint8_t *data;
    uint64_t bytes;
    uint32_t uid, gid;
    uint16_t mode;
    bool directory;
};

int tmpfs_private_begin(TmpfsPrivateTree *, const TmpfsPrivateLimits &);
int tmpfs_private_add(TmpfsPrivateTree *, const char *path, bool directory, const void *data,
                      uint64_t bytes, uint16_t mode, uint32_t uid, uint32_t gid);
int tmpfs_private_file(TmpfsPrivateTree *, const char *path, TmpfsPrivateFile *);
int tmpfs_private_seal(TmpfsPrivateTree *);
int tmpfs_private_snapshot(TmpfsPrivateTree *, TmpfsPrivateSnapshot *);
// Refuse changed/external references and retain the complete tree on failure.
// There is deliberately no raw-vnode export in this interface.
int tmpfs_private_discard(TmpfsPrivateTree *);
// Boot-owner-only managed publication. The whole deep tree remains bounded;
// registered runtime callbacks allocate/free only outside metadata spin locks.
// After success discard is forbidden, rootdir/dev identities are preserved,
// and ordinary linked nodes own a backend pin that rejects cache retirement.
int tmpfs_private_publish(TmpfsPrivateTree *);
// Boot owner only, before runtime launch: create/verify an empty directory and
// transfer its cache-only vnode to a subsequent pseudo-filesystem setup.
// Busy/open/nonempty nodes and existing mounts are preserved and rejected.
int tmpfs_private_mountpoint(TmpfsPrivateTree *, const char *path);
