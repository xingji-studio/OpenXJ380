#include "virtio_net.h"

extern "C" {
#include "../../include/netdev.h"
#include "../../include/pci/pci.h"
#include "../../include/proto.hpp"
}

static virtio_net_device_t *g_virtio_net_devices[8];
static uint32_t g_virtio_net_device_count;

static inline void vio_out8(virtio_net_device_t *dev, uint16_t reg, uint8_t value) {
    uint16_t port = dev->io_base + reg;
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void vio_out16(virtio_net_device_t *dev, uint16_t reg, uint16_t value) {
    uint16_t port = dev->io_base + reg;
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void vio_out32(virtio_net_device_t *dev, uint16_t reg, uint32_t value) {
    uint16_t port = dev->io_base + reg;
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t vio_in8(virtio_net_device_t *dev, uint16_t reg) {
    uint8_t value;
    uint16_t port = dev->io_base + reg;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline uint16_t vio_in16(virtio_net_device_t *dev, uint16_t reg) {
    uint16_t value;
    uint16_t port = dev->io_base + reg;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline uint32_t vio_in32(virtio_net_device_t *dev, uint16_t reg) {
    uint32_t value;
    uint16_t port = dev->io_base + reg;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline void vio_lock(uint8_t *lock)
{
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE)) __asm__ volatile("pause" ::: "memory");
}

static inline void vio_unlock(uint8_t *lock)
{
    __atomic_clear(lock, __ATOMIC_RELEASE);
}

static void *vio_dma_alloc(size_t bytes, uint64_t *phys_out)
{
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = alloc_frames_dma32(pages);
    if (phys == 0) return NULL;
    if (phys > 0xffffffffULL || pages * PAGE_SIZE > 0x100000000ULL - phys)
    {
        free_frames(phys, pages);
        return NULL;
    }
    void *virt = phys_to_virt(phys);
    memset(virt, 0, pages * PAGE_SIZE);
    *phys_out = phys;
    return virt;
}

// Only call after resetting the device, or before publishing a queue PFN.
static void virtio_free_queue(virtio_net_queue_t *queue)
{
    if (queue->phys) free_frames(queue->phys, (queue->ring_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    if (queue->buffer_phys) free_frames(queue->buffer_phys, (queue->buffer_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    free(queue->pending);
    *queue = {};
}

static bool virtio_setup_queue(virtio_net_device_t *dev, virtio_net_queue_t *queue, uint16_t index)
{
    vio_out16(dev, VIRTIO_PCI_QUEUE_SEL, index);
    uint16_t size = vio_in16(dev, VIRTIO_PCI_QUEUE_NUM);
    // PCI legacy QueueNum is read-only: layout and storage must use its full
    // advertised size, including the unused legacy event-index tail fields.
    if (size == 0 || size > VIRTIO_NET_MAX_QUEUE_SIZE || (size & (size - 1)) != 0 ||
        vio_in32(dev, VIRTIO_PCI_QUEUE_PFN) != 0)
        return false;
    queue->queue_size = size;
    size_t desc_bytes = sizeof(virtq_desc_t) * size;
    size_t avail_bytes = sizeof(uint16_t) * (3 + size);
    size_t used_offset = (desc_bytes + avail_bytes + VIRTIO_NET_QUEUE_ALIGN - 1) &
                         ~(size_t)(VIRTIO_NET_QUEUE_ALIGN - 1);
    queue->ring_bytes = used_offset + sizeof(uint16_t) * 3 + sizeof(virtq_used_elem_t) * size;
    queue->buffer_bytes = (uint32_t)size * VIRTIO_NET_BUFFER_SIZE;
    auto *memory = (uint8_t *)vio_dma_alloc(queue->ring_bytes, &queue->phys);
    queue->buffers = (uint8_t *)vio_dma_alloc(queue->buffer_bytes, &queue->buffer_phys);
    if (index == 1) queue->pending = (uint8_t *)calloc(size, 1);
    if (memory == NULL || queue->buffers == NULL || (index == 1 && queue->pending == NULL))
    {
        virtio_free_queue(queue);
        return false;
    }
    queue->desc = (virtq_desc_t *)memory;
    queue->avail = (volatile virtq_avail_t *)(memory + desc_bytes);
    queue->used = (volatile virtq_used_t *)(memory + used_offset);
    queue->avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
    for (uint16_t i = 0; i < size; ++i)
    {
        queue->desc[i].addr = queue->buffer_phys + (uint64_t)i * VIRTIO_NET_BUFFER_SIZE;
        queue->desc[i].len = VIRTIO_NET_BUFFER_SIZE;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    vio_out32(dev, VIRTIO_PCI_QUEUE_PFN, (uint32_t)(queue->phys >> 12));
    return true;
}

static void virtio_publish(virtio_net_queue_t *queue, uint16_t slot)
{
    queue->avail->ring[queue->available % queue->queue_size] = slot;
    __atomic_store_n(&queue->avail->idx, ++queue->available, __ATOMIC_RELEASE);
    // Make the index visible to DMA before notifying the device through I/O.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static int virtio_net_init(virtio_net_device_t *dev)
{
    vio_out8(dev, VIRTIO_PCI_STATUS, 0);
    for (unsigned i = 0; vio_in8(dev, VIRTIO_PCI_STATUS) != 0; ++i)
    {
        if (i == 10000) return -1;
        __asm__ volatile("pause" ::: "memory");
    }
    vio_out8(dev, VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    vio_out8(dev, VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    dev->host_features = vio_in32(dev, VIRTIO_PCI_HOST_FEATURES);
    uint32_t guest_features = dev->host_features & VIRTIO_NET_F_MAC;
    vio_out32(dev, VIRTIO_PCI_GUEST_FEATURES, guest_features);
    if (!virtio_setup_queue(dev, &dev->rx, 0) || !virtio_setup_queue(dev, &dev->tx, 1))
    {
        // In particular, detach the already configured RX queue if TX setup fails.
        vio_out8(dev, VIRTIO_PCI_STATUS, 0);
        virtio_free_queue(&dev->rx);
        virtio_free_queue(&dev->tx);
        return -1;
    }

    for (uint16_t i = 0; i < dev->rx.queue_size; ++i)
    {
        dev->rx.desc[i].flags = VIRTQ_DESC_F_WRITE;
        dev->rx.avail->ring[i] = i;
    }
    dev->rx.available = dev->rx.queue_size;
    __atomic_store_n(&dev->rx.avail->idx, dev->rx.available, __ATOMIC_RELEASE);
    if ((dev->host_features & VIRTIO_NET_F_MAC) != 0)
    {
        for (uint32_t i = 0; i < sizeof(dev->mac); ++i) dev->mac[i] = vio_in8(dev, VIRTIO_PCI_CONFIG + i);
    }
    else
    {
        dev->mac[0] = 0x02; dev->mac[1] = 0x58; dev->mac[2] = 0x4a;
        dev->mac[3] = (uint8_t)dev->io_base; dev->mac[4] = (uint8_t)(dev->io_base >> 8); dev->mac[5] = 1;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    vio_out8(dev, VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    // A device may ignore notifications issued before DRIVER_OK.
    vio_out16(dev, VIRTIO_PCI_QUEUE_NOTIFY, 0);
    return 0;
}

static bool virtio_reap_tx(virtio_net_queue_t *queue)
{
    uint16_t used = __atomic_load_n(&queue->used->idx, __ATOMIC_ACQUIRE);
    if ((uint16_t)(used - queue->last_used) > queue->in_flight) return false;
    while (queue->last_used != used)
    {
        uint32_t slot = queue->used->ring[queue->last_used % queue->queue_size].id;
        if (slot >= queue->queue_size || queue->pending[slot] == 0) return false;
        queue->pending[slot] = 0;
        --queue->in_flight;
        ++queue->last_used;
    }
    return true;
}

static int virtio_net_send(void *opaque, void *data, uint32_t len)
{
    auto *dev = (virtio_net_device_t *)opaque;
    if (dev == NULL || data == NULL || len == 0 || len > VIRTIO_NET_BUFFER_SIZE - sizeof(virtio_net_hdr_t)) return -1;
    auto *queue = &dev->tx;
    vio_lock(&queue->lock);
    if (queue->broken || !virtio_reap_tx(queue))
    {
        queue->broken = 1;
        vio_unlock(&queue->lock);
        return -1;
    }
    if (queue->in_flight == queue->queue_size)
    {
        vio_unlock(&queue->lock);
        return -1;
    }
    uint16_t slot = queue->next_slot;
    while (queue->pending[slot]) slot = (slot + 1) % queue->queue_size;
    queue->next_slot = (slot + 1) % queue->queue_size;
    uint8_t *buffer = queue->buffers + (size_t)slot * VIRTIO_NET_BUFFER_SIZE;
    memset(buffer, 0, sizeof(virtio_net_hdr_t));
    memcpy(buffer + sizeof(virtio_net_hdr_t), data, len);
    queue->desc[slot].len = sizeof(virtio_net_hdr_t) + len;
    queue->desc[slot].flags = 0;
    queue->pending[slot] = 1;
    ++queue->in_flight;
    virtio_publish(queue, slot);
    vio_out16(dev, VIRTIO_PCI_QUEUE_NOTIFY, 1);
    vio_unlock(&queue->lock);
    // The callback reports acceptance into the TX ring. Completion reclaims
    // the buffer later; no timeout can cause reuse while DMA still owns it.
    return (int)len;
}

static int virtio_net_receive(void *opaque, void *buffer, uint32_t buffer_size)
{
    auto *dev = (virtio_net_device_t *)opaque;
    if (dev == NULL || buffer == NULL || buffer_size == 0) return -1;
    auto *queue = &dev->rx;
    vio_lock(&queue->lock);
    uint16_t used = __atomic_load_n(&queue->used->idx, __ATOMIC_ACQUIRE);
    if (queue->broken || (uint16_t)(used - queue->last_used) > queue->queue_size)
    {
        queue->broken = 1;
        vio_unlock(&queue->lock);
        return -1;
    }
    if (queue->last_used == used)
    {
        vio_unlock(&queue->lock);
        return 0;
    }
    uint32_t slot = queue->used->ring[queue->last_used % queue->queue_size].id;
    uint32_t len = queue->used->ring[queue->last_used % queue->queue_size].len;
    if (slot >= queue->queue_size)
    {
        queue->broken = 1;
        vio_unlock(&queue->lock);
        return -1;
    }
    uint32_t payload = len >= sizeof(virtio_net_hdr_t) && len <= VIRTIO_NET_BUFFER_SIZE ?
                           len - sizeof(virtio_net_hdr_t) : 0;
    if (payload > buffer_size) payload = buffer_size;
    memcpy(buffer, queue->buffers + (size_t)slot * VIRTIO_NET_BUFFER_SIZE + sizeof(virtio_net_hdr_t), payload);
    ++queue->last_used;
    virtio_publish(queue, (uint16_t)slot);
    vio_out16(dev, VIRTIO_PCI_QUEUE_NOTIFY, 0);
    vio_unlock(&queue->lock);
    return (int)payload;
}

static void virtio_net_probe(pci_device_t *pci_dev) {
    if (pci_dev == NULL || pci_dev->vendor_id != VIRTIO_VENDOR_ID ||
        (pci_dev->device_id != VIRTIO_NET_LEGACY_DEVICE_ID &&
         pci_dev->device_id != VIRTIO_NET_DEVICE_ID + 0x1040))
        return;
    if (pci_dev->bars[0].mmio || pci_dev->bars[0].address > 0xffff) {
        printk("virtio-net: unsupported BAR0 (requires legacy I/O transport)\n");
        return;
    }
    uint32_t command = pci_read_command(pci_dev, PCI_CONF_COMMAND);
    pci_write_command(pci_dev, PCI_CONF_COMMAND,
                      command | PCI_COMMAND_IO | PCI_COMMAND_MASTER | PCI_RCMD_DISABLE_INTR);
    virtio_net_device_t *dev = (virtio_net_device_t *)calloc(1, sizeof(*dev));
    if (dev == NULL) return;
    dev->io_base = (uint16_t)pci_dev->bars[0].address;
    if (virtio_net_init(dev) != 0) {
        vio_out8(dev, VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        free(dev);
        return;
    }
    printk("virtio-net: legacy queues rx=%u tx=%u header=%u\n",
           (unsigned)dev->rx.queue_size, (unsigned)dev->tx.queue_size, (unsigned)sizeof(virtio_net_hdr_t));
    printk("virtio-net: %02x:%02x.%u MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           pci_dev->bus, pci_dev->slot, pci_dev->func, dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3],
           dev->mac[4], dev->mac[5]);
    if (g_virtio_net_device_count < 8) g_virtio_net_devices[g_virtio_net_device_count++] = dev;
    regist_netdev(dev, dev->mac, VIRTIO_NET_MTU, virtio_net_send, virtio_net_receive);
}

extern "C" __attribute__((used)) __attribute__((visibility("default"))) int dlmain(void) {
    pci_device_t *devices[PCI_DEVICE_MAX];
    uint32_t count = 0;
    pci_find_class_n(devices, &count, 0x020000);
    for (uint32_t i = 0; i < count; ++i) virtio_net_probe(devices[i]);
    return g_virtio_net_device_count != 0 ? 0 : -ENODEV;
}
