#include <user/environment.h>
#include <user/permissions.h>
#include <user/user.h>
#include <cpu/lock.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <stdio.h>
#include <task/pcb.h>
#include <task/scheduler.h>
#include <xapi_json.h>
#include "../build_settings.h"

namespace
{
struct Snapshot
{
    EnvironmentTable table;
    unsigned references;
    uint64_t hash;
};

struct AccountEnvironment
{
    char username[64];
    Snapshot *snapshot;
    uint64_t checked_at, published_hash, published_generation;
    uint64_t publish_attempted_at, publish_attempted_hash;
    unsigned interval;
};

// Entries never move; only immutable, reference-counted snapshots are replaced.
// No VFS, allocation or reclamation takes place under this publication lock.
spin_t environment_lock = SPIN_INIT;
AccountEnvironment accounts[130];
size_t account_count;
unsigned worker_started;

bool valid_username(const char *username)
{
    return username && username[0] && strlen(username) < 64 && !strchr(username, '/') &&
           !strchr(username, '\\') && strcmp(username, ".") && strcmp(username, "..");
}

void release(Snapshot *snapshot)
{
    if (snapshot && __atomic_sub_fetch(&snapshot->references, 1U, __ATOMIC_ACQ_REL) == 0)
    {
        environment_free(snapshot->table.values);
        free(snapshot);
    }
}

AccountEnvironment *account_for(const char *username)
{
    if (!valid_username(username)) return NULL;
    spin_lock(&environment_lock);
    size_t i = 0;
    for (; i < account_count; ++i) if (!strcmp(accounts[i].username, username)) break;
    if (i == account_count && account_count < 130)
    {
        strcpy(accounts[i].username, username);
        accounts[i].interval = ENVIRONMENT_SYNC_DEFAULT;
        ++account_count;
    }
    AccountEnvironment *result = i < account_count ? &accounts[i] : NULL;
    spin_unlock(&environment_lock);
    return result;
}

Snapshot *acquire(AccountEnvironment *account)
{
    spin_lock(&environment_lock);
    Snapshot *result = account->snapshot;
    if (result) __atomic_add_fetch(&result->references, 1U, __ATOMIC_RELAXED);
    spin_unlock(&environment_lock);
    return result;
}

char *read_text(const char *path, size_t *length, int *error)
{
    *length = 0;
    *error = 0;
    vfs_node_t node = vfs_open_no_follow(path);
    if (!node)
    {
        int reason = permissions_file_error();
        if (reason && reason != ENOENT) *error = -reason;
        return NULL;
    }
    if ((node->type & ~file_none) || node->linkto) *error = -EINVAL;
    else if (node->size > ENVIRONMENT_FILE_MAX + 1024) *error = -E2BIG;
    char *text = *error ? NULL : (char *)malloc(node->size + 1);
    if (!*error && !text) *error = -ENOMEM;
    if (!*error)
    {
        *length = node->size;
        if (vfs_read(node, text, 0, *length) != *length) *error = -EIO;
        else text[*length] = 0;
    }
    vfs_close(node);
    if (*error) { free(text); return NULL; }
    return text;
}

int apply_file(EnvironmentTable *table, const char *path)
{
    size_t length, line = 0;
    int error;
    char *text = read_text(path, &length, &error);
    if (text)
    {
        error = environment_parse(table, text, length, &line);
        free(text);
    }
    if (error) write_serial_fmt("Environment: cannot load %s line=%llu error=%d; keeping previous values\n",
                                path, (unsigned long long)line, error);
    return error;
}

uint64_t table_hash(const EnvironmentTable &table)
{
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < table.count; ++i)
    {
        for (const unsigned char *p = (unsigned char *)table.values[i]; *p; ++p)
            hash = (hash ^ *p) * 1099511628211ULL;
        hash = (hash ^ '\n') * 1099511628211ULL;
    }
    return hash ? hash : 1;
}

