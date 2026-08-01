#pragma once
#include "./string.hpp"
#include "./text/font.hpp"
namespace stardustui {
struct Colors{
    unsigned int primary;
    unsigned int on_primary;
    unsigned int primary_container;
    unsigned int on_primary_container;
    unsigned int secondary;
    unsigned int on_secondary;
    unsigned int secondary_container;
    unsigned int on_secondary_container;
    unsigned int tertiary;
    unsigned int on_tertiary;
    unsigned int tertiary_container;
    unsigned int on_tertiary_container;
    unsigned int error;
    unsigned int on_error;
    unsigned int error_container;
    unsigned int on_error_container;
    unsigned int background;
    unsigned int on_background;
    unsigned int surface;
    unsigned int on_surface;
    unsigned int surface_variant;
    unsigned int on_surface_variant;
    unsigned int inverse_surface;
    unsigned int inverse_on_surface;
    unsigned int outline;
    unsigned int outline_variant;
    unsigned int scrim;
    unsigned int shadow;
};
class Theme{
    public:
        Theme();
        Theme(const stardustui::string& path);
        ~Theme();

        Theme(const Theme&) = default;
        Theme& operator=(const Theme&) = default;
        Theme(Theme&&) = default;
        Theme& operator=(Theme&&) = default;

        static bool load_theme(const stardustui::string& path_or_name);
        static unsigned int get_theme_color(const stardustui::string& name);
        static Font get_theme_font();
        static Font get_theme_font(const stardustui::string& name);
        static const Colors& colors();
        static const Font& font();
        static const stardustui::string& path();
        static const stardustui::string& name();
        static const stardustui::string& version();
        static stardustui::string resolve_theme_path(const stardustui::string& path_or_name);
};
}
