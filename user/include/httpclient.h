#pragma once

#include "x3api.h"
#include "xposix/errno.h"

#ifndef XHTTP_REQUEST_BUFFER_SIZE
#define XHTTP_REQUEST_BUFFER_SIZE 2048
#endif

#ifndef XHTTP_RECV_BUFFER_SIZE
#define XHTTP_RECV_BUFFER_SIZE 4096
#endif

#ifndef XHTTP_IO_TIMEOUT_MS
#define XHTTP_IO_TIMEOUT_MS 30000
#endif

typedef int (*xhttp_response_callback_t)(const char *data, int len, void *user_data);

typedef struct xhttp_request {
    const char *method;
    const char *host;
    uint16_t    port;
    const char *path;
    const char *body;
    const char *content_type;
    const char *extra_headers;
} xhttp_request_t;

enum {
    XHTTP_OK                = 0,
    XHTTP_ERR_BAD_ARGUMENT  = -1,
    XHTTP_ERR_BAD_METHOD    = -2,
    XHTTP_ERR_BAD_HOST      = -3,
    XHTTP_ERR_BAD_PORT      = -4,
    XHTTP_ERR_SOCKET        = -5,
    XHTTP_ERR_CONNECT       = -6,
    XHTTP_ERR_REQUEST_BUILD = -7,
    XHTTP_ERR_SEND          = -8,
    XHTTP_ERR_RECV          = -9,
    XHTTP_ERR_TIMEOUT       = -10
};

static inline int xhttp_is_method_char(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '!' || ch == '#' || ch == '$' || ch == '%' || ch == '&' || ch == '\'' ||
           ch == '*' || ch == '+' || ch == '-' || ch == '.' || ch == '^' || ch == '_' ||
           ch == '`' || ch == '|' || ch == '~';
}

static inline int xhttp_send_all(int sockfd, const char *data, int len)
{
    int sent_total = 0;

    while (sent_total < len) {
        int sent = write(sockfd, (char *)data + sent_total, (uint64_t)(len - sent_total));
        if (sent <= 0) {
            return XHTTP_ERR_SEND;
        }
        sent_total += sent;
    }

    return XHTTP_OK;
}

static inline int xhttp_wait_fd(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;
    pfd.fd      = fd;
    pfd.events  = events;
    pfd.revents = 0;

    int poll_ret = poll(&pfd, 1, timeout_ms);
    if (poll_ret < 0) {
        return XHTTP_ERR_RECV;
    }
    if (poll_ret == 0) {
        return XHTTP_ERR_TIMEOUT;
    }
    if (pfd.revents & POLLNVAL) {
        return XHTTP_ERR_CONNECT;
    }
    if ((events & POLLIN) && (pfd.revents & (POLLIN | POLLHUP))) {
        return XHTTP_OK;
    }
    if ((events & POLLOUT) && (pfd.revents & POLLOUT)) {
        return XHTTP_OK;
    }
    if (pfd.revents & (POLLERR | POLLHUP)) {
        return XHTTP_ERR_CONNECT;
    }
    if ((pfd.revents & events) == 0) {
        return XHTTP_ERR_CONNECT;
    }
    return XHTTP_OK;
}

static inline int xhttp_recv_loop(int sockfd, xhttp_response_callback_t callback, void *user_data)
{
    char buffer[XHTTP_RECV_BUFFER_SIZE];
    struct pollfd pfd;
    pfd.fd      = sockfd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    while (1) {
        int poll_ret = poll(&pfd, 1, XHTTP_IO_TIMEOUT_MS);
        if (poll_ret < 0) {
            return XHTTP_ERR_RECV;
        }
        if (poll_ret == 0) {
            return XHTTP_ERR_TIMEOUT;
        }

        int got = read(sockfd, buffer, sizeof(buffer));
        if (got < 0) {
            return XHTTP_ERR_RECV;
        }
        if (got == 0) {
            break;
        }
        if (callback != NULL && callback(buffer, got, user_data) != 0) {
            break;
        }
    }

    return XHTTP_OK;
}

static inline int xhttp_validate_method(const char *method)
{
    if (method == NULL || method[0] == '\0') {
        return 0;
    }

    for (const char *p = method; *p != '\0'; ++p) {
        if (!xhttp_is_method_char(*p)) {
            return 0;
        }
    }

    return 1;
}

