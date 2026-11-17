// SPDX-License-Identifier: Apache-2.0
// Native partition operations resolve only admitted, same-owner disk handles.
#include "../../driver/fs/ext4/format.h"
#include <errno.h>
#include <fs/ext4/ext4.h>
#include <fs/vfs/vfs.h>
#include <installer/esp.h>
#include <installer/jobs.h>
#include <installer/native_protocol.h>
#include <installer/partition_install.h>
#include <ioctl.h>
#include <krlibc.h>
#include <proto.hpp>
#include <task/pcb.h>

extern int installer_format_ntfs_window(const BlockAccessWindow *) __attribute__((weak));

namespace {
using namespace xjinstaller;
using namespace xjinstaller::partition;

class StorageScope {
    tcb_t task;

  public:
    StorageScope() : task(get_current_task())
    {
        if (task) ++task->ime_storage_depth;
    }
    ~StorageScope()
    {
        if (task) --task->ime_storage_depth;
    }
};

struct Access
{
    vfs_node_t       node   = nullptr;
    BlockExtent      whole  = {};
    BlockAccessToken lease  = 0;
    bool             failed = false, written = false;

    int begin(vfs_node_t selected, const BlockExtent &extent)
    {
        node      = selected;
        whole     = extent;
        int error = block_access_require_flush(whole);
        return error ? error : block_access_claim(whole, true, &lease);
    }

    bool valid(uint64_t offset, size_t count)
    {
        BlockExtent current{};
        if (failed) return false;
        if (offset > whole.bytes || count > whole.bytes - offset || block_access_resolve_node(node, &current) ||
            current.generation != whole.generation || current.physical_generation != whole.physical_generation ||
            current.device != whole.device || current.physical_device != whole.physical_device ||
            current.start != whole.start || current.bytes != whole.bytes ||
            block_access_validate_owned_exclusive(lease, whole, offset, count))
            return fail();
        return true;
    }

    bool fail()
    {
        failed = true;
        if (lease) block_access_poison(lease);
        return false;
    }

    static bool read(void *opaque, uint64_t offset, void *data, size_t bytes)
    {
        auto        &a = *static_cast<Access *>(opaque);
        StorageScope scope;
        return a.valid(offset, bytes) && (vfs_read(a.node, data, offset, bytes) == bytes || a.fail());
    }

    static bool write(void *opaque, uint64_t offset, const void *data, size_t bytes)
    {
        auto &a = *static_cast<Access *>(opaque);
        if (!a.valid(offset, bytes)) return false;
        a.written = true;
        StorageScope scope;
        return vfs_write(a.node, const_cast<void *>(data), offset, bytes) == bytes || a.fail();
    }

    static bool flush(void *opaque)
    {
        auto        &a = *static_cast<Access *>(opaque);
        StorageScope scope;
        return a.valid(0, a.whole.bytes) && (vfs_ioctl(a.node, IOBLKSYNC, nullptr) == 0 || a.fail());
    }

