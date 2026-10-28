#include "ime_internal.h"
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <mutex.h>
#include <syscall/xapi_user.h>
#include <task/scheduler.h>

// The service mutex is never held across this file's VFS work. A separate,
// recoverable lock serializes clears, preference changes and delayed saves.
static mutex_t store_lock;

struct StoreScope
{
    tcb_t task;
    StoreScope() : task(get_current_task()) { if (task) ++task->ime_config_depth; }
    ~StoreScope() { if (task) --task->ime_config_depth; }
};

struct StoreHeader
{
    uint32_t magic, version, length, enabled;
    uint64_t generation, serial;
    uint32_t checksum, options; // V1 reserved=0; V2 persists non-learning preferences here.
};
static_assert(sizeof(StoreHeader) == 40, "Learning store layout changed");
static const uint32_t STORE_MAGIC = 0x32444d49; // IMD2

static uint32_t checksum(const void *data, size_t size)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t result = 2166136261U;
    for (size_t i = 0; i < size; ++i) result = (result ^ p[i]) * 16777619U;
    return result;
}

static uint32_t record_checksum(StoreHeader header, const void *data)
{
    header.checksum = 0;
    return checksum(&header, sizeof(header)) ^ checksum(data, header.length);
}

void ime_user_store_initialize()
{
    mutex_create(&store_lock, false);
}

void ime_user_store_process_exit(pcb_t process)
{
    mutex_abort_for_process(&store_lock, process);
}

struct StoreGuard
{
    StoreGuard() { mutex_lock_recoverable(&store_lock); }
    ~StoreGuard() { mutex_unlock(&store_lock); }
};

static void encode_component(char *out, const char *text)
{
    static const char digits[] = "0123456789abcdef";
    while (*text)
    {
        unsigned char c = (unsigned char)*text++;
        *out++ = digits[c >> 4];
        *out++ = digits[c & 15];
    }
    *out = 0;
}

static bool directory(const char *path)
{
    vfs_node_t node = vfs_open(path);
    if (!node && vfs_mkdir(path) == 0) node = vfs_open(path);
    if (!node) return false;
    bool ok = (node->type & file_dir) && !(node->type & file_symlink) && !node->linkto;
    vfs_close(node);
    return ok;
}

// Create only the validated policy's directory chain. Symlink/proxy parents
// remain forbidden, including during a migration from account storage.
static bool directories(const char *path)
{
    char current[512];
    strcpy(current, path);
    for (char *p = current + 1;; ++p)
    {
        if (*p != '/' && *p) continue;
        char saved = *p;
        *p = 0;
        if (!directory(current)) return false;
        *p = saved;
        if (!saved) return true;
    }
}

static unsigned char *read_slot(const char *path, StoreHeader *header,
                                uint32_t option_mask = ~(XIME_DATA_ENABLED | XIME_DATA_ASYNC | XIME_DATA_PENDING))
{
    vfs_node_t node = vfs_open(path);
    if (!node) return NULL;
    bool ok = !(node->type & (file_dir | file_symlink | file_block)) && !node->linkto &&
              node->size >= sizeof(*header) && node->size <= sizeof(*header) + XIME_USER_DATA_LIMIT;
    if (ok) ok = vfs_read(node, header, 0, sizeof(*header)) == sizeof(*header);
    if (ok)
        ok = header->magic == STORE_MAGIC && (header->version == 1 || header->version == 2) && header->enabled <= 1 &&
             header->length <= XIME_USER_DATA_LIMIT && header->generation && header->serial &&
             !(header->options & ~option_mask) &&
             (header->version != 1 || !header->options) && node->size == sizeof(*header) + header->length;
    unsigned char *data = ok ? (unsigned char *)malloc(header->length ? header->length : 1) : NULL;
    if (data && header->length)
        ok = vfs_read(node, data, sizeof(*header), header->length) == header->length;
    vfs_close(node);
    if (!data || !ok || header->checksum != record_checksum(*header, data))
    {
        free(data);
        return NULL;
    }
    return data;
}

