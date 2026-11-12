// SPDX-License-Identifier: Apache-2.0
// EPF eager linker: no libc, kernel exports, ELF symbol lookup or compiler TLS is used.
#include <stdint.h>
#include <stddef.h>
#include <epf_format.h>
#include "../rtld/platform.h"
#include "tls.h"

#define PAGE 4096ULL
#define UP(n) (((n) + PAGE - 1) & ~(PAGE - 1))
#define LIMIT (1ULL << 47)
#define MAX_STRINGS (4ULL << 20)
#define MAX_BYTES (256ULL << 20)
#ifndef EPF_TEST_LIBRARY_DIRECTORY
#define LIBRARY_DIRECTORY EPF_LIBRARY_DIRECTORY
#else
#define LIBRARY_DIRECTORY EPF_TEST_LIBRARY_DIRECTORY
#endif

typedef struct { uint64_t rva, size; } Range;
typedef struct { uint64_t target, value; unsigned width; } Write;
typedef struct
{
    uint64_t base, size, entry, features, align, tls_end;
    EpfSegment tls;
    EpfSegment segments[EPF_MAX_SEGMENTS];
    unsigned segment_count, dependency_count, needed_count;
    unsigned dependencies[EPF_MAX_NEEDED];
    uint64_t needed[EPF_MAX_NEEDED];
    char name[128];
    const uint8_t *file;
    uint64_t file_size;
    const uint8_t *strings, *symbols, *relas, *init, *fini;
    uint64_t string_size, symbol_count, rela_count, init_count, fini_count;
    Range metadata[7];
    unsigned metadata_count;
    Write *writes;
    unsigned write_count;
    uint32_t *buckets, *chains;
    unsigned bucket_count;
} Object;

static Object objects[EPF_MAX_MODULES];
static unsigned object_count, init_order[EPF_MAX_MODULES], init_count;
static uint64_t bytes_used, work_left = 100000000;
extern void rtld_enter(uint64_t entry, uint64_t *stack, void (*finalizer)(void)) __attribute__((noreturn));

static void fail(const char *reason) __attribute__((noreturn));

static void fail(const char *reason)
{
    rtld_write(2, "ld-epf: ");
    rtld_write(2, reason);
    rtld_write(2, "\n");
    rtld_exit(127);
}

static void work(void)
{
    if (!work_left--) fail("metadata work limit");
}

static int range(uint64_t start, uint64_t size, uint64_t limit)
{
    return start <= limit && size <= limit - start;
}

static int power(uint64_t value)
{
    return value && !(value & (value - 1));
}

static int overlap(uint64_t a, uint64_t size, uint64_t b, uint64_t length)
{
    return size && length && a < b + length && b < a + size;
}

static int zero(const uint8_t *data, uint64_t size)
{
    while (size--) { work(); if (*data++) return 0; }
    return 1;
}

static void copy(void *out, const void *in, uint64_t size)
{
    uint8_t *destination = out;
    const uint8_t *source = in;
    while (size--) *destination++ = *source++;
}

