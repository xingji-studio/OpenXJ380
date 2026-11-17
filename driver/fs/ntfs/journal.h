#pragma once
// SPDX-License-Identifier: Apache-2.0 AND MIT
// Native LFS/NTFS transaction profile. Layout/replay reference: DiscUtils,
// pinned and attributed in SOURCE.json and DISCUTILS_NOTICE.txt.
#include "native.h"

namespace xjntfs {

constexpr unsigned MaxMetadataImages = 64;
constexpr unsigned MaxJournalName    = 16;
constexpr unsigned MaxJournalPages   = 160;
constexpr unsigned JournalPage       = 4096;
constexpr unsigned FirstJournalPage  = 4;

struct WriteDevice
{
    Reader   read;
    Writer   write;
    Flusher  flush;
    void    *context;
    uint64_t bytes;
    unsigned logical_sector = 0; // Kernel supplies authenticated device geometry; image adapters may omit it.
};

enum class MetadataKind : uint8_t
{
    FileRecord,
    IndexBuffer,
    Raw
};

struct MetadataImage
{
    MetadataKind kind;
    uint64_t     file_reference, vcn, lcn, lsn;
    uint32_t     attribute_type;
    uint16_t     within, length, target;
    bool         named_index;
    unsigned     name_length;
    uint16_t     name[MaxJournalName];
    uint8_t      before[JournalPage], after[JournalPage], packed[JournalPage];
};

struct JournalAttribute
{
    uint64_t reference;
    uint32_t type;
    bool     named_index;
    unsigned name_length;
    uint16_t name[MaxJournalName];
};

// Caller-owned task/heap storage. It must survive uncertain I/O/owner death.
struct JournalWorkspace
{
    MetadataImage    images[MaxMetadataImages];
    JournalAttribute attributes[MaxMetadataImages];
    unsigned         count, attribute_count;
    uint8_t          page[JournalPage], sector[JournalPage], wire[JournalPage * 3];
    uint8_t          restarts[2][JournalPage];
};

using JournalValidator = int (*)(void *, JournalWorkspace &);
struct RecoveryWorkspace;
class WindowsRecovery;

// Bounded native LFS 1.1 records, NTFS client 1.0. It emits full FILE/INDX
// images and nonresident bitmap updates, with redo and undo, then COMMIT/FORGET.
// Home writes start only after the complete log and a dirty restart are durable.
// Foreign recovery analyzes bounded LFS 1.1/2.0 logs in memory and seals the
// result into a native committed snapshot before any home write. Unknown or
// unresolved transactions fail closed; see NTFS_WRITE_DESIGN.md.
class Journal {
  public:
    int  open(const WriteDevice &device, JournalWorkspace &workspace, RecoveryWorkspace *recovery = nullptr);
    bool needs_recovery() const
    {
        return pending_;
    }
    bool failed() const
    {
        return failed_;
    }
    void begin();
    int  add(MetadataKind kind, uint64_t file_reference, uint32_t type, bool named_index, uint64_t vcn, uint64_t lcn,
             unsigned within, unsigned length, const uint8_t *before, MetadataImage *&image);
    int  commit(JournalValidator validate, void *context);
    int  recover(JournalValidator validate, void *context);
    int  sync();
    uint64_t physical(const MetadataImage &image) const
    {
        return image.lcn * JournalPage + image.within;
    }
    bool     overlaps_log(uint64_t offset, uint64_t length) const;
    bool     overlaps_mirror(uint64_t offset, uint64_t length) const;
    uint64_t mft_lcn() const
    {
        return mft_lcn_;
    }
    uint64_t mirror_lcn() const
    {
        return mirror_lcn_;
    }
    uint64_t volume_bytes() const
    {
        return bytes_;
    }

  private:
    friend class WindowsRecovery;
    friend class WriteSession;
    WriteDevice        device_;
    JournalWorkspace  *work_;
    Stream             log_;
    uint64_t           bytes_, clusters_, mft_lcn_, mirror_lcn_, file_size_, last_lsn_, epoch_, previous_;
    uint32_t           revision_;
    unsigned           sector_, record_, sequence_bits_, selected_, next_page_, checkpoint_page_;
    uint16_t           client_sequence_;
    bool               opened_, failed_, pending_, fresh_, repair_, foreign_, native_profile_;
    RecoveryWorkspace *recovery_;
    int                read(uint64_t offset, void *output, Size length);
    int                write(uint64_t offset, const void *input, Size length);
    int                log_read(uint64_t offset, void *output, Size length);
    int                log_write(uint64_t offset, const void *input, Size length);
    int                bootstrap();
    int                read_restart(unsigned slot, bool &valid);
    int                scan();
    int                make_restart(bool clean, uint8_t *page);
    int                publish(bool clean);
    int                apply();
    int                pack_images();
    uint64_t           make_lsn(unsigned page) const;
    int                append(unsigned type, uint16_t redo, uint16_t undo, unsigned target, const MetadataImage *image,
                              const uint8_t *redo_data, unsigned redo_size, const uint8_t *undo_data, unsigned undo_size,
                              uint64_t &lsn);
    int                read_record(uint64_t lsn, unsigned &type, unsigned &next_page);
    int                open_native();
    int                emit(JournalValidator validate, void *context, bool importing);
};

} // namespace xjntfs
