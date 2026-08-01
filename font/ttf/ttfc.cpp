#define STB_TRUETYPE_IMPLEMENTATION
#include "./stb_ttf.h"
#include "krlibc.h"
#include "stdarg.h"
#include "stdint.h"
#include <fs/vfs/vfs.h>
#include <graphics/GOP.hpp>
#include <graphics/sheet.h>
#include <math.hpp>
#include <proto.hpp>
#include <ttf.h>

char          *ttf_buffer_c;
stbtt_fontinfo font_c;

SHEET_BUFFER LCD_AlphaBlend(SHEET_BUFFER foreground_color, SHEET_BUFFER background_color, uint8_t alpha);
void         put_bitmap(SHEET_INFO *sht, SHEET *csheet, unsigned char *bitmap, unsigned x, unsigned y, unsigned width,
                        unsigned heigh, unsigned bitmap_xsize, SHEET_BUFFER fc);
void print_fmt_box_ttf_c(SHEET_INFO *sht, SHEET *csheet, SHEET_BUFFER fc, int x, int y, int size, const char *fmt, ...);
unsigned char *TTF_PrintFont(stbtt_fontinfo *font_info, int *buf, unsigned *width, unsigned *heigh, int size);

unsigned char *TTF_Print_c(int *buf, unsigned *width, unsigned *heigh, int size)
{
    return TTF_PrintFont(&font_c, buf, width, heigh, size);
}

void print_box_ttf_c(SHEET_INFO *sht, SHEET *csheet, char *buf, SHEET_BUFFER fc,
                     /*unsigned bc, */ unsigned x, unsigned y, int size)
{
    print_fmt_box_ttf_c(sht, csheet, fc, (int)x, (int)y, size, "%s", buf);
}

#define TTF_BUF_SIZE 128

// 辅助函数：计算UTF-8字符的字节数
static int utf8_char_len(unsigned char c)
{
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; // 无效UTF-8 / 非前缀，按单字节处理
}

// 刷新缓冲区内容到屏幕
static void flush_ttf_buffer_c(FbTTFWriterData *data)
{
    if (data->idx == 0) return;

    // 确保缓冲区以'\0'结尾
    data->buf[data->idx] = '\0';

    // 转换UTF-8字符串到Rune数组
    size_t  utf_len   = utflen(data->buf);
    Rune   *r         = NULL;
    uint8_t free_rune = 0;
    if (utf_len > data->rune_buf_size)
    {
        r         = (Rune *)malloc((utf_len + 1) * sizeof(Rune));
        free_rune = 1;
    }
    else
    {
        r = data->rune_buf;
    }
    r[utf_len] = 0;
    int   i    = 0;
    char *p    = data->buf;
    while (*p != '\0')
    {
        p += chartorune(&r[i++], p);
    }

    // 查找图层
    SHEET *front_p = data->ct_sheet;

    // 渲染文本
    unsigned int   width  = 0;
    unsigned int   height = 0;
    unsigned char *bitmap = TTF_Print_c(r, &width, &height, data->size);
    if (bitmap == NULL)
    {
        write_serial_string("TTF Print Failed. Reason: Memory Allocate Failed.\n");
        if (free_rune) { free(r); }
        return;
    }
    put_bitmap(data->sht, data->ct_sheet, bitmap, data->x, data->y, width, height, 2560, data->color);
    refresh_part_sheet(data->sht, data->x, data->y, data->x + (int)width, data->y + (int)height);

    // 更新光标位置
    data->x += (int)width;

    free_frames((uint64_t)virt_to_phys((uint64_t)bitmap), (2560 * 1440 + PAGE_SIZE - 1) / PAGE_SIZE);
    if (free_rune) { free(r); }
    data->idx = 0; // 重置缓冲区
}

uint8_t fb_ttf_writer_handler_c(Writer *writer, char ch)
{
    FbTTFWriterData *fbttf = (FbTTFWriterData *)writer->data;

    // 处理缓冲区溢出
    if (fbttf->idx + utf8_char_len(ch) > fbttf->buf_size - 1) { flush_ttf_buffer_c(fbttf); }

    // 将字符添加到缓冲区
    fbttf->buf[fbttf->idx++] = ch;

    // 如果是换行符，立即刷新
    if (ch == '\n')
    {
        flush_ttf_buffer_c(fbttf);
        fbttf->y += fbttf->size + 4;
        fbttf->x  = fbttf->start_x;
    }

    return 1;
}

void print_fmt_box_ttf_c(SHEET_INFO *sht, SHEET *csheet, SHEET_BUFFER fc, int x, int y, int size, const char *fmt, ...)
{
    char *buf = (char *)malloc(TTF_BUF_SIZE * sizeof(char));
    Rune *rune_buf = (Rune *)malloc(TTF_BUF_SIZE * sizeof(Rune));
    if (buf == NULL || rune_buf == NULL)
    {
        free(buf);
        free(rune_buf);
        write_serial_string("print_fmt_box_ttf_c: buffer allocation failed\n");
        return;
    }
    buf[0] = '\0';

    // 初始化写入器数据
    FbTTFWriterData fb_ttf_writer_data = {
        .sht           = sht,
        .ct_sheet      = csheet,
        .color         = fc,
        .start_x       = x,
        .start_y       = y,
        .x             = x,
        .y             = y,
        .size          = size,
        .rune_buf      = rune_buf,
        .rune_buf_size = TTF_BUF_SIZE,
        .buf           = buf,
        .idx           = 0,
        .buf_size      = TTF_BUF_SIZE,
        .flush         = 0,
    };

    Writer fb_ttf_writer = {.data = &fb_ttf_writer_data, .handler = fb_ttf_writer_handler_c};

    va_list args;
    va_start(args, fmt);
    vwprintf(&fb_ttf_writer, fmt, args);
    va_end(args);

    // 刷新剩余内容
    flush_ttf_buffer_c(&fb_ttf_writer_data);

    free(buf);
    free(rune_buf);
}
