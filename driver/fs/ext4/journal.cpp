// SPDX-License-Identifier: Apache-2.0
// Original implementation from public JBD2 format descriptions. No Linux,
// e2fsprogs, lwext4, or other filesystem implementation code is incorporated.
// Later block-I/O design references are recorded in REFERENCES.json.
#include "journal.h"
#include <errno.h>

namespace xjext4 {
namespace {

constexpr uint32_t kMagic = 0xc03b3998U;
constexpr uint32_t kDescriptor = 1;
constexpr uint32_t kCommit = 2;
constexpr uint32_t kSuperV2 = 4;
constexpr uint32_t kEscape = 1;
constexpr uint32_t kSameUuid = 2;
constexpr uint32_t kLast = 8;
constexpr uint32_t kRevoke = 5;
constexpr uint32_t kSuppressed = 0x80000000U;
constexpr uint32_t kEmptyIndex = 0xffffffffU;
constexpr uint32_t kMaximumJournalBlocks = JournalWorkspace::kMaximumBlocks;

uint16_t big16(const uint8_t *bytes) { return uint16_t(bytes[0]) << 8 | bytes[1]; }

void put16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = uint8_t(value >> 8);
    bytes[1] = uint8_t(value);
}

uint32_t crc32c(uint32_t value, const uint8_t *bytes, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i)
    {
        value ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0x82f63b78U : 0);
    }
    return value;
}

uint32_t bucket(uint64_t block)
{
    return uint32_t((block ^ (block >> 32)) * 0x9e3779b1U);
}

bool at_least(uint32_t sequence, uint32_t previous)
{
    return int32_t(sequence - previous) >= 0;
}

uint32_t big32(const uint8_t *bytes)
{
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

void put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index) bytes[index] = uint8_t(value >> (24 - index * 8));
}

void zero(void *output, uint32_t length)
{
    auto *bytes = static_cast<uint8_t *>(output);
    for (uint32_t index = 0; index < length; ++index) bytes[index] = 0;
}

void copy(void *output, const void *input, uint32_t length)
{
    auto *destination = static_cast<uint8_t *>(output);
    const auto *source = static_cast<const uint8_t *>(input);
    for (uint32_t index = 0; index < length; ++index) destination[index] = source[index];
}

bool equal(const uint8_t *left, const uint8_t *right, uint32_t length)
{
    for (uint32_t index = 0; index < length; ++index) if (left[index] != right[index]) return false;
    return true;
}

bool all_zero(const uint8_t *bytes, uint32_t length)
{
    for (uint32_t index = 0; index < length; ++index) if (bytes[index]) return false;
    return true;
}

void header(uint8_t *bytes, uint32_t type, uint32_t sequence)
{
    put32(bytes, kMagic);
    put32(bytes + 4, type);
    put32(bytes + 8, sequence);
}

struct CommitImages
{
    const BlockUpdate *updates;
    unsigned count;
    uint32_t block_size;
};

bool read_commit_image(void *context, unsigned index, void *output)
{
    const auto &images = *static_cast<const CommitImages *>(context);
    if (index >= images.count || !output) return false;
    copy(output, images.updates[index].bytes, images.block_size);
    return true;
}

} // namespace

int Journal::fetch(uint64_t block, void *output)
{
    if (failed_) return -EIO;
    if (block >= device_.filesystem_blocks || !device_.read(device_.context, block, output, device_.block_size))
    {
        failed_ = true;
        return -EIO;
    }
    return 0;
}

int Journal::store(uint64_t block, const void *input)
{
    return store_run(block, input, 1);
}

unsigned Journal::transfer_limit() const
{
    if (!device_.workspace || device_.maximum_transfer_blocks < 2) return 1;
    return device_.maximum_transfer_blocks < kMaximumUpdates ? device_.maximum_transfer_blocks : kMaximumUpdates;
}

int Journal::store_run(uint64_t block, const void *input, unsigned count)
{
    if (failed_) return -EIO;
    if (!count || count > transfer_limit() || block >= device_.filesystem_blocks ||
        count > device_.filesystem_blocks - block ||
        !device_.write(device_.context, block, input, count * device_.block_size))
    {
        failed_ = true;
        return -EIO;
    }
    return 0;
}

