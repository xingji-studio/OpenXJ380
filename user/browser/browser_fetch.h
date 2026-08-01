#pragma once

#include "browser_platform.h"

#include <cstdint>
#include <string>

typedef int (*xhttp_response_callback_t)(const char *data, int len, void *user_data);

typedef struct xhttp_request
{
    const char *method;
    const char *host;
    std::uint16_t port;
    const char *path;
    const char *body;
    const char *content_type;
    const char *extra_headers;
} xhttp_request_t;

int xtls_request(const xhttp_request_t *request,
                 xhttp_response_callback_t callback,
                 void *user_data);

struct BrowserFetchResult
{
    bool        ok = false;
    int         status_code = 0;
    std::string error;
    std::string final_url;
    std::string content_type;
    std::string charset;
    std::string body;
};

BrowserFetchResult browser_fetch_url(const std::string &input_url);
