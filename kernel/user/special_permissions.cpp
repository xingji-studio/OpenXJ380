#include <user/special_permissions.h>
#include <input_method.h>
#include <fs/ext4/ext4.h>
#include <fs/vfs/vfs.h>
#include <mutex.h>
#include <errno.h>
#include <krlibc.h>
#include <stdio.h>

extern spin_t create_thread_lock;

namespace
{
using Header = SpecialPermissionSettingFileFormat;
using Record = SpecialPermissionAppRecord;
static_assert(sizeof(Header) == 96 && sizeof(Record) == 24, "Special permission disk ABI changed");

struct UserStore
{
    UserStore *next;
    char user[64];
    unsigned char *data;
    size_t bytes;
};

// No filesystem work runs with cache_lock held. Policy callbacks consult only
// this verified cache, so a VFS check cannot recursively enter the filesystem.
spin_t cache_lock = SPIN_INIT;
UserStore *stores;
mutex_t store_lock = {SPIN_INIT, MUTEX_UNLOCKED, NULL, NULL, 0, false};

struct StoreGuard
{
    int error;
    StoreGuard() : error(mutex_lock_recoverable(&store_lock)) {}
    ~StoreGuard() { if (!error) mutex_unlock(&store_lock); }
};

struct StoreScope
{
    tcb_t task;
    StoreScope() : task(get_current_task()) { if (task) ++task->ime_config_depth; }
    ~StoreScope() { if (task) --task->ime_config_depth; }
};

bool component(const char *user)
{
    if (!user || !*user || strlen(user) >= 64 || !strcmp(user, ".") || !strcmp(user, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)user; *p; ++p)
        if (*p < 32 || *p == '/' || *p == '\\') return false;
    return true;
}

bool folded_prefix(const char *text, const char *prefix)
{
    for (; *prefix; ++prefix, ++text)
    {
        unsigned char a = *text, b = *prefix;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (!a || a != b) return false;
    }
    return true;
}

bool beneath(const char *path, const char *base)
{
    size_t length = strlen(base);
    return folded_prefix(path, base) && (!path[length] || path[length] == '/');
}

uint32_t snapshot_checksum(const unsigned char *data, size_t bytes)
{
    uint32_t hash = 2166136261U;
    const size_t offset = offsetof(Header, Checksum);
    for (size_t i = 0; i < bytes; ++i)
        hash = (hash ^ (i >= offset && i < offset + sizeof(uint32_t) ? 0 : data[i])) * 16777619U;
    return hash;
}

size_t record_offset(const unsigned char *data, size_t bytes, const char *path)
{
    for (size_t offset = sizeof(Header); offset < bytes;)
    {
        Record record;
        memcpy(&record, data + offset, sizeof(record));
        if (!strcmp((const char *)data + offset + sizeof(record), path)) return offset;
        offset += record.RecordBytes;
    }
    return bytes;
}

bool valid_snapshot(const unsigned char *data, size_t bytes, const char *user)
{
    if (bytes < sizeof(Header) || bytes > XSP_STORE_LIMIT) return false;
    Header header;
    memcpy(&header, data, sizeof(header));
    if (header.Magic != XSP_STORE_MAGIC || header.Version != XSP_STORE_VERSION || header.Commit != XSP_STORE_COMMITTED ||
        !header.Generation || header.SpAppCount > XSP_MAX_APPS || header.RecordsBytes != bytes - sizeof(Header) ||
        !memchr(header.UserName, 0, sizeof(header.UserName)) || strcmp(header.UserName, user) ||
        header.Checksum != snapshot_checksum(data, bytes)) return false;
    size_t offset = sizeof(Header);
    for (uint32_t i = 0; i < header.SpAppCount; ++i)
    {
        if (bytes - offset < sizeof(Record)) return false;
        Record record;
        memcpy(&record, data + offset, sizeof(record));
        if (record.PathBytes < 2 || record.PathBytes > XSP_APP_PATH_BYTES ||
            record.RecordBytes != sizeof(Record) + record.PathBytes || record.RecordBytes > bytes - offset ||
            !record.Decided || (record.Decided & ~XSP_KNOWN_MASK) || (record.Granted & ~record.Decided)) return false;
        const char *path = (const char *)data + offset + sizeof(record);
        if (path[0] != '/' || path[record.PathBytes - 1] || strlen(path) != record.PathBytes - 1)
            return false;
        // A duplicate identity could otherwise make a refused decision depend
        // on record order. Reject it, including in a correctly checksummed file.
        for (size_t previous = sizeof(Header); previous < offset;)
        {
            Record prior;
            memcpy(&prior, data + previous, sizeof(prior));
            if (!strcmp(path, (const char *)data + previous + sizeof(prior))) return false;
            previous += prior.RecordBytes;
        }
        offset += record.RecordBytes;
    }
    return offset == bytes;
}

UserStore *find_store(const char *user)
{
    for (UserStore *entry = stores; entry; entry = entry->next)
        if (!strcmp(entry->user, user)) return entry;
    return NULL;
}

int decision(const UserStore *entry, const char *path, uint64_t spid)
{
    size_t offset = record_offset(entry->data, entry->bytes, path);
    if (offset == entry->bytes) return 0;
    Record record;
    memcpy(&record, entry->data + offset, sizeof(record));
    uint64_t bit = 1ULL << spid;
    return !(record.Decided & bit) ? 0 : record.Granted & bit ? 2 : 1;
}

bool exact_node(vfs_node_t node, const char *path, bool directory)
{
    if (!node || (node->type & (file_symlink | file_block)) || node->linkto ||
        bool(node->type & file_dir) != directory) return false;
    char *canonical = vfs_get_fullpath(node);
    bool match = canonical && !strcmp(canonical, path);
    free(canonical);
    return match;
}

bool missing_path()
{
    // Legacy FAT lookup can report an absent node with no saved errno.
    // Explicit policy/allocation/I/O errors still fail closed.
    int error = permissions_file_error();
    return !error || error == ENOENT;
}

int directories(const char *user, bool create)
{
    char path[160];
    snprintf(path, sizeof(path), "/system/config/user/%s", user);
    for (char *p = path + 1;; ++p)
    {
        if (*p && *p != '/') continue;
        char saved = *p;
        *p = 0;
        vfs_node_t node = vfs_open_no_follow(path);
        if (!node && create && missing_path() && !vfs_mkdir(path))
            node = vfs_open_no_follow(path);
        if (!node) return missing_path() ? -ENOENT : -EIO;
        bool valid = exact_node(node, path, true);
        int closed = vfs_close(node);
        if (!valid || closed) return -EIO;
        *p = saved;
        if (!saved) return 0;
    }
}

unsigned char *read_snapshot(const char *path, const char *user, size_t *bytes, int *error)
{
    *error = 0;
    vfs_node_t node = vfs_open_no_follow(path);
    if (!node)
    {
        *error = missing_path() ? -ENOENT : -EIO;
        return NULL;
    }
    *bytes = node->size;
    bool valid = exact_node(node, path, false) && *bytes >= sizeof(Header) && *bytes <= XSP_STORE_LIMIT;
    unsigned char *data = valid ? (unsigned char *)malloc(*bytes) : NULL;
    if (!data) *error = valid ? -ENOMEM : -EIO;
    if (data && (vfs_read(node, data, 0, *bytes) != *bytes || !valid_snapshot(data, *bytes, user))) *error = -EIO;
    if (vfs_close(node)) *error = -EIO;
    if (*error) { free(data); data = NULL; }
    return data;
}

// Caller holds store_lock. New cache nodes are immutable after publication;
// only the snapshot pointer/length are replaced under cache_lock.
int load_store(const char *user, UserStore **out)
{
    if (!component(user)) return -EINVAL;
    spin_lock(&cache_lock);
    *out = find_store(user);
    spin_unlock(&cache_lock);
    if (*out) return 0;
    StoreScope scope;
    int error = directories(user, false);
    if (error && error != -ENOENT) return error;
    char paths[2][192];
    snprintf(paths[0], sizeof(paths[0]), "/system/config/user/%s/permission.dat", user);
    snprintf(paths[1], sizeof(paths[1]), "/system/config/user/%.63s/permission.dat.backup", user);
    unsigned char *slots[2] = {};
    size_t bytes[2] = {};
    int errors[2] = {-ENOENT, -ENOENT};
    if (!error)
        for (unsigned i = 0; i < 2; ++i) slots[i] = read_snapshot(paths[i], user, &bytes[i], &errors[i]);
    int selected = slots[0] ? 0 : slots[1] ? 1 : -1;
    if (slots[0] && slots[1])
    {
        Header a, b;
        memcpy(&a, slots[0], sizeof(a));
        memcpy(&b, slots[1], sizeof(b));
        if (b.Generation > a.Generation) selected = 1;
    }
    if (selected < 0 && (errors[0] != -ENOENT || errors[1] != -ENOENT))
        return errors[0] == -ENOMEM || errors[1] == -ENOMEM ? -ENOMEM : -EIO;
    UserStore *entry = (UserStore *)calloc(1, sizeof(UserStore));
    if (!entry) { free(slots[0]); free(slots[1]); return -ENOMEM; }
    strcpy(entry->user, user);
    if (selected >= 0)
    {
        entry->data = slots[selected];
        entry->bytes = bytes[selected];
        free(slots[selected ^ 1]);
    }
    else
    {
        entry->bytes = sizeof(Header);
        entry->data = (unsigned char *)calloc(1, entry->bytes);
        if (!entry->data) { free(entry); return -ENOMEM; }
        Header header = {};
        header.Magic = XSP_STORE_MAGIC;
        header.Version = XSP_STORE_VERSION;
        header.Commit = XSP_STORE_COMMITTED;
        header.Generation = 1;
        strcpy(header.UserName, user);
        memcpy(entry->data, &header, sizeof(header));
        header.Checksum = snapshot_checksum(entry->data, entry->bytes);
        memcpy(entry->data, &header, sizeof(header));
    }
    spin_lock(&cache_lock);
    entry->next = stores;
    stores = entry;
    spin_unlock(&cache_lock);
    *out = entry;
    return 0;
}

bool write_snapshot(const char *path, const char *user, const unsigned char *data, size_t bytes)
{
    vfs_node_t node = vfs_open_no_follow(path);
    if (!node && missing_path() && !vfs_mkfile(path)) node = vfs_open_no_follow(path);
    if (!node) return false;
    bool valid = exact_node(node, path, false);
    Header pending;
    memcpy(&pending, data, sizeof(pending));
    pending.Commit = 0;
    uint32_t commit = XSP_STORE_COMMITTED, invalid = 0;
    if (valid) valid = !vfs_resize(node, bytes);
    // Invalidate even an existing file before changing any snapshot bytes.
    // The final four-byte marker has no zero bytes, so a short write of it
    // cannot accidentally equal a valid marker in the zeroed field.
    if (valid) valid = vfs_write(node, &invalid, offsetof(Header, Commit), sizeof(invalid)) == sizeof(invalid);
    if (valid) valid = vfs_write(node, &pending, 0, sizeof(pending)) == sizeof(pending);
    if (valid && bytes > sizeof(Header))
        valid = vfs_write(node, (void *)(data + sizeof(Header)), sizeof(Header), bytes - sizeof(Header)) ==
                bytes - sizeof(Header);
    if (valid) valid = vfs_write(node, &commit, offsetof(Header, Commit), sizeof(commit)) == sizeof(commit);
    if (valid && ext4fs_owns_node(node)) valid = !ext4fs_sync(node);
    if (vfs_close(node)) valid = false;
    size_t checked_bytes = 0;
    int error;
    unsigned char *checked = valid ? read_snapshot(path, user, &checked_bytes, &error) : NULL;
    valid = valid && checked && checked_bytes == bytes && !memcmp(checked, data, bytes);
    free(checked);
    return valid;
}
}

