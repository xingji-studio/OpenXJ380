#pragma once

#include <acpi/fadt.h>
#include <cpu/msr.h>
#include <mm/hhdm.h>

// Fixed-register fallback only. AML methods, EC transactions and control-method
// wake routing belong to a future namespace/AML implementation, not byte scans.
static constexpr uint32_t ACPI_SCI_EN       = 1u;
static constexpr uint32_t ACPI_SLP_TYP_MASK = 7u << 10;
static constexpr uint32_t ACPI_GBL_RLS      = 1u << 2;
static constexpr uint32_t ACPI_RTC_WAKE     = 1u << 10;
// PCIEXP_WAKE_DIS is active high, unlike RTC_EN.
static constexpr uint32_t ACPI_PCIEXP_WAKE               = 1u << 14;
static constexpr uint32_t ACPI_WAK_STS                   = 1u << 15;
static constexpr uint32_t ACPI_FADT_CONTROL_POWER_BUTTON = 1u << 4;
static constexpr uint32_t ACPI_FADT_NO_FIXED_RTC         = 1u << 6;
static constexpr uint32_t ACPI_FADT_PCIE_WAKE            = 1u << 14;
static constexpr uint32_t ACPI_FADT_HW_REDUCED           = 1u << 20;

struct AcpiPowerRegister
{
    uint64_t address;
    uint8_t  space, bytes;
};

struct AcpiShutdownRegisters
{
    const FixedAcpiDescriptionTable *fadt;
    AcpiPowerRegister                control[2], status[2], enable[2];
    AcpiPowerRegister                gpe_status[2], gpe_enable[2];
    uint8_t                          gpe_bytes[2];
    AcpiPowerRegister                sleep_control, sleep_status;
    bool                             reduced;
};

static bool acpi_mapped_range(const void *pointer, size_t bytes)
{
    uintptr_t address = (uintptr_t)pointer;
    if (!address || !bytes || address > ~uintptr_t(0) - (bytes - 1)) return false;
    uintptr_t last = address + bytes - 1;
    for (uintptr_t page = address & ~uintptr_t(4095);; page += 4096)
    {
        if (!page_virt_to_phys(page)) return false;
        if (page >= (last & ~uintptr_t(4095))) return true;
    }
}

static bool acpi_table_valid(const ACPI_TABLE_HEADER *table, const char *signature, size_t minimum, size_t maximum)
{
    if (!acpi_mapped_range(table, sizeof(*table)) || memcmp(table->sign, signature, 4) || table->length < minimum ||
        table->length > maximum || !acpi_mapped_range(table, table->length))
        return false;
    uint8_t        checksum = 0;
    const uint8_t *bytes    = (const uint8_t *)table;
    for (size_t i = 0; i < table->length; ++i)
        checksum = (uint8_t)(checksum + bytes[i]);
    return checksum == 0;
}

static bool acpi_fadt_has(const FixedAcpiDescriptionTable *fadt, size_t offset, size_t bytes)
{
    return offset <= fadt->h.length && bytes <= fadt->h.length - offset;
}

static const GenericAddressStructure *acpi_fadt_gas(const FixedAcpiDescriptionTable *fadt, size_t offset)
{
    return acpi_fadt_has(fadt, offset, sizeof(GenericAddressStructure))
               ? (const GenericAddressStructure *)((const uint8_t *)fadt + offset)
               : NULL;
}

static bool acpi_power_register(AcpiPowerRegister *out, const GenericAddressStructure *gas, uint32_t legacy,
                                uint8_t bytes)
{
    *out = {};
    if (gas && gas->address)
    {
        if (gas->register_bit_offset ||
            (gas->address_space_id != ACPI_GAS_SYSTEM_IO && gas->address_space_id != ACPI_GAS_SYSTEM_MEMORY))
            return false;
        // Event/GPE GAS widths describe the complete block; each half is
        // independently accessed with the width required by that register.
        uint8_t access = bytes == 1 ? 1 : bytes == 2 ? 2 : 3;
        if (gas->access_size && gas->access_size != access) return false;
        if (gas->register_bit_width && gas->register_bit_width < bytes * 8) return false;
        out->address = gas->address;
        out->space   = gas->address_space_id;
    }
    else
    {
        if (!legacy) return true;
        out->address = legacy;
        out->space   = ACPI_GAS_SYSTEM_IO;
    }
    if (bytes != 1 && bytes != 2 && bytes != 4) return false;
    out->bytes = bytes;
    if (out->space == ACPI_GAS_SYSTEM_IO) return out->address <= 0x10000ULL - bytes;
    if (out->address > ~uint64_t(0) - bytes || (out->address & (bytes - 1))) return false;
    uintptr_t address = (uintptr_t)phys_to_virt(out->address);
    return acpi_mapped_range((const void *)address, bytes) && page_virt_to_phys(address) == out->address &&
           page_virt_to_phys(address + bytes - 1) == out->address + bytes - 1;
}

