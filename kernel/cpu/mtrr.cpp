#include <cpu/msr.h>
#include <proto.hpp>
#include <stdint.h>

extern uint64_t *saved_mtrrs;

namespace
{
constexpr uint64_t CR0_NW = 1ULL << 29;
constexpr uint64_t CR0_CD = 1ULL << 30;
constexpr uint64_t CR4_PGE = 1ULL << 7;
constexpr uint64_t MTRR_ENABLE = 1ULL << 11;
constexpr uint64_t MTRR_FIXED = 1ULL << 8;
constexpr uint32_t PAT_MSR = 0x277;
constexpr uint32_t MTRR_DEFAULT_MSR = 0x2ff;
constexpr uint32_t fixed_mtrrs[] = {0x250, 0x258, 0x259, 0x268, 0x269, 0x26a,
                                  0x26b, 0x26c, 0x26d, 0x26e, 0x26f};

uint64_t saved_pat;
uint8_t saved_variable_count;
bool saved_fixed, saved_has_mtrr, saved_has_pat, cache_snapshot_ready;

uint32_t cache_features()
{
    uint32_t eax, ebx, ecx, edx;
    asm_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    return edx;
}

uint64_t cache_read_cr0()
{
    uint64_t value;
    asm volatile("mov %%cr0, %0" : "=r"(value) :: "memory");
    return value;
}

void cache_write_cr0(uint64_t value)
{
    asm volatile("mov %0, %%cr0" :: "r"(value) : "memory");
}

uint64_t cache_read_cr4()
{
    uint64_t value;
    asm volatile("mov %%cr4, %0" : "=r"(value) :: "memory");
    return value;
}

void cache_write_cr4(uint64_t value)
{
    asm volatile("mov %0, %%cr4" :: "r"(value) : "memory");
}

void cache_flush_tlb()
{
    uint64_t cr3;
    asm volatile("mov %%cr3, %0\n\tmov %0, %%cr3" : "=&r"(cr3) :: "memory");
}

void cache_flush()
{
    asm volatile("wbinvd" ::: "memory");
}

[[noreturn]] void cache_init_failed()
{
    write_serial_string("Fatal AP cache initialization: incompatible or missing BSP snapshot.\n");
    for (;;) asm volatile("cli; hlt" ::: "memory");
}
} // namespace

// Called on the BSP before any AP is started. The boot-owned buffer is sized
// for this CPU's variable MTRRs, eleven fixed MTRRs and the exact default MSR.
void mtrr_save()
{
    uint32_t features = cache_features();
    saved_has_mtrr = (features & (1U << 12)) != 0;
    saved_has_pat = (features & (1U << 16)) != 0;
    if (saved_has_pat) saved_pat = rdmsr(PAT_MSR);
    if (saved_has_mtrr)
    {
        if (saved_mtrrs == NULL) cache_init_failed();
        uint64_t capability = rdmsr(0xfe);
        saved_variable_count = capability & 0xff;
        saved_fixed = (capability & MTRR_FIXED) != 0;
        for (unsigned i = 0; i < unsigned(saved_variable_count) * 2; ++i)
            saved_mtrrs[i] = rdmsr(0x200 + i);
        unsigned base = unsigned(saved_variable_count) * 2;
        if (saved_fixed)
            for (unsigned i = 0; i < 11; ++i) saved_mtrrs[base + i] = rdmsr(fixed_mtrrs[i]);
        // Preserve enable/fixed-enable bits and memory types exactly. Disabling
        // MTRRs belongs to the local restore sequence, not the saved snapshot.
        saved_mtrrs[base + 11] = rdmsr(MTRR_DEFAULT_MSR);
    }
    __atomic_store_n(&cache_snapshot_ready, true, __ATOMIC_RELEASE);
}

// AP startup only, with IRQs disabled and before publishing a runnable task.
// INIT leaves CR0.CD/NW set. Restoring those entry bits would leave every AP
// in no-fill mode indefinitely; the final transition must enable caching.
void mtrr_restore()
{
    if (!__atomic_load_n(&cache_snapshot_ready, __ATOMIC_ACQUIRE)) cache_init_failed();
    uint32_t features = cache_features();
    if (saved_has_mtrr != ((features & (1U << 12)) != 0) ||
        (saved_has_pat && !(features & (1U << 16)))) cache_init_failed();
    if (saved_has_mtrr)
    {
        uint64_t capability = rdmsr(0xfe);
        if ((capability & 0xff) != saved_variable_count ||
            (saved_fixed && !(capability & MTRR_FIXED))) cache_init_failed();
    }

    uint64_t original_cr0 = cache_read_cr0();
    uint64_t original_cr4 = cache_read_cr4();
    cache_write_cr0((original_cr0 | CR0_CD) & ~CR0_NW);
    cache_flush();
    // Remove global as well as ordinary TLB entries while changing types.
    cache_write_cr4(original_cr4 & ~CR4_PGE);
    cache_flush_tlb();

    if (saved_has_mtrr)
    {
        wrmsr(MTRR_DEFAULT_MSR, rdmsr(MTRR_DEFAULT_MSR) & ~MTRR_ENABLE);
        for (unsigned i = 0; i < unsigned(saved_variable_count) * 2; ++i)
            wrmsr(0x200 + i, saved_mtrrs[i]);
        unsigned base = unsigned(saved_variable_count) * 2;
        if (saved_fixed)
            for (unsigned i = 0; i < 11; ++i) wrmsr(fixed_mtrrs[i], saved_mtrrs[base + i]);
        if (saved_has_pat) wrmsr(PAT_MSR, saved_pat);
        wrmsr(MTRR_DEFAULT_MSR, saved_mtrrs[base + 11]);
    }
    else if (saved_has_pat)
    {
        wrmsr(PAT_MSR, saved_pat);
    }
    cache_flush();
    cache_flush_tlb();
    cache_write_cr4(original_cr4);
    cache_write_cr0(original_cr0 & ~(CR0_CD | CR0_NW));
}
