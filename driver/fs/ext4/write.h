#pragma once
// SPDX-License-Identifier: Apache-2.0

#include "native.h"
#include "journal.h"
#include "attributes.h"
#include "directory_index.h"
#include "io_cache.h"

namespace xjext4 {

using Writer = bool (*)(void *, uint64_t byte_offset, const void *, Size);
using Flusher = bool (*)(void *);

struct WriteDevice
{
    Reader read;
    Writer write;
    Flusher flush;
    void *context;
    uint64_t bytes;
};

// Optional caller-owned arena. The session never allocates or frees it.
// Keep it alive through successful close, or until reboot after quarantine.
struct WriteStorage
{
    void *data;
    Size bytes;
};

// Caller-owned workspace: never place this on an interrupt stack. The session
// has one owner; its device and workspace must outlive open/close and recovery.
// The caller must exclusively own the device for the entire session. Do not
// place the large WriteSession or its workspace on a kernel/interrupt stack.
struct WriteWorkspace
{
    uint8_t blocks[16][4096];
    JournalWorkspace journal;
    DirectoryIndexWorkspace directory;
    ReadCache read_cache;
};

// Bounded journaled mutations. Each transaction has at most sixteen complete
// after-images; a large pwrite may commit a prefix and report partial progress.
// Mapping nodes, allocation metadata, file bytes and inode changes share the
// same transaction. Caller-owned storage must remain in task/heap context.
class WriteSession
{
public:
    using InitialValidator = int (*)(void *, const Volume &);
    // Read-only sizing accepts supported immutable geometry, including a dirty
    // volume's bootstrap view. Neither sizing function writes or flushes.
    static int storage_size(const WriteDevice &device, Size &bytes);
    static int storage_size_for(uint64_t blocks, uint32_t inodes, uint32_t groups, Size &bytes);
    // Optional root-volume identity/content check. Called on the strict final
    // recovery view before any log/home write, then discarded after open.
    int open(const WriteDevice &device, WriteWorkspace &workspace,
             InitialValidator validator = nullptr, void *validator_context = nullptr,
             const WriteStorage *storage = nullptr);
    int pwrite(uint32_t number, uint64_t offset, const void *data, Size length,
               uint64_t timestamp_seconds, Size &completed);
    int truncate(uint32_t number, uint64_t size, uint64_t timestamp_seconds);
    int create(uint32_t parent, const char *name, uint16_t mode, uint32_t uid, uint32_t gid,
               uint64_t timestamp_seconds, Inode &created);
    int unlink(uint32_t parent, const char *name, uint64_t timestamp_seconds, bool defer_cleanup = false);
    int rename(uint32_t source_parent, const char *source, uint32_t destination_parent,
               const char *destination, uint64_t timestamp_seconds, bool defer_target_cleanup = false);
    int reclaim_orphan(uint32_t number);
    int link(uint32_t parent, const char *name, uint32_t target, uint64_t timestamp_seconds);
    int symlink(uint32_t parent, const char *name, const char *target, uint32_t uid, uint32_t gid,
                uint64_t timestamp_seconds, Inode &created);
    int set_attributes(uint32_t number, const AttributeUpdate &attributes, uint64_t timestamp_seconds);
    int set_xattr(uint32_t number, const char *name, const void *value, Size length,
                  unsigned flags, uint64_t timestamp_seconds);
    int remove_xattr(uint32_t number, const char *name, uint64_t timestamp_seconds);
    int set_xattr_raw(uint32_t number, uint8_t index, const char *name, const void *value, Size length,
                      unsigned flags, uint64_t timestamp_seconds);
    int remove_xattr_raw(uint32_t number, uint8_t index, const char *name, uint64_t timestamp_seconds);
    int sync();
    int close();
    // Poweroff boundary: the caller has permanently stopped filesystem access.
    // Open-file holds cease to exist and their persistent orphans are reclaimed.
    int shutdown();
    bool failed() const { return failed_; }
    const Volume &view() const { return volume_; }

private:
    WriteDevice device_;
    WriteWorkspace *workspace_;
    Volume volume_;
    Inode journal_inode_;
    Journal journal_;
    uint8_t super_[1024];
    struct MetadataRange
    {
        uint64_t first;
        uint64_t count;
        uint32_t inode_group; // UINT32_MAX for every non-inode-table range.
    };
    struct GroupLayout
    {
        uint64_t bitmap;
        uint64_t inode_bitmap;
        uint64_t inode_table;
        uint32_t bitmap_checksum;
        uint32_t free_blocks;
        uint32_t free_inodes;
        uint16_t flags;
    };
    MetadataRange inline_metadata_[512];
    GroupLayout inline_groups_[128];
    MetadataRange *metadata_;
    GroupLayout *groups_;
    Size metadata_capacity_;
    uint64_t journal_data_[32768];
    uint64_t journal_metadata_[512];
    // Exact ownership sets are lazily cleared one touched 4 KiB page at a
    // time. A large sparse volume never clears its entire bitmap per write.
    struct Bitmap
    {
        uint8_t *data;
        uint32_t *generation;
        Size bytes, pages;
        uint64_t bits;
        uint32_t epoch;
    };
    uint8_t inline_mapping_[524288], inline_xattrs_[524288], inline_holds_[524288];
    uint32_t inline_generation_[3][128];
    Bitmap mapping_bits_, xattr_bits_, held_bits_;
    // Sorted bounded census batches preserve the global recovery EA-owner
    // proof without an allocation proportional to physical block count.
    struct RecoveryReference { uint64_t block; uint16_t count; };
    RecoveryReference recovery_references_[1024];
    uint32_t held_orphan_count_;
    uint8_t bitmap_cache_[4096];
    uint8_t descriptor_cache_[4096];
    uint32_t metadata_count_;
    uint32_t journal_blocks_;
    uint64_t journal_first_;
    uint64_t journal_last_;
    uint32_t journal_metadata_count_;
    uint32_t bitmap_group_;
    uint32_t first_inode_;
    bool verifying_journal_;
    uint64_t tx_blocks_[16];
    unsigned tx_count_;
    uint8_t inline_dirty_[128];
    uint8_t *tx_dirty_;
    bool tx_active_;
    bool validating_;
    const JournalTransaction *replay_view_;
    bool active_;
    bool failed_;
    InitialValidator initial_validator_;
    void *initial_validator_context_;
    bool initial_validated_;

