#include <acpi/fadt.h>
#include <cpu/regio.h>
#include <efi/boot.h>
#include <krlibc.h>
#include <mm/page.h>
#include <power.h>
#include <proto.hpp>

#define ACPI_GAS_SYSTEM_MEMORY 0
#define ACPI_GAS_SYSTEM_IO     1

static inline void reload_current_cr3()
{
    uint64_t cr3 = get_cr3();
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static bool with_efi_identity_map(uint64_t *saved_entry)
{
    page_table_t *pml4 = (page_table_t *)phys_to_virt(get_cr3());
    if (pml4 == NULL) return false;

    *saved_entry = pml4->entries[0].value;
    if (!(pml4->entries[256].value & PTE_PRESENT)) return false;

    // Bootloader put the firmware physical map in both halves. The kernel
    // later clears PML4[0], so RuntimeServices low pointers fault unless we
    // temporarily restore that entry before crossing back into UEFI.
    pml4->entries[0].value = pml4->entries[256].value;
    reload_current_cr3();
    return true;
}

static void restore_efi_identity_map(uint64_t saved_entry)
{
    page_table_t *pml4 = (page_table_t *)phys_to_virt(get_cr3());
    if (pml4 == NULL) return;

    pml4->entries[0].value = saved_entry;
    reload_current_cr3();
}

static bool aml_read_integer(const uint8_t **cursor, const uint8_t *end, uint8_t *value)
{
    if (*cursor >= end) return false;

    const uint8_t opcode = *(*cursor)++;
    switch (opcode)
    {
    case 0x00: *value = 0; return true; // ZeroOp
    case 0x01: *value = 1; return true; // OneOp
    case 0x0A:
        if (*cursor >= end) return false;
        *value = *(*cursor)++;
        return true;
    case 0x0B:
        if (end - *cursor < 2) return false;
        *value = (*cursor)[0];
        *cursor += 2;
        return true;
    case 0x0C:
    case 0x0E:
        if (*cursor >= end) return false;
        *value = (*cursor)[0];
        *cursor += opcode == 0x0C ? 4 : 8;
        return true;
    default: return false;
    }
}

static bool acpi_get_s5_sleep_types(const FixedAcpiDescriptionTable *fadt, uint16_t *slp_typa, uint16_t *slp_typb)
{
    if (fadt == NULL || slp_typa == NULL || slp_typb == NULL) return false;

    const uint64_t dsdt_phys = fadt->x_dsdt ? fadt->x_dsdt : fadt->dsdt;
    if (dsdt_phys == 0) return false;

    const ACPI_TABLE_HEADER *dsdt = (const ACPI_TABLE_HEADER *)phys_to_virt(dsdt_phys);
    if (dsdt == NULL || memcmp(dsdt->sign, "DSDT", 4) != 0 || dsdt->length <= sizeof(ACPI_TABLE_HEADER)) return false;

    const uint8_t *aml = (const uint8_t *)(dsdt + 1);
    const uint8_t *end = ((const uint8_t *)dsdt) + dsdt->length;

    for (const uint8_t *cursor = aml; cursor + 4 < end; ++cursor)
    {
        if (memcmp(cursor, "_S5_", 4) != 0) continue;
        if (!((cursor > aml && cursor[-1] == 0x08) || (cursor > aml + 1 && cursor[-2] == 0x08 && cursor[-1] == '\\')))
            continue;

        const uint8_t *pkg = cursor + 4;
        if (pkg >= end || *pkg != 0x12) continue; // PackageOp
        ++pkg;

        if (pkg >= end) continue;
        pkg += 1 + ((*pkg >> 6) & 0x3); // skip PkgLength
        if (pkg >= end) continue;
        ++pkg; // NumElements

        uint8_t a = 0;
        uint8_t b = 0;
        if (!aml_read_integer(&pkg, end, &a)) continue;
        if (!aml_read_integer(&pkg, end, &b)) continue;

        *slp_typa = ((uint16_t)a) << 10;
        *slp_typb = ((uint16_t)b) << 10;
        return true;
    }

    return false;
}

static void acpi_write_pm_control(uint32_t port, uint8_t width, uint16_t value)
{
    if (port == 0 || width == 0) return;
    if (width >= 2) outw((uint16_t)port, value);
    else outb((uint16_t)port, (uint8_t)value);
}

static void acpi_write_gas_u8(const GenericAddressStructure *gas, uint8_t value)
{
    if (gas == NULL || gas->address == 0) return;

    uint8_t access = gas->access_size;
    if (access == 0) {
        if (gas->register_bit_width <= 8) access = 1;
        else if (gas->register_bit_width <= 16) access = 2;
        else access = 3;
    }

    if (gas->address_space_id == ACPI_GAS_SYSTEM_IO) {
        if (access <= 1) outb((uint16_t)gas->address, value);
        else if (access == 2) outw((uint16_t)gas->address, value);
        else outl((uint16_t)gas->address, value);
        return;
    }

    if (gas->address_space_id == ACPI_GAS_SYSTEM_MEMORY) {
        uintptr_t va = (uintptr_t)phys_to_virt(gas->address);
        if (va == 0) return;
        if (access <= 1) *(volatile uint8_t *)va = value;
        else if (access == 2) *(volatile uint16_t *)va = value;
        else *(volatile uint32_t *)va = value;
    }
}

bool acpi_poweroff(BOOT_CONFIG *boot_config)
{
    if (boot_config == NULL)
    {
        write_serial_string("ACPI poweroff skipped: boot config unavailable.\n");
        return false;
    }

    const FixedAcpiDescriptionTable *fadt = (const FixedAcpiDescriptionTable *)phys_to_virt(boot_config->FADT);
    if (fadt == NULL)
    {
        write_serial_string("ACPI poweroff skipped: FADT unavailable.\n");
        return false;
    }

    uint16_t slp_typa = 0;
    uint16_t slp_typb = 0;
    if (!acpi_get_s5_sleep_types(fadt, &slp_typa, &slp_typb))
    {
        write_serial_string("ACPI poweroff skipped: _S5_ not found.\n");
        return false;
    }

    write_serial_fmt("ACPI poweroff: PM1a=0x%x PM1b=0x%x SLP_TYPa=0x%x SLP_TYPb=0x%x\n", fadt->pm1a_cnt_blk,
                     fadt->pm1b_cnt_blk, slp_typa, slp_typb);

    if (fadt->smi_cmd != 0 && fadt->acpi_enable != 0)
    {
        outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable);
        delay_ms_hp(10);
    }

    acpi_write_pm_control(fadt->pm1a_cnt_blk, fadt->pm1_cnt_len, slp_typa | ACPI_SLP_EN);
    if (fadt->pm1b_cnt_blk != 0) acpi_write_pm_control(fadt->pm1b_cnt_blk, fadt->pm1_cnt_len, slp_typb | ACPI_SLP_EN);
    return true;
}

