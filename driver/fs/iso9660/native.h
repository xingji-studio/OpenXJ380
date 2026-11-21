#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original, allocation-free ISO 9660 reader. Only on-disk format facts are used.
#include <stdint.h>

namespace xjiso {
using Size                           = __SIZE_TYPE__;
using Reader                         = bool (*)(void *, uint64_t, void *, Size);
constexpr unsigned Sector            = 2048;
constexpr unsigned MaxSections       = 1024;
constexpr unsigned MaxEntries        = 65536;
constexpr uint64_t MaxDirectoryBytes = 64ULL * 1024 * 1024;

struct Inode
{
    uint64_t identifier, size, data, records_end, modified;
    uint32_t first_bytes;
    uint16_t sections, version;
    bool     directory;
};

struct Entry
{
    Inode inode;
    char  name[256];
};

struct Cursor
{
    uint64_t position, end;
    unsigned entries;
};

struct Workspace
{
    uint8_t descriptor[Sector];
    uint8_t record[256], original_name[256];
};

class Volume {
    Reader   reader     = nullptr;
    void    *context    = nullptr;
    uint64_t bytes      = 0;
    Inode    root_inode = {};
    bool     unicode    = false;

    int read_exact(uint64_t offset, void *output, Size length) const;
    int record_at(uint64_t &position, uint64_t end, Inode &inode, Workspace &work) const;
    int decode_name(const uint8_t *name, unsigned length, bool directory, Entry &entry) const;

  public:
    int   mount(Reader input, void *opaque, uint64_t device_bytes, Workspace &work);
    Inode root() const
    {
        return root_inode;
    }
    uint64_t volume_bytes() const
    {
        return bytes;
    }
    bool joliet() const
    {
        return unicode;
    }
    bool names_equal(const char *a, const char *b) const;
    int  cursor(const Inode &directory, Cursor &cursor) const;
    // Returns 1 for an entry, 0 at EOF, or a negative errno. A complete Level 3
    // section chain is validated before any entry is exposed to the caller.
    int  next(Cursor &cursor, Entry &entry, Workspace &work) const;
    int  lookup(const Inode &directory, const char *name, Entry &entry, Workspace &work) const;
    int  read(const Inode &inode, uint64_t offset, void *output, Size length, Size &completed, Workspace &work) const;
};
} // namespace xjiso
