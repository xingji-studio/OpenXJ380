#pragma once
#include "string.hpp"
#include "vector.hpp"

namespace stardustui {

class Socket {
public:
    using byte = unsigned char;

    Socket();
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    bool connect(const char* host, unsigned short port);
    bool connect(const string& host, unsigned short port);
    bool close();
    bool is_open() const;

    bool send(const byte* data, int size, int& out_sent);
    bool send_all(const byte* data, int size);
    bool send_text(const char* text);
    bool send_text(const string& text);

    bool receive(byte* buffer, int capacity, int& out_received);
    bool receive_all(vector<byte>& out_data);

private:
    long long handle;
};

struct HttpRequest {
    string url;
    string method;
    string host;
    unsigned short port;
    string path;
    string body;
    string content_type;
    string extra_headers;
    bool use_tls;

    HttpRequest();
};

struct HttpResponse {
    int status_code;
    bool ok;
    string headers;
    vector<unsigned char> body;
    string content_type;
    string charset;
    string transfer_encoding;
    string error_message;

    HttpResponse();
    void clear();
    bool body_text(string& out) const;
};

class Http {
public:
    static bool request(const HttpRequest& request, HttpResponse& response);
    static bool request(const char* method,
                        const char* url,
                        HttpResponse& response,
                        const char* body = nullptr,
                        const char* content_type = nullptr,
                        const char* extra_headers = nullptr);
    static bool get(const char* url, HttpResponse& response);
    static bool post(const char* url,
                     const char* body,
                     const char* content_type,
                     HttpResponse& response,
                     const char* extra_headers = nullptr);
};

namespace network {

bool http_request(const HttpRequest& request, HttpResponse& response);
bool http_request(const char* method,
                  const char* url,
                  HttpResponse& response,
                  const char* body = nullptr,
                  const char* content_type = nullptr,
                  const char* extra_headers = nullptr);
bool http_get(const char* url, HttpResponse& response);
bool http_post(const char* url,
               const char* body,
               const char* content_type,
               HttpResponse& response,
               const char* extra_headers = nullptr);

}

}
