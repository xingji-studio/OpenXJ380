// XJ380 User-space C Library - stdio Functions
// This file provides standard C I/O functions

#include "./include/libsys.h"
#include "./include/stdint.h"
#include "./include/xposix/stdio.h"
#include "./include/xposix/stdlib.h"
#include "./include/xposix/string.h"
#include "./include/xposix/fcntl.h"
#include "./include/xposix/unistd.h"

extern "C" {

// Forward declarations
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int vsprintf(char *str, const char *format, va_list ap);
int snprintf(char *str, size_t size, const char *format, ...);

// Standard I/O streams
static FILE _stdin  = { 0, READ, 0, NULL, 0, 0, 0, NULL, 0 };
static FILE _stdout = { 1, WRITE, 0, NULL, 0, 0, 0, NULL, 0 };
static FILE _stderr = { 2, WRITE, 0, NULL, 0, 0, 0, NULL, 0 };

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

int printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}

int fprintf(FILE *stream, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfprintf(stream, format, args);
    va_end(args);
    return result;
}

int dprintf(int fd, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vdprintf(fd, format, args);
    va_end(args);
    return result;
}

int sprintf(char *str, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, (size_t)-1, format, args);
    va_end(args);
    return result;
}

int snprintf(char *str, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}

int vprintf(const char *format, va_list ap)
{
    return vfprintf(stdout, format, ap);
}

int vfprintf(FILE *stream, const char *format, va_list ap)
{
    if (stream == NULL || stream->fd < 0) return -1;

    char buffer[1024];
    int len = vsnprintf(buffer, sizeof(buffer), format, ap);
    if (len < 0) return len;

    if (len < (int)sizeof(buffer))
    {
        return write(stream->fd, buffer, len);
    }
    else
    {
        // Buffer too small, allocate larger buffer
        char *large_buffer = (char *)malloc(len + 1);
        if (large_buffer == NULL) return -1;

        va_list args_copy;
        va_copy(args_copy, ap);
        len = vsnprintf(large_buffer, len + 1, format, args_copy);
        va_end(args_copy);

        if (len > 0)
        {
            len = write(stream->fd, large_buffer, len);
        }

        free(large_buffer);
        return len;
    }
}

int vdprintf(int fd, const char *format, va_list ap)
{
    char buffer[1024];
    int len = vsnprintf(buffer, sizeof(buffer), format, ap);
    if (len < 0) return len;

    if (len < (int)sizeof(buffer))
    {
        return write(fd, buffer, len);
    }
    else
    {
        char *large_buffer = (char *)malloc(len + 1);
        if (large_buffer == NULL) return -1;

        va_list args_copy;
        va_copy(args_copy, ap);
        len = vsnprintf(large_buffer, len + 1, format, args_copy);
        va_end(args_copy);

        if (len > 0)
        {
            len = write(fd, large_buffer, len);
        }

        free(large_buffer);
        return len;
    }
}

// Helper: write a single char, tracking both actual output and total count
static void out_char(char **p, char *end, int *total, char c)
{
    (*total)++;
    if (*p < end) *(*p)++ = c;
}

