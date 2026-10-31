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

} // namespace

int WriteSession::set_attributes(uint32_t number, const AttributeUpdate &update, uint64_t seconds)
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
    int error = tx_begin();
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
        if (update.mask & kAttributeMode) mode = (mode & 0170000) | (update.mode & 07777);
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
    if (!error) return tx_commit();
    tx_cancel();
    return stop(error);
}

} // namespace xjext4
