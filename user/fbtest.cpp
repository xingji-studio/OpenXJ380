#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606

struct fb_bitfield
{
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_fix_screeninfo
{
    char     id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

struct fb_var_screeninfo
{
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

static void out(const char *s)
{
    const char *p = s;
    while (*p) ++p;
    write(1, s, p - s);
}

extern "C" int fbtest_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int fbtest_main_cpp(int, char **, char **)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0)
    {
        out("fbtest：open 失败\n");
        return 1;
    }

    fb_fix_screeninfo fix{};
    fb_var_screeninfo var{};
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0)
    {
        out("fbtest：ioctl 失败\n");
        close(fd);
        return 1;
    }

    uint8_t *fb = (uint8_t *)mmap(nullptr, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED)
    {
        out("fbtest：mmap 失败\n");
        close(fd);
        return 1;
    }

    uint32_t page = var.yres_virtual >= var.yres * 2 ? 1 : 0;
    uint8_t *dst = fb + (size_t)page * var.yres * fix.line_length;
    for (uint32_t y = 0; y < var.yres; ++y)
    {
        uint32_t *row = (uint32_t *)(dst + (size_t)y * fix.line_length);
        for (uint32_t x = 0; x < var.xres; ++x)
        {
            uint8_t r = (uint8_t)((x * 255) / (var.xres ? var.xres : 1));
            uint8_t g = (uint8_t)((y * 255) / (var.yres ? var.yres : 1));
            uint8_t b = 0x80;
            row[x] = ((uint32_t)r << var.red.offset) |
                     ((uint32_t)g << var.green.offset) |
                     ((uint32_t)b << var.blue.offset) |
                     (0xffu << var.transp.offset);
        }
    }

    var.xoffset = 0;
    var.yoffset = page * var.yres;
    if (ioctl(fd, FBIOPAN_DISPLAY, &var) < 0)
    {
        out("fbtest：pan 失败\n");
        return 1;
    }

    out("fbtest：通过\n");
    return 0;
}
