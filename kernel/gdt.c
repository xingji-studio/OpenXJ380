#include "include.h"

PUBLIC void init_descriptor(DESCRIPTOR *p_desc, u32 base, u32 limit, u16 attribute)
{
    p_desc->limit_low        = limit & 0x0FFFF;
    p_desc->base_low         = base & 0x0FFFF;
    p_desc->base_mid         = (base >> 16) & 0x0FF;
    p_desc->attr1            = attribute & 0xFF;
    p_desc->limit_high_attr2 = ((limit >> 16) & 0x0F) | (attribute >> 8) & 0xF0;
    p_desc->base_high        = (base >> 24) & 0x0FF;
}

PUBLIC u32 seg2phys(u16 seg)
{
    DESCRIPTOR *p_dest = &gdt[seg >> 3];
    return ((p_dest->base_high << 24) | (p_dest->base_mid << 16) | (p_dest->base_low));
}

PUBLIC void init_gdt_descriptors()
{
    int i;
    PROCESS *p_proc  = proc_table;
    u16 selector_ldt = INDEX_LDT_FIRST << 3;

    for (i = 0; i < NR_TASKS; i++) {
        init_descriptor(&gdt[selector_ldt >> 3],
            vir2phys(seg2phys(SELECTOR_KERNEL_DS), p_proc->ldts),
            LDT_SIZE * sizeof(DESCRIPTOR) - 1,
            DA_LDT);
        
        p_proc++;
        selector_ldt += 1 << 3;
    }
    
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = SELECTOR_KERNEL_DS;
    init_descriptor(&gdt[INDEX_TSS],
        vir2phys(seg2phys(SELECTOR_KERNEL_DS), &tss),
        sizeof(tss) - 1,
        DA_386TSS);
    tss.iobase = sizeof(tss);
}

PUBLIC void init_gdt()
{
    memcpy(&gdt,
            (void *) (*((u32 *) &gdt_ptr[2])),
            *((u16 *) (&gdt_ptr[0])) + 1
            );
    
    u16 *p_gdt_limit = (u16 *) (&gdt_ptr[0]);
    u32 *p_gdt_base  = (u32 *) (&gdt_ptr[2]);

    *p_gdt_limit = GDT_SIZE * sizeof(DESCRIPTOR) - 1;
    *p_gdt_base  = (u32) &gdt;

    init_gdt_descriptors();
}