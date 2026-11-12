// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

// EPF v1.0 disk sizes. The structures below hold decoded values; never cast
// file bytes to these structures or serialize their compiler-dependent layout.
#define EPF_HEADER_SIZE (128u)
#define EPF_SEGMENT_SIZE (64u)
#define EPF_SECTION_SIZE (64u)
#define EPF_NO_ENTRY (~0ULL)

#define EPF_REL (1u)
#define EPF_EXEC (2u)
#define EPF_SHARED (3u)
#define EPF_F_PIE (1u)
#define EPF_FEATURE_RUNTIME_RELOC (1ULL)
#define EPF_FEATURE_DYNAMIC_LINK (2ULL)
#define EPF_FEATURE_TLS (4ULL)
#define EPF_FEATURE_RELRO (8ULL)
#define EPF_MACHINE_X86_64 (62u)
// Project-local assignment, not a claim of registration in an EPF ABI registry.
#define EPF_OS_ABI_XJ380 (0x0380)
#define EPF_XJ380_ABI_VERSION (1u)
#define EPF_XJ380_TLS_ABI_VERSION (2u)
#define EPF_XJ380_CXX_TLS_ABI_VERSION (3u)

#define EPF_SEG_NULL (0u)
#define EPF_SEG_LOAD (1u)
#define EPF_SEG_DYNAMIC (2u)
#define EPF_SEG_INTERP (3u)
#define EPF_SEG_TLS (4u)
#define EPF_SEG_RELRO (5u)
#define EPF_SEG_NOTE (6u)
#define EPF_PF_R (1u)
#define EPF_PF_W (2u)
#define EPF_PF_X (4u)
#define EPF_PF_REQUIRED (0x80000000u)
#define EPF_SHF_ALLOC (1ULL)
#define EPF_SHF_WRITE (2ULL)
#define EPF_SHF_EXEC (4ULL)
#define EPF_SHF_TLS (8ULL)
#define EPF_SHF_REQUIRED (1ULL << 63)
#define EPF_SHT_NULL (0u)
#define EPF_SHT_PROGBITS (1u)
#define EPF_SHT_NOBITS (2u)
#define EPF_SHT_STRTAB (3u)
#define EPF_SHT_SYMTAB (4u)
#define EPF_SHT_RELA (5u)
#define EPF_SHT_DYNSYM (6u)
#define EPF_SHT_NOTE (7u)
#define EPF_SHT_DYNAMIC (8u)
#define EPF_DT_NULL (0u)
#define EPF_DT_RELA_RVA (6u)
#define EPF_DT_RELACNT (7u)
#define EPF_DT_RELAENT (8u)
#define EPF_DT_REQUIRED (1u)
#define EPF_RELOC_NONE (0u)
#define EPF_RELOC_RELATIVE32 (4u)
#define EPF_RELOC_RELATIVE64 (5u)
#define EPF_RELA_SIZE (32u)
#define EPF_DYNAMIC_SIZE (16u)

#define EPF_MAX_FILE_SIZE (64ULL << 20)
#define EPF_MAX_IMAGE_SIZE (1ULL << 30)
#define EPF_MAX_SEGMENTS (256u)
#define EPF_MAX_SECTIONS (4096u)
#define EPF_MAX_DYNAMIC_ENTRIES (256u)
#define EPF_MAX_RELOCATIONS (65536u)

typedef struct EpfHeader
{
    uint16_t type, machine, os_abi, abi_version;
    uint32_t flags;
    uint64_t required_features, image_base, entry_rva, image_size;
    uint64_t segment_table_offset, section_table_offset, file_size;
    uint32_t segment_count, section_count, shstr_index;
    uint16_t segment_entry_size, section_entry_size;
    uint64_t map_align;
    uint32_t arch_flags;
} EpfHeader;

typedef struct EpfSegment
{
    uint32_t type, flags;
    uint64_t file_offset, rva, file_size, mem_size, align;
} EpfSegment;

static inline uint16_t epf_read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t epf_read_u32(const uint8_t *p)
{
    return (uint32_t)epf_read_u16(p) | ((uint32_t)epf_read_u16(p + 2) << 16);
}

static inline uint64_t epf_read_u64(const uint8_t *p)
{
    return (uint64_t)epf_read_u32(p) | ((uint64_t)epf_read_u32(p + 4) << 32);
}

static inline int epf_has_magic(const uint8_t *p, uint64_t size)
{
    return p != 0 && size >= 4 && p[0] == 0x24 && p[1] == 0x45 && p[2] == 0x50 && p[3] == 0x46;
}

#define EPF_DT_STRTAB_RVA 1
#define EPF_DT_STRSZ 2
#define EPF_DT_SYMTAB_RVA 3
#define EPF_DT_SYMCNT 4
#define EPF_DT_SYMENT 5
#define EPF_DT_NEEDED 9
#define EPF_DT_SONAME 10
#define EPF_DT_INIT_ARRAY_RVA 11
#define EPF_DT_INIT_COUNT 12
#define EPF_DT_FINI_ARRAY_RVA 13
#define EPF_DT_FINI_COUNT 14
#define EPF_RELOC_ABS32 1
#define EPF_RELOC_ABS64 2
#define EPF_RELOC_PCREL32 3
#define EPF_SYMBOL_SIZE 32
#define EPF_MAX_SYMBOLS 65536
#define EPF_MAX_MODULES 32
#define EPF_MAX_NEEDED 32
#define EPF_MAX_LIFECYCLE 1024
#define EPF_INTERPRETER_PATH "/system/lib/ld-epf-x86_64.epf"
#define EPF_LIBRARY_DIRECTORY "/system/lib/epf/"

// AT_PHDR points to this immutable, page-aligned native handoff for interpreted
// EPF only. All fields are little-endian u64; descriptors follow at offset 64.
// AT_PHENT=64, AT_PHNUM=main segment count, AT_BASE=interpreter base,
// AT_ENTRY=main entry. This format does not use ELF program-header semantics.
#define EPF_HANDOFF_MAGIC 0x31464f4846504524ULL
#define EPF_HANDOFF_SIZE 64
#define EPF_HANDOFF_ADDRESS 0x08000000ULL

// XJ380 x86-64 scalar TLS relocations. These are EPF architecture assignments,
// not the ELF relocation numbers; no executable instruction bytes are patched.
#define EPF_RELOC_X86_64_DTPMOD64 0x003e0001u
#define EPF_RELOC_X86_64_DTPOFF64 0x003e0002u
#define EPF_RELOC_X86_64_TPOFF64 0x003e0003u
#define EPF_MAX_TLS_MODULE (1ULL << 20)
#define EPF_MAX_TLS_TOTAL (16ULL << 20)
#define EPF_MAX_TLS_ALIGN (2ULL << 20)
