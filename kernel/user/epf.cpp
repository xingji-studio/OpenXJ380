// SPDX-License-Identifier: Apache-2.0
#include <epf.h>
#include <errno.h>
#include <krlibc.h>
#include <mm/frame.h>
#include <mm/lazyalloc.h>
#include <mm/page.h>
#include <mm/vma.h>
#include <proto.hpp>
#include <rng.h>
#include <syscall/syscall.h>
#include <task/pcb.h>

static bool epf_range(uint64_t offset, uint64_t length, uint64_t size)
{
    return offset <= size && length <= size - offset;
}

static bool epf_power_of_two(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static bool epf_zero(const uint8_t *bytes, uint64_t size)
{
    for (uint64_t i = 0; i < size; ++i)
    {
        if (bytes[i] != 0) return false;
    }
    return true;
}

static bool epf_overlap(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size)
{
    // Call only with ranges whose ends have already been checked.
    return a_size != 0 && b_size != 0 && a < b + b_size && b < a + a_size;
}

static int epf_reject(const char *reason, uint32_t index = 0, int error = -ENOEXEC)
{
    write_serial_fmt("EPF reject: %s index=%u ret=%d\n", reason, index, error);
    return error;
}

static EpfSegment epf_segment(const uint8_t *file, const EpfHeader &header, uint32_t index)
{
    const uint8_t *p = file + header.segment_table_offset + (uint64_t)index * header.segment_entry_size;
    return {epf_read_u32(p), epf_read_u32(p + 4), epf_read_u64(p + 8), epf_read_u64(p + 16),
            epf_read_u64(p + 24), epf_read_u64(p + 32), epf_read_u64(p + 40)};
}

static bool epf_table(uint64_t offset, uint32_t count, uint16_t stride, uint64_t size)
{
    if (count == 0) return offset == 0;
    return offset >= EPF_HEADER_SIZE && (offset & 7) == 0 && offset <= size && stride != 0 &&
           count <= (size - offset) / stride;
}

static int epf_header(const uint8_t *file, uint64_t size, EpfHeader *header)
{
    if (!epf_has_magic(file, size) || size < EPF_HEADER_SIZE) return epf_reject("truncated header or magic");
    if (size > EPF_MAX_FILE_SIZE) return epf_reject("file limit", 0, -EFBIG);
    if (file[4] != 1 || file[5] != 0 || epf_read_u32(file + 20) != EPF_HEADER_SIZE)
        return epf_reject("unsupported version or header size");
    if (file[6] != 8 || file[7] != 1 || epf_read_u16(file + 10) != EPF_MACHINE_X86_64)
        return epf_reject("target must be little-endian x86-64");
    if (!epf_zero(file + 108, 20)) return epf_reject("header reserved fields");

    *header = {epf_read_u16(file + 8), epf_read_u16(file + 10), epf_read_u16(file + 12),
               epf_read_u16(file + 14), epf_read_u32(file + 16), epf_read_u64(file + 24),
               epf_read_u64(file + 32), epf_read_u64(file + 40), epf_read_u64(file + 48),
               epf_read_u64(file + 56), epf_read_u64(file + 64), epf_read_u64(file + 72),
               epf_read_u32(file + 80), epf_read_u32(file + 84), epf_read_u32(file + 88),
               epf_read_u16(file + 92), epf_read_u16(file + 94), epf_read_u64(file + 96),
               epf_read_u32(file + 104)};
    if (header->file_size != size) return epf_reject("file length mismatch");
    if (header->os_abi != EPF_OS_ABI_XJ380 || header->arch_flags ||
        (header->abi_version != EPF_XJ380_ABI_VERSION && header->abi_version != EPF_XJ380_TLS_ABI_VERSION &&
         header->abi_version != EPF_XJ380_CXX_TLS_ABI_VERSION))
        return epf_reject("unsupported native ABI", 0, -EOPNOTSUPP);
    if (header->type != EPF_EXEC)
        return epf_reject("only EXEC can run", 0,
                          header->type == EPF_REL || header->type == EPF_SHARED ? -EOPNOTSUPP : -ENOEXEC);
    if ((header->required_features & EPF_FEATURE_TLS) && header->abi_version < EPF_XJ380_TLS_ABI_VERSION)
        return epf_reject("TLS requires native ABI 2", 0, -EOPNOTSUPP);
    if (header->flags & ~EPF_F_PIE) return epf_reject("unknown image flags");
    if (header->required_features & ~(EPF_FEATURE_RUNTIME_RELOC | EPF_FEATURE_DYNAMIC_LINK |
                                      EPF_FEATURE_RELRO | EPF_FEATURE_TLS))
        return epf_reject("required runtime capability", 0, -EOPNOTSUPP);
    if (header->segment_entry_size != EPF_SEGMENT_SIZE || header->section_entry_size != EPF_SECTION_SIZE ||
        header->segment_count == 0 || header->segment_count > EPF_MAX_SEGMENTS ||
        header->section_count > EPF_MAX_SECTIONS ||
        !epf_table(header->segment_table_offset, header->segment_count, header->segment_entry_size, size) ||
        !epf_table(header->section_table_offset, header->section_count, header->section_entry_size, size) ||
        (header->shstr_index != 0 && header->shstr_index >= header->section_count))
        return epf_reject("invalid table range, stride or count");
    if (!epf_power_of_two(header->map_align) || header->map_align < PAGE_SIZE ||
        header->map_align % PAGE_SIZE || header->image_size == 0 || header->image_size > EPF_MAX_IMAGE_SIZE ||
        header->image_size % header->map_align ||
        ((header->flags & EPF_F_PIE) ? header->image_base != 0 : header->image_base < PAGE_SIZE) ||
        header->image_base % header->map_align || check_user_overflow(header->image_base, header->image_size) ||
        header->image_base >= USER_MMAP_START || header->image_size > USER_MMAP_START - header->image_base)
        return epf_reject("image address range or alignment");
    if (header->entry_rva == EPF_NO_ENTRY || header->entry_rva >= header->image_size)
        return epf_reject("missing or invalid entry");
    if (!(header->flags & EPF_F_PIE) &&
        (epf_overlap(header->image_base, header->image_size, XJ380_PRIVATE_MESSAGE_REVERT_ADDRESS, PAGE_SIZE) ||
         epf_overlap(header->image_base, header->image_size, EHDR_START_ADDR, PAGE_SIZE)))
        return epf_reject("reserved native address range");
    return 0;
}

static int epf_segments(const uint8_t *file, const EpfHeader &header)
{
    uint64_t last_end = 0;
    bool have_load = false, have_entry = false;
    for (uint32_t i = 0; i < header.segment_count; ++i)
    {
        EpfSegment segment = epf_segment(file, header, i);
        const uint8_t *raw = file + header.segment_table_offset + (uint64_t)i * header.segment_entry_size;
        if (segment.type == EPF_SEG_NULL)
        {
            if (!epf_zero(raw, EPF_SEGMENT_SIZE)) return epf_reject("nonzero NULL segment", i);
            continue;
        }
        if (!epf_zero(raw + 48, 16) || (segment.flags & ~(EPF_PF_R | EPF_PF_W | EPF_PF_X | EPF_PF_REQUIRED)) ||
            !epf_range(segment.file_offset, segment.file_size, header.file_size))
            return epf_reject("segment flags, reserved fields or file range", i);
        if (segment.type != EPF_SEG_LOAD)
        {
            if (segment.flags & (EPF_PF_R | EPF_PF_W | EPF_PF_X)) return epf_reject("non-LOAD permissions", i);
            if (segment.type == EPF_SEG_DYNAMIC)
            {
                if (!(segment.flags & EPF_PF_REQUIRED)) return epf_reject("DYNAMIC must be required", i);
                continue;
            }
            if (segment.type == EPF_SEG_INTERP || segment.type == EPF_SEG_RELRO || segment.type == EPF_SEG_TLS)
            {
                if (!(segment.flags & EPF_PF_REQUIRED)) return epf_reject("runtime segment must be required", i);
                continue;
            }
            if (segment.flags & EPF_PF_REQUIRED)
                return epf_reject("required metadata segment is not supported", i, -EOPNOTSUPP);
            continue;
        }
        if (!(segment.flags & EPF_PF_REQUIRED) || !(segment.flags & EPF_PF_R) ||
            (segment.flags & (EPF_PF_W | EPF_PF_X)) == (EPF_PF_W | EPF_PF_X) || segment.mem_size == 0 ||
            segment.file_size > segment.mem_size || !epf_range(segment.rva, segment.mem_size, header.image_size) ||
            !epf_power_of_two(segment.align) || segment.align < header.map_align ||
            segment.rva % segment.align || segment.file_offset % segment.align ||
            header.image_base % segment.align)
            return epf_reject("LOAD range, permissions or alignment", i);

        // image_size is aligned and bounds this sum, so rounding cannot wrap.
        uint64_t end = (segment.rva + segment.mem_size + header.map_align - 1) & ~(header.map_align - 1);
        if (have_load && segment.rva < last_end) return epf_reject("unsorted or overlapping LOAD pages", i);
        for (uint32_t j = 0; j < i; ++j)
        {
            EpfSegment previous = epf_segment(file, header, j);
            if (previous.type == EPF_SEG_LOAD && epf_overlap(segment.file_offset, segment.file_size,
                                                          previous.file_offset, previous.file_size))
                return epf_reject("overlapping LOAD file ranges", i);
        }
        if ((segment.flags & EPF_PF_X) && header.entry_rva >= segment.rva &&
            header.entry_rva - segment.rva < segment.file_size)
            have_entry = true;
        have_load = true;
        last_end = end;
    }
    if (!have_load || !have_entry || last_end != header.image_size)
        return epf_reject("entry must be file-backed executable code; image_size must be exact");
    return 0;
}

static int epf_tls(const uint8_t *file, const EpfHeader &header);

int epf_get_interpreter(const uint8_t *file, uint64_t size)
{
    EpfHeader header = {};
    int error = epf_header(file, size, &header);
    if (!error) error = epf_segments(file, header);
    if (!error) error = epf_tls(file, header);
    if (error) return error;
    bool present = false;
    for (uint32_t i = 0; i < header.segment_count; ++i)
    {
        EpfSegment segment = epf_segment(file, header, i);
        if (segment.type != EPF_SEG_INTERP) continue;
        if (present || segment.file_size != sizeof(EPF_INTERPRETER_PATH) || segment.mem_size != segment.file_size ||
            segment.rva != 0 || segment.align != 1 ||
            memcmp(file + segment.file_offset, EPF_INTERPRETER_PATH, sizeof(EPF_INTERPRETER_PATH)))
            return epf_reject("invalid native INTERP path or descriptor", i);
        present = true;
    }
    if (present != ((header.required_features & EPF_FEATURE_DYNAMIC_LINK) != 0))
        return epf_reject("DYNAMIC_LINK requires the native INTERP", 0, -EOPNOTSUPP);
    if ((header.required_features & EPF_FEATURE_TLS) && !present)
        return epf_reject("TLS requires the native INTERP", 0, -EOPNOTSUPP);
    return present ? 1 : 0;
}

static int epf_relro(const uint8_t *file, const EpfHeader &header)
{
    bool present = false;
    for (uint32_t i = 0; i < header.segment_count; ++i)
    {
        EpfSegment segment = epf_segment(file, header, i);
        if (segment.type != EPF_SEG_RELRO) continue;
        present = true;
        if (segment.file_offset || segment.file_size || segment.rva % PAGE_SIZE || segment.mem_size == 0 ||
            segment.mem_size % PAGE_SIZE || segment.align != PAGE_SIZE ||
            !epf_range(segment.rva, segment.mem_size, header.image_size)) return epf_reject("invalid RELRO", i);
        uint64_t cursor = segment.rva;
        for (uint32_t j = 0; j < header.segment_count; ++j)
        {
            EpfSegment load = epf_segment(file, header, j);
            uint64_t end = load.rva + ((load.mem_size + PAGE_SIZE - 1) & PAGE_MASK);
            if (load.type == EPF_SEG_LOAD && (load.flags & EPF_PF_W) && !(load.flags & EPF_PF_X) &&
                cursor >= load.rva && cursor < end) cursor = min(segment.rva + segment.mem_size, end);
        }
        if (cursor != segment.rva + segment.mem_size) return epf_reject("RELRO outside writable LOAD pages", i);
        for (uint32_t j = 0; j < i; ++j)
        {
            EpfSegment other = epf_segment(file, header, j);
            if (other.type == EPF_SEG_RELRO && epf_overlap(segment.rva, segment.mem_size, other.rva, other.mem_size))
                return epf_reject("overlapping RELRO ranges", i);
        }
    }
    if (present != ((header.required_features & EPF_FEATURE_RELRO) != 0))
        return epf_reject("RELRO capability declaration mismatch");
    return 0;
}

// Runtime metadata is resolved from initialized readable LOAD bytes only.
// The kernel file buffer remains immutable throughout validation and relocation.
static const uint8_t *epf_initialized(const uint8_t *file, const EpfHeader &header, uint64_t rva, uint64_t size)
{
    for (uint32_t i = 0; i < header.segment_count; ++i)
    {
        EpfSegment segment = epf_segment(file, header, i);
        if (segment.type == EPF_SEG_LOAD && (segment.flags & EPF_PF_R) && rva >= segment.rva &&
            epf_range(rva - segment.rva, size, segment.file_size))
            return file + segment.file_offset + (rva - segment.rva);
    }
    return nullptr;
}

static int epf_tls(const uint8_t *file, const EpfHeader &header)
{
    bool present = false;
    for (uint32_t i = 0; i < header.segment_count; ++i)
    {
        EpfSegment segment = epf_segment(file, header, i);
        if (segment.type != EPF_SEG_TLS) continue;
        if (present || !(header.required_features & EPF_FEATURE_TLS) || !segment.mem_size ||
            segment.mem_size > EPF_MAX_TLS_MODULE || segment.file_size > segment.mem_size ||
            !epf_power_of_two(segment.align) || segment.align > EPF_MAX_TLS_ALIGN)
            return epf_reject("invalid TLS template, limit or declaration", i);
        present = true;
        if (!segment.file_size)
        {
            if (segment.file_offset || segment.rva) return epf_reject("zero-file TLS descriptor", i);
        }
        else if (epf_initialized(file, header, segment.rva, segment.file_size) != file + segment.file_offset)
            return epf_reject("TLS must name initialized readable LOAD bytes", i);
        // mem_size is per-thread storage, not another ordinary LOAD range.
        // In particular TLS BSS must not overwrite adjacent image data.
    }
    return 0;
}

struct EpfRelocation
{
    uint64_t rva, addend, value;
    uint32_t width;
};

struct EpfRuntime
{
    EpfSegment directory = {};
    EpfRelocation *writes = nullptr;
    uint32_t count = 0;
    uint64_t min_base = 0, max_base = ~0ULL;

    ~EpfRuntime()
    {
        free(writes);
    }
};

static void epf_relocation_sift(EpfRelocation *writes, uint32_t count, uint32_t root)
{
    while (root < count / 2)
    {
        uint32_t child = root * 2 + 1;
        if (child + 1 < count && writes[child].rva < writes[child + 1].rva) ++child;
        if (writes[root].rva >= writes[child].rva) break;
        EpfRelocation saved = writes[root];
        writes[root] = writes[child];
        writes[child] = saved;
        root = child;
    }
}

static void epf_sort_relocations(EpfRuntime &runtime)
{
    // Bounded heapsort avoids quadratic validation on untrusted input order.
    for (uint32_t i = runtime.count / 2; i != 0; --i)
    {
        epf_relocation_sift(runtime.writes, runtime.count, i - 1);
    }
    for (uint32_t end = runtime.count; end > 1; --end)
    {
        EpfRelocation saved = runtime.writes[0];
        runtime.writes[0] = runtime.writes[end - 1];
        runtime.writes[end - 1] = saved;
        epf_relocation_sift(runtime.writes, end - 1, 0);
    }
}

static int epf_runtime(const uint8_t *file, const EpfHeader &header, EpfRuntime &runtime, bool interpreted = false)
{
    bool present = false;
    for (uint32_t i = 0; i < header.segment_count; ++i)
    {
        EpfSegment segment = epf_segment(file, header, i);
        if (segment.type != EPF_SEG_DYNAMIC) continue;
        if (present) return epf_reject("duplicate DYNAMIC", i);
        present = true;
        runtime.directory = segment;
    }
    if (!present)
        return (header.required_features & (EPF_FEATURE_RUNTIME_RELOC | EPF_FEATURE_DYNAMIC_LINK))
                   ? epf_reject("missing runtime directory") : 0;

    const EpfSegment &directory = runtime.directory;
    const uint8_t *entries = epf_initialized(file, header, directory.rva, directory.file_size);
    if (directory.file_size == 0 || directory.file_size % EPF_DYNAMIC_SIZE ||
        directory.file_size / EPF_DYNAMIC_SIZE > EPF_MAX_DYNAMIC_ENTRIES || (directory.rva & 7) ||
        (directory.file_offset & 7) || entries == nullptr || entries != file + directory.file_offset ||
        !epf_range(directory.rva, directory.mem_size, header.image_size))
        return epf_reject("DYNAMIC must be initialized readable LOAD content");

    uint64_t rela_rva = 0, rela_count = 0, rela_stride = 0;
    uint32_t seen = 0;
    bool terminated = false;
    for (uint64_t offset = 0; offset < directory.file_size; offset += EPF_DYNAMIC_SIZE)
    {
        const uint8_t *entry = entries + offset;
        uint32_t tag = epf_read_u32(entry), flags = epf_read_u32(entry + 4);
        uint64_t value = epf_read_u64(entry + 8);
        if (flags & ~EPF_DT_REQUIRED) return epf_reject("unknown dynamic entry flags");
        if (tag == EPF_DT_NULL)
        {
            if (!epf_zero(entry, directory.file_size - offset)) return epf_reject("invalid dynamic terminator");
            terminated = true;
            break;
        }
        if (interpreted)
        {
            if (tag > EPF_DT_FINI_COUNT && (flags & EPF_DT_REQUIRED))
                return epf_reject("unknown required dynamic tag", tag, -EOPNOTSUPP);
            continue;
        }
        if (tag >= EPF_DT_RELA_RVA && tag <= EPF_DT_RELAENT)
        {
            uint32_t bit = 1u << (tag - EPF_DT_RELA_RVA);
            if (seen & bit) return epf_reject("duplicate relocation directory tag", tag);
            seen |= bit;
            if (tag == EPF_DT_RELA_RVA) rela_rva = value;
            else if (tag == EPF_DT_RELACNT) rela_count = value;
            else rela_stride = value;
        }
        else if (tag <= 14 || (flags & EPF_DT_REQUIRED))
        {
            // Symbol/string tables, dependencies and INIT/FINI require the
            // later user-linker ABI. Never silently omit known runtime work.
            return epf_reject("unsupported dynamic binding or lifecycle tag", tag, -EOPNOTSUPP);
        }
    }
    if (interpreted) return terminated ? 0 : epf_reject("unterminated dynamic directory");
    if (!terminated || (seen != 0 && seen != 7) ||
        ((header.required_features & EPF_FEATURE_RUNTIME_RELOC) && seen != 7))
        return epf_reject("incomplete relocation directory");
    if (!seen) return 0;
    if (rela_stride != EPF_RELA_SIZE || rela_count > EPF_MAX_RELOCATIONS || (rela_rva & 7) ||
        (rela_count && !(header.required_features & EPF_FEATURE_RUNTIME_RELOC)))
        return epf_reject("relocation stride, count or capability declaration");
    uint64_t rela_size = rela_count * EPF_RELA_SIZE;
    const uint8_t *relas = epf_initialized(file, header, rela_rva, rela_size);
    if (!relas) return epf_reject("relocation table must be initialized readable LOAD content");
    if (epf_overlap(rela_rva, rela_size, directory.rva, directory.file_size))
        return epf_reject("runtime tables overlap");
    if (!rela_count) return 0;
    runtime.writes = (EpfRelocation *)calloc(rela_count, sizeof(EpfRelocation));
    if (!runtime.writes) return -ENOMEM;

    for (uint32_t i = 0; i < rela_count; ++i)
    {
        const uint8_t *raw = relas + (uint64_t)i * EPF_RELA_SIZE;
        uint64_t rva = epf_read_u64(raw), addend = epf_read_u64(raw + 8);
        uint32_t symbol = epf_read_u32(raw + 16), type = epf_read_u32(raw + 20);
        if (epf_read_u64(raw + 24)) return epf_reject("relocation reserved field", i);
        if (type == EPF_RELOC_NONE) continue;
        if (type != EPF_RELOC_RELATIVE32 && type != EPF_RELOC_RELATIVE64)
            return epf_reject("unsupported runtime relocation type", i, -EOPNOTSUPP);
        uint32_t width = type == EPF_RELOC_RELATIVE32 ? 4 : 8;
        if (symbol || !epf_range(rva, width, header.image_size)) return epf_reject("relocation symbol or range", i);
        bool writable = false;
        uint64_t checked = rva;
        for (uint32_t j = 0; j < header.segment_count; ++j)
        {
            EpfSegment segment = epf_segment(file, header, j);
            if (segment.type == EPF_SEG_LOAD && (segment.flags & EPF_PF_W) && !(segment.flags & EPF_PF_X) &&
                checked >= segment.rva && checked - segment.rva < segment.mem_size)
            {
                checked += min(rva + width - checked, segment.mem_size - (checked - segment.rva));
                if (checked == rva + width)
                {
                    writable = true;
                    break;
                }
            }
        }
        if (!writable || epf_overlap(rva, width, directory.rva, directory.file_size) ||
            epf_overlap(rva, width, rela_rva, rela_size)) return epf_reject("unsafe relocation write", i);

        uint64_t limit = width == 4 ? 0xffffffffULL : ~0ULL;
        if (addend & (1ULL << 63))
        {
            uint64_t magnitude = ~addend + 1;
            runtime.min_base = max(runtime.min_base, magnitude);
            if (width == 4) runtime.max_base = min(runtime.max_base, magnitude + limit);
        }
        else
        {
            if (addend > limit) return epf_reject("relative relocation overflow", i);
            runtime.max_base = min(runtime.max_base, limit - addend);
        }
        runtime.writes[runtime.count++] = {rva, addend, 0, width};
    }
    epf_sort_relocations(runtime);
    for (uint32_t i = 1; i < runtime.count; ++i)
    {
        if (runtime.writes[i].rva < runtime.writes[i - 1].rva + runtime.writes[i - 1].width)
            return epf_reject("overlapping relocation writes", i);
    }
    return 0;
}

static int epf_sections(const uint8_t *file, const EpfHeader &header)
{
    if (header.section_count == 0) return 0;
    const uint8_t *table = file + header.section_table_offset;
    if (!epf_zero(table, EPF_SECTION_SIZE)) return epf_reject("nonzero NULL section");
    uint64_t names_offset = 0, names_size = 0;
    if (header.shstr_index)
    {
        const uint8_t *names = table + (uint64_t)header.shstr_index * header.section_entry_size;
        names_offset = epf_read_u64(names + 24);
        names_size = epf_read_u64(names + 32);
        if (epf_read_u32(names + 4) != EPF_SHT_STRTAB || !names_size ||
            !epf_range(names_offset, names_size, header.file_size) || file[names_offset + names_size - 1] != 0)
            return epf_reject("invalid section name strings");
    }
    // Sections are optional metadata. Check their structure and agreement with
    // LOADs, but never use names, symbols or sections to build the image.
    for (uint32_t i = 1; i < header.section_count; ++i)
    {
        const uint8_t *p = table + (uint64_t)i * header.section_entry_size;
        uint32_t name = epf_read_u32(p), type = epf_read_u32(p + 4);
        uint64_t flags = epf_read_u64(p + 8), rva = epf_read_u64(p + 16);
        uint64_t offset = epf_read_u64(p + 24), size = epf_read_u64(p + 32);
        uint32_t link = epf_read_u32(p + 40), info = epf_read_u32(p + 44);
        uint64_t align = epf_read_u64(p + 48), stride = epf_read_u64(p + 56);
        if (type == EPF_SHT_NULL)
        {
            if (!epf_zero(p, EPF_SECTION_SIZE)) return epf_reject("nonzero NULL section", i);
            continue;
        }
        if ((names_size ? name >= names_size : name != 0) ||
            (flags & ~(EPF_SHF_ALLOC | EPF_SHF_WRITE | EPF_SHF_EXEC | EPF_SHF_TLS | EPF_SHF_REQUIRED)) ||
            (size != 0 && !epf_power_of_two(align)) ||
            (type == EPF_SHT_NOBITS ? offset != 0 : !epf_range(offset, size, header.file_size)))
            return epf_reject("section range, flags, name or alignment", i);
        if ((flags & EPF_SHF_TLS) || ((flags & EPF_SHF_REQUIRED) &&
            (type > EPF_SHT_DYNAMIC || type == EPF_SHT_DYNAMIC || type == EPF_SHT_RELA || type == EPF_SHT_DYNSYM)))
            return epf_reject("unsupported required section semantics", i, -EOPNOTSUPP);
        if (type <= EPF_SHT_DYNAMIC)
        {
            if (type == EPF_SHT_SYMTAB || type == EPF_SHT_DYNSYM || type == EPF_SHT_RELA)
            {
                if (stride != 32 || size % stride || link == 0 || link >= header.section_count ||
                    (type == EPF_SHT_RELA ? info == 0 || info >= header.section_count : info != 0))
                    return epf_reject("symbol or relocation section metadata", i);
                uint32_t linked_type = epf_read_u32(table + (uint64_t)link * header.section_entry_size + 4);
                if (type == EPF_SHT_RELA ? linked_type != EPF_SHT_SYMTAB && linked_type != EPF_SHT_DYNSYM
                                         : linked_type != EPF_SHT_STRTAB)
                    return epf_reject("invalid linked section type", i);
            }
            else if (link || info || (type == EPF_SHT_DYNAMIC ? stride != 16 || size % 16 : stride != 0))
                return epf_reject("section link, info or stride", i);
        }
        if (!(flags & EPF_SHF_ALLOC))
        {
            if (rva != 0) return epf_reject("non-ALLOC section RVA", i);
            continue;
        }
        if (!epf_range(rva, size, header.image_size) || (size != 0 && rva % align))
            return epf_reject("ALLOC section range", i);
        bool covered = false;
        for (uint32_t j = 0; j < header.segment_count; ++j)
        {
            EpfSegment segment = epf_segment(file, header, j);
            if (segment.type != EPF_SEG_LOAD || rva < segment.rva ||
                !epf_range(rva - segment.rva, size, segment.mem_size)) continue;
            uint64_t delta = rva - segment.rva;
            if (((flags & EPF_SHF_WRITE) != 0) != ((segment.flags & EPF_PF_W) != 0) ||
                ((flags & EPF_SHF_EXEC) != 0) != ((segment.flags & EPF_PF_X) != 0)) continue;
            covered = type == EPF_SHT_NOBITS ? delta >= segment.file_size
                                            : epf_range(delta, size, segment.file_size) &&
                                                  offset == segment.file_offset + delta;
            if (covered) break;
        }
        if (!covered) return epf_reject("ALLOC section disagrees with LOAD", i);
    }
    return 0;
}

struct EpfLoadTransaction
{
    pcb_t process;
    vma_t **ranges;
    uint32_t count = 0, inserted = 0;
    uint64_t mapped_end = 0;
    bool committed = false;

    ~EpfLoadTransaction()
    {
        if (!committed)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                vma_t *range = ranges[i];
                if ((range->vm_flags & VMA_READ) && range->vm_start < mapped_end)
                    unmap_page_range(process->pagedir, range->vm_start,
                                     min(range->vm_end, mapped_end) - range->vm_start);
                if (i < inserted) vma_remove(&process->vma_manager, range);
                vma_free(range);
            }
        }
        free(ranges);
    }
};

