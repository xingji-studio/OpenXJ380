#include "xhci.h"

static usb_driver *g_usb_drivers = NULL;
static usb_hub *g_usb_hubs = NULL;
static bool g_usb_core_inited = false;
static bool g_usb_hotplug_started = false;

static void usb_hotplug_worker(void *arg) {
    (void)arg;
    for (;;) {
        usb_scan_all_hubs();
        delay_ms_hp(20);
    }
}

static int usb_driver_match_id(const usb_driver_id *id, const usb_device *usbdev,
                               const usb_interface_info *iface) {
    int score = 0;

    if (!id || id->match_flags == 0 || !usbdev || !iface) {
        return -1;
    }
    if ((id->match_flags & USB_DRIVER_MATCH_VENDOR) &&
        id->vendor_id != usbdev->vendor_id) {
        return -1;
    }
    if ((id->match_flags & USB_DRIVER_MATCH_PRODUCT) &&
        id->product_id != usbdev->product_id) {
        return -1;
    }
    if ((id->match_flags & USB_DRIVER_MATCH_IFACE_CLASS) &&
        id->interface_class != iface->interface_class) {
        return -1;
    }
    if ((id->match_flags & USB_DRIVER_MATCH_IFACE_SUBCLASS) &&
        id->interface_subclass != iface->interface_subclass) {
        return -1;
    }
    if ((id->match_flags & USB_DRIVER_MATCH_IFACE_PROTOCOL) &&
        id->interface_protocol != iface->interface_protocol) {
        return -1;
    }

    if (id->match_flags & USB_DRIVER_MATCH_VENDOR) {
        score++;
    }
    if (id->match_flags & USB_DRIVER_MATCH_PRODUCT) {
        score++;
    }
    if (id->match_flags & USB_DRIVER_MATCH_IFACE_CLASS) {
        score++;
    }
    if (id->match_flags & USB_DRIVER_MATCH_IFACE_SUBCLASS) {
        score++;
    }
    if (id->match_flags & USB_DRIVER_MATCH_IFACE_PROTOCOL) {
        score++;
    }
    return score;
}

static void usb_unregister_hub(usb_hub *hub) {
    if (!hub || !hub->registered) {
        return;
    }

    usb_hub **link = &g_usb_hubs;
    while (*link) {
        if (*link == hub) {
            *link = hub->next;
            hub->next = NULL;
            hub->registered = false;
            return;
        }
        link = &(*link)->next;
    }
}

static void usb_disconnect_hub_children(usb_hub *hub) {
    if (!hub || !hub->children) {
        return;
    }

    for (uint8_t port = 0; port < hub->port_count; ++port) {
        if (hub->children[port]) {
            usb_disconnect_device(hub->children[port]);
        }
    }
}

