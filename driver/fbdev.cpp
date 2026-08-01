#include <device.h>
#include <efi/fbc.h>
#include <errno.h>
#include <fbdev.h>
#include <ioctl.h>
#include <krlibc.h>
#include <mm/hhdm.h>
#include <mm/page.h>
#include <proto.hpp>

namespace
{
constexpr uint32_t kFbPages = 2;

struct FbDevState
{
    const FrameBufferConfig *fbc;
    uint8_t                 *buffer;
    uint64_t                 phys;
    size_t                   bytes_per_page;
    size_t                   total_bytes;
    uint32_t                 active_page;
    bool                     ready;
};

FbDevState g_fbdev{};

static size_t fbdev_pitch()
{
    return (size_t)g_fbdev.fbc->pixels_per_scan_line * 4;
}

static void fbdev_present_buffer_region(uint8_t *src, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    if (!g_fbdev.ready || src == NULL) return;

    if (x1 > g_fbdev.fbc->horizontal_resolution) x1 = g_fbdev.fbc->horizontal_resolution;
    if (x2 > g_fbdev.fbc->horizontal_resolution) x2 = g_fbdev.fbc->horizontal_resolution;
    if (y1 > g_fbdev.fbc->vertical_resolution) y1 = g_fbdev.fbc->vertical_resolution;
    if (y2 > g_fbdev.fbc->vertical_resolution) y2 = g_fbdev.fbc->vertical_resolution;
    if (x1 >= x2 || y1 >= y2) return;

    uint8_t *dst = g_fbdev.fbc->frame_buffer;
    size_t pitch = fbdev_pitch();
    size_t row_bytes = (size_t)(x2 - x1) * 4;
    for (uint32_t y = y1; y < y2; ++y)
    {
        size_t off = (size_t)y * pitch + (size_t)x1 * 4;
        memcpy(dst + off, src + off, row_bytes);
    }
}

static uint8_t *fbdev_page_framebuffer(uint32_t page)
{
    if (!g_fbdev.ready || page >= kFbPages) return NULL;
    return g_fbdev.buffer + (size_t)page * g_fbdev.bytes_per_page;
}

static void fbdev_copy_page_to_gop(uint32_t page)
{
    uint32_t height = g_fbdev.fbc->vertical_resolution;
    fbdev_present_buffer_region(fbdev_page_framebuffer(page), 0, 0, g_fbdev.fbc->horizontal_resolution, height);
}

static void fbdev_present_page_byte_row(uint32_t page, uint32_t row, size_t byte_x1, size_t byte_x2)
{
    if (!g_fbdev.ready || page >= kFbPages) return;
    if (row >= g_fbdev.fbc->vertical_resolution) return;

    size_t visible_row_bytes = (size_t)g_fbdev.fbc->horizontal_resolution * 4;
    if (byte_x1 >= visible_row_bytes) return;
    if (byte_x2 > visible_row_bytes) byte_x2 = visible_row_bytes;
    if (byte_x1 >= byte_x2) return;

    uint32_t x1 = (uint32_t)(byte_x1 / 4);
    uint32_t x2 = (uint32_t)((byte_x2 + 3) / 4);
    fbdev_present_buffer_region(fbdev_page_framebuffer(page), x1, row, x2, row + 1);
}

static void fbdev_present_page_byte_range(uint32_t page, size_t offset_in_page, size_t bytes)
{
    if (!g_fbdev.ready || page >= kFbPages || bytes == 0) return;
    if (offset_in_page >= g_fbdev.bytes_per_page) return;

    size_t end = offset_in_page + MIN(bytes, g_fbdev.bytes_per_page - offset_in_page);
    size_t pitch = fbdev_pitch();
    if (pitch == 0 || end <= offset_in_page) return;

    uint32_t row_start = (uint32_t)(offset_in_page / pitch);
    uint32_t row_end = (uint32_t)((end - 1) / pitch);
    if (row_start >= g_fbdev.fbc->vertical_resolution) return;
    if (row_end >= g_fbdev.fbc->vertical_resolution) row_end = g_fbdev.fbc->vertical_resolution - 1;

    size_t start_byte_x = offset_in_page % pitch;
    size_t end_byte_x = ((end - 1) % pitch) + 1;

    if (row_start == row_end)
    {
        fbdev_present_page_byte_row(page, row_start, start_byte_x, end_byte_x);
        return;
    }

    fbdev_present_page_byte_row(page, row_start, start_byte_x, pitch);

    if (row_start + 1 < row_end)
    {
        fbdev_present_buffer_region(fbdev_page_framebuffer(page), 0, row_start + 1,
                                    g_fbdev.fbc->horizontal_resolution, row_end);
    }

    fbdev_present_page_byte_row(page, row_end, 0, end_byte_x);
}

static void fbdev_fill_fix(struct fb_fix_screeninfo *fix)
{
    memset(fix, 0, sizeof(*fix));
    strcpy(fix->id, "XJ380 fb0");
    fix->smem_start = g_fbdev.phys;
    fix->smem_len = (uint32_t)g_fbdev.total_bytes;
    fix->type = FB_TYPE_PACKED_PIXELS;
    fix->visual = FB_VISUAL_TRUECOLOR;
    fix->ypanstep = 1;
    fix->line_length = (uint32_t)fbdev_pitch();
    fix->accel = FB_ACCEL_NONE;
}

static void fbdev_fill_var(struct fb_var_screeninfo *var)
{
    memset(var, 0, sizeof(*var));
    var->xres = g_fbdev.fbc->horizontal_resolution;
    var->yres = g_fbdev.fbc->vertical_resolution;
    var->xres_virtual = g_fbdev.fbc->pixels_per_scan_line;
    var->yres_virtual = g_fbdev.fbc->vertical_resolution * kFbPages;
    var->xoffset = 0;
    var->yoffset = g_fbdev.active_page * g_fbdev.fbc->vertical_resolution;
    var->bits_per_pixel = 32;
    var->height = 0xffffffff;
    var->width = 0xffffffff;

    if (g_fbdev.fbc->pixel_format == PixelFormat::kRGBR)
    {
        var->red = {0, 8, 0};
        var->green = {8, 8, 0};
        var->blue = {16, 8, 0};
    }
    else
    {
        var->blue = {0, 8, 0};
        var->green = {8, 8, 0};
        var->red = {16, 8, 0};
    }
    var->transp = {24, 8, 0};
}

static size_t fbdev_read(int, uint8_t *buffer, size_t number, size_t offset)
{
    if (!g_fbdev.ready || buffer == NULL) return VFS_STATUS_FAILED;
    if (offset >= g_fbdev.total_bytes) return 0;
    size_t bytes = MIN(number, g_fbdev.total_bytes - offset);
    memcpy(buffer, g_fbdev.buffer + offset, bytes);
    return bytes;
}

static size_t fbdev_write(int, uint8_t *buffer, size_t number, size_t offset)
{
    if (!g_fbdev.ready || buffer == NULL) return VFS_STATUS_FAILED;
    if (offset >= g_fbdev.total_bytes) return 0;
    size_t bytes = MIN(number, g_fbdev.total_bytes - offset);
    memcpy(g_fbdev.buffer + offset, buffer, bytes);

    size_t active_start = (size_t)g_fbdev.active_page * g_fbdev.bytes_per_page;
    size_t active_end = active_start + g_fbdev.bytes_per_page;
    if (offset < active_end && offset + bytes > active_start)
    {
        size_t dirty_start = offset > active_start ? offset : active_start;
        size_t dirty_end = offset + bytes < active_end ? offset + bytes : active_end;
        fbdev_present_page_byte_range(g_fbdev.active_page, dirty_start - active_start, dirty_end - dirty_start);
    }
    return bytes;
}

static int fbdev_ioctl(device_t *, size_t req, void *arg)
{
    if (!g_fbdev.ready) return -ENODEV;
    if (arg == NULL) return -EFAULT;

    switch (req)
    {
    case FBIOGET_FSCREENINFO:
        fbdev_fill_fix((struct fb_fix_screeninfo *)arg);
        return EOK;
    case FBIOGET_VSCREENINFO:
        fbdev_fill_var((struct fb_var_screeninfo *)arg);
        return EOK;
    case FBIOPUT_VSCREENINFO:
    {
        struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
        if (var->bits_per_pixel != 0 && var->bits_per_pixel != 32) return -EINVAL;
        if (var->yoffset >= g_fbdev.fbc->vertical_resolution * kFbPages) return -EINVAL;
        uint32_t page = var->yoffset / g_fbdev.fbc->vertical_resolution;
        if (page >= kFbPages) return -EINVAL;
        g_fbdev.active_page = page;
        fbdev_copy_page_to_gop(page);
        fbdev_fill_var(var);
        return EOK;
    }
    case FBIOPAN_DISPLAY:
    {
        struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
        if (var->xoffset != 0) return -EINVAL;
        if (var->yoffset >= g_fbdev.fbc->vertical_resolution * kFbPages) return -EINVAL;
        uint32_t page = var->yoffset / g_fbdev.fbc->vertical_resolution;
        if (page >= kFbPages) return -EINVAL;
        g_fbdev.active_page = page;
        fbdev_copy_page_to_gop(page);
        fbdev_fill_var(var);
        return EOK;
    }
    default:
        return -ENOSYS;
    }
}

static void *fbdev_map(int, void *addr, size_t len)
{
    if (!g_fbdev.ready || addr == NULL) return NULL;
    if (len > g_fbdev.total_bytes) return NULL;
    size_t map_len = PADDING_UP(MIN(len, g_fbdev.total_bytes), PAGE_SIZE);
    if (map_len == 0) return NULL;
    page_map_range(get_current_directory(), (uint64_t)addr, g_fbdev.phys, map_len,
                   PTE_USER | PTE_PRESENT | PTE_WRITEABLE | PTE_NO_EXECUTE);
    return addr;
}
}

