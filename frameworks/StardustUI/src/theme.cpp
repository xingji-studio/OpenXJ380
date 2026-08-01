#include "../includes/theme.hpp"
#include "../platforms/platform.hpp"
#if !defined(XJ380)
#include <cstdlib>
#endif

namespace {
bool g_theme_initialized = false;
stardustui::string* g_theme_path = nullptr;
stardustui::string* g_theme_name = nullptr;
stardustui::string* g_theme_version = nullptr;
Font* g_theme_font = nullptr;
stardustui::Colors g_theme_colors;

stardustui::string& theme_path_storage()
{
    if (g_theme_path == nullptr) {
        g_theme_path = new stardustui::string();
    }
    return *g_theme_path;
}

stardustui::string& theme_name_storage()
{
    if (g_theme_name == nullptr) {
        g_theme_name = new stardustui::string();
    }
    return *g_theme_name;
}

stardustui::string& theme_version_storage()
{
    if (g_theme_version == nullptr) {
        g_theme_version = new stardustui::string();
    }
    return *g_theme_version;
}

Font& theme_font_storage()
{
    if (g_theme_font == nullptr) {
        g_theme_font = new Font();
    }
    return *g_theme_font;
}

bool starts_with(const char* text, const char* prefix)
{
    if (text == nullptr || prefix == nullptr) {
        return false;
    }

    int index = 0;
    while (prefix[index] != '\0') {
        if (text[index] != prefix[index]) {
            return false;
        }
        ++index;
    }
    return true;
}

bool contains_path_separator(const char* text)
{
    if (text == nullptr) {
        return false;
    }

    for (int index = 0; text[index] != '\0'; ++index) {
        if (text[index] == '/' || text[index] == '\\') {
            return true;
        }
    }
    return false;
}

bool has_extension(const char* text)
{
    if (text == nullptr) {
        return false;
    }

    int last_dot = -1;
    for (int index = 0; text[index] != '\0'; ++index) {
        if (text[index] == '/' || text[index] == '\\') {
            last_dot = -1;
            continue;
        }
        if (text[index] == '.') {
            last_dot = index;
        }
    }

    return last_dot >= 0;
}

bool read_home_directory(stardustui::string& out)
{
    out.assign("");
#if defined(XJ380)
    return false;
#else
    const char* home = std::getenv("HOME");
#if defined(STARDUSTUI_WINDOWS)
    if (home == nullptr || home[0] == '\0') {
        home = std::getenv("USERPROFILE");
    }
#endif
    if (home == nullptr || home[0] == '\0') {
        return false;
    }

    out.assign(home);
    return true;
#endif
}

bool expand_home_path(const stardustui::string& input, stardustui::string& out)
{
    out.assign(input.c_str());
    if (!starts_with(input.c_str(), "$HOME/")) {
        return true;
    }

    stardustui::string home;
    if (!read_home_directory(home)) {
        out.assign("");
        return false;
    }

    out = home;
    out.append(input.c_str() + 5);
    return true;
}

bool try_existing_file(const stardustui::string& candidate, stardustui::string& resolved)
{
    stardustui::string expanded;
    if (!expand_home_path(candidate, expanded) || expanded.length() <= 0) {
        return false;
    }

    if (!stardustui::File::exists(expanded)) {
        return false;
    }

    resolved = expanded;
    return true;
}

bool join_path(stardustui::string& out, const char* base, const char* leaf)
{
    if (base == nullptr || leaf == nullptr || base[0] == '\0' || leaf[0] == '\0') {
        return false;
    }

    out.assign(base);
    const int length = out.length();
    if (length > 0 && out.c_str()[length - 1] != '/') {
        out.push_char('/');
    }
    return out.append(leaf);
}

bool try_theme_in_directory(const char* directory,
                            const char* theme_name,
                            stardustui::string& resolved)
{
    if (directory == nullptr || theme_name == nullptr || directory[0] == '\0' || theme_name[0] == '\0') {
        return false;
    }

    stardustui::string candidate;
    if (join_path(candidate, directory, theme_name) && try_existing_file(candidate, resolved)) {
        return true;
    }

    if (has_extension(theme_name)) {
        return false;
    }

    if (join_path(candidate, directory, theme_name)) {
        candidate.append(".json");
        if (try_existing_file(candidate, resolved)) {
            return true;
        }
    }

    if (join_path(candidate, directory, theme_name)) {
        candidate.append(".theme.json");
        if (try_existing_file(candidate, resolved)) {
            return true;
        }
    }

    return false;
}

bool is_whitespace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

const char* skip_whitespace(const char* text)
{
    if (text == nullptr) {
        return nullptr;
    }

    while (*text != '\0' && is_whitespace(*text)) {
        ++text;
    }
    return text;
}

unsigned int parse_hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return static_cast<unsigned int>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<unsigned int>(10 + ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<unsigned int>(10 + ch - 'A');
    }
    return 16u;
}

