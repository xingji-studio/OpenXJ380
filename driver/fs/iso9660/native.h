#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original, allocation-free ISO 9660 reader. Only on-disk format facts are used.
#include "rock_ridge.h"

namespace xjiso {
constexpr unsigned Sector            = 2048;
constexpr unsigned MaxSections       = 1024;
constexpr unsigned MaxEntries        = 65536;
constexpr uint64_t MaxDirectoryBytes = 64ULL * 1024 * 1024;

struct Inode
{
    uint64_t identifier, size, data, records_end, modified, accessed, changed, parent;
    uint32_t first_bytes, mode, uid, gid, links, serial, major, minor;
    uint16_t sections, version;
    bool     directory, symlink, unsupported;
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
    uint64_t directory, parent;
};

struct Workspace
{
    uint8_t       descriptor[Sector];
    uint8_t       record[256], original_name[256];
    SuspWorkspace susp;
    RockRidgeInfo ridge;
};

class Volume {
    Reader         reader     = nullptr;
    void          *context    = nullptr;
    uint64_t       bytes      = 0;
    Inode          root_inode = {};
    bool           unicode    = false;
    RockRidgeState ridge      = {};

    int read_exact(uint64_t offset, void *output, Size length) const;
    int record_at(uint64_t &position, uint64_t end, Inode &inode, Workspace &work) const;
    int decode_name(const uint8_t *name, unsigned length, bool directory, Entry &entry) const;
    int apply_ridge(Inode &inode, Workspace &work, bool root_record = false) const;
    int relocate(Inode &inode, uint32_t block, uint64_t parent, Workspace &work) const;

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
    bool rock_ridge() const
    {
        return ridge.enabled;
    }
    bool names_equal(const char *a, const char *b) const;
    int  cursor(const Inode &directory, Cursor &cursor) const;
    // Returns 1 for an entry, 0 at EOF, or a negative errno. A complete Level 3
    // section chain is validated before any entry is exposed to the caller.
    int  next(Cursor &cursor, Entry &entry, Workspace &work) const;
    int  lookup(const Inode &directory, const char *name, Entry &entry, Workspace &work) const;
    int  read(const Inode &inode, uint64_t offset, void *output, Size length, Size &completed, Workspace &work) const;
    int  readlink(const Inode &inode, void *output, Size capacity, Size &completed, Workspace &work) const;
};
} // namespace xjiso
