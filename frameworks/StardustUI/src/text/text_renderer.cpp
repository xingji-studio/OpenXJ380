#include "../../includes/text/text_renderer.hpp"
#include "../../includes/text/font.hpp"
#include "../../includes/text/truetype/face.hpp"
#include "../../platforms/platform.hpp"
#include "../../third_party/ab_glyph_rasterizer/include/ab_glyph_rasterizer.hpp"

namespace stardustui::text {

namespace {
struct DecodedCodepoint {
    unsigned int value;
    int next_index;
};

struct TextMetrics {
    int width;
    int height;
    int ascent;
    int descent;
    int line_gap;
    int min_y;
    int max_y;

    TextMetrics() : width(0), height(0), ascent(0), descent(0), line_gap(0), min_y(0), max_y(0) {}
};

struct FaceCache {
    TrueTypeFace face;
    bool ready;
    const stardustui::File::byte* source_data;
    int source_size;
    bool source_is_default_memory;
    const char* fallback_path;

    FaceCache()
        : face(),
          ready(false),
          source_data(nullptr),
          source_size(0),
          source_is_default_memory(false),
          fallback_path(nullptr) {}
};

struct ResolvedGlyph {
    const TrueTypeFace* face;
    int glyph_index;

    ResolvedGlyph() : face(nullptr), glyph_index(0) {}
};

struct GlyphCacheEntry {
    const TrueTypeFace* face;
    unsigned int pixel_size;
    int glyph_index;
    GlyphBitmap bitmap;

