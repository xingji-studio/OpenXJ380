#pragma once

#include <pci/pci.h>

// Disarm wake without resetting controllers, changing D states, stopping DMA,
// or using CF8/CFC transactions that have no shared cross-CPU lock in this tree.
static void power_prepare_pci_wake()
{
    unsigned count = get_pci_num();
    if (count > PCI_DEVICE_MAX) count = PCI_DEVICE_MAX;
    for (unsigned i = 0; i < count; ++i)
    {
        pci_device_t *device = pci_devices[i];
        if (!device || !device->op || device->op->read != pci_read || !device->op->write) continue;
        if ((device->class_code >> 16) != 2 && (device->class_code >> 8) != 0x0C03) continue;
        uint32_t capability = device->capability_point & 0xFF;
        uint64_t visited    = 0;
        while (capability)
        {
            if (capability < 0x40 || capability > 0xFC || (capability & 3)) break;
            uint64_t bit = 1ULL << (capability / 4);
            if (visited & bit) break;
            visited        |= bit;
            uint32_t value  = device->op->read(device->bus, device->slot, device->func, device->segment, capability);
            if ((value & 0xFF) == 1)
            {
                // PME support occupies bits 15:11 of the PM capabilities word.
                if (capability > 0xF8 || !(value >> 27)) break;
                uint32_t control =
                    device->op->read(device->bus, device->slot, device->func, device->segment, capability + 4);
                if (control == ~uint32_t(0) || !(control & 0x8100u)) break;
                // The aligned dword contains PMCSR plus bridge/data bytes.
                // It does not overlap PCI Command/Status. Retain every other
                // field, clear PME_EN and acknowledge the W1C PME_Status.
                device->op->write(device->bus, device->slot, device->func, device->segment, capability + 4,
                                  (control & ~0x0100u) | 0x8000u);
                break;
            }
            capability = (value >> 8) & 0xFF;
        }
    }
}
