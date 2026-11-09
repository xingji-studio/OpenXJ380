// SPDX-License-Identifier: Apache-2.0
// Native x86-64 ELF interpreter. All pointers below belong to this process;
// no kernel symbol table, kernel callback or cross-process handle is involved.
#include "platform.h"
#include <elf.h>

#define PAGE 4096UL
#define PAGE_DOWN(value) ((value) & ~(PAGE - 1))
#define PAGE_UP(value) (((value) + PAGE - 1) & ~(PAGE - 1))
#define MAX_OBJECTS 32
#define MAX_PHDRS 64
#define MAX_NEEDED 32
#define MAX_SYMBOLS 65536
#define MAX_PATH 512
#define MAX_FILE (32UL * 1024 * 1024)
#define MAX_SPAN (64UL * 1024 * 1024)
#define USER_LIMIT (1UL << 47)

#ifdef XJ380_RTLD_TRACE
#define TRACE(message) rtld_syscall(7427, (long)("rtld: " message "\n"), 0, 0, 0, 0, 0)
#else
#define TRACE(message) ((void)0)
#endif

typedef struct
{
    uint64_t base, start, end, entry;
    Elf64_Phdr phdr[MAX_PHDRS];
    size_t phnum;
    char path[MAX_PATH];
    const char *strings, *soname, *runpath;
    size_t string_size, symbol_count;
    const Elf64_Sym *symbols;
    const Elf64_Rela *rela, *jmprel;
    size_t rela_size, jmprel_size;
    uint64_t init, fini, preinit_array, init_array, fini_array;
    size_t preinit_size, init_size, fini_size;
    size_t needed[MAX_NEEDED], needed_count;
    unsigned dependencies[MAX_NEEDED], dependency_count;
    unsigned initialized, symbolic;
} Object;

static Object objects[MAX_OBJECTS];
static size_t object_count;
static unsigned init_order[MAX_OBJECTS];
static size_t init_count;

extern void rtld_enter(uint64_t entry, uint64_t *stack, void (*finalizer)(void)) __attribute__((noreturn));

static void fail(const char *reason, const char *detail) __attribute__((noreturn));

static void fail(const char *reason, const char *detail)
{
    rtld_write(2, "ld-xj380: ");
    rtld_write(2, reason);
    if (detail)
    {
        rtld_write(2, ": ");
        rtld_write(2, detail);
    }
    rtld_write(2, "\n");
    rtld_exit(127);
}

static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static void copy_bytes(void *destination, const void *source, size_t length)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < length; ++i) out[i] = in[i];
}

static int range(uint64_t offset, uint64_t length, uint64_t limit)
{
    return offset <= limit && length <= limit - offset;
}

static uint64_t biased(Object *object, uint64_t address)
{
    if (!range(object->base, address, USER_LIMIT)) fail("address overflow", object->path);
    return object->base + address;
}

static int loaded(Object *object, uint64_t address, size_t length, unsigned required)
{
    if (!range(address, length, USER_LIMIT)) return 0;
    for (size_t i = 0; i < object->phnum; ++i)
    {
        Elf64_Phdr *p = &object->phdr[i];
        if (p->p_type != PT_LOAD || (p->p_flags & required) != required) continue;
        uint64_t start = object->base + p->p_vaddr;
        if (address >= start && range(address - start, length, p->p_memsz)) return 1;
    }
    return 0;
}

static void *pointer(Object *object, uint64_t address, size_t length)
{
    address = biased(object, address);
    if (!loaded(object, address, length, PF_R)) fail("table outside LOAD segments", object->path);
    return (void *)address;
}

static const char *string(Object *object, size_t offset)
{
    if (offset >= object->string_size) fail("string offset out of bounds", object->path);
    for (size_t i = offset; i < object->string_size; ++i)
        if (!object->strings[i]) return object->strings + offset;
    fail("unterminated string", object->path);
}

static void path_copy(char *out, const char *in)
{
    size_t i = 0;
    do
    {
        if (i == MAX_PATH) fail("path too long", NULL);
        out[i] = in[i];
    } while (in[i++]);
}

