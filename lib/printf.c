#include "include.h"

PRIVATE void itoa(u32 value, char **buf_ptr_addr, u8 base)
{
    u32 m = value % base;
    u32 i = value / base;
    if (i) itoa(i, buf_ptr_addr, base);

    if (m < 10) {
        *((*buf_ptr_addr)++) = m + '0';
    } else {
        *((*buf_ptr_addr)++) = m - 10 + 'A';
    }
}

PUBLIC u32 vsprintf(char *str, const char *format, va_list ap)
{
    char *buf_ptr = str;
    const char *index_ptr = format;
    char index_char = *index_ptr;
    int arg_int;
    char *arg_str;

    while (index_char) {
        if (index_char != '%') {
            *(buf_ptr++) = index_char;
            index_char = *(++index_ptr);
            continue;
        }
        index_char = *(++index_ptr);
        switch (index_char) {
            case 'x':
                arg_int = va_arg(ap, int);
                itoa(arg_int, &buf_ptr, 16);
                index_char = *(++index_ptr);
                break;

            case 'c':
                *(buf_ptr++) = va_arg(ap, char);
                index_char = *(++index_ptr);
                break;

            case 'd':
                arg_int = va_arg(ap, int);
                if (arg_int < 0) {
                    arg_int = -arg_int;
                    *buf_ptr++ = '-';
                }
                itoa(arg_int, &buf_ptr, 10);
                index_char = *(++index_ptr);
                break;
            case 's':
                arg_str = va_arg(ap, char*);
                strcpy(buf_ptr, arg_str);
                buf_ptr += strlen(arg_str);
                index_char = *(++index_ptr);
                break;
        }
    }
    
    return strlen(str);
}

PUBLIC u32 sprintf(char *s, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int i = vsprintf(s, format, args);
    va_end(args);
    return i;
}

PUBLIC u32 printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char buf[1024] = {0};
    vsprintf(buf, format, args);
    va_end(args);
    return write(buf);
}