#define ALL_IMPLEMENTATION

#include <cpu/lock.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <net/socket.h>

enum {
    DNSFS_KIND_ROOT = 0,
    DNSFS_KIND_RESOLVE = 1,
    DNSFS_KIND_SERVER = 2,
};

typedef struct dnsfs_handle {
    int    kind;
    spin_t lock;
    char   buffer[256];
    size_t length;
} dnsfs_handle_t;

static int          dnsfs_id   = 0;
static vfs_node_t   dnsfs_root = NULL;
static dnsfs_handle_t dnsfs_root_handle    = {DNSFS_KIND_ROOT, SPIN_INIT, {0}, 0};
static dnsfs_handle_t dnsfs_resolve_handle = {DNSFS_KIND_RESOLVE, SPIN_INIT, {0}, 0};
static dnsfs_handle_t dnsfs_server_handle  = {DNSFS_KIND_SERVER, SPIN_INIT, {0}, 0};

static bool dnsfs_parse_ipv4_text(const char *text, uint32_t *out_addr)
{
    uint32_t parts[4] = {0, 0, 0, 0};
    int      part     = 0;
    uint32_t value    = 0;

    if (text == NULL || out_addr == NULL) {
        return false;
    }

    for (const char *p = text;; ++p) {
        char ch = *p;
        if (ch >= '0' && ch <= '9') {
            value = value * 10 + (uint32_t)(ch - '0');
            if (value > 255) {
                return false;
            }
        } else if (ch == '.' || ch == '\0') {
            if (part >= 4) {
                return false;
            }
            parts[part++] = value;
            value         = 0;
            if (ch == '\0') {
                break;
            }
        } else {
            return false;
        }
    }

    if (part != 4) {
        return false;
    }

    *out_addr = ((parts[0] & 0xffU) << 24) |
                ((parts[1] & 0xffU) << 16) |
                ((parts[2] & 0xffU) << 8) |
                (parts[3] & 0xffU);
    *out_addr = ((*out_addr & 0x000000ffU) << 24) |
                ((*out_addr & 0x0000ff00U) << 8) |
                ((*out_addr & 0x00ff0000U) >> 8) |
                ((*out_addr & 0xff000000U) >> 24);
    return true;
}

static void dnsfs_format_ipv4_text(uint32_t addr, char *buffer, size_t size)
{
    uint32_t host = ((addr & 0x000000ffU) << 24) |
                    ((addr & 0x0000ff00U) << 8) |
                    ((addr & 0x00ff0000U) >> 8) |
                    ((addr & 0xff000000U) >> 24);
    snprintf(buffer, size, "%u.%u.%u.%u",
             (unsigned int)((host >> 24) & 0xffU),
             (unsigned int)((host >> 16) & 0xffU),
             (unsigned int)((host >> 8) & 0xffU),
             (unsigned int)(host & 0xffU));
}

static void dnsfs_format_ipv6_text(const uint8_t addr[16], char *buffer, size_t size)
{
    if (buffer == NULL || size == 0 || addr == NULL) {
        return;
    }

    snprintf(buffer, size, "%x:%x:%x:%x:%x:%x:%x:%x",
             (unsigned int)(((uint16_t)addr[0] << 8) | addr[1]),
             (unsigned int)(((uint16_t)addr[2] << 8) | addr[3]),
             (unsigned int)(((uint16_t)addr[4] << 8) | addr[5]),
             (unsigned int)(((uint16_t)addr[6] << 8) | addr[7]),
             (unsigned int)(((uint16_t)addr[8] << 8) | addr[9]),
             (unsigned int)(((uint16_t)addr[10] << 8) | addr[11]),
             (unsigned int)(((uint16_t)addr[12] << 8) | addr[13]),
             (unsigned int)(((uint16_t)addr[14] << 8) | addr[15]));
}

static size_t dnsfs_copy_text_out(const char *text, size_t text_len, void *addr, size_t offset, size_t size)
{
    if (offset >= text_len) {
        return 0;
    }
    size_t remain = text_len - offset;
    if (size > remain) {
        size = remain;
    }
    memcpy(addr, text + offset, size);
    return size;
}

static void dnsfs_trim_line(char *buffer)
{
    size_t len = strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\r' || buffer[len - 1] == '\n' || buffer[len - 1] == ' ' || buffer[len - 1] == '\t')) {
        buffer[--len] = '\0';
    }
    char *start = buffer;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (start != buffer) {
        memmove(buffer, start, strlen(start) + 1);
    }
}

static const char *dnsfs_extract_server_ip(char *buffer)
{
    dnsfs_trim_line(buffer);
    if (strncmp(buffer, "nameserver", 10) == 0 && (buffer[10] == ' ' || buffer[10] == '\t')) {
        char *cursor = buffer + 10;
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        return cursor;
    }
    return buffer;
}

