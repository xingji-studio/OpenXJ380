#define ALL_IMPLEMENTATION
#include <net/unixsock.h>
#include <net/socket.h>
#include <errno.h>
#include <krlibc.h>
#include <mm/heap.h>
#include <task/pcb.h>
#include <task/poll.h>
#include <syscall/fd_lifetime.h>

socket_provider_t g_unix_provider;

void init_unix_provider(void)
{
    static bool inited = false;
    if (inited) return;
    inited = true;
    memset(&g_unix_provider, 0, sizeof(g_unix_provider));
    g_unix_provider.create       = unix_create;
    g_unix_provider.close        = unix_close;
    g_unix_provider.read         = unix_read;
    g_unix_provider.write        = unix_write;
    g_unix_provider.bind         = unix_bind;
    g_unix_provider.recvfrom     = unix_recvfrom;
    g_unix_provider.sendto       = unix_sendto;
    g_unix_provider.getsockname  = unix_getsockname;
    g_unix_provider.getpeername  = unix_getpeername;
    g_unix_provider.getsockopt   = unix_getsockopt;
    g_unix_provider.connect      = unix_connect;
    g_unix_provider.listen       = unix_listen;
    g_unix_provider.accept       = unix_accept;
    g_unix_provider.shutdown     = unix_shutdown;
    g_unix_provider.poll         = unix_poll;
    g_unix_provider.set_flags    = unix_set_flags;
}

static lock_queue *g_unix_namespace = NULL;
static spin_t      g_unix_ns_lock   = SPIN_INIT;

static void unix_sock_init(struct unix_sock *sk, int type, int protocol)
{
    sk->state    = UNIX_FREE;
    sk->type     = type;
    sk->protocol = protocol;
    sk->peer     = NULL;
    sk->conn_queue = NULL;
    sk->in_cap = UNIX_RING_SIZE;
    sk->in_buf = (uint8_t *)malloc(UNIX_RING_SIZE);
    sk->in_head = sk->in_tail = 0;
    sk->dg_queue = NULL;
    sk->lock     = SPIN_INIT;
    sk->refcount = 1;
    sk->closed   = false;
    memset(&sk->local_addr, 0, sizeof(sk->local_addr));
    memset(&sk->peer_addr, 0, sizeof(sk->peer_addr));
}

static void unix_sock_retain(struct unix_sock *sk)
{
    __atomic_add_fetch(&sk->refcount, size_t(1), __ATOMIC_RELAXED);
}

static void unix_sock_free(struct unix_sock *sk)
{
    if (sk->in_buf) free(sk->in_buf);
    if (sk->conn_queue)
    {
        // Unaccepted endpoints own their original reference through the queue.
        // Detach/close their peer links outside every endpoint/namespace lock.
        while (auto *pending = static_cast<unix_sock *>(queue_dequeue(sk->conn_queue))) unix_close(pending);
        queue_destroy(sk->conn_queue);
    }
    if (sk->dg_queue)
    {
        while (void *datagram = queue_dequeue(sk->dg_queue)) free(datagram);
        queue_destroy(sk->dg_queue);
    }
    free(sk);
}

static void unix_sock_release(struct unix_sock *sk)
{
    if (__atomic_sub_fetch(&sk->refcount, size_t(1), __ATOMIC_ACQ_REL) == 0) unix_sock_free(sk);
}

// The link owns a reference. Pin it while its endpoint lock still protects the
// pointer, then inspect the peer separately. Never nest the two endpoint locks:
// simultaneous epoll waits on opposite socketpair ends previously deadlocked.
static struct unix_sock *unix_peer_acquire(struct unix_sock *sk)
{
    spin_lock(&sk->lock);
    struct unix_sock *peer = sk->peer;
    if (peer) unix_sock_retain(peer);
    spin_unlock(&sk->lock);
    return peer;
}

