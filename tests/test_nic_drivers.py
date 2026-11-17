"""Execute the actual NIC queue code against bounded DMA/register adapters."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def compile_run(header, adapters, production, cases):
    with tempfile.TemporaryDirectory(prefix="xj380-nic-unit-") as directory:
        work = Path(directory)
        source, binary = work / "nic.cpp", work / "nic"
        source.write_text('#include "' + str(ROOT / header) + '"\n' + adapters + production + cases)
        subprocess.run([os.environ.get("CXX", "clang++"), "-std=gnu++17", "-O1", "-g", "-Wall", "-Wextra",
                        "-Werror", "-fno-pie", "-no-pie", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                            "UBSAN_OPTIONS": "halt_on_error=1"})


class NicDriverTests(unittest.TestCase):
    def test_rtl_ownership_completion_and_circular_receive(self):
        source = (ROOT / "kmod/rtl8139/rtl8139.cpp").read_text()
        production = "\n".join(function(source, marker) for marker in (
            "static int rtl8139_init_device(", "static int rtl8139_send(",
            "static uint32_t rtl_ring_read(", "static int rtl8139_receive("))
        adapters = r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static std::vector<void *> allocations;
static uint32_t status[4], rx_config, multicast[2];
static int phases[4];
static unsigned transmissions;
static uint16_t capr;
static uint8_t command;
static bool stalled, tx_error;
static uint64_t clock_ns;
static void rtl_lock(uint8_t *lock) { assert(!*lock); *lock = 1; }
static void rtl_unlock(uint8_t *lock) { assert(*lock); *lock = 0; }
static uint64_t nanoTime() { return clock_ns += 1000000; }
static void printk(const char *, ...) {}
static void rtl8139_reset(rtl8139_device_t *)
{
    command = 0;
    for (unsigned i = 0; i < 4; ++i) { status[i] = RTL8139_TX_STATUS_HOST_OWNS; phases[i] = -1; }
}
static void rtl8139_read_mac(rtl8139_device_t *) {}
static void *rtl_dma_alloc(size_t bytes, uint64_t *physical)
{
    void *memory = std::calloc(1, bytes);
    assert(memory);
    allocations.push_back(memory);
    *physical = 0x100000 + allocations.size() * 0x10000;
    return memory;
}
static void rtl_out8(rtl8139_device_t *, uint16_t reg, uint8_t value)
{
    if (reg == RTL8139_REG_CHIP_CMD) command = value;
}
static uint8_t rtl_in8(rtl8139_device_t *, uint16_t reg)
{
    assert(reg == RTL8139_REG_CHIP_CMD);
    return command;
}
static void rtl_out16(rtl8139_device_t *, uint16_t reg, uint16_t value)
{
    if (reg == RTL8139_REG_RX_BUF_PTR) capr = value;
}
static void rtl_out32(rtl8139_device_t *, uint16_t reg, uint32_t value)
{
    if (reg >= RTL8139_REG_TX_STATUS0 && reg < RTL8139_REG_TX_ADDR0)
    {
        assert((value & RTL8139_TX_STATUS_HOST_OWNS) == 0);
        assert((value & 0x1fff) >= 60 && (value & 0x0f000000) == 0);
        unsigned slot = (reg - RTL8139_REG_TX_STATUS0) / 4;
        status[slot] = 0; phases[slot] = 0; ++transmissions;
    }
    if (reg == RTL8139_REG_RX_CONFIG) rx_config = value;
    if (reg == RTL8139_REG_MAR0 || reg == RTL8139_REG_MAR0 + 4) multicast[(reg - RTL8139_REG_MAR0) / 4] = value;
}
static uint32_t rtl_in32(rtl8139_device_t *, uint16_t reg)
{
    unsigned slot = (reg - RTL8139_REG_TX_STATUS0) / 4;
    assert(slot < 4);
    if (phases[slot] >= 0 && !stalled)
    {
        if (phases[slot]++ == 0) return 0;
        if (phases[slot] == 2) return RTL8139_TX_STATUS_HOST_OWNS; // DMA done, wire TX pending.
        status[slot] = RTL8139_TX_STATUS_HOST_OWNS |
                       (tx_error ? RTL8139_TX_STATUS_CARRIER_LOST : RTL8139_TX_STATUS_OK);
    }
    return status[slot];
}
'''
        cases = r'''
int main()
{
    rtl8139_device_t dev = {};
    assert(rtl8139_init_device(&dev) == 0 && transmissions == 0);
    assert((rx_config & RTL8139_RX_CONFIG_WRAP) == 0);
    assert(multicast[0] == 0xffffffffU && multicast[1] == 0xffffffffU);
    uint8_t frame[80]; std::memset(frame, 0x39, sizeof(frame));
    for (unsigned i = 0; i < 12; ++i)
    {
        unsigned slot = dev.tx_index;
        assert(rtl8139_send(&dev, frame, 42) == 42);
        assert(!std::memcmp(dev.tx_buffers[slot], frame, 42));
        for (unsigned n = 42; n < 60; ++n) assert(dev.tx_buffers[slot][n] == 0);
        assert(!dev.tx_pending[slot] && !dev.tx_lock);
    }
    unsigned slot = dev.tx_index, before = transmissions;
    status[slot] = 0; phases[slot] = -1;
    assert(rtl8139_send(&dev, frame, sizeof(frame)) == -1 && transmissions == before && !dev.tx_lock);
    status[slot] = RTL8139_TX_STATUS_HOST_OWNS;
    stalled = true;
    assert(rtl8139_send(&dev, frame, sizeof(frame)) == -1 && dev.tx_pending[slot] && !dev.tx_lock);
    dev.tx_index = slot;
    status[slot] = RTL8139_TX_STATUS_HOST_OWNS; phases[slot] = -1;
    before = transmissions;
    assert(rtl8139_send(&dev, frame, sizeof(frame)) == -1 && transmissions == before);
    status[slot] |= RTL8139_TX_STATUS_OK; stalled = false; tx_error = true;
    assert(rtl8139_send(&dev, frame, sizeof(frame)) == -1 && !dev.tx_pending[slot] && !dev.tx_lock);

    dev.rx_offset = RTL8139_RX_RING_SIZE - 8;
    auto put = [&](unsigned offset, uint8_t value) { dev.rx_ring[offset & RTL8139_RX_RING_MASK] = value; };
    unsigned start = dev.rx_offset;
    put(start, 1); put(start + 1, 0); put(start + 2, 24); put(start + 3, 0);
    for (unsigned i = 0; i < 20; ++i) put(start + 4 + i, uint8_t(i + 19));
    uint8_t receive[20] = {};
    assert(rtl8139_receive(&dev, receive, sizeof(receive)) == 20);
    for (unsigned i = 0; i < 20; ++i) assert(receive[i] == i + 19);
    assert(dev.rx_offset == 20 && capr == 4 && !dev.rx_lock);
    start = dev.rx_offset;
    put(start + 2, 0xf0); put(start + 3, 0xff);
    assert(rtl8139_receive(&dev, receive, sizeof(receive)) == 0 && dev.rx_offset == start && capr == 4);
    put(start, RTL8139_RX_STATUS_OK | RTL8139_RX_STATUS_CRC); put(start + 2, 24); put(start + 3, 0);
    assert(rtl8139_receive(&dev, receive, sizeof(receive)) == 0 && dev.rx_offset == start + 28);
    command |= RTL8139_CMD_RX_EMPTY;
    assert(rtl8139_receive(&dev, receive, sizeof(receive)) == 0 && !dev.rx_lock);
    for (void *allocation : allocations) std::free(allocation);
    puts("RTL8139 OWN/TOK/timeout ownership, padding, multicast and RX wrap PASS");
}
'''
        compile_run("kmod/rtl8139/rtl8139.h", adapters, production, cases)

    def test_virtio_legacy_layout_lifetime_and_ring_wrap(self):
        source = (ROOT / "kmod/virtio-net/virtio_net.cpp").read_text()
        production = "\n".join(function(source, marker) for marker in (
            "static inline void vio_lock(", "static inline void vio_unlock(",
            "static void *vio_dma_alloc(", "static void virtio_free_queue(", "static bool virtio_setup_queue(",
            "static void virtio_publish(", "static int virtio_net_init(", "static bool virtio_reap_tx(",
            "static int virtio_net_send(", "static int virtio_net_receive("))
        adapters = r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
constexpr size_t PAGE_SIZE = 4096;
static std::map<uint64_t, std::pair<void *, size_t>> memory;
static uint64_t next_physical = 0x100000;
static unsigned allocation_calls, fail_allocation;
static uint8_t hw_status;
static uint16_t selected, offered_size = 256, offered_tx_size;
static uint32_t pfns[2];
static uint64_t alloc_frames_dma32(size_t pages)
{
    if (++allocation_calls == fail_allocation) return 0;
    void *allocation = std::aligned_alloc(PAGE_SIZE, pages * PAGE_SIZE);
    assert(allocation);
    uint64_t physical = next_physical;
    next_physical += pages * PAGE_SIZE;
    memory[physical] = {allocation, pages};
    return physical;
}
static void *phys_to_virt(uint64_t physical) { return memory.at(physical).first; }
static void free_frames(uint64_t physical, size_t pages)
{
    assert(pfns[0] != physical / PAGE_SIZE && pfns[1] != physical / PAGE_SIZE);
    assert(memory.at(physical).second == pages);
    std::free(memory.at(physical).first);
    memory.erase(physical);
}
static void vio_out8(virtio_net_device_t *, uint16_t reg, uint8_t value)
{
    assert(reg == VIRTIO_PCI_STATUS);
    hw_status = value;
    if (!value) pfns[0] = pfns[1] = 0;
}
static uint8_t vio_in8(virtio_net_device_t *, uint16_t reg)
{
    return reg == VIRTIO_PCI_STATUS ? hw_status : uint8_t(reg);
}
static void vio_out16(virtio_net_device_t *, uint16_t reg, uint16_t value)
{
    if (reg == VIRTIO_PCI_QUEUE_SEL) { assert(value < 2); selected = value; }
    else { assert(reg == VIRTIO_PCI_QUEUE_NOTIFY && value < 2 && (hw_status & VIRTIO_STATUS_DRIVER_OK)); }
}
static uint16_t vio_in16(virtio_net_device_t *, uint16_t reg)
{
    assert(reg == VIRTIO_PCI_QUEUE_NUM);
    return selected == 1 && offered_tx_size ? offered_tx_size : offered_size;
}
static void vio_out32(virtio_net_device_t *, uint16_t reg, uint32_t value)
{
    if (reg == VIRTIO_PCI_QUEUE_PFN) pfns[selected] = value;
    else assert(reg == VIRTIO_PCI_GUEST_FEATURES && value == VIRTIO_NET_F_MAC);
}
static uint32_t vio_in32(virtio_net_device_t *, uint16_t reg)
{
    return reg == VIRTIO_PCI_QUEUE_PFN ? pfns[selected] : VIRTIO_NET_F_MAC;
}
'''
        cases = r'''
static void cleanup(virtio_net_device_t &dev)
{
    vio_out8(&dev, VIRTIO_PCI_STATUS, 0);
    virtio_free_queue(&dev.rx); virtio_free_queue(&dev.tx);
    assert(memory.empty());
}
static void complete(virtio_net_queue_t &queue, uint32_t slot, uint32_t bytes)
{
    uint16_t used = queue.used->idx;
    queue.used->ring[used % queue.queue_size].id = slot;
    queue.used->ring[used % queue.queue_size].len = bytes;
    __atomic_store_n(&queue.used->idx, uint16_t(used + 1), __ATOMIC_RELEASE);
}
int main()
{
    for (uint16_t size : {uint16_t(0), uint16_t(3), uint16_t(32769)})
    {
        virtio_net_device_t dev = {}; offered_size = size;
        assert(virtio_net_init(&dev) == -1 && memory.empty());
    }
    for (uint16_t size : {uint16_t(16), uint16_t(128), uint16_t(256), uint16_t(1024)})
    {
        virtio_net_device_t dev = {}; offered_size = size;
        assert(virtio_net_init(&dev) == 0 && dev.rx.queue_size == size && dev.tx.queue_size == size);
        size_t used_offset = (16 * size + 2 * (size + 3) + 4095) & ~size_t(4095);
        assert((const volatile uint8_t *)dev.rx.used - (uint8_t *)dev.rx.desc == (std::ptrdiff_t)used_offset);
        assert(dev.rx.avail->idx == size && dev.rx.avail->flags == VIRTQ_AVAIL_F_NO_INTERRUPT);
        for (unsigned i = 0; i < size; ++i)
        {
            assert(dev.rx.avail->ring[i] == i && dev.rx.desc[i].flags == VIRTQ_DESC_F_WRITE);
            assert(dev.rx.desc[i].addr == dev.rx.buffer_phys + i * VIRTIO_NET_BUFFER_SIZE);
        }
        cleanup(dev);
    }
    offered_size = 512; offered_tx_size = 256;
    virtio_net_device_t asymmetric = {};
    assert(virtio_net_init(&asymmetric) == 0 && asymmetric.rx.queue_size == 512 && asymmetric.tx.queue_size == 256);
    cleanup(asymmetric);
    offered_tx_size = 0;
    offered_size = 256;
    fail_allocation = allocation_calls + 4; // RX published, TX pool allocation fails.
    virtio_net_device_t failed = {};
    assert(virtio_net_init(&failed) == -1 && memory.empty() && !pfns[0] && !pfns[1]);
    fail_allocation = 0;
    virtio_net_device_t dev = {};
    assert(virtio_net_init(&dev) == 0);
    uint8_t frame[64]; std::memset(frame, 0x6a, sizeof(frame));
    for (unsigned i = 0; i < offered_size; ++i) assert(virtio_net_send(&dev, frame, sizeof(frame)) == 64);
    assert(dev.tx.in_flight == 256 && virtio_net_send(&dev, frame, sizeof(frame)) == -1 && dev.tx.available == 256);
    assert(dev.tx.desc[0].len == 74 && !std::memcmp(dev.tx.buffers + 10, frame, 64));
    complete(dev.tx, 9, 0); complete(dev.tx, 0, 0); // Completions may arrive out of order.
    assert(virtio_net_send(&dev, frame, sizeof(frame)) == 64 && dev.tx.avail->ring[0] == 0);
    assert(virtio_net_send(&dev, frame, sizeof(frame)) == 64 && dev.tx.avail->ring[1] == 9);
    complete(dev.tx, 9999, 0);
    assert(virtio_net_send(&dev, frame, sizeof(frame)) == -1 && dev.tx.broken && !dev.tx.lock);
    cleanup(dev);

    offered_size = 8; dev = {}; assert(virtio_net_init(&dev) == 0);
    uint8_t received[64];
    for (unsigned n = 0; n < 65540; ++n)
    {
        assert(virtio_net_send(&dev, frame, sizeof(frame)) == 64);
        unsigned slot = dev.tx.avail->ring[uint16_t(dev.tx.available - 1) % 8];
        complete(dev.tx, slot, 0);
        unsigned rx_slot = n % 8;
        std::memcpy(dev.rx.buffers + rx_slot * VIRTIO_NET_BUFFER_SIZE + 10, frame, sizeof(frame));
        complete(dev.rx, rx_slot, 74);
        assert(virtio_net_receive(&dev, received, sizeof(received)) == 64 && !std::memcmp(frame, received, 64));
    }
    assert(dev.tx.available == 4 && dev.rx.available == 12 && dev.rx.last_used == 4);
    complete(dev.rx, 0, VIRTIO_NET_BUFFER_SIZE + 1);
    assert(virtio_net_receive(&dev, received, sizeof(received)) == 0 && !dev.rx.broken);
    complete(dev.rx, 0, 9);
    assert(virtio_net_receive(&dev, received, sizeof(received)) == 0);
    complete(dev.rx, 0, 74);
    assert(virtio_net_receive(&dev, received, 7) == 7);
    unsigned available = dev.rx.available;
    complete(dev.rx, 1000, 74);
    assert(virtio_net_receive(&dev, received, sizeof(received)) == -1 && dev.rx.broken && !dev.rx.lock);
    assert(dev.rx.available == available);
    cleanup(dev);
    puts("VirtIO queue layout/partial cleanup/TX ownership/legacy header/malformed RX/16-bit wrap PASS");
}
'''
        compile_run("kmod/virtio-net/virtio_net.h", adapters, production, cases)


if __name__ == "__main__":
    unittest.main()