static uint64_t minimum(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static void protect(uint64_t address, uint64_t size, int permissions)
{
    if (size && rtld_syscall(10, (long)address, (long)size, permissions, 0, 0, 0)) fail("mprotect failed");
}

static uint8_t *allocate(uint64_t size)
{
    size = UP(size ? size : 1);
    if (size > MAX_BYTES - bytes_used) fail("memory limit");
    bytes_used += size;
    long address = rtld_syscall(9, 0, (long)size, 3, 0x22, -1, 0);
    if (rtld_failed(address) || !range((uint64_t)address, size, LIMIT)) fail("mmap failed");
    return (uint8_t *)address;
}

// The complete scalar must fit declared data, including adjacent LOADs, with
// no semantic gap. Rounded page padding is not a relocation destination.
static int covered(Object *object, uint64_t rva, uint64_t size, unsigned permissions, int initialized)
{
    if (!range(rva, size, object->size)) return 0;
    uint64_t cursor = rva, end = rva + size;
    for (unsigned i = 0; i < object->segment_count; ++i)
    {
        work();
        EpfSegment *segment = &object->segments[i];
        if (segment->type != EPF_SEG_LOAD || (segment->flags & permissions) != permissions) continue;
        uint64_t length = initialized ? segment->file_size : segment->mem_size;
        if (cursor >= segment->rva && cursor - segment->rva < length)
            cursor += minimum(end - cursor, length - (cursor - segment->rva));
        if (cursor == end) return 1;
    }
    return 0;
}

static const uint8_t *initialized(Object *object, uint64_t rva, uint64_t size)
{
    for (unsigned i = 0; i < object->segment_count; ++i)
    {
        EpfSegment *segment = &object->segments[i];
        if (segment->type == EPF_SEG_LOAD && rva >= segment->rva &&
            range(rva - segment->rva, size, segment->file_size))
            return object->file ? object->file + segment->file_offset + rva - segment->rva
                                : (const uint8_t *)(object->base + rva);
    }
    fail("runtime table outside initialized LOAD");
}

static const uint8_t *metadata(Object *object, uint64_t rva, uint64_t size)
{
    if (object->metadata_count == 7 || !range(rva, size, object->size)) fail("metadata range");
    for (unsigned i = 0; i < object->metadata_count; ++i)
        if (overlap(rva, size, object->metadata[i].rva, object->metadata[i].size)) fail("overlapping metadata tables");
    const uint8_t *source = initialized(object, rva, size);
    object->metadata[object->metadata_count++] = (Range){rva, size};
    uint8_t *result = allocate(size);
    copy(result, source, size);
    protect((uint64_t)result, UP(size ? size : 1), 1);
    return result;
}

static EpfSegment segment_decode(const uint8_t *raw)
{
    EpfSegment result = {epf_read_u32(raw), epf_read_u32(raw + 4), epf_read_u64(raw + 8),
                        epf_read_u64(raw + 16), epf_read_u64(raw + 24), epf_read_u64(raw + 32), epf_read_u64(raw + 40)};
    if (!zero(raw + 48, 16)) fail("segment reserved fields");
    return result;
}

static void segments(Object *object, int main)
{
    uint64_t last = 0, maximum_align = PAGE;
    unsigned directories = 0, interps = 0, relros = 0, tls_count = 0;
    for (unsigned i = 0; i < object->segment_count; ++i)
    {
        EpfSegment *segment = &object->segments[i];
        if (segment->flags & ~(EPF_PF_REQUIRED | EPF_PF_R | EPF_PF_W | EPF_PF_X)) fail("segment flags");
        if (!range(segment->file_offset, segment->file_size, object->file_size)) fail("segment file range");
        if (segment->type == EPF_SEG_NULL)
        {
            if (segment->flags || segment->file_offset || segment->rva || segment->file_size ||
                segment->mem_size || segment->align) fail("nonzero NULL segment");
            continue;
        }
        if (segment->type == EPF_SEG_LOAD)
        {
            if (!(segment->flags & EPF_PF_REQUIRED) || !(segment->flags & EPF_PF_R) ||
                (segment->flags & (EPF_PF_W | EPF_PF_X)) == (EPF_PF_W | EPF_PF_X) ||
                !segment->mem_size || segment->file_size > segment->mem_size ||
                !range(segment->rva, segment->mem_size, object->size) ||
                !power(segment->align) || segment->align < object->align ||
                segment->file_offset % segment->align || segment->rva % segment->align ||
                (main && object->base % segment->align) || segment->rva < last) fail("invalid LOAD");
            for (unsigned j = 0; j < i; ++j)
                if (object->segments[j].type == EPF_SEG_LOAD && overlap(segment->file_offset, segment->file_size,
                    object->segments[j].file_offset, object->segments[j].file_size))
                    fail("overlapping LOAD file bytes");
            last = (segment->rva + segment->mem_size + object->align - 1) & ~(object->align - 1);
            if (segment->align > maximum_align) maximum_align = segment->align;
            continue;
        }
        if (segment->flags & 7) fail("non-LOAD permissions");
        switch (segment->type)
        {
        case EPF_SEG_DYNAMIC:
            if (++directories != 1 || !(segment->flags & EPF_PF_REQUIRED) || !segment->file_size ||
                segment->file_size % 16 || segment->file_size / 16 > EPF_MAX_DYNAMIC_ENTRIES ||
                segment->file_offset % 8 || segment->rva % 8 || segment->mem_size != segment->file_size)
                fail("invalid DYNAMIC");
            break;
        case EPF_SEG_INTERP:
            if (!main || ++interps != 1 || !(segment->flags & EPF_PF_REQUIRED)) fail("nested or invalid INTERP");
            break;
        case EPF_SEG_TLS:
            if (++tls_count != 1 || !(segment->flags & EPF_PF_REQUIRED) ||
                !(object->features & EPF_FEATURE_TLS) || !segment->mem_size ||
                segment->mem_size > EPF_MAX_TLS_MODULE || segment->file_size > segment->mem_size ||
                !power(segment->align) || segment->align > EPF_MAX_TLS_ALIGN ||
                (!segment->file_size && (segment->file_offset || segment->rva))) fail("invalid TLS template");
            object->tls = *segment;
            break;
        case EPF_SEG_RELRO:
            ++relros;
            if (!(segment->flags & EPF_PF_REQUIRED) || segment->file_offset || segment->file_size ||
                !segment->mem_size || segment->rva % PAGE || segment->mem_size % PAGE || segment->align != PAGE ||
                !range(segment->rva, segment->mem_size, object->size)) fail("invalid RELRO");
            break;
        default:
            if (segment->flags & EPF_PF_REQUIRED) fail("unsupported required segment");
        }
    }
    if (last != object->size || directories != 1 || (main && interps != 1) || maximum_align > (2ULL << 20) ||
        (relros != 0) != ((object->features & EPF_FEATURE_RELRO) != 0)) fail("image layout or capabilities");
    if (object->tls.file_size)
    {
        int corresponding = 0;
        for (unsigned i = 0; i < object->segment_count; ++i)
        {
            EpfSegment *load = &object->segments[i];
            if (load->type == EPF_SEG_LOAD && object->tls.rva >= load->rva &&
                range(object->tls.rva - load->rva, object->tls.file_size, load->file_size) &&
                object->tls.file_offset == load->file_offset + object->tls.rva - load->rva) corresponding = 1;
        }
        if (!corresponding) fail("TLS file/RVA mismatch");
    }
    for (unsigned i = 0; i < object->segment_count; ++i)
    {
        EpfSegment *segment = &object->segments[i];
        if (segment->type != EPF_SEG_RELRO) continue;
        uint64_t cursor = segment->rva, end = cursor + segment->mem_size;
        for (unsigned j = 0; j < object->segment_count; ++j)
        {
            EpfSegment *load = &object->segments[j];
            if (load->type == EPF_SEG_LOAD && (load->flags & EPF_PF_W) && cursor >= load->rva &&
                cursor - load->rva < UP(load->mem_size)) cursor = minimum(end, load->rva + UP(load->mem_size));
            if (j < i && load->type == EPF_SEG_RELRO && overlap(segment->rva, segment->mem_size,
                load->rva, load->mem_size)) fail("overlapping RELRO");
        }
        if (cursor != end) fail("RELRO outside writable LOAD pages");
    }
}

static const char *string(Object *object, uint64_t offset)
{
    if (offset >= object->string_size) fail("string offset");
    for (uint64_t i = offset; i < object->string_size; ++i)
    {
        work();
        if (!object->strings[i]) return (const char *)object->strings + offset;
    }
    fail("unterminated string");
}

static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) { work(); ++a; ++b; }
    return *a == *b;
}