static inline int xhttp_parse_ipv4(const char *text, uint32_t *out_addr)
{
    uint32_t parts[4] = {0, 0, 0, 0};
    int      part     = 0;
    uint32_t value    = 0;

    if (text == NULL || out_addr == NULL) {
        return 0;
    }

    for (const char *p = text;; ++p) {
        char ch = *p;
        if (isdigit(ch)) {
            value = value * 10 + (uint32_t)(ch - '0');
            if (value > 255) {
                return 0;
            }
        } else if (ch == '.' || ch == '\0') {
            if (part >= 4) {
                return 0;
            }
            parts[part++] = value;
            value         = 0;
            if (ch == '\0') {
                break;
            }
        } else {
            return 0;
        }
    }

    if (part != 4) {
        return 0;
    }

    *out_addr = htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
    return 1;
}

static inline int xhttp_host_needs_brackets(const char *host)
{
    return host != NULL && strchr(host, ':') != NULL && host[0] != '[';
}

static inline void xhttp_format_host_header(const char *host, char *buffer, size_t size)
{
    if (buffer == NULL || size == 0) {
        return;
    }

    if (host == NULL) {
        buffer[0] = '\0';
        return;
    }

    if (xhttp_host_needs_brackets(host)) {
        snprintf(buffer, size, "[%s]", host);
    } else {
        snprintf(buffer, size, "%s", host);
    }
}