static void layout(Object *object)
{
    object->start = USER_LIMIT;
    object->end = 0;
    for (size_t i = 0; i < object->phnum; ++i)
    {
        Elf64_Phdr *p = &object->phdr[i];
        if (p->p_type == PT_INTERP && object != &objects[0]) fail("nested interpreter", object->path);
        if (p->p_type == PT_TLS && p->p_memsz) fail("TLS is not supported by native ELF ABI v1", object->path);
        if (p->p_type != PT_LOAD) continue;
        if (!p->p_memsz) continue;
        if (p->p_filesz > p->p_memsz || !range(p->p_vaddr, p->p_memsz, USER_LIMIT - PAGE) ||
            (p->p_flags & (PF_W | PF_X)) == (PF_W | PF_X) || !(p->p_flags & PF_R) ||
            (p->p_align > 1 && ((p->p_align & (p->p_align - 1)) ||
             ((p->p_vaddr - p->p_offset) & (p->p_align - 1)))))
            fail("invalid LOAD segment", object->path);
        uint64_t start = PAGE_DOWN(p->p_vaddr), end = PAGE_UP(p->p_vaddr + p->p_memsz);
        // The native profile uses separate pages for each LOAD. This also
        // prevents one segment's BSS or protection changes overwriting another.
        for (size_t j = 0; j < i; ++j)
        {
            Elf64_Phdr *q = &object->phdr[j];
            if (q->p_type == PT_LOAD && q->p_memsz && start < PAGE_UP(q->p_vaddr + q->p_memsz) &&
                PAGE_DOWN(q->p_vaddr) < end) fail("overlapping LOAD pages", object->path);
        }
        if (start < object->start) object->start = start;
        if (end > object->end) object->end = end;
    }
    if (object->end <= object->start || object->end - object->start > MAX_SPAN)
        fail("invalid image span", object->path);
    biased(object, object->end);
}

static void native_note(Object *object)
{
    for (size_t i = 0; i < object->phnum; ++i)
    {
        Elf64_Phdr *p = &object->phdr[i];
        if (p->p_type != PT_NOTE) continue;
        const unsigned char *notes = pointer(object, p->p_vaddr, p->p_filesz);
        size_t offset = 0;
        while (range(offset, 12, p->p_filesz))
        {
            uint32_t words[3];
            copy_bytes(words, notes + offset, sizeof(words));
            size_t name_size = ((uint64_t)words[0] + 3) & ~3UL;
            size_t desc_size = ((uint64_t)words[1] + 3) & ~3UL;
            offset += 12;
            if (!range(offset, name_size, p->p_filesz) || !range(offset + name_size, desc_size, p->p_filesz))
                fail("invalid ABI note", object->path);
            if (words[0] == 6 && words[1] == 4 && words[2] == 1 &&
                notes[offset] == 'X' && notes[offset + 1] == 'J' && notes[offset + 2] == '3' &&
                notes[offset + 3] == '8' && notes[offset + 4] == '0' && notes[offset + 5] == 0)
            {
                uint32_t version;
                copy_bytes(&version, notes + offset + name_size, sizeof(version));
                if (version != 1) fail("unsupported native ABI version", object->path);
                return;
            }
            offset += name_size + desc_size;
        }
    }
    fail("missing XJ380 native ABI note", object->path);
}

static size_t gnu_symbol_count(Object *object, uint64_t address)
{
    const uint32_t *hash = pointer(object, address, 16);
    uint32_t buckets = hash[0], first = hash[1], bloom = hash[2];
    if (!buckets || buckets > MAX_SYMBOLS || !bloom || bloom > MAX_SYMBOLS || first > MAX_SYMBOLS)
        fail("invalid GNU hash header", object->path);
    uint64_t bucket_address = address + 16 + (uint64_t)bloom * 8;
    const uint32_t *table = pointer(object, bucket_address, (size_t)buckets * 4);
    size_t count = first;
    for (size_t i = 0; i < buckets; ++i)
    {
        size_t symbol = table[i];
        if (!symbol) continue;
        if (symbol < first) fail("invalid GNU hash bucket", object->path);
        for (;; ++symbol)
        {
            if (symbol >= MAX_SYMBOLS) fail("GNU hash chain too long", object->path);
            const uint32_t *chain = pointer(object, bucket_address + (uint64_t)buckets * 4 +
                                           (uint64_t)(symbol - first) * 4, 4);
            if (symbol + 1 > count) count = symbol + 1;
            if (*chain & 1) break;
        }
    }
    return count;
}

