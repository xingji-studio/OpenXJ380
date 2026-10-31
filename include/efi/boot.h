#ifndef _BOOT_H_
#define _BOOT_H_
#include <efi/efi.h>
#include <mm/memory.h>
#include <smp/smp.h>
#include <efi/root_binding.h>
#include <efi/installer_handoff.h>

#define BOOT_FLAG_SAFE_MODE        (1ULL << 0)
#define BOOT_FLAG_VERBOSE          (1ULL << 1)
#define BOOT_FLAG_DISABLE_KMOD     (1ULL << 2)
#define BOOT_FLAG_SAFE_STORAGE_IO  (1ULL << 3)
#define BOOT_FLAG_BASE_VIDEO       (1ULL << 4)
#define BOOT_FLAG_LAST_KNOWN_GOOD  (1ULL << 5)
#define BOOT_FLAG_SHOW_PROGRESS    (1ULL << 6)
#define BOOT_FLAG_INSTALLER        (1ULL << 7)
#define BOOT_FLAG_BOUND_ROOT       (1ULL << 8)
#define BOOT_FLAG_INSTALLER_HANDOFF (1ULL << 9)

typedef struct
{
    MEMORY_MAP MemoryMap;

    uint64_t FADT;
    uint64_t MADT;
    uint64_t HPET;
    uint64_t MCFG;

    // Preallocated AP stacks used before the kernel memory allocator is available.
    uint64_t *saved_mtrrs;
    void     *temp_stack[MAX_CPU_NUM];

    int is_qemu;

    uint64_t boot_flags;

    uint64_t installer_root_pak;
    uint64_t installer_root_pak_size;
    uint64_t system_payload_pak;
    uint64_t system_payload_pak_size;
    // The legacy prefix ends here. root_binding requires BOOT_FLAG_BOUND_ROOT.
    XJ_ROOT_BINDING root_binding;
    // Read this versioned tail only with BOOT_FLAG_INSTALLER_HANDOFF.
    XJ_INSTALLER_HANDOFF installer_handoff;
} BOOT_CONFIG;

#define BOOT_CONFIG_LEGACY_BYTES __builtin_offsetof(BOOT_CONFIG, root_binding)
#define BOOT_CONFIG_INSTALLER_HANDOFF_OFFSET __builtin_offsetof(BOOT_CONFIG, installer_handoff)

#endif
