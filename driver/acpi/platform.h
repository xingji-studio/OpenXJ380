#pragma once

#include <efi/boot.h>
#include <uacpi/types.h>

bool            acpi_host_start(const BOOT_CONFIG *boot);
bool            acpi_host_task_context();
bool            acpi_host_quiesce();
bool            acpi_host_firmware_failed();
extern "C" bool acpi_aml_owns_global_lock();

uacpi_status acpi_ec_install_early();
uacpi_status acpi_ec_install_namespace();
void         acpi_ec_enable_queries(bool enabled);
void         acpi_ec_poll_queries();
