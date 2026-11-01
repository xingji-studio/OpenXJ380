// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t XNetHandle;
typedef int64_t  XNetResult;
typedef uint32_t XNetInterfaceId;

#define XAPI_NET                7503
#define XNET_ABI_VERSION        1U
#define XNET_INVALID_HANDLE     0ULL
#define XNET_IPV4               1U
#define XNET_IPV6               2U
#define XNET_TCP                1U
#define XNET_UDP                2U
#define XNET_MAX_WAIT_ITEMS     64U
#define XNET_MAX_HOST_BYTES     254U
#define XNET_MAX_TRANSFER_BYTES 65536U
#define XNET_MAX_OBJECTS        256U
#define XNET_CANCELLED          (-125LL) // -ECANCELED, including older SDK errno headers.

#define XNET_CAP_IPV4           1ULL
#define XNET_CAP_IPV6           2ULL
#define XNET_CAP_TCP            4ULL
#define XNET_CAP_UDP            8ULL
#define XNET_CAP_DNS            16ULL
#define XNET_CAP_TCP_NODELAY    32ULL
#define XNET_CAP_TCP_KEEPALIVE  64ULL
#define XNET_CAP_UDP_BROADCAST  128ULL
#define XNET_CAP_RUNTIME_CONFIG 256ULL

#define XNET_CREATED      1U
#define XNET_BOUND        2U
#define XNET_LISTENING    3U
#define XNET_CONNECTING   4U
#define XNET_CONNECTED    5U
#define XNET_FAILED       6U
#define XNET_READ_CLOSED  1U
#define XNET_WRITE_CLOSED 2U
#define XNET_SHUT_RECEIVE 1U
#define XNET_SHUT_SEND    2U
#define XNET_SHUT_BOTH    3U

#define XNET_READABLE             1U
#define XNET_WRITABLE             2U
#define XNET_ACCEPTABLE           4U
#define XNET_CONNECTION_READY     8U
#define XNET_DONE                 16U
#define XNET_CHANGED              32U
#define XNET_ERROR                64U
#define XNET_CLOSED               128U
#define XNET_INVALID              256U
#define XNET_EVENTS_ALL           511U
#define XNET_DATAGRAM_TRUNCATED   1U
#define XNET_CONFIG_STATIC        1U
#define XNET_CONFIG_DHCP          2U
#define XNET_DNS_AUTO             1U
#define XNET_DNS_MANUAL           2U
#define XNET_LINK_UP              1U
#define XNET_STACK_READY          2U
#define XNET_ADDRESS_READY        4U
#define XNET_DEFAULT_ROUTE        8U
#define XNET_DNS_CONFIGURED       16U
#define XNET_CONNECTIVITY_UNKNOWN 0U
#define XNET_CHANGE_RESYNC        1U

typedef struct XNetAddress
{
    uint32_t family, scope_id;
    uint8_t  bytes[16];
} XNetAddress;

typedef struct XNetEndpoint
{
    XNetAddress address;
    uint16_t    port, reserved16;
    uint32_t    reserved32;
} XNetEndpoint;

typedef struct XNetIoOptions
{
    uint32_t   size, version;
    int32_t    timeout_ms;
    uint32_t   reserved;
    XNetHandle cancel;
} XNetIoOptions;

typedef struct XNetOpenDesc
{
    uint32_t        size, version, family, transport;
    XNetInterfaceId interface_id;
    uint32_t        flags;
    uint64_t        reserved[2];
} XNetOpenDesc;

typedef struct XNetCapabilities
{
    uint32_t size, version, backend_present, max_objects;
    uint64_t features;
    uint32_t max_wait_items, max_transfer_bytes, max_dns_results, interface_count;
    uint64_t reserved[2];
} XNetCapabilities;

typedef struct XNetSocketInfo
{
    uint32_t     size, version, state, events;
    uint32_t     family, transport, interface_id, shutdown;
    int64_t      connection_result;
    XNetEndpoint local, peer;
    uint64_t     reserved[2];
} XNetSocketInfo;

typedef struct XNetDatagramInfo
{
    uint32_t     size, version, interface_id, flags;
    uint64_t     original_bytes;
    XNetEndpoint source;
} XNetDatagramInfo;

typedef struct XNetWaitItem
{
    uint32_t   size, version;
    XNetHandle object;
    uint32_t   events, ready;
    uint64_t   tag;
} XNetWaitItem;

typedef struct XNetTcpOptions
{
    uint32_t size, version, no_delay, keepalive;
    uint32_t keep_idle_ms, keep_interval_ms, keep_count, reserved;
} XNetTcpOptions;

typedef struct XNetUdpOptions
{
    uint32_t size, version, broadcast, reserved;
} XNetUdpOptions;

