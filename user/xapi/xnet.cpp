// SPDX-License-Identifier: Apache-2.0
#include "include/xnet.h"
#include "include/errno.h"
#include "include/libsys.h"
#include "include/time.h"
#include "include/xnet_abi.h"
#include "include/xnet_dns.h"
#include "include/xnet_manager.h"

namespace {
XNetResult dispatch(uint32_t operation, XNetHandle object = 0, const void *input = 0, void *output = 0,
                    void *auxiliary = 0, uint64_t count = 0, uint64_t argument = 0, const XNetIoOptions *io = 0)
{
    XNetCall call   = {};
    call.size       = sizeof(call);
    call.version    = XNET_ABI_VERSION;
    call.operation  = operation;
    call.object     = object;
    call.input      = (uint64_t)input;
    call.output     = (uint64_t)output;
    call.auxiliary  = (uint64_t)auxiliary;
    call.count      = count;
    call.argument   = argument;
    call.io.size    = sizeof(call.io);
    call.io.version = XNET_ABI_VERSION;
    if (io) call.io = *io;
    return (XNetResult)enter_syscall(XAPI_NET, (uint64_t)&call, 0, 0, 0, 0, 0);
}

bool ipv4(const char *start, const char *end, uint8_t *bytes)
{
    for (unsigned part = 0; part < 4; ++part)
    {
        unsigned value = 0, digits = 0;
        while (start != end && *start >= '0' && *start <= '9')
        {
            value = value * 10 + unsigned(*start++ - '0');
            if (++digits > 3 || value > 255) return false;
        }
        if (!digits) return false;
        bytes[part] = uint8_t(value);
        if (part != 3)
        {
            if (start == end || *start++ != '.') return false;
        }
    }
    return start == end;
}

int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *decimal(char *out, uint32_t value)
{
    char     digits[10];
    unsigned count = 0;
    do
    {
        digits[count++]  = char('0' + value % 10);
        value           /= 10;
    } while (value);
    while (count)
        *out++ = digits[--count];
    return out;
}

uint64_t monotonic_ms()
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) < 0) return ~uint64_t(0);
    return uint64_t(time.tv_sec) * 1000 + uint64_t(time.tv_nsec) / 1000000;
}

} // namespace

extern "C" XNetResult xapi_NetParseAddress(const char *text, XNetAddress *out)
{
    if (!text || !out) return -EINVAL;
    const char *end = text;
    while (*end && end - text < 80)
        ++end;
    if (*end || end == text) return -EINVAL;
    XNetAddress address = {};
    if (ipv4(text, end, address.bytes))
    {
        address.family = XNET_IPV4;
        *out           = address;
        return 0;
    }
    const char *scope = text;
    while (scope != end && *scope != '%')
        ++scope;
    if (scope != end)
    {
        if (scope + 1 == end) return -EINVAL;
        uint64_t value = 0;
        for (const char *p = scope + 1; p != end; ++p)
        {
            if (*p < '0' || *p > '9') return -EINVAL;
            value = value * 10 + unsigned(*p - '0');
            if (value > 0xffffffffULL) return -EINVAL;
        }
        if (!value) return -EINVAL;
        address.scope_id = uint32_t(value);
        end              = scope;
    }
    uint16_t    words[8] = {};
    unsigned    count    = 0;
    int         gap      = -1;
    const char *p        = text;
    if (p != end && *p == ':')
    {
        if (p + 1 == end || p[1] != ':') return -EINVAL;
        gap  = 0;
        p   += 2;
    }
    while (p != end)
    {
        if (count == 8) return -EINVAL;
        const char *segment = p;
        bool        dotted  = false;
        while (p != end && *p != ':')
        {
            dotted |= *p == '.';
            ++p;
        }
        if (dotted)
        {
            uint8_t tail[4];
            if (p != end || count > 6 || !ipv4(segment, end, tail)) return -EINVAL;
            words[count++] = uint16_t(uint16_t(tail[0]) * 256 + tail[1]);
            words[count++] = uint16_t(uint16_t(tail[2]) * 256 + tail[3]);
            break;
        }
        if (p == segment || p - segment > 4) return -EINVAL;
        unsigned value = 0;
        for (const char *digit = segment; digit != p; ++digit)
        {
            int nibble = hex(*digit);
            if (nibble < 0) return -EINVAL;
            value = value * 16 + unsigned(nibble);
        }
        words[count++] = uint16_t(value);
        if (p == end) break;
        if (++p == end) return -EINVAL;
        if (*p == ':')
        {
            if (gap >= 0) return -EINVAL;
            gap = int(count);
            ++p;
        }
    }
    if (gap < 0 && count != 8) return -EINVAL;
    if (gap >= 0)
    {
        if (count >= 8) return -EINVAL;
        unsigned zeros = 8 - count;
        for (int i = int(count) - 1; i >= gap; --i)
            words[i + zeros] = words[i];
        for (unsigned i = unsigned(gap); i < unsigned(gap) + zeros; ++i)
            words[i] = 0;
    }
    address.family = XNET_IPV6;
    for (unsigned i = 0; i < 8; ++i)
    {
        address.bytes[i * 2]     = uint8_t(words[i] >> 8);
        address.bytes[i * 2 + 1] = uint8_t(words[i]);
    }
    *out = address;
    return 0;
}