static int same_library(const char *a, const char *b)
{
    // The native FAT path namespace folds ASCII case. Keep this identity rule
    // stable even when the host tests use a case-sensitive filesystem.
    while (*a && *b)
    {
        work();
        unsigned left = (unsigned char)*a++, right = (unsigned char)*b++;
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right) return 0;
    }
    return *a == *b;
}

static uint32_t hash(const char *name)
{
    uint32_t value = 2166136261u;
    while (*name) { work(); value = (value ^ (uint8_t)*name++) * 16777619u; }
    return value;
}

static void name_copy(char *out, const char *name)
{
    unsigned i = 0;
    do
    {
        unsigned char c = (unsigned char)name[i];
        if ((i == 127 && c) || (c && (c <= 32 || c >= 127 || c == '/' || c == '\\' || c == ':')))
            fail("invalid dependency basename");
        out[i++] = (char)c;
    } while (out[i - 1]);
    if (i == 1 || equal(name, ".") || equal(name, "..")) fail("invalid dependency basename");
}

static void directory(Object *object)
{
    EpfSegment *dynamic = NULL;
    for (unsigned i = 0; i < object->segment_count; ++i)
        if (object->segments[i].type == EPF_SEG_DYNAMIC) dynamic = &object->segments[i];
    const uint8_t *entries = metadata(object, dynamic->rva, dynamic->file_size);
    // File and RVA must name the very same initialized bytes.
    int corresponding = 0;
    for (unsigned i = 0; i < object->segment_count; ++i)
    {
        EpfSegment *load = &object->segments[i];
        if (load->type == EPF_SEG_LOAD && dynamic->rva >= load->rva &&
            range(dynamic->rva - load->rva, dynamic->file_size, load->file_size) &&
            dynamic->file_offset == load->file_offset + dynamic->rva - load->rva) corresponding = 1;
    }
    if (!corresponding) fail("DYNAMIC file/RVA mismatch");
    uint64_t values[15] = {0};
    unsigned seen = 0, terminated = 0;
    for (uint64_t offset = 0; offset < dynamic->file_size; offset += 16)
    {
        uint32_t tag = epf_read_u32(entries + offset), flags = epf_read_u32(entries + offset + 4);
        uint64_t value = epf_read_u64(entries + offset + 8);
        if (flags & ~EPF_DT_REQUIRED) fail("dynamic entry flags");
        if (tag == 0)
        {
            if (!zero(entries + offset, dynamic->file_size - offset)) fail("dynamic NULL padding");
            terminated = 1;
            break;
        }
        if (tag > 14) { if (flags & EPF_DT_REQUIRED) fail("unknown required dynamic tag"); else continue; }
        if (tag == EPF_DT_NEEDED)
        {
            if (object->needed_count == EPF_MAX_NEEDED) fail("dependency limit");
            object->needed[object->needed_count++] = value;
        }
        else
        {
            if (seen & (1u << tag)) fail("duplicate dynamic tag");
            seen |= 1u << tag;
            values[tag] = value;
        }
    }
    if (!terminated) fail("unterminated dynamic directory");
    unsigned groups[] = {6, 56, 448, 6144, 24576}; // tags 1..2, 3..5, 6..8, 11..12, 13..14
    for (unsigned i = 0; i < 5; ++i)
        if ((seen & groups[i]) && (seen & groups[i]) != groups[i]) fail("incomplete dynamic table group");
    if ((seen & 62) != 62 || values[5] != EPF_SYMBOL_SIZE || !values[2] || values[2] > MAX_STRINGS ||
        !values[4] || values[4] > EPF_MAX_SYMBOLS || values[1] % 8 || values[3] % 8 || values[6] % 8 ||
        values[7] > EPF_MAX_RELOCATIONS || ((seen & 448) && values[8] != EPF_RELA_SIZE) ||
        values[12] > EPF_MAX_LIFECYCLE || values[14] > EPF_MAX_LIFECYCLE || values[11] % 8 || values[13] % 8 ||
        ((object->features & EPF_FEATURE_RUNTIME_RELOC) && !(seen & 448)) ||
        (values[7] && !(object->features & EPF_FEATURE_RUNTIME_RELOC))) fail("dynamic table limits or capabilities");
    object->string_size = values[2]; object->symbol_count = values[4]; object->rela_count = values[7];
    object->init_count = values[12]; object->fini_count = values[14];
    object->strings = metadata(object, values[1], values[2]);
    object->symbols = metadata(object, values[3], values[4] * EPF_SYMBOL_SIZE);
    if (seen & 448) object->relas = metadata(object, values[6], values[7] * EPF_RELA_SIZE);
    if (seen & 6144) object->init = metadata(object, values[11], values[12] * 8);
    if (seen & 24576) object->fini = metadata(object, values[13], values[14] * 8);
    if (object->strings[0] || !zero(object->symbols, EPF_SYMBOL_SIZE)) fail("invalid null string or symbol");
    if (seen & (1u << EPF_DT_SONAME)) { char name[128]; name_copy(name, string(object, values[10])); }
    for (unsigned i = 0; i < object->needed_count; ++i)
    { char name[128]; name_copy(name, string(object, object->needed[i])); }
    for (unsigned array = 0; array < 2; ++array)
    {
        const uint8_t *functions = array ? object->fini : object->init;
        uint64_t count = array ? object->fini_count : object->init_count;
        for (uint64_t i = 0; i < count; ++i)
            if (!covered(object, epf_read_u64(functions + i * 8), 1, EPF_PF_X, 1)) fail("lifecycle target");
    }
}