static void unix_ns_register(struct unix_sock *sk)
{
    if (sk->local_addr.sun_path[0] == '\0') return;
    spin_lock(&g_unix_ns_lock);
    if (g_unix_namespace == NULL)
        g_unix_namespace = queue_init();
    if (g_unix_namespace != NULL)
        queue_enqueue(g_unix_namespace, sk);
    spin_unlock(&g_unix_ns_lock);
}

static void unix_ns_unregister(struct unix_sock *sk)
{
    if (g_unix_namespace == NULL) return;
    spin_lock(&g_unix_ns_lock);
    lock_node *node = g_unix_namespace->head;
    while (node != NULL) {
        if (node->data == sk) {
            queue_remove_node(g_unix_namespace, node);
            break;
        }
        node = node->next;
    }
    spin_unlock(&g_unix_ns_lock);
}

static struct unix_sock *unix_ns_lookup(const char *path)
{
    if (g_unix_namespace == NULL || path == NULL) return NULL;
    spin_lock(&g_unix_ns_lock);
    lock_node *node = g_unix_namespace->head;
    while (node != NULL) {
        struct unix_sock *sk = (struct unix_sock *)node->data;
        if (sk != NULL && sk->state >= UNIX_BOUND &&
            sk->local_addr.sun_path[0] != '\0' &&
            !strcmp(sk->local_addr.sun_path, path)) {
            unix_sock_retain(sk);
            spin_unlock(&g_unix_ns_lock);
            return sk;
        }
        node = node->next;
    }
    spin_unlock(&g_unix_ns_lock);
    return NULL;
}

static size_t ring_readable(struct unix_sock *sk)
{
    if (sk->in_head >= sk->in_tail) return sk->in_head - sk->in_tail;
    return sk->in_cap - sk->in_tail + sk->in_head;
}

static size_t ring_writable(struct unix_sock *sk)
{
    size_t used = ring_readable(sk);
    size_t free_space = sk->in_cap - used;
    return free_space > 1 ? free_space - 1 : 0;
}

static size_t ring_write(struct unix_sock *sk, const uint8_t *data, size_t len)
{
    size_t avail = ring_writable(sk);
    if (avail == 0 || len == 0) return 0;
    size_t n = len < avail ? len : avail;
    for (size_t i = 0; i < n; i++) {
        sk->in_buf[sk->in_head] = data[i];
        sk->in_head = (sk->in_head + 1) % sk->in_cap;
    }
    return n;
}

static size_t ring_read(struct unix_sock *sk, uint8_t *data, size_t len)
{
    size_t avail = ring_readable(sk);
    if (avail == 0 || len == 0) return 0;
    size_t n = len < avail ? len : avail;
    for (size_t i = 0; i < n; i++) {
        data[i] = sk->in_buf[sk->in_tail];
        sk->in_tail = (sk->in_tail + 1) % sk->in_cap;
    }
    return n;
}

int unix_create(int domain, int type, int protocol, void **out_impl)
{
    (void)domain;
    if (out_impl == NULL) return -EINVAL;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -EPROTONOSUPPORT;

    struct unix_sock *sk = (struct unix_sock *)malloc(sizeof(struct unix_sock));
    if (sk == NULL) return -ENOMEM;
    unix_sock_init(sk, type, protocol);
    if (!sk->in_buf) { free(sk); return -ENOMEM; }
    *out_impl = sk;
    return 0;
}

