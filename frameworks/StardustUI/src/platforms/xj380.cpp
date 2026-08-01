#include "../../platforms/platform.hpp"
#include "../../includes/string.hpp"
#include "../../includes/text/text_renderer.hpp"
#include "../../platforms/xj380/xapi/xguiapi.h"
#include "../../platforms/xj380/xapi/xtuiapi.h"
#include "../../platforms/xj380/xapi/xposix.h"
#include "../../platforms/xj380/xapi/liballoc/alloc.h"

using namespace stardustui;

namespace stardustui {

namespace {
enum Xj380TextMode {
    Xj380TextModeCustomPath,
    Xj380TextModeCustomMemory
};

Xj380TextMode g_xj380_text_mode = Xj380TextModeCustomPath;

bool split_directory_and_file(const char* path, stardustui::string& out_directory, stardustui::string& out_file)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    const char* last_separator = nullptr;
    for (int index = 0; path[index] != '\0'; ++index) {
        if (path[index] == '/' || path[index] == '\\') {
            last_separator = path + index;
        }
    }

    if (last_separator == nullptr) {
        out_directory.assign(".");
        out_file.assign(path);
        return out_file.length() > 0;
    }

    out_directory.assign("");
    for (const char* cursor = path; cursor < last_separator; ++cursor) {
        if (!out_directory.push_char(*cursor)) {
            return false;
        }
    }

    if (out_directory.length() == 0) {
        out_directory.assign("/");
    }

