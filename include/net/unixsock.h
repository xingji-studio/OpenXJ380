#pragma once

#include <krlibc.h>
#include <lock_queue.h>
#include <net/socket.h>
#include <cpu/lock.h>

#define UNIX_PATH_MAX   108
#define UNIX_RING_SIZE  65536

struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[UNIX_PATH_MAX];
};

enum unix_sock_state {
    UNIX_FREE = 0,
    UNIX_BOUND,
    UNIX_LISTENING,
    UNIX_CONNECTING,
    UNIX_CONNECTED,
};

struct unix_sock {
    enum unix_sock_state state;
    int                  type;
    int                  protocol;
    struct sockaddr_un   local_addr;
    struct sockaddr_un   peer_addr;

    struct unix_sock *peer;
    lock_queue *conn_queue;

    uint8_t *in_buf;
    size_t   in_head, in_tail, in_cap;

    lock_queue *dg_queue;

    spin_t  lock;
    size_t  refcount;
    bool    closed;
};

struct unix_dgram {
    struct sockaddr_un addr;
    size_t             len;
    uint8_t            data[];
};

int  unix_create(int domain, int type, int protocol, void **out_impl);
int  unix_bind(void *impl, const struct sockaddr *addr, socklen_t addrlen);
int  unix_connect(void *impl, const struct sockaddr *addr, socklen_t addrlen);
int  unix_listen(void *impl, int backlog);
int  unix_accept(void *impl, void **out_new, struct sockaddr *addr, socklen_t *addrlen);
int  unix_getsockname(void *impl, struct sockaddr *addr, socklen_t *addrlen);
int  unix_getpeername(void *impl, struct sockaddr *addr, socklen_t *addrlen);
int  unix_getsockopt(void *impl, int level, int optname, void *optval, socklen_t *optlen);
int  unix_shutdown(void *impl, int how);
ssize_t unix_read(void *impl, void *buf, size_t size, uint64_t flags);
ssize_t unix_write(void *impl, const void *buf, size_t size, uint64_t flags);
ssize_t unix_sendto(void *impl, const void *buf, size_t size, uint64_t flags,
                    const struct sockaddr *addr, socklen_t addrlen);
ssize_t unix_recvfrom(void *impl, void *buf, size_t size, uint64_t flags,
                      struct sockaddr *addr, socklen_t *addrlen);
int  unix_poll(void *impl, size_t events);
int  unix_close(void *impl);
int  unix_set_flags(void *impl, uint64_t flags);
int  unix_socketpair(int type, int protocol, int sv[2]);

extern socket_provider_t g_unix_provider;
void init_unix_provider(void);
