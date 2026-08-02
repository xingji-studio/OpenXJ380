#define STB_TRUETYPE_IMPLEMENTATION
#include "./stb_ttf.h"
#include "krlibc.h"
#include "stdarg.h"
#include "stdint.h"
#include <fs/vfs/vfs.h>
#include <graphics/GOP.hpp>
#include <graphics/sheet.h>
#include <math.hpp>
#include <proto.hpp>
#include <ttf.h>

char          *ttf_buffer;
stbtt_fontinfo font;

extern char          *ttf_buffer_c;
extern stbtt_fontinfo font_c;

extern char          *ttf_buffer_m;
extern stbtt_fontinfo font_m;

static uint16_t ttf_u16(const uint8_t *data)
{
    return (uint16_t)((data[0] << 8) | data[1]);
}

static int16_t ttf_s16(const uint8_t *data)
{
    return (int16_t)ttf_u16(data);
}

static uint32_t ttf_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

typedef struct TTFTable
{
    uint32_t offset;
    uint32_t length;
} TTFTable;

static bool ttf_range_fits(size_t size, size_t offset, size_t length)
{
    return offset <= size && length <= size - offset;
}

static bool ttf_find_table(const uint8_t *data, size_t size, uint32_t font_offset, const char tag[4], TTFTable *table)
{
    if (!ttf_range_fits(size, font_offset, 12)) { return false; }

    uint16_t num_tables = ttf_u16(data + font_offset + 4);
    if (num_tables == 0 || num_tables > 128 || !ttf_range_fits(size, font_offset + 12, (size_t)num_tables * 16)) {
        return false;
    }

    for (uint16_t i = 0; i < num_tables; ++i) {
        const uint8_t *entry = data + font_offset + 12 + (size_t)i * 16;
        if (entry[0] == (uint8_t)tag[0] && entry[1] == (uint8_t)tag[1] && entry[2] == (uint8_t)tag[2] &&
            entry[3] == (uint8_t)tag[3]) {
            table->offset = ttf_u32(entry + 8);
            table->length = ttf_u32(entry + 12);
            return ttf_range_fits(size, table->offset, table->length);
        }
    }

    return false;
}

static bool ttf_validate_cmap_subtable(const uint8_t *table, uint32_t table_size, uint32_t offset)
{
    if (!ttf_range_fits(table_size, offset, 2)) { return false; }

    uint16_t format = ttf_u16(table + offset);
    if (format == 0) {
        if (!ttf_range_fits(table_size, offset, 6)) { return false; }
        uint16_t length = ttf_u16(table + offset + 2);
        return length >= 262 && ttf_range_fits(table_size, offset, length);
    }

    if (format == 4) {
        if (!ttf_range_fits(table_size, offset, 16)) { return false; }
        uint16_t length = ttf_u16(table + offset + 2);
        if (length < 16 || !ttf_range_fits(table_size, offset, length)) { return false; }

        uint16_t seg_count = ttf_u16(table + offset + 6) / 2;
        if (seg_count == 0 || length < 16 + (uint32_t)seg_count * 8) { return false; }

        const uint8_t *subtable = table + offset;
        for (uint16_t i = 0; i < seg_count; ++i) {
            uint16_t end    = ttf_u16(subtable + 14 + (uint32_t)i * 2);
            uint16_t start  = ttf_u16(subtable + 14 + (uint32_t)seg_count * 2 + 2 + (uint32_t)i * 2);
            uint16_t range_offset =
                ttf_u16(subtable + 14 + (uint32_t)seg_count * 6 + 2 + (uint32_t)i * 2);
            if (start > end) { return false; }
            if (range_offset != 0) {
                uint32_t range_offset_pos = 14 + (uint32_t)seg_count * 6 + 2 + (uint32_t)i * 2;
                uint32_t last_glyph_pos   = range_offset_pos + range_offset + ((uint32_t)end - start) * 2;
                if (!ttf_range_fits(length, last_glyph_pos, 2)) { return false; }
            }
        }
        return true;
    }

    if (format == 6) {
        if (!ttf_range_fits(table_size, offset, 10)) { return false; }
        uint16_t length = ttf_u16(table + offset + 2);
        uint16_t count  = ttf_u16(table + offset + 8);
        return length >= 10 + (uint32_t)count * 2 && ttf_range_fits(table_size, offset, length);
    }

    if (format == 12 || format == 13) {
        if (!ttf_range_fits(table_size, offset, 16)) { return false; }
        uint32_t length  = ttf_u32(table + offset + 4);
        uint32_t ngroups = ttf_u32(table + offset + 12);
        return length >= 16 && ngroups <= (length - 16) / 12 && ttf_range_fits(table_size, offset, length);
    }

    return false;
}

