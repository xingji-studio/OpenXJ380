#include "ec_protocol.h"
#include "platform.h"

#include <krlibc.h>
#include <proto.hpp>
#include <uacpi/acpi.h>
#include <uacpi/kernel_api.h>
#include <uacpi/opregion.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

namespace {
struct EcRegister
{
    uint64_t          address;
    uacpi_handle      io;
    volatile uint8_t *memory;
};

struct Controller
{
    EcRegister            data, control;
    uacpi_handle          mutex;
    bool                  global_lock;
    uacpi_namespace_node *node;
    uacpi_handle          query_work;
    uint32_t              query_pending, query_failed;
};

static Controller controllers[8];
static unsigned   controller_count;
static char       early_path[256];
static uint32_t   queries_enabled;

static uint8_t read_register(const EcRegister &reg)
{
    uint8_t value = 0xFF;
    if (reg.memory) return *reg.memory;
    uacpi_kernel_io_read8(reg.io, 0, &value);
    return value;
}

static void write_register(const EcRegister &reg, uint8_t value)
{
    if (reg.memory)
        *reg.memory = value;
    else
        uacpi_kernel_io_write8(reg.io, 0, value);
}

static bool map_register(EcRegister *reg, const acpi_gas &gas)
{
    if (!gas.address || gas.register_bit_offset || gas.register_bit_width != 8 ||
        (gas.access_size && gas.access_size != 1))
        return false;
    reg->address = gas.address;
    if (gas.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO)
        return uacpi_kernel_io_map(gas.address, 1, &reg->io) == UACPI_STATUS_OK;
    if (gas.address_space_id != UACPI_ADDRESS_SPACE_SYSTEM_MEMORY) return false;
    void *memory = uacpi_kernel_map(gas.address, 1);
    if (memory == UACPI_MAP_FAILED) return false;
    reg->memory = (volatile uint8_t *)memory;
    return true;
}

static void release_register(EcRegister &reg)
{
    if (reg.io) uacpi_kernel_io_unmap(reg.io);
    if (reg.memory) uacpi_kernel_unmap((void *)reg.memory, 1);
    reg = {};
}

static Controller *create_controller(const acpi_gas &data, const acpi_gas &control)
{
    if (controller_count == 8 || (data.address == control.address && data.address_space_id == control.address_space_id))
        return nullptr;
    Controller *ec = &controllers[controller_count];
    if (!map_register(&ec->data, data) || !map_register(&ec->control, control))
    {
        release_register(ec->data);
        release_register(ec->control);
        return nullptr;
    }
    ec->mutex      = uacpi_kernel_create_semaphore(1);
    ec->query_work = uacpi_kernel_create_work_item();
    if (!ec->mutex || !ec->query_work)
    {
        if (ec->mutex) uacpi_kernel_free_semaphore(ec->mutex);
        if (ec->query_work) uacpi_kernel_free_work_item(ec->query_work);
        release_register(ec->data);
        release_register(ec->control);
        *ec = {};
        return nullptr;
    }
    ++controller_count;
    return ec;
}

static AcpiEcTransport transport(Controller *ec)
{
    return {ec,
            [](void *p) { return read_register(((Controller *)p)->control); },
            [](void *p) { return read_register(((Controller *)p)->data); },
            [](void *p, uint8_t v) { write_register(((Controller *)p)->control, v); },
            [](void *p, uint8_t v) { write_register(((Controller *)p)->data, v); },
            [](void *) { return (uint64_t)uacpi_kernel_get_nanoseconds_since_boot(); },
            [](void *) { uacpi_kernel_stall(10); }};
}

static uacpi_status lock_controller(Controller *ec, uint32_t *sequence, bool *global)
{
    *global = false;
    if (ec->global_lock && !acpi_aml_owns_global_lock())
    {
        uacpi_status status = uacpi_acquire_global_lock(1000, sequence);
        if (status != UACPI_STATUS_OK) return status;
        *global = true;
    }
    uacpi_status status = uacpi_kernel_wait_for_semaphore(ec->mutex, 1000);
    if (status != UACPI_STATUS_OK && *global) uacpi_release_global_lock(*sequence);
    return status;
}

static void unlock_controller(Controller *ec, uint32_t sequence, bool global)
{
    uacpi_kernel_signal_semaphore(ec->mutex);
    if (global) uacpi_release_global_lock(sequence);
}

static void query_worker(uacpi_handle context)
{
    auto *ec = (Controller *)context;
    if (__atomic_load_n(&queries_enabled, __ATOMIC_ACQUIRE))
    {
        uint32_t     sequence = 0;
        bool         global   = false;
        uacpi_status status   = lock_controller(ec, &sequence, &global);
        uint8_t      query    = 0;
        if (status == UACPI_STATUS_OK)
        {
            if ((read_register(ec->control) & 0x20) && !acpi_ec_query(transport(ec), &query))
                status = UACPI_STATUS_HARDWARE_TIMEOUT;
            unlock_controller(ec, sequence, global);
        }
        if (status != UACPI_STATUS_OK)
        {
            __atomic_store_n(&ec->query_failed, 1, __ATOMIC_RELEASE);
            write_serial_fmt("ACPI EC: query delivery stopped after %s.\n", uacpi_status_to_string(status));
        }
        else if (query)
        {
            static const char hex[]    = "0123456789ABCDEF";
            char              method[] = "_Q00";
            method[2]                  = hex[query >> 4];
            method[3]                  = hex[query & 15];
            // _Qxx can read EC fields itself. Drop both transport locks first.
            status = uacpi_execute_simple(ec->node, method);
            if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND)
                write_serial_fmt("ACPI EC: %s failed: %s.\n", method, uacpi_status_to_string(status));
        }
    }
    __atomic_store_n(&ec->query_pending, 0, __ATOMIC_RELEASE);
}

