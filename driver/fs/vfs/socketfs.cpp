#define ALL_IMPLEMENTATION

#include <dlinker.h>
#include <cpu/lock.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <net/socket.h>
#include <syscall/syscall.h>
#include <task/pcb.h>

typedef struct socketfs_handle {
    socket_provider_t provider;
    void             *socket;
} socketfs_handle_t;

static int        socketfs_id      = 0;
static int        socketfs_next_id = 0;
static vfs_node_t socketfs_root    = NULL;

static spin_t           g_socket_provider_lock  = SPIN_INIT;
static socket_provider_t g_socket_provider_data = {};
static bool              g_socket_provider_ready = false;

static inline bool socketfs_provider_valid(const socket_provider_t *provider)
{
    return provider != NULL && provider->create != NULL && provider->close != NULL &&
           provider->read != NULL && provider->write != NULL && provider->bind != NULL &&
           provider->recvfrom != NULL && provider->sendto != NULL &&
           provider->getsockname != NULL && provider->getpeername != NULL &&
           provider->getsockopt != NULL &&
           provider->connect != NULL && provider->listen != NULL && provider->accept != NULL &&
           provider->shutdown != NULL && provider->poll != NULL && provider->set_flags != NULL &&
           provider->resolve_ipv4 != NULL && provider->resolve_ipv6 != NULL &&
           provider->set_dns_server != NULL &&
           provider->get_dns_server != NULL && provider->set_ipv4_config != NULL &&
           provider->get_netinfo != NULL;
}

static int socketfs_snapshot_provider(socket_provider_t *out_provider)
{
    if (out_provider == NULL) {
        return -EINVAL;
    }

    spin_lock(&g_socket_provider_lock);
    if (!g_socket_provider_ready || !socketfs_provider_valid(&g_socket_provider_data)) {
        memset(out_provider, 0, sizeof(*out_provider));
        spin_unlock(&g_socket_provider_lock);
        return -ENOSYS;
    }
    *out_provider = g_socket_provider_data;
    spin_unlock(&g_socket_provider_lock);
    return 0;
}

static inline socketfs_handle_t *socketfs_get_handle(vfs_node_t node)
{
    if (node == NULL || !(node->type & file_socket) || node->handle == NULL) {
        return NULL;
    }
    return (socketfs_handle_t *)node->handle;
}

static inline fd_file_handle *socketfs_get_fd_handle(int fd)
{
    tcb_t current = get_current_task();
    if (current == NULL || current->parent_group == NULL || current->parent_group->file_open == NULL) {
        return NULL;
    }
    if (fd < 0) {
        return NULL;
    }
    return (fd_file_handle *)queue_get(current->parent_group->file_open, fd);
}

static inline bool socketfs_is_socket_fd(int fd, fd_file_handle **out_handle)
{
    fd_file_handle *handle = socketfs_get_fd_handle(fd);
    if (out_handle != NULL) {
        *out_handle = handle;
    }
    return handle != NULL && handle->node != NULL && (handle->node->type & file_socket);
}

static inline void socketfs_sync_flags(socketfs_handle_t *handle, uint64_t flags)
{
    if (handle != NULL && handle->provider.set_flags != NULL) {
        handle->provider.set_flags(handle->socket, flags);
    }
}

static inline int socketfs_errno(ssize_t ret)
{
    return ret >= 0 ? 0 : (int)ret;
}

static errno_t socketfs_mount(const char *handle, vfs_node_t node)
{
    if ((uint64_t)handle != NETFS_REGISTER_ID) {
        return VFS_STATUS_FAILED;
    }
    node->fsid   = socketfs_id;
    node->type   = file_dir;
    node->handle = NULL;
    socketfs_root = node;
    return VFS_STATUS_SUCCESS;
}

static void socketfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    if (node == socketfs_root) {
        node->type = file_dir;
        return;
    }
    node->type = file_socket;
    node->size = (uint64_t)-1;
}