int unix_bind(void *impl, const struct sockaddr *addr, socklen_t addrlen)
{
    struct unix_sock *sk = (struct unix_sock *)impl;
    if (sk == NULL || addr == NULL) return -EINVAL;
    if (addrlen < (socklen_t)sizeof(sa_family_t)) return -EINVAL;

    const struct sockaddr_un *sunaddr = (const struct sockaddr_un *)addr;
    if (sunaddr->sun_family != AF_UNIX) return -EAFNOSUPPORT;

    spin_lock(&sk->lock);
    if (sk->state != UNIX_FREE) { spin_unlock(&sk->lock); return -EINVAL; }

    size_t max_path = 0;
    if (addrlen > (socklen_t)offsetof(struct sockaddr_un, sun_path)) {
        max_path = (size_t)addrlen - offsetof(struct sockaddr_un, sun_path);
        if (max_path > UNIX_PATH_MAX) max_path = UNIX_PATH_MAX;
    }

    if (max_path > 0 && sunaddr->sun_path[0] != '\0') {
        struct unix_sock *existing = unix_ns_lookup(sunaddr->sun_path);
        if (existing != NULL) {
            spin_unlock(&sk->lock);
            unix_sock_release(existing);
            return -EADDRINUSE;
        }
    }

    memcpy(&sk->local_addr, sunaddr, addrlen < sizeof(sk->local_addr) ? (size_t)addrlen : sizeof(sk->local_addr));
    sk->state = UNIX_BOUND;

    if (max_path > 0 && sunaddr->sun_path[0] != '\0')
        unix_ns_register(sk);

    spin_unlock(&sk->lock);
    return 0;
}

int unix_connect(void *impl, const struct sockaddr *addr, socklen_t addrlen)
{
    struct unix_sock *sk = (struct unix_sock *)impl;
    if (sk == NULL || addr == NULL) return -EINVAL;

    const struct sockaddr_un *sunaddr = (const struct sockaddr_un *)addr;
    if (sunaddr->sun_family != AF_UNIX) return -EAFNOSUPPORT;

    spin_lock(&sk->lock);
    if (sk->state != UNIX_FREE && sk->state != UNIX_BOUND) {
        spin_unlock(&sk->lock);
        return -EINVAL;
    }

    if (sk->local_addr.sun_family != AF_UNIX)
        sk->local_addr.sun_family = AF_UNIX;

    memcpy(&sk->peer_addr, sunaddr, addrlen < sizeof(sk->peer_addr) ? (size_t)addrlen : sizeof(sk->peer_addr));

    if (sk->type == SOCK_DGRAM) {
        sk->state = UNIX_CONNECTED;
        spin_unlock(&sk->lock);
        return 0;
    }

    struct unix_sock *listener = NULL;
    if (sunaddr->sun_path[0] != '\0')
        listener = unix_ns_lookup(sunaddr->sun_path);

    if (listener == NULL) { spin_unlock(&sk->lock); return -ECONNREFUSED; }

    spin_lock(&listener->lock);
    if (listener->state != UNIX_LISTENING || listener->conn_queue == NULL) {
        spin_unlock(&listener->lock);
        spin_unlock(&sk->lock);
        unix_sock_release(listener);
        return -ECONNREFUSED;
    }

    struct unix_sock *server_sk = (struct unix_sock *)malloc(sizeof(struct unix_sock));
    if (server_sk == NULL) {
        spin_unlock(&listener->lock);
        spin_unlock(&sk->lock);
        unix_sock_release(listener);
        return -ENOMEM;
    }
    unix_sock_init(server_sk, SOCK_STREAM, 0);
    server_sk->state = UNIX_CONNECTED;
    server_sk->local_addr = listener->local_addr;
    server_sk->peer_addr  = sk->local_addr;
    server_sk->peer = sk;

    // Both published peer links own independent endpoint references.
    unix_sock_retain(sk);
    unix_sock_retain(server_sk);
    sk->state = UNIX_CONNECTED;
    sk->peer  = server_sk;
    queue_enqueue(listener->conn_queue, server_sk);
    spin_unlock(&listener->lock);
    spin_unlock(&sk->lock);
    unix_sock_release(listener);
    return 0;
}

int unix_listen(void *impl, int backlog)
{
    struct unix_sock *sk = (struct unix_sock *)impl;
    if (sk == NULL) return -EINVAL;

    spin_lock(&sk->lock);
    if (sk->type != SOCK_STREAM) { spin_unlock(&sk->lock); return -EOPNOTSUPP; }
    if (sk->state != UNIX_BOUND) { spin_unlock(&sk->lock); return -EINVAL; }

    sk->state = UNIX_LISTENING;
    if (sk->conn_queue == NULL)
        sk->conn_queue = queue_init();
    (void)backlog;
    spin_unlock(&sk->lock);
    return 0;
}