int Journal::write_updates(const BlockUpdate *updates, unsigned count, bool log, uint8_t *single)
{
    const unsigned limit = transfer_limit();
    uint8_t *buffer = limit > 1 ? device_.workspace->transfer : single;
    for (unsigned first = 0; first < count;)
    {
        auto target = [&](unsigned index) -> uint64_t
        {
            return log ? mapping_[advance(first_, index + 1)] : updates[index].block;
        };
        uint64_t physical = target(first);
        unsigned run = 1;
        // Logical journal adjacency is insufficient: mappings may be fragmented
        // or wrap around the ring. Never combine across a physical discontinuity.
        while (run < limit && run < count - first && target(first + run) == physical + run) ++run;
        for (unsigned i = 0; i < run; ++i)
        {
            uint8_t *image = buffer + i * device_.block_size;
            copy(image, updates[first + i].bytes, device_.block_size);
            if (log && big32(image) == kMagic) zero(image, 4);
        }
        int error = store_run(physical, buffer, run);
        if (error) return error;
        first += run;
    }
    return 0;
}

int Journal::barrier()
{
    if (failed_) return -EIO;
    if (!device_.flush(device_.context))
    {
        failed_ = true;
        return -EIO;
    }
    return 0;
}

bool Journal::valid_target(uint64_t block) const
{
    if (block >= device_.filesystem_blocks) return false;
    unsigned slot = bucket(block) & mapping_index_mask_;
    while (mapping_index_[slot] != 0xffffU)
    {
        if (mapping_[mapping_index_[slot]] == block) return false;
        slot = (slot + 1) & mapping_index_mask_;
    }
    return true;
}

bool Journal::checksum_block(const uint8_t *block, uint32_t offset) const
{
    uint32_t sum = crc32c(checksum_seed_, block, offset);
    const uint8_t cleared[4] = {};
    sum = crc32c(sum, cleared, 4);
    sum = crc32c(sum, block + offset + 4, device_.block_size - offset - 4);
    return sum == big32(block + offset);
}

void Journal::seal_block(uint8_t *block, uint32_t offset) const
{
    zero(block + offset, 4);
    put32(block + offset, crc32c(checksum_seed_, block, device_.block_size));
}

void Journal::seal_super()
{
    if (!checksum_version_) return;
    zero(super_ + 0xfc, 4);
    put32(super_ + 0xfc, crc32c(0xffffffffU, super_, 1024));
}

uint32_t Journal::advance(uint32_t block, unsigned count) const
{
    uint32_t capacity = maximum_ - first_;
    return first_ + uint32_t((uint64_t(block - first_) + count) % capacity);
}

int Journal::read_image(const JournalReplayBlock &image, void *output)
{
    int error = fetch(mapping_[image.logical], output);
    if (error) return error;
    auto *bytes = static_cast<uint8_t *>(output);
    if ((image.flags & kEscape) ? big32(bytes) != 0 : big32(bytes) == kMagic) return -EUCLEAN;
    if (checksum_version_)
    {
        uint8_t sequence[4];
        put32(sequence, image.sequence);
        uint32_t checksum = crc32c(crc32c(checksum_seed_, sequence, 4), bytes, device_.block_size);
        if (checksum_version_ == 2) checksum &= 0xffffU;
        if (checksum != image.checksum) return -EUCLEAN;
    }
    if (image.flags & kEscape) put32(bytes, kMagic);
    return 0;
}

bool Journal::read_recovery_image(void *context, unsigned index, void *output)
{
    auto &images = *static_cast<RecoveryImages *>(context);
    return index < images.count && output && !images.journal->read_image(images.images[index], output);
}

bool Journal::find_recovery_image(void *context, uint64_t block, unsigned &index)
{
    const auto &images = *static_cast<RecoveryImages *>(context);
    unsigned slot = bucket(block) & (images.index_slots - 1);
    while (images.index[slot] != kEmptyIndex)
    {
        index = images.index[slot];
        if (index < images.count && images.images[index].block == block) return true;
        slot = (slot + 1) & (images.index_slots - 1);
    }
    return false;
}

