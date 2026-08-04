#include "../../kernel/build_settings.h"
#include "krlibc.h" // For memset
#include "mm/alloc/alloc.h"
#include "stdarg.h"
#include <console.h>
#include <cpu/lock.h>
#include <proto.hpp>
#include <dlinker.h>

spin_t serial_lock;
spin_t fmt_lock; // For write_serial_fmt

#define PORT 0x3f8 // COM1

int init_serial()
{
    serial_lock = SPIN_INIT;
    fmt_lock    = SPIN_INIT;

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

void write_serial_string(const char *str)
{
    if (str == NULL) {
        return;
    }

    spin_lock(&serial_lock);
    console_write(str);
    while (*str)
    {
        write_serial(*str++);
    }
    spin_unlock(&serial_lock);
}

void write_serial_dec(unsigned long long dec)
{
    char     str[255];
    int      radix   = 10;
    char     index[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"; // 索引表
    unsigned unum; // 存放要转换的整数的绝对值,转换的整数可能是负数
    int      i = 0, j,
        k; // i用来指示设置字符串相应位，转换之后i其实就是字符串的长度；转换后顺序是逆序的，有正负的情况，k用来指示调整顺序的开始位置;j用来指示调整顺序时的交换。

    // 获取要转换的整数的绝对值
    if (radix == 10 && dec < 0) // 要转换成十进制数并且是负数
    {
        unum     = (unsigned)-dec; // 将num的绝对值赋给unum
        str[i++] = '-';            // 在字符串最前面设置为'-'号，并且索引加1
    }
    else
        unum = (unsigned)dec; // 若是num为正，直接赋值给unum

    // 转换部分，注意转换后是逆序的
    do
    {
        str[i++] = index[unum % (unsigned)radix]; // 取unum的最后一位，并设置为str对应位，指示索引加1
        unum /= radix;                            // unum去掉最后一位

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

    write_serial_string(str);
}

void write_serial_hex(unsigned long long hex)
{
    // 输出16进制
    char  buf[32];
    char *p = buf;
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

    write_serial_string(buf);
}

typedef enum num_size
{
    HALF_2 = 0, // char
    HALF_1 = 1, // short
    INT    = 2, // int
    LONG_1 = 3, // long
    LONG_2 = 4, // long long
    SIZE_T = 5, // size_t
} num_size_t;

size_t wfmt_arg(Writer *writer, const char **fmt_ptr, va_list args) //NOLINT // `fmt_ptr` is a pointer to `fmt`
{
    char        *str           = 0; // for `%s`
    size_t       write_counter = 0;
    size_t       str_len       = 0; // for align `%s`
    size_t       result        = 0; // Same as write_counter?
    WriteHandler write         = writer->handler;

    num_formatter_t num_fmter = {};
    num_fmt_type    num_flag  = {};
    int64_t         size_cnt  = INT;

    // Error args
    if (!writer || !write || !fmt_ptr || !(*fmt_ptr) || **fmt_ptr != '%') { return 0; }

    while (true)
    {
        ++(*fmt_ptr); // Skip '%' or any flags
        switch (**fmt_ptr)
        {
        case '-': num_flag.left = true; break;
        case '+': num_flag.plus = true; break;
        case ' ': num_flag.space = true; break;
        case '#': num_flag.special = true; break;
        case '0': num_flag.zeropad = true; break;
        default: break;
        }

        /* Calc num_fmter.size */
        if (IS_DIGIT(**fmt_ptr)) { num_fmter.size = skip_atoi(fmt_ptr); }
        else if (**fmt_ptr == '*')
        {
            /* by the following argument */
            ++(*fmt_ptr); // Skip '*'
            num_fmter.size = (size_t)va_arg(args, int);
        }

        /* Calc num_fmter.precision */
        if (**fmt_ptr == '.')
        {
            ++(*fmt_ptr); // Skip '.'
            if (IS_DIGIT(**fmt_ptr)) { num_fmter.precision = skip_atoi(fmt_ptr); }
            else if (**fmt_ptr == '*')
            {
                /* by the following argument */
                ++(*fmt_ptr); // Skip '*'
                num_fmter.precision = (size_t)va_arg(args, int);
            }
        }

        /* Calc size_cnt */
        switch (**fmt_ptr)
        {
        case 'h':
            size_cnt--;
            if (size_cnt < HALF_2) size_cnt = HALF_2; // hh
            continue;
        case 'L':       // += 2
            size_cnt++; // fallthrough
        case 'l':
            size_cnt++;
            if (size_cnt > LONG_2) size_cnt = LONG_2; // ll
            continue;
        case 'z':
            size_cnt = SIZE_T; // z
            continue;
        default: break;
        }

        /* Read argument */
        switch (**fmt_ptr)
        {
        case 'c': num_fmter.num = va_arg(args, int); break;
        case 's':
            str                    = va_arg(args, char *);
            static char null_str[] = "(null)";
            if (str == 0) str = null_str;
            break;
        case 'd':
        case 'i':
            // NOLINTBEGIN
            switch (size_cnt)
            {
            case HALF_2: num_fmter.num = (size_t)(char)va_arg(args, int); break;
            case HALF_1: num_fmter.num = (size_t)(short)va_arg(args, int); break;
            case INT: num_fmter.num = (size_t)(int)va_arg(args, int); break;
            case LONG_1: num_fmter.num = (size_t)(long)va_arg(args, long); break;
            case LONG_2: num_fmter.num = (size_t)(long long)va_arg(args, long long); break;
            case SIZE_T: // fallthrough
            default: num_fmter.num = va_arg(args, size_t); break;
            }
            // NOLINTEND
            break;
        case 'o':
        case 'x':
        case 'X':
        case 'b':
        case 'u':
            switch (size_cnt)
            {
            case HALF_2: num_fmter.num = (size_t)(unsigned char)va_arg(args, int); break;
            case HALF_1: num_fmter.num = (size_t)(unsigned short)va_arg(args, int); break;
            case INT: num_fmter.num = (size_t)(unsigned int)va_arg(args, int); break;
            case LONG_1: num_fmter.num = (size_t)(unsigned long)va_arg(args, long); break;
            case LONG_2: num_fmter.num = (size_t)(unsigned long long)va_arg(args, long long); break;
            case SIZE_T: // fallthrough
            default: num_fmter.num = va_arg(args, size_t); break;
            }
            break;
        case 'p': num_fmter.num = (size_t)va_arg(args, void *); break;
        default: // may no data
            break;
        }

        /* Calc length of `%s` and set num_flag */
        switch (**fmt_ptr)
        {
        case 'c': break;
        case 's':
            str_len = strlen(str);
            if (num_fmter.size < str_len) num_fmter.size = str_len;
            break;
        case 'o': num_fmter.base = 8; break;
        case 'p':
            num_flag.small   = true;
            num_flag.special = true;
            num_flag.zeropad = true;
            if (num_fmter.size < 16) num_fmter.size = 16;
            num_fmter.base = 16;
            break;
        case 'x': num_flag.small = true; // fallthrough
        case 'X': num_fmter.base = 16; break;
        case 'd':
        case 'i': num_flag.sign = true;
        case 'u': num_fmter.base = 10; break;
        case 'b': num_fmter.base = 2; break;
        case 'n': va_arg(args, void *); break;
        case '%': break;
        default:
            // Unexpected
            return 0;
        }

        /* Write to arg space */
        switch (**fmt_ptr)
        {
        case 'c':
        {
            size_t char_pad = num_fmter.size > 1 ? (num_fmter.size - 1) : 0;
            // Right align
            if (!(num_flag.left))
            {
                while (write_counter < char_pad)
                {
                    write(writer, ' ');
                    ++write_counter;
                }
            }
            // Write char
            write(writer, (char)num_fmter.num);
            // Left align
            if (num_flag.left)
            {
                while (write_counter < char_pad + 1)
                {
                    write(writer, ' ');
                    ++write_counter;
                }
            }
            break;
        }
        case 's':
            // Right align
            if (!(num_flag.left))
            {
                while (write_counter < num_fmter.size - str_len)
                {
                    write(writer, ' ');
                    ++write_counter;
                }
                str_len = num_fmter.size;
            }
            // Write string
            while (write_counter < str_len)
            {
                write(writer, *str);
                ++str;
                ++write_counter;
            }
            // Left align
            if (num_flag.left)
            {
                while (write_counter < num_fmter.size - str_len)
                {
                    write(writer, ' ');
                    ++write_counter;
                }
            }
            break;
        case 'o':                                                               // fallthrough
        case 'p':                                                               // fallthrough
        case 'x':                                                               // fallthrough
        case 'X':                                                               // fallthrough
        case 'd':                                                               // fallthrough
        case 'i':                                                               // fallthrough
        case 'u':                                                               // fallthrough
        case 'b': write_counter += wnumber(writer, num_fmter, num_flag); break; // Format number with `Writer`
        case '%': write(writer, '%'); break;
        default: break;
        }
        break;
    }
    result = write_counter;
    return result;
}

/**
 * Use a `writer` to write formatted string
 */
size_t vwprintf(Writer *writer, const char *fmt, va_list args)
{
    const char  *fmt_ptr = fmt;
    WriteHandler write   = writer->handler;
    size_t       result  = 0;
    // TODO
    while (*fmt_ptr != '\0')
    {
        if (*fmt_ptr != '%')
        {
            write(writer, *fmt_ptr); // TODO: Catch Error
            fmt_ptr++;
            result++;
            continue;
        }
        // *fmt_ptr == '%'
        result += wfmt_arg(writer, &fmt_ptr, args);
        fmt_ptr++;
    }
    return result;
}

#define SERIAL_WRITE_BUFFER_SIZE 1024
char   serial_write_buffer[SERIAL_WRITE_BUFFER_SIZE] = {0};
size_t serial_write_buffer_index                     = 0;

uint8_t serial_write_handler(Writer *writer, char ch)
{
    (void)writer;
    serial_write_buffer[serial_write_buffer_index] = ch;
    ++serial_write_buffer_index;
    if (serial_write_buffer_index >= SERIAL_WRITE_BUFFER_SIZE - 1) // 1 for '\0'
    {
        write_serial_string(serial_write_buffer);
        serial_write_buffer_index = 0;
    } // Flush buffer
    serial_write_buffer[serial_write_buffer_index] = '\0';
    return 0;
}

Writer SerialWriter = {
    .data    = serial_write_buffer,
    .handler = serial_write_handler,
};

void serial_wprintf(const char *fmt, ...)
{
    spin_lock(&fmt_lock);
    va_list args;
    va_start(args, fmt);
    vwprintf(&SerialWriter, fmt, args);
    // flush buffer
    if (serial_write_buffer_index > 0)
    {
        write_serial_string(serial_write_buffer);
        serial_write_buffer_index = 0;
    }
    va_end(args);
    spin_unlock(&fmt_lock);
}

/**
 * Needn't heap now!!!
 */
int write_serial_fmt(const char *fmt, ...)
{
    spin_lock(&fmt_lock);
    va_list args;
    va_start(args, fmt);
    vwprintf(&SerialWriter, fmt, args);
    // flush buffer
    if (serial_write_buffer_index > 0)
    {
        write_serial_string(serial_write_buffer);
        serial_write_buffer_index = 0;
    }
    va_end(args);
    spin_unlock(&fmt_lock);
    return 0;
}

// 用于输出消息
int printk(const char *fmt, ...)
{
    spin_lock(&fmt_lock);

    write_serial_string("[XJ380 System Kernel][MESSAGE] ");

    va_list args;
    va_start(args, fmt);
    vwprintf(&SerialWriter, fmt, args);
    // flush buffer
    if (serial_write_buffer_index > 0)
    {
        write_serial_string(serial_write_buffer);
        serial_write_buffer_index = 0;
    }
    va_end(args);
    spin_unlock(&fmt_lock);
    return 0;
}

EXPORT_SYMBOL(printk);

// 用于输出调试消息
int pr_debug(const char *fmt, ...)
{
    spin_lock(&fmt_lock);

    write_serial_string("[XJ380 System Kernel][DEBUG] ");

    va_list args;
    va_start(args, fmt);
    vwprintf(&SerialWriter, fmt, args);
    // flush buffer
    if (serial_write_buffer_index > 0)
    {
        write_serial_string(serial_write_buffer);
        serial_write_buffer_index = 0;
    }
    va_end(args);
    spin_unlock(&fmt_lock);
    return 0;
}
EXPORT_SYMBOL(pr_debug);

// 用于输出警告消息
int pr_warn(const char *fmt, ...)
{
    spin_lock(&fmt_lock);

    write_serial_string("[XJ380 System Kernel][WARNING] ");

    va_list args;
    va_start(args, fmt);
    vwprintf(&SerialWriter, fmt, args);
    // flush buffer
    if (serial_write_buffer_index > 0)
    {
        write_serial_string(serial_write_buffer);
        serial_write_buffer_index = 0;
    }
    va_end(args);
    spin_unlock(&fmt_lock);
    return 0;
}
EXPORT_SYMBOL(pr_warn);

// 用于输出调试信息消息
int pr_info(const char *fmt, ...)
{
    spin_lock(&fmt_lock);

    write_serial_string("[XJ380 System Kernel][INFO] ");

    va_list args;
    va_start(args, fmt);
    vwprintf(&SerialWriter, fmt, args);
    // flush buffer
    if (serial_write_buffer_index > 0)
    {
        write_serial_string(serial_write_buffer);
        serial_write_buffer_index = 0;
    }
    va_end(args);
    spin_unlock(&fmt_lock);
    return 0;
}
EXPORT_SYMBOL(pr_info);

// 用于输出提示消息
int pr_notice(const char *fmt, ...)
{
    spin_lock(&fmt_lock);

    write_serial_string("[XJ380 System Kernel][NOTICE] ");

    va_list args;
    va_start(args, fmt);
    vwprintf(&SerialWriter, fmt, args);
    // flush buffer
    if (serial_write_buffer_index > 0)
    {
        write_serial_string(serial_write_buffer);
        serial_write_buffer_index = 0;
    }
    va_end(args);
    spin_unlock(&fmt_lock);
    return 0;
}
EXPORT_SYMBOL(pr_notice);

// 用于输出错误消息
int pr_err(const char *fmt, ...)
{
    spin_lock(&fmt_lock);

    write_serial_string("[XJ380 System Kernel][ERROR] ");

    va_list args;
    va_start(args, fmt);
    vwprintf(&SerialWriter, fmt, args);
    // flush buffer
    if (serial_write_buffer_index > 0)
    {
        write_serial_string(serial_write_buffer);
        serial_write_buffer_index = 0;
    }
    va_end(args);
    spin_unlock(&fmt_lock);
    return 0;
}



typedef struct UnsafeBufWriterData
{
    size_t idx; // index of buffer
    char  *buf; // Unsafe, because unknown size
} UnsafeBufWriterData;

uint8_t unsafe_buf_writer_handler(Writer *writer, char ch)
{
    UnsafeBufWriterData *data = (UnsafeBufWriterData *)writer->data;
    data->buf[data->idx]      = ch;
    ++(data->idx);
    data->buf[data->idx] = '\0'; // EOF (Must do, because the buffer may be full of dirty datas)
    return 0;                    // Right... Always success...?
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    UnsafeBufWriterData data = {
        .idx = 0,
        .buf = buf,
    };
    Writer UnsafeBufWriter = {
        .data    = &data,
        .handler = unsafe_buf_writer_handler,
    };
    size_t result = vwprintf(&UnsafeBufWriter, fmt, args);
    va_end(args);
    return (int)result; // Why use int??
}

EXPORT_SYMBOL(sprintf);
/**
 * 安全版本的 sprintf，限制输出字符数防止缓冲区溢出
 * 
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 * @param fmt 格式字符串
 * @param ... 可变参数
 * @return 实际应该写入的字符数（不包括终止空字符）
 */
// 使用安全的缓冲区写入器
typedef struct SafeBufWriterData
{
    size_t idx;       // 当前写入位置
    char  *buf;       // 输出缓冲区
    size_t size;      // 缓冲区总大小
    bool   truncated; // 是否发生了截断
} SafeBufWriterData;
uint8_t safe_buf_writer_handler(Writer *writer, char ch)
{
    SafeBufWriterData *data = (SafeBufWriterData *)writer->data;

    // 检查是否还有空间（为终止空字符保留一个位置）
    if (data->idx < data->size - 1)
    {
        data->buf[data->idx] = ch;
        ++(data->idx);
        data->buf[data->idx] = '\0'; // 始终保持以空字符结尾
    }
    else
    {
        // 缓冲区已满，标记截断但不再写入
        data->truncated = true;
    }
    return 0;
}
int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    if (size == 0)
    {
        return 0; // 缓冲区大小为0，直接返回
    }

    va_list args;
    va_start(args, fmt);

    SafeBufWriterData data = {.idx = 0, .buf = buf, .size = size, .truncated = false};

    Writer SafeBufWriter = {
        .data    = &data,
        .handler = safe_buf_writer_handler,
    };

    // 确保缓冲区初始为空字符串
    if (size > 0) { buf[0] = '\0'; }

    // 执行格式化输出
    size_t result = vwprintf(&SafeBufWriter, fmt, args);

    va_end(args);

    // 如果发生了截断，返回应该写入的字符数（不包括终止空字符）
    // 否则返回实际写入的字符数（不包括终止空字符）
    return (int)(data.truncated ? result : data.idx);
}
EXPORT_SYMBOL(snprintf);
/**
 * Needn't heap now!!!
 */
void write_serial_fmt_heapless(const char *fmt, ...) // same as write_serial_fmt
{
    spin_lock(&fmt_lock);
    va_list args;
    va_start(args, fmt);
    vwprintf(&SerialWriter, fmt, args);
    // flush buffer
    if (serial_write_buffer_index > 0)
    {
        write_serial_string(serial_write_buffer);
        serial_write_buffer_index = 0;
    }
    va_end(args);
    spin_unlock(&fmt_lock);
}

EXPORT_SYMBOL(write_serial_fmt);
EXPORT_SYMBOL(write_serial_string);