int unix_accept(void *impl, void **out_new, struct sockaddr *addr, socklen_t *addrlen)
{
    struct unix_sock *sk = (struct unix_sock *)impl;
    if (sk == NULL || out_new == NULL) return -EINVAL;

    spin_lock(&sk->lock);
    if (sk->state != UNIX_LISTENING || sk->conn_queue == NULL) {
        spin_unlock(&sk->lock);
        return -EINVAL;
    }

    while (sk->conn_queue->head == NULL) {
        spin_unlock(&sk->lock);
        scheduler_yield();
        spin_lock(&sk->lock);
        if (sk->state != UNIX_LISTENING) { spin_unlock(&sk->lock); return -EINVAL; }
    }

    struct unix_sock *new_sk = (struct unix_sock *)sk->conn_queue->head->data;
    queue_remove_node(sk->conn_queue, sk->conn_queue->head);

    struct unix_sock *client = new_sk->peer;

    if (addr != NULL && addrlen != NULL) {
        socklen_t copy_len = client != NULL ? sizeof(client->local_addr) : 0;
        if (*addrlen < copy_len) copy_len = *addrlen;
        if (client != NULL) memcpy(addr, &client->local_addr, copy_len);
        *addrlen = client != NULL ? sizeof(client->local_addr) : 0;
    }

    *out_new = new_sk;
    spin_unlock(&sk->lock);
    return 0;
}

int unix_getsockname(void *impl, struct sockaddr *addr, socklen_t *addrlen)
{
    struct unix_sock *sk = (struct unix_sock *)impl;
    if (sk == NULL || addr == NULL || addrlen == NULL) return -EINVAL;
    socklen_t copy = *addrlen < (socklen_t)sizeof(sk->local_addr) ? *addrlen : (socklen_t)sizeof(sk->local_addr);
    memcpy(addr, &sk->local_addr, copy);
    *addrlen = sizeof(sk->local_addr);
    return 0;
}

int unix_getpeername(void *impl, struct sockaddr *addr, socklen_t *addrlen)
{
    struct unix_sock *sk = (struct unix_sock *)impl;
    if (sk == NULL || addr == NULL || addrlen == NULL) return -EINVAL;
    if (sk->peer_addr.sun_family != AF_UNIX) return -ENOTCONN;
    socklen_t copy = *addrlen < (socklen_t)sizeof(sk->peer_addr) ? *addrlen : (socklen_t)sizeof(sk->peer_addr);
    memcpy(addr, &sk->peer_addr, copy);
    *addrlen = sizeof(sk->peer_addr);
    return 0;
}

int unix_getsockopt(void *impl, int level, int optname, void *optval, socklen_t *optlen)
{
    struct unix_sock *sk = (struct unix_sock *)impl;
    if (sk == NULL || optval == NULL || optlen == NULL) return -EINVAL;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_TYPE: {
            int val = sk->type;
            size_t copy_len = *optlen < (socklen_t)sizeof(val) ? (size_t)*optlen : sizeof(val);
            memcpy(optval, &val, copy_len);
            *optlen = (socklen_t)sizeof(val);
            return 0;
        }
        case SO_ERROR: {
            int val = 0;
            size_t copy_len = *optlen < (socklen_t)sizeof(val) ? (size_t)*optlen : sizeof(val);
            memcpy(optval, &val, copy_len);
            *optlen = (socklen_t)sizeof(val);
            return 0;
        }
        default: return -ENOPROTOOPT;
        }
    }
    return -ENOPROTOOPT;
}