void fbdev_setup(const FrameBufferConfig &fbc)
{
    if (g_fbdev.ready) return;

    g_fbdev.fbc = &fbc;
    g_fbdev.bytes_per_page = (size_t)fbc.pixels_per_scan_line * fbc.vertical_resolution * 4;
    g_fbdev.total_bytes = g_fbdev.bytes_per_page * kFbPages;
    size_t pages = PADDING_UP(g_fbdev.total_bytes, PAGE_SIZE) / PAGE_SIZE;
    g_fbdev.phys = alloc_frames(pages);
    if (g_fbdev.phys == 0)
    {
        write_serial_string("fbdev: cannot allocate backing buffer\n");
        return;
    }

    g_fbdev.buffer = (uint8_t *)driver_phys_to_virt(g_fbdev.phys);
    page_map_range(get_current_directory(), (uint64_t)g_fbdev.buffer, g_fbdev.phys,
                   pages * PAGE_SIZE, KERNEL_PTE_FLAGS);
    memset(g_fbdev.buffer, 0, pages * PAGE_SIZE);
    memcpy(g_fbdev.buffer, fbc.frame_buffer, g_fbdev.bytes_per_page);
    g_fbdev.active_page = 0;
    g_fbdev.ready = true;

    device_t fb{};
    fb.read = fbdev_read;
    fb.write = fbdev_write;
    fb.ioctl = fbdev_ioctl;
    fb.map = fbdev_map;
    fb.flag = 1;
    fb.size = g_fbdev.total_bytes;
    fb.sector_size = 1;
    fb.type = DEVICE_FB;
    fb.max_size = g_fbdev.total_bytes;
    strcpy(fb.drive_name, "fb0");

    regist_device(NULL, fb);
}

bool fbdev_ready()
{
    return g_fbdev.ready;
}

uint8_t *fbdev_active_framebuffer()
{
    return fbdev_page_framebuffer(g_fbdev.active_page);
}

uint8_t *fbdev_kernel_framebuffer()
{
    return fbdev_page_framebuffer(0);
}

void fbdev_present_region(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    fbdev_present_buffer_region(fbdev_active_framebuffer(), x1, y1, x2, y2);
}

void fbdev_present_kernel_region(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    fbdev_present_buffer_region(fbdev_kernel_framebuffer(), x1, y1, x2, y2);
}
