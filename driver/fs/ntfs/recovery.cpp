// SPDX-License-Identifier: Apache-2.0 AND MIT
// Copyright (c) 2026, DiscUtils contributors. Adapted to caller-owned bounded
// workspaces and crash-safe snapshot publication; see PROVENANCE.md.
#include "recovery.h"
#include "format.h"
#include <errno.h>
namespace xjntfs {
using namespace format;
namespace {
bool same_name(const JournalAttribute &a, const JournalAttribute &b)
{
    if (a.reference != b.reference || a.type != b.type || a.name_length != b.name_length) return false;
    for (unsigned n = 0; n < a.name_length; ++n)
        if (a.name[n] != b.name[n]) return false;
    return true;
}

void index_name(JournalAttribute &a)
{
    a.named_index = a.name_length == 4 && a.name[0] == '$' && a.name[1] == 'I' && a.name[2] == '3' && a.name[3] == '0';
}

int name(JournalAttribute &a, const uint8_t *p, unsigned length)
{
    if (length % 2 || length / 2 > MaxJournalName) return -EOPNOTSUPP;
    a.name_length = length / 2;
    for (unsigned i = 0; i < a.name_length; ++i)
    {
        a.name[i] = le16(p + i * 2);
        if (!a.name[i]) return -EIO;
    }
    index_name(a);
    return 0;
}
} // namespace
int WindowsRecovery::open(Journal &journal, RecoveryWorkspace &work)
{
    WindowsRecovery self;
    self.journal_ = &journal;
    self.work_    = &work;
    return self.run();
}

RecoveryAttribute *WindowsRecovery::attribute(unsigned id, bool create)
{
    auto &w = *work_;
    for (unsigned n = 0; n < w.attribute_count; ++n)
        if (w.attributes[n].id == id) return w.attributes + n;
    if (!create || w.attribute_count == MaxRecoveryAttributes) return nullptr;
    auto *a = w.attributes + w.attribute_count++;
    clear(a, sizeof(*a));
    a->id = id;
    return a;
}

RecoveryTransaction *WindowsRecovery::transaction(unsigned id, bool create)
{
    auto &w = *work_;
    for (unsigned n = 0; n < w.transaction_count; ++n)
        if (w.transactions[n].id == id) return w.transactions + n;
    if (!create || w.transaction_count == MaxRecoveryTransactions) return nullptr;
    auto *t = w.transactions + w.transaction_count++;
    clear(t, sizeof(*t));
    t->id = id;
    return t;
}

RecoveryDirty *WindowsRecovery::dirty(unsigned target, uint64_t vcn)
{
    auto &w = *work_;
    for (unsigned n = 0; n < w.dirty_count; ++n)
        if (w.dirty[n].target == target && vcn >= w.dirty[n].vcn && vcn - w.dirty[n].vcn < w.dirty[n].count)
            return w.dirty + n;
    return nullptr;
}

int WindowsRecovery::page(unsigned number, uint64_t lsn, uint8_t *output)
{
    auto &j     = *journal_;
    auto &w     = *work_;
    int   error = j.log_read(uint64_t(number) * JournalPage, output, JournalPage);
    if (error) return error;
    bool     valid  = !unpack(output, JournalPage, 0x44524352);
    uint64_t latest = valid ? le64(output + 8) : 0;
    for (unsigned n = 0; n < w.tail_count; ++n)
    {
        if (w.tail_target[n] != uint64_t(number) * JournalPage || (valid && w.tail_lsn[n] <= latest)) continue;
        error = j.log_read(uint64_t(w.tail_page[n]) * JournalPage, w.spare, JournalPage);
        if (error) return error;
        if (unpack(w.spare, JournalPage, 0x44524352)) return -EIO;
        copy(output, w.spare, JournalPage);
        latest = w.tail_lsn[n];
        valid  = true;
    }
    return valid && latest >= lsn ? 0 : -ENOENT;
}

int WindowsRecovery::record(uint64_t lsn, RecoveryRecord &out)
{
    auto    &j    = *journal_;
    auto    &w    = *work_;
    uint64_t mask = (uint64_t(1) << (64 - j.sequence_bits_)) - 1, offset = (lsn & mask) * 8;
    unsigned page_number = unsigned(offset / JournalPage), within = unsigned(offset % JournalPage);
    if (!lsn || page_number < w.first_page || offset >= j.file_size_ || within < w.data_offset ||
        within > JournalPage - 48)
        return -ENOENT;
    int error = page(page_number, lsn, w.page);
    if (error) return error;
    const uint8_t *p = w.page + within;
    if (le64(p) != lsn) return -ENOENT;
    out = {lsn,          le64(p + 8),  le64(p + 16), le32(p + 32), le32(p + 36),
           le32(p + 24), le16(p + 40), le16(p + 28), le16(p + 30)};
    if (out.flags & ~7U || out.client_index || out.client_sequence != j.client_sequence_ ||
        (out.type != 1 && out.type != 2))
        return -EOPNOTSUPP;
    if (out.length > MaxRecoveryData) return -EOPNOTSUPP;
    unsigned copied = 0, position = within + 48, visited = 0;
    while (copied < out.length)
    {
        unsigned amount = out.length - copied;
        if (amount > JournalPage - position) amount = JournalPage - position;
        copy(w.data + copied, w.page + position, amount);
        copied += amount;
        if (copied == out.length) break;
        if (++visited >= j.file_size_ / JournalPage - w.first_page) return -EIO;
        if (++page_number == j.file_size_ / JournalPage) page_number = w.first_page;
        error = page(page_number, lsn, w.page);
        if (error) return error;
        position = w.data_offset;
    }
    if (!w.oldest || lsn < w.oldest) w.oldest = lsn;
    return 0;
}

uint64_t WindowsRecovery::next(const RecoveryRecord &r) const
{
    auto    &j      = *journal_;
    auto    &w      = *work_;
    unsigned bits   = 64 - j.sequence_bits_;
    uint64_t offset = (r.lsn & ((uint64_t(1) << bits) - 1)) * 8, sequence = r.lsn >> bits;
    uint64_t remaining   = (uint64_t(r.length) + 48 + 7) & ~uint64_t(7);
    uint64_t page_offset = offset / JournalPage * JournalPage, room = JournalPage - offset % JournalPage, position;
    if (remaining < room)
        position = offset + remaining;
    else
    {
        remaining -= room;
        for (;;)
        {
            page_offset += JournalPage;
            if (page_offset >= j.file_size_)
            {
                page_offset = uint64_t(w.first_page) * JournalPage;
                ++sequence;
            }
            uint64_t available = JournalPage - w.data_offset;
            if (remaining < available)
            {
                position = page_offset + w.data_offset + remaining;
                break;
            }
            remaining -= available;
        }
    }
    if (JournalPage - position % JournalPage < 48)
    {
        page_offset = position / JournalPage * JournalPage + JournalPage;
        if (page_offset >= j.file_size_)
        {
            page_offset = uint64_t(w.first_page) * JournalPage;
            ++sequence;
        }
        position = page_offset + w.data_offset;
    }
    if (sequence >= (uint64_t(1) << j.sequence_bits_)) return 0;
    return (sequence << bits) | (position / 8);
}

int WindowsRecovery::parse()
{
    auto       &w   = *work_;
    auto       &r   = w.operation;
    const auto &lfs = w.current;
    if (lfs.type != 1 || lfs.length < 32) return -EIO;
    const uint8_t *d   = w.data;
    r.redo             = le16(d);
    r.undo             = le16(d + 2);
    r.redo_at          = le16(d + 4);
    r.redo_size        = le16(d + 6);
    r.undo_at          = le16(d + 8);
    r.undo_size        = le16(d + 10);
    r.target           = le16(d + 12);
    r.count            = le16(d + 14);
    r.record_offset    = le16(d + 16);
    r.attribute_offset = le16(d + 18);
    r.start            = unsigned(le16(d + 20)) * 512;
    r.vcn              = le64(d + 24);
    if (recovery_target(r.redo) < 0 || recovery_target(r.undo) < 0) return -EOPNOTSUPP;
    if (r.count > MaxRecoveryLcns) return -EOPNOTSUPP;
    unsigned minimum = 32 + r.count * 8;
    if (minimum > lfs.length || r.vcn > uint64_t(__INT64_MAX__) - r.count) return -EIO;
    if ((r.redo_size && (r.redo_at < minimum || r.redo_at % 8)) ||
        (r.undo_size && (r.undo_at < minimum || r.undo_at % 8)))
        return -EIO;
    for (unsigned n = 0; n < r.count; ++n)
    {
        r.lcns[n] = le64(d + 32 + n * 8);
        if (r.lcns[n] >= journal_->clusters_) return -EIO;
    }
    return 0;
}

int WindowsRecovery::table(uint64_t lsn, unsigned length, unsigned operation)
{
    auto &w = *work_;
    if (length > sizeof(w.table)) return -EOPNOTSUPP;
    int error = record(lsn, w.current);
    if (error) return error;
    error = parse();
    if (error) return error;
    const auto &op = w.operation;
    if (op.redo != operation || op.redo_size < length) return -EIO;
    clear(w.table, length);
    if (!(w.current.flags & 2) && op.redo_at < w.current.length)
    {
        unsigned amount = w.current.length - op.redo_at;
        if (amount > length) amount = length;
        copy(w.table, w.data + op.redo_at, amount);
    }
    return 0;
}

int WindowsRecovery::tables(const uint8_t *restart)
{
    auto   &w = *work_;
    uint8_t header[64];
    copy(header, restart, 64);
    w.version = le32(header);
    if (w.version > 1 || le32(header + 4)) return -EOPNOTSUPP;
    w.checkpoint = le64(header + 8);
    if (!w.checkpoint) return -EIO;
    w.clusters_per_page = 1;
    for (unsigned which = 0; which < 4; ++which)
    {
        unsigned length = le32(header + 48 + which * 4);
        if (!length) continue;
        int error = table(le64(header + 16 + which * 8), length, 0x1d + which);
        if (error) return error;
        const uint8_t *b = w.table;
        if (which == 1)
        {
            unsigned at = 0;
            while (at + 4 <= length)
            {
                unsigned target = le16(b + at), bytes = le16(b + at + 2);
                if (!target && !bytes) break;
                if (bytes > length - at - 4 || bytes + 6 > length - at) return -EIO;
                auto *a = attribute(target, false);
                if (!a) return -EIO;
                error = name(a->value, b + at + 4, bytes);
                if (error) return error;
                at += bytes + 6;
            }
            continue;
        }
        if (length < 24) return -EIO;
        unsigned size = le16(b), count = le16(b + 2);
        if (size < 8 || count > (length - 24) / size) return -EIO;
        if (which == 2)
        {
            unsigned lcns = w.version ? 32 : 36;
            if (size < lcns || (size - lcns) % 8) return -EIO;
            w.clusters_per_page = (size - lcns) / 8;
            if (!power_of_two(w.clusters_per_page) || w.clusters_per_page > MaxRecoveryLcns) return -EOPNOTSUPP;
        }
        for (unsigned n = 0; n < count; ++n)
        {
            unsigned at = 24 + n * size;
            if (le32(b + at) != 0xffffffffU) continue;
            if (which == 0)
            {
                if (size != (w.version ? 40U : 44U)) return -EOPNOTSUPP;
                auto *a = attribute(at, true);
                if (!a) return -EOPNOTSUPP;
                a->value.type      = le32(b + at + (w.version ? 8 : 28));
                a->value.reference = le64(b + at + (w.version ? 16 : 8));
                a->open_lsn        = le64(b + at + (w.version ? 24 : 16));
                a->index_bytes     = le32(b + at + (w.version ? 4 : 40));
            }
            else if (which == 2)
            {
                if (w.dirty_count == MaxRecoveryDirty) return -EOPNOTSUPP;
                auto &d        = w.dirty[w.dirty_count++];
                d.target       = le32(b + at + 4);
                d.count        = le32(b + at + 12);
                unsigned start = w.version ? 32 : 36;
                if (d.count > w.clusters_per_page || d.count > MaxRecoveryLcns) return -EIO;
                d.vcn    = le64(b + at + (w.version ? 16 : 20));
                d.oldest = le64(b + at + (w.version ? 24 : 28));
                if (d.vcn > uint64_t(__INT64_MAX__) - d.count) return -EIO;
                for (unsigned i = 0; i < d.count; ++i)
                {
                    d.lcns[i] = le64(b + at + start + i * 8);
                    if (d.lcns[i] >= journal_->clusters_) return -EIO;
                }
            }
            else
            {
                if (size < 32) return -EIO;
                auto *t = transaction(at, true);
                if (!t) return -EOPNOTSUPP;
                t->state     = b[at + 4];
                t->first     = le64(b + at + 8);
                t->previous  = le64(b + at + 16);
                t->undo_next = le64(b + at + 24);
                t->modified  = true; // The checkpoint may precede metadata already in the log.
                if (t->state < 1 || t->state > 3) return -EIO;
            }
        }
    }
    return 0;
}

int WindowsRecovery::analyze()
{
    auto    &j   = *journal_;
    auto    &w   = *work_;
    uint64_t lsn = w.checkpoint, first_after = 0;
    for (;;)
    {
        int error = record(lsn, w.current);
        if (error == -ENOENT && w.record_count && lsn > j.last_lsn_) break;
        if (error) return error == -ENOENT ? -EIO : error;
        if (w.record_count == MaxRecoveryRecords) return -EOPNOTSUPP;
        w.records[w.record_count++] = w.current;
        w.last_lsn                  = lsn;
        if (lsn != w.checkpoint && !first_after) first_after = lsn;
        if (lsn != w.checkpoint && w.current.type == 1)
        {
            error = parse();
            if (error) return error;
            auto &r = w.operation;
            auto *t = transaction(w.current.transaction, true);
            if (!t || !w.current.transaction) return -EOPNOTSUPP;
            if (!t->state)
            {
                t->state    = 1;
                t->first    = lsn;
                t->modified = false;
                t->previous = t->undo_next = 0;
            }
            uint64_t previous_undo = t->undo_next;
            t->previous            = lsn;
            t->undo_next           = r.undo == 1 ? w.current.undo_next : lsn;
            if (recovery_target(r.redo) > 0) t->modified = true;
            if (r.redo == 0x1c)
            {
                auto *a = attribute(r.target, true);
                if (!a) return -EOPNOTSUPP;
                unsigned length = w.version ? 40 : 44;
                if (r.redo_size < length - 8) return -EIO;
                clear(w.operation_data, length);
                if (!(w.current.flags & 2) && r.redo_at < w.current.length)
                {
                    unsigned amount = w.current.length - r.redo_at;
                    if (amount > length) amount = length;
                    copy(w.operation_data, w.data + r.redo_at, amount);
                }
                a->value.type      = le32(w.operation_data + (w.version ? 8 : 28));
                a->value.reference = le64(w.operation_data + (w.version ? 16 : 8));
                a->index_bytes     = le32(w.operation_data + (w.version ? 4 : 40));
                a->open_lsn        = le64(w.operation_data + (w.version ? 24 : 16));
                if (r.undo_size > MaxJournalName * 2 || r.undo_size % 2 || r.undo_at > w.current.length ||
                    r.undo_size > w.current.length - r.undo_at)
                    return -EOPNOTSUPP;
                error = name(a->value, w.data + r.undo_at, r.undo_size);
                if (error) return error;
            }
            else if (r.redo == 0xa)
            {
                if (r.redo_size % 16 || r.redo_at > w.current.length || r.redo_size > w.current.length - r.redo_at)
                    return -EIO;
                for (unsigned at = 0; at < r.redo_size; at += 16)
                {
                    uint64_t first = le64(w.data + r.redo_at + at), count = le64(w.data + r.redo_at + at + 8);
                    if (first > j.clusters_ || count > j.clusters_ - first) return -EIO;
                    for (unsigned n = 0; n < w.dirty_count; ++n)
                        for (unsigned k = 0; k < w.dirty[n].count; ++k)
                            if (w.dirty[n].lcns[k] >= first && w.dirty[n].lcns[k] - first < count)
                                w.dirty[n].lcns[k] = 0;
                }
            }
            else if (r.redo == 0x17)
            {
                auto *d = dirty(r.target, r.vcn);
                if (d && r.count && d->lcns[r.vcn - d->vcn]) d->lcns[r.vcn - d->vcn] = r.lcns[0];
            }
            else if (r.redo == 0x18)
                t->undo_next = w.current.undo_next;
            else if (r.redo == 0x19)
                t->state = 2;
            else if (r.redo == 0x1a)
                t->state = 3;
            else if (r.redo == 0x1b)
            {
                // Windows can finish an ordinary transaction with a CLR-shaped
                // FORGET whose undo-next is zero, without a separate COMMIT.
                // Its preceding update may still have a nonempty undo chain.
                // A nonterminal FORGET cannot resolve outstanding metadata undo;
                // prepared outcomes remain outside this recovery profile.
                bool terminal = r.undo == 1 && !w.current.undo_next;
                if (t->state == 2) return -EOPNOTSUPP;
                if (t->state != 3 && t->modified && previous_undo && !terminal) return -EUCLEAN;
                t->state = 0;
            }
            else if (recovery_target(r.redo) > 0 && r.count)
            {
                auto *d = dirty(r.target, r.vcn);
                if (!d)
                {
                    if (w.dirty_count == MaxRecoveryDirty) return -EOPNOTSUPP;
                    d = w.dirty + w.dirty_count++;
                    clear(d, sizeof(*d));
                    d->target = r.target;
                    d->vcn    = r.vcn & ~uint64_t(w.clusters_per_page - 1);
                    d->count  = w.clusters_per_page;
                    d->oldest = lsn;
                }
                if (r.vcn - d->vcn > d->count || r.count > d->count - (r.vcn - d->vcn)) return -EOPNOTSUPP;
                for (unsigned n = 0; n < r.count; ++n)
                    d->lcns[r.vcn - d->vcn + n] = r.lcns[n];
            }
        }
        uint64_t following = next(w.current);
        if (!following || following <= lsn) return -EIO;
        lsn = following;
    }
    if (w.last_lsn < j.last_lsn_) return -EIO;
    w.redo_lsn = first_after;
    for (unsigned n = 0; n < w.dirty_count; ++n)
        if (w.dirty[n].oldest && (!w.redo_lsn || w.dirty[n].oldest < w.redo_lsn)) w.redo_lsn = w.dirty[n].oldest;
    for (unsigned n = 0; n < w.transaction_count; ++n)
    {
        auto &t = w.transactions[n];
        if (t.state == 2) return -EOPNOTSUPP;
        if (t.state && t.first && (!w.redo_lsn || t.first < w.redo_lsn)) w.redo_lsn = t.first;
    }
    return 0;
}

int WindowsRecovery::snapshot(MetadataKind kind, const JournalAttribute &a, uint64_t vcn, uint64_t lcn, unsigned within,
                              unsigned length, MetadataImage *&image)
{
    auto &j = *journal_;
    auto &w = *j.work_;
    if (!lcn || lcn >= j.clusters_ || length > JournalPage || within > JournalPage - length) return -EIO;
    uint64_t physical = lcn * JournalPage + within;
    if (physical < uint64_t(JournalPage) * 2 || physical + length > (j.clusters_ - 1) * JournalPage ||
        j.overlaps_log(physical, length) || j.overlaps_mirror(physical, length))
        return -EIO;
    for (unsigned n = 0; n < w.count; ++n)
    {
        auto    &p     = w.images[n];
        uint64_t start = j.physical(p);
        if (start == physical && p.length == length && p.kind == kind && p.attribute_type == a.type &&
            p.file_reference == a.reference && p.vcn == vcn && p.name_length == a.name_length)
        {
            bool match = true;
            for (unsigned i = 0; i < a.name_length; ++i)
                if (p.name[i] != a.name[i]) match = false;
            if (match)
            {
                image = &p;
                return 0;
            }
        }
        if (start < physical + length && physical < start + p.length) return -EIO;
    }
    if (w.count == MaxMetadataImages) return -EOPNOTSUPP;
    unsigned target = 0;
    for (; target < w.attribute_count; ++target)
        if (same_name(w.attributes[target], a)) break;
    if (target == w.attribute_count)
    {
        if (target == MaxMetadataImages) return -EOPNOTSUPP;
        w.attributes[w.attribute_count++] = a;
    }
    image = w.images + w.count++;
    clear(image, sizeof(*image));
    image->kind           = kind;
    image->file_reference = a.reference;
    image->attribute_type = a.type;
    image->vcn            = vcn;
    image->lcn            = lcn;
    image->within         = uint16_t(within);
    image->length         = uint16_t(length);
    image->target         = uint16_t(target);
    image->named_index    = a.named_index;
    image->name_length    = a.name_length;
    for (unsigned n = 0; n < a.name_length; ++n)
        image->name[n] = a.name[n];
    int error = j.read(physical, image->before, length);
    if (error) return error;
    copy(image->after, image->before, length);
    if (kind == MetadataKind::IndexBuffer)
    {
        if (unpack(image->after, length, 0x58444e49)) clear(image->after, length);
        copy(image->before, image->after, length);
        image->lsn = le32(image->after) == 0x58444e49 ? le64(image->after + 8) : 0;
    }
    return 0;
}

int WindowsRecovery::apply(bool undo, uint32_t txid)
{
    auto    &j      = *journal_;
    auto    &w      = *work_;
    auto    &r      = w.operation;
    uint16_t op     = undo ? r.undo : r.redo;
    int      target = recovery_target(op);
    if (target <= 0) return target < 0 ? -EOPNOTSUPP : 0;
    if (!r.count) return undo || r.undo == 1 ? -EOPNOTSUPP : 0;
    auto *d = dirty(r.target, r.vcn);
    if (!undo)
    {
        if (!d || w.current.lsn < d->oldest) return 0;
        unsigned count = d->count - unsigned(r.vcn - d->vcn);
        if (count > MaxRecoveryLcns) return -EIO;
        for (unsigned i = 0; i < count; ++i)
            r.lcns[i] = d->lcns[r.vcn - d->vcn + i];
        r.count = count;
    }
    else if (d)
    {
        for (unsigned i = 0; i < r.count && r.vcn - d->vcn + i < d->count; ++i)
        {
            uint64_t current = d->lcns[r.vcn - d->vcn + i];
            if (!current)
                r.lcns[i] = 0;
            else if (current != r.lcns[i])
                return -EOPNOTSUPP;
        }
    }
    RecoveryAttribute *opened = attribute(r.target, false);
    // Reusing a table slot for a different stream invalidates the old identity.
    // A retained dirty record from before that OPEN is outside this profile.
    if (opened && opened->open_lsn && w.current.lsn < opened->open_lsn) return -EOPNOTSUPP;
    JournalAttribute a = {};
    if (opened)
        a = opened->value;
    else if (target == 1)
    {
        a.reference = w.mft_reference;
        a.type      = Data;
    }
    else
        return -EOPNOTSUPP;
    if (!uint16_t(a.reference >> 48)) return -EIO;
    if (target == 1 && ((a.reference & NumberMask) != 0 || a.type != Data || a.name_length)) return -EIO;
    if (target == 2 || (op == 8 && a.type == IndexAllocation))
    {
        target = 2;
        if (opened && opened->index_bytes && opened->index_bytes != JournalPage) return -EOPNOTSUPP;
    }
    unsigned length = undo ? r.undo_size : r.redo_size, offset = undo ? r.undo_at : r.redo_at;
    clear(w.operation_data, length);
    if (!(w.current.flags & (undo ? 4 : 2)) && offset < w.current.length)
    {
        unsigned amount = w.current.length - offset;
        if (amount > length) amount = length;
        copy(w.operation_data, w.data + offset, amount);
    }
    unsigned start = r.start + (target == 2 ? r.record_offset : 0), total = target == 1   ? j.record_
                                                                            : target == 2 ? JournalPage
                                                                                          : length;
    bool     bits      = op == 0x15 || op == 0x16;
    uint64_t first_bit = 0, bit_count = 0;
    if (target == 3)
    {
        start += r.record_offset;
        if (bits)
        {
            if (length < 8) return -EIO;
            first_bit = le32(w.operation_data);
            bit_count = le32(w.operation_data + 4);
            if (!bit_count) return 0;
            if (bit_count > uint64_t(MaxRecoveryLcns) * JournalPage * 8) return -EOPNOTSUPP;
            uint64_t at = uint64_t(start) + first_bit / 8;
            if (at > uint64_t(MaxRecoveryLcns) * JournalPage) return -EIO;
            start = unsigned(at);
            total = unsigned(((first_bit & 7) + bit_count + 7) / 8);
        }
        if (!total) return 0;
    }
    if (start > r.count * JournalPage || total > r.count * JournalPage - start) return -EIO;
    unsigned first = start / JournalPage, last = (start + total - 1) / JournalPage;
    if (!r.lcns[first]) return 0;
    for (unsigned n = first; n <= last; ++n)
        if (!r.lcns[n]) return -EOPNOTSUPP;
    if ((target == 1 && start % j.record_) || (target == 2 && (start % JournalPage || first != last)))
        return -EOPNOTSUPP;
    if (target == 1 || target == 2)
    {
        MetadataImage *image;
        // Full physical MFT clusters collect FILE and raw-MFT operations in a
        // single image, preserving every 1 KiB or 4 KiB record it contains.
        MetadataKind   kind  = target == 1 ? MetadataKind::Raw : MetadataKind::IndexBuffer;
        int            error = snapshot(kind, a, r.vcn + first, r.lcns[first], 0, JournalPage, image);
        if (error) return error;
        uint8_t *logical = target == 1 ? w.logical : image->after;
        unsigned within  = start % JournalPage;
        bool     valid   = true;
        if (target == 1)
        {
            copy(logical, image->after + within, j.record_);
            valid = !unpack(logical, j.record_, 0x454c4946);
        }
        else
            valid = le32(logical) == 0x58444e49 && le16(logical + 6) == 9;
        uint32_t signature = le32(logical);
        uint64_t page_lsn  = valid ? le64(logical + 8) : 0;
        if (signature == 0x44414142) return -EUCLEAN;
        if (signature == 0x454c4f48) return 0;
        if (!valid && target == 1 && op == 3 && signature == 0x454c4946) return -EUCLEAN;
        if (!undo && valid && page_lsn >= w.current.lsn) return 0;
        if (undo && valid && (page_lsn < w.current.lsn || page_lsn > w.last_lsn)) return -EUCLEAN;
        if (!valid && !((target == 1 && ((op == 2 && r.record_offset == 0 && length >= 48) || op == 3)) ||
                        (target == 2 && op == 8 && r.record_offset == 0 && length == JournalPage)))
            return -EUCLEAN;
        unsigned image_number = unsigned(image - j.work_->images);
        if (undo)
        {
            uint32_t &owner =
                target == 1 ? w.undo_file_owner[image_number][within / j.record_] : w.undo_owner[image_number];
            if ((owner && owner != txid) || (w.undo_raw_owner[image_number] && w.undo_raw_owner[image_number] != txid))
                return -EOPNOTSUPP;
            owner = txid;
        }
        if (!valid && op == 2) clear(logical, j.record_);
        if (target == 2 && op == 8)
        {
            copy(logical, w.operation_data, length);
            error = 0;
        }
        else
            error = recovery_action(op, r, logical, target == 1 ? j.record_ : JournalPage, w.operation_data, length,
                                    target == 2);
        if (error) return error;
        uint32_t expected = target == 1 ? 0x454c4946 : 0x58444e49;
        if (le32(logical) == expected)
        {
            put64(logical + 8, undo ? page_lsn : w.current.lsn);
            if (target == 1)
            {
                error = pack(logical, j.record_, image->after + within);
                if (error) return error;
            }
            else
                image->lsn = le64(logical + 8);
        }
        else if (target == 1 && op == 3)
            copy(image->after + within, logical, j.record_);
        else
            return -EIO;
        return 0;
    }
    // Plain updates can span clusters. Each snapshot preserves a full allocated
    // cluster; undo of different transactions may share only disjoint bitmap bits.
    unsigned done = 0;
    for (unsigned block = first; block <= last; ++block)
    {
        MetadataImage *image;
        int            error = snapshot(MetadataKind::Raw, a, r.vcn + block, r.lcns[block], 0, JournalPage, image);
        if (error) return error;
        unsigned at = block == first ? start % JournalPage : 0, amount = total - done;
        if (amount > JournalPage - at) amount = JournalPage - at;
        unsigned number = unsigned(image - j.work_->images);
        if (undo)
        {
            if (!bits)
            {
                if ((w.undo_raw_owner[number] && w.undo_raw_owner[number] != txid) ||
                    (w.undo_owner[number] && w.undo_owner[number] != txid))
                    return -EOPNOTSUPP;
                for (unsigned k = 0; k < 4; ++k)
                    if (w.undo_file_owner[number][k] && w.undo_file_owner[number][k] != txid) return -EOPNOTSUPP;
                w.undo_raw_owner[number] = txid;
            }
            else
            {
                if (w.undo_raw_owner[number] && w.undo_raw_owner[number] != txid) return -EOPNOTSUPP;
                if (w.undo_owner[number] != txid)
                {
                    for (unsigned k = 0; k < JournalPage; ++k)
                    {
                        w.undo_bits[number][k]    |= w.undo_current[number][k];
                        w.undo_current[number][k]  = 0;
                    }
                    w.undo_owner[number] = txid;
                }
            }
        }
        if (!bits)
            copy(image->after + at, w.operation_data + done, amount);
        else
        {
            for (unsigned byte = 0; byte < amount; ++byte)
            {
                uint64_t logical_byte = done + byte;
                uint8_t  mask         = 0;
                for (unsigned bit_number = 0; bit_number < 8; ++bit_number)
                {
                    uint64_t global_bit = logical_byte * 8 + bit_number;
                    if (global_bit >= (first_bit & 7) && global_bit - (first_bit & 7) < bit_count)
                        mask |= uint8_t(1U << bit_number);
                }
                if (undo)
                {
                    if (w.undo_bits[number][at + byte] & mask) return -EOPNOTSUPP;
                    w.undo_current[number][at + byte] |= mask;
                }
                if (op == 0x15)
                    image->after[at + byte] |= mask;
                else
                    image->after[at + byte] &= uint8_t(~mask);
            }
        }
        done += amount;
    }
    return 0;
}

int WindowsRecovery::replay()
{
    auto &w = *work_;
    if (w.redo_lsn)
    {
        uint64_t lsn   = w.redo_lsn;
        unsigned count = 0;
        while (lsn <= w.last_lsn)
        {
            if (++count > MaxRecoveryRecords) return -EOPNOTSUPP;
            int error = record(lsn, w.current);
            if (error) return error;
            if (w.current.type == 1)
            {
                error = parse();
                if (!error) error = apply(false, 0);
                if (error) return error;
            }
            if (lsn == w.last_lsn) break;
            uint64_t following = next(w.current);
            if (!following || following <= lsn || following > w.last_lsn) return -EIO;
            lsn = following;
        }
    }
    for (unsigned i = 0; i < w.transaction_count; ++i)
    {
        const auto t = w.transactions[i];
        if (t.state != 1) continue;
        uint64_t lsn   = t.undo_next;
        unsigned count = 0;
        while (lsn)
        {
            if (++count > MaxRecoveryRecords || lsn < t.first || lsn > t.previous || lsn > w.last_lsn) return -EIO;
            int error = record(lsn, w.current);
            if (error) return error;
            if (w.current.transaction != t.id || w.current.type != 1) return -EIO;
            uint64_t following = w.current.undo_next;
            if (following >= lsn) return -EIO;
            error = parse();
            if (!error) error = apply(true, t.id);
            if (error) return error;
            lsn = following;
        }
    }
    return 0;
}

int WindowsRecovery::run()
{
    auto &j        = *journal_;
    auto &w        = *work_;
    w.record_count = w.attribute_count = w.transaction_count = w.dirty_count = w.tail_count = 0;
    w.oldest = w.redo_lsn = w.last_lsn = 0;
    clear(w.undo_owner, sizeof(w.undo_owner));
    clear(w.undo_file_owner, sizeof(w.undo_file_owner));
    clear(w.undo_raw_owner, sizeof(w.undo_raw_owner));
    clear(w.undo_bits, sizeof(w.undo_bits));
    clear(w.undo_current, sizeof(w.undo_current));
    bool valid[2] = {};
    for (unsigned slot = 0; slot < 2; ++slot)
    {
        int error = j.log_read(uint64_t(slot) * JournalPage, j.work_->restarts[slot], JournalPage);
        if (error) return error;
        uint8_t *p = j.work_->restarts[slot];
        if (unpack(p, JournalPage, 0x52545352)) continue;
        unsigned ra = le16(p + 24), major = le16(p + 28), minor = le16(p + 26);
        if (le32(p + 16) != JournalPage || le32(p + 20) != JournalPage ||
            !((major == 1 && minor == 1) || (major == 2 && !minor)))
            return -EOPNOTSUPP;
        if (ra < 32 || ra % 8 || ra + 48 > JournalPage || le16(p + 4) + unsigned(le16(p + 6)) * 2 > ra) return -EIO;
        const uint8_t *a       = p + ra;
        unsigned       clients = le16(a + 8), array = le16(a + 22), bytes = le16(a + 20), bits = le32(a + 16),
                 data = le16(a + 38);
        uint64_t file = le64(a + 24);
        if (clients != 1) return -EOPNOTSUPP;
        if (array < 48 || array % 8 || bytes > JournalPage - ra || array + 160 > bytes || bits < 16 || bits > 60 ||
            file > j.log_.size || file < ((major == 2 ? 35U : 8U) * JournalPage) || file % JournalPage ||
            file / 8 > (uint64_t(1) << (64 - bits)) || le16(a + 36) != 48 || data < 40 || data % 8 ||
            data > JournalPage - 48 || (le16(a + 14) & ~3U))
            return -EIO;
        const uint8_t *c = a + array;
        if (!((le16(a + 10) == 0 && le16(a + 12) == 0xffff) || (le16(a + 10) == 0xffff && le16(a + 12) == 0)) ||
            le16(c + 16) != 0xffff || le16(c + 18) != 0xffff)
            return -EIO;
        if (le32(c + 28) != 8 || le16(c + 32) != 'N' || le16(c + 34) != 'T' || le16(c + 36) != 'F' ||
            le16(c + 38) != 'S')
            return -EOPNOTSUPP;
        uint64_t mask = (uint64_t(1) << (64 - bits)) - 1, current = le64(a);
        if ((current & mask) >= file / 8) return -EIO;
        if (le16(a + 12) != 0xffff &&
            (le64(c) > current || le64(c + 8) > current || (le64(c) && (le64(c) & mask) >= file / 8) ||
             (le64(c + 8) && (le64(c + 8) & mask) >= file / 8)))
            return -EIO;
        valid[slot] = true;
    }
    if (!valid[0] && !valid[1]) return -EIO;
    unsigned ra[2]    = {le16(j.work_->restarts[0] + 24), le16(j.work_->restarts[1] + 24)};
    unsigned selected = valid[1] && !valid[0];
    if (valid[0] && valid[1])
    {
        uint64_t a = le64(j.work_->restarts[0] + ra[0]), b = le64(j.work_->restarts[1] + ra[1]);
        if (a != b)
        {
            if ((a > b ? a - b : b - a) >= (uint64_t(1) << 63)) return -EIO;
            selected = b > a;
        }
        else
        {
            uint32_t delta = le32(j.work_->restarts[1] + ra[1] + 40) - le32(j.work_->restarts[0] + ra[0] + 40);
            if (delta == 0x80000000U) return -EIO;
            selected = delta && delta < 0x80000000U;
            if (!delta)
            {
                const uint8_t *first = j.work_->restarts[0] + ra[0], *second = j.work_->restarts[1] + ra[1];
                for (unsigned n = 0; n < 48; ++n)
                    if (first[n] != second[n]) return -EIO;
                unsigned ac = le16(first + 22), bc = le16(second + 22);
                for (unsigned n = 0; n < 160; ++n)
                    if (first[ac + n] != second[bc + n]) return -EIO;
            }
        }
    }
    const uint8_t *p = j.work_->restarts[selected], *a = p + ra[selected], *client = a + le16(a + 22);
    j.selected_        = selected;
    j.file_size_       = le64(a + 24);
    j.sequence_bits_   = le32(a + 16);
    j.last_lsn_        = le64(a);
    j.epoch_           = j.last_lsn_ >> (64 - j.sequence_bits_);
    j.revision_        = le32(a + 40);
    j.client_sequence_ = le16(client + 20);
    j.fresh_           = false;
    j.opened_          = true;
    j.pending_         = !(le16(a + 14) & 2) && le16(a + 12) != 0xffff;
    if (!j.pending_)
    {
        j.foreign_ = false;
        j.repair_  = true;
        return 0;
    }
    w.first_page         = le16(p + 28) == 2 ? 34 : 4;
    w.data_offset        = le16(a + 38);
    w.oldest             = le64(client);
    uint64_t restart_lsn = le64(client + 8);
    for (unsigned number = 2; number < w.first_page; ++number)
    {
        int error = j.log_read(uint64_t(number) * JournalPage, w.page, JournalPage);
        if (error) return error;
        if (unpack(w.page, JournalPage, 0x44524352)) continue;
        uint64_t target = le16(p + 28) == 2 ? le32(w.page + 60) : le64(w.page + 8);
        uint64_t lsn    = le64(w.page + (le16(p + 28) == 2 ? 8 : 32));
        if (target < uint64_t(w.first_page) * JournalPage || target >= j.file_size_ || target % JournalPage) continue;
        unsigned at       = w.tail_count++;
        w.tail_target[at] = target;
        w.tail_lsn[at]    = lsn;
        w.tail_page[at]   = number;
    }
    int error = j.read(j.mft_lcn_ * JournalPage, w.page, j.record_);
    if (error) return error;
    w.mft_reference = uint64_t(le16(w.page + 16)) << 48;
    error           = record(restart_lsn, w.current);
    if (error) return error;
    if (w.current.type != 2 || w.current.length < 64) return -EIO;
    error = tables(w.data);
    if (error) return error;
    error = analyze();
    if (error) return error;
    error = replay();
    if (error) return error;
    for (unsigned n = 0; n < j.work_->count;)
    {
        auto &image   = j.work_->images[n];
        bool  changed = false;
        for (unsigned k = 0; k < image.length; ++k)
            if (image.before[k] != image.after[k])
            {
                changed = true;
                break;
            }
        if (changed)
        {
            ++n;
            continue;
        }
        for (unsigned k = n + 1; k < j.work_->count; ++k)
            j.work_->images[k - 1] = j.work_->images[k];
        --j.work_->count;
    }
    // Reserve a whole replacement window outside every retained source page.
    // The active interval may wrap once; tail copies and restart pages are never reused.
    unsigned bits           = 64 - j.sequence_bits_;
    uint64_t mask           = (uint64_t(1) << bits) - 1;
    uint64_t oldest         = w.oldest ? w.oldest : w.checkpoint;
    uint64_t end            = next(w.records[w.record_count - 1]);
    uint64_t first_sequence = oldest >> bits, last_sequence = (end - 1) >> bits;
    if (last_sequence < first_sequence || last_sequence - first_sequence > 1) return -EIO;
    unsigned start = unsigned(((oldest & mask) * 8) / JournalPage),
             last  = unsigned((((end - 1) & mask) * 8) / JournalPage);
    unsigned run   = 0;
    w.safe_page    = 0;
    for (unsigned number = w.first_page; number < j.file_size_ / JournalPage; ++number)
    {
        bool active =
            last_sequence == first_sequence ? (number >= start && number <= last) : (number >= start || number <= last);
        if (active)
            run = 0;
        else
            ++run;
        if (run == MaxJournalPages)
        {
            w.safe_page = number + 1 - run;
            break;
        }
    }
    if (j.work_->count && !w.safe_page) return -ENOSPC;
    j.last_lsn_ = w.last_lsn;
    j.epoch_    = w.last_lsn >> bits;
    j.foreign_  = true;
    j.repair_   = false;
    return j.pack_images();
}
} // namespace xjntfs
