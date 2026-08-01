#include "../../includes/components/flex.hpp"

namespace {
int clamp_non_negative(int value) {
    return value < 0 ? 0 : value;
}

int min_int(int a, int b) {
    return a < b ? a : b;
}

int max_int(int a, int b) {
    return a > b ? a : b;
}
}

FlexLayout::FlexLayout(int width, int height)
    : items(),
      direction(Row),
      align_items(AlignStretch),
      justify_content(JustifyStart),
      gap(0),
      padding(0),
      layout_dirty(true) {
    this->set_bounds(0, 0, width, height);
}

FlexLayout::~FlexLayout() = default;

void FlexLayout::draw(unsigned long long handle) {
    this->perform_layout();

    const Sytel style = this->resolve_style();
    const bool has_background = style.has_background_color();
    const bool has_border = style.has_border_width() && style.has_border_color() && style.get_border_width(0) > 0;
    const unsigned int border_width = has_border ? style.get_border_width(0) : 0;
    const unsigned int border_color = has_border ? style.get_border_color(0) : 0;
    const unsigned int background_color = has_background ? style.get_background_color(0) : 0;
    const unsigned int radius = style.get_radius(0);

    if (has_border && this->get_width() > 0 && this->get_height() > 0) {
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
        if (has_background && inner_width > 0 && inner_height > 0) {
            const unsigned int inner_radius = radius > border_width ? radius - border_width : 0;
            draw_round_rect(handle, inner_x, inner_y, inner_width, inner_height, inner_radius, background_color);
        }
    } else if (has_background && this->get_width() > 0 && this->get_height() > 0) {
        draw_round_rect(handle,
                        static_cast<int>(this->x),
                        static_cast<int>(this->y),
                        this->get_width(),
                        this->get_height(),
                        radius,
                        background_color);
    }

    for (int index = 0; index < this->items.size(); ++index) {
        if (this->items[index].component != nullptr) {
            this->items[index].component->draw(handle);
        }
    }
}

void FlexLayout::update() {
    base_component::update();
    for (int index = 0; index < this->items.size(); ++index) {
        base_component* component = this->items[index].component;
        if (component == nullptr) {
            continue;
        }

        component->update();
        if (component->consume_redraw_request()) {
            this->request_redraw();
        }
    }
}

bool FlexLayout::contains(int x, int y) const {
    return base_component::contains(x, y);
}

int FlexLayout::get_preferred_width() const {
    return this->get_width();
}

int FlexLayout::get_preferred_height() const {
    return this->get_height();
}

void FlexLayout::set_bounds(int x, int y, int width, int height) {
    base_component::set_bounds(x, y, width, height);
    this->layout_dirty = true;
    this->request_redraw();
}

bool FlexLayout::handle_pointer_move(int x, int y) {
    this->perform_layout();

    bool changed = base_component::handle_pointer_move(x, y);
    for (int index = 0; index < this->items.size(); ++index) {
        base_component* component = this->items[index].component;
        if (component == nullptr) {
            continue;
        }

        if (component->handle_pointer_move(x, y)) {
            changed = true;
        }
    }

    return changed;
}

bool FlexLayout::handle_left_button(bool pressed, int x, int y) {
    this->perform_layout();

    bool changed = base_component::handle_left_button(pressed, x, y);
    for (int index = 0; index < this->items.size(); ++index) {
        base_component* component = this->items[index].component;
        if (component == nullptr) {
            continue;
        }

        if (component->handle_left_button(pressed, x, y)) {
            changed = true;
        }
    }

    return changed;
}

bool FlexLayout::handle_char_input(char ch, bool special) {
    this->perform_layout();

    bool changed = false;
    for (int index = 0; index < this->items.size(); ++index) {
        base_component* component = this->items[index].component;
        if (component == nullptr) {
            continue;
        }

        if (component->handle_char_input(ch, special)) {
            changed = true;
        }
    }

    return changed;
}

void FlexLayout::set_direction(Direction direction) {
    this->direction = direction;
    for (int index = 0; index < this->items.size(); ++index) {
        if (this->items[index].component == nullptr) {
            continue;
        }
        this->items[index].basis_main = direction == Row
            ? this->items[index].component->get_preferred_width()
            : this->items[index].component->get_preferred_height();
        this->items[index].basis_cross = direction == Row
            ? this->items[index].component->get_preferred_height()
            : this->items[index].component->get_preferred_width();
    }
    this->layout_dirty = true;
    this->request_redraw();
}

void FlexLayout::set_align_items(Align align) {
    this->align_items = align;
    this->layout_dirty = true;
    this->request_redraw();
}

void FlexLayout::set_justify_content(Justify justify) {
    this->justify_content = justify;
    this->layout_dirty = true;
    this->request_redraw();
}

void FlexLayout::set_gap(int gap) {
    this->gap = clamp_non_negative(gap);
    this->layout_dirty = true;
    this->request_redraw();
}

