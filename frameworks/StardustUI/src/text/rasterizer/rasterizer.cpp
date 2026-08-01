#include "../../../includes/text/rasterizer/rasterizer.hpp"
#if !defined(STARDUSTUI_LINUX)
#include "../../../third_party/ab_glyph_rasterizer/include/ab_glyph_rasterizer_c.h"
#endif

namespace stardustui::text {

namespace {

float abs_float(float value)
{
    return value < 0.0f ? -value : value;
}

float floor_float(float value)
{
    int integer = static_cast<int>(value);
    if (value < static_cast<float>(integer)) {
        return static_cast<float>(integer - 1);
    }
    return static_cast<float>(integer);
}

float ceil_float(float value)
{
    int integer = static_cast<int>(value);
    if (value > static_cast<float>(integer)) {
        return static_cast<float>(integer + 1);
    }
    return static_cast<float>(integer);
}

float sqrt_float(float value)
{
    if (value <= 0.0f) {
        return 0.0f;
    }

    float estimate = value > 1.0f ? value : 1.0f;
    for (int iteration = 0; iteration < 8; ++iteration) {
        estimate = 0.5f * (estimate + value / estimate);
    }
    return estimate;
}

Point lerp(float t, Point p0, Point p1)
{
    return point(p0.x + t * (p1.x - p0.x), p0.y + t * (p1.y - p0.y));
}

float distance(Point a, Point b)
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return sqrt_float(dx * dx + dy * dy);
}

void add_coverage(stardustui::vector<float>& pixels, long long index, float value)
{
    if (index < 0 || index >= pixels.size()) {
        return;
    }
    pixels[static_cast<int>(index)] += value;
}

}

Rasterizer::Rasterizer() : bitmap_width(0), bitmap_height(0), pixels() {}

Rasterizer::Rasterizer(int width, int height) : bitmap_width(0), bitmap_height(0), pixels()
{
    reset(width, height);
}

void Rasterizer::reset(int width, int height)
{
    bitmap_width = width < 0 ? 0 : width;
    bitmap_height = height < 0 ? 0 : height;
    pixels.clear();
    const int count = bitmap_width * bitmap_height + 4;
    if (count <= 0) {
        return;
    }

    if (!pixels.reserve(count)) {
        bitmap_width = 0;
        bitmap_height = 0;
        return;
    }

    for (int index = 0; index < count; ++index) {
        if (!pixels.push_back(0.0f)) {
            bitmap_width = 0;
            bitmap_height = 0;
            pixels.clear();
            return;
        }
    }
}

void Rasterizer::clear()
{
    for (int index = 0; index < pixels.size(); ++index) {
        pixels[index] = 0.0f;
    }
}

int Rasterizer::width() const
{
    return bitmap_width;
}

int Rasterizer::height() const
{
    return bitmap_height;
}

