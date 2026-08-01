#define DRAW_CPP
#include <efi/efi.h>
#include <efi/fbc.h>
#include <graphics/GOP.hpp>
#include <graphics/sheet.h>
#include <mm/alloc/alloc.h>
#include <proto.hpp>

EFI_GUID gEfiSimpleFileSystemProtocolGuid = {
    0x824e5c22, 0x6469, 0x1002, {0x81, 0x19, 0x20, 0xe1, 0xc0, 0x66, 0xe2, 0x3a}
};
EFI_GUID gEfiFileInfoGuid = {
    0x5c4e2f81, 0x7b4a, 0x4d03, {0xa3, 0x9f, 0x4d, 0x56, 0x9e, 0x1a, 0x3f, 0x78}
};
#define NULL 0
// auto operator new(size_t size, void *ptr) -> void *
// {
//     ptr = malloc(size);

//     return ptr;
// }

// auto operator new[](size_t size, void *ptr) -> void *
// {
//     ptr = malloc(size);

//     return ptr;
// }

// auto operator delete(void *ptr, size_t size) -> void
// {

//     free(ptr);
// }

// auto operator delete[](void *ptr, size_t size) -> void
// {

//     free(ptr);
// }

// auto operator delete(void *ptr) -> void
// {

//     free(ptr);
// }

void dot(const FrameBufferConfig &fbc, int x, int y, const PixelColor &c)
{
    // 边界检查
    if (x < 0 || x >= (int)fbc.horizontal_resolution || y < 0 || y >= (int)fbc.vertical_resolution)
        return;
    
    // 直接计算并写入，避免函数调用
    uint8_t *pixel = (uint8_t *)fbc.frame_buffer + (y * fbc.pixels_per_scan_line + x) * 4;
    
    if (fbc.pixel_format == PixelFormat::kRGBR) {
        pixel[0] = c.r;
        pixel[1] = c.g;
        pixel[2] = c.b;
    } else {
        pixel[0] = c.b;
        pixel[1] = c.g;
        pixel[2] = c.r;
    }
}

void rect(const FrameBufferConfig &fbc, int x1, int y1, int x2, int y2, const PixelColor &c)
{
    // 边界检查和调整
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)fbc.horizontal_resolution) x2 = (int)fbc.horizontal_resolution;
    if (y2 > (int)fbc.vertical_resolution) y2 = (int)fbc.vertical_resolution;
    if (x1 >= x2 || y1 >= y2) return;
    
    uint8_t *frame_buffer = (uint8_t *)fbc.frame_buffer;
    uint32_t pixels_per_scan_line = fbc.pixels_per_scan_line;
    int rect_width = x2 - x1;
    int rect_height = y2 - y1;
    
    // 预计算像素值
    uint32_t pixel_value;
    if (fbc.pixel_format == PixelFormat::kRGBR) {
        // RGB 格式
        pixel_value = (0 << 24) | (c.b << 16) | (c.g << 8) | c.r;
    } else {
        // BGR 格式
        pixel_value = (0 << 24) | (c.r << 16) | (c.g << 8) | c.b;
    }
    
    uint32_t *first_row = (uint32_t *)(frame_buffer + (y1 * pixels_per_scan_line + x1) * 4);
    for (int x = 0; x < rect_width; x++) {
        first_row[x] = pixel_value;
    }

    if (rect_height <= 1) {
        return;
    }

    size_t row_bytes = (size_t)rect_width * sizeof(uint32_t);
    for (int y = 1; y < rect_height; y++) {
        uint8_t *row_start = frame_buffer + ((y1 + y) * pixels_per_scan_line + x1) * 4;
        memcpy(row_start, first_row, row_bytes);
    }
}