static int epf_add_vma(EpfLoadTransaction &transaction, uint64_t start, uint64_t end, uint64_t flags)
{
    if (start == end) return 0;
    vma_t *range = vma_alloc();
    if (range == nullptr) return -ENOMEM;
    range->vm_start = start;
    range->vm_end = end;
    range->vm_flags = flags;
    range->vm_type = VMA_TYPE_ANON;
    range->vm_name = strdup(transaction.process->name);
    transaction.ranges[transaction.count++] = range;
    if (range->vm_name == nullptr) return -ENOMEM;
    if (vma_insert(&transaction.process->vma_manager, range) != 0) return -EEXIST;
    ++transaction.inserted;
    return 0;
}

static bool epf_lazy_collision(pcb_t process, uint64_t start, uint64_t size)
{
    if (process->virt_queue == nullptr) return false;
    bool collision = false;
    // Capture only bounded metadata under the queue lock. Allocation, mapping
    // and diagnostics happen after unlocking, with the target private or pinned.
    spin_lock(&process->virt_queue->lock);
    queue_foreach(process->virt_queue, node)
    {
        mm_virtual_page_t *range = (mm_virtual_page_t *)node->data;
        if (range == nullptr) continue;
        if (range->count > (~0ULL - range->start) / PAGE_SIZE ||
            epf_overlap(start, size, range->start, range->count * PAGE_SIZE))
        {
            collision = true;
            break;
        }
    }
    spin_unlock(&process->virt_queue->lock);
    return collision;
}