void Rasterizer::draw_line(Point p0, Point p1)
{
    if (abs_float(p0.y - p1.y) <= 0.000001f) {
        return;
    }

    float dir = 1.0f;
    if (p0.y > p1.y) {
        dir = -1.0f;
        const Point swap = p0;
        p0 = p1;
        p1 = swap;
    }

    const float dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    float x = p0.x;
    int y0 = static_cast<int>(p0.y);
    if (y0 < 0) {
        y0 = 0;
    }
    if (p0.y < 0.0f) {
        x -= p0.y * dxdy;
    }

    int y_end = static_cast<int>(ceil_float(p1.y));
    if (y_end > bitmap_height) {
        y_end = bitmap_height;
    }

    for (int y = y0; y < y_end; ++y) {
        const int line_start = y * bitmap_width;
        const float top = static_cast<float>(y);
        const float bottom = static_cast<float>(y + 1);
        const float segment_start = p0.y > top ? p0.y : top;
        const float segment_end = p1.y < bottom ? p1.y : bottom;
        const float dy = segment_end - segment_start;
        const float x_next = x + dxdy * dy;
        const float d = dy * dir;
        const float x0 = x < x_next ? x : x_next;
        const float x1 = x < x_next ? x_next : x;
        const float x0_floor = floor_float(x0);
        const int x0i = static_cast<int>(x0_floor);
        const float x1_ceil = ceil_float(x1);
        const int x1i = static_cast<int>(x1_ceil);
        const long long line_start_x0i = static_cast<long long>(line_start) + static_cast<long long>(x0i);
        if (line_start_x0i < 0) {
            x = x_next;
            continue;
        }

        if (x1i <= x0i + 1) {
            const float x_middle_fraction = 0.5f * (x + x_next) - x0_floor;
            add_coverage(pixels, line_start_x0i, d - d * x_middle_fraction);
            add_coverage(pixels, line_start_x0i + 1, d * x_middle_fraction);
        } else {
            const float span = x1 - x0;
            if (span <= 0.0f) {
                x = x_next;
                continue;
            }

            const float reciprocal = 1.0f / span;
            const float x0_fraction = x0 - x0_floor;
            const float a0 = 0.5f * reciprocal * (1.0f - x0_fraction) * (1.0f - x0_fraction);
            const float x1_fraction = x1 - x1_ceil + 1.0f;
            const float am = 0.5f * reciprocal * x1_fraction * x1_fraction;
            add_coverage(pixels, line_start_x0i, d * a0);

            if (x1i == x0i + 2) {
                add_coverage(pixels, line_start_x0i + 1, d * (1.0f - a0 - am));
            } else {
                const float a1 = reciprocal * (1.5f - x0_fraction);
                add_coverage(pixels, line_start_x0i + 1, d * (a1 - a0));
                for (int xi = x0i + 2; xi < x1i - 1; ++xi) {
                    add_coverage(pixels, static_cast<long long>(line_start) + static_cast<long long>(xi), d * reciprocal);
                }
                const float a2 = a1 + static_cast<float>(x1i - x0i - 3) * reciprocal;
                add_coverage(pixels,
                             static_cast<long long>(line_start) + static_cast<long long>(x1i - 1),
                             d * (1.0f - a2 - am));
            }
            add_coverage(pixels, static_cast<long long>(line_start) + static_cast<long long>(x1i), d * am);
        }

        x = x_next;
    }
}

void Rasterizer::draw_quad(Point p0, Point p1, Point p2)
{
    const float devx = p0.x - 2.0f * p1.x + p2.x;
    const float devy = p0.y - 2.0f * p1.y + p2.y;
    const float deviation_squared = devx * devx + devy * devy;
    if (deviation_squared < 0.333f) {
        draw_line(p0, p2);
        return;
    }

    const float tolerance = 3.0f;
    int segment_count = 1 + static_cast<int>(floor_float(sqrt_float(sqrt_float(tolerance * deviation_squared))));
    if (segment_count < 1) {
        segment_count = 1;
    }

    Point previous = p0;
    const float segment_inverse = 1.0f / static_cast<float>(segment_count);
    float t = 0.0f;
    for (int index = 0; index < segment_count - 1; ++index) {
        t += segment_inverse;
        const Point a = lerp(t, p0, p1);
        const Point b = lerp(t, p1, p2);
        const Point next = lerp(t, a, b);
        draw_line(previous, next);
        previous = next;
    }
    draw_line(previous, p2);
}

namespace {

void tessellate_cubic(Rasterizer& rasterizer, Point p0, Point p1, Point p2, Point p3, unsigned char depth)
{
    const float object_space_flatness_squared = 0.35f * 0.35f;
    const unsigned char max_recursion_depth = 16;

    const float long_length = distance(p0, p1) + distance(p1, p2) + distance(p2, p3);
    const float short_length = distance(p0, p3);
    const float flatness_squared = long_length * long_length - short_length * short_length;

    if (depth < max_recursion_depth && flatness_squared > object_space_flatness_squared) {
        const Point p01 = lerp(0.5f, p0, p1);
        const Point p12 = lerp(0.5f, p1, p2);
        const Point p23 = lerp(0.5f, p2, p3);
        const Point pa = lerp(0.5f, p01, p12);
        const Point pb = lerp(0.5f, p12, p23);
        const Point midpoint = lerp(0.5f, pa, pb);

        tessellate_cubic(rasterizer, p0, p01, pa, midpoint, static_cast<unsigned char>(depth + 1));
        tessellate_cubic(rasterizer, midpoint, pb, p23, p3, static_cast<unsigned char>(depth + 1));
        return;
    }

    rasterizer.draw_line(p0, p3);
}

}

