#pragma once
// SPDX-License-Identifier: Apache-2.0
// Bounded, freestanding partition editing. All offsets are device-relative.
#include <stdint.h>

namespace xjinstaller::partition {
constexpr unsigned SectorBytes     = 512;
constexpr unsigned MaximumEntries  = 128;
constexpr uint64_t MiB             = 1048576;
constexpr uint64_t Alignment       = MiB / SectorBytes;
constexpr uint64_t DefaultEspBytes = 100 * MiB;

enum class Scheme : uint32_t
{
    Unknown,
    Empty,
    Mbr,
    Gpt
};
enum class Filesystem : uint32_t
{
    Unknown,
    Fat32,
    Ext4,
    ExFat,
    Ntfs
};

struct Device
{
    void    *context;
    uint64_t bytes;
    bool (*read)(void *, uint64_t, void *, __SIZE_TYPE__);
    bool (*write)(void *, uint64_t, const void *, __SIZE_TYPE__);
    bool (*flush)(void *);
};

struct Entry
{
    uint64_t first, last, attributes;
    uint8_t  type[16], guid[16];
    uint16_t name[36];
    uint32_t number;
    bool     occupied, esp;
};

// Heap/caller-owned workspace, never put this object on a kernel stack.
// Retains every entry byte, header extension and padding for lossless editing.
struct Table
{
    Scheme   scheme;
    uint32_t count, entry_bytes;
    uint64_t bytes, first, last, primary_lba, backup_lba, fingerprint;
    uint8_t  mbr[512], primary[512], backup[512], entries[16384];
};

uint32_t   crc32(const void *, __SIZE_TYPE__);
bool       same(const Table &, const Table &);
int        load(const Device &, Table &);
int        initialize(uint64_t bytes, const uint8_t disk_guid[16], Table &);
int        entry(const Table &, unsigned number, Entry &);
bool       free_range(const Table &, uint64_t first, uint64_t last);
int        create(Table &, uint64_t first, uint64_t sectors, const uint8_t guid[16], Filesystem filesystem, bool esp,
                  unsigned &number);
int        erase(Table &, unsigned number);
int        set_filesystem(Table &, unsigned number, Filesystem);
// Shrinks/replaces only the selected entry, retaining every other partition.
// No filesystem data is moved. Caller must explicitly confirm data destruction.
int        split_for_install(Table &, unsigned number, const uint8_t esp_guid[16], unsigned &esp_number);
// A fresh comparison of ALL retained metadata precedes the first write.
// Backup first, then primary, with flush/readback gates. On failure there is
// no retry or rollback I/O; the device adapter must quarantine its write lease.
int        commit(const Device &, const Table &before, const Table &after, Table &scratch);
Filesystem identify(const uint8_t *first4096, __SIZE_TYPE__);
} // namespace xjinstaller::partition