static uint32_t acpi_power_read(const AcpiPowerRegister &reg)
{
    if (reg.space == ACPI_GAS_SYSTEM_IO)
    {
        if (reg.bytes == 1) return inb((uint16_t)reg.address);
        if (reg.bytes == 2) return inw((uint16_t)reg.address);
        return inl((uint16_t)reg.address);
    }
    void *address = phys_to_virt(reg.address);
    if (reg.bytes == 1) return *(volatile uint8_t *)address;
    if (reg.bytes == 2) return *(volatile uint16_t *)address;
    return *(volatile uint32_t *)address;
}

static void acpi_power_write(const AcpiPowerRegister &reg, uint32_t value)
{
    if (reg.space == ACPI_GAS_SYSTEM_IO)
    {
        if (reg.bytes == 1)
            outb((uint16_t)reg.address, (uint8_t)value);
        else if (reg.bytes == 2)
            outw((uint16_t)reg.address, (uint16_t)value);
        else
            outl((uint16_t)reg.address, value);
        return;
    }
    void *address = phys_to_virt(reg.address);
    if (reg.bytes == 1)
        *(volatile uint8_t *)address = (uint8_t)value;
    else if (reg.bytes == 2)
        *(volatile uint16_t *)address = (uint16_t)value;
    else
        *(volatile uint32_t *)address = value;
}

static uint8_t acpi_block_width(const GenericAddressStructure *gas, uint8_t legacy, bool event)
{
    if (gas && gas->address && gas->register_bit_width)
    {
        uint8_t bits = gas->register_bit_width;
        if (event) return bits == 32 ? 2 : bits == 64 ? 4 : 0;
        return bits == 16 ? 2 : bits == 32 ? 4 : 0;
    }
    if (event) return legacy == 4 ? 2 : legacy == 8 ? 4 : 0;
    return legacy == 2 || legacy == 4 ? legacy : 0;
}

static bool acpi_shutdown_registers(BOOT_CONFIG *boot_config, AcpiShutdownRegisters *registers)
{
    *registers = {};
    if (!boot_config || !boot_config->FADT) return false;
    const auto *fadt = (const FixedAcpiDescriptionTable *)phys_to_virt(boot_config->FADT);
    if (!acpi_table_valid((const ACPI_TABLE_HEADER *)fadt, "FACP", 116, 4096)) return false;
    registers->fadt    = fadt;
    registers->reduced = (fadt->flags & ACPI_FADT_HW_REDUCED) != 0;
    if (registers->reduced)
    {
        return acpi_power_register(
                   &registers->sleep_control,
                   acpi_fadt_gas(fadt, __builtin_offsetof(FixedAcpiDescriptionTable, sleep_control_reg)), 0, 1) &&
               acpi_power_register(&registers->sleep_status,
                                   acpi_fadt_gas(fadt, __builtin_offsetof(FixedAcpiDescriptionTable, sleep_status_reg)),
                                   0, 1) &&
               registers->sleep_control.address && registers->sleep_status.address;
    }
    const uint32_t control[2]        = {fadt->pm1a_cnt_blk, fadt->pm1b_cnt_blk};
    const uint32_t events[2]         = {fadt->pm1a_evt_blk, fadt->pm1b_evt_blk};
    const size_t   control_offset[2] = {__builtin_offsetof(FixedAcpiDescriptionTable, x_pm1a_cnt_blk),
                                        __builtin_offsetof(FixedAcpiDescriptionTable, x_pm1b_cnt_blk)};
    const size_t   event_offset[2]   = {__builtin_offsetof(FixedAcpiDescriptionTable, x_pm1a_evt_blk),
                                        __builtin_offsetof(FixedAcpiDescriptionTable, x_pm1b_evt_blk)};
    for (unsigned i = 0; i != 2; ++i)
    {
        const auto *gas = acpi_fadt_gas(fadt, control_offset[i]);
        if (!acpi_power_register(&registers->control[i], gas, control[i],
                                 acpi_block_width(gas, fadt->pm1_cnt_len, false)))
            return false;
        gas = acpi_fadt_gas(fadt, event_offset[i]);
        if (events[i] || (gas && gas->address))
        {
            uint8_t bytes = acpi_block_width(gas, fadt->pm1_evt_len, true);
            if (!acpi_power_register(&registers->status[i], gas, events[i], bytes)) return false;
            registers->enable[i]          = registers->status[i];
            registers->enable[i].address += bytes;
            if (registers->enable[i].address > ~uint64_t(0) - bytes ||
                (registers->enable[i].space == ACPI_GAS_SYSTEM_IO &&
                 registers->enable[i].address > 0x10000ULL - bytes) ||
                (registers->enable[i].space == ACPI_GAS_SYSTEM_MEMORY &&
                 !acpi_mapped_range(phys_to_virt(registers->enable[i].address), bytes)))
                return false;
        }
    }
    if (!registers->control[0].address) return false;
    const uint32_t gpe[2]     = {fadt->gpe0_blk, fadt->gpe1_blk};
    const uint8_t  lengths[2] = {fadt->gpe0_blk_len, fadt->gpe1_blk_len};
    const size_t   offsets[2] = {__builtin_offsetof(FixedAcpiDescriptionTable, x_gpe0_blk),
                                 __builtin_offsetof(FixedAcpiDescriptionTable, x_gpe1_blk)};
    for (unsigned i = 0; i != 2; ++i)
    {
        const auto *gas = acpi_fadt_gas(fadt, offsets[i]);
        if (!gpe[i] && !(gas && gas->address)) continue;
        if (!lengths[i] || (lengths[i] & 1)) return false;
        if (!acpi_power_register(&registers->gpe_status[i], gas, gpe[i], 1)) return false;
        uint64_t base = registers->gpe_status[i].address;
        if (base > ~uint64_t(0) - lengths[i] ||
            (registers->gpe_status[i].space == ACPI_GAS_SYSTEM_IO && base > 0x10000ULL - lengths[i]) ||
            (registers->gpe_status[i].space == ACPI_GAS_SYSTEM_MEMORY &&
             !acpi_mapped_range(phys_to_virt(base), lengths[i])))
            return false;
        registers->gpe_bytes[i]           = lengths[i] / 2;
        registers->gpe_enable[i]          = registers->gpe_status[i];
        registers->gpe_enable[i].address += lengths[i] / 2;
    }
    return true;
}

