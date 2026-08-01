#include "../../platforms/platform.hpp"
#include "../../includes/file.hpp"
#include "../../includes/text/text_renderer.hpp"
#include "../../includes/vector.hpp"

#include <SDL.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>

namespace stardustui {

// File platform adapter used by stardustui::File.

bool file_exists_platform(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }

    fclose(file);
    return true;
}

bool file_remove_platform(const char* path)
{
    return ::remove(path) == 0;
}

bool file_read_bytes_platform(const char* path, File::byte*& out_data, int& out_size)
{
    out_data = nullptr;
    out_size = 0;

    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    long length = ftell(file);
    if (length < 0 || length > 0x7fffffffL) {
        fclose(file);
        return false;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    if (length == 0) {
        fclose(file);
        return true;
    }

    out_data = new File::byte[(int)length];
    if (out_data == nullptr) {
        fclose(file);
        return false;
    }

    const unsigned int read_count = fread(out_data, 1, (unsigned int)length, file);
    fclose(file);

    if (read_count != (unsigned int)length) {
        delete[] out_data;
        out_data = nullptr;
        return false;
    }

    out_size = (int)length;
    return true;
}

bool file_write_bytes_platform(const char* path, const File::byte* data, int size)
{
    FILE* file = fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }

    if (size == 0) {
        fclose(file);
        return true;
    }

    const unsigned int written = fwrite(data, 1, (unsigned int)size, file);
    fclose(file);
    return written == (unsigned int)size;
}

bool file_append_text_platform(const char* path, const char* text, int length)
{
    FILE* file = fopen(path, "ab");
    if (file == nullptr) {
        return false;
    }

    const unsigned int written = fwrite(text, 1, (unsigned int)length, file);
    fclose(file);
    return written == (unsigned int)length;
}

