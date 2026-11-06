// SPDX-License-Identifier: Apache-2.0
#include <user/auth.h>
#include <user/auth_crypto.h>
#include <user/auth_store.h>
#include <user/permissions.h>
#include <fs/ext4/ext4.h>
#include <fs/vfs/vfs.h>
#include <input_method.h>
#include <mutex.h>
#include <errno.h>
#include <krlibc.h>
#include <proto.hpp>

extern void user_auth_session_started() __attribute__((weak));

namespace
{
const char *const primary_path = "/system/config/usereg.dat";
const char *const pending_path = "/system/config/usereg.dat.pending";
const char *const commit_path = "/system/config/usereg.dat.commit";
const uint64_t challenge_lifetime = 60000000000ULL;
// Recursive only because sealed operations call the existing kernel login
// helpers, which in turn enter this same serialized registry service.
mutex_t service_mutex = {SPIN_INIT, MUTEX_UNLOCKED, NULL, NULL, 0, true};
uint64_t next_challenge = 1, retry_after;
unsigned failed_attempts;
void reset_abandoned_service();

struct Guard
{
    int error;
    Guard()
    {
        bool recovered = false;
        error = mutex_lock_recoverable(&service_mutex, &recovered);
        if (!error && recovered) reset_abandoned_service();
    }
    ~Guard() { if (!error) mutex_unlock(&service_mutex); }
};

struct StorageScope
{
    tcb_t task;
    StorageScope() : task(get_current_task()) { if (task) ++task->ime_config_depth; }
    ~StorageScope() { if (task) --task->ime_config_depth; }
};

struct Erase
{
    void *data;
    size_t bytes;
    ~Erase() { auth_erase(data, bytes); }
};

struct Database
{
    AuthDatabase *data;
    Database() : data((AuthDatabase *)calloc(1, sizeof(AuthDatabase))) {}
    ~Database() { if (data) { auth_erase(data, sizeof(*data)); free(data); } }
};

struct SessionSlot
{
    uint64_t pid, session, id, expires, revision;
    uint32_t operation;
    bool challenge, authorized;
    char username[64];
    uint8_t nonce[32], grant[32];
};
SessionSlot slots[32];

void reset_abandoned_service()
{
    auth_forget_abandoned_contexts();
    auth_erase(slots, sizeof(slots));
}

bool valid_name(const char *name)
{
    if (!name || !name[0] || !memchr(name, 0, 64)) return false;
    for (const char *p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == ' ')) return false;
    return true;
}

bool password_valid(const char *password, bool allow_empty = false)
{
    return password && (allow_empty || *password) && memchr(password, 0, XAUTH_PASSWORD_BYTES);
}

uint32_t checksum(const void *data, size_t bytes, size_t skipped)
{
    const uint8_t *source = (const uint8_t *)data;
    uint32_t value = 2166136261U;
    for (size_t i = 0; i < bytes; ++i)
        value = (value ^ (i >= skipped && i < skipped + 4 ? 0 : source[i])) * 16777619U;
    return value;
}

size_t database_bytes(const AuthDatabase *db)
{
    return sizeof(AuthStoreHeader) + db->header.count * sizeof(AuthAccountRecord);
}

bool valid_database(const AuthDatabase *db, size_t bytes)
{
    const AuthStoreHeader &h = db->header;
    if (bytes < sizeof(h) || h.magic != AUTH_STORE_MAGIC || h.version != 2 ||
        h.count > AUTH_MAX_ACCOUNTS || h.reserved || !h.generation || h.commit != AUTH_STORE_COMMITTED ||
        bytes != database_bytes(db) || h.checksum != checksum(db, bytes, offsetof(AuthStoreHeader, checksum)))
        return false;
    for (uint32_t i = 0; i < h.count; ++i)
    {
        const AuthAccountRecord &a = db->accounts[i];
        if (!valid_name(a.name) || a.role < XUT_Admin || a.role > XUT_Custom || a.permissions > XPERM_ALL ||
            a.iterations < AUTH_PASSWORD_ITERATIONS || a.iterations > 10000000U || !a.revision || a.reserved)
            return false;
        for (uint32_t j = 0; j < i; ++j)
            if (!strcmp(a.name, db->accounts[j].name)) return false;
    }
    return true;
}

