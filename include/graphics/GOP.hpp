// XJ380图像头文件
// GOP.hpp
#pragma once

#include <efi/fbc.h>

struct PixelColor
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

inline uint8_t *PixelAt(int x, int y, const FrameBufferConfig &config)
{
    return (uint8_t *)config.frame_buffer + 4 * (config.pixels_per_scan_line * y + x);
}

void WriteRGBR(int x, int y, const PixelColor &c, const FrameBufferConfig &config)
{
    auto p = PixelAt(x, y, config);
    p[0]   = c.r;
    p[1]   = c.g;
    p[2]   = c.b; // 注意：这里实际上是RGB，但类名可能是为了某种特定格式
}

void WriteBGRR(int x, int y, const PixelColor &c, const FrameBufferConfig &config)
{
    auto p = PixelAt(x, y, config);
    p[0]   = c.b;
    p[1]   = c.g;
    p[2]   = c.r; // BGRR格式
}