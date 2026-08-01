#include "browser_image_decode.h"

#include <cctype>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "third_party/libwebp/src/webp/decode.h"

#define NANOSVG_CPLUSPLUS

static float nanosvg_fabsf(float value)
{
    return value < 0.0f ? -value : value;
}

extern "C" float floorf(float value) noexcept
{
    const long truncated = static_cast<long>(value);
    if (value < 0.0f && static_cast<float>(truncated) != value) {
        return static_cast<float>(truncated - 1);
    }
    return static_cast<float>(truncated);
}

extern "C" float ceilf(float value) noexcept
{
    const long truncated = static_cast<long>(value);
    if (value > 0.0f && static_cast<float>(truncated) != value) {
        return static_cast<float>(truncated + 1);
    }
    return static_cast<float>(truncated);
}

static float nanosvg_sqrtf(float value)
{
    if (value <= 0.0f) {
        return 0.0f;
    }

    float estimate = value > 1.0f ? value : 1.0f;
    for (int i = 0; i < 8; ++i) {
        estimate = 0.5f * (estimate + value / estimate);
    }
    return estimate;
}

static float nanosvg_wrap_radians(float value)
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 6.28318530717958647692f;
    while (value > kPi) {
        value -= kTwoPi;
    }
    while (value < -kPi) {
        value += kTwoPi;
    }
    return value;
}

extern "C" float sinf(float value) noexcept
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kHalfPi = 1.57079632679489661923f;

    value = nanosvg_wrap_radians(value);
    if (value > kHalfPi) {
        value = kPi - value;
    } else if (value < -kHalfPi) {
        value = -kPi - value;
    }

    const float x2 = value * value;
    return value * (1.0f - x2 * (1.0f / 6.0f) + x2 * x2 * (1.0f / 120.0f) -
                    x2 * x2 * x2 * (1.0f / 5040.0f));
}

extern "C" float cosf(float value) noexcept
{
    constexpr float kHalfPi = 1.57079632679489661923f;
    return sinf(kHalfPi - value);
}

extern "C" float tanf(float value) noexcept
{
    const float cosine = cosf(value);
    if (nanosvg_fabsf(cosine) < 1e-5f) {
        return value >= 0.0f ? 1e6f : -1e6f;
    }
    return sinf(value) / cosine;
}

static float nanosvg_atanf(float value)
{
    constexpr float kQuarterPi = 0.78539816339744830962f;
    if (value > 1.0f) {
        return 1.57079632679489661923f - nanosvg_atanf(1.0f / value);
    }
    if (value < -1.0f) {
        return -1.57079632679489661923f - nanosvg_atanf(1.0f / value);
    }

    const float abs_value = nanosvg_fabsf(value);
    return value * (kQuarterPi + 0.273f * (1.0f - abs_value));
}

extern "C" float atan2f(float y, float x) noexcept
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kHalfPi = 1.57079632679489661923f;

    if (x > 0.0f) {
        return nanosvg_atanf(y / x);
    }
    if (x < 0.0f) {
        return y >= 0.0f ? nanosvg_atanf(y / x) + kPi : nanosvg_atanf(y / x) - kPi;
    }
    if (y > 0.0f) {
        return kHalfPi;
    }
    if (y < 0.0f) {
        return -kHalfPi;
    }
    return 0.0f;
}

extern "C" float acosf(float value) noexcept
{
    if (value <= -1.0f) {
        return 3.14159265358979323846f;
    }
    if (value >= 1.0f) {
        return 0.0f;
    }
    const float y = nanosvg_sqrtf(1.0f - value * value);
    return atan2f(y, value);
}

static void nanosvg_swap_bytes(unsigned char *lhs, unsigned char *rhs, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char tmp = lhs[i];
        lhs[i] = rhs[i];
        rhs[i] = tmp;
    }
}

extern "C" __attribute__((weak)) void qsort(void *base,
                                            std::size_t count,
                                            std::size_t size,
                                            int (*compare)(const void *, const void *)) noexcept
{
    if (size == 0 || count < 2) {
        return;
    }

    unsigned char *bytes = static_cast<unsigned char *>(base);
    for (std::size_t i = 1; i < count; ++i) {
        std::size_t j = i;
        while (j > 0) {
            unsigned char *current = bytes + j * size;
            unsigned char *previous = current - size;
            if (compare(previous, current) <= 0) {
                break;
            }
            nanosvg_swap_bytes(previous, current, size);
            --j;
        }
    }
}

