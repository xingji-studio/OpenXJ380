#pragma once

#include "../file.hpp"
#include "../string.hpp"

class Font {
public:
    Font();
    Font(const stardustui::string& path, unsigned int size = 16);
    ~Font();

    Font(const Font& other);
    Font& operator=(const Font& other);

    bool load(const stardustui::string& path);
    bool load(const stardustui::File::byte* data, int size);
    void clear();

    bool is_loaded() const;
    const stardustui::string& path() const;
    const stardustui::File::byte* data() const;
    int data_size() const;

    void set_pixel_size(unsigned int size);
    unsigned int pixel_size() const;

    static bool set_default_font_path(const stardustui::string& path);
    static bool set_default_font_memory(const stardustui::File::byte* data, int size);
    static void clear_default_font();
    static bool has_default_font();
    static const stardustui::string& default_font_path();
    static const stardustui::File::byte* default_font_data();
    static int default_font_data_size();
    static stardustui::string resolve_font_path(const stardustui::string& path_or_name);

private:
    bool assign_bytes(const stardustui::File::byte* data, int size);

    stardustui::string font_path;
    unsigned int font_pixel_size;
    stardustui::File::byte* font_data;
    int font_data_size;
};
