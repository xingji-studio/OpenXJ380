// SPDX-License-Identifier: Apache-2.0
// Portions adapted from Uinxed-Kernel ntfs_runlist.c and ntfs_mft.c:
// Copyright (C) 2020 ViudiraTech. 2026/7/26 By MicroFish & JiTianYu391.
// Modified for XJ380: bounded parsing, fixed 512-byte USA
// stride, explicit errors and independent workspace. See PROVENANCE.md.
#include "native.h"
#include "format.h"
#include <errno.h>

namespace xjntfs {
using namespace format;

int Volume::fetch(uint64_t offset, void *output, Size length) const
{
    if (io_failed_) return -EIO;
    if (!reader_ || offset > bytes_ || length > bytes_ - offset) return -EIO;
    if (!length || reader_(context_, offset, output, length)) return 0;
    if (writer_) io_failed_ = true;
    return -EIO;
}

int Volume::visit_attributes(const uint8_t *base, uint64_t reference, uint32_t filter, bool i30,
                             AttributeVisitor visitor, void *context, Workspace &work) const
{
    const uint8_t *list        = nullptr;
    unsigned       list_length = 0, offset = le16(base + 20);
    Attribute      attribute;
    int            status;
    while (!(status = next_attribute(base, record_, offset, attribute)))
        if (attribute.type == 0x20)
        {
            if (list || attribute.data[9]) return -EIO;
            list        = attribute.data;
            list_length = attribute.length;
        }
    if (status != 1) return status;
    if (!list)
    {
        offset = le16(base + 20);
        while (!(status = next_attribute(base, record_, offset, attribute)))
        {
            const uint8_t *p = attribute.data;
            if (filter && (attribute.type != filter || (i30 ? !named_i30(p) : p[9] != 0))) continue;
            status = visitor(context, p, attribute.length, reference);
            if (status) return status;
        }
        return status == 1 ? 0 : status;
    }
    clear(&work.list_stream, sizeof(work.list_stream));
    bool found = false;
    status     = append_fragment(list, list_length, work.list_stream, clusters_, cluster_, bytes_, found);
    if (!status) status = complete_stream(work.list_stream, cluster_);
    if (status) return status;
    if (work.list_stream.flags || !work.list_stream.size || work.list_stream.initialized != work.list_stream.size)
        return -EIO;
    if (work.list_stream.size > MaxAttributeList) return -EOPNOTSUPP;
    status = stream_read(work.list_stream, 0, work.attribute_list, Size(work.list_stream.size));
    if (status) return status;
    unsigned position = 0, identities = 0;
    uint32_t previous_type = 0;
    while (position < work.list_stream.size)
    {
        unsigned       remaining = unsigned(work.list_stream.size) - position;
        const uint8_t *entry     = work.attribute_list + position;
        if (remaining < 26)
        {
            if (remaining > 7) return -EIO;
            for (unsigned i = 0; i < remaining; ++i)
                if (entry[i]) return -EIO;
            break;
        }
        unsigned length = le16(entry + 4), names = unsigned(entry[6]) * 2, name_offset = entry[7];
        uint32_t type  = le32(entry);
        uint64_t owner = le64(entry + 16), lowest = le64(entry + 8);
        uint16_t instance = le16(entry + 24);
        if (!type || type % 16 || type == 0x20 || type < previous_type || length < 26 || length > remaining ||
            (length % 8 && length != remaining) ||
            (names && (name_offset < 26 || name_offset > length || names > length - name_offset)) || !(owner >> 48))
            return -EIO;
        previous_type = type;
        if (identities == MaxAttributeEntries) return -EOPNOTSUPP;
        for (unsigned i = 0; i < identities; ++i)
            if (work.seen[i].reference == owner && work.seen[i].instance == instance) return -EIO;
        work.seen[identities++] = {owner, instance};
        bool selected_name      = !entry[6];
        if (i30)
            selected_name = entry[6] == 4 && le16(entry + name_offset) == '$' && le16(entry + name_offset + 2) == 'I' &&
                            le16(entry + name_offset + 4) == '3' && le16(entry + name_offset + 6) == '0';
        bool selected = !filter || (type == filter && selected_name);
        if (selected)
        {
            const uint8_t *record = base;
            if (owner != reference)
            {
                // No recursion or guessed physical record addresses. Sequence
                // and base-owner checks prevent stale/cross-file references.
                status = record_read(owner & NumberMask, uint16_t(owner >> 48), work.extension, reference);
                if (status) return status;
                record = work.extension;
            }
            offset       = le16(record + 20);
            bool matched = false;
            while (!(status = next_attribute(record, record_, offset, attribute)))
            {
                const uint8_t *p = attribute.data;
                if (owner != reference && attribute.type == 0x20) return -EIO;
                if (attribute.type != type || le16(p + 14) != instance || !same_attribute_name(p, entry)) continue;
                if (matched || (p[8] ? le64(p + 16) : 0) != lowest) return -EIO;
                matched = true;
                status  = visitor(context, p, attribute.length, owner);
                if (status) return status;
            }
            if (status != 1) return status;
            if (!matched) return -EIO;
        }
        position += length;
    }
    // The list is authoritative, but may not silently omit a base attribute.
    offset = le16(base + 20);
    while (!(status = next_attribute(base, record_, offset, attribute)))
    {
        if (attribute.type == 0x20) continue;
        bool listed = false;
        for (unsigned i = 0; i < identities; ++i)
            if (work.seen[i].reference == reference && work.seen[i].instance == le16(attribute.data + 14))
                listed = true;
        if (!listed) return -EIO;
    }
    return status == 1 ? 0 : status;
}

int Volume::stream_from_record(const uint8_t *record, uint32_t type, bool i30, Stream &stream, Workspace &work,
                               uint64_t reference, bool bootstrap) const
{
    clear(&stream, sizeof(stream));
    if (!reference) reference = record_reference(record);
    struct Builder
    {
        Stream        *stream;
        uint64_t       clusters, bytes;
        uint32_t       cluster;
        bool           found;
        const uint8_t *seed;
    } builder = {&stream, clusters_, bytes_, cluster_, false, nullptr};
    if (bootstrap)
    {
        // Seed the first $MFT extent before following its ATTRIBUTE_LIST:
        // extension records themselves must be read through this known map.
        unsigned  offset = le16(record + 20);
        Attribute attribute;
        int       status;
        while (!(status = next_attribute(record, record_, offset, attribute)))
            if (attribute.type == type && !attribute.data[9] && attribute.data[8] && !le64(attribute.data + 16))
            {
                if (builder.seed) return -EIO;
                builder.seed = attribute.data;
                status       = append_fragment(attribute.data, attribute.length, stream, clusters_, cluster_, bytes_,
                                               builder.found);
                if (status) return status;
                if (stream.flags) return -EOPNOTSUPP; // $MFT cannot use sparse/compressed bootstrap mappings.
            }
        if (status != 1) return status;
        if (!builder.seed) return -EIO;
    }
    auto append = [](void *context, const uint8_t *p, unsigned length, uint64_t) -> int {
        auto &builder = *static_cast<Builder *>(context);
        if (p == builder.seed) return 0;
        return append_fragment(p, length, *builder.stream, builder.clusters, builder.cluster, builder.bytes,
                               builder.found);
    };
    int error = visit_attributes(record, reference, type, i30, append, &builder, work);
    if (error) return error;
    return builder.found ? complete_stream(stream, cluster_) : -ENOENT;
}

int Volume::stream_read(const Stream &stream, uint64_t offset, void *output, Size length) const
{
    if (offset > stream.size || length > stream.size - offset || (!output && length)) return -EIO;
    auto *out = static_cast<uint8_t *>(output);
    if (stream.resident)
    {
        if (length) copy(out, stream.value + offset, length);
        return 0;
    }
    while (length)
    {
        Size amount = length;
        if (offset >= stream.initialized)
        {
            clear(out, length);
            return 0;
        }
        if (amount > stream.initialized - offset) amount = Size(stream.initialized - offset);
        uint64_t   vcn = offset / cluster_;
        const Run *run = nullptr;
        for (unsigned i = 0; i < stream.count; ++i)
            if (vcn >= stream.runs[i].vcn && vcn - stream.runs[i].vcn < stream.runs[i].length)
            {
                run = &stream.runs[i];
                break;
            }
        if (!run) return -EIO; // Missing mappings are corruption, not sparse holes.
        uint64_t in_run    = offset - run->vcn * cluster_;
        uint64_t available = run->length * cluster_ - in_run;
        if (amount > available) amount = Size(available);
        if (run->lcn < 0)
            clear(out, amount);
        else
        {
            int error = fetch(uint64_t(run->lcn) * cluster_ + in_run, out, amount);
            if (error) return error;
        }
        out    += amount;
        offset += amount;
        length -= amount;
    }
    return 0;
}

int Volume::record_read(uint64_t number, uint16_t sequence, uint8_t *buffer, uint64_t owner) const
{
    if (!record_ || number >= mft_.size / record_) return -EIO;
    int error = stream_read(mft_, number * record_, buffer, record_);
    return error ? error : record_layout(buffer, record_, number, sequence, owner);
}

int Volume::inode_from_record(const uint8_t *record, uint64_t number, Inode &inode, Workspace &work) const
{
    clear(&inode, sizeof(inode));
    inode.reference = number | uint64_t(le16(record + 16)) << 48;
    inode.links     = le16(record + 18);
    inode.directory = (le16(record + 22) & 2) != 0;
    struct Metadata
    {
        Inode *inode;
        bool   standard, data;
    } metadata   = {&inode, false, false};
    auto visitor = [](void *context, const uint8_t *p, unsigned, uint64_t) -> int {
        auto    &metadata = *static_cast<Metadata *>(context);
        Inode   &inode    = *metadata.inode;
        uint32_t type     = le32(p);
        if (type == 0x10 && !p[9])
        {
            if (metadata.standard || p[8] || le32(p + 16) < 48 || le16(p + 12)) return -EIO;
            metadata.standard    = true;
            const uint8_t *value = p + le16(p + 20);
            inode.created        = le64(value);
            inode.modified       = le64(value + 8);
            inode.accessed       = le64(value + 24);
            inode.attributes     = le32(value + 32);
            inode.case_sensitive = (le32(value + 40) & 1) != 0;
        }
        if (type == 0xc0) inode.unsupported = true;
        if (type == Data && !p[9] && (!p[8] || !le64(p + 16)))
        {
            if (metadata.data) return -EIO;
            metadata.data     = true;
            inode.nonresident = p[8] != 0;
            inode.size        = p[8] ? le64(p + 48) : le32(p + 16);
            inode.allocated   = p[8] ? le64(p + 40) : inode.size;
            if (le16(p + 12) & ~0x8000U) inode.unsupported = true;
        }
        return 0;
    };
    int status = visit_attributes(record, inode.reference, 0, false, visitor, &metadata, work);
    if (status) return status;
    if (!metadata.standard || (!metadata.data && !inode.directory && !inode.unsupported)) return -EIO;
    if (inode.size > uint64_t(__INT64_MAX__)) return -EIO;
    inode.unsupported = inode.unsupported || (inode.attributes & Unsupported);
    return 0;
}

namespace {

struct RestartState
{
    uint64_t lsn, file_size;
    uint32_t revision, system_page, log_page;
    uint16_t major, minor;
    bool     clean;
};

// Original read-only validator of published LFS layout facts. This neither
// interprets transaction operations nor changes/replays a restart area.
static int restart_state(uint8_t *page, unsigned bytes, uint64_t stream_size, RestartState &state)
{
    int error = unpack(page, bytes, 0x52545352); // RSTR
    if (error) return error;
    state.system_page = le32(page + 16);
    state.log_page    = le32(page + 20);
    state.minor       = le16(page + 26);
    state.major       = le16(page + 28);
    if (state.system_page != bytes || !power_of_two(state.log_page) || state.log_page < 512 ||
        state.log_page > MaxLogPage)
        return -EIO;
    if (!((state.major == 1 && state.minor == 1) || (state.major == 2 && !state.minor))) return -EOPNOTSUPP;
    unsigned offset = le16(page + 24), usa_end = le16(page + 4) + le16(page + 6) * 2;
    if (offset < 32 || offset % 8 || offset < usa_end || offset > bytes || bytes - offset < 48) return -EIO;
    const uint8_t *area    = page + offset;
    unsigned       clients = le16(area + 8), free_head = le16(area + 10), in_use = le16(area + 12);
    unsigned       flags = le16(area + 14), bits = le32(area + 16), length = le16(area + 20);
    unsigned       array = le16(area + 22), record_header = le16(area + 36), data_offset = le16(area + 38);
    if (flags & ~3U) return -EOPNOTSUPP;
    if (clients > 64) return -EOPNOTSUPP;
    state.file_size = le64(area + 24);
    state.lsn       = le64(area);
    state.revision  = le32(area + 40);
    if (length < 48 || length > bytes - offset || array < 48 || array % 8 || array > length ||
        clients * 160 > length - array || state.file_size > stream_size ||
        state.file_size < uint64_t(bytes) * 2 + uint64_t(state.log_page) * 2 || state.file_size % state.log_page ||
        !bits || bits >= 64 || 64 - bits >= 61 || (uint64_t(1) << (64 - bits)) < (state.file_size + 7) / 8 ||
        record_header < 40 || record_header % 8 || record_header > state.log_page / 2 || data_offset < 40 ||
        data_offset % 8 || data_offset >= state.log_page)
        return -EIO;
    uint64_t lsn_mask = (uint64_t(1) << (64 - bits)) - 1;
    if ((state.lsn & lsn_mask) >= state.file_size / 8) return -EIO;
    bool seen[64] = {};
    // Both lists must be bounded, disjoint and complete. Free slots need not
    // contain meaningful old client names; only active NTFS clients are used.
    for (unsigned list = 0; list < 2; ++list)
    {
        unsigned current = list ? in_use : free_head, previous = 0xffff;
        for (unsigned visited = 0; current != 0xffff; ++visited)
        {
            if (visited >= clients || current >= clients || seen[current]) return -EIO;
            seen[current]         = true;
            const uint8_t *client = area + array + current * 160;
            if (le16(client + 16) != previous) return -EIO;
            if (list)
            {
                unsigned name_length = le32(client + 28);
                if (name_length != 8 || le16(client + 32) != 'N' || le16(client + 34) != 'T' ||
                    le16(client + 36) != 'F' || le16(client + 38) != 'S')
                    return -EOPNOTSUPP;
                uint64_t oldest = le64(client), restart = le64(client + 8);
                if (oldest > state.lsn || restart > state.lsn ||
                    (oldest && (oldest & lsn_mask) >= state.file_size / 8) ||
                    (restart && (restart & lsn_mask) >= state.file_size / 8))
                    return -EIO;
            }
            previous = current;
            current  = le16(client + 18);
        }
    }
    for (unsigned i = 0; i < clients; ++i)
        if (!seen[i]) return -EIO;
    state.clean = (flags & 2) || in_use == 0xffff;
    return 0;
}

} // namespace

int Volume::check_log(Workspace &work) const
{
    int error = record_read(2, 0, work.record);
    if (!error) error = stream_from_record(work.record, Data, false, work.stream, work);
    if (error) return error;
    if (work.stream.size < 32 || work.stream.initialized != work.stream.size || work.stream.flags) return -EIO;
    error = stream_read(work.stream, 0, work.log_page, 32);
    if (error) return error;
    bool uninitialized = true;
    for (unsigned i = 0; i < 32; ++i)
        if (work.log_page[i] != 0xff) uninitialized = false;
    if (uninitialized)
    {
        if (work.stream.size > 64 * 1024 * 1024) return -EOPNOTSUPP;
        for (uint64_t offset = 0; offset < work.stream.size;)
        {
            Size length = MaxLogPage;
            if (length > work.stream.size - offset) length = Size(work.stream.size - offset);
            error = stream_read(work.stream, offset, work.log_page, length);
            if (error) return error;
            for (Size i = 0; i < length; ++i)
                if (work.log_page[i] != 0xff) return -EUCLEAN;
            offset += length;
        }
        return 0;
    }
    unsigned page_size = le32(work.log_page + 16);
    if (!power_of_two(page_size) || page_size < 512 || page_size > MaxLogPage ||
        work.stream.size < uint64_t(page_size) * 2)
        return -EIO;
    RestartState pages[2] = {};
    // Reject damaged redundancy rather than silently treating an unknown newer
    // page as clean. Repair/replay is outside this read-only driver.
    for (unsigned i = 0; i < 2; ++i)
    {
        error = stream_read(work.stream, uint64_t(i) * page_size, work.log_page, page_size);
        if (!error) error = restart_state(work.log_page, page_size, work.stream.size, pages[i]);
        if (error) return error;
    }
    if (pages[0].file_size != pages[1].file_size || pages[0].major != pages[1].major ||
        pages[0].minor != pages[1].minor || pages[0].log_page != pages[1].log_page)
        return -EIO;
    unsigned latest = 0;
    if (pages[0].lsn != pages[1].lsn)
    {
        uint64_t difference = pages[0].lsn > pages[1].lsn ? pages[0].lsn - pages[1].lsn : pages[1].lsn - pages[0].lsn;
        if (difference >= (uint64_t(1) << 63)) return -EOPNOTSUPP;
        latest = pages[1].lsn > pages[0].lsn;
    }
    else if (pages[0].revision != pages[1].revision)
    {
        uint32_t difference = pages[1].revision - pages[0].revision;
        if (difference == 0x80000000U) return -EIO;
        latest = difference < 0x80000000U;
    }
    else if (pages[0].clean != pages[1].clean)
        return -EIO;
    return pages[latest].clean ? 0 : -EUCLEAN;
}

int Volume::mount(Reader reader, void *context, uint64_t bytes, Workspace &work)
{
    return mount_internal(reader, context, bytes, work, false);
}

int Volume::mount_internal(Reader reader, void *context, uint64_t bytes, Workspace &work, bool staged)
{
    ready_     = false;
    io_failed_ = false;
    writer_    = nullptr;
    flusher_   = nullptr;
    reader_    = reader;
    context_   = context;
    bytes_     = bytes;
    clear(&mft_, sizeof(mft_));
    if (!reader || bytes < 512) return -EINVAL;
    int error = fetch(0, work.record, 512);
    if (error) return error;
    const uint8_t *boot    = work.record;
    const char     magic[] = "NTFS    ";
    for (unsigned i = 0; i < 8; ++i)
        if (boot[3 + i] != uint8_t(magic[i])) return -EINVAL;
    if (le16(boot + 510) != 0xaa55 || le16(boot + 14) || boot[16] || le16(boot + 17) || le16(boot + 19) ||
        le16(boot + 22) || le32(boot + 32))
        return -EIO;
    sector_      = le16(boot + 11);
    unsigned spc = boot[13];
    if (!power_of_two(sector_) || sector_ < 512 || sector_ > 4096 || !power_of_two(spc) || spc > 128)
        return -EOPNOTSUPP;
    cluster_         = sector_ * spc;
    uint64_t sectors = le64(boot + 40), mft = le64(boot + 48), mirror = le64(boot + 56);
    if (!sectors || sectors > bytes / sector_) return -EIO;
    bytes_      = sectors * sector_;
    clusters_   = sectors / spc;
    mirror_lcn_ = mirror;
    if (mft >= clusters_ || mirror >= clusters_ || mft == mirror) return -EIO;
    error = decode_size(boot[64], cluster_, MaxRecord, record_);
    if (!error) error = decode_size(boot[68], cluster_, MaxIndex, index_);
    if (error) return error;
    if (cluster_ > index_) return -EOPNOTSUPP;
    error = fetch(mft * cluster_, work.record, record_);
    if (!error) error = record_layout(work.record, record_, 0, 0);
    if (!error) error = stream_from_record(work.record, Data, false, mft_, work, 0, true);
    if (error) return error;
    if (mft_.resident || mft_.initialized != mft_.size || mft_.size % record_ || mft_.size / record_ < 16 ||
        !mft_.count || uint64_t(mft_.runs[0].lcn) != mft)
        return -EIO;
    for (unsigned i = 0; i < mft_.count; ++i)
        if (mft_.runs[i].lcn < 0) return -EIO;
    error = record_read(3, 0, work.record);
    if (!error) error = stream_from_record(work.record, 0x70, false, work.stream, work);
    if (error) return error;
    if (!work.stream.resident || work.stream.size < 12) return -EIO;
    if (work.stream.value[8] != 3 || work.stream.value[9] != 1) return -EOPNOTSUPP;
    uint16_t volume_flags = le16(work.stream.value + 10);
    if ((volume_flags & 1) && !staged) return -EUCLEAN;
    // Persistent policy bits do not request recovery. Unknown/repair/upgrade
    // states remain refused even though this driver never modifies the volume.
    if (volume_flags & ~(0x3c80U | (staged ? 1U : 0U))) return -EOPNOTSUPP;
    error = staged ? 0 : check_log(work);
    if (error) return error;
    error = record_read(10, 0, work.record);
    if (!error) error = stream_from_record(work.record, Data, false, work.stream, work);
    if (error) return error;
    if (work.stream.size != sizeof(upcase_) || work.stream.initialized != sizeof(upcase_) || work.stream.flags)
        return -EIO;
    error = stream_read(work.stream, 0, upcase_, sizeof(upcase_));
    if (error) return error;
    auto *raw_upcase = reinterpret_cast<uint8_t *>(upcase_);
    for (unsigned i = 0; i < 65536; ++i)
        upcase_[i] = le16(raw_upcase + i * 2);
    if (upcase_[0]) return -EIO;
    for (unsigned i = 0; i < 65536; ++i)
        if (upcase_[upcase_[i]] != upcase_[i] || (i >= 0xd800 && i <= 0xdfff && upcase_[i] != i)) return -EIO;
    for (unsigned i = 0; i < 26; ++i)
        if (upcase_['a' + i] != 'A' + i || upcase_['A' + i] != 'A' + i) return -EIO;
    ready_ = true;
    Inode directory;
    error = root(directory, work);
    if (!error)
    {
        uint64_t hibernation_file = 0;
        auto     hibernation      = [](void *context, const DirectoryEntry &entry) -> int {
            if (equal(entry.name, "hiberfil.sys", true))
            {
                auto &reference = *static_cast<uint64_t *>(context);
                if (reference && reference != entry.reference) return -EIO;
                reference = entry.reference;
            }
            return 0;
        };
        error = enumerate_directory(directory, hibernation, &hibernation_file, work);
        if (!error && hibernation_file)
        {
            Inode hiber;
            error      = read_inode(hibernation_file, hiber, work);
            Size count = 0;
            if (!error && hiber.unsupported) error = -EOPNOTSUPP;
            if (!error) error = read(hiber, 0, work.log_page, 4096, count, work);
            // Windows invalidates an inactive hibernation header with zeros.
            // Any other state (including hibr/wake or unknown signatures) is
            // refused, so Fast Startup never becomes an ordinary clean mount.
            if (!error)
                for (Size i = 0; i < count; ++i)
                    if (work.log_page[i])
                    {
                        error = -EOPNOTSUPP;
                        break;
                    }
        }
    }
    if (error) ready_ = false;
    return error;
}

int Volume::root(Inode &inode, Workspace &work) const
{
    if (!ready_) return -ENODEV;
    int error = record_read(5, 0, work.record);
    if (!error) error = inode_from_record(work.record, 5, inode, work);
    return error ? error : inode.directory && !inode.unsupported ? 0 : -EIO;
}

int Volume::read_inode(uint64_t reference, Inode &inode, Workspace &work) const
{
    if (!ready_) return -ENODEV;
    uint16_t sequence = uint16_t(reference >> 48);
    if (!sequence) return -EIO;
    int error = record_read(reference & NumberMask, sequence, work.record);
    return error ? error : inode_from_record(work.record, reference & NumberMask, inode, work);
}

int Volume::read(const Inode &inode, uint64_t offset, void *output, Size length, Size &completed, Workspace &work) const
{
    completed = 0;
    if (!ready_) return -ENODEV;
    if (!output && length) return -EINVAL;
    if (inode.directory) return -EISDIR;
    if (inode.unsupported) return -EOPNOTSUPP;
    if (!(inode.reference >> 48)) return -EIO;
    int   error = record_read(inode.reference & NumberMask, uint16_t(inode.reference >> 48), work.record);
    Inode current;
    if (!error) error = inode_from_record(work.record, inode.reference & NumberMask, current, work);
    if (!error && current.directory) error = -EISDIR;
    if (!error && current.unsupported) error = -EOPNOTSUPP;
    if (!error) error = stream_from_record(work.record, Data, false, work.stream, work);
    if (error) return error;
    if (work.stream.size != inode.size) return -EIO;
    if (!length || offset >= inode.size) return 0;
    if (length > inode.size - offset) length = Size(inode.size - offset);
    error = stream_read(work.stream, offset, output, length);
    if (!error) completed = length;
    return error;
}

// Activating the experimental writer is separate from mounting. A Reader-only
// mount can never acquire write capability through an ioctl or a file request.
int Volume::enable_overwrite(Writer writer, Flusher flusher, Workspace &work)
{
    if (io_failed_) return -EIO;
    if (!ready_) return -ENODEV;
    if (!writer || !flusher || writer_) return -EINVAL;
    if (mft_.size / record_ > MaxWriteRecords) return -EOPNOTSUPP;
    int error = record_read(0, 0, work.record);
    if (!error) error = stream_from_record(work.record, Bitmap, false, mft_bitmap_, work);
    if (!error) error = record_read(6, 0, work.record);
    if (!error) error = stream_from_record(work.record, Data, false, cluster_bitmap_, work);
    if (error) return error;
    if (mft_bitmap_.size < (mft_.size / record_ + 7) / 8 || mft_bitmap_.initialized != mft_bitmap_.size ||
        mft_bitmap_.flags || cluster_bitmap_.size < (clusters_ + 7) / 8 ||
        cluster_bitmap_.initialized != cluster_bitmap_.size || cluster_bitmap_.flags)
        return -EIO;
    // Kernel callers must additionally authenticate a real device durability
    // capability. Merely returning success from an unknown ioctl is insufficient.
    if (!flusher(context_))
    {
        io_failed_ = true;
        return -EIO;
    }
    writer_  = writer;
    flusher_ = flusher;
    return 0;
}

int Volume::sync()
{
    if (io_failed_) return -EIO;
    if (!ready_) return -ENODEV;
    if (!flusher_) return 0;
    if (flusher_(context_)) return 0;
    io_failed_ = true;
    return -EIO;
}

int Volume::overwrite(const Inode &inode, uint64_t offset, const void *input, Size length, Size &completed,
                      Workspace &work)
{
    completed = 0;
    if (io_failed_) return -EIO;
    if (!ready_) return -ENODEV;
    if (!writer_ || !flusher_) return -EROFS;
    if (!input && length) return -EINVAL;
    if (length > MaxOverwrite) return -EOPNOTSUPP;
    if (!(inode.reference >> 48)) return -EIO;
    int   error = record_read(inode.reference & NumberMask, uint16_t(inode.reference >> 48), work.record);
    Inode current;
    if (!error) error = inode_from_record(work.record, inode.reference & NumberMask, current, work);
    if (error) return error;
    if (current.directory) return -EISDIR;
    if ((inode.reference & NumberMask) < 16 || (current.attributes & (1U | 4U))) return -EPERM;
    if (current.unsupported) return -EOPNOTSUPP;
    if (current.size != inode.size) return -EIO;
    error = stream_from_record(work.record, Data, false, work.allocation, work);
    if (error) return error;
    Stream &target = work.allocation;
    if (target.resident || target.flags) return -EOPNOTSUPP;
    if (offset > target.size || length > target.size - offset || offset > target.initialized ||
        length > target.initialized - offset)
        return -EFBIG;
    if (!length) return 0;
    // Prove ownership of the entire target stream, not just a single lookup's
    // mapping. Otherwise a corrupt runlist could turn file I/O into metadata I/O.
    struct Identities
    {
        Workspace *work;
        unsigned   count;
    } identities  = {&work, 0};
    auto remember = [](void *context, const uint8_t *p, unsigned, uint64_t reference) -> int {
        auto &state = *static_cast<Identities *>(context);
        if (state.count == MaxAttributeEntries) return -EOPNOTSUPP;
        state.work->write_seen[state.count++] = {reference, le16(p + 14)};
        return 0;
    };
    error = visit_attributes(work.record, inode.reference, Data, false, remember, &identities, work);
    if (error) return error;
    uint64_t mirror_clusters = (uint64_t(record_) * 4 + cluster_ - 1) / cluster_;
    uint64_t boot_clusters   = (uint64_t(sector_) * 16 + cluster_ - 1) / cluster_;
    if (mirror_clusters > clusters_ - mirror_lcn_) return -EIO;
    for (unsigned i = 0; i < target.count; ++i)
    {
        const Run &run = target.runs[i];
        if (run.lcn <= 0 || uint64_t(run.lcn) >= clusters_ - 1 || run.length > clusters_ - 1 - uint64_t(run.lcn))
            return -EIO;
        // BPB locations remain protected if $Boot/$MFTMirr maps are damaged.
        if (uint64_t(run.lcn) < boot_clusters ||
            (uint64_t(run.lcn) < mirror_lcn_ + mirror_clusters && mirror_lcn_ < uint64_t(run.lcn) + run.length))
            return -EIO;
        for (unsigned j = 0; j < i; ++j)
        {
            const Run &other = target.runs[j];
            if (uint64_t(run.lcn) < uint64_t(other.lcn) + other.length &&
                uint64_t(other.lcn) < uint64_t(run.lcn) + run.length)
                return -EIO;
        }
    }
    uint64_t records = mft_.size / record_, bitmap_bytes = (records + 7) / 8;
    error = stream_read(mft_bitmap_, 0, work.log_page, Size(bitmap_bytes));
    if (error) return error;
    for (unsigned i = 0; i < 12; ++i)
        if (!(work.log_page[i / 8] & (1U << (i % 8)))) return -EIO;
    for (unsigned i = 0; i < identities.count; ++i)
    {
        uint64_t number = work.write_seen[i].reference & NumberMask;
        if (number >= records || !(work.log_page[number / 8] & (1U << (number % 8)))) return -EIO;
    }
    unsigned comparisons = 0;
    for (uint64_t number = 0; number < records; ++number)
    {
        if (!(work.log_page[number / 8] & (1U << (number % 8))))
        {
            error = stream_read(mft_, number * record_, work.record, 32);
            if (error) return error;
            if (le32(work.record) == 0x454c4946 && (le16(work.record + 22) & 1)) return -EIO;
            continue;
        }
        error = stream_read(mft_, number * record_, work.record, record_);
        if (!error) error = record_layout(work.record, record_, number, 0, le64(work.record + 32));
        if (error) return error;
        uint64_t  reference = record_reference(work.record);
        unsigned  position  = le16(work.record + 20);
        Attribute attribute;
        while (!(error = next_attribute(work.record, record_, position, attribute)))
        {
            const uint8_t *p = attribute.data;
            if (!p[8]) continue;
            bool ours = false;
            for (unsigned i = 0; i < identities.count; ++i)
                if (work.write_seen[i].reference == reference && work.write_seen[i].instance == le16(p + 14))
                    ours = true;
            if (ours)
            {
                if (attribute.type != Data || p[9]) return -EIO;
                continue;
            }
            clear(&work.stream, sizeof(work.stream));
            unsigned mapping = le16(p + 32);
            error = decode_runs(p + mapping, attribute.length - mapping, clusters_, le64(p + 16), le64(p + 24), true,
                                work.stream);
            // An empty nonresident attribute has no physical ownership.
            if (error && !le64(p + 16) && le64(p + 24) == __UINT64_MAX__ && !p[mapping] && !le64(p + 40) &&
                !le64(p + 48) && !le64(p + 56))
                error = 0;
            if (error) return error;
            for (unsigned i = 0; i < work.stream.count; ++i)
            {
                const Run &other = work.stream.runs[i];
                if (other.lcn < 0) continue;
                for (unsigned j = 0; j < target.count; ++j)
                {
                    if (++comparisons > MaxWriteComparisons) return -EOPNOTSUPP;
                    const Run &run = target.runs[j];
                    if (uint64_t(run.lcn) < uint64_t(other.lcn) + other.length &&
                        uint64_t(other.lcn) < uint64_t(run.lcn) + run.length)
                        return -EIO;
                }
            }
        }
        if (error != 1) return error;
    }
    // Every affected cluster must already be allocated. Allocation and VDL/EOF
    // changes would require metadata transactions and are deliberately absent.
    uint64_t end = offset + length, cached_bitmap_page = __UINT64_MAX__;
    for (const Run *run = target.runs; run != target.runs + target.count; ++run)
    {
        uint64_t first = run->vcn * cluster_, last = first + run->length * cluster_;
        if (offset >= last || end <= first) continue;
        uint64_t from = offset > first ? offset : first, until = end < last ? end : last;
        uint64_t cluster = uint64_t(run->lcn) + (from - first) / cluster_;
        uint64_t stop    = uint64_t(run->lcn) + (until - first - 1) / cluster_ + 1;
        for (; cluster < stop; ++cluster)
        {
            uint64_t byte = cluster / 8, page = byte / MaxRecord;
            if (page != cached_bitmap_page)
            {
                uint64_t start = page * MaxRecord;
                Size     size  = MaxRecord;
                if (size > cluster_bitmap_.size - start) size = Size(cluster_bitmap_.size - start);
                error = stream_read(cluster_bitmap_, start, work.index, size);
                if (error) return error;
                cached_bitmap_page = page;
            }
            if (!(work.index[byte % MaxRecord] & (1U << (cluster % 8)))) return -EIO;
        }
    }
    const auto *source    = static_cast<const uint8_t *>(input);
    bool        attempted = false;
    for (Size done = 0; done < length;)
    {
        uint64_t   logical = offset + done, vcn = logical / cluster_;
        const Run *run = nullptr;
        for (unsigned i = 0; i < target.count; ++i)
            if (vcn >= target.runs[i].vcn && vcn - target.runs[i].vcn < target.runs[i].length)
            {
                run = target.runs + i;
                break;
            }
        if (!run)
        {
            if (attempted) io_failed_ = true;
            return -EIO;
        }
        uint64_t physical = uint64_t(run->lcn) * cluster_ + logical - run->vcn * cluster_;
        uint64_t sector   = physical / sector_ * sector_;
        Size     within = Size(physical - sector), amount = sector_ - within;
        if (amount > length - done) amount = length - done;
        if (within || amount != sector_)
        {
            error = fetch(sector, work.root_index, sector_);
            if (error)
            {
                if (attempted) io_failed_ = true;
                return error;
            }
            copy(work.root_index + within, source + done, amount);
        }
        else
            copy(work.root_index, source + done, amount);
        attempted = true;
        if (!writer_(context_, sector, work.root_index, sector_))
        {
            io_failed_ = true;
            return -EIO;
        }
        done += amount;
    }
    error = sync();
    if (!error) completed = length; // Success is reported only after durability.
    return error;
}

int Volume::enumerate_directory(const Inode &inode, DirectoryVisitor visitor, void *context, Workspace &work) const
{
    if (!ready_) return -ENODEV;
    if (!visitor || !inode.directory) return -ENOTDIR;
    if (inode.unsupported) return -EOPNOTSUPP;
    if (!(inode.reference >> 48)) return -EIO;
    int   error = record_read(inode.reference & NumberMask, uint16_t(inode.reference >> 48), work.record);
    Inode current;
    if (!error) error = inode_from_record(work.record, inode.reference & NumberMask, current, work);
    if (!error && !current.directory) error = -ENOTDIR;
    if (!error && current.unsupported) error = -EOPNOTSUPP;
    if (!error) error = stream_from_record(work.record, IndexRoot, true, work.stream, work, inode.reference);
    if (error) return error;
    if (!work.stream.resident || work.stream.size < 32 || le32(work.stream.value) != 0x30 ||
        le32(work.stream.value + 4) != 1 || le32(work.stream.value + 8) != index_ ||
        work.stream.value[12] != index_ / cluster_)
        return -EOPNOTSUPP;
    unsigned root_size = unsigned(work.stream.size);
    copy(work.root_index, work.stream.value, root_size);
    error           = stream_from_record(work.record, IndexAllocation, true, work.allocation, work, inode.reference);
    bool allocation = error == 0;
    if (error && error != -ENOENT) return error;
    if (allocation)
    {
        error = stream_from_record(work.record, Bitmap, true, work.bitmap, work, inode.reference);
        if (error) return error;
        if (work.allocation.resident || work.allocation.size % index_ ||
            work.allocation.size != work.allocation.initialized || work.bitmap.initialized != work.bitmap.size ||
            (work.allocation.size / index_ + 7) / 8 > work.bitmap.size)
            return -EIO;
        for (unsigned i = 0; i < work.allocation.count; ++i)
            if (work.allocation.runs[i].lcn < 0) return -EIO;
    }
    unsigned depth = 0, seen = 0, entries = 0;
    work.path[0] = {__UINT64_MAX__, 0, false};
    for (;;)
    {
        Workspace::Frame &frame  = work.path[depth];
        const uint8_t    *buffer = work.root_index;
        unsigned          bytes = root_size, header = 16;
        if (depth)
        {
            if (!allocation || frame.vcn > __UINT64_MAX__ / cluster_) return -EIO;
            uint64_t offset = frame.vcn * cluster_;
            if (offset % index_ || offset > work.allocation.size || index_ > work.allocation.size - offset) return -EIO;
            uint8_t  bit;
            uint64_t number = offset / index_;
            error           = stream_read(work.bitmap, number / 8, &bit, 1);
            if (error || !(bit & (1U << (number % 8)))) return error ? error : -EIO;
            error = stream_read(work.allocation, offset, work.index, index_);
            if (!error) error = unpack(work.index, index_, 0x58444e49); // INDX
            if (error) return error;
            if (le64(work.index + 16) != frame.vcn) return -EIO;
            buffer = work.index;
            bytes  = index_;
            header = 24;
        }
        unsigned first, end;
        error = index_layout(buffer, bytes, header, first, end);
        if (error) return error;
        if (depth && first < unsigned(le16(buffer + 4)) + unsigned(le16(buffer + 6)) * 2) return -EIO;
        if (!frame.offset) frame.offset = first;
        unsigned position = frame.offset;
        if (position < first || position > end || end - position < 16) return -EIO;
        const uint8_t *entry  = buffer + position;
        unsigned       length = le16(entry + 8), key_length = le16(entry + 10), flags = le16(entry + 12);
        bool           child = flags & 1, last = flags & 2;
        if ((flags & ~3U) || length < 16 + unsigned(child) * 8 || length % 8 || length > end - position ||
            key_length > length - 16 - unsigned(child) * 8 || (last && (key_length || position + length != end)) ||
            (!last && key_length < 66) || (child && !(buffer[header + 12] & 1)))
            return -EIO;
        if (child && !frame.child_done)
        {
            uint64_t vcn = le64(entry + length - 8);
            if (depth + 1 == MaxDepth || seen == MaxIndexNodes) return -EOPNOTSUPP;
            for (unsigned i = 0; i < seen; ++i)
                if (work.visited[i] == vcn) return -EIO;
            work.visited[seen++] = vcn;
            frame.child_done     = true;
            work.path[++depth]   = {vcn, 0, false};
            continue;
        }
        if (last)
        {
            if (!depth) return 0;
            --depth;
            continue;
        }
        if (++entries > MaxEntries) return -EOPNOTSUPP;
        const uint8_t *name  = entry + 16;
        unsigned       units = name[64], name_space = name[65];
        if (!units || 66 + units * 2 > key_length || name_space > 3 || le64(name) != inode.reference ||
            !(le64(entry) >> 48))
            return -EIO;
        DirectoryEntry item = {};
        item.reference      = le64(entry);
        item.size           = le64(name + 48);
        item.attributes     = le32(name + 56);
        item.directory      = (item.attributes & (0x10 | 0x10000000)) != 0;
        item.name_space     = uint8_t(name_space);
        error               = utf8_name(name + 66, units, item.name);
        if (error) return error;
        if (equal(item.name, "."))
        {
            if (item.reference != inode.reference) return -EIO;
        }
        else if (equal(item.name, ".."))
            return -EIO;
        else
        {
            error = visitor(context, item);
            if (error) return error > 0 ? 0 : error;
        }
        frame.offset     += length;
        frame.child_done  = false;
    }
}

int Volume::lookup(const Inode &directory, const char *name, Inode &inode, Workspace &work) const
{
    DirectoryEntry entry;
    int            error = lookup_entry(directory, name, entry, work);
    return error ? error : read_inode(entry.reference, inode, work);
}

} // namespace xjntfs