// Helper: write a string, tracking both actual output and total count
static void out_string(char **p, char *end, int *total, const char *s, int len)
{
    *total += len;
    while (len-- > 0 && *p < end)
    {
        *(*p)++ = *s++;
    }
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
    if (str == NULL || size == 0) return -1;

    char *p = str;
    char *end = str + size - 1;
    int total = 0;

    while (*format)
    {
        if (*format != '%')
        {
            out_char(&p, end, &total, *format++);
            continue;
        }

        format++; // Skip '%'

        // Parse flags
        int left_align = 0;
        int zero_pad = 0;
        int plus_sign = 0;
        int space_sign = 0;
        int hash = 0;

        while (*format == '-' || *format == '0' || *format == '+' ||
               *format == ' ' || *format == '#')
        {
            if (*format == '-') left_align = 1;
            else if (*format == '0') zero_pad = 1;
            else if (*format == '+') plus_sign = 1;
            else if (*format == ' ') space_sign = 1;
            else if (*format == '#') hash = 1;
            format++;
        }

        // Parse width
        int width = 0;
        if (*format >= '1' && *format <= '9')
        {
            while (*format >= '0' && *format <= '9')
            {
                width = width * 10 + (*format - '0');
                format++;
            }
        }
        else if (*format == '*')
        {
            width = va_arg(ap, int);
            format++;
        }

        // Parse precision
        int precision = -1;
        if (*format == '.')
        {
            format++;
            precision = 0;
            if (*format >= '0' && *format <= '9')
            {
                while (*format >= '0' && *format <= '9')
                {
                    precision = precision * 10 + (*format - '0');
                    format++;
                }
            }
            else if (*format == '*')
            {
                precision = va_arg(ap, int);
                format++;
            }
        }

        // Parse length modifier
        int length = 0;
        if (*format == 'h')
        {
            length = 1;
            format++;
            if (*format == 'h')
            {
                length = 2;
                format++;
            }
        }
        else if (*format == 'l')
        {
            length = 3;
            format++;
            if (*format == 'l')
            {
                length = 4;
                format++;
            }
        }
        else if (*format == 'z')
        {
            length = 5;
            format++;
        }

        // Parse conversion specifier
        char spec = *format++;
        char numbuf[64];

        switch (spec)
        {
        case 'd':
        case 'i':
        {
            long long val;
            if (length == 4) val = va_arg(ap, long long);
            else if (length == 3) val = va_arg(ap, long);
            else val = va_arg(ap, int);

            char sign = 0;
            if (val < 0)
            {
                sign = '-';
                val = -val;
            }
            else if (plus_sign)
            {
                sign = '+';
            }
            else if (space_sign)
            {
                sign = ' ';
            }

            char *np = numbuf + sizeof(numbuf) - 1;
            *np = '\0';
            int digits = 0;
            if (val == 0)
            {
                *--np = '0';
                digits = 1;
            }
            else
            {
                while (val > 0)
                {
                    *--np = '0' + (int)(val % 10);
                    val /= 10;
                    digits++;
                }
            }

            int sign_len = (sign != 0) ? 1 : 0;
            int content_len = sign_len + digits;
            int padding = width > content_len ? width - content_len : 0;

            if (!left_align && !zero_pad)
            {
                while (padding-- > 0) out_char(&p, end, &total, ' ');
            }
            if (sign != 0) out_char(&p, end, &total, sign);
            if (!left_align && zero_pad)
            {
                while (padding-- > 0) out_char(&p, end, &total, '0');
            }
            char *d = np;
            while (*d) out_char(&p, end, &total, *d++);
            if (left_align)
            {
                while (padding-- > 0) out_char(&p, end, &total, ' ');
            }
            break;
        }

        case 'u':
        case 'x':
        case 'X':
        case 'o':
        {
            unsigned long long val;
            if (length == 4) val = va_arg(ap, unsigned long long);
            else if (length == 3) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);

            int base = 10;
            const char *hex_digits = "0123456789abcdef";
            if (spec == 'x' || spec == 'X') base = 16;
            else if (spec == 'o') base = 8;

            if (spec == 'X') hex_digits = "0123456789ABCDEF";

            unsigned long long orig_val = val;
            char *np = numbuf + sizeof(numbuf) - 1;
            *np = '\0';
            int num_digits = 0;
            if (val == 0)
            {
                *--np = '0';
                num_digits = 1;
            }
            else
            {
                while (val > 0)
                {
                    *--np = hex_digits[(int)(val % base)];
                    val /= base;
                    num_digits++;
                }
            }

            char prefix[2] = {0, 0};
            int prefix_len = 0;
            if (hash && spec == 'x' && orig_val != 0)
            {
                prefix[0] = '0';
                prefix[1] = 'x';
                prefix_len = 2;
            }
            else if (hash && spec == 'X' && orig_val != 0)
            {
                prefix[0] = '0';
                prefix[1] = 'X';
                prefix_len = 2;
            }

            int content_len = prefix_len + num_digits;
            int padding = width > content_len ? width - content_len : 0;

            if (!left_align && !zero_pad)
            {
                while (padding-- > 0) out_char(&p, end, &total, ' ');
            }
            for (int i = 0; i < prefix_len; i++)
                out_char(&p, end, &total, prefix[i]);
            if (!left_align && zero_pad)
            {
                while (padding-- > 0) out_char(&p, end, &total, '0');
            }
            char *d = np;
            while (*d) out_char(&p, end, &total, *d++);
            if (left_align)
            {
                while (padding-- > 0) out_char(&p, end, &total, ' ');
            }
            break;
        }

        case 'c':
        {
            char c = (char)va_arg(ap, int);
            int padding = width > 1 ? width - 1 : 0;
            if (!left_align)
            {
                while (padding-- > 0) out_char(&p, end, &total, ' ');
            }
            out_char(&p, end, &total, c);
            if (left_align)
            {
                while (padding-- > 0) out_char(&p, end, &total, ' ');
            }
            break;
        }

        case 's':
        {
            const char *sp = va_arg(ap, const char *);
            if (sp == NULL) sp = "(null)";

            int slen = strlen(sp);
            if (precision >= 0 && slen > precision) slen = precision;

            int padding = width > slen ? width - slen : 0;

            if (!left_align)
            {
                while (padding-- > 0) out_char(&p, end, &total, ' ');
            }
            out_string(&p, end, &total, sp, slen);
            if (left_align)
            {
                while (padding-- > 0) out_char(&p, end, &total, ' ');
            }
            break;
        }

        case 'p':
        {
            void *ptr = va_arg(ap, void *);
            if (ptr == NULL)
            {
                const char *nil_str = "(nil)";
                int len = strlen(nil_str);
                out_string(&p, end, &total, nil_str, len);
            }
            else
            {
                out_char(&p, end, &total, '0');
                out_char(&p, end, &total, 'x');

                unsigned long long val = (unsigned long long)ptr;
                char *np = numbuf + sizeof(numbuf) - 1;
                *np = '\0';
                int num_digits = 0;
                if (val == 0)
                {
                    *--np = '0';
                    num_digits = 1;
                }
                else
                {
                    while (val > 0)
                    {
                        *--np = "0123456789abcdef"[(int)(val % 16)];
                        val /= 16;
                        num_digits++;
                    }
                }
                char *d = np;
                while (*d) out_char(&p, end, &total, *d++);
            }
            break;
        }

        case '%':
            out_char(&p, end, &total, '%');
            break;

        default:
            out_char(&p, end, &total, '%');
            out_char(&p, end, &total, spec);
            break;
        }
    }

    // Null-terminate (always, even on truncation)
    if (p < end + 1) *p = '\0';
    return total;
}

int vsprintf(char *str, const char *format, va_list ap)
{
    return vsnprintf(str, (size_t)-1, format, ap);
}

} // extern "C"
