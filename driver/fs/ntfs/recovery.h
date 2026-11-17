#pragma once
// SPDX-License-Identifier: Apache-2.0 AND MIT
// Adapted from pinned MIT DiscUtils journal algorithms; retained copyright and
// complete grants are in DISCUTILS_LICENSE.txt / DISCUTILS_NOTICE.txt.
#include "journal.h"
namespace xjntfs {
constexpr unsigned MaxRecoveryRecords      = 4096;
constexpr unsigned MaxRecoveryAttributes   = 128;
constexpr unsigned MaxRecoveryTransactions = 128;
constexpr unsigned MaxRecoveryDirty        = 512;
constexpr unsigned MaxRecoveryLcns         = 32;
constexpr unsigned MaxRecoveryData         = 192 * 1024;
struct RecoveryRecord
{
    uint64_t lsn, previous, undo_next;
    uint32_t type, transaction, length;
    uint16_t flags, client_sequence, client_index;
};
struct RecoveryAttribute
{
    JournalAttribute value;
    uint64_t         open_lsn;
    uint32_t         id, index_bytes;
};
struct RecoveryDirty
{
    uint64_t vcn, oldest, lcns[MaxRecoveryLcns];
    unsigned target, count;
};
struct RecoveryTransaction
{
    uint64_t first, previous, undo_next;
    uint32_t id, state;
    bool     modified;
};
struct RecoveryOperation
{
    uint16_t redo, undo, redo_at, redo_size, undo_at, undo_size, target, record_offset, attribute_offset;
    unsigned start, count;
    uint64_t vcn, lcns[MaxRecoveryLcns];
};
struct RecoveryWorkspace
{
    RecoveryRecord      records[MaxRecoveryRecords], current;
    RecoveryAttribute   attributes[MaxRecoveryAttributes];
    RecoveryDirty       dirty[MaxRecoveryDirty];
    RecoveryTransaction transactions[MaxRecoveryTransactions];
    RecoveryOperation   operation;
    unsigned            record_count, attribute_count, dirty_count, transaction_count, version, clusters_per_page;
    uint64_t            checkpoint, oldest, redo_lsn, last_lsn, mft_reference;
    unsigned            first_page, data_offset, safe_page;
    uint8_t             data[MaxRecoveryData], table[65536], operation_data[65536];
    uint8_t             page[JournalPage], spare[JournalPage], logical[JournalPage];
    uint64_t            tail_target[32], tail_lsn[32];
    unsigned            tail_page[32];
    unsigned            tail_count;
    uint32_t            undo_owner[MaxMetadataImages];
    uint32_t            undo_file_owner[MaxMetadataImages][4], undo_raw_owner[MaxMetadataImages];
    uint8_t             undo_bits[MaxMetadataImages][JournalPage];
    uint8_t             undo_current[MaxMetadataImages][JournalPage];
};
// In-memory ARIES analysis/redo/undo. No home write occurs here. Persistent
// publication uses a new native committed snapshot in unused LFS pages, keeping
// the entire source recovery history intact until a replacement restart is durable.
class WindowsRecovery {
  public:
    static int open(Journal &journal, RecoveryWorkspace &work);

  private:
    Journal           *journal_;
    RecoveryWorkspace *work_;
    int                run();
    int                page(unsigned number, uint64_t lsn, uint8_t *output);
    int                record(uint64_t lsn, RecoveryRecord &output);
    uint64_t           next(const RecoveryRecord &record) const;
    int                parse();
    int                tables(const uint8_t *restart);
    int                table(uint64_t lsn, unsigned length, unsigned operation);
    int                analyze();
    int                replay();
    int                apply(bool undo, uint32_t transaction);
    int snapshot(MetadataKind kind, const JournalAttribute &attribute, uint64_t vcn, uint64_t lcn, unsigned within,
                 unsigned length, MetadataImage *&image);
    RecoveryAttribute   *attribute(unsigned id, bool create);
    RecoveryTransaction *transaction(unsigned id, bool create);
    RecoveryDirty       *dirty(unsigned target, uint64_t vcn);
};
// Checked logical FILE/INDX primitives, adapted from MIT NtfsLogActions.cs.
int recovery_action(uint16_t operation, const RecoveryOperation &record, uint8_t *page, unsigned size,
                    const uint8_t *data, unsigned length, bool index);
int recovery_target(uint16_t operation); // 0 bookkeeping, 1 FILE, 2 INDX, 3 raw; -1 unknown.
} // namespace xjntfs