// C ABI pointers below are read only by the syscall entry, never by a worker.
// Start copies hostname (NUL terminated, at most 253 bytes). family=0 selects
// supported families. total_timeout_ms must be positive; it bounds the task,
// independently of any WaitMany timeout. Native V1 returns one IPv4 candidate.
typedef struct XNetResolveDesc
{
    uint32_t    size, version;
    const char *hostname;
    uint32_t    family, interface_id;
    uint16_t    port, reserved16;
    uint32_t    total_timeout_ms;
    XNetHandle  cancel;
    uint64_t    reserved[2];
} XNetResolveDesc;

typedef struct XNetInterfaceAddress
{
    XNetAddress address;
    uint32_t    prefix_bits, reserved;
} XNetInterfaceAddress;

typedef struct XNetInterfaceInfo
{
    uint32_t    size, version, interface_id, flags;
    uint32_t    mtu, ipv4_method, dhcp_state, connectivity;
    uint8_t     mac[6], reserved8[2];
    char        name[16];
    uint64_t    generation;
    XNetAddress gateway;
} XNetInterfaceInfo;

typedef struct XNetInterfaceConfig
{
    uint32_t    size, version, interface_id, method;
    XNetAddress address, gateway;
    uint32_t    prefix_bits, reserved;
} XNetInterfaceConfig;

typedef struct XNetDnsConfig
{
    uint32_t    size, version, interface_id, method;
    uint32_t    server_count, reserved;
    XNetAddress servers[2];
} XNetDnsConfig;

typedef struct XNetChange
{
    uint32_t size, version, interface_id, flags;
    uint64_t generation;
} XNetChange;

// All results are 0 or negative errno, except byte-count I/O and WaitMany.
// Set size/version on descriptors AND output structures; reserved fields zero.
// IPv4 occupies bytes[0..3] in network order. Port is a host-order integer.
// All objects belong to this process/image. fork does not inherit native
// handles; exec/exit revoke them. NetClose accepts every native object kind.
XNetResult xapi_NetGetCapabilities(XNetCapabilities *caps);
XNetResult xapi_NetParseAddress(const char *text, XNetAddress *address);
XNetResult xapi_NetFormatAddress(const XNetAddress *address, char *text, uint32_t capacity);
XNetResult xapi_NetOpen(const XNetOpenDesc *desc, XNetHandle *socket);
XNetResult xapi_NetBind(XNetHandle socket, const XNetEndpoint *local);
XNetResult xapi_NetListen(XNetHandle socket, uint32_t backlog);
XNetResult xapi_NetAccept(XNetHandle socket, XNetHandle *accepted, XNetEndpoint *peer, const XNetIoOptions *io);
XNetResult xapi_NetConnect(XNetHandle socket, const XNetEndpoint *peer, const XNetIoOptions *io);
// NULL io: immediate. timeout_ms: 0 immediate, -1 infinite, >0 total milliseconds.
// Connect(0) starts an asynchronous attempt and returns -EINPROGRESS if pending.
// Receive(capacity>0)==0 means TCP EOF; UDP ReceiveFrom==0 can be an empty packet.
// TCP I/O may be partial. UDP sends are atomic; ReceiveFrom reports truncation.
XNetResult xapi_NetSend(XNetHandle socket, const void *data, uint64_t bytes, const XNetIoOptions *io);
XNetResult xapi_NetReceive(XNetHandle socket, void *buffer, uint64_t capacity, const XNetIoOptions *io);
XNetResult xapi_NetSendTo(XNetHandle socket, const XNetEndpoint *peer, const void *data, uint64_t bytes,
                          const XNetIoOptions *io);
XNetResult xapi_NetReceiveFrom(XNetHandle socket, void *buffer, uint64_t capacity, XNetDatagramInfo *info,
                               const XNetIoOptions *io);
XNetResult xapi_NetSendAll(XNetHandle socket, const void *data, uint64_t bytes, const XNetIoOptions *io,
                           uint64_t *sent);
XNetResult xapi_NetShutdown(XNetHandle socket, uint32_t direction);
XNetResult xapi_NetQuery(XNetHandle socket, XNetSocketInfo *info);
XNetResult xapi_NetSetTcpOptions(XNetHandle socket, const XNetTcpOptions *options);
XNetResult xapi_NetGetTcpOptions(XNetHandle socket, XNetTcpOptions *options);
XNetResult xapi_NetSetUdpOptions(XNetHandle socket, const XNetUdpOptions *options);
XNetResult xapi_NetGetUdpOptions(XNetHandle socket, XNetUdpOptions *options);
XNetResult xapi_NetClose(XNetHandle object);
// Level readiness, timeout returns 0; terminal conditions ignore interest masks.
// Closing a cancellation object also signals it. Cancellation is one-shot.
XNetResult xapi_NetWaitMany(XNetWaitItem *items, uint32_t count, const XNetIoOptions *io);
XNetResult xapi_NetCreateCancel(XNetHandle *cancel);
XNetResult xapi_NetSignalCancel(XNetHandle cancel);

#ifdef __cplusplus
}
#endif
