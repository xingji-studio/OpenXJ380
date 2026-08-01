extern "C" {
#include "../../include/dlinker.h"
#include "../../include/errno.h"
#include "../../include/krlibc.h"
#include "../../include/net/socket.h"
#include "../../include/netdev.h"
#include "../../include/task/pcb.h"
#include "../../include/task/poll.h"
#include "../../include/task/scheduler.h"
#include "lwip/api.h"
#include "lwip/def.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip6_addr.h"
#include "lwip/ip.h"
#include "lwip/netbuf.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/pbuf.h"
#include "lwip/prot/dhcp.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include "netif/ethernet.h"
}

typedef struct xj380_lwip_socket {
    struct netconn *conn;
    struct pbuf    *rx_buf;
    bool            is_listener;
} xj380_lwip_socket_t;

static struct netif g_netif;
static netdev_t    *g_netdev              = NULL;
static bool         g_stack_initialized   = false;
static bool         g_stack_ready         = false;
static bool         g_worker_started      = false;
static bool         g_provider_registered = false;
static bool         g_dhcp_started        = false;
static bool         g_dhcp_ready_logged   = false;
static uint8_t      g_ipv4_method         = SOCKET_IPV4_METHOD_NONE;
static constexpr uint32_t kDefaultDnsServer = ((uint32_t)5U << 24) |
                                              ((uint32_t)5U << 16) |
                                              ((uint32_t)5U << 8) |
                                              223U; // 223.5.5.5
static bool         g_dns_manual_override = true;
static uint32_t     g_rx_log_count        = 24;
static uint32_t     g_tx_log_count        = 24;
static uint32_t     g_connect_log_count   = 0;
static uint32_t     g_write_log_count     = 0;
static uint32_t     g_read_log_count      = 0;
static uint32_t     g_poll_log_count      = 0;
static uint32_t     g_getsockopt_log_count = 0;
static uint32_t     g_dns_manual_addr     = kDefaultDnsServer;
static uint32_t     g_manual_ipv4_addr    = 0;
static uint32_t     g_manual_ipv4_netmask = 0;
static uint32_t     g_manual_ipv4_gateway = 0;
static uint8_t      g_last_dhcp_state     = 0xff;

typedef struct xj380_dns_query {
    volatile int done;
    volatile int status;
    ip_addr_t     addr;
} xj380_dns_query_t;

static void xj380_update_net_state(void);

static int xj380_err_to_errno(err_t err)
{
    if (err == ERR_OK) {
        return 0;
    }
    return -err_to_errno(err);
}

static void xj380_apply_dns_server(uint32_t addr)
{
    ip4_addr_t ip4;
    ip4.addr = addr;

    ip_addr_t dns_addr;
    ip_addr_copy_from_ip4(dns_addr, ip4);
    dns_setserver(0, &dns_addr);
}

static int xj380_apply_manual_ipv4_config(uint32_t addr, uint32_t netmask, uint32_t gateway)
{
    if (addr == 0 || netmask == 0 || !ip4_addr_netmask_valid(netmask)) {
        return -EINVAL;
    }

    ip4_addr_t ip;
    ip.addr = addr;
    ip4_addr_t mask;
    mask.addr = netmask;
    ip4_addr_t gw;
    gw.addr = gateway;

    err_t err = netifapi_dhcp_stop(&g_netif);
    if (err != ERR_OK) {
        return xj380_err_to_errno(err);
    }

    err = netifapi_netif_set_addr(&g_netif, &ip, &mask, &gw);
    if (err != ERR_OK) {
        return xj380_err_to_errno(err);
    }

    g_dhcp_started        = false;
    g_dhcp_ready_logged   = false;
    g_stack_ready         = true;
    g_ipv4_method         = SOCKET_IPV4_METHOD_MANUAL;
    g_manual_ipv4_addr    = addr;
    g_manual_ipv4_netmask = netmask;
    g_manual_ipv4_gateway = gateway;
    g_last_dhcp_state     = 0xff;

    printk("netserver: manual ipv4 -> %u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u\n",
           ip4_addr1(&ip), ip4_addr2(&ip), ip4_addr3(&ip), ip4_addr4(&ip),
           ip4_addr1(&mask), ip4_addr2(&mask), ip4_addr3(&mask), ip4_addr4(&mask),
           ip4_addr1(&gw), ip4_addr2(&gw), ip4_addr3(&gw), ip4_addr4(&gw));
    return 0;
}

static int xj380_apply_dhcp_ipv4_config(void)
{
    err_t err = netifapi_dhcp_stop(&g_netif);
    if (err != ERR_OK) {
        return xj380_err_to_errno(err);
    }

    err = netifapi_netif_set_addr(&g_netif, NULL, NULL, NULL);
    if (err != ERR_OK) {
        return xj380_err_to_errno(err);
    }

    err = netifapi_dhcp_start(&g_netif);
    if (err != ERR_OK) {
        return xj380_err_to_errno(err);
    }

    g_dhcp_started        = true;
    g_dhcp_ready_logged   = false;
    g_stack_ready         = false;
    g_ipv4_method         = SOCKET_IPV4_METHOD_DHCP;
    g_manual_ipv4_addr    = 0;
    g_manual_ipv4_netmask = 0;
    g_manual_ipv4_gateway = 0;
    g_last_dhcp_state     = 0xff;

    printk("netserver: switched ipv4 method -> auto (dhcp)\n");
    return 0;
}

static uint8_t xj380_current_dhcp_state(void)
{
    struct dhcp *dhcp = netif_dhcp_data(&g_netif);
    return dhcp != NULL ? dhcp->state : DHCP_STATE_OFF;
}

static const char *xj380_dhcp_state_name(uint8_t state)
{
    switch (state) {
    case DHCP_STATE_OFF:
        return "off";
    case DHCP_STATE_REQUESTING:
        return "requesting";
    case DHCP_STATE_INIT:
        return "init";
    case DHCP_STATE_REBOOTING:
        return "rebooting";
    case DHCP_STATE_REBINDING:
        return "rebinding";
    case DHCP_STATE_RENEWING:
        return "renewing";
    case DHCP_STATE_SELECTING:
        return "selecting";
    case DHCP_STATE_INFORMING:
        return "informing";
    case DHCP_STATE_CHECKING:
        return "checking";
    case DHCP_STATE_PERMANENT:
        return "permanent";
    case DHCP_STATE_BOUND:
        return "bound";
    case DHCP_STATE_RELEASING:
        return "releasing";
    case DHCP_STATE_BACKING_OFF:
        return "backing-off";
    default:
        return "unknown";
    }
}