static void usb_scan_hub(usb_hub *hub) {
    if (!hub || !hub->ops || hub->port_count == 0) {
        return;
    }

    for (uint8_t port = 0; port < hub->port_count; ++port) {
        usb_port_status status;
        memset(&status, 0, sizeof(status));

        int present = hub->ops->detect(hub, port, &status);
        if (hub->ops->ack) {
            hub->ops->ack(hub, port, &status);
        }

        usb_device *child = hub->children ? hub->children[port] : NULL;
        if (present <= 0) {
            if (child) {
                if (hub->ops->disconnect) {
                    hub->ops->disconnect(hub, port);
                }
                usb_disconnect_device(child);
            }
            if (hub->port_changed) {
                hub->port_changed[port] = 0;
            }
            continue;
        }

        if (child) {
            if (hub->port_changed) {
                hub->port_changed[port] = 0;
            }
            continue;
        }

        uint8_t speed_id = 0;
        int rc = hub->ops->reset(hub, port, &speed_id);
        if (rc != 0) {
            continue;
        }
        uint64_t recovery_ms = 100;
        if (speed_id == XHCI_SPEED_LOW) {
            recovery_ms = 500;
        } else if (speed_id == XHCI_SPEED_FULL) {
            recovery_ms = 200;
        }
        delay_ms_hp(recovery_ms);

        usb_device *usbdev = NULL;
        rc = xhci_usb_enumerate_device(hub->controller, hub,
                                       (uint8_t)(port + 1), speed_id, &usbdev);
        if (rc != 0 || !usbdev) {
            delay_ms_hp(100);
            rc = hub->ops->reset(hub, port, &speed_id);
            if (rc == 0) {
                recovery_ms = 100;
                if (speed_id == XHCI_SPEED_LOW) {
                    recovery_ms = 500;
                } else if (speed_id == XHCI_SPEED_FULL) {
                    recovery_ms = 200;
                }
                delay_ms_hp(recovery_ms);
                rc = xhci_usb_enumerate_device(hub->controller, hub,
                                               (uint8_t)(port + 1), speed_id,
                                               &usbdev);
            }
        }
        if (rc != 0 || !usbdev) {
            printk("usb: enumerate failed port=%u rc=%d\n",
                   (unsigned int)(port + 1), rc);
            if (hub->port_changed) {
                hub->port_changed[port] = 0;
            }
            continue;
        }

        if (hub->children) {
            hub->children[port] = usbdev;
        }
        if (hub->port_changed) {
            hub->port_changed[port] = 0;
        }

        int probe_rc = usb_probe_device(usbdev);
        if (probe_rc != 0) {
            if (usb_fallback_probe_msc(usbdev) == 0) {
                continue;
            }
            (void)usb_fallback_probe_hub(usbdev);
            pr_debug("usb: hub scan probe failed topology=%s vid=%04x pid=%04x active=%02x/%02x/%02x ifaces=%u\n",
                     usbdev->topology, usbdev->vendor_id, usbdev->product_id,
                     usbdev->slot ? usbdev->slot->active_iface_class : 0,
                     usbdev->slot ? usbdev->slot->active_iface_subclass : 0,
                     usbdev->slot ? usbdev->slot->active_iface_protocol : 0,
                     usbdev->interface_count);
        }
    }

    hub->needs_rescan = false;
}

void usb_core_init(void) {
    if (g_usb_core_inited) {
        return;
    }

    g_usb_core_inited = true;
}

void usb_core_start_workers(void) {
    usb_core_init();

    if (!g_usb_hotplug_started) {
        size_t tid = create_kernel_thread((void *)usb_hotplug_worker, NULL,
                                          (char *)"usb-hotplug", NULL);
        if ((int64_t)tid < 0) {
            return;
        }
        g_usb_hotplug_started = true;
        printk("usb: hotplug worker started tid=%u\n", (unsigned int)tid);
    }
}

void usb_register_driver(usb_driver *driver) {
    if (!driver) {
        return;
    }

    for (usb_driver *cur = g_usb_drivers; cur; cur = cur->next) {
        if (cur == driver) {
            return;
        }
    }

    driver->next = g_usb_drivers;
    g_usb_drivers = driver;
}

void usb_register_hub(usb_hub *hub) {
    if (!hub || hub->registered) {
        return;
    }

    if (!hub->children && hub->port_count != 0) {
        hub->children = (usb_device **)malloc(sizeof(usb_device *) *
                                              hub->port_count);
        if (!hub->children) {
            return;
        }
        memset(hub->children, 0, sizeof(usb_device *) * hub->port_count);
    }

    if (!hub->port_changed && hub->port_count != 0) {
        hub->port_changed = (uint8_t *)malloc(hub->port_count);
        if (!hub->port_changed) {
            free(hub->children);
            hub->children = NULL;
            return;
        }
        memset(hub->port_changed, 1, hub->port_count);
    }

    hub->needs_rescan = true;
    hub->next = g_usb_hubs;
    g_usb_hubs = hub;
    hub->registered = true;
}

void usb_hub_mark_port_changed(usb_hub *hub, uint32_t port_idx) {
    if (!hub || port_idx >= hub->port_count) {
        return;
    }

    if (hub->port_changed) {
        hub->port_changed[port_idx] = 1;
    }
    hub->needs_rescan = true;
}

