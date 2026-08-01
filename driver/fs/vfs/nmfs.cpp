#define ALL_IMPLEMENTATION

#include <errno.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <net/socket.h>

enum {
    NMFS_KIND_ROOT = 0,
    NMFS_KIND_STATUS = 1,
    NMFS_KIND_STATE = 2,
    NMFS_KIND_IPV4 = 3,
    NMFS_KIND_IPV6 = 4,
};

typedef struct nmfs_handle {
    int kind;
} nmfs_handle_t;

static int           nmfs_id   = 0;
static vfs_node_t    nmfs_root = NULL;
static nmfs_handle_t nmfs_root_handle   = {NMFS_KIND_ROOT};
static nmfs_handle_t nmfs_status_handle = {NMFS_KIND_STATUS};
static nmfs_handle_t nmfs_state_handle  = {NMFS_KIND_STATE};
static nmfs_handle_t nmfs_ipv4_handle   = {NMFS_KIND_IPV4};
static nmfs_handle_t nmfs_ipv6_handle   = {NMFS_KIND_IPV6};

static bool nmfs_parse_ipv4_text(const char *text, uint32_t *out_addr)
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

static void nmfs_format_ipv4_text(uint32_t addr, char *buffer, size_t size)
{
    if (buffer == NULL || size == 0) return;

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

static void nmfs_format_ipv6_text(const uint8_t addr[16], char *buffer, size_t size)
{
    if (buffer == NULL || size == 0) return;
    if (addr == NULL) {
        buffer[0] = '\0';
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

static size_t nmfs_copy_text_out(const char *text, size_t text_len, void *addr, size_t offset, size_t size)
{
    if (text == NULL || addr == NULL || offset >= text_len) {
        return 0;
    }

    size_t remain = text_len - offset;
    if (size > remain) {
        size = remain;
    }
    memcpy(addr, text + offset, size);
    return size;
}

static void nmfs_trim_line(char *buffer)
{
    if (buffer == NULL) {
        return;
    }

    size_t len = strlen(buffer);
    while (len > 0 &&
           (buffer[len - 1] == '\r' || buffer[len - 1] == '\n' ||
            buffer[len - 1] == ' ' || buffer[len - 1] == '\t')) {
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

static int nmfs_parse_ipv4_config_text(char *input, socket_ipv4_config_t *out_config)
{
    bool method_set  = false;
    bool address_set = false;
    bool netmask_set = false;

    if (input == NULL || out_config == NULL) {
        return -EINVAL;
    }

    memset(out_config, 0, sizeof(*out_config));

    char *cursor = input;
    while (*cursor != '\0') {
        char *line = cursor;
        while (*cursor != '\0' && *cursor != '\n') {
            cursor++;
        }
        if (*cursor == '\n') {
            *cursor++ = '\0';
        }

        nmfs_trim_line(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        char *equals = strchr(line, '=');
        if (equals == NULL) {
            return -EINVAL;
        }
        *equals = '\0';

        char *key   = line;
        char *value = equals + 1;
        nmfs_trim_line(key);
        nmfs_trim_line(value);

        if (strcmp(key, "method") == 0) {
            if (strcmp(value, "auto") == 0 || strcmp(value, "dhcp") == 0) {
                out_config->method = SOCKET_IPV4_METHOD_DHCP;
            } else if (strcmp(value, "manual") == 0 || strcmp(value, "static") == 0) {
                out_config->method = SOCKET_IPV4_METHOD_MANUAL;
            } else {
                return -EINVAL;
            }
            method_set = true;
        } else if (strcmp(key, "address") == 0) {
            if (!nmfs_parse_ipv4_text(value, &out_config->ipv4_addr)) {
                return -EINVAL;
            }
            address_set = true;
        } else if (strcmp(key, "netmask") == 0) {
            if (!nmfs_parse_ipv4_text(value, &out_config->ipv4_netmask)) {
                return -EINVAL;
            }
            netmask_set = true;
        } else if (strcmp(key, "gateway") == 0) {
            if (value[0] == '\0' || strcmp(value, "0.0.0.0") == 0) {
                out_config->ipv4_gateway = 0;
            } else if (!nmfs_parse_ipv4_text(value, &out_config->ipv4_gateway)) {
                return -EINVAL;
            }
        } else {
            return -EINVAL;
        }
    }

    if (!method_set) {
        return -EINVAL;
    }
    if (out_config->method == SOCKET_IPV4_METHOD_MANUAL && (!address_set || !netmask_set)) {
        return -EINVAL;
    }

    return 0;
}

static const char *nmfs_state_name(const socket_netinfo_t *info)
{
    if (info == NULL || !info->stack_ready) return "starting";
    if (info->link_up && info->ip_ready) return "connected";
    if (info->link_up) return "configuring";
    return "disconnected";
}

static const char *nmfs_connectivity_name(const socket_netinfo_t *info)
{
    if (info == NULL || !info->stack_ready) return "unknown";
    return (info->link_up && info->ip_ready) ? "full" : "none";
}

static const char *nmfs_method_name(const socket_netinfo_t *info)
{
    if (info == NULL) return "disabled";

    switch (info->ipv4_method) {
    case SOCKET_IPV4_METHOD_DHCP:
        return "auto";
    case SOCKET_IPV4_METHOD_MANUAL:
        return "manual";
    default:
        return "disabled";
    }
}

static const char *nmfs_dhcp_state_name(const socket_netinfo_t *info)
{
    if (info == NULL || info->ipv4_method != SOCKET_IPV4_METHOD_DHCP) return "inactive";

    switch (info->dhcp_state) {
    case 0:
        return "off";
    case 1:
        return "requesting";
    case 2:
        return "init";
    case 3:
        return "rebooting";
    case 4:
        return "rebinding";
    case 5:
        return "renewing";
    case 6:
        return "selecting";
    case 7:
        return "informing";
    case 8:
        return "checking";
    case 9:
        return "permanent";
    case 10:
        return "bound";
    case 11:
        return "releasing";
    case 12:
        return "backing-off";
    default:
        return "unknown";
    }
}

static size_t nmfs_build_status_text(char *buffer, size_t size)
{
    socket_netinfo_t info;
    memset(&info, 0, sizeof(info));

    if (buffer == NULL || size == 0) return 0;

    if (socket_get_netinfo(&info) < 0) {
        return (size_t)snprintf(buffer, size,
                                "[device]\nmanaged=true\nstate=unavailable\n\n"
                                "[connectivity]\nstate=unknown\n");
    }

    char ip_text[32]      = "unassigned";
    char mask_text[32]    = "unassigned";
    char gateway_text[32] = "unassigned";
    char dns_text[32]     = "unassigned";
    char ipv6_linklocal_text[64] = "unassigned";
    char ipv6_global_text[64]    = "unassigned";
    if (info.ipv4_addr != 0) {
        nmfs_format_ipv4_text(info.ipv4_addr, ip_text, sizeof(ip_text));
    }
    if (info.ipv4_netmask != 0) {
        nmfs_format_ipv4_text(info.ipv4_netmask, mask_text, sizeof(mask_text));
    }
    if (info.ipv4_gateway != 0) {
        nmfs_format_ipv4_text(info.ipv4_gateway, gateway_text, sizeof(gateway_text));
    }
    if (info.dns_server != 0) {
        nmfs_format_ipv4_text(info.dns_server, dns_text, sizeof(dns_text));
    }
    if (info.ipv6_linklocal_ready) {
        nmfs_format_ipv6_text(info.ipv6_linklocal, ipv6_linklocal_text, sizeof(ipv6_linklocal_text));
    }
    if (info.ipv6_global_ready) {
        nmfs_format_ipv6_text(info.ipv6_global, ipv6_global_text, sizeof(ipv6_global_text));
    }

    const char *ifname     = info.ifname[0] != '\0' ? info.ifname : "e0";
    const char *method     = nmfs_method_name(&info);
    const char *dhcp_state = nmfs_dhcp_state_name(&info);

    return (size_t)snprintf(
        buffer, size,
        "[device]\n"
        "managed=true\n"
        "interface=%s\n"
        "state=%s\n"
        "carrier=%s\n"
        "mac=%02x:%02x:%02x:%02x:%02x:%02x\n"
        "mtu=%u\n"
        "\n"
        "[ipv4]\n"
        "method=%s\n"
        "dhcp-state=%s\n"
        "address=%s\n"
        "netmask=%s\n"
        "gateway=%s\n"
        "dns=%s\n"
        "\n"
        "[ipv6]\n"
        "link-local=%s\n"
        "global=%s\n"
        "\n"
        "[connectivity]\n"
        "state=%s\n",
        ifname,
        nmfs_state_name(&info),
        info.link_up ? "true" : "false",
        info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5],
        (unsigned int)info.mtu,
        method,
        dhcp_state,
        ip_text,
        mask_text,
        gateway_text,
        dns_text,
        ipv6_linklocal_text,
        ipv6_global_text,
        nmfs_connectivity_name(&info));
}

static size_t nmfs_build_state_text(char *buffer, size_t size)
{
    socket_netinfo_t info;
    memset(&info, 0, sizeof(info));

    if (buffer == NULL || size == 0) return 0;

    if (socket_get_netinfo(&info) < 0) {
        return (size_t)snprintf(buffer, size, "state=unavailable\n");
    }

    return (size_t)snprintf(buffer, size,
                            "state=%s\nconnectivity=%s\nmanaged=true\n",
                            nmfs_state_name(&info), nmfs_connectivity_name(&info));
}

static size_t nmfs_build_ipv4_text(char *buffer, size_t size)
{
    socket_netinfo_t info;
    memset(&info, 0, sizeof(info));

    if (buffer == NULL || size == 0) {
        return 0;
    }

    if (socket_get_netinfo(&info) < 0) {
        return (size_t)snprintf(buffer, size,
                                "method=auto\n"
                                "address=0.0.0.0\n"
                                "netmask=0.0.0.0\n"
                                "gateway=0.0.0.0\n");
    }

    char ip_text[32]      = "0.0.0.0";
    char mask_text[32]    = "0.0.0.0";
    char gateway_text[32] = "0.0.0.0";

    if (info.ipv4_addr != 0) {
        nmfs_format_ipv4_text(info.ipv4_addr, ip_text, sizeof(ip_text));
    }
    if (info.ipv4_netmask != 0) {
        nmfs_format_ipv4_text(info.ipv4_netmask, mask_text, sizeof(mask_text));
    }
    if (info.ipv4_gateway != 0) {
        nmfs_format_ipv4_text(info.ipv4_gateway, gateway_text, sizeof(gateway_text));
    }

    return (size_t)snprintf(buffer, size,
                            "method=%s\n"
                            "address=%s\n"
                            "netmask=%s\n"
                            "gateway=%s\n",
                            nmfs_method_name(&info),
                            ip_text,
                            mask_text,
                            gateway_text);
}

static size_t nmfs_build_ipv6_text(char *buffer, size_t size)
{
    socket_netinfo_t info;
    memset(&info, 0, sizeof(info));

    if (buffer == NULL || size == 0) {
        return 0;
    }

    if (socket_get_netinfo(&info) < 0) {
        return (size_t)snprintf(buffer, size,
                                "link-local=\n"
                                "global=\n");
    }

    char linklocal_text[64] = "";
    char global_text[64]    = "";
    if (info.ipv6_linklocal_ready) {
        nmfs_format_ipv6_text(info.ipv6_linklocal, linklocal_text, sizeof(linklocal_text));
    }
    if (info.ipv6_global_ready) {
        nmfs_format_ipv6_text(info.ipv6_global, global_text, sizeof(global_text));
    }

    return (size_t)snprintf(buffer, size,
                            "link-local=%s\n"
                            "global=%s\n",
                            linklocal_text,
                            global_text);
}

static errno_t nmfs_mount(const char *handle, vfs_node_t node)
{
    if ((uint64_t)handle != NM_REGISTER_ID) {
        return VFS_STATUS_FAILED;
    }

    node->fsid   = nmfs_id;
    node->type   = file_dir;
    node->handle = &nmfs_root_handle;
    nmfs_root    = node;

    vfs_node_t status_node = vfs_child_append(node, "status", &nmfs_status_handle);
    if (status_node != NULL) {
        status_node->type = file_stream;
        status_node->size = (uint64_t)-1;
    }

    vfs_node_t state_node = vfs_child_append(node, "state", &nmfs_state_handle);
    if (state_node != NULL) {
        state_node->type = file_stream;
        state_node->size = (uint64_t)-1;
    }

    vfs_node_t ipv4_node = vfs_child_append(node, "ipv4", &nmfs_ipv4_handle);
    if (ipv4_node != NULL) {
        ipv4_node->type = file_stream;
        ipv4_node->size = (uint64_t)-1;
    }

    vfs_node_t ipv6_node = vfs_child_append(node, "ipv6", &nmfs_ipv6_handle);
    if (ipv6_node != NULL) {
        ipv6_node->type = file_stream;
        ipv6_node->size = (uint64_t)-1;
    }

    return VFS_STATUS_SUCCESS;
}

static void nmfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    if (node == nmfs_root) {
        node->handle = &nmfs_root_handle;
        node->type   = file_dir;
        node->size   = 0;
    } else if (name != NULL && strcmp(name, "status") == 0) {
        node->handle = &nmfs_status_handle;
        node->type   = file_stream;
        node->size   = (uint64_t)-1;
    } else if (name != NULL && strcmp(name, "state") == 0) {
        node->handle = &nmfs_state_handle;
        node->type   = file_stream;
        node->size   = (uint64_t)-1;
    } else if (name != NULL && strcmp(name, "ipv4") == 0) {
        node->handle = &nmfs_ipv4_handle;
        node->type   = file_stream;
        node->size   = (uint64_t)-1;
    } else if (name != NULL && strcmp(name, "ipv6") == 0) {
        node->handle = &nmfs_ipv6_handle;
        node->type   = file_stream;
        node->size   = (uint64_t)-1;
    } else {
        node->type = file_stream;
        node->size = (uint64_t)-1;
    }
}

static errno_t nmfs_stat(void *file, vfs_node_t node)
{
    nmfs_handle_t *handle = (nmfs_handle_t *)file;
    if (handle == NULL) {
        return VFS_STATUS_FAILED;
    }

    node->fsid = nmfs_id;
    if (handle->kind == NMFS_KIND_ROOT) {
        node->type = file_dir;
        node->size = 0;
    } else {
        node->type = file_stream;
        node->size = (uint64_t)-1;
    }
    return EOK;
}

static size_t nmfs_read(void *file, void *addr, size_t offset, size_t size)
{
    nmfs_handle_t *handle = (nmfs_handle_t *)file;
    if (handle == NULL || addr == NULL || handle->kind == NMFS_KIND_ROOT) {
        return VFS_STATUS_FAILED;
    }

    char text[768];
    size_t text_len = 0;
    memset(text, 0, sizeof(text));

    if (handle->kind == NMFS_KIND_STATUS) {
        text_len = nmfs_build_status_text(text, sizeof(text));
    } else if (handle->kind == NMFS_KIND_STATE) {
        text_len = nmfs_build_state_text(text, sizeof(text));
    } else if (handle->kind == NMFS_KIND_IPV4) {
        text_len = nmfs_build_ipv4_text(text, sizeof(text));
    } else if (handle->kind == NMFS_KIND_IPV6) {
        text_len = nmfs_build_ipv6_text(text, sizeof(text));
    }

    if (text_len >= sizeof(text)) {
        text_len = sizeof(text) - 1;
    }
    return nmfs_copy_text_out(text, text_len, addr, offset, size);
}

static size_t nmfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)offset;
    nmfs_handle_t *handle = (nmfs_handle_t *)file;
    if (handle == NULL || addr == NULL || handle->kind == NMFS_KIND_ROOT) {
        return VFS_STATUS_FAILED;
    }

    if (handle->kind != NMFS_KIND_IPV4) {
        return VFS_STATUS_FAILED;
    }

    char input[256];
    if (size >= sizeof(input)) {
        size = sizeof(input) - 1;
    }
    memcpy(input, addr, size);
    input[size] = '\0';

    socket_ipv4_config_t config;
    if (nmfs_parse_ipv4_config_text(input, &config) < 0) {
        return VFS_STATUS_FAILED;
    }
    if (socket_set_ipv4_config(&config) < 0) {
        return VFS_STATUS_FAILED;
    }
    return size;
}

static int nmfs_dummy()
{
    return -ENOSYS;
}

static struct vfs_callback nmfs_callbacks = {
    .mount    = nmfs_mount,
    .unmount  = (vfs_unmount_t)empty,
    .open     = nmfs_open,
    .close    = (vfs_close_t)empty,
    .read     = nmfs_read,
    .write    = nmfs_write,
    .readlink = (vfs_readlink_t)nmfs_dummy,
    .mkdir    = (vfs_mk_t)empty,
    .mkfile   = (vfs_mk_t)empty,
    .link     = (vfs_mk_t)nmfs_dummy,
    .symlink  = (vfs_mk_t)nmfs_dummy,
    .stat     = nmfs_stat,
    .ioctl    = (vfs_ioctl_t)nmfs_dummy,
    .dup      = (vfs_dup_t)empty,
    .poll     = (vfs_poll_t)empty,
    .map      = (vfs_mapfile_t)empty,
    .resize   = (vfs_resize_t)nmfs_dummy,
    .del      = (vfs_del_t)empty,
    .rename   = (vfs_rename_t)empty,
};

int nmfs_setup(void)
{
    nmfs_id = vfs_regist("nmfs", &nmfs_callbacks, NM_REGISTER_ID, 0x4e4d4653);

    vfs_mkdir("/run");
    vfs_mkdir("/run/NetworkManager");
    vfs_node_t nm_root = vfs_open("/run/NetworkManager");
    if (nm_root == NULL) {
        return -ENOENT;
    }
    if (vfs_mount((const char *)NM_REGISTER_ID, nm_root) == VFS_STATUS_FAILED) {
        return -EIO;
    }
    return 0;
}
