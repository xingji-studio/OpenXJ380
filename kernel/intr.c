#include "include.h"
//意义暂时不明
PRIVATE void init_idt_desc(u8 vector, u8 desc_type, int_handler handler, u8 privilege)
{
    GATE *p_gate = &idt[vector];
    u32   base   = (u32) handler;

    p_gate->offset_low  = base & 0xFFFF;
    p_gate->selector    = SELECTOR_KERNEL_CS;
    p_gate->dcount      = 0;
    p_gate->attr        = desc_type | (privilege << 5);
    p_gate->offset_high = (base >> 16) & 0xFFFF;
}
//中断登记
PRIVATE void init_interruptions()
{
    init_8259A();

    init_idt_desc(INT_VECTOR_DIVIDE,       DA_386IGate, divide_error,          PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_DEBUG,        DA_386IGate, single_step_exception, PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_NMI,          DA_386IGate, nmi,                   PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_BREAKPOINT,   DA_386IGate, breakpoint_exception,  PRIVILEGE_USER);
    init_idt_desc(INT_VECTOR_OVERFLOW,     DA_386IGate, overflow,              PRIVILEGE_USER);
    init_idt_desc(INT_VECTOR_BOUNDS,       DA_386IGate, bounds_check,          PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_INVAL_OP,     DA_386IGate, inval_opcode,          PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_COPROC_NOT,   DA_386IGate, copr_not_available,    PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_DOUBLE_FAULT, DA_386IGate, double_fault,          PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_COPROC_SEG,   DA_386IGate, copr_seg_overrun,      PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_INVAL_TSS,    DA_386IGate, inval_tss,             PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_SEG_NOT,      DA_386IGate, segment_not_present,   PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_STACK_FAULT,  DA_386IGate, stack_exception,       PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_PROTECTION,   DA_386IGate, general_protection,    PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_PAGE_FAULT,   DA_386IGate, page_fault,            PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_COPROC_ERR,   DA_386IGate, copr_error,            PRIVILEGE_KRNL);

    init_idt_desc(INT_VECTOR_IRQ0 + 0,     DA_386IGate, hwint00,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ0 + 1,     DA_386IGate, hwint01,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ0 + 2,     DA_386IGate, hwint02,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ0 + 3,     DA_386IGate, hwint03,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ0 + 4,     DA_386IGate, hwint04,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ0 + 5,     DA_386IGate, hwint05,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ0 + 6,     DA_386IGate, hwint06,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ0 + 7,     DA_386IGate, hwint07,               PRIVILEGE_KRNL);

    init_idt_desc(INT_VECTOR_IRQ8 + 0,     DA_386IGate, hwint08,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ8 + 1,     DA_386IGate, hwint09,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ8 + 2,     DA_386IGate, hwint10,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ8 + 3,     DA_386IGate, hwint11,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ8 + 4,     DA_386IGate, hwint12,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ8 + 5,     DA_386IGate, hwint13,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ8 + 6,     DA_386IGate, hwint14,               PRIVILEGE_KRNL);
    init_idt_desc(INT_VECTOR_IRQ8 + 7,     DA_386IGate, hwint15,               PRIVILEGE_KRNL);

    init_idt_desc(INT_VECTOR_SYS_CALL,     DA_386IGate, sys_call,              PRIVILEGE_USER);
}
//中断相关
PUBLIC void init_idt()
{
    u16 *p_idt_limit = (u16 *) (&idt_ptr[0]);
    u32 *p_idt_base  = (u32 *) (&idt_ptr[2]);

    *p_idt_limit = IDT_SIZE * sizeof(GATE) - 1;
    *p_idt_base  = (u32) &idt;

    init_interruptions();
}
//30分自制异常系统
PUBLIC void exception_handler(int vec_no, int err_code, int eip, int cs, int eflags)
{
    disable_int();
    
    int i;

    char *err_msg[] = {
        "#DE Divide Error",
        "#DB RESERVED",
        "--  NMI Interrupt",
        "#BP Breakpoint",
        "#OF Overflow",
        "#BR Bound Range Exceeded",
        "#UD Invalid Opcode (Undefined Opcode)",
        "#NM Device Not Available (No Math Coprocessor)",
        "#DF Double Fault",
        "    Coprocessor Segment Overrun (reserved)",
        "#TS Invalid TSS",
        "#NP Segment Not Present",
        "#SS Stack Segment Fault",
        "#GP General Protection",
        "#PF Page Fault",
        "--  (Intel reserved. Do not use.)",
        "#MF x87 FPU Floating-Point Error (Math Fault)",
        "#AC Alignment Check",
        "#MC Machine Check",
        "#XF SIMD Floating-Point Exception"
    };

    draw_rect(0, 0, binfo->scrnx - 1, binfo->scrny - 1, 0xffffff);
    setX(0); setY(0);

    put_str("Exception! --> ");
    put_str(err_msg[vec_no]);
    put_str("\n\nEFLAGS: ");
    put_int(eflags);
    put_str("\nCS: ");
    put_int(cs);
    put_str("\nEIP: ");
    put_int(eip);

    if (err_code != 0xffffffff) {
        put_str("\nError code: ");
        put_int(err_code);
    }
}