static void dynamic(Object *object)
{
    const Elf64_Phdr *segment = NULL;
    for (size_t i = 0; i < object->phnum; ++i)
    {
        if (object->phdr[i].p_type != PT_DYNAMIC) continue;
        if (segment) fail("duplicate DYNAMIC segment", object->path);
        segment = &object->phdr[i];
    }
    if (!segment || segment->p_vaddr % 8 || segment->p_memsz % sizeof(Elf64_Dyn) ||
        segment->p_memsz > 4096 * sizeof(Elf64_Dyn)) fail("invalid DYNAMIC segment", object->path);
    const Elf64_Dyn *entries = pointer(object, segment->p_vaddr, segment->p_memsz);
    uint64_t strings = 0, symbols = 0, hash = 0, gnu_hash = 0;
    uint64_t rela = 0, jmprel = 0, rela_entry = 0, symbol_entry = 0, plt_type = 0;
    size_t soname = 0, runpath = 0, rpath = 0;
    size_t count = segment->p_memsz / sizeof(*entries), i;
    for (i = 0; i < count && entries[i].d_tag != DT_NULL; ++i)
    {
        uint64_t value = entries[i].d_un.d_val;
        switch (entries[i].d_tag)
        {
        case DT_STRTAB: strings = value; break;
        case DT_STRSZ: object->string_size = value; break;
        case DT_SYMTAB: symbols = value; break;
        case DT_SYMENT: symbol_entry = value; break;
        case DT_HASH: hash = value; break;
        case DT_GNU_HASH: gnu_hash = value; break;
        case DT_RELA: rela = value; break;
        case DT_RELASZ: object->rela_size = value; break;
        case DT_RELAENT: rela_entry = value; break;
        case DT_JMPREL: jmprel = value; break;
        case DT_PLTRELSZ: object->jmprel_size = value; break;
        case DT_PLTREL: plt_type = value; break;
        case DT_SONAME: soname = value; break;
        case DT_RUNPATH: runpath = value; break;
        case DT_RPATH: rpath = value; break;
        case DT_SYMBOLIC: object->symbolic = 1; break;
        case DT_INIT: object->init = value; break;
        case DT_FINI: object->fini = value; break;
        case DT_PREINIT_ARRAY: object->preinit_array = value; break;
        case DT_PREINIT_ARRAYSZ: object->preinit_size = value; break;
        case DT_INIT_ARRAY: object->init_array = value; break;
        case DT_INIT_ARRAYSZ: object->init_size = value; break;
        case DT_FINI_ARRAY: object->fini_array = value; break;
        case DT_FINI_ARRAYSZ: object->fini_size = value; break;
        case DT_NEEDED:
            if (object->needed_count == MAX_NEEDED) fail("too many dependencies", object->path);
            object->needed[object->needed_count++] = value;
            break;
        case DT_TEXTREL: fail("text relocations are unsupported", object->path);
        case DT_FLAGS:
            if (value & ~((uint64_t)DF_BIND_NOW | DF_ORIGIN | DF_SYMBOLIC))
                fail("unsupported dynamic flags", object->path);
            if (value & DF_SYMBOLIC) object->symbolic = 1;
            break;
        case DT_FLAGS_1:
            if (value & ~((uint64_t)DF_1_NOW | DF_1_PIE)) fail("unsupported dynamic flags", object->path);
            break;
        case DT_REL: case DT_RELSZ: case DT_RELR: case DT_RELRSZ:
        case DT_VERNEED: case DT_VERDEF: case DT_VERSYM:
            if (value) fail("unsupported relocation or symbol version table", object->path);
            break;
        }
    }
    if (i == count) fail("unterminated DYNAMIC segment", object->path);
    if (!strings || !object->string_size || !symbols || symbols % 8 || symbol_entry != sizeof(Elf64_Sym))
        fail("missing symbol tables", object->path);
    object->strings = pointer(object, strings, object->string_size);
    if (hash)
    {
        const uint32_t *table = pointer(object, hash, 8);
        if (table[0] > MAX_SYMBOLS || table[1] > MAX_SYMBOLS) fail("invalid SysV hash", object->path);
        pointer(object, hash, (2UL + table[0] + table[1]) * 4);
        object->symbol_count = table[1];
    }
    if (gnu_hash)
    {
        size_t gnu_count = gnu_symbol_count(object, gnu_hash);
        if (hash && gnu_count > object->symbol_count) fail("inconsistent symbol counts", object->path);
        if (!hash) object->symbol_count = gnu_count;
    }
    if (!hash && !gnu_hash) fail("missing symbol hash table", object->path);
    object->soname = soname ? string(object, soname) : NULL;
    object->runpath = (runpath || rpath) ? string(object, runpath ? runpath : rpath) : NULL;
    if (object->rela_size)
    {
        if (!rela || rela % 8 || rela_entry != sizeof(Elf64_Rela) || object->rela_size % sizeof(Elf64_Rela))
            fail("invalid RELA table", object->path);
        object->rela = pointer(object, rela, object->rela_size);
    }
    if (object->jmprel_size)
    {
        if (!jmprel || jmprel % 8 || plt_type != DT_RELA || object->jmprel_size % sizeof(Elf64_Rela))
            fail("invalid PLT relocation table", object->path);
        object->jmprel = pointer(object, jmprel, object->jmprel_size);
    }
    // GNU hash describes the exported subset. A main program with only
    // undefined imports can have empty buckets, so its relocation indexes
    // supply a lower bound for the otherwise unrecorded dynsym length.
    for (unsigned table_index = 0; table_index < 2; ++table_index)
    {
        const Elf64_Rela *table = table_index ? object->jmprel : object->rela;
        size_t size = table_index ? object->jmprel_size : object->rela_size;
        for (size_t j = 0; j < size / sizeof(*table); ++j)
        {
            size_t count_needed = (size_t)ELF64_R_SYM(table[j].r_info) + 1;
            if (count_needed > MAX_SYMBOLS || (hash && count_needed > object->symbol_count))
                fail("relocation symbol out of bounds", object->path);
            if (count_needed > object->symbol_count) object->symbol_count = count_needed;
        }
    }
    if (!object->symbol_count) object->symbol_count = 1;
    object->symbols = pointer(object, symbols, object->symbol_count * sizeof(Elf64_Sym));
}

