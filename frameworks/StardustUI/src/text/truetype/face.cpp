#include "../../../includes/text/truetype/face.hpp"

namespace stardustui::text {

namespace {
constexpr unsigned int kTagCmap = 0x636d6170U;
constexpr unsigned int kTagGlyf = 0x676c7966U;
constexpr unsigned int kTagHead = 0x68656164U;
constexpr unsigned int kTagHhea = 0x68686561U;
constexpr unsigned int kTagHmtx = 0x686d7478U;
constexpr unsigned int kTagLoca = 0x6c6f6361U;
constexpr unsigned int kTagMaxp = 0x6d617870U;
constexpr unsigned int kTagOs2  = 0x4f532f32U;
constexpr int kMaxCompoundDepth = 8;

int min_int(int a, int b)
{
    return a < b ? a : b;
}

void release_font_bytes(stardustui::File::byte*& bytes, int& size)
{
    if (bytes != nullptr) {
        stardustui::File::free_bytes(bytes);
        bytes = nullptr;
    }
    size = 0;
}

}

TrueTypeFace::TrueTypeFace()
    : font_bytes(nullptr),
      font_size(0),
      cmap_offset(0),
      glyf_offset(0),
      head_offset(0),
      hhea_offset(0),
      hmtx_offset(0),
      loca_offset(0),
      maxp_offset(0),
      os2_offset(0),
      units_per_em_value(0),
      ascender_value(0),
      descender_value(0),
      line_height_value(0),
      x_height_value(0),
      index_to_loc_format(0),
      glyph_count(0),
      hmetrics_count(0)
{
}

TrueTypeFace::~TrueTypeFace()
{
    clear();
}

TrueTypeFace::TrueTypeFace(const TrueTypeFace& other)
    : font_bytes(nullptr),
      font_size(0),
      cmap_offset(other.cmap_offset),
      glyf_offset(other.glyf_offset),
      head_offset(other.head_offset),
      hhea_offset(other.hhea_offset),
      hmtx_offset(other.hmtx_offset),
      loca_offset(other.loca_offset),
      maxp_offset(other.maxp_offset),
      os2_offset(other.os2_offset),
      units_per_em_value(other.units_per_em_value),
      ascender_value(other.ascender_value),
      descender_value(other.descender_value),
      line_height_value(other.line_height_value),
      x_height_value(other.x_height_value),
      index_to_loc_format(other.index_to_loc_format),
      glyph_count(other.glyph_count),
      hmetrics_count(other.hmetrics_count)
{
}

TrueTypeFace& TrueTypeFace::operator=(const TrueTypeFace& other)
{
    if (this == &other) {
        return *this;
    }

    clear();
    cmap_offset = other.cmap_offset;
    glyf_offset = other.glyf_offset;
    head_offset = other.head_offset;
    hhea_offset = other.hhea_offset;
    hmtx_offset = other.hmtx_offset;
    loca_offset = other.loca_offset;
    maxp_offset = other.maxp_offset;
    os2_offset = other.os2_offset;
    units_per_em_value = other.units_per_em_value;
    ascender_value = other.ascender_value;
    descender_value = other.descender_value;
    line_height_value = other.line_height_value;
    x_height_value = other.x_height_value;
    index_to_loc_format = other.index_to_loc_format;
    glyph_count = other.glyph_count;
    hmetrics_count = other.hmetrics_count;
    return *this;
}

void TrueTypeFace::clear()
{
    release_font_bytes(font_bytes, font_size);
    cmap_offset = 0;
    glyf_offset = 0;
    head_offset = 0;
    hhea_offset = 0;
    hmtx_offset = 0;
    loca_offset = 0;
    maxp_offset = 0;
    os2_offset = 0;
    units_per_em_value = 0;
    ascender_value = 0;
    descender_value = 0;
    line_height_value = 0;
    x_height_value = 0;
    index_to_loc_format = 0;
    glyph_count = 0;
    hmetrics_count = 0;
}

bool TrueTypeFace::load(const stardustui::string& path)
{
    stardustui::File file(path);
    stardustui::File::byte* bytes = nullptr;
    int size = 0;
    if (!file.read_bytes(bytes, size)) {
        return false;
    }

    clear();
    font_bytes = bytes;
    font_size = size;
    if (!parse_tables()) {
        clear();
        return false;
    }
    return true;
}

bool TrueTypeFace::load(const stardustui::File::byte* data, int size)
{
    clear();
    if (!assign_bytes(data, size)) {
        clear();
        return false;
    }
    if (!parse_tables()) {
        clear();
        return false;
    }
    return true;
}

bool TrueTypeFace::assign_bytes(const stardustui::File::byte* data, int size)
{
    if (size < 0 || (size > 0 && data == nullptr)) {
        return false;
    }

    if (size == 0) {
        return false;
    }

    font_bytes = new stardustui::File::byte[size];
    if (font_bytes == nullptr) {
        return false;
    }

    for (int index = 0; index < size; ++index) {
        font_bytes[index] = data[index];
    }
    font_size = size;
    return true;
}

bool TrueTypeFace::is_loaded() const
{
    return font_bytes != nullptr &&
           font_size > 0 &&
           units_per_em_value > 0 &&
           glyph_count > 0 &&
           hmetrics_count > 0;
}

const stardustui::File::byte* TrueTypeFace::data() const
{
    return font_bytes;
}

int TrueTypeFace::size() const
{
    return font_size;
}

int TrueTypeFace::units_per_em() const
{
    return units_per_em_value;
}

int TrueTypeFace::ascender() const
{
    return ascender_value;
}

int TrueTypeFace::descender() const
{
    return descender_value;
}

int TrueTypeFace::line_height() const
{
    return line_height_value;
}

int TrueTypeFace::x_height() const
{
    return x_height_value;
}

const stardustui::File::byte* TrueTypeFace::byte_at(int offset) const
{
    if (font_bytes == nullptr || offset < 0 || offset >= font_size) {
        return nullptr;
    }
    return font_bytes + offset;
}

unsigned short TrueTypeFace::read_u16(int offset) const
{
    const stardustui::File::byte* bytes = byte_at(offset);
    if (bytes == nullptr || byte_at(offset + 1) == nullptr) {
        return 0;
    }
    return static_cast<unsigned short>((static_cast<unsigned short>(bytes[0]) << 8) |
                                       static_cast<unsigned short>(bytes[1]));
}

short TrueTypeFace::read_s16(int offset) const
{
    return static_cast<short>(read_u16(offset));
}

unsigned int TrueTypeFace::read_u32(int offset) const
{
    const stardustui::File::byte* bytes = byte_at(offset);
    if (bytes == nullptr || byte_at(offset + 3) == nullptr) {
        return 0;
    }
    return (static_cast<unsigned int>(bytes[0]) << 24) |
           (static_cast<unsigned int>(bytes[1]) << 16) |
           (static_cast<unsigned int>(bytes[2]) << 8) |
           static_cast<unsigned int>(bytes[3]);
}

bool TrueTypeFace::locate_required_tables()
{
    if (font_bytes == nullptr || font_size < 12) {
        return false;
    }

    const unsigned int version = read_u32(0);
    const int table_count = read_u16(4);
    if ((version != 0x00010000U && version != 0x74727565U) || font_size < 12 + table_count * 16) {
        return false;
    }

    cmap_offset = 0;
    glyf_offset = 0;
    head_offset = 0;
    hhea_offset = 0;
    hmtx_offset = 0;
    loca_offset = 0;
    maxp_offset = 0;
    os2_offset = 0;

    for (int index = 0; index < table_count; ++index) {
        const int entry_offset = 12 + index * 16;
        const unsigned int tag = read_u32(entry_offset);
        const int offset = static_cast<int>(read_u32(entry_offset + 8));
        const int span = static_cast<int>(read_u32(entry_offset + 12));
        if (offset < 0 || span < 0 || offset + span > font_size) {
            return false;
        }

        int* target = nullptr;
        if (tag == kTagCmap) {
            target = &cmap_offset;
        } else if (tag == kTagGlyf) {
            target = &glyf_offset;
        } else if (tag == kTagHead) {
            target = &head_offset;
        } else if (tag == kTagHhea) {
            target = &hhea_offset;
        } else if (tag == kTagHmtx) {
            target = &hmtx_offset;
        } else if (tag == kTagLoca) {
            target = &loca_offset;
        } else if (tag == kTagMaxp) {
            target = &maxp_offset;
        } else if (tag == kTagOs2) {
            target = &os2_offset;
        } else {
            continue;
        }

        *target = offset;
    }

    return cmap_offset != 0 &&
           glyf_offset != 0 &&
           head_offset != 0 &&
           hhea_offset != 0 &&
           hmtx_offset != 0 &&
           loca_offset != 0 &&
           maxp_offset != 0 &&
           os2_offset != 0;
}

bool TrueTypeFace::parse_tables()
{
    if (font_bytes == nullptr || font_size <= 0) {
        return false;
    }

    if (!locate_required_tables()) {
        return false;
    }

    units_per_em_value = read_u16(head_offset + 18);
    ascender_value = read_s16(os2_offset + 68);
    descender_value = read_s16(os2_offset + 70);
    line_height_value = ascender_value - descender_value;
    const int os2_version = read_u16(os2_offset);
    x_height_value = os2_version >= 2 ? read_s16(os2_offset + 86) : 0;
    index_to_loc_format = read_u16(head_offset + 50);
    glyph_count = read_u16(maxp_offset + 4);
    hmetrics_count = read_u16(hhea_offset + 34);

    if (units_per_em_value <= 0 || glyph_count <= 0 || hmetrics_count <= 0) {
        return false;
    }
    return true;
}

int TrueTypeFace::glyph_index_from_codepoint(unsigned int codepoint) const
{
    if (!is_loaded()) {
        return 0;
    }

    if (codepoint == '\t' || codepoint == '\v' || codepoint == '\f' ||
        codepoint == '\r' || codepoint == '\n') {
        codepoint = ' ';
    }

    const int table_count = read_u16(cmap_offset + 2);
    int format12 = 0;
    int format4 = 0;
    int format0 = 0;

    for (int table = 0; table < table_count; ++table) {
        const int record_offset = cmap_offset + 4 + table * 8;
        const int platform = read_u16(record_offset);
        const int encoding = read_u16(record_offset + 2);
        const int subtable_offset = static_cast<int>(read_u32(record_offset + 4));
        const int format = read_u16(cmap_offset + subtable_offset);

        if (platform == 3 && encoding == 10 && format == 12) {
            format12 = cmap_offset + subtable_offset;
        } else if (platform == 3 && encoding == 1 && format == 4) {
            format4 = cmap_offset + subtable_offset;
        } else if (format == 0) {
            format0 = cmap_offset + subtable_offset;
        }
    }

    if (format12 != 0) {
        const int group_count = static_cast<int>(read_u32(format12 + 12));
        for (int group = 0; group < group_count; ++group) {
            const int group_offset = format12 + 16 + group * 12;
            const unsigned int start = read_u32(group_offset);
            const unsigned int end = read_u32(group_offset + 4);
            const unsigned int glyph = read_u32(group_offset + 8);
            if (start <= codepoint && codepoint <= end) {
                return static_cast<int>(glyph + (codepoint - start));
            }
        }
    }

    if (format4 != 0 && codepoint <= 0xFFFFU) {
        const int segment_count_x2 = read_u16(format4 + 6);
        const int end_code_offset = format4 + 14;
        const int start_code_offset = end_code_offset + 2 + segment_count_x2;
        const int id_delta_offset = start_code_offset + segment_count_x2;
        const int id_range_offset_offset = id_delta_offset + segment_count_x2;

        for (int segment = 0; segment < segment_count_x2; segment += 2) {
            const int end_code = read_u16(end_code_offset + segment);
            const int start_code = read_u16(start_code_offset + segment);
            if (codepoint < static_cast<unsigned int>(start_code) ||
                codepoint > static_cast<unsigned int>(end_code)) {
                continue;
            }

            const int delta = read_s16(id_delta_offset + segment);
            const int range_offset = read_u16(id_range_offset_offset + segment);
            if (range_offset == 0) {
                return static_cast<int>((codepoint + static_cast<unsigned int>(delta)) & 0xFFFFU);
            }

            const int glyph_offset = id_range_offset_offset + segment + range_offset +
                                     static_cast<int>(codepoint - static_cast<unsigned int>(start_code)) * 2;
            const int glyph = read_u16(glyph_offset);
            if (glyph == 0) {
                return 0;
            }
            return (glyph + delta) & 0xFFFF;
        }
    }

    if (format0 != 0 && codepoint < 256U) {
        const stardustui::File::byte* value = byte_at(format0 + 6 + static_cast<int>(codepoint));
        return value == nullptr ? 0 : static_cast<int>(value[0]);
    }

    return 0;
}

int TrueTypeFace::glyph_advance_width(int glyph_index) const
{
    if (!is_loaded() || glyph_index < 0) {
        return 0;
    }
    const int entry = min_int(glyph_index, hmetrics_count - 1);
    return read_u16(hmtx_offset + entry * 4);
}

int TrueTypeFace::glyph_left_side_bearing(int glyph_index) const
{
    if (!is_loaded() || glyph_index < 0) {
        return 0;
    }
    if (glyph_index < hmetrics_count) {
        return read_s16(hmtx_offset + glyph_index * 4 + 2);
    }
    return read_s16(hmtx_offset + hmetrics_count * 4 + (glyph_index - hmetrics_count) * 2);
}

bool TrueTypeFace::append_transformed_outline(const GlyphOutline& component,
                                              float a,
                                              float b,
                                              float c,
                                              float d,
                                              float e,
                                              float f,
                                              GlyphOutline& out_outline) const
{
    const int point_base = out_outline.points.size();
    for (int index = 0; index < component.points.size(); ++index) {
        GlyphOutlinePoint point = component.points[index];
        const float x = point.x;
        const float y = point.y;
        point.x = a * x + c * y + e;
        point.y = b * x + d * y + f;
        if (!out_outline.points.push_back(point)) {
            return false;
        }
    }

    for (int index = 0; index < component.contours.size(); ++index) {
        GlyphContour contour = component.contours[index];
        contour.start += point_base;
        contour.end += point_base;
        if (!out_outline.contours.push_back(contour)) {
            return false;
        }
    }
    return true;
}

bool TrueTypeFace::load_compound_glyph(int glyph_index, GlyphOutline& out_outline, int depth) const
{
    if (depth > kMaxCompoundDepth || glyph_index < 0 || glyph_index >= glyph_count) {
        return false;
    }

    const int glyph_start = glyf_offset +
        (index_to_loc_format ? static_cast<int>(read_u32(loca_offset + glyph_index * 4))
                             : static_cast<int>(read_u16(loca_offset + glyph_index * 2)) * 2);
    const int glyph_end = glyf_offset +
        (index_to_loc_format ? static_cast<int>(read_u32(loca_offset + glyph_index * 4 + 4))
                             : static_cast<int>(read_u16(loca_offset + glyph_index * 2 + 2)) * 2);
    if (glyph_start >= glyph_end) {
        return true;
    }

    const int contour_count = read_s16(glyph_start);
    if (contour_count >= 0) {
        return load_glyph_outline(glyph_index, out_outline);
    }

    int offset = glyph_start + 10;
    while (true) {
        const int flags = read_u16(offset);
        const int component_index = read_u16(offset + 2);
        if ((flags & 2) == 0) {
            return false;
        }

        float e = 0.0f;
        float f = 0.0f;
        if ((flags & 1) != 0) {
            e = static_cast<float>(read_s16(offset + 4));
            f = static_cast<float>(read_s16(offset + 6));
            offset += 8;
        } else {
            const stardustui::File::byte* ptr = byte_at(offset + 4);
            const stardustui::File::byte* ptr2 = byte_at(offset + 5);
            if (ptr == nullptr || ptr2 == nullptr) {
                return false;
            }
            e = static_cast<float>(static_cast<signed char>(ptr[0]));
            f = static_cast<float>(static_cast<signed char>(ptr2[0]));
            offset += 6;
        }

        float a = 1.0f;
        float b = 0.0f;
        float c = 0.0f;
        float d = 1.0f;
        if ((flags & 0x0008) != 0) {
            a = static_cast<float>(read_s16(offset)) / 16384.0f;
            d = a;
            offset += 2;
        } else if ((flags & 0x0040) != 0) {
            a = static_cast<float>(read_s16(offset)) / 16384.0f;
            d = static_cast<float>(read_s16(offset + 2)) / 16384.0f;
            offset += 4;
        } else if ((flags & 0x0080) != 0) {
            a = static_cast<float>(read_s16(offset)) / 16384.0f;
            b = static_cast<float>(read_s16(offset + 2)) / 16384.0f;
            c = static_cast<float>(read_s16(offset + 4)) / 16384.0f;
            d = static_cast<float>(read_s16(offset + 6)) / 16384.0f;
            offset += 8;
        }

        GlyphOutline component;
        if (!load_compound_glyph(component_index, component, depth + 1)) {
            return false;
        }
        if (!append_transformed_outline(component, a, b, c, d, e, f, out_outline)) {
            return false;
        }

        if ((flags & 0x0020) == 0) {
            break;
        }
    }

    return true;
}

bool TrueTypeFace::load_glyph_outline(int glyph_index, GlyphOutline& out_outline) const
{
    out_outline.clear();
    if (!is_loaded() || glyph_index < 0 || glyph_index >= glyph_count) {
        return false;
    }

    out_outline.advance_width = glyph_advance_width(glyph_index);
    out_outline.left_side_bearing = glyph_left_side_bearing(glyph_index);

    const int glyph_start = glyf_offset +
        (index_to_loc_format ? static_cast<int>(read_u32(loca_offset + glyph_index * 4))
                             : static_cast<int>(read_u16(loca_offset + glyph_index * 2)) * 2);
    const int glyph_end = glyf_offset +
        (index_to_loc_format ? static_cast<int>(read_u32(loca_offset + glyph_index * 4 + 4))
                             : static_cast<int>(read_u16(loca_offset + glyph_index * 2 + 2)) * 2);

    if (glyph_start >= glyph_end) {
        return true;
    }

    const int contour_count = read_s16(glyph_start);
    out_outline.x_min = read_s16(glyph_start + 2);
    out_outline.y_min = read_s16(glyph_start + 4);
    out_outline.x_max = read_s16(glyph_start + 6);
    out_outline.y_max = read_s16(glyph_start + 8);

    if (contour_count < 0) {
        return load_compound_glyph(glyph_index, out_outline, 0);
    }

    const int end_pts_offset = glyph_start + 10;
    const int instruction_length = read_u16(end_pts_offset + contour_count * 2);
    const int flags_offset = end_pts_offset + contour_count * 2 + 2 + instruction_length;

    int point_count = 0;
    if (contour_count > 0) {
        point_count = read_u16(end_pts_offset + (contour_count - 1) * 2) + 1;
    }
    if (point_count <= 0) {
        return true;
    }

    stardustui::vector<unsigned char> flags;
    if (!flags.reserve(point_count)) {
        return false;
    }

    int flag_cursor = flags_offset;
    while (flags.size() < point_count) {
        const stardustui::File::byte* flag_ptr = byte_at(flag_cursor++);
        if (flag_ptr == nullptr) {
            return false;
        }
        const unsigned char flag = flag_ptr[0];
        if (!flags.push_back(flag)) {
            return false;
        }
        if ((flag & 0x08) != 0) {
            const stardustui::File::byte* repeat_ptr = byte_at(flag_cursor++);
            if (repeat_ptr == nullptr) {
                return false;
            }
            const int repeat_count = repeat_ptr[0];
            for (int repeat = 0; repeat < repeat_count; ++repeat) {
                if (!flags.push_back(flag)) {
                    return false;
                }
            }
        }
    }

    int x_offset = flag_cursor;
    int y_offset = x_offset;
    for (int index = 0; index < point_count; ++index) {
        const unsigned char flag = flags[index];
        y_offset += (flag & 0x02) ? 1 : ((flag & 0x10) ? 0 : 2);
    }

    int current_x = 0;
    int current_y = 0;
    for (int index = 0; index < point_count; ++index) {
        const unsigned char flag = flags[index];
        if ((flag & 0x02) != 0) {
            const stardustui::File::byte* ptr = byte_at(x_offset++);
            if (ptr == nullptr) {
                return false;
            }
            current_x += (flag & 0x10) ? ptr[0] : -static_cast<int>(ptr[0]);
        } else if ((flag & 0x10) == 0) {
            current_x += read_s16(x_offset);
            x_offset += 2;
        }

        if ((flag & 0x04) != 0) {
            const stardustui::File::byte* ptr = byte_at(y_offset++);
            if (ptr == nullptr) {
                return false;
            }
            current_y += (flag & 0x20) ? ptr[0] : -static_cast<int>(ptr[0]);
        } else if ((flag & 0x20) == 0) {
            current_y += read_s16(y_offset);
            y_offset += 2;
        }

        GlyphOutlinePoint point;
        point.x = static_cast<float>(current_x);
        point.y = static_cast<float>(current_y);
        point.on_curve = (flag & 0x01) != 0;
        if (!out_outline.points.push_back(point)) {
            return false;
        }
    }

    int contour_start = 0;
    for (int contour = 0; contour < contour_count; ++contour) {
        GlyphContour entry;
        entry.start = contour_start;
        entry.end = read_u16(end_pts_offset + contour * 2);
        if (!out_outline.contours.push_back(entry)) {
            return false;
        }
        contour_start = entry.end + 1;
    }

    return true;
}

}