static inline int xhttp_connect_socket_fd(const char *host, uint16_t port)
{
    if (host == NULL || port == 0) {
        return XHTTP_ERR_BAD_ARGUMENT;
    }

    {
        char logbuf[160];
        snprintf(logbuf, sizeof(logbuf), "xhttp: connect begin host=%s port=%u\n",
                 host, (unsigned int)port);
        xapi_OutputSerial(logbuf);
    }

    char port_buffer[16];
    snprintf(port_buffer, sizeof(port_buffer), "%u", (unsigned int)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = NULL;
    if (getaddrinfo(host, port_buffer, &hints, &result) != 0 || result == NULL) {
        xapi_OutputSerial((char *)"xhttp: getaddrinfo failed\n");
        if (result != NULL) {
            freeaddrinfo(result);
        }
        return XHTTP_ERR_BAD_HOST;
    }
    xapi_OutputSerial((char *)"xhttp: getaddrinfo ok\n");

    int status = XHTTP_ERR_CONNECT;
    for (struct addrinfo *it = result; it != NULL; it = it->ai_next) {
        if (it->ai_addr == NULL) {
            continue;
        }

        {
            char logbuf[160];
            snprintf(logbuf, sizeof(logbuf), "xhttp: try ai_family=%d socktype=%d addrlen=%u\n",
                     it->ai_family, it->ai_socktype, (unsigned int)it->ai_addrlen);
            xapi_OutputSerial(logbuf);
        }

        int sockfd = socket(it->ai_family, SOCK_STREAM, 0);
        if (sockfd < 0) {
            {
                char logbuf[96];
                snprintf(logbuf, sizeof(logbuf), "xhttp: socket failed ret=%d\n", sockfd);
                xapi_OutputSerial(logbuf);
            }
            status = XHTTP_ERR_SOCKET;
            continue;
        }
        {
            char logbuf[96];
            snprintf(logbuf, sizeof(logbuf), "xhttp: socket ok fd=%d\n", sockfd);
            xapi_OutputSerial(logbuf);
        }

        int original_flags = fcntl(sockfd, F_GETFL, 0);
        {
            char logbuf[96];
            snprintf(logbuf, sizeof(logbuf), "xhttp: fcntl getfl=%d\n", original_flags);
            xapi_OutputSerial(logbuf);
        }
        if (original_flags >= 0) {
            int setfl_ret = fcntl(sockfd, F_SETFL, (uint64_t)(original_flags | O_NONBLOCK));
            char logbuf[96];
            snprintf(logbuf, sizeof(logbuf), "xhttp: fcntl setfl ret=%d new=0x%x\n",
                     setfl_ret, (unsigned int)(original_flags | O_NONBLOCK));
            xapi_OutputSerial(logbuf);
        }

        xapi_OutputSerial((char *)"xhttp: before connect\n");
        int connect_ret = connect(sockfd, it->ai_addr, it->ai_addrlen);
        int connect_errno = connect_ret < 0 ? errno : 0;
        {
            char logbuf[96];
            snprintf(logbuf, sizeof(logbuf), "xhttp: connect ret=%d errno=%d\n",
                     connect_ret, connect_errno);
            xapi_OutputSerial(logbuf);
        }
        if (connect_ret < 0 && connect_errno != EINPROGRESS &&
            connect_errno != EALREADY && connect_errno != EWOULDBLOCK) {
            close(sockfd);
            status = XHTTP_ERR_CONNECT;
            continue;
        }

        xapi_OutputSerial((char *)"xhttp: before poll wait\n");
        int wait_status = xhttp_wait_fd(sockfd, POLLOUT, XHTTP_IO_TIMEOUT_MS);
        {
            char logbuf[96];
            snprintf(logbuf, sizeof(logbuf), "xhttp: poll wait ret=%d\n", wait_status);
            xapi_OutputSerial(logbuf);
        }
        if (original_flags >= 0) {
            int restore_ret = fcntl(sockfd, F_SETFL, (uint64_t)original_flags);
            char logbuf[96];
            snprintf(logbuf, sizeof(logbuf), "xhttp: fcntl restore ret=%d flags=0x%x\n",
                     restore_ret, (unsigned int)original_flags);
            xapi_OutputSerial(logbuf);
        }
        if (wait_status == XHTTP_OK) {
            freeaddrinfo(result);
            {
                char logbuf[96];
                snprintf(logbuf, sizeof(logbuf), "xhttp: connect success fd=%d\n", sockfd);
                xapi_OutputSerial(logbuf);
            }
            return sockfd;
        }

        close(sockfd);
        status = wait_status == XHTTP_ERR_TIMEOUT ? XHTTP_ERR_TIMEOUT : XHTTP_ERR_CONNECT;
    }

    freeaddrinfo(result);
    return status;
}

static inline int xhttp_request(const xhttp_request_t *request,
                                xhttp_response_callback_t callback,
                                void *user_data)
{
    if (request == NULL || request->host == NULL || request->path == NULL) {
        return XHTTP_ERR_BAD_ARGUMENT;
    }
    if (!xhttp_validate_method(request->method)) {
        return XHTTP_ERR_BAD_METHOD;
    }
    if (request->port == 0) {
        return XHTTP_ERR_BAD_PORT;
    }

    {
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf), "xhttp: start method=%s host=%s port=%u path=%s\n",
                 request->method, request->host, (unsigned int)request->port, request->path);
        xapi_OutputSerial(logbuf);
    }

    int sockfd = xhttp_connect_socket_fd(request->host, request->port);
    {
        char logbuf[128];
        snprintf(logbuf, sizeof(logbuf), "xhttp: socket ret=%d\n", sockfd);
        xapi_OutputSerial(logbuf);
    }
    if (sockfd < 0) {
        char logbuf[128];
        snprintf(logbuf, sizeof(logbuf), "xhttp: socket failed ret=%d\n", sockfd);
        xapi_OutputSerial(logbuf);
        return sockfd == XHTTP_ERR_BAD_HOST ? XHTTP_ERR_BAD_HOST :
               (sockfd == XHTTP_ERR_TIMEOUT ? XHTTP_ERR_TIMEOUT :
                (sockfd == XHTTP_ERR_SOCKET ? XHTTP_ERR_SOCKET : XHTTP_ERR_CONNECT));
    }

    const char *path          = request->path[0] ? request->path : "/";
    const char *body          = request->body;
    const char *content_type  = request->content_type ? request->content_type : "text/plain";
    const char *extra_headers = request->extra_headers ? request->extra_headers : "";
    int         body_len      = body ? (int)strlen(body) : 0;
    char        host_header[320];
    xhttp_format_host_header(request->host, host_header, sizeof(host_header));

    char request_text[XHTTP_REQUEST_BUFFER_SIZE];
    int  request_len = 0;

    if (body_len > 0) {
        request_len = snprintf(request_text, sizeof(request_text),
                               "%s %s HTTP/1.0\r\n"
                               "Host: %s\r\n"
                               "Connection: close\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %d\r\n"
                               "%s"
                               "\r\n"
                               "%s",
                               request->method, path, host_header, content_type, body_len, extra_headers, body);
    } else {
        request_len = snprintf(request_text, sizeof(request_text),
                               "%s %s HTTP/1.0\r\n"
                               "Host: %s\r\n"
                               "Connection: close\r\n"
                               "%s"
                               "\r\n",
                               request->method, path, host_header, extra_headers);
    }

    if (request_len <= 0 || request_len >= (int)sizeof(request_text)) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
        return XHTTP_ERR_REQUEST_BUILD;
    }

    int status = xhttp_send_all(sockfd, request_text, request_len);
    if (status == XHTTP_OK) {
        status = xhttp_recv_loop(sockfd, callback, user_data);
    }

    {
        char logbuf[96];
        snprintf(logbuf, sizeof(logbuf), "xhttp: done status=%d\n", status);
        xapi_OutputSerial(logbuf);
    }
    shutdown(sockfd, SHUT_RDWR);
    close(sockfd);
    return status;
}