static bool ttf_validate_cmap(const uint8_t *data, const TTFTable *cmap)
{
    if (cmap->length < 4) { return false; }

    const uint8_t *table      = data + cmap->offset;
    uint16_t       num_tables = ttf_u16(table + 2);
    if (num_tables == 0 || cmap->length < 4 + (uint32_t)num_tables * 8) { return false; }

    uint32_t selected_offset = 0;
    bool     selected        = false;
    for (uint16_t i = 0; i < num_tables; ++i) {
        const uint8_t *record   = table + 4 + (uint32_t)i * 8;
        uint16_t       platform = ttf_u16(record);
        uint16_t       encoding = ttf_u16(record + 2);
        uint32_t       offset   = ttf_u32(record + 4);

        if (platform == 3 && (encoding == 1 || encoding == 10)) {
            selected_offset = offset;
            selected        = true;
        } else if (platform == 0) {
            selected_offset = offset;
            selected        = true;
        }
    }

    return selected && ttf_validate_cmap_subtable(table, cmap->length, selected_offset);
}

static bool ttf_validate_glyf(const uint8_t *data, const TTFTable *glyf, const TTFTable *loca, uint16_t num_glyphs,
                              uint16_t loc_format)
{
    uint32_t previous_offset = 0;
    for (uint32_t glyph = 0; glyph < num_glyphs; ++glyph) {
        uint32_t glyph_offset;
        uint32_t next_offset;
        if (loc_format == 0) {
            glyph_offset = (uint32_t)ttf_u16(data + loca->offset + glyph * 2) * 2;
            next_offset  = (uint32_t)ttf_u16(data + loca->offset + (glyph + 1) * 2) * 2;
        } else {
            glyph_offset = ttf_u32(data + loca->offset + glyph * 4);
            next_offset  = ttf_u32(data + loca->offset + (glyph + 1) * 4);
        }

        if (glyph_offset < previous_offset || next_offset < glyph_offset || next_offset > glyf->length) {
            return false;
        }
        previous_offset = glyph_offset;

        uint32_t glyph_size = next_offset - glyph_offset;
        if (glyph_size == 0) { continue; }
        if (glyph_size < 10) { return false; }

        const uint8_t *glyph_data         = data + glyf->offset + glyph_offset;
        int16_t        number_of_contours = ttf_s16(glyph_data);
        if (number_of_contours > 0) {
            uint32_t contours = (uint16_t)number_of_contours;
            uint32_t end_pts  = 10 + contours * 2;
            if (!ttf_range_fits(glyph_size, end_pts, 2)) { return false; }

            uint16_t last_point = ttf_u16(glyph_data + end_pts - 2);
            uint32_t points     = (uint32_t)last_point + 1;
            uint16_t ins_len    = ttf_u16(glyph_data + end_pts);
            uint32_t pos        = end_pts + 2 + ins_len;
            if (!ttf_range_fits(glyph_size, pos, 0)) { return false; }

            uint32_t x_bytes = 0;
            uint32_t y_bytes = 0;
            for (uint32_t i = 0; i < points; ++i) {
                if (!ttf_range_fits(glyph_size, pos, 1)) { return false; }
                uint8_t flags  = glyph_data[pos++];
                uint32_t count = 1;
                if (flags & 8) {
                    if (!ttf_range_fits(glyph_size, pos, 1)) { return false; }
                    count += glyph_data[pos++];
                }
                if (count > points - i) { return false; }
                for (uint32_t j = 0; j < count; ++j) {
                    if (flags & 2) {
                        x_bytes += 1;
                    } else if (!(flags & 16)) {
                        x_bytes += 2;
                    }
                    if (flags & 4) {
                        y_bytes += 1;
                    } else if (!(flags & 32)) {
                        y_bytes += 2;
                    }
                }
                i += count - 1;
            }
            if (!ttf_range_fits(glyph_size, pos, x_bytes) || !ttf_range_fits(glyph_size, pos + x_bytes, y_bytes)) {
                return false;
            }
        } else if (number_of_contours < 0) {
            uint32_t pos        = 10;
            bool     more       = true;
            bool     have_instructions = false;
            uint32_t components = 0;
            while (more) {
                if (++components > 1024 || !ttf_range_fits(glyph_size, pos, 4)) { return false; }
                uint16_t flags           = ttf_u16(glyph_data + pos);
                uint16_t component_glyph = ttf_u16(glyph_data + pos + 2);
                pos += 4;
                if (component_glyph >= num_glyphs) { return false; }
                if (flags & (1 << 8)) { have_instructions = true; }

                pos += (flags & 1) ? 4 : 2;
                if (flags & (1 << 3)) {
                    pos += 2;
                } else if (flags & (1 << 6)) {
                    pos += 4;
                } else if (flags & (1 << 7)) {
                    pos += 8;
                }
                if (!ttf_range_fits(glyph_size, pos, 0)) { return false; }
                more = (flags & (1 << 5)) != 0;
            }
            if (have_instructions) {
                if (!ttf_range_fits(glyph_size, pos, 2)) { return false; }
                uint16_t instructions = ttf_u16(glyph_data + pos);
                if (!ttf_range_fits(glyph_size, pos + 2, instructions)) { return false; }
            }
        }
    }

    return true;
}

