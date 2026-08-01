#ifndef _BOOT_H_
#define _BOOT_H_
#include <efi/efi.h>
#include <mm/memory.h>
#include <smp/smp.h>

#define BOOT_FLAG_SAFE_MODE        (1ULL << 0)
#define BOOT_FLAG_VERBOSE          (1ULL << 1)
#define BOOT_FLAG_DISABLE_KMOD     (1ULL << 2)
#define BOOT_FLAG_SAFE_STORAGE_IO  (1ULL << 3)
#define BOOT_FLAG_BASE_VIDEO       (1ULL << 4)
#define BOOT_FLAG_LAST_KNOWN_GOOD  (1ULL << 5)
#define BOOT_FLAG_SHOW_PROGRESS    (1ULL << 6)
#define BOOT_FLAG_INSTALLER        (1ULL << 7)

typedef struct
{
    MEMORY_MAP MemoryMap;

    uint64_t FADT;
    uint64_t MADT;
    uint64_t HPET;
    uint64_t MCFG;

    // SMP用的，因为懒得写内存管理，属于是另辟蹊径了……
    uint64_t *saved_mtrrs;
    void     *temp_stack[MAX_CPU_NUM];

    int is_qemu;

    uint64_t boot_flags;

    uint64_t installer_root_pak;
    uint64_t installer_root_pak_size;
    uint64_t system_payload_pak;
    uint64_t system_payload_pak_size;
} BOOT_CONFIG;

#endif
