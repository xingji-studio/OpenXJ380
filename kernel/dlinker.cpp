#include <proto.hpp>
#include <dlinker.h>
#include <fs/vfs/vfs.h>
#include <elf.h>
#include <id_alloc.h>

uint64_t kernel_modules_load_offset = 0;

#define KERNEL_MOD_SPACE_START 0xffffffffb0000000ULL
#define KERNEL_MOD_SPACE_END   0xffffffffc0000000ULL
#define USER_SO_BASE_START     0x40000000ULL
#define USER_SO_BASE_END       0x80000000ULL

extern dlfunc_t __ksymtab_start[]; // .ksymtab section
extern dlfunc_t __ksymtab_end[];
size_t          dlfunc_count = 0;

char fmt_buf[4096];

static uint8_t *dlinker_alloc_file_buffer(size_t size) {
    size_t pages = PADDING_UP(size, PAGE_SIZE) / PAGE_SIZE;
    if (pages == 0) {
        return NULL;
    }

    uint64_t phys = alloc_frames(pages);
    if (phys == 0) {
        return NULL;
    }
    return (uint8_t *)phys_to_virt(phys);
}

static void dlinker_free_file_buffer(void *buffer, size_t size) {
    if (buffer == NULL || size == 0) {
        return;
    }
    free_frames((uint64_t)virt_to_phys((uint64_t)buffer), PADDING_UP(size, PAGE_SIZE) / PAGE_SIZE);
}

static uint8_t *dlinker_read_entire_file(vfs_node_t file) {
    if (file == NULL || file->size == 0) {
        return NULL;
    }

    uint8_t *buffer = dlinker_alloc_file_buffer(file->size);
    if (buffer == NULL) {
        return NULL;
    }

    size_t got = vfs_read(file, buffer, 0, file->size);
    if (got != file->size) {
        dlinker_free_file_buffer(buffer, file->size);
        return NULL;
    }

    return buffer;
}

void *resolve_symbol(Elf64_Sym *symtab, uint32_t sym_idx) {
    if (symtab == NULL) return NULL;
    return (void *)symtab[sym_idx].st_value;
}

bool handle_relocations(Elf64_Rela *rela_start, Elf64_Sym *symtab, char *strtab, size_t jmprel_sz,
                        uint64_t offset) {
    if (jmprel_sz == 0) return true;
    if (rela_start == NULL || symtab == NULL || strtab == NULL) return false;

    Elf64_Rela *rela_plt   = rela_start;
    size_t      rela_count = jmprel_sz / sizeof(Elf64_Rela);

    for (size_t i = 0; i < rela_count; i++) {
        Elf64_Rela *rela        = &rela_plt[i];
        Elf64_Sym  *sym         = &symtab[ELF64_R_SYM(rela->r_info)];
        char       *sym_name    = &strtab[sym->st_name];
        dlfunc_t   *func        = find_func(sym_name);
        uint64_t   *target_addr = (uint64_t *)(rela->r_offset + offset);
        if (func != NULL) {
            *target_addr = (uint64_t)func->addr;
        } else {
            write_serial_fmt("Failed relocating %s at %p\n", sym_name, rela->r_offset + offset);
            return false;
        }
    }
    return true;
}

void *find_symbol_address(const char *symbol_name, Elf64_Ehdr *ehdr, uint64_t offset) {
    if (symbol_name == NULL || ehdr == NULL) return NULL;

    Elf64_Sym *symtab = NULL;
    char      *strtab = NULL;

    Elf64_Shdr *shdrs    = (Elf64_Shdr *)((char *)ehdr + ehdr->e_shoff);
    char       *shstrtab = (char *)ehdr + shdrs[ehdr->e_shstrndx].sh_offset;

    size_t symtabsz = 0;

    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB || shdrs[i].sh_type == SHT_DYNSYM) {
            symtab   = (Elf64_Sym *)((char *)ehdr + shdrs[i].sh_offset);
            symtabsz = shdrs[i].sh_size;
            strtab   = (char *)ehdr + shdrs[shdrs[i].sh_link].sh_offset;
            break;
        }
    }

    size_t num_symbols = symtabsz / sizeof(Elf64_Sym);
    if (symtab == NULL || strtab == NULL || num_symbols == 0) return NULL;

    for (size_t i = 0; i < num_symbols; i++) {
        Elf64_Sym *sym      = &symtab[i];
        char      *sym_name = &strtab[sym->st_name];
        if (strcmp(symbol_name, sym_name) == 0) {
            if (sym->st_shndx == SHN_UNDEF) {
                write_serial_fmt("Symbol %s is undefined.\n", sym_name);
                return NULL;
            }
            void *addr = (void*)((uint64_t)sym->st_value + offset);
            return addr;
        }
    }
    if (strcmp(symbol_name, "_init") != 0) {
        write_serial_fmt("Cannot find symbol %s in ELF file.\n", symbol_name);
    }
    return NULL;
}

dlinit_t load_dynamic(kernel_mode_t *kmod,Elf64_Phdr *phdrs, Elf64_Ehdr *ehdr, uint64_t offset) {
    Elf64_Dyn *dyn_entry = NULL;
    for (size_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn_entry = (Elf64_Dyn *)(phdrs[i].p_vaddr);
            break;
        }
    }
    if (dyn_entry == NULL) {
        write_serial_fmt("Dynamic section not found.\n");
        return NULL;
    }
    uint64_t addr_dyn = ((uint64_t)dyn_entry) + offset;
    dyn_entry         = (Elf64_Dyn *)addr_dyn;

    Elf64_Sym  *symtab = NULL;
    char       *strtab = NULL;
    Elf64_Rela *rel    = NULL;
    Elf64_Rela *jmprel = NULL;
    size_t      relsz = 0, symtabsz = 0, jmprel_sz = 0;

    while (dyn_entry->d_tag != DT_NULL) {
        switch (dyn_entry->d_tag) {
        case DT_SYMTAB:
            {uint64_t symtab_addr = dyn_entry->d_un.d_ptr + offset;
            symtab               = (Elf64_Sym *)symtab_addr;
            break;}
        case DT_STRTAB: strtab = (char *)dyn_entry->d_un.d_ptr + offset; break;
        case DT_RELA:
            {uint64_t rel_addr = dyn_entry->d_un.d_ptr + offset;
            rel               = (Elf64_Rela *)rel_addr;
            break;}
        case DT_RELASZ: relsz = dyn_entry->d_un.d_val; break;
        case DT_JMPREL:
            {uint64_t jmprel_addr = dyn_entry->d_un.d_ptr + offset;
            jmprel               = (Elf64_Rela *)jmprel_addr;
            break;}
        case DT_SYMENT: symtabsz = dyn_entry->d_un.d_val; break;
        case DT_PLTRELSZ: jmprel_sz = dyn_entry->d_un.d_val; break;
        case DT_PLTGOT: /* 需要解析 PLT 表 */ break;
        }
        dyn_entry++;
    }

    if (relsz != 0 && rel == NULL) return NULL;

    for (size_t i = 0; i < relsz / sizeof(Elf64_Rela); i++) {
        Elf64_Rela *r          = &rel[i];
        uint64_t   *reloc_addr = (uint64_t *)(r->r_offset + offset);
        uint32_t    sym_idx    = ELF64_R_SYM(r->r_info);
        uint32_t    type       = ELF64_R_TYPE(r->r_info);

        if (type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT) {
            Elf64_Sym *symbol = &symtab[sym_idx];
            void *symbol_addr = NULL;
            if (symbol->st_shndx == SHN_UNDEF) {
                dlfunc_t *external = find_func(&strtab[symbol->st_name]);
                if (external != NULL) symbol_addr = external->addr;
            } else {
                symbol_addr = (void *)(symbol->st_value + offset);
            }
            if (symbol_addr == NULL) return NULL;
            *reloc_addr = (uint64_t)symbol_addr;
        } else if (type == R_X86_64_RELATIVE) {
            *reloc_addr = (uint64_t)(offset + r->r_addend);
        } else if (type == R_X86_64_64) {
            Elf64_Sym *symbol = &symtab[sym_idx];
            void *symbol_addr = NULL;
            if (symbol->st_shndx == SHN_UNDEF) {
                dlfunc_t *external = find_func(&strtab[symbol->st_name]);
                if (external != NULL) symbol_addr = external->addr;
            } else {
                symbol_addr = (void *)(symbol->st_value + offset);
            }
            if (symbol_addr == NULL) return NULL;
            *reloc_addr = (uint64_t)symbol_addr + r->r_addend;
        }
    }
    if (!handle_relocations(jmprel, symtab, strtab, jmprel_sz, offset)) {
        write_serial_fmt("Failed to handle relocations.\n");
        return NULL;
    }

    void *entry = find_symbol_address("dlmain", ehdr, offset);
    if (entry == NULL) { entry = find_symbol_address("_dlmain", ehdr, offset); }
    kmod->entry = (dlinit_t)entry;

    void *tentry = find_symbol_address("dlstart", ehdr, offset);
    if (tentry == NULL) { tentry = find_symbol_address("_dlstart", ehdr, offset); }
    kmod->task_entry = (dlinit_t)tentry;

    dlinit_t dlinit_func = (dlinit_t)entry;
    return dlinit_func;
}

