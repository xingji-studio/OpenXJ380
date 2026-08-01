#pragma once
#include "vector.hpp"

namespace stardustui {
class string : public vector<char>
{
public:
    string() {
        ensure_terminator();
    }

    string(const char* text) {
        assign(text);
    }

    void assign(const char* text) {
        vector<char>::clear();

        if (text == nullptr) {
            ensure_terminator();
            return;
        }

        int index = 0;
        while (text[index] != '\0') {
            vector<char>::push_back(text[index]);
            ++index;
        }

        vector<char>::push_back('\0');
    }

    bool append(const char* text) {
        if (text == nullptr || text[0] == '\0') {
            ensure_terminator();
            return true;
        }

        ensure_terminator();

        int index = 0;
        int last_index = vector<char>::size() - 1;
        vector<char>::operator[](last_index) = text[index++];

        while (text[index] != '\0') {
            if (!vector<char>::push_back(text[index])) {
                return false;
            }
            ++index;
        }

        return vector<char>::push_back('\0');
    }

    bool push_char(const char& ch) {
        ensure_terminator();

        int last_index = vector<char>::size() - 1;
        vector<char>::operator[](last_index) = ch;

        return vector<char>::push_back('\0');
    }

    int length() const {
        int current_size = vector<char>::size();
        return current_size == 0 ? 0 : current_size - 1;
    }

    const char* c_str() const {
        const char* data = vector<char>::at(0);
        return data == nullptr ? &empty_value() : data;
    }

    char* data() {
        char* data = vector<char>::at(0);
        return data == nullptr ? &empty_value() : data;
    }

    bool equals(const char* text) const {
        if (text == nullptr) {
            return length() == 0;
        }

        int index = 0;
        while (true) {
            char current = index < vector<char>::size() ? vector<char>::operator[](index) : '\0';
            if (current != text[index]) {
                return false;
            }
            if (current == '\0') {
                return true;
            }
            ++index;
        }
    }

private:
    void ensure_terminator() {
        int current_size = vector<char>::size();
        if (current_size == 0) {
            vector<char>::push_back('\0');
            return;
        }

        if (vector<char>::operator[](current_size - 1) != '\0') {
            vector<char>::push_back('\0');
        }
    }

    static char& empty_value() {
        static char empty = '\0';
        return empty;
    }
};
}
