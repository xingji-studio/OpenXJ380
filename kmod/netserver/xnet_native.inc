// SPDX-License-Identifier: Apache-2.0
// Native provider adapter. Included by netserver.cpp to share its first-party
// netif and netconn glue; no lwIP source or legacy provider ABI is modified.
#include "../../include/net/xnet_address.h"
struct NativeSocket
{
    xj380_lwip_socket_t socket;
    uint32_t            state, family, transport, shutdown;
    int64_t             connection_result;
    bool                eof;
};
struct NativeDns
{
    unsigned    refs;
    int         done, result;
    XNetAddress address;
};

static int native_endpoint_ip(const XNetEndpoint *endpoint, ip_addr_t *ip, u16_t *port)
{
    if (!endpoint || endpoint->reserved16 || endpoint->reserved32) return -EINVAL;
    if (endpoint->address.family == XNET_IPV6)
    {
        ip6_addr_p_t packed;
        memcpy(&packed, endpoint->address.bytes, sizeof(packed));
        ip_addr_copy_from_ip6_packed(*ip, packed);
        if (ip6_addr_isipv4mappedipv6(ip_2_ip6(ip))) return -EAFNOSUPPORT;
        if (endpoint->address.scope_id > 1) return -ENODEV;
        if (endpoint->address.scope_id && !ip6_addr_has_scope(ip_2_ip6(ip), IP6_UNKNOWN)) return -EINVAL;
        // XNet scope 1 is the logical default interface, independent of lwIP's
        // internal netif number. An omitted link-local zone selects this NIC.
        ip6_addr_assign_zone(ip_2_ip6(ip), IP6_UNKNOWN, &g_netif);
        *port = endpoint->port;
        return 0;
    }
    if (endpoint->address.family != XNET_IPV4) return -EAFNOSUPPORT;
    if (endpoint->address.scope_id) return -EINVAL;
    for (unsigned i = 4; i < 16; ++i)
        if (endpoint->address.bytes[i]) return -EINVAL;
    ip4_addr_t addr;
    memcpy(&addr.addr, endpoint->address.bytes, 4);
    ip_addr_copy_from_ip4(*ip, addr);
    *port = endpoint->port;
    return 0;
}

static XNetEndpoint native_endpoint(const ip_addr_t *ip, u16_t port)
{
    XNetEndpoint endpoint   = {};
    if (IP_IS_V6(ip))
    {
        endpoint.address.family = XNET_IPV6;
        xj380_copy_ip6_to_bytes(ip_2_ip6(ip), endpoint.address.bytes);
        endpoint.address.scope_id = ip6_addr_has_scope(ip_2_ip6(ip), IP6_UNKNOWN) ? 1U : 0U;
        endpoint.port             = port;
        return endpoint;
    }
    endpoint.address.family = XNET_IPV4;
    memcpy(endpoint.address.bytes, &ip_2_ip4(ip)->addr, 4);
    endpoint.port = port;
    return endpoint;
}

static bool native_ipv6_ready()
{
    bool ready = false;
    LOCK_TCPIP_CORE();
    for (unsigned i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i)
        if (ip6_addr_isvalid(netif_ip6_addr_state(&g_netif, i)) && !ip6_addr_isany(netif_ip6_addr(&g_netif, i)))
            ready = true;
    UNLOCK_TCPIP_CORE();
    return ready;
}

static bool native_network_ready()
{
    return g_stack_initialized && (g_stack_ready || native_ipv6_ready());
}

static int native_open(const XNetOpenDesc *desc, void **out)
{
    if (desc->family != XNET_IPV4 && desc->family != XNET_IPV6) return -EAFNOSUPPORT;
    if (!g_stack_initialized) return -EAGAIN;
    auto *socket = static_cast<NativeSocket *>(calloc(1, sizeof(NativeSocket)));
    if (!socket) return -ENOMEM;
    enum netconn_type type = desc->family == XNET_IPV6
                                 ? (desc->transport == XNET_TCP ? NETCONN_TCP_IPV6 : NETCONN_UDP_IPV6)
                                 : (desc->transport == XNET_TCP ? NETCONN_TCP : NETCONN_UDP);
    socket->socket.conn    = netconn_new(type);
    if (!socket->socket.conn)
    {
        free(socket);
        return -ENOMEM;
    }
    netconn_set_nonblocking(socket->socket.conn, 1);
    if (desc->family == XNET_IPV6) netconn_set_ipv6only(socket->socket.conn, 1);
    socket->state     = XNET_CREATED;
    socket->family    = desc->family;
    socket->transport = desc->transport;
    *out              = socket;
    return 0;
}

