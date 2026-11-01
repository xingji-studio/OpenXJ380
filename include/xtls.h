// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "xnet.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t XTlsHandle;
#define XTLS_ABI_VERSION       1U
#define XTLS_PREPARING         1U
#define XTLS_RESOLVING         2U
#define XTLS_CONNECTING        3U
#define XTLS_HANDSHAKING       4U
#define XTLS_READY             5U
#define XTLS_FAILED            6U
#define XTLS_EVENT_READY       1U
#define XTLS_READABLE          2U
#define XTLS_WRITABLE          4U
#define XTLS_EOF               8U
#define XTLS_ERROR             16U
#define XTLS_EVENTS_ALL        31U
#define XTLS_FAILURE_NONE      0U
#define XTLS_FAILURE_TRUST     1U
#define XTLS_FAILURE_VERIFY    2U
#define XTLS_FAILURE_ENTROPY   3U
#define XTLS_FAILURE_CLOCK     4U
#define XTLS_FAILURE_PROTOCOL  5U
#define XTLS_FAILURE_TRANSPORT 6U
#define XTLS_CERT_EXPIRED      1U
#define XTLS_CERT_FUTURE       2U
#define XTLS_CERT_NAME         4U
#define XTLS_CERT_UNTRUSTED    8U
#define XTLS_CERT_OTHER        16U
#define XTLS_MAX_CA_BYTES      (4U * 1024U * 1024U)

// Input metadata/CA bytes are copied by Start. NULL trust inputs select the
// system CA bundle. Loading a CA file uses the caller's existing file rights;
// explicitly request file access first when consent is needed. No verify bypass.
typedef struct XTlsConnectDesc
{
    uint32_t    size, version;
    const char *host;
    const char *server_name;            // NULL uses host. Certificate name and SNI.
    uint32_t    port, total_timeout_ms; // 0 timeout selects 30000 ms.
    const void *ca_data;
    uint64_t    ca_bytes;
    const char *ca_file; // Mutually exclusive with ca_data.
    XNetHandle  cancel;
    uint64_t    reserved[2];
} XTlsConnectDesc;

typedef struct XTlsPeerInfo
{
    uint32_t     size, version, state, events;
    int64_t      result;
    int32_t      backend_error;
    uint32_t     failure, verify_flags, buffered_bytes;
    XNetEndpoint peer;
    char         server_name[256], protocol[16], cipher[96];
} XTlsPeerInfo;

// Cooperative progress: Wait/Send/Receive drive the operation; Start creates
// an owned operation without spawning a user thread. All returns are negative
// errno or success; Wait returns 1 ready / 0 timeout. io follows XNet semantics.
// Same-object overlapping progress/query returns EBUSY. Close/Cancel can wake
// an active call. SDK objects must not be passed to NetWaitMany/NetClose.
XNetResult xapi_TlsConnectStart(const XTlsConnectDesc *desc, XTlsHandle *tls);
XNetResult xapi_TlsWait(XTlsHandle tls, uint32_t mask, const XNetIoOptions *io, uint32_t *events);
XNetResult xapi_TlsGetPeerInfo(XTlsHandle tls, XTlsPeerInfo *info);
// A nonblocking Send returning EAGAIN retains an owned copy of the attempted
// prefix (at most 16384 bytes). Retry the same bytes; a different prefix gives
// EBUSY. A finite write timeout terminates the connection. No user pointer is kept.
XNetResult xapi_TlsSend(XTlsHandle tls, const void *data, uint64_t bytes, const XNetIoOptions *io);
XNetResult xapi_TlsReceive(XTlsHandle tls, void *data, uint64_t capacity, const XNetIoOptions *io);
// Send close_notify; Receive remains available. Close itself cancels immediately.
XNetResult xapi_TlsShutdown(XTlsHandle tls, const XNetIoOptions *io);
XNetResult xapi_TlsCancel(XTlsHandle tls);
XNetResult xapi_TlsClose(XTlsHandle tls);

#ifdef __cplusplus
}
#endif