bool elf_test_head(Elf64_Ehdr *ehdr) {
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3 ||
        ehdr->e_version != EV_CURRENT || ehdr->e_ehsize != sizeof(Elf64_Ehdr) ||
        ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        return false;
    }

    switch (ehdr->e_machine) {
    case EM_X86_64:
    case EM_386: break;
    default: return false;
    }

    return true;
}

static bool elf_file_range(uint64_t offset, uint64_t length, size_t file_size)
{
    return offset <= file_size && length <= file_size - offset;
}

static bool validate_shared_object(Elf64_Ehdr *ehdr, size_t file_size, uint64_t base_addr, bool is_kernel,
                                   uint64_t *image_size)
{
    if (ehdr == NULL || file_size < sizeof(*ehdr) || !elf_test_head(ehdr) || ehdr->e_type != ET_DYN ||
        ehdr->e_phnum == 0 || ehdr->e_phnum > file_size / sizeof(Elf64_Phdr) ||
        !elf_file_range(ehdr->e_phoff, (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr), file_size))
        return false;

    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)ehdr + ehdr->e_phoff);
    uint64_t min_addr = ~0ULL, max_addr = 0;
    bool have_load = false, have_dynamic_terminator = false;
    for (size_t i = 0; i < ehdr->e_phnum; ++i)
    {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type == PT_LOAD)
        {
            if (ph->p_filesz > ph->p_memsz || !elf_file_range(ph->p_offset, ph->p_filesz, file_size) ||
                ph->p_vaddr > ~0ULL - base_addr || ph->p_memsz > ~0ULL - (base_addr + ph->p_vaddr))
                return false;
            uint64_t start = base_addr + ph->p_vaddr;
            uint64_t end = start + ph->p_memsz;
            min_addr = min(min_addr, (uint64_t)PADDING_DOWN(start, PAGE_SIZE));
            max_addr = max(max_addr, (uint64_t)PADDING_UP(end, PAGE_SIZE));
            have_load = true;
        }
        else if (ph->p_type == PT_DYNAMIC)
        {
            if (ph->p_filesz < sizeof(Elf64_Dyn) || ph->p_filesz % sizeof(Elf64_Dyn) != 0 ||
                !elf_file_range(ph->p_offset, ph->p_filesz, file_size)) return false;
            Elf64_Dyn *dyn = (Elf64_Dyn *)((uint8_t *)ehdr + ph->p_offset);
            size_t count = ph->p_filesz / sizeof(Elf64_Dyn);
            for (size_t j = 0; j < count; ++j)
                if (dyn[j].d_tag == DT_NULL) { have_dynamic_terminator = true; break; }
        }
    }
    if (!have_load || !have_dynamic_terminator || max_addr <= min_addr) return false;
    for (size_t i = 0; i < ehdr->e_phnum; ++i)
    {
        if (phdrs[i].p_type != PT_DYNAMIC || phdrs[i].p_vaddr > ~0ULL - base_addr) continue;
        uint64_t dynamic_start = base_addr + phdrs[i].p_vaddr;
        bool contained = false;
        for (size_t j = 0; j < ehdr->e_phnum; ++j)
        {
            if (phdrs[j].p_type != PT_LOAD || phdrs[j].p_vaddr > ~0ULL - base_addr) continue;
            uint64_t load_start = base_addr + phdrs[j].p_vaddr;
            if (phdrs[j].p_memsz > ~0ULL - load_start) continue;
            uint64_t load_end = load_start + phdrs[j].p_memsz;
            if (dynamic_start >= load_start && dynamic_start <= load_end &&
                phdrs[i].p_memsz <= load_end - dynamic_start)
            {
                contained = true;
                break;
            }
        }
        if (!contained) return false;
    }
    if (is_kernel)
    {
        if (min_addr < KERNEL_MOD_SPACE_START || max_addr > KERNEL_MOD_SPACE_END) return false;
    }
    else if (min_addr < USER_SO_BASE_START || max_addr > USER_SO_BASE_END) return false;
    if (min_addr < base_addr) return false;
    *image_size = max_addr - base_addr;
    return true;
}

static bool handle_range_loaded(dlhandle_t *handle, uint64_t address, size_t length)
{
    if (handle == NULL || handle->ehdr == NULL || length == 0 || address > ~0ULL - length) return false;
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)handle->ehdr;
    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)ehdr + ehdr->e_phoff);
    for (size_t i = 0; i < ehdr->e_phnum; ++i)
    {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_vaddr > ~0ULL - handle->base_addr) continue;
        uint64_t start = handle->base_addr + phdrs[i].p_vaddr;
        if (phdrs[i].p_memsz > ~0ULL - start) continue;
        uint64_t end = start + phdrs[i].p_memsz;
        if (address >= start && address + length <= end) return true;
    }
    return false;
}

void load_segment(Elf64_Phdr *phdr, void *elf, page_directory_t *directory, bool is_user,
                  uint64_t offset, uint64_t *load_start) {
    size_t hi = PADDING_UP(phdr->p_vaddr + phdr->p_memsz, 0x1000) + offset;
    size_t lo = PADDING_DOWN(phdr->p_vaddr, 0x1000) + offset;
    if (load_start != NULL) {
        if (lo < *load_start) { *load_start = lo; }
    }
    uint64_t flags = PTE_PRESENT | PTE_WRITEABLE;
    if (is_user) flags |= PTE_USER;
    if ((phdr->p_flags & PF_R) && !(phdr->p_flags & PF_W)) {
        for (size_t i = lo; i < hi; i += 0x1000) {
            page_map_to(directory, i, alloc_frames(1), flags);
        }
    } else
        for (size_t i = lo; i < hi; i += 0x1000) {
            page_map_to(directory, i, alloc_frames(1), flags);
        }
    uint64_t          p_vaddr  = (uint64_t)phdr->p_vaddr + offset;
    uint64_t          p_filesz = (uint64_t)phdr->p_filesz;
    uint64_t          p_memsz  = (uint64_t)phdr->p_memsz;
    if (p_filesz > p_memsz) {
        p_filesz = p_memsz;
    }
    page_directory_t *dir      = get_current_directory();
    switch_process_page_directory(directory);
    if (p_filesz > 0) {
        memcpy((void *)p_vaddr, (void *)((uint64_t)elf + phdr->p_offset), p_filesz);
    }

    if (p_memsz > p_filesz) { // 这个是bss段
        memset((void *)(p_vaddr + p_filesz), 0, p_memsz - p_filesz);
    }
    switch_process_page_directory(dir);
}

