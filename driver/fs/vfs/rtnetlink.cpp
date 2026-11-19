// SPDX-License-Identifier: Apache-2.0
#define ALL_IMPLEMENTATION
#include <cpu/lock.h>
#include <errno.h>
#include <krlibc.h>
#include <net/rtnetlink.h>
#include <task/poll.h>

namespace {
constexpr uint16_t NL_REQUEST = 1, NL_MULTI = 2, NL_DUMP = 0x300;
constexpr uint16_t NL_ERROR = 2, NL_DONE = 3;
constexpr uint16_t RT_NEWLINK = 16, RT_GETLINK = 18, RT_NEWADDR = 20, RT_GETADDR = 22;
constexpr uint16_t RT_NEWROUTE = 24, RT_GETROUTE = 26;
constexpr uint16_t ROUTE_ATTR_OIF = 4, ROUTE_ATTR_GATEWAY = 5, ROUTE_ATTR_PRIORITY = 6, ROUTE_ATTR_PREFSRC = 7;
constexpr uint8_t  ROUTE_TABLE_MAIN = 254, ROUTE_SCOPE_UNIVERSE = 0, ROUTE_TYPE_UNICAST = 1;
constexpr uint64_t LINUX_PEEK = 2, LINUX_DONTWAIT = 0x40;
constexpr size_t   RESPONSE_BYTES = 2048, QUEUE_LIMIT = 8;

struct Header
{
    uint32_t length;
    uint16_t type, flags;
    uint32_t sequence, port;
};
struct Link
{
    uint8_t  family, pad;
    uint16_t hardware;
    int32_t  index;
    uint32_t flags, change;
};
struct Address
{
    uint8_t  family, prefix, flags, scope;
    uint32_t index;
};
struct Route
{
    uint8_t  family, destination_prefix, source_prefix, tos;
    uint8_t  table, protocol, scope, type;
    uint32_t flags;
};
struct Attribute
{
    uint16_t length, type;
};
static_assert(sizeof(Header) == 16 && sizeof(Link) == 16 && sizeof(Address) == 8 && sizeof(Route) == 12);
static_assert(sizeof(sockaddr_nl) == 12);

struct Response
{
    Response *next;
    size_t    length;
    size_t    offset, completion;
    uint8_t   bytes[RESPONSE_BYTES];
};
struct RouteSocket
{
    spin_t    lock;
    uint32_t  port;
    int       type;
    bool      bound;
    size_t    queued;
    Response *head, *tail;
};
static uint32_t next_port = 0x80000000U;

size_t aligned(size_t size)
{
    return (size + 3) & ~size_t(3);
}

// Builders operate on a private allocation. Only a finished datagram is
// published under the endpoint lock; no allocator/provider runs under it.
struct Builder
{
    Response *response;
    size_t    message = 0;
    bool      valid   = true;