bool exact_node(vfs_node_t node, const char *path, bool directory = false)
{
    if (!node || node->linkto || (node->type & (file_symlink | file_block)) ||
        bool(node->type & file_dir) != directory) return false;
    char *canonical = vfs_get_fullpath(node);
    bool valid = canonical && !strcmp(canonical, path);
    free(canonical);
    return valid;
}

int missing_error()
{
    int error = permissions_file_error();
    return !error || error == ENOENT ? -ENOENT : -EIO;
}

int read_file(const char *path, void *data, size_t capacity, size_t *bytes)
{
    *bytes = 0;
    vfs_node_t node = vfs_open_no_follow(path);
    if (!node) return missing_error();
    bool valid = exact_node(node, path) && node->size <= capacity;
    if (valid)
    {
        *bytes = node->size;
        valid = vfs_read(node, data, 0, *bytes) == *bytes;
    }
    if (vfs_close(node)) valid = false;
    return valid ? 0 : -EIO;
}

bool write_file(const char *path, const void *data, size_t bytes)
{
    vfs_node_t node = vfs_open_no_follow(path);
    if (!node && missing_error() == -ENOENT && !vfs_mkfile(path)) node = vfs_open_no_follow(path);
    if (!node) return false;
    bool valid = exact_node(node, path);
    // Clear the previous logical file before shrinking, including the legacy
    // plaintext tail. This is not a physical secure-erase guarantee for disks.
    uint8_t zero[512] = {};
    for (size_t offset = 0; valid && offset < node->size; offset += sizeof(zero))
    {
        size_t count = node->size - offset;
        if (count > sizeof(zero)) count = sizeof(zero);
        valid = vfs_write(node, zero, offset, count) == count;
    }
    if (valid) valid = !vfs_resize(node, bytes);
    if (valid) valid = vfs_write(node, (void *)data, 0, bytes) == bytes;
    if (valid && ext4fs_owns_node(node)) valid = !ext4fs_sync(node);
    if (vfs_close(node)) valid = false;
    void *checked = valid ? malloc(bytes) : NULL;
    size_t checked_bytes = 0;
    if (valid) valid = checked && !read_file(path, checked, bytes, &checked_bytes) &&
                       checked_bytes == bytes && auth_equal(checked, data, bytes);
    if (checked) { auth_erase(checked, bytes); free(checked); }
    return valid;
}

int ensure_directories()
{
    const char *paths[] = {"/system", "/system/config"};
    for (const char *path : paths)
    {
        vfs_node_t node = vfs_open_no_follow(path);
        if (!node && missing_error() == -ENOENT && !vfs_mkdir(path)) node = vfs_open_no_follow(path);
        if (!node) return -EIO;
        bool valid = exact_node(node, path, true);
        if (vfs_close(node)) valid = false;
        if (!valid) return -EIO;
    }
    return 0;
}

void prepare_database(AuthDatabase *db)
{
    db->header.magic = AUTH_STORE_MAGIC;
    db->header.version = 2;
    db->header.commit = AUTH_STORE_COMMITTED;
    db->header.reserved = 0;
    db->header.checksum = checksum(db, database_bytes(db), offsetof(AuthStoreHeader, checksum));
}

// pending is staged first. The small commit record selects its exact generation
// and checksum. Recovery NEVER falls back to an older password after commit.
int save_database(AuthDatabase *db)
{
    if (ensure_directories()) return -EIO;
    prepare_database(db);
    size_t bytes = database_bytes(db);
    if (!write_file(pending_path, db, bytes)) return -EIO;
    AuthCommitRecord commit = {AUTH_STORE_COMMIT_MAGIC, 2, db->header.generation, db->header.checksum, 0};
    commit.checksum = checksum(&commit, sizeof(commit), offsetof(AuthCommitRecord, checksum));
    if (!write_file(commit_path, &commit, sizeof(commit))) return -EIO;
    // Both the committed journal and publication must be readable before UI
    // success. If publication fails, the next read repairs it from the journal.
    return write_file(primary_path, db, bytes) ? 0 : -EIO;
}