void FlexLayout::set_padding(int padding) {
    this->padding = clamp_non_negative(padding);
    this->layout_dirty = true;
    this->request_redraw();
}

void FlexLayout::addComponent(base_component& component, int flex_grow) {
    this->addComponent(&component, flex_grow);
}

void FlexLayout::addComponent(base_component* component, int flex_grow) {
    if (component == nullptr) {
        return;
    }

    Item item;
    item.component = component;
    item.flex_grow = clamp_non_negative(flex_grow);
    item.basis_main = this->direction == Row ? component->get_preferred_width() : component->get_preferred_height();
    item.basis_cross = this->direction == Row ? component->get_preferred_height() : component->get_preferred_width();
    if (!this->items.push_back(item)) {
        return;
    }

    this->layout_dirty = true;
    this->request_redraw();
}

void FlexLayout::perform_layout() {
    if (!this->layout_dirty) {
        return;
    }

    const int count = this->items.size();
    if (count <= 0) {
        this->layout_dirty = false;
        return;
    }

    const int container_width = this->get_width();
    const int container_height = this->get_height();
    const int padding_twice = this->padding * 2;
    const int inner_width = clamp_non_negative(container_width - padding_twice);
    const int inner_height = clamp_non_negative(container_height - padding_twice);
    const int total_gap = this->gap * (count > 0 ? count - 1 : 0);
    const int available_main = clamp_non_negative((this->direction == Row ? inner_width : inner_height) - total_gap);
    const int available_cross = clamp_non_negative(this->direction == Row ? inner_height : inner_width);

    int preferred_total = 0;
    int total_grow = 0;
    int visible_count = 0;
    for (int index = 0; index < count; ++index) {
        base_component* component = this->items[index].component;
        if (component == nullptr) {
            continue;
        }

        preferred_total += max_int(0, this->items[index].basis_main);
        total_grow += this->items[index].flex_grow;
        ++visible_count;
    }

    if (visible_count <= 0) {
        this->layout_dirty = false;
        return;
    }

    int leftover = available_main - preferred_total;
    int overflow = 0;
    if (leftover < 0) {
        overflow = -leftover;
        leftover = 0;
    }

    int base_x = static_cast<int>(this->x) + this->padding;
    int base_y = static_cast<int>(this->y) + this->padding;
    int main_offset = 0;
    int dynamic_gap = this->gap;

    if (total_grow == 0) {
        if (this->justify_content == JustifyCenter) {
            main_offset = leftover / 2;
        } else if (this->justify_content == JustifyEnd) {
            main_offset = leftover;
        } else if (this->justify_content == JustifySpaceBetween && visible_count > 1) {
            dynamic_gap = this->gap + leftover / (visible_count - 1);
        }
    }

    int cursor = main_offset;
    int remaining_extra = leftover;
    int remaining_grow = total_grow;
    int remaining_overflow = overflow;
    int remaining_shrinkable = preferred_total;

    for (int index = 0; index < count; ++index) {
        base_component* component = this->items[index].component;
        if (component == nullptr) {
            continue;
        }

        const int preferred_main = max_int(0, this->items[index].basis_main);
        const int preferred_cross = max_int(0, this->items[index].basis_cross);

        int component_main = preferred_main;
        if (this->items[index].flex_grow > 0 && remaining_grow > 0) {
            int extra = (remaining_extra * this->items[index].flex_grow) / remaining_grow;
            component_main += extra;
            remaining_extra -= extra;
            remaining_grow -= this->items[index].flex_grow;
        }
        if (remaining_overflow > 0 && remaining_shrinkable > 0) {
            int shrink = (remaining_overflow * preferred_main) / remaining_shrinkable;
            if (shrink > component_main) {
                shrink = component_main;
            }
            component_main -= shrink;
            remaining_overflow -= shrink;
            remaining_shrinkable -= preferred_main;
        } else {
            remaining_shrinkable -= preferred_main;
        }
        component_main = clamp_non_negative(component_main);

        int component_cross = preferred_cross;
        if (this->align_items == AlignStretch || component_cross <= 0) {
            component_cross = available_cross;
        } else {
            component_cross = min_int(component_cross, available_cross);
        }

        int cross_offset = 0;
        if (this->align_items == AlignCenter) {
            cross_offset = (available_cross - component_cross) / 2;
        } else if (this->align_items == AlignEnd) {
            cross_offset = available_cross - component_cross;
        }
        cross_offset = clamp_non_negative(cross_offset);

        if (this->direction == Row) {
            component->set_bounds(base_x + cursor, base_y + cross_offset, component_main, component_cross);
        } else {
            component->set_bounds(base_x + cross_offset, base_y + cursor, component_cross, component_main);
        }

        cursor += component_main + dynamic_gap;
    }

    this->layout_dirty = false;
}