    void append(const void *data, size_t size)
    {
        size_t padded = aligned(size);
        if (padded > RESPONSE_BYTES - response->length)
        {
            valid = false;
            return;
        }
        memcpy(response->bytes + response->length, data, size);
        memset(response->bytes + response->length + size, 0, padded - size);
        response->length += padded;
    }
    void start(uint16_t type, const Header &request, uint32_t port)
    {
        message       = response->length;
        Header header = {sizeof(Header), type, NL_MULTI, request.sequence, port};
        append(&header, sizeof(header));
    }
    void attribute(uint16_t type, const void *data, size_t size)
    {
        Attribute attr = {uint16_t(sizeof(Attribute) + size), type};
        append(&attr, sizeof(attr));
        append(data, size);
    }
    void finish()
    {
        if (valid)
        {
            uint32_t length = uint32_t(response->length - message);
            memcpy(response->bytes + message, &length, sizeof(length));
        }
    }
};

void link(Builder &out, const Header &request, uint32_t port, uint32_t index, const char *name, const uint8_t mac[6],
          uint32_t mtu, bool loopback, bool up)
{
    out.start(RT_NEWLINK, request, port);
    Link value = {
        0, 0, uint16_t(loopback ? 772 : 1), int32_t(index), uint32_t(1 | (loopback ? 8 : 2 | 0x1000) | (up ? 0x40 : 0)),
        0};
    out.append(&value, sizeof(value));
    out.attribute(3, name, strlen(name) + 1); // IFLA_IFNAME
    out.attribute(1, mac, 6);                 // IFLA_ADDRESS
    uint8_t broadcast[6] = {};
    if (!loopback) memset(broadcast, 0xff, sizeof(broadcast));
    out.attribute(2, broadcast, sizeof(broadcast));
    out.attribute(4, &mtu, sizeof(mtu));
    out.finish();
}

void address(Builder &out, const Header &request, uint32_t port, uint32_t index, const char *name, int family,
             uint8_t prefix, uint8_t scope, const void *bytes, const void *broadcast = NULL)
{
    out.start(RT_NEWADDR, request, port);
    Address value = {uint8_t(family), prefix, 0x80, scope, index}; // IFA_F_PERMANENT
    out.append(&value, sizeof(value));
    size_t length = family == AF_INET ? 4 : 16;
    out.attribute(1, bytes, length);                        // IFA_ADDRESS
    if (family == AF_INET) out.attribute(2, bytes, length); // IFA_LOCAL
    if (broadcast) out.attribute(4, broadcast, length);
    out.attribute(3, name, strlen(name) + 1);
    out.finish();
}

void default_route(Builder &out, const Header &request, uint32_t port, uint8_t family, uint32_t index,
                   const void *gateway, size_t gateway_size, const void *preferred_source = NULL)
{
    out.start(RT_NEWROUTE, request, port);
    Route value = {family, 0, 0, 0, ROUTE_TABLE_MAIN, 0, ROUTE_SCOPE_UNIVERSE, ROUTE_TYPE_UNICAST, 0};
    out.append(&value, sizeof(value));
    uint32_t metric = 0;
    out.attribute(ROUTE_ATTR_OIF, &index, sizeof(index));
    out.attribute(ROUTE_ATTR_GATEWAY, gateway, gateway_size);
    if (preferred_source) out.attribute(ROUTE_ATTR_PREFSRC, preferred_source, sizeof(uint32_t));
    out.attribute(ROUTE_ATTR_PRIORITY, &metric, sizeof(metric));
    out.finish();
}

bool nonzero(const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        if (bytes[i]) return true;
    return false;
}

int create(int domain, int type, int protocol, void **result)
{
    if (!result) return -EINVAL;
    *result = NULL;
    if (domain != AF_NETLINK) return -EAFNOSUPPORT;
    if (protocol != 0) return -EPROTONOSUPPORT; // NETLINK_ROUTE
    if (type != SOCK_RAW && type != SOCK_DGRAM) return -ESOCKTNOSUPPORT;
    auto *socket = static_cast<RouteSocket *>(calloc(1, sizeof(RouteSocket)));
    if (!socket) return -ENOMEM;
    socket->lock = SPIN_INIT;
    socket->type = type;
    socket->port = __atomic_add_fetch(&next_port, uint32_t(1), __ATOMIC_RELAXED);
    if (!socket->port) socket->port = __atomic_add_fetch(&next_port, uint32_t(1), __ATOMIC_RELAXED);
    *result = socket;
    return 0;
}

int close(void *implementation)
{
    auto     *socket = static_cast<RouteSocket *>(implementation);
    // socketfs/FdOperation retire the implementation after its last user.
    // There is no peer retaining this local request/response endpoint.
    Response *response = socket->head;
    while (response)
    {
        Response *next = response->next;
        free(response);
        response = next;
    }
    free(socket);
    return 0;
}

int bind(void *implementation, const sockaddr *address, socklen_t length)
{
    if (!address || length < sizeof(sockaddr_nl)) return -EINVAL;
    auto *value = reinterpret_cast<const sockaddr_nl *>(address);
    if (value->nl_family != AF_NETLINK) return -EAFNOSUPPORT;
    // Only kernel query clients are supported; no user port namespace or
    // multicast subscriptions are registered by this read-only backend.
    if (value->nl_pid || value->nl_groups) return -EOPNOTSUPP;
    auto *socket = static_cast<RouteSocket *>(implementation);
    spin_lock(&socket->lock);
    int error = socket->bound ? -EINVAL : 0;
    if (!error) socket->bound = true;
    spin_unlock(&socket->lock);
    return error;
}

int getsockname(void *implementation, sockaddr *destination, socklen_t *length)
{
    if (!destination || !length) return -EINVAL;
    auto *socket = static_cast<RouteSocket *>(implementation);
    spin_lock(&socket->lock);
    sockaddr_nl value = {AF_NETLINK, 0, socket->bound ? socket->port : 0, 0};
    spin_unlock(&socket->lock);
    memcpy(destination, &value, MIN(size_t(*length), sizeof(value)));
    *length = sizeof(value);
    return 0;
}

ssize_t sendto(void *implementation, const void *buffer, size_t size, uint64_t flags, const sockaddr *destination,
               socklen_t length)
{
    (void)flags;
    if (!buffer || size < sizeof(Header) + 1) return -EINVAL;
    if (destination)
    {
        if (length < sizeof(sockaddr_nl)) return -EINVAL;
        auto *value = reinterpret_cast<const sockaddr_nl *>(destination);
        if (value->nl_family != AF_NETLINK) return -EAFNOSUPPORT;
        if (value->nl_pid || value->nl_groups) return -EOPNOTSUPP;
    }
    Header request;
    memcpy(&request, buffer, sizeof(request));
    if (request.length < sizeof(Header) + 1 || request.length > size || aligned(request.length) != aligned(size))
        return -EINVAL;
    if (!(request.flags & NL_REQUEST)) return -EINVAL;
    auto *socket = static_cast<RouteSocket *>(implementation);
    spin_lock(&socket->lock);
    socket->bound = true;
    uint32_t port = socket->port;
    spin_unlock(&socket->lock);

    socket_netinfo_t info                = {};
    bool             ethernet            = socket_get_netinfo(&info) == 0 && info.stack_ready && info.ifname[0];
    info.ifname[sizeof(info.ifname) - 1] = '\0';
    auto *response                       = static_cast<Response *>(calloc(1, sizeof(Response)));
    if (!response) return -ENOMEM;
    Builder out    = {response};
    uint8_t family = static_cast<const uint8_t *>(buffer)[sizeof(Header)];
    int     error  = 0;
    if ((request.flags & NL_DUMP) != NL_DUMP)
        error = -EOPNOTSUPP;
    else if (request.type == RT_GETLINK)
    {
        const uint8_t zero[6] = {};
        link(out, request, port, 1, "lo", zero, 65536, true, true);
        if (ethernet) link(out, request, port, 2, info.ifname, info.mac, info.mtu, false, info.link_up);
    }
    else if (request.type == RT_GETADDR)
    {
        if (family == AF_UNSPEC || family == AF_INET)
        {
            const uint8_t loopback[4] = {127, 0, 0, 1};
            address(out, request, port, 1, "lo", AF_INET, 8, 254, loopback);
            if (ethernet && info.ipv4_addr)
            {
                const auto *mask   = reinterpret_cast<const uint8_t *>(&info.ipv4_netmask);
                uint8_t     prefix = 0;
                for (size_t i = 0; i < 4; ++i)
                    for (unsigned bit = 0; bit < 8; ++bit)
                        prefix += (mask[i] >> bit) & 1;
                uint32_t broadcast = info.ipv4_addr | ~info.ipv4_netmask;
                address(out, request, port, 2, info.ifname, AF_INET, prefix, 0, &info.ipv4_addr, &broadcast);
            }
        }
        if (ethernet && (family == AF_UNSPEC || family == AF_INET6))
        {
            if (info.ipv6_linklocal_ready)
                address(out, request, port, 2, info.ifname, AF_INET6, 64, 253, info.ipv6_linklocal);
            // The module exposes SLAAC addresses; its current IPv6 interface
            // configuration uses the 64-bit prefix, with no static prefix API.
            if (info.ipv6_global_ready) address(out, request, port, 2, info.ifname, AF_INET6, 64, 0, info.ipv6_global);
        }
    }
    else if (request.type == RT_GETROUTE)
    {
        if (request.length < sizeof(Header) + sizeof(Route))
            error = -EINVAL;
        else if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6)
            error = -EAFNOSUPPORT;
        else
        {
            if (ethernet && info.ipv4_addr && info.ipv4_gateway && (family == AF_UNSPEC || family == AF_INET))
                default_route(out, request, port, AF_INET, 2, &info.ipv4_gateway, sizeof(info.ipv4_gateway),
                              &info.ipv4_addr);
            if (ethernet && nonzero(info.ipv6_gateway, sizeof(info.ipv6_gateway)) &&
                (family == AF_UNSPEC || family == AF_INET6))
                default_route(out, request, port, AF_INET6, 2, info.ipv6_gateway, sizeof(info.ipv6_gateway));
        }
    }
    else
        error = -EOPNOTSUPP;
    if (error)
    {
        response->length = 0;
        out.start(NL_ERROR, request, port);
        out.append(&error, sizeof(error));
        out.append(&request, sizeof(request));
        out.finish();
    }
    else
    {
        // Keep route records and NLMSG_DONE in distinct datagrams. Some Linux
        // clients stop scanning a packet once they find a zero-metric route
        // and receive again for completion. Retain both parts in one queued
        // request so queue pressure cannot publish a dump without its end.
        if (request.type == RT_GETROUTE) response->completion = response->length;
        out.start(NL_DONE, request, port);
        int success = 0;
        out.append(&success, sizeof(success));
        out.finish();
    }
    if (!out.valid)
    {
        free(response);
        return -EMSGSIZE;
    }
    spin_lock(&socket->lock);
    bool full = socket->queued >= QUEUE_LIMIT;
    if (!full)
    {
        if (socket->tail)
            socket->tail->next = response;
        else
            socket->head = response;
        socket->tail = response;
        ++socket->queued;
    }
    spin_unlock(&socket->lock);
    if (full)
    {
        free(response);
        return -ENOBUFS;
    }
    return ssize_t(size);
}