static uint64_t symbol_value(Object *object, const uint8_t *symbol)
{
    uint64_t value = epf_read_u64(symbol + 8);
    return symbol[27] == 2 ? object->base + value : value;
}

static void symbols(Object *object)
{
    object->bucket_count = 1;
    while (object->bucket_count < object->symbol_count * 2) object->bucket_count *= 2;
    object->buckets = (uint32_t *)allocate(object->bucket_count * 4);
    object->chains = (uint32_t *)allocate(object->symbol_count * 4);
    for (unsigned i = 1; i < object->symbol_count; ++i)
    {
        const uint8_t *symbol = object->symbols + (uint64_t)i * EPF_SYMBOL_SIZE;
        uint64_t value = epf_read_u64(symbol + 8), size = epf_read_u64(symbol + 16);
        const char *name = string(object, epf_read_u32(symbol));
        unsigned binding = symbol[24], type = symbol[25], visibility = symbol[26], domain = symbol[27];
        if (epf_read_u32(symbol + 4) || epf_read_u32(symbol + 28) || binding > 2 ||
            (type > 2 && type != 4) || visibility > 1 ||
            (domain != 0 && domain != 2 && domain != 3 && domain != 4) ||
            (type == 4 && !(object->features & EPF_FEATURE_TLS)) ||
            ((domain == 4) != (type == 4 && domain != 0)) ||
            (domain == 4 && !range(value, size ? size : 1, object->tls.mem_size)) ||
            (domain == 0 && (value || !*name || !binding)) ||
            (domain == 2 && !covered(object, value, size ? size : 1, EPF_PF_R, 0)) ||
            (domain == 2 && type == 2 && !covered(object, value, 1, EPF_PF_X, 1))) fail("invalid dynamic symbol");
        if (!domain || !binding || visibility) continue;
        unsigned bucket = hash(name) & (object->bucket_count - 1);
        for (unsigned j = object->buckets[bucket]; j; j = object->chains[j])
        {
            work();
            const uint8_t *other = object->symbols + (uint64_t)j * EPF_SYMBOL_SIZE;
            if (binding == 1 && other[24] == 1 && equal(name, string(object, epf_read_u32(other))))
                fail("duplicate strong export");
        }
        object->chains[i] = object->buckets[bucket];
        object->buckets[bucket] = i;
    }
}

typedef struct { Object *object; const uint8_t *symbol; } Resolved;

