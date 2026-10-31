#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

// First-party, bounded JBD2 redo writer. The format facts are documented at
// https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html
// Filesystem recovery flags and exclusive mount ownership belong to the caller.
namespace xjext4 {

// Valid only during a transaction-policy callback. Images are complete and
// unescaped; the reader copies block_size bytes into caller-owned storage.
// Callers may inspect images in any order, but must not retain this view.
struct JournalTransaction
{
    const uint64_t *blocks;
    unsigned count;
    uint32_t block_size;
    void *context;
    bool (*read)(void *context, unsigned index, void *output);
    // Optional indexed lookup for a large recovery overlay. Mutable commits
    // contain at most sixteen images and may leave this callback null.
    bool (*find)(void *context, uint64_t block, unsigned &index);
};

struct JournalReplayBlock
{
    uint64_t block;
    uint32_t logical;
    uint32_t flags;
    uint32_t sequence;
    uint32_t checksum;
};

// Optional caller-owned storage for a complete large journal scan. It belongs
// on the heap, not a kernel/interrupt stack; no allocator is imported here.
struct JournalWorkspace
{
    static constexpr unsigned kMaximumBlocks = 32768;
    static constexpr unsigned kIndexSlots = 65536;
    uint64_t mapping[kMaximumBlocks];
    JournalReplayBlock images[kMaximumBlocks];
    uint64_t targets[kMaximumBlocks];
    uint32_t index[kIndexSlots];
    uint16_t mapping_index[kIndexSlots];
};

struct JournalDevice
{
    void *context;
    bool (*read)(void *context, uint64_t physical_block, void *output, uint32_t bytes);
    bool (*write)(void *context, uint64_t physical_block, const void *input, uint32_t bytes);
    bool (*flush)(void *context);
    bool (*map)(void *context, uint32_t logical_block, uint64_t &physical_block);
    uint64_t filesystem_blocks;
    uint64_t journal_bytes;
    uint32_t block_size;
    uint32_t mapped_blocks;
    uint8_t uuid[16];
    // Optional nonmutating policy for complete, unescaped after-images. The
    // caller knows its filesystem metadata and may reject unsupported updates.
    bool (*validate)(void *context, uint64_t target, const void *after);
    // Optional whole-transaction policy, called after tag/image validation and
    // before any writes. This permits cross-checking dependent metadata through
    // an overlay without requiring the torn home copies to validate first.
    bool (*validate_transaction)(void *context, const JournalTransaction &transaction);
    JournalWorkspace *workspace;
};

struct BlockUpdate
{
    uint64_t block;
    const void *bytes;
};

class Journal
{
public:
    static constexpr unsigned kMaximumUpdates = 16;
    // Caller storage permits 32768 journal blocks; the allocator-free fallback
    // supports 4096 mapping blocks and sixteen recovery after-images.
    int open(const JournalDevice &device);
    int commit(const BlockUpdate *updates, unsigned count);
    // A guarded mount also validates an empty/stale recovery view before clear.
    int recover(bool &replayed, bool validate_empty = false);
    bool has_pending() const { return ready_ && start_ != 0; }
    uint32_t pending_start() const { return start_; }
    uint32_t sequence() const { return sequence_; }
    bool usable() const { return ready_ && !failed_; }
    bool failed() const { return failed_; }

private:
    int validate_empty_recovery();
    struct RecoveryImages
    {
        Journal *journal;
        JournalReplayBlock *images;
        const uint32_t *index;
        unsigned index_slots;
        unsigned count;
    };

    JournalDevice device_;
    uint8_t super_[4096];
    uint64_t mapping_small_[4096];
    uint64_t *mapping_;
    uint16_t mapping_index_small_[8192];
    uint16_t *mapping_index_;
    uint32_t mapping_index_mask_;
    uint32_t first_;
    uint32_t maximum_;
    uint32_t start_;
    uint32_t sequence_;
    bool wide_;
    uint8_t checksum_version_;
    uint32_t checksum_seed_;
    bool ready_;
    bool failed_;

    int fetch(uint64_t block, void *output);
    int store(uint64_t block, const void *input);
    int barrier();
    int clear();
    bool valid_target(uint64_t block) const;
    uint32_t advance(uint32_t block, unsigned count) const;
    static bool read_recovery_image(void *context, unsigned index, void *output);
    static bool find_recovery_image(void *context, uint64_t block, unsigned &index);
    int read_image(const JournalReplayBlock &image, void *output);
    int scan(JournalReplayBlock *images, unsigned capacity, unsigned &count,
             uint32_t &end, uint32_t &next_sequence, bool revokes,
             uint32_t *index, unsigned index_slots);
    bool checksum_block(const uint8_t *block, uint32_t checksum_offset) const;
    void seal_block(uint8_t *block, uint32_t checksum_offset) const;
    void seal_super();
};

} // namespace xjext4
