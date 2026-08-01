#include "../../includes/components/button.hpp"
#include "../../settings.hpp"
#include "../../platforms/platform.hpp"

namespace {
int max_int(int a, int b) {
    return a > b ? a : b;
}
}

Button::Button(const stardustui::string& text, int width, int height)
    : text(text) {
    this->set_bounds(0, 0, width, height);
}

Button::Button(const stardustui::string& text, int width, int height, const SytelRules& style)
    : text(text) {
    this->set_bounds(0, 0, width, height);
    this->set_style_rules(style);
}

Button::~Button() = default;

void Button::draw(unsigned long long handle) {
    const Sytel style = this->resolve_style();
    const unsigned int background_color = style.get_background_color(0x6750A4FF);
    const unsigned int border_color = style.get_border_color(background_color);
    const unsigned int border_width = style.get_border_width(1);
    const unsigned int text_color = style.get_color(0xFFFFFFFF);
    const unsigned int text_size = style.get_size(16);
    const unsigned int padding = style.get_padding(12);
    const unsigned int radius = style.get_radius(20);
    const int text_height = static_cast<int>(calc_text_height(this->text, text_size));

    draw_round_rect(handle,
                    static_cast<int>(this->x),
                    static_cast<int>(this->y),
                    this->get_width(),
                    this->get_height(),
                    radius,
                    border_color);

    const int inner_x = static_cast<int>(this->x) + static_cast<int>(border_width);
    const int inner_y = static_cast<int>(this->y) + static_cast<int>(border_width);
    const int inner_width = this->get_width() - static_cast<int>(border_width * 2);
    const int inner_height = this->get_height() - static_cast<int>(border_width * 2);
    if (inner_width > 0 && inner_height > 0) {
        const unsigned int inner_radius = radius > border_width ? radius - border_width : 0;
        draw_round_rect(handle, inner_x, inner_y, inner_width, inner_height, inner_radius, background_color);
    }

    const int text_width = static_cast<int>(calc_text_width(this->text, text_size));
    const int text_x = static_cast<int>(this->x) + max_int(static_cast<int>(padding), (this->get_width() - text_width) / 2);
    const int text_y = static_cast<int>(this->y) + max_int(0, (this->get_height() - text_height) / 2);
    draw_text_on_solid_background(handle,
                                  text_x,
                                  text_y,
                                  text_color,
                                  text_size,
                                  background_color,
                                  this->text);
}

int Button::get_preferred_width() const {
    return this->get_width();
}

int Button::get_preferred_height() const {
    return this->get_height();
}

void Button::set_text(const stardustui::string& text) {
    this->text = text;
    this->request_redraw();
}

const stardustui::string& Button::get_text() const {
    return this->text;
}