static Resolved resolve(Object *object, unsigned index)
{
    if (index >= object->symbol_count) fail("relocation symbol index");
    const uint8_t *symbol = object->symbols + (uint64_t)index * EPF_SYMBOL_SIZE;
    if (symbol[27]) return (Resolved){object, symbol}; // local definitions bind this module
    if (!index) return (Resolved){0, 0};
    const char *name = string(object, epf_read_u32(symbol));
    if (symbol[26]) { if (symbol[24] == 2) return (Resolved){0, 0}; else fail("undefined hidden symbol"); }
    unsigned queue[EPF_MAX_MODULES], count = 0, visited[EPF_MAX_MODULES] = {0};
    visited[object - objects] = 1;
    for (unsigned i = 0; i < object->dependency_count; ++i)
    {
        unsigned dependency = object->dependencies[i];
        if (!visited[dependency]) { visited[dependency] = 1; queue[count++] = dependency; }
    }
    Resolved weak = {0, 0};
    int have_weak = 0;
    uint32_t name_hash = hash(name);
    for (unsigned at = 0; at < count; ++at)
    {
        Object *dependency = &objects[queue[at]];
        unsigned weak_index = 0;
        for (unsigned j = dependency->buckets[name_hash & (dependency->bucket_count - 1)]; j; j = dependency->chains[j])
        {
            work();
            const uint8_t *candidate = dependency->symbols + (uint64_t)j * EPF_SYMBOL_SIZE;
            if (!equal(name, string(dependency, epf_read_u32(candidate)))) continue;
            if (candidate[24] == 1) return (Resolved){dependency, candidate};
            if (!weak_index || j < weak_index) weak_index = j;
        }
        if (!have_weak && weak_index)
        {
            weak = (Resolved){dependency, dependency->symbols + (uint64_t)weak_index * EPF_SYMBOL_SIZE};
            have_weak = 1;
        }
        for (unsigned j = 0; j < dependency->dependency_count; ++j)
        {
            unsigned next = dependency->dependencies[j];
            if (!visited[next]) { visited[next] = 1; queue[count++] = next; }
        }
    }
    if (have_weak) return weak;
    if (symbol[24] == 2) return (Resolved){0, 0};
    fail("unresolved strong symbol");
}

static void sift(Write *writes, unsigned count, unsigned root)
{
    while (root < count / 2)
    {
        unsigned child = root * 2 + 1;
        if (child + 1 < count && writes[child].target < writes[child + 1].target) ++child;
        if (writes[root].target >= writes[child].target) break;
        Write saved = writes[root]; writes[root] = writes[child]; writes[child] = saved;
        root = child;
    }
}

static void relocations(Object *object)
{
    object->writes = (Write *)allocate(object->rela_count * sizeof(Write));
    for (uint64_t i = 0; i < object->rela_count; ++i)
    {
        const uint8_t *record = object->relas + i * EPF_RELA_SIZE;
        uint64_t target = epf_read_u64(record), raw = epf_read_u64(record + 8);
        unsigned index = epf_read_u32(record + 16), type = epf_read_u32(record + 20);
        if (epf_read_u64(record + 24)) fail("relocation reserved field");
        if (type == EPF_RELOC_NONE) continue;
        int tls = type >= EPF_RELOC_X86_64_DTPMOD64 && type <= EPF_RELOC_X86_64_TPOFF64;
        if (!tls && (type < EPF_RELOC_ABS32 || type > EPF_RELOC_RELATIVE64)) fail("unsupported relocation");
        if (tls && !(object->features & EPF_FEATURE_TLS)) fail("TLS relocation capability");
        unsigned width = (tls || type == EPF_RELOC_ABS64 || type == EPF_RELOC_RELATIVE64) ? 8 : 4;
        if (!covered(object, target, width, EPF_PF_W, 0)) fail("relocation outside writable data");
        for (unsigned j = 0; j < object->metadata_count; ++j)
            if (overlap(target, width, object->metadata[j].rva, object->metadata[j].size))
                fail("relocation overwrites metadata");
        __int128 value;
        if (tls)
        {
            Resolved definition = index ? resolve(object, index) : (Resolved){object, 0};
            if (definition.symbol && (definition.symbol[27] != 4 || definition.symbol[25] != 4))
                fail("TLS symbol type mismatch");
            if (!definition.object)
            {
                if (raw || type == EPF_RELOC_X86_64_TPOFF64) fail("unresolved weak TLS offset");
                value = 0;
            }
            else
            {
                if (!definition.object->tls.mem_size) fail("TLS module has no storage");
                if (type == EPF_RELOC_X86_64_DTPMOD64)
                {
                    if (raw) fail("TLS module relocation addend");
                    value = definition.object - objects + 1;
                }
                else
                {
                    value = definition.symbol ? epf_read_u64(definition.symbol + 8) : 0;
                    value += (int64_t)raw;
                    if (value < 0 || value >= definition.object->tls.mem_size) fail("TLS offset range");
                    if (type == EPF_RELOC_X86_64_TPOFF64) value -= definition.object->tls_end;
                }
            }
        }
        else
        {
            if (type == EPF_RELOC_RELATIVE32 || type == EPF_RELOC_RELATIVE64)
            {
                if (index) fail("relative relocation symbol");
                value = object->base;
            }
            else
            {
                Resolved definition = resolve(object, index);
                if (definition.symbol && definition.symbol[27] == 4) fail("non-TLS relocation references TLS");
                value = definition.symbol ? symbol_value(definition.object, definition.symbol) : 0;
            }
            value += (int64_t)raw;
            if (type == EPF_RELOC_PCREL32) value -= object->base + target;
            if (type == EPF_RELOC_PCREL32 ? (value < -2147483648LL || value > 2147483647LL)
                : (value < 0 || value > (width == 4 ? (__int128)0xffffffffULL : (__int128)~0ULL)))
                fail("relocation overflow");
        }
        object->writes[object->write_count++] = (Write){target, (uint64_t)value, width};
    }
    unsigned count = object->write_count;
    for (unsigned i = count / 2; i; --i) sift(object->writes, count, i - 1);
    for (unsigned end = count; end > 1; --end)
    {
        Write saved = object->writes[0]; object->writes[0] = object->writes[end - 1]; object->writes[end - 1] = saved;
        sift(object->writes, end - 1, 0);
    }
    for (unsigned i = 1; i < count; ++i)
        if (object->writes[i].target < object->writes[i - 1].target + object->writes[i - 1].width)
            fail("overlapping relocation writes");
}