bool parse_color_text(const char* text, unsigned int& color)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    int offset = 0;
    if (text[0] == '#') {
        offset = 1;
    } else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        offset = 2;
    }

    int length = 0;
    while (text[offset + length] != '\0') {
        ++length;
    }

    if (length != 6 && length != 8) {
        return false;
    }

    unsigned int value = 0;
    for (int index = 0; index < length; ++index) {
        const unsigned int digit = parse_hex_digit(text[offset + index]);
        if (digit > 15u) {
            return false;
        }
        value = (value << 4) | digit;
    }

    if (length == 6) {
        value = (value << 8) | 0xFFu;
    }

    color = value;
    return true;
}

bool find_key_value(const char* start,
                    const char* end,
                    const char* key,
                    const char*& out_value)
{
    if (start == nullptr || key == nullptr || key[0] == '\0') {
        return false;
    }

    const char* cursor = start;
    while (*cursor != '\0' && (end == nullptr || cursor < end)) {
        if (*cursor != '"') {
            ++cursor;
            continue;
        }

        ++cursor;
        const char* key_start = cursor;
        int matched = 0;
        while (key[matched] != '\0' && key_start[matched] != '\0' && key_start[matched] == key[matched]) {
            ++matched;
        }

        if (key[matched] == '\0' && key_start[matched] == '"') {
            cursor = key_start + matched + 1;
            cursor = skip_whitespace(cursor);
            if (cursor == nullptr || *cursor != ':') {
                continue;
            }
            ++cursor;
            out_value = skip_whitespace(cursor);
            return out_value != nullptr && (end == nullptr || out_value < end);
        }

        while (*cursor != '\0' && *cursor != '"') {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor += 2;
            } else {
                ++cursor;
            }
        }
        if (*cursor == '"') {
            ++cursor;
        }
    }

    return false;
}

bool extract_string_value(const char* value_start, stardustui::string& out)
{
    out.assign("");
    if (value_start == nullptr || *value_start != '"') {
        return false;
    }

    const char* cursor = value_start + 1;
    while (*cursor != '\0' && *cursor != '"') {
        if (*cursor == '\\' && cursor[1] != '\0') {
            ++cursor;
        }
        out.push_char(*cursor);
        ++cursor;
    }

    return *cursor == '"';
}

bool extract_uint_value(const char* value_start, unsigned int& out)
{
    if (value_start == nullptr) {
        return false;
    }

    const char* cursor = value_start;
    if (*cursor < '0' || *cursor > '9') {
        return false;
    }

    unsigned int value = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        value = value * 10u + static_cast<unsigned int>(*cursor - '0');
        ++cursor;
    }

    out = value;
    return true;
}

bool extract_color_value(const char* start,
                         const char* end,
                         const char* key,
                         unsigned int fallback,
                         unsigned int& out)
{
    const char* value_start = nullptr;
    if (!find_key_value(start, end, key, value_start)) {
        out = fallback;
        return false;
    }

    if (*value_start == '"') {
        stardustui::string text;
        if (extract_string_value(value_start, text) && parse_color_text(text.c_str(), out)) {
            return true;
        }
    } else {
        unsigned int value = 0;
        if (extract_uint_value(value_start, value)) {
            out = value;
            return true;
        }
    }

    out = fallback;
    return false;
}

const char* find_matching_brace(const char* start)
{
    if (start == nullptr || *start != '{') {
        return nullptr;
    }

    int depth = 0;
    const char* cursor = start;
    while (*cursor != '\0') {
        if (*cursor == '"') {
            ++cursor;
            while (*cursor != '\0' && *cursor != '"') {
                if (*cursor == '\\' && cursor[1] != '\0') {
                    cursor += 2;
                } else {
                    ++cursor;
                }
            }
            if (*cursor == '\0') {
                return nullptr;
            }
        } else if (*cursor == '{') {
            ++depth;
        } else if (*cursor == '}') {
            --depth;
            if (depth == 0) {
                return cursor;
            }
        }
        ++cursor;
    }

    return nullptr;
}