int unix_shutdown(void *impl, int how)
{
    struct unix_sock *sk = (struct unix_sock *)impl;
    if (sk == NULL) return -EINVAL;
    (void)how;
    spin_lock(&sk->lock);
    sk->closed = true;
    spin_unlock(&sk->lock);
    return 0;
}

ssize_t unix_read(void *impl, void *buf, size_t size, uint64_t flags)
{
    struct unix_sock *sk = (struct unix_sock *)impl;
    if (sk == NULL || buf == NULL) return -EINVAL;

    if (sk->type == SOCK_DGRAM) {
        spin_lock(&sk->lock);
        if (sk->dg_queue == NULL) sk->dg_queue = queue_init();
        while (sk->dg_queue->head == NULL) {
            if (sk->closed) { spin_unlock(&sk->lock); return 0; }
            if (flags & O_NONBLOCK) { spin_unlock(&sk->lock); return -EAGAIN; }
            spin_unlock(&sk->lock);
            scheduler_yield();
            spin_lock(&sk->lock);
        }
        struct unix_dgram *dg = (struct unix_dgram *)sk->dg_queue->head->data;
        size_t n = size < dg->len ? size : dg->len;
        memcpy(buf, dg->data, n);
        queue_remove_node(sk->dg_queue, sk->dg_queue->head);
        free(dg);
        spin_unlock(&sk->lock);
        return (ssize_t)n;
    }

    spin_lock(&sk->lock);
    while (ring_readable(sk) == 0) {
        if (sk->closed || sk->peer == NULL) {
            spin_unlock(&sk->lock);
            return 0;
        }
        if (flags & O_NONBLOCK) { spin_unlock(&sk->lock); return -EAGAIN; }
        spin_unlock(&sk->lock);
        scheduler_yield();
        spin_lock(&sk->lock);
    }
    size_t n = ring_read(sk, (uint8_t *)buf, size);
    spin_unlock(&sk->lock);
    return (ssize_t)n;
}

ssize_t unix_write(void *impl, const void *buf, size_t size, uint64_t flags)
{
    struct unix_sock *sk = (struct unix_sock *)impl;
    if (sk == NULL || buf == NULL) return -EINVAL;

    if (sk->type == SOCK_DGRAM) {
        if (sk->peer_addr.sun_family != AF_UNIX || sk->peer_addr.sun_path[0] == '\0')
            return -EDESTADDRREQ;
        struct unix_sock *peer = unix_ns_lookup(sk->peer_addr.sun_path);
        if (peer == NULL) return -ECONNREFUSED;
        if (peer->dg_queue == NULL) {
            spin_lock(&peer->lock);
            if (peer->dg_queue == NULL) peer->dg_queue = queue_init();
            spin_unlock(&peer->lock);
        }
        struct unix_dgram *dg = (struct unix_dgram *)malloc(sizeof(struct unix_dgram) + size);
        if (dg == NULL) { unix_sock_release(peer); return -ENOMEM; }
        memcpy(&dg->addr, &sk->local_addr, sizeof(dg->addr));
        dg->len = size;
        memcpy(dg->data, buf, size);
        spin_lock(&peer->lock);
        queue_enqueue(peer->dg_queue, dg);
        spin_unlock(&peer->lock);
        unix_sock_release(peer);
        return (ssize_t)size;
    }

    struct unix_sock *peer = unix_peer_acquire(sk);
    if (peer == NULL) return -EPIPE;

    const uint8_t *src = (const uint8_t *)buf;
    spin_lock(&peer->lock);
    size_t total = 0;
    while (total < size)
    {
        if (peer->closed) break;
        size_t n = ring_write(peer, src + total, size - total);
        if (n == 0)
        {
            if (flags & O_NONBLOCK) break;
            spin_unlock(&peer->lock);
            scheduler_yield();
            spin_lock(&peer->lock);
            continue;
        }
        total += n;
    }
    bool closed = peer->closed;
    spin_unlock(&peer->lock);
    unix_sock_release(peer);
    return total ? (ssize_t)total : !size ? 0 : closed ? -EPIPE : -EAGAIN;
}

