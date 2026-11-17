#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

// Decimal constants are also read by the host ISO preflight. Keep the root
// admission, managed RAM backend and packager on the same bounded profile.
#define XJ_INSTALLER_ROOT_MAX_ENTRIES    32768ULL
#define XJ_INSTALLER_ROOT_MEMORY_MAX     268435456ULL
#define XJ_INSTALLER_ROOT_ENTRY_OVERHEAD 1024ULL
#define XJ_INSTALLER_ROOT_BASE_OVERHEAD  4096ULL

namespace installer {
constexpr uint64_t root_entry_limit  = XJ_INSTALLER_ROOT_MAX_ENTRIES;
constexpr uint64_t root_memory_limit = XJ_INSTALLER_ROOT_MEMORY_MAX;

inline uint64_t root_memory_budget(uint64_t allocatable_pages)
{
    // The copied root keeps the existing one-eighth share of proven free boot
    // RAM. This ceiling is independent of the package record arenas and is
    // not a heap reservation.
    constexpr uint64_t bytes_per_page_share = 4096 / 8;
    return allocatable_pages > root_memory_limit / bytes_per_page_share ? root_memory_limit
                                                                        : allocatable_pages * bytes_per_page_share;
}

inline bool root_memory_fits(uint64_t entries, uint64_t data_bytes, uint64_t budget = root_memory_limit)
{
    // Use subtraction/division so hostile counts cannot overflow the estimate.
    return budget >= XJ_INSTALLER_ROOT_BASE_OVERHEAD && data_bytes <= budget - XJ_INSTALLER_ROOT_BASE_OVERHEAD &&
           entries <= (budget - XJ_INSTALLER_ROOT_BASE_OVERHEAD - data_bytes) / XJ_INSTALLER_ROOT_ENTRY_OVERHEAD;
}
} // namespace installer