stardustui::Colors md3_default_colors()
{
    stardustui::Colors colors;
    colors.primary = 0x6750A4FF;
    colors.on_primary = 0xFFFFFFFF;
    colors.primary_container = 0xE9DDFFFF;
    colors.on_primary_container = 0x22005DFF;
    colors.secondary = 0x625B71FF;
    colors.on_secondary = 0xFFFFFFFF;
    colors.secondary_container = 0xE8DEF8FF;
    colors.on_secondary_container = 0x1E192BFF;
    colors.tertiary = 0x7E5260FF;
    colors.on_tertiary = 0xFFFFFFFF;
    colors.tertiary_container = 0xFFD9E3FF;
    colors.on_tertiary_container = 0x31101DFF;
    colors.error = 0xBA1A1AFF;
    colors.on_error = 0xFFFFFFFF;
    colors.error_container = 0xFFDAD6FF;
    colors.on_error_container = 0x410002FF;
    colors.background = 0xFFFBFFFF;
    colors.on_background = 0x1C1B1EFF;
    colors.surface = 0xFFFBFFFF;
    colors.on_surface = 0x1C1B1EFF;
    colors.surface_variant = 0xE7E0EBFF;
    colors.on_surface_variant = 0x49454EFF;
    colors.inverse_surface = 0x313033FF;
    colors.inverse_on_surface = 0xF4EFF4FF;
    colors.outline = 0x7A757FFF;
    colors.outline_variant = 0xCAC4CFFF;
    colors.scrim = 0x000000FF;
    colors.shadow = 0x000000FF;
    return colors;
}

void ensure_theme_defaults()
{
    if (g_theme_initialized) {
        return;
    }

    g_theme_colors = md3_default_colors();
    theme_path_storage().assign("");
    theme_name_storage().assign("md3-light");
    theme_version_storage().assign("1.0");
    theme_font_storage().clear();
    g_theme_initialized = true;
}

void apply_color_overrides(const char* start,
                           const char* end,
                           stardustui::Colors& colors)
{
    extract_color_value(start, end, "primary", colors.primary, colors.primary);
    extract_color_value(start, end, "on_primary", colors.on_primary, colors.on_primary);
    extract_color_value(start, end, "primary_container", colors.primary_container, colors.primary_container);
    extract_color_value(start, end, "on_primary_container", colors.on_primary_container, colors.on_primary_container);
    extract_color_value(start, end, "secondary", colors.secondary, colors.secondary);
    extract_color_value(start, end, "on_secondary", colors.on_secondary, colors.on_secondary);
    extract_color_value(start, end, "secondary_container", colors.secondary_container, colors.secondary_container);
    extract_color_value(start, end, "on_secondary_container", colors.on_secondary_container, colors.on_secondary_container);
    extract_color_value(start, end, "tertiary", colors.tertiary, colors.tertiary);
    extract_color_value(start, end, "on_tertiary", colors.on_tertiary, colors.on_tertiary);
    extract_color_value(start, end, "tertiary_container", colors.tertiary_container, colors.tertiary_container);
    extract_color_value(start, end, "on_tertiary_container", colors.on_tertiary_container, colors.on_tertiary_container);
    extract_color_value(start, end, "error", colors.error, colors.error);
    extract_color_value(start, end, "on_error", colors.on_error, colors.on_error);
    extract_color_value(start, end, "error_container", colors.error_container, colors.error_container);
    extract_color_value(start, end, "on_error_container", colors.on_error_container, colors.on_error_container);
    extract_color_value(start, end, "background", colors.background, colors.background);
    extract_color_value(start, end, "on_background", colors.on_background, colors.on_background);
    extract_color_value(start, end, "surface", colors.surface, colors.surface);
    extract_color_value(start, end, "on_surface", colors.on_surface, colors.on_surface);
    extract_color_value(start, end, "surface_variant", colors.surface_variant, colors.surface_variant);
    extract_color_value(start, end, "on_surface_variant", colors.on_surface_variant, colors.on_surface_variant);
    extract_color_value(start, end, "inverse_surface", colors.inverse_surface, colors.inverse_surface);
    extract_color_value(start, end, "inverse_on_surface", colors.inverse_on_surface, colors.inverse_on_surface);
    extract_color_value(start, end, "outline", colors.outline, colors.outline);
    extract_color_value(start, end, "outline_variant", colors.outline_variant, colors.outline_variant);
    extract_color_value(start, end, "scrim", colors.scrim, colors.scrim);
    extract_color_value(start, end, "shadow", colors.shadow, colors.shadow);
}