ssize_t unix_sendto(void *impl, const void *buf, size_t size, uint64_t flags,
                    const struct sockaddr *addr, socklen_t addrlen)
{
    (void)flags;
    struct unix_sock *sk = (struct unix_sock *)impl;
    if (sk == NULL || buf == NULL) return -EINVAL;

    // Anonymous connected streams (socketpair / Node IPC) have no pathname
    // peer address. Sending without an explicit destination uses their link.
    if (sk->type == SOCK_STREAM) return unix_write(impl, buf, size, flags);

    if (addr != NULL && addrlen >= (socklen_t)sizeof(sa_family_t)) {
        const struct sockaddr_un *sunaddr = (const struct sockaddr_un *)addr;
        if (sunaddr->sun_family != AF_UNIX) return -EAFNOSUPPORT;
        if (sunaddr->sun_path[0] == '\0') return -EDESTADDRREQ;

        struct unix_sock *peer = unix_ns_lookup(sunaddr->sun_path);
        if (peer == NULL) return -ECONNREFUSED;

        if (peer->dg_queue == NULL) {
            spin_lock(&peer->lock);
            if (peer->dg_queue == NULL) peer->dg_queue = queue_init();
            spin_unlock(&peer->lock);
        }
        struct unix_dgram *dg = (struct unix_dgram *)malloc(sizeof(struct unix_dgram) + size);
        if (dg == NULL) { unix_sock_release(peer); return -ENOMEM; }
        if (sk->local_addr.sun_family == AF_UNIX)
            memcpy(&dg->addr, &sk->local_addr, sizeof(dg->addr));
        else
            memset(&dg->addr, 0, sizeof(dg->addr));
        dg->addr.sun_family = AF_UNIX;
        dg->len = size;
        memcpy(dg->data, buf, size);
        spin_lock(&peer->lock);
        queue_enqueue(peer->dg_queue, dg);
        spin_unlock(&peer->lock);
        unix_sock_release(peer);
        return (ssize_t)size;
    }

    if (sk->peer_addr.sun_family != AF_UNIX) return -EDESTADDRREQ;
    return unix_write(impl, buf, size, flags);
}

ssize_t unix_recvfrom(void *impl, void *buf, size_t size, uint64_t flags,
                      struct sockaddr *addr, socklen_t *addrlen)
{
    (void)flags;
    struct unix_sock *sk = (struct unix_sock *)impl;
    if (sk == NULL || buf == NULL) return -EINVAL;

    // recvmsg uses the provider's recvfrom entry even for connected streams.
    // Receive stream bytes from the ring; the datagram queue cannot satisfy it.
    if (sk->type == SOCK_STREAM)
    {
        ssize_t result = unix_read(impl, buf, size, flags);
        if (result >= 0 && addrlen) *addrlen = 0;
        return result;
    }

    spin_lock(&sk->lock);
    if (sk->dg_queue == NULL) sk->dg_queue = queue_init();
    while (sk->dg_queue->head == NULL) {
        if (sk->closed) { spin_unlock(&sk->lock); return 0; }
        spin_unlock(&sk->lock);
        scheduler_yield();
        spin_lock(&sk->lock);
    }
    struct unix_dgram *dg = (struct unix_dgram *)sk->dg_queue->head->data;
    size_t n = size < dg->len ? size : dg->len;
    memcpy(buf, dg->data, n);

    if (addr != NULL && addrlen != NULL) {
        socklen_t clen = *addrlen < (socklen_t)sizeof(dg->addr) ? *addrlen : (socklen_t)sizeof(dg->addr);
        memcpy(addr, &dg->addr, clen);
        *addrlen = sizeof(dg->addr);
    }

    queue_remove_node(sk->dg_queue, sk->dg_queue->head);
    free(dg);
    spin_unlock(&sk->lock);
    return (ssize_t)n;
}

