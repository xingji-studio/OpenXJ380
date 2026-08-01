#pragma once

#ifndef XHCI_BUILTIN
extern "C" {
#endif
#include "../../include/errno.h"
#include "../../include/device.h"
#include "../../include/krlibc.h"
#include "../../include/mm/page.h"
#include "../../include/pci/pci.h"
#include "../../include/proto.hpp"
#ifndef XHCI_BUILTIN
}
#endif

#include "../../include/task/pcb.h"
#include <stdint.h>

#define XHCI_PCI_CLASS 0x0C0330U
#define XHCI_MMIO_PTE_FLAGS (PTE_PRESENT | PTE_WRITEABLE | (1ULL << 3) | (1ULL << 4))

#define XHCI_RING_TRBS 256U
#define XHCI_CONTEXTS 32U

#define XHCI_CMD_RS (1U << 0)
#define XHCI_CMD_HCRST (1U << 1)
#define XHCI_CMD_INTE (1U << 2)

#define XHCI_STS_HCH (1U << 0)
#define XHCI_STS_EINT (1U << 3)
#define XHCI_STS_CNR (1U << 11)

#define XHCI_PORTSC_CCS (1U << 0)
#define XHCI_PORTSC_PED (1U << 1)
#define XHCI_PORTSC_OCA (1U << 3)
#define XHCI_PORTSC_PR (1U << 4)
#define XHCI_PORTSC_PLS_SHIFT 5
#define XHCI_PORTSC_PLS_MASK 0xFU
#define XHCI_PORTSC_PP (1U << 9)
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_SPEED_MASK 0xFU
#define XHCI_PORTSC_PIC_MASK (0x3U << 14)
#define XHCI_PORTSC_LWS (1U << 16)
#define XHCI_PORTSC_CSC (1U << 17)
#define XHCI_PORTSC_PEC (1U << 18)
#define XHCI_PORTSC_WRC (1U << 19)
#define XHCI_PORTSC_OCC (1U << 20)
#define XHCI_PORTSC_PRC (1U << 21)
#define XHCI_PORTSC_PLC (1U << 22)
#define XHCI_PORTSC_CEC (1U << 23)
#define XHCI_PORTSC_CAS (1U << 24)
#define XHCI_PORTSC_WAKE_MASK (0x7U << 25)
#define XHCI_PORTSC_DR (1U << 30)

#define XHCI_PORTSC_RO_BITS                                                  \
    (XHCI_PORTSC_CCS | XHCI_PORTSC_OCA |                                     \
     (XHCI_PORTSC_SPEED_MASK << XHCI_PORTSC_SPEED_SHIFT) | XHCI_PORTSC_DR)
#define XHCI_PORTSC_RWS_BITS                                                 \
    ((XHCI_PORTSC_PLS_MASK << XHCI_PORTSC_PLS_SHIFT) | XHCI_PORTSC_PP |      \
     XHCI_PORTSC_PIC_MASK | XHCI_PORTSC_WAKE_MASK)

#define XHCI_PORTSC_CHANGE_BITS                                                 \
    (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | XHCI_PORTSC_OCC |   \
     XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)

#define XHCI_PLS_U0 0U
#define XHCI_PLS_POLLING 7U

#define XHCI_HCS1_MAX_SLOTS_MASK 0xFFU
#define XHCI_HCS1_MAX_INTRS_SHIFT 8
#define XHCI_HCS1_MAX_INTRS_MASK 0x7FFU
#define XHCI_HCS1_MAX_PORTS_SHIFT 24
#define XHCI_HCS1_MAX_PORTS_MASK 0xFFU

#define XHCI_HCC_CSZ (1U << 2)

#define XHCI_PROTOCOL_CAP_ID 0x02U
#define XHCI_LEGACY_CAP_ID 0x01U
#define XHCI_PROTOCOL_USB_NAME 0x20425355U

#define XHCI_TRB_C (1U << 0)
#define XHCI_TRB_LINK_TC (1U << 1)
#define XHCI_TRB_ISP (1U << 2)
#define XHCI_TRB_IOC (1U << 5)
#define XHCI_TRB_IDT (1U << 6)
#define XHCI_TRB_BSR (1U << 9)
#define XHCI_TRB_DIR (1U << 16)

#define XHCI_TRB_TYPE_SHIFT 10
#define XHCI_TRB_TYPE_MASK 0x3FU