static uacpi_status region_handler(uacpi_region_op operation, uacpi_handle argument)
{
    if (operation == UACPI_REGION_OP_ATTACH)
    {
        auto *attach = (uacpi_region_attach_data *)argument;
        if (attach->generic_info.base > 255 || !attach->generic_info.length ||
            attach->generic_info.length > 256 - attach->generic_info.base)
            return UACPI_STATUS_INVALID_ARGUMENT;
        attach->out_region_context = attach->handler_context;
        return UACPI_STATUS_OK;
    }
    if (operation == UACPI_REGION_OP_DETACH) return UACPI_STATUS_OK;
    if (operation != UACPI_REGION_OP_READ && operation != UACPI_REGION_OP_WRITE) return UACPI_STATUS_UNIMPLEMENTED;
    auto *request = (uacpi_region_rw_data *)argument;
    auto *ec      = (Controller *)request->handler_context;
    if (!ec || !request->byte_width || request->byte_width > 8 || request->address > 255 ||
        request->byte_width > 256 - request->address)
        return UACPI_STATUS_INVALID_ARGUMENT;
    uint32_t     global_sequence = 0;
    bool         locked_global   = false;
    // Global lock precedes the controller mutex: a Lock field can enter this
    // callback already holding _GL. Reversing them deadlocks mixed Lock and
    // NoLock fields on the same controller.
    uacpi_status status = lock_controller(ec, &global_sequence, &locked_global);
    if (status != UACPI_STATUS_OK) return status;
    {
        auto     io    = transport(ec);
        uint64_t value = operation == UACPI_REGION_OP_READ ? 0 : request->value;
        for (unsigned i = 0; i != request->byte_width; ++i)
        {
            uint8_t byte = (uint8_t)(value >> (i * 8));
            bool    okay = operation == UACPI_REGION_OP_READ
                               ? acpi_ec_read_byte(io, (uint8_t)(request->address + i), &byte)
                               : acpi_ec_write_byte(io, (uint8_t)(request->address + i), byte);
            if (!okay)
            {
                status = UACPI_STATUS_HARDWARE_TIMEOUT;
                break;
            }
            if (operation == UACPI_REGION_OP_READ) value |= (uint64_t)byte << (i * 8);
        }
        if (status == UACPI_STATUS_OK && operation == UACPI_REGION_OP_READ) request->value = value;
    }
    unlock_controller(ec, global_sequence, locked_global);
    return status;
}

struct IoResources
{
    uint16_t addresses[2];
    unsigned count;
    bool     invalid;
};

