#include <acpi/fadt.h>
#include <cpu/regio.h>
#include <efi/boot.h>
#include <krlibc.h>
#include <mm/page.h>
#include <power.h>
#include <proto.hpp>
#include <fs/ext4/ext4.h>
#include <task/scheduler.h>

#define ACPI_GAS_SYSTEM_MEMORY 0
#define ACPI_GAS_SYSTEM_IO     1

#include "power_acpi.h"
#include "power_pci.h"

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

static void try_efi_reset(EFI_SYSTEM_TABLE *system_table, EFI_RESET_TYPE reset_type)
{
    uint64_t flags = get_rflags();
    __asm__ volatile("cli" ::: "memory");

    // The low firmware pointers are only valid while the identity map is in
    // place. Keep timer preemption masked until both the map and FPU state
    // have been restored if a broken runtime implementation returns.
    uint64_t    saved_entry     = 0;
    bool        identity_mapped = with_efi_identity_map(&saved_entry);
    const char *failure         = "UEFI ResetSystem skipped: identity map unavailable.\n";

    if (identity_mapped)
    {
        if (system_table == NULL || system_table->RuntimeServices == NULL)
        {
            failure = "UEFI ResetSystem skipped: RuntimeServices unavailable.\n";
        }
        else
        {
            EFI_RUNTIME_SERVICES *runtime = system_table->RuntimeServices;
            constexpr size_t      required_size =
                __builtin_offsetof(EFI_RUNTIME_SERVICES, ResetSystem) + sizeof(runtime->ResetSystem);
            if (runtime->Hdr.Signature != EFI_RUNTIME_SERVICES_SIGNATURE || runtime->Hdr.HeaderSize < required_size)
            {
                failure = "UEFI ResetSystem skipped: invalid runtime services table.\n";
            }
            else if (runtime->ResetSystem == NULL) { failure = "UEFI ResetSystem skipped: entry point unavailable.\n"; }
            else
            {
                alignas(16) uint8_t fpu_state[512];
                __asm__ volatile("fxsave64 %0" : "=m"(fpu_state) : : "memory");
                runtime->ResetSystem(reset_type, EFI_SUCCESS, 0, NULL);
                __asm__ volatile("fxrstor64 %0" : : "m"(fpu_state) : "memory");
                failure = "UEFI ResetSystem returned unexpectedly.\n";
            }
        }
        restore_efi_identity_map(saved_entry);
    }

    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
    write_serial_string(failure);
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
    case 0x0C:
    case 0x0E: {
        unsigned bytes = opcode == 0x0B ? 2 : opcode == 0x0C ? 4 : 8;
        if ((size_t)(end - *cursor) < bytes) return false;
        *value = (*cursor)[0];
        for (unsigned i = 1; i < bytes; ++i)
            if ((*cursor)[i]) return false;
        *cursor += bytes;
        return true;
    }
    default: return false;
    }
}

static bool acpi_get_s5_sleep_types(const FixedAcpiDescriptionTable *fadt, uint16_t *slp_typa, uint16_t *slp_typb)
{
    if (fadt == NULL || slp_typa == NULL || slp_typb == NULL) return false;

    uint64_t dsdt_phys = fadt->dsdt;
    if (acpi_fadt_has(fadt, __builtin_offsetof(FixedAcpiDescriptionTable, x_dsdt), sizeof(fadt->x_dsdt)) &&
        fadt->x_dsdt)
        dsdt_phys = fadt->x_dsdt;
    if (dsdt_phys == 0) return false;

    const ACPI_TABLE_HEADER *dsdt = (const ACPI_TABLE_HEADER *)phys_to_virt(dsdt_phys);
    if (!acpi_table_valid(dsdt, "DSDT", sizeof(ACPI_TABLE_HEADER) + 1, 1024 * 1024)) return false;

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
        unsigned following     = *pkg >> 6;
        unsigned encoded_bytes = following + 1;
        if ((size_t)(end - pkg) < encoded_bytes) continue;
        uint32_t length = *pkg & (following ? 0x0F : 0x3F);
        for (unsigned i = 0; i < following; ++i)
            length |= (uint32_t)pkg[i + 1] << (4 + i * 8);
        if (length < encoded_bytes + 1 || length > (size_t)(end - pkg)) continue;
        const uint8_t *package_end  = pkg + length;
        pkg                        += encoded_bytes;
        if (*pkg++ < 2) continue;

        uint8_t a = 0;
        uint8_t b = 0;
        if (!aml_read_integer(&pkg, package_end, &a)) continue;
        if (!aml_read_integer(&pkg, package_end, &b)) continue;
        if (a > 7 || b > 7) continue;

        *slp_typa = ((uint16_t)a) << 10;
        *slp_typb = ((uint16_t)b) << 10;
        return true;
    }

    return false;
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
    AcpiShutdownRegisters registers{};
    if (!acpi_shutdown_registers(boot_config, &registers))
    {
        write_serial_string("ACPI poweroff skipped: invalid FADT/register description.\n");
        return false;
    }

    uint16_t slp_typa = 0;
    uint16_t slp_typb = 0;
    if (!acpi_get_s5_sleep_types(registers.fadt, &slp_typa, &slp_typb))
    {
        write_serial_string("ACPI poweroff skipped: _S5_ not found.\n");
        return false;
    }

    if (!acpi_enter_power_mode(registers))
    {
        write_serial_string("ACPI poweroff skipped: SCI_EN handshake failed.\n");
        return false;
    }
    acpi_disable_shutdown_wake(registers);
    write_serial_fmt("ACPI poweroff: fixed-register S5 fallback, types=%u/%u (AML methods unavailable).\n",
                     slp_typa >> 10, slp_typb >> 10);

    uint64_t flags = get_rflags();
    __asm__ volatile("cli" ::: "memory");
    if (registers.reduced)
    {
        acpi_power_write(registers.sleep_status, acpi_power_read(registers.sleep_status) & 0x80u);
        uint32_t control = (acpi_power_read(registers.sleep_control) & ~0x3Cu) | ((slp_typa >> 10) << 2);
        acpi_power_write(registers.sleep_control, control);
        __asm__ volatile("wbinvd" ::: "memory");
        acpi_power_write(registers.sleep_control, control | 0x20u);
    }
    else
    {
        uint32_t values[2]{};
        for (unsigned i = 0; i != 2; ++i)
        {
            if (!registers.control[i].address) continue;
            values[i] = (acpi_power_read(registers.control[i]) & ~(ACPI_SLP_TYP_MASK | ACPI_SLP_EN | ACPI_GBL_RLS)) |
                        (i ? slp_typb : slp_typa);
            acpi_power_write(registers.control[i], values[i]);
        }
        __asm__ volatile("wbinvd" ::: "memory");
        for (unsigned i = 0; i != 2; ++i)
            if (registers.control[i].address) acpi_power_write(registers.control[i], values[i] | ACPI_SLP_EN);
    }
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
    return true;
}