extern "C" XNetResult xapi_NetFormatAddress(const XNetAddress *address, char *text, uint32_t capacity)
{
    if (!address || !text || !capacity) return -EINVAL;
    char buffer[64], *out = buffer;
    if (address->family == XNET_IPV4)
    {
        if (address->scope_id) return -EINVAL;
        for (unsigned i = 4; i < 16; ++i)
            if (address->bytes[i]) return -EINVAL;
        for (unsigned i = 0; i < 4; ++i)
        {
            if (i) *out++ = '.';
            out = decimal(out, address->bytes[i]);
        }
    }
    else if (address->family == XNET_IPV6)
    {
        uint16_t words[8];
        for (unsigned i = 0; i < 8; ++i)
            words[i] = uint16_t(address->bytes[i * 2] * 256U + address->bytes[i * 2 + 1]);
        unsigned best = 8, length = 1;
        for (unsigned i = 0; i < 8;)
        {
            if (words[i])
            {
                ++i;
                continue;
            }
            unsigned start = i;
            while (i < 8 && !words[i])
                ++i;
            if (i - start > length)
            {
                best   = start;
                length = i - start;
            }
        }
        static const char digits[] = "0123456789abcdef";
        for (unsigned i = 0; i < 8;)
        {
            if (i == best)
            {
                *out++  = ':';
                *out++  = ':';
                i      += length;
                continue;
            }
            if (out != buffer && out[-1] != ':') *out++ = ':';
            bool emitted = false;
            for (int shift = 12; shift >= 0; shift -= 4)
            {
                unsigned digit = (words[i] >> shift) & 15U;
                if (digit || emitted || !shift)
                {
                    *out++  = digits[digit];
                    emitted = true;
                }
            }
            ++i;
        }
        if (address->scope_id)
        {
            *out++ = '%';
            out    = decimal(out, address->scope_id);
        }
    }
    else
        return -EAFNOSUPPORT;
    uint32_t bytes = uint32_t(out - buffer);
    if (capacity <= bytes) return -ENOBUFS;
    for (uint32_t i = 0; i < bytes; ++i)
        text[i] = buffer[i];
    text[bytes] = 0;
    return 0;
}

extern "C" XNetResult xapi_NetGetCapabilities(XNetCapabilities *p)
{
    return dispatch(XNET_OP_CAPS, 0, 0, p);
}

extern "C" XNetResult xapi_NetOpen(const XNetOpenDesc *p, XNetHandle *out)
{
    return dispatch(XNET_OP_OPEN, 0, p, out);
}