static bool xj380_ipv4_ready(void)
{
    if (!g_stack_initialized) {
        return false;
    }

    const ip4_addr_t *ip = netif_ip4_addr(&g_netif);
    return ip != NULL && !ip4_addr_isany_val(*ip);
}

static err_t xj380_linkoutput(struct netif *netif, struct pbuf *p)
{
    netdev_t *dev = (netdev_t *)netif->state;
    if (dev == NULL || p == NULL) {
        return ERR_IF;
    }

    uint8_t frame[2048];
    if (p->tot_len > sizeof(frame)) {
        return ERR_BUF;
    }

    size_t copied = 0;
    for (struct pbuf *cursor = p; cursor != NULL; cursor = cursor->next) {
        memcpy(frame + copied, cursor->payload, cursor->len);
        copied += cursor->len;
    }

    if (g_tx_log_count < 24 && copied >= 14) {
        uint16_t eth_type = ((uint16_t)frame[12] << 8) | frame[13];
        printk("netserver: tx frame len=%u type=0x%04x dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x\n",
               (unsigned int)copied,
               (unsigned int)eth_type,
               frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
               frame[6], frame[7], frame[8], frame[9], frame[10], frame[11]);
        g_tx_log_count++;
    }

    int sent = dev->send(dev->desc, frame, (uint32_t)copied);
    return sent == (int)copied ? ERR_OK : ERR_IF;
}

static err_t xj380_netif_init(struct netif *netif)
{
    netif->name[0]    = 'e';
    netif->name[1]    = '0';
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, g_netdev->mac, ETH_HWADDR_LEN);
    netif->mtu        = 1500;
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
#if LWIP_IPV6 && LWIP_IPV6_MLD
    netif->flags |= NETIF_FLAG_MLD6;
#endif
    netif->output     = etharp_output;
#if LWIP_IPV6
    netif->output_ip6 = ethip6_output;
#endif
    netif->linkoutput = xj380_linkoutput;
    return ERR_OK;
}

static int xj380_sockaddr_to_ip(const struct sockaddr *addr, socklen_t addrlen, ip_addr_t *ip, u16_t *port)
{
    if (addr == NULL || ip == NULL || port == NULL || addrlen < sizeof(struct sockaddr)) {
        return -EINVAL;
    }

    if (addr->sa_family == AF_INET) {
        if (addrlen < sizeof(struct sockaddr_in)) {
            return -EINVAL;
        }

        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        ip4_addr_t                ip4;
        ip4.addr = sin->sin_addr.s_addr;
        ip_addr_copy_from_ip4(*ip, ip4);
        *port = lwip_ntohs(sin->sin_port);
        return 0;
    }

#if LWIP_IPV6
    if (addr->sa_family == AF_INET6) {
        if (addrlen < sizeof(struct sockaddr_in6)) {
            return -EINVAL;
        }

        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;
        ip6_addr_p_t               packed;
        memcpy(&packed, sin6->sin6_addr.s6_addr, sizeof(packed));
        ip_addr_copy_from_ip6_packed(*ip, packed);
        ip6_addr_assign_zone(ip_2_ip6(ip), IP6_UNKNOWN, &g_netif);
        *port = lwip_ntohs(sin6->sin6_port);
        return 0;
    }
#endif

    return -EAFNOSUPPORT;
}

#if LWIP_IPV6
static void xj380_copy_ip6_to_bytes(const ip6_addr_t *ip6, uint8_t out[16])
{
    if (ip6 == NULL || out == NULL) {
        return;
    }

    ip6_addr_p_t packed;
    ip6_addr_copy_to_packed(packed, *ip6);
    memcpy(out, &packed, 16);
}
#endif

static void xj380_ip_to_sockaddr(const ip_addr_t *ip, u16_t port, struct sockaddr *addr, socklen_t *addrlen)
{
    if (addrlen == NULL) {
        return;
    }

#if LWIP_IPV6
    if (ip != NULL && IP_IS_V6(ip)) {
        if (*addrlen < sizeof(struct sockaddr_in6) || addr == NULL) {
            *addrlen = sizeof(struct sockaddr_in6);
            return;
        }

        struct sockaddr_in6 out6;
        memset(&out6, 0, sizeof(out6));
        out6.sin6_family = AF_INET6;
        out6.sin6_port   = lwip_htons(port);
        xj380_copy_ip6_to_bytes(ip_2_ip6(ip), out6.sin6_addr.s6_addr);
#if LWIP_IPV6_SCOPES
        out6.sin6_scope_id = ip6_addr_zone(ip_2_ip6(ip));
#else
        out6.sin6_scope_id = ip6_addr_islinklocal(ip_2_ip6(ip)) ? g_netif.num : 0;
#endif

        memcpy(addr, &out6, sizeof(out6));
        *addrlen = sizeof(out6);
        return;
    }
#endif

    if (*addrlen < sizeof(struct sockaddr_in) || addr == NULL) {
        *addrlen = sizeof(struct sockaddr_in);
        return;
    }

    struct sockaddr_in out;
    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port   = lwip_htons(port);
    if (ip != NULL && IP_IS_V4(ip)) {
        out.sin_addr.s_addr = ip_2_ip4(ip)->addr;
    }

    memcpy(addr, &out, sizeof(out));
    *addrlen = sizeof(out);
}

static void xj380_log_ip_addr(const char *prefix,
                              const ip_addr_t *ip,
                              u16_t port,
                              uint32_t extra_flags,
                              int state,
                              int pending_err)
{
    char        ipbuf[64];
    const char *ip_text = ip != NULL ? ipaddr_ntoa_r(ip, ipbuf, sizeof(ipbuf)) : "(null)";
    printk("%s%s:%u flags=0x%x state=%d pending_err=%d\n",
           prefix,
           ip_text != NULL ? ip_text : "(format-failed)",
           (unsigned int)port,
           (unsigned int)extra_flags,
           state,
           pending_err);
}

static bool xj380_mbox_has_data(sys_mbox_t *mbox)
{
    bool ready;
    spin_lock(&mbox->lock);
    ready = mbox->ptrRead != mbox->ptrWrite;
    spin_unlock(&mbox->lock);
    return ready;
}