static void native_close(void *impl)
{
    auto *socket = static_cast<NativeSocket *>(impl);
    if (!socket) return;
    struct netconn *conn = socket->socket.conn;
    // Abort before delete so Close/timeout cannot wait for FIN retransmission.
    // Listener PCBs have a distinct layout and must use netconn_delete directly.
    LOCK_TCPIP_CORE();
    if (socket->transport == XNET_TCP && conn->pcb.tcp && conn->pcb.tcp->state != LISTEN) tcp_abort(conn->pcb.tcp);
    UNLOCK_TCPIP_CORE();
    if (socket->socket.rx_buf) pbuf_free(socket->socket.rx_buf);
    netconn_delete(conn);
    free(socket);
}

static int native_bind(void *impl, const XNetEndpoint *endpoint)
{
    auto     *socket = static_cast<NativeSocket *>(impl);
    ip_addr_t ip;
    u16_t     port;
    int       result = native_endpoint_ip(endpoint, &ip, &port);
    if (result) return result;
    if (endpoint->address.family != socket->family) return -EAFNOSUPPORT;
    result = xj380_err_to_errno(netconn_bind(socket->socket.conn, &ip, port));
    if (!result) socket->state = XNET_BOUND;
    return result;
}

static int native_listen(void *impl, uint32_t backlog)
{
    auto *socket = static_cast<NativeSocket *>(impl);
    if (socket->transport != XNET_TCP) return -EOPNOTSUPP;
    int result = xj380_provider_listen(&socket->socket, backlog);
    if (!result) socket->state = XNET_LISTENING;
    return result;
}

static int native_accept(void *impl, void **out, XNetEndpoint *peer)
{
    auto           *listener = static_cast<NativeSocket *>(impl);
    struct netconn *conn     = NULL;
    int             result   = xj380_err_to_errno(netconn_accept(listener->socket.conn, &conn));
    if (result) return result;
    auto *socket = static_cast<NativeSocket *>(calloc(1, sizeof(NativeSocket)));
    if (!socket)
    {
        netconn_delete(conn);
        return -ENOMEM;
    }
    socket->socket.conn = conn;
    socket->state       = XNET_CONNECTED;
    socket->family      = listener->family;
    socket->transport   = XNET_TCP;
    netconn_set_nonblocking(conn, 1);
    ip_addr_t ip;
    u16_t     port;
    if (netconn_peer(conn, &ip, &port) == ERR_OK) *peer = native_endpoint(&ip, port);
    *out = socket;
    return 0;
}

static int native_connect(void *impl, const XNetEndpoint *endpoint)
{
    auto     *socket = static_cast<NativeSocket *>(impl);
    ip_addr_t ip;
    u16_t     port;
    int       result = native_endpoint_ip(endpoint, &ip, &port);
    if (result) return result;
    if (endpoint->address.family != socket->family) return -EAFNOSUPPORT;
    if (socket->family == XNET_IPV6 ? !native_ipv6_ready() : !g_stack_ready) return -ENETDOWN;
    result                    = xj380_err_to_errno(netconn_connect(socket->socket.conn, &ip, port));
    socket->connection_result = result;
    socket->state             = result == -EINPROGRESS ? XNET_CONNECTING : result ? XNET_FAILED : XNET_CONNECTED;
    return result;
}

