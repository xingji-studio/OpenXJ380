#include <cpu/regio.h>
#include <mm/hhdm.h>
#include <mm/page.h>
#include <proto.hpp>
#include <dlinker.h>
#include <stdint.h>

// 驱动恒等映射空间偏移，既然是请XIAOYI写的那就跟CPOS统一吧省的炸了，我也不知道为什么是这个数
#define DRIVER_AREA_MEM 0xffffb00000000000

uint64_t physical_memory_offset = 0;

void init_hhdm()
{
    physical_memory_offset = 0xffff800000000000; // 嗯，先这样吧，只要你不去改引导就没问题。
}

uint64_t get_physical_memory_offset()
{
    return physical_memory_offset;
}

void *phys_to_virt(uint64_t phys_addr)
{
    if (phys_addr == 0) return NULL;
    return (void *)(phys_addr + physical_memory_offset);
}
EXPORT_SYMBOL(phys_to_virt);

void *virt_to_phys(uint64_t virt_addr)
{
    if (virt_addr == 0) return NULL;
    return (void *)(virt_addr - physical_memory_offset);
}

EXPORT_SYMBOL(virt_to_phys);

void *driver_phys_to_virt(uint64_t phys_addr)
{
    if (phys_addr == 0) return NULL;
    return (void *)(phys_addr + DRIVER_AREA_MEM);
}
EXPORT_SYMBOL(driver_phys_to_virt);
void *driver_virt_to_phys(uint64_t virt_addr)
{
    if (virt_addr == 0) return NULL;
    return (void *)(virt_addr - DRIVER_AREA_MEM);
}
EXPORT_SYMBOL(driver_virt_to_phys);
uint64_t page_virt_to_phys(uint64_t va)
{
    uint64_t  pml4_phys = get_cr3();
    uint64_t *pml4      = (uint64_t *)phys_to_virt(pml4_phys);

    size_t pml4_idx = (va >> 39) & ENTRY_MASK;
    size_t pdpt_idx = (va >> 30) & ENTRY_MASK;
    size_t pd_idx   = (va >> 21) & ENTRY_MASK;
    size_t pt_idx   = (va >> 12) & ENTRY_MASK;
    size_t offset   = va & 0xFFF;

    uint64_t pml4e = pml4[pml4_idx];
    if (!(pml4e & PTE_PRESENT)) return 0; // not mapped
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4e & PTE_ADDR_MASK);

    uint64_t pdpte = pdpt[pdpt_idx];
    if (!(pdpte & PTE_PRESENT)) return 0;
    if (pdpte & PTE_HUGE) {
        return (pdpte & (PTE_ADDR_MASK & ~((1ULL << 30) - 1))) + (va & ((1ULL << 30) - 1));
    }
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpte & PTE_ADDR_MASK);

    uint64_t pde = pd[pd_idx];
    if (!(pde & PTE_PRESENT)) return 0;
    if (pde & PTE_HUGE) {
        return (pde & (PTE_ADDR_MASK & ~((1ULL << 21) - 1))) + (va & ((1ULL << 21) - 1));
    }
    uint64_t *pt = (uint64_t *)phys_to_virt(pde & PTE_ADDR_MASK);

    uint64_t pte = pt[pt_idx];
    if (!(pte & PTE_PRESENT)) return 0;

    uint64_t pa = (pte & PTE_ADDR_MASK) + offset;
    return pa;
}
