// SPDX-License-Identifier: Apache-2.0
// Bounded UEFI variable adapter; never invokes firmware from a GUI lock or VFS
// operation. Buffers are allocated first; the masked firmware interval performs
// only synchronous runtime calls, fixed copies and page/FPU-state restoration.
#include <cpu/regio.h>
#include <efi/efi.h>
#include <errno.h>
#include <installer/efi_boot.h>
#include <installer/partition_install.h>
#include <krlibc.h>
#include <mm/page.h>
#include <proto.hpp>
#include <task/pcb.h>
#include <task/scheduler.h>

extern EFI_SYSTEM_TABLE *EFI_ST;

namespace {
// UEFI 2.11 section 8 specifies these exact offsets. Keep the view local:
// unrelated historical EFI_RUNTIME_SERVICES declarations are not an ABI input.
struct VariableServices
{
    EFI_TABLE_HEADER header;
    uint64_t         time_services[4], virtual_services[2];
    EFI_STATUS(EFIAPI *get)(CHAR16 *, EFI_GUID *, uint32_t *, UINTN *, void *);
    uint64_t next;
    EFI_STATUS(EFIAPI *set)(CHAR16 *, EFI_GUID *, uint32_t, UINTN, void *);
};
static_assert(__builtin_offsetof(VariableServices, get) == 72, "UEFI GetVariable offset");
static_assert(__builtin_offsetof(VariableServices, set) == 88, "UEFI SetVariable offset");
unsigned char firmware_busy;

struct Workspace
{
    EFI_GUID guid = {
        0x8be4df61, 0x93ca, 0x11d2, {0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c}
    };
    CHAR16  name[10], order_name[10];
    uint8_t option[256], existing[4096], old_order[2048], new_order[2050];
    alignas(16) uint8_t fpu[512];
};

class FirmwareScope {
    page_table_t *pml4  = nullptr;
    uint64_t      flags = 0, previous = 0, cr3 = 0;
    Workspace    &work;

  public:
    bool active = false;
    explicit FirmwareScope(Workspace &storage) : work(storage)
    {
        flags = get_rflags();
        if (!scheduler_is_enabled() || !(flags & (1ULL << 9))) return;
        disable_scheduler();
        __asm__ volatile("cli" ::: "memory");
        cr3  = get_cr3();
        pml4 = static_cast<page_table_t *>(phys_to_virt(cr3));
        if (!pml4 || !(pml4->entries[256].value & PTE_PRESENT))
        {
            enable_scheduler();
            __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
            return;
        }
        previous = pml4->entries[0].value;
        __asm__ volatile("fxsave64 %0" : "=m"(work.fpu) : : "memory");
        pml4->entries[0].value = pml4->entries[256].value;
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
        active = true;
    }
    ~FirmwareScope()
    {
        if (!active) return;
        pml4->entries[0].value = previous;
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
        __asm__ volatile("fxrstor64 %0" : : "m"(work.fpu) : "memory");
        enable_scheduler();
        __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
    }
};

void boot_name(CHAR16 *name, unsigned number)
{
    const char hex[] = "0123456789ABCDEF";
    name[0]          = 'B';
    name[1]          = 'o';
    name[2]          = 'o';
    name[3]          = 't';
    for (unsigned i = 0; i < 4; ++i)
        name[4 + i] = hex[(number >> ((3 - i) * 4)) & 15];
    name[8] = 0;
}

int error(EFI_STATUS status)
{
    if (!EFI_ERROR(status)) return 0;
    if (status == EFI_OUT_OF_RESOURCES) return -ENOSPC;
    if (status == EFI_WRITE_PROTECTED) return -EROFS;
    if (status == EFI_UNSUPPORTED) return -EOPNOTSUPP;
    return -EIO;
}

int update(VariableServices *runtime, Workspace &w, size_t option_bytes)
{
    if (runtime->header.Signature != 0x56524553544e5552ULL || runtime->header.HeaderSize < 96 || !runtime->get ||
        !runtime->set)
        return -EOPNOTSUPP;
    const char order_name[] = "BootOrder";
    for (unsigned i = 0; i < sizeof(order_name); ++i)
        w.order_name[i] = uint8_t(order_name[i]);
    uint32_t   attributes  = 0;
    UINTN      order_bytes = sizeof(w.old_order);
    EFI_STATUS status      = runtime->get(w.order_name, &w.guid, &attributes, &order_bytes, w.old_order);
    if (status == EFI_NOT_FOUND)
        order_bytes = 0;
    else if (EFI_ERROR(status))
        return status == EFI_BUFFER_TOO_SMALL ? -E2BIG : error(status);
    if (order_bytes % 2 || order_bytes > sizeof(w.old_order)) return -EUCLEAN;
    int chosen = -1;
    // Reuse only an exact XJ380 option for this exact ESP. Never rewrite a
    // Windows/other OS option, including entries with oversized payloads.
    for (unsigned i = 0; i < order_bytes; i += 2)
    {
        unsigned slot = unsigned(w.old_order[i]) | unsigned(w.old_order[i + 1]) << 8;
        boot_name(w.name, slot);
        UINTN bytes = sizeof(w.existing);
        status      = runtime->get(w.name, &w.guid, &attributes, &bytes, w.existing);
        if (!EFI_ERROR(status) && bytes == option_bytes && !memcmp(w.existing, w.option, option_bytes))
        {
            chosen = int(slot);
            break;
        }
        if (EFI_ERROR(status) && status != EFI_NOT_FOUND && status != EFI_BUFFER_TOO_SMALL) return error(status);
    }
    if (chosen < 0)
    {
        for (unsigned slot = 0; slot < 1024; ++slot)
        {
            boot_name(w.name, slot);
            UINTN bytes = sizeof(w.existing);
            status      = runtime->get(w.name, &w.guid, &attributes, &bytes, w.existing);
            if (status == EFI_NOT_FOUND)
            {
                chosen = int(slot);
                break;
            }
            if (EFI_ERROR(status) && status != EFI_BUFFER_TOO_SMALL) return error(status);
        }
        if (chosen < 0) return -ENOSPC;
        status = runtime->set(w.name, &w.guid, 7, option_bytes, w.option);
        if (EFI_ERROR(status)) return error(status);
    }
    boot_name(w.name, unsigned(chosen));
    UINTN verified_bytes = sizeof(w.existing);
    status               = runtime->get(w.name, &w.guid, &attributes, &verified_bytes, w.existing);
    if (EFI_ERROR(status) || verified_bytes != option_bytes || memcmp(w.existing, w.option, option_bytes)) return -EIO;
    size_t new_bytes = 0;
    int    result    = xjinstaller::efi_boot_order(w.old_order, order_bytes, uint16_t(chosen), w.new_order,
                                                   sizeof(w.new_order), new_bytes);
    if (result) return result;
    status = runtime->set(w.order_name, &w.guid, 7, new_bytes, w.new_order);
    if (EFI_ERROR(status)) return error(status);
    verified_bytes = sizeof(w.existing);
    status         = runtime->get(w.order_name, &w.guid, &attributes, &verified_bytes, w.existing);
    return !EFI_ERROR(status) && verified_bytes == new_bytes && !memcmp(w.existing, w.new_order, new_bytes) ? 0 : -EIO;
}
} // namespace