    static bool home_read(void *, uint64_t, void *, Size);
    bool read_home(uint64_t offset, void *output, Size bytes);
    static bool journal_read(void *, uint64_t, void *, uint32_t);
    static bool journal_write(void *, uint64_t, const void *, uint32_t);
    static bool journal_flush(void *);
    static bool journal_map(void *, uint32_t, uint64_t &);
    static bool validate_update(void *, uint64_t, const void *);
    static bool validate_transaction(void *, const JournalTransaction &);
    static bool overlay_read(void *, uint64_t, void *, Size);
    int validate_afterstate();
    int validate_orphans_afterstate(const Volume &after);
    int validate_orphan_references(const Volume &after);
    int inode_mapping_end(const Volume &after, const Inode &inode, uint64_t &end);
    int begin_orphan_truncate(uint32_t number, uint64_t size, uint64_t seconds);
    int preflight_inode_mapping(const Inode &inode);
    int recover_orphans();
    int clean_orphan(uint32_t number, bool release_inode);
    int find_orphan(uint32_t number, uint32_t &previous, uint32_t &next, bool &found) const;
    int tx_add_orphan(uint32_t number, uint8_t *raw);
    int tx_remove_orphan(uint32_t number, uint8_t *raw);
    bool orphan_held(uint32_t number) const;
    void hold_orphan(uint32_t number, bool held);
    unsigned tx_image_count() const;
    uint64_t tx_image_block(unsigned index) const;
    bool tx_image_read(unsigned index, void *output) const;
    bool tx_contains(uint64_t block) const;
    bool tx_is_xattr_block(uint64_t block) const;
    int validate_xattrs_afterstate(const Volume &after);
    int tx_release_xattrs(uint32_t number, uint8_t *raw);
    int mutate_xattr(uint32_t number, uint8_t index, const char *name, const void *value, Size length,
                     unsigned flags, bool remove, uint64_t seconds);
    int validate_inode_mapping(const Volume &after, const Inode &inode, uint64_t &blocks);
    int validate_extent_node(const Volume &after, const Inode &inode, const uint8_t *node,
                            uint16_t depth, uint64_t lower, uint64_t upper, uint64_t &blocks);
    int validate_indirect_node(const Volume &after, uint64_t block, unsigned depth, uint64_t &blocks);
    int claim_mapping_block(const Volume &after, uint64_t block);
    bool protected_block(uint64_t block) const;
    int tx_begin();
    void tx_cancel();
    int tx_edit(uint64_t block, uint8_t *&data, bool zero = false);
    int tx_read(uint64_t block, uint8_t *output);
    void tx_forget(uint64_t block);
    uint32_t tx_inode_number(const uint8_t *raw) const;
    int tx_mapping_block(uint64_t block, uint64_t xattr);
    int tx_change_sectors(uint8_t *raw, int64_t blocks);
    int tx_mapping_witness(uint64_t block);
    int tx_inode(uint32_t number, uint8_t *&raw);
    int tx_descriptor(uint32_t group, uint8_t *&descriptor);
    int tx_bitmap(uint32_t group, bool inode, uint8_t *&bitmap, uint8_t *&descriptor);
    int tx_counts(uint32_t group, int blocks, int inodes, int directories);
    void tx_seal_group(uint32_t group, uint8_t *descriptor);
    int tx_allocate_inode(uint16_t mode, uint32_t uid, uint32_t gid, uint64_t seconds,
                          uint32_t &number, uint8_t *&raw);
    int tx_release_inode(uint32_t number, uint8_t *raw);
    int tx_allocate_block(uint64_t &block);
    int tx_release_block(uint64_t block);
    int tx_map(uint8_t *raw, uint32_t logical, uint64_t &physical, bool allocate = false);
    int tx_extent_map(uint32_t number, uint8_t *raw, uint32_t logical, uint64_t &physical, bool allocate);
    int tx_indirect_map(uint8_t *raw, uint32_t logical, uint64_t &physical, bool allocate);
    int tx_mapping_last(uint8_t *raw, uint32_t &logical, bool &found);
    int tx_mapping_remove_last(uint8_t *raw, uint32_t logical);
    int tx_extent_last(uint32_t number, uint8_t *raw, uint32_t &logical, bool &found, bool remove);
    int tx_indirect_last(uint8_t *raw, uint32_t &logical, bool &found, bool remove);
    int tx_resize(uint32_t number, uint8_t *raw, uint64_t size);
    int tx_finish_inode(uint32_t number, uint8_t *raw, uint64_t seconds, bool modify_mtime = true);
    void tx_seal_inode(uint32_t number, uint8_t *raw);
    int tx_commit();
    struct DirectoryPosition
    {
        uint64_t block;
        uint32_t number;
        uint16_t offset;
        uint16_t previous;
        uint8_t type;
    };
    struct IndexContext
    {
        WriteSession *session;
        uint8_t *raw;
    };
    DirectoryIndexIO ns_index_io(uint32_t number, uint8_t *raw, IndexContext &context);
    int ns_directory(uint32_t, uint8_t *&raw);
    int ns_find(uint32_t, uint8_t *raw, const char *, DirectoryPosition &);
    int ns_insert(uint32_t, uint8_t *raw, const char *, uint32_t child, uint8_t type);
    int ns_remove(uint32_t, uint8_t *raw, const DirectoryPosition &);
    int ns_empty(uint32_t, uint8_t *raw, uint32_t expected_parent);
    int ns_check_block(uint32_t, const uint8_t *raw, uint8_t *block) const;
    void ns_seal_block(uint32_t, const uint8_t *raw, uint8_t *block) const;
    static int collect_journal_metadata(void *, uint64_t);
    int configure_storage(const WriteStorage *storage);
    static bool bit_test(const Bitmap &, uint64_t bit);
    static bool bit_set(Bitmap &, uint64_t bit, bool value = true);
    static void bit_clear(Bitmap &);
    static bool bit_next(const Bitmap &, uint64_t first, uint64_t &found);
    const MetadataRange *metadata_range(uint64_t block) const;
    int check_writable_super();
    int build_layout();
    int add_metadata(uint64_t first, uint64_t count, uint32_t inode_group = 0xffffffffU);
    int check_inode_update(uint64_t block, uint32_t group, const void *after);
    int verify_recovered_layout();
    int set_recovery(bool enabled);
    int check_target_block(uint64_t block, uint32_t inode_group = 0xffffffffU);
    int stop(int error);
};

} // namespace xjext4