static int native_shutdown(void *impl, uint32_t direction)
{
    auto *socket = static_cast<NativeSocket *>(impl);
    if (socket->transport != XNET_TCP) return -EOPNOTSUPP;
    LOCK_TCPIP_CORE();
    struct tcp_pcb *pcb   = socket->socket.conn->pcb.tcp;
    err_t           error = pcb && socket->state == XNET_CONNECTED
                                ? tcp_shutdown(pcb, (direction & XNET_SHUT_RECEIVE) != 0, (direction & XNET_SHUT_SEND) != 0)
                                : ERR_CONN;
    UNLOCK_TCPIP_CORE();
    if (error == ERR_MEM) return -EAGAIN;
    if (!error) socket->shutdown |= direction;
    return xj380_err_to_errno(error);
}

static int native_query(void *impl, XNetSocketInfo *info)
{
    auto           *socket = static_cast<NativeSocket *>(impl);
    struct netconn *conn   = socket->socket.conn;
    LOCK_TCPIP_CORE();
    err_t error = conn->pending_err;
    if (socket->state == XNET_CONNECTING && conn->state != NETCONN_CONNECT)
    {
        socket->connection_result = error == ERR_OK && conn->pcb.tcp &&
                                            (conn->pcb.tcp->state == ESTABLISHED || conn->pcb.tcp->state == CLOSE_WAIT)
                                        ? 0
                                    : error != ERR_OK ? xj380_err_to_errno(error)
                                                      : -ECONNABORTED;
        socket->state             = socket->connection_result ? XNET_FAILED : XNET_CONNECTED;
    }
    uint32_t events = 0;
    if (socket->state == XNET_CONNECTED) events |= XNET_CONNECTION_READY;
    if (socket->state == XNET_FAILED || (error != ERR_OK && error != ERR_CLSD)) events |= XNET_ERROR;
    if (socket->state == XNET_LISTENING)
    {
        if (xj380_mbox_has_data(&conn->acceptmbox)) events |= XNET_ACCEPTABLE;
    }
    else if (socket->socket.rx_buf || socket->eof || xj380_mbox_has_data(&conn->recvmbox) || error == ERR_CLSD)
        events |= XNET_READABLE;
    if (socket->transport == XNET_UDP)
    {
        if (conn->pcb.udp && error == ERR_OK) events |= XNET_WRITABLE;
    }
    else if (socket->state == XNET_CONNECTED && !(socket->shutdown & XNET_WRITE_CLOSED) && conn->pcb.tcp &&
             tcp_sndbuf(conn->pcb.tcp) && tcp_sndqueuelen(conn->pcb.tcp) < TCP_SND_QUEUELEN && error == ERR_OK)
        events |= XNET_WRITABLE;
    info->state             = socket->state;
    info->events            = events;
    info->connection_result = socket->connection_result;
    info->shutdown          = socket->shutdown | (socket->eof ? XNET_READ_CLOSED : 0);
    UNLOCK_TCPIP_CORE();
    ip_addr_t ip;
    u16_t     port;
    if (netconn_getaddr(conn, &ip, &port, 1) == ERR_OK) info->local = native_endpoint(&ip, port);
    if (netconn_getaddr(conn, &ip, &port, 0) == ERR_OK) info->peer = native_endpoint(&ip, port);
    return 0;
}