namespace {
int count_text_length_linux(const char* text)
{
    if (text == nullptr) {
        return 0;
    }

    int length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

bool send_all_linux_fd(int fd, const unsigned char* data, int size)
{
    int sent_total = 0;
    while (sent_total < size) {
        const int sent_now = static_cast<int>(::send(fd,
                                                     reinterpret_cast<const char*>(data + sent_total),
                                                     static_cast<size_t>(size - sent_total),
                                                     0));
        if (sent_now <= 0) {
            return false;
        }
        sent_total += sent_now;
    }
    return true;
}

bool append_linux_bytes(stardustui::vector<unsigned char>& target, const unsigned char* data, int size)
{
    if (size <= 0) {
        return true;
    }
    if (data == nullptr) {
        return false;
    }
    if (!target.reserve(target.size() + size)) {
        return false;
    }
    for (int index = 0; index < size; ++index) {
        if (!target.push_back(data[index])) {
            return false;
        }
    }
    return true;
}

bool read_all_linux_fd(int fd, stardustui::vector<unsigned char>& out_response)
{
    out_response.clear();
    unsigned char buffer[4096];
    while (true) {
        const int received = static_cast<int>(::recv(fd, reinterpret_cast<char*>(buffer), sizeof(buffer), 0));
        if (received < 0) {
            return false;
        }
        if (received == 0) {
            return true;
        }
        if (!append_linux_bytes(out_response, buffer, received)) {
            return false;
        }
    }
}

bool build_http_request_linux(const HttpRequest& request, stardustui::string& out_request)
{
    out_request.assign("");
    const char* method = request.method.length() > 0 ? request.method.c_str() : "GET";
    const char* path = request.path.length() > 0 ? request.path.c_str() : "/";
    const char* host = request.host.c_str();
    const char* content_type = request.content_type.length() > 0 ? request.content_type.c_str() : "text/plain";
    const char* extra_headers = request.extra_headers.c_str();
    const char* body = request.body.c_str();
    const int body_length = request.body.length();

    out_request.append(method);
    out_request.append(" ");
    out_request.append(path);
    out_request.append(" HTTP/1.1\r\nHost: ");
    out_request.append(host);
    out_request.append("\r\nUser-Agent: StardustUI/1.0\r\nAccept: */*\r\nConnection: close\r\n");

    if (extra_headers != nullptr && extra_headers[0] != '\0') {
        out_request.append(extra_headers);
        const int header_length = count_text_length_linux(extra_headers);
        if (header_length < 2 ||
            extra_headers[header_length - 2] != '\r' ||
            extra_headers[header_length - 1] != '\n') {
            out_request.append("\r\n");
        }
    }

    if (body_length > 0) {
        char content_length_buffer[32];
        std::snprintf(content_length_buffer, sizeof(content_length_buffer), "%d", body_length);
        out_request.append("Content-Type: ");
        out_request.append(content_type);
        out_request.append("\r\nContent-Length: ");
        out_request.append(content_length_buffer);
        out_request.append("\r\n");
    }

    out_request.append("\r\n");
    if (body_length > 0) {
        out_request.append(body);
    }
    return true;
}

bool set_non_blocking_linux_fd(int fd, bool enabled)
{
    if (fd < 0) {
        return false;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }

    int next_flags = flags;
    if (enabled) {
        next_flags |= O_NONBLOCK;
    } else {
        next_flags &= ~O_NONBLOCK;
    }

    if (next_flags == flags) {
        return true;
    }

    return ::fcntl(fd, F_SETFL, next_flags) == 0;
}
}

bool socket_connect_platform(const char* host, unsigned short port, long long& out_handle)
{
    out_handle = 0;
    if (host == nullptr || host[0] == '\0' || port == 0) {
        return false;
    }

    char port_buffer[16];
    std::snprintf(port_buffer, sizeof(port_buffer), "%u", static_cast<unsigned int>(port));

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    if (::getaddrinfo(host, port_buffer, &hints, &result) != 0 || result == nullptr) {
        if (result != nullptr) {
            ::freeaddrinfo(result);
        }
        return false;
    }

    bool connected = false;
    for (struct addrinfo* current = result; current != nullptr; current = current->ai_next) {
        if (current->ai_addr == nullptr) {
            continue;
        }

        const int fd = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (::connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
            set_non_blocking_linux_fd(fd, true);
            out_handle = fd;
            connected = true;
            break;
        }

        ::close(fd);
    }

    ::freeaddrinfo(result);
    return connected;
}

bool socket_close_platform(long long handle)
{
    if (handle == 0) {
        return true;
    }
    return ::close(static_cast<int>(handle)) == 0;
}

bool socket_send_platform(long long handle, const unsigned char* data, int size, int& out_sent)
{
    out_sent = 0;
    if (handle == 0 || size < 0 || (size > 0 && data == nullptr)) {
        return false;
    }
    if (size == 0) {
        return true;
    }

    const int sent = static_cast<int>(::send(static_cast<int>(handle),
                                             reinterpret_cast<const char*>(data),
                                             static_cast<size_t>(size),
                                             0));
    if (sent < 0) {
        return false;
    }

    out_sent = sent;
    return true;
}

bool socket_receive_platform(long long handle, unsigned char* buffer, int capacity, int& out_received)
{
    out_received = 0;
    if (handle == 0 || capacity < 0 || (capacity > 0 && buffer == nullptr)) {
        return false;
    }
    if (capacity == 0) {
        return true;
    }

    struct pollfd descriptor{};
    descriptor.fd = static_cast<int>(handle);
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    const int ready = ::poll(&descriptor, 1, 0);
    if (ready < 0) {
        return false;
    }
    if (ready == 0) {
        out_received = 0;
        return true;
    }

    if ((descriptor.revents & POLLNVAL) != 0) {
        return false;
    }

    if ((descriptor.revents & (POLLIN | POLLHUP)) == 0) {
        out_received = 0;
        return true;
    }

    const int received = static_cast<int>(::recv(static_cast<int>(handle),
                                                 reinterpret_cast<char*>(buffer),
                                                 static_cast<size_t>(capacity),
                                                 0));
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            out_received = 0;
            return true;
        }
        return false;
    }

    out_received = received;
    return true;
}