static uacpi_iteration_decision collect_io(void *opaque, uacpi_resource *resource)
{
    auto    *result = (IoResources *)opaque;
    uint16_t address;
    if (resource->type == UACPI_RESOURCE_TYPE_IO)
    {
        if (resource->io.minimum != resource->io.maximum || resource->io.length != 1)
        {
            result->invalid = true;
            return UACPI_ITERATION_DECISION_BREAK;
        }
        address = resource->io.minimum;
    }
    else if (resource->type == UACPI_RESOURCE_TYPE_FIXED_IO)
    {
        if (resource->fixed_io.length != 1)
        {
            result->invalid = true;
            return UACPI_ITERATION_DECISION_BREAK;
        }
        address = resource->fixed_io.address;
    }
    else
        return UACPI_ITERATION_DECISION_CONTINUE;
    if (result->count == 2)
        result->invalid = true;
    else
        result->addresses[result->count++] = address;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision install_device(void *opaque, uacpi_namespace_node *node, uacpi_u32)
{
    static const char *const identifiers[] = {"PNP0C09", nullptr};
    if (!uacpi_device_matches_pnp_id(node, identifiers)) return UACPI_ITERATION_DECISION_CONTINUE;
    uint32_t     state  = 0;
    uacpi_status status = uacpi_eval_sta(node, &state);
    if (status == UACPI_STATUS_OK && !(state & ACPI_STA_RESULT_DEVICE_PRESENT))
        return UACPI_ITERATION_DECISION_NEXT_PEER;
    IoResources resources{};
    status = uacpi_for_each_device_resource(node, "_CRS", collect_io, &resources);
    if (status != UACPI_STATUS_OK || resources.invalid || resources.count != 2)
    {
        *(uacpi_status *)opaque = status == UACPI_STATUS_OK ? UACPI_STATUS_AML_INVALID_RESOURCE : status;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    Controller *ec = nullptr;
    for (unsigned i = 0; i != controller_count; ++i)
        if (controllers[i].data.io && controllers[i].control.io &&
            controllers[i].data.address == resources.addresses[0] &&
            controllers[i].control.address == resources.addresses[1])
            ec = &controllers[i];
    if (!ec)
    {
        acpi_gas data{1, 8, 0, 1, resources.addresses[0]};
        acpi_gas control{1, 8, 0, 1, resources.addresses[1]};
        ec = create_controller(data, control);
    }
    if (!ec)
    {
        *(uacpi_status *)opaque = UACPI_STATUS_MAPPING_FAILED;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    uint64_t global_lock = 0;
    status               = uacpi_eval_simple_integer(node, "_GLK", &global_lock);
    if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND)
    {
        *(uacpi_status *)opaque = status;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    ec->node        = node;
    ec->global_lock = global_lock != 0;
    status = uacpi_install_address_space_handler(node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, region_handler, ec);
    if (status != UACPI_STATUS_OK) *(uacpi_status *)opaque = status;
    write_serial_fmt("ACPI EC: _CRS data=0x%llx control=0x%llx global-lock=%u status=%s\n", ec->data.address,
                     ec->control.address, ec->global_lock, uacpi_status_to_string(status));
    return status == UACPI_STATUS_OK ? UACPI_ITERATION_DECISION_CONTINUE : UACPI_ITERATION_DECISION_BREAK;
}
} // namespace

uacpi_status acpi_ec_install_early()
{
    uacpi_table  table{};
    uacpi_status status = uacpi_table_find_by_signature("ECDT", &table);
    if (status == UACPI_STATUS_NOT_FOUND) return UACPI_STATUS_OK;
    if (status != UACPI_STATUS_OK) return status;
    auto *ecdt = (const acpi_ecdt *)table.ptr;
    if (ecdt->hdr.length <= sizeof(acpi_ecdt) || ecdt->hdr.length - sizeof(acpi_ecdt) > sizeof(early_path))
        status = UACPI_STATUS_INVALID_TABLE_LENGTH;
    else
    {
        size_t length = ecdt->hdr.length - sizeof(acpi_ecdt);
        if (!memchr(ecdt->ec_id, 0, length))
            status = UACPI_STATUS_INVALID_ARGUMENT;
        else
        {
            memcpy(early_path, ecdt->ec_id, length);
            Controller *ec = create_controller(ecdt->ec_data, ecdt->ec_control);
            if (!ec)
                status = UACPI_STATUS_MAPPING_FAILED;
            else
            {
                // _GLK does not exist until the namespace is loaded. Use the
                // firmware lock conservatively for any earlier EC accesses.
                ec->global_lock = true;
                status          = uacpi_install_address_space_handler(
                    uacpi_namespace_root(), UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, region_handler, ec);
                write_serial_fmt("ACPI EC: ECDT data=0x%llx control=0x%llx status=%s\n", ec->data.address,
                                 ec->control.address, uacpi_status_to_string(status));
            }
        }
    }
    uacpi_table_unref(&table);
    return status;
}

uacpi_status acpi_ec_install_namespace()
{
    uacpi_status result = UACPI_STATUS_OK;
    uacpi_status status = uacpi_namespace_for_each_child(uacpi_namespace_root(), install_device, nullptr,
                                                         UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, &result);
    if (status != UACPI_STATUS_OK) return status;
    if (result != UACPI_STATUS_OK) return result;
    if (early_path[0] && controller_count && !controllers[0].node)
    {
        status = uacpi_namespace_node_find(nullptr, early_path, &controllers[0].node);
        if (status != UACPI_STATUS_OK) return status;
        uint64_t global_lock = 0;
        status               = uacpi_eval_simple_integer(controllers[0].node, "_GLK", &global_lock);
        if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND) return status;
        controllers[0].global_lock = global_lock != 0;
    }
    return UACPI_STATUS_OK;
}

void acpi_ec_enable_queries(bool enabled)
{
    __atomic_store_n(&queries_enabled, enabled ? 1 : 0, __ATOMIC_RELEASE);
}

void acpi_ec_poll_queries()
{
    if (!__atomic_load_n(&queries_enabled, __ATOMIC_ACQUIRE)) return;
    for (unsigned i = 0; i != controller_count; ++i)
    {
        auto *ec = &controllers[i];
        if (!ec->node || __atomic_load_n(&ec->query_failed, __ATOMIC_ACQUIRE) || !(read_register(ec->control) & 0x20))
            continue;
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&ec->query_pending, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, ec->query_work, query_worker, ec);
    }
}