static bool ttf_validate_font_data(const uint8_t *data, size_t size, uint32_t *font_offset_out)
{
    if (size < 12) { return false; }

    uint32_t sfnt = ttf_u32(data);
    if (sfnt == 0x4f54544f) {
        write_serial_string("init_ttf: OpenType/CFF fonts are not accepted by the kernel stb path\n");
        return false;
    }
    if (sfnt != 0x00010000 && sfnt != 0x74727565) { return false; }

    uint16_t num_tables = ttf_u16(data + 4);
    if (num_tables == 0 || num_tables > 128 || !ttf_range_fits(size, 12, (size_t)num_tables * 16)) {
        return false;
    }

    for (uint16_t i = 0; i < num_tables; ++i) {
        TTFTable table = {
            .offset = ttf_u32(data + 12 + (size_t)i * 16 + 8),
            .length = ttf_u32(data + 12 + (size_t)i * 16 + 12),
        };
        if (!ttf_range_fits(size, table.offset, table.length)) { return false; }
    }

    TTFTable cmap;
    TTFTable head;
    TTFTable hhea;
    TTFTable hmtx;
    TTFTable maxp;
    TTFTable loca;
    TTFTable glyf;
    if (!ttf_find_table(data, size, 0, "cmap", &cmap) || !ttf_find_table(data, size, 0, "head", &head) ||
        !ttf_find_table(data, size, 0, "hhea", &hhea) || !ttf_find_table(data, size, 0, "hmtx", &hmtx) ||
        !ttf_find_table(data, size, 0, "maxp", &maxp) || !ttf_find_table(data, size, 0, "loca", &loca) ||
        !ttf_find_table(data, size, 0, "glyf", &glyf)) {
        return false;
    }

    if (head.length < 54 || hhea.length < 36 || maxp.length < 6) { return false; }

    uint16_t num_glyphs          = ttf_u16(data + maxp.offset + 4);
    uint16_t num_long_h_metrics  = ttf_u16(data + hhea.offset + 34);
    uint16_t index_to_loc_format = ttf_u16(data + head.offset + 50);
    if (num_glyphs == 0 || num_long_h_metrics == 0 || num_long_h_metrics > num_glyphs ||
        index_to_loc_format > 1) {
        return false;
    }

    size_t hmtx_min = (size_t)num_long_h_metrics * 4 + (size_t)(num_glyphs - num_long_h_metrics) * 2;
    size_t loca_min = (size_t)(num_glyphs + 1) * (index_to_loc_format == 0 ? 2 : 4);
    if (hmtx.length < hmtx_min || loca.length < loca_min) { return false; }
    if (!ttf_validate_cmap(data, &cmap)) { return false; }
    if (!ttf_validate_glyf(data, &glyf, &loca, num_glyphs, index_to_loc_format)) { return false; }

    *font_offset_out = 0;
    return true;
}

