#pragma once
#include "string.hpp"
#include "vector.hpp"

namespace stardustui {
class File {
public:
    using byte = unsigned char;

    File();
    explicit File(const char* path);
    explicit File(const string& path);

    void set_path(const char* path);
    void set_path(const string& path);
    const string& path() const;

    bool exists() const;
    bool remove() const;
    bool read_bytes(byte*& out_data, int& out_size) const;
    bool read_text(string& out) const;
    bool write_bytes(const byte* data, int size) const;
    bool write_bytes(const vector<byte>& data) const;
    bool write_text(const char* text) const;
    bool write_text(const string& text) const;
    bool append_text(const char* text) const;
    bool append_text(const string& text) const;
    static void free_bytes(byte* data);

    static bool exists(const char* path);
    static bool exists(const string& path);
    static bool remove(const char* path);
    static bool remove(const string& path);
    static bool read_bytes(const char* path, byte*& out_data, int& out_size);
    static bool read_bytes(const string& path, byte*& out_data, int& out_size);
    static bool read_text(const char* path, string& out);
    static bool read_text(const string& path, string& out);
    static bool write_bytes(const char* path, const byte* data, int size);
    static bool write_bytes(const string& path, const byte* data, int size);
    static bool write_bytes(const char* path, const vector<byte>& data);
    static bool write_bytes(const string& path, const vector<byte>& data);
    static bool write_text(const char* path, const char* text);
    static bool write_text(const string& path, const char* text);
    static bool write_text(const char* path, const string& text);
    static bool write_text(const string& path, const string& text);
    static bool append_text(const char* path, const char* text);
    static bool append_text(const string& path, const char* text);
    static bool append_text(const char* path, const string& text);
    static bool append_text(const string& path, const string& text);

private:
    string file_path;
};

namespace file {

using byte = File::byte;

bool exists(const char* path);
bool exists(const string& path);

bool remove(const char* path);
bool remove(const string& path);

bool read_bytes(const char* path, byte*& out_data, int& out_size);
bool read_bytes(const string& path, byte*& out_data, int& out_size);

bool read_text(const char* path, string& out);
bool read_text(const string& path, string& out);

bool write_bytes(const char* path, const byte* data, int size);
bool write_bytes(const string& path, const byte* data, int size);
bool write_bytes(const char* path, const vector<byte>& data);
bool write_bytes(const string& path, const vector<byte>& data);

bool write_text(const char* path, const char* text);
bool write_text(const string& path, const char* text);
bool write_text(const char* path, const string& text);
bool write_text(const string& path, const string& text);

bool append_text(const char* path, const char* text);
bool append_text(const string& path, const char* text);
bool append_text(const char* path, const string& text);
bool append_text(const string& path, const string& text);

}
}
