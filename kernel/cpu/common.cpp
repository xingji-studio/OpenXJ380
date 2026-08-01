#include <stdint.h>

// 端口写（8位）
void outb(uint16_t port, uint8_t value)
{
    asm volatile("outb %1, %0" : : "dN"(port), "a"(value));
}

// 端口读（8位）
uint8_t inb(uint16_t port)
{
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

// 端口写（16位）
void outw(uint16_t port, uint16_t value)
{
    asm volatile("outw %1, %0" : : "dN"(port), "a"(value));
}

// 端口读（16位）
uint16_t inw(uint16_t port)
{
    uint16_t ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

// 端口写（32位）
void outl(uint16_t port, uint32_t value)
{
    asm volatile("outl %1, %0" : : "dN"(port), "a"(value));
}

// 端口读（32位）
uint32_t inl(uint16_t port)
{
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

// 从I/O端口批量地读取数据到内存（16位）
void insw(uint16_t port, void *buf, unsigned long n)
{
    asm volatile("cld; rep; insw" : "+D"(buf), "+c"(n) : "d"(port));
}

// 从内存批量地写入数据到I/O端口（16位）
void outsw(uint16_t port, const void *buf, unsigned long n)
{
    asm volatile("cld; rep; outsw" : "+S"(buf), "+c"(n) : "d"(port));
}

// 从I/O端口批量地读取数据到内存（32位）
void insl(uint32_t port, void *addr, int cnt)
{
    asm volatile("cld;"
                 "repne; insl;"
                 : "=D"(addr), "=c"(cnt)
                 : "d"(port), "0"(addr), "1"(cnt)
                 : "memory", "cc");
}

// 从内存批量地写入数据到I/O端口（32位）
void outsl(uint32_t port, const void *addr, int cnt)
{
    asm volatile("cld;"
                 "repne; outsl;"
                 : "=S"(addr), "=c"(cnt)
                 : "d"(port), "0"(addr), "1"(cnt)
                 : "memory", "cc");
}

// 复活中断
void enable_intr()
{
    asm volatile("sti" ::: "memory");
}

// 谋害中断
void disable_intr()
{
    asm volatile("cli" ::: "memory");
}

// MFENCE
void io_mfence()
{
    __asm__ volatile("mfence	\n\t" ::: "memory");
}