static char *ttf_alloc_file_buffer(size_t bytes)
{
    size_t pages = PADDING_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    uint64_t phys = alloc_frames(pages);
    if (phys == 0) {
        return NULL;
    }
    return (char *)phys_to_virt(phys);
}

static void ttf_free_file_buffer(char *buffer, size_t bytes)
{
    if (buffer == NULL || bytes == 0) {
        return;
    }
    free_frames((uint64_t)virt_to_phys((uint64_t)buffer), PADDING_UP(bytes, PAGE_SIZE) / PAGE_SIZE);
}

static bool ttf_load_font_file(const char *path, char **buffer_out, stbtt_fontinfo *font_out)
{
    vfs_node_t v = vfs_open(path);
    if (!v)
    {
        write_serial_fmt("init_ttf: cannot open '%s'\n", path);
        return false;
    }

    size_t size = v->size;
    if (size == 0 || size > 32 * 1024 * 1024 || size > (size_t)-1 - PAGE_SIZE)
    {
        write_serial_fmt("init_ttf: invalid font size '%s' (%u bytes)\n", path, (unsigned)size);
        vfs_close(v);
        return false;
    }

    size_t allocation_size = size + PAGE_SIZE;
    char  *buffer = ttf_alloc_file_buffer(allocation_size);
    uint64_t read_begin = nanoTime();
    if (!buffer)
    {
        write_serial_fmt("init_ttf: cannot allocate '%s' (%u bytes)\n", path, (unsigned)size);
        vfs_close(v);
        return false;
    }
    memset(buffer, 0, allocation_size);

    size_t got = vfs_read(v, (uint8_t *)buffer, 0, size);
    uint64_t read_end = nanoTime();
    vfs_close(v);
    if (got != size)
    {
        write_serial_fmt("init_ttf: short read '%s' expected=%u got=%u\n", path, (unsigned)size, (unsigned)got);
        ttf_free_file_buffer(buffer, allocation_size);
        return false;
    }

    uint32_t font_offset = 0;
    if (!ttf_validate_font_data((uint8_t *)buffer, size, &font_offset))
    {
        write_serial_fmt("init_ttf: rejected unsafe or unsupported font '%s' magic=%02x%02x%02x%02x\n", path,
                         (uint8_t)buffer[0], (uint8_t)buffer[1], (uint8_t)buffer[2], (uint8_t)buffer[3]);
        ttf_free_file_buffer(buffer, allocation_size);
        return false;
    }

    uint64_t init_begin  = nanoTime();
    bool     ok          = stbtt_InitFont(font_out, (uint8_t *)buffer, (int)font_offset);
    uint64_t init_end    = nanoTime();
    if (!ok)
    {
        write_serial_fmt("init_ttf: invalid font '%s' magic=%02x%02x%02x%02x\n", path, (uint8_t)buffer[0],
                         (uint8_t)buffer[1], (uint8_t)buffer[2], (uint8_t)buffer[3]);
        ttf_free_file_buffer(buffer, allocation_size);
        return false;
    }

    write_serial_fmt("TTF: loaded '%s' size=%u read=%llums init=%llums magic=%02x%02x%02x%02x\n", path,
                     (unsigned)size, (read_end - read_begin) / 1000000ULL, (init_end - init_begin) / 1000000ULL,
                     (uint8_t)buffer[0], (uint8_t)buffer[1], (uint8_t)buffer[2], (uint8_t)buffer[3]);

    *buffer_out = buffer;
    return true;
}

