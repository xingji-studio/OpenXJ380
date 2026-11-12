#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original bounded installer package validation; no archive-library dependency.
#include <stdint.h>

namespace xjpak2 {

using Size = __SIZE_TYPE__;

enum class Kind : uint16_t
{
    Regular = 1,
    Directory = 2,
    Symlink = 3,
    Hardlink = 4
};
enum Metadata : unsigned
{
    Mode = 1,
    Owner = 2,
    Mtime = 4,
    Xattrs = 8
};
enum Error
{
    Invalid = -22,
    TooBig = -7,
    NoSpace = -28,
    Loop = -40,
    Unsupported = -95
};
struct Span
{
    const uint8_t *data;
    Size size;
};

// These upper bounds are a supported format profile, not a target capacity
// reservation. Each caller must lower them to its actual RAM/filesystem budget.
struct Limits
{
    uint64_t max_bytes = 8ULL << 30;
    uint64_t max_file_bytes = 4ULL << 30;
    uint64_t max_data_bytes = 8ULL << 30;
    uint64_t max_entries = 262144;
    uint64_t max_inodes = 262144;
    Size max_path = 4095;
    unsigned max_depth = 128;
    unsigned max_xattrs = 32;
    unsigned max_xattr_storage = 3968;
    bool allow_v1 = true;
    bool allow_trusted = false;
    bool allow_security = false;
};

struct HeaderInfo
{
    Size record_count = 0;
    uint64_t declared_inodes = 0;
    uint64_t declared_data_bytes = 0;
    bool legacy_v1 = false;
};

struct Record
{
    Span path, payload, xattrs;
    uint64_t object_id;
    Kind kind;
    unsigned metadata;
    uint32_t mode, uid, gid, mtime_nsec;
    int64_t mtime_seconds;
    uint16_t xattr_count;
};

struct Workspace
{
    Record *records;
    Size capacity;
};
struct Plan
{
    const Record *records = nullptr;
    Size count = 0;
    uint64_t inode_count = 0, data_bytes = 0;
    bool legacy_v1 = false;
    bool validated = false;
};

// Cheap header/count/declared-budget validation for bounded arena allocation.
// PAK2 header CRC is checked here; body CRC and record semantics are not.
// This never yields a valid Plan and must not authorize any target operation.
// The trusted caller supplies a mapped, immutable, non-user-writable package.
int inspect_header(const uint8_t *package, Size bytes, const Limits &, HeaderInfo &);

// Validate the entire package before the first target operation. There is no
// allocation, filesystem callback or target write interface in this API. Both
// immutable package and nonoverlapping caller arena must outlive every Plan use.
// Plan is cleared on entry and remains invalid after any failure; the arena may
// be modified. Validation does not establish destination lifetime/exclusivity,
// free-space reservation, authentication or atomic installation.
int validate(const uint8_t *package, Size bytes, const Limits &, Workspace, Plan &);
uint32_t crc32c(const uint8_t *, Size);

} // namespace xjpak2
