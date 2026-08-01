#pragma once
#include "../includes/string.hpp"
#include "../includes/file.hpp"
#include "../includes/network.hpp"
#include "../includes/text/font.hpp"

using window_message_proc = void (*)(unsigned long long type, unsigned long long h_data, unsigned long long l_data);

constexpr unsigned long long kWindowMessageMove = 1;
constexpr unsigned long long kWindowMessageLeftButtonDown = 2;
constexpr unsigned long long kWindowMessageLeftButtonUp = 3;
constexpr unsigned long long kWindowMessageLeftButtonClick = 4;
constexpr unsigned long long kWindowMessageChar = 5;
constexpr unsigned long long kWindowMessageSpecialChar = 6;
constexpr unsigned long long kWindowMessageResize = 7;

bool create_window(char *title, int width, int height, bool resizable, unsigned long long *handle);

void print_error(const char *message);

void log_serial(const char *message);

void append_debug_log(const char *message);

void refresh_window(unsigned long long handle);

void wait_window();

void set_window_message_processor(unsigned long long handle, window_message_proc proc);
void pump_window_events();
bool is_window_open(unsigned long long handle);
bool set_window_resizable(unsigned long long handle, bool resizable);

bool delete_window(unsigned long long handle);

void draw_pixel(unsigned long long handle, int x, int y, unsigned int color);
void draw_rect(unsigned long long handle, int x, int y, int width, int height, unsigned int color);
void draw_round_rect(unsigned long long handle,
                     int x,
                     int y,
                     int width,
                     int height,
                     unsigned int radius,
                     unsigned int color);
void clear_draw_commands(unsigned long long handle);

void draw_text(unsigned long long handle, int x, int y, unsigned int color, unsigned int size, const stardustui::string& text);
void draw_text_on_solid_background(unsigned long long handle,
                                   int x,
                                   int y,
                                   unsigned int color,
                                   unsigned int size,
                                   unsigned int background_color,
                                   const stardustui::string& text);

unsigned int calc_text_width(const stardustui::string& text, unsigned int size);
unsigned int calc_text_height(const stardustui::string& text, unsigned int size);

void sleep_ms(unsigned long long ms);

namespace stardustui {

bool set_text_font_path(const stardustui::string& path);
bool set_text_font_memory(const stardustui::File::byte* data, int size);
void clear_text_font();

// File I/O stays namespaced to avoid colliding with the legacy global window API above.
bool file_exists_platform(const char* path);
bool file_remove_platform(const char* path);
bool file_read_bytes_platform(const char* path, stardustui::File::byte*& out_data, int& out_size);
bool file_write_bytes_platform(const char* path, const stardustui::File::byte* data, int size);
bool file_append_text_platform(const char* path, const char* text, int length);

bool socket_connect_platform(const char* host, unsigned short port, long long& out_handle);
bool socket_close_platform(long long handle);
bool socket_send_platform(long long handle, const unsigned char* data, int size, int& out_sent);
bool socket_receive_platform(long long handle, unsigned char* buffer, int capacity, int& out_received);
bool http_request_platform(const HttpRequest& request,
                           vector<unsigned char>& out_response,
                           string& out_error);

}