ssize_t recvfrom(void *implementation, void *buffer, size_t size, uint64_t flags, sockaddr *source, socklen_t *length)
{
    if (!buffer || (source && !length)) return -EINVAL;
    auto *socket = static_cast<RouteSocket *>(implementation);
    for (;;)
    {
        spin_lock(&socket->lock);
        Response *response = socket->head;
        if (!response)
        {
            spin_unlock(&socket->lock);
            if (flags & (O_NONBLOCK | LINUX_DONTWAIT)) return -EAGAIN;
            scheduler_yield();
            continue;
        }
        size_t packet_end = response->completion && !response->offset ? response->completion : response->length;
        size_t received   = packet_end - response->offset;
        // Reject a short buffer without silently losing a partial datagram. The
        // legacy provider ABI cannot return recvmsg's MSG_TRUNC output flag.
        if (size < received)
        {
            spin_unlock(&socket->lock);
            return -EMSGSIZE;
        }
        memcpy(buffer, response->bytes + response->offset, received);
        bool retire = false;
        if (!(flags & LINUX_PEEK))
        {
            response->offset = packet_end;
            if (response->offset == response->length)
            {
                socket->head = response->next;
                if (!socket->head) socket->tail = NULL;
                --socket->queued;
                retire = true;
            }
        }
        spin_unlock(&socket->lock);
        if (retire) free(response);
        if (source)
        {
            sockaddr_nl value = {AF_NETLINK, 0, 0, 0};
            memcpy(source, &value, MIN(size_t(*length), sizeof(value)));
            *length = sizeof(value);
        }
        return ssize_t(received);
    }
}

