#include "xhci.h"

static int usb_external_hub_detect(usb_hub *hub, uint8_t port,
                                   usb_port_status *status) {
    if (!hub || !hub->usbdev || !hub->usbdev->slot) {
        return -ENODEV;
    }
    return xhci_hub_detect_port(hub->controller, hub->usbdev->slot,
                                (uint8_t)(port + 1), status);
}

static int usb_external_hub_reset(usb_hub *hub, uint8_t port,
                                  uint8_t *speed_out) {
    if (!hub || !hub->usbdev || !hub->usbdev->slot) {
        return -ENODEV;
    }
    return xhci_hub_reset_port(hub->controller, hub->usbdev->slot,
                               (uint8_t)(port + 1), speed_out);
}

static void usb_external_hub_disconnect(usb_hub *hub, uint8_t port) {
    if (!hub || !hub->usbdev || !hub->usbdev->slot) {
        return;
    }
    (void)xhci_hub_clear_port_feature(hub->controller, hub->usbdev->slot,
                                      (uint8_t)(port + 1), USB_PORT_FEAT_ENABLE);
}

static void usb_external_hub_ack(usb_hub *hub, uint8_t port,
                                 const usb_port_status *status) {
    if (!hub || !hub->usbdev || !hub->usbdev->slot || !status) {
        return;
    }
    xhci_hub_ack_port_change(hub->controller, hub->usbdev->slot,
                             (uint8_t)(port + 1), status);
}

static const usb_hub_ops g_external_hub_ops = {
    .detect = usb_external_hub_detect,
    .reset = usb_external_hub_reset,
    .disconnect = usb_external_hub_disconnect,
    .ack = usb_external_hub_ack,
};

static int usb_hub_probe(usb_device *usbdev, const usb_interface_info *iface) {
    if (!usbdev || !iface || !usbdev->slot) {
        return -EINVAL;
    }

    usbdev->slot->is_hub = true;
    int rc = xhci_setup_hub(usbdev->controller, usbdev->slot);
    if (rc != 0) {
        return rc;
    }

    usb_hub *hub = (usb_hub *)malloc(sizeof(*hub));
    if (!hub) {
        return -ENOMEM;
    }
    memset(hub, 0, sizeof(*hub));
    hub->controller = usbdev->controller;
    hub->usbdev = usbdev;
    hub->ops = &g_external_hub_ops;
    hub->port_count = usbdev->slot->hub_port_count;

    usbdev->child_hub = hub;
    usb_register_hub(hub);

    printk("usb: hub ready topology=%s ports=%u class=%02x/%02x/%02x\n",
           usbdev->topology, hub->port_count, iface->interface_class,
           iface->interface_subclass, iface->interface_protocol);
    return 0;
}

static void usb_hub_remove(usb_device *usbdev) {
    (void)usbdev;
}

static const usb_driver_id g_usb_hub_ids[] = {
    {
        .match_flags = USB_DRIVER_MATCH_IFACE_CLASS,
        .interface_class = USB_CLASS_HUB,
    },
    {0},
};

static usb_driver g_usb_hub_driver = {
    .name = "usb-hub",
    .priority = 0,
    .id_table = g_usb_hub_ids,
    .probe = usb_hub_probe,
    .remove = usb_hub_remove,
    .next = NULL,
};

void usb_register_builtin_hub_driver(void) {
    usb_register_driver(&g_usb_hub_driver);
}

int usb_fallback_probe_hub(usb_device *usbdev) {
    if (!usbdev) {
        return -EINVAL;
    }

    for (uint8_t i = 0; i < usbdev->interface_count; ++i) {
        const usb_interface_info *iface = &usbdev->interfaces[i];
        if (iface->interface_class == USB_CLASS_HUB) {
            int rc = usb_hub_probe(usbdev, iface);
            if (rc == 0 &&
                usbdev->bound_driver_count <
                    (sizeof(usbdev->bound_drivers) /
                     sizeof(usbdev->bound_drivers[0]))) {
                usbdev->bound_drivers[usbdev->bound_driver_count++] =
                    &g_usb_hub_driver;
            }
            return rc;
        }
    }
    return -ENODEV;
}