static long long nanosvg_strtoll(const char *text, char **endptr, int base)
{
    long value = std::strtol(text, endptr, base);
    return static_cast<long long>(value);
}

static int parse_hex_digits_exact(const char *text, int digits, unsigned int *out_value)
{
    if (text == nullptr || out_value == nullptr) {
        return 0;
    }

    unsigned int value = 0;
    for (int i = 0; i < digits; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        unsigned int digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<unsigned int>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<unsigned int>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            digit = static_cast<unsigned int>(ch - 'A' + 10);
        } else {
            return 0;
        }
        value = (value << 4U) | digit;
    }

    *out_value = value;
    return 1;
}

static const char *skip_spaces(const char *text)
{
    while (text != nullptr && *text != '\0' && std::isspace(static_cast<unsigned char>(*text)) != 0) {
        ++text;
    }
    return text;
}

static int parse_decimal_component(const char **cursor, unsigned int *value)
{
    const char *p = skip_spaces(*cursor);
    if (p == nullptr || *p < '0' || *p > '9') {
        return 0;
    }

    unsigned int parsed = 0;
    while (*p >= '0' && *p <= '9') {
        parsed = parsed * 10U + static_cast<unsigned int>(*p - '0');
        ++p;
    }
    *cursor = skip_spaces(p);
    *value = parsed;
    return 1;
}

static int nanosvg_sscanf(const char *text, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int matched = 0;

    if (std::strcmp(format, "#%2x%2x%2x") == 0) {
        unsigned int *a = va_arg(args, unsigned int *);
        unsigned int *b = va_arg(args, unsigned int *);
        unsigned int *c = va_arg(args, unsigned int *);
        if (text != nullptr && text[0] == '#' &&
            parse_hex_digits_exact(text + 1, 2, a) &&
            parse_hex_digits_exact(text + 3, 2, b) &&
            parse_hex_digits_exact(text + 5, 2, c)) {
            matched = 3;
        }
    } else if (std::strcmp(format, "#%1x%1x%1x") == 0) {
        unsigned int *a = va_arg(args, unsigned int *);
        unsigned int *b = va_arg(args, unsigned int *);
        unsigned int *c = va_arg(args, unsigned int *);
        if (text != nullptr && text[0] == '#' &&
            parse_hex_digits_exact(text + 1, 1, a) &&
            parse_hex_digits_exact(text + 2, 1, b) &&
            parse_hex_digits_exact(text + 3, 1, c)) {
            matched = 3;
        }
    } else if (std::strcmp(format, "rgb(%u, %u, %u)") == 0) {
        unsigned int *a = va_arg(args, unsigned int *);
        unsigned int *b = va_arg(args, unsigned int *);
        unsigned int *c = va_arg(args, unsigned int *);
        const char *p = text;
        if (p != nullptr && std::strncmp(p, "rgb(", 4) == 0) {
            p += 4;
            if (parse_decimal_component(&p, a) && *p == ',') {
                ++p;
                if (parse_decimal_component(&p, b) && *p == ',') {
                    ++p;
                    if (parse_decimal_component(&p, c) && *p == ')') {
                        matched = 3;
                    }
                }
            }
        }
    }

    va_end(args);
    return matched;
}

#define sscanf nanosvg_sscanf
#define strtoll nanosvg_strtoll
#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvg/nanosvgrast.h"
#undef sscanf
#undef strtoll

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#include "third_party/stb/stbi.h"