bool mmap_phdr_segment(Elf64_Ehdr *ehdr, Elf64_Phdr *phdrs, page_directory_t *directory,
                       bool is_user, uint64_t offset, uint64_t *load_start, uint64_t *load_size) {
    size_t i = 0;
    while (i < ehdr->e_phnum && phdrs[i].p_type != PT_LOAD) {
        i++;
    }

    if (i == ehdr->e_phnum) { return false; }

    uint64_t load_min = 0xffffffffffffffff;
    uint64_t load_max = 0x0000000000000000;

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            load_segment(&phdrs[i], (void *)ehdr, directory, is_user, offset, load_start);
            if (phdrs[i].p_vaddr + offset + phdrs[i].p_memsz > load_max)
                load_max = phdrs[i].p_vaddr + offset + phdrs[i].p_memsz;
            if (phdrs[i].p_vaddr + offset < load_min) load_min = phdrs[i].p_vaddr + offset;
        }
    }

    if (load_size) { *load_size = load_max - load_min; }

    return true;
}

bool is_dynamic(Elf64_Ehdr *ehdr) {
    if (ehdr->e_type != ET_DYN) { return false; }
    if (ehdr->e_phnum == 0 || ehdr->e_phoff == 0) { return false; }
    return true;
}

elf_start load_executor_elf(uint8_t *data, page_directory_t *dir, uint64_t offset,
                            uint64_t *load_start, pcb_t process) {
    if (data == NULL) return NULL;
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)data;
    if (!elf_test_head(ehdr)) { return NULL; }
    Elf64_Phdr       *phdrs = (Elf64_Phdr *)((char *)ehdr + ehdr->e_phoff);
    page_directory_t *cur   = get_current_directory();
    switch_process_page_directory(dir);
    uint64_t load_size = 0;
    if (!mmap_phdr_segment(ehdr, phdrs, dir, true, offset, load_start, &load_size)) { return NULL; }
    // VMA
    if (process != NULL) {
        vma_t *ld_so_vma = vma_alloc();

        ld_so_vma->vm_start  = *load_start;
        ld_so_vma->vm_end    = *load_start + load_size;
        ld_so_vma->vm_flags |= VMA_READ | VMA_WRITE | VMA_EXEC;

        ld_so_vma->vm_type = VMA_TYPE_ANON;
        ld_so_vma->vm_name = strdup(process->name);
        vma_insert(&process->vma_manager, ld_so_vma);
    }
    switch_process_page_directory(cur);
    return (elf_start)ehdr->e_entry;
}


// void dlinker_load(kernel_mode_t *kmod,cp_module_t *module) {
//     if (module == NULL) return;

//     Elf64_Ehdr *ehdr = (Elf64_Ehdr *)module->data;

//     Elf64_Phdr phdr[12];
//     if (ehdr->e_phnum > sizeof(phdr) / sizeof(phdr[0]) || ehdr->e_phnum < 1) {
//         write_serial_fmt("ELF file has wrong number of program headers");
//         return;
//     }

//     if (ehdr->e_type != ET_DYN) {
//         write_serial_fmt("ELF file is not a dynamic library.\n");
//         return;
//     }

//     uint64_t load_size = 0;

//     Elf64_Phdr *phdrs = (Elf64_Phdr *)((char *)ehdr + ehdr->e_phoff);
//     if (!mmap_phdr_segment(ehdr, phdrs, get_current_directory(), false,
//                            KERNEL_MOD_SPACE_START + kernel_modules_load_offset, NULL, &load_size)) {
//         write_serial_fmt("Cannot mmap elf segment.\n");
//         return;
//     }

//     dlinit_t dlinit =
//         load_dynamic(kmod,phdrs, ehdr, KERNEL_MOD_SPACE_START + kernel_modules_load_offset);
//     if (dlinit == NULL) {
//         dlinit = (dlinit_t)ehdr->e_entry;
//         if (dlinit == NULL) {
//             write_serial_fmt("Cannot find dlinit function.\n");
//             return;
//         }
//     }
//     write_serial_fmt("kmod: loaded module %s at 0x%x\n", module->module_name, KERNEL_MOD_SPACE_START + kernel_modules_load_offset);
//     kmod->entry_exit_code = dlinit();

//     kernel_modules_load_offset += (load_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
// }

static bool dlinker_mangled_global_matches(const char *mangled, const char *exported)
{
    if (mangled == NULL || exported == NULL || mangled[0] != '_' || mangled[1] != 'Z') return false;
    const char *cursor = mangled + 2;
    if (*cursor < '0' || *cursor > '9') return false;
    size_t length = 0;
    while (*cursor >= '0' && *cursor <= '9')
    {
        length = length * 10 + (size_t)(*cursor - '0');
        cursor++;
    }
    return length != 0 && strlen(exported) == length && strncmp(cursor, exported, length) == 0;
}

dlfunc_t *find_func(const char *name) {
    for (size_t i = 0; i < dlfunc_count; i++) {
        dlfunc_t *entry = &__ksymtab_start[i];
        if (strcmp(entry->name, name) == 0 || dlinker_mangled_global_matches(name, entry->name)) { return entry; }
    }
    return NULL;
}

void find_kernel_symbol() {
    dlfunc_count = __ksymtab_end - __ksymtab_start;
    for (size_t i = 0; i < dlfunc_count; i++) {
        dlfunc_t *entry = &__ksymtab_start[i];
    }
}

cp_module_t module_ls[256];
size_t      module_count = 0;

id_allocator_t *kmod_allocator = NULL;
kernel_mode_t *kmods[MAX_KERNEL_MODULE];

void extract_name(const char *input, char *output, size_t output_size) {
    const char *name = strrchr(input, '/');
    if (!name) { return; }
    name++;
    const char *dot = strchr(name, '.');
    if (dot) {
        size_t len = dot - name;
        if (len >= output_size) { len = output_size - 1; }
        strncpy(output, name, len);
        output[len] = '\0';
    } else {
        strncpy(output, name, output_size - 1);
        output[output_size - 1] = '\0';
    }
}

cp_module_t *get_module(const char *module_name) {
    if(module_name == NULL) return NULL;
    for (size_t i = 0; i < module_count; i++) {
        if (module_ls[i].is_use && strcmp(module_ls[i].module_name, module_name) == 0) {
            return &module_ls[i];
        }
    }
    return NULL;
}

cp_module_t *get_module_raw(const char *module_name) {
    if(module_name == NULL) return NULL;
    for (size_t i = 0; i < module_count; i++) {
        if (module_ls[i].is_use && strcmp(module_ls[i].raw_name, module_name) == 0) {
            return &module_ls[i];
        }
    }
    return NULL;
}