static int64_t native_send(void *impl, const void *data, uint32_t bytes, const XNetEndpoint *peer)
{
    auto *socket = static_cast<NativeSocket *>(impl);
    if (socket->transport == XNET_TCP) return xj380_provider_write(&socket->socket, data, bytes, O_NONBLOCK);
    // IP fragmentation is disabled in this backend; reject oversized datagrams
    // before handing any part of the packet to the network device.
    uint32_t overhead = socket->family == XNET_IPV6 ? 48 : 28;
    if (socket->family == XNET_IPV6 ? !native_ipv6_ready() : !g_stack_ready) return -ENETDOWN;
    struct netbuf buffer = {};
    ip_addr_t destination;
    u16_t port = 0;
    if (peer)
    {
        int result = native_endpoint_ip(peer, &destination, &port);
        if (result) return result;
        if (peer->address.family != socket->family) return -EAFNOSUPPORT;
    }
    LOCK_TCPIP_CORE();
    struct udp_pcb *pcb = socket->socket.conn->pcb.udp;
    if (!peer && pcb)
    {
        destination = pcb->remote_ip;
        port = pcb->remote_port;
    }
    bool denied = pcb && port && IP_IS_V4(&destination) && ip4_addr_isbroadcast(ip_2_ip4(&destination), &g_netif) &&
                  !ip_get_option(pcb, SOF_BROADCAST);
    struct netif *route = pcb && port ? ip_route(&pcb->local_ip, &destination) : NULL;
    uint32_t      mtu   = route ? (socket->family == XNET_IPV6 ? netif_mtu6(route) : route->mtu) : g_netif.mtu;
    // A zero IPv4 loopback MTU means that the IP length field is the limit.
    if (!mtu) mtu = 65535;
    UNLOCK_TCPIP_CORE();
    if (!port) return -EDESTADDRREQ;
    if (denied) return -EACCES;
    if (bytes > (mtu > overhead ? mtu - overhead : 0)) return -EMSGSIZE;
    ip_addr_set(&buffer.addr, &destination);
    buffer.port = port;
    err_t error;
    if (!bytes)
    {
        if (!netbuf_alloc(&buffer, 0)) return -ENOMEM;
        error = netconn_send(socket->socket.conn, &buffer);
    }
    else
    {
        error = netbuf_ref(&buffer, data, bytes);
        if (error == ERR_OK) error = netconn_send(socket->socket.conn, &buffer);
    }
    netbuf_free(&buffer);
    return error == ERR_OK ? int64_t(bytes) : xj380_err_to_errno(error);
}

static int64_t native_receive(void *impl, void *data, uint32_t capacity, XNetDatagramInfo *info)
{
    auto *socket = static_cast<NativeSocket *>(impl);
    if (socket->transport == XNET_TCP)
    {
        int64_t result = xj380_socket_read_impl(&socket->socket, data, capacity, O_NONBLOCK);
        if (!result && capacity) socket->eof = true;
        return result;
    }
    struct netbuf *buffer = NULL;
    int result = xj380_err_to_errno(netconn_recv_udp_raw_netbuf_flags(socket->socket.conn, &buffer, NETCONN_DONTBLOCK));
    if (result) return result;
    if (!buffer) return -EIO;
    info->source         = native_endpoint(netbuf_fromaddr(buffer), netbuf_fromport(buffer));
    info->original_bytes = netbuf_len(buffer);
    uint32_t bytes       = capacity < info->original_bytes ? capacity : uint32_t(info->original_bytes);
    if (bytes) netbuf_copy_partial(buffer, data, bytes, 0);
    if (bytes < info->original_bytes) info->flags |= XNET_DATAGRAM_TRUNCATED;
    netbuf_delete(buffer);
    return bytes;
}

static int native_tcp_options(void *impl, XNetTcpOptions *options, bool write)
{
    auto *socket = static_cast<NativeSocket *>(impl);
    if (write && (options->no_delay > 1 || options->keepalive > 1 || options->reserved)) return -EINVAL;
    // The current lwIP build does not include adjustable keepalive timing.
    if (write && (options->keep_interval_ms || options->keep_count || options->keep_idle_ms)) return -EOPNOTSUPP;
    LOCK_TCPIP_CORE();
    struct tcp_pcb *pcb = socket->socket.conn->pcb.tcp;
    if (!pcb || pcb->state == LISTEN)
    {
        UNLOCK_TCPIP_CORE();
        return -EOPNOTSUPP;
    }
    if (write)
    {
        if (options->no_delay)
            tcp_nagle_disable(pcb);
        else
            tcp_nagle_enable(pcb);
        if (options->keepalive)
            ip_set_option(pcb, SOF_KEEPALIVE);
        else
            ip_reset_option(pcb, SOF_KEEPALIVE);
    }
    else
    {
        *options = {sizeof(*options),
                    XNET_ABI_VERSION,
                    tcp_nagle_disabled(pcb) ? 1U : 0U,
                    ip_get_option(pcb, SOF_KEEPALIVE) ? 1U : 0U,
                    0,
                    0,
                    0,
                    0};
    }
    UNLOCK_TCPIP_CORE();
    return 0;
}

