// SPDX-License-Identifier: Apache-2.0
// Original synchronous fresh-disk coordinator. No external installer library.
#include "../../driver/fs/ext4/write.h"
#include <installer/factory.h>
#include <installer/ext4_import.h>
#include <installer/ext4_verify.h>
#include <installer/esp.h>
#include <installer/esp_verify.h>
#include <fs/ext4/ext4.h>
#include <device.h>
#include <cpu/regio.h>
#include <errno.h>
#include <input_method.h>
#include <ioctl.h>
#include <krlibc.h>
#include <task/pcb.h>
#include <task/scheduler.h>
#include "../../driver/fs/ext4/format.h"
#include "../../driver/fs/ext4/native.h"

namespace {
using namespace xjinstaller;

struct FactoryContext;
struct WindowReader { FactoryContext *context; uint64_t offset, bytes; };
struct FactoryContext
{
    FactoryContext *next;
    vfs_node_t target, source;
    BlockExtent whole;
    BlockAccessToken lease;
    FactoryOptions options;
    FactoryStatus status;
    DiskLayout layout;
    DiskLayoutWorkspace *gpt, *readback;
    uint8_t *backup_entries;
    xjext4::FormatWorkspace *format;
    xjext4::FormatStorage format_storage;
    xjext4::FormatGeometry geometry;
    Ext4ImportWorkspace *preflight;
    Ext4VerifyWorkspace *verify;
    EspVerifyWorkspace *esp_verify;
    EspPublication publication;
    XJ_ROOT_BINDING binding;
    EspPayload payload;
    WindowReader root_reader, esp_reader;
    bool published, pinned;
};

FactoryContext *contexts;
unsigned char context_lock;

class RegistryLock
{
    uint64_t flags;
public:
    RegistryLock()
    {
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
        while (__atomic_test_and_set(&context_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    }
    ~RegistryLock()
    {
        __atomic_clear(&context_lock, __ATOMIC_RELEASE);
        __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
    }
};

class StorageScope
{
    tcb_t task;
public:
    StorageScope() : task(get_current_task()) { ++task->ime_storage_depth; }
    ~StorageScope() { --task->ime_storage_depth; }
};

template <typename T> T *allocate() { return static_cast<T *>(calloc(1, sizeof(T))); }

bool span(const void *p, size_t bytes)
{
    uintptr_t base = reinterpret_cast<uintptr_t>(p);
    return base && bytes && bytes <= uintptr_t(-1) - base;
}

bool separate(const void *a, size_t as, const void *b, size_t bs)
{
    uintptr_t x = reinterpret_cast<uintptr_t>(a), y = reinterpret_cast<uintptr_t>(b);
    return x >= y ? x - y >= bs : y - x >= as;
}

bool equal_extent(const BlockExtent &a, const BlockExtent &b)
{
    return a.device == b.device && a.physical_device == b.physical_device && a.generation == b.generation &&
           a.physical_generation == b.physical_generation && a.start == b.start && a.bytes == b.bytes;
}

bool fail_io(FactoryContext *context)
{
    context->status.io_failed = true;
    block_access_poison(context->lease);
    return false;
}

void phase(FactoryContext *context, FactoryPhase value)
{
    context->status.phase = value;
    write_serial_fmt("FACTORY: phase=%u records=%llu data=%llu raw-read=%llu raw-written=%llu\n",
                     unsigned(value), context->status.package_records, context->status.package_data_bytes,
                     context->status.read_bytes, context->status.written_bytes);
}

bool ready(FactoryContext *context)
{
    if (context->status.io_failed) return false;
    BlockExtent current;
    if (block_access_validate_owned_exclusive(context->lease, context->whole, 0, context->whole.bytes) ||
        block_access_resolve_node(context->target, &current) || !equal_extent(current, context->whole) ||
        context->target->size != current.bytes) return fail_io(context);
    device_t *physical = get_device(current.physical_device);
    tcb_t task = get_current_task();
    if (!physical || physical->type != DEVICE_BLOCK || physical->vdiskid != current.physical_device ||
        physical->sector_size != 512 || physical->size != current.bytes || !task ||
        task->ime_storage_depth == uint32_t(-1)) return fail_io(context);
    return true;
}

bool read(void *opaque, uint64_t offset, void *data, size_t bytes)
{
    auto *context = static_cast<FactoryContext *>(opaque);
    if (!ready(context) || offset > context->whole.bytes || bytes > context->whole.bytes - offset)
        return fail_io(context);
    context->status.io_started = true;
    StorageScope scope;
    if (vfs_read(context->target, data, offset, bytes) != bytes) return fail_io(context);
    context->status.read_bytes += bytes;
    return true;
}

bool write(void *opaque, uint64_t offset, const void *data, size_t bytes)
{
    auto *context = static_cast<FactoryContext *>(opaque);
    if (!ready(context) || offset > context->whole.bytes || bytes > context->whole.bytes - offset)
        return fail_io(context);
    context->status.io_started = true;
    StorageScope scope;
    if (vfs_write(context->target, const_cast<void *>(data), offset, bytes) != bytes) return fail_io(context);
    context->status.written_bytes += bytes; // Confirmed prefixes only, not ambiguous persisted bytes.
    return true;
}

bool flush(void *opaque)
{
    auto *context = static_cast<FactoryContext *>(opaque);
    if (!ready(context)) return false;
    context->status.io_started = true;
    StorageScope scope;
    return vfs_ioctl(context->target, IOBLKSYNC, nullptr) == 0 || fail_io(context);
}

bool window_read(void *opaque, uint64_t offset, void *data, size_t bytes)
{
    auto *window = static_cast<WindowReader *>(opaque);
    if (offset > window->bytes || bytes > window->bytes - offset) return fail_io(window->context);
    return read(window->context, window->offset + offset, data, bytes);
}

// plan_format validates callback presence, but pure planning must never gain
// a route to the target even if a future change accidentally invokes one.
bool no_read(void *, uint64_t, void *, size_t) { return false; }
bool no_write(void *, uint64_t, const void *, size_t) { return false; }
bool no_flush(void *) { return false; }

const xjpak2::Record *required_file(const xjpak2::Plan &plan, const char *name)
{
    size_t bytes = strlen(name);
    for (size_t i = 0; i < plan.count; ++i)
    {
        const auto &record = plan.records[i];
        if (record.path.size == bytes && !memcmp(record.path.data, name, bytes))
            return record.kind == xjpak2::Kind::Regular && record.payload.size ? &record : nullptr;
    }
    return nullptr;
}

int preflight(FactoryContext *context, const uint8_t *package, size_t bytes)
{
    if (context->options.esp_bytes < EspMinimumBytes || context->options.esp_bytes % 512 ||
        context->options.esp_bytes / 512 > EspMaximumSectors) return -EINVAL;
    DiskLayoutOptions layout_options{};
    layout_options.disk_bytes = context->whole.bytes;
    layout_options.esp_bytes = context->options.esp_bytes;
    layout_options.identities = context->options.identities;
    int error = disk_layout_plan(layout_options, context->layout);
    if (error) return error;
    xjpak2::Limits limits;
    xjpak2::HeaderInfo header;
    error = xjpak2::inspect_header(package, bytes, limits, header);
    if (error || header.legacy_v1) return error ? error : -EOPNOTSUPP;
    if (header.record_count > size_t(-1) / sizeof(xjpak2::Record) ||
        header.record_count > size_t(-1) / sizeof(Ext4ImportEntry) ||
        header.record_count > size_t(-1) / sizeof(Ext4VerifyEntry)) return -E2BIG;
    context->gpt = allocate<DiskLayoutWorkspace>();
    context->readback = allocate<DiskLayoutWorkspace>();
    context->backup_entries = static_cast<uint8_t *>(malloc(DiskTableBytes));
    context->format = allocate<xjext4::FormatWorkspace>();
    context->preflight = allocate<Ext4ImportWorkspace>();
    context->verify = allocate<Ext4VerifyWorkspace>();
    context->esp_verify = allocate<EspVerifyWorkspace>();
    if (!context->gpt || !context->readback || !context->backup_entries || !context->format ||
        !context->preflight || !context->verify || !context->esp_verify) return -ENOMEM;
    auto &plan = *context->preflight;
    plan.capacity = header.record_count;
    plan.records = static_cast<xjpak2::Record *>(malloc(header.record_count * sizeof(xjpak2::Record)));
    plan.entries = static_cast<Ext4ImportEntry *>(malloc(header.record_count * sizeof(Ext4ImportEntry)));
    auto &verify = *context->verify;
    verify.records = plan.records; verify.capacity = header.record_count;
    verify.entries = static_cast<Ext4VerifyEntry *>(malloc(header.record_count * sizeof(Ext4VerifyEntry)));
    verify.volume = allocate<xjext4::Volume>();
    if (!plan.records || !plan.entries || !verify.entries || !verify.volume) return -ENOMEM;
    Ext4ImportRequirements requirements;
    error = ext4_import_prepare(package, bytes, limits, plan, requirements);
    if (error) return error;
    const auto *kernel = required_file(plan.plan, "/system/kernel.krl");
    const auto *loader = required_file(plan.plan, "/EFI/BOOT/BOOTX64.EFI");
    if (!kernel || !loader || !required_file(plan.plan, "/config/README.md") ||
        kernel->payload.size < 64 || memcmp(kernel->payload.data, "\x7f" "ELF", 4) ||
        kernel->payload.data[4] != 2 || kernel->payload.data[5] != 1 ||
        kernel->payload.data[18] != 62 || kernel->payload.data[19] ||
        loader->payload.size < 64 || loader->payload.data[0] != 'M' || loader->payload.data[1] != 'Z')
        return -ENOEXEC;
    context->payload = {loader->payload.data, loader->payload.size, kernel->payload.data, kernel->payload.size};
    EspRequirements esp_requirements{};
    error = esp_plan(context->options.esp_bytes, context->payload, esp_requirements);
    if (error) return error;
    context->esp_verify->cluster_bits_bytes = esp_verify_bitmap_bytes(context->options.esp_bytes);
    context->esp_verify->cluster_bits = static_cast<uint8_t *>(malloc(context->esp_verify->cluster_bits_bytes));
    if (!context->esp_verify->cluster_bits) return -ENOMEM;
    error = disk_layout_build(context->layout, *context->gpt);
    if (error) return error;
    context->root_reader = {context, context->layout.root_first_lba * 512,
                            (context->layout.root_last_lba - context->layout.root_first_lba + 1) * 512};
    context->esp_reader = {context, context->layout.esp_first_lba * 512, context->options.esp_bytes};
    xjext4::FormatOptions format_options{};
    memcpy(format_options.uuid, context->options.filesystem_uuid, 16);
    format_options.time_seconds = context->options.time_seconds;
    format_options.journal_mib = context->options.journal_mib;
    memcpy(format_options.label, "XJ380", 5);
    xjext4::Ext4FormatDevice device{nullptr, no_read, no_write, no_flush, context->root_reader.bytes};
    error = xjext4::format_storage_size(device.bytes, context->format_storage.bytes);
    if (!error && context->format_storage.bytes)
    {
        context->format_storage.data = malloc(context->format_storage.bytes);
        if (!context->format_storage.data) error = -ENOMEM;
    }
    if (!error) error = xjext4::plan_format(device, format_options, *context->format, context->geometry,
                                          &context->format_storage);
    // Pure admission check; actual builder storage is allocated before its
    // first format request and stays with the quarantined build on failure.
    size_t writer_bytes = 0;
    if (!error) error = xjext4::WriteSession::storage_size_for(context->geometry.blocks,
                         context->geometry.inodes, context->geometry.groups, writer_bytes);
    if (!error) error = ext4_import_check_budget(requirements, 4096, context->geometry.free_blocks,
                                                context->geometry.inodes - 11);
    if (error) return error;
    auto &binding = context->binding;
    memcpy(binding.bytes, "XJROOT01", 8);
    binding.bytes[8] = 1; binding.bytes[12] = XJ_ROOT_BINDING_SIZE;
    memcpy(binding.bytes + 16, context->options.identities.disk, 16);
    memcpy(binding.bytes + 32, context->options.identities.root, 16);
    memcpy(binding.bytes + 48, context->options.identities.esp, 16);
    memcpy(binding.bytes + 64, context->options.filesystem_uuid, 16);
    uint64_t kernel_bytes = context->payload.kernel_bytes;
    uint32_t kernel_crc = xj_root_binding_crc32c(context->payload.kernel, context->payload.kernel_bytes);
    for (unsigned i = 0; i < 8; ++i) binding.bytes[80 + i] = uint8_t(kernel_bytes >> (8 * i));
    for (unsigned i = 0; i < 4; ++i) binding.bytes[88 + i] = uint8_t(kernel_crc >> (8 * i));
    uint32_t descriptor_crc = xj_root_binding_crc32c(binding.bytes, 92);
    for (unsigned i = 0; i < 4; ++i) binding.bytes[92 + i] = uint8_t(descriptor_crc >> (8 * i));
    context->status.package_records = plan.plan.count;
    context->status.package_data_bytes = plan.plan.data_bytes;
    return 0;
}

void free_context(FactoryContext *context)
{
    if (context->verify)
    {
        free(context->verify->volume); free(context->verify->entries); free(context->verify);
    }
    if (context->preflight)
    {
        free(context->preflight->entries); free(context->preflight->records); free(context->preflight);
    }
    if (context->esp_verify) { free(context->esp_verify->cluster_bits); free(context->esp_verify); }
    free(context->format_storage.data);
    free(context->format); free(context->backup_entries); free(context->readback); free(context->gpt);
    free(context);
}

int execute(FactoryContext *context, const uint8_t *package, size_t bytes)
{
    auto &status = context->status;
    phase(context, FactoryPhase::InitialFlush);
    if (!flush(context)) return -EIO;
    const auto &layout = context->layout;
    const auto &gpt = *context->gpt;
    // Withdraw old boot discovery before modifying payload ranges. Then clear
    // the complete new ESP before publishing a new GPT: a formatter may write
    // its BPB before every directory/FAT block. Reused caller GUIDs must never
    // expose a stale valid root.bin while the new installation is incomplete.
    // This is destructive fresh installation, not secure erase or an upgrade.
    uint8_t *zeros = context->format->allocated;
    memset(zeros, 0, 65536);
    if (!write(context, 512, zeros, 512) ||
        !write(context, (layout.total_sectors - 1) * 512, zeros, 512) ||
        !write(context, 0, zeros, 512) || !flush(context)) return -EIO;
    if (!read(context, 0, context->readback->mbr, 512) ||
        !read(context, 512, context->readback->primary_header, 512) ||
        !read(context, (layout.total_sectors - 1) * 512, context->readback->backup_header, 512) ||
        memcmp(zeros, context->readback->mbr, 512) || memcmp(zeros, context->readback->primary_header, 512) ||
        memcmp(zeros, context->readback->backup_header, 512)) return -EIO;
    for (uint64_t offset = 0; offset < context->esp_reader.bytes; offset += 65536)
        if (!write(context, context->esp_reader.offset + offset, zeros, 65536)) return -EIO;
    if (!flush(context)) return -EIO;
    phase(context, FactoryPhase::EspInvalidated);
    if (!write(context, layout.backup_entries_lba * 512, gpt.entries, DiskTableBytes) ||
        !write(context, (layout.total_sectors - 1) * 512, gpt.backup_header, 512) ||
        !write(context, layout.primary_entries_lba * 512, gpt.entries, DiskTableBytes) ||
        !write(context, 512, gpt.primary_header, 512) || !write(context, 0, gpt.mbr, 512) || !flush(context))
        return -EIO;
    phase(context, FactoryPhase::GptWritten);
    auto &back = *context->readback;
    if (!read(context, 0, back.mbr, 512) || !read(context, 512, back.primary_header, 512) ||
        !read(context, layout.primary_entries_lba * 512, back.entries, DiskTableBytes) ||
        !read(context, (layout.total_sectors - 1) * 512, back.backup_header, 512) ||
        !read(context, layout.backup_entries_lba * 512, context->backup_entries, DiskTableBytes)) return -EIO;
    DiskLayoutReadback disk_back{{back.mbr, 512}, {back.primary_header, 512}, {back.entries, DiskTableBytes},
                                 {back.backup_header, 512}, {context->backup_entries, DiskTableBytes}};
    int error = disk_layout_validate(layout, disk_back);
    if (error) return error;
    phase(context, FactoryPhase::GptVerified);
    BlockAccessWindow root{context->target, context->whole, context->lease,
                            context->root_reader.offset, context->root_reader.bytes};
    ext4_format_options options{};
    memcpy(options.uuid, context->options.filesystem_uuid, 16);
    memcpy(options.label, "XJ380", 5);
    options.time_seconds = context->options.time_seconds; options.journal_mib = context->options.journal_mib;
    ext4_build_status root_status{};
    error = ext4fs_build_window(&root, &options, package, bytes, &root_status);
    if (root_status.format.io_failed || root_status.import_io_failed) status.io_failed = true;
    if (error || !root_status.import_complete || root_status.quarantined || root_status.import_io_failed ||
        root_status.format.io_failed) return error ? error : -EIO;
    phase(context, FactoryPhase::RootBuilt);
    BlockAccessWindow esp{context->target, context->whole, context->lease,
                           context->esp_reader.offset, context->esp_reader.bytes};
    EspBuildStatus esp_status{};
    error = fatfs_build_esp_window(&esp, &context->payload, &esp_status);
    if (esp_status.io_failed) status.io_failed = true;
    if (error || !esp_status.complete || esp_status.quarantined || esp_status.io_failed)
        return error ? error : -EIO;
    phase(context, FactoryPhase::EspBuilt);
    Ext4VerifyDevice root_device{window_read, &context->root_reader, context->root_reader.bytes};
    Ext4VerifyStatus verify_status{};
    xjpak2::Limits limits;
    error = ext4_verify_package(package, bytes, limits, root_device, context->options.filesystem_uuid,
                                 *context->verify, &verify_status);
    if (verify_status.io_failed) status.io_failed = true;
    if (error || !verify_status.complete || verify_status.io_failed ||
        verify_status.records_verified != status.package_records ||
        verify_status.data_bytes_verified != status.package_data_bytes) return error ? error : -EIO;
    phase(context, FactoryPhase::RootVerified);
    EspReadDevice esp_device{window_read, &context->esp_reader, context->esp_reader.bytes};
    EspExpectedFiles files{static_cast<const uint8_t *>(context->payload.loader), context->payload.loader_bytes,
                            static_cast<const uint8_t *>(context->payload.kernel), context->payload.kernel_bytes};
    error = esp_verify_prepare_publication(esp_device, files, context->binding,
                                            *context->esp_verify, context->publication);
    if (context->esp_verify->io_failed) status.io_failed = true;
    if (error || !context->esp_verify->complete || !context->publication.ready) return error ? error : -EIO;
    if (context->publication.sector_offset % 512 || context->publication.sector_offset > context->esp_reader.bytes ||
        512 > context->esp_reader.bytes - context->publication.sector_offset) return -EUCLEAN;
    phase(context, FactoryPhase::EspVerified);
    // No filesystem writer/session remains. The proposed sector was read back
    // from an allocated fixed-size placeholder. Flush every prerequisite before
    // the first request capable of making the binding valid.
    if (!flush(context)) return -EIO;
    phase(context, FactoryPhase::ActivationAttempted); status.activation_attempted = true;
    uint64_t activation = context->esp_reader.offset + context->publication.sector_offset;
    if (!write(context, activation, context->publication.sector, 512) || !flush(context)) return -EIO;
    if (!read(context, activation, context->esp_verify->data, 512) ||
        memcmp(context->esp_verify->data, context->publication.sector, 512)) return -EIO;
    phase(context, FactoryPhase::ActivationDurable); status.activation_durable = true;
    return 0;
}

} // namespace

int installer_build_ext4_disk(vfs_node_t target, vfs_node_t source, const xjinstaller::FactoryOptions *options,
                              const void *package, size_t bytes, xjinstaller::FactoryStatus *status)
{
    using namespace xjinstaller;
    if (!span(target, sizeof(*target)) || !span(source, sizeof(*source)) || !span(options, sizeof(*options)) ||
        !span(package, bytes) || !separate(package, bytes, target, sizeof(*target)) ||
        !separate(package, bytes, source, sizeof(*source)) || (status && (!span(status, sizeof(*status)) ||
        !separate(status, sizeof(*status), package, bytes) ||
        !separate(status, sizeof(*status), target, sizeof(*target)) ||
        !separate(status, sizeof(*status), source, sizeof(*source))))) return -EINVAL;
    FactoryOptions snapshot = *options;
    if (status) *status = {};
    auto refuse = [status](int error)
    {
        if (status) status->error = error;
        return error;
    };
    if (!(target->type & file_block) || !(source->type & file_block)) return refuse(-ENOTBLK);
    tcb_t task = get_current_task();
    if (!task || !scheduler_is_enabled() || !(get_rflags() & (1ULL << 9))) return refuse(-EWOULDBLOCK);
    if (task->ime_storage_depth) return refuse(-EBUSY);
    int availability = ext4fs_factory_ready();
    if (!availability) availability = fatfs_factory_ready();
    if (availability) return refuse(availability);
    if (!vfs_check_access(target, nullptr, IME_FILE_MOUNT) || !vfs_check_access(target, nullptr, IME_FILE_WRITE))
        return refuse(vfs_last_error() ? -vfs_last_error() : -EACCES);
    auto *context = allocate<FactoryContext>();
    if (!context) return refuse(-ENOMEM);
    context->target = target; context->source = source; context->options = snapshot;
    context->status.phase = FactoryPhase::Preflight;
    BlockExtent source_extent{};
    int error = block_access_resolve_node(target, &context->whole);
    if (!error) error = block_access_resolve_node(source, &source_extent);
    if (!error && (context->whole.device != context->whole.physical_device || context->whole.start ||
        context->whole.bytes != target->size || !context->whole.bytes)) error = -EINVAL;
    if (!error && context->whole.physical_device == source_extent.physical_device &&
        context->whole.physical_generation == source_extent.physical_generation) error = -EBUSY;
    if (!error) error = block_access_require_flush(context->whole);
    if (!error) error = block_access_claim(context->whole, true, &context->lease);
    if (!error) error = block_access_validate_owned_exclusive(context->lease, context->whole, 0, context->whole.bytes);
    if (!error)
    {
        device_t *physical = get_device(context->whole.physical_device);
        if (!physical || physical->type != DEVICE_BLOCK || physical->vdiskid != context->whole.physical_device ||
            physical->sector_size != 512 || physical->size != context->whole.bytes) error = -EOPNOTSUPP;
    }
    if (!error) error = preflight(context, static_cast<const uint8_t *>(package), bytes);
    if (!error)
    {
        vfs_child_lock();
        if (target->refcount == uint32_t(-1) || source->refcount == uint32_t(-1)) error = -EOVERFLOW;
        else { ++target->refcount; ++source->refcount; context->pinned = true; }
        vfs_child_unlock();
    }
    if (!error)
    {
        {
            RegistryLock lock;
            context->next = contexts; contexts = context; context->published = true;
        }
        context->status.phase = FactoryPhase::Published;
        error = execute(context, static_cast<const uint8_t *>(package), bytes);
    }
    context->status.error = error;
    if (error && context->published)
    {
        block_access_poison(context->lease);
        context->status.quarantined = true;
        if (status) *status = context->status;
        return error;
    }
    if (context->pinned)
    {
        int close_target = vfs_close(target), close_source = vfs_close(source);
        if (close_target || close_source) error = -EIO;
    }
    if (context->lease)
    {
        if (!error && block_access_release(context->lease)) error = -EIO;
        else if (error && !context->published) block_access_release(context->lease);
    }
    if (error && context->published)
    {
        block_access_poison(context->lease);
        context->status.error = error; context->status.quarantined = true;
        if (status) *status = context->status;
        return error;
    }
    if (context->published)
    {
        RegistryLock lock;
        FactoryContext **previous = &contexts;
        while (*previous && *previous != context) previous = &(*previous)->next;
        if (*previous) *previous = context->next;
    }
    if (!error) { context->status.phase = FactoryPhase::Complete; context->status.complete = true; }
    context->status.error = error;
    if (status) *status = context->status;
    free_context(context);
    return error;
}
