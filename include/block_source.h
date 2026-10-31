#pragma once
// SPDX-License-Identifier: Apache-2.0
// Original boot-source provenance sidecar; deliberately outside device_t ABI.

#include <block_access.h>
#include <efi/installer_handoff.h>
#include <pci/pci.h>

struct BlockSourceHardware
{
    pci_device_t *controller;
    uint32_t transport;
    uint32_t block_size;
    uint64_t bytes;
    bool read_only;
    uint16_t sata_port, sata_multiplier, lun;
    uint8_t ide_channel, ide_unit;
    uint32_t nvme_namespace;
    uint8_t nvme_eui64[8];
};

// Trusted built-in drivers call after whole-device registration, using the
// actual initialized controller/port/namespace and immutable driver geometry.
// The sidecar captures the registry generation. PCI controller objects remain
// boot-lived; future hotplug must invalidate registrations before freeing them.
int block_source_register(uint32_t device, const BlockSourceHardware &hardware);

// Source geometry may be read-only/2048-byte sectors. A unique live physical
// registration is required; unknown, duplicate and stale provenance fail
// closed. Caller must retain a real devfs node and reauthenticate this captured
// generation before claiming the source lifetime. This function neither opens
// a node nor takes a mount/target lease, and makes no target-512 assumption.
int block_source_match(const XJ_INSTALLER_SOURCE &source, BlockExtent *whole);
