#include <proto.hpp>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#include "./stbi.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "./stbir.h"

#include <fs/vfs/vfs.h>

static bool image_buffer_size(int width, int height, size_t *bytes)
{
    if (bytes == NULL || width <= 0 || height <= 0) return false;
    size_t pixels = (size_t)width * (size_t)height;
    if (pixels / (size_t)width != (size_t)height || pixels > (~(size_t)0) / 4) return false;
    *bytes = pixels * 4;
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
    if (blended_alpha <= 0.0f) return {0, 0, 0, 0};

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

/**
 * @brief 显示图片（并进行混色）
 * 
 * @param sht 图层信息结构
 * @param ct_sheet 图层编号
 * @param x 放置位置
 * @param y 放置位置
 * @param ow 目标宽度
 * @param oh 目标高度
 * @param path 路径
 */
spin_t PPlk = SPIN_INIT;

void PrintPicture_blend(SHEET_INFO *sht, SHEET *csheet, int x, int y, int ow, int oh, char *path)
{
    spin_lock(&PPlk);
    size_t output_bytes;
    if (!image_buffer_size(ow, oh, &output_bytes)) { spin_unlock(&PPlk); return; }
    int        w, h, bpp;
    vfs_node_t v = vfs_open(path);
    if (!v)
    {
        write_serial_fmt("PrintPic: cannot open '%s'\n", path);
        spin_unlock(&PPlk);
        return;
    }
    unsigned char *bf = (unsigned char *)malloc(v->size);
    if (!bf)
    {
        write_serial_fmt("PrintPic: failed to allocate %u bytes for '%s'\n", (unsigned)v->size, path);
        vfs_close(v);
        spin_unlock(&PPlk);
        return;
    }
    vfs_read(v, (uint8_t *)bf, 0, v->size);
    stbi_uc *b = stbi_load_from_memory(bf, v->size, &w, &h, &bpp, 4);
    /* source memory no longer needed after load */
    free(bf);
    if (!b)
    {
        write_serial_fmt("PrintPicture_blend: decode failed for '%s' (%u bytes)\n", path, (unsigned)v->size);
        write_serial_fmt("STBI Failure Reason: %s\n", stbi_failure_reason());
        vfs_close(v);
        spin_unlock(&PPlk);
        return;
    }
    uint8_t *b1 = (uint8_t *)malloc(output_bytes);
    if (!b1)
    {
        write_serial_fmt("PrintPic: output buffer allocate failed (%u bytes) for '%s' (ow=%d,oh=%d)\n",
                         (unsigned)(ow * oh * 4), path, ow, oh);
        stbi_image_free(b);
        vfs_close(v);
        spin_unlock(&PPlk);
        return;
    }
    stbir_resize_uint8(b, w, h, 0, b1, ow, oh, 0, 4);
    SHEET_BUFFER *buff = (SHEET_BUFFER *)b1;

    SHEET *front_p = csheet;
    SHEET_BUFFER *SheetBuffer = (SHEET_BUFFER *)(front_p->buffer);
    for (int i = 0; i < oh; i++)
    {
        for (int j = 0; j < ow; j++)
        {
            int dst_y = i + y;
            int dst_x = j + x;
            if (dst_x < 0 || dst_y < 0 || dst_x >= (int)front_p->width || dst_y >= (int)front_p->height) continue;
            if (buff[i * ow + j].a == 0) continue;
            SheetBuffer[dst_y * front_p->width + dst_x] =
                LCD_AlphaBlend(buff[i * ow + j], SheetBuffer[dst_y * front_p->width + dst_x], buff[i * ow + j].a);
        }
    }
    free(b1);
    stbi_image_free(b);
    vfs_close(v);
    spin_unlock(&PPlk);
}

/**
 * @brief 显示图片
 * 
 * @param sht 图层信息结构
 * @param ct_sheet 图层编号
 * @param x 放置位置
 * @param y 放置位置
 * @param ow 目标宽度
 * @param oh 目标高度
 * @param path 路径
 */
void PrintPicture(SHEET_INFO *sht, SHEET *csheet, int x, int y, int ow, int oh, char *path)
{
    size_t output_bytes;
    if (!image_buffer_size(ow, oh, &output_bytes)) return;
    int        w, h, bpp;
    vfs_node_t v = vfs_open(path);
    if (!v)
    {
        write_serial_fmt("PrintPicture: cannot open '%s'\n", path);
        return;
    }
    unsigned char *bf = (unsigned char *)malloc(v->size);
    if (!bf)
    {
        write_serial_fmt("PrintPicture: failed to allocate %u bytes for '%s'\n", (unsigned)v->size, path);
        vfs_close(v);
        return;
    }
    vfs_read(v, (uint8_t *)bf, 0, v->size);
    stbi_uc *b = stbi_load_from_memory(bf, v->size, &w, &h, &bpp, 4);
    free(bf);
    if (!b)
    {
        write_serial_fmt("PrintPic_PNG: decode failed for '%s' (%u bytes)\n", path, (unsigned)v->size);
        vfs_close(v);
        return;
    }
    uint8_t *b1 = (uint8_t *)malloc(output_bytes);
    if (!b1)
    {
        write_serial_fmt("PrintPic: output buffer allocate failed (%u bytes) for '%s' (ow=%d,oh=%d)\n",
                         (unsigned)(ow * oh * 4), path, ow, oh);
        stbi_image_free(b);
        vfs_close(v);
        return;
    }
    stbir_resize_uint8(b, w, h, 0, b1, ow, oh, 0, 4);
    SHEET_BUFFER *buff = (SHEET_BUFFER *)b1;

    SHEET *front_p = csheet;
    SHEET_BUFFER *SheetBuffer = (SHEET_BUFFER *)(front_p->buffer);
    for (int i = 0; i < oh; i++)
    {
        for (int j = 0; j < ow; j++)
        {
            int dst_y = i + y;
            int dst_x = j + x;
            if (dst_x < 0 || dst_y < 0 || dst_x >= (int)front_p->width || dst_y >= (int)front_p->height) continue;
            SheetBuffer[dst_y * front_p->width + dst_x] = buff[i * ow + j];
        }
    }
    free(b1);
    stbi_image_free(b);
    vfs_close(v);
}

bool LoadPicture(SHEET_BUFFER *buffer, int ow, int oh, char *path)
{
    size_t output_bytes;
    if (buffer == NULL || !image_buffer_size(ow, oh, &output_bytes)) return false;
    (void)output_bytes;
    int        w, h, bpp;
    vfs_node_t v = vfs_open(path);
    if (!v)
    {
        write_serial_fmt("LoadPicture: cannot open '%s'\n", path);
        return false;
    }
    unsigned char *bf = (unsigned char *)malloc(v->size);
    if (!bf)
    {
        write_serial_fmt("LoadPicture: failed to allocate %u bytes for '%s'\n", (unsigned)v->size, path);
        vfs_close(v);
        return false;
    }
    vfs_read(v, (uint8_t *)bf, 0, v->size);
    stbi_uc *b = stbi_load_from_memory(bf, v->size, &w, &h, &bpp, 4);
    free(bf);
    if (!b)
    {
        write_serial_fmt("LoadPicture: decode failed for '%s' (%u bytes)\n", path, (unsigned)v->size);
        vfs_close(v);
        return false;
    }
    int ok = stbir_resize_uint8(b, w, h, 0, (uint8_t *)(buffer), ow, oh, 0, 4);
    stbi_image_free(b);
    vfs_close(v);
    return ok != 0;
}

void LoadPictureOgM(SHEET_BUFFER **buffer, char *path)
{
    if (buffer == NULL) return;
    *buffer = NULL;
    int        w, h, bpp;
    vfs_node_t v = vfs_open(path);
    if (!v)
    {
        write_serial_fmt("LoadPictureOgM: cannot open '%s'\n", path);
        return;
    }
    unsigned char *bf = (unsigned char *)malloc(v->size);
    if (!bf)
    {
        write_serial_fmt("LoadPictureOgM: failed to allocate %u bytes for '%s'\n", (unsigned)v->size, path);
        vfs_close(v);
        return;
    }
    vfs_read(v, (uint8_t *)bf, 0, v->size);
    stbi_uc *b = stbi_load_from_memory(bf, v->size, &w, &h, &bpp, 4);
    if (!b)
    {
        write_serial_fmt("LoadPictureOgM: decode failed for '%s' (%u bytes)\n", path, (unsigned)v->size);
        free(bf);
        vfs_close(v);
        return;
    }
    size_t output_bytes;
    if (!image_buffer_size(w, h, &output_bytes))
    {
        stbi_image_free(b);
        free(bf);
        vfs_close(v);
        return;
    }
    *buffer = (SHEET_BUFFER *)malloc(output_bytes);
    if (!*buffer)
    {
        write_serial_fmt("LoadPictureOgM: output buffer alloc failed (%u bytes) for '%s'\n",
                         (unsigned)((size_t)w * h * 4), path);
        stbi_image_free(b);
        free(bf);
        vfs_close(v);
        return;
    }
    stbir_resize_uint8(b, w, h, 0, (uint8_t *)(*buffer), w, h, 0, 4);
    stbi_image_free(b);
    free(bf);
    vfs_close(v);
}

void GetPictureSize(int *w, int *h, char *path)
{
    spin_lock(&PPlk);
    int        bpp;
    vfs_node_t v = vfs_open(path);
    if (!v)
    {
        write_serial_fmt("GetPictureSize: cannot open '%s'\n", path);
        spin_unlock(&PPlk);
        return;
    }
    unsigned char *bf = (unsigned char *)malloc(v->size);
    if (!bf)
    {
        write_serial_fmt("GetPictureSize: failed to allocate %u bytes for '%s'\n", (unsigned)v->size, path);
        vfs_close(v);
        spin_unlock(&PPlk);
        return;
    }
    vfs_read(v, (uint8_t *)bf, 0, v->size);
    stbi_uc *b = stbi_load_from_memory(bf, v->size, w, h, &bpp, 0);
    /* source memory no longer needed after load */
    free(bf);
    stbi_image_free(b);
    vfs_close(v);
    spin_unlock(&PPlk);
}
