#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <efi/installer_handoff.h>
#include <mm/boot_memory.h>

namespace installer
{
constexpr uint64_t direct_map_base = 0xffff800000000000ULL;
constexpr uint64_t direct_map_bytes = 1ULL << 47;

struct TableReader
{
    void *context;
    // Boot page tables themselves are trusted architectural bootstrap state.
    // The adapter supplies mapped, stable table storage and may refuse reads.
    bool (*read_entry)(void *, uint64_t table_physical, unsigned index, uint64_t *);
    uint8_t physical_bits = 52;
    bool nx_enabled = true;
    bool gigabyte_pages = true;
};

struct Mapping
{
    uint64_t physical, remaining;
    bool supervisor;
};

constexpr uint64_t table_workspace_capacity = 8192;

struct TableWorkEntry
{
    uint64_t physical;
    unsigned level;
};

struct TableWorkspace
{
    // Large boot-lifetime scratch: supply static/BSS storage, never a boot stack object.
    TableWorkEntry entries[table_workspace_capacity];
    uint64_t count;
};

struct BootPackageProof
{
    boot_memory::PhysicalSpan root, system;
    uint32_t profile;
    bool valid;
};

bool resolve_mapping(TableReader, uint64_t root_table, uint64_t virtual_address, Mapping *);
bool verify_direct_span(TableReader, uint64_t root_table, uint64_t physical, uint64_t bytes);
// Visits every present non-leaf table in the current four-level tree. The reader
// must reject table pages overlapping packages before reading their first byte.
// Same-level sharing is legal; reuse at another level and capacity exhaustion fail.
// The workspace count is cleared before traversal and on failure. No allocation or I/O.
bool verify_table_closure(TableReader, uint64_t root_table, TableWorkspace *);
bool packages_overlap(const BootPackageProof &, uint64_t physical, uint64_t bytes);
// Maps are already readable bootstrap storage. No package byte is read here.
// Proves exact page ownership, disjoint map storage and supervisor HHDM mapping.
int prove_boot_packages(const XJ_INSTALLER_HANDOFF &, const boot_memory::MapView &,
                        uint64_t map_physical, TableReader, uint64_t root_table, BootPackageProof *);
} // namespace installer
