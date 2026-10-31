// SPDX-License-Identifier: Apache-2.0
// Original boot package ownership and x86-64 translation checks.

#include <installer/boot_proof.h>

namespace installer
{
namespace
{
constexpr uint64_t address_mask = 0x000ffffffffff000ULL;
constexpr uint64_t present = 1, user = 4, huge = 128;

bool canonical(uint64_t address)
{
    return address < (1ULL << 47) || address >= direct_map_base;
}

bool reader_root_valid(TableReader reader, uint64_t root_table, uint64_t *physical_mask)
{
    if (!reader.read_entry || reader.physical_bits < 32 || reader.physical_bits > 52 ||
        !root_table || (root_table & ~address_mask)) return false;
    *physical_mask = ((1ULL << reader.physical_bits) - 1) & address_mask;
    return !(root_table & ~*physical_mask);
}

bool present_entry_valid(TableReader reader, uint64_t physical_mask, unsigned level, uint64_t entry, bool *leaf)
{
    if ((entry & address_mask & ~physical_mask) || (!reader.nx_enabled && (entry >> 63))) return false;
    if (level == 4 && (entry & huge)) return false;
    if (level == 3 && (entry & huge) && !reader.gigabyte_pages) return false;
    *leaf = level == 1 || (entry & huge);
    if (*leaf && level != 1)
    {
        uint64_t mask = (1ULL << (12 + 9 * (level - 1))) - 1;
        // Large-page bit 12 is PAT; other physical bits below the leaf size
        // are reserved. Bit 7 at level 1 is PAT rather than another level.
        if (entry & address_mask & mask & ~(1ULL << 12)) return false;
    }
    return true;
}

bool visit_table(TableReader reader, uint64_t physical_mask, uint64_t physical, unsigned level,
                 TableWorkspace *workspace)
{
    if (!physical || (physical & ~physical_mask)) return false;
    for (uint64_t i = 0; i < workspace->count; ++i)
    {
        if (workspace->entries[i].physical == physical) return workspace->entries[i].level == level;
    }
    if (workspace->count == table_workspace_capacity) return false;
    workspace->entries[workspace->count++] = {physical, level};
    for (unsigned index = 0; index < 512; ++index)
    {
        uint64_t entry = 0;
        if (!reader.read_entry(reader.context, physical, index, &entry)) return false;
        if (!(entry & present)) continue;
        bool leaf = false;
        if (!present_entry_valid(reader, physical_mask, level, entry, &leaf)) return false;
        if (!leaf && !visit_table(reader, physical_mask, entry & address_mask, level - 1, workspace)) return false;
    }
    return true;
}
} // namespace

bool resolve_mapping(TableReader reader, uint64_t root_table, uint64_t address, Mapping *output)
{
    if (!output) return false;
    *output = {};
    uint64_t physical_mask = 0;
    if (!canonical(address) || !reader_root_valid(reader, root_table, &physical_mask)) return false;
    uint64_t table = root_table;
    bool user_access = true;
    for (unsigned level = 4; level != 0; --level)
    {
        unsigned shift = 12 + 9 * (level - 1);
        uint64_t entry = 0;
        if (!reader.read_entry(reader.context, table, unsigned((address >> shift) & 511), &entry) ||
            !(entry & present)) return false;
        bool leaf = false;
        if (!present_entry_valid(reader, physical_mask, level, entry, &leaf)) return false;
        user_access = user_access && (entry & user);
        if (leaf)
        {
            uint64_t mask = (1ULL << shift) - 1;
            *output = {(entry & address_mask & ~mask) + (address & mask),
                       (1ULL << shift) - (address & mask), !user_access};
            return true;
        }
        table = entry & address_mask;
        if (!table) return false;
    }
    return false;
}

bool verify_direct_span(TableReader reader, uint64_t root_table, uint64_t physical, uint64_t bytes)
{
    if (!physical || !bytes || physical >= direct_map_bytes || bytes > direct_map_bytes - physical) return false;
    while (bytes)
    {
        Mapping mapping = {};
        if (!resolve_mapping(reader, root_table, direct_map_base + physical, &mapping) ||
            !mapping.supervisor || mapping.physical != physical) return false;
        uint64_t step = bytes < mapping.remaining ? bytes : mapping.remaining;
        bytes -= step;
        physical += step;
    }
    return true;
}

bool verify_table_closure(TableReader reader, uint64_t root_table, TableWorkspace *workspace)
{
    if (!workspace) return false;
    workspace->count = 0;
    uint64_t physical_mask = 0;
    if (!reader_root_valid(reader, root_table, &physical_mask) ||
        !visit_table(reader, physical_mask, root_table, 4, workspace))
    {
        workspace->count = 0;
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
    if (!output) return -22;
    *output = {};
    if (!xj_installer_handoff_valid(&handoff)) return -22;
    BootPackageProof proof = {};
    boot_memory::PhysicalSpan map_span = {};
    boot_memory::PhysicalSpan loader_span = {};
    if (!boot_memory::covered_span(map, handoff.root.physical, handoff.root.pages * 4096,
                                   boot_memory::SpanPolicy::loader_data_only, &proof.root) ||
        !boot_memory::covered_span(map, handoff.system.physical, handoff.system.pages * 4096,
                                   boot_memory::SpanPolicy::loader_data_only, &proof.system) ||
        !boot_memory::covered_span(map, map_physical, map.map_bytes,
                                   boot_memory::SpanPolicy::loader_and_boot_services, &map_span) ||
        !boot_memory::covered_span(map, handoff.loader_image_physical, handoff.loader_image_bytes,
                                   boot_memory::SpanPolicy::loader_and_boot_services, &loader_span)) return -14;
    proof.profile = handoff.system_profile;
    proof.valid = true;
    if (packages_overlap(proof, map_span.start, map_span.end - map_span.start) ||
        packages_overlap(proof, loader_span.start, loader_span.end - loader_span.start) ||
        !verify_direct_span(reader, root_table, proof.root.start, proof.root.end - proof.root.start) ||
        !verify_direct_span(reader, root_table, proof.system.start, proof.system.end - proof.system.start)) return -14;
    *output = proof;
    return 0;
}
} // namespace installer