void qemu_poweroff_fallback()
{
    write_serial_string("Falling back to QEMU poweroff ports.\n");
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
}

void vmware_poweroff_fallback()
{
    // VMware / VirtualBox compatible ACPI shutdown port fallback.
    // If ACPI _S5_ parsing fails, this path can still power off the VM.
    write_serial_string("Falling back to VMware poweroff port.\n");
    outw(0x4004, 0x3400);
}

void poweroff_fallback(BOOT_CONFIG *boot_config)
{
    acpi_poweroff(boot_config);
    if (boot_config != NULL && boot_config->is_qemu == 1) {
        qemu_poweroff_fallback();
    } else {
        vmware_poweroff_fallback();
    }
}

static bool acpi_reboot(BOOT_CONFIG *boot_config)
{
    if (boot_config == NULL) return false;
    const FixedAcpiDescriptionTable *fadt = (const FixedAcpiDescriptionTable *)phys_to_virt(boot_config->FADT);
    if (fadt == NULL) return false;
    if (fadt->reset_reg.address == 0) return false;

    write_serial_fmt("ACPI reboot: reset_reg.space=%d addr=0x%llx val=0x%x\n", fadt->reset_reg.address_space_id,
                     fadt->reset_reg.address, fadt->reset_value);

    if (fadt->smi_cmd != 0 && fadt->acpi_enable != 0) {
        outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable);
        delay_ms_hp(10);
    }

    acpi_write_gas_u8(&fadt->reset_reg, fadt->reset_value);
    delay_ms_hp(50);
    return true;
}

[[noreturn]] void power_shutdown(EFI_SYSTEM_TABLE *system_table, BOOT_CONFIG *boot_config)
{
    uint64_t saved_entry     = 0;
    bool     identity_mapped = with_efi_identity_map(&saved_entry);

    if (!identity_mapped) { write_serial_string("UEFI runtime identity map restore failed.\n"); }

    if (system_table != NULL && system_table->RuntimeServices != NULL)
    {
        system_table->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        write_serial_string("UEFI ResetSystem returned unexpectedly.\n");
    }
    else
    {
        write_serial_string("UEFI ResetSystem skipped: RuntimeServices unavailable.\n");
    }

    if (identity_mapped) { restore_efi_identity_map(saved_entry); }

    poweroff_fallback(boot_config);

    while (1)
        __asm__ volatile("hlt");
}

static void reboot_fallback(BOOT_CONFIG *boot_config)
{
    write_serial_string("Falling back to legacy reboot ports.\n");

    acpi_reboot(boot_config);

    // Try keyboard controller reset first.
    for (int i = 0; i < 100000; ++i) {
        if ((inb(0x64) & 0x02) == 0) break;
        __asm__ volatile("pause");
    }
    outb(0x64, 0xFE);
    delay_ms_hp(50);

    // Then try chipset reset control port.
    outb(0xCF9, 0x02);
    delay_ms_hp(20);
    outb(0xCF9, 0x06);
    delay_ms_hp(20);

    // Try fast A20 reset bit path as another fallback.
    outb(0x92, (uint8_t)(inb(0x92) | 0x01));
    delay_ms_hp(20);

    // Last resort: force triple fault to reset.
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idtr = {0, 0};
    __asm__ volatile("lidt %0" : : "m"(null_idtr));
    __asm__ volatile("int3");
}

[[noreturn]] void power_reboot(EFI_SYSTEM_TABLE *system_table, BOOT_CONFIG *boot_config)
{
    (void)boot_config;

    uint64_t saved_entry     = 0;
    bool     identity_mapped = with_efi_identity_map(&saved_entry);

    if (!identity_mapped) { write_serial_string("UEFI runtime identity map restore failed.\n"); }

    if (system_table != NULL && system_table->RuntimeServices != NULL)
    {
        system_table->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
        write_serial_string("UEFI ResetSystem(reboot) returned unexpectedly.\n");
    }
    else
    {
        write_serial_string("UEFI reboot skipped: RuntimeServices unavailable.\n");
    }

    if (identity_mapped) { restore_efi_identity_map(saved_entry); }

    reboot_fallback(boot_config);

    while (1)
        __asm__ volatile("hlt");
}
