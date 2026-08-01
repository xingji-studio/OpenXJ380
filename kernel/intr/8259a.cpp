#include <proto.hpp>

#define INT_M_CTL       0x20
#define INT_M_CTLMASK   0x21
#define INT_S_CTL       0xA0
#define INT_S_CTLMASK   0xA1
#define INT_VECTOR_IRQ0 0x20
#define INT_VECTOR_IRQ8 0x28

/**
 * @brief 初始化8259A中断控制器
 */
void init_8259A()
{
    outb(INT_M_CTL, 0x11);
    outb(INT_S_CTL, 0x11);

    outb(INT_M_CTLMASK, INT_VECTOR_IRQ0);
    outb(INT_S_CTLMASK, INT_VECTOR_IRQ8);

    outb(INT_M_CTLMASK, 0x4);
    outb(INT_S_CTLMASK, 0x2);

    outb(INT_M_CTLMASK, 0x1);
    outb(INT_S_CTLMASK, 0x1);

    outb(INT_M_CTLMASK, 0xFF);
    outb(INT_S_CTLMASK, 0xFF);
}

/**
 * @brief 禁用8259A中断控制器
 */
void disable_8259A()
{
    outb(INT_M_CTLMASK, 0xff);
    outb(INT_S_CTLMASK, 0xff);
}