#define XHCI_TRB_SLOT_ID_SHIFT 24
#define XHCI_TRB_SLOT_ID_MASK 0xFFU
#define XHCI_TRB_EPID_SHIFT 16
#define XHCI_TRB_EPID_MASK 0x1FU

#define XHCI_TRB_INTR_TARGET_SHIFT 22
#define XHCI_TRB_INTR_TARGET_MASK 0x3FFU

#define XHCI_INPUT_ADD_SLOT (1U << 0)
#define XHCI_INPUT_ADD_EP0 (1U << 1)

#define XHCI_SLOT_CTX_HUB (1U << 26)
#define XHCI_SLOT_CTX_NUM_PORTS_SHIFT 24
#define XHCI_SLOT_SPEED_SHIFT 20
#define XHCI_SLOT_CTX_ENTRIES_SHIFT 27
#define XHCI_SLOT_RHPORT_SHIFT 16
#define XHCI_SLOT_TTT_SHIFT 16

#define XHCI_EP_CTX_CERR_SHIFT 1
#define XHCI_EP_CTX_TYPE_SHIFT 3
#define XHCI_EP_CTX_MAX_BURST_SHIFT 8
#define XHCI_EP_CTX_MAX_PACKET_SHIFT 16
#define XHCI_EP_TX_AVG_TRB_LENGTH_MASK 0xFFFFU
#define XHCI_EP_TX_MAX_ESIT_PAYLOAD_SHIFT 16

#define XHCI_EP_TYPE_CONTROL 4U
#define XHCI_EP_TYPE_BULK_OUT 2U
#define XHCI_EP_TYPE_BULK_IN 6U
#define XHCI_EP_TYPE_INTERRUPT_IN 7U

#define USB_DIR_OUT 0x00U
#define USB_DIR_IN 0x80U
#define USB_TYPE_STANDARD (0x00U << 5)
#define USB_TYPE_CLASS (0x01U << 5)
#define USB_RECIP_DEVICE 0x00U
#define USB_RECIP_INTERFACE 0x01U
#define USB_RECIP_ENDPOINT 0x02U
#define USB_RECIP_OTHER 0x03U

#define USB_REQ_GET_STATUS 0x00U
#define USB_REQ_CLEAR_FEATURE 0x01U
#define USB_REQ_SET_FEATURE 0x03U
#define USB_REQ_SET_ADDRESS 0x05U
#define USB_REQ_GET_DESCRIPTOR 0x06U
#define USB_REQ_SET_CONFIGURATION 0x09U
#define HID_REQ_SET_IDLE 0x0AU
#define HID_REQ_SET_PROTOCOL 0x0BU
#define USB_MSC_REQ_RESET 0xFFU
#define USB_MSC_REQ_GET_MAX_LUN 0xFEU

#define USB_DT_DEVICE 0x01U
#define USB_DT_CONFIG 0x02U
#define USB_DT_INTERFACE 0x04U
#define USB_DT_ENDPOINT 0x05U
#define USB_DT_SS_ENDPOINT_COMPANION 0x30U
#define USB_DT_HUB (USB_TYPE_CLASS | 0x09U)
#define USB_DT_HUB3 (USB_TYPE_CLASS | 0x0AU)

#define USB_CLASS_HID 0x03U
#define USB_CLASS_HUB 0x09U
#define USB_CLASS_MASS_STORAGE 0x08U

#define USB_INTERFACE_SUBCLASS_BOOT 0x01U
#define USB_INTERFACE_PROTOCOL_KEYBOARD 0x01U
#define USB_INTERFACE_PROTOCOL_MOUSE 0x02U
#define USB_MSC_SUBCLASS_SCSI 0x06U
#define USB_MSC_PROTOCOL_BBB 0x50U

#define USB_ENDPOINT_XFER_CONTROL 0x00U
#define USB_ENDPOINT_XFER_ISOC 0x01U
#define USB_ENDPOINT_XFER_BULK 0x02U
#define USB_ENDPOINT_XFER_INT 0x03U

#define USB_FEATURE_ENDPOINT_HALT 0x00U

#define HUB_REQ_SET_HUB_DEPTH 0x0CU

#define USB_PORT_FEAT_RESET 4U
#define USB_PORT_FEAT_ENABLE 1U
#define USB_PORT_FEAT_POWER 8U
#define USB_PORT_FEAT_C_CONNECTION 16U
#define USB_PORT_FEAT_C_ENABLE 17U
#define USB_PORT_FEAT_C_OVER_CURRENT 19U
#define USB_PORT_FEAT_C_RESET 20U

