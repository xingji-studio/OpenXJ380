// SPDX-License-Identifier: Apache-2.0
// Original fixed user boundary and kernel-owned native installer dispatch.

#include <installer/service.h>
#include <installer/native_protocol.h>
#include <installer/jobs.h>
#include <efi/installer_handoff.h>
#include <block_access.h>
#include <cpu/lock.h>
#include <device.h>
#include <errno.h>
#include <krlibc.h>
#include <mm/uaccess.h>
#include <rng.h>
#include <rtc.h>
#include <task/kernel_worker.h>
#include <task/pcb.h>
#include <user/permissions.h>

namespace
{
using namespace installer;

static_assert(sizeof(xj_inst_native_header) == 24, "native header ABI");
static_assert(sizeof(xj_inst_native_info) == 64, "native info ABI");
static_assert(sizeof(xj_inst_native_target) == 88, "native target ABI");
static_assert(sizeof(xj_inst_native_targets) == 2856, "native targets ABI");
static_assert(sizeof(xj_inst_native_release_targets) == 288, "native target release ABI");
static_assert(sizeof(xj_inst_native_submit) == 64, "native submit ABI");
static_assert(sizeof(xj_inst_native_handle) == 40, "native handle ABI");
static_assert(sizeof(xj_inst_native_job) == 96, "native job ABI");

enum class TicketState : unsigned { Free, Preparing, Dispatched, Running, Done, Quarantined };
struct WorkerTicket
{
    TicketState state;
    JobHandle handle;
};
WorkerTicket worker_tickets[job_capacity];
spin_t ticket_lock = SPIN_INIT;

class TicketLock
{
public:
    TicketLock() { spin_lock(&ticket_lock); }
    ~TicketLock() { spin_unlock(&ticket_lock); }
};

// The existing general uaccess helpers check leaf USER but do not establish
// effective upper-level USER/WRITE permissions. This local boundary checks
// every level before copying; no arbitrary kernel-half address is accepted.
bool permitted_page(page_directory_t *directory, uint64_t address, bool write)
{
    if (!directory || !directory->table) return false;
    page_table_t *table = directory->table;
    for (unsigned level = 4; level; --level)
    {
        unsigned shift = 12 + 9 * (level - 1);
        uint64_t entry = table->entries[(address >> shift) & 511].value;
        uint64_t required = PTE_PRESENT | PTE_USER | (write ? PTE_WRITEABLE : 0);
        if ((entry & required) != required || (level == 4 && (entry & PTE_HUGE))) return false;
        if (level == 1 || (entry & PTE_HUGE)) return true;
        table = static_cast<page_table_t *>(phys_to_virt(entry & PTE_ADDR_MASK));
        if (!table) return false;
    }
    return false;
}

bool permitted_range(page_directory_t *directory, uint64_t address, size_t bytes, bool write)
{
    constexpr uint64_t user_limit = 1ULL << 47;
    if (!address || !bytes || address >= user_limit || bytes > user_limit - address) return false;
    uint64_t end = address + bytes - 1;
    for (uint64_t page = address & ~4095ULL;; page += 4096)
    {
        if (!fault_in_user_page(directory, page) || !permitted_page(directory, page, write)) return false;
        if (page == (end & ~4095ULL)) return true;
    }
}

bool write_response(page_directory_t *directory, uint64_t address, const void *data, size_t bytes)
{
    return permitted_range(directory, address, bytes, true) &&
           copy_to_user_pagedir(directory, reinterpret_cast<void *>(address), data, bytes);
}

bool valid_header(const xj_inst_native_header &header, size_t bytes)
{
    return header.version == XJ_INST_NATIVE_VERSION && header.bytes == bytes &&
           !header.reserved[0] && !header.reserved[1];
}

int live_session(JobSessionView *view, bool ready)
{
    *view = {};
    if (!session_job_view(view) || !view->epoch) return -ENODEV;
    if (!ready) return 0;
    if (view->profile != XJ_INSTALLER_PROFILE_EXT4_PAK2) return -EOPNOTSUPP;
    if (view->components != XJ_INST_NATIVE_COMPONENTS_FULL ||
        view->language != XJ_INST_NATIVE_LANGUAGE_PACKAGE) return -EOPNOTSUPP;
    if (!view->ready) return -ENODEV;
    return session_validate_job_view(*view);
}

int require_admin()
{
    return permissions_have(user_type_permission_mask(XUT_Admin)) ? 0 : -EACCES;
}

WorkerTicket *reserve_ticket()
{
    TicketLock lock;
    for (auto &ticket : worker_tickets)
    {
        if (ticket.state != TicketState::Free && ticket.state != TicketState::Done) continue;
        ticket.handle = 0;
        ticket.state = TicketState::Preparing;
        return &ticket;
    }
    return nullptr;
}

void finish_ticket(WorkerTicket *ticket, bool quarantined)
{
    TicketLock lock;
    ticket->state = quarantined ? TicketState::Quarantined : TicketState::Free;
    if (!quarantined) ticket->handle = 0;
}

void native_worker(void *argument)
{
    uintptr_t address = reinterpret_cast<uintptr_t>(argument);
    uintptr_t first = reinterpret_cast<uintptr_t>(worker_tickets);
    if (address < first || address - first >= sizeof(worker_tickets) ||
        (address - first) % sizeof(WorkerTicket)) return;
    auto *ticket = static_cast<WorkerTicket *>(argument);
    JobHandle handle = 0;
    {
        TicketLock lock;
        if (ticket->state != TicketState::Dispatched) return;
        ticket->state = TicketState::Running;
        handle = ticket->handle;
    }
    // No user pointer, caller credential or target lease is transferred. The
    // admitted job performs its fresh factory claim in this actual worker TCB.
    jobs_run(handle);
    TicketLock lock;
    ticket->state = TicketState::Done;
}

bool nonzero(const uint8_t *identity)
{
    uint8_t bits = 0;
    for (unsigned i = 0; i < 16; ++i) bits |= identity[i];
    return bits != 0;
}

int fresh_options(xjinstaller::FactoryOptions *options)
{
    for (unsigned attempt = 0; attempt < 8; ++attempt)
    {
        uint8_t identities[64];
        if (!get_hardware_random_bytes(identities, sizeof(identities))) return -EOPNOTSUPP;
        if (!nonzero(identities) || !nonzero(identities + 16) || !nonzero(identities + 32) ||
            !nonzero(identities + 48)) continue;
        bool distinct = true;
        for (unsigned a = 0; a < 4; ++a)
            for (unsigned b = a + 1; b < 4; ++b)
                if (!memcmp(identities + 16 * a, identities + 16 * b, 16)) distinct = false;
        if (!distinct) continue;
        *options = {};
        memcpy(options->identities.disk, identities, 16);
        memcpy(options->identities.root, identities + 16, 16);
        memcpy(options->identities.esp, identities + 32, 16);
        memcpy(options->filesystem_uuid, identities + 48, 16);
        // GPT identities use the defined on-disk little-endian GUID fields.
        uint8_t *guids[4] = {options->identities.disk, options->identities.root,
                             options->identities.esp, options->filesystem_uuid};
        for (unsigned index = 0; index < 3; ++index)
        {
            uint8_t *guid = guids[index];
            guid[7] = (guid[7] & 15) | 0x40;
            guid[8] = (guid[8] & 63) | 0x80;
        }
        options->filesystem_uuid[6] = (options->filesystem_uuid[6] & 15) | 0x40;
        options->filesystem_uuid[8] = (options->filesystem_uuid[8] & 63) | 0x80;
        for (unsigned a = 0; a < 4; ++a)
            for (unsigned b = a + 1; b < 4; ++b)
                if (!memcmp(guids[a], guids[b], 16)) distinct = false;
        if (!distinct) continue;
        options->time_seconds = realtime_ns() / 1000000000ULL;
        options->journal_mib = 6;
        options->esp_bytes = 64ULL << 20;
        return 0;
    }
    return -EIO;
}

int release_targets(const xj_inst_native_release_targets &request)
{
    if (request.reserved || request.count > XJ_INST_NATIVE_MAX_TARGETS) return -EINVAL;
    for (unsigned i = request.count; i < XJ_INST_NATIVE_MAX_TARGETS; ++i)
        if (request.handles[i]) return -EINVAL;
    int error = 0;
    for (unsigned i = 0; i < request.count; ++i)
    {
        if (!request.handles[i]) continue;
        int result = jobs_release_target(request.handles[i]);
        // A submitted target handle is consumed into its job. Retrying an
        // already released handle is harmless; cross-owner handles still fail.
        if (result && result != -ENOENT && !error) error = result;
    }
    return error;
}

int enumerate_targets(xj_inst_native_targets *output)
{
    xj_inst_native_initialize(output, sizeof(*output));
    JobSessionView view = {};
    int error = live_session(&view, true);
    if (!error) error = require_admin();
    if (error) return error;
    output->session_epoch = view.epoch;
    for (uint32_t device = 0; device < 256; ++device)
    {
        BlockAccessToken management = 0;
        error = block_access_management_begin(&management);
        if (error) return error;
        uint64_t generation = block_access_generation(device);
        BlockExtent extent = {};
        device_t *hardware = get_device(device);
        uint64_t bytes = 0;
        uint32_t sector_bytes = 0;
        char name[XJ_INST_NATIVE_NAME_BYTES] = {};
        if (generation && hardware && hardware->type == DEVICE_BLOCK &&
            !block_access_resolve(device, generation, &extent) && extent.device == extent.physical_device &&
            !extent.start && hardware->size == extent.bytes)
        {
            bytes = hardware->size;
            sector_bytes = hardware->sector_size <= __UINT32_MAX__ ? hardware->sector_size : 0;
            for (unsigned i = 0; i < sizeof(hardware->drive_name) && i + 1 < sizeof(name); ++i)
            {
                name[i] = hardware->drive_name[i];
                if (!name[i]) break;
            }
        }
        block_access_management_end(management);
        if (!bytes || sector_bytes != 512) continue;
        JobHandle handle = 0;
        error = jobs_select_target(device, generation, &handle);
        if (error == -ENODEV || error == -ESTALE || error == -EBUSY || error == -EOPNOTSUPP) continue;
        if (error) return error;
        auto &target = output->targets[output->count++];
        target.handle = handle;
        target.bytes = bytes;
        target.sector_bytes = sector_bytes;
        target.flags = XJ_INST_NATIVE_TARGET_WRITABLE | XJ_INST_NATIVE_TARGET_FLUSH;
        memcpy(target.name, name, sizeof(target.name));
        if (output->count == XJ_INST_NATIVE_MAX_TARGETS)
        {
            output->flags = XJ_INST_NATIVE_LIST_CAPACITY_REACHED;
            break;
        }
    }
    return 0;
}

int cleanup_enumeration(const xj_inst_native_targets &targets)
{
    int error = 0;
    for (unsigned i = 0; i < targets.count; ++i)
    {
        int result = jobs_release_target(targets.targets[i].handle);
        if (result && !error) error = result;
    }
    return error;
}

int cleanup_queued(JobHandle handle)
{
    int error = jobs_cancel_queued(handle);
    if (!error) error = jobs_release(handle);
    return error;
}

union Request
{
    xj_inst_native_header header;
    xj_inst_native_release_targets targets;
    xj_inst_native_submit submit;
    xj_inst_native_handle handle;
};
} // namespace

