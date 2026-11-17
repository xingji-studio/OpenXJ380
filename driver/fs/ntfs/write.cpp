// SPDX-License-Identifier: Apache-2.0
// Original bounded allocation/namespace implementation. See PROVENANCE.md.
#include "write.h"
#include "format.h"
#include <errno.h>
namespace xjntfs {
using namespace format;
namespace {
unsigned aligned(unsigned n)
{
    return (n + 7) & ~7U;
}

bool bit(const uint8_t *bits, uint64_t number)
{
    return (bits[number / 8] >> (number % 8)) & 1;
}

void set_bit(uint8_t *bits, uint64_t number, bool value)
{
    if (value)
        bits[number / 8] |= uint8_t(1U << (number % 8));
    else
        bits[number / 8] &= uint8_t(~(1U << (number % 8)));
}

bool bytes_equal(const uint8_t *a, const uint8_t *b, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

bool passive_root_txf(const Attribute &a, uint64_t reference)
{
    // Windows FORMAT leaves this resident initializer on the root even when
    // no TxF transaction has used it. Preserve its opaque identifier; accept
    // only the observed empty-state form, never transaction recovery state.
    const uint8_t *p      = a.data;
    const char     name[] = "$TXF_DATA";
    if (a.type != 0x100 || p[8] || p[9] != 9 || le16(p + 12) || le32(p + 16) != 56) return false;
    for (unsigned n = 0; n < 9; ++n)
        if (le16(p + le16(p + 10) + n * 2) != unsigned(name[n])) return false;
    const uint8_t *value = p + le16(p + 20);
    if (le64(value) != reference || le64(value + 48) != 2) return false;
    for (unsigned n = 16; n < 48; ++n)
        if (value[n]) return false;
    return true;
}

int preserve_root_utility(const uint8_t *before, const uint8_t *after, unsigned bytes)
{
    unsigned  old_count = 0, new_count = 0, at = le16(before + 20);
    Attribute a;
    int       error;
    while (!(error = next_attribute(before, bytes, at, a)))
    {
        if (a.type != 0x100) continue;
        if (!passive_root_txf(a, 5 | uint64_t(le16(before + 16)) << 48)) return -EOPNOTSUPP;
        ++old_count;
        unsigned  next_at = le16(after + 20), matches = 0;
        Attribute candidate;
        int       status;
        while (!(status = next_attribute(after, bytes, next_at, candidate)))
            if (candidate.type == a.type && le16(candidate.data + 14) == le16(a.data + 14) &&
                candidate.length == a.length && bytes_equal(a.data, candidate.data, a.length))
                ++matches;
        if (status != 1 || matches != 1) return -EOPNOTSUPP;
    }
    if (error != 1) return error;
    at = le16(after + 20);
    while (!(error = next_attribute(after, bytes, at, a)))
        if (a.type == 0x100) ++new_count;
    return error != 1 ? error : old_count == new_count ? 0 : -EOPNOTSUPP;
}

uint8_t *attribute(uint8_t *record, uint32_t type, bool named = false)
{
    unsigned  at = le16(record + 20);
    Attribute a;
    while (!next_attribute(record, le32(record + 28), at, a))
        if (a.type == type && (named ? named_i30(a.data) : !a.data[9])) return const_cast<uint8_t *>(a.data);
    return nullptr;
}

int erase(uint8_t *record, uint8_t *p)
{
    unsigned offset = unsigned(p - record), length = le32(p + 4), used = le32(record + 24);
    if (offset > used || length > used - offset) return -EIO;
    copy(p, p + length, used - offset - length);
    clear(record + used - length, length);
    put32(record + 24, used - length);
    return 0;
}

int insert(uint8_t *record, const uint8_t *value, unsigned length)
{
    unsigned used = le32(record + 24), at = le16(record + 20);
    if (length > le32(record + 28) - used) return -EOPNOTSUPP;
    while (at + 4 <= used && le32(record + at) <= le32(value))
    {
        // The unnamed stream precedes named streams of the same attribute
        // type. Replacing DATA must not move it behind a retained named DATA.
        if (le32(record + at) == le32(value) && !value[9] && record[at + 9]) break;
        at += le32(record + at + 4);
    }
    if (at + 4 > used) return -EIO;
    for (unsigned i = used; i > at; --i)
        record[i + length - 1] = record[i - 1];
    copy(record + at, value, length);
    put32(record + 24, used + length);
    return 0;
}

unsigned resident(uint8_t *out, uint32_t type, bool named, const uint8_t *value, unsigned size, unsigned instance)
{
    unsigned start = named ? 32 : 24, length = aligned(start + size);
    clear(out, length);
    put32(out, type);
    put32(out + 4, length);
    put16(out + 14, uint16_t(instance));
    put32(out + 16, size);
    put16(out + 20, uint16_t(start));
    if (named)
    {
        out[9] = 4;
        put16(out + 10, 24);
        put16(out + 24, '$');
        put16(out + 26, 'I');
        put16(out + 28, '3');
        put16(out + 30, '0');
    }
    if (size) copy(out + start, value, size);
    if (type == 0x30) out[22] = 1; // Indexed FILE_NAME attribute.
    return length;
}

unsigned nonresident(uint8_t *out, uint32_t type, bool named, const Run *runs, unsigned count, uint64_t size,
                     uint64_t allocated, unsigned instance)
{
    clear(out, JournalPage);
    unsigned at = named ? 72 : 64;
    put32(out, type);
    out[8] = 1;
    put16(out + 14, uint16_t(instance));
    put16(out + 32, uint16_t(at));
    put64(out + 24, allocated / JournalPage ? allocated / JournalPage - 1 : 0);
    put64(out + 40, allocated);
    put64(out + 48, size);
    put64(out + 56, size);
    if (named)
    {
        out[9] = 4;
        put16(out + 10, 64);
        put16(out + 64, '$');
        put16(out + 66, 'I');
        put16(out + 68, '3');
        put16(out + 70, '0');
    }
    int64_t last = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned n = 1, d = 1;
        while (n < 8 && (runs[i].length >> (n * 8)))
            ++n;
        int64_t delta = runs[i].lcn - last;
        while (d < 8 && (delta < -(int64_t(1) << (d * 8 - 1)) || delta >= (int64_t(1) << (d * 8 - 1))))
            ++d;
        if (at + 1 + n + d + 1 > JournalPage) return 0;
        out[at++] = uint8_t(n | d << 4);
        for (unsigned j = 0; j < n; ++j)
            out[at++] = uint8_t(runs[i].length >> (j * 8));
        for (unsigned j = 0; j < d; ++j)
            out[at++] = uint8_t(uint64_t(delta) >> (j * 8));
        last = runs[i].lcn;
    }
    out[at++]       = 0;
    unsigned length = aligned(at);
    put32(out + 4, length);
    return length;
}

// Compare physical ownership of a stream prefix, allowing appended runs but
// never moving any previously addressable MFT or bitmap byte.
bool preserves(const Stream &before, const Stream &after)
{
    if (before.resident || after.resident) return false;
    unsigned j = 0;
    for (unsigned i = 0; i < before.count; ++i)
    {
        const Run &old = before.runs[i];
        uint64_t   at = old.vcn, left = old.length;
        if (old.lcn < 0) return false;
        while (left)
        {
            while (j < after.count && after.runs[j].vcn + after.runs[j].length <= at)
                ++j;
            if (j == after.count) return false;
            const Run &next = after.runs[j];
            if (next.lcn < 0 || next.vcn > at || uint64_t(next.lcn) + at - next.vcn != uint64_t(old.lcn) + at - old.vcn)
                return false;
            uint64_t amount = next.vcn + next.length - at;
            if (amount > left) amount = left;
            at   += amount;
            left -= amount;
        }
    }
    return true;
}

void new_record(uint8_t *p, uint64_t number, uint16_t sequence, bool directory, unsigned bytes)
{
    clear(p, bytes);
    put32(p, 0x454c4946);
    put16(p + 4, 48);
    put16(p + 6, uint16_t(bytes / 512 + 1));
    put16(p + 48, 1);
    put16(p + 16, sequence);
    put16(p + 18, 1);
    unsigned start = aligned(48 + (bytes / 512 + 1) * 2);
    put16(p + 20, uint16_t(start));
    put16(p + 22, directory ? 3 : 1);
    put32(p + 24, start + 8);
    put32(p + 28, bytes);
    put32(p + 44, uint32_t(number));
    put32(p + start, 0xffffffff);
}

// Instance values are local to one FILE record. Preserve an available value
// when moving an attribute, otherwise choose a bounded unused value.
unsigned free_instance(const uint8_t *p, unsigned preferred = 0)
{
    for (unsigned trial = 0; trial <= MaxRecord / 24; ++trial)
    {
        unsigned  id = trial ? trial - 1 : preferred, at = le16(p + 20);
        Attribute a;
        bool      used = false;
        while (!next_attribute(p, le32(p + 28), at, a))
            if (le16(a.data + 14) == id) used = true;
        if (!used) return id;
    }
    return 65536;
}

uint8_t *instance(uint8_t *p, const MftListEntry &entry)
{
    unsigned  at = le16(p + 20);
    Attribute a;
    while (!next_attribute(p, le32(p + 28), at, a))
        if (a.type == le32(entry.data) && le16(a.data + 14) == le16(entry.data + 24) &&
            same_attribute_name(a.data, entry.data))
            return const_cast<uint8_t *>(a.data);
    return nullptr;
}

unsigned fragment(uint8_t *out, uint32_t type, const Stream &stream, uint64_t low, uint64_t end, unsigned id,
                  Run *scratch)
{
    unsigned count = 0;
    for (unsigned n = 0; n < stream.count; ++n)
    {
        const Run &r     = stream.runs[n];
        uint64_t   first = low > r.vcn ? low : r.vcn;
        uint64_t   last  = end < r.vcn + r.length ? end : r.vcn + r.length;
        if (first < last) scratch[count++] = {first, last - first, r.lcn + int64_t(first - r.vcn)};
    }
    if (!count || scratch[0].vcn != low || scratch[count - 1].vcn + scratch[count - 1].length != end) return 0;
    unsigned length =
        nonresident(out, type, false, scratch, count, low ? 0 : stream.size, low ? 0 : stream.allocated, id);
    if (!length) return 0;
    put64(out + 16, low);
    put64(out + 24, end - 1);
    return length;
}

bool list_less(const MftListEntry &a, const MftListEntry &b)
{
    if (le32(a.data) != le32(b.data)) return le32(a.data) < le32(b.data);
    unsigned na = a.data[6], nb = b.data[6];
    for (unsigned n = 0; n < na && n < nb; ++n)
    {
        unsigned ca = le16(a.data + a.data[7] + n * 2), cb = le16(b.data + b.data[7] + n * 2);
        if (ca != cb) return ca < cb;
    }
    if (na != nb) return na < nb;
    return le64(a.data + 8) < le64(b.data + 8);
}

int utf16(const char *name, uint8_t *output, unsigned &count)
{
    count         = 0;
    const auto *p = reinterpret_cast<const uint8_t *>(name);
    while (*p)
    {
        uint32_t c       = *p++;
        unsigned more    = 0;
        uint32_t minimum = 0;
        if (c >= 0xc2 && c <= 0xdf)
        {
            c       &= 31;
            more     = 1;
            minimum  = 0x80;
        }
        else if (c >= 0xe0 && c <= 0xef)
        {
            c       &= 15;
            more     = 2;
            minimum  = 0x800;
        }
        else if (c >= 0xf0 && c <= 0xf4)
        {
            c       &= 7;
            more     = 3;
            minimum  = 0x10000;
        }
        else if (c >= 0x80)
            return -EILSEQ;
        for (unsigned i = 0; i < more; ++i)
        {
            unsigned b = *p++;
            if ((b & 0xc0) != 0x80) return -EILSEQ;
            c = (c << 6) | (b & 63);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return -EILSEQ;
        if (c >= 0x10000)
        {
            if (count >= 254) return -ENAMETOOLONG;
            c -= 0x10000;
            put16(output + count++ * 2, uint16_t(0xd800 + (c >> 10)));
            c = 0xdc00 + (c & 1023);
        }
        if (count == 255) return -ENAMETOOLONG;
        put16(output + count++ * 2, uint16_t(c));
    }
    return count ? 0 : -EINVAL;
}

unsigned index_entry(uint8_t *p, const DirectoryKey *key, bool child, uint64_t vcn = 0)
{
    unsigned length = key ? aligned(16 + key->length) : 16;
    if (child) length += 8;
    clear(p, length);
    put16(p + 8, uint16_t(length));
    put16(p + 12, uint16_t((child ? 1 : 0) | (key ? 0 : 2)));
    if (key)
    {
        put64(p, key->reference);
        put16(p + 10, uint16_t(key->length));
        copy(p + 16, key->name, key->length);
    }
    if (child) put64(p + length - 8, vcn);
    return length;
}
} // namespace

bool WriteSession::overlay_read(void *context, uint64_t offset, void *output, Size length)
{
    return read_overlay(context, offset, output, length, false);
}

bool WriteSession::before_read(void *context, uint64_t offset, void *output, Size length)
{
    return read_overlay(context, offset, output, length, true);
}

bool WriteSession::read_overlay(void *context, uint64_t offset, void *output, Size length, bool before)
{
    auto &self = *static_cast<WriteSession *>(context);
    if (!self.device_.read(self.device_.context, offset, output, length))
    {
        self.failed_ = true;
        return false;
    }
    auto *out = static_cast<uint8_t *>(output);
    for (unsigned i = 0; i < self.work_->journal.count; ++i)
    {
        const auto    &image = self.work_->journal.images[i];
        const uint8_t *bytes = image.packed;
        if (before)
        {
            bytes              = image.before;
            uint32_t signature = image.kind == MetadataKind::FileRecord ? 0x454c4946 : 0x58444e49;
            if (image.kind != MetadataKind::Raw && le32(bytes) == signature)
            {
                if (pack(bytes, image.length, self.work_->before_page)) { return false; }
                bytes = self.work_->before_page;
            }
        }
        for (unsigned mirror = 0; mirror < 2; ++mirror)
        {
            uint64_t start = self.journal_.physical(image);
            if (mirror)
            {
                uint64_t number = image.vcn * (JournalPage / self.record_) + image.within / self.record_;
                bool     prefix = image.kind == MetadataKind::Raw && image.attribute_type == Data &&
                              (image.file_reference & NumberMask) == 0 && image.vcn < self.record_ * 4 / JournalPage &&
                              !image.within && image.length == JournalPage;
                if ((image.kind != MetadataKind::FileRecord && !prefix) || number >= 4) continue;
                start = self.journal_.mirror_lcn() * JournalPage + number * self.record_;
            }
            uint64_t first = offset > start ? offset : start,
                     end   = offset + length < start + image.length ? offset + length : start + image.length;
            if (first < end) copy(out + first - offset, bytes + first - start, Size(end - first));
        }
    }
    return true;
}

int WriteSession::map(const Stream &stream, uint64_t offset, uint64_t &physical) const
{
    if (stream.resident || offset >= stream.allocated) return -EIO;
    for (unsigned i = 0; i < stream.count; ++i)
    {
        const Run &r = stream.runs[i];
        if (offset / JournalPage >= r.vcn && offset / JournalPage - r.vcn < r.length && r.lcn >= 0)
        {
            physical = uint64_t(r.lcn) * JournalPage + offset - r.vcn * JournalPage;
            return 0;
        }
    }
    return -EIO;
}

int WriteSession::audit(Volume &volume, Workspace &scratch, bool load)
{
    if (volume.cluster_ != JournalPage || (volume.record_ != 1024 && volume.record_ != JournalPage) ||
        volume.index_ != JournalPage || volume.clusters_ > uint64_t(MaxAllocationBitmap) * 8 ||
        volume.mft_.size / record_ > MaxWriteRecords)
        return -EOPNOTSUPP;
    uint64_t base_address;
    int      error = map(volume.mft_, 0, base_address);
    if (error || base_address != journal_.mft_lcn() * JournalPage) return error ? error : -EIO;
    error = volume.record_read(0, 0, scratch.record);
    if (!error) error = volume.stream_from_record(scratch.record, Bitmap, false, volume.mft_bitmap_, scratch);
    if (!error) error = volume.record_read(6, 0, scratch.record);
    if (!error) error = volume.stream_from_record(scratch.record, Data, false, volume.cluster_bitmap_, scratch);
    if (error) return error;
    uint64_t records = volume.mft_.size / record_, bytes = (volume.clusters_ + 7) / 8;
    if (volume.mft_bitmap_.flags || volume.cluster_bitmap_.flags || volume.mft_bitmap_.size < (records + 7) / 8 ||
        volume.cluster_bitmap_.size < bytes || volume.cluster_bitmap_.initialized < bytes)
        return -EIO;
    if (volume.mft_bitmap_.size > sizeof(work_->mft_bits) || volume.mft_bitmap_.initialized != volume.mft_bitmap_.size)
        return -EOPNOTSUPP;
    clear(work_->mft_bits, sizeof(work_->mft_bits));
    error = volume.stream_read(volume.cluster_bitmap_, 0, work_->allocation, Size(bytes));
    if (!error) error = volume.stream_read(volume.mft_bitmap_, 0, work_->mft_bits, Size(volume.mft_bitmap_.size));
    if (error) return error;
    // A corrupt bitmap must not hide the streams that protect MFT, log, boot,
    // mirror, allocation and UpCase storage from the free-cluster allocator.
    for (unsigned system = 0; system < 12; ++system)
        if (!bit(work_->mft_bits, system)) return -EIO;
    for (uint64_t bit_number = records; bit_number < volume.mft_bitmap_.size * 8; ++bit_number)
        if (bit(work_->mft_bits, bit_number)) return -EIO;
    clear(work_->ownership, Size(bytes));
    struct Scan
    {
        WriteSession *self;
        Volume       *volume;
    } scan       = {this, &volume};
    auto inspect = [](void *context, const uint8_t *p, unsigned length, uint64_t) -> int {
        auto &scan = *static_cast<Scan *>(context);
        auto &s    = *scan.self;
        if (!p[8]) return 0;
        if (!le64(p + 16) && le64(p + 24) == __UINT64_MAX__ && !le64(p + 40) && !le64(p + 48) && !le64(p + 56) &&
            !p[le16(p + 32)])
            return 0;
        Stream &decoded = s.work_->audit_stream;
        decoded.count   = 0;
        unsigned at     = le16(p + 32);
        int error = decode_runs(p + at, length - at, scan.volume->clusters_, le64(p + 16), le64(p + 24), true, decoded);
        if (error) return error;
        for (unsigned i = 0; i < decoded.count; ++i)
        {
            if (decoded.runs[i].lcn < 0) continue;
            for (uint64_t n = uint64_t(decoded.runs[i].lcn); n < uint64_t(decoded.runs[i].lcn) + decoded.runs[i].length;
                 ++n)
            {
                if (!bit(s.work_->allocation, n) || bit(s.work_->ownership, n)) return -EIO;
                set_bit(s.work_->ownership, n, true);
            }
        }
        return 0;
    };
    // Scan every allocated segment, including extension records. Even an
    // unlisted extension must protect its physical clusters from allocation.
    for (uint64_t n = 0; n < records; ++n)
    {
        error = volume.stream_read(volume.mft_, n * record_, scratch.record, record_);
        if (error) return error;
        if (!bit(work_->mft_bits, n))
        {
            if (le32(scratch.record) == 0x454c4946 && (le16(scratch.record + 22) & 1)) return -EIO;
            continue;
        }
        uint64_t owner = le64(scratch.record + 32);
        error          = record_layout(scratch.record, record_, n, 0, owner);
        if (error) return error;
        if (owner)
        {
            if ((owner & NumberMask) >= records || !bit(work_->mft_bits, owner & NumberMask)) return -EIO;
            error = volume.record_read(owner & NumberMask, uint16_t(owner >> 48), work_->page);
            if (error) return error;
        }
        unsigned  at = le16(scratch.record + 20);
        Attribute a;
        while (!(error = next_attribute(scratch.record, record_, at, a)))
        {
            error = inspect(&scan, a.data, a.length, n);
            if (error) return error;
        }
        if (error != 1) return error;
    }
    error = volume.record_read(1, 0, scratch.record);
    if (!error) error = volume.stream_from_record(scratch.record, Data, false, scratch.stream, scratch);
    uint64_t mirror = 0;
    if (!error) error = map(scratch.stream, 0, mirror);
    if (error || mirror != journal_.mirror_lcn() * JournalPage || scratch.stream.size < uint64_t(record_) * 4)
        return error ? error : -EIO;
    for (unsigned n = 1; n < record_ * 4 / JournalPage; ++n)
    {
        error = map(scratch.stream, uint64_t(n) * JournalPage, mirror);
        if (error || mirror != (journal_.mirror_lcn() + n) * JournalPage) return error ? error : -EIO;
    }
    // BPB, backup boot cluster, and MFTMirr remain reserved even if a damaged
    // system attribute tries to describe them differently.
    for (uint64_t n = 0; n < 2; ++n)
        if (!bit(work_->allocation, n)) return -EIO;
    for (unsigned n = 0; n < record_ * 4 / JournalPage; ++n)
        if (!bit(work_->allocation, journal_.mirror_lcn() + n)) return -EIO;
    if (load)
    {
        clusters_     = volume.clusters_;
        records_      = unsigned(records);
        bitmap_bytes_ = unsigned(bytes);
    }
    return 0;
}

int WriteSession::validate_callback(void *context, JournalWorkspace &)
{
    return static_cast<WriteSession *>(context)->validate();
}

int WriteSession::validate()
{
    auto &view    = work_->validation;
    auto &scratch = work_->scratch;
    int   error   = view.mount_internal(overlay_read, this, device_.bytes, scratch, true);
    if (error) return error;
    error = audit(view, scratch, false);
    if (error) return error;
    error = view.record_read(0, 0, scratch.record);
    if (error) return error;
    uint64_t mft_reference = uint64_t(le16(scratch.record + 16)) << 48;
    // A recovery must never move its own journal: the replacement commit is
    // still written through the bootstrap mapping until both restarts are durable.
    error = view.record_read(2, 0, scratch.record);
    if (!error) error = view.stream_from_record(scratch.record, Data, false, scratch.stream, scratch);
    if (error) return error;
    if (scratch.stream.count != journal_.log_.count || scratch.stream.size != journal_.log_.size ||
        scratch.stream.initialized != journal_.log_.initialized || scratch.stream.allocated != journal_.log_.allocated)
        return -EOPNOTSUPP;
    for (unsigned n = 0; n < scratch.stream.count; ++n)
        if (scratch.stream.runs[n].vcn != journal_.log_.runs[n].vcn ||
            scratch.stream.runs[n].lcn != journal_.log_.runs[n].lcn ||
            scratch.stream.runs[n].length != journal_.log_.runs[n].length)
            return -EOPNOTSUPP;
    for (unsigned n = 0; n < work_->journal.count; ++n)
    {
        const auto &image = work_->journal.images[n];
        if (image.kind != MetadataKind::FileRecord) continue;
        uint64_t number = image.vcn * (JournalPage / record_) + image.within / record_;
        if (number || le64(image.after + 32) || le64(image.before + 32))
        {
            if (number && le64(image.after + 32) != mft_reference && le64(image.before + 32) != mft_reference) continue;
        }
        error = validate_mft(image);
        if (error) return error;
        break;
    }
    // Root contents can change while its passive Windows utility initializer
    // stays byte-identical. Enforce this for native and imported WAL images.
    for (unsigned n = 0; n < work_->journal.count; ++n)
    {
        const auto    &image  = work_->journal.images[n];
        uint64_t       first  = image.vcn * (JournalPage / record_);
        const uint8_t *before = image.before, *after = image.after;
        if (image.kind == MetadataKind::FileRecord)
        {
            if (first + image.within / record_ != 5) continue;
        }
        else if (image.kind == MetadataKind::Raw && image.attribute_type == Data &&
                 !(image.file_reference & NumberMask) && !image.name_length && first <= 5 &&
                 5 - first < image.length / record_)
        {
            unsigned at = unsigned(5 - first) * record_;
            copy(work_->old_record, before + at, record_);
            copy(work_->mft_base, after + at, record_);
            error = unpack(work_->old_record, record_, 0x454c4946);
            if (!error) error = unpack(work_->mft_base, record_, 0x454c4946);
            if (error) return error;
            before = work_->old_record;
            after  = work_->mft_base;
        }
        else
            continue;
        error = preserve_root_utility(before, after, record_);
        if (error) return error;
    }
    copy(&work_->directory_validation, &view, sizeof(Volume));
    // A distinct parser object/workspace keeps the visitor non-reentrant.
    auto check_directory = [&](const Inode &inode) -> int {
        struct Children
        {
            WriteSession *self;
            Volume       *other;
            uint64_t      parent;
        } state    = {this, &work_->directory_validation, inode.reference};
        auto child = [](void *context, const DirectoryEntry &entry) -> int {
            auto      &state = *static_cast<Children *>(context);
            auto      &self  = *state.self;
            Volume    &other = *state.other;
            Workspace &inner = *self.reader_work_;
            if ((entry.reference & NumberMask) >= other.mft_.size / self.record_ ||
                !bit(self.work_->mft_bits, entry.reference & NumberMask))
                return -EIO;
            int error = other.record_read(entry.reference & NumberMask, uint16_t(entry.reference >> 48), inner.record);
            if (error) return error;
            struct Name
            {
                uint64_t              parent;
                const DirectoryEntry *entry;
                bool                  found;
            } name     = {state.parent, &entry, false};
            auto match = [](void *context, const uint8_t *p, unsigned, uint64_t) -> int {
                auto &state = *static_cast<Name *>(context);
                if (p[8] || le32(p + 16) < 66) return -EIO;
                const uint8_t *value = p + le16(p + 20);
                if (le64(value) != state.parent || value[65] != state.entry->name_space) return 0;
                if (le32(p + 16) != 66 + unsigned(value[64]) * 2) return -EIO;
                char utf8[MaxName];
                int  error = utf8_name(value + 66, value[64], utf8);
                if (error) return error;
                if (!equal(utf8, state.entry->name)) return 0;
                if (state.found) return -EIO;
                state.found = true;
                return 0;
            };
            error = other.visit_attributes(inner.record, entry.reference, 0x30, false, match, &name, inner);
            return error ? error : name.found ? 0 : -EIO;
        };
        return view.enumerate_directory(inode, child, &state, scratch);
    };
    unsigned checked_count = 0;
    auto     check_parent  = [&](uint64_t reference) -> int {
        for (unsigned n = 0; n < checked_count; ++n)
            if (work_->checked_parents[n] == reference) return 0;
        if (checked_count == MaxMetadataImages * 8) return -EOPNOTSUPP;
        if ((reference & NumberMask) >= view.mft_.size / record_ || !bit(work_->mft_bits, reference & NumberMask))
            return -EIO;
        Inode parent;
        int   error = view.read_inode(reference, parent, scratch);
        if (!error) error = check_directory(parent);
        if (!error) work_->checked_parents[checked_count++] = reference;
        return error;
    };
    // A changed child also makes its old/new parents relevant, even if no
    // directory image was logged. Check both directions of FILE_NAME links;
    // allocation consistency alone cannot detect a missing namespace update.
    auto check_file = [&](const uint8_t *p, uint64_t number, bool old) -> int {
        if (le32(p) != 0x454c4946 || !(le16(p + 22) & 1)) return 0;
        uint64_t reference = le64(p + 32);
        if (!reference) reference = number | uint64_t(le16(p + 16)) << 48;
        unsigned  at = le16(p + 20);
        Attribute a;
        int       error;
        while (!(error = next_attribute(p, record_, at, a)))
        {
            if (a.type != 0x30) continue;
            if (a.data[8] || le32(a.data + 16) < 66) return -EIO;
            const uint8_t *value            = a.data + le16(a.data + 20);
            uint64_t       parent_reference = le64(value);
            if (old)
            {
                // The old parent may itself have been deleted/reused. Its
                // own logged FILE before-image checks the surviving ancestor.
                Inode parent;
                if ((parent_reference & NumberMask) >= view.mft_.size / record_ ||
                    !bit(work_->mft_bits, parent_reference & NumberMask))
                    continue;
                error = work_->directory_validation.read_inode(parent_reference, parent, *reader_work_);
                if (error)
                {
                    if (failed()) return -EIO;
                    continue;
                }
                if (!parent.directory) continue;
            }
            error = check_parent(parent_reference);
            if (error) return error;
            if (old) continue;
            if (le32(a.data + 16) != 66 + unsigned(value[64]) * 2) return -EIO;
            struct Link
            {
                uint64_t reference;
                unsigned space;
                bool     found;
                char     name[MaxName];
            } link = {reference, value[65], false, {}};
            error  = utf8_name(value + 66, value[64], link.name);
            if (error) return error;
            // The raw index parser checks self-key identity but deliberately
            // omits '.' from visitor callbacks.
            if (equal(link.name, "."))
            {
                if ((reference & NumberMask) != 5 || parent_reference != reference) return -EIO;
                continue;
            }
            Inode parent;
            error = view.read_inode(parent_reference, parent, scratch);
            if (error) return error;
            auto contains = [](void *context, const DirectoryEntry &entry) -> int {
                auto &link = *static_cast<Link *>(context);
                if (entry.reference != link.reference || entry.name_space != link.space ||
                    !equal(entry.name, link.name))
                    return 0;
                if (link.found) return -EIO;
                link.found = true;
                return 0;
            };
            error = view.enumerate_directory(parent, contains, &link, scratch);
            if (error || !link.found) return error ? error : -EIO;
        }
        return error == 1 ? 0 : error;
    };
    // Resolve every logged destination through the staged NTFS stream. A log
    // record cannot authorize arbitrary physical writes by naming an LCN.
    for (unsigned i = 0; i < work_->journal.count; ++i)
    {
        const auto &image   = work_->journal.images[i];
        uint64_t    address = 0;
        if (image.kind == MetadataKind::FileRecord)
        {
            uint64_t n = image.vcn * (JournalPage / record_) + image.within / record_;
            if (n >= view.mft_.size / record_ || (n < 16 && n != 0 && n != 5 && n != 6)) return -EOPNOTSUPP;
            error = map(view.mft_, n * record_, address);
            if (image.attribute_type != Data || image.file_reference != mft_reference ||
                bit(work_->mft_bits, n) != bool(le16(image.after + 22) & 1))
                return -EIO;
        }
        else
        {
            // An unowned post-recovery page cannot overwrite a live stream.
            if (!bit(work_->ownership, image.lcn)) continue;
            error = view.record_read(image.file_reference & NumberMask, uint16_t(image.file_reference >> 48),
                                     scratch.record);
            if (error) return error;
            struct Builder
            {
                const MetadataImage *image;
                Stream              *stream;
                uint64_t             clusters, bytes;
                bool                 found;
            } builder = {&image, &scratch.stream, view.clusters_, view.bytes_, false};
            clear(&scratch.stream, sizeof(Stream));
            auto append = [](void *context, const uint8_t *p, unsigned length, uint64_t) -> int {
                auto       &b     = *static_cast<Builder *>(context);
                const auto &image = *b.image;
                if (le32(p) != image.attribute_type || p[9] != image.name_length) return 0;
                for (unsigned n = 0; n < image.name_length; ++n)
                    if (le16(p + le16(p + 10) + n * 2) != image.name[n]) return 0;
                return append_fragment(p, length, *b.stream, b.clusters, JournalPage, b.bytes, b.found);
            };
            if (image.attribute_type == 0x20)
            {
                unsigned  at = le16(scratch.record + 20);
                Attribute a;
                while (!(error = next_attribute(scratch.record, record_, at, a)))
                    if (a.type == 0x20)
                    {
                        error = append(&builder, a.data, a.length, image.file_reference);
                        break;
                    }
            }
            else
                error =
                    view.visit_attributes(scratch.record, image.file_reference, 0, false, append, &builder, scratch);
            if (!error && (!builder.found || scratch.stream.flags)) error = -EOPNOTSUPP;
            if (!error) error = complete_stream(scratch.stream, JournalPage);
            if (!error) error = map(scratch.stream, image.vcn * JournalPage + image.within, address);
        }
        if (error || address != journal_.physical(image)) return error ? error : -EIO;
        uint64_t references[4] = {};
        unsigned count         = 0;
        if (image.kind == MetadataKind::FileRecord && (le16(image.after + 22) & 3) == 3 && !le64(image.after + 32))
            references[count++] =
                (image.vcn * (JournalPage / record_) + image.within / record_) | uint64_t(le16(image.after + 16)) << 48;
        else if (image.kind == MetadataKind::IndexBuffer && image.named_index)
            references[count++] = image.file_reference;
        else if (image.kind == MetadataKind::Raw && image.attribute_type == Data &&
                 (image.file_reference & NumberMask) == 0 && !image.name_length)
        {
            for (unsigned n = 0; n < image.length / record_; ++n)
            {
                const uint8_t *p = image.after + n * record_;
                if (le32(p) == 0x454c4946 && (le16(p + 22) & 3) == 3 && !le64(p + 32))
                    references[count++] = (image.vcn * (JournalPage / record_) + n) | uint64_t(le16(p + 16)) << 48;
            }
        }
        for (unsigned n = 0; n < count; ++n)
        {
            error = check_parent(references[n]);
            if (error) return error;
        }
        if (image.kind == MetadataKind::FileRecord)
        {
            uint64_t number = image.vcn * (JournalPage / record_) + image.within / record_;
            error           = check_file(image.before, number, true);
            if (!error) error = check_file(image.after, number, false);
            if (error) return error;
        }
        else if (image.kind == MetadataKind::Raw && image.attribute_type == Data &&
                 !(image.file_reference & NumberMask) && !image.name_length)
        {
            for (unsigned at = 0; at + record_ <= image.length; at += record_)
            {
                uint64_t number = image.vcn * (JournalPage / record_) + at / record_;
                if (number >= view.mft_.size / record_) continue;
                uint8_t *p = work_->recovery.logical;
                copy(p, image.before + at, record_);
                if (!unpack(p, record_, 0x454c4946))
                {
                    error = check_file(p, image.vcn * (JournalPage / record_) + at / record_, true);
                    if (error) return error;
                }
                if (!bit(work_->mft_bits, number)) continue;
                copy(p, image.after + at, record_);
                error = unpack(p, record_, 0x454c4946);
                if (error) return error;
                error = check_file(p, image.vcn * (JournalPage / record_) + at / record_, false);
                if (error) return error;
            }
        }
    }
    return failed() ? -EIO : 0;
}

int WriteSession::open(const WriteDevice &device, Volume &volume, Workspace &reader_work, WriteWorkspace &work)
{
    opened_ = failed_ = false;
    device_           = device;
    volume_           = &volume;
    volume.io_failed_ = false;
    reader_work_      = &reader_work;
    work_             = &work;
    int error         = journal_.open(device, work.journal, &work.recovery);
    record_           = journal_.record_;
    if (!error) error = journal_.recover(validate_callback, this);
    if (!error) error = volume.mount(device.read, device.context, device.bytes, reader_work);
    if (!error) error = audit(volume, reader_work, true);
    if (!error) error = volume.record_read(0, 0, reader_work.record);
    if (!error) mft_reference_ = uint64_t(le16(reader_work.record + 16)) << 48;
    if (!error) opened_ = true;
    return error;
}

int WriteSession::begin()
{
    if (!opened_ || failed()) return -EIO;
    journal_.begin();
    work_->directory_count = work_->cluster_count = work_->allocated_count = 0;
    int error                                                              = audit(*volume_, *reader_work_, true);
    if (!error)
    {
        copy(&work_->mft_map, &volume_->mft_, sizeof(Stream));
        copy(&work_->mft_bitmap, &volume_->mft_bitmap_, sizeof(Stream));
        error = mft_load_entries();
    }
    return error;
}

int WriteSession::stage_record(uint64_t number, MetadataImage *&image)
{
    uint64_t address;
    int      error = map(work_->mft_map, number * record_, address);
    if (error) return error;
    for (unsigned i = 0; i < work_->journal.count; ++i)
        if (journal_.physical(work_->journal.images[i]) == address)
        {
            image = &work_->journal.images[i];
            return 0;
        }
    // New initialized records are unreachable in the old MFT. Their undo is
    // deallocation, never a guessed read beyond the old initialized length.
    if (number >= volume_->mft_.size / record_)
        clear(work_->page, record_);
    else
        error = volume_->stream_read(volume_->mft_, number * record_, work_->page, record_);
    if (error) return error;
    if (le32(work_->page) == 0x454c4946)
    {
        error = unpack(work_->page, record_, 0x454c4946);
        if (error) return error;
    }
    return journal_.add(MetadataKind::FileRecord, mft_reference_, Data, false, number / (JournalPage / record_),
                        address / JournalPage, unsigned(address % JournalPage), record_, work_->page, image);
}

int WriteSession::validate_mft(const MetadataImage &)
{
    // WAL before-images reconstruct the old map even if some home pages have
    // already reached media. This is also used during repeated recovery.
    auto &old_view = work_->before_validation;
    auto &view     = work_->validation;
    int   error    = old_view.mount_internal(before_read, this, device_.bytes, *reader_work_, true);
    if (error) return error;
    const Stream &old = old_view.mft_, &next = view.mft_;
    if (old.flags || next.flags || old.initialized != old.size || next.initialized != next.size ||
        next.size < old.size || next.size - old.size > JournalPage || next.size / record_ > MaxWriteRecords ||
        next.allocated < old.allocated || next.allocated - old.allocated > JournalPage || !preserves(old, next))
        return -EIO;
    error = old_view.record_read(0, 0, work_->old_record);
    if (!error) error = view.record_read(0, 0, work_->mft_base);
    if (error) return error;
    const uint8_t *before = work_->old_record, *after = work_->mft_base;
    for (unsigned n = 0; n < 48; ++n)
        if (!(n >= 8 && n < 16) && !(n >= 24 && n < 28) && !(n >= 40 && n < 42) && before[n] != after[n]) return -EIO;
    Stream &old_bitmap = work_->old_mft, &new_bitmap = work_->audit_stream;
    error = old_view.stream_from_record(before, Bitmap, false, old_bitmap, *reader_work_);
    if (!error) error = view.stream_from_record(after, Bitmap, false, new_bitmap, work_->scratch);
    if (error) return error;
    if (old_bitmap.flags || new_bitmap.flags || new_bitmap.initialized != new_bitmap.size ||
        new_bitmap.size < (next.size / record_ + 7) / 8 || new_bitmap.size < old_bitmap.size ||
        new_bitmap.size - old_bitmap.size > 8 || new_bitmap.size > sizeof(work_->mft_bits))
        return -EIO;
    if (!old_bitmap.resident &&
        (new_bitmap.resident || new_bitmap.allocated < old_bitmap.allocated ||
         new_bitmap.allocated - old_bitmap.allocated > JournalPage || !preserves(old_bitmap, new_bitmap)))
        return -EIO;
    struct Compare
    {
        WriteSession  *self;
        const uint8_t *original;
        unsigned       length, count;
    } compare  = {this, nullptr, 0, 0};
    auto count = [](void *context, const uint8_t *p, unsigned, uint64_t) -> int {
        auto &c = *static_cast<Compare *>(context);
        if (!p[9] && (le32(p) == Data || le32(p) == Bitmap)) return 0;
        if (le32(p) == 0x20) return 0;
        ++c.count;
        return 0;
    };
    error = view.visit_attributes(after, record_reference(after), 0, false, count, &compare, work_->scratch);
    if (error) return error;
    auto original = [](void *context, const uint8_t *p, unsigned length, uint64_t) -> int {
        auto &c = *static_cast<Compare *>(context);
        if (!p[9] && (le32(p) == Data || le32(p) == Bitmap)) return 0;
        if (le32(p) == 0x20) return 0;
        if (!c.count) return -EIO;
        --c.count;
        c.original       = p;
        c.length         = length;
        unsigned matches = 0;
        struct Match
        {
            Compare  *c;
            unsigned *matches;
        } match   = {&c, &matches};
        auto same = [](void *context, const uint8_t *next, unsigned length, uint64_t) -> int {
            auto       &m = *static_cast<Match *>(context);
            const auto &c = *m.c;
            if (length != c.length) return 0;
            for (unsigned n = 0; n < length; ++n)
                if (n != 14 && n != 15 && c.original[n] != next[n]) return 0;
            ++*m.matches;
            return 0;
        };
        auto &s     = *c.self;
        int   error = s.work_->validation.visit_attributes(s.work_->mft_base, record_reference(s.work_->mft_base), 0,
                                                           false, same, &match, s.work_->scratch);
        return error ? error : matches == 1 ? 0 : -EIO;
    };
    error = old_view.visit_attributes(before, record_reference(before), 0, false, original, &compare, *reader_work_);
    return error ? error : compare.count ? -EIO : 0;
}

int WriteSession::mft_entry(const uint8_t *p, uint64_t reference, unsigned &index)
{
    if (work_->mft_entry_count == MaxAttributeEntries) return -EOPNOTSUPP;
    index    = work_->mft_entry_count++;
    auto &e  = work_->mft_entries[index];
    e.length = aligned(26 + unsigned(p[9]) * 2);
    clear(e.data, e.length);
    put32(e.data, le32(p));
    put16(e.data + 4, uint16_t(e.length));
    e.data[6] = p[9];
    e.data[7] = p[9] ? 26 : 0;
    put64(e.data + 8, p[8] ? le64(p + 16) : 0);
    put64(e.data + 16, reference);
    put16(e.data + 24, le16(p + 14));
    if (p[9]) copy(e.data + 26, p + le16(p + 10), unsigned(p[9]) * 2);
    return 0;
}

int WriteSession::mft_load_entries()
{
    int error = volume_->record_read(0, 0, work_->old_record);
    if (error) return error;
    auto accept = [](void *, const uint8_t *, unsigned, uint64_t) -> int { return 0; };
    error = volume_->visit_attributes(work_->old_record, mft_reference_, 0, false, accept, nullptr, *reader_work_);
    if (error) return error;
    work_->mft_entry_count = 0;
    clear(&work_->mft_list_map, sizeof(Stream));
    const uint8_t *list = attribute(work_->old_record, 0x20);
    work_->mft_has_list = list;
    if (!list)
    {
        unsigned  at = le16(work_->old_record + 20);
        Attribute a;
        while (!(error = next_attribute(work_->old_record, record_, at, a)))
        {
            unsigned index;
            error = mft_entry(a.data, mft_reference_, index);
            if (error) return error;
        }
        return error == 1 ? 0 : error;
    }
    bool found = false;
    error = append_fragment(list, le32(list + 4), work_->mft_list_map, clusters_, JournalPage, device_.bytes, found);
    if (!error) error = complete_stream(work_->mft_list_map, JournalPage);
    if (error) return error;
    const Stream &map = work_->mft_list_map;
    if (map.flags || map.size != map.initialized || !map.size || map.size > MaxAttributeList) return -EOPNOTSUPP;
    error = volume_->stream_read(map, 0, work_->mft_list, Size(map.size));
    if (error) return error;
    for (unsigned at = 0; at + 26 <= map.size;)
    {
        if (work_->mft_entry_count == MaxAttributeEntries) return -EOPNOTSUPP;
        unsigned length = le16(work_->mft_list + at + 4);
        if (length < 26 || length > map.size - at || length > sizeof(MftListEntry::data)) return -EIO;
        auto &e  = work_->mft_entries[work_->mft_entry_count++];
        e.length = length;
        copy(e.data, work_->mft_list + at, length);
        at += length;
    }
    return 0;
}

int WriteSession::mft_extension(uint64_t prefix, MetadataImage *&image)
{
    // A DATA extent must not point to a record that only that extent makes
    // reachable. Reserve existing initialized records, never guessed new LCNs.
    uint64_t limit = volume_->mft_.size / record_;
    for (uint64_t n = 16; n < limit && n * record_ / JournalPage < prefix; ++n)
    {
        if (bit(work_->mft_bits, n)) continue;
        int error = stage_record(n, image);
        if (error) return error;
        if (le32(image->before) == 0x454c4946 && (le16(image->before + 22) & 1)) return -EIO;
        uint16_t sequence = uint16_t(le16(image->before + 16) + 1);
        if (!sequence) sequence = 1;
        new_record(image->after, n, sequence, false, record_);
        put16(image->after + 18, 0);
        put64(image->after + 32, mft_reference_);
        set_bit(work_->mft_bits, n, true);
        work_->mft_has_list = true;
        return 0;
    }
    return -ENOSPC;
}

int WriteSession::mft_place(unsigned index, const uint8_t *value, unsigned length)
{
    auto          &e     = work_->mft_entries[index];
    uint64_t       owner = le64(e.data + 16), low = le64(e.data + 8);
    uint32_t       type  = le32(e.data);
    bool           data  = type == Data && !e.data[6];
    MetadataImage *image = nullptr;
    if (owner)
    {
        int error = stage_record(owner & NumberMask, image);
        if (error) return error;
        if (length <= record_ - le32(image->after + 24))
        {
            unsigned id = free_instance(image->after, le16(value + 14));
            if (id > 65535) return -EOPNOTSUPP;
            copy(work_->recovery.operation_data, value, length);
            put16(work_->recovery.operation_data + 14, uint16_t(id));
            int error = insert(image->after, work_->recovery.operation_data, length);
            if (!error)
            {
                put16(e.data + 24, uint16_t(id));
                if (le16(image->after + 40) <= id) put16(image->after + 40, uint16_t(id + 1));
            }
            return error;
        }
        if (data && !low) return -EOPNOTSUPP;
    }
    if (length > record_ - aligned(48 + (record_ / 512 + 1) * 2) - 8) return -EOPNOTSUPP;
    uint64_t prefix = data ? low : volume_->mft_.size / JournalPage;
    image           = nullptr;
    for (unsigned n = 0; n < work_->journal.count; ++n)
    {
        auto    &candidate = work_->journal.images[n];
        uint64_t number    = candidate.vcn * (JournalPage / record_) + candidate.within / record_;
        if (candidate.kind == MetadataKind::FileRecord && number >= 16 && number * record_ / JournalPage < prefix &&
            le64(candidate.after + 32) == mft_reference_ && (le16(candidate.after + 22) & 1) &&
            length <= record_ - le32(candidate.after + 24))
        {
            image = &candidate;
            break;
        }
    }
    if (!image)
    {
        int error = mft_extension(prefix, image);
        if (error) return error;
    }
    unsigned id = free_instance(image->after, le16(value + 14));
    if (id > 65535) return -EOPNOTSUPP;
    copy(work_->recovery.operation_data, value, length);
    put16(work_->recovery.operation_data + 14, uint16_t(id));
    int error = insert(image->after, work_->recovery.operation_data, length);
    if (error) return error;
    if (le16(image->after + 40) <= id) put16(image->after + 40, uint16_t(id + 1));
    uint64_t number = image->vcn * (JournalPage / record_) + image->within / record_;
    put64(e.data + 16, number | uint64_t(le16(image->after + 16)) << 48);
    put16(e.data + 24, uint16_t(id));
    work_->mft_has_list = true;
    return 0;
}

int WriteSession::mft_stream(uint32_t type, const Stream &stream, uint64_t old_allocated)
{
    unsigned count = work_->mft_entry_count, tail = count;
    for (unsigned n = 0; n < count; ++n)
    {
        const auto &e = work_->mft_entries[n];
        if (le32(e.data) == type && !e.data[6] &&
            (tail == count || le64(e.data + 8) > le64(work_->mft_entries[tail].data + 8)))
            tail = n;
    }
    if (tail == count) return -EIO;
    for (unsigned n = 0; n < count; ++n)
    {
        auto &e = work_->mft_entries[n];
        if (le32(e.data) != type || e.data[6]) continue;
        uint64_t low = le64(e.data + 8), end = stream.allocated / JournalPage;
        for (unsigned k = 0; k < count; ++k)
        {
            const auto &next = work_->mft_entries[k];
            uint64_t    vcn  = le64(next.data + 8);
            if (le32(next.data) == type && !next.data[6] && vcn > low && vcn < end) end = vcn;
        }
        unsigned length = fragment(work_->attribute, type, stream, low, end, le16(e.data + 24), work_->allocated);
        if (!length) return -EOPNOTSUPP;
        int error = mft_place(n, work_->attribute, length);
        if (!error) continue;
        if ((error != -EOPNOTSUPP && error != -ENOSPC) || n != tail || stream.allocated <= old_allocated ||
            old_allocated / JournalPage <= low)
            return error;
        // Keep every old extent in a reachable record. Only the appended VCN
        // range needs a new extension record, reachable through the old map.
        length = fragment(work_->attribute, type, stream, low, old_allocated / JournalPage, le16(e.data + 24),
                          work_->allocated);
        if (!length) return -EOPNOTSUPP;
        error = mft_place(n, work_->attribute, length);
        if (error) return error;
        length = fragment(work_->attribute, type, stream, old_allocated / JournalPage, end, 0, work_->allocated);
        if (!length) return -EOPNOTSUPP;
        unsigned added;
        error = mft_entry(work_->attribute, 0, added);
        if (!error) error = mft_place(added, work_->attribute, length);
        if (error) return error;
    }
    return 0;
}

int WriteSession::mft_room(unsigned length)
{
    MetadataImage *base;
    int            error = stage_record(0, base);
    if (error) return error;
    while (length > record_ - le32(base->after + 24))
    {
        unsigned  at = le16(base->after + 20);
        Attribute a;
        bool      moved = false;
        while (!(error = next_attribute(base->after, record_, at, a)))
        {
            if (a.type == 0x10 || a.type == 0x20 || (a.type == Data && !a.data[9])) continue;
            unsigned n = 0;
            for (; n < work_->mft_entry_count; ++n)
            {
                const auto &e = work_->mft_entries[n];
                if (le64(e.data + 16) == mft_reference_ && le16(e.data + 24) == le16(a.data + 14)) break;
            }
            if (n == work_->mft_entry_count) return -EIO;
            copy(work_->attribute, a.data, a.length);
            error = erase(base->after, const_cast<uint8_t *>(a.data));
            if (error) return error;
            // Do not immediately reinsert it into the base segment.
            put64(work_->mft_entries[n].data + 16, 0);
            error = mft_place(n, work_->attribute, a.length);
            if (error) return error;
            moved = true;
            break;
        }
        if (!moved) return error && error != 1 ? error : -ENOSPC;
    }
    return 0;
}

int WriteSession::allocate_contiguous(unsigned count)
{
    unsigned run = 0;
    for (uint64_t n = 2; n + 1 < clusters_; ++n)
    {
        if (bit(work_->allocation, n) || journal_.overlaps_mirror(n * JournalPage, JournalPage) ||
            journal_.overlaps_log(n * JournalPage, JournalPage))
            run = 0;
        else
            ++run;
        if (run != count) continue;
        work_->cluster_count   = count;
        work_->allocated_count = 1;
        work_->allocated[0]    = {0, count, int64_t(n + 1 - count)};
        for (unsigned k = 0; k < count; ++k)
        {
            work_->new_clusters[k] = n + 1 - count + k;
            set_bit(work_->allocation, n + 1 - count + k, true);
        }
        return 0;
    }
    return -ENOSPC;
}

int WriteSession::mft_list()
{
    if (!work_->mft_has_list) return 0;
    MetadataImage *base;
    int            error = stage_record(0, base);
    if (error) return error;
    uint8_t *old = attribute(base->after, 0x20);
    unsigned id  = old ? le16(old + 14) : free_instance(base->after, le16(base->after + 40));
    if (id > 65535) return -EOPNOTSUPP;
    if (old)
    {
        error = erase(base->after, old);
        if (error) return error;
    }
    unsigned bytes = 0;
    for (unsigned n = 0; n < work_->mft_entry_count; ++n)
    {
        auto    &e = work_->mft_entries[n];
        // Canonicalize previously unaligned final entries before appending.
        unsigned old_length = e.length;
        e.length            = aligned(e.length);
        clear(e.data + old_length, e.length - old_length);
        put16(e.data + 4, uint16_t(e.length));
        if (e.length > MaxAttributeList - bytes) return -EOPNOTSUPP;
        bytes += e.length;
    }
    bool resident_list = aligned(24 + bytes) <= record_ - le32(base->after + 24);
    if (!resident_list)
    {
        // A bounded contiguous COW list keeps its base descriptor small, even
        // when adding a run to an already full base FILE would not fit.
        unsigned clusters = (bytes + JournalPage - 1) / JournalPage;
        error             = allocate_contiguous(clusters);
        if (error) return error;
        Stream &next = work_->replacement;
        clear(&next, sizeof(next));
        next.count   = 1;
        next.runs[0] = {0, clusters, work_->allocated[0].lcn};
        next.size = next.initialized = bytes;
        next.allocated               = uint64_t(clusters) * JournalPage;
        unsigned length = nonresident(work_->attribute, 0x20, false, next.runs, 1, bytes, next.allocated, id);
        error           = mft_room(length);
        if (error) return error;
        // mft_room uses attribute scratch while relocating retained fields.
        length = nonresident(work_->attribute, 0x20, false, next.runs, 1, bytes, next.allocated, id);
        error  = insert(base->after, work_->attribute, length);
        if (error) return error;
    }
    // Stable insertion sort preserves the order of multiple FILE_NAME values.
    for (unsigned n = 1; n < work_->mft_entry_count; ++n)
        for (unsigned k = n; k && list_less(work_->mft_entries[k], work_->mft_entries[k - 1]); --k)
        {
            MftListEntry temp         = work_->mft_entries[k];
            work_->mft_entries[k]     = work_->mft_entries[k - 1];
            work_->mft_entries[k - 1] = temp;
        }
    unsigned at = 0;
    for (unsigned n = 0; n < work_->mft_entry_count; ++n)
    {
        const auto &e = work_->mft_entries[n];
        copy(work_->mft_list + at, e.data, e.length);
        at += e.length;
    }
    if (resident_list)
    {
        unsigned length = resident(work_->attribute, 0x20, false, work_->mft_list, bytes, id);
        error           = insert(base->after, work_->attribute, length);
        if (error) return error;
    }
    else
    {
        const Stream &next = work_->replacement;
        for (unsigned n = 0; n < next.allocated / JournalPage; ++n)
        {
            uint64_t lcn = uint64_t(next.runs[0].lcn) + n;
            error        = volume_->fetch(lcn * JournalPage, work_->page, JournalPage);
            if (error) return error;
            MetadataImage *image;
            error = journal_.add(MetadataKind::Raw, mft_reference_, 0x20, false, n, lcn, 0, JournalPage, work_->page,
                                 image);
            if (error) return error;
            clear(image->after, JournalPage);
            unsigned amount = bytes - n * JournalPage;
            if (amount > JournalPage) amount = JournalPage;
            copy(image->after, work_->mft_list + n * JournalPage, amount);
        }
    }
    if (le16(base->after + 40) <= id) put16(base->after + 40, uint16_t(id + 1));
    if (work_->mft_list_map.count)
    {
        error = release_stream(work_->mft_list_map);
        if (error) return error;
    }
    // No allocated, unlisted extension may be left behind after relocation.
    for (unsigned n = 0; n < work_->journal.count; ++n)
    {
        auto &image = work_->journal.images[n];
        if (image.kind != MetadataKind::FileRecord || le64(image.after + 32) != mft_reference_) continue;
        unsigned start = le16(image.after + 20);
        if (le32(image.after + start) != 0xffffffffU) continue;
        put16(image.after + 22, 0);
        put64(image.after + 32, 0);
        uint16_t sequence = uint16_t(le16(image.after + 16) + 1);
        put16(image.after + 16, sequence ? sequence : 1);
        set_bit(work_->mft_bits, image.vcn * (JournalPage / record_) + image.within / record_, false);
    }
    return 0;
}

int WriteSession::grow_mft()
{
    if (records_ == MaxWriteRecords) return -ENOSPC;
    const unsigned per = JournalPage / record_, first = records_, next_records = (records_ + per) & ~(per - 1);
    uint64_t       old_mft = work_->mft_map.allocated, old_bitmap = work_->mft_bitmap.allocated;
    Stream        &mft = work_->mft_map, &bitmap = work_->mft_bitmap;
    uint64_t       size           = uint64_t(next_records) * record_;
    auto           append_cluster = [&](Stream &map) -> int {
        int error = allocate(1);
        if (error) return error;
        uint64_t lcn = work_->new_clusters[0];
        if (map.count && uint64_t(map.runs[map.count - 1].lcn) + map.runs[map.count - 1].length == lcn)
            ++map.runs[map.count - 1].length;
        else
        {
            if (map.count == MaxRuns) return -EOPNOTSUPP;
            map.runs[map.count++] = {map.allocated / JournalPage, 1, int64_t(lcn)};
        }
        map.allocated += JournalPage;
        return 0;
    };
    if (size > mft.allocated)
    {
        int error = append_cluster(mft);
        if (error) return error;
    }
    mft.size = mft.initialized = size;
    // Free DATA/BITMAP attribute space as one plan. The list descriptor stays
    // in place while DATA is encoded, reserving its existing bootstrap space.
    for (unsigned n = 0; n < work_->mft_entry_count; ++n)
    {
        const auto &e = work_->mft_entries[n];
        if (e.data[6] || (le32(e.data) != Data && le32(e.data) != Bitmap)) continue;
        MetadataImage *image;
        int            error = stage_record(le64(e.data + 16) & NumberMask, image);
        if (error) return error;
        uint8_t *p = instance(image->after, e);
        if (!p) return -EIO;
        error = erase(image->after, p);
        if (error) return error;
    }
    int error = mft_stream(Data, mft, old_mft);
    if (error) return error;
    unsigned bytes = (next_records + 63) / 64 * 8;
    if (bytes < bitmap.size) bytes = unsigned(bitmap.size);
    bool placed = false;
    if (bitmap.resident && bytes + 24 <= record_)
    {
        for (unsigned n = 0; n < work_->mft_entry_count; ++n)
        {
            auto &e = work_->mft_entries[n];
            if (le32(e.data) != Bitmap || e.data[6]) continue;
            unsigned length = resident(work_->attribute, Bitmap, false, work_->mft_bits, bytes, le16(e.data + 24));
            MetadataImage *owner;
            error = stage_record(le64(e.data + 16) & NumberMask, owner);
            if (error) return error;
            if (length <= record_ - le32(owner->after + 24))
            {
                error = mft_place(n, work_->attribute, length);
                if (error) return error;
                placed = true;
            }
            break;
        }
    }
    if (placed) { bitmap.size = bitmap.initialized = bitmap.resident_length = bytes; }
    else
    {
        if (bitmap.resident)
        {
            bitmap.resident  = false;
            bitmap.count     = 0;
            bitmap.allocated = 0;
        }
        while (bytes > bitmap.allocated)
        {
            error = append_cluster(bitmap);
            if (error) return error;
        }
        bitmap.size = bitmap.initialized = bytes;
        error                            = mft_stream(Bitmap, bitmap, old_bitmap);
        if (error) return error;
    }
    error = mft_list();
    if (error) return error;
    records_ = next_records;
    for (unsigned number = first; number < records_; ++number)
    {
        MetadataImage *fresh;
        error = stage_record(number, fresh);
        if (error) return error;
        new_record(fresh->after, number, 1, false, record_);
        put16(fresh->after + 18, 0);
        put16(fresh->after + 22, 0);
    }
    return 0;
}

int WriteSession::load_record(uint64_t reference, MetadataImage *&image, bool directory)
{
    uint64_t number = reference & NumberMask;
    if (number < 16 && !(directory && number == 5)) return -EPERM;
    Inode inode;
    int   error = volume_->read_inode(reference, inode, *reader_work_);
    if (error) return error;
    if (inode.directory != directory) return directory ? -ENOTDIR : -EISDIR;
    if (inode.unsupported || inode.case_sensitive || (inode.attributes & (1U | 4U)))
    {
        if (!(directory && number == 5 && !inode.unsupported && !inode.case_sensitive)) return -EOPNOTSUPP;
    }
    error = stage_record(number, image);
    if (error) return error;
    unsigned  at = le16(image->after + 20);
    Attribute a;
    while (!(error = next_attribute(image->after, record_, at, a)))
        if (a.type == 0x20 || a.type == 0x40 || a.type == 0xc0 ||
            (a.type == 0x100 && !(directory && number == 5 && passive_root_txf(a, reference))) || le16(a.data + 12) ||
            ((a.type == IndexRoot || a.type == IndexAllocation || a.type == Bitmap) && a.data[9] && !named_i30(a.data)))
            return -EOPNOTSUPP;
    return error == 1 ? 0 : error;
}

int WriteSession::allocate(unsigned count)
{
    work_->allocated_count = work_->cluster_count = 0;
    if (count > MaxCowClusters) return -EOPNOTSUPP;
    for (uint64_t n = 2; n + 1 < clusters_ && work_->cluster_count < count; ++n)
    {
        if (bit(work_->allocation, n) || journal_.overlaps_mirror(n * JournalPage, JournalPage) ||
            journal_.overlaps_log(n * JournalPage, JournalPage))
            continue;
        set_bit(work_->allocation, n, true);
        work_->new_clusters[work_->cluster_count++] = n;
        if (work_->allocated_count && uint64_t(work_->allocated[work_->allocated_count - 1].lcn) +
                                              work_->allocated[work_->allocated_count - 1].length ==
                                          n)
            ++work_->allocated[work_->allocated_count - 1].length;
        else
        {
            if (work_->allocated_count == MaxRuns) return -EOPNOTSUPP;
            work_->allocated[work_->allocated_count++] = {work_->cluster_count - 1, 1, int64_t(n)};
        }
    }
    return work_->cluster_count == count ? 0 : -ENOSPC;
}

int WriteSession::release_stream(const Stream &stream)
{
    if (stream.resident) return 0;
    if (stream.flags) return -EOPNOTSUPP;
    for (unsigned i = 0; i < stream.count; ++i)
    {
        const Run &r = stream.runs[i];
        if (r.lcn < 0) return -EOPNOTSUPP;
        for (uint64_t n = uint64_t(r.lcn); n < uint64_t(r.lcn) + r.length; ++n)
        {
            if (!bit(work_->allocation, n) || n < 2 || journal_.overlaps_mirror(n * JournalPage, JournalPage) ||
                n + 1 >= clusters_ || journal_.overlaps_log(n * JournalPage, JournalPage))
                return -EIO;
            set_bit(work_->allocation, n, false);
        }
    }
    return 0;
}

int WriteSession::stage_bitmap(const Stream &stream, uint64_t reference, uint32_t type, const uint8_t *bits,
                               unsigned bytes)
{
    if (stream.resident)
    {
        MetadataImage *image;
        uint64_t       owner = reference;
        if (reference == mft_reference_ && type == Bitmap)
            for (unsigned n = 0; n < work_->mft_entry_count; ++n)
            {
                const auto &e = work_->mft_entries[n];
                if (le32(e.data) == Bitmap && !e.data[6] && !le64(e.data + 8)) owner = le64(e.data + 16);
            }
        int error = stage_record(owner & NumberMask, image);
        if (error) return error;
        uint8_t *p = attribute(image->after, type);
        if (!p || p[8] || le32(p + 16) < bytes) return -EOPNOTSUPP;
        copy(p + le16(p + 20), bits, bytes);
        return 0;
    }
    for (unsigned at = 0; at < bytes; at += JournalPage)
    {
        unsigned size = bytes - at;
        if (size > JournalPage) size = JournalPage;
        uint64_t physical;
        int      error = map(stream, at, physical);
        if (error) return error;
        // Log complete sectors so repeated bitmap updates cannot overwrite
        // neighboring bytes using a stale read-modify-write image.
        unsigned amount = unsigned((size + volume_->sector_ - 1) / volume_->sector_ * volume_->sector_);
        error           = volume_->fetch(physical, work_->page, amount);
        if (error) return error;
        if (bytes_equal(work_->page, bits + at, size)) continue;
        MetadataImage *image;
        error = journal_.add(MetadataKind::Raw, reference, type, false, at / JournalPage, physical / JournalPage,
                             unsigned(physical % JournalPage), amount, work_->page, image);
        if (error) return error;
        copy(image->after, bits + at, size);
    }
    return 0;
}

int WriteSession::stage_bitmaps()
{
    Inode bitmap;
    int   error = volume_->read_inode((uint64_t(1) << 48) | 6, bitmap, *reader_work_);
    // System file sequence numbers are not assumed to be one.
    if (error)
    {
        error = volume_->record_read(6, 0, reader_work_->record);
        if (error) return error;
        bitmap.reference = 6 | uint64_t(le16(reader_work_->record + 16)) << 48;
    }
    error = stage_bitmap(volume_->cluster_bitmap_, bitmap.reference, Data, work_->allocation, bitmap_bytes_);
    if (!error)
        error =
            stage_bitmap(work_->mft_bitmap, mft_reference_, Bitmap, work_->mft_bits, unsigned(work_->mft_bitmap.size));
    return error;
}

int WriteSession::finish()
{
    int error = stage_bitmaps();
    if (!error) error = journal_.commit(validate_callback, this);
    if (!error) error = volume_->mount(device_.read, device_.context, device_.bytes, *reader_work_);
    if (error && (journal_.failed() || volume_->io_failed_)) failed_ = true;
    return error;
}

int WriteSession::sync()
{
    if (failed()) return -EIO;
    int error = journal_.sync();
    if (error) failed_ = true;
    return error;
}

int WriteSession::check_name(const Inode &parent, const char *name) const
{
    if (!name || !*name || equal(name, ".") || equal(name, "..")) return -EINVAL;
    bool valid;
    int  error = volume_->names_equal(name, name, true, valid);
    if (error) return error;
    unsigned length = 0;
    for (; name[length]; ++length)
    {
        unsigned c = uint8_t(name[length]);
        if (c < 32 || c == ':' || c == '/' || c == '\\' || c == '"' || c == '<' || c == '>' || c == '|' || c == '?' ||
            c == '*')
            return -EINVAL;
    }
    if (name[length - 1] == '.' || name[length - 1] == ' ') return -EINVAL;
    if (name[0] == '$') return -EPERM;
    if ((parent.reference & NumberMask) == 5 &&
        (equal(name, "hiberfil.sys", true) || equal(name, "pagefile.sys", true) || equal(name, "swapfile.sys", true)))
        return -EPERM;
    return 0;
}

int WriteSession::key(const Inode &parent, const char *name, const uint8_t *record, uint64_t reference,
                      DirectoryKey &result)
{
    clear(&result, sizeof(result));
    result.reference  = reference;
    uint8_t *standard = attribute(const_cast<uint8_t *>(record), 0x10);
    if (!standard || standard[8] || le32(standard + 16) < 48) return -EIO;
    const uint8_t *si = standard + le16(standard + 20);
    copy(result.name + 8, si, 32);
    put64(result.name, parent.reference);
    put32(result.name + 56, le32(si + 32));
    uint8_t *data = attribute(const_cast<uint8_t *>(record), Data);
    if (data)
    {
        put64(result.name + 40, data[8] ? le64(data + 40) : le32(data + 16));
        put64(result.name + 48, data[8] ? le64(data + 48) : le32(data + 16));
    }
    if (le16(record + 22) & 2) put32(result.name + 56, le32(result.name + 56) | 0x10000000U);
    unsigned count;
    int      error = utf16(name, result.name + 66, count);
    if (error) return error;
    result.name[64] = uint8_t(count);
    // Windows uses namespace zero for a single name without a DOS alias.
    // A standalone Win32-only (1) name is repaired by Windows chkdsk.
    result.name[65] = 0;
    result.length   = 66 + count * 2;
    return 0;
}

int WriteSession::find(const DirectoryEdit &directory, const char *name, unsigned &position) const
{
    for (unsigned i = 0; i < directory.count; ++i)
    {
        char        current[MaxName];
        const auto &key   = directory.keys[i];
        int         error = utf8_name(key.name + 66, key.name[64], current);
        if (error) return error;
        bool match;
        error = volume_->names_equal(name, current, directory.inode.case_sensitive, match);
        if (error) return error;
        if (match)
        {
            position = i;
            return 0;
        }
    }
    return -ENOENT;
}

int WriteSession::load_directory(const Inode &inode, DirectoryEdit *&directory)
{
    for (unsigned i = 0; i < work_->directory_count; ++i)
        if (work_->directories[i].inode.reference == inode.reference)
        {
            directory = &work_->directories[i];
            return 0;
        }
    if (work_->directory_count == 2) return -EOPNOTSUPP;
    MetadataImage *record;
    int            error = load_record(inode.reference, record, true);
    if (error) return error;
    directory        = &work_->directories[work_->directory_count++];
    directory->inode = inode;
    directory->count = 0;
    auto collect     = [](void *context, const DirectoryEntry &entry) -> int {
        auto &dir = *static_cast<DirectoryEdit *>(context);
        if (dir.count == MaxDirectoryKeys) return -EOPNOTSUPP;
        dir.entries[dir.count++] = entry;
        return 0;
    };
    error = volume_->enumerate_directory(inode, collect, directory, *reader_work_);
    if (error) return error;
    for (unsigned i = 0; i < directory->count; ++i)
    {
        const auto &entry = directory->entries[i];
        error =
            volume_->record_read(entry.reference & NumberMask, uint16_t(entry.reference >> 48), reader_work_->record);
        if (error) return error;
        bool      found = false;
        unsigned  at    = le16(reader_work_->record + 20);
        Attribute a;
        while (!(error = next_attribute(reader_work_->record, record_, at, a)))
        {
            if (a.type != 0x30 || a.data[8]) continue;
            unsigned       size  = le32(a.data + 16);
            const uint8_t *value = a.data + le16(a.data + 20);
            if (size < 66 || size != 66 + unsigned(value[64]) * 2 || size > 576) return -EIO;
            if (le64(value) != inode.reference || value[65] != entry.name_space) continue;
            char name[MaxName];
            error = utf8_name(value + 66, value[64], name);
            if (error) return error;
            if (!equal(name, entry.name)) continue;
            if (found) return -EIO;
            found         = true;
            auto &key     = directory->keys[i];
            key.reference = entry.reference;
            key.length    = size;
            copy(key.name, value, size);
        }
        if (error != 1) return error;
        if (!found) return -EOPNOTSUPP; // ATTRIBUTE_LIST name mutation is outside this profile.
    }
    if ((inode.reference & NumberMask) == 5)
    {
        if (directory->count == MaxDirectoryKeys) return -EOPNOTSUPP;
        // Preserve the root's special self-key exactly, including its stored
        // namespace and cached sizes. It is hidden by ordinary enumeration.
        const uint8_t *fn = attribute(record->after, 0x30);
        if (!fn || fn[8] || le32(fn + 16) != 68) return -EOPNOTSUPP;
        const uint8_t *value = fn + le16(fn + 20);
        if (le64(value) != inode.reference || value[64] != 1 || le16(value + 66) != '.') return -EIO;
        auto &self     = directory->keys[directory->count++];
        self.reference = inode.reference;
        self.length    = 68;
        copy(self.name, value, self.length);
    }
    return 0;
}

int WriteSession::rebuild_directory(DirectoryEdit &dir)
{
    MetadataImage *record;
    int            error = stage_record(dir.inode.reference & NumberMask, record);
    if (error) return error;
    // $UpCase supplies NTFS filename collation; exact UTF-16 breaks ties.
    auto compare = [this](const DirectoryKey &a, const DirectoryKey &b) -> int {
        unsigned n = a.name[64] < b.name[64] ? a.name[64] : b.name[64];
        for (unsigned pass = 0; pass < 2; ++pass)
        {
            for (unsigned i = 0; i < n; ++i)
            {
                uint16_t l = le16(a.name + 66 + i * 2), r = le16(b.name + 66 + i * 2);
                if (!pass)
                {
                    l = volume_->upcase_[l];
                    r = volume_->upcase_[r];
                }
                if (l != r) return l < r ? -1 : 1;
            }
            if (a.name[64] != b.name[64]) return a.name[64] < b.name[64] ? -1 : 1;
        }
        return a.name[65] < b.name[65] ? -1 : a.name[65] != b.name[65];
    };
    for (unsigned i = 1; i < dir.count; ++i)
        for (unsigned j = i; j && compare(dir.keys[j], dir.keys[j - 1]) < 0; --j)
        {
            DirectoryKey saved = dir.keys[j];
            dir.keys[j]        = dir.keys[j - 1];
            dir.keys[j - 1]    = saved;
        }
    unsigned total = 16;
    for (unsigned i = 0; i < dir.count; ++i)
        total += aligned(16 + dir.keys[i].length);
    if (total > 12288) return -EOPNOTSUPP;
    uint8_t *old = attribute(record->after, IndexAllocation, true);
    if (old)
    {
        error = volume_->stream_from_record(record->before, IndexAllocation, true, work_->old_index, *reader_work_,
                                            dir.inode.reference);
        if (!error) error = release_stream(work_->old_index);
        if (error) return error;
        error = erase(record->after, old);
        if (error) return error;
    }
    old = attribute(record->after, Bitmap, true);
    if (old)
    {
        error = erase(record->after, old);
        if (error) return error;
    }
    old = attribute(record->after, IndexRoot, true);
    if (!old || old[8]) return -EIO;
    error = erase(record->after, old);
    if (error) return error;
    unsigned used = le32(record->after + 24), instance = le16(record->after + 40);
    // Keep compact directories wholly resident; otherwise a small B-tree has
    // a one-child root, an internal INDX and up to four leaf INDX buffers.
    bool     small = aligned(32 + 32 + total) <= record_ - used;
    clear(work_->page, JournalPage);
    put32(work_->page, 0x30);
    put32(work_->page + 4, 1);
    put32(work_->page + 8, JournalPage);
    work_->page[12] = 1;
    if (small)
    {
        unsigned at = 32;
        for (unsigned i = 0; i < dir.count; ++i)
            at += index_entry(work_->page + at, &dir.keys[i], false);
        at += index_entry(work_->page + at, nullptr, false);
        put32(work_->page + 16, 16);
        put32(work_->page + 20, at - 16);
        put32(work_->page + 24, at - 16);
        unsigned length = resident(work_->attribute, IndexRoot, true, work_->page, at, instance++);
        error           = insert(record->after, work_->attribute, length);
    }
    else
    {
        unsigned starts[4], ends[4], promoted[3], leaves = 0, current = 0;
        while (current < dir.count)
        {
            if (leaves == 4) return -EOPNOTSUPP;
            starts[leaves] = current;
            unsigned bytes = 64 + 16;
            while (current < dir.count && bytes + aligned(16 + dir.keys[current].length) <= JournalPage)
                bytes += aligned(16 + dir.keys[current++].length);
            if (current == starts[leaves]) return -EOPNOTSUPP;
            ends[leaves] = current;
            if (current < dir.count)
            {
                if (leaves == 3) return -EOPNOTSUPP;
                promoted[leaves] = --ends[leaves];
            }
            ++leaves;
        }
        unsigned nodes = leaves + 1;
        error          = allocate(nodes);
        if (error) return error;
        // Save allocation in the attribute before allocating any other stream.
        unsigned length = nonresident(work_->attribute, IndexAllocation, true, work_->allocated, work_->allocated_count,
                                      nodes * JournalPage, nodes * JournalPage, instance++);
        if (!length) return -EOPNOTSUPP;
        error = insert(record->after, work_->attribute, length);
        if (error) return error;
        uint8_t bits[8] = {uint8_t((1U << nodes) - 1)};
        length          = resident(work_->attribute, Bitmap, true, bits, 8, instance++);
        error           = insert(record->after, work_->attribute, length);
        if (error) return error;
        index_entry(work_->page + 32, nullptr, true, 0);
        put32(work_->page + 16, 16);
        put32(work_->page + 20, 40);
        put32(work_->page + 24, 40);
        work_->page[28] = 1;
        length          = resident(work_->attribute, IndexRoot, true, work_->page, 56, instance++);
        error           = insert(record->after, work_->attribute, length);
        if (error) return error;
        for (unsigned node = 0; node < nodes; ++node)
        {
            clear(work_->page, JournalPage);
            put32(work_->page, 0x58444e49);
            put16(work_->page + 4, 40);
            put16(work_->page + 6, 9);
            put16(work_->page + 40, 1);
            put64(work_->page + 16, node);
            put32(work_->page + 24, 40);
            put32(work_->page + 32, JournalPage - 24);
            unsigned at = 64;
            if (!node)
            {
                work_->page[36] = 1;
                for (unsigned leaf = 0; leaf + 1 < leaves; ++leaf)
                    at += index_entry(work_->page + at, &dir.keys[promoted[leaf]], true, leaf + 1);
                at += index_entry(work_->page + at, nullptr, true, leaves);
            }
            else
            {
                for (unsigned i = starts[node - 1]; i < ends[node - 1]; ++i)
                    at += index_entry(work_->page + at, &dir.keys[i], false);
                at += index_entry(work_->page + at, nullptr, false);
            }
            if (at > JournalPage) return -EOPNOTSUPP;
            put32(work_->page + 28, at - 24);
            // An allocation can reuse an old index cluster released by this
            // transaction. Retain its real logical before image for native
            // undo and include a valid old page LSN in the next epoch choice.
            error = volume_->fetch(work_->new_clusters[node] * JournalPage, work_->attribute, JournalPage);
            if (error) return error;
            if (le32(work_->attribute) == 0x58444e49)
            {
                int old_layout = unpack(work_->attribute, JournalPage, 0x58444e49);
                if (old_layout) clear(work_->attribute, JournalPage);
            }
            MetadataImage *image;
            error = journal_.add(MetadataKind::IndexBuffer, dir.inode.reference, IndexAllocation, true, node,
                                 work_->new_clusters[node], 0, JournalPage, work_->attribute, image);
            if (error) return error;
            copy(image->after, work_->page, JournalPage);
        }
    }
    put16(record->after + 40, uint16_t(instance));
    return error;
}

int WriteSession::directories()
{
    for (unsigned i = 0; i < work_->directory_count; ++i)
    {
        int error = rebuild_directory(work_->directories[i]);
        if (error) return error;
    }
    return 0;
}

int WriteSession::update_names(MetadataImage &record, uint64_t size, uint64_t allocated)
{
    unsigned  at = le16(record.after + 20);
    Attribute a;
    int       error;
    unsigned  names = 0;
    while (!(error = next_attribute(record.after, record_, at, a)))
    {
        if (a.type != 0x30) continue;
        if (a.data[8] || le32(a.data + 16) < 66) return -EIO;
        auto *value = const_cast<uint8_t *>(a.data) + le16(a.data + 20);
        Inode parent;
        error = volume_->read_inode(le64(value), parent, *reader_work_);
        if (error) return error;
        DirectoryEdit *dir;
        error = load_directory(parent, dir);
        if (error) return error;
        bool found = false;
        for (unsigned i = 0; i < dir->count; ++i)
        {
            auto &key = dir->keys[i];
            if (key.reference != (uint64_t(le16(record.after + 16)) << 48 | le32(record.after + 44)) ||
                key.length != le32(a.data + 16) || !bytes_equal(key.name, value, key.length))
                continue;
            put64(key.name + 40, allocated);
            put64(key.name + 48, size);
            found = true;
        }
        if (!found) return -EIO;
        put64(value + 40, allocated);
        put64(value + 48, size);
        ++names;
    }
    return error != 1 ? error : names ? 0 : -EOPNOTSUPP;
}

int WriteSession::create(const Inode &parent, const char *name, bool directory, uint64_t time, Inode &result)
{
    int error = check_name(parent, name);
    if (error) return error;
    error = begin();
    if (error) return error;
    DirectoryEdit *dir;
    error = load_directory(parent, dir);
    if (error) return error;
    unsigned position;
    error = find(*dir, name, position);
    if (!error) return -EEXIST;
    if (error != -ENOENT) return error;
    if (dir->count == MaxDirectoryKeys) return -EOPNOTSUPP;
    auto available = [&](uint64_t n) -> bool {
        if (bit(work_->mft_bits, n)) return false;
        for (unsigned k = 0; k < work_->journal.count; ++k)
        {
            const auto &image = work_->journal.images[k];
            if (image.kind == MetadataKind::FileRecord &&
                image.vcn * (JournalPage / record_) + image.within / record_ == n && (le16(image.before + 22) & 1) &&
                !(le16(image.after + 22) & 1))
                return false;
        }
        return true;
    };
    uint64_t number = records_;
    unsigned free   = 0;
    for (uint64_t n = 24; n < records_; ++n)
        if (available(n))
        {
            if (!free) number = n;
            ++free;
        }
    bool reserve = false;
    for (unsigned n = 16; n < 24 && n < records_; ++n)
        if (available(n)) reserve = true;
    if (!free || (!reserve && free == 1 && records_ < MaxWriteRecords))
    {
        error = grow_mft();
        if (error) return error;
        for (number = 24; number < records_; ++number)
            if (available(number)) break;
        if (number == records_) return -ENOSPC;
    }
    MetadataImage *record;
    error = stage_record(number, record);
    if (error) return error;
    if (le32(record->before) == 0x454c4946 && (le16(record->before + 22) & 1)) return -EIO;
    uint16_t sequence = uint16_t(le16(record->before + 16) + 1);
    if (!sequence) sequence = 1;
    new_record(record->after, number, sequence, directory, record_);
    set_bit(work_->mft_bits, number, true);
    // Reuse the parent's valid $Secure identifier. NT ACL enforcement remains
    // the documented local-volume policy, independent from descriptor storage.
    MetadataImage *parent_record;
    error = stage_record(parent.reference & NumberMask, parent_record);
    if (error) return error;
    uint8_t *si = attribute(parent_record->after, 0x10);
    if (!si || si[8]) return -EIO;
    unsigned bytes = le32(si + 16);
    if (bytes != 48 && bytes != 72) return -EOPNOTSUPP;
    copy(work_->page, si + le16(si + 20), bytes);
    for (unsigned i = 0; i < 4; ++i)
        put64(work_->page + i * 8, time);
    put32(work_->page + 32, directory ? 0x10 : 0x20);
    put32(work_->page + 40, 0);
    if (bytes == 72) clear(work_->page + 56, 16); // No inherited quota charge or USN.
    unsigned instance = 0, length = resident(work_->attribute, 0x10, false, work_->page, bytes, instance++);
    error = insert(record->after, work_->attribute, length);
    if (error) return error;
    DirectoryKey created;
    error = key(parent, name, record->after, number | uint64_t(sequence) << 48, created);
    if (error) return error;
    length = resident(work_->attribute, 0x30, false, created.name, created.length, instance++);
    error  = insert(record->after, work_->attribute, length);
    if (error) return error;
    if (directory)
    {
        clear(work_->page, JournalPage);
        put32(work_->page, 0x30);
        put32(work_->page + 4, 1);
        put32(work_->page + 8, JournalPage);
        work_->page[12] = 1;
        put32(work_->page + 16, 16);
        put32(work_->page + 20, 32);
        put32(work_->page + 24, 32);
        index_entry(work_->page + 32, nullptr, false);
        length = resident(work_->attribute, IndexRoot, true, work_->page, 48, instance++);
    }
    else
        length = resident(work_->attribute, Data, false, nullptr, 0, instance++);
    error = insert(record->after, work_->attribute, length);
    if (error) return error;
    put16(record->after + 40, uint16_t(instance));
    dir->keys[dir->count++] = created;
    error                   = directories();
    if (!error) error = finish();
    if (!error) error = volume_->read_inode(created.reference, result, *reader_work_);
    return error;
}

int WriteSession::unlink(const Inode &parent, const char *name)
{
    int error = check_name(parent, name);
    if (error) return error;
    error = begin();
    if (error) return error;
    DirectoryEdit *dir;
    error = load_directory(parent, dir);
    if (error) return error;
    unsigned position;
    error = find(*dir, name, position);
    if (error) return error;
    uint64_t reference = dir->keys[position].reference;
    Inode    inode;
    error = volume_->read_inode(reference, inode, *reader_work_);
    if (error) return error;
    if (inode.links != 1) return -EOPNOTSUPP;
    MetadataImage *record;
    error = load_record(reference, record, inode.directory);
    if (error) return error;
    if (inode.directory)
    {
        struct Count
        {
            unsigned n;
        } count      = {};
        auto visitor = [](void *p, const DirectoryEntry &) -> int {
            ++static_cast<Count *>(p)->n;
            return 0;
        };
        error = volume_->enumerate_directory(inode, visitor, &count, *reader_work_);
        if (error) return error;
        if (count.n) return -ENOTEMPTY;
    }
    unsigned  at = le16(record->after + 20);
    Attribute a;
    while (!(error = next_attribute(record->after, record_, at, a)))
    {
        if (!a.data[8]) continue;
        // Only the unnamed data and $I30 allocation are mutable streams.
        if (!((a.type == Data && !a.data[9]) || (a.type == IndexAllocation && named_i30(a.data)))) return -EOPNOTSUPP;
        clear(&work_->data, sizeof(work_->data));
        bool found = false;
        error      = append_fragment(a.data, a.length, work_->data, clusters_, JournalPage, volume_->bytes_, found);
        if (!error) error = complete_stream(work_->data, JournalPage);
        if (!error) error = release_stream(work_->data);
        if (error) return error;
    }
    if (error != 1) return error;
    for (unsigned i = 0; i < dir->count;)
        if (dir->keys[i].reference == reference)
        {
            for (unsigned j = i + 1; j < dir->count; ++j)
                dir->keys[j - 1] = dir->keys[j];
            --dir->count;
        }
        else
            ++i;
    put16(record->after + 18, 0);
    put16(record->after + 22, 0);
    set_bit(work_->mft_bits, reference & NumberMask, false);
    error = directories();
    if (!error) error = finish();
    return error;
}

int WriteSession::rename(const Inode &parent, const char *name, const Inode &destination, const char *new_name)
{
    int error = check_name(parent, name);
    if (!error) error = check_name(destination, new_name);
    if (error) return error;
    error = begin();
    if (error) return error;
    DirectoryEdit *from, *to;
    error = load_directory(parent, from);
    if (!error) error = load_directory(destination, to);
    if (error) return error;
    unsigned position;
    error = find(*from, name, position);
    if (error) return error;
    uint64_t reference = from->keys[position].reference;
    unsigned existing;
    error = find(*to, new_name, existing);
    if (!error && to->keys[existing].reference != reference) return -EEXIST;
    if (error && error != -ENOENT) return error;
    Inode inode;
    error = volume_->read_inode(reference, inode, *reader_work_);
    if (error) return error;
    if (inode.links != 1) return -EOPNOTSUPP;
    if (inode.directory)
    {
        // Walk parent FILE_NAME references to prevent moving a directory below itself.
        Inode current = destination;
        for (unsigned depth = 0;; ++depth)
        {
            if (current.reference == reference) return -EINVAL;
            if ((current.reference & NumberMask) == 5) break;
            if (depth == MaxDepth) return -EOPNOTSUPP;
            error = volume_->record_read(current.reference & NumberMask, uint16_t(current.reference >> 48),
                                         reader_work_->record);
            if (error) return error;
            uint8_t *fn = attribute(reader_work_->record, 0x30);
            if (!fn || fn[8] || le32(fn + 16) < 66) return -EIO;
            error = volume_->read_inode(le64(fn + le16(fn + 20)), current, *reader_work_);
            if (error) return error;
        }
    }
    MetadataImage *record;
    error = load_record(reference, record, inode.directory);
    if (error) return error;
    // Remove both a long name and its DOS alias in this parent. The replacement
    // has a single Win32 FILE_NAME and one matching index key.
    for (unsigned at = le16(record->after + 20); le32(record->after + at) != 0xffffffffU;)
    {
        uint8_t *p = record->after + at;
        if (le32(p) == 0x30)
        {
            if (p[8] || le32(p + 16) < 66 || le64(p + le16(p + 20)) != parent.reference) return -EOPNOTSUPP;
            error = erase(record->after, p);
            if (error) return error;
        }
        else
            at += le32(p + 4);
    }
    DirectoryKey moved;
    error = key(destination, new_name, record->after, reference, moved);
    if (error) return error;
    unsigned instance = le16(record->after + 40),
             length   = resident(work_->attribute, 0x30, false, moved.name, moved.length, instance++);
    error             = insert(record->after, work_->attribute, length);
    if (error) return error;
    put16(record->after + 40, uint16_t(instance));
    for (unsigned i = 0; i < from->count;)
        if (from->keys[i].reference == reference)
        {
            for (unsigned j = i + 1; j < from->count; ++j)
                from->keys[j - 1] = from->keys[j];
            --from->count;
        }
        else
            ++i;
    if (to->count == MaxDirectoryKeys) return -EOPNOTSUPP;
    to->keys[to->count++] = moved;
    error                 = directories();
    if (!error) error = finish();
    return error;
}

int WriteSession::write_new_data(const Stream &old, uint64_t size, uint64_t offset, const void *input, Size length)
{
    auto         *source      = static_cast<const uint8_t *>(input);
    const Stream &replacement = work_->replacement;
    for (uint64_t at = 0; at < replacement.allocated; at += JournalPage)
    {
        clear(work_->page, JournalPage);
        Size keep = 0;
        if (at < old.size)
        {
            keep = Size(old.size - at);
            if (keep > JournalPage) keep = JournalPage;
            if (keep > size - at) keep = Size(size - at);
        }
        int error = keep ? volume_->stream_read(old, at, work_->page, keep) : 0;
        if (error) return error;
        uint64_t first = at > offset ? at : offset,
                 end   = at + JournalPage < offset + length ? at + JournalPage : offset + length;
        if (first < end) copy(work_->page + first - at, source + first - offset, Size(end - first));
        uint64_t physical;
        error = map(replacement, at, physical);
        if (error) return error;
        if (!device_.write(device_.context, physical, work_->page, JournalPage))
        {
            failed_ = true;
            return -EIO;
        }
    }
    if (replacement.allocated && !device_.flush(device_.context))
    {
        failed_ = true;
        return -EIO;
    }
    return 0;
}

int WriteSession::change_file(const Inode &inode, uint64_t size, uint64_t offset, const void *input, Size length,
                              uint64_t time)
{
    if (size > uint64_t(MaxCowClusters) * JournalPage || length > MaxOverwrite) return -EOPNOTSUPP;
    int error = begin();
    if (error) return error;
    MetadataImage *record;
    error = load_record(inode.reference, record);
    if (error) return error;
    error = volume_->stream_from_record(record->before, Data, false, work_->data, *reader_work_, inode.reference);
    if (error) return error;
    if (work_->data.flags) return -EOPNOTSUPP;
    uint8_t *p = attribute(record->after, Data);
    if (!p) return -EIO;
    unsigned instance = le16(p + 14);
    error             = erase(record->after, p);
    if (error) return error;
    // Every modified nonresident stream is replaced using free clusters. Old
    // contents remain reachable until the metadata commit decision is durable.
    unsigned count         = unsigned((size + JournalPage - 1) / JournalPage), attr_length;
    bool     resident_data = size <= 512 && aligned(24 + unsigned(size)) <= record_ - le32(record->after + 24);
    clear(&work_->replacement, sizeof(work_->replacement));
    if (resident_data)
    {
        clear(work_->page, 512);
        unsigned keep = unsigned(size < work_->data.size ? size : work_->data.size);
        if (keep)
        {
            error = volume_->stream_read(work_->data, 0, work_->page, keep);
            if (error) return error;
        }
        if (length) copy(work_->page + offset, input, length);
        attr_length = resident(work_->attribute, Data, false, work_->page, unsigned(size), instance);
    }
    else
    {
        error = allocate(count);
        if (error) return error;
        attr_length = nonresident(work_->attribute, Data, false, work_->allocated, work_->allocated_count, size,
                                  uint64_t(count) * JournalPage, instance);
        if (!attr_length) return -EOPNOTSUPP;
        work_->replacement.allocated = uint64_t(count) * JournalPage;
        work_->replacement.size = work_->replacement.initialized = size;
        work_->replacement.count                                 = work_->allocated_count;
        for (unsigned i = 0; i < work_->allocated_count; ++i)
            work_->replacement.runs[i] = work_->allocated[i];
    }
    error = insert(record->after, work_->attribute, attr_length);
    if (error) return error;
    error = release_stream(work_->data);
    if (error) return error;
    p = attribute(record->after, 0x10);
    if (!p || p[8]) return -EIO;
    if (time)
    {
        uint8_t *value = p + le16(p + 20);
        put64(value + 8, time);
        put64(value + 16, time);
    }
    error = update_names(*record, size, resident_data ? size : work_->replacement.allocated);
    if (!error) error = directories();
    if (!error) error = stage_bitmaps();
    // Validate and reserve every metadata page before writing unreachable data.
    // commit() repeats validation with final LSNs, before publishing the log.
    if (!error)
    {
        for (unsigned i = 0; i < work_->journal.count; ++i)
        {
            auto &image = work_->journal.images[i];
            if (image.kind == MetadataKind::Raw)
                copy(image.packed, image.after, image.length);
            else
            {
                error = pack(image.after, image.length, image.packed);
                if (error) break;
            }
        }
    }
    if (!error) error = validate();
    if (!error && !resident_data) error = write_new_data(work_->data, size, offset, input, length);
    if (!error) error = journal_.commit(validate_callback, this);
    if (!error) error = volume_->mount(device_.read, device_.context, device_.bytes, *reader_work_);
    if (error && (journal_.failed() || volume_->io_failed_)) failed_ = true;
    return error;
}

int WriteSession::resize(const Inode &inode, uint64_t size, uint64_t time)
{
    return change_file(inode, size, 0, nullptr, 0, time);
}

int WriteSession::pwrite(const Inode &inode, uint64_t offset, const void *input, Size length, uint64_t time,
                         Size &completed)
{
    completed = 0;
    if ((!input && length) || offset > uint64_t(__INT64_MAX__) || length > uint64_t(__INT64_MAX__) - offset)
        return -EINVAL;
    if (!length) return failed() ? -EIO : 0;
    if (!opened_ || failed()) return -EIO;
    Inode current;
    int   error = volume_->read_inode(inode.reference, current, *reader_work_);
    if (error) return error;
    uint64_t size = current.size > offset + length ? current.size : offset + length;
    error         = change_file(current, size, offset, input, length, time);
    if (!error) completed = length;
    return error;
}
} // namespace xjntfs