#define USB_PORT_STAT_CONNECTION 0x0001U
#define USB_PORT_STAT_ENABLE 0x0002U
#define USB_PORT_STAT_RESET 0x0010U
#define USB_PORT_STAT_POWER 0x0100U
#define USB_PORT_STAT_LOW_SPEED 0x0200U
#define USB_PORT_STAT_HIGH_SPEED 0x0400U
#define USB_PORT_STAT_LINK_SHIFT 5
#define USB_PORT_STAT_LINK_MASK (0x7U << USB_PORT_STAT_LINK_SHIFT)

#define USB_HUB_CHAR_TTT_SHIFT 5
#define USB_HUB_CHAR_TTT_MASK (0x3U << USB_HUB_CHAR_TTT_SHIFT)

#define XHCI_SPEED_FULL 1U
#define XHCI_SPEED_LOW 2U
#define XHCI_SPEED_HIGH 3U
#define XHCI_SPEED_SUPER 4U

#define XHCI_MSC_CBW_SIGNATURE 0x43425355U
#define XHCI_MSC_CSW_SIGNATURE 0x53425355U

#define SCSI_TEST_UNIT_READY 0x00U
#define SCSI_REQUEST_SENSE 0x03U
#define SCSI_INQUIRY 0x12U
#define SCSI_READ_CAPACITY_10 0x25U
#define SCSI_READ_10 0x28U
#define SCSI_WRITE_10 0x2AU
#define SCSI_SERVICE_ACTION_IN_16 0x9EU
#define SCSI_READ_16 0x88U
#define SCSI_WRITE_16 0x8AU

#ifdef __cplusplus
extern "C" {
#endif
void keyboard_push_input(uint8_t value);
void keyboard_usb_key_event(uint8_t usage, uint8_t value, uint8_t pressed);
void keyboard_dispatch_key_message(uint64_t msg_type, uint8_t value);
void mouse_inject_report(int dx, int dy, uint8_t buttons, int wheel);
#ifdef __cplusplus
}
#endif

static inline uint8_t xhci_endpoint_id_from_address(uint8_t endpoint_address) {
    if ((endpoint_address & 0x0FU) == 0) {
        return 1U;
    }

    uint8_t epid = (uint8_t)((endpoint_address & 0x0FU) << 1);
    return (endpoint_address & USB_DIR_IN) ? (uint8_t)(epid | 1U) : epid;
}

enum xhci_trb_type {
    XHCI_TRB_TYPE_RESERVED = 0,
    XHCI_TRB_TYPE_NORMAL = 1,
    XHCI_TRB_TYPE_SETUP = 2,
    XHCI_TRB_TYPE_DATA = 3,
    XHCI_TRB_TYPE_STATUS = 4,
    XHCI_TRB_TYPE_LINK = 6,
    XHCI_TRB_TYPE_ENABLE_SLOT_CMD = 9,
    XHCI_TRB_TYPE_DISABLE_SLOT_CMD = 10,
    XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD = 11,
    XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD = 12,
    XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD = 13,
    XHCI_TRB_TYPE_TRANSFER_EVENT = 32,
    XHCI_TRB_TYPE_COMMAND_COMPLETION_EVENT = 33,
    XHCI_TRB_TYPE_PORT_STATUS_EVENT = 34,
    XHCI_TRB_TYPE_HOST_CONTROLLER_EVENT = 37,
};

enum xhci_completion_code {
    XHCI_CC_INVALID = 0,
    XHCI_CC_SUCCESS = 1,
    XHCI_CC_DATA_BUFFER_ERROR = 2,
    XHCI_CC_BABBLE_DETECTED = 3,
    XHCI_CC_USB_TRANSACTION_ERROR = 4,
    XHCI_CC_TRB_ERROR = 5,
    XHCI_CC_STALL_ERROR = 6,
    XHCI_CC_RESOURCE_ERROR = 7,
    XHCI_CC_SHORT_PACKET = 13,
};

struct xhci_cap_regs {
    uint8_t cap_length;
    uint8_t reserved0;
    uint16_t hci_version;
    uint32_t hcs_params1;
    uint32_t hcs_params2;
    uint32_t hcs_params3;
    uint32_t hcc_params1;
    uint32_t db_offset;
    uint32_t runtime_offset;
} __attribute__((packed));

struct xhci_ext_cap {
    uint32_t cap;
    uint32_t data[2];
} __attribute__((packed));

