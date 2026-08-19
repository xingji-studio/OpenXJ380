#include <cpu/msr.h>
#include <proto.hpp>
#include <stdint.h>

extern uint64_t *saved_mtrrs;

void mtrr_save()
{
    if (!mtrr_supported()) { return; }

    uint64_t ia32_mtrrcap = rdmsr(0xfe);

    uint8_t var_reg_count = ia32_mtrrcap & 0xff;

    // 此处应有malloc，没有也没关系炸不了
    /* save variable range MTRRs */
    for (uint8_t i = 0; i < var_reg_count * 2; i += 2)
    {
        saved_mtrrs[i]     = rdmsr(0x200 + i);
        saved_mtrrs[i + 1] = rdmsr(0x200 + i + 1);
    }

    /* save fixed range MTRRs */
    saved_mtrrs[var_reg_count * 2 + 0]  = rdmsr(0x250);
    saved_mtrrs[var_reg_count * 2 + 1]  = rdmsr(0x258);
    saved_mtrrs[var_reg_count * 2 + 2]  = rdmsr(0x259);
    saved_mtrrs[var_reg_count * 2 + 3]  = rdmsr(0x268);
    saved_mtrrs[var_reg_count * 2 + 4]  = rdmsr(0x269);
    saved_mtrrs[var_reg_count * 2 + 5]  = rdmsr(0x26a);
    saved_mtrrs[var_reg_count * 2 + 6]  = rdmsr(0x26b);
    saved_mtrrs[var_reg_count * 2 + 7]  = rdmsr(0x26c);
    saved_mtrrs[var_reg_count * 2 + 8]  = rdmsr(0x26d);
    saved_mtrrs[var_reg_count * 2 + 9]  = rdmsr(0x26e);
    saved_mtrrs[var_reg_count * 2 + 10] = rdmsr(0x26f);

    /* save MTRR default type */
    saved_mtrrs[var_reg_count * 2 + 11] = rdmsr(0x2ff);

    /* make sure that the saved MTRR default has MTRRs off */
    saved_mtrrs[var_reg_count * 2 + 11] &= ~((uint64_t)1 << 11);
}