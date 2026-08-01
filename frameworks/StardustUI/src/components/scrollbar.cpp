#include "../../includes/components/scrollbar.hpp"

namespace {
int clamp_int(int value, int minimum, int maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

int max_int(int a, int b) {
    return a > b ? a : b;
}
}

ScrollBar::ScrollBar(int width, int height)
    : content_size(0),
      page_size(0),
      value(0),
      dragging(false),
      drag_anchor_y(0),
      drag_anchor_value(0),
      change_callback_func(nullptr) {
    this->set_bounds(0, 0, width, height);
}

ScrollBar::ScrollBar(int width, int height, const SytelRules& style)
    : content_size(0),
      page_size(0),
      value(0),
      dragging(false),
      drag_anchor_y(0),
      drag_anchor_value(0),
      change_callback_func(nullptr) {
    this->set_bounds(0, 0, width, height);
    this->set_style_rules(style);
}

ScrollBar::~ScrollBar() = default;

void ScrollBar::draw(unsigned long long handle) {
    const Sytel style = this->resolve_style();
    const unsigned int background_color = style.get_background_color(0xE7E0EBFF);
    const unsigned int border_color = style.get_border_color(background_color);
    const unsigned int border_width = style.get_border_width(1);
    const unsigned int thumb_color = style.get_color(0x6750A4FF);
    const unsigned int radius = style.get_radius(static_cast<unsigned int>(this->get_width() / 2));

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

    const int track_x = this->get_track_x();
    const int track_y = this->get_track_y();
    const int track_width = this->get_track_width();
    const int thumb_height = this->get_thumb_height();
    if (track_width > 0 && thumb_height > 0) {
        const unsigned int thumb_radius = static_cast<unsigned int>(track_width / 2);
        draw_round_rect(handle,
                        track_x,
                        track_y + this->get_thumb_offset(),
                        track_width,
                        thumb_height,
                        thumb_radius,
                        thumb_color);
    }
}

void ScrollBar::update() {
    base_component::update();
}

int ScrollBar::get_preferred_width() const {
    return this->get_width();
}

int ScrollBar::get_preferred_height() const {
    return this->get_height();
}

bool ScrollBar::handle_pointer_move(int x, int y) {
    const bool hovered = this->contains(x, y);
    bool changed = this->hover_active != hovered || this->mouse_active != hovered;
    this->set_hover_state(hovered);
    this->set_mouse_state(hovered);

    if (!this->dragging || this->get_max_value() <= 0) {
        return changed;
    }

    const int max_thumb_offset = this->get_max_thumb_offset();
    if (max_thumb_offset <= 0) {
        return changed;
    }

    const int delta_y = y - this->drag_anchor_y;
    const int next_value = this->drag_anchor_value + (delta_y * this->get_max_value()) / max_thumb_offset;
    if (this->set_value_internal(next_value, true)) {
        changed = true;
    }

    return changed;
}

bool ScrollBar::handle_left_button(bool pressed, int x, int y) {
    const bool inside = this->contains(x, y);
    bool changed = false;

    if (pressed) {
        changed = this->set_focus(inside);
        this->set_click_state(inside);
        this->dragging = false;
        if (!inside) {
            return changed;
        }

        this->set_mouse_state(true);
        this->set_hover_state(true);

        if (this->get_max_value() <= 0) {
            return true;
        }

        const int local_y = y - this->get_track_y();
        const int thumb_offset = this->get_thumb_offset();
        const int thumb_height = this->get_thumb_height();
        if (local_y >= thumb_offset && local_y < thumb_offset + thumb_height) {
            this->dragging = true;
            this->drag_anchor_y = y;
            this->drag_anchor_value = this->value;
            return true;
        }

        if (local_y < thumb_offset) {
            changed = this->set_value_internal(this->value - max_int(1, this->page_size), true) || changed;
        } else {
            changed = this->set_value_internal(this->value + max_int(1, this->page_size), true) || changed;
        }
        return changed;
    }

    const bool was_active = this->click_active;
    const bool was_dragging = this->dragging;
    this->dragging = false;
    this->set_click_state(false);
    if (was_active && inside) {
        this->run_callback();
    }
    return changed || was_active || was_dragging;
}

void ScrollBar::set_range(int content_size, int page_size) {
    if (content_size < 0) {
        content_size = 0;
    }
    if (page_size < 0) {
        page_size = 0;
    }

    const bool changed = this->content_size != content_size || this->page_size != page_size;
    this->content_size = content_size;
    this->page_size = page_size;
    const bool value_changed = this->set_value_internal(this->value, false);
    if (changed || value_changed) {
        this->request_redraw();
    }
}

void ScrollBar::set_value(int value) {
    this->set_value_internal(value, false);
}

int ScrollBar::get_value() const {
    return this->value;
}

int ScrollBar::get_content_size() const {
    return this->content_size;
}

int ScrollBar::get_page_size() const {
    return this->page_size;
}

int ScrollBar::get_max_value() const {
    const int max_value = this->content_size - this->page_size;
    return max_value > 0 ? max_value : 0;
}

bool ScrollBar::is_dragging() const {
    return this->dragging;
}

void ScrollBar::set_change_callback(void (*func)(ScrollBar&, int)) {
    this->change_callback_func = func;
}

int ScrollBar::get_track_x() const {
    return static_cast<int>(this->x) + 1;
}

int ScrollBar::get_track_y() const {
    return static_cast<int>(this->y) + 1;
}

int ScrollBar::get_track_width() const {
    return this->get_width() - 2;
}

int ScrollBar::get_track_height() const {
    return this->get_height() - 2;
}

int ScrollBar::get_thumb_height() const {
    const int track_height = this->get_track_height();
    if (track_height <= 0) {
        return 0;
    }
    if (this->content_size <= 0 || this->page_size <= 0 || this->content_size <= this->page_size) {
        return track_height;
    }

    int thumb_height = (track_height * this->page_size) / this->content_size;
    thumb_height = max_int(12, thumb_height);
    return clamp_int(thumb_height, 12, track_height);
}

int ScrollBar::get_thumb_offset() const {
    const int max_value = this->get_max_value();
    const int max_thumb_offset = this->get_max_thumb_offset();
    if (max_value <= 0 || max_thumb_offset <= 0) {
        return 0;
    }
    return (max_thumb_offset * this->value) / max_value;
}

int ScrollBar::get_max_thumb_offset() const {
    const int track_height = this->get_track_height();
    const int thumb_height = this->get_thumb_height();
    return track_height > thumb_height ? track_height - thumb_height : 0;
}

bool ScrollBar::set_value_internal(int value, bool notify) {
    const int clamped = clamp_int(value, 0, this->get_max_value());
    if (clamped == this->value) {
        return false;
    }

    this->value = clamped;
    this->request_redraw();
    if (notify && this->change_callback_func != nullptr) {
        this->change_callback_func(*this, this->value);
    }
    return true;
}
