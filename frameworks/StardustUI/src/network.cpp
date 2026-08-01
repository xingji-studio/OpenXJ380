#include "../includes/network.hpp"
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

bool append_bytes(vector<unsigned char>& target, const unsigned char* data, int size)
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

bool assign_text_from_bytes(const unsigned char* data, int size, string& out)
{
    out.assign("");
    if (size <= 0) {
        return true;
    }

    if (data == nullptr) {
        return false;
    }

    char* buffer = new char[size + 1];
    if (buffer == nullptr) {
        return false;
    }

    for (int index = 0; index < size; ++index) {
        buffer[index] = static_cast<char>(data[index]);
    }
    buffer[size] = '\0';
    out.assign(buffer);
    delete[] buffer;
    return true;
}

bool starts_with(const char* text, const char* prefix)
{
    if (text == nullptr || prefix == nullptr) {
        return false;
    }

    int index = 0;
    while (prefix[index] != '\0') {
        if (text[index] != prefix[index]) {
            return false;
        }
        ++index;
    }
    return true;
}

char to_lower_ascii(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

bool equals_ignore_case(const char* left, const char* right)
{
    if (left == nullptr || right == nullptr) {
        return left == right;
    }

    int index = 0;
    while (left[index] != '\0' || right[index] != '\0') {
        if (to_lower_ascii(left[index]) != to_lower_ascii(right[index])) {
            return false;
        }
        ++index;
    }
    return true;
}

bool contains_ignore_case(const char* haystack, const char* needle)
{
    if (haystack == nullptr || needle == nullptr || needle[0] == '\0') {
        return false;
    }

    const int haystack_length = text_length(haystack);
    const int needle_length = text_length(needle);
    if (needle_length > haystack_length) {
        return false;
    }

    for (int start = 0; start <= haystack_length - needle_length; ++start) {
        bool matched = true;
        for (int index = 0; index < needle_length; ++index) {
            if (to_lower_ascii(haystack[start + index]) != to_lower_ascii(needle[index])) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return true;
        }
    }

    return false;
}

bool parse_unsigned_short(const char* text, unsigned short& out_port)
{
    out_port = 0;
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    unsigned int value = 0;
    for (int index = 0; text[index] != '\0'; ++index) {
        const char ch = text[index];
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10u + static_cast<unsigned int>(ch - '0');
        if (value > 65535u) {
            return false;
        }
    }

    if (value == 0u) {
        return false;
    }

    out_port = static_cast<unsigned short>(value);
    return true;
}

bool parse_url(const string& url,
               string& out_scheme,
               string& out_host,
               unsigned short& out_port,
               string& out_path)
{
    out_scheme.assign("");
    out_host.assign("");
    out_path.assign("/");
    out_port = 0;

    const char* raw = url.c_str();
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }

    int scheme_end = -1;
    for (int index = 0; raw[index] != '\0'; ++index) {
        if (raw[index] == ':' &&
            raw[index + 1] == '/' &&
            raw[index + 2] == '/') {
            scheme_end = index;
            break;
        }
    }

    if (scheme_end <= 0) {
        return false;
    }

    char* scheme_buffer = new char[scheme_end + 1];
    if (scheme_buffer == nullptr) {
        return false;
    }
    for (int index = 0; index < scheme_end; ++index) {
        scheme_buffer[index] = raw[index];
    }
    scheme_buffer[scheme_end] = '\0';
    out_scheme.assign(scheme_buffer);
    delete[] scheme_buffer;

    if (!equals_ignore_case(out_scheme.c_str(), "http") &&
        !equals_ignore_case(out_scheme.c_str(), "https")) {
        return false;
    }

    const char* authority = raw + scheme_end + 3;
    int authority_length = 0;
    while (authority[authority_length] != '\0' && authority[authority_length] != '/') {
        ++authority_length;
    }

    if (authority_length <= 0) {
        return false;
    }

    int host_start = 0;
    int host_end = authority_length;
    int port_start = -1;

    if (authority[0] == '[') {
        int closing = -1;
        for (int index = 1; index < authority_length; ++index) {
            if (authority[index] == ']') {
                closing = index;
                break;
            }
        }
        if (closing <= 1) {
            return false;
        }

        host_start = 1;
        host_end = closing;
        if (closing + 1 < authority_length) {
            if (authority[closing + 1] != ':') {
                return false;
            }
            port_start = closing + 2;
        }
    } else {
        for (int index = 0; index < authority_length; ++index) {
            if (authority[index] == ':') {
                host_end = index;
                port_start = index + 1;
                break;
            }
        }
    }

    if (host_end <= host_start) {
        return false;
    }

    char* host_buffer = new char[host_end - host_start + 1];
    if (host_buffer == nullptr) {
        return false;
    }
    for (int index = host_start; index < host_end; ++index) {
        host_buffer[index - host_start] = authority[index];
    }
    host_buffer[host_end - host_start] = '\0';
    out_host.assign(host_buffer);
    delete[] host_buffer;

    if (out_host.length() <= 0) {
        return false;
    }

    if (port_start >= 0) {
        const int port_length = authority_length - port_start;
        if (port_length <= 0) {
            return false;
        }
        char* port_buffer = new char[port_length + 1];
        if (port_buffer == nullptr) {
            return false;
        }
        for (int index = 0; index < port_length; ++index) {
            port_buffer[index] = authority[port_start + index];
        }
        port_buffer[port_length] = '\0';
        const bool parsed = parse_unsigned_short(port_buffer, out_port);
        delete[] port_buffer;
        if (!parsed) {
            return false;
        }
    } else {
        out_port = equals_ignore_case(out_scheme.c_str(), "https") ? 443 : 80;
    }

    if (authority[authority_length] == '/') {
        out_path.assign(authority + authority_length);
    } else {
        out_path.assign("/");
    }

    return true;
}

