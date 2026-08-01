#pragma once

#include "../string.hpp"
#include "../vector.hpp"

namespace stardustui::text {

struct GlyphBitmap {
    int width;
    int height;
    int offset_x;
    int offset_y;
    int advance;
    stardustui::vector<unsigned char> alpha;

    GlyphBitmap() : width(0), height(0), offset_x(0), offset_y(0), advance(0), alpha() {}
};

struct TextBitmap {
    int width;
    int height;
    stardustui::vector<unsigned int> pixels;

    TextBitmap() : width(0), height(0), pixels() {}
};

bool measure_text(const stardustui::string& text, unsigned int pixel_size, unsigned int& out_width, unsigned int& out_height);
bool rasterize_text(const stardustui::string& text, unsigned int color, unsigned int pixel_size, TextBitmap& out_bitmap);

}
