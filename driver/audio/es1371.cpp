/*
Copyright (c) 1998, 1999 The NetBSD Foundation, Inc.
All rights reserved.

This code is derived from software contributed to The NetBSD Foundation
by Lennart Augustsson <augustss@netbsd.org> and Charles M. Hannum.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/
// ES1371 AC'97 output. Codec/SRC initialization follows the BSD-2-Clause
// eap(4) driver, OpenBSD eap.c revision 1.66 (2024-09-01), ES1371 support
// credited there to Ezra Story. XJ380 DMA and playback integration is new.
#include "pcm_ring.h"
#include <pci/pci.h>

namespace {
PcmRing  ring;
uint16_t io;

bool wait_bits(unsigned reg, uint32_t mask, uint32_t value)
{
    for (unsigned i = 0; i < 5000; ++i)
    {
        if ((inl(io + reg) & mask) == value) return true;
        delay_us_hp(1);
    }
    write_serial_fmt("audio: ES1371 register %x timeout value=%x mask=%x expected=%x\n", reg, inl(io + reg), mask,
                     value);
    return false;
}

bool src_write(unsigned index, uint16_t value)
{
    if (!wait_bits(0x10, 0x00800000, 0)) return false;
    uint32_t control = inl(io + 0x10) & 0x00780000;
    outl(io + 0x10, control | (index << 25) | 0x01000000 | value);
    return wait_bits(0x10, 0x00800000, 0);
}

bool codec_command(unsigned reg, uint16_t value, bool read)
{
    if (!wait_bits(0x14, 0x40000000, 0) || !wait_bits(0x10, 0x00800000, 0)) return false;
    uint32_t saved = inl(io + 0x10) & 0x00780000;
    outl(io + 0x10, saved | 0x00010000);
    // Expose the codec safe-access window in the SRC state machine.
    if (!wait_bits(0x10, 0x00870000, 0) || !wait_bits(0x10, 0x00870000, 0x00010000))
    {
        outl(io + 0x10, saved);
        return false;
    }
    outl(io + 0x14, (reg << 16) | value | (read ? 0x00800000U : 0));
    bool ready = wait_bits(0x10, 0x00800000, 0);
    outl(io + 0x10, saved);
    return ready && wait_bits(0x14, 0x40000000, 0);
}

void stop()
{
    spin_lock(&ring.lock);
    outl(io, inl(io) & ~0x20U);
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
    spin_lock(&ring.lock);
    outl(io, inl(io) & ~0x20U);
    // DAC2 S16 stereo, cyclic frame buffer, two-byte end increment.
    // Keep period events enabled even with PCI INTx masked: VMware advances
    // its emulated DAC on these events. The worker acknowledges them below.
    outl(io + 0x20, 0x0010020c);
    outl(io + 0x0c, 0x0c);
    outl(io + 0x38, (uint32_t)ring.physical);
    outl(io + 0x3c, PcmRing::total / 4 - 1);
    outl(io + 0x28, AUDIO_OUTPUT_FRAMES - 1);
    ring.reset(48000);
    outl(io, inl(io) | 0x20U);
    spin_unlock(&ring.lock);
    return 0;
}

int progress(uint64_t *completed)
{
    spin_lock(&ring.lock);
    uint32_t status = inl(io + 4);
    if (status & 2)
    {
        uint32_t serial = inl(io + 0x20);
        outl(io + 0x20, serial & ~0x200U);
        outl(io + 0x20, serial);
    }
    outl(io + 0x0c, 0x0c);
    unsigned offset = (inl(io + 0x3c) >> 16) * 4;
    bool     good   = !ring.failed && ring.observe(offset);
    if (!good)
    {
        ring.failed  = true;
        ring.running = false;
        outl(io, inl(io) & ~0x20U);
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

const AudioBackend backend = {"Ensoniq ES1371", 48000,          start,      stop,      progress,
                              refill,           abort_playback, set_worker, interrupts};
} // namespace

const AudioBackend *es1371_probe()
{
    pci_device_t *device = pci_find_vid_did(0x1274, 0x1371);
    if (!device || device->bars[0].mmio || !device->bars[0].address || device->bars[0].address > 0xffc0) return nullptr;
    io               = (uint16_t)device->bars[0].address;
    uint32_t command = device->op->read(device->bus, device->slot, device->func, device->segment, PCI_CONF_COMMAND);
    device->op->write(device->bus, device->slot, device->func, device->segment, PCI_CONF_COMMAND,
                      (command & 0xffff) | PCI_COMMAND_IO | PCI_COMMAND_MASTER | PCI_RCMD_DISABLE_INTR);
    outl(io + 0x20, 0);
    outl(io, 0);
    outl(io + 0x18, 0);
    outl(io, 0x00004000); // AC-link warm reset
    delay_us_hp(20);
    outl(io, 0);
    delay_us_hp(20);
    outl(io + 0x10, 0x00400000);
    for (unsigned i = 0; i < 128; ++i)
        if (!src_write(i, 0)) return nullptr;
    // Fixed 48kHz ratio, unity volume. DAC1 and ADC stay disabled.
    const unsigned engines[] = {0x70, 0x74, 0x78};
    for (unsigned e : engines)
        if (!src_write(e, 16 << 4) || !src_write(e + 1, 16 << 10)) return nullptr;
    for (unsigned i = 0x7c; i <= 0x7f; ++i)
        if (!src_write(i, 1 << 12)) return nullptr;
    outl(io + 0x10, 0);
    if (!codec_command(0, 0, false) || !codec_command(0x26, 0, false) || !codec_command(0x02, 0, false) ||
        !codec_command(0x04, 0, false) || !codec_command(0x18, 0, false))
        return nullptr;
    if (!codec_command(0x7c, 0, true) || !wait_bits(0x14, 0x80000000, 0x80000000)) return nullptr;
    uint16_t vendor = (uint16_t)inl(io + 0x14);
    write_serial_fmt("audio: ES1371 %x:%x IO=%x codec vendor=%x\n", device->vendor_id, device->device_id, io, vendor);
    return &backend;
}
