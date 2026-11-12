// SPDX-License-Identifier: Apache-2.0
// Original package lifetime staging and explicit native-service admission.

#include <installer/boot_proof.h>
#include <installer/package.h>
#include <installer/session.h>
#include <installer/context.h>
#include <installer/source_lifetime.h>
#include <installer/jobs.h>
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
BlockExtent source_extent;
SourceLifetime retained_source;

struct TableContext
{
    boot_memory::MapView map;
    BootPackageProof packages;
    uint64_t checked_tables[16];
    unsigned next_table;
};

bool read_table(void *opaque, uint64_t physical, unsigned index, uint64_t *output)
{
    auto &context = *static_cast<TableContext *>(opaque);
    if (!physical || (physical & 4095) || physical >= direct_map_bytes || index >= 512 ||
        packages_overlap(context.packages, physical, 4096)) return false;
    bool checked = false;
    for (uint64_t cached : context.checked_tables) if (cached == physical) checked = true;
    if (!checked)
    {
        // The final map is validated once before any table read. Avoid running
        // its overlap scan for every PTE. Firmware's current page tables are
        // trusted bootstrap storage, not a caller-controlled pointer graph.
        bool owned = false;
        for (uint64_t i = 0; i < context.map.map_bytes / context.map.descriptor_bytes; ++i)
        {
            boot_memory::Descriptor descriptor = {};
            if (!boot_memory::descriptor_at(context.map, i, &descriptor)) return false;
            if (physical >= descriptor.physical_start &&
                physical - descriptor.physical_start < descriptor.pages * 4096)
            {
                owned = descriptor.type >= 1 && descriptor.type <= 4;
                break;
            }
        }
        if (!owned) return false;
        context.checked_tables[context.next_table++ % 16] = physical;
    }
    *output = reinterpret_cast<volatile const uint64_t *>(direct_map_base + physical)[index];
    return true;
}