static inline int xhttp_request_method(const char *method,
                                       const char *host,
                                       uint16_t port,
                                       const char *path,
                                       const char *body,
                                       const char *content_type,
                                       const char *extra_headers,
                                       xhttp_response_callback_t callback,
                                       void *user_data)
{
    xhttp_request_t request;
    request.method        = method;
    request.host          = host;
    request.port          = port;
    request.path          = path;
    request.body          = body;
    request.content_type  = content_type;
    request.extra_headers = extra_headers;
    return xhttp_request(&request, callback, user_data);
}

static inline int xhttp_get_request(const char *host,
                                    uint16_t port,
                                    const char *path,
                                    const char *extra_headers,
                                    xhttp_response_callback_t callback,
                                    void *user_data)
{
    return xhttp_request_method("GET", host, port, path, NULL, NULL, extra_headers, callback, user_data);
}

static inline int xhttp_post_request(const char *host,
                                     uint16_t port,
                                     const char *path,
                                     const char *body,
                                     const char *content_type,
                                     const char *extra_headers,
                                     xhttp_response_callback_t callback,
                                     void *user_data)
{
    return xhttp_request_method("POST", host, port, path, body, content_type, extra_headers, callback, user_data);
}

static inline int xhttp_put_request(const char *host,
                                    uint16_t port,
                                    const char *path,
                                    const char *body,
                                    const char *content_type,
                                    const char *extra_headers,
                                    xhttp_response_callback_t callback,
                                    void *user_data)
{
    return xhttp_request_method("PUT", host, port, path, body, content_type, extra_headers, callback, user_data);
}

static inline int xhttp_patch_request(const char *host,
                                      uint16_t port,
                                      const char *path,
                                      const char *body,
                                      const char *content_type,
                                      const char *extra_headers,
                                      xhttp_response_callback_t callback,
                                      void *user_data)
{
    return xhttp_request_method("PATCH", host, port, path, body, content_type, extra_headers, callback, user_data);
}

static inline int xhttp_delete_request(const char *host,
                                       uint16_t port,
                                       const char *path,
                                       const char *body,
                                       const char *content_type,
                                       const char *extra_headers,
                                       xhttp_response_callback_t callback,
                                       void *user_data)
{
    return xhttp_request_method("DELETE", host, port, path, body, content_type, extra_headers, callback, user_data);
}

static inline int xhttp_head_request(const char *host,
                                     uint16_t port,
                                     const char *path,
                                     const char *extra_headers,
                                     xhttp_response_callback_t callback,
                                     void *user_data)
{
    return xhttp_request_method("HEAD", host, port, path, NULL, NULL, extra_headers, callback, user_data);
}

static inline int xhttp_options_request(const char *host,
                                        uint16_t port,
                                        const char *path,
                                        const char *extra_headers,
                                        xhttp_response_callback_t callback,
                                        void *user_data)
{
    return xhttp_request_method("OPTIONS", host, port, path, NULL, NULL, extra_headers, callback, user_data);
}

static inline int xhttp_trace_request(const char *host,
                                      uint16_t port,
                                      const char *path,
                                      const char *extra_headers,
                                      xhttp_response_callback_t callback,
                                      void *user_data)
{
    return xhttp_request_method("TRACE", host, port, path, NULL, NULL, extra_headers, callback, user_data);
}

static inline int xhttp_connect_request(const char *host,
                                        uint16_t port,
                                        const char *path,
                                        const char *extra_headers,
                                        xhttp_response_callback_t callback,
                                        void *user_data)
{
    return xhttp_request_method("CONNECT", host, port, path, NULL, NULL, extra_headers, callback, user_data);
}