void usb_scan_all_hubs(void) {
    usb_hub *snapshot[64];
    uint32_t count = 0;

    for (usb_hub *hub = g_usb_hubs; hub && count < 64; hub = hub->next) {
        snapshot[count++] = hub;
    }

    for (uint32_t i = 0; i < count; ++i) {
        usb_hub *hub = snapshot[i];
        if (hub && hub->registered) {
            usb_scan_hub(hub);
        }
    }
}

void usb_fill_topology(usb_device *usbdev) {
    if (!usbdev) {
        return;
    }

    if (!usbdev->parent_hub || !usbdev->parent_hub->usbdev ||
        usbdev->parent_hub->usbdev->is_root_hub) {
        snprintf(usbdev->topology, sizeof(usbdev->topology), "usb-%u",
                 (unsigned int)usbdev->port);
        usbdev->level = 1;
        return;
    }

    snprintf(usbdev->topology, sizeof(usbdev->topology), "%s.%u",
             usbdev->parent_hub->usbdev->topology, (unsigned int)usbdev->port);
    usbdev->level = (uint8_t)(usbdev->parent_hub->usbdev->level + 1);
}

int usb_parse_configuration(usb_device *usbdev) {
    if (!usbdev || !usbdev->config_buffer ||
        usbdev->config_length < sizeof(usb_config_descriptor)) {
        return -EINVAL;
    }

    uint8_t *ptr = (uint8_t *)usbdev->config_buffer;
    uint8_t *end = ptr + usbdev->config_length;
    usb_interface_info *cur = NULL;
    usb_endpoint_info *last_ep = NULL;

    memcpy(&usbdev->config_desc, ptr, sizeof(usbdev->config_desc));
    usbdev->config_value = usbdev->config_desc.bConfigurationValue;
    usbdev->interface_count = 0;
    memset(usbdev->interfaces, 0, sizeof(usbdev->interfaces));

    while (ptr + sizeof(usb_descriptor_header) <= end) {
        usb_descriptor_header *hdr = (usb_descriptor_header *)ptr;
        if (hdr->bLength == 0 || ptr + hdr->bLength > end) {
            break;
        }

        if (hdr->bDescriptorType == USB_DT_INTERFACE &&
            hdr->bLength >= sizeof(usb_interface_descriptor)) {
            if (usbdev->interface_count >= USB_CORE_MAX_IFACES) {
                break;
            }

            usb_interface_descriptor *iface = (usb_interface_descriptor *)ptr;
            cur = &usbdev->interfaces[usbdev->interface_count++];
            memset(cur, 0, sizeof(*cur));
            cur->number = iface->bInterfaceNumber;
            cur->alt_setting = iface->bAlternateSetting;
            cur->interface_class = iface->bInterfaceClass;
            cur->interface_subclass = iface->bInterfaceSubClass;
            cur->interface_protocol = iface->bInterfaceProtocol;
            last_ep = NULL;
        } else if (cur && hdr->bDescriptorType == USB_DT_ENDPOINT &&
                   hdr->bLength >= sizeof(usb_endpoint_descriptor)) {
            if (cur->endpoint_count >= USB_CORE_MAX_ENDPOINTS) {
                ptr += hdr->bLength;
                continue;
            }

            usb_endpoint_descriptor *ep = (usb_endpoint_descriptor *)ptr;
            last_ep = &cur->endpoints[cur->endpoint_count++];
            memset(last_ep, 0, sizeof(*last_ep));
            last_ep->address = ep->bEndpointAddress;
            last_ep->attributes = ep->bmAttributes;
            last_ep->interval = ep->bInterval;
            last_ep->max_packet = (uint16_t)(ep->wMaxPacketSize & 0x07FFU);
        } else if (last_ep &&
                   hdr->bDescriptorType == USB_DT_SS_ENDPOINT_COMPANION &&
                   hdr->bLength >= sizeof(usb_ss_ep_comp_descriptor)) {
            usb_ss_ep_comp_descriptor *comp =
                (usb_ss_ep_comp_descriptor *)ptr;
            last_ep->max_burst = comp->bMaxBurst;
        }

        ptr += hdr->bLength;
    }

    return 0;
}