int unix_poll(void *impl, size_t events)
{
    auto *sk = static_cast<unix_sock *>(impl);
    if (!sk) return -EINVAL;
    int revents = 0;
    spin_lock(&sk->lock);
    if (sk->type == SOCK_DGRAM)
    {
        if (sk->dg_queue && sk->dg_queue->head) revents |= POLLIN;
    }
    else if (ring_readable(sk)) revents |= POLLIN;
    if (sk->closed) revents |= POLLHUP;
    if (sk->state == UNIX_LISTENING && sk->conn_queue && sk->conn_queue->head) revents |= POLLIN;
    unix_sock *peer = sk->peer;
    if (peer) unix_sock_retain(peer);
    else if (sk->type == SOCK_DGRAM) revents |= POLLOUT;
    spin_unlock(&sk->lock);

    if (peer)
    {
        spin_lock(&peer->lock);
        if (!peer->closed && ring_writable(peer)) revents |= POLLOUT;
        spin_unlock(&peer->lock);
        unix_sock_release(peer);
    }
    // EOF/hangup must reach epoll even when its caller requested only input.
    return revents & (int(events) | POLLHUP | POLLERR);
}

int unix_close(void *impl)
{
    auto *sk = static_cast<unix_sock *>(impl);
    if (!sk) return 0;
    spin_lock(&sk->lock);
    sk->closed = true;
    unix_sock *peer = sk->peer;
    sk->peer = NULL;
    bool bound = sk->state >= UNIX_BOUND;
    sk->state = UNIX_FREE;
    spin_unlock(&sk->lock);

    if (peer)
    {
        // Transfer the link's reference to this local until detachment ends.
        // Concurrent opposite-end close may already own the other backlink.
        spin_lock(&peer->lock);
        bool backlink = peer->peer == sk;
        if (backlink)
        {
            peer->closed = true;
            peer->peer = NULL;
        }
        spin_unlock(&peer->lock);
        if (backlink) unix_sock_release(sk);
        unix_sock_release(peer);
    }
    if (bound) unix_ns_unregister(sk);
    unix_sock_release(sk); // The provider's original open-endpoint reference.
    return 0;
}

int unix_set_flags(void *impl, uint64_t flags)
{
    (void)impl; (void)flags;
    return 0;
}

int unix_socketpair(int type, int protocol, int sv[2])
{
    if (sv == NULL) return -EINVAL;
    if (type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
    uint64_t flags = O_RDWR | (type & (SOCK_NONBLOCK | SOCK_CLOEXEC));
    type &= SOCK_TYPE_MASK;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -EPROTONOSUPPORT;

    init_unix_provider();

    void *s1 = NULL, *s2 = NULL;
    int ret = unix_create(AF_UNIX, type, protocol, &s1);
    if (ret < 0) return ret;
    ret = unix_create(AF_UNIX, type, protocol, &s2);
    if (ret < 0) { unix_close(s1); return ret; }

    struct unix_sock *sk1 = (struct unix_sock *)s1;
    struct unix_sock *sk2 = (struct unix_sock *)s2;

    sk1->state = UNIX_CONNECTED;
    sk2->state = UNIX_CONNECTED;
    sk1->local_addr.sun_family = AF_UNIX;
    sk2->local_addr.sun_family = AF_UNIX;
    sk1->peer = sk2;
    sk2->peer = sk1;
    unix_sock_retain(sk2);
    unix_sock_retain(sk1);

    // The allocator consumes each supplied implementation on every path.
    sv[0] = socketfs_alloc_fd_with_provider(s1, flags, &g_unix_provider);
    if (sv[0] < 0) { unix_close(s2); return sv[0]; }
    sv[1] = socketfs_alloc_fd_with_provider(s2, flags, &g_unix_provider);
    if (sv[1] < 0)
    {
        fd_file_handle *handle = fd_detach_descriptor(get_current_task()->parent_group, size_t(sv[0]));
        if (handle) fd_retire_handle(handle);
        return sv[1];
    }
    return 0;
}