bool legacy_valid(const UserRegisterList *legacy)
{
    if (legacy->user_count < 1 || legacy->user_count > 128) return false;
    for (int i = 0; i < legacy->user_count; ++i)
    {
        const UserInfo &u = legacy->uinf[i];
        uint32_t role;
        memcpy(&role, &u.user_type, sizeof(role));
        const uint8_t *permissions = (const uint8_t *)&u.user_prms;
        for (size_t j = 0; j < sizeof(u.user_prms); ++j)
            if (permissions[j] > 1) return false;
        if (!valid_name(u.name) || !memchr(u.password, 0, sizeof(u.password)) || role > XUT_Custom) return false;
    }
    return true;
}

int load_database(AuthDatabase *db)
{
    StorageScope storage;
    AuthCommitRecord commit = {};
    size_t bytes = 0;
    int committed = read_file(commit_path, &commit, sizeof(commit), &bytes);
    if (!committed)
    {
        if (bytes != sizeof(commit) || commit.magic != AUTH_STORE_COMMIT_MAGIC || commit.version != 2 ||
            !commit.generation || commit.checksum != checksum(&commit, sizeof(commit),
                                                               offsetof(AuthCommitRecord, checksum))) return -EIO;
        int error = read_file(primary_path, db, sizeof(*db), &bytes);
        if (!error && valid_database(db, bytes) && db->header.generation == commit.generation &&
            db->header.checksum == commit.snapshot_checksum) return 0;
        memset(db, 0, sizeof(*db));
        error = read_file(pending_path, db, sizeof(*db), &bytes);
        if (error || !valid_database(db, bytes) || db->header.generation != commit.generation ||
            db->header.checksum != commit.snapshot_checksum) return -EIO;
        return write_file(primary_path, db, bytes) ? 0 : -EIO;
    }
    if (committed != -ENOENT) return committed;

    // A missing commit marker is only valid for a new machine or the exact
    // legacy ABI. A v2 database with a lost marker must not enable OOBE.
    UserRegisterList *legacy = (UserRegisterList *)calloc(1, sizeof(UserRegisterList));
    if (!legacy) return -ENOMEM;
    Erase erase = {legacy, sizeof(*legacy)};
    int error = read_file(primary_path, legacy, sizeof(*legacy), &bytes);
    if (error == -ENOENT)
    {
        size_t staged_bytes = 0;
        int staged = read_file(pending_path, db, sizeof(*db), &staged_bytes);
        memset(db, 0, sizeof(*db));
        // An interrupted first creation can be retried through OOBE, but an
        // existing journal without its marker requires explicit recovery.
        error = staged == -ENOENT ? 0 : -EIO;
    }
    else if (!error)
    {
        if (bytes != sizeof(*legacy) || !legacy_valid(legacy)) error = -EIO;
        else
        {
            memset(db, 0, sizeof(*db));
            db->header.generation = 1;
            for (int i = 0; !error && i < legacy->user_count; ++i)
            {
                const UserInfo &u = legacy->uinf[i];
                if (u.user_type == XUT_Root || u.user_type == XUT_System) continue;
                AuthAccountRecord &a = db->accounts[db->header.count++];
                strcpy(a.name, u.name);
                a.role = u.user_type;
                a.permissions = user_permission_mask(u.user_prms);
                a.iterations = AUTH_PASSWORD_ITERATIONS;
                a.revision = 1;
                error = auth_random(a.salt, sizeof(a.salt));
                if (!error) error = auth_password_hash(u.password, a.salt, a.iterations, a.hash);
            }
            if (!error)
            {
                prepare_database(db);
                error = valid_database(db, database_bytes(db)) ? save_database(db) : -EIO;
            }
        }
    }
    // The eraser must run before free; legacy credentials never enter caches.
    auth_erase(legacy, sizeof(*legacy));
    erase.data = NULL;
    erase.bytes = 0;
    free(legacy);
    return error;
}

