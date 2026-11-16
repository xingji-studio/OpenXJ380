// SPDX-License-Identifier: Apache-2.0
// Original package lifetime staging and explicit native-service admission.

#include <installer/boot_proof.h>
#include <installer/package.h>
#include <installer/session.h>
#include <installer/context.h>
#include <installer/source_lifetime.h>
#include <installer/jobs.h>
#include <installer/payloads.h>
#include <installer/stream_index.h>
#include <installer/source_reader.h>
#include <installer_mode.h>
#include <installer_protocol.h>
#include <task/pcb.h>
#include <task/scheduler.h>
#include <cpu/regio.h>
#include <cpu/msr.h>
#include <krlibc.h>
#include <proto.hpp>
#include "../build_settings.h"

extern char _kernel_start[], __ksymtab_end[];

namespace installer
{
namespace
{
enum State : unsigned { empty, checking, memory_proven, packages_proven, source_retained, rejected };
unsigned state;
// 0: never activated, 1: active, 2: permanently revoked until reboot.
unsigned native_admission;
BOOT_CONFIG saved_config;
BootPackageProof package_proof;
TableWorkspace table_workspace;
uint64_t arena_budget;
xjpak2::Record *root_records, *system_records;
xjpak2::Plan root_plan, system_plan;
XJ_INSTALLER_PACKAGE_SPAN ext4_span;
BlockExtent source_extent;
SourceLifetime retained_source;
xjinstaller::StreamIndex source_index;
xjpak2::DataReader source_reader;
SourceReadContext source_read_context;

struct TableContext
{
    boot_memory::MapView map;
    BootPackageProof packages;
    uint64_t checked_tables[16];
    unsigned next_table;
};

bool table_read_failed(const char *reason, uint64_t physical, unsigned index)
{
    write_serial_fmt_heapless("installer: memory proof table-read failed reason=%s table=0x%llx index=%u\n", reason,
                              (unsigned long long)physical, index);
    return false;
}

bool read_table(void *opaque, uint64_t physical, unsigned index, uint64_t *output)
{
    auto &context = *static_cast<TableContext *>(opaque);
    if (!physical || (physical & 4095) || physical >= direct_map_bytes || index >= 512)
        return table_read_failed("address-or-index", physical, index);
    if (packages_overlap(context.packages, physical, 4096))
        return table_read_failed("package-overlap", physical, index);
    bool checked = false;
    for (uint64_t cached : context.checked_tables) if (cached == physical) checked = true;
    if (!checked)
    {
        // The final map is validated once before any table read. Avoid running
        // its overlap scan for every PTE. Firmware's current page tables are
        // trusted bootstrap storage, not a caller-controlled pointer graph.
        bool owned = false;
        bool                    found  = false;
        boot_memory::Descriptor region = {};
        for (uint64_t i = 0; i < context.map.map_bytes / context.map.descriptor_bytes; ++i)
        {
            boot_memory::Descriptor descriptor = {};
            if (!boot_memory::descriptor_at(context.map, i, &descriptor))
            {
                write_serial_fmt_heapless("installer: memory proof invalid table descriptor=%llu\n",
                                          (unsigned long long)i);
                return table_read_failed("efi-descriptor-invalid", physical, index);
            }
            if (physical >= descriptor.physical_start &&
                physical - descriptor.physical_start < descriptor.pages * 4096)
            {
                found  = true;
                region = descriptor;
                owned = descriptor.type >= 1 && descriptor.type <= 4;
                break;
            }
        }
        if (!owned)
        {
            write_serial_fmt_heapless("installer: memory proof table owner found=%u type=%u start=0x%llx "
                                      "pages=0x%llx attributes=0x%llx allowed_types=1..4\n",
                                      unsigned(found), region.type, (unsigned long long)region.physical_start,
                                      (unsigned long long)region.pages, (unsigned long long)region.attributes);
            return table_read_failed("efi-ownership", physical, index);
        }
        context.checked_tables[context.next_table++ % 16] = physical;
    }
    *output = reinterpret_cast<volatile const uint64_t *>(direct_map_base + physical)[index];
    return true;
}

bool protected_span_failed(const char *reason, uint64_t address, uint64_t bytes, uint64_t physical = 0)
{
    write_serial_fmt_heapless("installer: memory proof protected span failed reason=%s va=0x%llx "
                              "remaining=0x%llx physical=0x%llx\n",
                              reason, (unsigned long long)address, (unsigned long long)bytes,
                              (unsigned long long)physical);
    return false;
}

bool protected_virtual_span(TableReader reader, uint64_t table, const BootPackageProof &proof,
                            uint64_t address, uint64_t bytes)
{
    if (!bytes || address > __UINT64_MAX__ - bytes) return protected_span_failed("bounds", address, bytes);
    while (bytes)
    {
        Mapping mapping = {};
        if (!resolve_mapping(reader, table, address, &mapping))
            return protected_span_failed("unmapped", address, bytes);
        if (!mapping.supervisor) return protected_span_failed("user-accessible", address, bytes, mapping.physical);
        uint64_t step = bytes < mapping.remaining ? bytes : mapping.remaining;
        // Reject overlap at page granularity, including the last partial page.
        uint64_t physical = mapping.physical & ~4095ULL;
        uint64_t covered = ((mapping.physical & 4095) + step + 4095) & ~4095ULL;
        if (packages_overlap(proof, physical, covered))
            return protected_span_failed("package-overlap", address, covered, physical);
        address += step;
        bytes -= step;
    }
    return true;
}

int refuse(int error)
{
    __atomic_store_n(&state, rejected, __ATOMIC_RELEASE);
    return error;
}

void log_boot_map(const boot_memory::MapView &map)
{
    // Failure-only, bounded dump of trusted bootstrap map storage. Copy the raw
    // prefix so even a descriptor rejected by descriptor_at remains diagnosable.
    // No package memory is read and no allocation, VFS or scheduler call is made.
    if (!map.bytes || map.descriptor_bytes < boot_memory::descriptor_prefix_bytes || map.map_bytes > map.buffer_bytes ||
        map.buffer_bytes > __UINTPTR_MAX__ - reinterpret_cast<uintptr_t>(map.bytes))
    {
        write_serial_fmt_heapless("installer: memory proof EFI map dump unavailable: structural bounds\n");
        return;
    }
    uint64_t           count      = map.map_bytes / map.descriptor_bytes;
    constexpr uint64_t dump_limit = 256;
    for (uint64_t i = 0; i < count && i < dump_limit; ++i)
    {
        const auto *record   = static_cast<const uint8_t *>(map.bytes) + i * map.descriptor_bytes;
        uint32_t    type     = 0;
        uint64_t    physical = 0, virtual_start = 0, pages = 0, attributes = 0;
        memcpy(&type, record, sizeof(type));
        memcpy(&physical, record + 8, sizeof(physical));
        memcpy(&virtual_start, record + 16, sizeof(virtual_start));
        memcpy(&pages, record + 24, sizeof(pages));
        memcpy(&attributes, record + 32, sizeof(attributes));
        write_serial_fmt_heapless("installer: memory proof EFI[%llu] type=%u physical=0x%llx virtual=0x%llx "
                                  "pages=0x%llx attributes=0x%llx\n",
                                  (unsigned long long)i, type, (unsigned long long)physical,
                                  (unsigned long long)virtual_start, (unsigned long long)pages,
                                  (unsigned long long)attributes);
    }
    if (count > dump_limit)
        write_serial_fmt_heapless("installer: memory proof EFI map dump truncated shown=%llu total=%llu\n",
                                  (unsigned long long)dump_limit, (unsigned long long)count);
}

int boot_refuse(const char *stage, int error, const boot_memory::MapView *map = nullptr,
                const BootProofDiagnostic *diagnostic = nullptr)
{
    write_serial_fmt_heapless("installer: memory proof failed stage=%s error=%d\n", stage, error);
    if (diagnostic && diagnostic->reason)
        write_serial_fmt_heapless("installer: memory proof detail reason=%s address=0x%llx bytes=0x%llx "
                                  "physical=0x%llx table=0x%llx level=%u index=%u entry=0x%llx\n",
                                  diagnostic->reason, (unsigned long long)diagnostic->address,
                                  (unsigned long long)diagnostic->bytes, (unsigned long long)diagnostic->physical,
                                  (unsigned long long)diagnostic->table, diagnostic->level, diagnostic->index,
                                  (unsigned long long)diagnostic->entry);
    if (map) log_boot_map(*map);
    return refuse(error);
}

int validate_package(const XJ_INSTALLER_PACKAGE_SPAN &span, bool legacy,
                     uint64_t &remaining_budget, xjpak2::Record **records, xjpak2::Plan &plan,
                     const xjpak2::DataReader *reader = nullptr)
{
    xjpak2::Limits limits;
    if (legacy)
    {
        limits.max_path = 511;
        limits.max_file_bytes = 0xffffffffULL;
    }
    auto *bytes = reinterpret_cast<const uint8_t *>(direct_map_base + span.physical);
    xjpak2::HeaderInfo header;
    int result = xjpak2::inspect_header(bytes, span.bytes, limits, header);
    if (result < 0) return result;
    if (header.legacy_v1 != legacy) return -95;
    if (header.record_count > __SIZE_MAX__ / sizeof(xjpak2::Record)) return -12;
    uint64_t needed = uint64_t(header.record_count) * sizeof(xjpak2::Record);
    if (needed > remaining_budget) return -12;
    *records = needed ? static_cast<xjpak2::Record *>(malloc(needed)) : nullptr;
    if (needed && !*records) return -12;
    result = reader ? xjpak2::validate_indexed(bytes, span.bytes, limits, {*records, header.record_count}, plan, *reader) :
                      xjpak2::validate(bytes, span.bytes, limits, {*records, header.record_count}, plan);
    if (result < 0)
    {
        free(*records);
        *records = nullptr;
        return result;
    }
    remaining_budget -= needed;
    return 0;
}


} // namespace

int session_boot_prepare(const BOOT_CONFIG &config, const void *original_config)
{
    if (!(config.boot_flags & BOOT_FLAG_INSTALLER)) return 0;
    unsigned expected = empty;
    if (!__atomic_compare_exchange_n(&state, &expected, checking, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        write_serial_fmt_heapless("installer: memory proof failed stage=session-state error=-16 state=%u\n", expected);
        return -16;
    }
    write_serial_fmt_heapless("installer: memory proof begin flags=0x%llx\n", (unsigned long long)config.boot_flags);
    // Preserve the legacy-prefix boundary: an absent handoff flag means the
    // extension may be unmapped, so even diagnostic reads must wait for it.
    if (!(config.boot_flags & BOOT_FLAG_INSTALLER_HANDOFF)) return boot_refuse("handoff-flag", -95);
    if (config.boot_flags & BOOT_FLAG_BOUND_ROOT) return boot_refuse("bound-root-conflict", -95);
    const auto &handoff = config.installer_handoff;
    write_serial_fmt_heapless("installer: memory proof handoff magic=0x%x version=%u "
                              "bytes=%u expected_bytes=%u root_format=%u profile=%u\n",
                              handoff.magic, handoff.version, handoff.bytes, unsigned(sizeof(handoff)),
                              handoff.root_format, handoff.system_profile);
    write_serial_fmt_heapless("installer: memory proof root physical=0x%llx bytes=0x%llx pages=0x%llx\n",
                              (unsigned long long)handoff.root.physical, (unsigned long long)handoff.root.bytes,
                              (unsigned long long)handoff.root.pages);
    write_serial_fmt_heapless("installer: memory proof system physical=0x%llx bytes=0x%llx pages=0x%llx\n",
                              (unsigned long long)handoff.system.physical, (unsigned long long)handoff.system.bytes,
                              (unsigned long long)handoff.system.pages);
    write_serial_fmt_heapless("installer: memory proof loader physical=0x%llx bytes=0x%llx\n",
                              (unsigned long long)handoff.loader_image_physical,
                              (unsigned long long)handoff.loader_image_bytes);
    if (!xj_installer_handoff_valid(&handoff)) return boot_refuse("handoff-invalid", -95);
    if (config.installer_root_pak != handoff.root.physical || config.installer_root_pak_size != handoff.root.bytes ||
        config.system_payload_pak != handoff.system.physical || config.system_payload_pak_size != handoff.system.bytes)
    {
        write_serial_fmt_heapless(
            "installer: memory proof legacy spans root=0x%llx root_bytes=0x%llx "
            "system=0x%llx system_bytes=0x%llx\n",
            (unsigned long long)config.installer_root_pak, (unsigned long long)config.installer_root_pak_size,
            (unsigned long long)config.system_payload_pak, (unsigned long long)config.system_payload_pak_size);
        return boot_refuse("legacy-span-mismatch", -22);
    }
    uint64_t map_physical = reinterpret_cast<uintptr_t>(config.MemoryMap.Buffer);
    write_serial_fmt_heapless("installer: memory proof map physical=0x%llx bytes=0x%llx stride=%llu version=%u\n",
                              (unsigned long long)map_physical, (unsigned long long)config.MemoryMap.MapSize,
                              (unsigned long long)config.MemoryMap.DescriptorSize, config.MemoryMap.DescriptorVersion);
    if (!map_physical || map_physical >= direct_map_bytes || config.MemoryMap.MapSize > direct_map_bytes - map_physical)
        return boot_refuse("memory-map-bounds", -14);
    // The readable boot config, final map and current translation tree are the
    // architecture's existing bootstrap contract. Package addresses receive no
    // such trust and are not dereferenced until the complete proof succeeds.
    boot_memory::MapView map = {reinterpret_cast<const void *>(direct_map_base + map_physical),
                               config.MemoryMap.MapSize, config.MemoryMap.MapSize, config.MemoryMap.DescriptorSize};
    boot_memory::Summary summary = {};
    if (!boot_memory::validate_map(map, &summary)) return boot_refuse("memory-map-invalid", -14, &map);
    TableContext context = {};
    context.map = map;
    context.packages = {{handoff.root.physical, handoff.root.physical + handoff.root.pages * 4096},
                        {handoff.system.physical, handoff.system.physical + handoff.system.pages * 4096},
                        handoff.system_profile, true};
    TableReader reader = {&context, read_table};
    BootProofDiagnostic diagnostic = {};
    reader.diagnostic              = &diagnostic;
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    asm_cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    uint32_t maximum_leaf = eax;
    reader.physical_bits = 36; // Architectural fallback before CPUID address widths.
    reader.gigabyte_pages = false;
    if (maximum_leaf >= 0x80000008)
    {
        asm_cpuid(0x80000008, 0, &eax, &ebx, &ecx, &edx);
        reader.physical_bits = eax & 255;
    }
    if (maximum_leaf >= 0x80000001)
    {
        asm_cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
        reader.gigabyte_pages = (edx & (1U << 26)) != 0;
    }
    reader.nx_enabled = (rdmsr(0xc0000080) & (1ULL << 11)) != 0;
    uint64_t table = get_cr3() & 0x000ffffffffff000ULL;
    write_serial_fmt_heapless("installer: memory proof CPU cr3=0x%llx max_extended_leaf=0x%x "
                              "physical_bits=%u nx_enabled=%u gigabyte_pages=%u table_capacity=%llu\n",
                              (unsigned long long)table, maximum_leaf, unsigned(reader.physical_bits),
                              unsigned(reader.nx_enabled), unsigned(reader.gigabyte_pages),
                              (unsigned long long)table_workspace_capacity);
    BootPackageProof proof = {};
    int result = prove_boot_packages(handoff, map, map_physical, reader, table, &proof);
    if (result < 0) return boot_refuse("package-ownership-and-mapping", result, &map, &diagnostic);
    uint64_t kernel_start = reinterpret_cast<uintptr_t>(_kernel_start);
    uint64_t kernel_end = reinterpret_cast<uintptr_t>(__ksymtab_end);
    if (kernel_end <= kernel_start)
    {
        write_serial_fmt_heapless("installer: memory proof kernel start=0x%llx end=0x%llx\n",
                                  (unsigned long long)kernel_start, (unsigned long long)kernel_end);
        return boot_refuse("kernel-bounds", -14, &map);
    }
    if (!protected_virtual_span(reader, table, proof, kernel_start, kernel_end - kernel_start))
        return boot_refuse("kernel-mapping", -14, &map, &diagnostic);
    if (!protected_virtual_span(reader, table, proof, reinterpret_cast<uintptr_t>(original_config),
                                sizeof(BOOT_CONFIG)))
        return boot_refuse("boot-config-mapping", -14, &map, &diagnostic);
    if (!protected_virtual_span(reader, table, proof, get_rsp() & ~4095ULL, 4096))
        return boot_refuse("bsp-stack-mapping", -14, &map, &diagnostic);
    // Firmware allocates AP stacks and saved MTRRs separately. Their page-rounded
    // storage must not alias either retained package even when in one EFI region.
    unsigned stack_index = 0;
    for (auto *stack : config.temp_stack)
    {
        if (!stack || !protected_virtual_span(reader, table, proof, reinterpret_cast<uintptr_t>(stack), STACK_SIZE))
        {
            write_serial_fmt_heapless("installer: memory proof AP stack index=%u va=0x%llx bytes=0x%llx\n", stack_index,
                                      (unsigned long long)reinterpret_cast<uintptr_t>(stack),
                                      (unsigned long long)STACK_SIZE);
            return boot_refuse(stack ? "ap-stack-mapping" : "ap-stack-missing", -14, &map, &diagnostic);
        }
        ++stack_index;
    }
    if (config.saved_mtrrs)
    {
        uint64_t physical = reinterpret_cast<uintptr_t>(config.saved_mtrrs);
        uint64_t bytes = ((rdmsr(0xfe) & 255) * 2 + 12) * sizeof(uint64_t);
        if (physical >= direct_map_bytes || bytes > direct_map_bytes - physical)
        {
            write_serial_fmt_heapless("installer: memory proof MTRR physical=0x%llx bytes=0x%llx\n",
                                      (unsigned long long)physical, (unsigned long long)bytes);
            return boot_refuse("mtrr-bounds", -14, &map);
        }
        if (!protected_virtual_span(reader, table, proof, direct_map_base + physical, bytes))
            return boot_refuse("mtrr-mapping", -14, &map, &diagnostic);
    }
    // Include branches unrelated to the addresses checked above. An active
    // table hidden in another PML4 branch must never alias a package page.
    if (!verify_table_closure(reader, table, &table_workspace))
        return boot_refuse("table-closure", -14, &map, &diagnostic);
    saved_config = config;
    package_proof = proof;
    // Both arenas combined are capped by currently free conventional RAM and a
    // fixed ceiling. This is a pre-allocation budget, not a later disk guarantee.
    arena_budget = summary.allocatable_pages * 4096 / 8;
    if (arena_budget > (64ULL << 20)) arena_budget = 64ULL << 20;
    __atomic_store_n(&state, memory_proven, __ATOMIC_RELEASE);
    write_serial_fmt_heapless("installer: memory proof passed tables=%llu arena_budget=0x%llx\n",
                              (unsigned long long)table_workspace.count, (unsigned long long)arena_budget);
    return 0;
}

int session_packages_prepare()
{
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != memory_proven) return -19;
    // Boot-thread only; no state/registry lock may cover allocation or parsing.
    uint64_t budget = arena_budget;
    int result = validate_package(saved_config.installer_handoff.root, true, budget, &root_records, root_plan);
    ext4_span = saved_config.installer_handoff.system;
    if (result >= 0 && package_proof.profile == XJ_INSTALLER_PROFILE_STREAM_INDEX)
    {
        const auto &whole = saved_config.installer_handoff.system;
        const auto *data = reinterpret_cast<const uint8_t *>(direct_map_base + whole.physical);
        result = xjinstaller::stream_index_validate(data, whole.bytes,
                    saved_config.installer_handoff.source.volume_bytes, source_index);
        if (!result)
        {
            source_read_context.source = &retained_source;
            source_read_context.index = &source_index;
            source_reader = {read_stream_source, &source_read_context, source_index.data_bytes};
            ext4_span = {whole.physical + uint64_t(source_index.ext4.data - data), source_index.ext4.size, 0};
        }
    }
    else if (result >= 0 && package_proof.profile == XJ_INSTALLER_PROFILE_DUAL)
    {
        const auto &whole = saved_config.installer_handoff.system;
        const auto *data = reinterpret_cast<const uint8_t *>(direct_map_base + whole.physical);
        xj_installer_payloads payloads;
        if (!xj_installer_split_payloads(data, whole.bytes, &payloads)) result = -22;
        else
        {
            // Older dual media can supply its ext4 package. The subspan stays
            // covered by the proven parent span and shares its frame lifetime.
            ext4_span = {whole.physical + uint64_t(payloads.ext4 - data), payloads.ext4_bytes, 0};
        }
    }
    if (result >= 0)
        result = validate_package(ext4_span,
                                   package_proof.profile == XJ_INSTALLER_PROFILE_FAT32_PAK1,
                                   budget, &system_records, system_plan,
                                   source_reader.read ? &source_reader : nullptr);
    if (result < 0)
    {
        free(root_records);
        free(system_records);
        root_records = system_records = nullptr;
        root_plan = {};
        system_plan = {};
        return refuse(result);
    }
    __atomic_store_n(&state, packages_proven, __ATOMIC_RELEASE);
    return 0;
}

bool session_snapshot(SessionSnapshot *output)
{
    if (!output) return false;
    *output = {};
    unsigned observed = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
    if (observed != packages_proven && observed != source_retained) return false;
    output->epoch = 1;
    output->profile = package_proof.profile;
    output->ready = observed == source_retained &&
                    __atomic_load_n(&native_admission, __ATOMIC_ACQUIRE) == 1 &&
                    (package_proof.profile == XJ_INSTALLER_PROFILE_EXT4_PAK2 ||
                     package_proof.profile == XJ_INSTALLER_PROFILE_DUAL ||
                     package_proof.profile == XJ_INSTALLER_PROFILE_STREAM_INDEX) &&
                    installer_root_is_tmpfs_ready() && validate_source(retained_source) == 0;
    return true;
}

int session_source_prepare()
{
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != packages_proven) return -19;
    // Boot-owned storage survives caller exit. The exclusive source lease
    // prevents deletion/re-registration and excludes every physical alias.
    // Other worker TCBs may validate it but never borrow its I/O authority.
    int result = retain_source(saved_config.installer_handoff.source, &retained_source,
                               package_proof.profile == XJ_INSTALLER_PROFILE_STREAM_INDEX);
    if (result < 0) return refuse(result);
    result = publish_source(&retained_source);
    if (result < 0)
    {
        quarantine_source(&retained_source);
        return refuse(result);
    }
    source_extent = retained_source.reference.extent;
    __atomic_store_n(&state, source_retained, __ATOMIC_RELEASE);
    return 0;
}

