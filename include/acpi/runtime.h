#pragma once

#include <efi/boot.h>

enum class AcpiSleepPreparation
{
    Unavailable,
    Ready,
    Failed,
};

// Queue BSP initialization after the normal scheduler opens. Old loaders that
// do not supply the optional RSDP tail retain the firmware power path.
void acpi_runtime_start(const BOOT_CONFIG *boot);

// Task context, with interrupts and scheduling enabled. _PTS(5) is evaluated
// at most once; a failed method must not be bypassed by a raw S5 transition.
AcpiSleepPreparation acpi_runtime_prepare_s5(uint16_t *type_a, uint16_t *type_b);