void module_setup() {
    vfs_node_t mr = vfs_open("/mod");
    if (mr == NULL) {
        write_serial_fmt("kmod: /mod not found\n");
        return;
    }
    if (mr->type != file_dir) {
        write_serial_string("kmod: /mod is not a directory\n");
        vfs_close(mr);
        return;
    }
    list_foreach(mr->child,i) {
        if (module_count >= MAX_KERNEL_MODULE) {
            write_serial_string("kmod: module list full\n");
            break;
        }
        vfs_node_t ch = (vfs_node_t)i->data;
        if (ch == NULL || ch->name == NULL) {
            continue;
        }
        size_t path_len = strlen("/mod/") + strlen(ch->name) + 1;
        char *buf = (char *)malloc(path_len);
        if (buf == NULL) {
            write_serial_fmt("kmod: no memory for %s\n", ch->name);
            continue;
        }
        snprintf(buf, path_len, "/mod/%s", ch->name);
        bool exists = false;
        for (size_t j = 0; j < module_count; j++) {
            if (module_ls[j].is_use && strcmp(module_ls[j].raw_name, buf) == 0) {
                exists = true;
                break;
            }
        }
        if (exists) {
            free(buf);
            continue;
        }
        extract_name(buf, module_ls[module_count].module_name, sizeof(char) * 20);
        module_ls[module_count].path = buf;
        uint8_t *data = dlinker_read_entire_file(ch);
        if (data == NULL) {
            write_serial_fmt("kmod: Failed to read %s\n", buf);
            free(buf);
            continue;
        }
        module_ls[module_count].data = data;
        module_ls[module_count].size = ch->size;
        snprintf(module_ls[module_count].raw_name, sizeof(module_ls[module_count].raw_name), "%s", buf);
        module_ls[module_count].is_use = true;
        write_serial_fmt("kmod: Module %s %s\n", module_ls[module_count].module_name, buf);
        module_count++;
    }
    vfs_close(mr);
}

static bool ends_with_sys(const char *str) {
    size_t len = strlen(str);
    if (len < 4) return false;
    return strcmp(str + len - 4, ".sys") == 0;
}

void start_all_kernel_module() {
    for (size_t i = 0; i < MAX_KERNEL_MODULE; i++) {
        kernel_mode_t *kmod = kmods[i];
        if(kmod == NULL) continue;
        if(kmod->task_entry == NULL) continue;
        if(kmod->entry_exit_code & 0x7FFFFFFFFFFFF000) {
            write_serial_fmt("kmod: cannot start mod(%s) - exit_code: %d\n",kmod->module->module_name,
                  kmod->entry_exit_code);
        }else{
            int ret = kmod->task_entry();
        }
    }
}

void load_all_kernel_module() {
    kmod_allocator = id_allocator_create(MAX_KERNEL_MODULE);
    for (size_t i = 0; i < module_count; i++) {
        if (module_ls[i].is_use) {
            if (ends_with_sys(module_ls[i].raw_name)) {
                cp_module_t *mod = get_module(module_ls[i].module_name);
                if (mod) {
                    int id = id_alloc(kmod_allocator);
                    kmods[id] = (kernel_mode_t*)calloc(1,sizeof(kernel_mode_t));
                    dlinker_load(kmods[id],mod);
                    kmods[id]->module = mod;
                }
            }
        }
    }
}


















// 全局符号表（所有已加载动态库的符号）
typedef struct {
    const char*     name;
    void*           addr;
    dlhandle_t*     owner;
} global_symbol_entry_t;

#define MAX_GLOBAL_SYMBOLS 4096
global_symbol_entry_t global_symbol_table[MAX_GLOBAL_SYMBOLS];
size_t global_symbol_count = 0;

// 动态库句柄链表
static dlhandle_t* dlhandle_list = NULL;
static size_t dlhandle_count = 0;

extern dlfunc_t __ksymtab_start[]; // .ksymtab section
extern dlfunc_t __ksymtab_end[];

// 用户态动态库加载地址范围
static uint64_t user_so_next_addr = USER_SO_BASE_START;

// ==================== 辅助函数 ====================

static void *dynamic_ptr(dlhandle_t *handle, Elf64_Xword ptr) {
    if (handle == NULL || ptr == 0 || ptr > ~0ULL - handle->base_addr) {
        return NULL;
    }
    uint64_t address = handle->base_addr + ptr;
    return handle_range_loaded(handle, address, 1) ? (void *)address : NULL;
}

static bool symbol_is_defined(Elf64_Sym *sym) {
    return sym != NULL && sym->st_shndx != SHN_UNDEF;
}

static bool symbol_is_exported(Elf64_Sym *sym) {
    if (!symbol_is_defined(sym)) {
        return false;
    }
    unsigned char bind = ELF64_ST_BIND(sym->st_info);
    unsigned char type = ELF64_ST_TYPE(sym->st_info);
    unsigned char vis  = ELF64_ST_VISIBILITY(sym->st_other);
    return (bind == STB_GLOBAL || bind == STB_WEAK) &&
           (type == STT_NOTYPE || type == STT_OBJECT || type == STT_FUNC || type == STT_GNU_IFUNC) &&
           (vis == STV_DEFAULT || vis == STV_PROTECTED);
}

static size_t dynsym_count_from_sysv_hash(dlhandle_t *handle, Elf64_Xword ptr) {
    uint32_t *hash = (uint32_t *)dynamic_ptr(handle, ptr);
    if (hash == NULL || !handle_range_loaded(handle, (uint64_t)hash, 2 * sizeof(uint32_t))) {
        return 0;
    }
    return hash[1] <= 65536 ? hash[1] : 0;
}

static size_t dynsym_count_from_gnu_hash(dlhandle_t *handle, Elf64_Xword ptr) {
    uint32_t *hash = (uint32_t *)dynamic_ptr(handle, ptr);
    if (hash == NULL || !handle_range_loaded(handle, (uint64_t)hash, 4 * sizeof(uint32_t))) {
        return 0;
    }

    uint32_t nbuckets   = hash[0];
    uint32_t symoffset  = hash[1];
    uint32_t bloom_size = hash[2];
    if (nbuckets == 0 || nbuckets > 65536 || bloom_size == 0 || bloom_size > 65536) return 0;
    uint64_t *bloom     = (uint64_t *)(hash + 4);
    uint32_t *buckets   = (uint32_t *)(bloom + bloom_size);
    if (!handle_range_loaded(handle, (uint64_t)bloom, (size_t)bloom_size * sizeof(uint64_t)) ||
        !handle_range_loaded(handle, (uint64_t)buckets, (size_t)nbuckets * sizeof(uint32_t))) return 0;
    uint32_t *chains    = buckets + nbuckets;
    uint32_t max_sym    = symoffset;

    for (uint32_t i = 0; i < nbuckets; i++) {
        uint32_t sym = buckets[i];
        if (sym == 0) {
            continue;
        }
        if (sym < symoffset) return 0;
        while (true) {
            uint32_t *chain = &chains[sym - symoffset];
            if (sym >= 65536 || !handle_range_loaded(handle, (uint64_t)chain, sizeof(*chain))) return 0;
            if ((*chain & 1) != 0) break;
            sym++;
        }
        if (sym + 1 > max_sym) {
            max_sym = sym + 1;
        }
    }

    return max_sym;
}

static size_t dynsym_count_from_sections(dlhandle_t *handle) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)handle->ehdr;
    if (ehdr == NULL || ehdr->e_shoff == 0 || ehdr->e_shentsize != sizeof(Elf64_Shdr)) {
        return 0;
    }

    Elf64_Shdr *shdrs = (Elf64_Shdr *)((char *)ehdr + ehdr->e_shoff);
    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_DYNSYM) {
            size_t ent = shdrs[i].sh_entsize ? shdrs[i].sh_entsize : sizeof(Elf64_Sym);
            return shdrs[i].sh_size / ent;
        }
    }
    return 0;
}

static dlfunc_t *find_export(dlhandle_t *handle, const char *name) {
    if (handle == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < handle->export_count; i++) {
        if (strcmp(handle->exports[i].name, name) == 0) {
            return &handle->exports[i];
        }
    }
    return NULL;
}

