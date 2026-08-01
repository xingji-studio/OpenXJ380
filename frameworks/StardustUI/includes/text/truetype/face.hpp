#pragma once

#include "../../file.hpp"
#include "../../string.hpp"
#include "../../vector.hpp"

namespace stardustui::text {

struct GlyphOutlinePoint {
    float x;
    float y;
    bool on_curve;

    GlyphOutlinePoint() : x(0.0f), y(0.0f), on_curve(false) {}
};

struct GlyphContour {
    int start;
    int end;

    GlyphContour() : start(0), end(-1) {}
};

struct GlyphOutline {
    stardustui::vector<GlyphOutlinePoint> points;
    stardustui::vector<GlyphContour> contours;
    int advance_width;
    int left_side_bearing;
    int x_min;
    int y_min;
    int x_max;
    int y_max;

    GlyphOutline()
        : points(),
          contours(),
          advance_width(0),
          left_side_bearing(0),
          x_min(0),
          y_min(0),
          x_max(0),
          y_max(0) {}

    void clear()
    {
        points.clear();
        contours.clear();
        advance_width = 0;
        left_side_bearing = 0;
        x_min = 0;
        y_min = 0;
        x_max = 0;
        y_max = 0;
    }
};

class TrueTypeFace {
public:
    TrueTypeFace();
    ~TrueTypeFace();
    TrueTypeFace(const TrueTypeFace& other);
    TrueTypeFace& operator=(const TrueTypeFace& other);

    bool load(const stardustui::string& path);
    bool load(const stardustui::File::byte* data, int size);
    void clear();

    bool is_loaded() const;
    const stardustui::File::byte* data() const;
    int size() const;

    int units_per_em() const;
    int ascender() const;
    int descender() const;
    int line_height() const;
    int x_height() const;

    int glyph_index_from_codepoint(unsigned int codepoint) const;
    int glyph_advance_width(int glyph_index) const;
    int glyph_left_side_bearing(int glyph_index) const;
    bool load_glyph_outline(int glyph_index, GlyphOutline& out_outline) const;

private:
    bool assign_bytes(const stardustui::File::byte* data, int size);
    bool parse_tables();
    bool load_compound_glyph(int glyph_index, GlyphOutline& out_outline, int depth) const;

    unsigned short read_u16(int offset) const;
    short read_s16(int offset) const;
    unsigned int read_u32(int offset) const;
    const stardustui::File::byte* byte_at(int offset) const;

    bool locate_required_tables();
    bool append_transformed_outline(const GlyphOutline& component,
                                    float a,
                                    float b,
                                    float c,
                                    float d,
                                    float e,
                                    float f,
                                    GlyphOutline& out_outline) const;

    stardustui::File::byte* font_bytes;
    int font_size;
    int cmap_offset;
    int glyf_offset;
    int head_offset;
    int hhea_offset;
    int hmtx_offset;
    int loca_offset;
    int maxp_offset;
    int os2_offset;

    int units_per_em_value;
    int ascender_value;
    int descender_value;
    int line_height_value;
    int x_height_value;
    int index_to_loc_format;
    int glyph_count;
    int hmetrics_count;
};

}
