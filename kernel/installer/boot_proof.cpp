// SPDX-License-Identifier: Apache-2.0
// Original boot package ownership and x86-64 translation checks.

#include <installer/boot_proof.h>

namespace installer
{
namespace
{
constexpr uint64_t address_mask = 0x000ffffffffff000ULL;
constexpr uint64_t present = 1, user = 4, huge = 128;

bool table_failed(TableReader reader, const char *reason, uint64_t table, unsigned level = 0, unsigned index = 0,
                  uint64_t entry = 0)
{
    if (reader.diagnostic && !reader.diagnostic->reason)
    {
        reader.diagnostic->reason = reason;
        reader.diagnostic->table  = table;
        reader.diagnostic->level  = level;
        reader.diagnostic->index  = index;
        reader.diagnostic->entry  = entry;
    }
    return false;
}

bool span_failed(TableReader reader, const char *reason, uint64_t address, uint64_t bytes, uint64_t physical = 0)
{
    if (reader.diagnostic)
    {
        // Keep the leaf failure (including its PTE) when a containing span fails.
        if (!reader.diagnostic->reason) reader.diagnostic->reason = reason;
        reader.diagnostic->address  = address;
        reader.diagnostic->bytes    = bytes;
        reader.diagnostic->physical = physical;
    }
    return false;
}

bool canonical(uint64_t address)
{
    return address < (1ULL << 47) || address >= direct_map_base;
}

bool reader_root_valid(TableReader reader, uint64_t root_table, uint64_t *physical_mask)
{
    if (!reader.read_entry) return table_failed(reader, "table-reader-missing", root_table);
    if (reader.physical_bits < 32 || reader.physical_bits > 52)
        return table_failed(reader, "physical-width-invalid", root_table);
    if (!root_table || (root_table & ~address_mask))
        return table_failed(reader, "root-table-address-invalid", root_table);
    *physical_mask = ((1ULL << reader.physical_bits) - 1) & address_mask;
    if (root_table & ~*physical_mask) return table_failed(reader, "root-table-physical-width", root_table);
    return true;
}

bool entry_is_leaf(unsigned level, uint64_t entry)
{
    return level == 1 || ((level == 2 || level == 3) && (entry & huge));
}

bool present_entry_valid(TableReader reader, uint64_t physical_mask, uint64_t table, unsigned level, unsigned index,
                         uint64_t entry, bool *leaf, bool allow_high_leaf = false)
{
    if ((entry & address_mask & ~physical_mask) && !(allow_high_leaf && entry_is_leaf(level, entry)))
        return table_failed(reader, "entry-physical-width", table, level, index, entry);
    if (!reader.nx_enabled && (entry >> 63))
        return table_failed(reader, "entry-nx-without-nxe", table, level, index, entry);
    if (level == 4 && (entry & huge)) return table_failed(reader, "pml4-huge-bit", table, level, index, entry);
    if (level == 3 && (entry & huge) && !reader.gigabyte_pages)
        return table_failed(reader, "gigabyte-pages-unsupported", table, level, index, entry);
    *leaf = entry_is_leaf(level, entry);
    if (*leaf && level != 1)
    {
        uint64_t mask = (1ULL << (12 + 9 * (level - 1))) - 1;
        // Large-page bit 12 is PAT; other physical bits below the leaf size
        // are reserved. Bit 7 at level 1 is PAT rather than another level.
        if (entry & address_mask & mask & ~(1ULL << 12))
            return table_failed(reader, "huge-page-reserved-bits", table, level, index, entry);
    }
    return true;
}

bool visit_table(TableReader reader, uint64_t physical_mask, uint64_t physical, unsigned level,
                 TableWorkspace *workspace)
{
    if (!physical || (physical & ~physical_mask))
        return table_failed(reader, "child-table-address-invalid", physical, level);
    for (uint64_t i = 0; i < workspace->count; ++i)
    {
        if (workspace->entries[i].physical == physical)
        {
            if (workspace->entries[i].level != level)
                return table_failed(reader, "table-level-conflict", physical, level);
            return true;
        }
    }
    if (workspace->count == table_workspace_capacity)
        return table_failed(reader, "table-workspace-full", physical, level);
    workspace->entries[workspace->count++] = {physical, level};
    for (unsigned index = 0; index < 512; ++index)
    {
        uint64_t entry = 0;
        if (!reader.read_entry(reader.context, physical, index, &entry))
            return table_failed(reader, "table-read-failed", physical, level, index);
        if (!(entry & present)) continue;
        bool leaf = false;
        // Ownership closure follows table pointers, not leaf target addresses.
        // Firmware can retain a wider flat mapping when CPUID omits its width.
        // Permit that leaf's width alone; non-leaf pointers and every actual
        // translation still use the conservative mask before any target read.
        bool allow_high_leaf = !reader.physical_bits_known && reader.physical_bits == 36;
        if (!present_entry_valid(reader, physical_mask, physical, level, index, entry, &leaf, allow_high_leaf))
            return false;
        if (leaf && (entry & address_mask & ~physical_mask))
        {
            auto &report = workspace->report;
            if (!report.high_leaf_count)
            {
                report.first_table = physical;
                report.first_entry = entry;
                report.first_level = level;
                report.first_index = index;
            }
            ++report.high_leaf_count;
        }
        if (!leaf && !visit_table(reader, physical_mask, entry & address_mask, level - 1, workspace)) return false;
    }
    return true;
}
} // namespace

bool resolve_mapping(TableReader reader, uint64_t root_table, uint64_t address, Mapping *output)
{
    if (!output) return span_failed(reader, "mapping-output-missing", address, 0);
    *output = {};
    uint64_t physical_mask = 0;
    if (!canonical(address)) return span_failed(reader, "virtual-address-noncanonical", address, 0);
    if (!reader_root_valid(reader, root_table, &physical_mask))
        return span_failed(reader, "root-table-invalid", address, 0);
    uint64_t table = root_table;
    bool user_access = true;
    for (unsigned level = 4; level != 0; --level)
    {
        unsigned shift = 12 + 9 * (level - 1);
        unsigned index = unsigned((address >> shift) & 511);
        uint64_t entry = 0;
        if (!reader.read_entry(reader.context, table, index, &entry))
        {
            table_failed(reader, "table-read-failed", table, level, index);
            return span_failed(reader, "mapping-unreadable", address, 0);
        }
        if (!(entry & present))
        {
            table_failed(reader, "entry-not-present", table, level, index, entry);
            return span_failed(reader, "mapping-absent", address, 0);
        }
        bool leaf = false;
        if (!present_entry_valid(reader, physical_mask, table, level, index, entry, &leaf))
            return span_failed(reader, "mapping-entry-invalid", address, 0);
        user_access = user_access && (entry & user);
        if (leaf)
        {
            uint64_t mask = (1ULL << shift) - 1;
            *output = {(entry & address_mask & ~mask) + (address & mask),
                       (1ULL << shift) - (address & mask), !user_access};
            return true;
        }
        table = entry & address_mask;
        if (!table)
        {
            table_failed(reader, "child-table-zero", 0, level - 1, index, entry);
            return span_failed(reader, "mapping-child-invalid", address, 0);
        }
    }
    return span_failed(reader, "mapping-walk-exhausted", address, 0);
}

bool verify_direct_span(TableReader reader, uint64_t root_table, uint64_t physical, uint64_t bytes)
{
    if (!physical || !bytes || physical >= direct_map_bytes || bytes > direct_map_bytes - physical)
        return span_failed(reader, "direct-span-bounds", physical, bytes);
    while (bytes)
    {
        Mapping mapping = {};
        if (!resolve_mapping(reader, root_table, direct_map_base + physical, &mapping))
            return span_failed(reader, "direct-span-unmapped", direct_map_base + physical, bytes);
        if (!mapping.supervisor)
            return span_failed(reader, "direct-span-user-accessible", direct_map_base + physical, bytes,
                               mapping.physical);
        if (mapping.physical != physical)
            return span_failed(reader, "direct-span-wrong-physical", direct_map_base + physical, bytes,
                               mapping.physical);
        uint64_t step = bytes < mapping.remaining ? bytes : mapping.remaining;
        bytes -= step;
        physical += step;
    }
    return true;
}

bool verify_table_closure(TableReader reader, uint64_t root_table, TableWorkspace *workspace)
{
    if (!workspace) return table_failed(reader, "table-workspace-missing", root_table);
    workspace->count = 0;
    workspace->report      = {};
    uint64_t physical_mask = 0;
    if (!reader_root_valid(reader, root_table, &physical_mask) ||
        !visit_table(reader, physical_mask, root_table, 4, workspace))
    {
        workspace->count = 0;
        workspace->report = {};
        return false;
    }
    return true;
}

bool packages_overlap(const BootPackageProof &proof, uint64_t physical, uint64_t bytes)
{
    if (!proof.valid || !bytes) return false;
    if (physical > __UINT64_MAX__ - bytes) return true;
    uint64_t end = physical + bytes;
    return (physical < proof.root.end && proof.root.start < end) ||
           (physical < proof.system.end && proof.system.start < end);
}

int prove_boot_packages(const XJ_INSTALLER_HANDOFF &handoff, const boot_memory::MapView &map,
                        uint64_t map_physical, TableReader reader, uint64_t root_table, BootPackageProof *output)
{
    if (!output)
    {
        span_failed(reader, "package-proof-output-missing", 0, 0);
        return -22;
    }
    *output = {};
    if (!xj_installer_handoff_valid(&handoff))
    {
        span_failed(reader, "package-handoff-invalid", 0, 0);
        return -22;
    }
    BootPackageProof proof = {};
    boot_memory::PhysicalSpan map_span = {};
    boot_memory::PhysicalSpan loader_span = {};
    if (!boot_memory::covered_span(map, handoff.root.physical, handoff.root.pages * 4096,
                                   boot_memory::SpanPolicy::loader_data_only, &proof.root))
    {
        span_failed(reader, "root-package-efi-coverage", handoff.root.physical, handoff.root.pages * 4096);
        return -14;
    }
    if (!boot_memory::covered_span(map, handoff.system.physical, handoff.system.pages * 4096,
                                   boot_memory::SpanPolicy::loader_data_only, &proof.system))
    {
        span_failed(reader, "system-package-efi-coverage", handoff.system.physical, handoff.system.pages * 4096);
        return -14;
    }
    if (!boot_memory::covered_span(map, map_physical, map.map_bytes, boot_memory::SpanPolicy::loader_and_boot_services,
                                   &map_span))
    {
        span_failed(reader, "memory-map-efi-coverage", map_physical, map.map_bytes);
        return -14;
    }
    if (!boot_memory::covered_span(map, handoff.loader_image_physical, handoff.loader_image_bytes,
                                   boot_memory::SpanPolicy::loader_and_boot_services, &loader_span))
    {
        span_failed(reader, "loader-image-efi-coverage", handoff.loader_image_physical, handoff.loader_image_bytes);
        return -14;
    }
    proof.profile = handoff.system_profile;
    proof.valid = true;
    if (packages_overlap(proof, map_span.start, map_span.end - map_span.start))
    {
        span_failed(reader, "memory-map-package-overlap", map_span.start, map_span.end - map_span.start);
        return -14;
    }
    if (packages_overlap(proof, loader_span.start, loader_span.end - loader_span.start))
    {
        span_failed(reader, "loader-image-package-overlap", loader_span.start, loader_span.end - loader_span.start);
        return -14;
    }
    if (!verify_direct_span(reader, root_table, proof.root.start, proof.root.end - proof.root.start)) return -14;
    if (!verify_direct_span(reader, root_table, proof.system.start, proof.system.end - proof.system.start)) return -14;
    *output = proof;
    return 0;
}
} // namespace installer
