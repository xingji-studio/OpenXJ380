#pragma once

#include <efi/efi.h>
#include <efi/boot.h>

bool acpi_poweroff(BOOT_CONFIG *boot_config);
void qemu_poweroff_fallback();
void vmware_poweroff_fallback();
void poweroff_fallback(BOOT_CONFIG *boot_config);
[[noreturn]] void power_shutdown(EFI_SYSTEM_TABLE *system_table, BOOT_CONFIG *boot_config);
[[noreturn]] void power_reboot(EFI_SYSTEM_TABLE *system_table, BOOT_CONFIG *boot_config);