static bool write_slot(const char *path, StoreHeader *header, const void *data)
{
    header->checksum = record_checksum(*header, data);
    vfs_node_t node = vfs_open(path);
    if (!node && vfs_mkfile(path) == 0) node = vfs_open(path);
    if (!node) return false;
    bool ok = !(node->type & (file_dir | file_symlink | file_block)) && !node->linkto;
    if (ok) ok = vfs_resize(node, sizeof(*header) + header->length) == 0;
    if (ok) ok = vfs_write(node, header, 0, sizeof(*header)) == sizeof(*header);
    if (ok && header->length)
        ok = vfs_write(node, (void *)data, sizeof(*header), header->length) == header->length;
    vfs_close(node);
    StoreHeader check = {};
    unsigned char *verify = read_slot(path, &check);
    ok = ok && verify && !memcmp(&check, header, sizeof(check));
    if (ok && header->length) ok = !memcmp(verify, data, header->length);
    free(verify);
    return ok;
}

static int authorize_again(const XImeUserData *request, const ImeUserIdentity &identity)
{
    ImeUserIdentity current = {};
    int result = ime_user_authorize(request, &current);
    if (result) return result;
    return strcmp(identity.user, current.user) || strcmp(identity.id, current.id) ? -ESTALE : 0;
}

static int store_transaction(XImeUserData &request, const ImeUserIdentity &identity,
                             unsigned char *incoming, bool authorize)
{
    int error = 0;
    StoreGuard guard;
    StoreScope scope;
    error = authorize ? authorize_again(&request, identity) : 0;
    char path[512], paths[2][768], account[129], engine[97], account_directory[XIME_PATH_BYTES + 64];
    encode_component(account, identity.user);
    encode_component(engine, identity.id);
    const ImePlatform *platform = ime_platform();
    if (!platform) return -ENODEV;
    const char *pattern = platform->account_store, *slot = strchr(pattern, '*');
    size_t prefix = slot - pattern;
    memcpy(account_directory, pattern, prefix);
    strcpy(account_directory + prefix, identity.user);
    strcpy(account_directory + prefix + strlen(identity.user), slot + 1);
    char legacy_directory[512];
    snprintf(legacy_directory, sizeof(legacy_directory), "%s/e%s", account_directory, engine);
    const ImeProviderPolicy *provider = ime_provider(identity.id);
    uint32_t option_mask = provider ? provider->option_mask :
                           ~(XIME_DATA_ENABLED | XIME_DATA_ASYNC | XIME_DATA_PENDING);
    uint32_t defaults = provider ? provider->default_flags : XIME_DATA_ENABLED;
    bool relocated = provider && provider->data_root;
    bool dirs = false;
    if (!error)
    {
        if (relocated)
        {
            dirs = directories(provider->data_root);
            strcpy(path, provider->data_root);
        }
        else
        {
            strcpy(path, legacy_directory);
            dirs = directories(path);
        }
        for (unsigned i = 0; i < 2; ++i)
            snprintf(paths[i], sizeof(paths[i]), "%s/u%s.%u", path, account, i);
        if (!dirs) error = -EIO;
    }
    StoreHeader headers[2] = {};
    unsigned char *slots[2] = {};
    if (!error)
        for (unsigned i = 0; i < 2; ++i) slots[i] = read_slot(paths[i], &headers[i], option_mask);
    int selected = slots[0] ? 0 : -1;
    if (slots[1] && (selected < 0 || headers[1].serial > headers[0].serial)) selected = 1;
    // Copy the newest verified legacy snapshot to both new slots before
    // removing either old file. A failed migration leaves the old data intact.
    if (!error && relocated && selected < 0)
    {
        StoreHeader legacy_headers[2] = {};
        unsigned char *legacy[2] = {};
        char old_paths[2][768];
        for (unsigned i = 0; i < 2; ++i)
        {
            snprintf(old_paths[i], sizeof(old_paths[i]), "%s/u%s.%u", legacy_directory, account, i);
            legacy[i] = read_slot(old_paths[i], &legacy_headers[i], option_mask);
        }
        int old = legacy[0] ? 0 : -1;
        if (legacy[1] && (old < 0 || legacy_headers[1].serial > legacy_headers[0].serial)) old = 1;
        if (old >= 0)
        {
            StoreHeader migrated = legacy_headers[old];
            if (migrated.version == 1) migrated.options = defaults & option_mask;
            migrated.version = 2;
            if (!write_slot(paths[0], &migrated, legacy[old]) || !write_slot(paths[1], &migrated, legacy[old])) error = -EIO;
            else
            {
                for (unsigned i = 0; i < 2; ++i)
                {
                    vfs_node_t node = vfs_open(old_paths[i]);
                    if (node) vfs_delete(node); // Releases the opened node reference.
                    slots[i] = read_slot(paths[i], &headers[i], option_mask);
                }
                selected = slots[0] ? 0 : slots[1] ? 1 : -1;
                if (selected < 0) error = -EIO;
            }
        }
        free(legacy[0]); free(legacy[1]);
    }
    StoreHeader current = {STORE_MAGIC, 2, 0, (defaults & XIME_DATA_ENABLED) != 0, 1, 0, 0, defaults & option_mask};
    if (selected >= 0) current = headers[selected];
    if (current.version == 1) current.options = defaults & option_mask;
    current.version = 2;
    unsigned char *data = selected >= 0 ? slots[selected] : NULL;
    if (!error && authorize) error = authorize_again(&request, identity);
    if (!error && request.operation == XIME_DATA_SAVE)
    {
        if (request.generation != current.generation) error = -ESTALE;
        else if (!current.enabled) error = -EPERM;
        else { current.length = request.length; data = incoming; }
    }
    bool preference = request.operation == XIME_DATA_SET_ENABLED || request.operation == XIME_DATA_SET_OPTIONS;
    if (!error && (request.operation == XIME_DATA_CLEAR || preference))
    {
        if (request.operation == XIME_DATA_SET_OPTIONS && request.generation != current.generation) error = -ESTALE;
        else if (current.generation == ~0ULL) error = -EOVERFLOW;
        else ++current.generation;
        if (request.operation == XIME_DATA_CLEAR) { current.length = 0; data = NULL; }
        else
        {
            current.enabled = (request.flags & XIME_DATA_ENABLED) != 0;
            if (request.operation == XIME_DATA_SET_OPTIONS)
                current.options = request.flags & option_mask;
        }
    }
    if (!error && (request.operation == XIME_DATA_SAVE || request.operation == XIME_DATA_CLEAR || preference))
    {
        if (current.serial == ~0ULL) error = -EOVERFLOW;
        else
        {
            ++current.serial;
            // Overwrite the older slot, retaining the newest verified snapshot.
            unsigned destination = selected == 0 ? 1 : 0;
            if (!write_slot(paths[destination], &current, data)) error = -EIO;
            else if (request.operation == XIME_DATA_CLEAR)
                write_slot(paths[destination ^ 1], &current, data);
        }
    }
    if (!error && request.operation == XIME_DATA_LOAD)
    {
        if (request.capacity < current.length) error = -ENOSPC;
        else if (current.length && !copy_to_user_pagedir(xapi_current_pagedir(), (void *)request.buffer,
                                                        data, current.length)) error = -EFAULT;
    }
    request.flags = (current.enabled ? XIME_DATA_ENABLED : 0) | current.options;
    request.length = current.length;
    request.generation = current.generation;
    free(slots[0]);
    free(slots[1]);
    if (!error && (request.operation == XIME_DATA_CLEAR || preference))
        ime_reset_input();
    return error;
}