    out_file.assign(last_separator + 1);
    return out_file.length() > 0;
}

bool query_file_length(const char* path, unsigned long long& out_length)
{
    out_length = 0;
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    stardustui::string directory_path;
    stardustui::string file_name;
    if (split_directory_and_file(path, directory_path, file_name)) {
        UINT32 count = 0;
        DirNode nodes[255];
        xapi_SearchFile(directory_path.data(), &count, nodes);
        if (count != 404) {
            for (UINT32 index = 0; index < count && index < 255; ++index) {
                if (stardustui::string(nodes[index].filename).equals(file_name.c_str())) {
                    out_length = nodes[index].length;
                    return true;
                }
            }
        }
    }

    XFILE* file = xapi_OpenFile((char*)path);
    if (file == nullptr) {
        return false;
    }

    out_length = file->length;
    xapi_CloseFile(file);
    return true;
}

int count_text_length_xj380(const char* text)
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

bool append_xj380_bytes(stardustui::vector<unsigned char>& target, const unsigned char* data, int size)
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

bool set_non_blocking_xj380_fd(int fd, bool enabled)
{
    if (fd < 0) {
        return false;
    }

    const int flags = fcntl(fd, F_GETFL, 0);
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

    return fcntl(fd, F_SETFL, static_cast<uint64_t>(next_flags)) == 0;
}

bool build_http_request_xj380(const HttpRequest& request, stardustui::string& out_request)
{
    out_request.assign("");
    const char* method = request.method.length() > 0 ? request.method.c_str() : "GET";
    const char* path = request.path.length() > 0 ? request.path.c_str() : "/";
    const char* host = request.host.c_str();
    const char* body = request.body.c_str();
    const char* content_type = request.content_type.length() > 0 ? request.content_type.c_str() : "text/plain";
    const char* extra_headers = request.extra_headers.c_str();
    const int body_length = count_text_length_xj380(body);

    out_request.append(method);
    out_request.append(" ");
    out_request.append(path);
    out_request.append(" HTTP/1.1\r\nHost: ");
    out_request.append(host);
    out_request.append("\r\nConnection: close\r\n");

    if (extra_headers != nullptr && extra_headers[0] != '\0') {
        out_request.append(extra_headers);
        const int header_length = count_text_length_xj380(extra_headers);
        if (header_length < 2 ||
            extra_headers[header_length - 2] != '\r' ||
            extra_headers[header_length - 1] != '\n') {
            out_request.append("\r\n");
        }
    }

    if (body_length > 0) {
        char content_length_buffer[32];
        snprintf(content_length_buffer, sizeof(content_length_buffer), "%d", body_length);
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
}

// File platform adapter used by stardustui::File.

bool file_exists_platform(const char* path)
{
    unsigned long long length = 0;
    return query_file_length(path, length);
}

bool file_remove_platform(const char* path)
{
    return (long long)xapi_DeleteFile((char*)path) >= 0;
}

bool file_read_bytes_platform(const char* path, File::byte*& out_data, int& out_size)
{
    out_data = nullptr;
    out_size = 0;
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    XFILE* file = xapi_OpenFile((char*)path);
    if (file == nullptr) {
        return false;
    }

    const unsigned long long length = file->length;
    if (length == 0) {
        xapi_CloseFile(file);
        return true;
    }

    if (length > 0x7fffffffULL) {
        xapi_CloseFile(file);
        return false;
    }

    out_data = new File::byte[(int)length];
    if (out_data == nullptr) {
        xapi_CloseFile(file);
        return false;
    }

    const long long read_result = (long long)xapi_ReadFile((char*)path, (char*)out_data, length, 0);
    xapi_CloseFile(file);
    if (read_result < 0 || (unsigned long long)read_result != length) {
        delete[] out_data;
        out_data = nullptr;
        return false;
    }

    out_size = (int)length;
    return true;
}

bool file_write_bytes_platform(const char* path, const File::byte* data, int size)
{
    xapi_DeleteFile((char*)path);
    xapi_CreateFile((char*)path);

    if (size == 0) {
        return true;
    }

    return (long long)xapi_WriteFile((char*)path, (char*)data, (unsigned long long)size, 0) >= 0;
}

bool file_append_text_platform(const char* path, const char* text, int length)
{
    XFILE* file = xapi_OpenFile((char*)path);
    unsigned long long offset = 0;
    if (file != nullptr) {
        offset = file->length;
        xapi_CloseFile(file);
    } else {
        xapi_CreateFile((char*)path);
    }

    return (long long)xapi_WriteFile((char*)path, (char*)text, (unsigned long long)length, offset) >= 0;
}

bool socket_connect_platform(const char* host, unsigned short port, long long& out_handle)
{
    out_handle = 0;
    if (host == nullptr || host[0] == '\0' || port == 0) {
        return false;
    }

    char port_buffer[16];
    snprintf(port_buffer, sizeof(port_buffer), "%u", static_cast<unsigned int>(port));

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    if (getaddrinfo(host, port_buffer, &hints, &result) != 0 || result == nullptr) {
        if (result != nullptr) {
            freeaddrinfo(result);
        }
        return false;
    }

    bool connected = false;
    for (struct addrinfo* current = result; current != nullptr; current = current->ai_next) {
        if (current->ai_addr == nullptr) {
            continue;
        }

        const int fd = socket(current->ai_family, SOCK_STREAM, 0);
        if (fd < 0) {
            continue;
        }

        if (connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
            set_non_blocking_xj380_fd(fd, true);
            out_handle = fd;
            connected = true;
            break;
        }

        close(fd);
    }

    freeaddrinfo(result);
    return connected;
}

bool socket_close_platform(long long handle)
{
    if (handle == 0) {
        return true;
    }
    shutdown(static_cast<int>(handle), SHUT_RDWR);
    return close(static_cast<int>(handle)) == 0;
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

    const int sent = write(static_cast<int>(handle),
                           reinterpret_cast<char*>(const_cast<unsigned char*>(data)),
                           static_cast<uint64_t>(size));
    if (sent <= 0) {
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

    const int ready = poll(&descriptor, 1, 0);
    if (ready < 0) {
        return false;
    }
    if (ready == 0) {
        return true;
    }

    if ((descriptor.revents & POLLNVAL) != 0) {
        return false;
    }
    if ((descriptor.revents & (POLLIN | POLLHUP)) == 0) {
        return true;
    }

    const int received = read(static_cast<int>(handle),
                              reinterpret_cast<char*>(buffer),
                              static_cast<uint64_t>(capacity));
    if (received < 0) {
        return false;
    }

    out_received = received;
    return true;
}

bool http_request_platform(const HttpRequest& request,
                           stardustui::vector<unsigned char>& out_response,
                           stardustui::string& out_error)
{
    out_response.clear();
    out_error.assign("");

    if (request.use_tls) {
        out_error.assign("HTTPS is not implemented on XJ380 backend yet");
        return false;
    }

    stardustui::string request_text;
    if (!build_http_request_xj380(request, request_text)) {
        out_error.assign("Failed to build request");
        return false;
    }

    long long handle = 0;
    if (!socket_connect_platform(request.host.c_str(), request.port, handle)) {
        out_error.assign("connect failed");
        return false;
    }

    int sent_total = 0;
    const unsigned char* send_data = reinterpret_cast<const unsigned char*>(request_text.c_str());
    const int send_size = request_text.length();
    while (sent_total < send_size) {
        int sent_now = 0;
        if (!socket_send_platform(handle, send_data + sent_total, send_size - sent_total, sent_now) || sent_now <= 0) {
            socket_close_platform(handle);
            out_error.assign("send failed");
            return false;
        }
        sent_total += sent_now;
    }

    unsigned char buffer[2048];
    while (true) {
        struct pollfd descriptor{};
        descriptor.fd = static_cast<int>(handle);
        descriptor.events = POLLIN;
        descriptor.revents = 0;

        const int ready = poll(&descriptor, 1, 5000);
        if (ready < 0) {
            socket_close_platform(handle);
            out_error.assign("recv failed");
            return false;
        }
        if (ready == 0) {
            socket_close_platform(handle);
            out_error.assign("recv timeout");
            return false;
        }

        const int received = read(static_cast<int>(handle),
                                  reinterpret_cast<char*>(buffer),
                                  sizeof(buffer));
        if (received < 0) {
            socket_close_platform(handle);
            out_error.assign("recv failed");
            return false;
        }
        if (received == 0) {
            break;
        }
        if (!append_xj380_bytes(out_response, buffer, received)) {
            socket_close_platform(handle);
            out_error.assign("out of memory");
            return false;
        }
    }

    socket_close_platform(handle);
    return true;
}

}

using operator_size_t = decltype(sizeof(0));
namespace {
window_message_proc g_window_message_proc = nullptr;

struct CachedPlatformTextBitmap {
    stardustui::string text;
    unsigned int color;
    unsigned int size;
    int width;
    int height;
    stardustui::vector<XCOLORA> pixels;

    CachedPlatformTextBitmap() : text(), color(0), size(0), width(0), height(0), pixels() {}
};

bool should_cache_platform_text(const stardustui::string& text, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (text.length() > 64) {
        return false;
    }
    if (width * height > 8192) {
        return false;
    }
    return true;
}

stardustui::vector<CachedPlatformTextBitmap>& platform_text_cache()
{
    static stardustui::vector<CachedPlatformTextBitmap>* cache = nullptr;
    if (cache == nullptr) {
        cache = new stardustui::vector<CachedPlatformTextBitmap>();
    }
    return *cache;
}

void clear_platform_text_cache()
{
    platform_text_cache().release_storage();
}

void reset_platform_text_cache()
{
    clear_platform_text_cache();
}

CachedPlatformTextBitmap* find_cached_platform_text(const stardustui::string& text, unsigned int color, unsigned int size)
{
    stardustui::vector<CachedPlatformTextBitmap>& cache = platform_text_cache();
    for (int index = 0; index < cache.size(); ++index) {
        CachedPlatformTextBitmap& entry = cache[index];
        if (entry.color == color && entry.size == size && entry.text.equals(text.c_str())) {
            return &entry;
        }
    }
    return nullptr;
}

CachedPlatformTextBitmap* cache_platform_text(const stardustui::string& text,
                                              unsigned int color,
                                              unsigned int size,
                                              const stardustui::text::TextBitmap& bitmap)
{
    if (!should_cache_platform_text(text, bitmap.width, bitmap.height)) {
        return nullptr;
    }

    CachedPlatformTextBitmap entry;
    entry.text = text;
    entry.color = color;
    entry.size = size;
    entry.width = bitmap.width;
    entry.height = bitmap.height;
    const int pixel_count = bitmap.width * bitmap.height;
    if (pixel_count > 0 && !entry.pixels.reserve(pixel_count)) {
        return nullptr;
    }

    for (int index = 0; index < pixel_count; ++index) {
        XCOLORA pixel{};
        const unsigned int source = bitmap.pixels[index];
        pixel.Red = static_cast<UINT8>((source >> 24) & 0xFFu);
        pixel.Green = static_cast<UINT8>((source >> 16) & 0xFFu);
        pixel.Blue = static_cast<UINT8>((source >> 8) & 0xFFu);
        pixel.Alpha = static_cast<UINT8>(source & 0xFFu);
        if (!entry.pixels.push_back(pixel)) {
            return nullptr;
        }
    }

    stardustui::vector<CachedPlatformTextBitmap>& cache = platform_text_cache();
    if (cache.size() >= 64) {
        clear_platform_text_cache();
    }
    if (!cache.push_back(entry)) {
        return nullptr;
    }
    return &cache[cache.size() - 1];
}

void dispatch_xj380_message(unsigned long long type, unsigned long long h_data, unsigned long long l_data)
{
    if (g_window_message_proc == nullptr) {
        return;
    }

    if (type == MSG_MOVE) {
        g_window_message_proc(kWindowMessageMove, h_data, l_data);
        return;
    }

    if (type == MSG_LBUTTON) {
        g_window_message_proc(kWindowMessageLeftButtonClick, h_data, l_data);
        return;
    }

    if (type == MSG_CHAR) {
        g_window_message_proc(kWindowMessageChar, 0, l_data);
        return;
    }

    if (type == MSG_SPCHAR) {
        g_window_message_proc(kWindowMessageSpecialChar, 0, l_data);
    }
}

unsigned char blend_channel(unsigned char source, unsigned char destination, unsigned int alpha)
{
    const unsigned int inverse_alpha = 255u - alpha;
    return static_cast<unsigned char>((static_cast<unsigned int>(source) * alpha +
                                       static_cast<unsigned int>(destination) * inverse_alpha) / 255u);
}

bool blend_and_write_platform_text(unsigned long long handle,
                                   int x,
                                   int y,
                                   int width,
                                   int height,
                                   const XCOLORA* source_pixels)
{
    if (handle == 0 || source_pixels == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    unsigned long long window_width = 0;
    unsigned long long window_height = 0;
    xapi_GetWindowSize(handle, &window_width, &window_height);

    int content_width = static_cast<int>(window_width);
    int content_height = static_cast<int>(window_height);
    if (content_width <= 0 || content_height <= 0) {
        return false;
    }

    content_width -= 24;
    content_height -= 47;
    if (content_width <= 0 || content_height <= 0) {
        return false;
    }

    int source_x = 0;
    int source_y = 0;
    int clipped_x = x;
    int clipped_y = y;
    int clipped_width = width;
    int clipped_height = height;

    if (clipped_x < 0) {
        source_x = -clipped_x;
        clipped_width += clipped_x;
        clipped_x = 0;
    }
    if (clipped_y < 0) {
        source_y = -clipped_y;
        clipped_height += clipped_y;
        clipped_y = 0;
    }
    if (clipped_x >= content_width || clipped_y >= content_height) {
        return false;
    }
    if (clipped_x + clipped_width > content_width) {
        clipped_width = content_width - clipped_x;
    }
    if (clipped_y + clipped_height > content_height) {
        clipped_height = content_height - clipped_y;
    }
    if (clipped_width <= 0 || clipped_height <= 0) {
        return false;
    }

    const int pixel_count = clipped_width * clipped_height;
    XCOLOR* destination_pixels = new XCOLOR[pixel_count];
    if (destination_pixels == nullptr) {
        return false;
    }

    xapi_ReadBuffer(handle,
                    static_cast<UINT32>(clipped_x),
                    static_cast<UINT32>(clipped_y),
                    static_cast<UINT32>(clipped_width),
                    static_cast<UINT32>(clipped_height),
                    destination_pixels);

    for (int row = 0; row < clipped_height; ++row) {
        for (int column = 0; column < clipped_width; ++column) {
            const int source_index = (source_y + row) * width + (source_x + column);
            const int destination_index = row * clipped_width + column;
            const XCOLORA& source = source_pixels[source_index];
            if (source.Alpha == 0) {
                continue;
            }

            if (source.Alpha == 255) {
                destination_pixels[destination_index].Red = source.Red;
                destination_pixels[destination_index].Green = source.Green;
                destination_pixels[destination_index].Blue = source.Blue;
                continue;
            }

            destination_pixels[destination_index].Red =
                blend_channel(source.Red, destination_pixels[destination_index].Red, source.Alpha);
            destination_pixels[destination_index].Green =
                blend_channel(source.Green, destination_pixels[destination_index].Green, source.Alpha);
            destination_pixels[destination_index].Blue =
                blend_channel(source.Blue, destination_pixels[destination_index].Blue, source.Alpha);
        }
    }

    xapi_WriteBuffer(handle,
                     static_cast<UINT32>(clipped_x),
                     static_cast<UINT32>(clipped_y),
                     static_cast<UINT32>(clipped_width),
                     static_cast<UINT32>(clipped_height),
                     destination_pixels);
    delete[] destination_pixels;
    return true;
}

bool write_platform_text_on_solid_background(unsigned long long handle,
                                             int x,
                                             int y,
                                             int width,
                                             int height,
                                             unsigned int background_color,
                                             const XCOLORA* source_pixels)
{
    if (handle == 0 || source_pixels == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    unsigned long long window_width = 0;
    unsigned long long window_height = 0;
    xapi_GetWindowSize(handle, &window_width, &window_height);

    int content_width = static_cast<int>(window_width) - 24;
    int content_height = static_cast<int>(window_height) - 47;
    if (content_width <= 0 || content_height <= 0) {
        return false;
    }

    int source_x = 0;
    int source_y = 0;
    int clipped_x = x;
    int clipped_y = y;
    int clipped_width = width;
    int clipped_height = height;

    if (clipped_x < 0) {
        source_x = -clipped_x;
        clipped_width += clipped_x;
        clipped_x = 0;
    }
    if (clipped_y < 0) {
        source_y = -clipped_y;
        clipped_height += clipped_y;
        clipped_y = 0;
    }
    if (clipped_x >= content_width || clipped_y >= content_height) {
        return false;
    }
    if (clipped_x + clipped_width > content_width) {
        clipped_width = content_width - clipped_x;
    }
    if (clipped_y + clipped_height > content_height) {
        clipped_height = content_height - clipped_y;
    }
    if (clipped_width <= 0 || clipped_height <= 0) {
        return false;
    }

    const int pixel_count = clipped_width * clipped_height;
    XCOLOR* destination_pixels = new XCOLOR[pixel_count];
    if (destination_pixels == nullptr) {
        return false;
    }

    const unsigned char background_red = static_cast<unsigned char>((background_color >> 24) & 0xFFu);
    const unsigned char background_green = static_cast<unsigned char>((background_color >> 16) & 0xFFu);
    const unsigned char background_blue = static_cast<unsigned char>((background_color >> 8) & 0xFFu);

    for (int row = 0; row < clipped_height; ++row) {
        for (int column = 0; column < clipped_width; ++column) {
            const int source_index = (source_y + row) * width + (source_x + column);
            const int destination_index = row * clipped_width + column;
            const XCOLORA& source = source_pixels[source_index];

            if (source.Alpha == 0) {
                destination_pixels[destination_index].Red = background_red;
                destination_pixels[destination_index].Green = background_green;
                destination_pixels[destination_index].Blue = background_blue;
                continue;
            }

            if (source.Alpha == 255) {
                destination_pixels[destination_index].Red = source.Red;
                destination_pixels[destination_index].Green = source.Green;
                destination_pixels[destination_index].Blue = source.Blue;
                continue;
            }

            destination_pixels[destination_index].Red =
                blend_channel(source.Red, background_red, source.Alpha);
            destination_pixels[destination_index].Green =
                blend_channel(source.Green, background_green, source.Alpha);
            destination_pixels[destination_index].Blue =
                blend_channel(source.Blue, background_blue, source.Alpha);
        }
    }

    xapi_WriteBuffer(handle,
                     static_cast<UINT32>(clipped_x),
                     static_cast<UINT32>(clipped_y),
                     static_cast<UINT32>(clipped_width),
                     static_cast<UINT32>(clipped_height),
                     destination_pixels);
    delete[] destination_pixels;
    return true;
}

}

void *operator new(operator_size_t size)
{
    return malloc(size);
}

void *operator new[](operator_size_t size)
{
    return malloc(size);
}

void operator delete(void *ptr) noexcept
{
    free(ptr);
}

void operator delete[](void *ptr) noexcept
{
    free(ptr);
}

void operator delete(void *ptr, operator_size_t) noexcept
{
    free(ptr);
}

void operator delete[](void *ptr, operator_size_t) noexcept
{
    free(ptr);
}

bool create_window(char *title, int width, int height, bool, unsigned long long *handle)
{
    if (title == nullptr || handle == nullptr || width <= 0 || height <= 0) return false;

    XWINDOW xwin{};
    xwin.width  = width;
    xwin.height = height;
    xwin.title  = title;
    xwin.sets   = XWIN_NORMAL;

    HDLE native_handle{};
    xapi_CreateWindow(&native_handle, &xwin);
    *handle = native_handle;
    return true;
}

void print_error(const char *message)
{
    static char kErrorFormat[] = "Error: %s";
    xapi_Printf(kErrorFormat, message);
}

void log_serial(const char *message)
{
    xapi_OutputSerial((char *)message);
}

void append_debug_log(const char *message)
{
    (void)message;
}

void refresh_window(unsigned long long handle)
{
    xapi_RefreshWindow(handle);
}

void set_window_message_processor(unsigned long long handle, window_message_proc proc)
{
    g_window_message_proc = proc;
    SetMsgPrcor(handle, dispatch_xj380_message);
}

void wait_window()
{
    while (true) {
        __asm__ __volatile__("pause");
    }
}

void pump_window_events()
{
}

bool is_window_open(unsigned long long handle)
{
    return handle != 0;
}

bool set_window_resizable(unsigned long long, bool)
{
    return false;
}

bool delete_window(unsigned long long handle)
{
    if (handle == 0) return false;

    xapi_CloseWindow(handle);
    return true;
}
void draw_pixel(unsigned long long handle, int x, int y, unsigned int color)
{
    xapi_DrawPoint(handle, x, y, color);
}

void draw_rect(unsigned long long handle, int x, int y, int width, int height, unsigned int color)
{
    if (width <= 0 || height <= 0) return;

    xapi_DrawRect(handle,
                  static_cast<UINT32>(x),
                  static_cast<UINT32>(y),
                  static_cast<UINT32>(x + width - 1),
                  static_cast<UINT32>(y + height - 1),
                  color,
                  true);
}

void draw_round_rect(unsigned long long handle,
                     int x,
                     int y,
                     int width,
                     int height,
                     unsigned int radius,
                     unsigned int color)
{
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

    const unsigned int source_red = (color >> 24) & 0xFFu;
    const unsigned int source_green = (color >> 16) & 0xFFu;
    const unsigned int source_blue = (color >> 8) & 0xFFu;
    const unsigned int base_alpha = (color & 0xFFu) == 0u ? 0xFFu : (color & 0xFFu);
    const float radius_value = static_cast<float>(resolved_radius);
    const float radius_squared = radius_value * radius_value;
    XCOLOR* edge_pixels = nullptr;
    if (resolved_radius > 0) {
        edge_pixels = new XCOLOR[resolved_radius];
    }

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

        bool needs_left_blend = false;
        bool needs_right_blend = false;
        int left_run = 0;
        int right_run = 0;
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
            const int left_x = x + column;
            const int right_x = x + width - 1 - column;

            if (alpha >= 255u) {
                draw_rect(handle, left_x, draw_y, 1, 1, color);
                if (right_x != left_x) {
                    draw_rect(handle, right_x, draw_y, 1, 1, color);
                }
                continue;
            }

            if (edge_pixels != nullptr && left_x >= 0 && draw_y >= 0) {
                edge_pixels[column].Red = static_cast<UINT8>(alpha);
                edge_pixels[column].Green = 0;
                edge_pixels[column].Blue = 0;
                needs_left_blend = true;
                left_run = column + 1;
            }

            if (edge_pixels != nullptr && right_x != left_x && right_x >= 0 && draw_y >= 0) {
                edge_pixels[column].Red = static_cast<UINT8>(alpha);
                edge_pixels[column].Green = 0;
                edge_pixels[column].Blue = 0;
                needs_right_blend = true;
                right_run = column + 1;
            }
        }

        if (edge_pixels != nullptr && needs_left_blend && left_run > 0) {
            xapi_ReadBuffer(handle, static_cast<UINT32>(x), static_cast<UINT32>(draw_y), static_cast<UINT32>(left_run), 1, edge_pixels);
            for (int column = 0; column < left_run; ++column) {
                const unsigned int alpha = edge_pixels[column].Red;
                if (alpha == 0u || alpha >= 255u) {
                    continue;
                }
                edge_pixels[column].Red = blend_channel(static_cast<unsigned char>(source_red), edge_pixels[column].Red, alpha);
                edge_pixels[column].Green = blend_channel(static_cast<unsigned char>(source_green), edge_pixels[column].Green, alpha);
                edge_pixels[column].Blue = blend_channel(static_cast<unsigned char>(source_blue), edge_pixels[column].Blue, alpha);
            }
            xapi_WriteBuffer(handle, static_cast<UINT32>(x), static_cast<UINT32>(draw_y), static_cast<UINT32>(left_run), 1, edge_pixels);
        }

        if (edge_pixels != nullptr && needs_right_blend && right_run > 0) {
            const int start_x = x + width - right_run;
            xapi_ReadBuffer(handle, static_cast<UINT32>(start_x), static_cast<UINT32>(draw_y), static_cast<UINT32>(right_run), 1, edge_pixels);
            for (int column = 0; column < right_run; ++column) {
                const unsigned int alpha = edge_pixels[right_run - 1 - column].Red;
                if (alpha == 0u || alpha >= 255u) {
                    continue;
                }
                edge_pixels[column].Red = blend_channel(static_cast<unsigned char>(source_red), edge_pixels[column].Red, alpha);
                edge_pixels[column].Green = blend_channel(static_cast<unsigned char>(source_green), edge_pixels[column].Green, alpha);
                edge_pixels[column].Blue = blend_channel(static_cast<unsigned char>(source_blue), edge_pixels[column].Blue, alpha);
            }
            xapi_WriteBuffer(handle, static_cast<UINT32>(start_x), static_cast<UINT32>(draw_y), static_cast<UINT32>(right_run), 1, edge_pixels);
        }
    }

    delete[] edge_pixels;
}

void clear_draw_commands(unsigned long long)
{
}

void draw_text(unsigned long long handle, int x, int y, unsigned int color, unsigned int size, const stardustui::string& text)
{
    const unsigned int resolved_size = size == 0 ? 12 : size;
    CachedPlatformTextBitmap* cached = find_cached_platform_text(text, color, resolved_size);
    XCOLORA* transient_pixels = nullptr;
    int transient_width = 0;
    int transient_height = 0;
    if (cached == nullptr) {
        stardustui::text::TextBitmap bitmap;
        if (!stardustui::text::rasterize_text(text, color, resolved_size, bitmap)) {
            return;
        }
        if (bitmap.width <= 0 || bitmap.height <= 0) {
            return;
        }

        cached = cache_platform_text(text, color, resolved_size, bitmap);
        if (cached == nullptr) {
            const int pixel_count = bitmap.width * bitmap.height;
            transient_pixels = new XCOLORA[pixel_count];
            if (transient_pixels == nullptr) {
                return;
            }
            transient_width = bitmap.width;
            transient_height = bitmap.height;
            for (int index = 0; index < pixel_count; ++index) {
                const unsigned int source = bitmap.pixels[index];
                transient_pixels[index].Red = static_cast<UINT8>((source >> 24) & 0xFFu);
                transient_pixels[index].Green = static_cast<UINT8>((source >> 16) & 0xFFu);
                transient_pixels[index].Blue = static_cast<UINT8>((source >> 8) & 0xFFu);
                transient_pixels[index].Alpha = static_cast<UINT8>(source & 0xFFu);
            }

            blend_and_write_platform_text(handle, x, y, transient_width, transient_height, transient_pixels);
            delete[] transient_pixels;
            return;
        }
    }

    if (cached->width <= 0 || cached->height <= 0 || cached->pixels.size() <= 0) {
        return;
    }

    blend_and_write_platform_text(handle,
                                  x,
                                  y,
                                  cached->width,
                                  cached->height,
                                  cached->pixels.size() > 0 ? &cached->pixels[0] : nullptr);
}

void draw_text_on_solid_background(unsigned long long handle,
                                   int x,
                                   int y,
                                   unsigned int color,
                                   unsigned int size,
                                   unsigned int background_color,
                                   const stardustui::string& text)
{
    const unsigned int resolved_size = size == 0 ? 12 : size;
    CachedPlatformTextBitmap* cached = find_cached_platform_text(text, color, resolved_size);
    XCOLORA* transient_pixels = nullptr;
    int transient_width = 0;
    int transient_height = 0;
    if (cached == nullptr) {
        stardustui::text::TextBitmap bitmap;
        if (!stardustui::text::rasterize_text(text, color, resolved_size, bitmap)) {
            return;
        }
        if (bitmap.width <= 0 || bitmap.height <= 0) {
            return;
        }

        cached = cache_platform_text(text, color, resolved_size, bitmap);
        if (cached == nullptr) {
            const int pixel_count = bitmap.width * bitmap.height;
            transient_pixels = new XCOLORA[pixel_count];
            if (transient_pixels == nullptr) {
                return;
            }
            transient_width = bitmap.width;
            transient_height = bitmap.height;
            for (int index = 0; index < pixel_count; ++index) {
                const unsigned int source = bitmap.pixels[index];
                transient_pixels[index].Red = static_cast<UINT8>((source >> 24) & 0xFFu);
                transient_pixels[index].Green = static_cast<UINT8>((source >> 16) & 0xFFu);
                transient_pixels[index].Blue = static_cast<UINT8>((source >> 8) & 0xFFu);
                transient_pixels[index].Alpha = static_cast<UINT8>(source & 0xFFu);
            }

            write_platform_text_on_solid_background(handle,
                                                    x,
                                                    y,
                                                    transient_width,
                                                    transient_height,
                                                    background_color,
                                                    transient_pixels);
            delete[] transient_pixels;
            return;
        }
    }

    if (cached->width <= 0 || cached->height <= 0 || cached->pixels.size() <= 0) {
        return;
    }

    write_platform_text_on_solid_background(handle,
                                            x,
                                            y,
                                            cached->width,
                                            cached->height,
                                            background_color,
                                            cached->pixels.size() > 0 ? &cached->pixels[0] : nullptr);
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
    xapi_Sleep(ms);
}

namespace stardustui {

bool set_text_font_path(const stardustui::string& path)
{
    g_xj380_text_mode = Xj380TextModeCustomPath;
    reset_platform_text_cache();
    return Font::set_default_font_path(path);
}

bool set_text_font_memory(const stardustui::File::byte* data, int size)
{
    g_xj380_text_mode = Xj380TextModeCustomMemory;
    reset_platform_text_cache();
    return Font::set_default_font_memory(data, size);
}

void clear_text_font()
{
    g_xj380_text_mode = Xj380TextModeCustomPath;
    reset_platform_text_cache();
    Font::clear_default_font();
}

}
