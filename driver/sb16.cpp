// SPDX-License-Identifier: Apache-2.0
// Creative SB16 DSP command interface, ISA IRQ 5 / 16-bit DMA channel 5.
#include "sb16.h"
#include "audio/pcm_ring.h"
#include <dma.h>
#include <pctable/idt.h>

extern void init_idt_desc(uint8_t, uint8_t, int_func, uint8_t, void *);

namespace {
PcmRing               ring;
bool                  present;
bool                  owned;
thread_control_block *worker;
uint64_t              irq_count;

bool command(uint8_t value)
{
    for (unsigned i = 0; i < 10000; ++i)
    {
        if (!(inb(SB_WRITE) & 0x80))
        {
            outb(SB_WRITE, value);
            return true;
        }
        delay_us_hp(1);
    }
    return false;
}

int read_dsp()
{
    for (unsigned i = 0; i < 10000; ++i)
    {
        if (inb(SB_STATE) & 0x80) return inb(SB_READ);
        delay_us_hp(1);
    }
    return -1;
}

unsigned position()
{
    // The 8237 word-channel count is in words minus one. Repeated snapshots
    // prevent a low-byte rollover between the two 8-bit port reads.
    auto count = []() -> unsigned {
        outb(0xd8, 0);
        unsigned lo = inb(0xc6);
        return lo | ((unsigned)inb(0xc6) << 8);
    };
    unsigned a = count(), b = count();
    for (unsigned i = 0; i < 4 && ((a - b) & 0xffff) > 128; ++i)
    {
        a = b;
        b = count();
    }
    if (b >= PcmRing::total / 2) return PcmRing::total;
    return (PcmRing::total - (b + 1) * 2) % PcmRing::total;
}

void halt_locked()
{
    // Mask DMA before pausing the DSP; do not release its retained storage.
    outb(0xd4, 5);
    ring.running = false;
}

void stop()
{
    spin_lock(&ring.lock);
    halt_locked();
    spin_unlock(&ring.lock);
    command(DSP_CMD_STOP_16BIT);
    __atomic_store_n(&owned, false, __ATOMIC_RELEASE);
}

void abort_playback()
{
    spin_lock(&ring.lock);
    ring.failed = true;
    spin_unlock(&ring.lock);
    stop();
}

int start_owned()
{
    if (!present) return -ENODEV;
    if (!ring.data)
    {
        ring.physical = alloc_frames_dma16(PcmRing::total / PAGE_SIZE);
        if (!ring.physical) return -ENOMEM;
        ring.data = (uint8_t *)phys_to_virt(ring.physical);
    }
    if (ring.physical >= 0x1000000 || (ring.physical >> 17) != ((ring.physical + PcmRing::total - 1) >> 17))
        return -EIO;
    memset(ring.data, 0, PcmRing::total);
    if (!sb_reset() || !command(0x41) || !command(44100 >> 8) || !command(44100 & 255) || !command(DSP_CMD_SPEAKER_ON))
        return -EIO;
    outb(SB_MIXER, 0x81);
    outb(SB_MIXER_DATA, 0x22); // 8-bit DMA1, 16-bit DMA5
    outb(SB_MIXER, 0x80);
    outb(SB_MIXER_DATA, 2); // IRQ5
    spin_lock(&ring.lock);
    // 0x58 = memory-to-device, auto-init, single transfer. DMA5 is word based.
    dma_start(0x58, 5, (uint32_t *)(uintptr_t)ring.physical, PcmRing::total);
    ring.reset(44100);
    spin_unlock(&ring.lock);
    unsigned samples = PcmRing::bytes / 2 - 1;
    bool     ok      = command(0xb6) && command(0x30) && command(samples & 255) && command(samples >> 8);
    if (!ok)
    {
        spin_lock(&ring.lock);
        ring.failed = true;
        halt_locked();
        spin_unlock(&ring.lock);
        command(DSP_CMD_STOP_16BIT);
    }
    return ok ? 0 : -EIO;
}

int start()
{
    if (__atomic_exchange_n(&owned, true, __ATOMIC_ACQ_REL)) return -EBUSY;
    int result = start_owned();
    if (result) __atomic_store_n(&owned, false, __ATOMIC_RELEASE);
    return result;
}

int progress(uint64_t *completed)
{
    spin_lock(&ring.lock);
    bool ok = !ring.failed && ring.observe(position());
    if (!ok)
    {
        ring.failed = true;
        halt_locked();
    }
    *completed = ring.sequence;
    spin_unlock(&ring.lock);
    if (!ok) command(DSP_CMD_STOP_16BIT);
    return ok ? 0 : -EIO;
}

int refill(uint64_t n, const int16_t *pcm)
{
    uint64_t completed;
    if (progress(&completed) || ring.refill(n, pcm) || progress(&completed)) return -EIO;
    spin_lock(&ring.lock);
    bool valid = ring.refill_valid(n);
    spin_unlock(&ring.lock);
    return valid ? 0 : -EIO;
}

void set_worker(thread_control_block *task)
{
    __atomic_store_n(&worker, task, __ATOMIC_RELEASE);
}

uint64_t interrupts()
{
    return __atomic_load_n(&irq_count, __ATOMIC_RELAXED);
}

const AudioBackend backend = {"Sound Blaster 16", 44100,      start,     stop, progress, refill,
                              abort_playback,     set_worker, interrupts};
} // namespace

