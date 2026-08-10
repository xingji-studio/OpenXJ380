#include <cpu/longm.h>
#include <apic/apic.h>
#include <mm/memory.h>
#include <pctable/gdt.h>
#include <pctable/idt.h>
#include <proto.hpp>

IDT_GATE_TYPE idt_gate[IDT_SIZE] __attribute__((__aligned__(PAGE_SIZE)));
IDTR_TYPE     idt_ptr;

extern void save_registers();

void init_idt_desc(uint8_t number, uint8_t gate_type, int_func function, uint8_t privilege, void *data);

/**
 * @brief 初始化中断描述符表（IDT）
 */
void init_idt()
{
    write_serial_string("Initializing IDT...\n");
    // 初始化IDTR寄存器和门描述符
    uint16_t *p_idt_limit = (uint16_t *)(&idt_ptr.size);
    uint64_t *p_idt_base  = (uint64_t *)(&idt_ptr.base);

    *p_idt_limit = IDT_SIZE * sizeof(IDT_GATE_TYPE) - 1;
    *p_idt_base  = (uint64_t)(&idt_gate);

    for (int i = 0; i < IDT_SIZE; i++)
    {
        init_idt_desc(i, X86_64_INTR_GATE, undefined_interrupt, RING0, NULL);
    }

    init_idt_desc(0, X86_64_INTR_GATE, divide_error, RING0, NULL);
    init_idt_desc(1, X86_64_INTR_GATE, debug, RING0, NULL);
    init_idt_desc(2, X86_64_INTR_GATE, nmi, RING0, NULL);
    init_idt_desc(3, X86_64_TRAP_GATE, int3, RING3, NULL);
    init_idt_desc(4, X86_64_TRAP_GATE, overflow, RING3, NULL);
    init_idt_desc(5, X86_64_INTR_GATE, bounds, RING0, NULL);
    init_idt_desc(6, X86_64_INTR_GATE, undefined_opcode, RING0, NULL);
    init_idt_desc(7, X86_64_INTR_GATE, dev_not_avaliable, RING0, NULL);
    init_idt_desc(8, X86_64_INTR_GATE, double_fault, RING0, NULL);
    init_idt_desc(9, X86_64_INTR_GATE, coprocessor_segment_overrun, RING0, NULL);
    init_idt_desc(10, X86_64_INTR_GATE, invalid_TSS, RING0, NULL);
    init_idt_desc(11, X86_64_INTR_GATE, segment_not_exists, RING0, NULL);
    init_idt_desc(12, X86_64_INTR_GATE, stack_segment_fault, RING0, NULL);
    init_idt_desc(13, X86_64_INTR_GATE, general_protection, RING0, NULL);
    init_idt_desc(14, X86_64_INTR_GATE, page_fault, RING0, NULL);
    init_idt_desc(16, X86_64_INTR_GATE, x87_FPU_error, RING0, NULL);
    init_idt_desc(17, X86_64_INTR_GATE, alignment_check, RING0, NULL);
    init_idt_desc(18, X86_64_INTR_GATE, machine_check, RING0, NULL);
    init_idt_desc(19, X86_64_INTR_GATE, SIMD_exception, RING0, NULL);
    init_idt_desc(20, X86_64_INTR_GATE, virtualization_exception, RING0, NULL);

    init_idt_desc(32, X86_64_INTR_GATE, save_registers, RING0, NULL); // timer
    init_idt_desc(33, X86_64_INTR_GATE, keyboard_handler, RING0, NULL);
    init_idt_desc(34, X86_64_INTR_GATE, mouse_handler, RING0, NULL);
    init_idt_desc(LAPIC_RESCHEDULE_VECTOR, X86_64_INTR_GATE, reschedule_ipi_handler, RING0, NULL);

    __asm__ volatile("lidt %0" : : "m"(idt_ptr));

    write_serial_string("IDT Initialize Success.\n");
}

/**
 * @brief 初始化中断描述符表（IDT）中的一个描述符
 *
 * @param number 中断向量号
 * @param gate_type 门类型（如中断门、陷阱门等）
 * @param function 中断处理函数的指针
 * @param privilege 特权级别（如RING0、RING3等）
 */
void *idt_datas[256];
void  init_idt_desc(uint8_t number, uint8_t gate_type, int_func function, uint8_t privilege, void *data)
{
    IDT_GATE_TYPE *p_gate = &idt_gate[number];
    uint64_t       f_base = (uint64_t)function;

    p_gate->offset_1        = f_base & 0xFFFF;
    p_gate->selector        = SELECTOR_KERNEL_CS;
    p_gate->ist             = 0; // 不使用IST
    p_gate->type_attributes = gate_type | (privilege << 5);
    p_gate->offset_2        = (f_base >> 16) & 0xFFFF;
    p_gate->offset_3        = (f_base >> 32) & 0xFFFFFFFF;
    p_gate->zero            = 0;
    idt_datas[number]       = data;
}
