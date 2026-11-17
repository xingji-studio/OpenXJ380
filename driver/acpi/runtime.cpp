#include "platform.h"

#include <acpi/runtime.h>
#include <cpu/msr.h>
#include <proto.hpp>
#include <task/pcb.h>
#include <task/scheduler.h>
#include <uacpi/sleep.h>
#include <uacpi/uacpi.h>

namespace {
enum RuntimeState : uint32_t
{
    Off,
    Starting,
    Ready,
    Failed
};
static uint32_t runtime_state, preparation_state;
static uint16_t sleep_a, sleep_b;

static void initialize(void *)
{
    uacpi_status status = uacpi_initialize(UACPI_FLAG_BAD_CSUM_FATAL | UACPI_FLAG_BAD_TBL_SIGNATURE_FATAL |
                                           UACPI_FLAG_PROACTIVE_TBL_CSUM);
    const char  *step   = "initialize";
    if (status == UACPI_STATUS_OK)
    {
        step   = "early EC";
        status = acpi_ec_install_early();
    }
    if (status == UACPI_STATUS_OK)
    {
        step   = "namespace load";
        status = uacpi_namespace_load();
    }
    if (status == UACPI_STATUS_OK)
    {
        step   = "namespace EC";
        status = acpi_ec_install_namespace();
    }
    if (status == UACPI_STATUS_OK)
    {
        step   = "namespace initialize";
        status = uacpi_namespace_initialize();
    }
    if (status == UACPI_STATUS_OK && acpi_host_firmware_failed()) status = UACPI_STATUS_DENIED;
    if (status != UACPI_STATUS_OK)
    {
        write_serial_fmt("ACPI runtime: %s failed: %s; firmware shutdown remains available.\n", step,
                         uacpi_status_to_string(status));
        uacpi_state_reset();
        __atomic_store_n(&runtime_state, Failed, __ATOMIC_RELEASE);
        return;
    }
    write_serial_string("ACPI runtime: namespace and EC handlers ready.\n");
    acpi_ec_enable_queries(true);
    __atomic_store_n(&runtime_state, Ready, __ATOMIC_RELEASE);
}

static bool sleep_types()
{
    uacpi_object *object = nullptr;
    uacpi_status  status = uacpi_eval_simple_package(nullptr, "\\_S5", &object);
    if (status != UACPI_STATUS_OK) return false;
    uacpi_object_array package{};
    uint64_t           a = 0, b = 0;
    status     = uacpi_object_get_package(object, &package);
    bool valid = status == UACPI_STATUS_OK && package.count &&
                 uacpi_object_get_integer(package.objects[0], &a) == UACPI_STATUS_OK;
    if (valid && package.count > 1)
        valid = uacpi_object_get_integer(package.objects[1], &b) == UACPI_STATUS_OK;
    else if (valid)
    {
        b  = a >> 8;
        a &= 0xFF;
    }
    valid = valid && a <= 7 && b <= 7;
    uacpi_object_unref(object);
    if (!valid) return false;
    sleep_a = (uint16_t)a << 10;
    sleep_b = (uint16_t)b << 10;
    return true;
}
} // namespace

void acpi_runtime_start(const BOOT_CONFIG *boot)
{
    if (!boot || !(boot->boot_flags & BOOT_FLAG_ACPI_ROOT) || !boot->acpi_rsdp)
    {
        write_serial_string("ACPI runtime: loader supplies no RSDP; using legacy power support.\n");
        return;
    }
    // Creation pins the workers to this CPU. Ownership SMIs and AML/GPE work
    // must originate from the BSP; do not change task affinity or boot gates.
    if (!(rdmsr(0x1B) & (1u << 8)) || !nanoTime()) return;
    uint32_t expected = Off;
    if (!__atomic_compare_exchange_n(&runtime_state, &expected, Starting, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    if (!acpi_host_start(boot) ||
        (int64_t)create_kernel_thread((void *)initialize, nullptr, (char *)"acpi-init", nullptr) < 0)
    {
        __atomic_store_n(&runtime_state, Failed, __ATOMIC_RELEASE);
        write_serial_string("ACPI runtime: worker creation failed.\n");
    }
}

AcpiSleepPreparation acpi_runtime_prepare_s5(uint16_t *type_a, uint16_t *type_b)
{
    uint32_t state = __atomic_load_n(&runtime_state, __ATOMIC_ACQUIRE);
    if (state == Off) return AcpiSleepPreparation::Unavailable;
    if (!type_a || !type_b || !acpi_host_task_context()) return AcpiSleepPreparation::Failed;
    uint64_t start = nanoTime();
    while (state == Starting)
    {
        uint64_t now = nanoTime();
        if (now < start || now - start >= 30000000000ULL) return AcpiSleepPreparation::Failed;
        scheduler_sleep_ns(1000000);
        state = __atomic_load_n(&runtime_state, __ATOMIC_ACQUIRE);
    }
    if (state != Ready) return AcpiSleepPreparation::Failed;
    uint32_t expected = Off;
    if (__atomic_compare_exchange_n(&preparation_state, &expected, Starting, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        bool         valid = sleep_types();
        uacpi_status status =
            valid ? uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5) : UACPI_STATUS_AML_INCOMPATIBLE_OBJECT_TYPE;
        if (status == UACPI_STATUS_OK && acpi_host_firmware_failed()) status = UACPI_STATUS_DENIED;
        acpi_ec_enable_queries(false);
        if (!acpi_host_quiesce() && status == UACPI_STATUS_OK) status = UACPI_STATUS_TIMEOUT;
        write_serial_fmt("ACPI runtime: _PTS(5)/S5 preparation %s.\n", uacpi_status_to_string(status));
        __atomic_store_n(&preparation_state, status == UACPI_STATUS_OK ? Ready : Failed, __ATOMIC_RELEASE);
    }
    else
    {
        start = nanoTime();
        while (__atomic_load_n(&preparation_state, __ATOMIC_ACQUIRE) == Starting)
        {
            uint64_t now = nanoTime();
            if (now < start || now - start >= 30000000000ULL) return AcpiSleepPreparation::Failed;
            scheduler_sleep_ns(1000000);
        }
    }
    if (__atomic_load_n(&preparation_state, __ATOMIC_ACQUIRE) != Ready) return AcpiSleepPreparation::Failed;
    *type_a = sleep_a;
    *type_b = sleep_b;
    return AcpiSleepPreparation::Ready;
}
