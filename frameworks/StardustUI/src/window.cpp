#include "../includes/window.hpp"
namespace {
Window* g_active_window = nullptr;

void dispatch_window_message(unsigned long long type, unsigned long long h_data, unsigned long long l_data) {
	if (g_active_window != nullptr) {
		g_active_window->handle_message(type, h_data, l_data);
	}
}
}

Window::Window(const char* title, int width, int height, bool resizable) {
	this->title.assign(title);
	this->width = width;
	this->height = height;
	this->handle = 0;
	this->resizable = resizable;
	this->needs_redraw = true;
	this->background_needs_clear = true;
}

Window::~Window() {
	append_debug_log("stardustui: Window destructor\n");
	if (g_active_window == this) {
		g_active_window = nullptr;
	}
	if (this->handle != 0) {
		append_debug_log("stardustui: destructor closing handle\n");
		delete_window(this->handle);
	}
}

void Window::show() {
	static constexpr char kCreateWindowFailed[] = "Failed to create window";
	append_debug_log("stardustui: Window::show enter\n");
	if (!create_window(this->title.data(), this->width, this->height, this->resizable, &this->handle)) {
		append_debug_log("stardustui: create_window failed\n");
		error(kCreateWindowFailed);
		return;
	}

	append_debug_log("stardustui: create_window ok\n");
	g_active_window = this;
	this->background_needs_clear = true;
	draw_rect(this->handle, 0, 0, this->width, this->height, 0xFFFFFFFF);
	refresh_window(this->handle);
	append_debug_log("stardustui: set_window_message_processor begin\n");
	set_window_message_processor(this->handle, dispatch_window_message);
	append_debug_log("stardustui: set_window_message_processor ok\n");
	append_debug_log("stardustui: draw_components begin\n");
	draw_components();
	append_debug_log("stardustui: draw_components ok\n");
	append_debug_log("stardustui: refresh_window begin\n");
	refresh_window(this->handle);
	append_debug_log("stardustui: refresh_window ok\n");
	append_debug_log("stardustui: wait_window enter\n");
	while (is_window_open(this->handle)) {
		pump_window_events();
		for (int i = 0; i < components.size(); ++i) {
			components[i]->update();
			if (components[i]->consume_redraw_request()) {
				this->needs_redraw = true;
			}
		}
		if (this->needs_redraw) {
			draw_components();
			refresh_window(this->handle);
			this->needs_redraw = false;
		}
		sleep_ms(16);
	}
}

void Window::hide() {
	if (this->handle != 0) {
		append_debug_log("stardustui: Window::hide closing handle\n");
		delete_window(this->handle);
		this->handle = 0;
	}
	if (g_active_window == this) {
		g_active_window = nullptr;
	}
}

int Window::getWidth() {
	return this->width;
}

int Window::getHeight() {
	return this->height;
}

const char* Window::getTitle() {
	return this->title.c_str();
}

bool Window::isResizable() const {
	return this->resizable;
}

void Window::setResizable(bool resizable) {
	this->resizable = resizable;
	if (this->handle != 0) {
		set_window_resizable(this->handle, resizable);
	}
}

void Window::error(const char* msg) {
	print_error(msg);
}

void Window::handle_message(unsigned long long type, unsigned long long h_data, unsigned long long l_data) {
	if (type == kWindowMessageResize) {
		const int old_width = this->width;
		const int old_height = this->height;
		const int new_width = static_cast<int>(h_data);
		const int new_height = static_cast<int>(l_data);
		if (new_width > 0) {
			this->width = new_width;
		}
		if (new_height > 0) {
			this->height = new_height;
		}
		for (int i = 0; i < this->components.size(); ++i) {
			if (this->components[i] != nullptr) {
				this->components[i]->on_parent_resize(old_width, old_height, this->width, this->height);
			}
		}
		this->background_needs_clear = true;
		this->needs_redraw = true;
		return;
	}

	bool changed = false;
	for (int i = 0; i < this->components.size(); ++i) {
		base_component* component = this->components[i];
		if (component == nullptr) {
			continue;
		}

		if (type == kWindowMessageMove) {
			if (component->handle_pointer_move(static_cast<int>(h_data), static_cast<int>(l_data))) {
				changed = true;
			}
		} else if (type == kWindowMessageLeftButtonDown) {
			if (component->handle_left_button(true, static_cast<int>(h_data), static_cast<int>(l_data))) {
				changed = true;
			}
		} else if (type == kWindowMessageLeftButtonUp) {
			if (component->handle_left_button(false, static_cast<int>(h_data), static_cast<int>(l_data))) {
				changed = true;
			}
		} else if (type == kWindowMessageLeftButtonClick) {
			if (component->handle_left_button(true, static_cast<int>(h_data), static_cast<int>(l_data))) {
				changed = true;
			}
			if (component->handle_left_button(false, static_cast<int>(h_data), static_cast<int>(l_data))) {
				changed = true;
			}
		} else if (type == kWindowMessageChar) {
			if (component->handle_char_input(static_cast<char>(l_data), false)) {
				changed = true;
			}
		} else if (type == kWindowMessageSpecialChar) {
			if (component->handle_char_input(static_cast<char>(l_data), true)) {
				changed = true;
			}
		}
	}

	if (changed) {
		this->needs_redraw = true;
	}
}

void Window::addComponent(base_component& component) {
	addComponent(&component);
}

void Window::addComponent(base_component* component) {
	if (component == nullptr) {
		return;
	}

	if (!this->components.push_back(component)) {
		error("Failed to grow component storage");
		return;
	}

	this->needs_redraw = true;
	this->background_needs_clear = true;
}

void Window::draw_components() {
	clear_draw_commands(this->handle);
#ifdef XJ380
	if (this->background_needs_clear) {
		draw_rect(this->handle, 0, 0, this->width, this->height, 0xFFFFFFFF);
		this->background_needs_clear = false;
	}
#else
	draw_rect(this->handle, 0, 0, this->width, this->height, 0xFFFFFFFF);
#endif
	for (int i = 0; i < this->components.size(); ++i) {
		if (this->components[i] != nullptr) {
			this->components[i]->draw(this->handle);
		}
	}
}