bool protected_virtual_span(TableReader reader, uint64_t table, const BootPackageProof &proof,
                            uint64_t address, uint64_t bytes)
{
    if (!bytes || address > __UINT64_MAX__ - bytes) return false;
    while (bytes)
    {
        Mapping mapping = {};
        if (!resolve_mapping(reader, table, address, &mapping) || !mapping.supervisor) return false;
        uint64_t step = bytes < mapping.remaining ? bytes : mapping.remaining;
        // Reject overlap at page granularity, including the last partial page.
        uint64_t physical = mapping.physical & ~4095ULL;
        uint64_t covered = ((mapping.physical & 4095) + step + 4095) & ~4095ULL;
        if (packages_overlap(proof, physical, covered)) return false;
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

int validate_package(const XJ_INSTALLER_PACKAGE_SPAN &span, bool legacy,
                     uint64_t &remaining_budget, xjpak2::Record **records, xjpak2::Plan &plan)
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
    result = xjpak2::validate(bytes, span.bytes, limits, {*records, header.record_count}, plan);
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
        return -16;
    if (!(config.boot_flags & BOOT_FLAG_INSTALLER_HANDOFF) || (config.boot_flags & BOOT_FLAG_BOUND_ROOT) ||
        !xj_installer_handoff_valid(&config.installer_handoff)) return refuse(-95);
    const auto &handoff = config.installer_handoff;
    if (config.installer_root_pak != handoff.root.physical || config.installer_root_pak_size != handoff.root.bytes ||
        config.system_payload_pak != handoff.system.physical || config.system_payload_pak_size != handoff.system.bytes)
        return refuse(-22);
    uint64_t map_physical = reinterpret_cast<uintptr_t>(config.MemoryMap.Buffer);
    if (!map_physical || map_physical >= direct_map_bytes ||
        config.MemoryMap.MapSize > direct_map_bytes - map_physical) return refuse(-14);
    // The readable boot config, final map and current translation tree are the
    // architecture's existing bootstrap contract. Package addresses receive no
    // such trust and are not dereferenced until the complete proof succeeds.
    boot_memory::MapView map = {reinterpret_cast<const void *>(direct_map_base + map_physical),
                               config.MemoryMap.MapSize, config.MemoryMap.MapSize, config.MemoryMap.DescriptorSize};
    boot_memory::Summary summary = {};
    if (!boot_memory::validate_map(map, &summary)) return refuse(-14);
    TableContext context = {};
    context.map = map;
    context.packages = {{handoff.root.physical, handoff.root.physical + handoff.root.pages * 4096},
                        {handoff.system.physical, handoff.system.physical + handoff.system.pages * 4096},
                        handoff.system_profile, true};
    TableReader reader = {&context, read_table};
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
    BootPackageProof proof = {};
    int result = prove_boot_packages(handoff, map, map_physical, reader, table, &proof);
    if (result < 0) return refuse(result);
    uint64_t kernel_start = reinterpret_cast<uintptr_t>(_kernel_start);
    uint64_t kernel_end = reinterpret_cast<uintptr_t>(__ksymtab_end);
    if (kernel_end <= kernel_start ||
        !protected_virtual_span(reader, table, proof, kernel_start, kernel_end - kernel_start) ||
        !protected_virtual_span(reader, table, proof, reinterpret_cast<uintptr_t>(original_config),
                                sizeof(BOOT_CONFIG)) ||
        !protected_virtual_span(reader, table, proof, get_rsp() & ~4095ULL, 4096)) return refuse(-14);
    // Firmware allocates AP stacks and saved MTRRs separately. Their page-rounded
    // storage must not alias either retained package even when in one EFI region.
    for (auto *stack : config.temp_stack)
        if (!stack || !protected_virtual_span(reader, table, proof, reinterpret_cast<uintptr_t>(stack), STACK_SIZE))
            return refuse(-14);
    if (config.saved_mtrrs)
    {
        uint64_t physical = reinterpret_cast<uintptr_t>(config.saved_mtrrs);
        uint64_t bytes = ((rdmsr(0xfe) & 255) * 2 + 12) * sizeof(uint64_t);
        if (physical >= direct_map_bytes || bytes > direct_map_bytes - physical ||
            !protected_virtual_span(reader, table, proof, direct_map_base + physical, bytes)) return refuse(-14);
    }
    // Include branches unrelated to the addresses checked above. An active
    // table hidden in another PML4 branch must never alias a package page.
    if (!verify_table_closure(reader, table, &table_workspace)) return refuse(-14);
    saved_config = config;
    package_proof = proof;
    // Both arenas combined are capped by currently free conventional RAM and a
    // fixed ceiling. This is a pre-allocation budget, not a later disk guarantee.
    arena_budget = summary.allocatable_pages * 4096 / 8;
    if (arena_budget > (64ULL << 20)) arena_budget = 64ULL << 20;
    __atomic_store_n(&state, memory_proven, __ATOMIC_RELEASE);
    return 0;
}

int session_packages_prepare()
{
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != memory_proven) return -19;
    // Boot-thread only; no state/registry lock may cover allocation or parsing.
    uint64_t budget = arena_budget;
    int result = validate_package(saved_config.installer_handoff.root, true, budget, &root_records, root_plan);
    if (result >= 0)
        result = validate_package(saved_config.installer_handoff.system,
                                   package_proof.profile == XJ_INSTALLER_PROFILE_FAT32_PAK1,
                                   budget, &system_records, system_plan);
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
                    package_proof.profile == XJ_INSTALLER_PROFILE_EXT4_PAK2 &&
                    installer_root_is_tmpfs_ready() && validate_source(retained_source) == 0;
    return true;
}

int session_source_prepare()
{
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != packages_proven) return -19;
    // Boot-owned storage survives caller exit. The exclusive source lease
    // prevents deletion/re-registration and excludes every physical alias.
    // Other worker TCBs may validate it but never borrow its I/O authority.
    int result = retain_source(saved_config.installer_handoff.source, &retained_source);
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
    output->package = reinterpret_cast<const void *>(direct_map_base + saved_config.installer_handoff.system.physical);
    output->package_bytes = saved_config.installer_handoff.system.bytes;
    output->ready = __atomic_load_n(&native_admission, __ATOMIC_ACQUIRE) == 1 &&
                    package_proof.profile == XJ_INSTALLER_PROFILE_EXT4_PAK2 &&
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