uint64_t do_xapi_InstallerNative(uint64_t operation, uint64_t request_address, uint64_t request_bytes,
                                uint64_t response_address, uint64_t response_bytes)
{
    using namespace installer;
    size_t expected_request = 0, expected_response = 0;
    switch (operation)
    {
    case XJ_INST_NATIVE_INFO:
        expected_request = sizeof(xj_inst_native_header);
        expected_response = sizeof(xj_inst_native_info);
        break;
    case XJ_INST_NATIVE_ENUMERATE:
        expected_request = sizeof(xj_inst_native_header);
        expected_response = sizeof(xj_inst_native_targets);
        break;
    case XJ_INST_NATIVE_RELEASE_TARGETS: expected_request = sizeof(xj_inst_native_release_targets); break;
    case XJ_INST_NATIVE_SUBMIT:
        expected_request = sizeof(xj_inst_native_submit);
        expected_response = sizeof(xj_inst_native_handle);
        break;
    case XJ_INST_NATIVE_QUERY:
        expected_request = sizeof(xj_inst_native_handle);
        expected_response = sizeof(xj_inst_native_job);
        break;
    case XJ_INST_NATIVE_CANCEL:
    case XJ_INST_NATIVE_RELEASE_JOB:
        expected_request = sizeof(xj_inst_native_handle);
        break;
    default: return uint64_t(-ENOSYS);
    }
    if (request_bytes != expected_request || response_bytes != expected_response ||
        (!expected_response && response_address)) return uint64_t(-EINVAL);
    tcb_t task = get_current_task();
    page_directory_t *directory = task && task->parent_group ? task->parent_group->pagedir : nullptr;
    Request request = {};
    if (!permitted_range(directory, request_address, expected_request, false) ||
        !copy_from_user_pagedir(directory, &request, reinterpret_cast<void *>(request_address), expected_request))
        return uint64_t(-EFAULT);
    if (request.header.version != XJ_INST_NATIVE_VERSION) return uint64_t(-EPROTONOSUPPORT);
    if (!valid_header(request.header, expected_request)) return uint64_t(-EINVAL);
    if (expected_response && !permitted_range(directory, response_address, expected_response, true))
        return uint64_t(-EFAULT);

    if (operation == XJ_INST_NATIVE_INFO)
    {
        JobSessionView view = {};
        int error = live_session(&view, false);
        if (error) return uint64_t(error);
        xj_inst_native_info response;
        xj_inst_native_initialize(&response, sizeof(response));
        response.session_epoch = view.epoch;
        response.components = view.components;
        response.package_bytes = view.package_bytes;
        response.filesystem = view.profile;
        response.language = view.language;
        response.flags = XJ_INST_NATIVE_INFO_SESSION;
        if (view.profile == XJ_INSTALLER_PROFILE_EXT4_PAK2)
        {
            response.flags |= XJ_INST_NATIVE_INFO_EXT4;
            if (view.ready && view.components == XJ_INST_NATIVE_COMPONENTS_FULL &&
                view.language == XJ_INST_NATIVE_LANGUAGE_PACKAGE && !session_validate_job_view(view))
                response.flags |= XJ_INST_NATIVE_INFO_READY;
        }
        return write_response(directory, response_address, &response, sizeof(response)) ? 0 : uint64_t(-EFAULT);
    }
    if (operation == XJ_INST_NATIVE_ENUMERATE)
    {
        xj_inst_native_targets response = {};
        int error = enumerate_targets(&response);
        if (!error && !write_response(directory, response_address, &response, sizeof(response))) error = -EFAULT;
        if (error)
        {
            int cleanup = cleanup_enumeration(response);
            if (cleanup) error = cleanup;
        }
        return uint64_t(error);
    }
    if (operation == XJ_INST_NATIVE_RELEASE_TARGETS) return uint64_t(release_targets(request.targets));
    if (operation == XJ_INST_NATIVE_SUBMIT)
    {
        JobSessionView view = {};
        int error = live_session(&view, true);
        if (!error) error = require_admin();
        if (error) return uint64_t(error);
        auto &input = request.submit;
        if (input.reserved) return uint64_t(-EINVAL);
        if (input.filesystem != XJ_INST_NATIVE_FILESYSTEM_EXT4 || input.mode != XJ_INST_NATIVE_MODE_FRESH ||
            input.language != XJ_INST_NATIVE_LANGUAGE_PACKAGE ||
            input.components != XJ_INST_NATIVE_COMPONENTS_FULL) return uint64_t(-EOPNOTSUPP);
        if (!input.target_handle || input.session_epoch != view.epoch) return uint64_t(-ESTALE);
        JobRequest job_request = {};
        job_request.mode = input.mode;
        job_request.language = input.language;
        job_request.components = input.components;
        error = fresh_options(&job_request.factory);
        if (error) return uint64_t(error);
        WorkerTicket *ticket = reserve_ticket();
        if (!ticket) return uint64_t(-ENOSPC);
        JobHandle job = 0;
        error = jobs_queue(input.target_handle, &job_request, &job);
        if (error) { finish_ticket(ticket, false); return uint64_t(error); }
        ticket->handle = job;
        xj_inst_native_handle response;
        xj_inst_native_initialize(&response, sizeof(response));
        response.session_epoch = view.epoch;
        response.handle = job;
        if (!write_response(directory, response_address, &response, sizeof(response)))
        {
            error = cleanup_queued(job);
            finish_ticket(ticket, error != 0);
            return uint64_t(error ? error : -EFAULT);
        }
        {
            TicketLock lock;
            ticket->state = TicketState::Dispatched;
        }
        size_t tid = create_kernel_worker(reinterpret_cast<void *>(native_worker), ticket, "Native installer");
        if (int64_t(tid) < 0)
        {
            error = cleanup_queued(job);
            finish_ticket(ticket, error != 0);
            xj_inst_native_initialize(&response, sizeof(response));
            write_response(directory, response_address, &response, sizeof(response));
            return uint64_t(error ? error : int64_t(tid));
        }
        return 0;
    }
    if (!request.handle.handle || !request.handle.session_epoch) return uint64_t(-EINVAL);
    if (operation == XJ_INST_NATIVE_CANCEL || operation == XJ_INST_NATIVE_RELEASE_JOB)
    {
        uint64_t epoch = 0;
        int error = jobs_owned_epoch(request.handle.handle, &epoch);
        if (error) return uint64_t(error);
        if (epoch != request.handle.session_epoch) return uint64_t(-ESTALE);
        return uint64_t(operation == XJ_INST_NATIVE_CANCEL ? jobs_cancel_queued(request.handle.handle) :
                                                           jobs_release(request.handle.handle));
    }
    JobSnapshot snapshot = {};
    int error = jobs_query(request.handle.handle, &snapshot);
    if (error) return uint64_t(error);
    if (snapshot.session_epoch != request.handle.session_epoch) return uint64_t(-ESTALE);
    xj_inst_native_job response;
    xj_inst_native_initialize(&response, sizeof(response));
    response.session_epoch = snapshot.session_epoch;
    response.handle = snapshot.handle;
    response.state = uint32_t(snapshot.state);
    response.phase = uint32_t(snapshot.factory.phase);
    response.error = snapshot.error;
    if (snapshot.outcome_unknown) response.flags |= XJ_INST_NATIVE_JOB_OUTCOME_UNKNOWN;
    if (snapshot.factory.io_started) response.flags |= XJ_INST_NATIVE_JOB_IO_STARTED;
    if (snapshot.factory.io_failed) response.flags |= XJ_INST_NATIVE_JOB_IO_FAILED;
    if (snapshot.factory.activation_attempted) response.flags |= XJ_INST_NATIVE_JOB_ACTIVATION_ATTEMPTED;
    if (snapshot.factory.activation_durable) response.flags |= XJ_INST_NATIVE_JOB_ACTIVATION_DURABLE;
    response.read_bytes = snapshot.factory.read_bytes;
    response.written_bytes = snapshot.factory.written_bytes;
    response.package_records = snapshot.factory.package_records;
    response.package_data_bytes = snapshot.factory.package_data_bytes;
    return write_response(directory, response_address, &response, sizeof(response)) ? 0 : uint64_t(-EFAULT);
}
