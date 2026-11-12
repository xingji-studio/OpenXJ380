// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <audio_backend.h>
#include <proto.hpp>

// DMA storage is retained across sessions. Never free it while a controller
// could still reference it. The permanent audio worker owns PCI polling;
// SB16's IRQ shares metadata through the IRQ-saving lock.
struct PcmRing
{
    static constexpr unsigned bytes    = AUDIO_OUTPUT_FRAMES * 4;
    static constexpr unsigned total    = bytes * AUDIO_OUTPUT_PERIODS;
    spin_t                    lock     = SPIN_INIT;
    uint8_t                  *data     = nullptr;
    uint64_t                  physical = 0, sequence = 0, last_poll = 0, last_move = 0;
    unsigned                  position = 0, rate = 48000;
    bool                      running = false, failed = false, ready[AUDIO_OUTPUT_PERIODS] = {};

    bool allocate()
    {
        if (!data)
        {
            physical = alloc_frames_dma32(total / PAGE_SIZE);
            if (!physical) return false;
            data = (uint8_t *)phys_to_virt(physical);
        }
        memset(data, 0, total);
        return true;
    }

    void reset(unsigned sample_rate)
    {
        rate     = sample_rate;
        sequence = position = 0;
        last_poll = last_move = nanoTime();
        failed                = false;
        for (bool &r : ready)
            r = true;
        running = true;
    }

    // Called with lock held. Time bounds distinguish a stopped host backend
    // from a worker gap that could conceal a complete DMA-ring revolution.
    bool observe(unsigned offset)
    {
        uint64_t now     = nanoTime();
        uint64_t ring_ns = (uint64_t)total * 1000000000ULL / (rate * 4U);
        if (!running || offset >= total || now - last_poll >= ring_ns) return false;
        last_poll = now;
        if (offset != position) last_move = now;
        position = offset;
        if (now - last_move > 5000000000ULL) return false;
        unsigned active   = offset / bytes;
        unsigned advanced = (active + AUDIO_OUTPUT_PERIODS - sequence % AUDIO_OUTPUT_PERIODS) % AUDIO_OUTPUT_PERIODS;
        if (!ready[active]) return false;
        for (unsigned i = 0; i < advanced; ++i)
        {
            unsigned slot = sequence % AUDIO_OUTPUT_PERIODS;
            if (!ready[slot]) return false;
            ready[slot] = false;
            ++sequence;
        }
        return true;
    }

    bool refill_valid(uint64_t n)
    {
        return running && !failed && n && n <= sequence && sequence - n < AUDIO_OUTPUT_PERIODS - 1;
    }

    int refill(uint64_t n, const int16_t *pcm)
    {
        spin_lock(&lock);
        bool valid = refill_valid(n);
        spin_unlock(&lock);
        if (!valid) return -EIO;
        unsigned slot = (n - 1) % AUDIO_OUTPUT_PERIODS;
        memcpy(data + slot * bytes, pcm, bytes);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        spin_lock(&lock);
        valid = refill_valid(n);
        if (valid) ready[slot] = true;
        spin_unlock(&lock);
        return valid ? 0 : -EIO;
    }
};