bool http_request_platform(const HttpRequest& request,
                           vector<unsigned char>& out_response,
                           string& out_error)
{
    out_response.clear();
    out_error.assign("");

    stardustui::string request_text;
    if (!build_http_request_linux(request, request_text)) {
        out_error.assign("Failed to build request");
        return false;
    }

    if (!request.use_tls) {
        long long handle = 0;
        if (!socket_connect_platform(request.host.c_str(), request.port, handle)) {
            out_error.assign("connect failed");
            return false;
        }

        const bool sent_ok = send_all_linux_fd(static_cast<int>(handle),
                                               reinterpret_cast<const unsigned char*>(request_text.c_str()),
                                               request_text.length());
        const bool read_ok = sent_ok && read_all_linux_fd(static_cast<int>(handle), out_response);
        socket_close_platform(handle);

        if (!sent_ok) {
            out_error.assign("send failed");
            return false;
        }
        if (!read_ok) {
            out_error.assign("recv failed");
            return false;
        }
        return true;
    }

    stardustui::string command;
    command.append("printf '%s' \"");
    const char* raw = request_text.c_str();
    for (int index = 0; raw[index] != '\0'; ++index) {
        const char ch = raw[index];
        if (ch == '\\') {
            command.append("\\\\");
        } else if (ch == '"') {
            command.append("\\\"");
        } else if (ch == '\r') {
            command.append("\\r");
        } else if (ch == '\n') {
            command.append("\\n");
        } else if (ch == '%') {
            command.append("%%");
        } else {
            char single[2] = {ch, '\0'};
            command.append(single);
        }
    }
    command.append("\" | openssl s_client -quiet -connect ");
    command.append(request.host.c_str());
    command.append(":");
    char port_buffer[16];
    std::snprintf(port_buffer, sizeof(port_buffer), "%u", static_cast<unsigned int>(request.port));
    command.append(port_buffer);
    command.append(" 2>/dev/null");

    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        out_error.assign("openssl s_client failed");
        return false;
    }

    unsigned char buffer[4096];
    while (true) {
        const size_t read_count = std::fread(buffer, 1, sizeof(buffer), pipe);
        if (read_count > 0) {
            if (!append_linux_bytes(out_response, buffer, static_cast<int>(read_count))) {
                ::pclose(pipe);
                out_error.assign("Out of memory");
                return false;
            }
        }
        if (read_count < sizeof(buffer)) {
            if (std::feof(pipe)) {
                break;
            }
            if (std::ferror(pipe)) {
                ::pclose(pipe);
                out_error.assign("tls read failed");
                return false;
            }
        }
    }

    ::pclose(pipe);
    return out_response.size() > 0;
}

bool set_text_font_path(const stardustui::string& path)
{
    return Font::set_default_font_path(path);
}

bool set_text_font_memory(const stardustui::File::byte* data, int size)
{
    return Font::set_default_font_memory(data, size);
}

void clear_text_font()
{
    Font::clear_default_font();
}

}

namespace {
struct DrawCommand {
    enum Type {
        Pixel,
        Rect,
        Text
    };

    Type type;
    int x;
    int y;
    int width;
    int height;
    unsigned int color;
    unsigned int size;
    stardustui::string text;

    DrawCommand() : type(Pixel), x(0), y(0), width(0), height(0), color(0), size(0), text() {}
};

struct WindowState {
    SDL_Window *window;
    SDL_Renderer *renderer;
    Uint32 window_id;
    window_message_proc message_proc;
    stardustui::vector<DrawCommand> commands;

