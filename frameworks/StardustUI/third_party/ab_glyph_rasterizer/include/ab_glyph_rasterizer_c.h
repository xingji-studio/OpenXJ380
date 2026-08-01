#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AbGlyphRasterizerHandle AbGlyphRasterizerHandle;

typedef struct AbGlyphPoint {
    float x;
    float y;
} AbGlyphPoint;

AbGlyphRasterizerHandle* stardustui_abgr_create(unsigned int width, unsigned int height);
void stardustui_abgr_destroy(AbGlyphRasterizerHandle* handle);

int stardustui_abgr_reset(AbGlyphRasterizerHandle* handle, unsigned int width, unsigned int height);
int stardustui_abgr_clear(AbGlyphRasterizerHandle* handle);
int stardustui_abgr_dimensions(const AbGlyphRasterizerHandle* handle, unsigned int* out_width, unsigned int* out_height);

int stardustui_abgr_draw_line(AbGlyphRasterizerHandle* handle, AbGlyphPoint p0, AbGlyphPoint p1);
int stardustui_abgr_draw_quad(AbGlyphRasterizerHandle* handle, AbGlyphPoint p0, AbGlyphPoint p1, AbGlyphPoint p2);
int stardustui_abgr_draw_cubic(
    AbGlyphRasterizerHandle* handle,
    AbGlyphPoint p0,
    AbGlyphPoint p1,
    AbGlyphPoint p2,
    AbGlyphPoint p3
);

int stardustui_abgr_write_u8_alpha(const AbGlyphRasterizerHandle* handle, unsigned char* out_pixels, unsigned int out_len);

#ifdef __cplusplus
}
#endif