static bool xj380_socket_is_udp(const xj380_lwip_socket_t *socket)
{
    return socket != NULL && socket->conn != NULL &&
           NETCONNTYPE_GROUP(netconn_type(socket->conn)) == NETCONN_UDP;
}

static void xj380_dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    xj380_dns_query_t *query = (xj380_dns_query_t *)arg;
    if (query == NULL) {
        return;
    }

    if (ipaddr == NULL) {
        query->status = -EHOSTUNREACH;
        query->done   = 1;
        printk("netserver: dns callback name=%s status=not-found\n", name ? name : "(null)");
        return;
    }

    query->addr = *ipaddr;
    query->status = 0;
    query->done   = 1;
    char ipbuf[64];
    printk("netserver: dns callback name=%s addr=%s\n",
           name ? name : "(null)",
           ipaddr_ntoa_r(ipaddr, ipbuf, sizeof(ipbuf)));
}

static ssize_t xj380_socket_read_impl(xj380_lwip_socket_t *socket, void *buffer, size_t size, uint64_t flags)
{
    u8_t    api_flags = NETCONN_NOAUTORCVD;
    ssize_t recvd     = 0;
    size_t  left      = size;

    if (flags & O_NONBLOCK) {
        api_flags |= NETCONN_DONTBLOCK;
    }

    if (g_read_log_count < 32) {
        printk("[DEBUG-xbps-net] read begin socket=%p size=%u flags=0x%x api_flags=0x%x state=%d pending_err=%d rx_buf=%p recv_ready=%d nonblock=%d\n",
               socket,
               (unsigned int)size,
               (unsigned int)flags,
               (unsigned int)api_flags,
               socket != NULL && socket->conn != NULL ? (int)socket->conn->state : -1,
               socket != NULL && socket->conn != NULL ? (int)socket->conn->pending_err : -1,
               socket != NULL ? socket->rx_buf : NULL,
               socket != NULL && socket->conn != NULL ? (int)xj380_mbox_has_data(&socket->conn->recvmbox) : -1,
               socket != NULL && socket->conn != NULL ? (int)netconn_is_nonblocking(socket->conn) : -1);
        g_read_log_count++;
    }

    while (left > 0) {
        struct pbuf *p = socket->rx_buf;
        if (p == NULL) {
            err_t err = netconn_recv_tcp_pbuf_flags(socket->conn, &p, api_flags);
            if (g_read_log_count < 64) {
                printk("[DEBUG-xbps-net] read recv err=%d p=%p tot=%u len=%u state=%d pending_err=%d recvd=%lld\n",
                       (int)err,
                       p,
                       p != NULL ? (unsigned int)p->tot_len : 0,
                       p != NULL ? (unsigned int)p->len : 0,
                       (int)socket->conn->state,
                       (int)socket->conn->pending_err,
                       (long long)recvd);
                g_read_log_count++;
            }
            if (err != ERR_OK) {
                if (recvd > 0) {
                    break;
                }
                if (err == ERR_CLSD) {
                    return 0;
                }
                return xj380_err_to_errno(err);
            }
            socket->rx_buf = p;
        }

        u16_t copy_len = (u16_t)MIN(left, (size_t)p->tot_len);
        pbuf_copy_partial(p, (uint8_t *)buffer + recvd, copy_len, 0);
        recvd += copy_len;
        left  -= copy_len;

        if (p->tot_len > copy_len) {
            socket->rx_buf = pbuf_free_header(p, copy_len);
        } else {
            socket->rx_buf = NULL;
            pbuf_free(p);
        }

        api_flags |= NETCONN_DONTBLOCK | NETCONN_NOFIN;
        if ((flags & O_NONBLOCK) && recvd > 0) {
            break;
        }
    }

    if (recvd > 0) {
        netconn_tcp_recvd(socket->conn, (size_t)recvd);
    }
    if (g_read_log_count < 96) {
        printk("[DEBUG-xbps-net] read end ret=%lld state=%d pending_err=%d rx_buf=%p recv_ready=%d nonblock=%d\n",
               (long long)recvd,
               (int)socket->conn->state,
               (int)socket->conn->pending_err,
               socket->rx_buf,
               (int)xj380_mbox_has_data(&socket->conn->recvmbox),
               (int)netconn_is_nonblocking(socket->conn));
        g_read_log_count++;
    }
    return recvd;
}

static ssize_t xj380_socket_recvfrom_impl(xj380_lwip_socket_t *socket,
                                          void *buffer,
                                          size_t size,
                                          uint64_t flags,
                                          struct sockaddr *addr,
                                          socklen_t *addrlen)
{
    if (size > 0xffffU) {
        size = 0xffffU;
    }

    u8_t api_flags = 0;
    if (flags & O_NONBLOCK) {
        api_flags |= NETCONN_DONTBLOCK;
    }

    struct netbuf *buf = NULL;
    err_t err = netconn_recv_udp_raw_netbuf_flags(socket->conn, &buf, api_flags);
    if (err != ERR_OK) {
        return xj380_err_to_errno(err);
    }
    if (buf == NULL) {
        return -EIO;
    }

    u16_t copy_len = (u16_t)MIN(size, (size_t)netbuf_len(buf));
    netbuf_copy_partial(buf, buffer, copy_len, 0);
    if (addrlen != NULL) {
        xj380_ip_to_sockaddr(netbuf_fromaddr(buf), netbuf_fromport(buf), addr, addrlen);
    }
    netbuf_delete(buf);
    return copy_len;
}

static ssize_t xj380_socket_sendto_impl(xj380_lwip_socket_t *socket,
                                        const void *buffer,
                                        size_t size,
                                        uint64_t flags,
                                        const struct sockaddr *addr,
                                        socklen_t addrlen)
{
    (void)flags;
    if (size > 0xffffU) {
        return -EMSGSIZE;
    }

    struct netbuf buf;
    memset(&buf, 0, sizeof(buf));

    if (addr != NULL) {
        ip_addr_t ip;
        u16_t     port;
        int ret = xj380_sockaddr_to_ip(addr, addrlen, &ip, &port);
        if (ret < 0) {
            return ret;
        }
        ip_addr_set(&buf.addr, &ip);
        buf.port = port;
    }

    err_t err = netbuf_ref(&buf, buffer, (u16_t)size);
    if (err == ERR_OK) {
        err = netconn_send(socket->conn, &buf);
    }
    netbuf_free(&buf);
    if (err != ERR_OK) {
        return xj380_err_to_errno(err);
    }
    return (ssize_t)size;
}

