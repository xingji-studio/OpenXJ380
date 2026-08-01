#include "../../includes/components/canvas.hpp"
#include "../../settings.hpp"
#include "../../platforms/platform.hpp"

Canvas::Canvas(int width, int height)
    : commands(),
      refresh_callback(nullptr) {
    this->set_bounds(0, 0, width, height);
}

Canvas::~Canvas() = default;

void Canvas::draw(unsigned long long handle) {
    for (int index = 0; index < this->commands.size(); ++index) {
        const Command& command = this->commands[index];
        if (command.type == Command::Pixel) {
            draw_pixel(handle, static_cast<int>(this->x) + command.x, static_cast<int>(this->y) + command.y, command.color);
            continue;
        }

        draw_rect(handle,
                  static_cast<int>(this->x) + command.x,
                  static_cast<int>(this->y) + command.y,
                  command.width,
                  command.height,
                  command.color);
    }
}

void Canvas::update() {
    base_component::update();
    if (this->refresh_callback == nullptr) {
        return;
    }

    this->clear();
    this->refresh_callback(*this);
    this->request_redraw();
}

bool Canvas::contains(int x, int y) const {
    return base_component::contains(x, y);
}

int Canvas::get_preferred_width() const {
    return this->get_width();
}

int Canvas::get_preferred_height() const {
    return this->get_height();
}

void Canvas::set_bounds(int x, int y, int width, int height) {
    base_component::set_bounds(x, y, width, height);
    this->request_redraw();
}

void Canvas::set_refresh_callback(RefreshCallback callback) {
    this->refresh_callback = callback;
    this->request_redraw();
}

void Canvas::clear() {
    this->commands.clear();
    this->request_redraw();
}

void Canvas::set_pixel(int x, int y, unsigned int color) {
    if (x < 0 || y < 0 || x >= this->get_width() || y >= this->get_height()) {
        return;
    }

    Command command;
    command.type = Command::Pixel;
    command.x = x;
    command.y = y;
    command.color = color;
    this->commands.push_back(command);
    this->request_redraw();
}

void Canvas::fill_rect(int x, int y, int width, int height, unsigned int color) {
    if (width <= 0 || height <= 0) {
        return;
    }

    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x >= this->get_width() || y >= this->get_height()) {
        return;
    }

    if (x + width > this->get_width()) {
        width = this->get_width() - x;
    }
    if (y + height > this->get_height()) {
        height = this->get_height() - y;
    }
    if (width <= 0 || height <= 0) {
        return;
    }

    Command command;
    command.type = Command::Rect;
    command.x = x;
    command.y = y;
    command.width = width;
    command.height = height;
    command.color = color;
    this->commands.push_back(command);
    this->request_redraw();
}