static void load(Object *object)
{
    char path[sizeof(LIBRARY_DIRECTORY) + 128];
    copy(path, LIBRARY_DIRECTORY, sizeof(LIBRARY_DIRECTORY) - 1);
    copy(path + sizeof(LIBRARY_DIRECTORY) - 1, object->name, rtld_length(object->name) + 1);
    long fd = rtld_syscall(2, (long)path, 0, 0, 0, 0, 0);
    if (rtld_failed(fd)) fail("dependency not found");
    long size = rtld_syscall(8, fd, 0, 2, 0, 0, 0);
    if (size < EPF_HEADER_SIZE || size > (long)EPF_MAX_FILE_SIZE || rtld_syscall(8, fd, 0, 0, 0, 0, 0))
        fail("library file size");
    uint8_t *file = allocate((uint64_t)size);
    for (long at = 0; at < size;)
    {
        long got = rtld_syscall(0, fd, (long)(file + at), size - at, 0, 0, 0);
        if (got == -4) continue;
        if (got <= 0 || got > size - at) fail("library read failed");
        at += got;
    }
    rtld_syscall(3, fd, 0, 0, 0, 0, 0);
    protect((uint64_t)file, UP((uint64_t)size), 1);
    if (!epf_has_magic(file, (uint64_t)size) || file[4] != 1 || file[5] != 0 || file[6] != 8 || file[7] != 1 ||
        epf_read_u16(file + 8) != EPF_SHARED || epf_read_u16(file + 10) != EPF_MACHINE_X86_64 ||
        epf_read_u16(file + 12) != EPF_OS_ABI_XJ380 || (epf_read_u16(file + 14) != EPF_XJ380_ABI_VERSION &&
         epf_read_u16(file + 14) != EPF_XJ380_TLS_ABI_VERSION &&
         epf_read_u16(file + 14) != EPF_XJ380_CXX_TLS_ABI_VERSION) ||
        epf_read_u32(file + 16) || epf_read_u32(file + 20) != EPF_HEADER_SIZE || epf_read_u64(file + 32) ||
        epf_read_u64(file + 40) != EPF_NO_ENTRY || epf_read_u64(file + 72) != (uint64_t)size ||
        epf_read_u16(file + 92) != EPF_SEGMENT_SIZE || epf_read_u16(file + 94) != EPF_SECTION_SIZE ||
        epf_read_u32(file + 104) || !zero(file + 108, 20)) fail("unsupported library header or ABI");
    object->features = epf_read_u64(file + 24);
    if ((object->features & EPF_FEATURE_TLS) && epf_read_u16(file + 14) < EPF_XJ380_TLS_ABI_VERSION)
        fail("TLS requires native ABI 2");
    object->size = epf_read_u64(file + 48); object->align = epf_read_u64(file + 96);
    object->segment_count = epf_read_u32(file + 80); object->file_size = (uint64_t)size; object->file = file;
    uint64_t table = epf_read_u64(file + 56), section_table = epf_read_u64(file + 64);
    uint64_t section_count = epf_read_u32(file + 84), shstr = epf_read_u32(file + 88);
    if (!object->segment_count || object->segment_count > EPF_MAX_SEGMENTS || table < EPF_HEADER_SIZE || table % 8 ||
        !range(table, object->segment_count * EPF_SEGMENT_SIZE, (uint64_t)size) || section_count > EPF_MAX_SECTIONS ||
        (section_count ? (section_table < EPF_HEADER_SIZE || section_table % 8 || shstr >= section_count ||
            !range(section_table, section_count * EPF_SECTION_SIZE, (uint64_t)size)) : (section_table || shstr)) ||
        !power(object->align) || object->align < PAGE || object->align > (2ULL << 20) || !object->size ||
        object->size > (64ULL << 20) || object->size % object->align ||
        (object->features & ~(EPF_FEATURE_RUNTIME_RELOC | EPF_FEATURE_DYNAMIC_LINK |
                              EPF_FEATURE_RELRO | EPF_FEATURE_TLS)) ||
        !(object->features & EPF_FEATURE_DYNAMIC_LINK)) fail("library layout or resource limit");
    uint64_t alignment = object->align;
    for (unsigned i = 0; i < object->segment_count; ++i)
    {
        object->segments[i] = segment_decode(file + table + (uint64_t)i * EPF_SEGMENT_SIZE);
        if (object->segments[i].type == EPF_SEG_LOAD && object->segments[i].align > alignment)
            alignment = object->segments[i].align;
    }
    segments(object, 0);
    directory(object);
    uint64_t reservation = (uint64_t)allocate(object->size + alignment);
    object->base = (reservation + alignment - 1) & ~(alignment - 1);
    // Revoke alignment padding too. Every live image page is NX during copies.
    protect(reservation, UP(object->size + alignment), 0);
    for (unsigned i = 0; i < object->segment_count; ++i)
    {
        EpfSegment *segment = &object->segments[i];
        if (segment->type != EPF_SEG_LOAD) continue;
        protect(object->base + segment->rva, UP(segment->mem_size), 3);
        copy((void *)(object->base + segment->rva), file + segment->file_offset, segment->file_size);
    }
    if (rtld_syscall(11, (long)file, (long)UP((uint64_t)size), 0, 0, 0, 0)) fail("file release failed");
    bytes_used -= UP((uint64_t)size);
    object->file = NULL;
}

