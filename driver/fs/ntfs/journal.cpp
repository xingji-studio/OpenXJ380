// SPDX-License-Identifier: Apache-2.0 AND MIT
// Original bounded writer; LFS/NTFS layout and recovery conventions reviewed
// against MIT DiscUtils. Retained notices and exact references: PROVENANCE.md.
#include "journal.h"
#include "format.h"
#include "recovery.h"
#include <errno.h>

namespace xjntfs {
using namespace format;
namespace {
constexpr unsigned DataOffset = 64, LfsHeader = 48, NtfsHeader = 32;
constexpr unsigned OpenAttribute = 0x1c, Commit = 0x1a, Forget = 0x1b;
constexpr uint64_t FirstRecord = uint64_t(FirstJournalPage) * JournalPage + DataOffset;

bool same(const uint8_t *a, const uint8_t *b, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (a[i] != b[i]) return false;
    return true;
}
} // namespace

int Journal::read(uint64_t offset, void *output, Size length)
{
    if (failed_ || offset > bytes_ || length > bytes_ - offset) return -EIO;
    if (device_.read(device_.context, offset, output, length)) return 0;
    failed_ = true;
    return -EIO;
}

int Journal::write(uint64_t offset, const void *input, Size length)
{
    if (failed_ || offset > bytes_ || length > bytes_ - offset) return -EIO;
    const auto *source = static_cast<const uint8_t *>(input);
    if (!(offset % sector_) && !(length % sector_))
    {
        if (device_.write(device_.context, offset, input, length)) return 0;
        failed_ = true;
        return -EIO;
    }
    while (length)
    {
        uint64_t aligned = offset / sector_ * sector_;
        unsigned within = unsigned(offset - aligned), amount = sector_ - within;
        if (amount > length) amount = unsigned(length);
        if (within || amount != sector_)
        {
            int error = read(aligned, work_->sector, sector_);
            if (error) return error;
            copy(work_->sector + within, source, amount);
        }
        else
            copy(work_->sector, source, amount);
        if (!device_.write(device_.context, aligned, work_->sector, sector_))
        {
            failed_ = true;
            return -EIO;
        }
        offset += amount;
        source += amount;
        length -= amount;
    }
    return 0;
}

int Journal::sync()
{
    if (failed_) return -EIO;
    if (device_.flush(device_.context)) return 0;
    failed_ = true;
    return -EIO;
}

int Journal::log_read(uint64_t offset, void *output, Size length)
{
    if (offset > log_.size || length > log_.size - offset) return -EIO;
    auto *out = static_cast<uint8_t *>(output);
    while (length)
    {
        const Run *found = nullptr;
        uint64_t   vcn   = offset / JournalPage;
        for (unsigned i = 0; i < log_.count; ++i)
            if (vcn >= log_.runs[i].vcn && vcn - log_.runs[i].vcn < log_.runs[i].length)
            {
                found = log_.runs + i;
                break;
            }
        if (!found || found->lcn < 0) return -EIO;
        uint64_t within    = offset - found->vcn * JournalPage;
        Size     amount    = length;
        uint64_t available = found->length * JournalPage - within;
        if (amount > available) amount = Size(available);
        int error = read(uint64_t(found->lcn) * JournalPage + within, out, amount);
        if (error) return error;
        offset += amount;
        out    += amount;
        length -= amount;
    }
    return 0;
}

int Journal::log_write(uint64_t offset, const void *input, Size length)
{
    if (offset > log_.size || length > log_.size - offset) return -EIO;
    const auto *source = static_cast<const uint8_t *>(input);
    while (length)
    {
        const Run *found = nullptr;
        uint64_t   vcn   = offset / JournalPage;
        for (unsigned i = 0; i < log_.count; ++i)
            if (vcn >= log_.runs[i].vcn && vcn - log_.runs[i].vcn < log_.runs[i].length)
            {
                found = log_.runs + i;
                break;
            }
        if (!found || found->lcn < 0) return -EIO;
        uint64_t within    = offset - found->vcn * JournalPage;
        Size     amount    = length;
        uint64_t available = found->length * JournalPage - within;
        if (amount > available) amount = Size(available);
        int error = write(uint64_t(found->lcn) * JournalPage + within, source, amount);
        if (error) return error;
        offset += amount;
        source += amount;
        length -= amount;
    }
    return 0;
}

bool Journal::overlaps_log(uint64_t offset, uint64_t length) const
{
    for (unsigned i = 0; i < log_.count; ++i)
    {
        uint64_t start = uint64_t(log_.runs[i].lcn) * JournalPage;
        uint64_t bytes = log_.runs[i].length * JournalPage;
        if (offset < start + bytes && start < offset + length) return true;
    }
    return false;
}

bool Journal::overlaps_mirror(uint64_t offset, uint64_t length) const
{
    uint64_t begin = mirror_lcn_ * JournalPage;
    return offset < begin + uint64_t(record_) * 4 && begin < offset + length;
}

int Journal::bootstrap()
{
    int error = read(0, work_->page, 512);
    if (error) return error;
    const uint8_t magic[8] = {'N', 'T', 'F', 'S', ' ', ' ', ' ', ' '};
    if (!same(work_->page + 3, magic, 8) || le16(work_->page + 510) != 0xaa55) return -EINVAL;
    sector_ = le16(work_->page + 11);
    if (device_.logical_sector && sector_ != device_.logical_sector) return -EINVAL;
    if ((sector_ != 512 && sector_ != JournalPage) || unsigned(work_->page[13]) * sector_ != JournalPage ||
        (work_->page[68] != 244 && work_->page[68] != 1))
        return -EOPNOTSUPP;
    int geometry = decode_size(work_->page[64], JournalPage, MaxRecord, record_);
    if (geometry || (record_ != 1024 && record_ != JournalPage) || record_ < sector_) return -EOPNOTSUPP;
    uint64_t sectors = le64(work_->page + 40);
    if (!sectors || sectors > device_.bytes / sector_) return -EIO;
    bytes_      = sectors * sector_;
    clusters_   = bytes_ / JournalPage;
    mft_lcn_    = le64(work_->page + 48);
    mirror_lcn_ = le64(work_->page + 56);
    if (!mft_lcn_ || !mirror_lcn_ || mft_lcn_ == mirror_lcn_ || mft_lcn_ >= clusters_ ||
        mirror_lcn_ > clusters_ - record_ * 4 / JournalPage)
        return -EIO;
    // Record 2 is in the fixed first-four-record prefix and never modified by
    // this writer. Either prefix can bootstrap recovery if record 0 is torn.
    error = read(mft_lcn_ * JournalPage + record_ * 2, work_->page, record_);
    if (!error) error = record_layout(work_->page, record_, 2, 0);
    if (error && !failed_)
    {
        error = read(mirror_lcn_ * JournalPage + record_ * 2, work_->page, record_);
        if (!error) error = record_layout(work_->page, record_, 2, 0);
    }
    if (error) return error;
    clear(&log_, sizeof(log_));
    unsigned  offset = le16(work_->page + 20);
    Attribute attribute;
    bool      found = false;
    while (!(error = next_attribute(work_->page, record_, offset, attribute)))
    {
        if (attribute.type == 0x20) return -EOPNOTSUPP;
        if (attribute.type != Data || attribute.data[9]) continue;
        error = append_fragment(attribute.data, attribute.length, log_, clusters_, JournalPage, bytes_, found);
        if (error) return error;
    }
    if (error != 1 || !found) return -EIO;
    error = complete_stream(log_, JournalPage);
    if (error) return error;
    if (log_.resident || log_.flags || log_.initialized != log_.size || log_.size < 1024 * 1024 ||
        log_.size % JournalPage || log_.size > 64 * 1024 * 1024)
        return -EOPNOTSUPP;
    for (unsigned i = 0; i < log_.count; ++i)
    {
        const Run &run = log_.runs[i];
        if (run.lcn <= 0 || uint64_t(run.lcn) + run.length >= clusters_ ||
            (uint64_t(run.lcn) <= mft_lcn_ && mft_lcn_ < uint64_t(run.lcn) + run.length) ||
            overlaps_mirror(uint64_t(run.lcn) * JournalPage, run.length * JournalPage))
            return -EIO;
        for (unsigned j = 0; j < i; ++j)
            if (uint64_t(run.lcn) < uint64_t(log_.runs[j].lcn) + log_.runs[j].length &&
                uint64_t(log_.runs[j].lcn) < uint64_t(run.lcn) + run.length)
                return -EIO;
    }
    return 0;
}

int Journal::read_restart(unsigned slot, bool &valid)
{
    valid     = false;
    int error = log_read(uint64_t(slot) * JournalPage, work_->restarts[slot], JournalPage);
    if (error) return error;
    uint8_t *page = work_->restarts[slot];
    if (unpack(page, JournalPage, 0x52545352)) return 0;
    unsigned ra = le16(page + 24);
    if (le32(page + 16) != JournalPage || le32(page + 20) != JournalPage || le16(page + 28) != 1 ||
        le16(page + 26) != 1 || ra != 64)
        return -EOPNOTSUPP;
    const uint8_t *area = page + ra;
    unsigned       bits = le32(area + 16);
    if (le16(area + 8) != 1 || le16(area + 20) != 208 || le16(area + 22) != 48 || le16(area + 36) != LfsHeader ||
        le16(area + 38) != DataOffset || bits < 16 || bits > 60 || le64(area + 24) > log_.size ||
        le64(area + 24) < (FirstJournalPage + MaxJournalPages) * JournalPage || le64(area + 24) % JournalPage ||
        (le16(area + 14) & ~3U))
        return -EOPNOTSUPP;
    if (le64(area + 24) / 8 > (uint64_t(1) << (64 - bits)) ||
        unsigned(le16(page + 4)) + unsigned(le16(page + 6)) * 2 > ra)
        return -EIO;
    uint64_t mask = (uint64_t(1) << (64 - bits)) - 1;
    if ((le64(area) & mask) >= le64(area + 24) / 8) return -EIO;
    const uint8_t *client = area + 48;
    // A fresh Windows log client can have sequence zero. Identity is checked
    // against each LFS record; this is distinct from a FILE reference sequence.
    if (le16(client + 16) != 0xffff || le16(client + 18) != 0xffff || le32(client + 28) != 8 ||
        le16(client + 32) != 'N' || le16(client + 34) != 'T' || le16(client + 36) != 'F' || le16(client + 38) != 'S')
        return -EIO;
    if (le16(area + 12) != 0xffff && (le64(client) > le64(area) || le64(client + 8) > le64(area) ||
                                      (le64(client) && (le64(client) & mask) >= le64(area + 24) / 8) ||
                                      (le64(client + 8) && (le64(client + 8) & mask) >= le64(area + 24) / 8)))
        return -EIO;
    if (!((le16(area + 10) == 0 && le16(area + 12) == 0xffff) || (le16(area + 10) == 0xffff && le16(area + 12) == 0)))
        return -EIO;
    valid = true;
    return 0;
}

int Journal::open(const WriteDevice &device, JournalWorkspace &workspace, RecoveryWorkspace *recovery)
{
    opened_ = failed_ = pending_ = fresh_ = repair_ = foreign_ = native_profile_ = false;
    record_                                                                      = 0;
    work_                                                                        = &workspace;
    device_                                                                      = device;
    bytes_                                                                       = device.bytes;
    recovery_                                                                    = recovery;
    checkpoint_page_                                                             = FirstJournalPage;
    work_->count = work_->attribute_count = 0;
    if (!device.read || !device.write || !device.flush || !device.bytes) return -EINVAL;
    int error = bootstrap();
    if (error) return error;
    error = open_native();
    if (error && !failed_ && !native_profile_ && recovery_ &&
        (error == -EOPNOTSUPP || error == -EIO || error == -EUCLEAN))
    {
        work_->count = work_->attribute_count = 0;
        error                                 = WindowsRecovery::open(*this, *recovery_);
    }
    return error;
}

int Journal::open_native()
{
    int  error;
    bool valid[2] = {};
    for (unsigned i = 0; i < 2; ++i)
    {
        error = read_restart(i, valid[i]);
        if (error) return error;
    }
    if (!valid[0] && !valid[1])
    {
        file_size_    = log_.size;
        unsigned bits = 0;
        for (uint64_t size = file_size_; size; size >>= 1)
            ++bits;
        sequence_bits_ = 67 - bits;
        last_lsn_ = epoch_ = 0;
        revision_          = 2;
        client_sequence_   = 1;
        selected_          = 0;
        // Initial creation has no old restart. Recognize only our canonical
        // initial clean image over FF: slot zero is FF, slot one has FF or
        // exact image bytes, and every data page is FF. No home write can have
        // begun in this state. Unknown initialized damage is never reset.
        make_restart(true, work_->page);
        error = pack(work_->page, JournalPage, work_->sector);
        if (error) return error;
        bool partial = false;
        for (unsigned byte = 0; byte < JournalPage; ++byte)
        {
            if (work_->restarts[0][byte] != 0xff) return -EIO;
            uint8_t value = work_->restarts[1][byte];
            if (value != 0xff && value != work_->sector[byte]) return -EIO;
            if (value != 0xff) partial = true;
        }
        for (uint64_t offset = 2 * JournalPage; offset < log_.size; offset += JournalPage)
        {
            error = log_read(offset, work_->page, JournalPage);
            if (error) return error;
            for (unsigned i = 0; i < JournalPage; ++i)
                if (work_->page[i] != 0xff) return -EIO;
        }
        revision_ = 1;
        fresh_    = true;
        repair_   = partial;
    }
    else
    {
        selected_ = valid[1] && (!valid[0] || le64(work_->restarts[1] + 64) > le64(work_->restarts[0] + 64));
        if (valid[0] && valid[1] && le64(work_->restarts[0] + 64) == le64(work_->restarts[1] + 64))
        {
            uint32_t diff = le32(work_->restarts[1] + 104) - le32(work_->restarts[0] + 104);
            if (diff == 0x80000000U) return -EIO;
            if (diff)
                selected_ = diff < 0x80000000U;
            else if (!same(work_->restarts[0] + 64, work_->restarts[1] + 64, 208))
                return -EIO;
        }
        const uint8_t *area = work_->restarts[selected_] + 64;
        file_size_          = le64(area + 24);
        sequence_bits_      = le32(area + 16);
        last_lsn_           = le64(area);
        revision_           = le32(area + 40);
        client_sequence_    = le16(area + 48 + 20);
        epoch_              = last_lsn_ >> (64 - sequence_bits_);
        pending_            = !(le16(area + 14) & 2) && le16(area + 12) != 0xffff;
        repair_             = !valid[0] || !valid[1] || !same(work_->restarts[0] + 64, work_->restarts[1] + 64, 208) ||
                  (!pending_ && le16(area + 12) != 0xffff);
        if (pending_)
        {
            error = scan();
            if (error) return error;
        }
    }
    opened_ = true;
    return 0;
}

void Journal::begin()
{
    if (!pending_) work_->count = work_->attribute_count = 0;
}

int Journal::add(MetadataKind kind, uint64_t reference, uint32_t type, bool named_index, uint64_t vcn, uint64_t lcn,
                 unsigned within, unsigned length, const uint8_t *before, MetadataImage *&image)
{
    image = nullptr;
    if (!opened_ || pending_ || failed_) return -EIO;
    if (!length || length > JournalPage || within > JournalPage - length || lcn >= clusters_ || !lcn) return -EIO;
    uint64_t start = lcn * JournalPage + within;
    if (start < uint64_t(JournalPage) * 2 || start + length > (clusters_ - 1) * JournalPage ||
        overlaps_log(start, length) || overlaps_mirror(start, length))
        return -EIO;
    for (unsigned i = 0; i < work_->count; ++i)
    {
        MetadataImage &other = work_->images[i];
        uint64_t       base  = physical(other);
        if (base == start && other.length == length && other.kind == kind && other.file_reference == reference &&
            other.attribute_type == type && other.vcn == vcn)
        {
            image = &other;
            return 0;
        }
        if (base < start + length && start < base + other.length) return -EIO;
    }
    if (work_->count == MaxMetadataImages) return -EOPNOTSUPP;
    unsigned target = 0;
    for (; target < work_->attribute_count; ++target)
    {
        const auto &attribute = work_->attributes[target];
        if (attribute.reference == reference && attribute.type == type && attribute.named_index == named_index) break;
    }
    if (target == work_->attribute_count)
    {
        auto &entry = work_->attributes[work_->attribute_count++];
        clear(&entry, sizeof(entry));
        entry.reference   = reference;
        entry.type        = type;
        entry.named_index = named_index;
        if (named_index)
        {
            entry.name_length = 4;
            entry.name[0]     = '$';
            entry.name[1]     = 'I';
            entry.name[2]     = '3';
            entry.name[3]     = '0';
        }
    }
    image                 = work_->images + work_->count++;
    image->kind           = kind;
    image->file_reference = reference;
    image->attribute_type = type;
    image->named_index    = named_index;
    image->vcn            = vcn;
    image->lcn            = lcn;
    image->within         = uint16_t(within);
    image->length         = uint16_t(length);
    image->target         = uint16_t(target);
    image->lsn            = 0;
    image->name_length    = work_->attributes[target].name_length;
    for (unsigned n = 0; n < image->name_length; ++n)
        image->name[n] = work_->attributes[target].name[n];
    copy(image->before, before, length);
    copy(image->after, before, length);
    return 0;
}

uint64_t Journal::make_lsn(unsigned page) const
{
    return (epoch_ << (64 - sequence_bits_)) | ((uint64_t(page) * JournalPage + DataOffset) / 8);
}

int Journal::append(unsigned type, uint16_t redo, uint16_t undo, unsigned target, const MetadataImage *image,
                    const uint8_t *redo_data, unsigned redo_size, const uint8_t *undo_data, unsigned undo_size,
                    uint64_t &lsn)
{
    unsigned lcns    = image ? 1 : 0;
    unsigned redo_at = NtfsHeader + lcns * 8;
    // Windows keeps the otherwise unused first LCN slot in control records.
    // The minimum client header is therefore 40 bytes even with no mapped LCN.
    if (redo_at < 40) redo_at = 40;
    unsigned undo_at = (redo_at + redo_size + 7) & ~7U;
    unsigned payload = type == 2 ? redo_size : undo_at + undo_size;
    unsigned pages   = (LfsHeader + payload + (JournalPage - DataOffset) - 1) / (JournalPage - DataOffset);
    if (!pages || pages > 3 || next_page_ + pages > checkpoint_page_ + MaxJournalPages ||
        uint64_t(next_page_ + pages) * JournalPage > file_size_)
        return -ENOSPC;
    unsigned data_size = pages * (JournalPage - DataOffset) - LfsHeader;
    clear(work_->wire, sizeof(work_->wire));
    lsn = make_lsn(next_page_);
    put64(work_->wire, lsn);
    // A client restart record is outside the metadata transaction chain.
    uint64_t transaction_previous = previous_ == make_lsn(checkpoint_page_) ? 0 : previous_;
    put64(work_->wire + 8, type == 2 ? 0 : transaction_previous);
    put64(work_->wire + 16, type == 2 || redo == Commit || redo == Forget ? 0 : transaction_previous);
    put32(work_->wire + 24, data_size);
    put16(work_->wire + 28, client_sequence_);
    put16(work_->wire + 30, 0);
    put32(work_->wire + 32, type);
    put32(work_->wire + 36, type == 2 ? 0 : 0x18);
    unsigned flags = pages > 1 ? 1 : 0;
    if (type == 1)
    {
        // Empty buffers also need the LFS zero-data flags. In particular,
        // Windows rejects an OPEN with absent undo or a COMMIT with absent
        // redo when these flags are missing. A compensation marker has no
        // ordinary undo buffer.
        if (!redo_size) flags |= 2;
        if (!undo_size && undo != 1) flags |= 4;
    }
    put16(work_->wire + 40, uint16_t(flags));
    uint8_t *data = work_->wire + LfsHeader;
    if (type == 2)
        copy(data, redo_data, redo_size);
    else
    {
        put16(data, redo);
        put16(data + 2, undo);
        put16(data + 4, uint16_t(redo_at));
        put16(data + 6, uint16_t(redo_size));
        put16(data + 8, uint16_t(undo_at));
        put16(data + 10, uint16_t(undo_size));
        put16(data + 12, uint16_t(24 + target * 40));
        put16(data + 14, uint16_t(lcns));
        unsigned blocks = record_ / 512;
        if (image)
        {
            blocks = image->kind == MetadataKind::FileRecord    ? record_ / 512
                     : image->kind == MetadataKind::IndexBuffer ? JournalPage / 512
                                                                : 0;
            put16(data + 20, uint16_t(image->within / 512));
            put64(data + 24, image->vcn);
            put64(data + NtfsHeader, image->lcn);
        }
        else
        {
            put64(data + NtfsHeader, ~uint64_t(0));
            if (redo == OpenAttribute)
            {
                const auto &attribute = work_->attributes[target];
                blocks                = attribute.type == IndexAllocation ? JournalPage / 512
                                        : attribute.type == Data && !(attribute.reference & NumberMask) ? record_ / 512
                                                                                                        : 0;
            }
        }
        // Observed MST size hint, in the format's fixed 512-byte units.
        put16(data + 22, uint16_t(blocks));
        if (redo_size) copy(data + redo_at, redo_data, redo_size);
        if (undo_size) copy(data + undo_at, undo_data, undo_size);
    }
    unsigned copied = 0, total = LfsHeader + data_size;
    for (unsigned i = 0; i < pages; ++i)
    {
        clear(work_->page, JournalPage);
        put32(work_->page, 0x44524352);
        put16(work_->page + 4, 40);
        put16(work_->page + 6, 9);
        put64(work_->page + 8, lsn);
        put32(work_->page + 16, i + 1 == pages ? 1 : 0);
        put16(work_->page + 20, uint16_t(pages));
        put16(work_->page + 22, uint16_t(i + 1));
        put16(work_->page + 24, JournalPage);
        put64(work_->page + 32, i + 1 == pages ? lsn : 0);
        put16(work_->page + 40, uint16_t((epoch_ + next_page_ + i) % 65534 + 1));
        unsigned amount = total - copied;
        if (amount > JournalPage - DataOffset) amount = JournalPage - DataOffset;
        copy(work_->page + DataOffset, work_->wire + copied, amount);
        copied    += amount;
        int error  = pack(work_->page, JournalPage, work_->sector);
        if (!error) error = log_write(uint64_t(next_page_ + i) * JournalPage, work_->sector, JournalPage);
        if (error) return error;
    }
    previous_   = lsn;
    next_page_ += pages;
    return 0;
}

int Journal::make_restart(bool clean, uint8_t *page)
{
    clear(page, JournalPage);
    put32(page, 0x52545352);
    put16(page + 4, 32);
    put16(page + 6, 9);
    put32(page + 16, JournalPage);
    put32(page + 20, JournalPage);
    put16(page + 24, 64);
    put16(page + 26, 1);
    put16(page + 28, 1);
    put16(page + 32, uint16_t(revision_ % 65534 + 1));
    uint8_t *area = page + 64;
    put64(area, last_lsn_);
    put16(area + 8, 1);
    put16(area + 10, clean ? 0 : 0xffff);
    put16(area + 12, clean ? 0xffff : 0);
    put16(area + 14, clean ? 2 : 0);
    put32(area + 16, sequence_bits_);
    put16(area + 20, 208);
    put16(area + 22, 48);
    put64(area + 24, file_size_);
    put32(area + 32, JournalPage - DataOffset - LfsHeader);
    put16(area + 36, LfsHeader);
    put16(area + 38, DataOffset);
    put32(area + 40, revision_);
    uint8_t *client     = area + 48;
    uint64_t checkpoint = last_lsn_ ? make_lsn(checkpoint_page_) : 0;
    put64(client, checkpoint);
    put64(client + 8, checkpoint);
    put16(client + 16, 0xffff);
    put16(client + 18, 0xffff);
    put16(client + 20, client_sequence_);
    put32(client + 28, 8);
    put16(client + 32, 'N');
    put16(client + 34, 'T');
    put16(client + 36, 'F');
    put16(client + 38, 'S');
    return 0;
}

int Journal::publish(bool clean)
{
    ++revision_;
    make_restart(clean, work_->page);
    int error = pack(work_->page, JournalPage, work_->sector);
    if (error) return error;
    // Alternate first: a complete known restart always survives an interrupted
    // replacement. Home writes never precede a durable dirty restart.
    for (unsigned i = 0; i < 2; ++i)
    {
        unsigned slot = i ? selected_ : 1 - selected_;
        error         = log_write(uint64_t(slot) * JournalPage, work_->sector, JournalPage);
        if (!error) error = sync();
        if (error) return error;
    }
    repair_ = fresh_ = false;
    pending_         = !clean;
    if (clean) foreign_ = false;
    return 0;
}

int Journal::pack_images()
{
    for (unsigned i = 0; i < work_->count; ++i)
    {
        MetadataImage &image = work_->images[i];
        if (image.kind != MetadataKind::Raw)
        {
            put64(image.after + 8, image.lsn);
            int error = pack(image.after, image.length, image.packed);
            if (error) return error;
        }
        else
            copy(image.packed, image.after, image.length);
    }
    return 0;
}

int Journal::apply()
{
    // Mirrored MFT records must reach media before their primary copies.
    bool mirrored = false;
    for (unsigned i = 0; i < work_->count; ++i)
    {
        const MetadataImage &image      = work_->images[i];
        bool                 raw_prefix = image.kind == MetadataKind::Raw && image.attribute_type == Data &&
                          (image.file_reference & NumberMask) == 0 && image.vcn < record_ * 4 / JournalPage &&
                          !image.within && image.length == JournalPage;
        if (image.kind != MetadataKind::FileRecord && !raw_prefix) continue;
        uint64_t number = image.vcn * (JournalPage / record_) + image.within / record_;
        if (number < 4)
        {
            int error = write(mirror_lcn_ * JournalPage + number * record_, image.packed, image.length);
            if (error) return error;
            mirrored = true;
        }
    }
    int error = mirrored ? sync() : 0;
    for (unsigned i = 0; !error && i < work_->count; ++i)
        error = write(physical(work_->images[i]), work_->images[i].packed, work_->images[i].length);
    if (!error) error = sync();
    if (!error) error = publish(true);
    return error;
}

int Journal::commit(JournalValidator validate, void *context)
{
    return emit(validate, context, false);
}

int Journal::emit(JournalValidator validate, void *context, bool importing)
{
    if (!opened_ || failed_ || (pending_ && !importing) || !validate) return -EIO;
    checkpoint_page_ = importing ? recovery_->safe_page : FirstJournalPage;
    if (!work_->count) return 0;
    // Include the file-size endpoint in the LFS offset width. Older bounded
    // profiles remain readable; the new commit always uses canonical widths.
    unsigned size_bits = 0;
    for (uint64_t size = file_size_; size; size >>= 1)
        ++size_bits;
    sequence_bits_   = 67 - size_bits;
    uint64_t maximum = last_lsn_;
    for (unsigned i = 0; i < work_->count; ++i)
    {
        const auto &image = work_->images[i];
        if (work_->images[i].kind != MetadataKind::Raw &&
            (le32(work_->images[i].before) == 0x454c4946 || le32(work_->images[i].before) == 0x58444e49) &&
            le64(work_->images[i].before + 8) > maximum)
            maximum = le64(work_->images[i].before + 8);
        if (image.kind == MetadataKind::Raw && image.attribute_type == Data && !(image.file_reference & NumberMask) &&
            !image.name_length)
        {
            // Foreign recovery groups four FILE records into a physical MFT
            // cluster. An unchanged neighbor can carry a later page LSN.
            for (unsigned at = 0; at + record_ <= image.length; at += record_)
                if (le32(image.before + at) == 0x454c4946 && le64(image.before + at + 8) > maximum)
                    maximum = le64(image.before + at + 8);
        }
    }
    epoch_ = (maximum >> (64 - sequence_bits_)) + 1;
    if (!epoch_ || epoch_ >= (uint64_t(1) << sequence_bits_)) return -EOVERFLOW;
    // Compute exact log reservation before issuing any write or flush.
    unsigned pages = checkpoint_page_ + 1 + work_->attribute_count;
    for (unsigned i = 0; i < work_->count; ++i)
    {
        MetadataImage &image = work_->images[i];
        image.lsn            = make_lsn(pages);
        unsigned undo_size =
            image.kind == MetadataKind::FileRecord && le32(image.before) != 0x454c4946 ? 0 : image.length;
        unsigned payload  = NtfsHeader + 8 + image.length + undo_size;
        pages            += (LfsHeader + payload + JournalPage - DataOffset - 1) / (JournalPage - DataOffset);
    }
    pages += 2;
    if (pages > checkpoint_page_ + MaxJournalPages || uint64_t(pages) * JournalPage > file_size_) return -ENOSPC;
    int error = pack_images();
    if (!error) error = validate(context, *work_);
    if (error) return error;
    if (!importing && (fresh_ || repair_))
    {
        uint64_t next_epoch = epoch_;
        epoch_              = last_lsn_ >> (64 - sequence_bits_);
        error               = publish(true);
        epoch_              = next_epoch;
        if (error) return error;
    }
    // A cleared, durable window prevents stale sector trailers from a much
    // older log epoch from masquerading as a complete new record after a tear.
    clear(work_->page, JournalPage);
    unsigned clear_begin = importing ? checkpoint_page_ : 2;
    for (unsigned page = clear_begin; page < checkpoint_page_ + MaxJournalPages; ++page)
    {
        error = log_write(uint64_t(page) * JournalPage, work_->page, JournalPage);
        if (error) return error;
    }
    error = sync();
    if (error) return error;
    next_page_   = checkpoint_page_;
    previous_    = 0;
    uint64_t lsn = make_lsn(checkpoint_page_);
    clear(work_->sector, JournalPage);
    put32(work_->sector, 1);
    put64(work_->sector + 8, lsn);
    put32(work_->sector + 80, JournalPage);
    error = append(2, 0, 0, 0, nullptr, work_->sector, 112, nullptr, 0, lsn);
    if (error) return error;
    for (unsigned i = 0; i < work_->attribute_count; ++i)
    {
        const auto &attribute = work_->attributes[i];
        clear(work_->sector, JournalPage);
        put32(work_->sector, 0xffffffff);
        put32(work_->sector + 4, attribute.named_index ? JournalPage : 0);
        put32(work_->sector + 8, attribute.type);
        put64(work_->sector + 16, attribute.reference);
        // The OPEN payload records the preceding global LSN, which may be a
        // client restart even though the first transaction previous-LSN is 0.
        put64(work_->sector + 24, previous_);
        uint8_t name[MaxJournalName * 2] = {};
        for (unsigned n = 0; n < attribute.name_length; ++n)
            put16(name + n * 2, attribute.name[n]);
        error = append(1, OpenAttribute, 0, i, nullptr, work_->sector, 40, name, attribute.name_length * 2, lsn);
        if (error) return error;
    }
    for (unsigned i = 0; i < work_->count; ++i)
    {
        MetadataImage &image     = work_->images[i];
        uint16_t       operation = image.kind == MetadataKind::FileRecord ? 2 : 8;
        uint16_t       undo      = operation;
        unsigned       undo_size = image.length;
        if (image.kind == MetadataKind::FileRecord && le32(image.before) != 0x454c4946)
        {
            undo      = 3;
            undo_size = 0;
        }
        error =
            append(1, operation, undo, image.target, &image, image.after, image.length, image.before, undo_size, lsn);
        if (error) return error;
        if (lsn != image.lsn)
        {
            failed_ = true;
            return -EIO;
        }
    }
    error = append(1, Commit, 1, 0, nullptr, nullptr, 0, nullptr, 0, lsn);
    if (!error) error = append(1, Forget, 1, 0, nullptr, nullptr, 0, nullptr, 0, lsn);
    if (!error) error = sync();
    if (error) return error;
    last_lsn_ = lsn;
    error     = publish(false);
    if (!error) error = apply();
    return error;
}

int Journal::read_record(uint64_t lsn, unsigned &type, unsigned &next_page)
{
    uint64_t mask   = (uint64_t(1) << (64 - sequence_bits_)) - 1;
    uint64_t offset = (lsn & mask) * 8;
    if (offset % JournalPage != DataOffset || offset / JournalPage < checkpoint_page_ ||
        offset / JournalPage >= checkpoint_page_ + MaxJournalPages)
        return -EOPNOTSUPP;
    unsigned page  = unsigned(offset / JournalPage);
    int      error = log_read(uint64_t(page) * JournalPage, work_->page, JournalPage);
    if (!error) error = unpack(work_->page, JournalPage, 0x44524352);
    if (error) return error;
    const uint8_t *header = work_->page + DataOffset;
    if (le64(header) != lsn || le16(header + 28) != client_sequence_ || le16(header + 30)) return -EIO;
    unsigned data = le32(header + 24), pages = (data + LfsHeader) / (JournalPage - DataOffset);
    if (!pages || pages > 3 || data + LfsHeader != pages * (JournalPage - DataOffset) ||
        page + pages > checkpoint_page_ + MaxJournalPages)
        return -EOPNOTSUPP;
    type = le32(header + 32);
    if ((type != 1 && type != 2) || (type == 1 && le32(header + 36) != 0x18)) return -EOPNOTSUPP;
    unsigned copied = 0;
    for (unsigned i = 0; i < pages; ++i)
    {
        if (i)
        {
            error = log_read(uint64_t(page + i) * JournalPage, work_->page, JournalPage);
            if (!error) error = unpack(work_->page, JournalPage, 0x44524352);
            if (error) return error;
        }
        if (le64(work_->page + 8) != lsn || le16(work_->page + 20) != pages || le16(work_->page + 22) != i + 1 ||
            le16(work_->page + 24) != JournalPage || le32(work_->page + 16) != (i + 1 == pages ? 1U : 0U))
            return -EIO;
        copy(work_->wire + copied, work_->page + DataOffset, JournalPage - DataOffset);
        copied += JournalPage - DataOffset;
    }
    next_page = page + pages;
    return 0;
}

int Journal::scan()
{
    const uint8_t *client     = work_->restarts[selected_] + 112;
    uint64_t       checkpoint = le64(client + 8);
    epoch_                    = last_lsn_ >> (64 - sequence_bits_);
    uint64_t mask             = (uint64_t(1) << (64 - sequence_bits_)) - 1;
    uint64_t offset           = (checkpoint & mask) * 8;
    checkpoint_page_          = unsigned(offset / JournalPage);
    if (checkpoint_page_ < FirstJournalPage || offset % JournalPage != DataOffset ||
        uint64_t(checkpoint_page_ + MaxJournalPages) * JournalPage > file_size_ ||
        checkpoint != make_lsn(checkpoint_page_) || le64(client) != checkpoint || !epoch_)
        return -EOPNOTSUPP;
    uint64_t lsn = checkpoint, previous = 0;
    bool     committed = false, forgotten = false;
    unsigned records = 0;
    while (lsn <= last_lsn_)
    {
        unsigned type, next;
        int      error = read_record(lsn, type, next);
        if (error) return error;
        uint8_t *data = work_->wire + LfsHeader;
        if (!records)
        {
            // Canonical restarts plus a whole-page-padded client checkpoint
            // identify our bounded profile. Once recognized, a broken native
            // COMMIT/chain must fail closed, rather than being reinterpreted
            // as a foreign ordinary transaction ending with terminal FORGET.
            native_profile_ = type == 2 && le32(work_->wire + 24) == JournalPage - DataOffset - LfsHeader;
            if (type != 2 || le32(data) != 1 || le32(data + 4) || le64(data + 8) != checkpoint || le64(data + 16) ||
                le64(data + 24) || le64(data + 32) || le64(data + 40) || le32(data + 48) || le32(data + 52) ||
                le32(data + 56) || le32(data + 60) || le32(data + 80) != JournalPage)
                return -EOPNOTSUPP;
        }
        else
        {
            if (type != 1 || (le64(work_->wire + 8) != previous && !(records == 1 && !le64(work_->wire + 8))) ||
                forgotten)
                return -EIO;
            unsigned redo = le16(data), undo = le16(data + 2), redo_at = le16(data + 4);
            unsigned redo_size = le16(data + 6), undo_at = le16(data + 8), undo_size = le16(data + 10);
            unsigned target = le16(data + 12), lcns = le16(data + 14);
            unsigned size = le32(work_->wire + 24);
            if ((redo_at & 7) || (undo_at & 7) || redo_at < NtfsHeader + lcns * 8 || redo_at > size ||
                redo_size > size - redo_at || undo_at > size || undo_size > size - undo_at)
                return -EIO;
            if (redo == OpenAttribute && !committed)
            {
                if (lcns || redo_size != 40 || undo_size > MaxJournalName * 2 || undo_size % 2 ||
                    target != 24 + work_->attribute_count * 40 || work_->attribute_count == MaxMetadataImages ||
                    work_->count || undo)
                    return -EOPNOTSUPP;
                JournalAttribute &attribute = work_->attributes[work_->attribute_count++];
                attribute.reference         = le64(data + redo_at + 16);
                attribute.type              = le32(data + redo_at + 8);
                attribute.name_length       = undo_size / 2;
                for (unsigned n = 0; n < attribute.name_length; ++n)
                    attribute.name[n] = le16(data + undo_at + n * 2);
                attribute.named_index = attribute.name_length == 4 && attribute.name[0] == '$' &&
                                        attribute.name[1] == 'I' && attribute.name[2] == '3' &&
                                        attribute.name[3] == '0';
                // Retain v4 logs whose OPEN payload used its own LSN, and
                // accept the observed preceding-global-LSN form emitted now.
                if (!uint16_t(attribute.reference >> 48) || le32(data + redo_at) != 0xffffffffU ||
                    (le64(data + redo_at + 24) != lsn && le64(data + redo_at + 24) != previous))
                    return -EIO;
            }
            else if ((redo == 2 || redo == 8) && !committed)
            {
                if (lcns != 1 || target < 24 || (target - 24) % 40 || (target - 24) / 40 >= work_->attribute_count ||
                    work_->count == MaxMetadataImages || !redo_size || redo_size > JournalPage ||
                    (undo != redo && !(redo == 2 && undo == 3)) || (undo == redo && undo_size != redo_size))
                    return -EOPNOTSUPP;
                auto       &image     = work_->images[work_->count++];
                const auto &attribute = work_->attributes[(target - 24) / 40];
                image.file_reference  = attribute.reference;
                image.attribute_type  = attribute.type;
                image.named_index     = attribute.named_index;
                image.name_length     = attribute.name_length;
                for (unsigned n = 0; n < image.name_length; ++n)
                    image.name[n] = attribute.name[n];
                image.target = uint16_t((target - 24) / 40);
                image.lcn    = le64(data + NtfsHeader);
                image.vcn    = le64(data + 24);
                image.within = uint16_t(le16(data + 20) * 512);
                image.length = uint16_t(redo_size);
                image.lsn    = lsn;
                image.kind   = redo == 2                           ? MetadataKind::FileRecord
                               : attribute.type == IndexAllocation ? MetadataKind::IndexBuffer
                                                                   : MetadataKind::Raw;
                if (le16(data + 16) || le16(data + 18) || image.within > JournalPage - image.length || !image.lcn ||
                    image.lcn >= clusters_ ||
                    (image.kind == MetadataKind::FileRecord &&
                     (image.length != record_ || image.attribute_type != Data ||
                      (image.file_reference & NumberMask) != 0 || image.within % record_)) ||
                    (image.kind == MetadataKind::IndexBuffer &&
                     (image.length != JournalPage || image.within || image.attribute_type != IndexAllocation)) ||
                    (image.kind == MetadataKind::Raw &&
                     !(image.attribute_type == Data || image.attribute_type == Bitmap || image.attribute_type == 0x20)))
                    return -EOPNOTSUPP;
                uint64_t address = physical(image);
                if (address < uint64_t(JournalPage) * 2 || address + image.length > (clusters_ - 1) * JournalPage ||
                    overlaps_log(address, image.length) || overlaps_mirror(address, image.length))
                    return -EIO;
                for (unsigned j = 0; j + 1 < work_->count; ++j)
                    if (physical(work_->images[j]) < address + image.length &&
                        address < physical(work_->images[j]) + work_->images[j].length)
                        return -EIO;
                copy(image.after, data + redo_at, image.length);
                clear(image.before, image.length);
                if (undo_size) copy(image.before, data + undo_at, undo_size);
                if (image.kind != MetadataKind::Raw)
                {
                    uint32_t signature = image.kind == MetadataKind::FileRecord ? 0x454c4946 : 0x58444e49;
                    if (le32(image.after) != signature || le64(image.after + 8) != lsn) return -EIO;
                }
            }
            else if (redo == Commit && !committed && work_->count && !lcns && !redo_size && !undo_size && undo == 1)
                committed = true;
            else if (redo == Forget && committed && !lcns && !redo_size && !undo_size && undo == 1)
                forgotten = true;
            else
                return -EOPNOTSUPP;
        }
        ++records;
        if (lsn == last_lsn_) break;
        previous = lsn;
        lsn      = make_lsn(next);
        if (lsn <= previous || lsn > last_lsn_) return -EIO;
    }
    if (!forgotten) return -EOPNOTSUPP;
    return pack_images();
}

int Journal::recover(JournalValidator validate, void *context)
{
    if (!opened_ || failed_ || !validate) return -EIO;
    int error = validate(context, *work_);
    if (error) return error;
    if (foreign_)
    {
        if (!work_->count)
        {
            error = sync();
            if (!error) error = publish(true);
            return error;
        }
        return emit(validate, context, true);
    }
    if (pending_) return apply();
    if (repair_) return publish(true);
    return 0;
}

} // namespace xjntfs
