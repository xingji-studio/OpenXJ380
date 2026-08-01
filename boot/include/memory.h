// 我也不知道该放哪，应该不会有什么冲突罢，那就先放全局了
#ifndef _MEMORY_H_
#define _MEMORY_H_
#define FREE_MEMORY 0
#define OS_COTA 1 // code&data
// #define OS_CODE     1
// #define OS_DATA     2
#define AP_CODE 3
#define AP_DATA 4
#define UEFI_MEMORY 11
#define MMIO_MEMORY 12
#pragma pack(1)
#include <efi.h>

typedef struct __packed
{
  UINTN MapSize;
  VOID *Buffer;
  UINTN MapKey;
  UINTN DescriptorSize;
  UINT32 DescriptorVersion;
} MEMORY_MAP;

typedef struct
{
  UINT32 Type;
  UINT64 PhysicalStart;
  UINT64 PageSize;
} OS_MEMORY_DESCRIPTOR;

#pragma pack()

typedef struct
{
    UINT32 Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} __attribute__((__aligned__(16))) EfiMemoryDesc;

#define PAGE_SIZE ((size_t)4096UL)
#define TOP_LEVEL 4   // 页表层数，给SMP初始化AP用的，动了会不会炸我也不清楚

#endif