int session_root_package_view(xjpak2::Plan *plan, uint64_t *epoch)
{
    if (plan) *plan = {};
    if (epoch) *epoch = 0;
    if (!plan || !epoch) return -22;
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != source_retained) return -19;
    int result = validate_source(retained_source);
    if (result < 0) return result;
    // Match the private-root builder's conservative metadata admission budget.
    // This is not an allocator reservation: every later allocation still has
    // a rollback path. The same RAM bound already limits the retained arenas.
    if (root_plan.inode_count > (__UINT64_MAX__ - 4096) / 1024) return -12;
    uint64_t overhead = root_plan.inode_count * 1024 + 4096;
    if (overhead > arena_budget || root_plan.data_bytes > arena_budget - overhead) return -12;
    *plan = root_plan;
    *epoch = 1;
    return 0;
}

bool session_job_view(JobSessionView *output)
{
    if (!output) return false;
    *output = {};
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != source_retained || validate_source(retained_source) < 0)
        return false;
    output->epoch = 1;
    output->components = XJ380_INSTALLER_COMPONENT_ALL;
    output->profile = package_proof.profile;
    // Explicitly keep the packaged locale; no post-activation mutation or
    // in-place package edits implement the old language override options.
    output->language = uint32_t(-1);
    output->source = source_node(retained_source);
    output->source_extent = source_extent;
    output->package = reinterpret_cast<const void *>(direct_map_base + ext4_span.physical);
    output->package_bytes = ext4_span.bytes;
    output->source_reader = source_reader;
    output->ready = __atomic_load_n(&native_admission, __ATOMIC_ACQUIRE) == 1 &&
                    (package_proof.profile == XJ_INSTALLER_PROFILE_EXT4_PAK2 ||
                     package_proof.profile == XJ_INSTALLER_PROFILE_DUAL ||
                     package_proof.profile == XJ_INSTALLER_PROFILE_STREAM_INDEX) &&
                    installer_root_is_tmpfs_ready();
    return output->source != nullptr;
}

