#pragma once

#include "acpi/acpi.h"
#include "stdint.h"

typedef struct
{
    struct ACPISDTHeader Header;
    uint64_t             Reserved;
} __attribute__((packed)) MCFG;

typedef struct
{
    uint64_t base_address;
    uint16_t pci_segment_group;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
} __attribute__((packed)) MCFG_ENTRY;