int refresh(AccountEnvironment *account)
{
    Snapshot *previous = acquire(account);
    Snapshot *next = (Snapshot *)calloc(1, sizeof(Snapshot));
    if (!next) { release(previous); return -ENOMEM; }
    next->references = 1;
    int error = environment_set(&next->table, ENVP_SYSTEM_VERSION);
    if (!error) error = environment_set(&next->table,
        "PATH=/usr/local/sbin:/usr/sbin:/sbin:/usr/bin:/bin:/apps:/system");
    if (!error && scheduler_is_enabled())
    {
        PermissionProfileScope scope(account->username);
        error = apply_file(&next->table, "/system/system.env");
        char path[128];
        snprintf(path, sizeof(path), "/users/%s/user.env", account->username);
        if (!error) error = apply_file(&next->table, path);
    }
    if (error)
    {
        release(next);
        release(previous);
        return error;
    }
    next->hash = table_hash(next->table);
    size_t next_count = next->table.count;
    spin_lock(&environment_lock);
    bool replace = account->snapshot == previous && (!previous || previous->hash != next->hash);
    if (replace) account->snapshot = next;
    spin_unlock(&environment_lock);
    if (replace) write_serial_fmt("Environment: loaded account=%s variables=%llu\n", account->username,
                                   (unsigned long long)next_count);
    if (replace) release(previous); // Drop the cache's old ownership as well.
    else release(next);
    release(previous);
    return 0;
}

bool write_new_file(const char *path, const char *text, size_t length)
{
    vfs_node_t node = vfs_open_no_follow(path);
    if (!node && vfs_mkfile(path) == EOK) node = vfs_open_no_follow(path);
    if (!node) return false;
    bool ok = !(node->type & ~file_none) && !node->linkto && !vfs_resize(node, 0) &&
              vfs_write(node, (void *)text, 0, length) == length;
    vfs_close(node);
    return ok;
}

bool install_hook(const char *username, const char *leaf)
{
    char path[160];
    snprintf(path, sizeof(path), "/users/%s/%s", username, leaf);
    size_t length;
    int error;
    char *old = read_text(path, &length, &error);
    const char *hook = "\n# XJ380 environment bridge v1\n"
        "if [ -n \"${BASH_VERSION-}\" ] && [ -r /system/resources/environment/bash.sh ]; then\n"
        "    . /system/resources/environment/bash.sh\nfi\n";
    if (error || (old && strstr(old, hook))) { free(old); return !error; }
    // Append only our hook. Existing shell configuration bytes stay untouched,
    // including on filesystems whose rename cannot replace an existing file.
    size_t added = strlen(hook);
    vfs_node_t node = vfs_open_no_follow(path);
    if (!node && vfs_mkfile(path) == EOK) node = vfs_open_no_follow(path);
    bool ok = node && !(node->type & ~file_none) && !node->linkto &&
              node->size == length && vfs_write(node, (void *)hook, length, added) == added;
    if (node) vfs_close(node);
    free(old);
    return ok;
}

bool publish_linux(AccountEnvironment *account, Snapshot *snapshot)
{
    char home[128];
    snprintf(home, sizeof(home), "/users/%s", account->username);
    vfs_node_t directory = vfs_open_no_follow(home);
    if (!directory) return false;
    bool exists = (directory->type & file_dir) && !(directory->type & file_symlink);
    vfs_close(directory);
    if (!exists) return false;
    // Two data slots provide a complete previous version on FAT as well as
    // ext4. The reader accepts matching header/footer generations from one
    // captured read; environment values are data, never sourced shell code.
    if (!account->published_generation)
    {
        for (unsigned slot = 0; slot < 2; ++slot)
        {
            char path[160];
            snprintf(path, sizeof(path), "%s/.xj380-env.%u", home, slot);
            size_t length;
            int error;
            char *old = read_text(path, &length, &error);
            const char *prefix = "# XJ380 environment 1 ";
            if (old && !strncmp(old, prefix, strlen(prefix)))
            {
                const char *p = old + strlen(prefix);
                uint64_t generation = 0;
                while (*p >= '0' && *p <= '9' && generation < 100000000000000000ULL)
                    generation = generation * 10 + (*p++ - '0');
                char footer[64];
                snprintf(footer, sizeof(footer), "# end %llu\n", (unsigned long long)generation);
                size_t footer_size = strlen(footer);
                if (*p == '\n' && length >= footer_size &&
                    !memcmp(old + length - footer_size, footer, footer_size) &&
                    generation > account->published_generation)
                    account->published_generation = generation;
            }
            free(old);
        }
    }
    uint64_t generation = account->published_generation + 1;
    if (generation >= 1000000000000000000ULL) return false;
    size_t capacity = snapshot->table.bytes + 256;
    char *text = (char *)malloc(capacity);
    if (!text) return false;
    size_t position = (size_t)snprintf(text, capacity, "# XJ380 environment 1 %llu\n",
                                      (unsigned long long)generation);
    for (size_t i = 0; i < snapshot->table.count; ++i)
    {
        const char *entry = snapshot->table.values[i];
        size_t length = strlen(entry);
        memcpy(text + position, entry, length);
        position += length;
        text[position++] = '\n';
    }
    position += (size_t)snprintf(text + position, capacity - position, "# end %llu\n",
                                 (unsigned long long)generation);
    char path[160];
    snprintf(path, sizeof(path), "%s/.xj380-env.%u", home, (unsigned)(generation & 1));
    bool ok = write_new_file(path, text, position);
    free(text);
    if (ok) account->published_generation = generation;
    return ok && install_hook(account->username, ".bashrc") && install_hook(account->username, ".profile");
}

