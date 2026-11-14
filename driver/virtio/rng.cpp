// SPDX-License-Identifier: Apache-2.0
#include <virtio/rng.h>

#include <krlibc.h>
#include <cpu/lock.h>
#include <pci/pci.h>
#include <proto.hpp>
#include <task/scheduler.h>

namespace
{
constexpr size_t kBufferBytes = 512;
constexpr uint64_t kTimeout = 100000000ULL;

struct Descriptor
{
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
};

struct Used
{
    uint32_t id, length;
};

struct Device
{
    pci_device_t *pci;
    volatile uint8_t *common, *notify;
    uint32_t notify_length, notify_multiplier, notify_offset;
    uint16_t port, size, available, consumed;
    Descriptor *descriptors;
    volatile uint16_t *avail, *used;
    uint8_t *buffer;
    size_t position, length;
    bool pending, ready;
};

Device device;
spin_t device_lock = SPIN_INIT;

static void erase(void *buffer, size_t bytes)
{
    volatile uint8_t *p = static_cast<volatile uint8_t *>(buffer);
    while (bytes--) *p++ = 0;
}

static uint32_t config(pci_device_t *pci, unsigned offset)
{
    return pci->op->read(pci->bus, pci->slot, pci->func, pci->segment, offset & ~3U);
}

static uint8_t config_byte(pci_device_t *pci, unsigned offset)
{
    return uint8_t(config(pci, offset) >> ((offset & 3) * 8));
}

static uint32_t io_read(uint16_t port, unsigned width)
{
    uint32_t value;
    if (width == 1)
    {
        uint8_t byte;
        __asm__ volatile("inb %1, %0" : "=a"(byte) : "Nd"(port));
        value = byte;
    }
    else if (width == 2)
    {
        uint16_t word;
        __asm__ volatile("inw %1, %0" : "=a"(word) : "Nd"(port));
        value = word;
    }
    else __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void io_write(uint16_t port, unsigned width, uint32_t value)
{
    if (width == 1) __asm__ volatile("outb %0, %1" : : "a"(uint8_t(value)), "Nd"(port));
    else if (width == 2) __asm__ volatile("outw %0, %1" : : "a"(uint16_t(value)), "Nd"(port));
    else __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t mmio_read(volatile uint8_t *base, unsigned offset, unsigned width)
{
    if (width == 1) return base[offset];
    if (width == 2) return *reinterpret_cast<volatile uint16_t *>(base + offset);
    return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

static void mmio_write(volatile uint8_t *base, unsigned offset, unsigned width, uint32_t value)
{
    if (width == 1) base[offset] = uint8_t(value);
    else if (width == 2) *reinterpret_cast<volatile uint16_t *>(base + offset) = uint16_t(value);
    else *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
}

static void address_write(unsigned offset, uint64_t address)
{
    mmio_write(device.common, offset, 4, uint32_t(address));
    mmio_write(device.common, offset + 4, 4, uint32_t(address >> 32));
}

static uint8_t status()
{
    return device.common ? mmio_read(device.common, 20, 1) : io_read(device.port + 18, 1);
}

static void set_status(uint8_t value)
{
    if (device.common) mmio_write(device.common, 20, 1, value);
    else io_write(device.port + 18, 1, value);
}

static volatile uint8_t *capability(pci_device_t *pci, unsigned cap, unsigned minimum, uint32_t *length)
{
    uint8_t bar = config_byte(pci, cap + 4);
    uint32_t offset = config(pci, cap + 8), bytes = config(pci, cap + 12);
    if (bar >= 6 || !pci->bars[bar].mmio || !pci->bars[bar].address || bytes < minimum ||
        (offset & 3) || offset > pci->bars[bar].size || bytes > pci->bars[bar].size - offset ||
        pci->bars[bar].address > __UINT64_MAX__ - offset) return nullptr;
    *length = bytes;
    return static_cast<volatile uint8_t *>(phys_to_virt(pci->bars[bar].address + offset));
}

static bool modern_transport(pci_device_t *pci)
{
    // Bound and validate the untrusted PCI capability chain, including cycles.
    uint64_t visited = 0;
    unsigned cap = config_byte(pci, 0x34);
    for (unsigned count = 0; cap && count < 48; ++count)
    {
        if (cap < 0x40 || cap > 0xfc || (cap & 3) || (visited & (1ULL << (cap / 4)))) return false;
        visited |= 1ULL << (cap / 4);
        unsigned length = config_byte(pci, cap + 2);
        if (config_byte(pci, cap) == 9 && length >= 16 && cap + length <= 256)
        {
            unsigned type = config_byte(pci, cap + 3);
            uint32_t bytes = 0;
            if (type == 1 && !device.common) device.common = capability(pci, cap, 56, &bytes);
            if (type == 2 && length >= 20 && !device.notify)
            {
                device.notify = capability(pci, cap, 2, &device.notify_length);
                device.notify_multiplier = config(pci, cap + 16);
            }
        }
        cap = config_byte(pci, cap + 1);
    }
    return device.common && device.notify;
}

static void post_buffer()
{
    erase(device.buffer, kBufferBytes);
    device.position = device.length = 0;
    device.avail[2 + device.available % device.size] = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    device.avail[1] = ++device.available;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    device.pending = true;
    if (device.common) mmio_write(device.notify, device.notify_offset, 2, 0);
    else io_write(device.port + 16, 2, 0);
}

static bool setup(pci_device_t *pci)
{
    device = {};
    device.pci = pci;
    bool modern = modern_transport(pci);
    if (!modern)
    {
        device.common = device.notify = nullptr;
        if (pci->device_id != 0x1005 || pci->bars[0].mmio || !pci->bars[0].address ||
            pci->bars[0].address > 0xffe0) return false;
        device.port = uint16_t(pci->bars[0].address);
    }
    // Polling driver: disable INTx, and leave MSI-X queue vectors unassigned.
    uint32_t command = (config(pci, 4) & 0xffff) | PCI_COMMAND_MASTER | PCI_RCMD_DISABLE_INTR;
    command |= modern ? PCI_COMMAND_MEMORY : PCI_COMMAND_IO;
    pci->op->write(pci->bus, pci->slot, pci->func, pci->segment, 4, command);
    set_status(0);
    for (unsigned i = 0; status() && i < 1024; ++i) __asm__ volatile("pause");
    if (status()) return false;
    set_status(3);
    if (modern)
    {
        mmio_write(device.common, 0, 4, 1);
        if (!(mmio_read(device.common, 4, 4) & 1)) return false; // VIRTIO_F_VERSION_1
        mmio_write(device.common, 8, 4, 0);
        mmio_write(device.common, 12, 4, 0);
        mmio_write(device.common, 8, 4, 1);
        mmio_write(device.common, 12, 4, 1);
        set_status(11);
        if (!(status() & 8)) return false;
        mmio_write(device.common, 16, 2, 0xffff);
        mmio_write(device.common, 22, 2, 0);
        if (mmio_read(device.common, 28, 2)) return false;
        device.size = uint16_t(mmio_read(device.common, 24, 2));
        if (device.size > 8) device.size = 8;
    }
    else
    {
        io_write(device.port + 4, 4, 0);
        io_write(device.port + 14, 2, 0);
        device.size = uint16_t(io_read(device.port + 12, 2));
        if (io_read(device.port + 8, 4)) return false;
    }
    if (!device.size || device.size > 1024 || (device.size & (device.size - 1))) return false;
    size_t avail_offset = sizeof(Descriptor) * device.size;
    size_t used_offset = (avail_offset + (3 + device.size) * 2 + 4095) & ~size_t(4095);
    size_t pages = (used_offset + 6 + sizeof(Used) * device.size + 4095) / 4096;
    uint64_t queue = alloc_frames_dma32(pages), buffer = alloc_frames_dma32(1);
    if (!queue || !buffer)
    {
        if (queue) free_frames(queue, pages);
        if (buffer) free_frames(buffer, 1);
        return false;
    }
    auto *memory = static_cast<uint8_t *>(phys_to_virt(queue));
    memset(memory, 0, pages * 4096);
    device.descriptors = reinterpret_cast<Descriptor *>(memory);
    device.avail = reinterpret_cast<volatile uint16_t *>(memory + avail_offset);
    device.used = reinterpret_cast<volatile uint16_t *>(memory + used_offset);
    device.buffer = static_cast<uint8_t *>(phys_to_virt(buffer));
    device.descriptors[0] = {buffer, kBufferBytes, 2, 0}; // device-writable only
    device.avail[0] = 1; // VIRTQ_AVAIL_F_NO_INTERRUPT
    if (modern)
    {
        uint64_t offset = uint64_t(mmio_read(device.common, 30, 2)) * device.notify_multiplier;
        if (offset > device.notify_length - 2 || (offset & 1))
        {
            free_frames(queue, pages);
            free_frames(buffer, 1);
            return false;
        }
        device.notify_offset = uint32_t(offset);
        mmio_write(device.common, 24, 2, device.size);
        mmio_write(device.common, 26, 2, 0xffff);
        address_write(32, queue);
        address_write(40, queue + avail_offset);
        address_write(48, queue + used_offset);
        mmio_write(device.common, 28, 2, 1);
    }
    else io_write(device.port + 8, 4, uint32_t(queue >> 12));
    // After queue publication these pages are permanently pinned, including on
    // timeout/reset failure. A late DMA completion must never hit reused memory.
    set_status(modern ? 15 : 7);
    if (!(status() & 4)) return false;
    __atomic_store_n(&device.ready, true, __ATOMIC_RELEASE);
    post_buffer();
    write_serial_fmt("virtio-rng: %02x:%02x.%u transport=%s queue=%u\n", pci->bus, pci->slot, pci->func,
                     modern ? "modern" : "legacy", unsigned(device.size));
    return true;
}

static bool receive()
{
    uint16_t index = device.used[1];
    if (index == device.consumed) return false;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    auto *entries = reinterpret_cast<volatile Used *>(device.used + 2);
    unsigned slot = device.consumed % device.size;
    uint32_t id = entries[slot].id, length = entries[slot].length;
    if (uint16_t(index - device.consumed) != 1 || id != 0 || !length || length > kBufferBytes)
    {
        __atomic_store_n(&device.ready, false, __ATOMIC_RELEASE);
        set_status(0); // DMA memory stays pinned even if reset is delayed.
        return false;
    }
    ++device.consumed;
    device.length = length;
    device.position = 0;
    device.pending = false;
    return true;
}
}

void virtio_rng_init()
{
    for (uint32_t i = 0; i < get_pci_num() && i < PCI_DEVICE_MAX; ++i)
    {
        pci_device_t *pci = pci_devices[i];
        if (pci && pci->vendor_id == 0x1af4 && (pci->device_id == 0x1005 || pci->device_id == 0x1044))
        {
            if (setup(pci)) return;
            if (device.port || device.common) set_status(0);
        }
    }
    write_serial_string("virtio-rng: no usable PCI entropy device\n");
}

bool virtio_rng_read(void *output, size_t bytes, bool nonblocking)
{
    if (!bytes) return true;
    if (!output) return false;
    if (!scheduler_is_enabled()) nonblocking = true;
    size_t done = 0;
    uint64_t start = nonblocking ? 0 : nanoTime();
    for (unsigned polls = 0; done < bytes; ++polls)
    {
        // Only ring metadata and bounded copies are protected. A caller may
        // sleep, yield or terminate while waiting without abandoning a global
        // busy flag or preventing other callers from collecting late DMA.
        spin_lock(&device_lock);
        bool was_ready = device.ready;
        bool progress = false;
        if (device.ready && device.position < device.length)
        {
            size_t count = bytes - done;
            if (count > device.length - device.position) count = device.length - device.position;
            memcpy(static_cast<uint8_t *>(output) + done, device.buffer + device.position, count);
            erase(device.buffer + device.position, count);
            device.position += count;
            done += count;
            if (device.position == device.length) post_buffer();
            progress = true;
        }
        else if (device.ready && device.pending) progress = receive();
        bool ready = device.ready;
        spin_unlock(&device_lock);
        if (was_ready && !ready) write_serial_string("virtio-rng: invalid completion, device disabled\n");
        if (done == bytes) return true;
        if (!ready) break;
        if (progress) continue;
        if (nonblocking || !scheduler_is_enabled() || polls >= 1000000 || nanoTime() - start >= kTimeout) break;
        if ((polls & 255) == 255) scheduler_yield();
        else __asm__ volatile("pause" ::: "memory");
    }
    erase(output, bytes);
    return false;
}