    WindowState()
        : window(nullptr),
          renderer(nullptr),
          window_id(0),
          message_proc(nullptr),
          commands() {}
};

stardustui::vector<WindowState*> g_windows;
char g_last_error[256];
bool g_sdl_ready = false;

int max_int(int a, int b)
{
    return a > b ? a : b;
}

bool string_equals(const char* left, const char* right)
{
    if (left == nullptr || right == nullptr) {
        return left == right;
    }

    int index = 0;
    while (left[index] != '\0' || right[index] != '\0') {
        if (left[index] != right[index]) {
            return false;
        }
        ++index;
    }
    return true;
}

void apply_linux_text_input_environment()
{
    const char* backend = std::getenv("STARDUSTUI_LINUX_BACKEND");
    if (backend != nullptr && backend[0] != '\0') {
        if (string_equals(backend, "wayland")) {
            SDL_setenv("SDL_VIDEODRIVER", "wayland", 1);
        } else if (string_equals(backend, "x11")) {
            SDL_setenv("SDL_VIDEODRIVER", "x11", 1);
        }
    }

    const char* gtk_im_module = std::getenv("STARDUSTUI_GTK_IM_MODULE");
    if (gtk_im_module != nullptr && gtk_im_module[0] != '\0') {
        SDL_setenv("GTK_IM_MODULE", gtk_im_module, 1);
    }
}

void set_last_error(const char *message)
{
    if (message == nullptr) {
        g_last_error[0] = '\0';
        return;
    }

    int index = 0;
    while (message[index] != '\0' && index + 1 < static_cast<int>(sizeof(g_last_error))) {
        g_last_error[index] = message[index];
        ++index;
    }
    g_last_error[index] = '\0';
}

WindowState *to_state(unsigned long long handle)
{
    return reinterpret_cast<WindowState*>(handle);
}

unsigned long long from_state(WindowState *state)
{
    return reinterpret_cast<unsigned long long>(state);
}

bool has_state(WindowState *state)
{
    for (int index = 0; index < g_windows.size(); ++index) {
        if (g_windows[index] == state) {
            return true;
        }
    }

    return false;
}

WindowState *find_state_by_window_id(Uint32 window_id)
{
    for (int index = 0; index < g_windows.size(); ++index) {
        WindowState *state = g_windows[index];
        if (state != nullptr && state->window_id == window_id) {
            return state;
        }
    }

    return nullptr;
}

void remove_state(WindowState *state)
{
    for (int index = 0; index < g_windows.size(); ++index) {
        if (g_windows[index] == state) {
            g_windows[index] = nullptr;
            return;
        }
    }
}

SDL_Color to_sdl_color(unsigned int color)
{
    SDL_Color result{};
    result.r = static_cast<Uint8>((color >> 24) & 0xFF);
    result.g = static_cast<Uint8>((color >> 16) & 0xFF);
    result.b = static_cast<Uint8>((color >> 8) & 0xFF);
    result.a = static_cast<Uint8>(color & 0xFF);
    return result;
}

bool ensure_sdl()
{
    if (!g_sdl_ready) {
        apply_linux_text_input_environment();
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            set_last_error(SDL_GetError());
            return false;
        }
        g_sdl_ready = true;
    }

    SDL_StartTextInput();

    return true;
}

void draw_command(WindowState *state, const DrawCommand& command)
{
    if (state == nullptr || state->renderer == nullptr) {
        return;
    }

    SDL_Color color = to_sdl_color(command.color);

    if (command.type == DrawCommand::Pixel) {
        SDL_SetRenderDrawColor(state->renderer, color.r, color.g, color.b, color.a);
        SDL_RenderDrawPoint(state->renderer, command.x, command.y);
        return;
    }

    if (command.type == DrawCommand::Rect) {
        SDL_SetRenderDrawColor(state->renderer, color.r, color.g, color.b, color.a);
        SDL_Rect rect{};
        rect.x = command.x;
        rect.y = command.y;
        rect.w = command.width;
        rect.h = command.height;
        SDL_RenderFillRect(state->renderer, &rect);
        return;
    }

    stardustui::text::TextBitmap bitmap;
    if (!stardustui::text::rasterize_text(command.text, command.color, command.size == 0 ? 12 : command.size, bitmap)) {
        return;
    }
    if (bitmap.width <= 0 || bitmap.height <= 0) {
        return;
    }

    SDL_Texture* texture = SDL_CreateTexture(state->renderer,
                                             SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STATIC,
                                             bitmap.width,
                                             bitmap.height);
    if (texture == nullptr) {
        return;
    }

    stardustui::vector<unsigned char> upload_pixels;
    const int total_pixels = bitmap.width * bitmap.height;
    if (!upload_pixels.reserve(total_pixels * 4)) {
        SDL_DestroyTexture(texture);
        return;
    }

    for (int index = 0; index < total_pixels; ++index) {
        const unsigned int pixel = bitmap.pixels[index];
        const unsigned char red = static_cast<unsigned char>((pixel >> 24) & 0xFFu);
        const unsigned char green = static_cast<unsigned char>((pixel >> 16) & 0xFFu);
        const unsigned char blue = static_cast<unsigned char>((pixel >> 8) & 0xFFu);
        const unsigned char alpha = static_cast<unsigned char>(pixel & 0xFFu);

        if (!upload_pixels.push_back(red) ||
            !upload_pixels.push_back(green) ||
            !upload_pixels.push_back(blue) ||
            !upload_pixels.push_back(alpha)) {
            SDL_DestroyTexture(texture);
            return;
        }
    }

    if (SDL_UpdateTexture(texture, nullptr, upload_pixels.at(0), bitmap.width * 4) != 0) {
        SDL_DestroyTexture(texture);
        return;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

    SDL_Rect destination_rect{};
    destination_rect.x = command.x;
    destination_rect.y = command.y;
    destination_rect.w = bitmap.width;
    destination_rect.h = bitmap.height;
    SDL_RenderCopy(state->renderer, texture, nullptr, &destination_rect);
    SDL_DestroyTexture(texture);
}

void redraw(WindowState *state)
{
    if (state == nullptr || state->renderer == nullptr) {
        return;
    }

    SDL_SetRenderDrawColor(state->renderer, 255, 255, 255, 255);
    SDL_RenderClear(state->renderer);

    for (int index = 0; index < state->commands.size(); ++index) {
        draw_command(state, state->commands[index]);
    }

    SDL_RenderPresent(state->renderer);
}

void dispatch_mouse_move(WindowState *state, const SDL_MouseMotionEvent& motion)
{
    if (state == nullptr || state->message_proc == nullptr) {
        return;
    }

    state->message_proc(kWindowMessageMove,
                        static_cast<unsigned long long>(motion.x),
                        static_cast<unsigned long long>(motion.y));
}

void dispatch_mouse_button(WindowState *state, bool pressed, const SDL_MouseButtonEvent& button)
{
    if (state == nullptr || state->message_proc == nullptr || button.button != SDL_BUTTON_LEFT) {
        return;
    }

    state->message_proc(pressed ? kWindowMessageLeftButtonDown : kWindowMessageLeftButtonUp,
                        static_cast<unsigned long long>(button.x),
                        static_cast<unsigned long long>(button.y));
}

void apply_window_outer_size(WindowState *state, int outer_width, int outer_height)
{
    if (state == nullptr || state->window == nullptr) {
        return;
    }

    int top = 0;
    int left = 0;
    int bottom = 0;
    int right = 0;
    if (SDL_GetWindowBordersSize(state->window, &top, &left, &bottom, &right) != 0) {
        return;
    }

    const int client_width = max_int(1, outer_width - left - right);
    const int client_height = max_int(1, outer_height - top - bottom);
    SDL_SetWindowSize(state->window, client_width, client_height);
}
}