static errno_t socketfs_stat(void *file, vfs_node_t node)
{
    (void)file;
    node->type = file_socket;
    node->size = (uint64_t)-1;
    node->fsid = socketfs_id;
    return EOK;
}

static size_t socketfs_read_cb(void *file, void *addr, size_t offset, size_t size)
{
    (void)offset;
    socketfs_handle_t *handle = (socketfs_handle_t *)file;
    if (handle == NULL || handle->provider.read == NULL) {
        return VFS_STATUS_FAILED;
    }
    ssize_t ret = handle->provider.read(handle->socket, addr, size, 0);
    return ret < 0 ? VFS_STATUS_FAILED : (size_t)ret;
}

static size_t socketfs_write_cb(void *file, const void *addr, size_t offset, size_t size)
{
    (void)offset;
    socketfs_handle_t *handle = (socketfs_handle_t *)file;
    if (handle == NULL || handle->provider.write == NULL) {
        return VFS_STATUS_FAILED;
    }
    ssize_t ret = handle->provider.write(handle->socket, addr, size, 0);
    return ret < 0 ? VFS_STATUS_FAILED : (size_t)ret;
}

static errno_t socketfs_ioctl(void *file, size_t req, void *arg)
{
    (void)file;
    (void)req;
    (void)arg;
    return -ENOTTY;
}

static errno_t socketfs_poll_cb(void *file, size_t events)
{
    socketfs_handle_t *handle = (socketfs_handle_t *)file;
    if (handle == NULL || handle->provider.poll == NULL) {
        return -EINVAL;
    }
    return handle->provider.poll(handle->socket, events);
}

static errno_t socketfs_del(void *parent, vfs_node_t node)
{
    (void)parent;
    socketfs_handle_t *handle = socketfs_get_handle(node);
    if (handle != NULL) {
        if (socketfs_provider_valid(&handle->provider)) {
            handle->provider.close(handle->socket);
        }
        free(handle);
        node->handle = NULL;
    }
    return EOK;
}

static int socketfs_dummy()
{
    return -ENOSYS;
}

static struct vfs_callback socketfs_callbacks = {
    .mount    = socketfs_mount,
    .unmount  = (vfs_unmount_t)empty,
    .open     = socketfs_open,
    .close    = (vfs_close_t)empty,
    .read     = socketfs_read_cb,
    .write    = socketfs_write_cb,
    .readlink = (vfs_readlink_t)socketfs_dummy,
    .mkdir    = (vfs_mk_t)empty,
    .mkfile   = (vfs_mk_t)empty,
    .link     = (vfs_mk_t)socketfs_dummy,
    .symlink  = (vfs_mk_t)socketfs_dummy,
    .stat     = socketfs_stat,
    .ioctl    = socketfs_ioctl,
    .dup      = (vfs_dup_t)empty,
    .poll     = socketfs_poll_cb,
    .map      = (vfs_mapfile_t)empty,
    .resize   = (vfs_resize_t)socketfs_dummy,
    .del      = socketfs_del,
    .rename   = (vfs_rename_t)empty,
};