extern "C" void sb16_handler(struct X64_REGS *, uint64_t)
{
    inb(SB_INTR16);
    __atomic_add_fetch(&irq_count, 1, __ATOMIC_RELAXED);
    send_eoi();
    thread_control_block *task = __atomic_load_n(&worker, __ATOMIC_ACQUIRE);
    if (task) scheduler_wake_task(task);
}

bool sb_reset()
{
    outb(SB_RESET, 1);
    delay_us_hp(5);
    outb(SB_RESET, 0);
    return read_dsp() == 0xaa;
}

void sb_out(uint8_t value)
{
    command(value);
}

void sb16_set_volume(uint8_t left, uint8_t right)
{
    outb(SB_MIXER, 0x22);
    outb(SB_MIXER_DATA, ((left > 15 ? 15 : left) << 4) | (right > 15 ? 15 : right));
}

void sb16_set_sample_rate(uint16_t rate)
{
    command(0x41);
    command(rate >> 8);
    command(rate & 255);
}

const AudioBackend *sb16_probe()
{
    if (!sb_reset() || !command(DSP_CMD_GET_VERSION)) return nullptr;
    int major = read_dsp(), minor = read_dsp();
    if (major < 4 || minor < 0) return nullptr;
    init_idt_desc(40, X86_64_INTR_GATE, sb16_interr, 0, nullptr);
    present = true;
    sb16_set_volume(15, 15);
    write_serial_fmt("audio: SB16 DSP %u.%u IRQ5 DMA5 at 0x220\n", major, minor);
    return &backend;
}

void sb16_init()
{
    sb16_probe();
}

// Keep the legacy synchronous entry points for existing kernel callers.
// Shared XAudio ownership returns busy, and every hardware wait is bounded.
void sb16_play(const uint8_t *data, size_t size)
{
    if (!data || !size || (size & 3) || start()) return;
    uint64_t done = 0, supplied = 0, tail = 0;
    int16_t  block[AUDIO_OUTPUT_FRAMES * 2];
    size_t   offset = 0;
    while (!progress(&done))
    {
        while (supplied < done)
        {
            size_t n = size - offset;
            if (n > sizeof(block)) n = sizeof(block);
            memset(block, 0, sizeof(block));
            memcpy(block, data + offset, n);
            if (refill(++supplied, block))
            {
                stop();
                return;
            }
            offset += n;
            if (n) tail = supplied + AUDIO_OUTPUT_PERIODS;
        }
        if (offset == size && done >= tail) break;
        scheduler_sleep_ns(1000000);
    }
    stop();
}

void sb16_send_data(const uint8_t *data, size_t size)
{
    sb16_play(data, size);
}