SHEET_BUFFER LCD_AlphaBlend(SHEET_BUFFER foreground_color, SHEET_BUFFER background_color, uint8_t alpha)
{
    uint8_t       text_r   = foreground_color.r;
    uint8_t       text_g   = foreground_color.g;
    uint8_t       text_b   = foreground_color.b;
    uint8_t       text_a   = foreground_color.a;
    uint8_t       coverage = alpha;
    SHEET_BUFFER *dst      = &background_color;

    // 转换为浮点运算（0.0-1.0范围）
    float cov       = coverage / 255.0f;
    float txt_alpha = text_a / 255.0f;
    float src_alpha = cov * txt_alpha;

    // 源颜色预乘alpha
    float src_r = (text_r / 255.0f) * src_alpha;
    float src_g = (text_g / 255.0f) * src_alpha;
    float src_b = (text_b / 255.0f) * src_alpha;

    // 目标颜色预乘alpha
    float dst_a = dst->a / 255.0f;
    float dst_r = (dst->r / 255.0f) * dst_a;
    float dst_g = (dst->g / 255.0f) * dst_a;
    float dst_b = (dst->b / 255.0f) * dst_a;

    // 混合计算
    float blended_alpha = src_alpha + dst_a * (1.0f - src_alpha);

    // 颜色混合
    float blended_r = (src_r + dst_r * (1.0f - src_alpha)) / blended_alpha;
    float blended_g = (src_g + dst_g * (1.0f - src_alpha)) / blended_alpha;
    float blended_b = (src_b + dst_b * (1.0f - src_alpha)) / blended_alpha;

    // 转换回0-255并存储
    dst->r = (uint8_t)(blended_r * 255 + 0.5f);
    dst->g = (uint8_t)(blended_g * 255 + 0.5f);
    dst->b = (uint8_t)(blended_b * 255 + 0.5f);
    dst->a = (uint8_t)(blended_alpha * 255 + 0.5f);
    return *dst;
}

void set_size(int s1) {}

int roundf(double n)
{
    // 若为负数，则先化为正数再进行四舍五入
    if (n > 0)
        return n - int(n) >= 0.5 ? int(n) + 1 : int(n);
    else
        return -n - int(-n) >= 0.5 ? -(int(-n) + 1) : -int(-n);
}

unsigned char *TTF_PrintFont(stbtt_fontinfo *font_info, int *buf, unsigned *width, unsigned *heigh, int size);

unsigned char *TTF_Print(int *buf, unsigned *width, unsigned *heigh, int size)
{
    return TTF_PrintFont(&font, buf, width, heigh, size);
}

