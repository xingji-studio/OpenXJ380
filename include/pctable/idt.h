#ifndef _IDT_H_
#define _IDT_H_

#define IDT_SIZE         256
#define X86_64_INTR_GATE 0x8E
#define X86_64_TRAP_GATE 0x8F

#ifdef __cplusplus
extern "C" {
#endif
// 除法错误
void divide_error();
// 调试
void debug();
// 不可屏蔽中断
void nmi();
//
void int3();
// 溢出
void overflow();
// 边界问题
void bounds();
// 未定义的操作数
void undefined_opcode();
// 设备不可用
void dev_not_avaliable();
void double_fault();
void coprocessor_segment_overrun();
void invalid_TSS();
void segment_not_exists();
void stack_segment_fault();
void general_protection();
// 缺页异常
void page_fault();
void x87_FPU_error();
void alignment_check();
void machine_check();
void SIMD_exception();
void virtualization_exception();

void keyboard_handler();
void mouse_handler();

void syscall_handler();

void nv_handler();

void sb16_interr();

// 默认处理程序
void undefined_interrupt();

void hda_interrupt();

#ifdef __cplusplus
}
#endif

// 中断入口
typedef void (*int_func)();

#include <stdint.h>

typedef struct
{
    uint16_t size; // 大小，单位字节
    uint64_t base; // IDT的地址（不是物理地址，但是XJ380是1：1页表好像也没啥区别）
} __attribute__((packed)) IDTR_TYPE;

typedef struct
{
    uint16_t offset_1; // 地址偏移量的第0~15位，BaseAddress + Offset = 函数地址
    uint16_t selector; // 指向GDT的一个代码段
    uint8_t  ist; // Interrupt Stack Table的偏移量，存储在Task State Segment中。全部为零则不使用IST。
    uint8_t
        type_attributes; // 门类型，DPL和Present位。详见https://wiki.osdev.org/Interrupt_Descriptor_Table#Structure_on_x86-64。
    uint16_t offset_2; // 地址偏移量的第16~31位
    uint32_t offset_3; // 地址偏移量的第32~63位
    uint32_t zero;     // 保留
} __attribute__((packed)) IDT_GATE_TYPE;

#endif