extern "C" XNetResult xapi_NetBind(XNetHandle h, const XNetEndpoint *p)
{
    return dispatch(XNET_OP_BIND, h, p);
}

extern "C" XNetResult xapi_NetListen(XNetHandle h, uint32_t backlog)
{
    return dispatch(XNET_OP_LISTEN, h, 0, 0, 0, 0, backlog);
}

extern "C" XNetResult xapi_NetAccept(XNetHandle h, XNetHandle *out, XNetEndpoint *peer, const XNetIoOptions *io)
{
    return dispatch(XNET_OP_ACCEPT, h, 0, out, peer, 0, 0, io);
}

extern "C" XNetResult xapi_NetConnect(XNetHandle h, const XNetEndpoint *peer, const XNetIoOptions *io)
{
    return dispatch(XNET_OP_CONNECT, h, peer, 0, 0, 0, 0, io);
}

extern "C" XNetResult xapi_NetSend(XNetHandle h, const void *p, uint64_t n, const XNetIoOptions *io)
{
    return dispatch(XNET_OP_SEND, h, p, 0, 0, n, 0, io);
}

extern "C" XNetResult xapi_NetReceive(XNetHandle h, void *p, uint64_t n, const XNetIoOptions *io)
{
    return dispatch(XNET_OP_RECEIVE, h, 0, p, 0, n, 0, io);
}

extern "C" XNetResult xapi_NetSendTo(XNetHandle h, const XNetEndpoint *peer, const void *p, uint64_t n,
                                     const XNetIoOptions *io)
{
    return dispatch(XNET_OP_SEND_TO, h, p, 0, (void *)peer, n, 0, io);
}

extern "C" XNetResult xapi_NetReceiveFrom(XNetHandle h, void *p, uint64_t n, XNetDatagramInfo *info,
                                          const XNetIoOptions *io)
{
    return dispatch(XNET_OP_RECEIVE_FROM, h, 0, p, info, n, 0, io);
}

extern "C" XNetResult xapi_NetShutdown(XNetHandle h, uint32_t direction)
{
    return dispatch(XNET_OP_SHUTDOWN, h, 0, 0, 0, 0, direction);
}

extern "C" XNetResult xapi_NetQuery(XNetHandle h, XNetSocketInfo *p)
{
    return dispatch(XNET_OP_QUERY, h, 0, p);
}

extern "C" XNetResult xapi_NetClose(XNetHandle h)
{
    return dispatch(XNET_OP_CLOSE, h);
}

extern "C" XNetResult xapi_NetWaitMany(XNetWaitItem *items, uint32_t n, const XNetIoOptions *io)
{
    return dispatch(XNET_OP_WAIT, 0, 0, items, 0, n, 0, io);
}

extern "C" XNetResult xapi_NetCreateCancel(XNetHandle *out)
{
    return dispatch(XNET_OP_CANCEL_CREATE, 0, 0, out);
}

extern "C" XNetResult xapi_NetSignalCancel(XNetHandle h)
{
    return dispatch(XNET_OP_CANCEL_SIGNAL, h);
}

extern "C" XNetResult xapi_NetResolveStart(const XNetResolveDesc *p, XNetHandle *out)
{
    return dispatch(XNET_OP_RESOLVE_START, 0, p, out);
}

extern "C" XNetResult xapi_NetResolveGetResult(XNetHandle h, XNetEndpoint *p, uint32_t n, uint32_t *required)
{
    return dispatch(XNET_OP_RESOLVE_RESULT, h, 0, p, required, n);
}

extern "C" XNetResult xapi_NetEnumInterfaces(XNetInterfaceId *p, uint32_t n, uint32_t *required)
{
    return dispatch(XNET_OP_ENUM, 0, 0, p, required, n);
}

extern "C" XNetResult xapi_NetGetInterfaceInfo(XNetInterfaceId id, XNetInterfaceInfo *p)
{
    return dispatch(XNET_OP_INTERFACE, 0, 0, p, 0, 0, id);
}

