#include "../../platforms/platform.hpp"
#include "../../includes/file.hpp"
#include "../../includes/text/text_renderer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <windows.h>
#include <windowsx.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace {
void clear_text_bitmap_cache();
void clear_font_handle_cache();
stardustui::string& registered_font_path_storage();
void to_wide(const char *text, wchar_t *buffer, int buffer_size);

bool ensure_winsock_started()
{
    static bool initialized = false;
    static bool ready = false;
    if (initialized) {
        return ready;
    }

    initialized = true;
    WSADATA data{};
    ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    return ready;
}

int count_text_length_windows(const char* text)
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

bool append_windows_bytes(stardustui::vector<unsigned char>& target, const unsigned char* data, int size)
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

bool build_http_request_windows(const stardustui::HttpRequest& request, stardustui::string& out_request)
{
    out_request.assign("");
    const char* method = request.method.length() > 0 ? request.method.c_str() : "GET";
    const char* path = request.path.length() > 0 ? request.path.c_str() : "/";
    const char* host = request.host.c_str();
    const char* body = request.body.c_str();
    const char* content_type = request.content_type.length() > 0 ? request.content_type.c_str() : "text/plain";
    const char* extra_headers = request.extra_headers.c_str();
    const int body_length = count_text_length_windows(body);

    out_request.append(method);
    out_request.append(" ");
    out_request.append(path);
    out_request.append(" HTTP/1.1\r\nHost: ");
    out_request.append(host);
    out_request.append("\r\nConnection: close\r\n");

    if (extra_headers != nullptr && extra_headers[0] != '\0') {
        out_request.append(extra_headers);
        const int header_length = count_text_length_windows(extra_headers);
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

bool read_all_windows_socket(SOCKET socket_handle, stardustui::vector<unsigned char>& out_response)
{
    unsigned char buffer[4096];
    while (true) {
        const int received = recv(socket_handle, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (received > 0) {
            if (!append_windows_bytes(out_response, buffer, received)) {
                return false;
            }
            continue;
        }
        if (received == 0) {
            return true;
        }
        return false;
    }
}
}

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

bool socket_connect_platform(const char* host, unsigned short port, long long& out_handle)
{
    out_handle = 0;
    if (!ensure_winsock_started() || host == nullptr || host[0] == '\0' || port == 0) {
        return false;
    }

    char port_buffer[16];
    std::snprintf(port_buffer, sizeof(port_buffer), "%u", static_cast<unsigned int>(port));

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    if (getaddrinfo(host, port_buffer, &hints, &result) != 0 || result == nullptr) {
        if (result != nullptr) {
            freeaddrinfo(result);
        }
        return false;
    }

    bool connected = false;
    for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
        if (current->ai_addr == nullptr) {
            continue;
        }

        SOCKET socket_handle = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (socket_handle == INVALID_SOCKET) {
            continue;
        }

        if (connect(socket_handle, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0) {
            u_long non_blocking = 1;
            ioctlsocket(socket_handle, FIONBIO, &non_blocking);
            out_handle = static_cast<long long>(socket_handle);
            connected = true;
            break;
        }

        closesocket(socket_handle);
    }

    freeaddrinfo(result);
    return connected;
}

bool socket_close_platform(long long handle)
{
    if (handle == 0) {
        return true;
    }
    return closesocket(static_cast<SOCKET>(handle)) == 0;
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

    const int sent = send(static_cast<SOCKET>(handle),
                          reinterpret_cast<const char*>(data),
                          size,
                          0);
    if (sent == SOCKET_ERROR) {
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

    WSAPOLLFD descriptor{};
    descriptor.fd = static_cast<SOCKET>(handle);
    descriptor.events = POLLRDNORM;
    descriptor.revents = 0;
    const int ready = WSAPoll(&descriptor, 1, 0);
    if (ready == SOCKET_ERROR) {
        return false;
    }
    if (ready == 0) {
        return true;
    }

    if ((descriptor.revents & POLLNVAL) != 0) {
        return false;
    }
    if ((descriptor.revents & (POLLRDNORM | POLLHUP)) == 0) {
        return true;
    }

    const int received = recv(static_cast<SOCKET>(handle),
                              reinterpret_cast<char*>(buffer),
                              capacity,
                              0);
    if (received == SOCKET_ERROR) {
        const int error_code = WSAGetLastError();
        if (error_code == WSAEWOULDBLOCK) {
            return true;
        }
        return false;
    }

    out_received = received;
    return true;
}

bool http_request_platform(const stardustui::HttpRequest& request,
                           stardustui::vector<unsigned char>& out_response,
                           stardustui::string& out_error)
{
    out_response.clear();
    out_error.assign("");

    if (request.use_tls) {
        out_error.assign("HTTPS is not implemented on Windows backend yet");
        return false;
    }

    stardustui::string request_text;
    if (!build_http_request_windows(request, request_text)) {
        out_error.assign("Failed to build request");
        return false;
    }

    long long handle = 0;
    if (!socket_connect_platform(request.host.c_str(), request.port, handle)) {
        out_error.assign("connect failed");
        return false;
    }

    SOCKET socket_handle = static_cast<SOCKET>(handle);
    u_long blocking = 0;
    ioctlsocket(socket_handle, FIONBIO, &blocking);

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

    const bool read_ok = read_all_windows_socket(socket_handle, out_response);
    socket_close_platform(handle);
    if (!read_ok) {
        out_error.assign("recv failed");
        return false;
    }

    return true;
}

bool set_text_font_path(const stardustui::string& path)
{
    clear_text_bitmap_cache();
    clear_font_handle_cache();
    const stardustui::string resolved = Font::resolve_font_path(path);
    if (resolved.length() > 0 && !registered_font_path_storage().equals(resolved.c_str())) {
        wchar_t wide_path[1024];
        to_wide(resolved.c_str(), wide_path, static_cast<int>(sizeof(wide_path) / sizeof(wide_path[0])));
        RemoveFontResourceExW(wide_path, FR_PRIVATE, nullptr);
        AddFontResourceExW(wide_path, FR_PRIVATE, nullptr);
        registered_font_path_storage() = resolved;
    }
    return Font::set_default_font_path(path);
}

bool set_text_font_memory(const stardustui::File::byte* data, int size)
{
    clear_text_bitmap_cache();
    clear_font_handle_cache();
    return Font::set_default_font_memory(data, size);
}

void clear_text_font()
{
    clear_text_bitmap_cache();
    clear_font_handle_cache();
    if (registered_font_path_storage().length() > 0) {
        wchar_t wide_path[1024];
        to_wide(registered_font_path_storage().c_str(), wide_path, static_cast<int>(sizeof(wide_path) / sizeof(wide_path[0])));
        RemoveFontResourceExW(wide_path, FR_PRIVATE, nullptr);
        registered_font_path_storage().assign("");
    }
    Font::clear_default_font();
}

}

namespace {
const wchar_t kWindowClassName[] = L"StardustUIWindow";

enum DrawCommandType {
    DrawCommandPixel,
    DrawCommandRect,
    DrawCommandRoundRect,
    DrawCommandText
};

struct DrawCommand {
    DrawCommandType type;
    int x;
    int y;
    int width;
    int height;
    unsigned int radius;
    unsigned int color;
    unsigned int size;
    stardustui::string text;

    DrawCommand() : type(DrawCommandPixel), x(0), y(0), width(0), height(0), radius(0), color(0), size(0), text() {}
};

struct WindowState {
    HWND handle;
    window_message_proc message_proc;
    stardustui::vector<DrawCommand> commands;
    wchar_t pending_high_surrogate;

    WindowState() : handle(nullptr), message_proc(nullptr), commands(), pending_high_surrogate(0) {}
};

stardustui::vector<WindowState*> g_windows;

struct CachedTextBitmap {
    stardustui::string text;
    unsigned int color;
    unsigned int size;
    int width;
    int height;
    HBITMAP dib;

    CachedTextBitmap() : text(), color(0), size(0), width(0), height(0), dib(nullptr) {}
};

unsigned int premultiply_channel(unsigned int channel, unsigned int alpha);

struct CachedFontHandle {
    unsigned int size;
    HFONT font;

    CachedFontHandle() : size(0), font(nullptr) {}
};

stardustui::string& registered_font_path_storage()
{
    static stardustui::string* path = nullptr;
    if (path == nullptr) {
        path = new stardustui::string();
    }
    return *path;
}

stardustui::vector<CachedTextBitmap>& text_bitmap_cache()
{
    static stardustui::vector<CachedTextBitmap>* cache = nullptr;
    if (cache == nullptr) {
        cache = new stardustui::vector<CachedTextBitmap>();
    }
    return *cache;
}

void clear_text_bitmap_cache()
{
    stardustui::vector<CachedTextBitmap>& cache = text_bitmap_cache();
    for (int index = 0; index < cache.size(); ++index) {
        if (cache[index].dib != nullptr) {
            DeleteObject(cache[index].dib);
            cache[index].dib = nullptr;
        }
    }
    cache.release_storage();
}

stardustui::vector<CachedFontHandle>& font_handle_cache()
{
    static stardustui::vector<CachedFontHandle>* cache = nullptr;
    if (cache == nullptr) {
        cache = new stardustui::vector<CachedFontHandle>();
    }
    return *cache;
}

void clear_font_handle_cache()
{
    stardustui::vector<CachedFontHandle>& cache = font_handle_cache();
    for (int index = 0; index < cache.size(); ++index) {
        if (cache[index].font != nullptr) {
            DeleteObject(cache[index].font);
            cache[index].font = nullptr;
        }
    }
    cache.release_storage();
}

bool should_cache_text_bitmap(const stardustui::string& text, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (text.length() > 96) {
        return false;
    }
    if (width * height > 16384) {
        return false;
    }
    return true;
}

unsigned int text_bitmap_memory_bytes()
{
    stardustui::vector<CachedTextBitmap>& cache = text_bitmap_cache();
    unsigned int total = 0;
    for (int index = 0; index < cache.size(); ++index) {
        total += static_cast<unsigned int>(cache[index].width * cache[index].height) * static_cast<unsigned int>(sizeof(unsigned int));
    }
    return total;
}

CachedTextBitmap* find_cached_text_bitmap(const stardustui::string& text, unsigned int color, unsigned int size)
{
    stardustui::vector<CachedTextBitmap>& cache = text_bitmap_cache();
    for (int index = 0; index < cache.size(); ++index) {
        CachedTextBitmap& entry = cache[index];
        if (entry.color == color && entry.size == size && entry.text.equals(text.c_str())) {
            return &entry;
        }
    }
    return nullptr;
}

CachedTextBitmap* cache_text_bitmap(const stardustui::string& text,
                                    unsigned int color,
                                    unsigned int size,
                                    HDC device_context,
                                    const stardustui::text::TextBitmap& bitmap)
{
    if (device_context == nullptr || !should_cache_text_bitmap(text, bitmap.width, bitmap.height)) {
        return nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmap.width;
    info.bmiHeader.biHeight = -bitmap.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* dib_bits = nullptr;
    HBITMAP dib = CreateDIBSection(device_context, &info, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
    if (dib == nullptr || dib_bits == nullptr) {
        if (dib != nullptr) {
            DeleteObject(dib);
        }
        return nullptr;
    }

    unsigned int* destination = static_cast<unsigned int*>(dib_bits);
    for (int y = 0; y < bitmap.height; ++y) {
        for (int x = 0; x < bitmap.width; ++x) {
            const unsigned int pixel = bitmap.pixels[y * bitmap.width + x];
            const unsigned int red = (pixel >> 24) & 0xFFu;
            const unsigned int green = (pixel >> 16) & 0xFFu;
            const unsigned int blue = (pixel >> 8) & 0xFFu;
            const unsigned int alpha = pixel & 0xFFu;
            destination[y * bitmap.width + x] = (alpha << 24) |
                                                (premultiply_channel(red, alpha) << 16) |
                                                (premultiply_channel(green, alpha) << 8) |
                                                premultiply_channel(blue, alpha);
        }
    }

    CachedTextBitmap entry;
    entry.text = text;
    entry.color = color;
    entry.size = size;
    entry.width = bitmap.width;
    entry.height = bitmap.height;
    entry.dib = dib;

    stardustui::vector<CachedTextBitmap>& cache = text_bitmap_cache();
    static const unsigned int kCacheLimitBytes = 8u * 1024u * 1024u;
    if (cache.size() >= 32 || text_bitmap_memory_bytes() >= kCacheLimitBytes) {
        clear_text_bitmap_cache();
    }
    if (!cache.push_back(entry)) {
        DeleteObject(dib);
        return nullptr;
    }
    return &cache[cache.size() - 1];
}

HWND to_hwnd(unsigned long long handle)
{
    return reinterpret_cast<HWND>(handle);
}

unsigned long long from_hwnd(HWND handle)
{
    return reinterpret_cast<unsigned long long>(handle);
}

HINSTANCE get_instance()
{
    return GetModuleHandleW(nullptr);
}

void to_wide(const char *text, wchar_t *buffer, int buffer_size)
{
    if (buffer == nullptr || buffer_size <= 0) {
        return;
    }

    buffer[0] = L'\0';
    if (text == nullptr) {
        return;
    }

    int written = MultiByteToWideChar(CP_UTF8, 0, text, -1, buffer, buffer_size);
    if (written > 0) {
        return;
    }

    written = MultiByteToWideChar(CP_ACP, 0, text, -1, buffer, buffer_size);
    if (written > 0) {
        return;
    }

    int index = 0;
    while (text[index] != '\0' && index + 1 < buffer_size) {
        buffer[index] = static_cast<unsigned char>(text[index]);
        ++index;
    }
    buffer[index] = L'\0';
}

int utf8_to_wide_chars(const char* text, wchar_t* buffer, int buffer_size)
{
    if (buffer == nullptr || buffer_size <= 0) {
        return 0;
    }

    buffer[0] = L'\0';
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }

    int written = MultiByteToWideChar(CP_UTF8, 0, text, -1, buffer, buffer_size);
    if (written > 0) {
        return written - 1;
    }

    written = MultiByteToWideChar(CP_ACP, 0, text, -1, buffer, buffer_size);
    if (written > 0) {
        return written - 1;
    }

    int index = 0;
    while (text[index] != '\0' && index + 1 < buffer_size) {
        buffer[index] = static_cast<unsigned char>(text[index]);
        ++index;
    }
    buffer[index] = L'\0';
    return index;
}

HFONT get_cached_font(unsigned int size)
{
    const unsigned int resolved_size = size == 0 ? 12u : size;
    stardustui::vector<CachedFontHandle>& cache = font_handle_cache();
    for (int index = 0; index < cache.size(); ++index) {
        if (cache[index].size == resolved_size && cache[index].font != nullptr) {
            return cache[index].font;
        }
    }

    const int pixel_height = -static_cast<int>(resolved_size);
    LOGFONTW logfont{};
    logfont.lfHeight = pixel_height;
    logfont.lfWeight = FW_NORMAL;
    logfont.lfCharSet = DEFAULT_CHARSET;
    logfont.lfOutPrecision = OUT_DEFAULT_PRECIS;
    logfont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    logfont.lfQuality = CLEARTYPE_QUALITY;
    logfont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;

    const stardustui::string& default_path = Font::default_font_path();
    bool copied_face_name = false;
    if (default_path.length() > 0) {
        wchar_t wide_path[1024];
        to_wide(default_path.c_str(), wide_path, static_cast<int>(sizeof(wide_path) / sizeof(wide_path[0])));
        AddFontResourceExW(wide_path, FR_PRIVATE, nullptr);

        const char* path_text = default_path.c_str();
        const wchar_t* face_name = nullptr;
        if (stardustui::string(path_text).equals("XJ380F") ||
            stardustui::string(path_text).equals("XJ380F.ttf") ||
            stardustui::string(path_text).equals("XJ380F.otf")) {
            face_name = L"XJ380F";
        } else if (stardustui::string(path_text).equals("XJ380C") ||
                   stardustui::string(path_text).equals("XJ380C.ttf") ||
                   stardustui::string(path_text).equals("XJ380C.otf")) {
            face_name = L"XJ380C";
        } else if (strstr(path_text, "xiaolai") != nullptr || strstr(path_text, "XiaoLai") != nullptr) {
            face_name = L"XiaoLai";
        }

        if (face_name != nullptr) {
            for (int index = 0; index < LF_FACESIZE; ++index) {
                logfont.lfFaceName[index] = face_name[index];
                if (face_name[index] == L'\0') {
                    copied_face_name = true;
                    break;
                }
            }
        }
    }

    if (!copied_face_name) {
        const wchar_t fallback_face[] = L"Microsoft YaHei UI";
        for (int index = 0; index < LF_FACESIZE; ++index) {
            logfont.lfFaceName[index] = fallback_face[index];
            if (fallback_face[index] == L'\0') {
                break;
            }
        }
    }

    HFONT font = CreateFontIndirectW(&logfont);
    if (font == nullptr) {
        font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        return font;
    }

    CachedFontHandle entry;
    entry.size = resolved_size;
    entry.font = font;
    if (!cache.push_back(entry)) {
        DeleteObject(font);
        return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    return font;
}

COLORREF to_colorref(unsigned int color)
{
    unsigned int red = (color >> 24) & 0xFF;
    unsigned int green = (color >> 16) & 0xFF;
    unsigned int blue = (color >> 8) & 0xFF;
    return RGB(red, green, blue);
}

unsigned int normalize_alpha(unsigned int alpha)
{
    return alpha == 0u ? 255u : alpha;
}

unsigned int premultiply_channel(unsigned int channel, unsigned int alpha)
{
    return (channel * alpha + 127u) / 255u;
}

unsigned int to_premultiplied_bgra(unsigned int color)
{
    const unsigned int alpha = normalize_alpha(color & 0xFFu);
    const unsigned int red = (color >> 24) & 0xFFu;
    const unsigned int green = (color >> 16) & 0xFFu;
    const unsigned int blue = (color >> 8) & 0xFFu;
    return (alpha << 24) |
           (premultiply_channel(red, alpha) << 16) |
           (premultiply_channel(green, alpha) << 8) |
           premultiply_channel(blue, alpha);
}

int encode_utf8(unsigned int codepoint, char* out_bytes)
{
    if (out_bytes == nullptr) {
        return 0;
    }

    if (codepoint <= 0x7Fu) {
        out_bytes[0] = static_cast<char>(codepoint);
        return 1;
    }
    if (codepoint <= 0x7FFu) {
        out_bytes[0] = static_cast<char>(0xC0u | ((codepoint >> 6) & 0x1Fu));
        out_bytes[1] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        return 2;
    }
    if (codepoint <= 0xFFFFu) {
        out_bytes[0] = static_cast<char>(0xE0u | ((codepoint >> 12) & 0x0Fu));
        out_bytes[1] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
        out_bytes[2] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        return 3;
    }

    out_bytes[0] = static_cast<char>(0xF0u | ((codepoint >> 18) & 0x07u));
    out_bytes[1] = static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu));
    out_bytes[2] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
    out_bytes[3] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
    return 4;
}

void dispatch_utf8_char(WindowState* state, unsigned int codepoint)
{
    if (state == nullptr || state->message_proc == nullptr) {
        return;
    }

    char utf8[4];
    const int count = encode_utf8(codepoint, utf8);
    for (int index = 0; index < count; ++index) {
        state->message_proc(kWindowMessageChar,
                            0,
                            static_cast<unsigned long long>(static_cast<unsigned char>(utf8[index])));
    }
}

WindowState *find_state(HWND handle)
{
    for (int index = 0; index < g_windows.size(); ++index) {
        WindowState *state = g_windows[index];
        if (state != nullptr && state->handle == handle) {
            return state;
        }
    }

    return nullptr;
}

void remove_state(HWND handle)
{
    for (int index = 0; index < g_windows.size(); ++index) {
        WindowState *state = g_windows[index];
        if (state == nullptr || state->handle != handle) {
            continue;
        }

        delete state;
        g_windows[index] = nullptr;
        return;
    }
}

void render_command(HDC device_context, const DrawCommand& command)
{
    if (command.type == DrawCommandPixel) {
        const unsigned int alpha = command.color & 0xFFu;
        if (alpha > 0 && alpha < 255) {
            DrawCommand rect_command = command;
            rect_command.type = DrawCommandRect;
            rect_command.width = 1;
            rect_command.height = 1;
            render_command(device_context, rect_command);
            return;
        }
        SetPixel(device_context, command.x, command.y, to_colorref(command.color));
        return;
    }

    if (command.type == DrawCommandRect) {
        const unsigned int alpha = command.color & 0xFFu;
        if (alpha == 0) {
            return;
        }

        if (alpha < 255) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = 1;
            info.bmiHeader.biHeight = -1;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            void* dib_bits = nullptr;
            HBITMAP dib = CreateDIBSection(device_context, &info, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
            if (dib == nullptr || dib_bits == nullptr) {
                if (dib != nullptr) {
                    DeleteObject(dib);
                }
                return;
            }

            unsigned int* destination = static_cast<unsigned int*>(dib_bits);
            destination[0] = to_premultiplied_bgra(command.color);

            HDC memory_dc = CreateCompatibleDC(device_context);
            if (memory_dc == nullptr) {
                DeleteObject(dib);
                return;
            }

            HGDIOBJ old_bitmap = SelectObject(memory_dc, dib);
            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;
            AlphaBlend(device_context,
                       command.x,
                       command.y,
                       command.width,
                       command.height,
                       memory_dc,
                       0,
                       0,
                       1,
                       1,
                       blend);

            if (old_bitmap != nullptr) {
                SelectObject(memory_dc, old_bitmap);
            }
            DeleteDC(memory_dc);
            DeleteObject(dib);
            return;
        }

        RECT rect{};
        rect.left = command.x;
        rect.top = command.y;
        rect.right = command.x + command.width;
        rect.bottom = command.y + command.height;
        HBRUSH brush = CreateSolidBrush(to_colorref(command.color));
        if (brush != nullptr) {
            FillRect(device_context, &rect, brush);
            DeleteObject(brush);
        }
        return;
    }

    if (command.type == DrawCommandRoundRect) {
        const unsigned int alpha = command.color & 0xFFu;
        if (alpha == 0u) {
            return;
        }

        const int diameter = static_cast<int>(command.radius) * 2;
        HBRUSH brush = CreateSolidBrush(to_colorref(command.color));
        if (brush == nullptr) {
            return;
        }

        if (alpha >= 255u) {
            HGDIOBJ old_brush = SelectObject(device_context, brush);
            HGDIOBJ old_pen = SelectObject(device_context, GetStockObject(NULL_PEN));
            RoundRect(device_context,
                      command.x,
                      command.y,
                      command.x + command.width,
                      command.y + command.height,
                      diameter,
                      diameter);
            if (old_pen != nullptr) {
                SelectObject(device_context, old_pen);
            }
            if (old_brush != nullptr) {
                SelectObject(device_context, old_brush);
            }
            DeleteObject(brush);
            return;
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = command.width;
        info.bmiHeader.biHeight = -command.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* dib_bits = nullptr;
        HBITMAP dib = CreateDIBSection(device_context, &info, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
        if (dib == nullptr || dib_bits == nullptr) {
            DeleteObject(brush);
            if (dib != nullptr) {
                DeleteObject(dib);
            }
            return;
        }

        const unsigned int premultiplied = to_premultiplied_bgra(command.color);
        unsigned int* pixels = static_cast<unsigned int*>(dib_bits);
        const int pixel_count = command.width * command.height;
        for (int index = 0; index < pixel_count; ++index) {
            pixels[index] = 0;
        }

        HDC memory_dc = CreateCompatibleDC(device_context);
        if (memory_dc == nullptr) {
            DeleteObject(brush);
            DeleteObject(dib);
            return;
        }

        HGDIOBJ old_bitmap = SelectObject(memory_dc, dib);
        for (int index = 0; index < pixel_count; ++index) {
            pixels[index] = premultiplied;
        }
        HGDIOBJ old_brush = SelectObject(memory_dc, brush);
        HGDIOBJ old_pen = SelectObject(memory_dc, GetStockObject(NULL_PEN));
        RoundRect(memory_dc, 0, 0, command.width, command.height, diameter, diameter);

        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(device_context,
                   command.x,
                   command.y,
                   command.width,
                   command.height,
                   memory_dc,
                   0,
                   0,
                   command.width,
                   command.height,
                   blend);

        if (old_pen != nullptr) {
            SelectObject(memory_dc, old_pen);
        }
        if (old_brush != nullptr) {
            SelectObject(memory_dc, old_brush);
        }
        if (old_bitmap != nullptr) {
            SelectObject(memory_dc, old_bitmap);
        }
        DeleteDC(memory_dc);
        DeleteObject(brush);
        DeleteObject(dib);
        return;
    }

    wchar_t wide_text[2048];
    const int wide_length = utf8_to_wide_chars(command.text.c_str(),
                                               wide_text,
                                               static_cast<int>(sizeof(wide_text) / sizeof(wide_text[0])));
    if (wide_length <= 0) {
        return;
    }

    HFONT font = get_cached_font(command.size == 0 ? 12 : command.size);
    HGDIOBJ old_font = SelectObject(device_context, font);
    const int previous_mode = SetBkMode(device_context, TRANSPARENT);
    const COLORREF previous_color = SetTextColor(device_context, to_colorref(command.color));

    RECT rect{};
    rect.left = command.x;
    rect.top = command.y;
    rect.right = command.x + 4096;
    rect.bottom = command.y + 4096;
    DrawTextW(device_context,
              wide_text,
              wide_length,
              &rect,
              DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);

    SetTextColor(device_context, previous_color);
    SetBkMode(device_context, previous_mode);
    if (old_font != nullptr) {
        SelectObject(device_context, old_font);
    }
}

void render_all_commands(HDC device_context, WindowState *state, const RECT *paint_rect)
{
    if (device_context == nullptr || state == nullptr) {
        return;
    }

    RECT client_rect{};
    GetClientRect(state->handle, &client_rect);

    const int width = client_rect.right - client_rect.left;
    const int height = client_rect.bottom - client_rect.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    HDC memory_dc = CreateCompatibleDC(device_context);
    if (memory_dc == nullptr) {
        for (int index = 0; index < state->commands.size(); ++index) {
            render_command(device_context, state->commands[index]);
        }
        return;
    }

    HBITMAP bitmap = CreateCompatibleBitmap(device_context, width, height);
    if (bitmap == nullptr) {
        DeleteDC(memory_dc);
        for (int index = 0; index < state->commands.size(); ++index) {
            render_command(device_context, state->commands[index]);
        }
        return;
    }

    HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
    HBRUSH background = CreateSolidBrush(RGB(255, 255, 255));
    if (background != nullptr) {
        FillRect(memory_dc, &client_rect, background);
        DeleteObject(background);
    }

    for (int index = 0; index < state->commands.size(); ++index) {
        render_command(memory_dc, state->commands[index]);
    }

    RECT blit_rect = client_rect;
    if (paint_rect != nullptr) {
        blit_rect = *paint_rect;
    }

    const int blit_width = blit_rect.right - blit_rect.left;
    const int blit_height = blit_rect.bottom - blit_rect.top;
    if (blit_width > 0 && blit_height > 0) {
        BitBlt(device_context,
               blit_rect.left,
               blit_rect.top,
               blit_width,
               blit_height,
               memory_dc,
               blit_rect.left,
               blit_rect.top,
               SRCCOPY);
    }

    if (old_bitmap != nullptr) {
        SelectObject(memory_dc, old_bitmap);
    }
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC device_context = BeginPaint(hwnd, &paint);
        WindowState *state = find_state(hwnd);
        if (state != nullptr) {
            render_all_commands(device_context, state, &paint.rcPaint);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        WindowState *state = find_state(hwnd);
        if (state != nullptr && state->message_proc != nullptr) {
            state->message_proc(
                kWindowMessageMove,
                static_cast<unsigned long long>(GET_X_LPARAM(lparam)),
                static_cast<unsigned long long>(GET_Y_LPARAM(lparam)));
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        WindowState *state = find_state(hwnd);
        if (state != nullptr && state->message_proc != nullptr) {
            SetCapture(hwnd);
            state->message_proc(
                kWindowMessageLeftButtonDown,
                static_cast<unsigned long long>(GET_X_LPARAM(lparam)),
                static_cast<unsigned long long>(GET_Y_LPARAM(lparam)));
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        WindowState *state = find_state(hwnd);
        if (state != nullptr && state->message_proc != nullptr) {
            ReleaseCapture();
            state->message_proc(
                kWindowMessageLeftButtonUp,
                static_cast<unsigned long long>(GET_X_LPARAM(lparam)),
                static_cast<unsigned long long>(GET_Y_LPARAM(lparam)));
        }
        return 0;
    }
    case WM_CHAR: {
        WindowState *state = find_state(hwnd);
        if (state != nullptr && state->message_proc != nullptr) {
            const unsigned int code_unit = static_cast<unsigned int>(wparam);
            if (code_unit == '\b' || code_unit == '\r' || code_unit == '\n') {
                state->pending_high_surrogate = 0;
                state->message_proc(kWindowMessageSpecialChar,
                                    0,
                                    static_cast<unsigned long long>(code_unit == '\r' ? '\n' : code_unit));
            } else if (code_unit >= 0xD800u && code_unit <= 0xDBFFu) {
                state->pending_high_surrogate = static_cast<wchar_t>(code_unit);
            } else if (code_unit >= 0xDC00u && code_unit <= 0xDFFFu) {
                if (state->pending_high_surrogate >= 0xD800 && state->pending_high_surrogate <= 0xDBFF) {
                    const unsigned int high = static_cast<unsigned int>(state->pending_high_surrogate) - 0xD800u;
                    const unsigned int low = code_unit - 0xDC00u;
                    state->pending_high_surrogate = 0;
                    dispatch_utf8_char(state, 0x10000u + ((high << 10) | low));
                }
            } else {
                state->pending_high_surrogate = 0;
                dispatch_utf8_char(state, code_unit);
            }
        }
        return 0;
    }
    case WM_SIZE: {
        WindowState *state = find_state(hwnd);
        if (state != nullptr && state->message_proc != nullptr) {
            state->message_proc(kWindowMessageResize,
                                static_cast<unsigned long long>(LOWORD(lparam)),
                                static_cast<unsigned long long>(HIWORD(lparam)));
        }
        return 0;
    }
    case WM_DESTROY:
        remove_state(hwnd);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

bool register_window_class()
{
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = get_instance();
    window_class.lpszClassName = kWindowClassName;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground = nullptr;

    ATOM result = RegisterClassW(&window_class);
    registered = result != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}
}

bool create_window(char *title, int width, int height, bool resizable, unsigned long long *handle)
{
    if (title == nullptr || handle == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    if (!register_window_class()) {
        return false;
    }

    wchar_t wide_title[512];
    to_wide(title, wide_title, static_cast<int>(sizeof(wide_title) / sizeof(wide_title[0])));

    DWORD style = resizable ? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, style, FALSE);

    HWND native_handle = CreateWindowExW(
        0,
        kWindowClassName,
        wide_title,
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        get_instance(),
        nullptr);

    if (native_handle == nullptr) {
        return false;
    }

    *handle = from_hwnd(native_handle);
    WindowState *state = new WindowState();
    if (state != nullptr) {
        state->handle = native_handle;
        g_windows.push_back(state);
    }
    ShowWindow(native_handle, SW_SHOW);
    UpdateWindow(native_handle);
    return true;
}

void print_error(const char *message)
{
    MessageBoxA(nullptr, message == nullptr ? "Unknown error" : message, "StardustUI", MB_ICONERROR | MB_OK);
}

void log_serial(const char *message)
{
    if (message != nullptr) {
        OutputDebugStringA(message);
    }
}

void append_debug_log(const char *message)
{
    log_serial(message);
}

void refresh_window(unsigned long long handle)
{
    HWND window = to_hwnd(handle);
    if (window != nullptr) {
        InvalidateRect(window, nullptr, FALSE);
        UpdateWindow(window);
    }
}

void set_window_message_processor(unsigned long long handle, window_message_proc proc)
{
    HWND window = to_hwnd(handle);
    WindowState *state = find_state(window);
    if (state != nullptr) {
        state->message_proc = proc;
    }
}

void wait_window()
{
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void pump_window_events()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool is_window_open(unsigned long long handle)
{
    HWND window = to_hwnd(handle);
    return window != nullptr && IsWindow(window) != 0;
}

bool delete_window(unsigned long long handle)
{
    HWND window = to_hwnd(handle);
    if (window == nullptr || !IsWindow(window)) {
        return false;
    }

    return DestroyWindow(window) != 0;
}

bool set_window_resizable(unsigned long long handle, bool resizable)
{
    HWND window = to_hwnd(handle);
    if (window == nullptr || !IsWindow(window)) {
        return false;
    }

    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (resizable) {
        style |= WS_THICKFRAME;
        style |= WS_MAXIMIZEBOX;
    } else {
        style &= ~static_cast<LONG_PTR>(WS_THICKFRAME);
        style &= ~static_cast<LONG_PTR>(WS_MAXIMIZEBOX);
    }

    SetWindowLongPtrW(window, GWL_STYLE, style);
    SetWindowPos(window,
                 nullptr,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    return true;
}

void draw_pixel(unsigned long long handle, int x, int y, unsigned int color)
{
    HWND window = to_hwnd(handle);
    if (window == nullptr) {
        return;
    }

    DrawCommand command;
    command.type = DrawCommandPixel;
    command.x = x;
    command.y = y;
    command.color = color;
    WindowState *state = find_state(window);
    if (state != nullptr) {
        state->commands.push_back(command);
    }
}

void draw_rect(unsigned long long handle, int x, int y, int width, int height, unsigned int color)
{
    HWND window = to_hwnd(handle);
    if (window == nullptr || width <= 0 || height <= 0) {
        return;
    }

    DrawCommand command;
    command.type = DrawCommandRect;
    command.x = x;
    command.y = y;
    command.width = width;
    command.height = height;
    command.color = color;

    WindowState *state = find_state(window);
    if (state != nullptr) {
        state->commands.push_back(command);
    }
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

    HWND window = to_hwnd(handle);
    if (window == nullptr) {
        return;
    }

    DrawCommand command;
    command.type = DrawCommandRoundRect;
    command.x = x;
    command.y = y;
    command.width = width;
    command.height = height;
    command.radius = static_cast<unsigned int>(resolved_radius);
    command.color = color;

    WindowState *state = find_state(window);
    if (state != nullptr) {
        state->commands.push_back(command);
    }
}

void clear_draw_commands(unsigned long long handle)
{
    HWND window = to_hwnd(handle);
    WindowState *state = find_state(window);
    if (state != nullptr) {
        state->commands.clear();
    }
}

void draw_text(unsigned long long handle, int x, int y, unsigned int color, unsigned int size, const stardustui::string& text)
{
    HWND window = to_hwnd(handle);
    if (window == nullptr) {
        return;
    }

    DrawCommand command;
    command.type = DrawCommandText;
    command.x = x;
    command.y = y;
    command.color = color;
    command.size = size;
    command.text = text;
    WindowState *state = find_state(window);
    if (state != nullptr) {
        state->commands.push_back(command);
    }
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
    HDC device_context = GetDC(nullptr);
    if (device_context == nullptr) {
        return static_cast<unsigned int>(text.length() * (size == 0 ? 12 : size));
    }
    wchar_t wide_text[2048];
    const int wide_length = utf8_to_wide_chars(text.c_str(),
                                               wide_text,
                                               static_cast<int>(sizeof(wide_text) / sizeof(wide_text[0])));
    HFONT font = get_cached_font(size == 0 ? 12 : size);
    HGDIOBJ old_font = SelectObject(device_context, font);
    RECT rect{};
    rect.left = 0;
    rect.top = 0;
    rect.right = 4096;
    rect.bottom = 4096;
    DrawTextW(device_context, wide_text, wide_length, &rect, DT_CALCRECT | DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);
    if (old_font != nullptr) {
        SelectObject(device_context, old_font);
    }
    ReleaseDC(nullptr, device_context);
    return static_cast<unsigned int>(rect.right - rect.left);
}

unsigned int calc_text_height(const stardustui::string& text, unsigned int size)
{
    HDC device_context = GetDC(nullptr);
    if (device_context == nullptr) {
        return size == 0 ? 12U : size;
    }
    wchar_t wide_text[2048];
    const int wide_length = utf8_to_wide_chars(text.c_str(),
                                               wide_text,
                                               static_cast<int>(sizeof(wide_text) / sizeof(wide_text[0])));
    HFONT font = get_cached_font(size == 0 ? 12 : size);
    HGDIOBJ old_font = SelectObject(device_context, font);
    RECT rect{};
    rect.left = 0;
    rect.top = 0;
    rect.right = 4096;
    rect.bottom = 4096;
    DrawTextW(device_context, wide_text, wide_length, &rect, DT_CALCRECT | DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);
    if (old_font != nullptr) {
        SelectObject(device_context, old_font);
    }
    ReleaseDC(nullptr, device_context);
    return static_cast<unsigned int>(rect.bottom - rect.top);
}

void sleep_ms(unsigned long long ms)
{
    Sleep(static_cast<DWORD>(ms));
}