bool create_window(char *title, int width, int height, bool resizable, unsigned long long *handle)
{
    if (title == nullptr || handle == nullptr || width <= 0 || height <= 0) {
        set_last_error("invalid window title, size, or handle output");
        return false;
    }

    if (!ensure_sdl()) {
        return false;
    }

    WindowState *state = new WindowState();
    if (state == nullptr) {
        set_last_error("failed to allocate window state");
        return false;
    }

    state->window = SDL_CreateWindow(title,
                                     SDL_WINDOWPOS_CENTERED,
                                     SDL_WINDOWPOS_CENTERED,
                                     width,
                                     height,
                                     SDL_WINDOW_SHOWN | (resizable ? SDL_WINDOW_RESIZABLE : 0));
    if (state->window == nullptr) {
        set_last_error(SDL_GetError());
        delete state;
        return false;
    }

    apply_window_outer_size(state, width, height);
    state->renderer = SDL_CreateRenderer(state->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (state->renderer == nullptr) {
        state->renderer = SDL_CreateRenderer(state->window, -1, SDL_RENDERER_SOFTWARE);
    }

    if (state->renderer == nullptr) {
        set_last_error(SDL_GetError());
        SDL_DestroyWindow(state->window);
        delete state;
        return false;
    }

    SDL_SetRenderDrawBlendMode(state->renderer, SDL_BLENDMODE_BLEND);

    state->window_id = SDL_GetWindowID(state->window);
    *handle = from_state(state);
    g_windows.push_back(state);
    set_last_error(nullptr);
    return true;
}

void print_error(const char *message)
{
    std::fprintf(stderr, "Error: %s\n", message == nullptr ? "Unknown error" : message);
    if (g_last_error[0] != '\0') {
        std::fprintf(stderr, "Linux platform detail: %s\n", g_last_error);
    }
}

void log_serial(const char *message)
{
    if (message != nullptr) {
        std::fputs(message, stderr);
    }
}

void append_debug_log(const char *message)
{
    log_serial(message);
}

void refresh_window(unsigned long long handle)
{
    redraw(to_state(handle));
}

void wait_window()
{
    while (!g_windows.empty()) {
        pump_window_events();
        sleep_ms(16);
    }
}

void set_window_message_processor(unsigned long long handle, window_message_proc proc)
{
    WindowState *state = to_state(handle);
    if (state != nullptr) {
        state->message_proc = proc;
    }
}

void pump_window_events()
{
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            for (int index = 0; index < g_windows.size(); ++index) {
                if (g_windows[index] != nullptr) {
                    delete_window(from_state(g_windows[index]));
                }
            }
            break;
        }

        Uint32 window_id = 0;
        if (event.type == SDL_MOUSEMOTION) {
            window_id = event.motion.windowID;
        } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
            window_id = event.button.windowID;
        } else if (event.type == SDL_TEXTINPUT) {
            window_id = event.text.windowID;
        } else if (event.type == SDL_KEYDOWN) {
            window_id = event.key.windowID;
        } else if (event.type == SDL_WINDOWEVENT) {
            window_id = event.window.windowID;
        }

        WindowState *state = find_state_by_window_id(window_id);
        if (state == nullptr) {
            continue;
        }

        if (event.type == SDL_MOUSEMOTION) {
            dispatch_mouse_move(state, event.motion);
        } else if (event.type == SDL_MOUSEBUTTONDOWN) {
            dispatch_mouse_button(state, true, event.button);
        } else if (event.type == SDL_MOUSEBUTTONUP) {
            dispatch_mouse_button(state, false, event.button);
        } else if (event.type == SDL_TEXTINPUT && state->message_proc != nullptr) {
            const char* text = event.text.text;
            for (int index = 0; text[index] != '\0'; ++index) {
                state->message_proc(kWindowMessageChar, 0, static_cast<unsigned long long>(static_cast<unsigned char>(text[index])));
            }
        } else if (event.type == SDL_KEYDOWN && state->message_proc != nullptr) {
            if (event.key.keysym.sym == SDLK_BACKSPACE) {
                state->message_proc(kWindowMessageSpecialChar, 0, static_cast<unsigned long long>('\b'));
            } else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER) {
                state->message_proc(kWindowMessageSpecialChar, 0, static_cast<unsigned long long>('\n'));
            }
        } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            if (state->message_proc != nullptr) {
                state->message_proc(kWindowMessageResize,
                                    static_cast<unsigned long long>(event.window.data1),
                                    static_cast<unsigned long long>(event.window.data2));
            }
            redraw(state);
        } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_EXPOSED) {
            redraw(state);
        } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) {
            delete_window(from_state(state));
        }
    }
}