unsigned read_interval(const char *username)
{
    char path[128];
    snprintf(path, sizeof(path), "/users/%s/settings.json", username);
    size_t length;
    int error;
    char *text = read_text(path, &length, &error);
    unsigned interval = ENVIRONMENT_SYNC_DEFAULT;
    if (text)
    {
        XJsonDocument document = {};
        if (xapi_parseJson(text, &document) == XAPI_JSON_PARSE_OK)
        {
            XJsonValue *value = xapi_jsonObjectGet(document.root, "linux_env_sync_interval_seconds");
            if (value && value->type == XAPI_JSON_VALUE_NUMBER &&
                value->as.numberValue >= ENVIRONMENT_SYNC_MIN && value->as.numberValue <= ENVIRONMENT_SYNC_MAX &&
                (double)(unsigned)value->as.numberValue == value->as.numberValue)
                interval = (unsigned)value->as.numberValue;
        }
        xapi_freeJsonDocument(&document);
        free(text);
    }
    return interval;
}

void environment_worker(void *)
{
    while (true)
    {
        char username[64];
        if (permissions_session_name(username, sizeof(username))) (void)account_for(username);
        size_t count;
        spin_lock(&environment_lock);
        count = account_count;
        spin_unlock(&environment_lock);
        for (size_t i = 0; i < count; ++i)
        {
            AccountEnvironment *account = &accounts[i];
            unsigned interval = read_interval(account->username);
            uint64_t now = nanoTime();
            if (interval != account->interval || !account->checked_at ||
                now - account->checked_at >= (uint64_t)interval * 1000000000ULL)
            {
                account->interval = interval;
                account->checked_at = now;
                (void)refresh(account);
            }
            Snapshot *snapshot = acquire(account);
            if (snapshot && account->published_hash != snapshot->hash &&
                (account->publish_attempted_hash != snapshot->hash ||
                 now - account->publish_attempted_at >= (uint64_t)interval * 1000000000ULL))
            {
                account->publish_attempted_at = now;
                account->publish_attempted_hash = snapshot->hash;
                if (publish_linux(account, snapshot)) account->published_hash = snapshot->hash;
            }
            release(snapshot);
        }
        scheduler_sleep_ns(1000000000ULL);
    }
}
} // namespace

char **environment_for_user(const char *username, size_t *count)
{
    AccountEnvironment *account = account_for(username);
    if (!account) return NULL;
    Snapshot *snapshot = acquire(account);
    if (!snapshot)
    {
        (void)refresh(account);
        snapshot = acquire(account);
    }
    if (!snapshot)
    {
        // Bad files must not prevent login; subsequent checks can repair them.
        char *fallback[] = {(char *)ENVP_SYSTEM_VERSION,
            (char *)"PATH=/usr/local/sbin:/usr/sbin:/sbin:/usr/bin:/bin:/apps:/system", NULL};
        return environment_clone(fallback, count);
    }
    char **result = environment_clone(snapshot->table.values, count);
    release(snapshot);
    return result;
}

int environment_reload(const char *username)
{
    AccountEnvironment *account = account_for(username);
    return account ? refresh(account) : -EINVAL;
}

bool environment_init_profile(const char *username)
{
    if (!valid_username(username)) return false;
    char path[128];
    snprintf(path, sizeof(path), "/users/%s/user.env", username);
    vfs_node_t existing = vfs_open_no_follow(path);
    if (existing)
    {
        bool ok = !(existing->type & (file_dir | file_symlink));
        vfs_close(existing);
        return ok;
    }
    const char *seed = "# Account environment. NAME=VALUE; overrides /system/system.env.\n";
    return write_new_file(path, seed, strlen(seed));
}

void environment_start_worker()
{
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&worker_started, &expected, 1U, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;
    if ((int64_t)create_kernel_thread((void *)environment_worker, NULL, (char *)"environment", NULL) < 0)
        __atomic_store_n(&worker_started, 0U, __ATOMIC_RELEASE);
}