int socketfs_alloc_fd_with_provider(void *impl, uint64_t flags, const socket_provider_t *provider)
{
    if (socketfs_root == NULL || impl == NULL || provider == NULL) {
        return -ENOSYS;
    }

    char name[32];
    snprintf(name, sizeof(name), "sock%d", socketfs_next_id++);

    vfs_node_t node = vfs_node_alloc(socketfs_root, name);
    if (node == NULL) {
        return -ENOMEM;
    }

    socketfs_handle_t *socket_handle = (socketfs_handle_t *)calloc(1, sizeof(socketfs_handle_t));
    if (socket_handle == NULL) {
        socketfs_root->child = list_delete(socketfs_root->child, node);
        free(node->name);
        free(node);
        return -ENOMEM;
    }

    socket_handle->provider = *provider;
    socket_handle->socket   = impl;

    node->type   = file_socket | file_delete;
    node->fsid   = socketfs_id;
    node->handle = socket_handle;
    node->size   = (uint64_t)-1;
    node->flags  = flags;
    node->mode   = 0777;

    fd_file_handle *fd_handle = (fd_file_handle *)calloc(1, sizeof(fd_file_handle));
    if (fd_handle == NULL) {
        socketfs_del(NULL, node);
        socketfs_root->child = list_delete(socketfs_root->child, node);
        free(node->name);
        free(node);
        return -ENOMEM;
    }

    fd_handle->node   = node;
    fd_handle->offset = 0;
    fd_handle->flags  = flags;
    fd_handle->fd     = queue_enqueue_lowest(get_current_task()->parent_group->file_open, fd_handle);
    if ((int)fd_handle->fd < 0) {
        free(fd_handle);
        socketfs_del(NULL, node);
        socketfs_root->child = list_delete(socketfs_root->child, node);
        free(node->name);
        free(node);
        return -EMFILE;
    }

    return (int)fd_handle->fd;
}

static int socketfs_alloc_fd(void *impl, uint64_t flags)
{
    socket_provider_t provider = {};
    int ret = socketfs_snapshot_provider(&provider);
    if (ret < 0) return ret;
    return socketfs_alloc_fd_with_provider(impl, flags, &provider);
}

#include <net/unixsock.h>

extern socket_provider_t g_unix_provider;
extern void init_unix_provider(void);

int socket_provider_register(const socket_provider_t *provider)
{
    if (!socketfs_provider_valid(provider)) {
        return -EINVAL;
    }

    spin_lock(&g_socket_provider_lock);
    g_socket_provider_data  = *provider;
    g_socket_provider_ready = true;
    spin_unlock(&g_socket_provider_lock);
    return 0;
}
EXPORT_SYMBOL(socket_provider_register);

int socketfs_setup(void)
{
    socketfs_id = vfs_regist("socketfs", &socketfs_callbacks, NETFS_REGISTER_ID, 0x534f434b);
    vfs_mkdir("/sock");
    vfs_node_t node = vfs_open("/sock");
    if (node == NULL) {
        return -ENOENT;
    }
    if (vfs_mount((char *)NETFS_REGISTER_ID, node) == VFS_STATUS_FAILED) {
        return -EIO;
    }
    return 0;
}

int socket_open(int domain, int type, int protocol)
{
    if (domain == AF_UNIX || domain == AF_LOCAL) {
        init_unix_provider();
        void *impl = NULL;
        int ret = unix_create(domain, type, protocol, &impl);
        if (ret < 0) return ret;
        return socketfs_alloc_fd_with_provider(impl, 0, &g_unix_provider);
    }

    if (domain != AF_INET) {
        return -EAFNOSUPPORT;
    }

    socket_provider_t provider = {};
    int               ret      = socketfs_snapshot_provider(&provider);
    if (ret < 0) {
        return ret;
    }

    printk("socketfs: open domain=%d type=%d protocol=%d create=%p\n",
           domain, type, protocol, provider.create);

    void *impl = NULL;
    ret = provider.create(domain, type, protocol, &impl);
    if (ret < 0) {
        printk("socketfs: provider create failed ret=%d\n", ret);
        return ret;
    }

    printk("socketfs: provider create ok impl=%p\n", impl);
    return socketfs_alloc_fd(impl, 0);
}

int socket_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    fd_file_handle *fd_handle = NULL;
    if (!socketfs_is_socket_fd(fd, &fd_handle) || addr == NULL) {
        return -EBADF;
    }

    socketfs_handle_t *handle = socketfs_get_handle(fd_handle->node);
    if (handle == NULL || handle->provider.bind == NULL) {
        return -EBADF;
    }

    return handle->provider.bind(handle->socket, addr, addrlen);
}