static int native_udp_options(void *impl, XNetUdpOptions *options, bool write)
{
    auto *socket = static_cast<NativeSocket *>(impl);
    if (write && (options->broadcast > 1 || options->reserved)) return -EINVAL;
    if (socket->family == XNET_IPV6)
    {
        if (write && options->broadcast) return -EOPNOTSUPP;
        if (!write) *options = {sizeof(*options), XNET_ABI_VERSION, 0, 0};
        return 0;
    }
    LOCK_TCPIP_CORE();
    struct udp_pcb *pcb = socket->socket.conn->pcb.udp;
    if (!pcb)
    {
        UNLOCK_TCPIP_CORE();
        return -ENOTCONN;
    }
    if (write)
    {
        if (options->broadcast)
            ip_set_option(pcb, SOF_BROADCAST);
        else
            ip_reset_option(pcb, SOF_BROADCAST);
    }
    else
        *options = {sizeof(*options), XNET_ABI_VERSION, ip_get_option(pcb, SOF_BROADCAST) ? 1U : 0U, 0};
    UNLOCK_TCPIP_CORE();
    return 0;
}

static void native_dns_release(void *impl)
{
    auto *query = static_cast<NativeDns *>(impl);
    if (query && __atomic_sub_fetch(&query->refs, 1, __ATOMIC_ACQ_REL) == 0) free(query);
}

static void native_dns_found(const char *, const ip_addr_t *ip, void *arg)
{
    auto *query   = static_cast<NativeDns *>(arg);
    query->result = ip ? 0 : -ENOENT;
    if (ip) query->address = native_endpoint(ip, 0).address;
    __atomic_store_n(&query->done, 1, __ATOMIC_RELEASE);
    native_dns_release(query); // A callback pin outlives timeout/close/cancel.
}

static int native_dns_start(const char *host, uint32_t family, void **out)
{
    if (family && family != XNET_IPV4 && family != XNET_IPV6) return -EAFNOSUPPORT;
    auto *query = static_cast<NativeDns *>(calloc(1, sizeof(NativeDns)));
    if (!query) return -ENOMEM;
    query->refs = 1;
    ip_addr_t address;
    XNetAddress literal;
    if (!xnet_address::parse(host, &literal))
    {
        if (family && literal.family != family)
        {
            free(query);
            return -EAFNOSUPPORT;
        }
        XNetEndpoint endpoint = {};
        endpoint.address      = literal;
        u16_t ignored;
        int   error = native_endpoint_ip(&endpoint, &address, &ignored);
        if (error)
        {
            free(query);
            return error;
        }
        query->address = native_endpoint(&address, 0).address;
        query->done    = 1;
    }
    else
    {
        // A malformed IPv6 literal must never become a DNS hostname.
        if (strchr(host, ':') || strchr(host, '%'))
        {
            free(query);
            return -EINVAL;
        }
        if (!native_network_ready())
        {
            free(query);
            return -ENETDOWN;
        }
        __atomic_add_fetch(&query->refs, 1, __ATOMIC_RELAXED);
        LOCK_TCPIP_CORE();
        uint8_t type  = family == XNET_IPV6   ? LWIP_DNS_ADDRTYPE_IPV6
                        : family == XNET_IPV4 ? LWIP_DNS_ADDRTYPE_IPV4
                        : g_stack_ready       ? LWIP_DNS_ADDRTYPE_IPV4_IPV6
                                              : LWIP_DNS_ADDRTYPE_IPV6_IPV4;
        err_t   error = dns_gethostbyname_addrtype(host, &address, native_dns_found, query, type);
        UNLOCK_TCPIP_CORE();
        if (error != ERR_INPROGRESS)
        {
            if (error == ERR_OK)
            {
                query->address = native_endpoint(&address, 0).address;
                __atomic_store_n(&query->done, 1, __ATOMIC_RELEASE);
            }
            native_dns_release(query); // No callback was registered.
            if (error != ERR_OK)
            {
                native_dns_release(query);
                return xj380_err_to_errno(error);
            }
        }
    }
    *out = query;
    return 0;
}