static long open_library(Object *requester, const char *name, char path[MAX_PATH])
{
    for (const char *p = name; *p; ++p)
    {
        if (*p != '/') continue;
        path_copy(path, name);
        return rtld_syscall(2, (long)path, 0, 0, 0, 0, 0);
    }
    const char *search = requester->runpath;
    if (search)
    {
        while (*search)
        {
            size_t length = 0;
            while (*search && *search != ':')
            {
                if (*search == '$')
                {
                    const char *token = "$ORIGIN";
                    size_t matched = 0;
                    while (token[matched] && search[matched] == token[matched]) ++matched;
                    if (token[matched]) fail("unsupported RUNPATH token", requester->path);
                    const char *last = NULL;
                    for (const char *p = requester->path; *p; ++p) if (*p == '/') last = p;
                    if (!last || requester->path[0] != '/') fail("ORIGIN requires an absolute executable path", NULL);
                    for (const char *p = requester->path; p < last; ++p)
                    {
                        if (length + 1 >= MAX_PATH) fail("RUNPATH too long", NULL);
                        path[length++] = *p;
                    }
                    search += matched;
                }
                else
                {
                    if (length + 1 >= MAX_PATH) fail("RUNPATH too long", NULL);
                    path[length++] = *search++;
                }
            }
            if (*search == ':') ++search;
            if (!length) continue;
            if (length + 1 + rtld_length(name) >= MAX_PATH) fail("library path too long", NULL);
            path[length++] = '/';
            path_copy(path + length, name);
            long fd = rtld_syscall(2, (long)path, 0, 0, 0, 0, 0);
            if (fd >= 0) return fd;
            if (fd != -2 && fd != -20) return fd;
        }
    }
    const char prefix[] = "/system/lib/";
    if (sizeof(prefix) + rtld_length(name) > MAX_PATH) fail("library name too long", NULL);
    copy_bytes(path, prefix, sizeof(prefix) - 1);
    path_copy(path + sizeof(prefix) - 1, name);
    return rtld_syscall(2, (long)path, 0, 0, 0, 0, 0);
}