bool extract_named_string(const char* start,
                          const char* end,
                          const char* key,
                          stardustui::string& out)
{
    const char* value_start = nullptr;
    if (!find_key_value(start, end, key, value_start)) {
        return false;
    }
    return extract_string_value(value_start, out);
}

void apply_font_overrides(const char* text, Font& font)
{
    const char* font_value = nullptr;
    if (!find_key_value(text, nullptr, "font", font_value)) {
        return;
    }

    stardustui::string identifier;
    unsigned int pixel_size = font.pixel_size();

    if (*font_value == '"') {
        if (!extract_string_value(font_value, identifier)) {
            return;
        }
    } else if (*font_value == '{') {
        const char* font_end = find_matching_brace(font_value);
        if (font_end == nullptr) {
            return;
        }

        if (!extract_named_string(font_value, font_end, "name", identifier)) {
            extract_named_string(font_value, font_end, "path", identifier);
        }

        const char* size_value = nullptr;
        unsigned int parsed_size = 0;
        if (find_key_value(font_value, font_end, "size", size_value) &&
            extract_uint_value(size_value, parsed_size)) {
            pixel_size = parsed_size;
        }
    } else {
        return;
    }

    const char* top_level_font_size = nullptr;
    unsigned int parsed_top_level_font_size = 0;
    if (find_key_value(text, nullptr, "font_size", top_level_font_size) &&
        extract_uint_value(top_level_font_size, parsed_top_level_font_size)) {
        pixel_size = parsed_top_level_font_size;
    }

    if (identifier.length() <= 0) {
        return;
    }

    Font parsed_font(identifier, pixel_size);
    if (!parsed_font.is_loaded()) {
        return;
    }

    font = parsed_font;
}
}

stardustui::Theme::Theme()
{
    ensure_theme_defaults();
}

stardustui::Theme::Theme(const stardustui::string &path)
{
    ensure_theme_defaults();
    load_theme(path);
}

stardustui::Theme::~Theme() {}

bool stardustui::Theme::load_theme(const stardustui::string &path_or_name)
{
    ensure_theme_defaults();

    const stardustui::string resolved_path = resolve_theme_path(path_or_name);
    if (resolved_path.length() <= 0) {
        return false;
    }

    stardustui::string json_data;
    File file(resolved_path);
    if (!file.read_text(json_data)) {
        return false;
    }

    stardustui::Colors parsed_colors = md3_default_colors();
    stardustui::string parsed_name("md3-light");
    stardustui::string parsed_version("1.0");
    Font parsed_font;

    const char* text = json_data.c_str();
    extract_named_string(text, nullptr, "name", parsed_name);
    extract_named_string(text, nullptr, "version", parsed_version);

    const char* colors_value = nullptr;
    if (find_key_value(text, nullptr, "colors", colors_value) && *colors_value == '{') {
        const char* colors_end = find_matching_brace(colors_value);
        if (colors_end != nullptr) {
            apply_color_overrides(colors_value, colors_end, parsed_colors);
        }
    }

    apply_font_overrides(text, parsed_font);

    g_theme_colors = parsed_colors;
    theme_path_storage() = resolved_path;
    theme_name_storage() = parsed_name;
    theme_version_storage() = parsed_version;
    theme_font_storage() = parsed_font;

    if (theme_font_storage().path().length() > 0) {
        Font::set_default_font_path(theme_font_storage().path());
    } else if (theme_font_storage().data() != nullptr && theme_font_storage().data_size() > 0) {
        Font::set_default_font_memory(theme_font_storage().data(), theme_font_storage().data_size());
    }

    return true;
}

