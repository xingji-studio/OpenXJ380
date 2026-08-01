#pragma once

#include "../stdint.h"

typedef uint16_t sa_family_t;
typedef uint16_t in_port_t;
typedef uint32_t socklen_t;

#ifdef __cplusplus
extern "C" {
#endif

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_LOCAL  AF_UNIX
#define AF_INET   2
#define AF_INET6  10

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_TYPE_MASK 0xf
#define SOCK_CLOEXEC 02000000
#define SOCK_NONBLOCK 00004000

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

static inline uint16_t htons(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

static inline uint16_t ntohs(uint16_t value)
{
    return htons(value);
}

static inline uint32_t htonl(uint32_t value)
{
    return ((value & 0x000000ffU) << 24) |
           ((value & 0x0000ff00U) << 8) |
           ((value & 0x00ff0000U) >> 8) |
           ((value & 0xff000000U) >> 24);
}

static inline uint32_t ntohl(uint32_t value)
{
    return htonl(value);
}

int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int sv[2]);
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int shutdown(int sockfd, int how);

#ifdef __cplusplus
}
#endif