static void seal(Object *object)
{
    for (unsigned i = 0; i < object->segment_count; ++i)
    {
        EpfSegment *segment = &object->segments[i];
        if (segment->type == EPF_SEG_LOAD)
            protect(object->base + segment->rva, UP(segment->mem_size),
                    1 | ((segment->flags & EPF_PF_W) ? 2 : 0) | ((segment->flags & EPF_PF_X) ? 4 : 0));
    }
    for (unsigned i = 0; i < object->segment_count; ++i)
    {
        EpfSegment *segment = &object->segments[i];
        if (segment->type == EPF_SEG_RELRO) protect(object->base + segment->rva, segment->mem_size, 1);
    }
}

static uint64_t tls_size, tls_alignment = 16;

static void layout_tls(void)
{
    for (unsigned i = 0; i < object_count; ++i)
    {
        Object *object = &objects[i];
        if (!object->tls.mem_size) continue;
        tls_size = (tls_size + object->tls.mem_size + object->tls.align - 1) & ~(object->tls.align - 1);
        if (tls_size > EPF_MAX_TLS_TOTAL) fail("aggregate TLS size limit");
        object->tls_end = tls_size;
        if (object->tls.align > tls_alignment) tls_alignment = object->tls.align;
    }
    tls_size = (tls_size + tls_alignment - 1) & ~(tls_alignment - 1);
}

static void initialize_tls(void)
{
    EpfTlsContext *context = (EpfTlsContext *)allocate(sizeof(EpfTlsContext));
    context->magic = EPF_TLS_CONTEXT_MAGIC; context->version = 1;
    context->module_count = object_count; context->block_size = tls_size; context->alignment = tls_alignment;
    if (rtld_syscall(318, (long)&context->canary, 8, 0, 0, 0, 0) != 8) fail("TLS canary entropy");
    context->canary &= ~255ULL;
    for (unsigned i = 0; i < object_count; ++i)
    {
        Object *object = &objects[i];
        EpfTlsModule *module = &context->modules[i];
        module->end = object->tls_end; module->memory_size = object->tls.mem_size;
        module->file_size = object->tls.file_size;
        if (module->file_size)
        {
            uint8_t *snapshot = allocate(module->file_size);
            copy(snapshot, (const void *)(object->base + object->tls.rva), module->file_size);
            protect((uint64_t)snapshot, UP(module->file_size), 1);
            module->template_address = (uint64_t)snapshot;
        }
    }
    protect((uint64_t)context, UP(sizeof(EpfTlsContext)), 1);
    uint64_t pointer = epf_tls_allocate(context);
    if (!pointer || epf_tls_install(pointer)) fail("main TLS allocation or FS setup");
}

static void finalize(void)
{
    // Main-thread TLS destructors precede module FINI callbacks while every
    // module is still mapped. Both runtime copies use the current FS list.
    epf_tls_run_destructors();
    while (init_count)
    {
        Object *object = &objects[init_order[--init_count]];
        for (uint64_t i = object->fini_count; i; --i)
            ((void (*)(void))(object->base + epf_read_u64(object->fini + (i - 1) * 8)))();
    }
    // Also reclaim callbacks first registered by a FINI callback itself.
    epf_tls_run_destructors();
}