static int xj380_provider_create(int domain, int type, int protocol, void **out_socket)
{
    printk("netserver: socket create domain=%d type=%d protocol=%d stack_ready=%d\n",
           domain, type, protocol, (int)g_stack_ready);

    if (out_socket == NULL) {
        return -EINVAL;
    }
    if (!g_stack_ready) {
        for (int retry = 0; retry < 1000 && !g_stack_ready; ++retry) {
            xj380_update_net_state();
            delay_ms_hp(10);
        }
        if (!g_stack_ready) {
            return -ENETDOWN;
        }
    }
    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        return -EAFNOSUPPORT;
    }
    if (type == SOCK_STREAM && protocol != 0 && protocol != 6) {
        return -EPROTONOSUPPORT;
    }
    if (type == SOCK_DGRAM && protocol != 0 && protocol != 17) {
        return -EPROTONOSUPPORT;
    }

    enum netconn_type conn_type;
    if (domain == AF_INET) {
        conn_type = type == SOCK_DGRAM ? NETCONN_UDP : NETCONN_TCP;
    }
#if LWIP_IPV6
    else if (domain == AF_INET6) {
        conn_type = type == SOCK_DGRAM ? NETCONN_UDP_IPV6 : NETCONN_TCP_IPV6;
    }
#endif
    else {
        return -EAFNOSUPPORT;
    }

    printk("netserver: netconn_new begin type=%d\n", (int)conn_type);
    struct netconn *conn = netconn_new(conn_type);
    if (conn == NULL) {
        printk("netserver: netconn_new failed\n");
        return -ENOMEM;
    }

    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)calloc(1, sizeof(xj380_lwip_socket_t));
    if (socket == NULL) {
        netconn_delete(conn);
        return -ENOMEM;
    }

    socket->conn = conn;
    *out_socket  = socket;
    printk("netserver: socket create ok socket=%p\n", socket);
    return 0;
}

static int xj380_provider_close(void *socket_ptr)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    if (socket == NULL) {
        return -EINVAL;
    }

    if (socket->rx_buf != NULL) {
        pbuf_free(socket->rx_buf);
    }
    if (socket->conn != NULL) {
        netconn_delete(socket->conn);
    }
    free(socket);
    return 0;
}

static ssize_t xj380_provider_read(void *socket_ptr, void *buffer, size_t size, uint64_t flags)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    if (socket == NULL || buffer == NULL) {
        return -EINVAL;
    }
    if (xj380_socket_is_udp(socket)) {
        return xj380_socket_recvfrom_impl(socket, buffer, size, flags, NULL, NULL);
    }
    return xj380_socket_read_impl(socket, buffer, size, flags);
}

static ssize_t xj380_provider_write(void *socket_ptr, const void *buffer, size_t size, uint64_t flags)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    if (socket == NULL || buffer == NULL) {
        return -EINVAL;
    }
    if (xj380_socket_is_udp(socket)) {
        return xj380_socket_sendto_impl(socket, buffer, size, flags, NULL, 0);
    }

    size_t written   = 0;
    u8_t   api_flags = NETCONN_COPY;
    if (flags & O_NONBLOCK) {
        api_flags |= NETCONN_DONTBLOCK;
    }

    if (g_write_log_count < 16) {
        printk("netserver: write size=%u flags=0x%x state=%d pending_err=%d\n",
               (unsigned int)size,
               (unsigned int)flags,
               (int)socket->conn->state,
               (int)socket->conn->pending_err);
        g_write_log_count++;
    }

    err_t err = netconn_write_partly(socket->conn, buffer, size, api_flags, &written);
    if (err != ERR_OK) {
        if (g_write_log_count < 24) {
            printk("netserver: write err=%d written=%u state=%d pending_err=%d\n",
                   (int)err,
                   (unsigned int)written,
                   (int)socket->conn->state,
                   (int)socket->conn->pending_err);
            g_write_log_count++;
        }
        if (written > 0) {
            return (ssize_t)written;
        }
        return xj380_err_to_errno(err);
    }
    if (g_write_log_count < 32) {
        printk("[DEBUG-xbps-net] write ok requested=%u written=%u state=%d pending_err=%d nonblock=%d\n",
               (unsigned int)size,
               (unsigned int)written,
               (int)socket->conn->state,
               (int)socket->conn->pending_err,
               (int)netconn_is_nonblocking(socket->conn));
        g_write_log_count++;
    }
    return (ssize_t)written;
}

static ssize_t xj380_provider_recvfrom(void *socket_ptr,
                                       void *buffer,
                                       size_t size,
                                       uint64_t flags,
                                       struct sockaddr *addr,
                                       socklen_t *addrlen)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    if (socket == NULL || buffer == NULL) {
        return -EINVAL;
    }
    if (xj380_socket_is_udp(socket)) {
        return xj380_socket_recvfrom_impl(socket, buffer, size, flags, addr, addrlen);
    }
    return xj380_socket_read_impl(socket, buffer, size, flags);
}

static ssize_t xj380_provider_sendto(void *socket_ptr,
                                     const void *buffer,
                                     size_t size,
                                     uint64_t flags,
                                     const struct sockaddr *addr,
                                     socklen_t addrlen)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    if (socket == NULL || buffer == NULL) {
        return -EINVAL;
    }
    if (xj380_socket_is_udp(socket)) {
        return xj380_socket_sendto_impl(socket, buffer, size, flags, addr, addrlen);
    }
    if (addr != NULL) {
        return -EISCONN;
    }
    return xj380_provider_write(socket_ptr, buffer, size, flags);
}

static int xj380_provider_bind(void *socket_ptr, const struct sockaddr *addr, socklen_t addrlen)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    ip_addr_t            ip;
    u16_t                port;

    if (socket == NULL) {
        return -EINVAL;
    }

    int ret = xj380_sockaddr_to_ip(addr, addrlen, &ip, &port);
    if (ret < 0) {
        return ret;
    }

    return xj380_err_to_errno(netconn_bind(socket->conn, &ip, port));
}