bool decode_chunked_body(const vector<unsigned char>& input, vector<unsigned char>& output)
{
    output.clear();
    int cursor = 0;

    while (cursor < input.size()) {
        int line_end = -1;
        int delimiter_size = 0;
        for (int index = cursor; index < input.size(); ++index) {
            if (input[index] == '\r' && index + 1 < input.size() && input[index + 1] == '\n') {
                line_end = index;
                delimiter_size = 2;
                break;
            }
            if (input[index] == '\n') {
                line_end = index;
                delimiter_size = 1;
                break;
            }
        }

        if (line_end < 0) {
            return false;
        }

        unsigned int chunk_size = 0;
        bool saw_digit = false;
        for (int index = cursor; index < line_end; ++index) {
            const unsigned char ch = input[index];
            if (ch == ';') {
                break;
            }

            unsigned int value = 0;
            if (ch >= '0' && ch <= '9') {
                value = static_cast<unsigned int>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                value = static_cast<unsigned int>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                value = static_cast<unsigned int>(ch - 'A' + 10);
            } else {
                return false;
            }

            saw_digit = true;
            chunk_size = (chunk_size << 4u) | value;
        }

        if (!saw_digit) {
            return false;
        }

        cursor = line_end + delimiter_size;
        if (chunk_size == 0u) {
            return true;
        }

        if (cursor + static_cast<int>(chunk_size) > input.size()) {
            return false;
        }

        if (!append_bytes(output, input.at(cursor), static_cast<int>(chunk_size))) {
            return false;
        }
        cursor += static_cast<int>(chunk_size);

        if (cursor < input.size() && input[cursor] == '\r') {
            ++cursor;
        }
        if (cursor < input.size() && input[cursor] == '\n') {
            ++cursor;
        }
    }

    return false;
}