static int dnsfs_parse_resolve_request(char *buffer, int *out_family, char **out_host)
{
    if (buffer == NULL || out_family == NULL || out_host == NULL) {
        return -EINVAL;
    }

    dnsfs_trim_line(buffer);
    if (buffer[0] == '\0') {
        return -EINVAL;
    }

    *out_family = AF_INET;
    *out_host   = buffer;

    char *space = strchr(buffer, ' ');
    if (space == NULL) {
        return 0;
    }

    *space = '\0';
    char *value = space + 1;
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    if (*value == '\0') {
        return -EINVAL;
    }

    if (strcmp(buffer, "ipv4") == 0 || strcmp(buffer, "a") == 0) {
        *out_family = AF_INET;
        *out_host   = value;
        return 0;
    }
    if (strcmp(buffer, "ipv6") == 0 || strcmp(buffer, "aaaa") == 0) {
        *out_family = AF_INET6;
        *out_host   = value;
        return 0;
    }
    if (strcmp(buffer, "auto") == 0 || strcmp(buffer, "unspec") == 0) {
        *out_family = AF_UNSPEC;
        *out_host   = value;
        return 0;
    }

    *space = ' ';
    *out_family = AF_INET;
    *out_host   = buffer;
    return 0;
}

static errno_t dnsfs_mount(const char *handle, vfs_node_t node)
{
    if ((uint64_t)handle != DNS_REGISTER_ID) {
        return VFS_STATUS_FAILED;
    }

    node->fsid   = dnsfs_id;
    node->type   = file_dir;
    node->handle = &dnsfs_root_handle;
    dnsfs_root   = node;

    vfs_node_t resolve_node = vfs_child_append(node, "resolve", &dnsfs_resolve_handle);
    if (resolve_node != NULL) {
        resolve_node->type = file_stream;
        resolve_node->size = (uint64_t)-1;
    }

    vfs_node_t server_node = vfs_child_append(node, "server", &dnsfs_server_handle);
    if (server_node != NULL) {
        server_node->type = file_stream;
        server_node->size = (uint64_t)-1;
    }

    return VFS_STATUS_SUCCESS;
}

static void dnsfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    if (node == dnsfs_root) {
        node->handle = &dnsfs_root_handle;
        node->type = file_dir;
        node->size = 0;
    } else if (name != NULL && strcmp(name, "resolve") == 0) {
        node->handle = &dnsfs_resolve_handle;
        node->type = file_stream;
        node->size = (uint64_t)-1;
    } else if (name != NULL && strcmp(name, "server") == 0) {
        node->handle = &dnsfs_server_handle;
        node->type = file_stream;
        node->size = (uint64_t)-1;
    } else {
        node->type = file_stream;
        node->size = (uint64_t)-1;
    }
}

static errno_t dnsfs_stat(void *file, vfs_node_t node)
{
    dnsfs_handle_t *handle = (dnsfs_handle_t *)file;
    if (handle == NULL) {
        return VFS_STATUS_FAILED;
    }

    node->fsid = dnsfs_id;
    if (handle->kind == DNSFS_KIND_ROOT) {
        node->type = file_dir;
        node->size = 0;
    } else {
        node->type = file_stream;
        node->size = (uint64_t)-1;
    }
    return EOK;
}

static size_t dnsfs_read(void *file, void *addr, size_t offset, size_t size)
{
    dnsfs_handle_t *handle = (dnsfs_handle_t *)file;
    if (handle == NULL || addr == NULL || handle->kind == DNSFS_KIND_ROOT) {
        return VFS_STATUS_FAILED;
    }

    if (handle->kind == DNSFS_KIND_SERVER) {
        uint32_t dns_addr = 0;
        if (socket_get_dns_server(&dns_addr) < 0) {
            return 0;
        }

        char ip_text[64];
        char resolv_conf[96];
        dnsfs_format_ipv4_text(dns_addr, ip_text, sizeof(ip_text));
        snprintf(resolv_conf, sizeof(resolv_conf), "nameserver %s\n", ip_text);
        return dnsfs_copy_text_out(resolv_conf, strlen(resolv_conf), addr, offset, size);
    }

    spin_lock(&handle->lock);
    size_t copied = dnsfs_copy_text_out(handle->buffer, handle->length, addr, offset, size);
    spin_unlock(&handle->lock);
    return copied;
}