int Journal::open(const JournalDevice &device)
{
    ready_ = failed_ = false;
    start_ = sequence_ = first_ = maximum_ = 0;
    checksum_version_ = 0;
    wide_ = false;
    if (!device.read || !device.write || !device.flush || !device.map ||
        (device.block_size != 1024 && device.block_size != 2048 && device.block_size != 4096) ||
        !device.filesystem_blocks || device.journal_bytes % device.block_size ||
        device.mapped_blocks > device.journal_bytes / device.block_size ||
        device.mapped_blocks < kMaximumUpdates + 3)
        return -EINVAL;
    if (device.mapped_blocks > kMaximumJournalBlocks || (!device.workspace && device.mapped_blocks > 4096))
        return -EOPNOTSUPP;
    device_ = device;
    mapping_ = device.workspace ? device.workspace->mapping : mapping_small_;
    mapping_index_ = device.workspace ? device.workspace->mapping_index : mapping_index_small_;
    mapping_index_mask_ = device.workspace ? JournalWorkspace::kIndexSlots - 1 : 8191;
    for (unsigned index = 0; index <= mapping_index_mask_; ++index) mapping_index_[index] = 0xffffU;
    // Pin an injective mapping for this mount. The caller cannot change the
    // supplied storage while the journal remains open.
    for (uint32_t logical = 0; logical < device_.mapped_blocks; ++logical)
    {
        uint64_t physical;
        if (!device_.map(device_.context, logical, physical) || !physical ||
            physical >= device_.filesystem_blocks) return -EUCLEAN;
        unsigned slot = bucket(physical) & mapping_index_mask_;
        while (mapping_index_[slot] != 0xffffU)
        {
            if (mapping_[mapping_index_[slot]] == physical) return -EUCLEAN;
            slot = (slot + 1) & mapping_index_mask_;
        }
        mapping_[logical] = physical;
        mapping_index_[slot] = uint16_t(logical);
    }
    int result = fetch(mapping_[0], super_);
    if (result) return result;
    if (big32(super_) != kMagic || big32(super_ + 4) != kSuperV2 ||
        big32(super_ + 0xc) != device_.block_size || big32(super_ + 0x10) != device_.mapped_blocks ||
        !equal(super_ + 0x30, device_.uuid, 16) || big32(super_ + 0x40) != 1) return -EUCLEAN;
    uint32_t incompatible = big32(super_ + 0x28);
    if (big32(super_ + 0x24) || (incompatible & ~0x1bU) || big32(super_ + 0x2c) || big32(super_ + 0x44))
        return -EOPNOTSUPP;
    if ((incompatible & 0x18U) == 0x18U) return -EUCLEAN;
    checksum_version_ = (incompatible & 0x10U) ? 3 : (incompatible & 8U) ? 2 : 0;
    if (checksum_version_)
    {
        if (super_[0x50] != 4) return -EOPNOTSUPP;
        uint32_t sum = crc32c(0xffffffffU, super_, 0xfc);
        const uint8_t cleared[4] = {};
        sum = crc32c(sum, cleared, 4);
        sum = crc32c(sum, super_ + 0x100, 1024 - 0x100);
        if (sum != big32(super_ + 0xfc)) return -EUCLEAN;
    }
    if (big32(super_ + 0x20)) return -EUCLEAN;
    maximum_ = big32(super_ + 0x10);
    first_ = big32(super_ + 0x14);
    start_ = big32(super_ + 0x1c);
    sequence_ = big32(super_ + 0x18);
    if (!first_ || first_ >= maximum_ || maximum_ - first_ < kMaximumUpdates + 2 ||
        (start_ && (start_ < first_ || start_ >= maximum_))) return -EUCLEAN;
    wide_ = (incompatible & 2) != 0;
    checksum_seed_ = crc32c(0xffffffffU, device_.uuid, 16);
    ready_ = true;
    return 0;
}

int Journal::clear()
{
    put32(super_ + 0x1c, 0);
    put32(super_ + 0x18, sequence_ + 1U);
    seal_super();
    int result = store(mapping_[0], super_);
    if (!result) result = barrier();
    if (!result)
    {
        start_ = 0;
        ++sequence_;
    }
    return result;
}

