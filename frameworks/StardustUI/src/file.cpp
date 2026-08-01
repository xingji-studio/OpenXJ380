#include "../includes/file.hpp"
#include "../platforms/platform.hpp"


namespace stardustui {
namespace {
int text_length(const char* text)
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
}

File::File() : file_path() {}

File::File(const char* path) : file_path()
{
    set_path(path);
}

File::File(const string& path) : file_path(path) {}

void File::set_path(const char* path)
{
    file_path.assign(path);
}

void File::set_path(const string& path)
{
    file_path = path;
}

const string& File::path() const
{
    return file_path;
}

bool File::exists() const
{
    return exists(file_path);
}

bool File::remove() const
{
    return remove(file_path);
}

bool File::read_bytes(byte*& out_data, int& out_size) const
{
    return read_bytes(file_path, out_data, out_size);
}

bool File::read_text(string& out) const
{
    return read_text(file_path, out);
}

bool File::write_bytes(const byte* data, int size) const
{
    return write_bytes(file_path, data, size);
}

bool File::write_bytes(const vector<byte>& data) const
{
    return write_bytes(file_path, data);
}

bool File::write_text(const char* text) const
{
    return write_text(file_path, text);
}

bool File::write_text(const string& text) const
{
    return write_text(file_path, text);
}

bool File::append_text(const char* text) const
{
    return append_text(file_path, text);
}

bool File::append_text(const string& text) const
{
    return append_text(file_path, text);
}

void File::free_bytes(byte* data)
{
    delete[] data;
}

bool File::exists(const char* path)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    return stardustui::file_exists_platform(path);
}

bool File::exists(const string& path)
{
    return exists(path.c_str());
}

bool File::remove(const char* path)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    return stardustui::file_remove_platform(path);
}

bool File::remove(const string& path)
{
    return remove(path.c_str());
}

bool File::read_bytes(const char* path, byte*& out_data, int& out_size)
{
    out_data = nullptr;
    out_size = 0;

    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    return stardustui::file_read_bytes_platform(path, out_data, out_size);
}

bool File::read_bytes(const string& path, byte*& out_data, int& out_size)
{
    return read_bytes(path.c_str(), out_data, out_size);
}

bool File::read_text(const char* path, string& out)
{
    byte* bytes = nullptr;
    int size = 0;
    if (!read_bytes(path, bytes, size)) {
        out.assign("");
        return false;
    }

    if (size == 0) {
        out.assign("");
        return true;
    }

    char* text = new char[size + 1];
    if (text == nullptr) {
        free_bytes(bytes);
        out.assign("");
        return false;
    }

    for (int index = 0; index < size; ++index) {
        text[index] = (char)bytes[index];
    }
    text[size] = '\0';

    out.assign(text);
    delete[] text;
    free_bytes(bytes);
    return true;
}

bool File::read_text(const string& path, string& out)
{
    return read_text(path.c_str(), out);
}

bool File::write_bytes(const char* path, const byte* data, int size)
{
    if (path == nullptr || path[0] == '\0' || size < 0) {
        return false;
    }

    if (size > 0 && data == nullptr) {
        return false;
    }

    return stardustui::file_write_bytes_platform(path, data, size);
}

bool File::write_bytes(const string& path, const byte* data, int size)
{
    return write_bytes(path.c_str(), data, size);
}

bool File::write_bytes(const char* path, const vector<byte>& data)
{
    if (data.empty()) {
        return write_bytes(path, nullptr, 0);
    }

    const byte* buffer = data.at(0);
    if (buffer == nullptr) {
        return false;
    }

    return write_bytes(path, buffer, data.size());
}

bool File::write_bytes(const string& path, const vector<byte>& data)
{
    return write_bytes(path.c_str(), data);
}

bool File::write_text(const char* path, const char* text)
{
    if (text == nullptr) {
        return write_bytes(path, nullptr, 0);
    }

    return write_bytes(path, (const byte*)text, text_length(text));
}

bool File::write_text(const string& path, const char* text)
{
    return write_text(path.c_str(), text);
}

bool File::write_text(const char* path, const string& text)
{
    return write_text(path, text.c_str());
}

bool File::write_text(const string& path, const string& text)
{
    return write_text(path.c_str(), text.c_str());
}

bool File::append_text(const char* path, const char* text)
{
    if (path == nullptr || path[0] == '\0' || text == nullptr) {
        return false;
    }

    const int length = text_length(text);
    if (length == 0) {
        return true;
    }

    return stardustui::file_append_text_platform(path, text, length);
}

bool File::append_text(const string& path, const char* text)
{
    return append_text(path.c_str(), text);
}

bool File::append_text(const char* path, const string& text)
{
    return append_text(path, text.c_str());
}

bool File::append_text(const string& path, const string& text)
{
    return append_text(path.c_str(), text.c_str());
}

namespace file {

bool exists(const char* path)
{
    return File::exists(path);
}

bool exists(const string& path)
{
    return File::exists(path);
}

bool remove(const char* path)
{
    return File::remove(path);
}

bool remove(const string& path)
{
    return File::remove(path);
}

bool read_bytes(const char* path, byte*& out_data, int& out_size)
{
    return File::read_bytes(path, out_data, out_size);
}

bool read_bytes(const string& path, byte*& out_data, int& out_size)
{
    return File::read_bytes(path, out_data, out_size);
}

bool read_text(const char* path, string& out)
{
    return File::read_text(path, out);
}

bool read_text(const string& path, string& out)
{
    return File::read_text(path, out);
}

bool write_bytes(const char* path, const byte* data, int size)
{
    return File::write_bytes(path, data, size);
}

bool write_bytes(const string& path, const byte* data, int size)
{
    return File::write_bytes(path, data, size);
}

bool write_bytes(const char* path, const vector<byte>& data)
{
    return File::write_bytes(path, data);
}

bool write_bytes(const string& path, const vector<byte>& data)
{
    return File::write_bytes(path, data);
}

bool write_text(const char* path, const char* text)
{
    return File::write_text(path, text);
}

bool write_text(const string& path, const char* text)
{
    return File::write_text(path, text);
}

bool write_text(const char* path, const string& text)
{
    return File::write_text(path, text);
}

bool write_text(const string& path, const string& text)
{
    return File::write_text(path, text);
}

bool append_text(const char* path, const char* text)
{
    return File::append_text(path, text);
}

bool append_text(const string& path, const char* text)
{
    return File::append_text(path, text);
}

bool append_text(const char* path, const string& text)
{
    return File::append_text(path, text);
}

bool append_text(const string& path, const string& text)
{
    return File::append_text(path, text);
}

}
}
