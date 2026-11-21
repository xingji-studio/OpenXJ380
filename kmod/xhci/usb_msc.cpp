#include "xhci.h"
#include <ioctl.h>
#ifdef XHCI_BUILTIN
#    include <block_source.h>
#    include <efi/boot.h>
extern BOOT_CONFIG *EFI_BC;
#endif
extern "C" {
#include "../../include/fs/partition.h"
}

static xhci_msc_state *g_usb_msc_by_drive[256];
static uint32_t g_usb_msc_count = 0;
static unsigned char   g_usb_msc_lock;

// Only pointer capture/publication is locked. Transfers, device-manager calls
// and buffer teardown run with the caller's interrupt/scheduler state intact.
class UsbMscRegistryLock {
    uint64_t flags;

  public:
    UsbMscRegistryLock()
    {
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
        while (__atomic_test_and_set(&g_usb_msc_lock, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause");
    }
    ~UsbMscRegistryLock()
    {
        __atomic_clear(&g_usb_msc_lock, __ATOMIC_RELEASE);
        __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
    }
};

#ifdef XHCI_BUILTIN
static bool usb_msc_source_hardware(const usb_device *device, BlockSourceHardware &hardware)
{
    hardware            = {};
    hardware.controller = device->controller->pci_dev;
    hardware.transport  = XJ_INSTALLER_SOURCE_USB;
    hardware.block_size = device->slot->msc.block_size;
    hardware.bytes      = device->slot->msc.block_count * hardware.block_size;
    hardware.read_only  = device->slot->msc.hardware_read_only;
    uint8_t  ports[XJ_INSTALLER_USB_DEPTH_MAX], interfaces[XJ_INSTALLER_USB_DEPTH_MAX];
    unsigned count     = 0;
    uint8_t  interface = device->slot->msc.interface_number;
    while (device && !device->is_root_hub)
    {
        if (!device->parent_hub || !device->port || count == XJ_INSTALLER_USB_DEPTH_MAX) return false;
        ports[count]        = device->port - 1;
        interfaces[count++] = interface;
        interface           = device->parent_hub->interface_number;
        device              = device->parent_hub->usbdev;
    }
    if (!device || !count) return false;
    hardware.usb_depth = count;
    for (unsigned i = 0; i < count; ++i)
    {
        hardware.usb_ports[i]      = ports[count - i - 1];
        hardware.usb_interfaces[i] = interfaces[count - i - 1];
    }
    return true;
}
#endif

static bool usb_msc_iface_is_bot(const usb_interface_info *iface) {
    return iface &&
           iface->interface_class == USB_CLASS_MASS_STORAGE &&
           iface->interface_subclass == USB_MSC_SUBCLASS_SCSI &&
           iface->interface_protocol == USB_MSC_PROTOCOL_BBB;
}

static void usb_msc_bind_driver(usb_device *usbdev, usb_driver *driver) {
    if (!usbdev || !driver) {
        return;
    }
    if (usbdev->bound_driver_count >=
        (sizeof(usbdev->bound_drivers) / sizeof(usbdev->bound_drivers[0]))) {
        return;
    }
    usbdev->bound_drivers[usbdev->bound_driver_count++] = driver;
}

static uint32_t usb_msc_get_be32(const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t usb_msc_get_be64(const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static void usb_msc_ascii_field(char *dst, size_t dst_len, const uint8_t *src,
                                size_t src_len) {
    if (!dst || dst_len == 0) {
        return;
    }

    size_t end = src_len;
    while (end > 0 && src[end - 1] == ' ') {
        --end;
    }

    size_t out = 0;
    for (size_t i = 0; i < end && out + 1 < dst_len; ++i) {
        uint8_t ch = src[i];
        dst[out++] = (ch >= 0x20U && ch < 0x7FU) ? (char)ch : '?';
    }
    dst[out] = '\0';
}

static bool usb_msc_extract_iface_from_config(usb_device *usbdev,
                                              usb_interface_info *iface_out) {
    if (!usbdev || !iface_out || !usbdev->config_buffer ||
        usbdev->config_length < sizeof(usb_config_descriptor)) {
        return false;
    }

    memset(iface_out, 0, sizeof(*iface_out));

    uint8_t *ptr = (uint8_t *)usbdev->config_buffer;
    uint8_t *end = ptr + usbdev->config_length;
    bool matched = false;
    usb_endpoint_info *last_ep = NULL;

    while (ptr + sizeof(usb_descriptor_header) <= end) {
        usb_descriptor_header *hdr = (usb_descriptor_header *)ptr;
        if (hdr->bLength == 0 || ptr + hdr->bLength > end) {
            break;
        }

        if (hdr->bDescriptorType == USB_DT_INTERFACE &&
            hdr->bLength >= sizeof(usb_interface_descriptor)) {
            usb_interface_descriptor *iface = (usb_interface_descriptor *)ptr;
            matched = iface->bAlternateSetting == 0 &&
                      iface->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                      iface->bInterfaceSubClass == USB_MSC_SUBCLASS_SCSI &&
                      iface->bInterfaceProtocol == USB_MSC_PROTOCOL_BBB;

            if (matched) {
                memset(iface_out, 0, sizeof(*iface_out));
                iface_out->number = iface->bInterfaceNumber;
                iface_out->alt_setting = iface->bAlternateSetting;
                iface_out->interface_class = iface->bInterfaceClass;
                iface_out->interface_subclass = iface->bInterfaceSubClass;
                iface_out->interface_protocol = iface->bInterfaceProtocol;
            }
            last_ep = NULL;
        } else if (matched && hdr->bDescriptorType == USB_DT_ENDPOINT &&
                   hdr->bLength >= sizeof(usb_endpoint_descriptor)) {
            if (iface_out->endpoint_count >= USB_CORE_MAX_ENDPOINTS) {
                ptr += hdr->bLength;
                continue;
            }

            usb_endpoint_descriptor *ep = (usb_endpoint_descriptor *)ptr;
            last_ep = &iface_out->endpoints[iface_out->endpoint_count++];
            memset(last_ep, 0, sizeof(*last_ep));
            last_ep->address = ep->bEndpointAddress;
            last_ep->attributes = ep->bmAttributes;
            last_ep->interval = ep->bInterval;
            last_ep->max_packet = (uint16_t)(ep->wMaxPacketSize & 0x07FFU);
        } else if (matched && last_ep &&
                   hdr->bDescriptorType == USB_DT_SS_ENDPOINT_COMPANION &&
                   hdr->bLength >= sizeof(usb_ss_ep_comp_descriptor)) {
            usb_ss_ep_comp_descriptor *comp =
                (usb_ss_ep_comp_descriptor *)ptr;
            last_ep->max_burst = comp->bMaxBurst;
        } else if (matched && hdr->bDescriptorType == USB_DT_INTERFACE) {
            break;
        }

        ptr += hdr->bLength;
    }

    return matched && iface_out->endpoint_count != 0;
}

static bool usb_msc_fill_endpoints(xhci_slot_state *slot,
                                   const usb_interface_info *iface) {
    if (!slot || !iface) {
        return false;
    }

    memset(&slot->msc, 0, sizeof(slot->msc));
    slot->msc.device_id = -1;
    slot->msc.interface_number = iface->number;

    for (uint8_t i = 0; i < iface->endpoint_count; ++i) {
        const usb_endpoint_info *ep = &iface->endpoints[i];
        if ((ep->attributes & 0x03U) != USB_ENDPOINT_XFER_BULK ||
            ep->max_packet == 0) {
            continue;
        }

        if ((ep->address & USB_DIR_IN) != 0 && slot->msc.bulk_in_epid == 0) {
            slot->msc.bulk_in_address = ep->address;
            slot->msc.bulk_in_epid = xhci_endpoint_id_from_address(ep->address);
            slot->msc.bulk_in_max_packet = ep->max_packet;
            slot->msc.bulk_in_max_burst = ep->max_burst;
        } else if ((ep->address & USB_DIR_IN) == 0 &&
                   slot->msc.bulk_out_epid == 0) {
            slot->msc.bulk_out_address = ep->address;
            slot->msc.bulk_out_epid =
                xhci_endpoint_id_from_address(ep->address);
            slot->msc.bulk_out_max_packet = ep->max_packet;
            slot->msc.bulk_out_max_burst = ep->max_burst;
        }
    }

    slot->msc.supported = slot->msc.bulk_in_epid != 0 &&
                          slot->msc.bulk_out_epid != 0 &&
                          slot->msc.bulk_in_max_packet != 0 &&
                          slot->msc.bulk_out_max_packet != 0;
    return slot->msc.supported;
}

static size_t usb_msc_device_rw(int drive, uint8_t *buffer, size_t number,
                                size_t lba, bool write) {
    if (drive < 0 || drive >= 256 || !buffer || number == 0) {
        return 0;
    }

    xhci_msc_state *msc;
    {
        UsbMscRegistryLock lock;
        msc = g_usb_msc_by_drive[drive];
        if (!msc || !msc->ready || msc->disconnected || !msc->slot || !msc->controller || !msc->block_size ||
            (write && (msc->hardware_read_only || msc->boot_source_read_only)))
            return 0;
        __atomic_add_fetch(&msc->io_users, 1U, __ATOMIC_ACQ_REL);
    }

    while (__sync_lock_test_and_set(&msc->io_lock, 1U) != 0U) {
        __asm__ volatile("pause" ::: "memory");
    }

    size_t transferred = 0;
    uint8_t *ptr = buffer;
    uint64_t cur_lba = lba;
    size_t remaining = number;

    while (remaining != 0) {
        if (__atomic_load_n(&msc->disconnected, __ATOMIC_ACQUIRE)) break;
        uint32_t chunk = (remaining > 0xFFFFU) ? 0xFFFFU : (uint32_t)remaining;
        if (cur_lba + chunk > msc->block_count) {
            break;
        }

        if (xhci_msc_scsi_rw(msc->controller, msc->slot, ptr, cur_lba, chunk,
                             write) != 0) {
            break;
        }

        transferred += chunk;
        remaining -= chunk;
        cur_lba += chunk;
        ptr += (uint64_t)chunk * msc->block_size;
    }

    __sync_lock_release(&msc->io_lock);
    __atomic_sub_fetch(&msc->io_users, 1U, __ATOMIC_RELEASE);
    return transferred;
}

static size_t usb_msc_device_read(int drive, uint8_t *buffer, size_t number,
                                  size_t lba) {
    return usb_msc_device_rw(drive, buffer, number, lba, false);
}

static size_t usb_msc_device_write(int drive, uint8_t *buffer, size_t number,
                                   size_t lba) {
    return usb_msc_device_rw(drive, buffer, number, lba, true);
}

static int usb_msc_device_ioctl(device_t *device, size_t request, void *argument)
{
    (void)argument;
    if (request != IOBLKSYNC) return -EOPNOTSUPP;
    if (!device || device->vdiskid >= 256) return -ENODEV;

    xhci_msc_state *msc;
    {
        UsbMscRegistryLock lock;
        msc = g_usb_msc_by_drive[device->vdiskid];
        if (!msc || !msc->ready || msc->disconnected || !msc->slot || !msc->controller) return -ENODEV;
        if (msc->hardware_read_only || msc->boot_source_read_only) return -EROFS;
        __atomic_add_fetch(&msc->io_users, 1U, __ATOMIC_ACQ_REL);
    }

    // Serialize the flush after earlier READ/WRITE commands and pin the slot
    // through disconnect. No registry spin lock is held during USB transfers.
    while (__sync_lock_test_and_set(&msc->io_lock, 1U) != 0U)
        __asm__ volatile("pause" ::: "memory");
    int result = __atomic_load_n(&msc->disconnected, __ATOMIC_ACQUIRE) ? -ENODEV :
                 xhci_msc_scsi_sync_cache(msc->controller, msc->slot);
    __sync_lock_release(&msc->io_lock);
    __atomic_sub_fetch(&msc->io_users, 1U, __ATOMIC_RELEASE);
    return result;
}

static void usb_msc_fill_block_device(device_t *dev,
                                      const xhci_msc_state *msc) {
    memset(dev, 0, sizeof(*dev));
    dev->size = msc->block_count * msc->block_size;
    dev->sector_size = msc->block_size;
    dev->type = DEVICE_BLOCK;
    dev->read = usb_msc_device_read;
    dev->write       = (msc->hardware_read_only || msc->boot_source_read_only) ? NULL : usb_msc_device_write;
    dev->flag = 1;
    dev->ioctl = usb_msc_device_ioctl;
    dev->poll = (pollf)empty;
    dev->map = (mapf)empty;
    dev->max_size = (0x1FFFFU / msc->block_size) * (uint64_t)msc->block_size;
    if (dev->max_size == 0) {
        dev->max_size = msc->block_size;
    }
    sprintf(dev->drive_name, "udisk%u", g_usb_msc_count++);
}

static int usb_msc_register_disk(usb_device *usbdev, xhci_msc_state *msc,
                                 const uint8_t *inquiry) {
#ifdef XHCI_BUILTIN
    BlockSourceHardware hardware;
    bool                have_hardware = usb_msc_source_hardware(usbdev, hardware);
    if (have_hardware && EFI_BC && (EFI_BC->boot_flags & BOOT_FLAG_INSTALLER_HANDOFF) &&
        (EFI_BC->boot_flags & BOOT_FLAG_INSTALLER) &&
        block_source_is_boot_usb(EFI_BC->installer_handoff.source, hardware))
    {
        // Freeze before either the whole node or a partition alias is exposed.
        // The BOT command helper independently enforces this immutable guard.
        msc->boot_source_read_only = true;
        hardware.usb_source_guard  = true;
        printk("usb: installer source protected topology=%s hardware_read_only=%u\n", usbdev->topology,
               unsigned(msc->hardware_read_only));
    }
#endif
    device_t dev;
    usb_msc_fill_block_device(&dev, msc);

    // Durability is backed by SYNCHRONIZE CACHE, not the old no-op ioctl.
    // Read-only installation sources retain their ordinary registration.
    int device_id = dev.write ? regist_durable_block_device(NULL, dev) : regist_device(NULL, dev);
    if (device_id < 0 || device_id >= 256) {
        printk("usb: msc probe failed topology=%s stage=regist-device rc=%d\n",
               usbdev->topology, device_id);
        return -ENOSPC;
    }

    msc->device_id = device_id;
#ifdef XHCI_BUILTIN
    msc->device_generation = block_access_generation(device_id);
#endif
    {
        UsbMscRegistryLock lock;
        g_usb_msc_by_drive[device_id] = msc;
    }
    usbdev->driver_data = msc;
    // Publish source provenance only after the hotplug partition scan has
    // released its discovery lease. A waiting installer may retain it at once.
    partition_device_added((size_t)device_id);
#ifdef XHCI_BUILTIN
    if (have_hardware)
    {
        int result = block_source_register(device_id, hardware);
        if (result)
        {
            {
                UsbMscRegistryLock lock;
                g_usb_msc_by_drive[device_id] = NULL;
            }
            delete_device(device_id);
            usbdev->driver_data = NULL;
            return result;
        }
    }
#endif

    char vendor[9];
    char product[17];
    usb_msc_ascii_field(vendor, sizeof(vendor), &inquiry[8], 8);
    usb_msc_ascii_field(product, sizeof(product), &inquiry[16], 16);

    printk("usb: msc ready %s topology=%s blocks=%llu block_size=%u max_lun=%u vendor='%s' product='%s'\n",
           dev.drive_name, usbdev->topology, msc->block_count, msc->block_size,
           msc->max_lun, vendor, product);
    return 0;
}

static int usb_msc_probe(usb_device *usbdev, const usb_interface_info *iface) {
    if (!usbdev || !iface || !usbdev->slot) {
        return -EINVAL;
    }
    if (iface->interface_protocol != USB_MSC_PROTOCOL_BBB) {
        return -ENODEV;
    }

    xhci_slot_state *slot = usbdev->slot;
    if (!usb_msc_fill_endpoints(slot, iface)) {
        printk("usb: msc probe failed topology=%s stage=endpoints if=%u class=%02x/%02x/%02x eps=%u\n",
               usbdev->topology, iface->number, iface->interface_class,
               iface->interface_subclass, iface->interface_protocol,
               iface->endpoint_count);
        return -ENODEV;
    }

    pr_debug("usb: msc probe start topology=%s if=%u in=0x%02x/%u out=0x%02x/%u\n",
             usbdev->topology, iface->number, slot->msc.bulk_in_address,
             slot->msc.bulk_in_epid, slot->msc.bulk_out_address,
             slot->msc.bulk_out_epid);

    xhci_boot_stage(usbdev->controller, slot->port_id, UsbBootStage::MscEndpoints);
    int rc = xhci_configure_msc_endpoints(usbdev->controller, slot);
    if (rc != 0) {
        printk("usb: msc probe failed topology=%s stage=configure rc=%d in=0x%02x/%u out=0x%02x/%u\n",
               usbdev->topology, rc, slot->msc.bulk_in_address,
               slot->msc.bulk_in_epid, slot->msc.bulk_out_address,
               slot->msc.bulk_out_epid);
        xhci_boot_result(usbdev->controller, rc);
        return rc;
    }

    pr_debug("usb: msc endpoints configured topology=%s slot=%u\n",
             usbdev->topology, slot->slot_id);

    xhci_msc_state *msc = &slot->msc;

    uint8_t max_lun = 0;
    if (xhci_control_request(
            usbdev->controller, slot,
            USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
            USB_MSC_REQ_GET_MAX_LUN, 0, msc->interface_number, &max_lun, 1,
            NULL) == 0) {
        msc->max_lun = max_lun;
    }

    uint8_t inquiry[36];
    memset(inquiry, 0, sizeof(inquiry));
    (void)xhci_msc_scsi_inquiry(usbdev->controller, slot, inquiry,
                                sizeof(inquiry));

    xhci_boot_stage(usbdev->controller, slot->port_id, UsbBootStage::MscReady);
    rc = -EIO;
    for (int attempt = 0; attempt < 20; ++attempt) {
        rc = xhci_msc_scsi_test_unit_ready(usbdev->controller, slot);
        if (rc == 0) {
            break;
        }
        uint8_t sense[18];
        memset(sense, 0, sizeof(sense));
        (void)xhci_msc_scsi_request_sense(usbdev->controller, slot, sense,
                                          sizeof(sense));
        delay_ms_hp(100);
    }
    if (rc != 0) {
        printk("usb: msc probe failed topology=%s stage=test-unit-ready rc=%d\n",
               usbdev->topology, rc);
        xhci_release_msc(slot);
        xhci_boot_result(usbdev->controller, rc);
        return rc;
    }

    xhci_boot_stage(usbdev->controller, slot->port_id, UsbBootStage::MscCapacity);
    uint8_t cap10[8];
    memset(cap10, 0, sizeof(cap10));
    rc = xhci_msc_scsi_read_capacity10(usbdev->controller, slot, cap10);
    if (rc != 0) {
        printk("usb: msc probe failed topology=%s stage=read-capacity10 rc=%d\n",
               usbdev->topology, rc);
        xhci_release_msc(slot);
        xhci_boot_result(usbdev->controller, rc);
        return rc;
    }

    uint64_t last_lba = usb_msc_get_be32(&cap10[0]);
    uint32_t block_size = usb_msc_get_be32(&cap10[4]);
    if (last_lba == 0xFFFFFFFFULL) {
        uint8_t cap16[32];
        memset(cap16, 0, sizeof(cap16));
        rc = xhci_msc_scsi_read_capacity16(usbdev->controller, slot, cap16);
        if (rc != 0) {
            printk("usb: msc probe failed topology=%s stage=read-capacity16 rc=%d\n",
                   usbdev->topology, rc);
            xhci_release_msc(slot);
            xhci_boot_result(usbdev->controller, rc);
            return rc;
        }
        last_lba = usb_msc_get_be64(&cap16[0]);
        block_size = usb_msc_get_be32(&cap16[8]);
        msc->use_read16 = true;
    }

    if (block_size == 0) {
        printk("usb: msc probe failed topology=%s stage=capacity-invalid last_lba=%llu block_size=%u\n",
               usbdev->topology, last_lba, block_size);
        xhci_release_msc(slot);
        return -EIO;
    }

    msc->block_size = block_size;
    msc->block_count = last_lba + 1ULL;
    if (!msc->block_count || msc->block_count > (~uint64_t(0)) / block_size)
    {
        xhci_release_msc(slot);
        return -EOVERFLOW;
    }
    uint8_t mode[4]         = {};
    int     mode_result     = xhci_msc_scsi_mode_sense6(usbdev->controller, slot, mode);
    msc->hardware_read_only = mode_result == 0 && (mode[2] & 0x80);

    xhci_boot_stage(usbdev->controller, slot->port_id, UsbBootStage::MscRegister);
    rc = usb_msc_register_disk(usbdev, msc, inquiry);
    if (rc != 0) {
        xhci_release_msc(slot);
        xhci_boot_result(usbdev->controller, rc);
        return rc;
    }
    return 0;
}

static void usb_msc_remove(usb_device *usbdev) {
    if (!usbdev || !usbdev->slot) {
        return;
    }

    xhci_msc_state *msc = &usbdev->slot->msc;
    if (msc->device_id >= 0 && msc->device_id < 256) {
        {
            UsbMscRegistryLock lock;
            __atomic_store_n(&msc->disconnected, true, __ATOMIC_RELEASE);
            g_usb_msc_by_drive[msc->device_id] = NULL;
            usbdev->quarantined = msc->boot_source_read_only || __atomic_load_n(&msc->io_users, __ATOMIC_ACQUIRE) != 0;
        }
#ifdef XHCI_BUILTIN
        block_source_disconnect(msc->device_id, msc->device_generation);
        if (msc->boot_source_read_only)
            printk("usb: installer source disconnected topology=%s generation=%llu; retained until reboot\n",
                   usbdev->topology, msc->device_generation);
#endif
        delete_device(msc->device_id);
    }
    usbdev->driver_data = NULL;
    if (!usbdev->quarantined) xhci_release_msc(usbdev->slot);
}

static const usb_driver_id g_usb_msc_ids[] = {
    {
        .match_flags = USB_DRIVER_MATCH_IFACE_CLASS |
                       USB_DRIVER_MATCH_IFACE_SUBCLASS |
                       USB_DRIVER_MATCH_IFACE_PROTOCOL,
        .interface_class = USB_CLASS_MASS_STORAGE,
        .interface_subclass = USB_MSC_SUBCLASS_SCSI,
        .interface_protocol = USB_MSC_PROTOCOL_BBB,
    },
    {0},
};

static usb_driver g_usb_msc_driver = {
    .name = "usb-msc",
    .priority = 0,
    .id_table = g_usb_msc_ids,
    .probe = usb_msc_probe,
    .remove = usb_msc_remove,
    .next = NULL,
};

void usb_register_builtin_msc_driver(void) {
    usb_register_driver(&g_usb_msc_driver);
}

int usb_fallback_probe_msc(usb_device *usbdev) {
    if (!usbdev) {
        return -EINVAL;
    }

    for (uint8_t i = 0; i < usbdev->interface_count; ++i) {
        const usb_interface_info *iface = &usbdev->interfaces[i];
        if (usb_msc_iface_is_bot(iface)) {
            int rc = usb_msc_probe(usbdev, iface);
            if (rc == 0) {
                usb_msc_bind_driver(usbdev, &g_usb_msc_driver);
            }
            return rc;
        }
    }

    usb_interface_info hint;
    if (!usb_msc_extract_iface_from_config(usbdev, &hint)) {
        if (usbdev->slot &&
            usbdev->slot->active_iface_class == USB_CLASS_MASS_STORAGE &&
            usbdev->slot->active_iface_subclass == USB_MSC_SUBCLASS_SCSI &&
            usbdev->slot->active_iface_protocol == USB_MSC_PROTOCOL_BBB) {
            pr_debug("usb: msc fallback saw BOT class at topology=%s but could not recover endpoints from config\n",
                     usbdev->topology);
        }
        return -ENODEV;
    }

    pr_debug("usb: msc fallback recovered iface topology=%s if=%u eps=%u\n",
             usbdev->topology, hint.number, hint.endpoint_count);
    int rc = usb_msc_probe(usbdev, &hint);
    if (rc == 0) {
        usb_msc_bind_driver(usbdev, &g_usb_msc_driver);
    }
    return rc;
}
