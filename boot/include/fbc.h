// 版权所有©XINGJI Studios 2017-2026 保留所有权利。
// XJ380图像头文件
#ifndef FBC_HPP_
#define FBC_HPP_

#include <efi.h>

enum PixelFormat
{
    kRGBR, // 带k表示内核会用到
    kBGRR
}; // only支持这两种

struct FrameBufferConfig
{
    UINT8 *frame_buffer;
    UINT32 pixels_per_scan_line;
    UINT32 horizontal_resolution;
    UINT32 vertical_resolution;
    enum PixelFormat pixel_format;
};

#endif