void rtld_start(uint64_t *stack)
{
    uint64_t argc = stack[0];
    if (argc > 4096) fail("argument count");
    uint64_t *env = stack + argc + 2;
    unsigned count = 0;
    while (*env && count++ < 4096) ++env;
    if (*env) fail("environment count");
    uint64_t *aux = env + 1;
    const uint8_t *handoff = NULL;
    uint64_t entry = 0, phent = 0, phnum = 0;
    for (unsigned i = 0; i < 64; ++i, aux += 2)
    {
        if (!aux[0]) break;
        if (aux[0] == 3) handoff = (const uint8_t *)aux[1];
        if (aux[0] == 4) phent = aux[1];
        if (aux[0] == 5) phnum = aux[1];
        if (aux[0] == 9) entry = aux[1];
        if (aux[0] == 6 && aux[1] != PAGE) fail("page size");
        if (i == 63) fail("unterminated auxiliary vector");
    }
    if (!handoff || epf_read_u64(handoff) != EPF_HANDOFF_MAGIC || epf_read_u64(handoff + 8) != 1 ||
        phent != EPF_SEGMENT_SIZE || !phnum || phnum > EPF_MAX_SEGMENTS ||
        epf_read_u64(handoff + 40) != phnum || epf_read_u64(handoff + 32) != entry) fail("kernel handoff");
    Object *main = &objects[0];
    object_count = 1;
    main->base = epf_read_u64(handoff + 16); main->size = epf_read_u64(handoff + 24); main->entry = entry;
    main->features = epf_read_u64(handoff + 48); main->align = epf_read_u64(handoff + 56);
    main->segment_count = (unsigned)phnum; main->file_size = EPF_MAX_FILE_SIZE;
    if (!range(main->base, main->size, LIMIT) || !main->size || main->size > EPF_MAX_IMAGE_SIZE ||
        !power(main->align) || main->align < PAGE || main->align > (2ULL << 20) ||
        main->size % main->align || !(main->features & EPF_FEATURE_DYNAMIC_LINK) ||
        (main->features & ~(EPF_FEATURE_RUNTIME_RELOC | EPF_FEATURE_DYNAMIC_LINK |
                            EPF_FEATURE_RELRO | EPF_FEATURE_TLS)))
        fail("main image layout");
    for (unsigned i = 0; i < phnum; ++i)
        main->segments[i] = segment_decode(handoff + EPF_HANDOFF_SIZE + (uint64_t)i * EPF_SEGMENT_SIZE);
    segments(main, 1);
    if (entry < main->base || !covered(main, entry - main->base, 1, EPF_PF_X, 1)) fail("main entry");
    directory(main);
    // Breadth-first discovery fixes dependency and export lookup order. Identity
    // is the case-folded path under one fixed directory, never a SONAME.
    for (unsigned at = 0; at < object_count; ++at)
    {
        Object *object = &objects[at];
        for (unsigned i = 0; i < object->needed_count; ++i)
        {
            char name[128]; name_copy(name, string(object, object->needed[i]));
            unsigned found;
            for (found = 1; found < object_count; ++found) if (same_library(name, objects[found].name)) break;
            if (found == object_count)
            {
                if (object_count == EPF_MAX_MODULES) fail("module count limit");
                ++object_count;
                copy(objects[found].name, name, rtld_length(name) + 1);
                load(&objects[found]);
            }
            unsigned duplicate = 0;
            for (unsigned j = 0; j < object->dependency_count; ++j)
                if (object->dependencies[j] == found) duplicate = 1;
            if (!duplicate) object->dependencies[object->dependency_count++] = found;
        }
    }
    // Stable topological order: dependencies first, then earliest discovery.
    unsigned order[EPF_MAX_MODULES], ordered[EPF_MAX_MODULES] = {0};
    for (unsigned at = 0; at < object_count;)
    {
        // Finish a whole ready layer before publishing it as completed. This
        // preserves discovery order within a layer even when an early module
        // becomes ready before an unrelated later leaf has been visited.
        unsigned first = at;
        for (unsigned selected = 0; selected < object_count; ++selected)
        {
            if (ordered[selected]) continue;
            Object *object = &objects[selected];
            unsigned ready = 1;
            for (unsigned j = 0; j < object->dependency_count; ++j) if (!ordered[object->dependencies[j]]) ready = 0;
            if (ready) order[at++] = selected;
        }
        if (first == at) fail("dependency cycle");
        for (unsigned i = first; i < at; ++i) ordered[order[i]] = 1;
    }
    layout_tls();
    for (unsigned i = 0; i < object_count; ++i) symbols(&objects[i]);
    for (unsigned i = 0; i < object_count; ++i) relocations(&objects[i]);
    // All metadata and values across the graph are checked before the first
    // write or constructor. Immutable snapshots survive RELRO and finalization.
    for (unsigned i = 0; i < object_count; ++i)
    {
        Object *object = &objects[i];
        for (unsigned j = 0; j < object->write_count; ++j)
        {
            Write *write = &object->writes[j];
            uint8_t *target = (uint8_t *)(object->base + write->target);
            for (unsigned byte = 0; byte < write->width; ++byte) target[byte] = (uint8_t)(write->value >> (8 * byte));
        }
        seal(object);
    }
    initialize_tls();
    for (unsigned i = 0; i < object_count; ++i)
    {
        Object *object = &objects[order[i]];
        for (uint64_t j = 0; j < object->init_count; ++j)
            ((void (*)(void))(object->base + epf_read_u64(object->init + j * 8)))();
        init_order[init_count++] = order[i];
    }
    rtld_enter(entry, stack, finalize);
}