static bool epf_image_available(pcb_t process, uint64_t base, uint64_t size)
{
    if (epf_overlap(base, size, XJ380_PRIVATE_MESSAGE_REVERT_ADDRESS, PAGE_SIZE) ||
        epf_overlap(base, size, EHDR_START_ADDR, PAGE_SIZE)) return false;
    if (vma_find_intersection(&process->vma_manager, base, base + size) || epf_lazy_collision(process, base, size))
        return false;
    for (uint64_t rva = 0; rva < size; rva += PAGE_SIZE)
    {
        uint64_t flags = 0;
        if (page_table_get_flags(process->pagedir, base + rva, &flags)) return false;
    }
    return true;
}

static int epf_choose_base(const uint8_t *file, EpfHeader &header, EpfRuntime &runtime, pcb_t process)
{
    if (header.flags & EPF_F_PIE)
    {
        // Keep this ABI's PIE window below 4 GiB, supporting both relative
        // integer widths. Bounds also account for every signed addend before
        // randomization, so valid placement cannot fail by accidental overflow.
        uint64_t lower = max((uint64_t)0x10000000, runtime.min_base);
        uint64_t upper = min((uint64_t)0x100000000 - header.image_size, runtime.max_base);
        upper = min(upper, (uint64_t)USER_MMAP_START - header.image_size);
        uint64_t align = header.map_align;
        for (uint32_t i = 0; i < header.segment_count; ++i)
        {
            EpfSegment segment = epf_segment(file, header, i);
            if (segment.type == EPF_SEG_LOAD) align = max(align, segment.align);
        }
        if (lower > upper || align > upper) return epf_reject("no representable PIE base");
        uint64_t first = (lower + align - 1) & ~(align - 1), last = upper & ~(align - 1);
        if (first > last) return epf_reject("no aligned PIE base");
        uint64_t slots = (last - first) / align + 1, random = 0;
        if (!get_hardware_random_bytes(&random, sizeof(random)))
            return epf_reject("PIE requires checked hardware entropy", 0, -EOPNOTSUPP);
        uint64_t index = random % slots;
        bool placed = false;
        for (uint64_t attempt = 0; attempt < min(slots, (uint64_t)64); ++attempt)
        {
            uint64_t base = first + ((index + attempt) % slots) * align;
            if (epf_image_available(process, base, header.image_size))
            {
                header.image_base = base;
                placed = true;
                break;
            }
        }
        if (!placed) return epf_reject("PIE candidate reservations occupied", 0, -EEXIST);
    }
    else if (!epf_image_available(process, header.image_base, header.image_size))
        return epf_reject("image reservation collision", 0, -EEXIST);
    if (header.image_base < runtime.min_base || header.image_base > runtime.max_base)
        return epf_reject("relative relocation overflow");
    for (uint32_t i = 0; i < runtime.count; ++i)
    {
        EpfRelocation &relocation = runtime.writes[i];
        relocation.value = (relocation.addend & (1ULL << 63)) ? header.image_base - (~relocation.addend + 1)
                                                            : header.image_base + relocation.addend;
    }
    return 0;
}