int special_permissions_load_user(const char *user)
{
    StoreGuard guard;
    if (guard.error) return guard.error;
    UserStore *entry;
    return load_store(user, &entry);
}

int special_permissions_cached(const char *user, const char *executable, uint64_t spid)
{
    if (!user || !executable || spid < XSPT_ReadPicture || spid > XSPT_DeletePicture) return 0;
    spin_lock(&cache_lock);
    UserStore *entry = find_store(user);
    int result = entry ? decision(entry, executable, spid) : 0;
    spin_unlock(&cache_lock);
    return result;
}

int special_permissions_get(const char *user, const char *executable, uint64_t spid)
{
    if (!executable || executable[0] != '/' || strlen(executable) >= XSP_APP_PATH_BYTES ||
        spid < XSPT_ReadPicture || spid > XSPT_DeletePicture) return -EINVAL;
    int error = special_permissions_load_user(user);
    return error ? error : special_permissions_cached(user, executable, spid);
}

int special_permissions_save(const char *user, const char *executable, uint64_t spid, bool approved)
{
    if (!executable || executable[0] != '/' || strlen(executable) >= XSP_APP_PATH_BYTES ||
        spid < XSPT_ReadPicture || spid > XSPT_DeletePicture) return -EINVAL;
    StoreGuard guard;
    if (guard.error) return guard.error;
    UserStore *entry;
    int error = load_store(user, &entry);
    if (error) return error;
    int existing = decision(entry, executable, spid);
    if (existing) return existing == 2 ? 0 : -EACCES;
    Header header;
    memcpy(&header, entry->data, sizeof(header));
    size_t offset = record_offset(entry->data, entry->bytes, executable);
    bool append = offset == entry->bytes;
    if ((append && header.SpAppCount == XSP_MAX_APPS) || header.Generation == ~0ULL) return -ENOSPC;
    Record record = {};
    if (append)
    {
        record.PathBytes = strlen(executable) + 1;
        record.RecordBytes = sizeof(record) + record.PathBytes;
        ++header.SpAppCount;
    }
    else memcpy(&record, entry->data + offset, sizeof(record));
    size_t bytes = entry->bytes + (append ? record.RecordBytes : 0);
    if (bytes > XSP_STORE_LIMIT) return -ENOSPC;
    unsigned char *data = (unsigned char *)malloc(bytes);
    if (!data) return -ENOMEM;
    memcpy(data, entry->data, entry->bytes);
    record.Decided |= 1ULL << spid;
    if (approved) record.Granted |= 1ULL << spid;
    memcpy(data + offset, &record, sizeof(record));
    if (append) memcpy(data + offset + sizeof(record), executable, record.PathBytes);
    ++header.Generation;
    header.RecordsBytes = bytes - sizeof(header);
    header.Checksum = 0;
    memcpy(data, &header, sizeof(header));
    header.Checksum = snapshot_checksum(data, bytes);
    memcpy(data, &header, sizeof(header));
    StoreScope scope;
    char path[192], backup[192];
    snprintf(path, sizeof(path), "/system/config/user/%s/permission.dat", user);
    snprintf(backup, sizeof(backup), "/system/config/user/%.63s/permission.dat.backup", user);
    error = directories(user, true);
    // Verify the previous snapshot in a separate file before touching the
    // primary. A failed/short primary write cannot turn refusal into approval.
    if (!error && (!write_snapshot(backup, user, entry->data, entry->bytes) ||
                   !write_snapshot(path, user, data, bytes))) error = -EIO;
    if (!error)
    {
        spin_lock(&cache_lock);
        unsigned char *previous = entry->data;
        entry->data = data;
        entry->bytes = bytes;
        spin_unlock(&cache_lock);
        free(previous);
    }
    else free(data);
    return error ? error : approved ? 0 : -EACCES;
}