AuthAccountRecord *find_account(AuthDatabase *db, const char *name)
{
    for (uint32_t i = 0; i < db->header.count; ++i)
        if (!strcmp(db->accounts[i].name, name)) return &db->accounts[i];
    return NULL;
}

void public_user(const AuthAccountRecord &account, UserInfo *out)
{
    memset(out, 0, sizeof(*out));
    strcpy(out->name, account.name);
    out->user_type = (UserType)account.role;
    out->user_prms = user_permissions_from_mask((uint8_t)account.permissions);
}

int check_password(const AuthAccountRecord *account, const char *password)
{
    uint64_t now = nanoTime();
    if (now < retry_after) return -EAGAIN;
    uint8_t candidate[32] = {};
    Erase erase = {candidate, sizeof(candidate)};
    // Unknown names do equivalent KDF work and return the same error.
    const uint8_t dummy_salt[32] = {};
    int error = auth_password_hash(password, account ? account->salt : dummy_salt,
                                   account ? account->iterations : AUTH_PASSWORD_ITERATIONS, candidate);
    if (error) return error;
    if (!account || !auth_equal(candidate, account->hash, sizeof(candidate)))
    {
        if (failed_attempts < 5) ++failed_attempts;
        retry_after = nanoTime() + (1ULL << failed_attempts) * 1000000000ULL;
        return -EACCES;
    }
    failed_attempts = 0;
    retry_after = 0;
    return 0;
}

bool caller_session(UserInfo *user, uint64_t *generation, uint64_t *pid)
{
    tcb_t task = get_current_task();
    if (!task || !task->parent_group) return false;
    *pid = task->parent_group->pid;
    if (!permissions_session_snapshot(user, generation))
    {
        memset(user, 0, sizeof(*user));
        return true;
    }
    // A process from an old login cannot act on the newly logged-in account.
    return !strcmp(task->parent_group->permission_user.name, user->name);
}
}

int user_auth_oobe_state()
{
    Guard guard;
    if (guard.error) return guard.error;
    Database db;
    if (!db.data) return -ENOMEM;
    int error = load_database(db.data);
    return error ? error : db.data->header.count == 0 ? 1 : 0;
}

int user_auth_list(UserInfo *out, int count)
{
    if (!out || count < 0) return -EINVAL;
    Guard guard;
    if (guard.error) return guard.error;
    Database db;
    if (!db.data) return -ENOMEM;
    int error = load_database(db.data);
    if (error) return error;
    int copied = 0;
    while (copied < count && (uint32_t)copied < db.data->header.count)
    {
        public_user(db.data->accounts[copied], &out[copied]);
        ++copied;
    }
    return copied;
}

int user_auth_login(const char *username, const char *password, UserInfo *out)
{
    if (!valid_name(username) || !password_valid(password, true) || !out) return -EINVAL;
    Guard guard;
    if (guard.error) return guard.error;
    Database db;
    if (!db.data) return -ENOMEM;
    int error = load_database(db.data);
    if (error) return error;
    AuthAccountRecord *account = find_account(db.data, username);
    error = check_password(account, password);
    if (!error) public_user(*account, out);
    return error;
}

int user_auth_create_first(const char *username, const char *password, UserInfo *out)
{
    if (!valid_name(username) || !password_valid(password) || !out) return -EINVAL;
    Guard guard;
    if (guard.error) return guard.error;
    Database db;
    if (!db.data) return -ENOMEM;
    int error = load_database(db.data);
    if (error) return error;
    if (db.data->header.count)
    {
        // Preserve authenticated OOBE retry after account/profile publication
        // was interrupted; it can never replace an existing first account.
        AuthAccountRecord &first = db.data->accounts[0];
        error = !strcmp(first.name, username) ? check_password(&first, password) : -EEXIST;
        if (!error) public_user(first, out);
        return error;
    }
    AuthAccountRecord &account = db.data->accounts[0];
    strcpy(account.name, username);
    account.role = XUT_Admin;
    account.permissions = user_type_permission_mask(XUT_Admin);
    account.iterations = AUTH_PASSWORD_ITERATIONS;
    account.revision = 1;
    error = auth_random(account.salt, sizeof(account.salt));
    if (!error) error = auth_password_hash(password, account.salt, account.iterations, account.hash);
    if (error) return error;
    db.data->header.count = 1;
    ++db.data->header.generation;
    StorageScope storage;
    error = save_database(db.data);
    if (!error) public_user(account, out);
    return error;
}