int Journal::commit(const BlockUpdate *updates, unsigned count)
{
    if (!ready_) return -EINVAL;
    if (failed_) return -EIO;
    if (start_) return -EBUSY;
    if (!updates || !count || count > kMaximumUpdates) return -EINVAL;
    for (unsigned index = 0; index < count; ++index)
    {
        if (!updates[index].bytes || !valid_target(updates[index].block) ||
            (!wide_ && updates[index].block > 0xffffffffULL)) return -EINVAL;
        for (unsigned previous = 0; previous < index; ++previous)
            if (updates[previous].block == updates[index].block) return -EINVAL;
    }
    for (unsigned index = 0; index < count; ++index)
        if (device_.validate && !device_.validate(device_.context, updates[index].block, updates[index].bytes))
            return -EOPNOTSUPP;
    if (device_.validate_transaction)
    {
        uint64_t targets[kMaximumUpdates];
        for (unsigned index = 0; index < count; ++index) targets[index] = updates[index].block;
        CommitImages images = {updates, count, device_.block_size};
        JournalTransaction transaction = {targets, count, device_.block_size, &images, read_commit_image, nullptr};
        if (!device_.validate_transaction(device_.context, transaction)) return failed_ ? -EIO : -EOPNOTSUPP;
    }
    uint8_t block[4096];
    // Persist the log anchor before any transaction records. The filesystem
    // caller must already have persisted its RECOVER bit before calling us.
    start_ = first_;
    put32(super_ + 0x1c, start_);
    seal_super();
    int result = store(mapping_[0], super_);
    if (!result) result = barrier();
    // Invalidate the future commit slot before publishing a new descriptor.
    // Otherwise a shorter transaction could mistake old data or an old commit
    // for this transaction's completion after a crash.
    if (!result)
    {
        zero(block, device_.block_size);
        result = store(mapping_[advance(first_, count + 1)], block);
    }
    if (!result) result = barrier();
    zero(block, device_.block_size);
    header(block, kDescriptor, sequence_);
    uint32_t position = 12;
    uint8_t encoded_sequence[4];
    put32(encoded_sequence, sequence_);
    uint32_t data_seed = crc32c(checksum_seed_, encoded_sequence, 4);
    for (unsigned index = 0; index < count; ++index)
    {
        const auto *bytes = static_cast<const uint8_t *>(updates[index].bytes);
        uint32_t flags = index ? kSameUuid : 0;
        if (index + 1 == count) flags |= kLast;
        if (big32(bytes) == kMagic) flags |= kEscape;
        uint32_t checksum = 0;
        if (checksum_version_)
        {
            const uint8_t cleared[4] = {};
            checksum = flags & kEscape ?
                crc32c(crc32c(data_seed, cleared, 4), bytes + 4, device_.block_size - 4) :
                crc32c(data_seed, bytes, device_.block_size);
        }
        put32(block + position, uint32_t(updates[index].block));
        if (checksum_version_ == 2)
        {
            put16(block + position + 4, uint16_t(checksum));
            put16(block + position + 6, uint16_t(flags));
        }
        else put32(block + position + 4, flags);
        position += 8;
        if (wide_ || checksum_version_ == 3)
        {
            put32(block + position, uint32_t(updates[index].block >> 32));
            position += 4;
        }
        if (checksum_version_ == 3)
        {
            put32(block + position, checksum);
            position += 4;
        }
        // The Linux v2 wire format reserves two extra tag bytes, including
        // when the optional high block word is absent. V3 has a fixed tag.
        if (checksum_version_ == 2) position += 2;
        if (!(flags & kSameUuid))
        {
            copy(block + position, device_.uuid, 16);
            position += 16;
        }
    }
    if (checksum_version_) seal_block(block, device_.block_size - 4);
    if (!result) result = store(mapping_[first_], block);
    if (!result) result = write_updates(updates, count, true, block);
    if (!result) result = barrier();
    if (!result)
    {
        zero(block, device_.block_size);
        header(block, kCommit, sequence_);
        if (checksum_version_) seal_block(block, 16);
        result = store(mapping_[advance(first_, count + 1)], block);
    }
    if (!result) result = barrier();
    if (!result) result = write_updates(updates, count, false, block);
    if (!result) result = barrier();
    if (!result) result = clear();
    return result;
}