static int native_dns_poll(void *impl, XNetAddress *address)
{
    auto *query = static_cast<NativeDns *>(impl);
    if (!query) return -EINVAL;
    if (!__atomic_load_n(&query->done, __ATOMIC_ACQUIRE)) return -EINPROGRESS;
    if (!query->result) *address = query->address;
    return query->result;
}

static uint32_t native_prefix(uint32_t mask)
{
    uint8_t bytes[4];
    memcpy(bytes, &mask, 4);
    uint32_t bits = 0;
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned b = 0; b < 8; ++b)
            bits += (bytes[i] >> b) & 1U;
    return bits;
}

static int native_interface(XNetInterfaceInfo *info, XNetInterfaceAddress *addresses, uint32_t *count,
                            XNetDnsConfig *dns)
{
    *info              = {};
    *dns               = {};
    info->size         = sizeof(*info);
    info->version      = XNET_ABI_VERSION;
    info->interface_id = 1;
    info->connectivity = XNET_CONNECTIVITY_UNKNOWN;
    info->name[0]      = 'e';
    info->name[1]      = '0';
    dns->size          = sizeof(*dns);
    dns->version       = XNET_ABI_VERSION;
    dns->interface_id  = 1;
    dns->method        = g_dns_manual_override ? XNET_DNS_MANUAL : XNET_DNS_AUTO;
    if (!g_stack_initialized)
    {
        *count = 0;
        return 0;
    }
    LOCK_TCPIP_CORE();
    dns->method = g_dns_manual_override ? XNET_DNS_MANUAL : XNET_DNS_AUTO;
    info->flags = XNET_STACK_READY;
    if (netif_is_up(&g_netif) && netif_is_link_up(&g_netif)) info->flags |= XNET_LINK_UP;
    info->mtu         = g_netif.mtu;
    info->ipv4_method = g_ipv4_method;
    info->dhcp_state  = xj380_current_dhcp_state();
    memcpy(info->mac, g_netif.hwaddr, 6);
    uint32_t          n = 0, capacity = *count;
    const ip4_addr_t *ip = netif_ip4_addr(&g_netif);
    if (ip && ip->addr)
    {
        info->flags |= XNET_ADDRESS_READY;
        if (n < capacity)
        {
            addresses[0]                = {};
            addresses[0].address.family = XNET_IPV4;
            memcpy(addresses[0].address.bytes, &ip->addr, 4);
            addresses[0].prefix_bits = native_prefix(netif_ip4_netmask(&g_netif)->addr);
        }
        ++n;
    }
    for (unsigned i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i)
    {
        const ip6_addr_t *address = netif_ip6_addr(&g_netif, i);
        if (!ip6_addr_isvalid(netif_ip6_addr_state(&g_netif, i)) || ip6_addr_isany(address)) continue;
        info->flags |= XNET_IPV6_ADDRESS_READY;
        if (!ip6_addr_islinklocal(address) && !ip6_addr_isloopback(address)) info->flags |= XNET_IPV6_GLOBAL_READY;
        if (n < capacity)
        {
            ip_addr_t ip6;
            ip_addr_copy_from_ip6(ip6, *address);
            addresses[n]             = {};
            addresses[n].address     = native_endpoint(&ip6, 0).address;
            addresses[n].prefix_bits = 64;
        }
        ++n;
    }
    for (unsigned i = 0; i < LWIP_ND6_NUM_ROUTERS; ++i)
    {
        const auto &router = default_router_list[i];
        if (router.neighbor_entry && router.neighbor_entry->netif == &g_netif && router.invalidation_timer)
            info->flags |= XNET_IPV6_DEFAULT_ROUTE;
    }
    info->gateway.family = XNET_IPV4;
    memcpy(info->gateway.bytes, &netif_ip4_gw(&g_netif)->addr, 4);
    if (netif_ip4_gw(&g_netif)->addr) info->flags |= XNET_DEFAULT_ROUTE;
    for (unsigned i = 0; i < 2; ++i)
    {
        const ip_addr_t *server = dns_getserver(i);
        if (server && !ip_addr_isany(server))
        {
            dns->servers[dns->server_count++]  = native_endpoint(server, 0).address;
            info->flags                       |= XNET_DNS_CONFIGURED;
        }
    }
    UNLOCK_TCPIP_CORE();
    *count = n;
    return 0;
}