static void *allocate(size_t size)
{
    long address = rtld_syscall(9, 0, (long)size, 3, 0x22, -1, 0);
    if (rtld_failed(address)) fail("mmap failed", NULL);
    return (void *)address;
}

static unsigned load_library(Object *requester, const char *name)
{
    if (!*name) fail("empty dependency name", requester->path);
    for (size_t i = 0; i < object_count; ++i)
        if (equal(objects[i].path, name) || (objects[i].soname && equal(objects[i].soname, name))) return (unsigned)i;
    char path[MAX_PATH];
    long fd = open_library(requester, name, path);
    if (fd < 0) fail("cannot open library", name);
    TRACE("dependency opened");
    for (size_t i = 0; i < object_count; ++i)
    {
        if (!equal(objects[i].path, path)) continue;
        rtld_syscall(3, fd, 0, 0, 0, 0, 0);
        return (unsigned)i;
    }
    if (object_count == MAX_OBJECTS) fail("too many shared objects", name);
    long file_size = rtld_syscall(8, fd, 0, 2, 0, 0, 0);
    if (file_size < (long)sizeof(Elf64_Ehdr) || file_size > (long)MAX_FILE ||
        rtld_syscall(8, fd, 0, 0, 0, 0, 0) != 0) fail("invalid library size", path);
    unsigned char *file = allocate((size_t)file_size);
    size_t done = 0;
    while (done < (size_t)file_size)
    {
        long got = rtld_syscall(0, fd, (long)(file + done), file_size - (long)done, 0, 0, 0);
        if (got == -4) continue;
        if (got <= 0) fail("cannot read library", path);
        done += (size_t)got;
    }
    rtld_syscall(3, fd, 0, 0, 0, 0, 0);
    TRACE("dependency read");
    const Elf64_Ehdr *header = (const Elf64_Ehdr *)file;
    if (header->e_ident[0] != 127 || header->e_ident[1] != 'E' || header->e_ident[2] != 'L' ||
        header->e_ident[3] != 'F' || header->e_ident[EI_CLASS] != ELFCLASS64 ||
        header->e_ident[EI_DATA] != ELFDATA2LSB || header->e_ident[EI_VERSION] != EV_CURRENT ||
        header->e_machine != EM_X86_64 || header->e_type != ET_DYN || header->e_version != EV_CURRENT ||
        header->e_ehsize != sizeof(*header) || header->e_phentsize != sizeof(Elf64_Phdr) ||
        !header->e_phnum || header->e_phnum > MAX_PHDRS ||
        !range(header->e_phoff, (uint64_t)header->e_phnum * sizeof(Elf64_Phdr), (size_t)file_size))
        fail("invalid shared ELF header", path);
    unsigned index = (unsigned)object_count++;
    Object *object = &objects[index];
    path_copy(object->path, path);
    object->phnum = header->e_phnum;
    copy_bytes(object->phdr, file + header->e_phoff, object->phnum * sizeof(Elf64_Phdr));
    layout(object);
    uint64_t mapping = (uint64_t)allocate(object->end - object->start);
    if (mapping < object->start) fail("invalid mmap bias", path);
    object->base = mapping - object->start;
    for (size_t i = 0; i < object->phnum; ++i)
    {
        const Elf64_Phdr *p = &object->phdr[i];
        if (p->p_type != PT_LOAD || !p->p_memsz) continue;
        if (!range(p->p_offset, p->p_filesz, (size_t)file_size) ||
            (p->p_align > 1 && object->base % p->p_align)) fail("invalid segment file range or alignment", path);
        copy_bytes((void *)biased(object, p->p_vaddr), file + p->p_offset, p->p_filesz);
    }
    if (rtld_syscall(11, (long)file, file_size, 0, 0, 0, 0)) fail("cannot release file buffer", path);
    TRACE("dependency mapped");
    native_note(object);
    dynamic(object);
    return index;
}