int socket_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    fd_file_handle *fd_handle = NULL;
    if (!socketfs_is_socket_fd(fd, &fd_handle) || addr == NULL) {
        return -EBADF;
    }

    socketfs_handle_t *handle = socketfs_get_handle(fd_handle->node);
    if (handle == NULL || handle->provider.connect == NULL) {
        return -EBADF;
    }

    return handle->provider.connect(handle->socket, addr, addrlen);
}

int socket_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    fd_file_handle *fd_handle = NULL;
    if (!socketfs_is_socket_fd(fd, &fd_handle) || addrlen == NULL) {
        return -EBADF;
    }

    socketfs_handle_t *handle = socketfs_get_handle(fd_handle->node);
    if (handle == NULL || handle->provider.getsockname == NULL) {
        return -EBADF;
    }

    return handle->provider.getsockname(handle->socket, addr, addrlen);
}

int socket_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    fd_file_handle *fd_handle = NULL;
    if (!socketfs_is_socket_fd(fd, &fd_handle) || addrlen == NULL) {
        return -EBADF;
    }

    socketfs_handle_t *handle = socketfs_get_handle(fd_handle->node);
    if (handle == NULL || handle->provider.getpeername == NULL) {
        return -EBADF;
    }

    return handle->provider.getpeername(handle->socket, addr, addrlen);
}

int socket_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{
    fd_file_handle *fd_handle = NULL;
    if (!socketfs_is_socket_fd(fd, &fd_handle) || optval == NULL || optlen == NULL) {
        return -EBADF;
    }

    socketfs_handle_t *handle = socketfs_get_handle(fd_handle->node);
    if (handle == NULL || handle->provider.getsockopt == NULL) {
        return -EBADF;
    }

    return handle->provider.getsockopt(handle->socket, level, optname, optval, optlen);
}

int socket_listen(int fd, int backlog)
{
    fd_file_handle *fd_handle = NULL;
    if (!socketfs_is_socket_fd(fd, &fd_handle)) {
        return -EBADF;
    }

    socketfs_handle_t *handle = socketfs_get_handle(fd_handle->node);
    if (handle == NULL || handle->provider.listen == NULL) {
        return -EBADF;
    }

    return handle->provider.listen(handle->socket, backlog);
}

int socket_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    fd_file_handle *fd_handle = NULL;
    if (!socketfs_is_socket_fd(fd, &fd_handle)) {
        return -EBADF;
    }

    socketfs_handle_t *handle = socketfs_get_handle(fd_handle->node);
    if (handle == NULL || handle->provider.accept == NULL) {
        return -EBADF;
    }

    void *new_socket = NULL;
    int   ret        = handle->provider.accept(handle->socket, &new_socket, addr, addrlen);
    if (ret < 0) {
        return ret;
    }

    int new_fd = socketfs_alloc_fd_with_provider(new_socket, fd_handle->node->flags, &handle->provider);
    if (new_fd < 0)
    {
        handle->provider.close(new_socket);
        return new_fd;
    }
    return new_fd;
}

int socket_shutdown(int fd, int how)
{
    fd_file_handle *fd_handle = NULL;
    if (!socketfs_is_socket_fd(fd, &fd_handle)) {
        return -EBADF;
    }

    socketfs_handle_t *handle = socketfs_get_handle(fd_handle->node);
    if (handle == NULL || handle->provider.shutdown == NULL) {
        return -EBADF;
    }

    return handle->provider.shutdown(handle->socket, how);
}

ssize_t socket_read(vfs_node_t node, void *buffer, size_t size)
{
    socketfs_handle_t *handle = socketfs_get_handle(node);
    if (handle == NULL || buffer == NULL || handle->provider.read == NULL) {
        return -EBADF;
    }
    socketfs_sync_flags(handle, node->flags);
    return handle->provider.read(handle->socket, buffer, size, node->flags);
}

ssize_t socket_write(vfs_node_t node, const void *buffer, size_t size)
{
    socketfs_handle_t *handle = socketfs_get_handle(node);
    if (handle == NULL || buffer == NULL || handle->provider.write == NULL) {
        return -EBADF;
    }
    socketfs_sync_flags(handle, node->flags);
    return handle->provider.write(handle->socket, buffer, size, node->flags);
}