int usb_probe_device(usb_device *usbdev) {
    if (!usbdev) {
        return -EINVAL;
    }
    if (usbdev->bound_driver_count != 0) {
        return 0;
    }

    typedef struct {
        usb_driver *driver;
        const usb_interface_info *iface;
        int score;
    } usb_probe_candidate;

    usb_probe_candidate candidates[32];
    int candidate_count = 0;

    for (uint8_t iface_idx = 0; iface_idx < usbdev->interface_count; ++iface_idx) {
        const usb_interface_info *iface = &usbdev->interfaces[iface_idx];

        for (usb_driver *driver = g_usb_drivers; driver; driver = driver->next) {
            if (!driver->id_table) {
                continue;
            }

            for (const usb_driver_id *id = driver->id_table; id->match_flags;
                 ++id) {
                int score = usb_driver_match_id(id, usbdev, iface);
                if (score < 0) {
                    continue;
                }
                if (candidate_count >= (int)(sizeof(candidates) /
                                             sizeof(candidates[0]))) {
                    break;
                }

                candidates[candidate_count].driver = driver;
                candidates[candidate_count].iface = iface;
                candidates[candidate_count].score = score + driver->priority * 16;
                candidate_count++;
            }
        }
    }

    for (int i = 0; i < candidate_count; ++i) {
        for (int j = i + 1; j < candidate_count; ++j) {
            if (candidates[j].score > candidates[i].score) {
                usb_probe_candidate tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }

    for (int i = 0; i < candidate_count; ++i) {
        if (candidates[i].driver->probe(usbdev, candidates[i].iface) == 0) {
            if (usbdev->bound_driver_count <
                (sizeof(usbdev->bound_drivers) /
                 sizeof(usbdev->bound_drivers[0]))) {
                usbdev->bound_drivers[usbdev->bound_driver_count++] =
                    candidates[i].driver;
            }
            return 0;
        }
    }

    pr_debug("usb: no driver matched topology=%s vid=%04x pid=%04x ifaces=%u\n",
             usbdev->topology, usbdev->vendor_id, usbdev->product_id,
             usbdev->interface_count);
    for (uint8_t i = 0; i < usbdev->interface_count; ++i) {
        const usb_interface_info *iface = &usbdev->interfaces[i];
        pr_debug("usb:   iface=%u alt=%u class=%02x/%02x/%02x eps=%u\n",
                 iface->number, iface->alt_setting, iface->interface_class,
                 iface->interface_subclass, iface->interface_protocol,
                 iface->endpoint_count);
    }

    return -ENODEV;
}

void usb_disconnect_device(usb_device *usbdev) {
    if (!usbdev) {
        return;
    }

    if (usbdev->child_hub) {
        usb_hub *hub = usbdev->child_hub;
        usb_unregister_hub(hub);
        usb_disconnect_hub_children(hub);
        free(hub->port_changed);
        free(hub->children);
        free(hub);
        usbdev->child_hub = NULL;
    }

    for (uint8_t i = 0; i < usbdev->bound_driver_count; ++i) {
        if (usbdev->bound_drivers[i] && usbdev->bound_drivers[i]->remove) {
            usbdev->bound_drivers[i]->remove(usbdev);
        }
    }
    usbdev->bound_driver_count = 0;

    if (usbdev->parent_hub && usbdev->port != 0 &&
        usbdev->port <= usbdev->parent_hub->port_count &&
        usbdev->parent_hub->children) {
        usbdev->parent_hub->children[usbdev->port - 1] = NULL;
    }

    usbdev->online = false;
    if (!usbdev->is_root_hub) {
        xhci_usb_release_device(usbdev);
        free(usbdev);
    }
}
