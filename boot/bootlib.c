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

char *Hex2Char(unsigned long long hex)
{
    // 输出16进制
    char  buf[32];
    char *p  = buf;
    char *re = buf;
    char  ch;
    int   i, flag = 0;

    *p++ = '0';
    *p++ = 'x'; // 先存一个0x

    if (hex == 0)
        *p++ = '0'; // 如果是0，直接0x0趋势
    else
    {
        for (i = 60; i >= 0; i -= 4)
        {                          // 每次4位
            ch = (hex >> i) & 0xF; // 0~9, A~F
            // 28（冗余）
            if (flag || ch > 0)
            {                // 跳过前导0
                flag  = 1;   // 没有前导0就把flag设为1，这样后面再有0也不会忽略
                ch   += '0'; // 0~9 => '0'~'9'
                if (ch > '9')
                {
                    ch += 7; // 'A' - '9' = 7
                }
                *p++ = ch; // 写入
            }
        }
    }
    *p = '\0'; // 结束符
    return re;
}

char *Dec2Char(unsigned long long dec)
{
    char     str[255];
    char    *re      = str;
    int      radix   = 10;
    char     index[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"; // 索引表
    unsigned unum;                                             // 存放要转换的整数的绝对值,转换的整数可能是负数
    int      i = 0, j,
        k; // i用来指示设置字符串相应位，转换之后i其实就是字符串的长度；转换后顺序是逆序的，有正负的情况，k用来指示调整顺序的开始位置;j用来指示调整顺序时的交换。

    // 获取要转换的整数的绝对值
    if (radix == 10 && (long long)dec < 0) // 要转换成十进制数并且是负数
    {
        unum     = (unsigned)-dec; // 将num的绝对值赋给unum
        str[i++] = '-';            // 在字符串最前面设置为'-'号，并且索引加1
    }
    else
        unum = (unsigned)dec; // 若是num为正，直接赋值给unum

    // 转换部分，注意转换后是逆序的
    do
    {
        str[i++]  = index[unum % (unsigned)radix]; // 取unum的最后一位，并设置为str对应位，指示索引加1
        unum     /= radix;                         // unum去掉最后一位

    } while (unum); // 直至unum为0退出循环

    str[i] = '\0'; // 在字符串最后添加'\0'字符，c语言字符串以'\0'结束。

    // 将顺序调整过来
    if (str[0] == '-')
        k = 1; // 如果是负数，符号不用调整，从符号后面开始调整
    else
        k = 0; // 不是负数，全部都要调整

    char temp;                         // 临时变量，交换两个值时用到
    for (j = k; j <= (i - 1) / 2; j++) // 头尾一一对称交换，i其实就是字符串的长度，索引最大值比长度少1
    {
        temp               = str[j];             // 头部赋值给临时变量
        str[j]             = str[i - 1 + k - j]; // 尾部赋值给头部
        str[i - 1 + k - j] = temp;               // 将临时变量的值(其实就是之前的头部值)赋给尾部
    }
    return re;
}