ssize_t socket_recvfrom(int fd, void *buffer, size_t size, uint64_t flags, struct sockaddr *addr, socklen_t *addrlen)
{
    fd_file_handle *fd_handle = NULL;
    if (!socketfs_is_socket_fd(fd, &fd_handle)) {
        return -EBADF;
    }

    socketfs_handle_t *handle = socketfs_get_handle(fd_handle->node);
    if (handle == NULL || buffer == NULL || handle->provider.recvfrom == NULL) {
        return -EBADF;
    }
    socketfs_sync_flags(handle, fd_handle->node->flags);
    return handle->provider.recvfrom(handle->socket, buffer, size, fd_handle->node->flags | flags, addr, addrlen);
}

ssize_t socket_sendto(int fd, const void *buffer, size_t size, uint64_t flags,
                      const struct sockaddr *addr, socklen_t addrlen)
{
    fd_file_handle *fd_handle = NULL;
    if (!socketfs_is_socket_fd(fd, &fd_handle)) {
        return -EBADF;
    }

    socketfs_handle_t *handle = socketfs_get_handle(fd_handle->node);
    if (handle == NULL || buffer == NULL || handle->provider.sendto == NULL) {
        return -EBADF;
    }
    socketfs_sync_flags(handle, fd_handle->node->flags);
    return handle->provider.sendto(handle->socket, buffer, size, fd_handle->node->flags | flags, addr, addrlen);
}

int socket_apply_flags(vfs_node_t node, uint64_t flags)
{
    socketfs_handle_t *handle = socketfs_get_handle(node);
    if (handle == NULL || handle->provider.set_flags == NULL) {
        return -EBADF;
    }
    node->flags = flags;
    return handle->provider.set_flags(handle->socket, flags);
}

int socket_resolve_ipv4(const char *hostname, uint32_t *out_addr)
{
    socket_provider_t provider = {};
    int               ret      = socketfs_snapshot_provider(&provider);
    if (ret < 0) {
        return ret;
    }
    if (hostname == NULL || out_addr == NULL) {
        return -EINVAL;
    }
    return provider.resolve_ipv4(hostname, out_addr);
}

int socket_resolve_ipv6(const char *hostname, uint8_t out_addr[16])
{
    socket_provider_t provider = {};
    int               ret      = socketfs_snapshot_provider(&provider);
    if (ret < 0) {
        return ret;
    }
    if (hostname == NULL || out_addr == NULL) {
        return -EINVAL;
    }
    return provider.resolve_ipv6(hostname, out_addr);
}

int socket_set_dns_server(uint32_t addr)
{
    socket_provider_t provider = {};
    int               ret      = socketfs_snapshot_provider(&provider);
    if (ret < 0) {
        return ret;
    }
    return provider.set_dns_server(addr);
}

int socket_get_dns_server(uint32_t *out_addr)
{
    socket_provider_t provider = {};
    int               ret      = socketfs_snapshot_provider(&provider);
    if (ret < 0) {
        return ret;
    }
    if (out_addr == NULL) {
        return -EINVAL;
    }
    return provider.get_dns_server(out_addr);
}

int socket_set_ipv4_config(const socket_ipv4_config_t *config)
{
    socket_provider_t provider = {};
    int               ret      = socketfs_snapshot_provider(&provider);
    if (ret < 0) {
        return ret;
    }
    if (config == NULL) {
        return -EINVAL;
    }
    return provider.set_ipv4_config(config);
}

int socket_get_netinfo(socket_netinfo_t *out_info)
{
    socket_provider_t provider = {};
    int               ret      = socketfs_snapshot_provider(&provider);
    if (ret < 0) {
        return ret;
    }
    if (out_info == NULL) {
        return -EINVAL;
    }
    return provider.get_netinfo(out_info);
}
