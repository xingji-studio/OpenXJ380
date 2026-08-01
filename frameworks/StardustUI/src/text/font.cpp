#include "../../includes/text/font.hpp"
#if !defined(XJ380)
#include <cstdlib>
#endif

namespace {
stardustui::string* g_default_font_path = nullptr;
stardustui::File::byte* g_default_font_data = nullptr;
int g_default_font_data_size = 0;

stardustui::string& default_font_path_storage()
{
    if (g_default_font_path == nullptr) {
        g_default_font_path = new stardustui::string();
    }
    return *g_default_font_path;
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

bool try_candidate(const stardustui::string& candidate, stardustui::string& resolved)
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

bool try_font_name_in_directory(const char* directory,
                                const char* name,
                                stardustui::string& resolved)
{
    if (directory == nullptr || name == nullptr || directory[0] == '\0' || name[0] == '\0') {
        return false;
    }

    stardustui::string candidate;
    if (join_path(candidate, directory, name) && try_candidate(candidate, resolved)) {
        return true;
    }

    if (has_extension(name)) {
        return false;
    }

    static const char* kExtensions[] = {
        ".ttf",
        ".otf",
        ".ttc"
    };

    for (unsigned int index = 0; index < sizeof(kExtensions) / sizeof(kExtensions[0]); ++index) {
        if (!join_path(candidate, directory, name)) {
            continue;
        }
        candidate.append(kExtensions[index]);
        if (try_candidate(candidate, resolved)) {
            return true;
        }
    }

    return false;
}

#if defined(STARDUSTUI_WINDOWS)
bool try_windows_font_directory(const char* root_directory,
                                const char* name,
                                stardustui::string& resolved)
{
    if (root_directory == nullptr || root_directory[0] == '\0') {
        return false;
    }

    stardustui::string font_directory;
    if (!join_path(font_directory, root_directory, "Fonts")) {
        return false;
    }

    return try_font_name_in_directory(font_directory.c_str(), name, resolved);
}

bool try_windows_system_font(const char* name, stardustui::string& resolved)
{
    const char* windir = std::getenv("WINDIR");
    if (try_windows_font_directory(windir, name, resolved)) {
        return true;
    }

    const char* system_root = std::getenv("SystemRoot");
    if (try_windows_font_directory(system_root, name, resolved)) {
        return true;
    }

    static const char* kFallbackDirs[] = {
        "C:/Windows",
        "C:\\Windows",
        "C:/WINNT"
    };

    for (unsigned int index = 0; index < sizeof(kFallbackDirs) / sizeof(kFallbackDirs[0]); ++index) {
        if (try_windows_font_directory(kFallbackDirs[index], name, resolved)) {
            return true;
        }
    }

    return false;
}
#endif
}

Font::Font()
    : font_path(),
      font_pixel_size(16),
      font_data(nullptr),
      font_data_size(0)
{
}

Font::Font(const stardustui::string& path, unsigned int size)
    : font_path(),
      font_pixel_size(size == 0 ? 16 : size),
      font_data(nullptr),
      font_data_size(0)
{
    load(path);
}

Font::~Font()
{
    clear();
}

Font::Font(const Font& other)
    : font_path(),
      font_pixel_size(other.font_pixel_size),
      font_data(nullptr),
      font_data_size(0)
{
    font_path = other.font_path;
    if (other.font_data != nullptr && other.font_data_size > 0) {
        assign_bytes(other.font_data, other.font_data_size);
    }
}

Font& Font::operator=(const Font& other)
{
    if (this == &other) {
        return *this;
    }

    clear();
    font_path = other.font_path;
    font_pixel_size = other.font_pixel_size;
    if (other.font_data != nullptr && other.font_data_size > 0) {
        assign_bytes(other.font_data, other.font_data_size);
    }
    return *this;
}

bool Font::assign_bytes(const stardustui::File::byte* data, int size)
{
    if (size < 0 || (size > 0 && data == nullptr)) {
        return false;
    }

    stardustui::File::byte* bytes = nullptr;
    if (size > 0) {
        bytes = new stardustui::File::byte[size];
        if (bytes == nullptr) {
            return false;
        }

        for (int index = 0; index < size; ++index) {
            bytes[index] = data[index];
        }
    }

    if (font_data != nullptr) {
        stardustui::File::free_bytes(font_data);
    }

    font_data = bytes;
    font_data_size = size;
    return true;
}

bool Font::load(const stardustui::string& path)
{
    const stardustui::string resolved_path = resolve_font_path(path);
    if (resolved_path.length() <= 0) {
        return false;
    }

    stardustui::File file(resolved_path);
    stardustui::File::byte* bytes = nullptr;
    int size = 0;
    if (!file.read_bytes(bytes, size)) {
        return false;
    }

    clear();
    font_path = resolved_path;
    font_data = bytes;
    font_data_size = size;
    return true;
}

bool Font::load(const stardustui::File::byte* data, int size)
{
    clear();
    return assign_bytes(data, size);
}

void Font::clear()
{
    font_path.assign("");
    if (font_data != nullptr) {
        stardustui::File::free_bytes(font_data);
        font_data = nullptr;
    }
    font_data_size = 0;
}

bool Font::is_loaded() const
{
    return font_data != nullptr && font_data_size > 0;
}

const stardustui::string& Font::path() const
{
    return font_path;
}

const stardustui::File::byte* Font::data() const
{
    return font_data;
}

int Font::data_size() const
{
    return font_data_size;
}

void Font::set_pixel_size(unsigned int size)
{
    font_pixel_size = size == 0 ? 16 : size;
}

unsigned int Font::pixel_size() const
{
    return font_pixel_size;
}

bool Font::set_default_font_path(const stardustui::string& path)
{
    const stardustui::string resolved_path = resolve_font_path(path);
    if (resolved_path.length() <= 0) {
        return false;
    }

    if (g_default_font_data != nullptr) {
        stardustui::File::free_bytes(g_default_font_data);
        g_default_font_data = nullptr;
    }
    g_default_font_data_size = 0;
    default_font_path_storage() = resolved_path;
    return true;
}

bool Font::set_default_font_memory(const stardustui::File::byte* data, int size)
{
    if (size < 0 || (size > 0 && data == nullptr)) {
        return false;
    }

    stardustui::File::byte* bytes = nullptr;
    if (size > 0) {
        bytes = new stardustui::File::byte[size];
        if (bytes == nullptr) {
            return false;
        }

        for (int index = 0; index < size; ++index) {
            bytes[index] = data[index];
        }
    }

    if (g_default_font_data != nullptr) {
        stardustui::File::free_bytes(g_default_font_data);
    }

    g_default_font_data = bytes;
    g_default_font_data_size = size;
    default_font_path_storage().assign("");
    return true;
}

void Font::clear_default_font()
{
    if (g_default_font_path != nullptr) {
        g_default_font_path->assign("");
    }
    if (g_default_font_data != nullptr) {
        stardustui::File::free_bytes(g_default_font_data);
        g_default_font_data = nullptr;
    }
    g_default_font_data_size = 0;
}

bool Font::has_default_font()
{
    return (g_default_font_data != nullptr && g_default_font_data_size > 0) ||
           default_font_path_storage().length() > 0;
}

const stardustui::string& Font::default_font_path()
{
    return default_font_path_storage();
}

const stardustui::File::byte* Font::default_font_data()
{
    return g_default_font_data;
}

int Font::default_font_data_size()
{
    return g_default_font_data_size;
}

stardustui::string Font::resolve_font_path(const stardustui::string& path_or_name)
{
    stardustui::string resolved;
    if (path_or_name.length() <= 0) {
        return resolved;
    }

    if (try_candidate(path_or_name, resolved)) {
        return resolved;
    }

    const char* text = path_or_name.c_str();
    if (contains_path_separator(text)) {
        if (has_extension(text)) {
            return stardustui::string();
        }

        static const char* kExtensions[] = {
            ".ttf",
            ".otf",
            ".ttc"
        };

        for (unsigned int index = 0; index < sizeof(kExtensions) / sizeof(kExtensions[0]); ++index) {
            stardustui::string candidate(path_or_name.c_str());
            candidate.append(kExtensions[index]);
            if (try_candidate(candidate, resolved)) {
                return resolved;
            }
        }

        return stardustui::string();
    }

    stardustui::string home;
    if (read_home_directory(home)) {
        stardustui::string user_dir(home.c_str());
        user_dir.append("/.cache/font");
        if (try_font_name_in_directory(user_dir.c_str(), text, resolved)) {
            return resolved;
        }
    }

    static const char* kSystemDirs[] = {
        "/system/font",
        "system/font"
    };

    for (unsigned int index = 0; index < sizeof(kSystemDirs) / sizeof(kSystemDirs[0]); ++index) {
        if (try_font_name_in_directory(kSystemDirs[index], text, resolved)) {
            return resolved;
        }
    }

#if defined(STARDUSTUI_WINDOWS)
    if (try_windows_system_font(text, resolved)) {
        return resolved;
    }

    static const char* kWindowsFallbackFonts[] = {
        "msyh",
        "msyhbd",
        "Microsoft YaHei",
        "segoeui",
        "arialuni",
        "arial",
        "simsun",
        "simhei"
    };

    for (unsigned int index = 0; index < sizeof(kWindowsFallbackFonts) / sizeof(kWindowsFallbackFonts[0]); ++index) {
        if (try_windows_system_font(kWindowsFallbackFonts[index], resolved)) {
            return resolved;
        }
    }
#endif

    return stardustui::string();
}