    Device device()
    {
        return {this, whole.bytes, read, write, flush};
    }
    void finish()
    {
        if (lease && !failed)
        {
            if (block_access_release(lease))
                fail();
            else
                lease = 0;
        }
    }
};

int borrow(installer::JobHandle handle, uint64_t epoch, Access &access)
{
    vfs_node_t  node = nullptr;
    BlockExtent extent{};
    int         error = installer::jobs_borrow_target(handle, epoch, &node, &extent);
    if (!error)
    {
        error = access.begin(node, extent);
        if (error) installer::jobs_return_target(handle);
    }
    return error;
}

int finish(installer::JobHandle handle, Access &a, int error)
{
    a.finish();
    int result = installer::jobs_return_target(handle, a.failed);
    return result ? result : error;
}

int load_table(Access &access, Table &table, installer::JobHandle handle, const char *role)
{
    LoadDiagnostic diagnostic{};
    int error = load(access.device(), table, &diagnostic);
    if (error)
    {
        write_serial_fmt("installer: partition reject handle=%llu role=%s reason=%s error=%d\n",
                         (unsigned long long)handle, role,
                         diagnostic.reason ? diagnostic.reason : "table-validation", error);
        write_serial_fmt("installer: partition field lba=%llu slot=%u actual=0x%llx expected=0x%llx bytes=%llu\n",
                         (unsigned long long)diagnostic.lba, diagnostic.slot,
                         (unsigned long long)diagnostic.actual, (unsigned long long)diagnostic.expected,
                         (unsigned long long)access.whole.bytes);
    }
    else if (diagnostic.pmbr_size_saturated)
    {
        write_serial_fmt("installer: partition compat handle=%llu role=%s pmbr-size=0xffffffff disk-last-lba=%llu "
                         "GPT-verified\n", (unsigned long long)handle, role,
                         (unsigned long long)(access.whole.bytes / 512 - 1));
    }
    return error;
}

bool pin(vfs_node_t node)
{
    vfs_child_lock();
    bool allowed = !vfs_node_retiring_locked(node) && node->refcount != uint32_t(-1);
    if (allowed) ++node->refcount;
    vfs_child_unlock();
    return allowed;
}

Filesystem filesystem(Access &a, const Entry &e)
{
    alignas(16) uint8_t *data = static_cast<uint8_t *>(malloc(4096));
    if (!data) return Filesystem::Unknown;
    Filesystem fs = Filesystem::Unknown;
    if ((e.last - e.first + 1) * 512 >= 4096 && Access::read(&a, e.first * 512, data, 4096)) fs = identify(data, 4096);
    if (!a.failed) free(data); // Failed DMA buffers stay pinned to reboot.
    return fs;
}

bool add(xj_inst_partition_layout &out, uint64_t first, uint64_t last, unsigned number, unsigned fs, unsigned flags)
{
    if (first > last || out.count == XJ_INST_NATIVE_PARTITION_ROWS) return false;
    auto &row      = out.rows[out.count++];
    row.first_lba  = first;
    row.sectors    = last - first + 1;
    row.number     = number;
    row.filesystem = fs;
    row.flags      = flags;
    return true;
}

// Use the formatter's own pure planner before changing the GPT type. The
// callbacks deliberately fail: planning must never issue device requests.
int format_preflight(const PartitionInstall &plan, const installer::JobRequest &job)
{
    Entry selected{};
    int   error = entry(plan.after, plan.root_number, selected);
    if (error || !selected.occupied) return error ? error : -EINVAL;
    uint64_t bytes = (selected.last - selected.first + 1) * 512;
    if (plan.tool_filesystem == 1 || plan.tool_filesystem == 3)
        return bytes < (plan.tool_filesystem == 1 ? 64 * MiB : MiB) || bytes / 512 >= 0x100000000ULL ? -EINVAL : 0;
    if (plan.tool_filesystem == 4)
        return !installer_format_ntfs_window                               ? -EOPNOTSUPP
               : bytes < 32 * MiB || (bytes / 512 - 1) / 8 > 0xffffffffULL ? -EINVAL
                                                                           : 0;
    auto *work = static_cast<xjext4::FormatWorkspace *>(malloc(sizeof(xjext4::FormatWorkspace)));
    if (!work) return -ENOMEM;
    xjext4::FormatStorage storage{};
    error = xjext4::format_storage_size(bytes, storage.bytes);
    if (!error && storage.bytes)
    {
        storage.data = malloc(storage.bytes);
        if (!storage.data) error = -ENOMEM;
    }
    xjext4::FormatOptions options{};
    memcpy(options.uuid, job.factory.filesystem_uuid, 16);
    options.time_seconds = job.factory.time_seconds;
    options.journal_mib  = job.factory.journal_mib;
    xjext4::FormatGeometry   geometry{};
    xjext4::Ext4FormatDevice device{nullptr, [](void *, uint64_t, void *, size_t) { return false; },
                                    [](void *, uint64_t, const void *, size_t) { return false; },
                                    [](void *) { return false; }, bytes};
    if (!error) error = xjext4::plan_format(device, options, *work, geometry, &storage);
    free(storage.data);
    free(work);
    return error;
}
} // namespace

