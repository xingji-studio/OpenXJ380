#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

// Bounded NTFS 3.1 reader. Explicit metadata transactions live in write.h.
// A caller-owned workspace keeps large scratch buffers off the stack;
// callers serialize operations using the same workspace and device lifetime.
namespace xjntfs {

using Size                             = __SIZE_TYPE__;
using Reader                           = bool (*)(void *, uint64_t, void *, Size);
using Writer                           = bool (*)(void *, uint64_t, const void *, Size);
using Flusher                          = bool (*)(void *);
constexpr unsigned MaxWriteRecords     = 65536;
constexpr unsigned MaxOverwrite        = 1024 * 1024;
constexpr unsigned MaxWriteComparisons = 4 * 1024 * 1024;
constexpr unsigned MaxRecord           = 4096;
constexpr unsigned MaxIndex            = 16384;
constexpr unsigned MaxRuns             = 1024;
constexpr unsigned MaxDepth            = 16;
constexpr unsigned MaxIndexNodes       = 4096;
constexpr unsigned MaxEntries          = 65536;
constexpr unsigned MaxName             = 1021;
constexpr unsigned MaxAttributeList    = 65536;
constexpr unsigned MaxAttributeEntries = 1024;
constexpr unsigned MaxLogPage          = 65536;

struct Run
{
    uint64_t vcn, length;
    int64_t  lcn; // -1 denotes an explicitly encoded sparse run.
};

struct Stream
{
    uint64_t size, initialized, allocated;
    unsigned count, resident_length;
    uint16_t flags;
    bool     resident;
    Run      runs[MaxRuns];
    uint8_t  value[MaxRecord];
};

struct Inode
{
    uint64_t reference, size, allocated;
    uint64_t created, modified, accessed; // NTFS 100 ns units since 1601.
    uint32_t attributes;
    uint16_t links;
    bool     directory, unsupported, case_sensitive, nonresident;
};

struct DirectoryEntry
{
    uint64_t reference, size;
    uint32_t attributes;
    bool     directory;
    uint8_t  name_space;
    char     name[MaxName];
};

// Visitors must not reenter this Volume/workspace. Zero continues, a positive
// result stops successfully, and a negative result aborts the enumeration.
using DirectoryVisitor = int (*)(void *, const DirectoryEntry &);

struct Workspace
{
    uint8_t record[MaxRecord];
    uint8_t root_index[MaxRecord];
    uint8_t index[MaxIndex];
    uint8_t extension[MaxRecord], attribute_list[MaxAttributeList], log_page[MaxLogPage];
    Stream  stream, allocation, bitmap, list_stream;
    struct AttributeIdentity
    {
        uint64_t reference;
        uint16_t instance;
    } seen[MaxAttributeEntries], write_seen[MaxAttributeEntries];
    uint64_t visible[MaxEntries];
    struct Frame
    {
        uint64_t vcn;
        unsigned offset;
        bool     child_done;
    } path[MaxDepth];
    uint64_t visited[MaxIndexNodes];
};

class Volume {
  public:
    int  mount(Reader reader, void *context, uint64_t bytes, Workspace &work);
    int  enable_overwrite(Writer writer, Flusher flusher, Workspace &work);
    int  overwrite(const Inode &inode, uint64_t offset, const void *input, Size length, Size &completed,
                   Workspace &work);
    int  sync();
    bool writable() const
    {
        return writer_ && flusher_ && !io_failed_;
    }
    int root(Inode &inode, Workspace &work) const;
    int read_inode(uint64_t reference, Inode &inode, Workspace &work) const;
    int read(const Inode &inode, uint64_t offset, void *output, Size length, Size &completed, Workspace &work) const;
    int list_directory(const Inode &inode, DirectoryVisitor visitor, void *context, Workspace &work) const;
    int lookup(const Inode &directory, const char *name, Inode &inode, Workspace &work) const;
    int lookup_entry(const Inode &directory, const char *name, DirectoryEntry &entry, Workspace &work) const;
    int names_equal(const char *left, const char *right, bool sensitive, bool &match) const;
    uint32_t cluster_size() const
    {
        return cluster_;
    }
    uint64_t bytes() const
    {
        return bytes_;
    }

  private:
    friend class WriteSession;
    int          mount_internal(Reader reader, void *context, uint64_t bytes, Workspace &work, bool staged);
    Reader       reader_;
    Writer       writer_;
    Flusher      flusher_;
    void        *context_;
    uint64_t     bytes_, clusters_, mirror_lcn_;
    uint32_t     cluster_, record_, index_;
    uint16_t     sector_;
    bool         ready_;
    mutable bool io_failed_;
    Stream       mft_, mft_bitmap_, cluster_bitmap_;
    uint16_t     upcase_[65536];

    int fetch(uint64_t offset, void *output, Size length) const;
    int stream_read(const Stream &stream, uint64_t offset, void *output, Size length) const;
    int record_read(uint64_t number, uint16_t sequence, uint8_t *buffer, uint64_t owner = 0) const;
    using AttributeVisitor = int (*)(void *, const uint8_t *, unsigned, uint64_t);
    int visit_attributes(const uint8_t *base, uint64_t reference, uint32_t filter, bool i30, AttributeVisitor visitor,
                         void *context, Workspace &work) const;
    int stream_from_record(const uint8_t *record, uint32_t type, bool i30, Stream &stream, Workspace &work,
                           uint64_t reference = 0, bool bootstrap = false) const;
    int inode_from_record(const uint8_t *record, uint64_t number, Inode &inode, Workspace &work) const;
    int enumerate_directory(const Inode &inode, DirectoryVisitor visitor, void *context, Workspace &work) const;
    int check_log(Workspace &work) const;
};

} // namespace xjntfs