struct xhci_op_regs {
    uint32_t usb_cmd;
    uint32_t usb_sts;
    uint32_t page_size;
    uint32_t reserved0[2];
    uint32_t dnctrl;
    uint32_t crcr_low;
    uint32_t crcr_high;
    uint32_t reserved1[4];
    uint32_t dcbaap_low;
    uint32_t dcbaap_high;
    uint32_t config;
} __attribute__((packed));

struct xhci_port_regs {
    uint32_t portsc;
    uint32_t portpmsc;
    uint32_t portli;
    uint32_t reserved0;
} __attribute__((packed));

struct xhci_db_reg {
    uint32_t doorbell;
} __attribute__((packed));

struct xhci_intr_regs {
    uint32_t iman;
    uint32_t imod;
    uint32_t erstsz;
    uint32_t reserved0;
    uint32_t erstba_low;
    uint32_t erstba_high;
    uint32_t erdp_low;
    uint32_t erdp_high;
} __attribute__((packed));

struct xhci_trb {
    uint32_t ptr_low;
    uint32_t ptr_high;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

struct xhci_erst_entry {
    uint32_t ptr_low;
    uint32_t ptr_high;
    uint32_t size;
    uint32_t reserved0;
} __attribute__((packed));

struct xhci_dcbaa_entry {
    uint32_t ptr_low;
    uint32_t ptr_high;
} __attribute__((packed));

struct xhci_input_control_ctx {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t reserved0[6];
} __attribute__((packed));

struct xhci_slot_ctx {
    uint32_t ctx[8];
} __attribute__((packed));

struct xhci_ep_ctx {
    uint32_t ctx[2];
    uint32_t deq_low;
    uint32_t deq_high;
    uint32_t tx_info;
    uint32_t reserved0[3];
} __attribute__((packed));

struct usb_setup_packet {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

struct usb_descriptor_header {
    uint8_t bLength;
    uint8_t bDescriptorType;
} __attribute__((packed));

struct usb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed));

struct usb_ss_ep_comp_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bMaxBurst;
    uint8_t bmAttributes;
    uint16_t wBytesPerInterval;
} __attribute__((packed));

struct usb_hub_descriptor_prefix {
    uint8_t bDescLength;
    uint8_t bDescriptorType;
    uint8_t bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPwrOn2PwrGood;
    uint8_t bHubContrCurrent;
} __attribute__((packed));

struct usb_port_status {
    uint16_t wPortStatus;
    uint16_t wPortChange;
} __attribute__((packed));

struct xhci_portmap {
    uint8_t start;
    uint8_t count;
};

struct xhci_ring {
    xhci_trb *trbs;
    uint64_t phys;
    uint32_t count;
    uint32_t enqueue_idx;
    uint32_t completion_idx;
    uint32_t wait_target_idx;
    uint32_t cycle_state;
    bool link_trb;
    bool target_waiting;
    xhci_trb event;
};

struct xhci_event_ring {
    xhci_trb *trbs;
    uint64_t phys;
    uint32_t count;
    uint32_t dequeue_idx;
    uint32_t cycle_state;
};

enum xhci_hid_kind {
    XHCI_HID_NONE = 0,
    XHCI_HID_KEYBOARD = 1,
    XHCI_HID_MOUSE = 2,
};

struct xhci_hid_state {
    bool supported;
    bool ready;
    bool pending;
    uint8_t kind;
    uint8_t interface_number;
    uint8_t endpoint_address;
    uint8_t endpoint_interval;
    uint8_t endpoint_epid;
    uint8_t interval_reg;
    uint8_t max_burst;
    uint16_t max_packet;
    uint16_t report_buffer_len;
    xhci_ring ring;
    void *report_buffer;
    uint64_t report_buffer_phys;
    uint8_t prev_report[8];
    bool caps_lock;
    uint8_t reserved0[6];
};

struct xhci_slot_state;
struct xhci_controller;
struct usb_device;
struct usb_hub;
struct usb_driver;

struct xhci_msc_state {
    bool supported;
    bool ready;
    bool use_read16;
    uint8_t interface_number;
    uint8_t max_lun;
    uint8_t bulk_in_address;
    uint8_t bulk_out_address;
    uint8_t bulk_in_epid;
    uint8_t bulk_out_epid;
    uint8_t bulk_in_max_burst;
    uint8_t bulk_out_max_burst;
    uint16_t bulk_in_max_packet;
    uint16_t bulk_out_max_packet;
    uint32_t block_size;
    uint32_t tag;
    uint64_t block_count;
    xhci_ring bulk_in_ring;
    xhci_ring bulk_out_ring;
    xhci_slot_state *slot;
    xhci_controller *controller;
    volatile uint32_t io_lock;
    int device_id;
};

