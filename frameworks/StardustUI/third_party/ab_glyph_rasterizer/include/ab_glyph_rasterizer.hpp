#pragma once

#include "./ab_glyph_rasterizer_c.h"

namespace stardustui::third_party {

struct AbGlyphPointCpp {
    float x;
    float y;
};

class AbGlyphRasterizer {
public:
    AbGlyphRasterizer(unsigned int width, unsigned int height)
        : handle(stardustui_abgr_create(width, height)) {}

    ~AbGlyphRasterizer() {
        stardustui_abgr_destroy(handle);
    }

    AbGlyphRasterizer(const AbGlyphRasterizer&) = delete;
    AbGlyphRasterizer& operator=(const AbGlyphRasterizer&) = delete;

    bool valid() const {
        return handle != nullptr;
    }

    bool reset(unsigned int width, unsigned int height) {
        return stardustui_abgr_reset(handle, width, height) != 0;
    }

    bool clear() {
        return stardustui_abgr_clear(handle) != 0;
    }

    bool dimensions(unsigned int& width, unsigned int& height) const {
        return stardustui_abgr_dimensions(handle, &width, &height) != 0;
    }

    bool draw_line(AbGlyphPointCpp p0, AbGlyphPointCpp p1) {
        return stardustui_abgr_draw_line(handle, to_c(p0), to_c(p1)) != 0;
    }

    bool draw_quad(AbGlyphPointCpp p0, AbGlyphPointCpp p1, AbGlyphPointCpp p2) {
        return stardustui_abgr_draw_quad(handle, to_c(p0), to_c(p1), to_c(p2)) != 0;
    }

    bool draw_cubic(AbGlyphPointCpp p0, AbGlyphPointCpp p1, AbGlyphPointCpp p2, AbGlyphPointCpp p3) {
        return stardustui_abgr_draw_cubic(handle, to_c(p0), to_c(p1), to_c(p2), to_c(p3)) != 0;
    }

    bool write_u8_alpha(unsigned char* out_pixels, unsigned int out_len) const {
        return stardustui_abgr_write_u8_alpha(handle, out_pixels, out_len) != 0;
    }

private:
    static AbGlyphPoint to_c(AbGlyphPointCpp point) {
        AbGlyphPoint out{};
        out.x = point.x;
        out.y = point.y;
        return out;
    }

    AbGlyphRasterizerHandle* handle;
};

}