bool is_window_open(unsigned long long handle)
{
    WindowState *state = to_state(handle);
    return state != nullptr && has_state(state);
}

bool set_window_resizable(unsigned long long handle, bool resizable)
{
    WindowState *state = to_state(handle);
    if (state == nullptr || state->window == nullptr) {
        return false;
    }

    SDL_SetWindowResizable(state->window, resizable ? SDL_TRUE : SDL_FALSE);
    return true;
}

bool delete_window(unsigned long long handle)
{
    WindowState *state = to_state(handle);
    if (state == nullptr || !has_state(state)) {
        return false;
    }

    remove_state(state);

    if (state->renderer != nullptr) {
        SDL_DestroyRenderer(state->renderer);
    }
    if (state->window != nullptr) {
        SDL_DestroyWindow(state->window);
    }

    delete state;

    bool any_window = false;
    for (int index = 0; index < g_windows.size(); ++index) {
        if (g_windows[index] != nullptr) {
            any_window = true;
            break;
        }
    }

    if (!any_window) {
        SDL_StopTextInput();
        if (g_sdl_ready) {
            SDL_Quit();
            g_sdl_ready = false;
        }
    }

    return true;
}

void draw_pixel(unsigned long long handle, int x, int y, unsigned int color)
{
    WindowState *state = to_state(handle);
    if (state == nullptr) {
        return;
    }

    DrawCommand command;
    command.type = DrawCommand::Pixel;
    command.x = x;
    command.y = y;
    command.color = color;
    state->commands.push_back(command);
}

void draw_rect(unsigned long long handle, int x, int y, int width, int height, unsigned int color)
{
    WindowState *state = to_state(handle);
    if (state == nullptr || width <= 0 || height <= 0) {
        return;
    }

    DrawCommand command;
    command.type = DrawCommand::Rect;
    command.x = x;
    command.y = y;
    command.width = width;
    command.height = height;
    command.color = color;
    state->commands.push_back(command);
}

