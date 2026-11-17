#pragma once

#include <stdint.h>

#define VIRTIO_VENDOR_ID 0x1af4
#define VIRTIO_NET_DEVICE_ID 1
#define VIRTIO_NET_LEGACY_DEVICE_ID 0x1000
#define VIRTIO_NET_MTU 1500
#define VIRTIO_NET_MAX_QUEUE_SIZE 32768
#define VIRTIO_NET_BUFFER_SIZE 2048
#define VIRTIO_NET_QUEUE_ALIGN 4096

#define VIRTIO_PCI_HOST_FEATURES 0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_PFN 0x08
#define VIRTIO_PCI_QUEUE_NUM 0x0c
#define VIRTIO_PCI_QUEUE_SEL 0x0e
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10
#define VIRTIO_PCI_STATUS 0x12
#define VIRTIO_PCI_ISR 0x13
#define VIRTIO_PCI_CONFIG 0x14

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER 0x02
#define VIRTIO_STATUS_DRIVER_OK 0x04
#define VIRTIO_STATUS_FAILED 0x80

#define VIRTIO_NET_F_MAC (1U << 5)
#define VIRTIO_NET_F_STATUS (1U << 16)
#define VIRTIO_RING_F_INDIRECT_DESC (1U << 28)
#define VIRTIO_RING_F_EVENT_IDX (1U << 29)
#define VIRTIO_NET_F_CSUM (1U << 0)
#define VIRTQ_DESC_F_WRITE 2
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1

typedef struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed)) virtio_net_hdr_t;

// Legacy transport without VIRTIO_NET_F_MRG_RXBUF has a ten-byte header.
static_assert(sizeof(virtio_net_hdr_t) == 10, "legacy VirtIO network header size");

typedef struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} virtq_avail_t;

typedef struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
} virtq_used_t;

static_assert(sizeof(virtq_desc_t) == 16 && sizeof(virtq_used_elem_t) == 8, "VirtIO split-ring element sizes");

typedef struct virtio_net_queue {
    virtq_desc_t *desc;
    volatile virtq_avail_t *avail;
    volatile virtq_used_t *used;
    uint64_t phys;
    uint64_t buffer_phys;
    uint8_t *buffers;
    uint8_t *pending;
    uint32_t ring_bytes;
    uint32_t buffer_bytes;
    uint16_t last_used;
    uint16_t queue_size;
    uint16_t available;
    uint16_t next_slot;
    uint16_t in_flight;
    uint8_t lock;
    uint8_t broken;
} virtio_net_queue_t;

typedef struct virtio_net_device {
    uint16_t io_base;
    uint8_t mac[6];
    uint32_t host_features;
    virtio_net_queue_t rx;
    virtio_net_queue_t tx;
} virtio_net_device_t;