    GlyphCacheEntry() : face(nullptr), pixel_size(0), glyph_index(0), bitmap() {}
};

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

int max_int(int a, int b)
{
    return a > b ? a : b;
}

float min_float(float a, float b)
{
    return a < b ? a : b;
}

float max_float(float a, float b)
{
    return a > b ? a : b;
}

DecodedCodepoint decode_utf8(const char* text, int index)
{
    DecodedCodepoint result{};
    result.value = 0xFFFDu;
    result.next_index = index;
    if (text == nullptr || text[index] == '\0') {
        return result;
    }

    const unsigned char first = static_cast<unsigned char>(text[index]);
    int bytes = 0;
    if ((first & 0x80u) == 0x00u) {
        bytes = 1;
        result.value = first;
    } else if ((first & 0xE0u) == 0xC0u) {
        bytes = 2;
        result.value = first & 0x1Fu;
    } else if ((first & 0xF0u) == 0xE0u) {
        bytes = 3;
        result.value = first & 0x0Fu;
    } else if ((first & 0xF8u) == 0xF0u) {
        bytes = 4;
        result.value = first & 0x07u;
    } else {
        result.next_index = index + 1;
        return result;
    }

    result.next_index = index + 1;
    for (int remaining = 1; remaining < bytes; ++remaining) {
        const unsigned char next = static_cast<unsigned char>(text[result.next_index]);
        if ((next & 0xC0u) != 0x80u) {
            result.value = 0xFFFDu;
            return result;
        }
        result.value = (result.value << 6) | (next & 0x3Fu);
        ++result.next_index;
    }

    if (result.value == '\t' || result.value == '\v' || result.value == '\f' || result.value == '\r') {
        result.value = ' ';
    }
    return result;
}

GlyphOutlinePoint midpoint(const GlyphOutlinePoint& a, const GlyphOutlinePoint& b)
{
    GlyphOutlinePoint point;
    point.x = 0.5f * (a.x + b.x);
    point.y = 0.5f * (a.y + b.y);
    point.on_curve = true;
    return point;
}

bool points_equal(const GlyphOutlinePoint& a, const GlyphOutlinePoint& b)
{
    return a.x == b.x && a.y == b.y;
}

stardustui::third_party::AbGlyphPointCpp make_ab_point(const GlyphOutlinePoint& point,
                                                       float scale,
                                                       int origin_x,
                                                       int origin_y)
{
    stardustui::third_party::AbGlyphPointCpp transformed{};
    transformed.x = point.x * scale - static_cast<float>(origin_x);
    transformed.y = -point.y * scale - static_cast<float>(origin_y);
    return transformed;
}

bool draw_contour_with_ab_rasterizer(stardustui::third_party::AbGlyphRasterizer& rasterizer,
                                     const GlyphOutline& outline,
                                     const GlyphContour& contour,
                                     float scale,
                                     int origin_x,
                                     int origin_y)
{
    if (contour.start < 0 || contour.end < contour.start || contour.end >= outline.points.size()) {
        return true;
    }

    const GlyphOutlinePoint& start_point = outline.points[contour.start];
    const GlyphOutlinePoint& previous_point = outline.points[contour.end];
    GlyphOutlinePoint current_on = start_point;
    if (!current_on.on_curve) {
        current_on = previous_point.on_curve ? previous_point : midpoint(previous_point, start_point);
    }

    const GlyphOutlinePoint contour_start = current_on;
    for (int index = contour.start; index <= contour.end; ++index) {
        const GlyphOutlinePoint point = outline.points[index];
        if (point.on_curve) {
            if (!points_equal(current_on, point) &&
                !rasterizer.draw_line(make_ab_point(current_on, scale, origin_x, origin_y),
                                      make_ab_point(point, scale, origin_x, origin_y))) {
                return false;
            }
            current_on = point;
            continue;
        }

        const GlyphOutlinePoint next = index == contour.end ? outline.points[contour.start] : outline.points[index + 1];
        const GlyphOutlinePoint end_point = next.on_curve ? next : midpoint(point, next);
        if (!rasterizer.draw_quad(make_ab_point(current_on, scale, origin_x, origin_y),
                                  make_ab_point(point, scale, origin_x, origin_y),
                                  make_ab_point(end_point, scale, origin_x, origin_y))) {
            return false;
        }
        current_on = end_point;
    }

    if (!points_equal(current_on, contour_start) &&
        !rasterizer.draw_line(make_ab_point(current_on, scale, origin_x, origin_y),
                              make_ab_point(contour_start, scale, origin_x, origin_y))) {
        return false;
    }

    return true;
}

bool blend_text_pixel(unsigned int color, unsigned char alpha, unsigned int& out_pixel)
{
    if (alpha == 0) {
        out_pixel = 0;
        return true;
    }

    if ((color & 0xFFu) == 0u) {
        color |= 0xFFu;
    }

    const unsigned int red = (color >> 24) & 0xFFu;
    const unsigned int green = (color >> 16) & 0xFFu;
    const unsigned int blue = (color >> 8) & 0xFFu;
    const unsigned int base_alpha = color & 0xFFu;
    const unsigned int final_alpha = (base_alpha * static_cast<unsigned int>(alpha)) / 255u;
    out_pixel = (red << 24) | (green << 16) | (blue << 8) | final_alpha;
    return true;
}

FaceCache& face_cache()
{
    static FaceCache* cache = nullptr;
    if (cache == nullptr) {
        cache = new FaceCache();
    }
    return *cache;
}

#ifdef XJ380
FaceCache& fallback_face_cache_one()
{
    static FaceCache* cache = nullptr;
    if (cache == nullptr) {
        cache = new FaceCache();
    }
    return *cache;
}

FaceCache& fallback_face_cache_two()
{
    static FaceCache* cache = nullptr;
    if (cache == nullptr) {
        cache = new FaceCache();
    }
    return *cache;
}
#endif

stardustui::vector<GlyphCacheEntry>& glyph_cache()
{
    static stardustui::vector<GlyphCacheEntry>* cache = nullptr;
    if (cache == nullptr) {
        cache = new stardustui::vector<GlyphCacheEntry>();
    }
    return *cache;
}

void clear_glyph_cache()
{
    glyph_cache().release_storage();
}

bool load_cached_face(FaceCache& cache,
                      const stardustui::File::byte* data,
                      int size,
                      bool is_default_memory,
                      const char* fallback_path)
{
    if (data == nullptr || size <= 0) {
        return false;
    }

    if (cache.ready &&
        cache.source_data == data &&
        cache.source_size == size &&
        cache.source_is_default_memory == is_default_memory &&
        cache.fallback_path == fallback_path) {
        return true;
    }

    cache.face.clear();
    cache.ready = cache.face.load(data, size);
    cache.source_data = data;
    cache.source_size = size;
    cache.source_is_default_memory = is_default_memory;
    cache.fallback_path = fallback_path;
    clear_glyph_cache();
    return cache.ready;
}

bool load_cached_face_from_path(FaceCache& cache, const char* path)
{
    if (path == nullptr) {
        cache.face.clear();
        cache.ready = false;
        cache.source_data = nullptr;
        cache.source_size = 0;
        cache.source_is_default_memory = false;
        cache.fallback_path = nullptr;
        clear_glyph_cache();
        return false;
    }

    if (cache.ready &&
        cache.fallback_path != nullptr &&
        stardustui::string(cache.fallback_path).equals(path)) {
        return true;
    }

    cache.face.clear();
    cache.ready = cache.face.load(stardustui::string(path));
    cache.source_data = nullptr;
    cache.source_size = 0;
    cache.source_is_default_memory = false;
    cache.fallback_path = cache.ready ? path : nullptr;
    clear_glyph_cache();
    return cache.ready;
}

int collect_faces(const TrueTypeFace** out_faces, int max_faces)
{
    if (out_faces == nullptr || max_faces <= 0) {
        return 0;
    }

    int count = 0;
    const stardustui::string& default_font_path = Font::default_font_path();
    const bool has_default = Font::has_default_font();
    const bool default_is_memory = has_default && default_font_path.length() == 0;

    if (has_default) {
        FaceCache& primary = face_cache();
        if (default_font_path.length() > 0) {
            if (load_cached_face_from_path(primary, default_font_path.c_str())) {
                out_faces[count++] = &primary.face;
            }
        } else if (load_cached_face(primary,
                                    Font::default_font_data(),
                                    Font::default_font_data_size(),
                                    default_is_memory,
                                    nullptr)) {
            out_faces[count++] = &primary.face;
        }
    }

#ifdef XJ380
    static const char* xj380_font_c = "/system/font/XJ380C.ttf";
    static const char* xj380_font_f = "/system/font/XJ380F.ttf";

    if (count > 0) {
        return count;
    }

    if (count < max_faces) {
        FaceCache& fallback_one = fallback_face_cache_one();
        if (load_cached_face_from_path(fallback_one, xj380_font_c)) {
            out_faces[count++] = &fallback_one.face;
        }
    }

    if (count < max_faces) {
        FaceCache& fallback_two = fallback_face_cache_two();
        if (load_cached_face_from_path(fallback_two, xj380_font_f)) {
            bool duplicate = false;
            for (int index = 0; index < count; ++index) {
                if (out_faces[index] == &fallback_two.face) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                out_faces[count++] = &fallback_two.face;
            }
        }
    }
#else
    if (count == 0) {
#if defined(STARDUSTUI_WINDOWS)
        static const char* fallback_paths[] = {
            "C:/Windows/Fonts/msyh.ttc",
            "C:\\Windows\\Fonts\\msyh.ttc",
            "C:/Windows/Fonts/msyh.ttf",
            "C:\\Windows\\Fonts\\msyh.ttf",
            "C:/Windows/Fonts/msyhbd.ttc",
            "C:\\Windows\\Fonts\\msyhbd.ttc",
            "C:/Windows/Fonts/segoeui.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "C:\\Windows\\Fonts\\arial.ttf",
            "C:/Windows/Fonts/simsun.ttc",
            "C:\\Windows\\Fonts\\simsun.ttc",
            "C:/Windows/Fonts/simhei.ttf",
            "C:\\Windows\\Fonts\\simhei.ttf",
            "/home/archzero/C++/XJ380/font/ttf/XJ380C.ttf",
            "/home/archzero/C++/XJ380/font/ttf/XJ380F.ttf",
            "/home/archzero/C++/XJ380/frameworks/StardustUI/fonts/xiaolai.ttf"
        };
#else
        static const char* fallback_paths[] = {
            "/home/archzero/C++/XJ380/font/ttf/XJ380C.ttf",
            "/home/archzero/C++/XJ380/font/ttf/XJ380F.ttf",
            "/home/archzero/C++/XJ380/third_party/litehtml/containers/test/fonts/ahem.ttf",
            "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
        };
#endif

        FaceCache& primary = face_cache();
        for (unsigned int index = 0; index < sizeof(fallback_paths) / sizeof(fallback_paths[0]); ++index) {
            if (load_cached_face_from_path(primary, fallback_paths[index])) {
                out_faces[count++] = &primary.face;
                break;
            }
        }
    }
#endif

    return count;
}

ResolvedGlyph resolve_glyph(const TrueTypeFace** faces, int face_count, unsigned int codepoint)
{
    ResolvedGlyph glyph;
    for (int index = 0; index < face_count; ++index) {
        if (faces[index] == nullptr) {
            continue;
        }

        const int glyph_index = faces[index]->glyph_index_from_codepoint(codepoint);
        if (glyph_index != 0) {
            glyph.face = faces[index];
            glyph.glyph_index = glyph_index;
            return glyph;
        }
    }

    if (face_count > 0) {
        glyph.face = faces[0];
    }
    return glyph;
}

bool append_alpha_pixel(stardustui::vector<unsigned char>& buffer, unsigned char value)
{
    return buffer.push_back(value);
}

bool rasterize_glyph_bitmap(const TrueTypeFace& face, int glyph_index, unsigned int pixel_size, GlyphBitmap& out_bitmap)
{
    out_bitmap = GlyphBitmap();

    const int size = pixel_size == 0 ? 16 : static_cast<int>(pixel_size);
    const int advance_units = face.glyph_advance_width(glyph_index);
    const float scale = static_cast<float>(size) / static_cast<float>(face.units_per_em());
    out_bitmap.advance = advance_units <= 0 ? max_int(1, size / 2)
                                            : static_cast<int>(ceil_float(static_cast<float>(advance_units) * scale));

    GlyphOutline raw_outline;
    if (!face.load_glyph_outline(glyph_index, raw_outline) || raw_outline.contours.empty()) {
        return true;
    }

    float min_x = raw_outline.points[0].x * scale;
    float max_x = min_x;
    float min_y = -raw_outline.points[0].y * scale;
    float max_y = min_y;
    for (int point_index = 1; point_index < raw_outline.points.size(); ++point_index) {
        const GlyphOutlinePoint& point = raw_outline.points[point_index];
        const float x = point.x * scale;
        const float y = -point.y * scale;
        min_x = min_float(min_x, x);
        max_x = max_float(max_x, x);
        min_y = min_float(min_y, y);
        max_y = max_float(max_y, y);
    }

    const int start_x = static_cast<int>(floor_float(min_x)) - 1;
    const int end_x = static_cast<int>(ceil_float(max_x)) + 1;
    const int start_y = static_cast<int>(floor_float(min_y)) - 1;
    const int end_y = static_cast<int>(ceil_float(max_y)) + 1;
    out_bitmap.offset_x = start_x;
    out_bitmap.offset_y = start_y;
    out_bitmap.width = max_int(0, end_x - start_x);
    out_bitmap.height = max_int(0, end_y - start_y);
    if (out_bitmap.width <= 0 || out_bitmap.height <= 0) {
        return true;
    }

    const int total_pixels = out_bitmap.width * out_bitmap.height;
    if (!out_bitmap.alpha.reserve(total_pixels)) {
        return false;
    }
    for (int index = 0; index < total_pixels; ++index) {
        if (!append_alpha_pixel(out_bitmap.alpha, 0)) {
            out_bitmap.alpha.clear();
            return false;
        }
    }

    stardustui::third_party::AbGlyphRasterizer rasterizer(static_cast<unsigned int>(out_bitmap.width),
                                                          static_cast<unsigned int>(out_bitmap.height));
    if (!rasterizer.valid()) {
        out_bitmap.alpha.clear();
        return false;
    }

    for (int contour_index = 0; contour_index < raw_outline.contours.size(); ++contour_index) {
        if (!draw_contour_with_ab_rasterizer(rasterizer,
                                             raw_outline,
                                             raw_outline.contours[contour_index],
                                             scale,
                                             start_x,
                                             start_y)) {
            out_bitmap.alpha.clear();
            return false;
        }
    }

    if (!rasterizer.write_u8_alpha(&out_bitmap.alpha[0], static_cast<unsigned int>(total_pixels))) {
        out_bitmap.alpha.clear();
        return false;
    }
    return true;
}

const GlyphBitmap* get_glyph_bitmap(const TrueTypeFace& face, int glyph_index, unsigned int pixel_size)
{
    stardustui::vector<GlyphCacheEntry>& cache = glyph_cache();
    for (int index = 0; index < cache.size(); ++index) {
        GlyphCacheEntry& entry = cache[index];
        if (entry.face == &face &&
            entry.pixel_size == pixel_size &&
            entry.glyph_index == glyph_index) {
            return &entry.bitmap;
        }
    }

    GlyphCacheEntry entry;
    entry.face = &face;
    entry.pixel_size = pixel_size;
    entry.glyph_index = glyph_index;
    if (!rasterize_glyph_bitmap(face, glyph_index, pixel_size, entry.bitmap)) {
        return nullptr;
    }
#ifdef XJ380
    if (cache.size() >= 96) {
        clear_glyph_cache();
    }
#endif
    if (!cache.push_back(entry)) {
        return nullptr;
    }
    return &cache[cache.size() - 1].bitmap;
}

TextMetrics compute_metrics(const TrueTypeFace& face, unsigned int pixel_size)
{
    TextMetrics metrics;
    const int size = pixel_size == 0 ? 16 : static_cast<int>(pixel_size);
    if (!face.is_loaded() || face.units_per_em() <= 0) {
        metrics.width = 0;
        metrics.height = size;
        metrics.ascent = size;
        metrics.descent = 0;
        return metrics;
    }

    const float scale = static_cast<float>(size) / static_cast<float>(face.units_per_em());
    metrics.ascent = static_cast<int>(ceil_float(static_cast<float>(face.ascender()) * scale));
    metrics.descent = static_cast<int>(ceil_float(static_cast<float>(-face.descender()) * scale));
    metrics.line_gap = max_int(0, static_cast<int>(ceil_float(static_cast<float>(face.line_height()) * scale)) - metrics.ascent - metrics.descent);
    metrics.height = max_int(1, metrics.ascent + metrics.descent + metrics.line_gap);
    metrics.min_y = -metrics.descent;
    metrics.max_y = metrics.ascent;
    return metrics;
}

bool measure_text_internal(const stardustui::string& text,
                           unsigned int pixel_size,
                           const TrueTypeFace** faces,
                           int face_count,
                           TextMetrics& out_metrics)
{
    out_metrics = TextMetrics();
    if (faces == nullptr || face_count <= 0 || faces[0] == nullptr) {
        out_metrics.height = pixel_size == 0 ? 16 : static_cast<int>(pixel_size);
        return true;
    }

    out_metrics = compute_metrics(*faces[0], pixel_size);
    for (int face_index = 1; face_index < face_count; ++face_index) {
        if (faces[face_index] == nullptr) {
            continue;
        }
        const TextMetrics face_metrics = compute_metrics(*faces[face_index], pixel_size);
        out_metrics.ascent = max_int(out_metrics.ascent, face_metrics.ascent);
        out_metrics.descent = max_int(out_metrics.descent, face_metrics.descent);
        out_metrics.line_gap = max_int(out_metrics.line_gap, face_metrics.line_gap);
        out_metrics.height = max_int(out_metrics.height, face_metrics.height);
    }

    const int base_ascent = out_metrics.ascent;
    const int base_descent = out_metrics.descent;
    const int base_line_gap = out_metrics.line_gap;
    const int base_line_height = max_int(1, base_ascent + base_descent + base_line_gap);

    const char* raw = text.c_str();
    if (raw == nullptr || raw[0] == '\0') {
        return true;
    }

    const int fallback_advance = static_cast<int>(pixel_size == 0 ? 8U : (pixel_size * 2U) / 3U);
    int current_width = 0;
    int max_width = 0;
    int pen_y = 0;
    int min_y = 0;
    int max_y = out_metrics.height;
    bool has_ink = false;

    for (int index = 0; raw[index] != '\0';) {
        DecodedCodepoint decoded = decode_utf8(raw, index);
        index = decoded.next_index;

        if (decoded.value == '\n') {
            if (current_width > max_width) {
                max_width = current_width;
            }
            current_width = 0;
            pen_y += base_line_height;
            max_y = max_int(max_y, pen_y + out_metrics.height);
            continue;
        }

        const ResolvedGlyph glyph = resolve_glyph(faces, face_count, decoded.value);
        int advance = glyph.face == nullptr ? 0 : glyph.face->glyph_advance_width(glyph.glyph_index);
        if (advance <= 0 || glyph.face == nullptr || glyph.face->units_per_em() <= 0) {
            advance = fallback_advance;
        } else {
            const float scale = static_cast<float>(pixel_size == 0 ? 16U : pixel_size) /
                                static_cast<float>(glyph.face->units_per_em());
            advance = static_cast<int>(ceil_float(static_cast<float>(advance) * scale));
        }

        if (glyph.face != nullptr) {
            const GlyphBitmap* bitmap = get_glyph_bitmap(*glyph.face, glyph.glyph_index, pixel_size);
            if (bitmap != nullptr && bitmap->width > 0 && bitmap->height > 0) {
                const int glyph_top = pen_y + out_metrics.ascent + bitmap->offset_y;
                const int glyph_bottom = glyph_top + bitmap->height;
                if (!has_ink) {
                    min_y = glyph_top;
                    max_y = glyph_bottom;
                    has_ink = true;
                } else {
                    if (glyph_top < min_y) {
                        min_y = glyph_top;
                    }
                    if (glyph_bottom > max_y) {
                        max_y = glyph_bottom;
                    }
                }
            }
        }

        current_width += advance;
    }

    if (current_width > max_width) {
        max_width = current_width;
    }
    out_metrics.width = max_width;
    if (has_ink) {
        out_metrics.min_y = min_y;
        out_metrics.max_y = max_y;
        out_metrics.height = max_int(1, max_y - min_y);
        out_metrics.ascent = max_int(0, base_ascent - min_y);
        out_metrics.descent = max_int(0, max_y - base_ascent);
        out_metrics.line_gap = max_int(0, base_line_height - out_metrics.ascent - out_metrics.descent);
    }
    return true;
}
}

bool measure_text(const stardustui::string& text, unsigned int pixel_size, unsigned int& out_width, unsigned int& out_height)
{
    const TrueTypeFace* faces[3] = {nullptr, nullptr, nullptr};
    const int face_count = collect_faces(faces, 3);
    if (face_count <= 0 || faces[0] == nullptr) {
        out_width = static_cast<unsigned int>(text.length()) * (pixel_size == 0 ? 8U : (pixel_size * 2U) / 3U);
        out_height = pixel_size == 0 ? 16U : pixel_size;
        return true;
    }

    TextMetrics metrics;
    if (!measure_text_internal(text, pixel_size, faces, face_count, metrics)) {
        return false;
    }

    out_width = static_cast<unsigned int>(max_int(0, metrics.width));
    out_height = static_cast<unsigned int>(max_int(1, metrics.height));
    return true;
}

bool rasterize_text(const stardustui::string& text, unsigned int color, unsigned int pixel_size, TextBitmap& out_bitmap)
{
    out_bitmap.width = 0;
    out_bitmap.height = 0;
    out_bitmap.pixels.clear();

    const TrueTypeFace* faces[3] = {nullptr, nullptr, nullptr};
    const int face_count = collect_faces(faces, 3);
    if (face_count <= 0 || faces[0] == nullptr) {
        unsigned int width = 0;
        unsigned int height = 0;
        if (!measure_text(text, pixel_size, width, height)) {
            return false;
        }
        out_bitmap.width = static_cast<int>(width);
        out_bitmap.height = static_cast<int>(height);
        const int count = out_bitmap.width * out_bitmap.height;
        if (count <= 0) {
            return true;
        }
        if (!out_bitmap.pixels.reserve(count)) {
            return false;
        }
        for (int index = 0; index < count; ++index) {
            if (!out_bitmap.pixels.push_back(0)) {
                return false;
            }
        }
        return true;
    }

    TextMetrics metrics;
    if (!measure_text_internal(text, pixel_size, faces, face_count, metrics)) {
        return false;
    }

    out_bitmap.width = max_int(0, metrics.width);
    out_bitmap.height = max_int(1, metrics.height);
    const int pixel_count = out_bitmap.width * out_bitmap.height;
    if (pixel_count <= 0) {
        return true;
    }

    if (!out_bitmap.pixels.reserve(pixel_count)) {
        out_bitmap.width = 0;
        out_bitmap.height = 0;
        return false;
    }
    for (int index = 0; index < pixel_count; ++index) {
        if (!out_bitmap.pixels.push_back(0)) {
            out_bitmap.width = 0;
            out_bitmap.height = 0;
            out_bitmap.pixels.clear();
            return false;
        }
    }

    const int size = pixel_size == 0 ? 16 : static_cast<int>(pixel_size);
    const int line_height = max_int(1, metrics.ascent + metrics.descent + metrics.line_gap);
    const int ascent = metrics.ascent;
    int pen_x = 0;
    int baseline_y = ascent;

    const char* raw = text.c_str();
    for (int index = 0; raw[index] != '\0';) {
        DecodedCodepoint decoded = decode_utf8(raw, index);
        index = decoded.next_index;

        if (decoded.value == '\n') {
            pen_x = 0;
            baseline_y += line_height;
            continue;
        }

        const ResolvedGlyph resolved = resolve_glyph(faces, face_count, decoded.value);
        if (resolved.face == nullptr) {
            pen_x += max_int(1, size / 2);
            continue;
        }

        const GlyphBitmap* glyph = get_glyph_bitmap(*resolved.face, resolved.glyph_index, pixel_size);
        if (glyph == nullptr) {
            pen_x += max_int(1, size / 2);
            continue;
        }

        for (int y = 0; y < glyph->height; ++y) {
            const int target_y = baseline_y + glyph->offset_y + y;
            if (target_y < 0 || target_y >= out_bitmap.height) {
                continue;
            }
            for (int x = 0; x < glyph->width; ++x) {
                const int target_x = pen_x + glyph->offset_x + x;
                if (target_x < 0 || target_x >= out_bitmap.width) {
                    continue;
                }
                const unsigned char alpha = glyph->alpha[y * glyph->width + x];
                if (alpha == 0) {
                    continue;
                }
                unsigned int pixel = 0;
                blend_text_pixel(color, alpha, pixel);
                out_bitmap.pixels[target_y * out_bitmap.width + target_x] = pixel;
            }
        }

        pen_x += glyph->advance;
    }
    return true;
}

}