ssize_t read(void *socket, void *buffer, size_t size, uint64_t flags)
{
    return recvfrom(socket, buffer, size, flags & O_NONBLOCK, NULL, NULL);
}

ssize_t write(void *socket, const void *buffer, size_t size, uint64_t flags)
{
    return sendto(socket, buffer, size, flags, NULL, 0);
}

int connect(void *, const sockaddr *, socklen_t)
{
    return -EOPNOTSUPP;
}

int getpeername(void *, sockaddr *, socklen_t *)
{
    return -ENOTCONN;
}

int getsockopt(void *implementation, int level, int name, void *value, socklen_t *length)
{
    if (!value || !length) return -EINVAL;
    if (level != SOL_SOCKET || (name != SO_TYPE && name != SO_ERROR)) return -ENOPROTOOPT;
    auto *socket = static_cast<RouteSocket *>(implementation);
    int   result = name == SO_TYPE ? socket->type : 0;
    memcpy(value, &result, MIN(size_t(*length), sizeof(result)));
    *length = sizeof(result);
    return 0;
}

int listen(void *, int)
{
    return -EOPNOTSUPP;
}

int accept(void *, void **, sockaddr *, socklen_t *)
{
    return -EOPNOTSUPP;
}

int shutdown(void *, int)
{
    return -EOPNOTSUPP;
}

int set_flags(void *, uint64_t)
{
    return 0;
}

int poll(void *implementation, size_t events)
{
    auto *socket = static_cast<RouteSocket *>(implementation);
    spin_lock(&socket->lock);
    int ready = (socket->head ? POLLIN : 0) | (socket->queued < QUEUE_LIMIT ? POLLOUT : 0);
    spin_unlock(&socket->lock);
    return int(events) & ready;
}
} // namespace

const socket_provider_t route_netlink_provider = {
    .create      = create,
    .close       = close,
    .read        = read,
    .write       = write,
    .recvfrom    = recvfrom,
    .sendto      = sendto,
    .bind        = bind,
    .connect     = connect,
    .getsockname = getsockname,
    .getpeername = getpeername,
    .getsockopt  = getsockopt,
    .listen      = listen,
    .accept      = accept,
    .shutdown    = shutdown,
    .poll        = poll,
    .set_flags   = set_flags,
};

int route_netlink_open(int type, int protocol)
{
    void *implementation = NULL;
    int   error          = create(AF_NETLINK, type, protocol, &implementation);
    if (error) return error;
    return socketfs_alloc_fd_with_provider(implementation, 0, &route_netlink_provider);
}
