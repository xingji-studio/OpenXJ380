// SPDX-License-Identifier: Apache-2.0
// Two checksummed text snapshots. Only the inactive slot is replaced; its
// commit marker is invalidated before any contents change and published last.
#include "xnet_settings.h"
#include <errno.h>
#include <fs/ext4/ext4.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <stdio.h>
#include <user/permissions.h>

namespace {
constexpr size_t limit = 1024, marker_offset = 7, marker_bytes = 8, body_offset = 16;
const char      *paths[]     = {"/config/network/settings.0.conf", "/config/network/settings.1.conf"};
const char       committed[] = "XNETDONE", invalid[] = "00000000";

void defaults(XNetSavedConfig *config)
{
    *config         = {};
    config->size    = sizeof(*config);
    config->version = 1;
    config->ipv4    = {sizeof(XNetInterfaceConfig), 1, 1, XNET_CONFIG_DHCP, {}, {}, 0, 0};
    config->dns     = {sizeof(XNetDnsConfig), 1, 1, XNET_DNS_AUTO, 0, 0, {}};
}

bool zero(const void *value, size_t bytes)
{
    const auto *p = static_cast<const unsigned char *>(value);
    for (size_t i = 0; i < bytes; ++i)
        if (p[i]) return false;
    return true;
}

bool ipv4_value(const XNetAddress &address)
{
    return address.family == XNET_IPV4 && !address.scope_id && zero(address.bytes + 4, 12);
}

uint32_t number(const XNetAddress &address)
{
    return uint32_t(address.bytes[0]) << 24 | uint32_t(address.bytes[1]) << 16 | uint32_t(address.bytes[2]) << 8 |
           address.bytes[3];
}

bool unicast(const XNetAddress &address)
{
    return ipv4_value(address) && address.bytes[0] && address.bytes[0] < 224 && number(address) != 0xffffffffU;
}

uint32_t checksum(const char *data, size_t bytes)
{
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < bytes; ++i)
    {
        crc ^= uint8_t(data[i]);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
}

struct Reader
{
    const char *data;
    size_t      bytes, offset;
    bool        field(const char *key, char *out, size_t capacity)
    {
        size_t key_bytes = strlen(key);
        if (offset + key_bytes >= bytes || memcmp(data + offset, key, key_bytes)) return false;
        offset      += key_bytes;
        size_t used  = 0;
        while (offset < bytes && data[offset] != '\n')
        {
            unsigned char ch = data[offset++];
            if (ch < 33 || ch > 126 || used + 1 >= capacity) return false;
            out[used++] = char(ch);
        }
        if (offset == bytes) return false;
        ++offset;
        out[used] = 0;
        return true;
    }
    bool integer(const char *key, uint64_t *value)
    {
        char text[21];
        if (!field(key, text, sizeof(text)) || !text[0] || (text[0] == '0' && text[1])) return false;
        uint64_t n = 0;
        for (const char *p = text; *p; ++p)
        {
            if (*p < '0' || *p > '9' || n > (~uint64_t(0) - uint64_t(*p - '0')) / 10) return false;
            n = n * 10 + uint64_t(*p - '0');
        }
        *value = n;
        return true;
    }
    bool address(const char *key, XNetAddress *out)
    {
        char text[16];
        if (!field(key, text, sizeof(text))) return false;
        XNetAddress result = {};
        result.family      = XNET_IPV4;
        const char *p      = text;
        for (unsigned part = 0; part < 4; ++part)
        {
            unsigned n = 0, digits = 0;
            bool     leading_zero = *p == '0';
            while (*p >= '0' && *p <= '9')
            {
                n = n * 10 + unsigned(*p++ - '0');
                if (++digits > 3 || n > 255 || (leading_zero && digits > 1)) return false;
            }
            if (!digits || (part != 3 && *p++ != '.') || (part == 3 && *p)) return false;
            result.bytes[part] = uint8_t(n);
        }
        *out = result;
        return true;
    }
};

int decode(const char *data, size_t bytes, XNetSavedConfig *config)
{
    Reader   reader = {data, bytes, 0};
    char     text[16];
    uint64_t enabled, prefix;
    defaults(config);
    if (!reader.field("commit=", text, sizeof(text)) || strcmp(text, committed) ||
        !reader.field("format=", text, sizeof(text)) || strcmp(text, "xnet-1") ||
        !reader.integer("revision=", &config->revision) || !config->revision ||
        config->revision == XNET_CONFIG_ANY_REVISION || !reader.integer("enabled=", &enabled) || enabled > 1 ||
        !reader.field("ipv4=", text, sizeof(text)))
        return -EBADMSG;
    config->flags = enabled ? XNET_SAVED_ENABLED : 0;
    if (!strcmp(text, "static"))
        config->ipv4.method = XNET_CONFIG_STATIC;
    else if (strcmp(text, "dhcp"))
        return -EBADMSG;
    if (!reader.address("address=", &config->ipv4.address) || !reader.integer("prefix=", &prefix) || prefix > 32 ||
        !reader.address("gateway=", &config->ipv4.gateway) || !reader.field("dns=", text, sizeof(text)))
        return -EBADMSG;
    config->ipv4.prefix_bits = uint32_t(prefix);
    if (!strcmp(text, "manual"))
    {
        config->dns.method       = XNET_DNS_MANUAL;
        config->dns.server_count = 1;
    }
    else if (strcmp(text, "auto"))
        return -EBADMSG;
    if (!reader.address("server=", &config->dns.servers[0])) return -EBADMSG;
    if (config->ipv4.method == XNET_CONFIG_DHCP)
    {
        if (number(config->ipv4.address) || number(config->ipv4.gateway)) return -EBADMSG;
        config->ipv4.address = config->ipv4.gateway = {};
    }
    if (config->dns.method == XNET_DNS_AUTO)
    {
        if (number(config->dns.servers[0])) return -EBADMSG;
        config->dns.servers[0] = {};
    }
    size_t crc_offset = reader.offset;
    if (!reader.field("crc32=", text, sizeof(text)) || strlen(text) != 8 || reader.offset != bytes) return -EBADMSG;
    uint32_t crc = 0;
    for (unsigned i = 0; i < 8; ++i)
    {
        char     ch    = text[i];
        unsigned digit = ch >= '0' && ch <= '9'   ? unsigned(ch - '0')
                         : ch >= 'a' && ch <= 'f' ? unsigned(ch - 'a' + 10)
                                                  : 16;
        if (digit == 16) return -EBADMSG;
        crc = (crc << 4) | digit;
    }
    if (crc_offset < body_offset || checksum(data + body_offset, crc_offset - body_offset) != crc ||
        xnet_settings_validate(*config))
        return -EBADMSG;
    return 0;
}

int encode(const XNetSavedConfig &config, char *data, size_t *bytes)
{
    const auto &ip  = config.ipv4.address.bytes;
    const auto &gw  = config.ipv4.gateway.bytes;
    const auto &dns = config.dns.servers[0].bytes;
    int         n   = snprintf(data, limit,
                               "commit=00000000\nformat=xnet-1\nrevision=%llu\nenabled=%u\nipv4=%s\n"
                                         "address=%u.%u.%u.%u\nprefix=%u\ngateway=%u.%u.%u.%u\ndns=%s\nserver=%u.%u.%u.%u\n",
                               (unsigned long long)config.revision, config.flags & XNET_SAVED_ENABLED ? 1U : 0U,
                     config.ipv4.method == XNET_CONFIG_STATIC ? "static" : "dhcp", ip[0], ip[1], ip[2], ip[3],
                               config.ipv4.prefix_bits, gw[0], gw[1], gw[2], gw[3],
                     config.dns.method == XNET_DNS_MANUAL ? "manual" : "auto", dns[0], dns[1], dns[2], dns[3]);
    if (n < int(body_offset) || size_t(n) >= limit) return -EOVERFLOW;
    int tail =
        snprintf(data + n, limit - size_t(n), "crc32=%08x\n", checksum(data + body_offset, size_t(n) - body_offset));
    if (tail < 0 || size_t(tail) >= limit - size_t(n)) return -EOVERFLOW;
    *bytes = size_t(n + tail);
    return 0;
}

int missing_error()
{
    // Older FAT name lookup reports absence without setting errno.
    int error = permissions_file_error();
    return !error || error == ENOENT ? -ENOENT : -error;
}

bool exact(vfs_node_t node, const char *path, bool directory)
{
    uint16_t allowed = directory ? file_dir : file_none;
    if (!node || node->linkto || (node->type & ~allowed) || bool(node->type & file_dir) != directory) return false;
    char *full   = vfs_get_fullpath(node);
    bool  result = full && !strcmp(full, path);
    free(full);
    return result;
}

int directories(bool create)
{
    static const char *directory_paths[] = {"/config", "/config/network"};
    for (const char *path : directory_paths)
    {
        vfs_node_t node = vfs_open_no_follow(path);
        if (!node && create && missing_error() == -ENOENT)
        {
            if (vfs_mkdir(path)) return -vfs_last_error();
            node = vfs_open_no_follow(path);
        }
        if (!node) return missing_error();
        bool valid  = exact(node, path, true);
        int  closed = vfs_close(node);
        if (!valid) return -ELOOP;
        if (closed) return -EIO;
    }
    return 0;
}

int read_slot(unsigned slot, XNetSavedConfig *config)
{
    vfs_node_t node = vfs_open_no_follow(paths[slot]);
    if (!node) return missing_error();
    int      error = exact(node, paths[slot], false) ? 0 : -ELOOP;
    uint64_t size  = node->size;
    char    *data  = NULL;
    if (!error && (size < body_offset || size >= limit)) error = -EBADMSG;
    if (!error)
    {
        data = static_cast<char *>(malloc(size));
        if (!data) error = -ENOMEM;
    }
    if (!error && vfs_read(node, data, 0, size) != size) error = -EIO;
    if (!error) error = decode(data, size, config);
    if (vfs_close(node)) error = -EIO;
    free(data);
    return error;
}

int read_current(XNetSavedConfig *config, int *slot)
{
    defaults(config);
    *slot     = -1;
    int error = directories(false);
    if (error) return error == -ENOENT ? 0 : error;
    XNetSavedConfig slots[2];
    int             errors[] = {read_slot(0, &slots[0]), read_slot(1, &slots[1])};
    for (int result : errors)
        if (result && result != -ENOENT && result != -EBADMSG) return result;
    bool bad = errors[0] == -EBADMSG || errors[1] == -EBADMSG;
    if (errors[0] && errors[1]) return bad ? -EBADMSG : 0;
    unsigned chosen = !errors[0] && (errors[1] || slots[0].revision >= slots[1].revision) ? 0 : 1;
    if (!errors[0] && !errors[1] && slots[0].revision == slots[1].revision &&
        memcmp(&slots[0], &slots[1], sizeof(slots[0])))
    {
        *config = slots[chosen];
        *slot   = int(chosen);
        return -EBADMSG;
    }
    *config = slots[chosen];
    if (bad) config->flags |= XNET_SAVED_RECOVERED;
    *slot = int(chosen);
    return 0;
}

int flush(vfs_node_t node)
{
    if (ext4fs_owns_node(node)) return ext4fs_sync(node);
    vfs_filesystem_t fs = get_filesystem_node(node);
    // FAT already f_syncs each successful write. A volatile/pseudo filesystem
    // must not claim that a persistent policy has been durably saved.
    return fs && !strcmp(fs->name, "fatfs") ? 0 : -EOPNOTSUPP;
}

int write_slot(unsigned destination, const XNetSavedConfig &next)
{
    int        error = 0;
    vfs_node_t node  = vfs_open_no_follow(paths[destination]);
    if (!node && missing_error() == -ENOENT)
    {
        if (vfs_mkfile(paths[destination])) return -vfs_last_error();
        node = vfs_open_no_follow(paths[destination]);
    }
    if (!node) return missing_error();
    if (!exact(node, paths[destination], false))
    {
        vfs_close(node);
        return -ELOOP;
    }
    char *data = static_cast<char *>(malloc(limit));
    if (!data)
    {
        vfs_close(node);
        return -ENOMEM;
    }
    size_t bytes = 0;
    error        = encode(next, data, &bytes);
    if (!error) error = flush(node); // Reject unsupported storage before changing snapshot bytes.
    if (!error && vfs_write(node, (void *)invalid, marker_offset, marker_bytes) != marker_bytes) error = -EIO;
    if (!error) error = flush(node);
    if (!error && vfs_resize(node, bytes)) error = -vfs_last_error();
    if (!error && vfs_write(node, data, 0, bytes) != bytes) error = -EIO;
    if (!error) error = flush(node);
    if (!error && vfs_write(node, (void *)committed, marker_offset, marker_bytes) != marker_bytes) error = -EIO;
    if (!error) error = flush(node);
    if (vfs_close(node)) error = -EIO;
    free(data);
    XNetSavedConfig checked;
    if (!error) error = read_slot(destination, &checked);
    if (!error && memcmp(&next, &checked, sizeof(next))) error = -EIO;
    return error;
}

} // namespace