static size_t dnsfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)offset;
    dnsfs_handle_t *handle = (dnsfs_handle_t *)file;
    if (handle == NULL || addr == NULL || handle->kind == DNSFS_KIND_ROOT) {
        return VFS_STATUS_FAILED;
    }

    char input[256];
    if (size >= sizeof(input)) {
        size = sizeof(input) - 1;
    }
    memcpy(input, addr, size);
    input[size] = '\0';

    if (handle->kind == DNSFS_KIND_SERVER) {
        const char *ip_text = dnsfs_extract_server_ip(input);
        if (strcmp(ip_text, "auto") == 0 || strcmp(ip_text, "dhcp") == 0 || ip_text[0] == '\0') {
            if (socket_set_dns_server(0) < 0) {
                return VFS_STATUS_FAILED;
            }
            return size;
        }
        uint32_t    dns_addr = 0;
        if (!dnsfs_parse_ipv4_text(ip_text, &dns_addr)) {
            return VFS_STATUS_FAILED;
        }
        if (socket_set_dns_server(dns_addr) < 0) {
            return VFS_STATUS_FAILED;
        }
        return size;
    }

    int   family = AF_INET;
    char *host   = NULL;
    if (dnsfs_parse_resolve_request(input, &family, &host) < 0 || host == NULL || host[0] == '\0') {
        spin_lock(&handle->lock);
        handle->buffer[0] = '\0';
        handle->length    = 0;
        spin_unlock(&handle->lock);
        return size;
    }

    char resolved_text[96];
    memset(resolved_text, 0, sizeof(resolved_text));

    int status = -EINVAL;
    if (family == AF_INET) {
        uint32_t resolved_addr = 0;
        status = socket_resolve_ipv4(host, &resolved_addr);
        if (status >= 0) {
            dnsfs_format_ipv4_text(resolved_addr, resolved_text, sizeof(resolved_text));
        }
    } else if (family == AF_INET6) {
        uint8_t resolved_addr[16];
        memset(resolved_addr, 0, sizeof(resolved_addr));
        status = socket_resolve_ipv6(host, resolved_addr);
        if (status >= 0) {
            dnsfs_format_ipv6_text(resolved_addr, resolved_text, sizeof(resolved_text));
        }
    } else {
        uint8_t resolved_addr6[16];
        memset(resolved_addr6, 0, sizeof(resolved_addr6));
        status = socket_resolve_ipv6(host, resolved_addr6);
        if (status >= 0) {
            dnsfs_format_ipv6_text(resolved_addr6, resolved_text, sizeof(resolved_text));
        } else {
            uint32_t resolved_addr4 = 0;
            status = socket_resolve_ipv4(host, &resolved_addr4);
            if (status >= 0) {
                dnsfs_format_ipv4_text(resolved_addr4, resolved_text, sizeof(resolved_text));
            }
        }
    }

    if (status < 0 || resolved_text[0] == '\0') {
        spin_lock(&handle->lock);
        handle->buffer[0] = '\0';
        handle->length    = 0;
        spin_unlock(&handle->lock);
        return size;
    }
    spin_lock(&handle->lock);
    strncpy(handle->buffer, resolved_text, sizeof(handle->buffer) - 1);
    handle->buffer[sizeof(handle->buffer) - 1] = '\0';
    handle->length = strlen(handle->buffer);
    spin_unlock(&handle->lock);
    return size;
}

static int dnsfs_dummy()
{
    return -ENOSYS;
}

static struct vfs_callback dnsfs_callbacks = {
    .mount    = dnsfs_mount,
    .unmount  = (vfs_unmount_t)empty,
    .open     = dnsfs_open,
    .close    = (vfs_close_t)empty,
    .read     = dnsfs_read,
    .write    = dnsfs_write,
    .readlink = (vfs_readlink_t)dnsfs_dummy,
    .mkdir    = (vfs_mk_t)empty,
    .mkfile   = (vfs_mk_t)empty,
    .link     = (vfs_mk_t)dnsfs_dummy,
    .symlink  = (vfs_mk_t)dnsfs_dummy,
    .stat     = dnsfs_stat,
    .ioctl    = (vfs_ioctl_t)dnsfs_dummy,
    .dup      = (vfs_dup_t)empty,
    .poll     = (vfs_poll_t)empty,
    .map      = (vfs_mapfile_t)empty,
    .resize   = (vfs_resize_t)dnsfs_dummy,
    .del      = (vfs_del_t)empty,
    .rename   = (vfs_rename_t)empty,
};

int dnsfs_setup(void)
{
    dnsfs_id = vfs_regist("dnsfs", &dnsfs_callbacks, DNS_REGISTER_ID, 0x444e5346);

    vfs_mkdir("/run");
    vfs_mkdir("/run/dns");
    vfs_node_t dns_root = vfs_open("/run/dns");
    if (dns_root == NULL) {
        return -ENOENT;
    }
    if (vfs_mount((const char *)DNS_REGISTER_ID, dns_root) == VFS_STATUS_FAILED) {
        return -EIO;
    }

    vfs_mkdir("/etc");
    if (vfs_open("/etc/resolv.conf") == NULL) {
        vfs_symlink("/run/dns/server", "/etc/resolv.conf");
    }

    return 0;
}