// Two scans share one parser: first collect only committed images; second
// apply revocations to the latest image of each target. Neither pass writes.
int Journal::scan(JournalReplayBlock *images, unsigned capacity, unsigned &count,
                  uint32_t &end, uint32_t &next_sequence, bool revokes,
                  uint32_t *index, unsigned index_slots)
{
    const uint32_t stop = end;
    uint32_t cursor = start_, expected = sequence_, consumed = 0;
    const uint32_t ring = maximum_ - first_;
    unsigned committed_count = 0, transaction_begin = 0;
    bool transaction_started = false;
    uint8_t block[4096];
    if (!revokes)
    {
        count = 0;
        end = start_;
        next_sequence = sequence_;
        for (unsigned i = 0; i < index_slots; ++i) index[i] = kEmptyIndex;
    }
    else if (cursor == stop) return 0;
    for (;;)
    {
        if (consumed >= ring) return -EUCLEAN;
        int error = fetch(mapping_[cursor], block);
        if (error) return error;
        if (big32(block) != kMagic || big32(block + 8) != expected)
        {
            if (revokes) return -EUCLEAN;
            bool blank = all_zero(block, device_.block_size);
            bool older = big32(block) == kMagic && !at_least(big32(block + 8), expected);
            // Stale bytes after the last committed transaction are a normal
            // ring boundary. Unknown current/future records remain untouched.
            if (!blank && !older && (transaction_started || end == start_)) return -EUCLEAN;
            if (big32(block) == kMagic && at_least(big32(block + 8), expected)) return -EUCLEAN;
            count = committed_count;
            next_sequence = expected + (transaction_started ? 1U : 0U);
            return 0;
        }
        uint32_t type = big32(block + 4);
        if (type == kDescriptor)
        {
            if (checksum_version_ && !checksum_block(block, device_.block_size - 4)) return -EUCLEAN;
            transaction_started = true;
            uint32_t limit = device_.block_size - (checksum_version_ ? 4 : 0);
            uint32_t position = 12, tags = 0;
            bool last = false;
            while (!last)
            {
                uint32_t bytes = checksum_version_ == 3 ? 16 : (wide_ ? 12 : 8) + (checksum_version_ == 2 ? 2 : 0);
                if (position > limit) return -EUCLEAN;
                // A completely filled descriptor may omit LAST_TAG: the next
                // log record continues the same transaction with more tags.
                if (bytes > limit - position)
                {
                    if (!tags) return -EUCLEAN;
                    break;
                }
                uint64_t target = big32(block + position);
                uint32_t flags = checksum_version_ == 2 ? big16(block + position + 6) : big32(block + position + 4);
                // Only the low sixteen flag bits are defined. Linux-generated
                // fixtures may retain unspecified upper bits in this word;
                // they carry no tag semantics. Unknown low flags remain
                // unsupported, and v2/v3 descriptor checksums cover all bytes.
                flags &= 0xffffU;
                uint32_t checksum = checksum_version_ == 2 ? big16(block + position + 4) : 0;
                position += 8;
                if (wide_ || checksum_version_ == 3)
                {
                    uint32_t high = big32(block + position);
                    if (high && !wide_) return -EUCLEAN;
                    target |= uint64_t(high) << 32;
                    position += 4;
                }
                if (checksum_version_ == 3)
                {
                    checksum = big32(block + position);
                    position += 4;
                }
                if (checksum_version_ == 2) position += 2;
                if (flags & ~(kEscape | kSameUuid | kLast)) return -EOPNOTSUPP;
                if (!(flags & kSameUuid))
                {
                    if (position > limit || limit - position < 16 ||
                        (!equal(block + position, device_.uuid, 16) && !all_zero(block + position, 16)))
                        return -EUCLEAN;
                    position += 16;
                }
                if (!valid_target(target)) return -EUCLEAN;
                if (consumed + tags + 2 >= ring) return -EUCLEAN;
                if (!revokes)
                {
                    if (count == capacity) return -E2BIG;
                    unsigned slot = bucket(target) & (index_slots - 1);
                    while (index[slot] != kEmptyIndex)
                    {
                        const JournalReplayBlock &prior = images[index[slot]];
                        if (prior.block == target)
                        {
                            if (prior.sequence == expected) return -EUCLEAN;
                            break;
                        }
                        slot = (slot + 1) & (index_slots - 1);
                    }
                    images[count] = {target, advance(cursor, tags + 1), flags, expected, checksum};
                    index[slot] = count++;
                }
                ++tags;
                last = (flags & kLast) != 0;
            }
            cursor = advance(cursor, tags + 1);
            consumed += tags + 1;
            continue;
        }
        if (type == kRevoke)
        {
            if (!(big32(super_ + 0x28) & 1U)) return -EUCLEAN;
            if (checksum_version_ && !checksum_block(block, device_.block_size - 4)) return -EUCLEAN;
            uint32_t bytes = big32(block + 12);
            uint32_t width = wide_ ? 8 : 4;
            uint32_t limit = device_.block_size - (checksum_version_ ? 4 : 0);
            if (bytes < 16 || bytes > limit || (bytes - 16) % width) return -EUCLEAN;
            for (uint32_t position = 16; position < bytes; position += width)
            {
                uint64_t target = wide_ ? (uint64_t(big32(block + position)) << 32) |
                                          big32(block + position + 4) : big32(block + position);
                if (!valid_target(target)) return -EUCLEAN;
                if (revokes)
                {
                    unsigned slot = bucket(target) & (index_slots - 1);
                    while (index[slot] != kEmptyIndex)
                    {
                        JournalReplayBlock &image = images[index[slot]];
                        if (image.block == target)
                        {
                            if (at_least(expected, image.sequence)) image.flags |= kSuppressed;
                            break;
                        }
                        slot = (slot + 1) & (index_slots - 1);
                    }
                }
            }
            transaction_started = true;
            cursor = advance(cursor, 1);
            ++consumed;
            continue;
        }
        if (type != kCommit) return -EOPNOTSUPP;
        if (!transaction_started) return -EUCLEAN;
        if (checksum_version_ && !checksum_block(block, 16))
        {
            if (revokes) return -EUCLEAN;
            // A torn commit never authorizes checkpointing. A following TID,
            // however, proves this is not simply an unfinished log tail.
            error = fetch(mapping_[advance(cursor, 1)], block);
            if (error) return error;
            if (big32(block) == kMagic && big32(block + 8) == expected + 1U) return -EUCLEAN;
            count = committed_count;
            next_sequence = expected + 1U;
            return 0;
        }
        if (!checksum_version_ && !all_zero(block + 12, 36)) return -EOPNOTSUPP;
        if (!revokes)
        {
            for (unsigned i = transaction_begin; i < count; ++i)
            {
                error = read_image(images[i], block);
                if (error) return error;
            }
            committed_count = transaction_begin = count;
        }
        ++expected;
        cursor = advance(cursor, 1);
        ++consumed;
        transaction_started = false;
        if (revokes)
        {
            if (cursor == stop) return 0;
        }
        else
        {
            end = cursor;
            next_sequence = expected;
        }
    }
}

