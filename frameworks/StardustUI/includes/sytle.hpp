#pragma once

class Sytel {
public:
    Sytel();
    ~Sytel();
    Sytel(const Sytel&) = default;
    Sytel& operator=(const Sytel&) = default;
    Sytel(Sytel&&) = default;
    Sytel& operator=(Sytel&&) = default;

    void set_color(unsigned int color);
    void set_size(unsigned int size);
    void set_background_color(unsigned int color);
    void set_border_color(unsigned int color);
    void set_border_width(unsigned int width);
    void set_radius(unsigned int radius);
    void set_padding(unsigned int padding);

    void unset_color();
    void unset_size();
    void unset_background_color();
    void unset_border_color();
    void unset_border_width();
    void unset_radius();
    void unset_padding();

    bool has_color() const;
    bool has_size() const;
    bool has_background_color() const;
    bool has_border_color() const;
    bool has_border_width() const;
    bool has_radius() const;
    bool has_padding() const;
    bool empty() const;

    unsigned int get_color(unsigned int fallback = 0) const;
    unsigned int get_size(unsigned int fallback = 0) const;
    unsigned int get_background_color(unsigned int fallback = 0) const;
    unsigned int get_border_color(unsigned int fallback = 0) const;
    unsigned int get_border_width(unsigned int fallback = 0) const;
    unsigned int get_radius(unsigned int fallback = 0) const;
    unsigned int get_padding(unsigned int fallback = 0) const;

    void clear();
    void merge_from(const Sytel& sytel);

private:
    bool color_enabled;
    bool size_enabled;
    bool background_color_enabled;
    bool border_color_enabled;
    bool border_width_enabled;
    bool radius_enabled;
    bool padding_enabled;

    unsigned int color;
    unsigned int size;
    unsigned int background_color;
    unsigned int border_color;
    unsigned int border_width;
    unsigned int radius;
    unsigned int padding;
};

class SytelRules {
public:
    SytelRules();
    ~SytelRules();
    SytelRules(const SytelRules&) = default;
    SytelRules& operator=(const SytelRules&) = default;
    SytelRules(SytelRules&&) = default;
    SytelRules& operator=(SytelRules&&) = default;

    void set_base_sytel(const Sytel& sytel);
    void set_on_mouse_sytel(const Sytel& sytel);
    void set_on_click_sytel(const Sytel& sytel);
    void set_on_hover_sytel(const Sytel& sytel);

    void unset_base_sytel();
    void unset_on_mouse_sytel();
    void unset_on_click_sytel();
    void unset_on_hover_sytel();

    bool has_base_sytel() const;
    bool has_on_mouse_sytel() const;
    bool has_on_click_sytel() const;
    bool has_on_hover_sytel() const;
    bool has_any_sytel() const;

    const Sytel& get_base_sytel() const;
    const Sytel& get_on_mouse_sytel() const;
    const Sytel& get_on_click_sytel() const;
    const Sytel& get_on_hover_sytel() const;

    Sytel resolve(bool on_mouse, bool on_click, bool on_hover) const;
    void clear();

private:
    Sytel base_sytel;
    Sytel on_mouse_sytel;
    Sytel on_click_sytel;
    Sytel on_hover_sytel;
};
