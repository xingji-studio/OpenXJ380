#include "bootlib.h"

// common.cpp

// 端口写（8位）
void outb(UINT16 port, UINT8 value)
{
    asm volatile("outb %1, %0" : : "dN"(port), "a"(value));
}

// 端口读（8位）
UINT8 inb(UINT16 port)
{
    UINT8 ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

// 端口写（16位）
void outw(UINT16 port, UINT16 value)
{
    asm volatile("outw %1, %0" : : "dN"(port), "a"(value));
}

// 端口读（16位）
UINT16 inw(UINT16 port)
{
    UINT16 ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

// 端口写（32位）
void outl(UINT16 port, UINT32 value)
{
    asm volatile("outl %1, %0" : : "dN"(port), "a"(value));
}

// 端口读（32位）
UINT32 inl(UINT16 port)
{
    UINT32 ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

// 从I/O端口批量地读取数据到内存（16位）
void insw(UINT16 port, VOID *buf, unsigned long n)
{
    asm volatile("cld; rep; insw" : "+D"(buf), "+c"(n) : "d"(port));
}

// 从内存批量地写入数据到I/O端口（16位）
void outsw(UINT16 port, const VOID *buf, unsigned long n)
{
    asm volatile("cld; rep; outsw" : "+S"(buf), "+c"(n) : "d"(port));
}

// 从I/O端口批量地读取数据到内存（32位）
void insl(UINT32 port, VOID *addr, int cnt)
{
    asm volatile("cld;"
                 "repne; insl;"
                 : "=D"(addr), "=c"(cnt)
                 : "d"(port), "0"(addr), "1"(cnt)
                 : "memory", "cc");
}

// 从内存批量地写入数据到I/O端口（32位）
void outsl(UINT32 port, const VOID *addr, int cnt)
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
    asm volatile("sti");
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

// serial_port.cpp

int init_serial()
{
    outb(PORT + 1, 0x00); // Disable all interrupts
    outb(PORT + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(PORT + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    outb(PORT + 1, 0x00); //                  (hi byte)
    outb(PORT + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(PORT + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(PORT + 4, 0x0B); // IRQs enabled, RTS/DSR set
    outb(PORT + 4, 0x1E); // Set in loopback mode, test the serial chip
    outb(PORT + 0, 0xAE); // Test serial chip (send byte 0xAE and check if serial returns same byte)

    // Check if serial is faulty (i.e: not same byte as sent)
    if (inb(PORT + 0) != 0xAE)
    {
        // 不支持
        return 1;
    }

    // If serial is not faulty set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
    outb(PORT + 4, 0x0F);
    return 0;
}

int is_transmit_empty()
{
    return inb(PORT + 5) & 0x20;
}

void write_serial(char a)
{
    while (is_transmit_empty() == 0)
        ;

    outb(PORT, a);
}

void write_serial_string(char *str)
{
    while (*str)
    {
        write_serial(*str++);
    }
}

// string.c
char *strcpy(char *dest, const char *src)
{
    // assert(dest != NULL && src != NULL);
    char *r = dest;
    while ((*dest++ = *src++))
        ;
    return r;
}

UINT64 strlen(const char *str)
{
    // assert(str != NULL);
    const char *p = str;
    while (*p++)
        ;
    return p - str - 1;
}

UINT64 strcmp(char *from_str, char *cmp_str)
{
    while ((*from_str != '\0') && (*from_str == *cmp_str))
    {
        from_str++;
        cmp_str++;
    }
    return *from_str - *cmp_str;
}

UINT64 part_strcmp(char *from_str, char *cmp_str, UINT64 size)
{
    char temp[32], from[32];
    for (UINT64 i = 0; i < size; i++)
    {
        temp[i] = from_str[i];
        from[i] = cmp_str[i];
    }
    temp[size] = '\0';
    from[size] = '\0';
    UINT64 ret;
    ret = strcmp(from, temp);
    return ret;
}

void write_serial_hex(UINT64 value)
{
    char buffer[19];
    char *cursor = buffer;

    *cursor++ = '0';
    *cursor++ = 'x';

    UINT8 started = 0;
    for (int shift = 60; shift >= 0; shift -= 4)
    {
        UINT8 digit = (UINT8)((value >> shift) & 0x0f);
        if (digit == 0 && !started && shift != 0) continue;

        started   = 1;
        *cursor++ = (char)(digit < 10 ? '0' + digit : 'A' + digit - 10);
    }

    *cursor = '\0';
    write_serial_string(buffer);
}

void write_serial_dec(UINT64 value)
{
    char buffer[21];
    UINT64 length = 0;

    do
    {
        buffer[length++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);

    for (UINT64 left = 0, right = length - 1; left < right; left++, right--)
    {
        char digit    = buffer[left];
        buffer[left]  = buffer[right];
        buffer[right] = digit;
    }

    buffer[length] = '\0';
    write_serial_string(buffer);
}