// 查找符号（支持全局查找）
static void* find_symbol_internal(const char* name, dlhandle_t* exclude) {
    // 1. 先在全局符号表中查找
    for (size_t i = 0; i < global_symbol_count; i++) {
        if (strcmp(global_symbol_table[i].name, name) == 0 ||
            dlinker_mangled_global_matches(name, global_symbol_table[i].name)) {
            // 确保不是来自排除的句柄
            if (exclude == NULL || global_symbol_table[i].owner != exclude) {
                return global_symbol_table[i].addr;
            }
        }
    }

    // 2. 在内核符号表中查找
    for (size_t i = 0; i < dlfunc_count; i++) {
        dlfunc_t* entry = &__ksymtab_start[i];
        if (strcmp(entry->name, name) == 0 || dlinker_mangled_global_matches(name, entry->name)) {
            return entry->addr;
        }
    }

    // 3. 在指定的动态库中查找（如果提供了句柄）
    if (exclude != NULL) {
        for (size_t i = 0; i < exclude->export_count; i++) {
            if (strcmp(exclude->exports[i].name, name) == 0) {
                return exclude->exports[i].addr;
            }
        }
    }
    
    return NULL;
}

// 添加符号到全局符号表
static bool add_global_symbol(const char* name, void* addr, dlhandle_t* owner) {
    if (global_symbol_count >= MAX_GLOBAL_SYMBOLS) {
        write_serial_fmt("Global symbol table full\n");
        return false;
    }
    
    // 检查是否已存在
    for (size_t i = 0; i < global_symbol_count; i++) {
        if (strcmp(global_symbol_table[i].name, name) == 0) {
            if (global_symbol_table[i].owner == owner) {
                global_symbol_table[i].addr = addr;
            }
            return true;
        }
    }
    
    // 添加新符号
    global_symbol_table[global_symbol_count].name = name;
    global_symbol_table[global_symbol_count].addr = addr;
    global_symbol_table[global_symbol_count].owner = owner;
    global_symbol_count++;
    
    return true;
}

// 从动态库提取导出符号
static void extract_exports_from_dynamic(dlhandle_t* handle, Elf64_Dyn* dyn) {
    Elf64_Dyn* entry = dyn;
    while (entry->d_tag != DT_NULL) {
        if (entry->d_tag == DT_SYMTAB) {
            handle->symtab = (Elf64_Sym*)dynamic_ptr(handle, entry->d_un.d_ptr);
        } else if (entry->d_tag == DT_STRTAB) {
            handle->strtab = (char*)dynamic_ptr(handle, entry->d_un.d_ptr);
        } else if (entry->d_tag == DT_STRSZ) {
            handle->strtabsz = entry->d_un.d_val;
        } else if (entry->d_tag == DT_SYMENT) {
            handle->symtabsz = entry->d_un.d_val;
        } else if (entry->d_tag == DT_HASH) {
            handle->sym_count = dynsym_count_from_sysv_hash(handle, entry->d_un.d_ptr);
        } else if (entry->d_tag == DT_GNU_HASH) {
            handle->sym_count = dynsym_count_from_gnu_hash(handle, entry->d_un.d_ptr);
        }
        entry++;
    }
    
    if (handle->sym_count == 0) {
        handle->sym_count = dynsym_count_from_sections(handle);
    }
    if (handle->symtabsz == 0) {
        handle->symtabsz = sizeof(Elf64_Sym);
    }
    if (handle->symtab && handle->strtab && handle->strtabsz && handle->sym_count &&
        handle->sym_count <= 65536 &&
        handle_range_loaded(handle, (uint64_t)handle->symtab, handle->sym_count * sizeof(Elf64_Sym)) &&
        handle_range_loaded(handle, (uint64_t)handle->strtab, handle->strtabsz)) {
        size_t num_symbols = handle->sym_count;

        // 计算导出符号数量
        size_t export_count = 0;
        for (size_t i = 0; i < num_symbols; i++) {
            Elf64_Sym* sym = &handle->symtab[i];
            if (symbol_is_exported(sym) && sym->st_name < handle->strtabsz &&
                memchr(handle->strtab + sym->st_name, '\0', handle->strtabsz - sym->st_name) != NULL) {
                export_count++;
            }
        }

        // 分配并填充导出表
        handle->exports = (dlfunc_t*)calloc(export_count, sizeof(dlfunc_t));
        handle->export_count = 0;
        
        for (size_t i = 0; i < num_symbols; i++) {
            Elf64_Sym* sym = &handle->symtab[i];
            if (symbol_is_exported(sym) && sym->st_name < handle->strtabsz &&
                memchr(handle->strtab + sym->st_name, '\0', handle->strtabsz - sym->st_name) != NULL &&
                sym->st_value <= ~0ULL - handle->base_addr &&
                handle_range_loaded(handle, handle->base_addr + sym->st_value, sym->st_size ? sym->st_size : 1)) {
                char* sym_name = &handle->strtab[sym->st_name];
                
                handle->exports[handle->export_count].name = strdup(sym_name);
                handle->exports[handle->export_count].addr = 
                    (void*)(handle->base_addr + sym->st_value);
                handle->export_count++;
                
                // 添加到全局符号表
                add_global_symbol(sym_name, 
                    handle->exports[handle->export_count - 1].addr, handle);
            }
        }
    }
}

// 解析依赖项
static bool resolve_dependencies(dlhandle_t* handle, Elf64_Dyn* dyn) {
    Elf64_Dyn* entry = dyn;
    
    // 先计算依赖项数量
    size_t dep_count = 0;
    while (entry->d_tag != DT_NULL) {
        if (entry->d_tag == DT_NEEDED) {
            dep_count++;
            if (dep_count > 64) return false;
        }
        entry++;
    }
    
    if (dep_count == 0) return true;
    
    // 分配依赖项数组
    handle->dependencies = (dlhandle_t**)calloc(dep_count, sizeof(dlhandle_t*));
    handle->dep_count = 0;
    
    // 重新遍历，加载依赖项
    entry = dyn;
    while (entry->d_tag != DT_NULL) {
        if (entry->d_tag == DT_NEEDED) {
            if (handle->strtab == NULL || entry->d_un.d_val >= handle->strtabsz ||
                memchr(handle->strtab + entry->d_un.d_val, '\0', handle->strtabsz - entry->d_un.d_val) == NULL)
                return false;
            const char* lib_name = (char*)(handle->strtab + entry->d_un.d_val);
            write_serial_fmt("Loading dependency: %s for %s\n", lib_name, handle->path);
            
            // 查找是否已加载
            dlhandle_t* dep_handle = NULL;
            dlhandle_t* current = dlhandle_list;
            while (current) {
                if (strstr(current->path, lib_name)) {
                    dep_handle = current;
                    break;
                }
                current = current->next;
            }
            
            // 如果未加载，则加载
            if (!dep_handle) {
                // 构建完整路径
                char full_path[384];
                if (lib_name[0] == '/' || strchr(lib_name, '/')) {
                    snprintf(full_path, sizeof(full_path), "%s", lib_name);
                } else if (handle->is_kernel) {
                    snprintf(full_path, sizeof(full_path), "/lib/modules/%s", lib_name);
                } else {
                    bool found = false;
                    // 1) Try RPATH/RUNPATH
                    if (handle->rpath) {
                        char rp_buf[384];
                        snprintf(rp_buf, sizeof(rp_buf), "%s/%s", handle->rpath, lib_name);
                        vfs_node_t test = vfs_open(rp_buf);
                        if (test) {
                            snprintf(full_path, sizeof(full_path), "%s", rp_buf);
                            vfs_close(test);
                            found = true;
                        }
                    }
                    // 2) Fall back to /lib/
                    if (!found)
                        snprintf(full_path, sizeof(full_path), "/lib/%s", lib_name);
                }
                
                dep_handle = load_shared_object_internal(full_path, 
                    handle->is_kernel ? RTLD_KERNEL : 0, handle->pd);
                if (!dep_handle) {
                    write_serial_fmt("Failed to load dependency: %s\n", lib_name);
                    return false;
                }
            }
            
            handle->dependencies[handle->dep_count++] = dep_handle;
        }
        entry++;
    }
    
    return true;
}