static int xj380_provider_connect(void *socket_ptr, const struct sockaddr *addr, socklen_t addrlen)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    ip_addr_t            ip;
    u16_t                port;

    if (socket == NULL) {
        return -EINVAL;
    }

    int ret = xj380_sockaddr_to_ip(addr, addrlen, &ip, &port);
    if (ret < 0) {
        return ret;
    }

    if (g_connect_log_count < 16) {
        xj380_log_ip_addr("netserver: connect dst=", &ip, port,
                          (uint32_t)netconn_is_nonblocking(socket->conn),
                          (int)socket->conn->state,
                          (int)socket->conn->pending_err);
        g_connect_log_count++;
    }

    err_t err = netconn_connect(socket->conn, &ip, port);

    if (g_connect_log_count < 24) {
        printk("netserver: connect ret=%d state=%d pending_err=%d\n",
               (int)err,
               (int)socket->conn->state,
               (int)socket->conn->pending_err);
        g_connect_log_count++;
    }

    return xj380_err_to_errno(err);
}

static int xj380_provider_sockname_impl(void *socket_ptr, struct sockaddr *addr, socklen_t *addrlen, u8_t local)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    if (socket == NULL || addrlen == NULL) {
        return -EINVAL;
    }

    ip_addr_t ip;
    u16_t     port = 0;
    err_t err = netconn_getaddr(socket->conn, &ip, &port, local);
    if (err != ERR_OK) {
        return xj380_err_to_errno(err);
    }

    xj380_ip_to_sockaddr(&ip, port, addr, addrlen);
    return 0;
}

static int xj380_provider_getsockname(void *socket_ptr, struct sockaddr *addr, socklen_t *addrlen)
{
    return xj380_provider_sockname_impl(socket_ptr, addr, addrlen, 1);
}

static int xj380_provider_getpeername(void *socket_ptr, struct sockaddr *addr, socklen_t *addrlen)
{
    return xj380_provider_sockname_impl(socket_ptr, addr, addrlen, 0);
}

static int xj380_provider_getsockopt(void *socket_ptr, int level, int optname, void *optval, socklen_t *optlen)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    if (socket == NULL || socket->conn == NULL || optval == NULL || optlen == NULL) {
        return -EINVAL;
    }
    if (*optlen < sizeof(int)) {
        return -EINVAL;
    }

    int value = 0;
    err_t pending_before = socket->conn->pending_err;
    switch (level) {
    case SOL_SOCKET:
        switch (optname) {
        case SO_TYPE:
            switch (NETCONNTYPE_GROUP(netconn_type(socket->conn))) {
            case NETCONN_TCP: value = SOCK_STREAM; break;
            case NETCONN_UDP: value = SOCK_DGRAM; break;
            default: value = SOCK_RAW; break;
            }
            break;
        case SO_ERROR:
            value = err_to_errno(netconn_err(socket->conn));
            break;
        case SO_KEEPALIVE:
            value = socket->conn->pcb.ip != NULL ? ip_get_option(socket->conn->pcb.ip, SOF_KEEPALIVE) != 0 : 0;
            break;
        case SO_BROADCAST:
            value = socket->conn->pcb.ip != NULL ? ip_get_option(socket->conn->pcb.ip, SOF_BROADCAST) != 0 : 0;
            break;
        case SO_REUSEADDR:
            value = socket->conn->pcb.ip != NULL ? ip_get_option(socket->conn->pcb.ip, SOF_REUSEADDR) != 0 : 0;
            break;
        default:
            return -ENOPROTOOPT;
        }
        break;
    case IPPROTO_TCP:
        if (NETCONNTYPE_GROUP(netconn_type(socket->conn)) != NETCONN_TCP || socket->conn->pcb.tcp == NULL) {
            return -ENOPROTOOPT;
        }
        switch (optname) {
        case TCP_NODELAY:
            value = tcp_nagle_disabled(socket->conn->pcb.tcp);
            break;
        case TCP_KEEPALIVE:
            value = (int)socket->conn->pcb.tcp->keep_idle;
            break;
        default:
            return -ENOPROTOOPT;
        }
        break;
    default:
        return -ENOPROTOOPT;
    }

    *(int *)optval = value;
    *optlen = sizeof(int);
    if (g_getsockopt_log_count < 48) {
        printk("[DEBUG-xbps-net] getsockopt socket=%p level=%d opt=%d value=%d len=%u state=%d pending_before=%d pending_after=%d\n",
               socket,
               level,
               optname,
               value,
               (unsigned int)*optlen,
               (int)socket->conn->state,
               (int)pending_before,
               (int)socket->conn->pending_err);
        g_getsockopt_log_count++;
    }
    return 0;
}

static int xj380_provider_listen(void *socket_ptr, int backlog)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    if (socket == NULL) {
        return -EINVAL;
    }

    err_t err = netconn_listen_with_backlog(socket->conn, (u8_t)backlog);
    if (err != ERR_OK) {
        return xj380_err_to_errno(err);
    }
    socket->is_listener = true;
    return 0;
}

static int xj380_provider_accept(void *socket_ptr, void **out_socket, struct sockaddr *addr, socklen_t *addrlen)
{
    xj380_lwip_socket_t *listener = (xj380_lwip_socket_t *)socket_ptr;
    if (listener == NULL || out_socket == NULL) {
        return -EINVAL;
    }

    struct netconn *new_conn = NULL;
    err_t           err      = netconn_accept(listener->conn, &new_conn);
    if (err != ERR_OK) {
        return xj380_err_to_errno(err);
    }

    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)calloc(1, sizeof(xj380_lwip_socket_t));
    if (socket == NULL) {
        netconn_delete(new_conn);
        return -ENOMEM;
    }

    socket->conn = new_conn;
    if (netconn_is_nonblocking(listener->conn)) {
        netconn_set_nonblocking(socket->conn, 1);
    }

    if (addrlen != NULL) {
        ip_addr_t peer_addr;
        u16_t     peer_port = 0;
        if (netconn_peer(new_conn, &peer_addr, &peer_port) == ERR_OK) {
            xj380_ip_to_sockaddr(&peer_addr, peer_port, addr, addrlen);
        }
    }

    *out_socket = socket;
    return 0;
}

static int xj380_provider_shutdown(void *socket_ptr, int how)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    if (socket == NULL) {
        return -EINVAL;
    }

    int shut_rx = how != SHUT_WR;
    int shut_tx = how != SHUT_RD;
    return xj380_err_to_errno(netconn_shutdown(socket->conn, shut_rx, shut_tx));
}

