// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "xtls.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t XHttpHandle;
#define XHTTP_ABI_VERSION       1U
#define XHTTP_CONNECTING        1U
#define XHTTP_SENDING           2U
#define XHTTP_READING_HEADERS   3U
#define XHTTP_READING_BODY      4U
#define XHTTP_COMPLETE          5U
#define XHTTP_FAILED            6U
#define XHTTP_HEADERS_READY     1U
#define XHTTP_BODY_READY        2U
#define XHTTP_DONE              4U
#define XHTTP_ERROR             8U
#define XHTTP_EVENTS_ALL        15U
#define XHTTP_LENGTH_KNOWN      1U
#define XHTTP_CHUNKED           2U
#define XHTTP_REUSE_CONNECTION  1U // Opt-in GET/HEAD reuse within this process/image; default remains close.
#define XHTTP_MAX_REQUEST_BYTES (1024U * 1024U)
#define XHTTP_MAX_HEADERS       64U
#define XHTTP_MAX_HEADER_BYTES  32768U
#define XHTTP_MAX_URL_BYTES     4096U

typedef struct XHttpHeader
{
    const char *name, *value;
} XHttpHeader;

typedef struct XHttpRequestDesc
{
    uint32_t           size, version;
    const char        *url;
    const char        *method; // NULL = GET. ASCII token, copied.
    const XHttpHeader *headers;
    uint32_t           header_count, max_redirects; // 0 = return 3xx; maximum 10.
    const void        *body;
    uint64_t           body_bytes;       // Binary-safe, fixed body copied at Start, max 1 MiB.
    int32_t            total_timeout_ms; // 0 = 30000; -1 = no overall deadline.
    union
    {
        uint32_t flags; // XHTTP_REUSE_CONNECTION; unknown flags are rejected.
        uint32_t reserved; // Source compatibility with the original zero-only field.
    };
    uint64_t           max_response_bytes; // 0 = streaming without a total body-size limit.
    XNetHandle         cancel;
    const void        *ca_data;
    uint64_t           ca_bytes;
    const char        *ca_file;
    uint64_t           reserved2[2];
} XHttpRequestDesc;

typedef struct XHttpResponseInfo
{
    uint32_t size, version, state, events;
    uint32_t status_code, flags, header_count, redirects;
    int64_t  result;
    uint64_t content_length, body_bytes;
    char     final_url[XHTTP_MAX_URL_BYTES];
} XHttpResponseInfo;

// Wait/Read drive cooperative progress. No background callback enters app code.
// Response bytes are bounded-buffer streaming; no complete response allocation.
// 4xx/5xx are HTTP status codes, not transport errors. HTTPS always verifies.
// Same-object overlapping progress/query returns EBUSY; Close/Cancel can wake
// an active call. Wait returns 1 event / 0 no readiness. With an unread body,
// waiting only for DONE returns 0 until Read consumes the buffered bytes.
XNetResult xapi_HttpRequestStart(const XHttpRequestDesc *desc, XHttpHandle *request);
XNetResult xapi_HttpWait(XHttpHandle request, uint32_t mask, const XNetIoOptions *io, uint32_t *events);
XNetResult xapi_HttpGetResponseInfo(XHttpHandle request, XHttpResponseInfo *info);
XNetResult xapi_HttpGetTlsInfo(XHttpHandle request, XTlsPeerInfo *info);
// Case-insensitive name; index selects repeated fields. required includes NUL.
// NULL/0 queries size. No field: ENOENT; short buffer: ENOBUFS + required.
XNetResult xapi_HttpGetHeader(XHttpHandle request, const char *name, uint32_t index, char *value, uint32_t capacity,
                              uint32_t *required);
XNetResult xapi_HttpRead(XHttpHandle request, void *data, uint64_t capacity, const XNetIoOptions *io);
XNetResult xapi_HttpCancel(XHttpHandle request);
XNetResult xapi_HttpClose(XHttpHandle request);
// Close only idle opt-in connections. Active requests retain their own lifetime.
XNetResult xapi_HttpCloseIdleConnections(void);

#ifdef __cplusplus
}
#endif