static uint64_t symbol_address(Object *object, const Elf64_Sym *symbol)
{
    unsigned type = ELF64_ST_TYPE(symbol->st_info);
    if (type != STT_NOTYPE && type != STT_OBJECT && type != STT_FUNC)
        fail("unsupported symbol type", string(object, symbol->st_name));
    if (symbol->st_shndx == SHN_ABS) return symbol->st_value;
    if (symbol->st_shndx >= SHN_LORESERVE) fail("unsupported symbol section", object->path);
    uint64_t value = biased(object, symbol->st_value);
    if (!loaded(object, value, symbol->st_size ? symbol->st_size : 1, PF_R))
        fail("symbol outside LOAD segments", object->path);
    return value;
}

static uint64_t resolve(Object *requester, size_t index)
{
    if (index >= requester->symbol_count) fail("relocation symbol out of bounds", requester->path);
    const Elf64_Sym *symbol = &requester->symbols[index];
    unsigned bind = ELF64_ST_BIND(symbol->st_info), visibility = ELF64_ST_VISIBILITY(symbol->st_other);
    if (symbol->st_shndx != SHN_UNDEF && (bind == STB_LOCAL || visibility != STV_DEFAULT || requester->symbolic))
        return symbol_address(requester, symbol);
    const char *name = string(requester, symbol->st_name);
    // objects[] is populated breadth first, preserving each DT_NEEDED order.
    for (size_t i = 0; i < object_count; ++i)
    {
        Object *object = &objects[i];
        for (size_t j = 1; j < object->symbol_count; ++j)
        {
            const Elf64_Sym *candidate = &object->symbols[j];
            unsigned candidate_bind = ELF64_ST_BIND(candidate->st_info);
            unsigned candidate_visibility = ELF64_ST_VISIBILITY(candidate->st_other);
            if (candidate->st_shndx == SHN_UNDEF || (candidate_bind != STB_GLOBAL && candidate_bind != STB_WEAK) ||
                candidate_visibility == STV_HIDDEN || candidate_visibility == STV_INTERNAL) continue;
            if (equal(name, string(object, candidate->st_name))) return symbol_address(object, candidate);
        }
    }
    if (bind == STB_WEAK) return 0;
    fail("undefined symbol", name);
}

static void relocate(Object *object, const Elf64_Rela *table, size_t bytes, int bootstrap)
{
    for (size_t i = 0; i < bytes / sizeof(*table); ++i)
    {
        Elf64_Rela relocation = table[i];
        unsigned type = ELF64_R_TYPE(relocation.r_info);
        size_t symbol = ELF64_R_SYM(relocation.r_info);
        if (type == R_X86_64_NONE) continue;
        uint64_t target = biased(object, relocation.r_offset);
        if ((target & 7) || !loaded(object, target, 8, PF_W)) fail("invalid relocation destination", object->path);
        uint64_t value;
        if (type == R_X86_64_RELATIVE)
        {
            if (symbol) fail("RELATIVE relocation has a symbol", object->path);
            // ELF addends are signed. Modular addition is intentional here.
            value = object->base + (uint64_t)relocation.r_addend;
        }
        else
        {
            if (bootstrap || (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT))
                fail("unsupported relocation type", object->path);
            value = resolve(object, symbol);
            if (type == R_X86_64_64) value += (uint64_t)relocation.r_addend;
        }
        *(uint64_t *)target = value;
    }
}

