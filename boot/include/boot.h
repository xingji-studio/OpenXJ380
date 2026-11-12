// 我也不知道该放哪，应该不会有什么冲突罢，那就先放全局了
#ifndef _BOOT_H_
#define _BOOT_H_
#include <efi.h>
#include <memory.h>
#include "../../include/efi/root_binding.h"
#include "../../include/efi/installer_handoff.h"

#define MAX_CPU_NUM 256

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

    UINT64 FADT;
    UINT64 MADT;
    UINT64 HPET;
    UINT64 MCFG;

    // Preallocated AP stacks used before the kernel memory allocator is available.
    UINT64 *saved_mtrrs;
    void   *temp_stack[MAX_CPU_NUM];

    int is_qemu;

    UINT64 boot_flags;

    UINT64 installer_root_pak;
    UINT64 installer_root_pak_size;
    UINT64 system_payload_pak;
    UINT64 system_payload_pak_size;
    // The legacy prefix ends here. root_binding requires BOOT_FLAG_BOUND_ROOT.
    XJ_ROOT_BINDING root_binding;
    // Read this versioned tail only with BOOT_FLAG_INSTALLER_HANDOFF.
    XJ_INSTALLER_HANDOFF installer_handoff;
} BOOT_CONFIG;

#define BOOT_CONFIG_LEGACY_BYTES __builtin_offsetof(BOOT_CONFIG, root_binding)
#define BOOT_CONFIG_INSTALLER_HANDOFF_OFFSET __builtin_offsetof(BOOT_CONFIG, installer_handoff)

#endif
