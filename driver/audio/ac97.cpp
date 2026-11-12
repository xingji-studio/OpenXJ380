// SPDX-License-Identifier: Apache-2.0
// Intel 82801AA/AB AC'97 programming model, Intel manual 298028-001.
// NAMBAR accesses the codec; NABMBAR owns PCM-out bus mastering.
#include "pcm_ring.h"
#include <pci/pci.h>

namespace {
PcmRing  ring;
uint16_t mixer, bus;
struct Descriptor
{
    uint32_t address, control;
};
Descriptor *descriptors;
uint64_t    descriptor_phys;

bool codec_access()
{
    for (unsigned i = 0; i < 1000; ++i)
    {
        if (!(inb(bus + 0x34) & 1)) return true;
        delay_us_hp(10);
    }
    return false;
}

bool codec_write(unsigned reg, uint16_t value)
{
    if (!codec_access()) return false;
    outw(mixer + reg, value);
    return true;
}

void stop()
{
    spin_lock(&ring.lock);
    outb(bus + 0x1b, 0);
    ring.running = false;
    spin_unlock(&ring.lock);
}

void abort_playback()
{
    spin_lock(&ring.lock);
    ring.failed = true;
    spin_unlock(&ring.lock);
    stop();
}

int start()
{
    if (ring.running) return -EBUSY;
    if (!ring.allocate()) return -ENOMEM;
    if (!descriptors)
    {
        descriptor_phys = alloc_frames_dma32(1);
        if (!descriptor_phys) return -ENOMEM;
        descriptors = (Descriptor *)phys_to_virt(descriptor_phys);
    }
    outb(bus + 0x1b, 0);
    outb(bus + 0x1b, 2);
    unsigned wait = 0;
    while (inb(bus + 0x1b) & 2)
    {
        if (++wait == 1000) return -ETIMEDOUT;
        delay_us_hp(10);
    }
    for (unsigned i = 0; i < 32; ++i)
    {
        descriptors[i].address = (uint32_t)(ring.physical + (i % AUDIO_OUTPUT_PERIODS) * PcmRing::bytes);
        // Length is in 16-bit samples, including both channels.
        descriptors[i].control = (PcmRing::bytes / 2) | (1U << 31);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    outl(bus + 0x10, (uint32_t)descriptor_phys);
    outb(bus + 0x15, 31);
    outw(bus + 0x16, 0x1c);
    spin_lock(&ring.lock);
    ring.reset(48000);
    // Interrupts stay masked. The permanent XAudio worker polls CIV/PICB.
    outb(bus + 0x1b, 1);
    spin_unlock(&ring.lock);
    return 0;
}

int progress(uint64_t *completed)
{
    spin_lock(&ring.lock);
    unsigned current   = inb(bus + 0x14) & 31;
    unsigned remaining = inw(bus + 0x18);
    // CIV can change while PICB is read. Retry a bounded, consistent snapshot.
    unsigned retry = 0;
    while (current != (inb(bus + 0x14) & 31) && retry++ < 4)
    {
        current   = inb(bus + 0x14) & 31;
        remaining = inw(bus + 0x18);
    }
    uint16_t status = inw(bus + 0x16);
    unsigned offset = (current % AUDIO_OUTPUT_PERIODS) * PcmRing::bytes;
    bool     good   = remaining <= PcmRing::bytes / 2 && !(status & 0x10) && retry < 4;
    if (good) good = ring.observe((offset + PcmRing::bytes - remaining * 2) % PcmRing::total);
    if (!good)
    {
        ring.failed  = true;
        ring.running = false;
        outb(bus + 0x1b, 0);
    }
    else
    {
        outb(bus + 0x15, (uint8_t)((ring.sequence + 31) % 32));
        outw(bus + 0x16, status & 0x1c);
    }
    *completed = ring.sequence;
    spin_unlock(&ring.lock);
    return good ? 0 : -EIO;
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

void     set_worker(thread_control_block *) {}

uint64_t interrupts()
{
    return 0;
}

const AudioBackend backend = {"Intel ICH AC97", 48000,          start,      stop,      progress,
                              refill,           abort_playback, set_worker, interrupts};
} // namespace

const AudioBackend *ac97_probe()
{
    // These ICH generations share the legacy I/O NAM/NABM layout. MMIO-only
    // variants and unrelated class-0401 devices are deliberately not claimed.
    const uint16_t ids[]  = {0x2415, 0x2425, 0x2445, 0x2485, 0x24c5, 0x24d5, 0x266e};
    pci_device_t  *device = nullptr;
    for (uint16_t id : ids)
        if ((device = pci_find_vid_did(0x8086, id))) break;
    if (!device || device->bars[0].mmio || device->bars[1].mmio || !device->bars[0].address ||
        !device->bars[1].address || device->bars[0].address > 0xff00 || device->bars[1].address > 0xff00)
        return nullptr;
    mixer            = (uint16_t)device->bars[0].address;
    bus              = (uint16_t)device->bars[1].address;
    uint32_t command = device->op->read(device->bus, device->slot, device->func, device->segment, PCI_CONF_COMMAND);
    device->op->write(device->bus, device->slot, device->func, device->segment, PCI_CONF_COMMAND,
                      (command & 0xffff) | PCI_COMMAND_IO | PCI_COMMAND_MASTER | PCI_RCMD_DISABLE_INTR);
    outl(bus + 0x2c, (inl(bus + 0x2c) & ~8U) | 2U);
    unsigned wait = 0;
    while (!(inl(bus + 0x30) & (1U << 8)))
    {
        if (++wait == 1000)
        {
            write_serial_string("audio: ICH AC97 codec-ready timeout\n");
            return nullptr;
        }
        delay_us_hp(100);
    }
    if (!codec_write(0, 0) || !codec_write(0x26, 0) || !codec_write(0x2a, 0) || !codec_write(0x02, 0) ||
        !codec_write(0x18, 0))
        return nullptr;
    write_serial_fmt("audio: ICH AC97 %x:%x NAM=%x NABM=%x codec=%x:%x\n", device->vendor_id, device->device_id, mixer,
                     bus, inw(mixer + 0x7c), inw(mixer + 0x7e));
    return &backend;
}