namespace
{

constexpr std::size_t kMaxEncodedImageBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxDecodedPixels = 4096 * 4096;

static std::string lower_copy(const std::string &value)
{
    std::string lowered = value;
    for (char &ch : lowered) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered;
}

static bool looks_like_svg(const std::string &encoded, const std::string &content_type)
{
    const std::string lowered_type = lower_copy(content_type);
    if (lowered_type.find("image/svg+xml") != std::string::npos) {
        return true;
    }

    std::size_t start = 0;
    if (encoded.size() >= 3 &&
        static_cast<unsigned char>(encoded[0]) == 0xEF &&
        static_cast<unsigned char>(encoded[1]) == 0xBB &&
        static_cast<unsigned char>(encoded[2]) == 0xBF) {
        start = 3;
    }
    while (start < encoded.size() && std::isspace(static_cast<unsigned char>(encoded[start])) != 0) {
        ++start;
    }

    return encoded.compare(start, 4, "<svg") == 0 ||
           encoded.compare(start, 5, "<?xml") == 0;
}

static bool looks_like_webp(const std::string &encoded)
{
    return encoded.size() >= 12 &&
           std::memcmp(encoded.data(), "RIFF", 4) == 0 &&
           std::memcmp(encoded.data() + 8, "WEBP", 4) == 0;
}

static bool decode_webp(const std::string &encoded,
                        BrowserDecodedImage &decoded,
                        std::string &error)
{
    int width = 0;
    int height = 0;
    if (WebPGetInfo(reinterpret_cast<const std::uint8_t *>(encoded.data()),
                    encoded.size(),
                    &width,
                    &height) == 0) {
        error = "WebP 头无效";
        return false;
    }

    const bool valid_size = width > 0 && height > 0;
    const std::size_t pixel_count = valid_size
                                        ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
                                        : 0;
    if (!valid_size || pixel_count == 0 || pixel_count > kMaxDecodedPixels) {
        error = "解码后的图片超过限制";
        return false;
    }

    decoded.width = width;
    decoded.height = height;
    decoded.pixels.resize(pixel_count);
    std::uint8_t *out = reinterpret_cast<std::uint8_t *>(decoded.pixels.data());
    if (WebPDecodeRGBAInto(reinterpret_cast<const std::uint8_t *>(encoded.data()),
                           encoded.size(),
                           out,
                           pixel_count * sizeof(XCOLORA),
                           width * static_cast<int>(sizeof(XCOLORA))) == nullptr) {
        decoded = BrowserDecodedImage();
        error = "WebP 解码失败";
        return false;
    }
    return true;
}

static bool decode_svg(const std::string &encoded,
                       BrowserDecodedImage &decoded,
                       std::string &error)
{
    std::vector<char> source(encoded.begin(), encoded.end());
    source.push_back('\0');

    NSVGimage *image = nsvgParse(source.data(), "px", 96.0f);
    if (image == nullptr) {
        error = "SVG 解析失败";
        return false;
    }

    const int width = static_cast<int>(image->width + 0.5f);
    const int height = static_cast<int>(image->height + 0.5f);
    const bool valid_size = width > 0 && height > 0;
    const std::size_t pixel_count = valid_size
                                        ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
                                        : 0;
    if (!valid_size || pixel_count == 0 || pixel_count > kMaxDecodedPixels) {
        nsvgDelete(image);
        error = "解码后的图片超过限制";
        return false;
    }

    NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
    if (rasterizer == nullptr) {
        nsvgDelete(image);
        error = "SVG 光栅化器初始化失败";
        return false;
    }

    decoded.width = width;
    decoded.height = height;
    decoded.pixels.resize(pixel_count);
    std::memset(decoded.pixels.data(), 0, pixel_count * sizeof(XCOLORA));
    nsvgRasterize(rasterizer,
                  image,
                  0.0f,
                  0.0f,
                  1.0f,
                  reinterpret_cast<unsigned char *>(decoded.pixels.data()),
                  width,
                  height,
                  width * static_cast<int>(sizeof(XCOLORA)));

    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(image);
    return true;
}

} // namespace

bool browser_decode_image(const std::string &encoded,
                          const std::string &content_type,
                          BrowserDecodedImage &decoded,
                          std::string &error)
{
    decoded = BrowserDecodedImage();

    if (encoded.empty()) {
        error = "图片内容为空";
        return false;
    }
    if (encoded.size() > kMaxEncodedImageBytes) {
        error = "图片过大";
        return false;
    }

    if (looks_like_svg(encoded, content_type)) {
        return decode_svg(encoded, decoded, error);
    }

    if (looks_like_webp(encoded)) {
        return decode_webp(encoded, decoded, error);
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *data = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(encoded.data()),
                                          static_cast<int>(encoded.size()),
                                          &width,
                                          &height,
                                          &channels,
                                          4);
    if (data == nullptr) {
        error = "图片解码失败";
        return false;
    }

    const bool valid_size = width > 0 && height > 0;
    const std::size_t pixel_count = valid_size
                                        ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
                                        : 0;
    if (!valid_size || pixel_count == 0 || pixel_count > kMaxDecodedPixels) {
        stbi_image_free(data);
        error = "解码后的图片超过限制";
        return false;
    }

    decoded.width = width;
    decoded.height = height;
    decoded.pixels.resize(pixel_count);
    std::memcpy(decoded.pixels.data(), data, pixel_count * sizeof(XCOLORA));
    stbi_image_free(data);
    return true;
}
