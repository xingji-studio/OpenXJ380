// SPDX-License-Identifier: Apache-2.0
// Original inode attribute transactions; no filesystem implementation imported.
#include "write.h"
#include "attributes.h"
#include <errno.h>

namespace xjext4 {
namespace {

uint16_t read16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t read32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void store16(uint8_t *p, uint16_t value) { p[0] = uint8_t(value); p[1] = uint8_t(value >> 8); }
void store32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
}

int check_attributes(const AttributeUpdate &update, uint64_t seconds)
{
    if (update.mask & ~31U) return -EINVAL;
    if (!update.mask) return 0;
    if (seconds > 0x37fffffffULL ||
        ((update.mask & kAttributeAtime) && (update.atime_seconds < -0x80000000LL ||
                                           update.atime_seconds > 0x37fffffffLL)) ||
        ((update.mask & kAttributeMtime) && (update.mtime_seconds < -0x80000000LL ||
                                           update.mtime_seconds > 0x37fffffffLL))) return -EOVERFLOW;
    if (((update.mask & kAttributeAtime) && update.atime_nanoseconds >= 1000000000U) ||
        ((update.mask & kAttributeMtime) && update.mtime_nanoseconds >= 1000000000U)) return -EINVAL;
    return 0;
}

} // namespace

int WriteSession::set_attributes(uint32_t number, const AttributeUpdate &update, uint64_t seconds)
{
    AttributeChange change = {number, update};
    return set_attributes_batch(&change, 1, seconds);
}

int WriteSession::set_attributes_batch(const AttributeChange *changes, unsigned count, uint64_t seconds)
{
    PerformanceScope measured(active_ ? device_.performance : nullptr, &Performance::attributes);
    if (!count) return 0;
    if (!changes) return -EINVAL;
    if (count > Journal::kExtendedUpdates) return -E2BIG;
    unsigned changed = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        int error = check_attributes(changes[i].update, seconds);
        if (error) return error;
        if (changes[i].update.mask) ++changed;
    }
    if (active_ && device_.performance) device_.performance->attribute_requests += count;
    if (!changed) return 0;
    if (!active_) return -EINVAL;
    if (count > tx_capacity_) return -E2BIG;
    int error = tx_begin();
    if (error) return error;
    for (unsigned i = 0; !error && i < count; ++i)
    {
        if (changes[i].update.mask)
            error = tx_set_attributes(changes[i].inode, changes[i].update, seconds);
    }
    if (error)
    {
        tx_cancel();
        return stop(error);
    }
    error = tx_commit();
    if (!error && device_.performance) device_.performance->attributes_committed += changed;
    return error;
}

int WriteSession::tx_set_attributes(uint32_t number, const AttributeUpdate &update, uint64_t seconds)
{
    int error = 0;
    uint8_t *raw = nullptr;
    if (!error) error = tx_inode(number, raw);
    if (!error && (read32(raw + 0x20) & 0x30U)) error = -EPERM;
    bool extra = !error && volume_.inode_size_ >= 160 && read16(raw + 0x80) >= 16;
    if (!error && !extra && (((update.mask & kAttributeAtime) && update.atime_nanoseconds) ||
                            ((update.mask & kAttributeMtime) && update.mtime_nanoseconds))) error = -EOPNOTSUPP;
    if (!error && !extra && (((update.mask & kAttributeAtime) && update.atime_seconds > 0x7fffffffLL) ||
                            ((update.mask & kAttributeMtime) && update.mtime_seconds > 0x7fffffffLL))) error = -EOVERFLOW;
    if (!error)
    {
        uint16_t mode = read16(raw);
        if (update.mask & kAttributeUid)
        {
            store16(raw + 2, uint16_t(update.uid));
            store16(raw + 0x78, uint16_t(update.uid >> 16));
            mode &= ~06000;
        }
        if (update.mask & kAttributeGid)
        {
            store16(raw + 0x18, uint16_t(update.gid));
            store16(raw + 0x7a, uint16_t(update.gid >> 16));
            mode &= ~06000;
        }
        // An ownership-only update clears privilege bits. A trusted combined
        // update explicitly supplies the final mode, after changing ownership.
        if (update.mask & kAttributeMode) mode = (mode & 0170000) | (update.mode & 07777);
        store16(raw, mode);
        if (update.mask & kAttributeAtime)
        {
            store32(raw + 8, uint32_t(update.atime_seconds));
            uint32_t epoch = uint32_t((update.atime_seconds - int64_t(int32_t(update.atime_seconds))) >> 32);
            if (extra) store32(raw + 0x8c, (update.atime_nanoseconds << 2) | epoch);
        }
        if (update.mask & kAttributeMtime)
        {
            store32(raw + 0x10, uint32_t(update.mtime_seconds));
            uint32_t epoch = uint32_t((update.mtime_seconds - int64_t(int32_t(update.mtime_seconds))) >> 32);
            if (extra) store32(raw + 0x88, (update.mtime_nanoseconds << 2) | epoch);
        }
        error = tx_finish_inode(number, raw, seconds, false);
    }
    return error;
}

} // namespace xjext4
