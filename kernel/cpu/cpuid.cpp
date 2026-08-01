#include <stdint.h>

/**
 *
 * @brief CPUID
 *
 * @param mop 格式（0=CPU厂商 1=CPU参数）
 *
 */
void asm_cpuid(uint32_t mop, uint32_t sop, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid \n\t" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) : "0"(mop), "2"(sop) : "memory");
}

/**
 * @brief 获取CPU型号
 *
 * @param c 用于存储型号的字符串
 */
void get_cpu_name(char *c)
{
    uint32_t *v = (uint32_t *)(c);
    asm_cpuid(0x80000002, 0, &v[0], &v[1], &v[2], &v[3]);
    asm_cpuid(0x80000003, 0, &v[4], &v[5], &v[6], &v[7]);
    asm_cpuid(0x80000004, 0, &v[8], &v[9], &v[10], &v[11]);
    c[47] = '\0';
}

void get_cpu_vendor(char *c)
{
    uint32_t *v = (uint32_t *)(c);
    uint32_t zero;
    asm_cpuid(0, 0, &zero, &v[0], &v[2], &v[1]);
    c[12] = '\0';
}

bool mtrr_supported()
{
    uint32_t eax, ebx, ecx, edx;
    asm_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    return !!(edx & (1 << 12));
}