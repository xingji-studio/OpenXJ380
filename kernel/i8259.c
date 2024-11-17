#include "include.h"

PUBLIC void init_8259A()
{
    out_byte(INT_M_CTL, 0x11);
    out_byte(INT_S_CTL, 0x11);

    out_byte(INT_M_CTLMASK, INT_VECTOR_IRQ0);
    out_byte(INT_S_CTLMASK, INT_VECTOR_IRQ8);

    out_byte(INT_M_CTLMASK, 0x4);
    out_byte(INT_S_CTLMASK, 0x2);

    out_byte(INT_M_CTLMASK, 0x1);
    out_byte(INT_S_CTLMASK, 0x1);

    out_byte(INT_M_CTLMASK, 0xFF);
    out_byte(INT_S_CTLMASK, 0xFF);

    int i;
    for (i = 0; i < NR_IRQ; i++) irq_table[i] = spurious_irq;
}

PUBLIC void spurious_irq(int irq)
{
    put_str("\nspurious_irq: ");
    put_int(irq);
    put_str("\n");
}

PUBLIC void put_irq_handler(int irq, irq_handler handler)
{
    disable_irq(irq);
    irq_table[irq] = handler;
}