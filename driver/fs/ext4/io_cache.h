#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original fixed-storage cache; BSD design references and retained notices are
// recorded in REFERENCES.json and LWEXT4_BSD_NOTICE.txt. No upstream code is ported.

#include "native.h"
#include "performance.h"

namespace xjext4 {

// Clean copies only. The exclusive device owner must invalidate before EVERY
// attempted write, even if the device reports failure after writing some bytes.
// Only a fully successful backend write may publish its complete block copies.
// Transaction/recovery overlays belong above this cache. No allocation, dirty
// writeback, read-ahead, retained caller pointers or internal locking occurs.
struct ReadCache
{
    // A fresh import can validate 64 after-images plus inode/bitmap/directory
    // metadata. Keep those clean before-images resident through the complete
    // guard instead of rereading them after each sweep. All storage is owned
    // by the caller on the heap; invalidation and failure rules are unchanged.
    static constexpr unsigned kEntries = 128;
    static_assert(kEntries < 255, "LRU ranks must fit in uint8_t");
    uint8_t data[kEntries][4096];
    uint64_t blocks[kEntries];
    uint8_t rank[kEntries]; // Zero is invalid; valid ranks are exactly 1..N (MRU..LRU).

    void reset()
    {
        for (unsigned i = 0; i < kEntries; ++i) rank[i] = 0;
    }

    void discard(unsigned slot)
    {
        unsigned old = rank[slot];
        if (!old) return;
        rank[slot] = 0;
        for (unsigned i = 0; i < kEntries; ++i)
            if (rank[i] > old) --rank[i];
    }

    void touch(unsigned slot)
    {
        unsigned old = rank[slot] ? rank[slot] : kEntries + 1;
        for (unsigned i = 0; i < kEntries; ++i)
            if (rank[i] && rank[i] < old) ++rank[i];
        rank[slot] = 1;
    }

    void invalidate(uint64_t offset, Size length, uint32_t size)
    {
        if (!length) return;
        if (!size || length - 1 > ~uint64_t(0) - offset) { reset(); return; }
        uint64_t first = offset / size, last = (offset + length - 1) / size;
        for (unsigned i = 0; i < kEntries; ++i)
            if (rank[i] && blocks[i] >= first && blocks[i] <= last) discard(i);
    }

    // The exclusive caller has already invalidated the range and completed
    // its backend write. Remember those exact bytes without rereading them.
    // Partial/unaligned writes leave the range cold; never retain a caller's
    // pointer or install speculative transaction/recovery after-images here.
    void complete_write(uint64_t offset, const void *input, Size length, uint32_t size)
    {
        if (!input || !length || (size != 1024 && size != 2048 && size != 4096) ||
            offset % size || length % size || length - 1 > ~uint64_t(0) - offset) return;
        const auto *source = static_cast<const uint8_t *>(input);
        for (Size at = 0; at < length; at += size)
        {
            uint64_t block = (offset + at) / size;
            unsigned slot = kEntries;
            for (unsigned i = 0; i < kEntries; ++i)
                if (rank[i] && blocks[i] == block) { slot = i; break; }
            if (slot == kEntries)
            {
                slot = 0;
                for (unsigned i = 0; i < kEntries; ++i)
                {
                    if (!rank[i]) { slot = i; break; }
                    if (rank[i] > rank[slot]) slot = i;
                }
            }
            discard(slot);
            for (Size i = 0; i < size; ++i) data[slot][i] = source[at + i];
            blocks[slot] = block;
            touch(slot);
        }
    }

    // Geometry and reader identity must stay fixed between resets. Larger or
    // crossing requests go directly to the backend without splitting its I/O.
    bool read(Reader reader, void *context, uint64_t limit, uint32_t size,
              uint64_t offset, void *output, Size length, Performance *performance = nullptr)
    {
        if (!reader || (!output && length) || offset > limit || length > limit - offset) return false;
        if (!length) return true;
        auto backend = [&](uint64_t position, void *data, Size bytes) -> bool
        {
            PerformanceScope measured(performance, &Performance::backend_read);
            return reader(context, position, data, bytes);
        };
        if ((size != 1024 && size != 2048 && size != 4096) || length > size - offset % size)
        {
            if (performance) ++performance->cache_bypasses;
            return backend(offset, output, length);
        }
        uint64_t block = offset / size;
        unsigned slot = kEntries;
        for (unsigned i = 0; i < kEntries; ++i)
            if (rank[i] && blocks[i] == block) { slot = i; break; }
        if (slot == kEntries)
        {
            if (performance) ++performance->cache_misses;
            // Do not extend a legal partial read beyond the device's end.
            if (size > limit - block * size) return backend(offset, output, length);
            slot = 0;
            for (unsigned i = 0; i < kEntries; ++i)
            {
                if (!rank[i]) { slot = i; break; }
                if (rank[i] > rank[slot]) slot = i;
            }
            discard(slot);
            if (!backend(block * size, data[slot], size)) return false;
            blocks[slot] = block;
        }
        else if (performance) ++performance->cache_hits;
        touch(slot);
        auto *destination = static_cast<uint8_t *>(output);
        for (Size i = 0; i < length; ++i) destination[i] = data[slot][offset % size + i];
        return true;
    }
};

} // namespace xjext4
