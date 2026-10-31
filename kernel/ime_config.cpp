#include <user/permissions.h>
#include "ime_internal.h"
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <ime_utf8.h>
#include <krlibc.h>

static const ImePlatform *platform;
static const uint64_t PRIVATE_NODE = 1ULL << 60, READ_ONLY_NODE = 1ULL << 61, ANCHOR_NODE = 1ULL << 62;
static const uint32_t MAX_PATH_RULES = 32;

const ImePlatform *ime_platform()
{
    return __atomic_load_n(&platform, __ATOMIC_ACQUIRE);
}

static bool component_equal(const char *a, size_t size, const char *b, size_t length)
{
    if (size == 1 && a[0] == '*') return length != 0;
    if (size != length) return false;
    for (size_t i = 0; i < size; ++i)
    {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return true;
}

static const char *component_end(const char *p)
{
    while (*p && *p != '/') ++p;
    return p;
}

static bool valid_path(const char *path, bool pattern = false)
{
    if (!path || !memchr(path, 0, XIME_PATH_BYTES) || path[0] != '/' || !path[1]) return false;
    for (const char *p = path + 1; *p;)
    {
        const char *end = component_end(p);
        size_t size = end - p;
        if (!size || (size == 1 && *p == '.') || (size == 2 && p[0] == '.' && p[1] == '.')) return false;
        for (size_t i = 0; i < size; ++i)
            if (p[i] == '\\' || (p[i] == '*' && (!pattern || size != 1))) return false;
        if (*end && !end[1]) return false;
        p = *end ? end + 1 : end;
    }
    return true;
}

static uint64_t rule_access(const ImePathRule &rule)
{
    return rule.access == IME_PATH_PRIVATE ? PRIVATE_NODE : READ_ONLY_NODE;
}

// Paths are matched by components, so a rule never accepts a sibling sharing
// only a string prefix. A wildcard consumes exactly one directory component.
static uint64_t path_policy(const ImePlatform *value, const char *path, bool *variable_anchor = NULL)
{
    uint64_t result = 0;
    if (!value) return result;
    for (uint32_t i = 0; i < value->path_count; ++i)
    {
        const char *rule = value->paths[i].path + 1, *part = path + 1;
        bool variable = false;
        while (*rule && *part)
        {
            const char *r = component_end(rule), *p = component_end(part);
            if (!component_equal(rule, r - rule, part, p - part)) break;
            variable |= r - rule == 1 && *rule == '*';
            rule = *r ? r + 1 : r;
            part = *p ? p + 1 : p;
        }
        if (!*rule) result |= rule_access(value->paths[i]);
        else if (!*part)
        {
            // Fixed ancestors cannot be shadowed even before they exist.
            // Account directories may be created, but existing ones are pinned.
            if (!variable) result |= ANCHOR_NODE;
            else if (variable_anchor) *variable_anchor = true;
        }
    }
    return result;
}

int ime_install_platform(const ImePlatform *value)
{
    if (!value || value->size != sizeof(*value) || value->version != 1 || !value->load || !value->save ||
        !valid_path(value->manager) || value->provider_count > XIME_MAX_INSTALLED ||
        !value->path_count || value->path_count > MAX_PATH_RULES || !value->paths ||
        (value->provider_count && !value->providers)) return -EINVAL;
    for (uint32_t i = 0; i < value->path_count; ++i)
        if (!valid_path(value->paths[i].path, true) ||
            (value->paths[i].access != IME_PATH_PRIVATE && value->paths[i].access != IME_PATH_READ_ONLY))
            return -EINVAL;
    if (!valid_path(value->account_store, true)) return -EINVAL;
    const char *account = strchr(value->account_store, '*');
    if (!account || strchr(account + 1, '*') ||
        !(path_policy(value, value->account_store) & PRIVATE_NODE)) return -EINVAL;
    if (!(path_policy(value, value->manager) & READ_ONLY_NODE)) return -EINVAL;
    for (uint32_t i = 0; i < value->provider_count; ++i)
    {
        const ImeProviderPolicy &provider = value->providers[i];
        if (!ime_valid_id(provider.id) || !valid_path(provider.executable) || !valid_path(provider.settings) ||
            !strcmp(provider.executable, provider.settings) || !strcmp(provider.executable, value->manager) ||
            !strcmp(provider.settings, value->manager) ||
            !(path_policy(value, provider.executable) & READ_ONLY_NODE) ||
            !(path_policy(value, provider.settings) & READ_ONLY_NODE) ||
            (provider.data_root && (!valid_path(provider.data_root) ||
                                   !(path_policy(value, provider.data_root) & PRIVATE_NODE))) ||
            (provider.option_mask & (XIME_DATA_ENABLED | XIME_DATA_ASYNC | XIME_DATA_PENDING)) ||
            (provider.default_flags & ~(provider.option_mask | XIME_DATA_ENABLED))) return -EINVAL;
        for (uint32_t j = 0; j < i; ++j)
            if (!strcmp(provider.id, value->providers[j].id) ||
                !strcmp(provider.settings, value->providers[j].settings) ||
                !strcmp(provider.executable, value->providers[j].executable) ||
                !strcmp(provider.executable, value->providers[j].settings) ||
                !strcmp(provider.settings, value->providers[j].executable)) return -EINVAL;
    }
    const ImePlatform *expected = NULL;
    if (__atomic_compare_exchange_n(&platform, &expected, value, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) return 0;
    return expected == value ? 0 : -EBUSY;
}

const ImeProviderPolicy *ime_provider(const char *id)
{
    const ImePlatform *value = ime_platform();
    for (uint32_t i = 0; value && i < value->provider_count; ++i)
        if (!strcmp(value->providers[i].id, id)) return &value->providers[i];
    return NULL;
}

const ImeProviderPolicy *ime_settings_provider(const char *executable)
{
    const ImePlatform *value = ime_platform();
    for (uint32_t i = 0; executable && value && i < value->provider_count; ++i)
        if (!strcmp(value->providers[i].settings, executable)) return &value->providers[i];
    return NULL;
}

// Low bits remember matching rule prefixes across FAT long/short aliases;
// high bits enforce access. Only full subtree restrictions inherit to children.
static uint64_t name_policy(vfs_node_t parent, const char *name)
{
    const ImePlatform *value = ime_platform();
    if (!value || !parent || !name) return 0;
    const char *parts[64];
    unsigned depth = 0;
    vfs_node_t node = parent;
    while (node && node != rootdir && depth < 64)
    {
        parts[depth++] = node->name ? node->name : "";
        node = node->parent;
    }
    if (node != rootdir) return 0;
    uint64_t result = 0;
    for (uint32_t i = 0; i < value->path_count; ++i)
    {
        const char *rule = value->paths[i].path + 1;
        bool matched = true;
        bool known_parent = __atomic_load_n(&parent->ime_protection, __ATOMIC_RELAXED) & (1ULL << i);
        for (unsigned j = 0; j <= depth; ++j)
        {
            if (!*rule) break;
            const char *end = component_end(rule), *part = j == depth ? name : parts[depth - j - 1];
            if ((!known_parent || j == depth) && !component_equal(rule, end - rule, part, strlen(part)))
            {
                matched = false;
                break;
            }
            rule = *end ? end + 1 : end;
        }
        if (!matched) continue;
        if (!*rule) result |= rule_access(value->paths[i]);
        else result |= (1ULL << i) | ANCHOR_NODE;
    }
    return result;
}

void ime_vfs_resolved_name(vfs_node_t node, const char *name)
{
    if (node) __atomic_fetch_or(&node->ime_protection, name_policy(node->parent, name), __ATOMIC_RELAXED);
}

static uint64_t node_policy(vfs_node_t node)
{
    uint64_t result = 0;
    for (unsigned links = 0; node && links < 16; ++links, node = node->linkto)
    {
        result |= __atomic_load_n(&node->ime_protection, __ATOMIC_RELAXED) | name_policy(node->parent, node->name);
        vfs_node_t parent = node->parent;
        for (unsigned depth = 0; parent && depth < 64; ++depth, parent = parent->parent)
            result |= (__atomic_load_n(&parent->ime_protection, __ATOMIC_RELAXED) |
                       name_policy(parent->parent, parent->name)) &
                      (PRIVATE_NODE | READ_ONLY_NODE);
        if (node == rootdir) result |= ANCHOR_NODE;
    }
    return result;
}

static bool ime_policy_access(vfs_node_t node, const char *path, ImeFileOperation operation)
{
    tcb_t task = get_current_task();
    if (!task || task->task_level != TASK_APPLICATION_LEVEL) return true;
    if (task->ime_config_depth && !task->permission_profile_owner) return true;
    if (permissions_dialog_metadata(node, operation)) return true;
    if (task->permission_profile_owner) return true;
    if (operation == IME_FILE_MOUNT || (node && (node->type & file_block) && !task->ime_storage_depth)) return false;
    bool mutation = operation == IME_FILE_WRITE || operation == IME_FILE_REMOVE ||
                    operation == IME_FILE_MOUNT || operation == IME_FILE_READ_WRITE;
    uint64_t protection = node_policy(node);
    if (path)
    {
        char *normalized = normalize_path(path);
        if (!normalized) return false;
        bool variable_anchor = false;
        protection |= path_policy(ime_platform(), normalized, &variable_anchor);
        char *slash = strrchr(normalized, '/');
        if (slash && slash != normalized)
        {
            *slash = 0;
            vfs_node_t parent = vfs_open(normalized);
            if (parent)
            {
                uint64_t flags = name_policy(parent, slash + 1) | node_policy(parent);
                protection |= flags & (PRIVATE_NODE | READ_ONLY_NODE);
                variable_anchor |= (name_policy(parent, slash + 1) & ANCHOR_NODE) != 0;
                vfs_close(parent);
            }
            *slash = '/';
        }
        if (variable_anchor && mutation)
        {
            vfs_node_t target = vfs_open(normalized);
            if (target)
            {
                if (target->type & file_dir) protection |= ANCHOR_NODE;
                vfs_close(target);
            }
        }
        free(normalized);
    }
    if (protection & PRIVATE_NODE) return false;
    return !mutation || !(protection & (READ_ONLY_NODE | ANCHOR_NODE));
}

bool ime_vfs_access(vfs_node_t node, const char *path, ImeFileOperation operation)
{
    return permissions_file_access(node, path, operation) && ime_policy_access(node, path, operation);
}

bool ime_vfs_requirements(vfs_node_t node, const char *path, ImeFileOperation operation, uint8_t *required)
{
    return permissions_file_requirements(node, path, operation, required) && ime_policy_access(node, path, operation);
}

bool ime_valid_id(const char *id)
{
    if (!id || !id[0] || !strcmp(id, ".") || !strcmp(id, "..")) return false;
    for (size_t i = 0; i < XIME_ID_BYTES; ++i)
    {
        char c = id[i];
        if (!c) return true;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return false;
}

// Only the trusted callback's current thread receives temporary VFS access.
// No user process, including its other threads, inherits this scope.
struct ConfigScope
{
    tcb_t task;
    ConfigScope() : task(get_current_task()) { if (task) ++task->ime_config_depth; }
    ~ConfigScope() { if (task) --task->ime_config_depth; }
};

int ime_config_load(ImeRegistry *registry)
{
    const ImePlatform *value = ime_platform();
    if (!value) return -ENODEV;
    ConfigScope scope;
    int error = value->load(registry);
    if (error) return error;
    if (registry->count > XIME_MAX_INSTALLED || registry->mode > XIME_MODE_CHINESE ||
        !memchr(registry->active, 0, sizeof(registry->active)) ||
        (registry->active[0] && !ime_valid_id(registry->active))) return -EINVAL;
    for (uint32_t i = 0; i < registry->count; ++i)
    {
        XImeInfo &entry = registry->entries[i];
        if (!ime_valid_id(entry.id) || !entry.name[0] || !ime_utf8_valid(entry.name, sizeof(entry.name)) ||
            !valid_path(entry.executable)) return -EINVAL;
        const ImeProviderPolicy *provider = ime_provider(entry.id);
        if (provider && strcmp(provider->executable, entry.executable)) return -EINVAL;
        for (uint32_t j = 0; j < i; ++j)
            if (!strcmp(entry.id, registry->entries[j].id) ||
                !strcmp(entry.executable, registry->entries[j].executable)) return -EINVAL;
    }
    return 0;
}

int ime_config_save(const ImeRegistry *registry)
{
    const ImePlatform *value = ime_platform();
    if (!value) return -ENODEV;
    ConfigScope scope;
    return value->save(registry);
}