unsigned int stardustui::Theme::get_theme_color(const stardustui::string& name)
{
    ensure_theme_defaults();

    if (name.equals("primary")) return g_theme_colors.primary;
    if (name.equals("on_primary")) return g_theme_colors.on_primary;
    if (name.equals("primary_container")) return g_theme_colors.primary_container;
    if (name.equals("on_primary_container")) return g_theme_colors.on_primary_container;
    if (name.equals("secondary")) return g_theme_colors.secondary;
    if (name.equals("on_secondary")) return g_theme_colors.on_secondary;
    if (name.equals("secondary_container")) return g_theme_colors.secondary_container;
    if (name.equals("on_secondary_container")) return g_theme_colors.on_secondary_container;
    if (name.equals("tertiary")) return g_theme_colors.tertiary;
    if (name.equals("on_tertiary")) return g_theme_colors.on_tertiary;
    if (name.equals("tertiary_container")) return g_theme_colors.tertiary_container;
    if (name.equals("on_tertiary_container")) return g_theme_colors.on_tertiary_container;
    if (name.equals("error")) return g_theme_colors.error;
    if (name.equals("on_error")) return g_theme_colors.on_error;
    if (name.equals("error_container")) return g_theme_colors.error_container;
    if (name.equals("on_error_container")) return g_theme_colors.on_error_container;
    if (name.equals("background")) return g_theme_colors.background;
    if (name.equals("on_background")) return g_theme_colors.on_background;
    if (name.equals("surface")) return g_theme_colors.surface;
    if (name.equals("on_surface")) return g_theme_colors.on_surface;
    if (name.equals("surface_variant")) return g_theme_colors.surface_variant;
    if (name.equals("on_surface_variant")) return g_theme_colors.on_surface_variant;
    if (name.equals("inverse_surface")) return g_theme_colors.inverse_surface;
    if (name.equals("inverse_on_surface")) return g_theme_colors.inverse_on_surface;
    if (name.equals("outline")) return g_theme_colors.outline;
    if (name.equals("outline_variant")) return g_theme_colors.outline_variant;
    if (name.equals("scrim")) return g_theme_colors.scrim;
    if (name.equals("shadow")) return g_theme_colors.shadow;
    return 0;
}

Font stardustui::Theme::get_theme_font()
{
    ensure_theme_defaults();
    return theme_font_storage();
}

Font stardustui::Theme::get_theme_font(const stardustui::string& name)
{
    ensure_theme_defaults();
    if (name.length() <= 0) {
        return theme_font_storage();
    }

    Font font(name);
    if (!font.is_loaded()) {
        return theme_font_storage();
    }
    font.set_pixel_size(theme_font_storage().pixel_size());
    return font;
}

const stardustui::Colors& stardustui::Theme::colors()
{
    ensure_theme_defaults();
    return g_theme_colors;
}

const Font& stardustui::Theme::font()
{
    ensure_theme_defaults();
    return theme_font_storage();
}

const stardustui::string& stardustui::Theme::path()
{
    ensure_theme_defaults();
    return theme_path_storage();
}

const stardustui::string& stardustui::Theme::name()
{
    ensure_theme_defaults();
    return theme_name_storage();
}

const stardustui::string& stardustui::Theme::version()
{
    ensure_theme_defaults();
    return theme_version_storage();
}

stardustui::string stardustui::Theme::resolve_theme_path(const stardustui::string& path_or_name)
{
    stardustui::string resolved;
    if (path_or_name.length() <= 0) {
        return resolved;
    }

    if (try_existing_file(path_or_name, resolved)) {
        return resolved;
    }

    const char* text = path_or_name.c_str();
    if (contains_path_separator(text)) {
        if (has_extension(text)) {
            return stardustui::string();
        }

        stardustui::string candidate(path_or_name.c_str());
        candidate.append(".json");
        if (try_existing_file(candidate, resolved)) {
            return resolved;
        }

        candidate.assign(path_or_name.c_str());
        candidate.append(".theme.json");
        if (try_existing_file(candidate, resolved)) {
            return resolved;
        }
        return stardustui::string();
    }

    stardustui::string home;
    if (read_home_directory(home)) {
        stardustui::string user_dir(home.c_str());
        user_dir.append("/.config/stardustui/theme");
        if (try_theme_in_directory(user_dir.c_str(), text, resolved)) {
            return resolved;
        }
    }

    if (try_theme_in_directory("/etc/stardustui/theme", text, resolved)) {
        return resolved;
    }

    return stardustui::string();
}