static bool epf_apply_relocations(page_directory_t *directory, uint64_t base, const EpfRuntime &runtime)
{
    for (uint32_t i = 0; i < runtime.count; ++i)
    {
        const EpfRelocation &relocation = runtime.writes[i];
        // A scalar may straddle pages. Resolve each byte through the explicit
        // target directory, never assuming contiguous physical backing.
        for (uint32_t byte = 0; byte < relocation.width; ++byte)
        {
            uint64_t physical = translate_address(directory, base + relocation.rva + byte);
            if (!physical) return false;
            uint8_t *destination = (uint8_t *)phys_to_virt(physical);
            if (!destination) return false;
            *destination = (uint8_t)(relocation.value >> (8 * byte));
        }
    }
    return true;
}

static int epf_map_image(const uint8_t *file, const EpfHeader &header, EpfRuntime &runtime,
                         EpfLoadTransaction &transaction, bool interpreted)
{
    pcb_t process = transaction.process;
    int error = 0;
    uint64_t cursor = header.image_base;
    // Reserve gaps with permissionless VMAs, so stack/mmap allocation cannot
    // later occupy a hole. Only LOAD pages receive physical backing.
    for (uint32_t i = 0; i < header.segment_count; ++i)
    {
        EpfSegment segment = epf_segment(file, header, i);
        if (segment.type != EPF_SEG_LOAD) continue;
        uint64_t start = header.image_base + segment.rva;
        uint64_t end = start + ((segment.mem_size + PAGE_SIZE - 1) & PAGE_MASK);
        error = epf_add_vma(transaction, cursor, start, 0);
        if (error) return error;
        for (uint64_t part = start; part < end;)
        {
            uint64_t next = end;
            bool sealed = false;
            if (!interpreted)
            {
                for (uint32_t j = 0; j < header.segment_count; ++j)
                {
                    EpfSegment relro = epf_segment(file, header, j);
                    if (relro.type != EPF_SEG_RELRO) continue;
                    uint64_t first = header.image_base + relro.rva, last = first + relro.mem_size;
                    if (first > part) next = min(next, first);
                    if (part >= first && part < last) { next = min(next, last); sealed = true; }
                }
            }
            uint64_t flags = VMA_READ | ((segment.flags & EPF_PF_W) && !sealed ? VMA_WRITE : 0) |
                             ((segment.flags & EPF_PF_X) ? VMA_EXEC : 0);
            error = epf_add_vma(transaction, part, next, flags);
            if (error) return error;
            part = next;
        }
        cursor = end;
    }
    error = epf_add_vma(transaction, cursor, header.image_base + header.image_size, 0);
    if (error) return error;

    for (uint32_t i = 0; i < header.segment_count; ++i)
    {
        EpfSegment segment = epf_segment(file, header, i);
        if (segment.type != EPF_SEG_LOAD) continue;
        for (uint64_t offset = 0; offset < segment.mem_size; offset += PAGE_SIZE)
        {
            uint64_t physical = alloc_frames(1);
            if (!physical) return -ENOMEM;
            void *destination = phys_to_virt(physical);
            if (!destination)
            {
                free_frames(physical, 1);
                return -ENOMEM;
            }
            memset(destination, 0, PAGE_SIZE);
            if (offset < segment.file_size)
                memcpy(destination, file + segment.file_offset + offset, min(PAGE_SIZE, segment.file_size - offset));
            uint64_t address = header.image_base + segment.rva + offset;
            if (!page_map_new_user_page(process->pagedir, address, physical,
                                        PTE_PRESENT | PTE_USER | PTE_WRITEABLE | PTE_NO_EXECUTE | PTE_FRAME_ALLOCATED))
            {
                free_frames(physical, 1);
                return -ENOMEM;
            }
            transaction.mapped_end = address + PAGE_SIZE;
        }
    }
    if (!interpreted && !epf_apply_relocations(process->pagedir, header.image_base, runtime)) return -ENOMEM;
    for (uint32_t i = 0; i < header.segment_count; ++i)
    {
        EpfSegment segment = epf_segment(file, header, i);
        if (segment.type != EPF_SEG_LOAD) continue;
        uint64_t flags = PTE_PRESENT | PTE_USER | ((segment.flags & EPF_PF_W) ? PTE_WRITEABLE : 0) |
                         ((segment.flags & EPF_PF_X) ? 0 : PTE_NO_EXECUTE);
        for (uint64_t offset = 0; offset < segment.mem_size; offset += PAGE_SIZE)
        {
            uint64_t page_flags = flags;
            if (!interpreted)
            {
                for (uint32_t j = 0; j < header.segment_count; ++j)
                {
                    EpfSegment relro = epf_segment(file, header, j);
                    if (relro.type == EPF_SEG_RELRO && segment.rva + offset >= relro.rva &&
                        segment.rva + offset - relro.rva < relro.mem_size) page_flags &= ~PTE_WRITEABLE;
                }
            }
            if (!page_table_update_flags(process->pagedir, header.image_base + segment.rva + offset, page_flags))
                return -ENOMEM;
        }
    }

    return 0;
}