int xnet_settings_validate(const XNetSavedConfig &config)
{
    const auto &ip  = config.ipv4;
    const auto &dns = config.dns;
    if (config.size != sizeof(config) || config.version != 1 || config.reserved || config.reserved64[0] ||
        config.reserved64[1] || (config.flags & ~(XNET_SAVED_ENABLED | XNET_SAVED_RECOVERED)) ||
        ip.size != sizeof(ip) || ip.version != 1 || ip.reserved || dns.size != sizeof(dns) || dns.version != 1 ||
        dns.reserved)
        return -EINVAL;
    if (ip.interface_id > 1 || dns.interface_id > 1) return -ENODEV;
    if (ip.method == XNET_CONFIG_DHCP)
    {
        if (ip.prefix_bits || !zero(&ip.address, sizeof(ip.address)) || !zero(&ip.gateway, sizeof(ip.gateway)))
            return -EINVAL;
    }
    else if (ip.method == XNET_CONFIG_STATIC)
    {
        if (!unicast(ip.address) || ip.address.bytes[0] == 127 || !ipv4_value(ip.gateway) || !ip.prefix_bits ||
            ip.prefix_bits > 32)
            return -EINVAL;
        uint32_t mask = 0xffffffffU << (32 - ip.prefix_bits), host = number(ip.address) & ~mask;
        if (ip.prefix_bits < 31 && (!host || host == ~mask)) return -EINVAL;
        if (number(ip.gateway) && (!unicast(ip.gateway) || number(ip.gateway) == number(ip.address) ||
                                   ((number(ip.gateway) ^ number(ip.address)) & mask)))
            return -EINVAL;
        if (number(ip.gateway) && ip.prefix_bits < 31 &&
            (!(number(ip.gateway) & ~mask) || (number(ip.gateway) & ~mask) == ~mask))
            return -EINVAL;
    }
    else
        return -EINVAL;
    if (!zero(&dns.servers[1], sizeof(dns.servers[1]))) return -EINVAL;
    if (dns.method == XNET_DNS_AUTO)
    {
        if (dns.server_count || !zero(&dns.servers[0], sizeof(dns.servers[0]))) return -EINVAL;
        if (ip.method != XNET_CONFIG_DHCP) return -EOPNOTSUPP;
    }
    else if (dns.method == XNET_DNS_MANUAL)
    {
        if (dns.server_count > 1) return -EOPNOTSUPP;
        if (dns.server_count != 1 || !unicast(dns.servers[0])) return -EINVAL;
    }
    else
        return -EINVAL;
    return 0;
}

