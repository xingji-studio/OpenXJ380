#include "./wbuttons.h"
#include <global_color.h>
#include <graphics/window/window.h>
#include <proto.hpp>
#include <ttf.h>

/**
 * @brief 调整窗口标题栏/底部主题宽度
 * 
 * @param dst 目标缓冲区
 * @param src 源缓冲区
 * @param width 目标宽度
 * @param swidth 源宽度
 * @param height 高度
 */
void resize_theme_width(SHEET_BUFFER *dst, SHEET_BUFFER *src, uint32_t width, uint32_t swidth, uint32_t height)
{
    if (width < swidth)
    {
        // 反向调整：目标宽度小于源宽度
        int shrink = swidth - width;
        int left_part = width / 2;
        int right_part = width - left_part;
        
        for (int y = 0; y < height; y++)
        {
            // 复制左侧部分
            for (int x = 0; x < left_part; x++)
            {
                dst[y * width + x] = src[y * swidth + x];
            }
            // 复制右侧部分
            for (int x = left_part; x < width; x++)
            {
                dst[y * width + x] = src[y * swidth + (x + shrink)];
            }
        }
        return;
    }
    
    // 目标宽度大于等于源宽度
    int minus = width - swidth;
    int left_part = swidth / 2;
    int right_part = swidth - left_part - 1;
    
    for (int y = 0; y < height; y++)
    {
        // 复制左侧部分
        for (int x = 0; x <= left_part; x++)
        {
            dst[y * width + x] = src[y * swidth + x];
        }
        // 扩展中间部分
        for (int x = left_part + 1; x <= width - right_part; x++)
        {
            dst[y * width + x] = src[y * swidth + left_part];
        }
        // 复制右侧部分
        for (int x = width - right_part + 1; x < width; x++)
        {
            dst[y * width + x] = src[y * swidth + (x - minus)];
        }
    }
}

/**
 * @brief 调整窗口左右侧主题高度
 * 
 * @param dst 目标缓冲区
 * @param src 源缓冲区
 * @param width 宽度
 * @param height 目标高度
 * @param sheight 源高度
 */
void resize_theme_height(SHEET_BUFFER *dst, SHEET_BUFFER *src, uint32_t width, uint32_t height, uint32_t sheight)
{
    if (height < sheight)
    {
        resize_theme_unheight(dst, src, width, height, sheight);
        return;
    }
    
    int minus = height - sheight;
    int top_part = sheight / 2;
    int bottom_part = sheight - top_part - 1;
    
    for (int x = 0; x < width; x++)
    {
        // 复制顶部部分
        for (int y = 0; y <= top_part; y++)
        {
            dst[y * width + x] = src[y * width + x];
        }
        // 扩展中间部分
        for (int y = top_part + 1; y <= height - bottom_part; y++)
        {
            dst[y * width + x] = src[top_part * width + x];
        }
        // 复制底部部分
        for (int y = height - bottom_part + 1; y < height; y++)
        {
            dst[y * width + x] = src[(y - minus) * width + x];
        }
    }
}

/**
 * @brief 高度缩减调整
 * 
 * @param dst 目标缓冲区
 * @param src 源缓冲区
 * @param width 宽度
 * @param height 目标高度
 * @param sheight 源高度
 */
void resize_theme_unheight(SHEET_BUFFER *dst, SHEET_BUFFER *src, uint32_t width, uint32_t height, uint32_t sheight)
{
    int shrink = sheight - height;
    int top_part = height / 2;
    int bottom_part = height - top_part;
    
    for (int x = 0; x < width; x++)
    {
        // 复制顶部部分
        for (int y = 0; y < top_part; y++)
        {
            dst[y * width + x] = src[y * width + x];
        }
        // 复制底部部分
        for (int y = top_part; y < height; y++)
        {
            dst[y * width + x] = src[(y + shrink) * width + x];
        }
    }
}