static int epf_map_handoff(pcb_t process, const uint8_t *file, const EpfHeader &header,
                           EpfLoadTransaction &transaction)
{
    uint64_t size = (EPF_HANDOFF_SIZE + header.segment_count * EPF_SEGMENT_SIZE + PAGE_SIZE - 1) & PAGE_MASK;
    uint64_t base = EPF_HANDOFF_ADDRESS;
    if (!epf_image_available(process, base, size)) return -EEXIST;
    int error = epf_add_vma(transaction, base, base + size, VMA_READ);
    if (error) return error;
    uint64_t values[8] = {EPF_HANDOFF_MAGIC, 1, header.image_base, header.image_size,
                          header.image_base + header.entry_rva, header.segment_count,
                          header.required_features, header.map_align};
    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE)
    {
        uint64_t physical = alloc_frames(1);
        if (!physical) return -ENOMEM;
        uint8_t *destination = (uint8_t *)phys_to_virt(physical);
        if (!destination) { free_frames(physical, 1); return -ENOMEM; }
        memset(destination, 0, PAGE_SIZE);
        for (uint64_t i = 0; i < PAGE_SIZE && offset + i < EPF_HANDOFF_SIZE + header.segment_count * 64; ++i)
        {
            uint64_t at = offset + i;
            destination[i] = at < EPF_HANDOFF_SIZE ? (uint8_t)(values[at / 8] >> (8 * (at % 8)))
                : file[header.segment_table_offset + at - EPF_HANDOFF_SIZE];
        }
        if (!page_map_new_user_page(process->pagedir, base + offset, physical,
                                    PTE_PRESENT | PTE_USER | PTE_NO_EXECUTE | PTE_FRAME_ALLOCATED))
        { free_frames(physical, 1); return -ENOMEM; }
        transaction.mapped_end = base + offset + PAGE_SIZE;
    }
    return 0;
}