static void protect(Object *object)
{
    // Group adjacent pages with equal permissions, including unmapped holes.
    for (uint64_t cursor = object->start; cursor < object->end;)
    {
        unsigned flags = 0;
        uint64_t end = object->end;
        for (size_t i = 0; i < object->phnum; ++i)
        {
            const Elf64_Phdr *p = &object->phdr[i];
            if (p->p_type != PT_LOAD || !p->p_memsz) continue;
            uint64_t start = PAGE_DOWN(p->p_vaddr), limit = PAGE_UP(p->p_vaddr + p->p_memsz);
            if (cursor >= start && cursor < limit)
            {
                flags = p->p_flags;
                if (limit < end) end = limit;
            }
            else if (start > cursor && start < end) end = start;
        }
        long prot = (flags & PF_R ? 1 : 0) | (flags & PF_W ? 2 : 0) | (flags & PF_X ? 4 : 0);
        if (rtld_syscall(10, (long)biased(object, cursor), (long)(end - cursor), prot, 0, 0, 0))
            fail("cannot protect LOAD pages", object->path);
        cursor = end;
    }
    for (size_t i = 0; i < object->phnum; ++i)
    {
        const Elf64_Phdr *p = &object->phdr[i];
        if (p->p_type != PT_GNU_RELRO || !p->p_memsz) continue;
        uint64_t start = PAGE_DOWN(biased(object, p->p_vaddr));
        uint64_t end = PAGE_UP(biased(object, p->p_vaddr + p->p_memsz));
        if (!loaded(object, biased(object, p->p_vaddr), p->p_memsz, PF_R | PF_W) ||
            rtld_syscall(10, (long)start, (long)(end - start), 1, 0, 0, 0))
            fail("cannot protect RELRO pages", object->path);
    }
}

static void call_function(Object *object, uint64_t address, int argc, char **argv, char **envp)
{
    if (!address) return;
    if (!loaded(object, address, 1, PF_X)) fail("initializer outside executable segment", object->path);
    ((void (*)(int, char **, char **))address)(argc, argv, envp);
}

static void call_array(Object *object, uint64_t address, size_t size, int reverse,
                       int argc, char **argv, char **envp)
{
    if (!size) return;
    if (!address || size % sizeof(uint64_t)) fail("invalid initializer array", object->path);
    const uint64_t *array = pointer(object, address, size);
    size_t count = size / sizeof(uint64_t);
    for (size_t i = 0; i < count; ++i)
    {
        uint64_t function = array[reverse ? count - i - 1 : i];
        if (function) call_function(object, function, argc, argv, envp);
    }
}

static void initialize(unsigned index, int argc, char **argv, char **envp)
{
    Object *object = &objects[index];
    if (object->initialized) return;
    object->initialized = 1; // Break dependency cycles before descending.
    for (size_t i = 0; i < object->dependency_count; ++i) initialize(object->dependencies[i], argc, argv, envp);
    if (object->init) call_function(object, biased(object, object->init), argc, argv, envp);
    call_array(object, object->init_array, object->init_size, 0, argc, argv, envp);
    init_order[init_count++] = index;
}

static void finalize(void)
{
    while (init_count)
    {
        Object *object = &objects[init_order[--init_count]];
        call_array(object, object->fini_array, object->fini_size, 1, 0, NULL, NULL);
        if (object->fini) call_function(object, biased(object, object->fini), 0, NULL, NULL);
    }
}

// Exercise interpreter self relocation even when every code reference can be
// bound PC-relatively by -Bsymbolic. This pointer is used only after bootstrap.
static void (*volatile finalizer_entry)(void) = finalize;

static void initialize_runtime(int argc, char **argv, char **envp)
{
    for (size_t i = 1; i < object_count; ++i)
    {
        Object *object = &objects[i];
        if (!object->soname || !equal(object->soname, "libxapi.so.1")) continue;
        for (size_t j = 1; j < object->symbol_count; ++j)
        {
            const Elf64_Sym *symbol = &object->symbols[j];
            if (symbol->st_shndx == SHN_UNDEF || ELF64_ST_BIND(symbol->st_info) != STB_GLOBAL ||
                ELF64_ST_VISIBILITY(symbol->st_other) != STV_DEFAULT ||
                !equal(string(object, symbol->st_name), "__xapi_rtld_init_v1")) continue;
            uint64_t address = symbol_address(object, symbol);
            if (ELF64_ST_TYPE(symbol->st_info) != STT_FUNC || !loaded(object, address, 1, PF_X))
                fail("invalid shared XAPI bootstrap", object->path);
            ((void (*)(int, char **, char **, void (*)(void)))address)(argc, argv, envp, finalize);
            return;
        }
        fail("missing shared XAPI bootstrap v1", object->path);
    }
}