struct SaveJob
{
    XImeUserData request;
    ImeUserIdentity identity;
    unsigned char *data;
    uint32_t state; // 1 queued, 2 running, 3 complete
    int result;
};
static spin_t job_lock = SPIN_INIT;
static SaveJob *save_job;
static uint32_t worker_started; // 0 stopped, 1 starting, 2 ready

static bool service_save_job()
{
    spin_lock(&job_lock);
    SaveJob *job = save_job;
    if (!job || job->state != 1) { spin_unlock(&job_lock); return false; }
    job->state = 2;
    spin_unlock(&job_lock);
    // Identity was authorized when queued. The generation is still checked
    // under store_lock, so a delayed write cannot undo CLEAR/SET_ENABLED.
    int result = store_transaction(job->request, job->identity, job->data, false);
    free(job->data);
    spin_lock(&job_lock);
    job->data = NULL;
    job->result = result;
    job->state = 3;
    spin_unlock(&job_lock);
    return true;
}

static void save_worker(void *)
{
    while (true)
    {
        if (!service_save_job()) scheduler_sleep_ns(20000000ULL);
    }
}

static int queue_save(const XImeUserData &request, const ImeUserIdentity &identity, unsigned char *data)
{
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&worker_started, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        int64_t tid = (int64_t)create_kernel_thread((void *)save_worker, NULL, (char *)"ime-learning", NULL);
        if (tid <= 0)
        {
            __atomic_store_n(&worker_started, 0, __ATOMIC_RELEASE);
            return -ENOMEM;
        }
        __atomic_store_n(&worker_started, 2, __ATOMIC_RELEASE);
    }
    else if (expected != 2)
    {
        // Another caller is still creating the worker. Do not publish a job
        // that could be stranded if that creation fails.
        return -EAGAIN;
    }
    SaveJob *next = (SaveJob *)calloc(1, sizeof(SaveJob));
    if (!next) return -ENOMEM;
    next->request = request;
    next->request.flags &= ~XIME_DATA_ASYNC;
    next->identity = identity;
    next->data = data;
    next->state = 1;
    spin_lock(&job_lock);
    SaveJob *old = save_job;
    if (old && old->state != 3)
    {
        spin_unlock(&job_lock);
        free(next);
        return -EAGAIN;
    }
    save_job = next;
    spin_unlock(&job_lock);
    free(old);
    return 0;
}

