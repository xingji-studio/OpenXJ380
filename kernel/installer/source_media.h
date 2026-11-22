#pragma once
// SPDX-License-Identifier: Apache-2.0
// Private, bounded file access under the installer's existing whole-disk lease.
#include "../../driver/fs/iso9660/native.h"
#include <installer/stream_index.h>

namespace installer {
class SourceMedia {
    struct Extent
    {
        uint64_t logical, physical, bytes;
    };
    struct FatFile
    {
        uint64_t bytes;
        unsigned first, count;
    };
    xjiso::Reader    input        = nullptr;
    void            *context      = nullptr;
    uint64_t         device_bytes = 0, volume_offset = 0, volume_bytes = 0;
    uint64_t         fat_start = 0, data_start = 0, fat_bytes = 0;
    uint32_t         cluster_bytes = 0, clusters = 0, root = 0, sector_bytes = 0;
    unsigned         fats = 0, active_fat = 0, extent_count = 0;
    uint64_t         cached_fat = 0;
    bool             mirrored = false, cache_valid = false, optical = false;
    const char      *stage = "uninitialized";
    uint8_t          sector[4096], fat[4096], mirror[4096];
    Extent           extents[xjinstaller::StreamExtentMaximum];
    unsigned         order[xjinstaller::StreamExtentMaximum];
    FatFile          fat_files[4] = {};
    xjiso::Volume    iso;
    xjiso::Workspace work;
    xjiso::Inode     iso_files[4] = {};

    int raw(uint64_t, void *, xjiso::Size);
    int fat_boot();
    int fat_next(uint32_t, uint32_t &);
    int fat_find(uint32_t, const char *, bool, uint32_t &, uint64_t &);
    int fat_lookup(const char *, uint32_t &, uint64_t &);
    int fat_map(unsigned, uint32_t, uint64_t);
    int fat_disjoint();
    int lookup(unsigned, uint64_t);
    int verify_catalog(const xjinstaller::StreamIndex &);

  public:
    // FAT is confined to the actual loaded EFI volume. If its payload is
    // absent, ISO lookup is confined to that same retained physical device.
    int  open(xjiso::Reader, void *, uint64_t device_bytes, uint64_t volume_offset, uint64_t volume_bytes,
              const xjinstaller::StreamIndex &);
    int  read(unsigned file, uint64_t offset, void *, xjiso::Size);
    bool is_iso() const
    {
        return optical;
    }

    const char *diagnostic() const
    {
        return stage;
    }
};
} // namespace installer
