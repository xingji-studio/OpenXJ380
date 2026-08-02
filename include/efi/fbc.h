// XJ380 framebuffer configuration header.
// Derived from MikanOS kernel/frame_buffer_config.hpp
// (https://github.com/uchan-nos/mikanos), Copyright Yuuki Uchida, Apache-2.0.
// See third_party/mikanos/LICENSE and THIRD_PARTY_NOTICES.md.
// Modified by XINGJI Studios.
#ifndef FBC_HPP_
#define FBC_HPP_

#include <stdint.h>

enum PixelFormat
{
    kRGBR, // 带k表示内核会用到
    kBGRR
}; // only支持这两种

struct FrameBufferConfig
{
    uint8_t         *frame_buffer;
    uint32_t         pixels_per_scan_line;
    uint32_t         horizontal_resolution;
    uint32_t         vertical_resolution;
    enum PixelFormat pixel_format;
};

#endif
