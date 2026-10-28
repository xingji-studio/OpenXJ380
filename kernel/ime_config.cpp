#include "ime_internal.h"
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <ime_utf8.h>
#include <krlibc.h>
#include <xapi_json.h>

static const size_t   CONFIG_LIMIT = 32768;
static const char    *CONFIG_PATH  = "/settings.json";
static const char    *PENDING_PATH = "/settings.json.ime-new";
static const char    *BACKUP_PATH  = "/settings.json.ime-backup";
static const uint64_t USER_DATA_NODE = 1ULL << 60;
static const uint64_t CONFIG_NODE  = 1ULL << 61;
static const uint64_t MANAGER_NODE = 1ULL << 62;

// Only the calling kernel transaction gains access; other threads of that
// process remain unprivileged, including threads running on another CPU.
struct ConfigScope
{
    tcb_t task;
    ConfigScope() : task(get_current_task())
    {
        if (task) ++task->ime_config_depth;
    }
    ~ConfigScope()
    {
        if (task) --task->ime_config_depth;
    }
};

static bool equal_fold(const char *a, const char *b)
{
    while (*a && *b)
    {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return *a == *b;
}

static bool user_data_parent(vfs_node_t node)
{
    for (unsigned depth = 0; node && depth < 64; ++depth, node = node->parent)
    {
        if (node->ime_protection & USER_DATA_NODE) return true;
        if (node->name && equal_fold(node->name, "xingji-pinyin") && node->parent &&
            node->parent->parent == rootdir && node->parent->name && equal_fold(node->parent->name, "config"))
            return true;
        if (node->name && equal_fold(node->name, "ime") && node->parent && node->parent->parent &&
            node->parent->parent->parent == rootdir && node->parent->parent->name &&
            equal_fold(node->parent->parent->name, "users")) return true;
    }
    return false;
}

static uint64_t protected_name(vfs_node_t parent, const char *name)
{
    if (!parent || !name) return 0;
    if (parent->parent == rootdir && parent->name && equal_fold(parent->name, "config") &&
        equal_fold(name, "xingji-pinyin")) return USER_DATA_NODE;
    // Both executables and their containing directory are immutable to apps;
    // the settings role is derived from this verified install location.
    unsigned depth = 0;
    for (vfs_node_t node = parent; node && node != rootdir && depth++ < 64; node = node->parent)
    {
        if (node->ime_protection & MANAGER_NODE) return MANAGER_NODE;
        if (node->name && equal_fold(node->name, "xingji-pinyin") && node->parent &&
            node->parent->parent == rootdir && node->parent->name && equal_fold(node->parent->name, "apps"))
            return MANAGER_NODE;
    }
    if (parent->parent == rootdir && parent->name && equal_fold(parent->name, "apps") &&
        equal_fold(name, "xingji-pinyin")) return MANAGER_NODE;
    if (user_data_parent(parent) ||
        (equal_fold(name, "ime") && parent->parent && parent->parent->parent == rootdir &&
         parent->parent->name && equal_fold(parent->parent->name, "users"))) return USER_DATA_NODE;
    if (parent == rootdir && (equal_fold(name, "settings.json") || equal_fold(name, "settings.json.ime-new") ||
                              equal_fold(name, "settings.json.ime-backup")))
        return CONFIG_NODE;
    if (parent->name && equal_fold(parent->name, "system") && parent->parent && parent->parent->name &&
        equal_fold(parent->parent->name, "apps") && parent->parent->parent == rootdir &&
        equal_fold(name, "ctrlmenu.elf"))
        return MANAGER_NODE;
    return 0;
}

void ime_vfs_resolved_name(vfs_node_t node, const char *name)
{
    if (node) node->ime_protection |= protected_name(node->parent, name);
}

static uint64_t node_protection(vfs_node_t node)
{
    if (!node) return 0;
    uint64_t flags = node->ime_protection | protected_name(node->parent, node->name);
    if (user_data_parent(node)) flags |= USER_DATA_NODE;
    // Resolved symlink/proxy objects must retain the target's restrictions.
    for (unsigned i = 0; node->linkto && i < 16; ++i)
    {
        node   = node->linkto;
        flags |= node->ime_protection | protected_name(node->parent, node->name);
    }
    return flags;
}

bool ime_vfs_access(vfs_node_t node, const char *path, ImeFileOperation operation)
{
    tcb_t task = get_current_task();
    if (!task || task->task_level != TASK_APPLICATION_LEVEL || task->ime_config_depth) return true;
    // An ordinary application must not remount the namespace or bypass the
    // filesystem through a raw device. Installer writes run in its kernel worker.
    if (operation == IME_FILE_MOUNT || (node && (node->type & file_block) && !task->ime_storage_depth)) return false;
    uint64_t protection = node_protection(node);
    if (path)
    {
        char *normalized = normalize_path(path);
        if (!normalized) return false;
        if (equal_fold(normalized, CONFIG_PATH) || equal_fold(normalized, PENDING_PATH) ||
            equal_fold(normalized, BACKUP_PATH))
            protection |= CONFIG_NODE;
        if (equal_fold(normalized, "/apps/system/ctrlmenu.elf")) protection |= MANAGER_NODE;
        const char *pinyin_roots[] = {IME_PINYIN_CONFIG, "/apps/xingji-pinyin"};
        size_t normalized_length = strlen(normalized);
        for (unsigned i = 0; i < 2; ++i)
        {
            size_t length = strlen(pinyin_roots[i]);
            if (normalized_length < length) continue;
            char saved = normalized[length];
            normalized[length] = 0;
            bool matches = equal_fold(normalized, pinyin_roots[i]);
            normalized[length] = saved;
            if (matches && (!saved || saved == '/')) protection |= i == 0 ? USER_DATA_NODE : MANAGER_NODE;
        }
        // Creation of duplicate directory/symlink nodes must not shadow the
        // protected manager's ancestors in the VFS namespace.
        if (operation != IME_FILE_READ &&
            (equal_fold(normalized, "/") || equal_fold(normalized, "/apps") || equal_fold(normalized, "/apps/system") ||
             equal_fold(normalized, "/config")))
        {
            free(normalized);
            return false;
        }
        // Protect per-user learning subtrees even before their nodes exist.
        char *user_end = NULL;
        if (normalized[0] == '/')
        {
            char *first = strchr(normalized + 1, '/');
            if (first)
            {
                char saved = *first; *first = 0;
                bool users = equal_fold(normalized, "/users");
                *first = saved;
                if (users)
                {
                    user_end = strchr(first + 1, '/');
                    if (user_end)
                    {
                        char *end = strchr(user_end + 1, '/');
                        char tail = end ? *end : 0;
                        if (end) *end = 0;
                        if (equal_fold(user_end + 1, "ime")) protection |= USER_DATA_NODE;
                        if (end) *end = tail;
                    }
                    if (operation != IME_FILE_READ && !user_end)
                    {
                        vfs_node_t target = vfs_open(normalized);
                        bool directory = target && (target->type & file_dir);
                        if (target) vfs_close(target);
                        if (directory) { free(normalized); return false; }
                    }
                }
            }
            else if (operation != IME_FILE_READ && equal_fold(normalized, "/users"))
            {
                free(normalized);
                return false;
            }
        }
        // Check the resolved parent as well as the spelling of the path. This
        // catches aliases/symlinks for creation and rename destinations.
        char *slash = strrchr(normalized, '/');
        if (slash && slash != normalized)
        {
            *slash            = 0;
            vfs_node_t parent = vfs_open(normalized);
            if (parent)
            {
                protection |= protected_name(parent, slash + 1);
                bool users_parent = parent->parent == rootdir && parent->name && equal_fold(parent->name, "users");
                bool users_root = parent == rootdir && equal_fold(slash + 1, "users");
                bool config_root = parent == rootdir && equal_fold(slash + 1, "config");
                if (operation != IME_FILE_READ && (users_parent || users_root || config_root))
                {
                    *slash = '/';
                    vfs_node_t target = vfs_open(normalized);
                    bool directory = target && (target->type & file_dir);
                    if (target) vfs_close(target);
                    if (directory) { vfs_close(parent); free(normalized); return false; }
                }
                vfs_close(parent);
            }
        }
        free(normalized);
    }
    if (protection & (CONFIG_NODE | USER_DATA_NODE)) return false;
    if ((protection & MANAGER_NODE) && operation != IME_FILE_READ) return false;
    if (operation == IME_FILE_REMOVE && node)
    {
        // Prevent replacement of the protected executable's ancestor directories.
        if (node == rootdir || (node->parent == rootdir && equal_fold(node->name, "users")) ||
            (node->parent == rootdir && equal_fold(node->name, "config")) ||
            ((node->type & file_dir) && node->parent && node->parent->parent == rootdir &&
             equal_fold(node->parent->name, "users")) ||
            (node->parent == rootdir && equal_fold(node->name, "apps")) ||
            (node->parent && node->parent->parent == rootdir && equal_fold(node->parent->name, "apps") &&
             equal_fold(node->name, "system")))
            return false;
    }
    return true;
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

static char *read_config(const char *path)
{
    vfs_node_t node = vfs_open(path);
    if (!node) return NULL;
    size_t size = node->size;
    if (!size || size > CONFIG_LIMIT || (node->type & (file_dir | file_symlink)))
    {
        vfs_close(node);
        return NULL;
    }
    char *text = (char *)malloc(size + 1);
    if (!text)
    {
        vfs_close(node);
        return NULL;
    }
    size_t got = vfs_read(node, text, 0, size);
    vfs_close(node);
    if (got != size)
    {
        free(text);
        return NULL;
    }
    text[size] = 0;
    if (strlen(text) != size)
    {
        free(text);
        return NULL;
    }
    return text;
}

static bool valid_document(const char *text, xapi_JsonDocument *doc)
{
    // Bound recursion before handing data to the existing JSON parser.
    int  depth  = 0;
    bool quoted = false, escaped = false;
    for (const char *p = text; *p; ++p)
    {
        if (quoted)
        {
            if (escaped)
                escaped = false;
            else if (*p == '\\')
                escaped = true;
            else if (*p == '"')
                quoted = false;
        }
        else if (*p == '"')
            quoted = true;
        else if (*p == '{' || *p == '[')
        {
            if (++depth > 24) return false;
        }
        else if (*p == '}' || *p == ']')
        {
            if (--depth < 0) return false;
        }
    }
    if (depth || quoted || xapi_parseJson(text, doc) != XAPI_JSON_PARSE_OK) return false;
    if (!doc->root || doc->root->type != XAPI_JSON_VALUE_OBJECT)
    {
        xapi_freeJsonDocument(doc);
        return false;
    }
    return true;
}

static const char *json_string(const xapi_JsonValue *obj, const char *key)
{
    xapi_JsonValue *v = xapi_jsonObjectGet(obj, key);
    return v && v->type == XAPI_JSON_VALUE_STRING ? v->as.stringValue : NULL;
}

static bool parse_registry(const char *text, ImeRegistry *out)
{
    xapi_JsonDocument doc = {};
    if (!valid_document(text, &doc)) return false;
    memset(out, 0, sizeof(*out));
    out->mode                   = XIME_MODE_CHINESE;
    xapi_JsonValue *ime         = xapi_jsonObjectGet(doc.root, "input_method");
    bool            ok          = true;
    unsigned        occurrences = 0;
    for (xapi_JsonObjectEntry *e = doc.root->as.objectValue; e; e = e->next)
        if (!strcmp(e->key, "input_method")) ++occurrences;
    if (occurrences > 1) ok = false;
    if (ime && ok)
    {
        out->provisioned          = true;
        xapi_JsonValue *version   = xapi_jsonObjectGet(ime, "version");
        xapi_JsonValue *installed = xapi_jsonObjectGet(ime, "installed");
        const char     *active = json_string(ime, "active"), *mode = json_string(ime, "mode");
        if (ime->type != XAPI_JSON_VALUE_OBJECT || !version || version->type != XAPI_JSON_VALUE_NUMBER ||
            version->as.numberValue != 1 || !installed || installed->type != XAPI_JSON_VALUE_ARRAY)
            ok = false;
        if (active && (strlen(active) >= XIME_ID_BYTES || (active[0] && !ime_valid_id(active)))) ok = false;
        if (ok)
        {
            if (active) strcpy(out->active, active);
            if (mode && !strcmp(mode, "en")) out->mode = XIME_MODE_ENGLISH;
            for (xapi_JsonValue *v = installed->as.arrayValue; v; v = v->next)
            {
                const char *id = json_string(v, "id"), *name = json_string(v, "name"),
                           *exe = json_string(v, "executable");
                if (out->count == XIME_MAX_INSTALLED || !id || !name || !exe || !ime_valid_id(id) ||
                    strlen(id) >= XIME_ID_BYTES || strlen(name) >= XIME_NAME_BYTES || !name[0] ||
                    strlen(exe) >= XIME_PATH_BYTES || exe[0] != '/' || !ime_utf8_valid(name, XIME_NAME_BYTES))
                {
                    ok = false;
                    break;
                }
                for (uint32_t i = 0; i < out->count; ++i)
                    if (!strcmp(out->entries[i].id, id)) ok = false;
                if (!ok) break;
                XImeInfo *info = &out->entries[out->count++];
                strcpy(info->id, id);
                strcpy(info->name, name);
                strcpy(info->executable, exe);
            }
        }
    }
    xapi_freeJsonDocument(&doc);
    return ok;
}

struct JsonWriter
{
    char  *text;
    size_t used;
    bool   ok;
    void   append(const char *s, size_t n)
    {
        if (n > CONFIG_LIMIT - used)
        {
            ok = false;
            return;
        }
        memcpy(text + used, s, n);
        used       += n;
        text[used]  = 0;
    }
    void put(const char *s)
    {
        append(s, strlen(s));
    }
    void string(const char *s)
    {
        put("\"");
        while (*s)
        {
            unsigned char c = *s++;
            if (c == '\\' || c == '"')
            {
                char escaped[2] = {'\\', (char)c};
                append(escaped, 2);
            }
            else if (c < 32)
            {
                char esc[7];
                sprintf(esc, "\\u%04x", c);
                put(esc);
            }
            else
            {
                char value = (char)c;
                append(&value, 1);
            }
        }
        put("\"");
    }
};

static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
        ++p;
    return p;
}

static const char *skip_value(const char *p)
{
    int  depth  = 0;
    bool quoted = false, escaped = false;
    for (; *p; ++p)
    {
        if (quoted)
        {
            if (escaped)
                escaped = false;
            else if (*p == '\\')
                escaped = true;
            else if (*p == '"')
                quoted = false;
            continue;
        }
        if (*p == '"')
            quoted = true;
        else if (*p == '{' || *p == '[')
            ++depth;
        else if (*p == '}' || *p == ']')
        {
            if (!depth) return p;
            --depth;
        }
        else if (*p == ',' && !depth)
            return p;
    }
    return p;
}

static bool write_verified(const char *path, const char *text)
{
    size_t     length = strlen(text);
    vfs_node_t node   = vfs_open(path);
    if (!node && vfs_mkfile(path) == 0) node = vfs_open(path);
    if (!node || node->type & (file_dir | file_symlink))
    {
        if (node) vfs_close(node);
        return false;
    }
    bool ok = vfs_resize(node, length) == 0 && vfs_write(node, (void *)text, 0, length) == length;
    vfs_close(node);
    char *check = read_config(path);
    ok          = ok && check && !strcmp(check, text);
    free(check);
    return ok;
}

static void remove_journal(const char *path)
{
    vfs_node_t node = vfs_open(path);
    if (node) vfs_delete(node); // vfs_delete releases the acquired reference.
}

int ime_config_load(ImeRegistry *registry)
{
    ConfigScope scope;
    char       *pending   = read_config(PENDING_PATH);
    ImeRegistry recovered = {};
    if (pending && parse_registry(pending, &recovered))
    {
        if (!write_verified(CONFIG_PATH, pending))
        {
            free(pending);
            return -EIO;
        }
        remove_journal(PENDING_PATH);
    }
    free(pending);
    char *text = read_config(CONFIG_PATH);
    bool  ok   = text && parse_registry(text, registry);
    free(text);
    if (!ok)
    {
        char *backup = read_config(BACKUP_PATH);
        if (backup && parse_registry(backup, registry) && write_verified(CONFIG_PATH, backup)) ok = true;
        free(backup);
    }
    if (!ok)
    {
        vfs_node_t existing = vfs_open(CONFIG_PATH);
        if (existing)
        {
            vfs_close(existing);
            return -EINVAL;
        }
        memset(registry, 0, sizeof(*registry));
        registry->mode = XIME_MODE_CHINESE;
    }
    return 0;
}

int ime_config_save(const ImeRegistry *registry)
{
    ConfigScope scope;
    char       *old = read_config(CONFIG_PATH);
    if (!old) old = strdup("{}");
    xapi_JsonDocument doc = {};
    if (!old || !valid_document(old, &doc))
    {
        free(old);
        return -EINVAL;
    }
    char *buffer = (char *)malloc(CONFIG_LIMIT + 1);
    if (!buffer)
    {
        free(old);
        xapi_freeJsonDocument(&doc);
        return -ENOMEM;
    }
    JsonWriter w = {buffer, 0, true};
    w.put("{");
    const char *p = skip_space(old) + 1;
    // Copy unrelated members byte for byte, preserving numeric precision and
    // application settings unknown to this service.
    for (xapi_JsonObjectEntry *e = doc.root->as.objectValue; e; e = e->next)
    {
        p               = skip_space(p);
        const char *end = skip_value(p);
        if (strcmp(e->key, "input_method"))
        {
            w.append(p, (size_t)(end - p));
            w.put(",");
        }
        p = *end == ',' ? end + 1 : end;
    }
    w.put("\"input_method\":{\"version\":1,\"active\":");
    w.string(registry->active);
    w.put(",\"mode\":");
    w.string(registry->mode == XIME_MODE_CHINESE ? "zh" : "en");
    w.put(",\"installed\":[");
    for (uint32_t i = 0; i < registry->count; ++i)
    {
        if (i) w.put(",");
        const XImeInfo *entry = &registry->entries[i];
        w.put("{\"id\":");
        w.string(entry->id);
        w.put(",\"name\":");
        w.string(entry->name);
        w.put(",\"executable\":");
        w.string(entry->executable);
        w.put(",\"abi_version\":1}");
    }
    w.put("]}}");
    xapi_freeJsonDocument(&doc);
    int result = -EIO;
    // A bounded redo journal is needed because FAT rename is not atomic here.
    // Keep the old document until the replacement has been read back successfully.
    if (!w.ok)
        result = -ENOSPC;
    else if (write_verified(BACKUP_PATH, old) && write_verified(PENDING_PATH, buffer))
    {
        if (write_verified(CONFIG_PATH, buffer))
        {
            remove_journal(PENDING_PATH);
            result = 0;
        }
        else
        {
            write_verified(CONFIG_PATH, old);
            remove_journal(PENDING_PATH);
        }
    }
    if (result) remove_journal(PENDING_PATH);
    free(buffer);
    free(old);
    return result;
}