static void* resolve_relocation_symbol(dlhandle_t* handle, Elf64_Sym* sym) {
    if (handle == NULL || sym == NULL) {
        return NULL;
    }

    if (symbol_is_defined(sym)) {
        if (sym->st_value > ~0ULL - handle->base_addr ||
            !handle_range_loaded(handle, handle->base_addr + sym->st_value, sym->st_size ? sym->st_size : 1))
            return NULL;
        unsigned char bind = ELF64_ST_BIND(sym->st_info);
        unsigned char vis  = ELF64_ST_VISIBILITY(sym->st_other);
        if (bind == STB_LOCAL || vis == STV_HIDDEN || vis == STV_INTERNAL || vis == STV_PROTECTED) {
            return (void *)(handle->base_addr + sym->st_value);
        }
    }

    const char *name = NULL;
    if (handle->strtab != NULL && sym->st_name < handle->strtabsz &&
        memchr(handle->strtab + sym->st_name, '\0', handle->strtabsz - sym->st_name) != NULL)
        name = &handle->strtab[sym->st_name];
    if (name != NULL && name[0] != '\0') {
        void *addr = find_symbol_internal(name, handle);
        if (addr != NULL) {
            return addr;
        }

        dlfunc_t *self_export = find_export(handle, name);
        if (self_export != NULL) {
            return self_export->addr;
        }
    }

    if (symbol_is_defined(sym)) {
        return (void *)(handle->base_addr + sym->st_value);
    }

    if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
        return NULL;
    }

    write_serial_fmt("Undefined symbol: %s\n", name ? name : "<noname>");
    return NULL;
}

static bool call_dynamic_initializers(dlhandle_t* handle) {
    if (handle == NULL || handle->dynamic == NULL || handle->init_called) {
        return true;
    }

    void (**preinit_array)(void) = NULL;
    size_t preinit_array_sz = 0;
    void (*init_func)(void) = NULL;
    void (**init_array)(void) = NULL;
    size_t init_array_sz = 0;

    for (Elf64_Dyn *entry = handle->dynamic; entry->d_tag != DT_NULL; entry++) {
        switch (entry->d_tag) {
        case DT_PREINIT_ARRAY:
            preinit_array = (void (**)(void))dynamic_ptr(handle, entry->d_un.d_ptr);
            break;
        case DT_PREINIT_ARRAYSZ:
            preinit_array_sz = entry->d_un.d_val;
            break;
        case DT_INIT:
            init_func = (void (*)(void))dynamic_ptr(handle, entry->d_un.d_ptr);
            break;
        case DT_INIT_ARRAY:
            init_array = (void (**)(void))dynamic_ptr(handle, entry->d_un.d_ptr);
            break;
        case DT_INIT_ARRAYSZ:
            init_array_sz = entry->d_un.d_val;
            break;
        }
    }

    for (size_t i = 0; preinit_array && i < preinit_array_sz / sizeof(void (*)(void)); i++) {
        if (preinit_array[i]) {
            preinit_array[i]();
        }
    }
    if (init_func) {
        init_func();
    }
    for (size_t i = 0; init_array && i < init_array_sz / sizeof(void (*)(void)); i++) {
        if (init_array[i]) {
            init_array[i]();
        }
    }

    handle->init_called = true;
    return true;
}

static void call_dynamic_finalizers(dlhandle_t* handle) {
    if (handle == NULL || handle->dynamic == NULL || !handle->init_called) {
        return;
    }

    void (*fini_func)(void) = NULL;
    void (**fini_array)(void) = NULL;
    size_t fini_array_sz = 0;

    for (Elf64_Dyn *entry = handle->dynamic; entry->d_tag != DT_NULL; entry++) {
        switch (entry->d_tag) {
        case DT_FINI:
            fini_func = (void (*)(void))dynamic_ptr(handle, entry->d_un.d_ptr);
            break;
        case DT_FINI_ARRAY:
            fini_array = (void (**)(void))dynamic_ptr(handle, entry->d_un.d_ptr);
            break;
        case DT_FINI_ARRAYSZ:
            fini_array_sz = entry->d_un.d_val;
            break;
        }
    }

    for (size_t i = fini_array_sz / sizeof(void (*)(void)); fini_array && i > 0; i--) {
        if (fini_array[i - 1]) {
            fini_array[i - 1]();
        }
    }
    if (fini_func) {
        fini_func();
    }
    handle->init_called = false;
}