struct xhci_msc_cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_length;
    uint8_t cdb[16];
} __attribute__((packed));

struct xhci_msc_csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_residue;
    uint8_t status;
} __attribute__((packed));

struct xhci_slot_state {
    bool used;
    bool configured;
    bool is_hub;
    bool hub_context_configured;
    uint8_t port_id;
    uint8_t parent_port;
    uint8_t speed_id;
    uint8_t level;
    uint8_t parent_slot_id;
    uint8_t config_value;
    uint8_t active_iface_class;
    uint8_t active_iface_subclass;
    uint8_t active_iface_protocol;
    uint8_t hub_port_count;
    uint32_t slot_id;
    uint32_t route_string;
    uint32_t tt_info;
    uint32_t child_tt_info_base;
    uint16_t max_packet0;
    uint16_t hub_characteristics;
    uint16_t config_length;
    uint16_t reserved0;
    usb_device *usbdev;
    xhci_ring ep0_ring;
    void *output_ctx;
    uint64_t output_ctx_phys;
    void *config_buffer;
    xhci_hid_state hid;
    xhci_msc_state msc;
    usb_device_descriptor device_desc;
    usb_config_descriptor config_desc;
};

struct xhci_controller {
    pci_device_t *pci_dev;
    xhci_controller *next;
    void *mmio;
    xhci_cap_regs *caps;
    xhci_op_regs *op;
    xhci_port_regs *ports;
    xhci_db_reg *db;
    xhci_intr_regs *ir0;

    uint32_t ext_caps_off;
    uint32_t num_ports;
    uint32_t num_slots;
    uint32_t num_irqs;
    uint32_t context_size;

    xhci_portmap usb2;
    xhci_portmap usb3;

    xhci_dcbaa_entry *dcbaa;
    uint64_t dcbaa_phys;

    xhci_ring cmd_ring;
    xhci_event_ring event_ring;
    xhci_erst_entry *erst;
    uint64_t erst_phys;

    uint64_t *scratchpad_array;
    uint64_t scratchpad_array_phys;
    void *scratchpad_pages;
    uint64_t scratchpad_pages_phys;
    uint32_t scratchpad_count;

    xhci_slot_state *slots;
    uint8_t *root_port_pending;
    volatile uint32_t event_lock;
    bool service_worker_started;
    bool hid_worker_started;
    bool hid_devices_present;
    usb_hub *root_hub;
    usb_device *root_usbdev;
};

#define USB_CORE_MAX_IFACES 8
#define USB_CORE_MAX_ENDPOINTS 8
#define USB_DRIVER_MATCH_VENDOR 0x0001U
#define USB_DRIVER_MATCH_PRODUCT 0x0002U
#define USB_DRIVER_MATCH_IFACE_CLASS 0x0004U
#define USB_DRIVER_MATCH_IFACE_SUBCLASS 0x0008U
#define USB_DRIVER_MATCH_IFACE_PROTOCOL 0x0010U

struct usb_endpoint_info {
    uint8_t address;
    uint8_t attributes;
    uint8_t interval;
    uint8_t max_burst;
    uint16_t max_packet;
};

struct usb_interface_info {
    uint8_t number;
    uint8_t alt_setting;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t endpoint_count;
    usb_endpoint_info endpoints[USB_CORE_MAX_ENDPOINTS];
};

struct usb_driver_id {
    uint16_t match_flags;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
};

struct usb_hub_ops {
    int (*detect)(usb_hub *hub, uint8_t port, usb_port_status *status);
    int (*reset)(usb_hub *hub, uint8_t port, uint8_t *speed_out);
    void (*disconnect)(usb_hub *hub, uint8_t port);
    void (*ack)(usb_hub *hub, uint8_t port, const usb_port_status *status);
};

struct usb_hub {
    xhci_controller *controller;
    usb_device *usbdev;
    const usb_hub_ops *ops;
    uint8_t port_count;
    bool registered;
    bool needs_rescan;
    usb_device **children;
    uint8_t *port_changed;
    usb_hub *next;
};