int xnet_settings_read(XNetSavedConfig *config)
{
    XNetSavedConfig result;
    int             slot;
    int             error = read_current(&result, &slot);
    if (!error) *config = result;
    return error;
}

int xnet_settings_save(const XNetSavedConfig &config)
{
    int error = xnet_settings_validate(config);
    if (error) return error;
    XNetSavedConfig previous;
    int             slot;
    error      = read_current(&previous, &slot);
    bool force = config.revision == XNET_CONFIG_ANY_REVISION;
    if (error && !(force && error == -EBADMSG)) return error;
    if (!force && config.revision != previous.revision) return -ESTALE;
    if (previous.revision >= XNET_CONFIG_ANY_REVISION - 1) return -EOVERFLOW;
    XNetSavedConfig next = config;
    next.flags &= XNET_SAVED_ENABLED;
    next.revision = previous.revision + 1;
    next.ipv4.interface_id = 1;
    next.dns.interface_id = 1;

    error = directories(true);
    if (error) return error;
    unsigned destination = slot == 0 ? 1 : 0;
    error                = write_slot(destination, next);
    // A successful disable survives losing either file. After committing the
    // first tombstone, the previous slot is inactive and can be mirrored too.
    if (!error && !(next.flags & XNET_SAVED_ENABLED)) error = write_slot(1 - destination, next);
    return error;
}
