#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>
#include "boot_debug_color.h"
#include <console.h>

// Only the privileged console lock is replaced by the host runner. Pixel,
// scrolling, clearing and boot-marker code are the production implementations.
extern const uint8_t _binary___font_hankaku_bin_start[256 * 16] = {};

static constexpr size_t GUARD = 64;
static constexpr uint8_t SENTINEL = 0xa5;

static uint32_t pixel(const FrameBufferConfig &fbc, uint32_t x, uint32_t y)
{
    const uint8_t *p = fbc.frame_buffer + (static_cast<size_t>(y) * fbc.pixels_per_scan_line + x) * 4;
    assert(p[3] == 0);
    return fbc.pixel_format == kRGBR ? (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]
                                    : (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}

static void check_guards(const std::vector<uint8_t> &storage, const FrameBufferConfig &fbc)
{
    for (size_t i = 0; i < GUARD; ++i)
    {
        assert(storage[i] == SENTINEL);
        assert(storage[storage.size() - 1 - i] == SENTINEL);
    }
    for (uint32_t y = 0; y < fbc.vertical_resolution; ++y)
        for (uint32_t x = fbc.horizontal_resolution * 4; x < fbc.pixels_per_scan_line * 4; ++x)
            assert(fbc.frame_buffer[static_cast<size_t>(y) * fbc.pixels_per_scan_line * 4 + x] == SENTINEL);
}

static void normal_screen(PixelFormat format)
{
    const uint32_t width = 800, height = 600, stride = width + 17;
    std::vector<uint8_t> storage(GUARD * 2 + size_t(stride) * height * 4, SENTINEL);
    FrameBufferConfig fbc = {storage.data() + GUARD, stride, width, height, format};
    auto untouched = storage;
    boot_debug::initialize(fbc);
    boot_debug::begin(fbc, boot_debug::Step::Cpu);
#if DEVELOP_MODE_BOOT_DEBUG_COLOR == 1
    assert(boot_debug::reserved_height(fbc) == 112);
    assert(pixel(fbc, 8 + 2 * 20, 8) == 0x00cfe8);
    assert(pixel(fbc, 8 + 2 * 20 + 8, 8) == 0x252b3b);
    boot_debug::returned(fbc, boot_debug::Step::Cpu);
    assert(pixel(fbc, 8 + 2 * 20 + 15, 8) == 0x00cfe8);
    boot_debug::skip(fbc, boot_debug::Step::InstallerPackages, boot_debug::Step::InstallerRootCheck);
    assert(pixel(fbc, 8 + 20, 8 + 2 * 20) == 0x6c7080);
    assert(pixel(fbc, 8 + 6 * 20, 8 + 2 * 20) == 0x252b3b);
    auto marked = storage;
    boot_debug::begin(fbc, boot_debug::Step::Count);
    boot_debug::returned(fbc, static_cast<boot_debug::Step>(~0U));
    boot_debug::skip(fbc, boot_debug::Step::Count, boot_debug::Step::Count);
    boot_debug::skip(fbc, boot_debug::Step::Cpu, boot_debug::Step::BootConfig);
    assert(storage == marked);
    uint32_t top = boot_debug::reserved_height(fbc);
    std::vector<uint8_t> band(fbc.frame_buffer, fbc.frame_buffer + size_t(stride) * top * 4);
    console_init(fbc);
    assert(pixel(fbc, 0, top) == 0x0d1223);
    for (int line = 0; line < 200; ++line) console_write("scroll\n");
    console_write("\033[2J\033[Hclear\n");
    assert(std::equal(band.begin(), band.end(), fbc.frame_buffer));
#else
    boot_debug::returned(fbc, boot_debug::Step::Cpu);
    boot_debug::skip(fbc, boot_debug::Step::BootConfig, boot_debug::Step::Scheduler);
    assert(boot_debug::reserved_height(fbc) == 0);
    assert(storage == untouched);
    console_init(fbc);
    assert(pixel(fbc, 0, 0) == 0x0d1223);
#endif
    check_guards(storage, fbc);
}

static void small_screen(uint32_t width, uint32_t height)
{
    uint32_t stride = width + 3;
    std::vector<uint8_t> storage(GUARD * 2 + size_t(stride) * height * 4, SENTINEL);
    FrameBufferConfig fbc = {storage.data() + GUARD, stride, width, height, kBGRR};
    auto untouched = storage;
    uint32_t top = boot_debug::reserved_height(fbc);
    assert(top == 0 || (top % 16 == 0 && top + 16 <= height));
    boot_debug::initialize(fbc);
    boot_debug::begin(fbc, boot_debug::Step::Scheduler);
    boot_debug::returned(fbc, boot_debug::Step::Scheduler);
    boot_debug::skip(fbc, boot_debug::Step::Xhci);
    if (top == 0) assert(storage == untouched);
    for (uint32_t y = top; y < height; ++y)
        for (uint32_t x = 0; x < stride * 4; ++x)
            assert(fbc.frame_buffer[size_t(y) * stride * 4 + x] == SENTINEL);
    check_guards(storage, fbc);
}

int main()
{
    normal_screen(kRGBR);
    normal_screen(kBGRR);
    for (auto size : {std::pair{332U, 128U}, std::pair{200U, 128U}, std::pair{80U, 64U},
                      std::pair{32U, 32U}, std::pair{31U, 32U}, std::pair{800U, 31U}})
        small_screen(size.first, size.second);

    FrameBufferConfig invalid = {nullptr, 800, 800, 600, kRGBR};
    assert(boot_debug::reserved_height(invalid) == 0);
    boot_debug::initialize(invalid);
    boot_debug::begin(invalid, boot_debug::Step::Cpu);
    boot_debug::returned(invalid, boot_debug::Step::Cpu);
    boot_debug::skip(invalid, boot_debug::Step::Cpu);
    invalid.frame_buffer = reinterpret_cast<uint8_t *>(~uintptr_t(0) - 1);
    assert(boot_debug::reserved_height(invalid) == 0);
    boot_debug::initialize(invalid);
    invalid.pixels_per_scan_line = 799;
    assert(boot_debug::reserved_height(invalid) == 0);
    boot_debug::initialize(invalid);
}