struct usb_device {
    xhci_controller *controller;
    xhci_slot_state *slot;
    usb_hub *parent_hub;
    usb_hub *child_hub;
    bool is_root_hub;
    bool online;
    uint8_t port;
    uint8_t speed_id;
    uint8_t level;
    uint8_t config_value;
    uint8_t interface_count;
    uint16_t vendor_id;
    uint16_t product_id;
    void *driver_data;
    usb_driver *bound_drivers[4];
    uint8_t bound_driver_count;
    char topology[64];
    usb_device_descriptor device_desc;
    usb_config_descriptor config_desc;
    void *config_buffer;
    uint16_t config_length;
    usb_interface_info interfaces[USB_CORE_MAX_IFACES];
};

struct usb_driver {
    const char *name;
    int priority;
    const usb_driver_id *id_table;
    int (*probe)(usb_device *usbdev, const usb_interface_info *iface);
    void (*remove)(usb_device *usbdev);
    usb_driver *next;
};

void usb_core_init(void);
void usb_core_start_workers(void);
void usb_register_driver(usb_driver *driver);
void usb_register_hub(usb_hub *hub);
void usb_hub_mark_port_changed(usb_hub *hub, uint32_t port_idx);
void usb_scan_all_hubs(void);
void usb_fill_topology(usb_device *usbdev);
int usb_parse_configuration(usb_device *usbdev);
int usb_probe_device(usb_device *usbdev);
void usb_disconnect_device(usb_device *usbdev);
void usb_register_builtin_hub_driver(void);
void usb_register_builtin_msc_driver(void);
int usb_fallback_probe_hub(usb_device *usbdev);
int usb_fallback_probe_msc(usb_device *usbdev);

int xhci_usb_enumerate_device(xhci_controller *xhci, usb_hub *parent_hub,
                              uint8_t parent_port, uint8_t speed_id,
                              usb_device **device_out);
void xhci_usb_release_device(usb_device *usbdev);

int xhci_control_request(xhci_controller *xhci, xhci_slot_state *slot,
                         uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index, void *data,
                         uint16_t length, uint32_t *actual_len);
int xhci_set_configuration(xhci_controller *xhci, xhci_slot_state *slot,
                           uint8_t config_value);
int xhci_configure_msc_endpoints(xhci_controller *xhci,
                                 xhci_slot_state *slot);
void xhci_release_msc(xhci_slot_state *slot);
int xhci_msc_scsi_test_unit_ready(xhci_controller *xhci,
                                  xhci_slot_state *slot);
int xhci_msc_scsi_request_sense(xhci_controller *xhci,
                                xhci_slot_state *slot, void *data,
                                uint8_t alloc_len);
int xhci_msc_scsi_inquiry(xhci_controller *xhci, xhci_slot_state *slot,
                          void *data, uint8_t alloc_len);
int xhci_msc_scsi_read_capacity10(xhci_controller *xhci,
                                  xhci_slot_state *slot, void *data);
int xhci_msc_scsi_read_capacity16(xhci_controller *xhci,
                                  xhci_slot_state *slot, void *data);
int xhci_msc_scsi_rw(xhci_controller *xhci, xhci_slot_state *slot,
                     void *buffer, uint64_t lba, uint32_t blocks, bool write);

int xhci_hub_get_descriptor(xhci_controller *xhci, xhci_slot_state *slot,
                            usb_hub_descriptor_prefix *desc);
int xhci_hub_set_depth(xhci_controller *xhci, xhci_slot_state *slot);
int xhci_hub_set_port_feature(xhci_controller *xhci, xhci_slot_state *slot,
                              uint8_t port, uint16_t feature);
int xhci_hub_clear_port_feature(xhci_controller *xhci, xhci_slot_state *slot,
                                uint8_t port, uint16_t feature);
int xhci_hub_get_port_status(xhci_controller *xhci, xhci_slot_state *slot,
                             uint8_t port, usb_port_status *status);
void xhci_hub_ack_port_change(xhci_controller *xhci, xhci_slot_state *slot,
                              uint8_t port, const usb_port_status *status);
int xhci_hub_detect_port(xhci_controller *xhci, xhci_slot_state *slot,
                         uint8_t port, usb_port_status *status);
int xhci_hub_reset_port(xhci_controller *xhci, xhci_slot_state *slot,
                        uint8_t port, uint8_t *speed_out);
int xhci_setup_hub(xhci_controller *xhci, xhci_slot_state *slot);
int xhci_setup(void);
void xhci_start_workers(void);