// 内部加载函数
dlhandle_t* load_shared_object_internal(const char* path, int flags, 
                                               page_directory_t* pd) {
    if (!path) return NULL;

    dlhandle_t* current = dlhandle_list;
    while (current) {
        if (current->path && strcmp(current->path, path) == 0) {
            write_serial_fmt("Reusing shared object: %s at 0x%llx\n", path, current->base_addr);
            if (current->ref_count == 0) {
                current->ref_count = 1;
                call_dynamic_initializers(current);
            } else {
                current->ref_count++;
            }
            return current;
        }
        current = current->next;
    }
    
    // 1. 打开文件
    vfs_node_t file = vfs_open(path);
    if (!file) {
        write_serial_fmt("Failed to open: %s\n", path);
        return NULL;
    }
    
    // 2. 读取文件到内存
    size_t file_size = file->size;
    uint8_t* file_data = dlinker_read_entire_file(file);
    if (!file_data) {
        vfs_close(file);
        return NULL;
    }
    vfs_close(file);
    
    // 3. 解析 ELF 头
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)file_data;
    if (file_size < sizeof(Elf64_Ehdr) || !elf_test_head(ehdr)) {
        write_serial_fmt("Invalid ELF file: %s\n", path);
        dlinker_free_file_buffer(file_data, file_size);
        return NULL;
    }
    
    // 4. 检查是否为动态库
    if (ehdr->e_type != ET_DYN) {
        write_serial_fmt("Not a shared object: %s\n", path);
        dlinker_free_file_buffer(file_data, file_size);
        return NULL;
    }
    
    // 5. 确定加载地址
    uint64_t base_addr = 0;
    bool is_kernel = (flags & RTLD_KERNEL) != 0;
    
    if (is_kernel) {
        // 内核模块：使用固定地址空间
        if (kernel_modules_load_offset >= (0xffffffffc0000000 - 0xffffffffb0000000)) {
            write_serial_fmt("Kernel module space exhausted\n");
            dlinker_free_file_buffer(file_data, file_size);
            return NULL;
        }
        base_addr = 0xffffffffb0000000 + kernel_modules_load_offset;
    } else {
        // 用户态动态库：分配地址空间
        if (user_so_next_addr >= USER_SO_BASE_END) {
            write_serial_fmt("User SO space exhausted\n");
            dlinker_free_file_buffer(file_data, file_size);
            return NULL;
        }
        base_addr = user_so_next_addr;
    }

    uint64_t validated_load_size = 0;
    if (!validate_shared_object(ehdr, file_size, base_addr, is_kernel, &validated_load_size))
    {
        write_serial_fmt("Unsafe or malformed shared object: %s\n", path);
        dlinker_free_file_buffer(file_data, file_size);
        return NULL;
    }
    
    // 6. 创建句柄
    dlhandle_t* handle = (dlhandle_t*)calloc(1, sizeof(dlhandle_t));
    handle->path = strdup(path);
    handle->base_addr = base_addr;
    handle->ehdr = file_data; // 保留文件数据指针
    handle->size = file_size;
    handle->is_kernel = is_kernel;
    handle->pd = pd;
    handle->ref_count = 1;
    
    // 7. 映射段
    Elf64_Phdr* phdrs = (Elf64_Phdr*)((char*)ehdr + ehdr->e_phoff);
    uint64_t load_size = 0;
    
    if (!mmap_phdr_segment(ehdr, phdrs, pd ? pd : get_current_directory(), 
                          !is_kernel, base_addr, NULL, &load_size)) {
        write_serial_fmt("Failed to map segments: %s\n", path);
        free(handle->path);
        free(handle);
        dlinker_free_file_buffer(file_data, file_size);
        return NULL;
    }
    if (load_size > validated_load_size) {
        write_serial_fmt("Mapped size exceeds validated image: %s\n", path);
        return NULL;
    }
    
    // 8. 更新地址空间
    if (is_kernel) {
        kernel_modules_load_offset += (validated_load_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    } else {
        user_so_next_addr += (validated_load_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    }
    
    // 9. 处理动态段
    Elf64_Phdr* dynamic_phdr = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dynamic_phdr = &phdrs[i];
            break;
        }
    }
    
    if (dynamic_phdr) {
        Elf64_Dyn* dyn = (Elf64_Dyn*)(base_addr + dynamic_phdr->p_vaddr);
        handle->dynamic = dyn;
        
        // 提取符号信息
        extract_exports_from_dynamic(handle, dyn);

        // 提取 RPATH/RUNPATH
        {
            Elf64_Dyn *rp = dyn;
            char *rpath_str = NULL;
            char *runpath_str = NULL;
            while (rp->d_tag != DT_NULL) {
                if ((rp->d_tag == DT_RPATH || rp->d_tag == DT_RUNPATH) && handle->strtab != NULL &&
                    rp->d_un.d_val < handle->strtabsz &&
                    memchr(handle->strtab + rp->d_un.d_val, '\0', handle->strtabsz - rp->d_un.d_val) != NULL)
                {
                    if (rp->d_tag == DT_RPATH) rpath_str = handle->strtab + rp->d_un.d_val;
                    else runpath_str = handle->strtab + rp->d_un.d_val;
                }
                rp++;
            }
            const char *search = runpath_str ? runpath_str : rpath_str;
            if (search) {
                handle->rpath = strdup(search);
                write_serial_fmt("RPATH/RUNPATH for %s: %s\n", path, handle->rpath);
            }
        }

        // 解析依赖项
        if (!resolve_dependencies(handle, dyn)) {
            write_serial_fmt("Failed to resolve dependencies: %s\n", path);
            // 清理部分加载的资源
            return NULL;
        }
        
        // 处理重定位
        if (!process_dynamic_relocations(dyn, handle)) {
            write_serial_fmt("Failed to process relocations: %s\n", path);
            return NULL;
        }
        if (!call_dynamic_initializers(handle)) {
            write_serial_fmt("Failed to run initializers: %s\n", path);
            return NULL;
        }
    }
    
    // 11. 添加到句柄链表
    if (!dlhandle_list) {
        dlhandle_list = handle;
    } else {
        handle->next = dlhandle_list;
        dlhandle_list->prev = handle;
        dlhandle_list = handle;
    }
    dlhandle_count++;
    
    write_serial_fmt("Loaded shared object: %s at 0x%llx\n", path, base_addr);
    return handle;
}

// 处理动态重定位
bool process_dynamic_relocations(Elf64_Dyn* dyn, dlhandle_t* handle) {
    Elf64_Rela* rela = NULL;
    size_t relasz = 0;
    size_t relaent = sizeof(Elf64_Rela);
    Elf64_Rela* jmprel = NULL;
    size_t pltrelsz = 0;
    int pltrel_type = DT_RELA;
    
    Elf64_Dyn* entry = dyn;
    while (entry->d_tag != DT_NULL) {
        switch (entry->d_tag) {
        case DT_RELA:
            rela = (Elf64_Rela*)dynamic_ptr(handle, entry->d_un.d_ptr);
            break;
        case DT_RELASZ:
            relasz = entry->d_un.d_val;
            break;
        case DT_RELAENT:
            relaent = entry->d_un.d_val;
            break;
        case DT_JMPREL:
            jmprel = (Elf64_Rela*)dynamic_ptr(handle, entry->d_un.d_ptr);
            break;
        case DT_PLTRELSZ:
            pltrelsz = entry->d_un.d_val;
            break;
        case DT_PLTREL:
            pltrel_type = entry->d_un.d_val;
            break;
        }
        entry++;
    }
    if (relaent != sizeof(Elf64_Rela)) {
        write_serial_fmt("Unsupported RELA entry size: %u\n", relaent);
        return false;
    }
    if (pltrel_type != DT_RELA) {
        write_serial_fmt("Unsupported PLT relocation format: %d\n", pltrel_type);
        return false;
    }
    if ((relasz % sizeof(Elf64_Rela)) != 0 || (pltrelsz % sizeof(Elf64_Rela)) != 0 ||
        (relasz != 0 && (rela == NULL || !handle_range_loaded(handle, (uint64_t)rela, relasz))) ||
        (pltrelsz != 0 && (jmprel == NULL || !handle_range_loaded(handle, (uint64_t)jmprel, pltrelsz))))
        return false;
    
    // 处理普通重定位
    if (rela && relasz) {
        size_t rela_count = relasz / sizeof(Elf64_Rela);
        for (size_t i = 0; i < rela_count; i++) {
            Elf64_Rela* r = &rela[i];
            uint32_t type = ELF64_R_TYPE(r->r_info);
            uint32_t sym_idx = ELF64_R_SYM(r->r_info);
            if (r->r_offset > ~0ULL - handle->base_addr ||
                (sym_idx != 0 && (handle->symtab == NULL || sym_idx >= handle->sym_count))) return false;
            uint64_t* target = (uint64_t*)(handle->base_addr + r->r_offset);
            size_t target_size = (type == R_X86_64_PC32 || type == R_X86_64_PLT32 ||
                                  type == R_X86_64_32 || type == R_X86_64_32S || type == R_X86_64_SIZE32)
                                     ? sizeof(uint32_t) : sizeof(uint64_t);
            
            Elf64_Sym* sym = NULL;
            if (sym_idx && handle->symtab) {
                sym = &handle->symtab[sym_idx];
            }
            if (type == R_X86_64_COPY && sym != NULL) target_size = sym->st_size;
            if (type != R_X86_64_NONE && !handle_range_loaded(handle, (uint64_t)target, target_size)) return false;
            
            switch (type) {
            case R_X86_64_NONE:
                break;
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT: {
                if (sym) {
                    void* addr = resolve_relocation_symbol(handle, sym);
                    if (!addr && ELF64_ST_BIND(sym->st_info) != STB_WEAK) {
                        return false;
                    }
                    *target = (uint64_t)addr;
                }
                break;
            }
            case R_X86_64_RELATIVE:
                *target = handle->base_addr + r->r_addend;
                break;
            case R_X86_64_64:
                if (sym) {
                    void* addr = resolve_relocation_symbol(handle, sym);
                    if (!addr && ELF64_ST_BIND(sym->st_info) != STB_WEAK) {
                        return false;
                    }
                    *target = (uint64_t)addr + r->r_addend;
                }
                break;
            case R_X86_64_COPY:
                if (sym) {
                    void* addr = resolve_relocation_symbol(handle, sym);
                    if (!addr && ELF64_ST_BIND(sym->st_info) != STB_WEAK) {
                        return false;
                    }
                    if (addr && sym->st_size) {
                        memcpy(target, addr, sym->st_size);
                    }
                }
                break;
            case R_X86_64_PC32:
            case R_X86_64_PLT32:
                if (sym) {
                    void* addr = resolve_relocation_symbol(handle, sym);
                    if (!addr && ELF64_ST_BIND(sym->st_info) != STB_WEAK) {
                        return false;
                    }
                    *(uint32_t *)target = (uint32_t)((uint64_t)addr + r->r_addend - (uint64_t)target);
                }
                break;
            case R_X86_64_PC64:
                if (sym) {
                    void* addr = resolve_relocation_symbol(handle, sym);
                    if (!addr && ELF64_ST_BIND(sym->st_info) != STB_WEAK) {
                        return false;
                    }
                    *target = (uint64_t)addr + r->r_addend - (uint64_t)target;
                }
                break;
            case R_X86_64_32:
            case R_X86_64_32S:
                if (sym) {
                    void* addr = resolve_relocation_symbol(handle, sym);
                    if (!addr && ELF64_ST_BIND(sym->st_info) != STB_WEAK) {
                        return false;
                    }
                    *(uint32_t *)target = (uint32_t)((uint64_t)addr + r->r_addend);
                }
                break;
            case R_X86_64_SIZE32:
                if (sym) {
                    *(uint32_t *)target = (uint32_t)(sym->st_size + r->r_addend);
                }
                break;
            case R_X86_64_SIZE64:
                if (sym) {
                    *target = sym->st_size + r->r_addend;
                }
                break;
            case R_X86_64_IRELATIVE: {
                uint64_t (*resolver)(void) = (uint64_t (*)(void))(handle->base_addr + r->r_addend);
                *target = resolver();
                break;
            }
            default:
                write_serial_fmt("Unhandled relocation type: %u\n", type);
                return false;
            }
        }
    }
    
    // 处理 PLT 重定位
    if (jmprel && pltrelsz) {
        size_t jmprel_count = pltrelsz / sizeof(Elf64_Rela);
        for (size_t i = 0; i < jmprel_count; i++) {
            Elf64_Rela* r = &jmprel[i];
            uint32_t sym_idx = ELF64_R_SYM(r->r_info);
            if (r->r_offset > ~0ULL - handle->base_addr || sym_idx == 0 || handle->symtab == NULL ||
                sym_idx >= handle->sym_count) return false;
            uint64_t* target = (uint64_t*)(handle->base_addr + r->r_offset);
            if (!handle_range_loaded(handle, (uint64_t)target, sizeof(uint64_t))) return false;
            
            if (sym_idx && handle->symtab) {
                Elf64_Sym* sym = &handle->symtab[sym_idx];
                void* addr = resolve_relocation_symbol(handle, sym);
                if (!addr && ELF64_ST_BIND(sym->st_info) != STB_WEAK) {
                    return false;
                }
                *target = (uint64_t)addr;
            }
        }
    }
    
    return true;
}