void special_permissions_store_exit(pcb_t process)
{
    mutex_abort_for_process(&store_lock, process);
}

bool special_permissions_store_path(const char *path, bool ancestor)
{
    const char *base = "/system/config/user";
    return beneath(path, base) || (ancestor && beneath(base, path));
}

uint64_t special_permissions_path_mask(const char *path, int operation, const char *user)
{
    if (!path || !folded_prefix(path, "/users/")) return 0;
    const char *owner = path + 7, *slash = strchr(owner, '/');
    if (!slash || !beneath(slash, "/picture")) return 0;
    // A different user's album is never authorized by this user's grants.
    if (!user || strlen(user) != size_t(slash - owner) || strncmp(owner, user, slash - owner)) return ~0ULL;
    switch (operation)
    {
    case IME_FILE_READ:
    case IME_FILE_LIST:
    case IME_FILE_STAT:
    case IME_FILE_EXPORT: return 1ULL << XSPT_ReadPicture;
    case IME_FILE_WRITE: return 1ULL << XSPT_WritePicture;
    case IME_FILE_REMOVE: return 1ULL << XSPT_DeletePicture;
    case IME_FILE_READ_WRITE: return (1ULL << XSPT_ReadPicture) | (1ULL << XSPT_WritePicture);
    default: return 0;
    }
}

int special_permissions_access(pcb_t process, const char *user, const char *path, int operation)
{
    if (special_permissions_store_path(path, operation == IME_FILE_REMOVE)) return -EACCES;
    uint64_t required = special_permissions_path_mask(path, operation, user);
    if (!required) return 0;
    if (required == ~0ULL || !process) return -EACCES;
    char executable[XSP_APP_PATH_BYTES];
    spin_lock(&create_thread_lock);
    const char *source = process->exe_path;
    size_t length = source ? strlen(source) : 0;
    if (length && length < sizeof(executable)) memcpy(executable, source, length + 1);
    spin_unlock(&create_thread_lock);
    if (!length || length >= sizeof(executable)) return -EACCES;
    for (uint64_t id = XSPT_ReadPicture; id <= XSPT_DeletePicture; ++id)
        if ((required & (1ULL << id)) && special_permissions_cached(user, executable, id) != 2) return -EACCES;
    return 1;
}
