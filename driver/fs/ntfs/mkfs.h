#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original fresh NTFS 3.1 metadata encoder. No archive or formatter dependency.
#include <stdint.h>

namespace xjntfs::mkfs {
struct Device
{
    void    *context;
    uint64_t bytes;
    bool (*read)(void *, uint64_t, void *, __SIZE_TYPE__);
    bool (*write)(void *, uint64_t, const void *, __SIZE_TYPE__);
    bool (*flush)(void *);
};

struct Options
{
    uint64_t serial, time_seconds, partition_lba;
    uint32_t (*uppercase)(uint32_t) = nullptr;
};

struct Status
{
    bool io_started, io_failed, complete;
};

// Heap-owned and retained through completion or reboot after an I/O failure.
struct Workspace
{
    uint8_t records[64][1024], boot[8192], index[4096], definitions[4096];
    uint8_t buffer[65536], verify[65536];
};

// Quick format of an offline 512-byte logical-sector volume, 32 MiB..16 TiB.
// All requests stay in the supplied device window. Metadata is flushed and
// read back before the backup and primary BPBs are published, in that order.
// The first failed callback ends all I/O; there is no cleanup/retry/rollback.
int format(const Device &, const Options &, Workspace &, Status &);
} // namespace xjntfs::mkfs
