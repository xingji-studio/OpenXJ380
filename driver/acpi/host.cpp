#include "platform.h"

#include <cpu/regio.h>
#include <krlibc.h>
#include <mm/hhdm.h>
#include <pci/mcfg.h>
#include <proto.hpp>
#include <task/pcb.h>
#include <task/scheduler.h>
#include <uacpi/kernel_api.h>

namespace {
struct HostSpin
{
    uint8_t held;
};

struct HostSemaphore
{
    uint32_t units;
};

struct IoRange
{
    uint32_t base, length;
};

struct WorkItem
{
    WorkItem          *next;
    uacpi_work_handler handler;
    uacpi_handle       context;
};

static uint64_t                rsdp_address;
static MCFG_ENTRY              ecam[64];
static unsigned                ecam_count;
static HostSpin                work_lock{}, sci_lock{};
static WorkItem               *work_head, *work_tail;
static uint32_t                work_active, sci_active, quiesced;
static uint32_t                firmware_failed;
static uacpi_interrupt_handler sci_handler;
static uacpi_handle            sci_context;

static uint64_t lock(HostSpin *spin)
{
    uint64_t flags = get_rflags();
    __asm__ volatile("cli" ::: "memory");
    while (__atomic_test_and_set(&spin->held, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    return flags;
}

static void unlock(HostSpin *spin, uint64_t flags)
{
    __atomic_clear(&spin->held, __ATOMIC_RELEASE);
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

static void *mapped_physical(uint64_t address, size_t bytes)
{
    if (!bytes || address >= (1ULL << 47) || bytes > (1ULL << 47) - address) return nullptr;
    uintptr_t va = (uintptr_t)phys_to_virt(address);
    if (!va || va > ~uintptr_t(0) - (bytes - 1)) return nullptr;
    uintptr_t last = va + bytes - 1;
    if (page_virt_to_phys(va) != address) return nullptr;
    for (uintptr_t page = va & ~uintptr_t(4095);; page += 4096)
    {
        if (page_virt_to_phys(page) != ((address & ~uint64_t(4095)) + page - (va & ~uintptr_t(4095)))) return nullptr;
        if (page >= (last & ~uintptr_t(4095))) break;
    }
    return (void *)va;
}

static void sci_worker(void *)
{
    for (;;)
    {
        uint64_t flags   = lock(&sci_lock);
        auto     handler = sci_handler;
        auto     context = sci_context;
        bool active = !__atomic_load_n(&quiesced, __ATOMIC_ACQUIRE) || __atomic_load_n(&work_active, __ATOMIC_ACQUIRE);
        if (active) __atomic_add_fetch(&sci_active, 1, __ATOMIC_ACQ_REL);
        unlock(&sci_lock, flags);
        // The existing IRQ router has no shared, level-triggered SCI API.
        // Deliver SCI by BSP polling, keeping its IOAPIC input masked. This
        // also services firmware-global-lock completion while AML sleeps.
        if (active)
        {
            if (handler) handler(context);
            acpi_ec_poll_queries();
            __atomic_sub_fetch(&sci_active, 1, __ATOMIC_RELEASE);
        }
        scheduler_sleep_ns(2000000);
    }
}

static void work_worker(void *)
{
    for (;;)
    {
        uint64_t           flags   = lock(&work_lock);
        WorkItem          *item    = work_head;
        uacpi_work_handler handler = nullptr;
        uacpi_handle       context = nullptr;
        if (item)
        {
            work_head = item->next;
            if (!work_head) work_tail = nullptr;
            handler = item->handler;
            context = item->context;
        }
        unlock(&work_lock, flags);
        if (handler)
        {
            // The callback may free or reschedule item. Do not dereference it
            // after this point; only the independent outstanding count lives on.
            handler(context);
            __atomic_sub_fetch(&work_active, 1, __ATOMIC_RELEASE);
        }
        else
            scheduler_sleep_ns(1000000);
    }
}

static bool io_access(uacpi_handle handle, size_t offset, unsigned bytes, uint16_t *port)
{
    auto *range = (IoRange *)handle;
    if (!range || offset > range->length || bytes > range->length - offset) return false;
    *port = (uint16_t)(range->base + offset);
    return true;
}

static void *pci_access(uacpi_handle handle, size_t offset, unsigned bytes)
{
    if (!handle || offset > 4096 - bytes || (offset & (bytes - 1))) return nullptr;
    return (uint8_t *)handle + offset;
}
} // namespace

bool acpi_host_task_context()
{
    return scheduler_is_enabled() && get_current_task() && (get_rflags() & (1ULL << 9));
}

bool acpi_host_firmware_failed()
{
    return __atomic_load_n(&firmware_failed, __ATOMIC_ACQUIRE) != 0;
}

bool acpi_host_start(const BOOT_CONFIG *boot)
{
    rsdp_address = boot->acpi_rsdp;
    auto *mcfg   = (const MCFG *)mapped_physical(boot->MCFG, sizeof(MCFG));
    if (mcfg && !memcmp(mcfg->Header.Signature, "MCFG", 4) && mcfg->Header.Length >= sizeof(MCFG) &&
        mcfg->Header.Length <= sizeof(MCFG) + sizeof(ecam) &&
        (mcfg->Header.Length - sizeof(MCFG)) % sizeof(MCFG_ENTRY) == 0 &&
        mapped_physical(boot->MCFG, mcfg->Header.Length))
    {
        uint8_t sum = 0;
        for (size_t i = 0; i < mcfg->Header.Length; ++i)
            sum += ((const uint8_t *)mcfg)[i];
        if (!sum)
        {
            ecam_count = (mcfg->Header.Length - sizeof(MCFG)) / sizeof(MCFG_ENTRY);
            memcpy(ecam, mcfg + 1, ecam_count * sizeof(MCFG_ENTRY));
        }
    }
    int64_t sci = (int64_t)create_kernel_thread((void *)sci_worker, nullptr, (char *)"acpi-sci", nullptr);
    if (sci < 0) return false;
    return (int64_t)create_kernel_thread((void *)work_worker, nullptr, (char *)"acpi-work", nullptr) >= 0;
}

bool acpi_host_quiesce()
{
    __atomic_store_n(&quiesced, 1, __ATOMIC_RELEASE);
    uint64_t start = nanoTime();
    for (;;)
    {
        // Keep SCI delivery available to in-flight AML, which can need a
        // firmware-global-lock completion. Park it only once work has drained.
        uint64_t flags = lock(&sci_lock);
        bool idle = !__atomic_load_n(&sci_active, __ATOMIC_ACQUIRE) && !__atomic_load_n(&work_active, __ATOMIC_ACQUIRE);
        unlock(&sci_lock, flags);
        if (idle) return true;
        uint64_t now = nanoTime();
        if (now < start || now - start >= 10000000000ULL) return false;
        scheduler_sleep_ns(1000000);
    }
}

extern "C" {
uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out)
{
    if (!out || !rsdp_address) return UACPI_STATUS_NOT_FOUND;
    *out = rsdp_address;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr address, uacpi_size length)
{
    void *mapped = mapped_physical(address, length);
    return mapped ? mapped : UACPI_MAP_FAILED;
}

void uacpi_kernel_unmap(void *, uacpi_size) {} // Borrowed permanent HHDM mappings.

void uacpi_kernel_log(uacpi_log_level, const uacpi_char *message)
{
    write_serial_fmt("ACPI: %s", message);
}

void *uacpi_kernel_alloc(uacpi_size bytes)
{
    return malloc(bytes);
}
void uacpi_kernel_free(void *pointer)
{
    free(pointer);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot()
{
    return nanoTime();
}

void uacpi_kernel_stall(uacpi_u8 usec)
{
    uint64_t start = nanoTime();
    for (unsigned i = 0; i != 1000000; ++i)
    {
        uint64_t now = nanoTime();
        if (now < start || now - start >= (uint64_t)usec * 1000) return;
        __asm__ volatile("pause");
    }
}

void uacpi_kernel_sleep(uacpi_u64 milliseconds)
{
    while (milliseconds)
    {
        uint64_t slice = milliseconds > 1000 ? 1000 : milliseconds;
        scheduler_sleep_ns(slice * 1000000);
        milliseconds -= slice;
    }
}

uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 units)
{
    auto *semaphore = (HostSemaphore *)malloc(sizeof(HostSemaphore));
    if (semaphore) semaphore->units = units;
    return semaphore;
}

void uacpi_kernel_free_semaphore(uacpi_handle handle)
{
    free(handle);
}

uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle handle, uacpi_u16 timeout)
{
    auto *semaphore = (HostSemaphore *)handle;
    if (!semaphore) return UACPI_STATUS_INVALID_ARGUMENT;
    uint64_t start = nanoTime();
    for (;;)
    {
        uint32_t units = __atomic_load_n(&semaphore->units, __ATOMIC_ACQUIRE);
        while (units)
            if (__atomic_compare_exchange_n(&semaphore->units, &units, units - 1, false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                return UACPI_STATUS_OK;
        if (!timeout) return UACPI_STATUS_TIMEOUT;
        if (!acpi_host_task_context()) return UACPI_STATUS_DENIED;
        uint64_t now = nanoTime();
        if (timeout != 0xFFFF && (now < start || now - start >= (uint64_t)timeout * 1000000))
            return UACPI_STATUS_TIMEOUT;
        scheduler_sleep_ns(1000000);
    }
}

void uacpi_kernel_signal_semaphore(uacpi_handle handle)
{
    auto    *semaphore = (HostSemaphore *)handle;
    uint32_t units     = __atomic_load_n(&semaphore->units, __ATOMIC_RELAXED);
    while (units != ~uint32_t(0) && !__atomic_compare_exchange_n(&semaphore->units, &units, units + 1, false,
                                                                 __ATOMIC_RELEASE, __ATOMIC_RELAXED))
    {
    }
}

uacpi_thread_id uacpi_kernel_get_thread_id()
{
    return (uacpi_thread_id)get_current_task();
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts()
{
    uint64_t flags = get_rflags();
    __asm__ volatile("cli" ::: "memory");
    return flags;
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state flags)
{
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

uacpi_handle uacpi_kernel_create_spinlock()
{
    return calloc(1, sizeof(HostSpin));
}
void uacpi_kernel_free_spinlock(uacpi_handle spin)
{
    free(spin);
}
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle spin)
{
    return lock((HostSpin *)spin);
}
void uacpi_kernel_unlock_spinlock(uacpi_handle spin, uacpi_cpu_flags flags)
{
    unlock((HostSpin *)spin, flags);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *request)
{
    // uACPI 7 does not propagate this callback's return value into AML.
    // Record Fatal separately so S5 preparation cannot report success.
    if (request && request->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        __atomic_store_n(&firmware_failed, 1, __ATOMIC_RELEASE);
    write_serial_string("ACPI: firmware Breakpoint/Fatal request rejected.\n");
    return UACPI_STATUS_DENIED;
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler,
                                                    uacpi_handle context, uacpi_handle *out)
{
    if (!handler || !out) return UACPI_STATUS_INVALID_ARGUMENT;
    uint64_t flags = lock(&sci_lock);
    if (sci_handler)
    {
        unlock(&sci_lock, flags);
        return UACPI_STATUS_ALREADY_EXISTS;
    }
    sci_context = context;
    sci_handler = handler;
    *out        = &sci_handler;
    unlock(&sci_lock, flags);
    write_serial_fmt("ACPI: SCI %u serviced by BSP polling.\n", irq);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler, uacpi_handle handle)
{
    uint64_t flags = lock(&sci_lock);
    if (handle != &sci_handler || sci_handler != handler)
    {
        unlock(&sci_lock, flags);
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    sci_handler = nullptr;
    unlock(&sci_lock, flags);
    while (__atomic_load_n(&sci_active, __ATOMIC_ACQUIRE))
        scheduler_sleep_ns(1000000);
    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_work_item()
{
    return calloc(1, sizeof(WorkItem));
}
void uacpi_kernel_free_work_item(uacpi_handle item)
{
    free(item);
}

void uacpi_kernel_schedule_work(uacpi_work_type, uacpi_handle handle, uacpi_work_handler handler, uacpi_handle context)
{
    auto *item    = (WorkItem *)handle;
    item->next    = nullptr;
    item->handler = handler;
    item->context = context;
    __atomic_add_fetch(&work_active, 1, __ATOMIC_ACQ_REL);
    uint64_t flags = lock(&work_lock);
    if (work_tail)
        work_tail->next = item;
    else
        work_head = item;
    work_tail = item;
    unlock(&work_lock, flags);
}

uacpi_status uacpi_kernel_wait_for_work_completion()
{
    while (__atomic_load_n(&sci_active, __ATOMIC_ACQUIRE))
        scheduler_sleep_ns(1000000);
    while (__atomic_load_n(&work_active, __ATOMIC_ACQUIRE))
        scheduler_sleep_ns(1000000);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size length, uacpi_handle *out)
{
    if (!out || base > 0xFFFF || !length || length > 0x10000 - base) return UACPI_STATUS_INVALID_ARGUMENT;
    auto *range = (IoRange *)malloc(sizeof(IoRange));
    if (!range) return UACPI_STATUS_OUT_OF_MEMORY;
    *range = {(uint32_t)base, (uint32_t)length};
    *out   = range;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle)
{
    free(handle);
}

#define ACPI_HOST_IO(bits, type, read_port, write_port)                                                                \
    uacpi_status uacpi_kernel_io_read##bits(uacpi_handle handle, uacpi_size offset, type *value)                       \
    {                                                                                                                  \
        uint16_t port;                                                                                                 \
        if (!value || !io_access(handle, offset, sizeof(type), &port)) return UACPI_STATUS_INVALID_ARGUMENT;           \
        *value = read_port(port);                                                                                      \
        return UACPI_STATUS_OK;                                                                                        \
    }                                                                                                                  \
    uacpi_status uacpi_kernel_io_write##bits(uacpi_handle handle, uacpi_size offset, type value)                       \
    {                                                                                                                  \
        uint16_t port;                                                                                                 \
        if (!io_access(handle, offset, sizeof(type), &port)) return UACPI_STATUS_INVALID_ARGUMENT;                     \
        write_port(port, value);                                                                                       \
        return UACPI_STATUS_OK;                                                                                        \
    }
ACPI_HOST_IO(8, uacpi_u8, inb, outb)
ACPI_HOST_IO(16, uacpi_u16, inw, outw)
ACPI_HOST_IO(32, uacpi_u32, inl, outl)
#undef ACPI_HOST_IO

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out)
{
    if (!out || address.device >= 32 || address.function >= 8) return UACPI_STATUS_INVALID_ARGUMENT;
    for (unsigned i = 0; i != ecam_count; ++i)
    {
        const auto &entry = ecam[i];
        if (entry.pci_segment_group != address.segment || address.bus < entry.start_bus || address.bus > entry.end_bus)
            continue;
        // MCFG base corresponds to bus zero, including entries whose first
        // routed bus is nonzero. Byte/word writes use their exact width.
        uint64_t offset =
            ((uint64_t)address.bus << 20) | ((uint64_t)address.device << 15) | ((uint64_t)address.function << 12);
        if (entry.base_address > ~uint64_t(0) - offset) return UACPI_STATUS_MAPPING_FAILED;
        *out = mapped_physical(entry.base_address + offset, 4096);
        return *out ? UACPI_STATUS_OK : UACPI_STATUS_MAPPING_FAILED;
    }
    return UACPI_STATUS_NOT_FOUND;
}

void uacpi_kernel_pci_device_close(uacpi_handle) {}

#define ACPI_HOST_PCI(bits, type)                                                                                      \
    uacpi_status uacpi_kernel_pci_read##bits(uacpi_handle handle, uacpi_size offset, type *value)                      \
    {                                                                                                                  \
        auto *address = (volatile type *)pci_access(handle, offset, sizeof(type));                                     \
        if (!address || !value) return UACPI_STATUS_INVALID_ARGUMENT;                                                  \
        *value = *address;                                                                                             \
        return UACPI_STATUS_OK;                                                                                        \
    }                                                                                                                  \
    uacpi_status uacpi_kernel_pci_write##bits(uacpi_handle handle, uacpi_size offset, type value)                      \
    {                                                                                                                  \
        auto *address = (volatile type *)pci_access(handle, offset, sizeof(type));                                     \
        if (!address) return UACPI_STATUS_INVALID_ARGUMENT;                                                            \
        *address = value;                                                                                              \
        return UACPI_STATUS_OK;                                                                                        \
    }
ACPI_HOST_PCI(8, uacpi_u8)
ACPI_HOST_PCI(16, uacpi_u16)
ACPI_HOST_PCI(32, uacpi_u32)
#undef ACPI_HOST_PCI
}