static int xj380_provider_poll(void *socket_ptr, size_t events)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    if (socket == NULL) {
        return -EINVAL;
    }

    int out = 0;
    bool recv_ready = false;
    bool accept_ready = false;

    if (events & EPOLLIN) {
        if (socket->rx_buf != NULL) {
            out |= EPOLLIN;
        } else if (socket->is_listener) {
            accept_ready = xj380_mbox_has_data(&socket->conn->acceptmbox);
            if (accept_ready) {
                out |= EPOLLIN;
            }
        } else if ((recv_ready = xj380_mbox_has_data(&socket->conn->recvmbox)) ||
                   socket->conn->pending_err == ERR_CLSD) {
            out |= EPOLLIN;
        }
    }

    if (events & EPOLLOUT) {
        if (socket->conn->state != NETCONN_CONNECT && socket->conn->pending_err == ERR_OK) {
            out |= EPOLLOUT;
        }
    }

    if (socket->conn->pending_err == ERR_CLSD) {
        out |= EPOLLHUP;
    } else if (socket->conn->pending_err != ERR_OK) {
        out |= EPOLLERR;
    }

    if (g_poll_log_count < 96) {
        printk("[DEBUG-xbps-net] poll socket=%p events=0x%x out=0x%x state=%d pending_err=%d rx_buf=%p recv_ready=%d accept_ready=%d nonblock=%d\n",
               socket,
               (unsigned int)events,
               (unsigned int)out,
               (int)socket->conn->state,
               (int)socket->conn->pending_err,
               socket->rx_buf,
               (int)recv_ready,
               (int)accept_ready,
               (int)netconn_is_nonblocking(socket->conn));
        g_poll_log_count++;
    }
    return out;
}

static int xj380_provider_set_flags(void *socket_ptr, uint64_t flags)
{
    xj380_lwip_socket_t *socket = (xj380_lwip_socket_t *)socket_ptr;
    if (socket == NULL) {
        return -EINVAL;
    }

    netconn_set_nonblocking(socket->conn, (flags & O_NONBLOCK) != 0);
    return 0;
}

static int xj380_resolve_host(const char *hostname, uint8_t family, ip_addr_t *out_addr)
{
    if (hostname == NULL || out_addr == NULL || hostname[0] == '\0') {
        return -EINVAL;
    }
#if !LWIP_IPV6
    if (family == AF_INET6) {
        return -EAFNOSUPPORT;
    }
#endif
    if (!g_stack_ready) {
        for (int retry = 0; retry < 1000 && !g_stack_ready; ++retry) {
            xj380_update_net_state();
            delay_ms_hp(10);
        }
        if (!g_stack_ready) {
            return -ENETDOWN;
        }
    }

    ip_addr_t resolved_addr;
    if (ipaddr_aton(hostname, &resolved_addr)) {
        if ((family == AF_INET && !IP_IS_V4(&resolved_addr)) ||
            (family == AF_INET6 && !IP_IS_V6(&resolved_addr))) {
            return -EAFNOSUPPORT;
        }
        *out_addr = resolved_addr;
        return 0;
    }

    printk("netserver: dns resolve start host=%s family=%u\n",
           hostname, (unsigned int)family);

    xj380_dns_query_t query;
    memset(&query, 0, sizeof(query));

    LOCK_TCPIP_CORE();
    u8_t dns_addrtype = LWIP_DNS_ADDRTYPE_IPV4;
#if LWIP_IPV6
    if (family == AF_INET6) {
        dns_addrtype = LWIP_DNS_ADDRTYPE_IPV6;
    }
#endif
    err_t err = dns_gethostbyname_addrtype(hostname, &resolved_addr, xj380_dns_found_cb, &query, dns_addrtype);
    UNLOCK_TCPIP_CORE();

    if (err != ERR_OK) {
        if (err != ERR_INPROGRESS) {
            printk("netserver: dns resolve host=%s err=%d\n", hostname, (int)err);
            return xj380_err_to_errno(err);
        }

        uint32_t start_ms = sys_now();
        while (!query.done) {
            if ((uint32_t)(sys_now() - start_ms) >= 5000U) {
                printk("netserver: dns resolve timeout host=%s\n", hostname);
                return -ETIMEDOUT;
            }
            scheduler_yield();
        }

        if (query.status < 0) {
            printk("netserver: dns resolve failed host=%s status=%d\n", hostname, query.status);
            return query.status;
        }
        resolved_addr = query.addr;
    }

    if ((family == AF_INET && !IP_IS_V4(&resolved_addr)) ||
        (family == AF_INET6 && !IP_IS_V6(&resolved_addr))) {
        return -EAFNOSUPPORT;
    }

    *out_addr = resolved_addr;
    char ipbuf[64];
    printk("netserver: dns resolve done host=%s addr=%s\n",
           hostname,
           ipaddr_ntoa_r(&resolved_addr, ipbuf, sizeof(ipbuf)));
    return 0;
}

static int xj380_provider_resolve_ipv4(const char *hostname, uint32_t *out_addr)
{
    if (out_addr == NULL) {
        return -EINVAL;
    }

    ip_addr_t resolved_addr;
    int       ret = xj380_resolve_host(hostname, AF_INET, &resolved_addr);
    if (ret < 0) {
        return ret;
    }

    *out_addr = ip_2_ip4(&resolved_addr)->addr;
    return 0;
}

static int xj380_provider_resolve_ipv6(const char *hostname, uint8_t out_addr[16])
{
    if (out_addr == NULL) {
        return -EINVAL;
    }

#if !LWIP_IPV6
    (void)hostname;
    memset(out_addr, 0, 16);
    return -EAFNOSUPPORT;
#else
    ip_addr_t resolved_addr;
    int       ret = xj380_resolve_host(hostname, AF_INET6, &resolved_addr);
    if (ret < 0) {
        return ret;
    }

    xj380_copy_ip6_to_bytes(ip_2_ip6(&resolved_addr), out_addr);
    return 0;
#endif
}

static int xj380_provider_set_dns_server(uint32_t addr)
{
    if (addr == 0) {
        g_dns_manual_override = false;
        g_dns_manual_addr     = 0;
        return 0;
    }

    xj380_apply_dns_server(addr);
    g_dns_manual_override = true;
    g_dns_manual_addr     = addr;
    return 0;
}

static int xj380_provider_get_dns_server(uint32_t *out_addr)
{
    if (out_addr == NULL) {
        return -EINVAL;
    }

    const ip_addr_t *dns_addr = dns_getserver(0);
    if (dns_addr == NULL || !IP_IS_V4(dns_addr)) {
        return -ENODATA;
    }

    *out_addr = ip_2_ip4(dns_addr)->addr;
    return 0;
}

