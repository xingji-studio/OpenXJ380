// XJ380 boot framebuffer configuration header.
// Derived from MikanOS kernel/frame_buffer_config.hpp
// (https://github.com/uchan-nos/mikanos), Copyright Yuuki Uchida, Apache-2.0.
// See third_party/mikanos/LICENSE and THIRD_PARTY_NOTICES.md.
// Modified by XINGJI Studios.
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
