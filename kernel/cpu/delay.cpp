#include <stdint.h>
#include <dlinker.h>
// 读取TSC
static inline uint64_t rdtsc()
{
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

// 简单延时函数（基于循环）
void delay(uint32_t count)
{
    for (volatile uint32_t i = 0; i < count; i++)
        ;
}

// 获取CPU频率
uint64_t get_cpu_freq_mhz()
{
    uint64_t start, end;
    start = rdtsc();
    delay(1000000); // 延时一段时间
    end = rdtsc();
    return (end - start) / 1000000; // 返回TSC差值，即CPU周期数
}

extern uint64_t nanoTime();

void delay_ns(uint64_t ns)
{
    uint64_t old_t = nanoTime();
    while (nanoTime() - old_t < ns)
    {
        continue;
    }
}
void delay_us_hp(uint64_t us)
{
    delay_ns(us * 1000);
}

void delay_ms_hp(uint64_t ms)
{
    delay_us_hp(ms * 1000);
}

EXPORT_SYMBOL(delay_ms_hp);

void delay_s_hp(uint64_t s)
{
    for (uint64_t i = 0; i < s; i++)
    {
        delay_ms_hp(1000);
    }
}

// 微秒级延时
void delay_us(uint64_t us)
{
    delay_us_hp(us);
    // static uint64_t freq_mhz = get_cpu_freq_mhz();
    // uint64_t        ticks    = us * freq_mhz; // 需要等待的CPU周期数
    // uint64_t        start    = rdtsc();
    // uint64_t        end      = start + ticks;

    // // 处理计数器溢出的情况
    // if (end < start)
    // {
    //     while (rdtsc() >= start)
    //     {
    //         __asm__ volatile("nop");
    //     }
    // }

    // while (rdtsc() < end)
    // {
    //     __asm__ volatile("nop");
    // }
}

// 毫秒级延时
void delay_ms(uint64_t ms)
{
    // delay_us(ms * 1000);
    delay_ms_hp(ms);
}
// 秒级延时
void delay_s(uint64_t s)
{
    // for (uint64_t i = 0; i < s; i++)
    // {
    //     delay_ms(1000);
    // }
    delay_s_hp(s);
}