static int xj380_provider_set_ipv4_config(const socket_ipv4_config_t *config)
{
    if (config == NULL) {
        return -EINVAL;
    }
    if (!g_stack_initialized) {
        return -EAGAIN;
    }

    switch (config->method) {
    case SOCKET_IPV4_METHOD_DHCP:
        return xj380_apply_dhcp_ipv4_config();
    case SOCKET_IPV4_METHOD_MANUAL:
        return xj380_apply_manual_ipv4_config(config->ipv4_addr, config->ipv4_netmask,
                                              config->ipv4_gateway);
    default:
        return -EINVAL;
    }
}

static int xj380_provider_get_netinfo(socket_netinfo_t *out_info)
{
    if (out_info == NULL) {
        return -EINVAL;
    }

    memset(out_info, 0, sizeof(*out_info));

    if (g_netdev != NULL) {
        memcpy(out_info->mac, g_netdev->mac, sizeof(out_info->mac));
        out_info->mtu = g_netdev->mtu;
    }

    out_info->ifname[0] = 'e';
    out_info->ifname[1] = '0';
    out_info->ifname[2] = '\0';
    out_info->stack_ready = g_stack_ready ? 1 : 0;
    out_info->ipv4_method = g_ipv4_method;

    if (!g_stack_ready) {
        return 0;
    }

    out_info->link_up = (netif_is_up(&g_netif) && netif_is_link_up(&g_netif)) ? 1 : 0;
    out_info->mtu     = g_netif.mtu;
    out_info->dhcp_state = xj380_current_dhcp_state();
    memcpy(out_info->mac, g_netif.hwaddr, sizeof(out_info->mac));

    const ip4_addr_t *ip = netif_ip4_addr(&g_netif);
    if (ip != NULL && !ip4_addr_isany_val(*ip)) {
        out_info->ip_ready     = 1;
        out_info->ipv4_addr    = ip->addr;
        out_info->ipv4_netmask = netif_ip4_netmask(&g_netif)->addr;
        out_info->ipv4_gateway = netif_ip4_gw(&g_netif)->addr;
    }

#if LWIP_IPV6
    for (u8_t i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
        if (!ip6_addr_isvalid(netif_ip6_addr_state(&g_netif, i))) {
            continue;
        }

        const ip6_addr_t *ip6 = netif_ip6_addr(&g_netif, i);
        if (ip6 == NULL || ip6_addr_isany(ip6)) {
            continue;
        }

        if (!out_info->ipv6_linklocal_ready && ip6_addr_islinklocal(ip6)) {
            xj380_copy_ip6_to_bytes(ip6, out_info->ipv6_linklocal);
            out_info->ipv6_linklocal_ready = 1;
            out_info->ip_ready = 1;
            continue;
        }

        if (!out_info->ipv6_global_ready && !ip6_addr_islinklocal(ip6) && !ip6_addr_isloopback(ip6)) {
            xj380_copy_ip6_to_bytes(ip6, out_info->ipv6_global);
            out_info->ipv6_global_ready = 1;
            out_info->ip_ready = 1;
        }
    }
#endif

    uint32_t dns_addr = 0;
    if (xj380_provider_get_dns_server(&dns_addr) == 0) {
        out_info->dns_server = dns_addr;
    }

    return 0;
}

static socket_provider_t g_socket_provider = {
    .create    = xj380_provider_create,
    .close     = xj380_provider_close,
    .read      = xj380_provider_read,
    .write     = xj380_provider_write,
    .recvfrom  = xj380_provider_recvfrom,
    .sendto    = xj380_provider_sendto,
    .bind      = xj380_provider_bind,
    .connect   = xj380_provider_connect,
    .getsockname = xj380_provider_getsockname,
    .getpeername = xj380_provider_getpeername,
    .getsockopt = xj380_provider_getsockopt,
    .listen    = xj380_provider_listen,
    .accept    = xj380_provider_accept,
    .shutdown  = xj380_provider_shutdown,
    .poll      = xj380_provider_poll,
    .set_flags = xj380_provider_set_flags,
    .resolve_ipv4 = xj380_provider_resolve_ipv4,
    .resolve_ipv6 = xj380_provider_resolve_ipv6,
    .set_dns_server = xj380_provider_set_dns_server,
    .get_dns_server = xj380_provider_get_dns_server,
    .set_ipv4_config = xj380_provider_set_ipv4_config,
    .get_netinfo = xj380_provider_get_netinfo,
};

static void xj380_update_net_state(void)
{
    if (!g_stack_initialized) {
        return;
    }

    uint8_t dhcp_state = xj380_current_dhcp_state();
    if (dhcp_state != g_last_dhcp_state) {
        g_last_dhcp_state = dhcp_state;
        printk("netserver: dhcp state -> %s (%u)\n",
               xj380_dhcp_state_name(dhcp_state), (unsigned int)dhcp_state);
        if (dhcp_state != DHCP_STATE_BOUND && dhcp_state != DHCP_STATE_RENEWING &&
            dhcp_state != DHCP_STATE_REBINDING) {
            g_dhcp_ready_logged = false;
        }
    }

    if (g_dns_manual_override) {
        const ip_addr_t *dns_addr = dns_getserver(0);
        uint32_t current_dns = 0;
        if (dns_addr != NULL && IP_IS_V4(dns_addr)) {
            current_dns = ip_2_ip4(dns_addr)->addr;
        }
        if (current_dns != g_dns_manual_addr) {
            xj380_apply_dns_server(g_dns_manual_addr);
        }
    }

    const ip4_addr_t *ip = netif_ip4_addr(&g_netif);
    if (!g_stack_ready && xj380_ipv4_ready()) {
        g_stack_ready = true;
    }

    if (!g_dhcp_ready_logged && dhcp_supplied_address(&g_netif) && !ip4_addr_isany_val(*ip)) {
        const ip4_addr_t *mask = netif_ip4_netmask(&g_netif);
        const ip4_addr_t *gw   = netif_ip4_gw(&g_netif);
        g_dhcp_ready_logged    = true;

        printk("netserver: dhcp lease %u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u\n",
               ip4_addr1(ip), ip4_addr2(ip), ip4_addr3(ip), ip4_addr4(ip),
               ip4_addr1(mask), ip4_addr2(mask), ip4_addr3(mask), ip4_addr4(mask),
               ip4_addr1(gw), ip4_addr2(gw), ip4_addr3(gw), ip4_addr4(gw));
    }
}