static bool native_ipv4_value(const XNetAddress &address)
{
    if (address.family != XNET_IPV4 || address.scope_id) return false;
    for (unsigned i = 4; i < 16; ++i)
        if (address.bytes[i]) return false;
    return true;
}

static int native_apply_interface(const XNetInterfaceConfig *config)
{
    if (config->interface_id > 1) return -ENODEV;
    socket_ipv4_config_t value = {};
    value.method               = config->method;
    if (config->reserved) return -EINVAL;
    if (config->method == XNET_CONFIG_STATIC)
    {
        if (!native_ipv4_value(config->address) || !native_ipv4_value(config->gateway) || !config->prefix_bits ||
            config->prefix_bits > 32)
            return -EINVAL;
        memcpy(&value.ipv4_addr, config->address.bytes, 4);
        memcpy(&value.ipv4_gateway, config->gateway.bytes, 4);
        uint8_t mask[4] = {};
        for (unsigned bit = 0; bit < config->prefix_bits; ++bit)
            mask[bit / 8] |= 0x80U >> (bit % 8);
        memcpy(&value.ipv4_netmask, mask, 4);
    }
    else if (config->method != XNET_CONFIG_DHCP)
        return -EINVAL;
    return xj380_provider_set_ipv4_config(&value);
}

static int native_apply_dns(const XNetDnsConfig *config)
{
    if (config->interface_id > 1) return -ENODEV;
    if (config->reserved) return -EINVAL;
    if (!g_stack_initialized) return -EAGAIN;
    if (config->method == XNET_DNS_AUTO)
    {
        if (config->server_count) return -EINVAL;
        return xj380_provider_set_dns_server(0);
    }
    if (config->method != XNET_DNS_MANUAL || !config->server_count) return -EINVAL;
    if (config->server_count > 2) return -EOPNOTSUPP;
    ip_addr_t servers[2] = {};
    for (unsigned i = 0; i < config->server_count; ++i)
    {
        XNetEndpoint endpoint = {};
        endpoint.address      = config->servers[i];
        u16_t ignored;
        int   error = native_endpoint_ip(&endpoint, &servers[i], &ignored);
        if (error) return error;
        if (ip_addr_isany(&servers[i]) || ip_addr_ismulticast(&servers[i])) return -EINVAL;
    }
    LOCK_TCPIP_CORE();
    for (unsigned i = 0; i < 2; ++i)
    {
        g_dns_manual_servers[i] = servers[i];
        dns_setserver(i, i < config->server_count ? &servers[i] : NULL);
    }
    g_dns_manual_count    = config->server_count;
    g_dns_manual_override = true;
    g_dns_manual_addr     = IP_IS_V4(&servers[0]) ? ip_2_ip4(&servers[0])->addr : 0;
    UNLOCK_TCPIP_CORE();
    return 0;
}

static const XNetProvider native_provider = {sizeof(XNetProvider),
                                             XNET_ABI_VERSION,
                                             XNET_CAP_IPV4 | XNET_CAP_IPV6 | XNET_CAP_TCP | XNET_CAP_UDP |
                                                 XNET_CAP_DNS | XNET_CAP_TCP_NODELAY | XNET_CAP_TCP_KEEPALIVE |
                                                 XNET_CAP_UDP_BROADCAST | XNET_CAP_RUNTIME_CONFIG,
                                             native_open,
                                             native_close,
                                             native_bind,
                                             native_listen,
                                             native_accept,
                                             native_connect,
                                             native_shutdown,
                                             native_query,
                                             native_send,
                                             native_receive,
                                             native_tcp_options,
                                             native_udp_options,
                                             native_dns_start,
                                             native_dns_poll,
                                             native_dns_release,
                                             native_interface,
                                             native_apply_interface,
                                             native_apply_dns};