bool parse_http_response(const vector<unsigned char>& raw, HttpResponse& response)
{
    response.clear();
    if (raw.size() <= 0) {
        response.error_message.assign("Empty HTTP response");
        return false;
    }

    int header_end = -1;
    int delimiter_size = 0;
    for (int index = 0; index + 1 < raw.size(); ++index) {
        if (index + 3 < raw.size() &&
            raw[index] == '\r' &&
            raw[index + 1] == '\n' &&
            raw[index + 2] == '\r' &&
            raw[index + 3] == '\n') {
            header_end = index;
            delimiter_size = 4;
            break;
        }

        if (index + 1 < raw.size() &&
            raw[index] == '\n' &&
            raw[index + 1] == '\n') {
            header_end = index;
            delimiter_size = 2;
            break;
        }
    }

    if (header_end < 0) {
        response.error_message.assign("Bad HTTP response headers");
        return false;
    }

    if (!assign_text_from_bytes(raw.at(0), header_end, response.headers)) {
        response.error_message.assign("Failed to decode HTTP headers");
        return false;
    }

    const char* headers_text = response.headers.c_str();
    if (headers_text == nullptr || headers_text[0] == '\0') {
        response.error_message.assign("Missing HTTP status line");
        return false;
    }

    int first_line_end = 0;
    while (headers_text[first_line_end] != '\0' &&
           headers_text[first_line_end] != '\r' &&
           headers_text[first_line_end] != '\n') {
        ++first_line_end;
    }

    int first_space = -1;
    for (int index = 0; index < first_line_end; ++index) {
        if (headers_text[index] == ' ') {
            first_space = index;
            break;
        }
    }
    if (first_space < 0 || first_space + 3 >= first_line_end) {
        response.error_message.assign("Invalid HTTP status line");
        return false;
    }

    int status_code = 0;
    for (int index = first_space + 1; index < first_line_end; ++index) {
        const char ch = headers_text[index];
        if (ch < '0' || ch > '9') {
            break;
        }
        status_code = status_code * 10 + (ch - '0');
    }
    response.status_code = status_code;

    int content_length = -1;
    const int header_length = response.headers.length();
    int line_start = first_line_end;
    while (line_start < header_length) {
        while (line_start < header_length &&
               (headers_text[line_start] == '\r' || headers_text[line_start] == '\n')) {
            ++line_start;
        }
        if (line_start >= header_length) {
            break;
        }

        int line_end = line_start;
        while (line_end < header_length &&
               headers_text[line_end] != '\r' &&
               headers_text[line_end] != '\n') {
            ++line_end;
        }

        int colon = -1;
        for (int index = line_start; index < line_end; ++index) {
            if (headers_text[index] == ':') {
                colon = index;
                break;
            }
        }
        if (colon > line_start) {
            const int name_length = colon - line_start;
            const int value_start_initial = colon + 1;
            int value_start = value_start_initial;
            while (value_start < line_end &&
                   (headers_text[value_start] == ' ' || headers_text[value_start] == '\t')) {
                ++value_start;
            }
            const int value_length = line_end - value_start;

            char* name_buffer = new char[name_length + 1];
            char* value_buffer = new char[value_length + 1];
            if (name_buffer == nullptr || value_buffer == nullptr) {
                delete[] name_buffer;
                delete[] value_buffer;
                response.error_message.assign("Out of memory while parsing headers");
                return false;
            }

            for (int index = 0; index < name_length; ++index) {
                name_buffer[index] = headers_text[line_start + index];
            }
            name_buffer[name_length] = '\0';

            for (int index = 0; index < value_length; ++index) {
                value_buffer[index] = headers_text[value_start + index];
            }
            value_buffer[value_length] = '\0';

            if (equals_ignore_case(name_buffer, "Content-Type")) {
                response.content_type.assign(value_buffer);
                for (int index = 0; value_buffer[index] != '\0'; ++index) {
                    if (value_buffer[index] == ';') {
                        value_buffer[index] = '\0';
                        response.content_type.assign(value_buffer);

                        const char* parameter = value_buffer + index + 1;
                        while (*parameter == ' ' || *parameter == '\t') {
                            ++parameter;
                        }
                        if (contains_ignore_case(parameter, "charset=")) {
                            const char* charset = parameter;
                            while (*charset != '\0' &&
                                   to_lower_ascii(charset[0]) != 'c') {
                                ++charset;
                            }
                            if (starts_with(charset, "charset=") ||
                                starts_with(charset, "Charset=") ||
                                starts_with(charset, "CHARSET=")) {
                                response.charset.assign(charset + 8);
                            }
                        }
                        break;
                    }
                }
            } else if (equals_ignore_case(name_buffer, "Transfer-Encoding")) {
                response.transfer_encoding.assign(value_buffer);
            } else if (equals_ignore_case(name_buffer, "Content-Length")) {
                int parsed_length = 0;
                bool valid = value_buffer[0] != '\0';
                for (int index = 0; value_buffer[index] != '\0'; ++index) {
                    if (value_buffer[index] < '0' || value_buffer[index] > '9') {
                        valid = false;
                        break;
                    }
                    parsed_length = parsed_length * 10 + (value_buffer[index] - '0');
                }
                if (valid) {
                    content_length = parsed_length;
                }
            }

            delete[] name_buffer;
            delete[] value_buffer;
        }

        line_start = line_end;
    }

    const int body_start = header_end + delimiter_size;
    const int body_size = raw.size() - body_start;
    vector<unsigned char> body_bytes;
    if (body_size > 0) {
        if (!append_bytes(body_bytes, raw.at(body_start), body_size)) {
            response.error_message.assign("Out of memory while reading body");
            return false;
        }
    }

    if (contains_ignore_case(response.transfer_encoding.c_str(), "chunked")) {
        vector<unsigned char> decoded;
        if (!decode_chunked_body(body_bytes, decoded)) {
            response.error_message.assign("Failed to decode chunked body");
            return false;
        }
        response.body = decoded;
    } else if (content_length >= 0 && content_length < body_bytes.size()) {
        response.body.clear();
        if (!append_bytes(response.body, body_bytes.at(0), content_length)) {
            response.error_message.assign("Out of memory while trimming body");
            return false;
        }
    } else {
        response.body = body_bytes;
    }

    response.ok = true;
    response.error_message.assign("");
    return true;
}