int session_activate_native()
{
    // Only the trusted graphics worker calls this after its initialization.
    // Publication alone never authorizes a user request or fabricates a user.
    tcb_t caller = get_current_task();
    if (!caller || !kernel_group || caller->parent_group != kernel_group) return -13;
    if (!scheduler_is_enabled() || !installer_root_is_tmpfs_ready() ||
        __atomic_load_n(&state, __ATOMIC_ACQUIRE) != source_retained) return -19;
    int result = validate_source(retained_source);
    if (result < 0) return result;
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&native_admission, &expected, 1, false,
                                     __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) return -16;
    return 0;
}

void session_revoke_native()
{
    __atomic_store_n(&native_admission, 2, __ATOMIC_RELEASE);
}

int session_validate_job_view(const JobSessionView &view)
{
    JobSessionView current = {};
    if (!session_job_view(&current) || !current.ready) return -19;
    if (!view.ready || view.epoch != current.epoch || view.profile != current.profile ||
        view.components != current.components || view.language != current.language ||
        view.source != current.source || view.package != current.package ||
        view.package_bytes != current.package_bytes ||
        view.fat32_package != current.fat32_package || view.fat32_package_bytes != current.fat32_package_bytes ||
        view.source_reader.read != current.source_reader.read || view.source_reader.context != current.source_reader.context ||
        view.source_reader.bytes != current.source_reader.bytes ||
        view.source_extent.device != current.source_extent.device ||
        view.source_extent.physical_device != current.source_extent.physical_device ||
        view.source_extent.generation != current.source_extent.generation ||
        view.source_extent.physical_generation != current.source_extent.physical_generation ||
        view.source_extent.start != current.source_extent.start ||
        view.source_extent.bytes != current.source_extent.bytes)
        return -116;
    return 0;
}
} // namespace installer
