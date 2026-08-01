#pragma once
#include <proto.hpp>

// font/ttf/ttf.cpp
void print_box_ttf(SHEET_INFO *sht, SHEET *csheet, char *buf, SHEET_BUFFER fc, unsigned x, unsigned y, int size);
void print_box_ttfl(SHEET_INFO *sht, SHEET *csheet, char *buf, SHEET_BUFFER fc,
                   /*unsigned bc, */ unsigned x, unsigned y, int size, uint32_t *i_width);
void print_fmt_box_ttf(SHEET_INFO *sht, SHEET *csheet, SHEET_BUFFER fc, int x, int y, int size, const char *fmt,
                       ...);
void print_fmt_box_ttf(SHEET_INFO *sht, SHEET *csheet, SHEET_BUFFER fc, int x, int y, int size, uint32_t *i_width, const char *fmt,
                       ...);
                       
void print_box_ttf_c(SHEET_INFO *sht, SHEET *csheet, char *buf, SHEET_BUFFER fc,
				   /*unsigned bc, */ unsigned x, unsigned y, int size);
void init_ttf();

// 劲爆美味！
uint64_t calc_ttf_length(char *str, int size);

typedef struct FbTTFWriterData
{
    SHEET_INFO  *sht;
    SHEET      *ct_sheet;
    SHEET_BUFFER color;
    int          start_x;
    int          start_y;
    int          x;
    int          y;
    int          size;
    uint32_t    *i_width;

    // 缓冲区设计
    Rune   *rune_buf;      // Rune缓冲区指针
    size_t  rune_buf_size; // Rune缓冲区总大小
    char   *buf;           // 缓冲区指针
    size_t  idx;           // 当前写入位置
    size_t  buf_size;      // 缓冲区总大小
    uint8_t flush;         // 刷新标志
} FbTTFWriterData;