void Rasterizer::draw_cubic(Point p0, Point p1, Point p2, Point p3)
{
    tessellate_cubic(*this, p0, p1, p2, p3, 0);
}

const stardustui::vector<float>& Rasterizer::coverage() const
{
    return pixels;
}

}

#if !defined(STARDUSTUI_LINUX)
struct AbGlyphRasterizerHandle {
    stardustui::text::Rasterizer inner;

    AbGlyphRasterizerHandle(unsigned int width, unsigned int height)
        : inner(static_cast<int>(width), static_cast<int>(height)) {}
};

extern "C" {

AbGlyphRasterizerHandle* stardustui_abgr_create(unsigned int width, unsigned int height)
{
    return new AbGlyphRasterizerHandle(width, height);
}

void stardustui_abgr_destroy(AbGlyphRasterizerHandle* handle)
{
    delete handle;
}

int stardustui_abgr_reset(AbGlyphRasterizerHandle* handle, unsigned int width, unsigned int height)
{
    if (handle == nullptr) {
        return 0;
    }
    handle->inner.reset(static_cast<int>(width), static_cast<int>(height));
    return 1;
}

int stardustui_abgr_clear(AbGlyphRasterizerHandle* handle)
{
    if (handle == nullptr) {
        return 0;
    }
    handle->inner.clear();
    return 1;
}

int stardustui_abgr_dimensions(const AbGlyphRasterizerHandle* handle, unsigned int* out_width, unsigned int* out_height)
{
    if (handle == nullptr) {
        return 0;
    }

    if (out_width != nullptr) {
        *out_width = static_cast<unsigned int>(handle->inner.width());
    }
    if (out_height != nullptr) {
        *out_height = static_cast<unsigned int>(handle->inner.height());
    }
    return 1;
}

int stardustui_abgr_draw_line(AbGlyphRasterizerHandle* handle, AbGlyphPoint p0, AbGlyphPoint p1)
{
    if (handle == nullptr) {
        return 0;
    }
    handle->inner.draw_line(stardustui::text::point(p0.x, p0.y), stardustui::text::point(p1.x, p1.y));
    return 1;
}

int stardustui_abgr_draw_quad(AbGlyphRasterizerHandle* handle, AbGlyphPoint p0, AbGlyphPoint p1, AbGlyphPoint p2)
{
    if (handle == nullptr) {
        return 0;
    }
    handle->inner.draw_quad(stardustui::text::point(p0.x, p0.y),
                            stardustui::text::point(p1.x, p1.y),
                            stardustui::text::point(p2.x, p2.y));
    return 1;
}

int stardustui_abgr_draw_cubic(
    AbGlyphRasterizerHandle* handle,
    AbGlyphPoint p0,
    AbGlyphPoint p1,
    AbGlyphPoint p2,
    AbGlyphPoint p3
)
{
    if (handle == nullptr) {
        return 0;
    }
    handle->inner.draw_cubic(stardustui::text::point(p0.x, p0.y),
                             stardustui::text::point(p1.x, p1.y),
                             stardustui::text::point(p2.x, p2.y),
                             stardustui::text::point(p3.x, p3.y));
    return 1;
}

int stardustui_abgr_write_u8_alpha(const AbGlyphRasterizerHandle* handle, unsigned char* out_pixels, unsigned int out_len)
{
    if (handle == nullptr || out_pixels == nullptr) {
        return 0;
    }

    const stardustui::vector<float>& coverage = handle->inner.coverage();
    const int total_pixels = handle->inner.width() * handle->inner.height();
    float accumulated = 0.0f;
    for (int index = 0; index < total_pixels; ++index) {
        accumulated += coverage[index];
        if (index >= static_cast<int>(out_len)) {
            continue;
        }

        float alpha = accumulated < 0.0f ? -accumulated : accumulated;
        if (alpha < 0.0f) {
            alpha = 0.0f;
        } else if (alpha > 1.0f) {
            alpha = 1.0f;
        }
        out_pixels[index] = static_cast<unsigned char>(alpha * 255.0f);
    }

    return 1;
}

}
#endif