bool prepare_request(const HttpRequest& input, HttpRequest& resolved, HttpResponse& response)
{
    resolved = input;
    response.clear();

    if (resolved.method.length() <= 0) {
        resolved.method.assign("GET");
    }

    if (resolved.url.length() > 0) {
        string scheme;
        string host;
        string path;
        unsigned short port = 0;
        if (!parse_url(resolved.url, scheme, host, port, path)) {
            response.error_message.assign("Invalid URL");
            return false;
        }

        resolved.host = host;
        resolved.port = port;
        resolved.path = path;
        resolved.use_tls = equals_ignore_case(scheme.c_str(), "https");
    }

    if (resolved.host.length() <= 0) {
        response.error_message.assign("Missing host");
        return false;
    }

    if (resolved.port == 0) {
        resolved.port = resolved.use_tls ? 443 : 80;
    }

    if (resolved.path.length() <= 0) {
        resolved.path.assign("/");
    } else if (resolved.path.c_str()[0] != '/') {
        string prefixed("/");
        prefixed.append(resolved.path.c_str());
        resolved.path = prefixed;
    }

    return true;
}

}

Socket::Socket() : handle(0) {}

Socket::~Socket()
{
    close();
}

Socket::Socket(Socket&& other) noexcept : handle(other.handle)
{
    other.handle = 0;
}

Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other) {
        close();
        this->handle = other.handle;
        other.handle = 0;
    }
    return *this;
}

bool Socket::connect(const char* host, unsigned short port)
{
    close();
    if (host == nullptr || host[0] == '\0' || port == 0) {
        return false;
    }
    return socket_connect_platform(host, port, this->handle);
}

bool Socket::connect(const string& host, unsigned short port)
{
    return connect(host.c_str(), port);
}

bool Socket::close()
{
    if (this->handle == 0) {
        return true;
    }

    const bool closed = socket_close_platform(this->handle);
    this->handle = 0;
    return closed;
}

bool Socket::is_open() const
{
    return this->handle != 0;
}

bool Socket::send(const byte* data, int size, int& out_sent)
{
    out_sent = 0;
    if (!is_open() || size < 0 || (size > 0 && data == nullptr)) {
        return false;
    }
    return socket_send_platform(this->handle, data, size, out_sent);
}

bool Socket::send_all(const byte* data, int size)
{
    if (size < 0 || (size > 0 && data == nullptr)) {
        return false;
    }

    int sent_total = 0;
    while (sent_total < size) {
        int sent_now = 0;
        if (!send(data + sent_total, size - sent_total, sent_now) || sent_now <= 0) {
            return false;
        }
        sent_total += sent_now;
    }

    return true;
}