void rtld_start(uint64_t *stack)
{
    TRACE("bootstrap");
    size_t argc = stack[0];
    if (argc > 4096) fail("invalid initial stack", NULL);
    char **argv = (char **)(stack + 1), **envp = argv + argc + 1;
    char **environment_end = envp;
    while (*environment_end) ++environment_end;
    const Elf64_auxv_t *aux = (const Elf64_auxv_t *)(environment_end + 1);
    uint64_t phdr = 0, phnum = 0, base = 0, entry = 0, phent = 0;
    const char *execfn = NULL;
    for (size_t i = 0; aux[i].a_type != AT_NULL; ++i)
    {
        if (i == 64) fail("invalid auxiliary vector", NULL);
        uint64_t value = aux[i].a_un.a_val;
        switch (aux[i].a_type)
        {
        case AT_BASE: base = value; break;
        case AT_PHDR: phdr = value; break;
        case AT_PHENT: phent = value; break;
        case AT_PHNUM: phnum = value; break;
        case AT_ENTRY: entry = value; break;
        case AT_EXECFN: execfn = (const char *)value; break;
        case AT_PAGESZ: if (value != PAGE) fail("unsupported page size", NULL); break;
        }
    }
    if (!base || !phdr || !phnum || phnum > MAX_PHDRS || phent != sizeof(Elf64_Phdr) || !entry || !execfn)
        fail("missing execution metadata", NULL);

    // This interpreter is linked -Bsymbolic with hidden code/data and no
    // external imports. Until self relocation completes all references are
    // PC-relative; only the local descriptor and its stack storage are used.
    Object self;
    for (size_t i = 0; i < sizeof(self); ++i) ((volatile unsigned char *)&self)[i] = 0;
    self.base = base;
    path_copy(self.path, "ld-xj380");
    const Elf64_Ehdr *header = (const Elf64_Ehdr *)base;
    if (header->e_phnum > MAX_PHDRS || header->e_phentsize != sizeof(Elf64_Phdr)) fail("invalid interpreter", NULL);
    self.phnum = header->e_phnum;
    copy_bytes(self.phdr, (void *)(base + header->e_phoff), self.phnum * sizeof(Elf64_Phdr));
    layout(&self);
    dynamic(&self);
    if (self.needed_count || self.jmprel_size) fail("interpreter has external dependencies", NULL);
    relocate(&self, self.rela, self.rela_size, 1);
    native_note(&self);
    TRACE("self relocated");

    Object *main = &objects[0];
    path_copy(main->path, execfn);
    main->phnum = phnum;
    copy_bytes(main->phdr, (void *)phdr, phnum * sizeof(Elf64_Phdr));
    int found_bias = 0;
    for (size_t i = 0; i < main->phnum; ++i)
    {
        if (main->phdr[i].p_type != PT_PHDR) continue;
        if (main->phdr[i].p_vaddr > phdr || found_bias) fail("invalid PHDR segment", main->path);
        main->base = phdr - main->phdr[i].p_vaddr;
        found_bias = 1;
    }
    if (!found_bias) fail("native main requires PT_PHDR", main->path);
    layout(main);
    if (!loaded(main, phdr, phnum * sizeof(Elf64_Phdr), PF_R) || !loaded(main, entry, 1, PF_X))
        fail("invalid main entry or program headers", main->path);
    main->entry = entry;
    native_note(main);
    dynamic(main);
    TRACE("main validated");
    object_count = 1;
    for (size_t i = 0; i < object_count; ++i)
    {
        Object *object = &objects[i];
        for (size_t j = 0; j < object->needed_count; ++j)
            object->dependencies[object->dependency_count++] = load_library(object, string(object, object->needed[j]));
    }
    for (size_t i = 0; i < object_count; ++i)
    {
        relocate(&objects[i], objects[i].rela, objects[i].rela_size, 0);
        relocate(&objects[i], objects[i].jmprel, objects[i].jmprel_size, 0);
    }
    TRACE("relocated");
    for (size_t i = 0; i < object_count; ++i) protect(&objects[i]);
    protect(&self);
    TRACE("protected");
    initialize_runtime((int)argc, argv, envp);
    call_array(main, main->preinit_array, main->preinit_size, 0, (int)argc, argv, envp);
    initialize(0, (int)argc, argv, envp);
    TRACE("initialized");
    if (finalizer_entry != finalize) fail("interpreter self relocation failed", NULL);
    rtld_enter(entry, stack, finalizer_entry);
}