int Journal::validate_empty_recovery()
{
    if (!device_.validate_transaction) return -EINVAL;
    JournalTransaction empty = {nullptr, 0, device_.block_size, nullptr, nullptr, nullptr};
    return device_.validate_transaction(device_.context, empty) ? 0 : -EOPNOTSUPP;
}

int Journal::recover(bool &replayed, bool validate_empty)
{
    replayed = false;
    if (!ready_) return -EINVAL;
    if (failed_) return -EIO;
    if (!start_) return validate_empty ? validate_empty_recovery() : 0;
    uint8_t block[4096];
    int result = fetch(mapping_[start_], block);
    if (result) return result;
    if (all_zero(block, device_.block_size) ||
        (big32(block) == kMagic && big32(block + 4) == kDescriptor && big32(block + 8) == sequence_ - 1U))
    {
        if (validate_empty)
        {
            result = validate_empty_recovery();
            if (result) return result;
        }
        return clear();
    }
    if (big32(block) != kMagic) return -EUCLEAN;
    if (big32(block + 8) != sequence_) return -EUCLEAN;
    // A heap-owned workspace scales to the entire journal. The small fallback
    // retains the old standalone API without adding heap allocation or a large
    // recovery frame to callers that only use bounded single transactions.
    JournalReplayBlock small_images[kMaximumUpdates];
    uint64_t small_targets[kMaximumUpdates];
    uint32_t small_index[64];
    JournalReplayBlock *images = device_.workspace ? device_.workspace->images : small_images;
    uint64_t *targets = device_.workspace ? device_.workspace->targets : small_targets;
    uint32_t *index = device_.workspace ? device_.workspace->index : small_index;
    unsigned capacity = device_.workspace ? JournalWorkspace::kMaximumBlocks : kMaximumUpdates;
    unsigned slots = device_.workspace ? JournalWorkspace::kIndexSlots : 64;
    unsigned count = 0;
    uint32_t end = start_, next_sequence = sequence_;
    result = scan(images, capacity, count, end, next_sequence, false, index, slots);
    if (result) return result;
    bool committed = end != start_;
    // Collapse overwritten blocks before considering revokes. The index is
    // independent of physical journal mapping, which remains pinned and hashed.
    for (unsigned i = 0; i < slots; ++i) index[i] = kEmptyIndex;
    unsigned unique = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        JournalReplayBlock candidate = images[i];
        unsigned slot = bucket(candidate.block) & (slots - 1);
        while (index[slot] != kEmptyIndex && images[index[slot]].block != candidate.block)
            slot = (slot + 1) & (slots - 1);
        if (index[slot] == kEmptyIndex) index[slot] = unique++;
        images[index[slot]] = candidate;
    }
    count = unique;
    if (committed)
    {
        result = scan(images, capacity, count, end, next_sequence, true, index, slots);
        if (result) return result;
    }
    unsigned kept = 0;
    for (unsigned i = 0; i < count; ++i)
        if (!(images[i].flags & kSuppressed)) images[kept++] = images[i];
    count = kept;
    for (unsigned i = 0; i < slots; ++i) index[i] = kEmptyIndex;
    for (unsigned i = 0; i < count; ++i)
    {
        targets[i] = images[i].block;
        unsigned slot = bucket(targets[i]) & (slots - 1);
        while (index[slot] != kEmptyIndex) slot = (slot + 1) & (slots - 1);
        index[slot] = i;
        result = read_image(images[i], block);
        if (result) return result;
        if (device_.validate && !device_.validate(device_.context, targets[i], block)) return -EOPNOTSUPP;
    }
    if (count && device_.validate_transaction)
    {
        RecoveryImages images_view = {this, images, index, slots, count};
        JournalTransaction transaction = {targets, count, device_.block_size, &images_view,
                                          read_recovery_image, find_recovery_image};
        if (!device_.validate_transaction(device_.context, transaction)) return failed_ ? -EIO : -EOPNOTSUPP;
    }
    if (!count && validate_empty)
    {
        result = validate_empty_recovery();
        if (result) return result;
    }
    const unsigned limit = transfer_limit();
    uint8_t *transfer = limit > 1 ? device_.workspace->transfer : block;
    for (unsigned first = 0; first < count;)
    {
        unsigned run = 1;
        while (run < limit && run < count - first && targets[first + run] == targets[first] + run) ++run;
        for (unsigned i = 0; i < run; ++i)
        {
            result = read_image(images[first + i], transfer + i * device_.block_size);
            if (result) return result;
        }
        result = store_run(targets[first], transfer, run);
        if (result) return result;
        first += run;
    }
    result = barrier();
    if (!result)
    {
        // Do not reuse a TID that was observed in an incomplete tail, even when
        // no home block from that transaction could legally have been written.
        sequence_ = next_sequence - 1U;
        result = clear();
    }
    if (!result) replayed = committed;
    return result;
}

} // namespace xjext4
