#include "../../includes/components/base.hpp"
base_component::base_component()
    : mouse_active(false),
      click_active(false),
      hover_active(false),
      focused(false),
      redraw_requested(false),
      click_feedback_frames(0),
      anchors(AnchorLeft | AnchorTop),
      x(0),
      y(0),
      width(0),
      height(0) {}

base_component::~base_component() = default;
void base_component::draw(unsigned long long) {}

void base_component::update() {
    if (this->click_feedback_frames > 0) {
        --this->click_feedback_frames;
        if (this->click_feedback_frames == 0 && this->click_active) {
            this->set_click_state(false);
            this->request_redraw();
        }
    }
}

void base_component::callback(void (*func)()) {
    this->callback_func = func;
}

void base_component::set_style_rules(const SytelRules& rules) {
    this->style_rules = rules;
}

const SytelRules& base_component::get_style_rules() const {
    return this->style_rules;
}

void base_component::clear_style_rules() {
    this->style_rules.clear();
}

void base_component::set_mouse_state(bool active) {
    this->mouse_active = active;
}

void base_component::set_click_state(bool active) {
    this->click_active = active;
}

void base_component::set_hover_state(bool active) {
    this->hover_active = active;
}

bool base_component::is_mouse_active() const {
    return this->mouse_active;
}

bool base_component::is_click_active() const {
    return this->click_active;
}

bool base_component::is_hover_active() const {
    return this->hover_active;
}

Sytel base_component::resolve_style() const {
    return this->style_rules.resolve(this->mouse_active, this->click_active, this->hover_active);
}

int base_component::get_preferred_width() const {
    return static_cast<int>(this->width);
}

int base_component::get_preferred_height() const {
    return static_cast<int>(this->height);
}

bool base_component::contains(int x, int y) const {
    return x >= static_cast<int>(this->x) &&
           y >= static_cast<int>(this->y) &&
           x < static_cast<int>(this->x + this->width) &&
           y < static_cast<int>(this->y + this->height);
}

void base_component::set_bounds(int x, int y, int width, int height) {
    this->x = static_cast<unsigned int>(x < 0 ? 0 : x);
    this->y = static_cast<unsigned int>(y < 0 ? 0 : y);
    this->width = static_cast<unsigned int>(width < 0 ? 0 : width);
    this->height = static_cast<unsigned int>(height < 0 ? 0 : height);
}

bool base_component::handle_pointer_move(int x, int y) {
    const bool hovered = this->contains(x, y);
    const bool changed = this->hover_active != hovered || this->mouse_active != hovered;
    this->set_hover_state(hovered);
    this->set_mouse_state(hovered);
    return changed;
}

bool base_component::handle_left_button(bool pressed, int x, int y) {
    const bool inside = this->contains(x, y);
    if (pressed) {
        this->set_focus(inside);
        this->click_feedback_frames = 0;
        const bool changed = this->click_active != inside;
        this->set_click_state(inside);
        if (inside) {
            this->set_mouse_state(true);
            this->set_hover_state(true);
        }
        return changed;
    }

    const bool was_active = this->click_active;
    if (was_active && inside) {
        this->run_callback();
        this->click_feedback_frames = 1;
        this->request_redraw();
        return true;
    }
    this->set_click_state(false);
    return was_active;
}

bool base_component::handle_char_input(char, bool) {
    return false;
}

bool base_component::set_focus(bool focused) {
    const bool changed = this->focused != focused;
    this->focused = focused;
    return changed;
}

void base_component::on_parent_resize(int old_width, int old_height, int new_width, int new_height) {
    const int delta_width = new_width - old_width;
    const int delta_height = new_height - old_height;

    int next_x = static_cast<int>(this->x);
    int next_y = static_cast<int>(this->y);
    int next_width = static_cast<int>(this->width);
    int next_height = static_cast<int>(this->height);

    if ((this->anchors & AnchorLeft) != 0u && (this->anchors & AnchorRight) != 0u) {
        next_width += delta_width;
    } else if ((this->anchors & AnchorRight) != 0u && (this->anchors & AnchorLeft) == 0u) {
        next_x += delta_width;
    }

    if ((this->anchors & AnchorTop) != 0u && (this->anchors & AnchorBottom) != 0u) {
        next_height += delta_height;
    } else if ((this->anchors & AnchorBottom) != 0u && (this->anchors & AnchorTop) == 0u) {
        next_y += delta_height;
    }

    this->set_bounds(next_x, next_y, next_width, next_height);
    this->request_redraw();
}

bool base_component::has_focus() const {
    return this->focused;
}

int base_component::get_width() const {
    return static_cast<int>(this->width);
}

int base_component::get_height() const {
    return static_cast<int>(this->height);
}

void base_component::set_anchors(unsigned int anchors) {
    this->anchors = anchors;
}

unsigned int base_component::get_anchors() const {
    return this->anchors;
}

void base_component::request_redraw() {
    this->redraw_requested = true;
}

bool base_component::consume_redraw_request() {
    const bool requested = this->redraw_requested;
    this->redraw_requested = false;
    return requested;
}

bool base_component::has_pending_redraw() const {
    return this->redraw_requested;
}

void base_component::run_callback() {
    if (this->callback_func != nullptr) {
        this->callback_func();
    }
}