int installer_partition_layout(const xj_inst_partition_request *request, xj_inst_partition_layout *output)
{
    if (!request || !output || request->action || request->partition || request->esp_handle || request->esp_partition ||
        request->esp_fingerprint || request->first_lba || request->sectors || request->filesystem ||
        request->fingerprint)
    {
        write_serial_string("installer: partition reject reason=layout-request-fields error=-22\n");
        return -EINVAL;
    }
    auto *table = static_cast<Table *>(calloc(1, sizeof(Table)));
    if (!table) return -ENOMEM;
    Access access;
    int    error = borrow(request->target_handle, request->session_epoch, access);
    if (error)
    {
        write_serial_fmt("installer: partition reject handle=%llu reason=borrow-target error=%d\n",
                         request->target_handle, error);
        free(table);
        return error;
    }
    error = load_table(access, *table, request->target_handle, "target");
    if (!error)
    {
        xj_inst_native_initialize(output, sizeof(*output));
        output->session_epoch = request->session_epoch;
        output->fingerprint   = table->fingerprint;
        output->disk_bytes    = table->bytes;
        output->scheme        = unsigned(table->scheme);
        uint64_t cursor       = 0;
        // Sort presentation by location while retaining the GPT slot number.
        for (unsigned pass = 0; pass < table->count; ++pass)
        {
            Entry chosen{};
            bool  have = false;
            for (unsigned n = 1; n <= table->count; ++n)
            {
                Entry e{};
                if (entry(*table, n, e) || !e.occupied || e.first < cursor) continue;
                if (!have || e.first < chosen.first)
                {
                    chosen = e;
                    have   = true;
                }
            }
            if (!have) break;
            if (table->scheme == Scheme::Gpt && chosen.first > cursor)
            {
                uint64_t start = cursor > table->first ? cursor : table->first;
                start          = (start + Alignment - 1) / Alignment * Alignment;
                uint64_t end   = chosen.first / Alignment * Alignment;
                if (end > start) add(*output, start, end - 1, 0, 0, XJ_INST_NATIVE_PARTITION_FREE);
            }
            unsigned flags = (chosen.esp ? XJ_INST_NATIVE_PARTITION_ESP : 0) |
                             (table->scheme != Scheme::Gpt ? XJ_INST_NATIVE_PARTITION_READONLY : 0);
            add(*output, chosen.first, chosen.last, chosen.number, unsigned(filesystem(access, chosen)), flags);
            cursor = chosen.last + 1;
            // The next occupied entry can be unaligned. Do not round it away.
        }
        if (table->scheme == Scheme::Gpt || table->scheme == Scheme::Empty)
        {
            if (cursor < table->first) cursor = table->first;
            cursor       = ((cursor + Alignment - 1) / Alignment) * Alignment;
            uint64_t end = (table->last + 1) / Alignment * Alignment;
            if (end > cursor) add(*output, cursor, end - 1, 0, 0, XJ_INST_NATIVE_PARTITION_FREE);
        }
        if (access.failed) error = -EIO;
    }
    error = finish(request->target_handle, access, error);
    write_serial_fmt("installer: partition layout handle=%llu scheme=%u rows=%u result=%d\n", request->target_handle,
                     unsigned(table->scheme), output->count, error);
    if (!access.failed) free(table);
    return error;
}

void installer_release_partition_plan(const xjinstaller::PartitionInstall *selected)
{
    if (!selected) return;
    if (selected->esp_node) vfs_close(selected->esp_node);
    free(const_cast<PartitionInstall *>(selected));
}

int installer_partition_prepare(const xj_inst_partition_request *request, bool install, installer::JobRequest *job)
{
    if (!request || !job || !request->fingerprint || !request->target_handle || request->filesystem < 1 ||
        request->filesystem > 4 || (install && (request->action || request->filesystem != 2)) ||
        (!install && (request->action < 1 || request->action > 3 || request->esp_handle || request->esp_partition ||
                      request->esp_fingerprint)))
        return -EINVAL;
    auto *plan = static_cast<PartitionInstall *>(calloc(1, sizeof(PartitionInstall)));
    if (!plan) return -ENOMEM;
    Access access;
    int    error = borrow(request->target_handle, request->session_epoch, access);
    if (error)
    {
        free(plan);
        return error;
    }
    error = load_table(access, plan->before, request->target_handle, "target");
    if (!error && plan->before.fingerprint != request->fingerprint) error = -ESTALE;
    if (!error && plan->before.scheme != Scheme::Gpt && plan->before.scheme != Scheme::Empty) error = -EOPNOTSUPP;
    plan->after = plan->before;
    if (!error && plan->before.scheme == Scheme::Empty)
    {
        if (request->partition || (!install && request->action != XJ_INST_NATIVE_PARTITION_CREATE))
            error = -EINVAL;
        else
            error = initialize(plan->before.bytes, job->factory.identities.disk, plan->after);
    }
    plan->root_number     = request->partition;
    plan->tool_filesystem = request->filesystem;
    if (!error && (!request->partition || (!install && request->action == XJ_INST_NATIVE_PARTITION_CREATE)))
    {
        if (request->partition || !request->first_lba || !request->sectors)
            error = -EINVAL;
        else
            error = create(plan->after, request->first_lba, request->sectors, job->factory.identities.root,
                           Filesystem(request->filesystem), false, plan->root_number);
    }
    else if (!error && (request->first_lba || request->sectors))
        error = -EINVAL;
    if (!error && !install && request->action == XJ_INST_NATIVE_PARTITION_DELETE)
        error = erase(plan->after, plan->root_number);
    else if (!error && (!install || !request->esp_handle))
    {
        if (install)
            error = split_for_install(plan->after, plan->root_number, job->factory.identities.esp, plan->esp_number);
        else
            error = set_filesystem(plan->after, plan->root_number, Filesystem(request->filesystem));
    }
    else if (!error)
        error = set_filesystem(plan->after, plan->root_number, Filesystem::Ext4);
    if (!error && !install && request->action == XJ_INST_NATIVE_PARTITION_FORMAT) error = format_preflight(*plan, *job);
    vfs_node_t root_node = access.node;
    if (!error && install && (!request->esp_handle || request->esp_handle == request->target_handle))
    {
        plan->esp_before = plan->before;
        if (request->esp_handle)
        {
            Entry esp{};
            if (request->esp_fingerprint != plan->before.fingerprint ||
                entry(plan->before, request->esp_partition, esp) || !esp.occupied || !esp.esp ||
                request->esp_partition == plan->root_number || filesystem(access, esp) != Filesystem::Fat32)
                error = -EINVAL;
            else
            {
                plan->esp_number = request->esp_partition;
                plan->reuse_esp  = true;
            }
        }
        if (!error)
        {
            if (!pin(root_node))
                error = -EBUSY;
            else
                plan->esp_node = root_node;
        }
    }
    error = finish(request->target_handle, access, error);
    if (!error && install && request->esp_handle && request->esp_handle != request->target_handle)
    {
        Access other;
        error = borrow(request->esp_handle, request->session_epoch, other);
        if (!error)
        {
            error = load_table(other, plan->esp_before, request->esp_handle, "esp");
            Entry esp{};
            if (!error && (other.whole.physical_device == access.whole.physical_device ||
                           plan->esp_before.fingerprint != request->esp_fingerprint ||
                           entry(plan->esp_before, request->esp_partition, esp) || !esp.occupied || !esp.esp ||
                           filesystem(other, esp) != Filesystem::Fat32))
                error = -EINVAL;
            if (!error)
            {
                if (!pin(other.node))
                    error = -EBUSY;
                else
                {
                    plan->esp_node      = other.node;
                    plan->esp_number    = request->esp_partition;
                    plan->separate_disk = plan->reuse_esp = true;
                }
            }
            error = finish(request->esp_handle, other, error);
            if (other.failed) return error; // Keep every DMA-visible table/node.
        }
    }
    if (error)
    {
        if (!access.failed) installer_release_partition_plan(plan);
        return error;
    }
    job->action            = install ? 0 : request->action;
    job->factory.partition = plan;
    return 0;
}

