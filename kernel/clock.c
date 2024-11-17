#include "include.h"

PUBLIC void clock_handler(int irq)  //意义暂时不明
{
    ticks++;
    p_proc_ready->ticks--;

    if (k_reenter != 0) return;

    if (p_proc_ready->ticks > 0) return;

    schedule();
}

PUBLIC void milli_delay(int ms) //sleep
{
    int t = get_ticks();
    while (((get_ticks() - t) * 1000 / HZ) < ms);
}

PUBLIC void Sleep(int ms) //Windows也有这个玩意
{
    int t = get_ticks();
    while (((get_ticks() - t) * 1000 / HZ) < ms);
}

PUBLIC void init_clock()        //初始化时钟中断
{
    put_irq_handler(CLOCK_IRQ, clock_handler);
    enable_irq(CLOCK_IRQ);

    out_byte(TIMER_MODE, RATE_GENERATOR);
    out_byte(TIMER0,     (u8) (TIMER_FREQ / HZ));
    out_byte(TIMER0,     (u8) ((TIMER_FREQ / HZ) >> 8));
}