int user_auth_begin(const XAuthBegin *request, XAuthChallenge *challenge)
{
    if (!request || !challenge || request->size != sizeof(*request) || request->version != XAUTH_VERSION ||
        request->reserved || request->operation < XAUTH_LOGIN || request->operation > XAUTH_CHANGE_PASSWORD ||
        !memchr(request->username, 0, sizeof(request->username))) return -EINVAL;
    Guard guard;
    if (guard.error) return guard.error;
    UserInfo user = {};
    uint64_t session = 0, pid = 0, now = nanoTime();
    if (!caller_session(&user, &session, &pid)) return -EACCES;
    bool self = request->operation >= XAUTH_VERIFY_PASSWORD;
    if (self && !user.name[0]) return -EACCES;
    const char *name = self ? user.name : request->username;
    if (!valid_name(name)) return -EINVAL;
    if (!self && user.name[0] && strcmp(name, user.name)) return -EACCES;
    if (request->operation != XAUTH_CHANGE_PASSWORD && now < retry_after) return -EAGAIN;

    SessionSlot saved = {};
    Erase erase_saved = {&saved, sizeof(saved)};
    SessionSlot *slot = NULL;
    for (SessionSlot &entry : slots)
    {
        if (entry.expires <= now) auth_erase(&entry, sizeof(entry));
        if (entry.pid == pid)
        {
            if (entry.authorized && entry.session == session && auth_equal(entry.grant, request->grant, 32))
                saved = entry;
            slot = &entry;
        }
    }
    if (request->operation == XAUTH_CHANGE_PASSWORD && !saved.authorized) return -EACCES;
    if (!slot)
        for (SessionSlot &entry : slots) if (!entry.pid) { slot = &entry; break; }
    if (!slot) return -EAGAIN;
    memset(challenge, 0, sizeof(*challenge));
    int error = auth_public_key(challenge->modulus);
    if (!error) error = auth_random(challenge->nonce, sizeof(challenge->nonce));
    if (error) return error;
    auth_erase(slot, sizeof(*slot));
    slot->pid = pid;
    slot->session = session;
    slot->id = next_challenge++;
    slot->expires = nanoTime() + challenge_lifetime;
    slot->operation = request->operation;
    slot->challenge = true;
    slot->revision = saved.revision;
    strcpy(slot->username, name);
    memcpy(slot->nonce, challenge->nonce, sizeof(slot->nonce));
    if (saved.authorized) memcpy(slot->grant, saved.grant, sizeof(slot->grant));
    challenge->size = sizeof(*challenge);
    challenge->version = XAUTH_VERSION;
    challenge->operation = request->operation;
    challenge->id = slot->id;
    challenge->exponent = 65537;
    strcpy(challenge->username, name);
    return 0;
}