static int store_status(uint64_t address, XImeUserData &request)
{
    int result = 0;
    request.flags = 0;
    spin_lock(&job_lock);
    if (save_job && save_job->request.session == request.session)
    {
        if (save_job->state != 3) request.flags = XIME_DATA_PENDING;
        else result = save_job->result;
    }
    spin_unlock(&job_lock);
    if (!copy_to_user_pagedir(xapi_current_pagedir(), (void *)address, &request, sizeof(request))) return -EFAULT;
    return result;
}
int64_t ime_user_store_syscall(uint64_t address)
{
    XImeUserData request = {};
    if (!address || !copy_from_user_pagedir(xapi_current_pagedir(), &request, (void *)address, sizeof(request)))
        return -EFAULT;
    if (request.size != sizeof(request) || request.version != XIME_ABI_V2 ||
        request.operation > XIME_DATA_SET_OPTIONS || (request.flags & XIME_DATA_PENDING) ||
        request.capacity > XIME_USER_DATA_LIMIT || request.length > XIME_USER_DATA_LIMIT ||
        !memchr(request.id, 0, sizeof(request.id))) return -EINVAL;
    ImeUserIdentity identity = {};
    int error = ime_user_authorize(&request, &identity);
    if (error) return error;
    const ImeProviderPolicy *provider = ime_provider(identity.id);
    uint32_t option_mask = provider ? provider->option_mask :
                           ~(XIME_DATA_ENABLED | XIME_DATA_ASYNC | XIME_DATA_PENDING);
    if (request.flags & ~(option_mask | XIME_DATA_ENABLED | XIME_DATA_ASYNC)) return -EINVAL;
    if (!strcmp(identity.id, ".") || !strcmp(identity.id, "..")) return -EINVAL;
    if ((request.operation == XIME_DATA_SAVE && request.length > request.capacity) ||
        ((request.operation == XIME_DATA_SAVE || request.operation == XIME_DATA_LOAD) &&
         (request.length || request.capacity) && !request.buffer)) return -EINVAL;

    if ((request.flags & XIME_DATA_ASYNC) && request.operation != XIME_DATA_SAVE) return -EINVAL;
    if (request.operation == XIME_DATA_STATUS) return store_status(address, request);
    unsigned char *incoming = NULL;
    if (request.operation == XIME_DATA_SAVE && request.length)
    {
        incoming = (unsigned char *)malloc(request.length);
        if (!incoming) return -ENOMEM;
        if (!copy_from_user_pagedir(xapi_current_pagedir(), incoming, (void *)request.buffer, request.length))
        {
            free(incoming);
            return -EFAULT;
        }
    }
    if (request.flags & XIME_DATA_ASYNC)
    {
        error = queue_save(request, identity, incoming);
        if (error) free(incoming);
        return error;
    }
    error = store_transaction(request, identity, incoming, true);
    if ((!error || error == -ENOSPC) &&
        !copy_to_user_pagedir(xapi_current_pagedir(), (void *)address, &request, sizeof(request))) error = -EFAULT;
    free(incoming);
    return error;
}
