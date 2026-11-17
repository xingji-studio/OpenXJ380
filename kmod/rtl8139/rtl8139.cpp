#include "rtl8139.h"

extern "C" {
#include "../../include/netdev.h"
#include "../../include/pci/pci.h"
#include "../../include/proto.hpp"
}

static rtl8139_device_t *g_rtl8139_devices[8];
static uint32_t g_rtl8139_device_count;

static inline void rtl_out8(rtl8139_device_t *dev, uint16_t reg, uint8_t value) {
    uint16_t port = dev->io_base + reg;
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void rtl_out16(rtl8139_device_t *dev, uint16_t reg, uint16_t value) {
    uint16_t port = dev->io_base + reg;
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void rtl_out32(rtl8139_device_t *dev, uint16_t reg, uint32_t value) {
    uint16_t port = dev->io_base + reg;
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t rtl_in8(rtl8139_device_t *dev, uint16_t reg) {
    uint8_t value;
    uint16_t port = dev->io_base + reg;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline uint16_t rtl_in16(rtl8139_device_t *dev, uint16_t reg) {
    uint16_t value;
    uint16_t port = dev->io_base + reg;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline uint32_t rtl_in32(rtl8139_device_t *dev, uint16_t reg) {
    uint32_t value;
    uint16_t port = dev->io_base + reg;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline void rtl_lock(uint8_t *lock) {
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE)) __asm__ volatile("pause" ::: "memory");
}

static inline void rtl_unlock(uint8_t *lock) { __atomic_clear(lock, __ATOMIC_RELEASE); }

static uint32_t rtl_pci_read(pci_device_t *device, uint32_t offset) {
    if (device == NULL || device->op == NULL || device->op->read == NULL) return 0xffffffffU;
    return device->op->read(device->bus, device->slot, device->func, device->segment, offset & ~3U);
}

static void rtl_pci_enable(pci_device_t *device) {
    if (device == NULL || device->op == NULL || device->op->write == NULL) return;
    uint32_t command_status = rtl_pci_read(device, PCI_CONF_COMMAND);
    uint32_t command = (command_status & 0xffffU) | PCI_COMMAND_IO | PCI_COMMAND_MASTER;
    device->op->write(device->bus, device->slot, device->func, device->segment, PCI_CONF_COMMAND, command);
}

static void *rtl_dma_alloc(size_t bytes, uint64_t *phys_out) {
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = alloc_frames_dma32(pages);
    if (phys == 0) return NULL;
    void *virt = phys_to_virt(phys);
    memset(virt, 0, pages * PAGE_SIZE);
    if (phys_out != NULL) *phys_out = phys;
    return virt;
}

static void rtl_dma_free(void *virt, size_t bytes) {
    if (virt == NULL) return;
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    free_frames((uint64_t)virt_to_phys((uint64_t)virt), pages);
}

static bool rtl8139_wait_reset(rtl8139_device_t *dev) {
    for (uint32_t i = 0; i < 100000; ++i) {
        if ((rtl_in8(dev, RTL8139_REG_CHIP_CMD) & RTL8139_CMD_RESET) == 0) return true;
        __asm__ volatile("pause" ::: "memory");
    }
    return false;
}

static void rtl8139_reset(rtl8139_device_t *dev) {
    rtl_out8(dev, RTL8139_REG_CHIP_CMD, RTL8139_CMD_RESET);
    rtl8139_wait_reset(dev);
}

static void rtl8139_read_mac(rtl8139_device_t *dev) {
    for (uint32_t i = 0; i < sizeof(dev->mac); ++i) dev->mac[i] = rtl_in8(dev, RTL8139_REG_MAC0 + i);
}

static int rtl8139_init_device(rtl8139_device_t *dev) {
    rtl8139_reset(dev);
    if (rtl_in8(dev, RTL8139_REG_CHIP_CMD) & RTL8139_CMD_RESET) {
        printk("rtl8139: reset timeout at I/O 0x%x\n", dev->io_base);
        return -1;
    }

    /* Wake the PHY, unlock configuration, and use the classic 32 KiB ring. */
    rtl_out8(dev, RTL8139_REG_CONFIG1, 0x00);
    rtl_out8(dev, RTL8139_REG_CFG9346, 0xc0);
    rtl_out8(dev, RTL8139_REG_CHIP_CMD, 0x00);
    dev->rx_ring = (uint8_t *)rtl_dma_alloc(RTL8139_RX_ALLOC_SIZE, &dev->rx_ring_phys);
    if (dev->rx_ring == NULL || dev->rx_ring_phys > 0xffffffffULL) return -1;

    for (uint32_t i = 0; i < RTL8139_TX_DESC_COUNT; ++i) {
        dev->tx_buffers[i] = (uint8_t *)rtl_dma_alloc(RTL8139_TX_BUFFER_SIZE, &dev->tx_phys[i]);
        if (dev->tx_buffers[i] == NULL || dev->tx_phys[i] > 0xffffffffULL) return -1;
        rtl_out32(dev, RTL8139_REG_TX_ADDR0 + i * 4, (uint32_t)dev->tx_phys[i]);
        // Reset leaves OWN set. Writing a zero length here starts a bogus DMA.
    }

    rtl_out32(dev, RTL8139_REG_RX_BUF, (uint32_t)dev->rx_ring_phys);
    rtl_out16(dev, RTL8139_REG_RX_BUF_PTR, 0xfff0);
    rtl_out16(dev, RTL8139_REG_INTR_MASK, 0);
    rtl_out16(dev, RTL8139_REG_INTR_STATUS, 0xffff);
    // Use a circular DMA ring, matching rtl_ring_read() when a frame crosses its end.
    rtl_out32(dev, RTL8139_REG_MAR0, 0xffffffffU);
    rtl_out32(dev, RTL8139_REG_MAR0 + 4, 0xffffffffU);
    rtl_out32(dev, RTL8139_REG_RX_CONFIG,
              RTL8139_RX_CONFIG_RING_32K |
              RTL8139_RX_CONFIG_ACCEPT_BROADCAST | RTL8139_RX_CONFIG_ACCEPT_MULTICAST |
              RTL8139_RX_CONFIG_ACCEPT_PHYSICAL);
    rtl_out32(dev, RTL8139_REG_TX_CONFIG, 3U << 24);
    rtl_out8(dev, RTL8139_REG_CHIP_CMD, RTL8139_CMD_RX_ENABLE | RTL8139_CMD_TX_ENABLE);
    rtl_out8(dev, RTL8139_REG_CFG9346, 0x00);
    dev->rx_offset = 0;
    dev->tx_index = 0;
    rtl8139_read_mac(dev);
    return 0;
}

static int rtl8139_send(void *opaque, void *data, uint32_t len)
{
    rtl8139_device_t *dev = (rtl8139_device_t *)opaque;
    if (dev == NULL || data == NULL || len == 0 || len > RTL8139_TX_BUFFER_SIZE) return -1;
    rtl_lock(&dev->tx_lock);
    uint8_t index = dev->tx_index;
    uint32_t status = rtl_in32(dev, RTL8139_REG_TX_STATUS0 + index * 4);
    const uint32_t completed = RTL8139_TX_STATUS_OK | RTL8139_TX_STATUS_ABORTED | RTL8139_TX_STATUS_CARRIER_LOST;
    // TSD.OWN=1 returns the buffer to the host; zero means DMA still owns it.
    if ((status & RTL8139_TX_STATUS_HOST_OWNS) == 0 ||
        (dev->tx_pending[index] && (status & completed) == 0))
    {
        rtl_unlock(&dev->tx_lock);
        return -1;
    }
    memcpy(dev->tx_buffers[index], data, len);
    uint32_t wire_len = len < RTL8139_MIN_FRAME_SIZE ? RTL8139_MIN_FRAME_SIZE : len;
    if (wire_len > len) memset(dev->tx_buffers[index] + len, 0, wire_len - len);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    dev->tx_pending[index] = true;
    rtl_out32(dev, RTL8139_REG_TX_STATUS0 + index * 4, RTL8139_TX_FIFO_THRESHOLD | wire_len);
    dev->tx_index = (uint8_t)((index + 1) % RTL8139_TX_DESC_COUNT);
    uint64_t start = nanoTime();
    while (true) {
        status = rtl_in32(dev, RTL8139_REG_TX_STATUS0 + index * 4);
        // DMA completion alone does not imply that transmission succeeded.
        if ((status & completed) != 0) break;
        if (nanoTime() - start > 100000000ULL) {
            rtl_unlock(&dev->tx_lock);
            return -1;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    dev->tx_pending[index] = false;
    rtl_unlock(&dev->tx_lock);
    return (status & RTL8139_TX_STATUS_OK) != 0 &&
           (status & (RTL8139_TX_STATUS_ABORTED | RTL8139_TX_STATUS_CARRIER_LOST)) == 0 ? (int)len : -1;
}

static uint32_t rtl_ring_read(const rtl8139_device_t *dev, uint32_t offset, void *dst, uint32_t len) {
    uint32_t ring_size = RTL8139_RX_RING_SIZE;
    offset &= RTL8139_RX_RING_MASK;
    uint32_t first = ring_size - offset;
    if (first > len) first = len;
    memcpy(dst, dev->rx_ring + offset, first);
    if (first < len) memcpy((uint8_t *)dst + first, dev->rx_ring, len - first);
    return len;
}

static int rtl8139_receive(void *opaque, void *buffer, uint32_t buffer_size) {
    rtl8139_device_t *dev = (rtl8139_device_t *)opaque;
    if (dev == NULL || buffer == NULL || buffer_size == 0) return -1;
    rtl_lock(&dev->rx_lock);
    if ((rtl_in8(dev, RTL8139_REG_CHIP_CMD) & RTL8139_CMD_RX_EMPTY) != 0) {
        rtl_unlock(&dev->rx_lock);
        return 0;
    }

    uint32_t packet_offset = dev->rx_offset;
    uint8_t header[4];
    rtl_ring_read(dev, packet_offset, header, sizeof(header));
    uint16_t status = (uint16_t)(header[0] | ((uint16_t)header[1] << 8));
    uint16_t size = (uint16_t)(header[2] | ((uint16_t)header[3] << 8));
    // Early receive can expose the header before DMA has finished the frame.
    if (size == 0xfff0)
    {
        rtl_unlock(&dev->rx_lock);
        return 0;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint32_t payload = size >= 4 ? (uint32_t)size - 4 : 0;
    uint32_t copy_len = payload < buffer_size ? payload : buffer_size;
    const uint16_t rx_errors = RTL8139_RX_STATUS_CRC | RTL8139_RX_STATUS_ALIGN | RTL8139_RX_STATUS_TOO_LONG |
                               RTL8139_RX_STATUS_RUNT | RTL8139_RX_STATUS_BAD_SYMBOL;
    if (size < 4 || size > RTL8139_MAX_FRAME_SIZE + 4 ||
        (status & RTL8139_RX_STATUS_OK) == 0 || (status & rx_errors) != 0)
        copy_len = 0;
    if (copy_len != 0) rtl_ring_read(dev, packet_offset + 4, buffer, copy_len);

    uint32_t advance = size >= 4 && size <= RTL8139_MAX_FRAME_SIZE + 4 ? (uint32_t)size + 4 : 4;
    dev->rx_offset = ((packet_offset + advance + 3) & ~3U) & RTL8139_RX_RING_MASK;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    rtl_out16(dev, RTL8139_REG_RX_BUF_PTR, (uint16_t)(dev->rx_offset - 16));
    rtl_unlock(&dev->rx_lock);
    return (int)copy_len;
}

static void rtl8139_probe(pci_device_t *pci_dev) {
    if (pci_dev == NULL || pci_dev->vendor_id != RTL8139_VENDOR_ID || pci_dev->device_id != RTL8139_DEVICE_ID) return;
    if (pci_dev->bars[0].mmio || pci_dev->bars[0].address > 0xffff) {
        printk("rtl8139: unsupported BAR0 (requires I/O BAR)\n");
        return;
    }
    rtl_pci_enable(pci_dev);
    rtl8139_device_t *dev = (rtl8139_device_t *)calloc(1, sizeof(*dev));
    if (dev == NULL) return;
    dev->io_base = (uint16_t)pci_dev->bars[0].address;
    if (rtl8139_init_device(dev) != 0) {
        rtl_out8(dev, RTL8139_REG_CHIP_CMD, 0);
        rtl_dma_free(dev->rx_ring, RTL8139_RX_ALLOC_SIZE);
        for (uint32_t i = 0; i < RTL8139_TX_DESC_COUNT; ++i) rtl_dma_free(dev->tx_buffers[i], RTL8139_TX_BUFFER_SIZE);
        free(dev);
        return;
    }
    printk("rtl8139: %02x:%02x.%u MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           pci_dev->bus, pci_dev->slot, pci_dev->func, dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3],
           dev->mac[4], dev->mac[5]);
    if (g_rtl8139_device_count < 8) g_rtl8139_devices[g_rtl8139_device_count++] = dev;
    regist_netdev(dev, dev->mac, RTL8139_MTU, rtl8139_send, rtl8139_receive);
}

extern "C" __attribute__((used)) __attribute__((visibility("default"))) int dlmain(void) {
    pci_device_t *devices[PCI_DEVICE_MAX];
    uint32_t count = 0;
    pci_find_class_n(devices, &count, 0x020000);
    for (uint32_t i = 0; i < count; ++i) rtl8139_probe(devices[i]);
    return g_rtl8139_device_count != 0 ? 0 : -ENODEV;
}
