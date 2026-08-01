// 我也不知道该放哪，应该不会有什么冲突罢，那就先放全局了
#ifndef _MEMORY_H_
#define _MEMORY_H_
#define FREE_MEMORY 0
#define OS_COTA     1 // code&data
// #define OS_CODE     1
// #define OS_DATA     2
#define AP_CODE     3
#define AP_DATA     4
#define UEFI_MEMORY 11
#define MMIO_MEMORY 12
#pragma pack(1)
#include <efi/efi.h>

typedef struct __packed
{
    uint64_t MapSize;
    void    *Buffer;
    uint64_t MapKey;
    uint64_t DescriptorSize;
    uint32_t DescriptorVersion;
} MEMORY_MAP;

typedef struct
{
    uint32_t Type;
    uint64_t PhysicalStart;
    uint64_t PageSize;
} OS_MEMORY_DESCRIPTOR;

#pragma pack()

typedef struct
{
    uint32_t             Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    uint64_t             NumberOfPages;
    uint64_t             Attribute;
} __attribute__((__aligned__(16))) EfiMemoryDesc;

#define PAGE_SIZE ((size_t)4096UL)
#define TOP_LEVEL 4 // 页表层数，给SMP初始化AP用的，动了会不会炸我也不清楚

#endif