unsigned char *TTF_PrintFont(stbtt_fontinfo *font_info, int *buf, unsigned *width, unsigned *heigh, int size)
{
    /* 创建位图 */
    int            bitmap_w = 2560; /* 位图的宽 */
    int            bitmap_h = 1440; /* 位图的高 */
    unsigned char *bitmap =
        (unsigned char *)(phys_to_virt(alloc_frames((bitmap_w * bitmap_h + PAGE_SIZE - 1) / PAGE_SIZE)));
    if (!bitmap) { return NULL; }
    memset((void *)(bitmap), 0, bitmap_w * bitmap_h);

    if (font_info == NULL || buf == NULL || size <= 0 || size > 256 || font_info->numGlyphs <= 0)
    {
        *width = 0;
        *heigh = 0;
        return bitmap;
    }

    /* "STB"的 unicode 编码 */
    int *word = buf;

    /* 计算字体缩放 */
    float pixels = size * 2;                                 /* 字体大小（字号） */
    float scale  = stbtt_ScaleForPixelHeight(font_info, pixels); /* scale = pixels / (ascent - descent) */

    /**
	 * 获取垂直方向上的度量
	 * ascent：字体从基线到顶部的高度；
	 * descent：基线到底部的高度，通常为负值；
	 * lineGap：两个字体之间的间距；
	 * 行间距为：ascent - descent + lineGap。
	 */
    int ascent  = 0;
    int descent = 0;
    int lineGap = 0;
    stbtt_GetFontVMetrics(font_info, &ascent, &descent, &lineGap);

    /* 根据缩放调整字高 */
    ascent  = roundf(ascent * scale);
    descent = roundf(descent * scale);

    int x = 0; /*位图的x*/
    int render_right = 0;

    int offset_x = 0; /* 整体的x偏移量 */
    /* 循环加载word中每个字符 */
    unsigned h = (unsigned)MIN(bitmap_h, MAX(0, (int)((float)(ascent - descent + lineGap * scale))));
    for (int i = 0; word[i] != 0; ++i)
    {
        int glyph = stbtt_FindGlyphIndex(font_info, word[i]);
        if (glyph < 0 || glyph >= font_info->numGlyphs) { glyph = 0; }

        /**
		 * 获取水平方向上的度量
		 * advanceWidth：字宽；
		 * 字形绘制位置使用 bitmap box 的 x1，不能直接使用 left side bearing。
		 */
        int advanceWidth    = 0;
        stbtt_GetGlyphHMetrics(font_info, glyph, &advanceWidth, NULL);

        /* 获取字符的边框（边界） */
        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetGlyphBitmapBox(font_info, glyph, scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        /* 计算位图的y (不同字符的高度不同） */
        int y = ascent + c_y1;

        /* 渲染字符 */
        if (i <= 0) { offset_x = -c_x1; } /* 把首个字符的左侧偏移，给整体偏移回去 */
        int glyph_w = c_x2 - c_x1;
        int glyph_h = c_y2 - c_y1;
        int dst_x   = x + c_x1 + offset_x;
        int dst_y   = y;
        if (glyph_w > 0 && glyph_h > 0 && glyph_w <= bitmap_w && glyph_h <= bitmap_h) {
            render_right = MAX(render_right, dst_x + glyph_w);
            if (dst_x >= 0 && dst_y >= 0 && dst_x + glyph_w <= bitmap_w && dst_y + glyph_h <= bitmap_h) {
                stbtt_MakeGlyphBitmap(font_info, bitmap + dst_y * bitmap_w + dst_x, glyph_w, glyph_h, bitmap_w,
                                      scale, scale, glyph);
            } else {
                int copy_x0 = MAX(0, -dst_x);
                int copy_y0 = MAX(0, -dst_y);
                int copy_x1 = MIN(glyph_w, bitmap_w - dst_x);
                int copy_y1 = MIN(glyph_h, bitmap_h - dst_y);
                if (copy_x1 > copy_x0 && copy_y1 > copy_y0) {
                    unsigned char *glyph_bitmap = (unsigned char *)malloc((size_t)glyph_w * glyph_h);
                    if (glyph_bitmap == NULL) { continue; }
                    memset(glyph_bitmap, 0, (size_t)glyph_w * glyph_h);
                    stbtt_MakeGlyphBitmap(font_info, glyph_bitmap, glyph_w, glyph_h, glyph_w, scale, scale, glyph);
                    for (int gy = copy_y0; gy < copy_y1; ++gy) {
                        memcpy(bitmap + (dst_y + gy) * bitmap_w + dst_x + copy_x0,
                               glyph_bitmap + gy * glyph_w + copy_x0, copy_x1 - copy_x0);
                    }
                    free(glyph_bitmap);
                }
            }
        }

        /* 调整x */
        x += roundf(advanceWidth * scale);

        /* 调整字距 */
        int kern;
        int next_glyph = stbtt_FindGlyphIndex(font_info, word[i + 1]);
        if (next_glyph < 0 || next_glyph >= font_info->numGlyphs) { next_glyph = 0; }
        kern  = stbtt_GetGlyphKernAdvance(font_info, glyph, next_glyph);
        x    += roundf(kern * scale);
        if (x >= bitmap_w) { break; }
    }
    *width = (unsigned)MIN(bitmap_w, MAX(render_right, MAX(0, x + offset_x)));
    *heigh = h;

    return bitmap;
}

void put_bitmap(SHEET_INFO *sht, SHEET *csheet, unsigned char *bitmap, unsigned x, unsigned y, unsigned width,
                unsigned heigh, unsigned bitmap_xsize, SHEET_BUFFER fc)
{
    // 查找图层
    SHEET *front_p = sht->start;
    while (true)
    {
        if (front_p == csheet) { break; }
        else if (front_p->next == NULL) { return; }
        front_p = front_p->next;
    }

    if (bitmap == NULL || x >= (unsigned)front_p->width || y >= (unsigned)front_p->height) { return; }

    unsigned clipped_width  = MIN(width, (unsigned)front_p->width - x);
    unsigned clipped_height = MIN(heigh, (unsigned)front_p->height - y);
    SHEET_BUFFER *SheetBuffer = (SHEET_BUFFER *)(front_p->buffer);
    for (unsigned i = 0; i < clipped_width; i++)
    {
        for (unsigned j = 0; j < clipped_height; j++)
        {
            SheetBuffer[(y + j) * front_p->width + (x + i)] =
                LCD_AlphaBlend(fc, SheetBuffer[(y + j) * front_p->width + (x + i)], bitmap[j * bitmap_xsize + i]);
        }
    }
}

void print_box_ttf(SHEET_INFO *sht, SHEET *csheet, char *buf, SHEET_BUFFER fc,
                   /*unsigned bc, */ unsigned x, unsigned y, int size)
{
    uint32_t temp_i_width;
    print_fmt_box_ttf(sht, csheet, fc, (int)x, (int)y, size, &temp_i_width, "%s", buf);
}

void print_box_ttfl(SHEET_INFO *sht, SHEET *csheet, char *buf, SHEET_BUFFER fc,
                    /*unsigned bc, */ unsigned x, unsigned y, int size, uint32_t *i_width)
{
    print_fmt_box_ttf(sht, csheet, fc, (int)x, (int)y, size, i_width, "%s", buf);
}

extern SHEET *ttf_measurement_sheet;

uint64_t calc_ttf_length(char *str, int size)
{
    // Render into the measurement sheet and return the accumulated glyph width.
    uint32_t temp_i_width = 0;
    print_fmt_box_ttf(
        sht_img,
        ttf_measurement_sheet,
        {0, 0, 0, 0}, 0, 0, size, &temp_i_width, "%s", str);
    return temp_i_width;
}

#define TTF_BUF_SIZE 128

// 辅助函数：计算UTF-8字符的字节数
static int utf8_char_len(unsigned char c)
{
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; // 无效UTF-8 / 非前缀，按单字节处理
}

// 刷新缓冲区内容到屏幕
static void flush_ttf_buffer(FbTTFWriterData *data)
{
    if (data->idx == 0) return;

    // 确保缓冲区以'\0'结尾
    data->buf[data->idx] = '\0';

    // 转换UTF-8字符串到Rune数组
    size_t  utf_len   = utflen(data->buf);
    Rune   *r         = NULL;
    uint8_t free_rune = 0;
    if (utf_len > data->rune_buf_size)
    {
        r         = (Rune *)malloc((utf_len + 1) * sizeof(Rune));
        if (r == NULL) { return; }
        free_rune = 1;
    }
    else
    {
        r = data->rune_buf;
    }
    r[utf_len] = 0;
    int   i    = 0;
    char *p    = data->buf;
    while (*p != '\0')
    {
        p += chartorune(&r[i++], p);
    }

    // 渲染文本
    unsigned int   width  = 0;
    unsigned int   height = 0;
    unsigned char *bitmap = TTF_Print(r, &width, &height, data->size);

    if (bitmap == NULL)
    {
        write_serial_fmt("TTF Print Failed. Reason: Memory Allocate Failed.\n");
        if (free_rune) { free(r); }
        return;
    }

    if (data->i_width != NULL) { *data->i_width = width; }

    // 查找图层
    SHEET *front_p = data->sht->start;
    while (front_p != NULL)
    {
        if (front_p == data->ct_sheet) break;
        front_p = front_p->next;
    }
    if (front_p == NULL)
    {
        if (free_rune) { free(r); }
        free_frames((uint64_t)virt_to_phys((uint64_t)bitmap), (2560 * 1440 + PAGE_SIZE - 1) / PAGE_SIZE);
        return;
    }

    put_bitmap(data->sht, data->ct_sheet, bitmap, data->x, data->y, width, height, 2560, data->color);
    refresh_part_sheet(data->sht, data->x, data->y, data->x + (int)width, data->y + (int)height);

    if (data->i_width != NULL) { *data->i_width = width; }

    // 更新光标位置
    data->x += (int)width;

    free_frames((uint64_t)virt_to_phys((uint64_t)bitmap), (2560 * 1440 + PAGE_SIZE - 1) / PAGE_SIZE);
    if (free_rune) { free(r); }
    data->idx = 0; // 重置缓冲区
}

uint8_t fb_ttf_writer_handler(Writer *writer, char ch)
{
    FbTTFWriterData *fbttf = (FbTTFWriterData *)writer->data;

    // 处理缓冲区溢出
    if (fbttf->idx + utf8_char_len(ch) > fbttf->buf_size - 1) { flush_ttf_buffer(fbttf); }

    // 将字符添加到缓冲区
    fbttf->buf[fbttf->idx++] = ch;

    // 如果是换行符，立即刷新
    if (ch == '\n')
    {
        flush_ttf_buffer(fbttf);
        fbttf->y += fbttf->size + 4;
        fbttf->x  = fbttf->start_x;
    }

    return 1;
}

void print_fmt_box_ttf(SHEET_INFO *sht, SHEET *csheet, SHEET_BUFFER fc, int x, int y, int size, const char *fmt, ...)
{
    uint32_t temp_i_width;
    char *buf   = (char *)malloc(TTF_BUF_SIZE * sizeof(char));
    Rune *rune_buf = (Rune *)malloc(TTF_BUF_SIZE * sizeof(Rune));
    if (buf == NULL || rune_buf == NULL)
    {
        free(buf);
        free(rune_buf);
        write_serial_string("print_fmt_box_ttf: buffer allocation failed\n");
        return;
    }
    buf[0] = '\0';

    // 初始化写入器数据
    FbTTFWriterData fb_ttf_writer_data = {
        .sht           = sht,
        .ct_sheet      = csheet,
        .color         = fc,
        .start_x       = x,
        .start_y       = y,
        .x             = x,
        .y             = y,
        .size          = size,
        .i_width       = &temp_i_width,
        .rune_buf      = rune_buf,
        .rune_buf_size = TTF_BUF_SIZE,
        .buf           = buf,
        .idx           = 0,
        .buf_size      = TTF_BUF_SIZE,
        .flush         = 0,
    };

    Writer fb_ttf_writer = {.data = &fb_ttf_writer_data, .handler = fb_ttf_writer_handler};

    va_list args;
    va_start(args, fmt);
    vwprintf(&fb_ttf_writer, fmt, args);
    va_end(args);

    // 刷新剩余内容
    flush_ttf_buffer(&fb_ttf_writer_data);

    free(buf);
    free(rune_buf);
}

void print_fmt_box_ttf(SHEET_INFO *sht, SHEET *csheet, SHEET_BUFFER fc, int x, int y, int size, uint32_t *i_width,
                       const char *fmt, ...)
{
    char *buf   = (char *)malloc(TTF_BUF_SIZE * sizeof(char));
    Rune *rune_buf = (Rune *)malloc(TTF_BUF_SIZE * sizeof(Rune));
    if (buf == NULL || rune_buf == NULL)
    {
        free(buf);
        free(rune_buf);
        write_serial_string("print_fmt_box_ttf: buffer allocation failed\n");
        return;
    }
    buf[0] = '\0';

    // 初始化写入器数据
    FbTTFWriterData fb_ttf_writer_data = {
        .sht           = sht,
        .ct_sheet      = csheet,
        .color         = fc,
        .start_x       = x,
        .start_y       = y,
        .x             = x,
        .y             = y,
        .size          = size,
        .i_width       = i_width,
        .rune_buf      = rune_buf,
        .rune_buf_size = TTF_BUF_SIZE,
        .buf           = buf,
        .idx           = 0,
        .buf_size      = TTF_BUF_SIZE,
        .flush         = 0,
    };

    Writer fb_ttf_writer = {.data = &fb_ttf_writer_data, .handler = fb_ttf_writer_handler};

    va_list args;
    va_start(args, fmt);
    vwprintf(&fb_ttf_writer, fmt, args);
    va_end(args);

    // 刷新剩余内容
    flush_ttf_buffer(&fb_ttf_writer_data);

    free(buf);
    free(rune_buf);
}

// void draw_text(SHEET_INFO *sht, WINDOWLS* wls, char *buf, SHEET_BUFFER fc,
// 				/*unsigned bc, */ unsigned x, unsigned y, int size)
// {
// 	print_box_ttf(sht, &(wls->w_sheet), buf, fc,
// 				  /*bc, */ x, y, size);
// }

void init_ttf()
{
    write_serial_string("Initializing Font...\n");
    if (!ttf_load_font_file("/system/font/XJ380F.ttf", &ttf_buffer, &font)) {
        return;
    }
    set_size(16);
    if (!ttf_load_font_file("/system/font/XJ380C.ttf", &ttf_buffer_c, &font_c)) {
        return;
    }
    set_size(16);

    write_serial_string("TTF Initialize Success.\n");
}