void* dlopen(const char* filename, int flags) {
    if (!filename) {
        // 返回全局句柄
        return (void*)0x1;
    }
    
    // 检查是否已加载
    dlhandle_t* current = dlhandle_list;
    while (current) {
        if (strcmp(current->path, filename) == 0) {
            // 已加载，增加引用计数（TODO）
            current->ref_count++;
            return current;
        }
        current = current->next;
    }
    if (flags & RTLD_NOLOAD) {
        return NULL;
    }
    
    // 判断是否是内核模块
    bool is_kernel = false;
    if (strstr(filename, ".sys") || strstr(filename, "/system/")) {
        is_kernel = true;
        flags |= RTLD_KERNEL;
    }
    
    // 加载动态库
    page_directory_t* pd = NULL;
    if (!is_kernel) {
        pd = get_current_directory(); // 当前进程页表
    }
    
    dlhandle_t* handle = load_shared_object_internal(filename, flags, pd);
    return (void*)handle;
}

void* dlsym(void* handle, const char* symbol) {
    if (!symbol) return NULL;
    
    if (handle == RTLD_DEFAULT || handle == RTLD_NEXT || handle == (void*)0x1) {
        // 全局查找
        return find_symbol_internal(symbol, NULL);
    }
    
    dlhandle_t* dlhandle = (dlhandle_t*)handle;
    
    // 1. 先在当前动态库中查找
    for (size_t i = 0; i < dlhandle->export_count; i++) {
        if (strcmp(dlhandle->exports[i].name, symbol) == 0) {
            return dlhandle->exports[i].addr;
        }
    }
    
    // 2. 在依赖项中查找
    for (size_t i = 0; i < dlhandle->dep_count; i++) {
        dlhandle_t* dep = dlhandle->dependencies[i];
        for (size_t j = 0; j < dep->export_count; j++) {
            if (strcmp(dep->exports[j].name, symbol) == 0) {
                return dep->exports[j].addr;
            }
        }
    }
    
    // 3. 全局查找
    return find_symbol_internal(symbol, dlhandle);
}

int dlclose(void* handle) {
    if (!handle || handle == (void*)0x1) return -1;
    
    dlhandle_t* dlhandle = (dlhandle_t*)handle;
    
    if (dlhandle->ref_count > 1) {
        dlhandle->ref_count--;
        return 0;
    }
    call_dynamic_finalizers(dlhandle);
    dlhandle->ref_count = 0;
    
    write_serial_fmt("dlclose: %s \n", dlhandle->path);
    return 0;
}

// 错误信息缓冲区
static char dlerror_buf[256];
static char* dlerror_ptr = NULL;

// 标准 dlerror 实现
char* dlerror() {
    char* result = dlerror_ptr;
    dlerror_ptr = NULL;
    return result;
}

// 设置错误信息
static void set_dlerror(const char* msg) {
    strncpy(dlerror_buf, msg, sizeof(dlerror_buf) - 1);
    dlerror_buf[sizeof(dlerror_buf) - 1] = '\0';
    dlerror_ptr = dlerror_buf;
}

void dlinker_load(kernel_mode_t* kmod, cp_module_t* module) {
    if (!module) return;
    
    
    dlhandle_t* handle = load_shared_object_internal(strdup(module->path), RTLD_KERNEL, NULL);
    if (!handle) {
        write_serial_fmt("Failed to load kernel module: %s\n", module->module_name);
        return;
    }
    
    // 查找初始化函数
    dlfunc_t *entry_symbol = find_export(handle, "dlmain");
    if (!entry_symbol) entry_symbol = find_export(handle, "_dlmain");
    void *entry = entry_symbol ? entry_symbol->addr : NULL;
    
    kmod->entry = (dlinit_t)entry;
    kmod->task_entry = NULL; // 由模块决定
    
    // 执行初始化
    if (kmod->entry) {
        write_serial_fmt("find dlmain \n");
        kmod->entry_exit_code = kmod->entry();
    }
    
    kmod->module = module;
    write_serial_fmt("Loaded kernel module: %s at 0x%llx\n", 
                     module->module_name, handle->base_addr);
}

void dlinker_init() {
    // 初始化内核符号表
    dlfunc_count = __ksymtab_end - __ksymtab_start;
    write_serial_fmt("Found %lu kernel symbols\n", dlfunc_count);
    
    // 将内核符号添加到全局符号表
    for (size_t i = 0; i < dlfunc_count; i++) {
        dlfunc_t* entry = &__ksymtab_start[i];
        add_global_symbol(entry->name, entry->addr, NULL); // NULL 表示内核
    }
    
    write_serial_fmt("Dynamic linker initialized\n");
}
EXPORT_SYMBOL(page_virt_to_phys);