int installer_run_partition_tool(vfs_node_t node, const installer::JobRequest *job, xjinstaller::FactoryStatus *status)
{
    if (!node || !job || !status || !job->factory.partition || !job->action) return -EINVAL;
    const auto &plan    = *job->factory.partition;
    auto       *access  = static_cast<Access *>(calloc(1, sizeof(Access)));
    auto       *scratch = static_cast<Table *>(calloc(1, sizeof(Table)));
    if (!access || !scratch)
    {
        free(access);
        free(scratch);
        return -ENOMEM;
    }
    BlockExtent extent{};
    int         error = block_access_resolve_node(node, &extent);
    if (!error) error = access->begin(node, extent);
    if (!error)
    {
        status->phase = FactoryPhase::GptWritten;
        if (same(plan.before, plan.after))
        {
            error = load(access->device(), *scratch);
            if (!error && !same(plan.before, *scratch)) error = -ESTALE;
        }
        else
            error = commit(access->device(), plan.before, plan.after, *scratch);
        status->io_started = access->written;
    }
    if (!error && job->action == XJ_INST_NATIVE_PARTITION_FORMAT)
    {
        Entry e{};
        error = entry(plan.after, plan.root_number, e);
        BlockAccessWindow window{node, extent, access->lease, e.first * 512, (e.last - e.first + 1) * 512};
        if (!error && plan.tool_filesystem == 2)
        {
            ext4_format_options options{};
            memcpy(options.uuid, job->factory.filesystem_uuid, 16);
            options.time_seconds = job->factory.time_seconds;
            options.journal_mib  = job->factory.journal_mib;
            memcpy(options.label, "XJ380", 5);
            ext4_format_status result{};
            error           = ext4fs_format_window(&window, &options, &result);
            access->failed |= result.io_failed;
        }
        else if (!error && (plan.tool_filesystem == 1 || plan.tool_filesystem == 3))
        {
            EspBuildStatus result{};
            error           = fatfs_format_window(&window, plan.tool_filesystem, &result);
            access->failed |= result.io_failed || result.quarantined;
        }
        else if (!error)
            error = installer_format_ntfs_window ? installer_format_ntfs_window(&window) : -EOPNOTSUPP;
        // A formatter can publish valid metadata before its final flush fails.
        // Never release the parent after a failed child operation.
        if (error) access->failed = true;
        status->io_started = true;
    }
    status->quarantined = error && (access->written || access->failed);
    if (status->quarantined)
        block_access_poison(access->lease);
    else
        access->finish();
    if (access->failed && !error) error = -EIO;
    status->io_failed    = access->failed;
    status->quarantined |= error && access->failed;
    status->complete = status->activation_durable = !error;
    status->error                                 = error;
    if (!status->quarantined)
    {
        free(scratch);
        free(access);
    }
    return error;
}
