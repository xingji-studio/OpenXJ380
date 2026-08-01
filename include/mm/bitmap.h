#pragma once

#include "stdint.h"

typedef struct
{
    uint8_t *buffer;
    size_t   length;
} Bitmap;

/**
 * 初始化 Bitmap
 * @param bitmap Bitmap 结构体指针
 * @param buffer Bitmap 缓冲区
 * @param size 缓冲区大小 (字节)
 */
void bitmap_init(Bitmap *bitmap, uint8_t *buffer, size_t size);

/**
 * 获取 Bitmap 中指定索引的位
 * @param bitmap Bitmap 结构体指针
 * @param index 位索引
 * @return true/false
 */
bool bitmap_get(const Bitmap *bitmap, size_t index);

/**
 * 设置 Bitmap 中指定索引的位
 * @param bitmap Bitmap 结构体指针
 * @param index 位索引
 * @param value true/false
 */
void bitmap_set(Bitmap *bitmap, size_t index, bool value);

/**
 * 设置 Bitmap 中指定范围的位
 * @param bitmap Bitmap 结构体指针
 * @param start 起始索引
 * @param end 结束索引 (不包含)
 * @param value true/false
 */
void bitmap_set_range(Bitmap *bitmap, size_t start, size_t end, bool value);

/**
 * 在 Bitmap 中查找指定长度的连续位
 * @param bitmap Bitmap 结构体指针
 * @param length 查找长度 (位)
 * @param value true/false
 * @return 找到的起始索引, 如果未找到则返回 -1
 */
size_t bitmap_find_range(const Bitmap *bitmap, size_t length, bool value);
