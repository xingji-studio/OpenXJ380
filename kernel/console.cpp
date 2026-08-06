#include <console.h>
#include <cpu/lock.h>
#include <stdint.h>

extern const uint8_t _binary___font_hankaku_bin_start;

static constexpr uint32_t CHARACTER_WIDTH  = 8;
static constexpr uint32_t CHARACTER_HEIGHT = 16;
static constexpr uint32_t FOREGROUND       = 0x00ffffff;
static constexpr uint32_t BACKGROUND       = 0x000d1223;

static const FrameBufferConfig *g_framebuffer = nullptr;
static uint32_t                 g_column      = 0;
static uint32_t                 g_row         = 0;
static uint32_t                 g_columns     = 0;
static uint32_t                 g_rows        = 0;
static uint8_t                  g_escape_state = 0;
static spin_t                   g_console_lock = SPIN_INIT;

static void put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    uint8_t *pixel = g_framebuffer->frame_buffer +
                     ((uint64_t)y * g_framebuffer->pixels_per_scan_line + x) * 4;
    uint8_t red   = (uint8_t)((color >> 16) & 0xff);
    uint8_t green = (uint8_t)((color >> 8) & 0xff);
    uint8_t blue  = (uint8_t)(color & 0xff);
    if (g_framebuffer->pixel_format == PixelFormat::kRGBR)
    {
        pixel[0] = red;
        pixel[1] = green;
        pixel[2] = blue;
    }
    else
    {
        pixel[0] = blue;
        pixel[1] = green;
        pixel[2] = red;
    }
    pixel[3] = 0;
}

static void clear_screen()
{
    for (uint32_t y = 0; y < g_framebuffer->vertical_resolution; y++)
        for (uint32_t x = 0; x < g_framebuffer->horizontal_resolution; x++)
            put_pixel(x, y, BACKGROUND);
    g_column = 0;
    g_row    = 0;
}

static void scroll_screen()
{
    uint32_t width  = g_framebuffer->horizontal_resolution;
    uint32_t height = g_framebuffer->vertical_resolution;
    for (uint32_t y = CHARACTER_HEIGHT; y < height; y++)
    {
        uint32_t *source = (uint32_t *)(g_framebuffer->frame_buffer +
                           (uint64_t)y * g_framebuffer->pixels_per_scan_line * 4);
        uint32_t *target = (uint32_t *)(g_framebuffer->frame_buffer +
                           (uint64_t)(y - CHARACTER_HEIGHT) * g_framebuffer->pixels_per_scan_line * 4);
        for (uint32_t x = 0; x < width; x++) target[x] = source[x];
    }
    for (uint32_t y = height - CHARACTER_HEIGHT; y < height; y++)
        for (uint32_t x = 0; x < width; x++) put_pixel(x, y, BACKGROUND);
    g_row = g_rows - 2;
}

static void new_line()
{
    g_column = 0;
    if (g_rows <= 1)
    {
        g_row = 0;
        return;
    }
    g_row++;
    if (g_row >= g_rows - 1) scroll_screen();
}

static void draw_character(uint8_t character)
{
    const uint8_t *glyph = &_binary___font_hankaku_bin_start + (uint32_t)character * CHARACTER_HEIGHT;
    uint32_t origin_x = g_column * CHARACTER_WIDTH;
    uint32_t origin_y = g_row * CHARACTER_HEIGHT;
    for (uint32_t y = 0; y < CHARACTER_HEIGHT; y++)
        for (uint32_t x = 0; x < CHARACTER_WIDTH; x++)
            put_pixel(origin_x + x, origin_y + y, (glyph[y] & (0x80U >> x)) ? FOREGROUND : BACKGROUND);

    g_column++;
    if (g_column >= g_columns) new_line();
}

static void put_character(char value)
{
    if (g_escape_state != 0)
    {
        if (g_escape_state == 1 && value == '[') g_escape_state = 2;
        else if (g_escape_state == 2 && (value == '2' || value == ';')) g_escape_state = 3;
        else if (value == 'J' || value == 'H') { clear_screen(); g_escape_state = 0; }
        else g_escape_state = 0;
        return;
    }
    if (value == '\033') { g_escape_state = 1; return; }
    if (value == '\r') { g_column = 0; return; }
    if (value == '\n') { new_line(); return; }
    if (value == '\b')
    {
        if (g_column > 0) g_column--;
        draw_character(' ');
        if (g_column > 0) g_column--;
        return;
    }
    if (value == '\t')
    {
        do { draw_character(' '); } while ((g_column & 3U) != 0);
        return;
    }
    if ((uint8_t)value >= 32 && (uint8_t)value < 128) draw_character((uint8_t)value);
}

void console_init(const FrameBufferConfig &fbc)
{
    if (fbc.frame_buffer == nullptr || fbc.horizontal_resolution < CHARACTER_WIDTH ||
        fbc.vertical_resolution < CHARACTER_HEIGHT)
        return;

    g_framebuffer = &fbc;
    g_columns     = fbc.horizontal_resolution / CHARACTER_WIDTH;
    g_rows        = fbc.vertical_resolution / CHARACTER_HEIGHT;
    spin_init(&g_console_lock);
    clear_screen();
}

void console_write(const char *str)
{
    if (g_framebuffer == nullptr || str == nullptr) return;
    spin_lock(&g_console_lock);
    while (*str != '\0') put_character(*str++);
    spin_unlock(&g_console_lock);
}