static bool acpi_enter_power_mode(const AcpiShutdownRegisters &registers)
{
    if (registers.reduced || (acpi_power_read(registers.control[0]) & ACPI_SCI_EN)) return true;
    const auto *fadt = registers.fadt;
    if (!fadt->smi_cmd || fadt->smi_cmd > 0xFFFF || !fadt->acpi_enable) return false;
    // ACPI requires ownership commands to originate from the boot processor.
    // Existing kernel GUI workers stay on their creating BSP; other callers
    // may use an already-enabled ACPI mode or fall back to firmware.
    if (!(rdmsr(0x1B) & (1u << 8))) return false;
    outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable);
    for (unsigned i = 0; i != 300; ++i)
    {
        if (acpi_power_read(registers.control[0]) & ACPI_SCI_EN) return true;
        delay_ms_hp(10);
    }
    return (acpi_power_read(registers.control[0]) & ACPI_SCI_EN) != 0;
}

static void acpi_disable_shutdown_wake(const AcpiShutdownRegisters &registers)
{
    if (registers.reduced) return; // Legacy PM1/GPE fields are reserved.
    uint32_t flags = registers.fadt->flags;
    for (unsigned i = 0; i != 2; ++i)
    {
        if (!registers.enable[i].address) continue;
        uint32_t enabled = acpi_power_read(registers.enable[i]);
        uint32_t clear   = ACPI_WAK_STS;
        if (!(flags & ACPI_FADT_NO_FIXED_RTC))
        {
            enabled &= ~ACPI_RTC_WAKE;
            clear   |= ACPI_RTC_WAKE;
        }
        if (flags & ACPI_FADT_PCIE_WAKE)
        {
            enabled |= ACPI_PCIEXP_WAKE;
            clear   |= ACPI_PCIEXP_WAKE;
        }
        // Preserve power-button enables and unrelated event bits. Status is
        // W1C: clear only observed, defined wake status, never a full RMW word.
        acpi_power_write(registers.enable[i], enabled);
        acpi_power_write(registers.status[i], acpi_power_read(registers.status[i]) & clear);
    }
    if (flags & ACPI_FADT_CONTROL_POWER_BUTTON)
    {
        write_serial_string("ACPI shutdown: retaining GPEs for control-method power button.\n");
        return;
    }
    // A namespace power-button device can override even a clear FADT flag.
    // Keep enables until AML _PRW routing is available; do not guess which
    // GPE belongs to the physical key. Acknowledge only pending status here.
    write_serial_string("ACPI shutdown: retaining GPE enables without AML wake routing.\n");
    for (unsigned i = 0; i != 2; ++i)
    {
        for (unsigned n = 0; n < registers.gpe_bytes[i]; ++n)
        {
            auto status     = registers.gpe_status[i];
            status.address += n;
            acpi_power_write(status, acpi_power_read(status));
        }
    }
}

static bool acpi_prepare_shutdown(BOOT_CONFIG *boot_config, AcpiShutdownRegisters *registers)
{
    if (!acpi_shutdown_registers(boot_config, registers))
    {
        write_serial_string("ACPI shutdown preparation skipped: invalid FADT/register description.\n");
        return false;
    }
    if (!acpi_enter_power_mode(*registers))
    {
        write_serial_string("ACPI shutdown preparation skipped: SCI_EN handshake failed.\n");
        return false;
    }
    acpi_disable_shutdown_wake(*registers);
    return true;
}