uint64_t load_user_epf_image(const uint8_t *file, uint64_t size, pcb_t process,
                             const uint8_t *interpreter, uint64_t interpreter_size)
{
    if (process == nullptr || process->pagedir == nullptr) return (uint64_t)-EINVAL;
    EpfHeader header = {}, interp_header = {};
    int interpreted = epf_get_interpreter(file, size);
    if (interpreted < 0) return (uint64_t)interpreted;
    if ((interpreter != nullptr) != (interpreted != 0)) return (uint64_t)-ENOEXEC;
    int error = epf_header(file, size, &header);
    if (!error) error = epf_sections(file, header);
    if (!error) error = epf_relro(file, header);
    EpfRuntime runtime, interp_runtime;
    if (!error) error = epf_runtime(file, header, runtime, interpreted != 0);
    if (!error) error = epf_choose_base(file, header, runtime, process);
    if (error) return (uint64_t)error;

    if (interpreted)
    {
        if (epf_get_interpreter(interpreter, interpreter_size) != 0) return (uint64_t)-ENOEXEC;
        error = epf_header(interpreter, interpreter_size, &interp_header);
        // A stale interpreter can understand TLS yet omit ABI 3's main-thread
        // destructor hook, even when the program embeds its own C++ runtime.
        if (!error && interp_header.abi_version < header.abi_version)
            error = epf_reject("INTERP native ABI is older than main", 0, -EOPNOTSUPP);
        if (!error) error = epf_sections(interpreter, interp_header);
        if (!error) error = epf_relro(interpreter, interp_header);
        if (!error) error = epf_runtime(interpreter, interp_header, interp_runtime);
        if (error) return (uint64_t)error;
    }
    EpfLoadTransaction transaction{process, (vma_t **)calloc(4 * header.segment_count + 1, sizeof(vma_t *))};
    EpfLoadTransaction interp_transaction{process,
        (vma_t **)calloc(4 * interp_header.segment_count + 1, sizeof(vma_t *))};
    EpfLoadTransaction handoff_transaction{process, (vma_t **)calloc(1, sizeof(vma_t *))};
    if (!transaction.ranges || !interp_transaction.ranges || !handoff_transaction.ranges) return (uint64_t)-ENOMEM;
    error = epf_map_image(file, header, runtime, transaction, interpreted != 0);
    if (!error && interpreted)
    {
        error = epf_choose_base(interpreter, interp_header, interp_runtime, process);
        if (!error) error = epf_map_image(interpreter, interp_header, interp_runtime, interp_transaction, false);
        if (!error) error = epf_map_handoff(process, file, header, handoff_transaction);
    }
    if (error) return (uint64_t)error;

    // Publish both images and the immutable handoff only after every step succeeds.
    process->linux_abi = false;
    process->load_start = header.image_base;
    process->aux_phdr = interpreted ? EPF_HANDOFF_ADDRESS : 0;
    process->aux_phent = interpreted ? EPF_SEGMENT_SIZE : 0;
    process->aux_phnum = interpreted ? header.segment_count : 0;
    process->aux_base = interpreted ? interp_header.image_base : 0;
    process->aux_execfn = 0;
    process->aux_entry = header.image_base + header.entry_rva;
    process->elf_file = nullptr;
    process->elf_size = size;
    transaction.committed = interp_transaction.committed = handoff_transaction.committed = true;
    write_serial_fmt("EPF loaded %s base=0x%llx entry=0x%llx size=%llu interp=%d\n", process->name, header.image_base,
                     process->aux_entry, header.image_size, interpreted);
    return interpreted ? interp_header.image_base + interp_header.entry_rva : process->aux_entry;
}
