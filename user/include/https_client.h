#pragma once

#include "httpclient.h"

enum {
    XHTTP_ERR_TLS_INIT      = -32,
    XHTTP_ERR_TLS_SEED      = -33,
    XHTTP_ERR_TLS_CONFIG    = -34,
    XHTTP_ERR_TLS_HANDSHAKE = -35,
    XHTTP_ERR_TLS_SEND      = -36,
    XHTTP_ERR_TLS_RECV      = -37,
    XHTTP_ERR_TLS_CA_LOAD   = -38,
    XHTTP_ERR_TLS_CA_PARSE  = -39,
    XHTTP_ERR_TLS_VERIFY    = -40
};

int xtls_request(const xhttp_request_t *request,
                 xhttp_response_callback_t callback,
                 void *user_data);
