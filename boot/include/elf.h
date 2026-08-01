#ifndef _ELF_H_
#define _ELF_H_

#define PT_LOAD 1 // PT_LOAD值不变
#include <efi.h>
typedef struct
{
    UINT32 p_type;   /* Segment type */
    UINT32 p_flags;  /* Segment flags */
    UINT64 p_offset; /* Segment file offset */
    UINT64 p_vaddr;  /* Segment virtual address */
    UINT64 p_paddr;  /* Segment physical address */
    UINT64 p_filesz; /* Segment size in file */
    UINT64 p_memsz;  /* Segment size in memory */
    UINT64 p_align;  /* Segment alignment */
} Elf64_Phdr;

typedef struct
{
    UINT8 e_ident[16];  /* Magic number and other info */
    UINT16 e_type;      /* Object file type */
    UINT16 e_machine;   /* Architecture */
    UINT32 e_version;   /* Object file version */
    UINT64 e_entry;     /* Entry point virtual address */
    UINT64 e_phoff;     /* Program header table file offset */
    UINT64 e_shoff;     /* Section header table file offset */
    UINT32 e_flags;     /* Processor-specific flags */
    UINT16 e_ehsize;    /* ELF header size in bytes */
    UINT16 e_phentsize; /* Program header table entry size */
    UINT16 e_phnum;     /* Program header table entry count */
    UINT16 e_shentsize; /* Section header table entry size */
    UINT16 e_shnum;     /* Section header table entry count */
    UINT16 e_shstrndx;  /* Section header string table index */
} Elf64_Ehdr;

void CalcLoadAddressRange(Elf64_Ehdr *ehdr, UINT64 *first, UINT64 *last)
{
    Elf64_Phdr *phdr = (Elf64_Phdr *)((UINT64)ehdr + ehdr->e_phoff); // 第一个 program header 地址
    *first = 0xffffffffffffffff;                                     // UINT64最大值
    *last = 0;                                                       // UINT64最小值

    for (UINT16 i = 0; i < ehdr->e_phnum; i++)
    { // 遍历每一个 program header
        if (phdr[i].p_type != PT_LOAD)
            continue; // 只关心LOAD段
        *first = min(*first, phdr[i].p_vaddr);
        *last = max(*last, phdr[i].p_vaddr + phdr[i].p_memsz); // 每一个program header首尾取最值
    }
}

void CopyLoadSegments(Elf64_Ehdr *ehdr)
{
    Elf64_Phdr *phdr = (Elf64_Phdr *)((UINT64)ehdr + ehdr->e_phoff); // 第一个 program header 地址
    for (UINT16 i = 0; i < ehdr->e_phnum; i++)
    { // 遍历每一个 program header
        if (phdr[i].p_type != PT_LOAD)
            continue; // 只关心LOAD段

        UINT64 segm_in_file = (UINT64)ehdr + phdr[i].p_offset;                    // 段在文件中的位置
        xmemcpy((VOID *)phdr[i].p_vaddr, (VOID *)segm_in_file, phdr[i].p_filesz); // 将文件中大小的部分copy过去
        UINTN remain_bytes = phdr[i].p_memsz - phdr[i].p_filesz;                  // 两者之差
        xmemset((VOID *)(phdr[i].p_vaddr + phdr[i].p_filesz), 0, remain_bytes);   // 赋值为0
    }
}
#endif