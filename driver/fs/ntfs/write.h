#pragma once
// SPDX-License-Identifier: Apache-2.0
#include "journal.h"
#include "recovery.h"

namespace xjntfs {
constexpr unsigned MaxAllocationBitmap = 1024 * 1024;
constexpr unsigned MaxDirectoryKeys    = 64;
constexpr unsigned MaxCowClusters      = 4096; // A bounded 16 MiB atomic file mutation.
struct DirectoryKey
{
    uint64_t reference;
    unsigned length;
    uint8_t  name[576]; // FILE_NAME value, including UTF-16 name.
};
struct DirectoryEdit
{
    Inode          inode;
    unsigned       count;
    DirectoryEntry entries[MaxDirectoryKeys];
    DirectoryKey   keys[MaxDirectoryKeys];
};
struct MftListEntry
{
    unsigned length;
    uint8_t  data[544]; // Aligned entry plus the maximum 255-unit attribute name.
};
struct WriteWorkspace
{
    JournalWorkspace  journal;
    RecoveryWorkspace recovery;
    Volume            validation, directory_validation, before_validation;
    Workspace         scratch;
    uint8_t           allocation[MaxAllocationBitmap], ownership[MaxAllocationBitmap];
    uint8_t           mft_bits[MaxWriteRecords / 8], page[JournalPage], attribute[JournalPage];
    DirectoryEdit     directories[2];
    Stream            data, replacement, old_index, audit_stream, mft_map, mft_bitmap, old_mft, mft_list_map;
    MftListEntry      mft_entries[MaxAttributeEntries];
    uint8_t           mft_list[MaxAttributeList], old_record[MaxRecord], mft_base[MaxRecord], before_page[JournalPage];
    unsigned          mft_entry_count;
    bool              mft_has_list;
    Run               allocated[MaxRuns];
    uint64_t          new_clusters[MaxCowClusters];
    uint64_t          checked_parents[MaxMetadataImages * 8];
    unsigned          allocated_count, cluster_count, directory_count;
};

// Explicit, serialized, bounded metadata writer. The ordinary Volume API has
// no recovery side effects. A failed device operation invalidates this session.
class WriteSession {
  public:
    int  open(const WriteDevice &device, Volume &volume, Workspace &reader_work, WriteWorkspace &work);
    int  create(const Inode &parent, const char *name, bool directory, uint64_t time, Inode &result);
    int  unlink(const Inode &parent, const char *name);
    int  rename(const Inode &parent, const char *name, const Inode &destination, const char *new_name);
    int  resize(const Inode &inode, uint64_t size, uint64_t time);
    int  pwrite(const Inode &inode, uint64_t offset, const void *input, Size length, uint64_t time, Size &completed);
    int  sync();
    bool failed() const
    {
        return failed_ || journal_.failed() || (volume_ && volume_->io_failed_);
    }
    Volume &volume()
    {
        return *volume_;
    }

  private:
    WriteDevice     device_;
    Volume         *volume_;
    Workspace      *reader_work_;
    WriteWorkspace *work_;
    Journal         journal_;
    uint64_t        mft_reference_, clusters_;
    unsigned        bitmap_bytes_, records_, record_;
    bool            opened_, failed_;
    static bool     before_read(void *context, uint64_t offset, void *output, Size length);
    static bool     read_overlay(void *context, uint64_t offset, void *output, Size length, bool before);
    static bool     overlay_read(void *context, uint64_t offset, void *output, Size length);
    static int      validate_callback(void *context, JournalWorkspace &);
    int             validate();
    int             audit(Volume &volume, Workspace &scratch, bool load);
    int             begin();
    int             finish();
    int             map(const Stream &stream, uint64_t offset, uint64_t &physical) const;
    int             stage_record(uint64_t number, MetadataImage *&image);
    int             grow_mft();
    int             mft_load_entries();
    int             mft_extension(uint64_t prefix_clusters, MetadataImage *&image);
    int             mft_place(unsigned entry, const uint8_t *value, unsigned length);
    int             mft_stream(uint32_t type, const Stream &stream, uint64_t old_allocated);
    int             mft_list();
    int             mft_room(unsigned length);
    int             mft_entry(const uint8_t *attribute, uint64_t reference, unsigned &index);
    int             allocate_contiguous(unsigned count);
    int             validate_mft(const MetadataImage &image);
    int             load_record(uint64_t reference, MetadataImage *&image, bool directory = false);
    int             allocate(unsigned count);
    int             release_stream(const Stream &stream);
    int stage_bitmap(const Stream &stream, uint64_t reference, uint32_t type, const uint8_t *bits, unsigned bytes);
    int stage_bitmaps();
    int load_directory(const Inode &inode, DirectoryEdit *&directory);
    int rebuild_directory(DirectoryEdit &directory);
    int directories();
    int key(const Inode &parent, const char *name, const uint8_t *record, uint64_t reference, DirectoryKey &key);
    int find(const DirectoryEdit &directory, const char *name, unsigned &position) const;
    int check_name(const Inode &parent, const char *name) const;
    int change_file(const Inode &inode, uint64_t size, uint64_t offset, const void *input, Size length, uint64_t time);
    int update_names(MetadataImage &record, uint64_t size, uint64_t allocated);
    int write_new_data(const Stream &old, uint64_t size, uint64_t offset, const void *input, Size length);
};
} // namespace xjntfs