int installer_register_boot_entry(const xjinstaller::PartitionInstall *plan)
{
    if (!plan || !EFI_ST || !get_current_task()) return -ENODEV;
    // KernelMain retains the firmware's physical SystemTable pointer, while
    // normal kernel execution has already removed the low identity map. Read
    // it through HHDM before entering the temporary firmware address space.
    uintptr_t system_address = reinterpret_cast<uintptr_t>(EFI_ST);
    if (system_address >= (1ULL << 39)) return -EOPNOTSUPP;
    auto     *system  = static_cast<EFI_SYSTEM_TABLE *>(phys_to_virt(system_address));
    uintptr_t address = reinterpret_cast<uintptr_t>(system->RuntimeServices);
    if (!address) return -ENODEV;
    if (address >= (1ULL << 39)) return -EOPNOTSUPP;
    // The freestanding allocator guarantees only eight-byte malloc alignment.
    // FXSAVE/FXRSTOR require sixteen even when the C++ type has alignas(16).
    auto *work = static_cast<Workspace *>(aligned_alloc(alignof(Workspace), sizeof(Workspace)));
    if (!work) return -ENOMEM;
    // Use initialization before disabling interrupts; firmware only borrows
    // these buffers synchronously and no OS allocator runs in FirmwareScope.
    *work                               = Workspace{};
    const auto                   &table = plan->separate_disk ? plan->esp_before : plan->after;
    xjinstaller::partition::Entry esp{};
    int                           result = xjinstaller::partition::entry(table, plan->esp_number, esp);
    size_t                        bytes  = 0;
    if (!result)
        result = xjinstaller::efi_boot_option(plan->esp_number, esp.first, esp.last - esp.first + 1, esp.guid,
                                              work->option, sizeof(work->option), bytes);
    if (!result)
    {
        if (__atomic_test_and_set(&firmware_busy, __ATOMIC_ACQUIRE))
            result = -EBUSY;
        else
        {
            {
                FirmwareScope scope(*work);
                result =
                    scope.active ? update(reinterpret_cast<VariableServices *>(address), *work, bytes) : -EWOULDBLOCK;
            }
            __atomic_clear(&firmware_busy, __ATOMIC_RELEASE);
        }
    }
    free(work);
    return result;
}