extern "C" XNetResult xapi_NetGetInterfaceAddresses(XNetInterfaceId id, XNetInterfaceAddress *p, uint32_t n,
                                                    uint32_t *required)
{
    return dispatch(XNET_OP_ADDRESSES, 0, 0, p, required, n, id);
}

extern "C" XNetResult xapi_NetGetDnsConfig(XNetDnsConfig *p)
{
    return dispatch(XNET_OP_DNS_GET, 0, 0, p);
}

extern "C" XNetResult xapi_NetApplyInterfaceConfig(const XNetInterfaceConfig *p)
{
    return dispatch(XNET_OP_CONFIG_APPLY, 0, p);
}

extern "C" XNetResult xapi_NetApplyDnsConfig(const XNetDnsConfig *p)
{
    return dispatch(XNET_OP_DNS_APPLY, 0, p);
}

extern "C" XNetResult xapi_NetWatchCreate(XNetInterfaceId id, XNetHandle *out)
{
    return dispatch(XNET_OP_WATCH_CREATE, 0, 0, out, 0, 0, id);
}

extern "C" XNetResult xapi_NetWatchRead(XNetHandle h, XNetChange *p)
{
    return dispatch(XNET_OP_WATCH_READ, h, 0, p);
}

extern "C" XNetResult xapi_NetSetTcpOptions(XNetHandle h, const XNetTcpOptions *p)
{
    return dispatch(XNET_OP_TCP_SET, h, p);
}

extern "C" XNetResult xapi_NetGetTcpOptions(XNetHandle h, XNetTcpOptions *p)
{
    return dispatch(XNET_OP_TCP_GET, h, 0, p);
}

extern "C" XNetResult xapi_NetSetUdpOptions(XNetHandle h, const XNetUdpOptions *p)
{
    return dispatch(XNET_OP_UDP_SET, h, p);
}

extern "C" XNetResult xapi_NetGetUdpOptions(XNetHandle h, XNetUdpOptions *p)
{
    return dispatch(XNET_OP_UDP_GET, h, 0, p);
}

extern "C" XNetResult xapi_NetSendAll(XNetHandle h, const void *data, uint64_t bytes, const XNetIoOptions *io,
                                      uint64_t *sent)
{
    if (!sent || bytes > 0x7fffffffffffffffULL) return -EINVAL;
    *sent                 = 0;
    XNetIoOptions options = {sizeof(options), XNET_ABI_VERSION, 0, 0, 0};
    if (io) options = *io;
    if (options.size != sizeof(options) || options.version != XNET_ABI_VERSION || options.reserved ||
        options.timeout_ms < -1)
        return -EINVAL;
    XNetSocketInfo info = {};
    info.size           = sizeof(info);
    info.version        = XNET_ABI_VERSION;
    XNetResult query    = xapi_NetQuery(h, &info);
    if (query < 0) return query;
    if (info.transport != XNET_TCP) return -EPROTOTYPE;
    int32_t  budget = options.timeout_ms;
    uint64_t start  = budget > 0 ? monotonic_ms() : 0;
    if (start == ~uint64_t(0)) return -EIO;
    while (*sent < bytes)
    {
        if (budget > 0)
        {
            uint64_t now = monotonic_ms();
            if (now == ~uint64_t(0)) return -EIO;
            uint64_t elapsed = now - start;
            if (elapsed >= uint32_t(budget)) return -ETIMEDOUT;
            options.timeout_ms = budget - int32_t(elapsed);
        }
        uint64_t remaining = bytes - *sent;
        if (remaining > XNET_MAX_TRANSFER_BYTES) remaining = XNET_MAX_TRANSFER_BYTES;
        XNetResult result = xapi_NetSend(h, (const uint8_t *)data + *sent, remaining, &options);
        if (result < 0) return result;
        if (!result || uint64_t(result) > remaining) return -EIO;
        *sent += uint64_t(result);
    }
    return 0;
}
