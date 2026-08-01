extern "C" {
#include <errno.h>
#include <net/socket.h>
#include <netdev.h>
#include <proto.hpp>
#include <task/pcb.h>
#include <task/poll.h>
#include "lwip/api.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/pbuf.h"
#include "lwip/prot/dhcp.h"
#include "lwip/tcpip.h"
#include "netif/ethernet.h"
}

namespace {

constexpr uint32_t kDefaultDns = 0x050505DFU;

struct NetSocket {
    netconn *connection;
    netbuf  *received;
    size_t   received_offset;
    int      domain;
    int      type;
    uint64_t flags;
};

netif    g_interface = {};
netdev_t *g_device = nullptr;
bool      g_stack_ready = false;
bool      g_provider_ready = false;
bool      g_worker_started = false;
uint8_t   g_ipv4_method = SOCKET_IPV4_METHOD_DHCP;

static int system_error(err_t error)
{
    switch (error) {
        case ERR_OK: return 0;
        case ERR_MEM:
        case ERR_BUF: return -ENOMEM;
        case ERR_TIMEOUT: return -ETIMEDOUT;
        case ERR_RTE: return -ENETUNREACH;
        case ERR_INPROGRESS: return -EINPROGRESS;
        case ERR_WOULDBLOCK: return -EAGAIN;
        case ERR_USE: return -EADDRINUSE;
        case ERR_ALREADY: return -EALREADY;
        case ERR_ISCONN: return -EISCONN;
        case ERR_CONN: return -ENOTCONN;
        case ERR_IF: return -ENETDOWN;
        case ERR_ABRT: return -ECONNABORTED;
        case ERR_RST: return -ECONNRESET;
        case ERR_CLSD: return 0;
        case ERR_VAL:
        case ERR_ARG: return -EINVAL;
        default: return -EIO;
    }
}

static bool is_udp(const NetSocket *socket)
{
    return (netconn_type(socket->connection) & 0xF0) == NETCONN_UDP;
}

static int decode_address(const sockaddr *address, socklen_t length, ip_addr_t *ip, uint16_t *port)
{
    if (address == nullptr || ip == nullptr || port == nullptr || length < sizeof(sockaddr_in)) {
        return -EINVAL;
    }
    if (address->sa_family != AF_INET) {
        return -EAFNOSUPPORT;
    }
    const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(address);
    ip->addr = ipv4->sin_addr.s_addr;
    *port = ipv4->sin_port;
    return 0;
}

static int encode_address(const ip_addr_t *ip, uint16_t port, sockaddr *address, socklen_t *length)
{
    if (length == nullptr) {
        return -EINVAL;
    }
    if (address == nullptr || *length < sizeof(sockaddr_in)) {
        *length = sizeof(sockaddr_in);
        return -EINVAL;
    }
    auto *ipv4 = reinterpret_cast<sockaddr_in *>(address);
    memset(ipv4, 0, sizeof(*ipv4));
    ipv4->sin_family = AF_INET;
    ipv4->sin_port = port;
    ipv4->sin_addr.s_addr = ip->addr;
    *length = sizeof(*ipv4);
    return 0;
}

static err_t transmit(netif *interface, pbuf *packet)
{
    (void)interface;
    if (g_device == nullptr || packet == nullptr) {
        return ERR_IF;
    }
    void *frame = malloc(packet->tot_len);
    if (frame == nullptr) {
        return ERR_MEM;
    }
    pbuf_copy_partial(packet, frame, packet->tot_len, 0);
    int result = netdev_send(g_device, frame, packet->tot_len);
    free(frame);
    return result < 0 ? ERR_IF : ERR_OK;
}

static err_t initialize_interface(netif *interface)
{
    interface->name[0] = 'e';
    interface->name[1] = '0';
    interface->output = etharp_output;
    interface->linkoutput = transmit;
    interface->mtu = static_cast<u16_t>(g_device->mtu);
    interface->hwaddr_len = 6;
    memcpy(interface->hwaddr, g_device->mac, 6);
    interface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void stack_initialized(void *semaphore)
{
    sys_sem_signal(static_cast<sys_sem_t *>(semaphore));
}

static int start_stack()
{
    sys_sem_t initialized;
    if (sys_sem_new(&initialized, 0) != ERR_OK) {
        return -ENOMEM;
    }
    tcpip_init(stack_initialized, &initialized);
    sys_arch_sem_wait(&initialized, 0);
    sys_sem_free(&initialized);

    ip4_addr_t zero;
    ip4_addr_set_zero(&zero);
    if (netif_add(&g_interface, &zero, &zero, &zero, nullptr, initialize_interface, tcpip_input) == nullptr) {
        return -EIO;
    }
    netif_set_default(&g_interface);
    netif_set_up(&g_interface);
    netif_set_link_up(&g_interface);
    err_t dhcp_result = netifapi_dhcp_start(&g_interface);
    if (dhcp_result != ERR_OK) {
        return system_error(dhcp_result);
    }
    ip_addr_t dns;
    dns.addr = kDefaultDns;
    dns_setserver(0, &dns);
    g_stack_ready = true;
    return 0;
}

static void receive_frames()
{
    const uint32_t capacity = g_device->mtu + 32U;
    void *frame = malloc(capacity);
    if (frame == nullptr) {
        return;
    }
    for (;;) {
        int length = netdev_recv(g_device, frame, capacity);
        if (length <= 0) {
            break;
        }
        pbuf *packet = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(length), PBUF_POOL);
        if (packet == nullptr) {
            break;
        }
        if (pbuf_take(packet, frame, static_cast<u16_t>(length)) != ERR_OK || g_interface.input(packet, &g_interface) != ERR_OK) {
            pbuf_free(packet);
        }
    }
    free(frame);
}

static int provider_create(int domain, int type, int protocol, void **output)
{
    if (output == nullptr || (domain != AF_INET && domain != AF_UNSPEC)) {
        return -EAFNOSUPPORT;
    }
    int base_type = type & SOCK_TYPE_MASK;
    netconn_type connection_type;
    if (base_type == SOCK_STREAM && (protocol == 0 || protocol == IPPROTO_TCP)) {
        connection_type = NETCONN_TCP;
    } else if (base_type == SOCK_DGRAM && (protocol == 0 || protocol == IPPROTO_UDP)) {
        connection_type = NETCONN_UDP;
    } else {
        return -EPROTONOSUPPORT;
    }
    netconn *connection = netconn_new(connection_type);
    if (connection == nullptr) {
        return -ENOMEM;
    }
    auto *socket = static_cast<NetSocket *>(calloc(1, sizeof(NetSocket)));
    if (socket == nullptr) {
        netconn_delete(connection);
        return -ENOMEM;
    }
    socket->connection = connection;
    socket->domain = AF_INET;
    socket->type = base_type;
    socket->flags = static_cast<uint64_t>(type & (SOCK_NONBLOCK | SOCK_CLOEXEC));
    netconn_set_nonblocking(connection, (socket->flags & SOCK_NONBLOCK) != 0);
    *output = socket;
    return 0;
}

static int provider_close(void *opaque)
{
    auto *socket = static_cast<NetSocket *>(opaque);
    if (socket == nullptr) {
        return -EINVAL;
    }
    if (socket->received != nullptr) {
        netbuf_delete(socket->received);
    }
    err_t result = netconn_delete(socket->connection);
    free(socket);
    return system_error(result);
}

static ssize_t receive_data(NetSocket *socket, void *buffer, size_t size, uint64_t flags,
                            sockaddr *address, socklen_t *address_length)
{
    if (socket == nullptr || (buffer == nullptr && size != 0)) {
        return -EINVAL;
    }
    if (size == 0) {
        return 0;
    }
    if (socket->received == nullptr) {
        bool temporary_nonblocking = (flags & MSG_DONTWAIT) != 0 && !netconn_is_nonblocking(socket->connection);
        if (temporary_nonblocking) {
            netconn_set_nonblocking(socket->connection, true);
        }
        err_t result = netconn_recv(socket->connection, &socket->received);
        if (temporary_nonblocking) {
            netconn_set_nonblocking(socket->connection, false);
        }
        if (result != ERR_OK) {
            return system_error(result);
        }
        socket->received_offset = 0;
    }
    size_t available = netbuf_len(socket->received) - socket->received_offset;
    size_t copied = size < available ? size : available;
    netbuf_copy_partial(socket->received, buffer, static_cast<u16_t>(copied),
                        static_cast<u16_t>(socket->received_offset));
    if (address_length != nullptr && is_udp(socket)) {
        encode_address(netbuf_fromaddr(socket->received), netbuf_fromport(socket->received), address, address_length);
    }
    if ((flags & MSG_PEEK) == 0) {
        socket->received_offset += copied;
        if (socket->received_offset == netbuf_len(socket->received)) {
            netbuf_delete(socket->received);
            socket->received = nullptr;
            socket->received_offset = 0;
        }
    }
    return static_cast<ssize_t>(copied);
}

static ssize_t provider_read(void *opaque, void *buffer, size_t size, uint64_t flags)
{
    return receive_data(static_cast<NetSocket *>(opaque), buffer, size, flags, nullptr, nullptr);
}

static ssize_t provider_recvfrom(void *opaque, void *buffer, size_t size, uint64_t flags,
                                 sockaddr *address, socklen_t *address_length)
{
    return receive_data(static_cast<NetSocket *>(opaque), buffer, size, flags, address, address_length);
}

static ssize_t provider_sendto(void *opaque, const void *buffer, size_t size, uint64_t flags,
                               const sockaddr *address, socklen_t address_length)
{
    (void)flags;
    auto *socket = static_cast<NetSocket *>(opaque);
    if (socket == nullptr || (buffer == nullptr && size != 0) || size > 0xFFFFU) {
        return -EINVAL;
    }
    if (!is_udp(socket)) {
        size_t written = 0;
        err_t result = netconn_write_partly(socket->connection, buffer, size, NETCONN_COPY, &written);
        return result == ERR_OK ? static_cast<ssize_t>(written) : system_error(result);
    }
    netbuf *packet = netbuf_new();
    if (packet == nullptr || netbuf_ref(packet, buffer, static_cast<u16_t>(size)) != ERR_OK) {
        if (packet != nullptr) {
            netbuf_delete(packet);
        }
        return -ENOMEM;
    }
    err_t result;
    if (address != nullptr) {
        ip_addr_t ip;
        uint16_t port;
        int decoded = decode_address(address, address_length, &ip, &port);
        if (decoded != 0) {
            netbuf_delete(packet);
            return decoded;
        }
        result = netconn_sendto(socket->connection, packet, &ip, port);
    } else {
        result = netconn_send(socket->connection, packet);
    }
    netbuf_delete(packet);
    return result == ERR_OK ? static_cast<ssize_t>(size) : system_error(result);
}

static ssize_t provider_write(void *opaque, const void *buffer, size_t size, uint64_t flags)
{
    return provider_sendto(opaque, buffer, size, flags, nullptr, 0);
}

static int provider_bind(void *opaque, const sockaddr *address, socklen_t length)
{
    ip_addr_t ip;
    uint16_t port;
    int result = decode_address(address, length, &ip, &port);
    if (result != 0) {
        return result;
    }
    return system_error(netconn_bind(static_cast<NetSocket *>(opaque)->connection, &ip, port));
}

static int provider_connect(void *opaque, const sockaddr *address, socklen_t length)
{
    ip_addr_t ip;
    uint16_t port;
    int result = decode_address(address, length, &ip, &port);
    if (result != 0) {
        return result;
    }
    return system_error(netconn_connect(static_cast<NetSocket *>(opaque)->connection, &ip, port));
}

static int socket_name(void *opaque, sockaddr *address, socklen_t *length, bool local)
{
    ip_addr_t ip;
    uint16_t port = 0;
    err_t result = netconn_getaddr(static_cast<NetSocket *>(opaque)->connection, &ip, &port, local ? 1 : 0);
    return result == ERR_OK ? encode_address(&ip, port, address, length) : system_error(result);
}

static int provider_getsockname(void *opaque, sockaddr *address, socklen_t *length)
{
    return socket_name(opaque, address, length, true);
}

static int provider_getpeername(void *opaque, sockaddr *address, socklen_t *length)
{
    return socket_name(opaque, address, length, false);
}

static int provider_getsockopt(void *opaque, int level, int option, void *value, socklen_t *length)
{
    auto *socket = static_cast<NetSocket *>(opaque);
    if (socket == nullptr || value == nullptr || length == nullptr || *length < sizeof(int) || level != SOL_SOCKET) {
        return -EINVAL;
    }
    int result;
    switch (option) {
        case SO_TYPE: result = socket->type; break;
        case SO_ERROR: result = -system_error(netconn_err(socket->connection)); break;
        case SO_RCVBUF: result = PBUF_POOL_SIZE * PBUF_POOL_BUFSIZE; break;
        case SO_RCVTIMEO:
        case SO_SNDTIMEO: result = 0; break;
        default: return -ENOPROTOOPT;
    }
    *static_cast<int *>(value) = result;
    *length = sizeof(int);
    return 0;
}

static int provider_listen(void *opaque, int backlog)
{
    if (backlog < 0) {
        return -EINVAL;
    }
    return system_error(netconn_listen_with_backlog(static_cast<NetSocket *>(opaque)->connection,
                                                     static_cast<u8_t>(backlog > 255 ? 255 : backlog)));
}

static int provider_accept(void *opaque, void **output, sockaddr *address, socklen_t *length)
{
    if (output == nullptr) {
        return -EINVAL;
    }
    netconn *accepted = nullptr;
    err_t result = netconn_accept(static_cast<NetSocket *>(opaque)->connection, &accepted);
    if (result != ERR_OK) {
        return system_error(result);
    }
    auto *socket = static_cast<NetSocket *>(calloc(1, sizeof(NetSocket)));
    if (socket == nullptr) {
        netconn_delete(accepted);
        return -ENOMEM;
    }
    socket->connection = accepted;
    socket->domain = AF_INET;
    socket->type = SOCK_STREAM;
    *output = socket;
    if (address != nullptr && length != nullptr) {
        provider_getpeername(socket, address, length);
    }
    return 0;
}

static int provider_shutdown(void *opaque, int how)
{
    if (how < SHUT_RD || how > SHUT_RDWR) {
        return -EINVAL;
    }
    return system_error(netconn_shutdown(static_cast<NetSocket *>(opaque)->connection,
                                         how != SHUT_WR, how != SHUT_RD));
}

static int provider_poll(void *opaque, size_t events)
{
    auto *socket = static_cast<NetSocket *>(opaque);
    int ready = 0;
    err_t error = netconn_err(socket->connection);
    if (error != ERR_OK) {
        ready |= POLLERR;
    }
    if ((events & POLLIN) != 0 &&
        (socket->received != nullptr || socket->connection->recvmbox.count != 0 ||
         socket->connection->acceptmbox.count != 0)) {
        ready |= POLLIN;
    }
    if ((events & POLLOUT) != 0 && error == ERR_OK) {
        ready |= POLLOUT;
    }
    return ready;
}

static int provider_set_flags(void *opaque, uint64_t flags)
{
    auto *socket = static_cast<NetSocket *>(opaque);
    socket->flags = flags;
    netconn_set_nonblocking(socket->connection, (flags & SOCK_NONBLOCK) != 0);
    return 0;
}

static int provider_resolve_ipv4(const char *hostname, uint32_t *output)
{
    if (hostname == nullptr || output == nullptr) {
        return -EINVAL;
    }
    ip_addr_t address;
    err_t result = netconn_gethostbyname(hostname, &address);
    if (result == ERR_OK) {
        *output = address.addr;
    }
    return system_error(result);
}

static int provider_resolve_ipv6(const char *hostname, uint8_t output[16])
{
    (void)hostname;
    (void)output;
    return -EAFNOSUPPORT;
}

static int provider_set_dns(uint32_t address)
{
    ip_addr_t server;
    server.addr = address == 0 ? kDefaultDns : address;
    dns_setserver(0, &server);
    return 0;
}

static int provider_get_dns(uint32_t *output)
{
    if (output == nullptr) {
        return -EINVAL;
    }
    const ip_addr_t *server = dns_getserver(0);
    if (server == nullptr) {
        return -ENOENT;
    }
    *output = server->addr;
    return 0;
}

static int provider_set_ipv4(const socket_ipv4_config_t *configuration)
{
    if (configuration == nullptr) {
        return -EINVAL;
    }
    if (configuration->method == SOCKET_IPV4_METHOD_DHCP) {
        netifapi_dhcp_stop(&g_interface);
        ip4_addr_t zero;
        ip4_addr_set_zero(&zero);
        netifapi_netif_set_addr(&g_interface, &zero, &zero, &zero);
        err_t result = netifapi_dhcp_start(&g_interface);
        if (result == ERR_OK) {
            g_ipv4_method = SOCKET_IPV4_METHOD_DHCP;
        }
        return system_error(result);
    }
    if (configuration->method != SOCKET_IPV4_METHOD_MANUAL) {
        return -EINVAL;
    }
    netifapi_dhcp_stop(&g_interface);
    ip4_addr_t ip = {configuration->ipv4_addr};
    ip4_addr_t mask = {configuration->ipv4_netmask};
    ip4_addr_t gateway = {configuration->ipv4_gateway};
    err_t result = netifapi_netif_set_addr(&g_interface, &ip, &mask, &gateway);
    if (result == ERR_OK) {
        g_ipv4_method = SOCKET_IPV4_METHOD_MANUAL;
    }
    return system_error(result);
}

static int provider_get_netinfo(socket_netinfo_t *output)
{
    if (output == nullptr) {
        return -EINVAL;
    }
    memset(output, 0, sizeof(*output));
    output->ipv4_addr = g_interface.ip_addr.addr;
    output->ipv4_netmask = g_interface.netmask.addr;
    output->ipv4_gateway = g_interface.gw.addr;
    provider_get_dns(&output->dns_server);
    output->mtu = g_interface.mtu;
    memcpy(output->mac, g_interface.hwaddr, 6);
    output->link_up = netif_is_link_up(&g_interface);
    output->stack_ready = g_stack_ready;
    output->ip_ready = !ip4_addr_isany_val(g_interface.ip_addr);
    output->ipv4_method = g_ipv4_method;
    dhcp *client = netif_dhcp_data(&g_interface);
    output->dhcp_state = client != nullptr ? client->state : DHCP_STATE_OFF;
    output->ifname[0] = 'e';
    output->ifname[1] = '0';
    return 0;
}

socket_provider_t g_provider = {
    .create = provider_create,
    .close = provider_close,
    .read = provider_read,
    .write = provider_write,
    .recvfrom = provider_recvfrom,
    .sendto = provider_sendto,
    .bind = provider_bind,
    .connect = provider_connect,
    .getsockname = provider_getsockname,
    .getpeername = provider_getpeername,
    .getsockopt = provider_getsockopt,
    .listen = provider_listen,
    .accept = provider_accept,
    .shutdown = provider_shutdown,
    .poll = provider_poll,
    .set_flags = provider_set_flags,
    .resolve_ipv4 = provider_resolve_ipv4,
    .resolve_ipv6 = provider_resolve_ipv6,
    .set_dns_server = provider_set_dns,
    .get_dns_server = provider_get_dns,
    .set_ipv4_config = provider_set_ipv4,
    .get_netinfo = provider_get_netinfo,
};

static void worker(uint64_t argument)
{
    (void)argument;
    while ((g_device = get_default_netdev()) == nullptr) {
        delay_ms_hp(100);
    }
    int result = start_stack();
    if (result != 0) {
        printk("netserver: stack initialization failed: %d\n", result);
        return;
    }
    printk("netserver: lwIP ready on e0\n");
    for (;;) {
        receive_frames();
        delay_ms_hp(1);
    }
}

}

extern "C" __attribute__((used)) __attribute__((visibility("default"))) int dlstart(void)
{
    return 0;
}

extern "C" __attribute__((used)) __attribute__((visibility("default"))) int dlmain(void)
{
    if (!g_provider_ready) {
        int result = socket_provider_register(&g_provider);
        if (result < 0) {
            return result;
        }
        g_provider_ready = true;
    }
    if (g_worker_started) {
        return 0;
    }
    g_worker_started = true;
    return create_kernel_thread(reinterpret_cast<void *>(worker), nullptr, const_cast<char *>("netserver"), nullptr) == 0
               ? -EIO
               : 0;
}