static bool hypervisor_vendor_is(const char *expected)
{
    uint32_t eax, ebx, ecx, edx;
    asm_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1u << 31))) return false;

    asm_cpuid(0x40000000, 0, &eax, &ebx, &ecx, &edx);
    if (eax < 0x40000000) return false;
    const uint32_t vendor[3] = {ebx, ecx, edx};
    return memcmp(vendor, expected, sizeof(vendor)) == 0;
}

static bool qemu_hypervisor()
{
    return hypervisor_vendor_is("TCGTCGTCGTCG") || hypervisor_vendor_is("KVMKVMKVM\0\0\0");
}

void qemu_poweroff_fallback()
{
    if (!qemu_hypervisor())
    {
        write_serial_string("QEMU poweroff ports skipped: hypervisor not identified.\n");
        return;
    }
    write_serial_string("Falling back to QEMU poweroff ports.\n");
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
}

void vmware_poweroff_fallback()
{
    // Retain the historical public symbol, but this fixed PM1 control port
    // belongs to VirtualBox. VMware and physical machines use EFI/ACPI.
    if (!hypervisor_vendor_is("VBoxVBoxVBox"))
    {
        write_serial_string("VirtualBox poweroff port skipped: hypervisor not identified.\n");
        return;
    }
    write_serial_string("Falling back to VirtualBox poweroff port.\n");
    outw(0x4004, 0x3400);
}

void poweroff_fallback(BOOT_CONFIG *boot_config)
{
    acpi_poweroff(boot_config);
    if (boot_config != NULL && boot_config->is_qemu == 1) { qemu_poweroff_fallback(); }
    else if (hypervisor_vendor_is("VBoxVBoxVBox")) { vmware_poweroff_fallback(); }
    else { write_serial_string("VM poweroff ports skipped: no supported virtual platform.\n"); }
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

static void finish_filesystems_before_poweroff()
{
    // Normal power actions run in task context. Fatal/early-boot paths cannot
    // wait for storage or acquire filesystem mutexes; leave recovery enabled.
    if (!scheduler_is_enabled() || !get_current_task())
    {
        write_serial_string("EXT4: power cleanup unavailable; recovery required on next boot.\n");
        return;
    }
    uint64_t flags = get_rflags();
    __asm__ volatile("sti" ::: "memory");
    int error = ext4fs_finish_all();
    if (!(flags & (1ULL << 9))) __asm__ volatile("cli" ::: "memory");
    write_serial_fmt("EXT4: power cleanup %s (%d)\n", error ? "failed" : "complete", error);
}

[[noreturn]] void power_shutdown(EFI_SYSTEM_TABLE *system_table, BOOT_CONFIG *boot_config)
{
    finish_filesystems_before_poweroff();
    // ResetSystem normally never returns. Disarm validated fixed-register
    // wake sources before entering firmware, not just on the ACPI fallback.
    power_prepare_pci_wake();
    AcpiShutdownRegisters registers{};
    acpi_prepare_shutdown(boot_config, &registers);
    write_serial_string("Power: trying UEFI ResetSystem(shutdown).\n");
    try_efi_reset(system_table, EfiResetShutdown);

    poweroff_fallback(boot_config);

    write_serial_string("Poweroff methods returned; system halted.\n");
    __asm__ volatile("cli" ::: "memory");
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
    finish_filesystems_before_poweroff();
    write_serial_string("Power: trying UEFI ResetSystem(reboot).\n");
    try_efi_reset(system_table, EfiResetCold);

    reboot_fallback(boot_config);

    while (1)
        __asm__ volatile("hlt");
}
