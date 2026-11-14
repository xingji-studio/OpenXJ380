#include "boot_debug_color.h"

#if DEVELOP_MODE_BOOT_DEBUG_COLOR == 1
namespace boot_debug {
namespace {

constexpr uint32_t COLUMNS    = 16;
constexpr uint32_t STEP_COUNT = static_cast<uint32_t>(Step::Count);
constexpr uint32_t ROWS       = (STEP_COUNT + COLUMNS - 1) / COLUMNS;
constexpr uint32_t BACKGROUND = 0x000d1223;
constexpr uint32_t PENDING    = 0x00252b3b;
constexpr uint32_t SKIPPED    = 0x006c7080;

struct Layout
{
    uint32_t cell;
    uint32_t gap;
    uint32_t margin;
    uint32_t height;
};

static Layout layout_for(const FrameBufferConfig &fbc)
{
    if (fbc.frame_buffer == nullptr || fbc.horizontal_resolution == 0 || fbc.vertical_resolution < 32 ||
        fbc.pixels_per_scan_line < fbc.horizontal_resolution ||
        (fbc.pixel_format != kRGBR && fbc.pixel_format != kBGRR))
        return {};

    // FrameBufferConfig has no allocation length. Check the declared visible
    // rectangle and address arithmetic without assuming tightly packed rows.
    uintptr_t address = reinterpret_cast<uintptr_t>(fbc.frame_buffer);
    uintptr_t maximum = ~static_cast<uintptr_t>(0);
    uint64_t last_pixel = static_cast<uint64_t>(fbc.vertical_resolution - 1) * fbc.pixels_per_scan_line +
                          fbc.horizontal_resolution - 1;
    if (address > maximum - 3 || last_pixel > (maximum - address - 3) / 4) return {};

    for (uint32_t cell = 16; cell >= 2; cell /= 2)
    {
        uint32_t gap    = cell / 4;
        uint32_t margin = gap * 2;
        uint32_t width  = margin * 2 + COLUMNS * cell + (COLUMNS - 1) * gap;
        uint32_t height = margin * 2 + ROWS * cell + (ROWS - 1) * gap;
        // Match the console's 16-pixel text rows and leave at least one row.
        height = (height + 15) & ~15U;
        if (width <= fbc.horizontal_resolution && height <= fbc.vertical_resolution - 16)
            return {cell, gap, margin, height};
    }
    return {};
}

static uint32_t step_color(Step step)
{
    if (step <= Step::Console) return 0x00e8edf2;
    if (step <= Step::Apic) return 0x0000cfe8;
    if (step <= Step::Heap) return 0x004d88ff;
    if (step <= Step::Rng) return 0x0031d67b;
    if (step <= Step::Mouse) return 0x00ff9b32;
    if (step <= Step::XhciWorkers) return 0x00b878ff;
    if (step <= Step::Stdio) return 0x00f4d35e;
    if (step <= Step::LoadModules) return 0x00ff69b4;
    if (step <= Step::SerialRuntime) return 0x002de2c3;
    if (step == Step::BootLogo) return 0x006aabff;
    if (step == Step::InputMethod) return 0x007ef5a6;
    if (step == Step::AudioRuntime) return 0x0054eaf8;
    return 0x00fafafa;
}

static void fill(const FrameBufferConfig &fbc, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                 uint32_t color)
{
    if (x >= fbc.horizontal_resolution || y >= fbc.vertical_resolution) return;
    if (width > fbc.horizontal_resolution - x) width = fbc.horizontal_resolution - x;
    if (height > fbc.vertical_resolution - y) height = fbc.vertical_resolution - y;
    uint8_t red   = static_cast<uint8_t>(color >> 16);
    uint8_t green = static_cast<uint8_t>(color >> 8);
    uint8_t blue  = static_cast<uint8_t>(color);
    for (uint32_t row = 0; row < height; ++row)
    {
        volatile uint8_t *pixel = fbc.frame_buffer +
                                  (static_cast<uint64_t>(y + row) * fbc.pixels_per_scan_line + x) * 4;
        for (uint32_t column = 0; column < width; ++column)
        {
            pixel[0] = fbc.pixel_format == kRGBR ? red : blue;
            pixel[1] = green;
            pixel[2] = fbc.pixel_format == kRGBR ? blue : red;
            pixel[3] = 0;
            pixel += 4;
        }
    }
}

static void paint(const FrameBufferConfig &fbc, const Layout &layout, Step step, uint32_t width, uint32_t color)
{
    uint32_t slot = static_cast<uint32_t>(step);
    uint32_t x    = layout.margin + (slot % COLUMNS) * (layout.cell + layout.gap);
    uint32_t y    = layout.margin + (slot / COLUMNS) * (layout.cell + layout.gap);
    fill(fbc, x, y, width, layout.cell, color);
}

} // namespace

uint32_t reserved_height(const FrameBufferConfig &fbc)
{
    return layout_for(fbc).height;
}

void initialize(const FrameBufferConfig &fbc)
{
    Layout layout = layout_for(fbc);
    if (layout.cell == 0) return;
    // BSP-only, bounded, allocation-free stores: usable before CPU/heap setup.
    // No console or graphics locks are acquired, even with the boot gate shut.
    fill(fbc, 0, 0, fbc.horizontal_resolution, layout.height, BACKGROUND);
    for (uint32_t slot = 0; slot < STEP_COUNT; ++slot)
        paint(fbc, layout, static_cast<Step>(slot), layout.cell, PENDING);
    __asm__ volatile("sfence" ::: "memory");
}

void begin(const FrameBufferConfig &fbc, Step step)
{
    Layout layout = layout_for(fbc);
    if (layout.cell == 0 || step >= Step::Count) return;
    paint(fbc, layout, step, layout.cell, PENDING);
    paint(fbc, layout, step, layout.cell / 2, step_color(step));
    // Drain write-combining stores before entering a potentially stuck probe.
    __asm__ volatile("sfence" ::: "memory");
}

void returned(const FrameBufferConfig &fbc, Step step)
{
    Layout layout = layout_for(fbc);
    if (layout.cell == 0 || step >= Step::Count) return;
    paint(fbc, layout, step, layout.cell, step_color(step));
    __asm__ volatile("sfence" ::: "memory");
}

void skip(const FrameBufferConfig &fbc, Step first, Step last)
{
    Layout layout = layout_for(fbc);
    if (layout.cell == 0 || first > last || last >= Step::Count) return;
    for (uint32_t slot = static_cast<uint32_t>(first); slot <= static_cast<uint32_t>(last); ++slot)
        paint(fbc, layout, static_cast<Step>(slot), layout.cell, SKIPPED);
    __asm__ volatile("sfence" ::: "memory");
}

} // namespace boot_debug
#endif