static void xj380_poll_input(void)
{
    uint8_t rx_buffer[2048];

    while (true) {
        int frame_len = g_netdev->recv(g_netdev->desc, rx_buffer, sizeof(rx_buffer));
        if (frame_len <= 0) {
            return;
        }

        if (g_rx_log_count < 24 && frame_len >= 14) {
            uint16_t eth_type = ((uint16_t)rx_buffer[12] << 8) | rx_buffer[13];
            printk("netserver: rx frame len=%u type=0x%04x dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x netif_flags=0x%x hwaddr_len=%u\n",
                   (unsigned int)frame_len,
                   (unsigned int)eth_type,
                   rx_buffer[0], rx_buffer[1], rx_buffer[2], rx_buffer[3], rx_buffer[4], rx_buffer[5],
                   rx_buffer[6], rx_buffer[7], rx_buffer[8], rx_buffer[9], rx_buffer[10], rx_buffer[11],
                   (unsigned int)g_netif.flags,
                   (unsigned int)g_netif.hwaddr_len);
            g_rx_log_count++;
        }

        struct pbuf *packet = pbuf_alloc(PBUF_RAW, (u16_t)frame_len, PBUF_POOL);
        if (packet == NULL) {
            continue;
        }

        size_t copied = 0;
        for (struct pbuf *cursor = packet; cursor != NULL; cursor = cursor->next) {
            memcpy(cursor->payload, rx_buffer + copied, cursor->len);
            copied += cursor->len;
        }

        if (g_netif.input(packet, &g_netif) != ERR_OK) {
            pbuf_free(packet);
        }
    }
}

static int xj380_setup_stack(void)
{
    bool       tcpip_ready = false;

    tcpip_init([](void *arg) { *(bool *)arg = true; }, &tcpip_ready);
    while (!tcpip_ready) {
        scheduler_yield();
    }

    LOCK_TCPIP_CORE();

    struct netif *added = netif_add(&g_netif, NULL, NULL, NULL, g_netdev, xj380_netif_init, tcpip_input);
    if (added == NULL) {
        UNLOCK_TCPIP_CORE();
        return -EIO;
    }

    netif_set_default(&g_netif);
    netif_set_link_up(&g_netif);
    netif_set_up(&g_netif);

    // The current lwIP port leaves admin/link flags and hwaddr_len inconsistent
    // after init; force the ethernet netif state so ARP/TCP can run.
    g_netif.hwaddr_len = ETH_HWADDR_LEN;
    memcpy(g_netif.hwaddr, g_netdev->mac, ETH_HWADDR_LEN);
    g_netif.flags |= (NETIF_FLAG_UP | NETIF_FLAG_LINK_UP);
#if LWIP_IPV6_AUTOCONFIG
    g_netif.ip6_autoconfig_enabled = 1;
#endif
#if LWIP_IPV6
    netif_create_ip6_linklocal_address(&g_netif, 1);
#endif

    printk("netserver: netif ready flags=0x%x up=%u link=%u mtu=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           (unsigned int)g_netif.flags,
           (unsigned int)netif_is_up(&g_netif),
           (unsigned int)netif_is_link_up(&g_netif),
           (unsigned int)g_netif.mtu,
           g_netif.hwaddr[0], g_netif.hwaddr[1], g_netif.hwaddr[2],
           g_netif.hwaddr[3], g_netif.hwaddr[4], g_netif.hwaddr[5]);

#if LWIP_IPV6
    {
        char             ip6buf[64];
        const ip6_addr_t *ll = netif_ip6_addr(&g_netif, 0);
        printk("netserver: ipv6 link-local=%s\n",
               ll != NULL ? ip6addr_ntoa_r(ll, ip6buf, sizeof(ip6buf)) : "(null)");
    }
#endif

    err_t dhcp_err = dhcp_start(&g_netif);
    if (dhcp_err != ERR_OK) {
        printk("netserver: dhcp_start failed: %d\n", (int)dhcp_err);
        UNLOCK_TCPIP_CORE();
        return xj380_err_to_errno(dhcp_err);
    }

    if (g_dns_manual_override && g_dns_manual_addr != 0) {
        xj380_apply_dns_server(g_dns_manual_addr);
        ip4_addr_t ip4;
        ip4.addr = g_dns_manual_addr;
        printk("netserver: default dns -> %u.%u.%u.%u\n",
               ip4_addr1(&ip4), ip4_addr2(&ip4), ip4_addr3(&ip4), ip4_addr4(&ip4));
    }

    printk("netserver: dhcp client started on e0\n");

    UNLOCK_TCPIP_CORE();

    g_dhcp_started      = true;
    g_dhcp_ready_logged = false;
    g_ipv4_method       = SOCKET_IPV4_METHOD_DHCP;
    g_last_dhcp_state   = 0xff;
    g_stack_initialized = true;
    g_stack_ready = xj380_ipv4_ready();
    return 0;
}

static void netserver_thread(uint64_t arg)
{
    (void)arg;
    printk("netserver: worker started\n");

    while (g_netdev == NULL) {
        g_netdev = get_default_netdev();
        if (g_netdev == NULL) {
            delay_ms_hp(100);
        }
    }

    printk("netserver: netdev ready, mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           g_netdev->mac[0], g_netdev->mac[1], g_netdev->mac[2],
           g_netdev->mac[3], g_netdev->mac[4], g_netdev->mac[5]);

    int ret = xj380_setup_stack();
    if (ret != 0) {
        printk("netserver: init failed: %d\n", ret);
        return;
    }

    while (true) {
        xj380_poll_input();
        xj380_update_net_state();
        delay_ms_hp(1);
    }
}

extern "C"
__attribute__((used)) __attribute__((visibility("default"))) int dlstart(void)
{
    return 0;
}

extern "C"
__attribute__((used)) __attribute__((visibility("default"))) int dlmain(void)
{
    if (!g_provider_registered) {
        int ret = socket_provider_register(&g_socket_provider);
        if (ret < 0) {
            printk("netserver: provider register failed: %d\n", ret);
            return ret;
        }
        g_provider_registered = true;
    }

    if (g_worker_started) {
        printk("netserver: dlmain ignored, worker already started\n");
        return 0;
    }

    g_worker_started = true;
    size_t tid = create_kernel_thread((void *)netserver_thread, NULL, (char *)"netserver", NULL);
    return tid == 0 ? -EIO : 0;
}