int user_auth_finish(const XAuthRequest *request, XAuthResult *result, unsigned *operation)
{
    if (!request || !result || !operation || request->size != sizeof(*request) || request->version != XAUTH_VERSION)
        return -EINVAL;
    *operation = 0;
    Guard guard;
    if (guard.error) return guard.error;
    UserInfo user = {};
    uint64_t session = 0, pid = 0;
    if (!caller_session(&user, &session, &pid)) return -EACCES;
    SessionSlot saved = {};
    for (SessionSlot &slot : slots)
        if (slot.pid == pid && slot.id == request->id && slot.challenge)
        {
            saved = slot;
            auth_erase(&slot, sizeof(slot)); // Consume even invalid ciphertext.
            break;
        }
    Erase erase_saved = {&saved, sizeof(saved)};
    if (!saved.challenge || saved.session != session || saved.expires <= nanoTime()) return -EACCES;
    XAuthPlaintext plain = {};
    Erase erase = {&plain, sizeof(plain)};
    size_t bytes = 0;
    int error = auth_decrypt(request->ciphertext, &plain, sizeof(plain), &bytes);
    if (error || bytes != sizeof(plain) || plain.version != XAUTH_VERSION || plain.operation != saved.operation ||
        plain.id != saved.id || !auth_equal(plain.nonce, saved.nonce, 32) ||
        !auth_equal(plain.username, saved.username, 64) || !auth_equal(plain.grant, saved.grant, 32) ||
        !password_valid(plain.password, saved.operation == XAUTH_LOGIN || saved.operation == XAUTH_VERIFY_PASSWORD))
        return -EACCES;
    memset(result, 0, sizeof(*result));
    result->size = sizeof(*result);
    result->version = XAUTH_VERSION;
    if (saved.operation == XAUTH_LOGIN) error = user_session_login(saved.username, plain.password);
    else if (saved.operation == XAUTH_CREATE_FIRST) error = user_session_create_first(saved.username, plain.password);
    else
    {
        Database db;
        if (!db.data) return -ENOMEM;
        error = load_database(db.data);
        if (error) return error;
        AuthAccountRecord *account = find_account(db.data, saved.username);
        if (!account) return -EACCES;
        if (saved.operation == XAUTH_VERIFY_PASSWORD)
        {
            error = check_password(account, plain.password);
            if (!error)
            {
                error = auth_random(result->grant, sizeof(result->grant));
                if (error) return error;
                for (SessionSlot &slot : slots) if (!slot.pid)
                {
                    slot = saved;
                    slot.challenge = false;
                    slot.authorized = true;
                    slot.revision = account->revision;
                    slot.expires = nanoTime() + challenge_lifetime;
                    memcpy(slot.grant, result->grant, sizeof(slot.grant));
                    break;
                }
            }
        }
        else
        {
            if (account->revision != saved.revision || db.data->header.generation == (uint64_t)-1 ||
                account->revision == (uint64_t)-1) return -ESTALE;
            error = auth_random(account->salt, sizeof(account->salt));
            if (!error) error = auth_password_hash(plain.password, account->salt, account->iterations, account->hash);
            if (error) return error;
            ++account->revision;
            ++db.data->header.generation;
            StorageScope storage;
            error = save_database(db.data);
            if (!error)
                for (SessionSlot &slot : slots)
                    if (!strcmp(slot.username, saved.username)) auth_erase(&slot, sizeof(slot));
        }
    }
    if (!error)
    {
        // Finish the login transaction before another authentication request
        // can observe the published account with an unfinished ready state.
        if (saved.operation == XAUTH_LOGIN || saved.operation == XAUTH_CREATE_FIRST)
        {
            if (user_auth_session_started) user_auth_session_started();
            user_session_set_ready(true);
        }
        *operation = saved.operation;
    }
    return error;
}

int user_auth_account_info(XAccountInfo *info)
{
    if (!info) return -EINVAL;
    Guard guard;
    if (guard.error) return guard.error;
    UserInfo user = {};
    uint64_t session = 0, pid = 0;
    if (!caller_session(&user, &session, &pid) || !user.name[0]) return -EACCES;
    Database db;
    if (!db.data) return -ENOMEM;
    int error = load_database(db.data);
    if (error) return error;
    AuthAccountRecord *account = find_account(db.data, user.name);
    if (!account) return -ENOENT;
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->version = XAUTH_VERSION;
    strcpy(info->username, account->name);
    info->user_type = account->role;
    info->permissions = account->role == XUT_Custom ? account->permissions :
                         user_type_permission_mask((UserType)account->role);
    info->credential_revision = account->revision;
    return 0;
}

void user_auth_process_exit(process_control_block *process)
{
    if (!process) return;
    // Recover and invalidate transient crypto before another caller can use
    // it, while the stopped owner's TCB is still alive. Do not independently
    // unlock the mutex and leave a race before damaged crypto is discarded.
    Guard guard;
    if (guard.error) return;
    for (SessionSlot &slot : slots)
        if (slot.pid == process->pid) auth_erase(&slot, sizeof(slot));
}