bool Socket::send_text(const char* text)
{
    if (text == nullptr) {
        return false;
    }
    return send_all(reinterpret_cast<const byte*>(text), text_length(text));
}

bool Socket::send_text(const string& text)
{
    return send_text(text.c_str());
}

bool Socket::receive(byte* buffer, int capacity, int& out_received)
{
    out_received = 0;
    if (!is_open() || capacity < 0 || (capacity > 0 && buffer == nullptr)) {
        return false;
    }
    return socket_receive_platform(this->handle, buffer, capacity, out_received);
}

bool Socket::receive_all(vector<byte>& out_data)
{
    out_data.clear();
    if (!is_open()) {
        return false;
    }

    byte buffer[1024];
    while (true) {
        int received = 0;
        if (!receive(buffer, static_cast<int>(sizeof(buffer)), received)) {
            return false;
        }
        if (received <= 0) {
            return true;
        }
        if (!append_bytes(out_data, buffer, received)) {
            return false;
        }
    }
}

HttpRequest::HttpRequest()
    : url(),
      method("GET"),
      host(),
      port(0),
      path("/"),
      body(),
      content_type("text/plain"),
      extra_headers(),
      use_tls(false)
{
}

HttpResponse::HttpResponse()
    : status_code(0),
      ok(false),
      headers(),
      body(),
      content_type(),
      charset(),
      transfer_encoding(),
      error_message()
{
}

void HttpResponse::clear()
{
    status_code = 0;
    ok = false;
    headers.assign("");
    body.clear();
    content_type.assign("");
    charset.assign("");
    transfer_encoding.assign("");
    error_message.assign("");
}

bool HttpResponse::body_text(string& out) const
{
    if (body.size() <= 0) {
        out.assign("");
        return true;
    }
    return assign_text_from_bytes(body.at(0), body.size(), out);
}

bool Http::request(const HttpRequest& request, HttpResponse& response)
{
    HttpRequest resolved;
    if (!prepare_request(request, resolved, response)) {
        return false;
    }

    vector<unsigned char> raw_response;
    string transport_error;
    if (!http_request_platform(resolved, raw_response, transport_error)) {
        response.clear();
        response.error_message = transport_error;
        if (response.error_message.length() <= 0) {
            response.error_message.assign("HTTP request failed");
        }
        return false;
    }

    if (!parse_http_response(raw_response, response)) {
        return false;
    }

    return true;
}

bool Http::request(const char* method,
                   const char* url,
                   HttpResponse& response,
                   const char* body,
                   const char* content_type,
                   const char* extra_headers)
{
    HttpRequest request;
    if (method != nullptr && method[0] != '\0') {
        request.method.assign(method);
    }
    if (url != nullptr) {
        request.url.assign(url);
    }
    if (body != nullptr) {
        request.body.assign(body);
    }
    if (content_type != nullptr) {
        request.content_type.assign(content_type);
    }
    if (extra_headers != nullptr) {
        request.extra_headers.assign(extra_headers);
    }
    return Http::request(request, response);
}

bool Http::get(const char* url, HttpResponse& response)
{
    return request("GET", url, response, nullptr, nullptr, nullptr);
}

bool Http::post(const char* url,
                const char* body,
                const char* content_type,
                HttpResponse& response,
                const char* extra_headers)
{
    return request("POST", url, response, body, content_type, extra_headers);
}

namespace network {

bool http_request(const HttpRequest& request, HttpResponse& response)
{
    return Http::request(request, response);
}

bool http_request(const char* method,
                  const char* url,
                  HttpResponse& response,
                  const char* body,
                  const char* content_type,
                  const char* extra_headers)
{
    return Http::request(method, url, response, body, content_type, extra_headers);
}

bool http_get(const char* url, HttpResponse& response)
{
    return Http::get(url, response);
}

bool http_post(const char* url,
               const char* body,
               const char* content_type,
               HttpResponse& response,
               const char* extra_headers)
{
    return Http::post(url, body, content_type, response, extra_headers);
}

}

}