void draw_round_rect(unsigned long long handle,
                     int x,
                     int y,
                     int width,
                     int height,
                     unsigned int radius,
                     unsigned int color)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    int resolved_radius = static_cast<int>(radius);
    const int max_radius_x = width / 2;
    const int max_radius_y = height / 2;
    if (resolved_radius > max_radius_x) {
        resolved_radius = max_radius_x;
    }
    if (resolved_radius > max_radius_y) {
        resolved_radius = max_radius_y;
    }

    if (resolved_radius <= 0) {
        draw_rect(handle, x, y, width, height, color);
        return;
    }

    const unsigned int base_alpha = (color & 0xFFu) == 0u ? 0xFFu : (color & 0xFFu);
    const float radius_value = static_cast<float>(resolved_radius);
    const float radius_squared = radius_value * radius_value;

    for (int row = 0; row < height; ++row) {
        const int mirror_row = row < resolved_radius ? row : (height - 1 - row);
        const bool rounded_row = mirror_row < resolved_radius;
        const int draw_y = y + row;

        if (!rounded_row) {
            draw_rect(handle, x, draw_y, width, 1, color);
            continue;
        }

        if (width > resolved_radius * 2) {
            draw_rect(handle, x + resolved_radius, draw_y, width - resolved_radius * 2, 1, color);
        }

        for (int column = 0; column < resolved_radius; ++column) {
            unsigned int covered_samples = 0;
            for (int sample_y = 0; sample_y < 4; ++sample_y) {
                for (int sample_x = 0; sample_x < 4; ++sample_x) {
                    const float pixel_x = static_cast<float>(column) + (static_cast<float>(sample_x) + 0.5f) * 0.25f;
                    const float pixel_y = static_cast<float>(mirror_row) + (static_cast<float>(sample_y) + 0.5f) * 0.25f;
                    const float dx = radius_value - pixel_x;
                    const float dy = radius_value - pixel_y;
                    if (dx * dx + dy * dy <= radius_squared) {
                        ++covered_samples;
                    }
                }
            }

            if (covered_samples == 0) {
                continue;
            }

            const unsigned int coverage = (covered_samples * 255u + 8u) / 16u;
            const unsigned int alpha = (base_alpha * coverage + 127u) / 255u;
            const unsigned int pixel_color = (color & 0xFFFFFF00u) | (alpha & 0xFFu);
            const int left_x = x + column;
            const int right_x = x + width - 1 - column;
            draw_rect(handle, left_x, draw_y, 1, 1, pixel_color);
            if (right_x != left_x) {
                draw_rect(handle, right_x, draw_y, 1, 1, pixel_color);
            }
        }
    }
}

void clear_draw_commands(unsigned long long handle)
{
    WindowState *state = to_state(handle);
    if (state != nullptr) {
        state->commands.clear();
    }
}

void draw_text(unsigned long long handle, int x, int y, unsigned int color, unsigned int size, const stardustui::string& text)
{
    WindowState *state = to_state(handle);
    if (state == nullptr) {
        return;
    }

    DrawCommand command;
    command.type = DrawCommand::Text;
    command.x = x;
    command.y = y;
    command.color = color;
    command.size = size;
    command.text = text;
    state->commands.push_back(command);
}

void draw_text_on_solid_background(unsigned long long handle,
                                   int x,
                                   int y,
                                   unsigned int color,
                                   unsigned int size,
                                   unsigned int,
                                   const stardustui::string& text)
{
    draw_text(handle, x, y, color, size, text);
}

unsigned int calc_text_width(const stardustui::string& text, unsigned int size)
{
    unsigned int width = 0;
    unsigned int height = 0;
    if (!stardustui::text::measure_text(text, size == 0 ? 12 : size, width, height)) {
        return static_cast<unsigned int>(text.length() * (size == 0 ? 12 : size));
    }
    return width;
}

unsigned int calc_text_height(const stardustui::string& text, unsigned int size)
{
    unsigned int width = 0;
    unsigned int height = 0;
    if (!stardustui::text::measure_text(text, size == 0 ? 12 : size, width, height)) {
        return size == 0 ? 12U : size;
    }
    return height;
}

void sleep_ms(unsigned long long ms)
{
    SDL_Delay(static_cast<Uint32>(ms));
}
