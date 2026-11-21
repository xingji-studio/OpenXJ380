#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original bounded decoder of the IEEE P1281/P1282 on-disk records.
#include <stdint.h>

namespace xjiso {
using Size                              = __SIZE_TYPE__;
using Reader                            = bool (*)(void *, uint64_t, void *, Size);
constexpr unsigned MaxLinkBytes         = 4095;
constexpr unsigned MaxContinuationAreas = 32;
constexpr unsigned MaxSystemUseBytes    = 65536;

struct RockRidgeState
{
    unsigned skip, sequence, extensions, root_skip;
    bool     enabled;
};

struct RockRidgeInfo
{
    uint32_t mode, uid, gid, links, serial, child, parent, major, minor;
    uint64_t modified, accessed, changed, created;
    unsigned name_size, target_size;
    char     name[256], target[MaxLinkBytes + 1];
    bool     attributes, name_present, symlink, relocated, unsupported;
    bool     have_modified, have_accessed, have_changed, have_created;
};

struct SuspWorkspace
{
    uint8_t  entry[256];
    uint64_t visited[MaxContinuationAreas];
};

uint64_t iso_recording_time(const uint8_t *time);
int      rock_ridge_probe(Reader reader, void *context, uint64_t bytes, const uint8_t *record, RockRidgeState &state,
                          SuspWorkspace &work);
int rock_ridge_parse(Reader reader, void *context, uint64_t bytes, const uint8_t *record, const RockRidgeState &state,
                     RockRidgeInfo &info, SuspWorkspace &work, bool root_record = false);
} // namespace xjiso
