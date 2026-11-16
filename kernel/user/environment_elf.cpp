#include <user/environment.h>
#include <elf.h>
#include <krlibc.h>

static bool environment_elf_range(uint64_t offset, uint64_t length, size_t size)
{
    return offset <= size && length <= size - offset;
}

static bool environment_elf_note(const uint8_t *bytes, uint64_t offset, uint64_t length, size_t size)
{
    if (!environment_elf_range(offset, length, size)) return false;
    uint64_t end = offset + length;
    while (end - offset >= 12)
    {
        uint32_t note[3];
        memcpy(note, bytes + offset, sizeof(note));
        offset += sizeof(note);
        uint64_t names = ((uint64_t)note[0] + 3) & ~3ULL;
        uint64_t description = ((uint64_t)note[1] + 3) & ~3ULL;
        if (names > end - offset || description > end - offset - names) break;
        if (note[0] == 6 && note[1] == 4 && note[2] == 0x58454e56 && !memcmp(bytes + offset, "XJ380", 6))
        {
            uint32_t version;
            memcpy(&version, bytes + offset + names, sizeof(version));
            if (version == 1) return true;
        }
        offset += names + description;
    }
    return false;
}

static bool environment_elf_entry_jump(const uint8_t *bytes, size_t size, const Elf64_Ehdr &header,
                                      uint64_t target)
{
    if (header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum > 4096 ||
        !environment_elf_range(header.e_phoff, (uint64_t)header.e_phnum * sizeof(Elf64_Phdr), size)) return false;
    for (unsigned i = 0; i < header.e_phnum; ++i)
    {
        Elf64_Phdr program;
        memcpy(&program, bytes + header.e_phoff + (uint64_t)i * sizeof(program), sizeof(program));
        if (program.p_type != PT_LOAD || header.e_entry < program.p_vaddr ||
            !environment_elf_range(program.p_offset, program.p_filesz, size)) continue;
        uint64_t displacement = header.e_entry - program.p_vaddr;
        if (displacement >= program.p_filesz) continue;
        const uint8_t *entry = bytes + program.p_offset + displacement;
        uint64_t available = program.p_filesz - displacement, prefix = 0;
        if (available >= 4 && !memcmp(entry, "\xf3\x0f\x1e\xfa", 4)) prefix = 4; // ENDBR64
        if (available - prefix >= 5 && entry[prefix] == 0xe9)
        {
            int32_t relative;
            memcpy(&relative, entry + prefix + 1, sizeof(relative));
            return header.e_entry + prefix + 5 + (int64_t)relative == target;
        }
        if (available - prefix >= 2 && entry[prefix] == 0xeb)
            return header.e_entry + prefix + 2 + (int64_t)(signed char)entry[prefix + 1] == target;
    }
    return false;
}

bool environment_elf_native_entry(const void *image, size_t size)
{
    if (!image || size < sizeof(Elf64_Ehdr)) return false;
    const uint8_t *bytes = (const uint8_t *)image;
    Elf64_Ehdr header;
    memcpy(&header, bytes, sizeof(header));
    if (memcmp(header.e_ident, "\177ELF", 4) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB) return false;

    // GNU and Linux share OSABI value 3. The XAPI startup note explicitly
    // identifies the native register ABI, including in stripped executables.
    if (header.e_phentsize == sizeof(Elf64_Phdr) && header.e_phnum <= 4096 &&
        environment_elf_range(header.e_phoff, (uint64_t)header.e_phnum * sizeof(Elf64_Phdr), size))
    {
        for (unsigned i = 0; i < header.e_phnum; ++i)
        {
            Elf64_Phdr program;
            memcpy(&program, bytes + header.e_phoff + (uint64_t)i * sizeof(program), sizeof(program));
            if (program.p_type == PT_NOTE &&
                environment_elf_note(bytes, program.p_offset, program.p_filesz, size)) return true;
        }
    }

    // Existing XAPI ELF applications predate the note. Their symbol table
    // identifies xapi_start at the actual entry; an arbitrary Linux symbol
    // named xapi_start elsewhere in the file must not change its ABI.
    if (header.e_shentsize != sizeof(Elf64_Shdr) || header.e_shnum > 4096 ||
        !environment_elf_range(header.e_shoff, (uint64_t)header.e_shnum * sizeof(Elf64_Shdr), size)) return false;
    for (unsigned i = 0; i < header.e_shnum; ++i)
    {
        Elf64_Shdr symbols;
        memcpy(&symbols, bytes + header.e_shoff + (uint64_t)i * sizeof(symbols), sizeof(symbols));
        if (symbols.sh_type == SHT_NOTE &&
            environment_elf_note(bytes, symbols.sh_offset, symbols.sh_size, size)) return true;
        if (symbols.sh_type != SHT_SYMTAB || symbols.sh_link >= header.e_shnum ||
            symbols.sh_entsize != sizeof(Elf64_Sym) || symbols.sh_size % sizeof(Elf64_Sym) ||
            !environment_elf_range(symbols.sh_offset, symbols.sh_size, size)) continue;
        Elf64_Shdr strings;
        memcpy(&strings, bytes + header.e_shoff + (uint64_t)symbols.sh_link * sizeof(strings), sizeof(strings));
        if (strings.sh_type != SHT_STRTAB || !environment_elf_range(strings.sh_offset, strings.sh_size, size))
            continue;
        for (uint64_t offset = 0; offset < symbols.sh_size; offset += sizeof(Elf64_Sym))
        {
            Elf64_Sym symbol;
            memcpy(&symbol, bytes + symbols.sh_offset + offset, sizeof(symbol));
            if (symbol.st_shndx != SHN_UNDEF &&
                (symbol.st_info & 15) == STT_FUNC && symbol.st_name <= strings.sh_size &&
                strings.sh_size - symbol.st_name >= 11 &&
                !memcmp(bytes + strings.sh_offset + symbol.st_name, "xapi_start", 11) &&
                (symbol.st_value == header.e_entry ||
                 environment_elf_entry_jump(bytes, size, header, symbol.st_value))) return true;
        }
    }
    return false;
}
