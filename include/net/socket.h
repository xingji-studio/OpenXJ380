#pragma once

#include <fs/vfs/vfs.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t sa_family_t;
typedef uint16_t in_port_t;
typedef uint32_t socklen_t;

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_LOCAL  AF_UNIX
#define AF_INET   2
#define AF_INET6  10

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define SOCK_TYPE_MASK 0xf
#define SOCK_CLOEXEC 02000000
#define SOCK_NONBLOCK 00004000

#define SOL_SOCKET 1
#define SO_DEBUG 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_LINGER 13
#define SO_RCVLOWAT 18
#define SO_SNDLOWAT 19
#define SO_ACCEPTCONN 30
#define SO_SNDTIMEO 66
#define SO_RCVTIMEO 67

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define TCP_NODELAY 1
#define TCP_KEEPALIVE 2
#define TCP_KEEPIDLE 3
#define TCP_KEEPINTVL 4
#define TCP_KEEPCNT 5

#define MSG_PEEK 0x01
#define MSG_WAITALL 0x02
#define MSG_OOB 0x04
#define MSG_DONTWAIT 0x08
#define MSG_MORE 0x10
#define MSG_NOSIGNAL 0x20

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

struct in_addr {
    uint32_t s_addr;
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_in {
    sa_family_t     sin_family;
    in_port_t       sin_port;
    struct in_addr  sin_addr;
    unsigned char   sin_zero[8];
};

struct sockaddr_in6 {
    sa_family_t     sin6_family;
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

typedef struct socket_netinfo {
    uint32_t ipv4_addr;
    uint32_t ipv4_netmask;
    uint32_t ipv4_gateway;
    uint32_t dns_server;
    uint32_t mtu;
    uint8_t  mac[6];
    uint8_t  link_up;
    uint8_t  stack_ready;
    uint8_t  ip_ready;
    uint8_t  ipv4_method;
    uint8_t  dhcp_state;
    uint8_t  reserved[3];
    char     ifname[16];
    uint8_t  ipv6_linklocal[16];
    uint8_t  ipv6_global[16];
    uint8_t  ipv6_linklocal_ready;
    uint8_t  ipv6_global_ready;
    uint8_t  reserved2[2];
} socket_netinfo_t;

typedef struct socket_ipv4_config {
    uint32_t ipv4_addr;
    uint32_t ipv4_netmask;
    uint32_t ipv4_gateway;
    uint8_t  method;
    uint8_t  reserved[3];
} socket_ipv4_config_t;

enum {
    SOCKET_IPV4_METHOD_NONE   = 0,
    SOCKET_IPV4_METHOD_MANUAL = 1,
    SOCKET_IPV4_METHOD_DHCP   = 2,
};

typedef struct socket_provider {
    int     (*create)(int domain, int type, int protocol, void **out_socket);
    int     (*close)(void *socket);
    ssize_t (*read)(void *socket, void *buffer, size_t size, uint64_t flags);
    ssize_t (*write)(void *socket, const void *buffer, size_t size, uint64_t flags);
    ssize_t (*recvfrom)(void *socket, void *buffer, size_t size, uint64_t flags,
                        struct sockaddr *addr, socklen_t *addrlen);
    ssize_t (*sendto)(void *socket, const void *buffer, size_t size, uint64_t flags,
                      const struct sockaddr *addr, socklen_t addrlen);
    int     (*bind)(void *socket, const struct sockaddr *addr, socklen_t addrlen);
    int     (*connect)(void *socket, const struct sockaddr *addr, socklen_t addrlen);
    int     (*getsockname)(void *socket, struct sockaddr *addr, socklen_t *addrlen);
    int     (*getpeername)(void *socket, struct sockaddr *addr, socklen_t *addrlen);
    int     (*getsockopt)(void *socket, int level, int optname, void *optval, socklen_t *optlen);
    int     (*listen)(void *socket, int backlog);
    int     (*accept)(void *socket, void **out_socket, struct sockaddr *addr, socklen_t *addrlen);
    int     (*shutdown)(void *socket, int how);
    int     (*poll)(void *socket, size_t events);
    int     (*set_flags)(void *socket, uint64_t flags);
    int     (*resolve_ipv4)(const char *hostname, uint32_t *out_addr);
    int     (*resolve_ipv6)(const char *hostname, uint8_t out_addr[16]);
    int     (*set_dns_server)(uint32_t addr);
    int     (*get_dns_server)(uint32_t *out_addr);
    int     (*set_ipv4_config)(const socket_ipv4_config_t *config);
    int     (*get_netinfo)(socket_netinfo_t *out_info);
} socket_provider_t;

int     socket_provider_register(const socket_provider_t *provider);
int     socketfs_setup(void);
int     socketfs_alloc_fd_with_provider(void *impl, uint64_t flags, const socket_provider_t *provider);
int     socket_open(int domain, int type, int protocol);
int     socket_bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int     socket_connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int     socket_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);
int     socket_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);
int     socket_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int     socket_listen(int fd, int backlog);
int     socket_accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int     socket_shutdown(int fd, int how);
ssize_t socket_read(vfs_node_t node, void *buffer, size_t size);
ssize_t socket_write(vfs_node_t node, const void *buffer, size_t size);
ssize_t socket_recvfrom(int fd, void *buffer, size_t size, uint64_t flags, struct sockaddr *addr, socklen_t *addrlen);
ssize_t socket_sendto(int fd, const void *buffer, size_t size, uint64_t flags,
                      const struct sockaddr *addr, socklen_t addrlen);
int     socket_apply_flags(vfs_node_t node, uint64_t flags);
int     socket_resolve_ipv4(const char *hostname, uint32_t *out_addr);
int     socket_resolve_ipv6(const char *hostname, uint8_t out_addr[16]);
int     socket_set_dns_server(uint32_t addr);
int     socket_get_dns_server(uint32_t *out_addr);
int     socket_set_ipv4_config(const socket_ipv4_config_t *config);
int     socket_get_netinfo(socket_netinfo_t *out_info);

#ifdef __cplusplus
}
#endif
