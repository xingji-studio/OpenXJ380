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
    uint32_t checksum, reserved;
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

static unsigned char *read_slot(const char *path, StoreHeader *header)
{
    vfs_node_t node = vfs_open(path);
    if (!node) return NULL;
    bool ok = !(node->type & (file_dir | file_symlink | file_block)) && !node->linkto &&
              node->size >= sizeof(*header) && node->size <= sizeof(*header) + XIME_USER_DATA_LIMIT;
    if (ok) ok = vfs_read(node, header, 0, sizeof(*header)) == sizeof(*header);
    if (ok)
        ok = header->magic == STORE_MAGIC && header->version == 1 && header->enabled <= 1 &&
             header->length <= XIME_USER_DATA_LIMIT && header->generation && header->serial &&
             !header->reserved && node->size == sizeof(*header) + header->length;
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
    char path[512], paths[2][512], account[129], engine[97];
    encode_component(account, identity.user);
    encode_component(engine, identity.id);
    bool dirs = false;
    if (!error)
    {
        dirs = directory("/users");
        snprintf(path, sizeof(path), "/users/%s", identity.user);
        dirs = dirs && directory(path);
        snprintf(path, sizeof(path), "/users/%s/ime", identity.user);
        dirs = dirs && directory(path);
        snprintf(path, sizeof(path), "/users/%s/ime/e%s", identity.user, engine);
        dirs = dirs && directory(path);
        for (unsigned i = 0; i < 2; ++i)
            snprintf(paths[i], sizeof(paths[i]), "/users/%s/ime/e%s/u%s.%u", identity.user, engine, account, i);
        if (!dirs) error = -EIO;
    }
    StoreHeader headers[2] = {};
    unsigned char *slots[2] = {};
    if (!error)
        for (unsigned i = 0; i < 2; ++i) slots[i] = read_slot(paths[i], &headers[i]);
    int selected = slots[0] ? 0 : -1;
    if (slots[1] && (selected < 0 || headers[1].serial > headers[0].serial)) selected = 1;
    StoreHeader current = {STORE_MAGIC, 1, 0, 1, 1, 0, 0, 0};
    if (selected >= 0) current = headers[selected];
    unsigned char *data = selected >= 0 ? slots[selected] : NULL;
    if (!error && authorize) error = authorize_again(&request, identity);
    if (!error && request.operation == XIME_DATA_SAVE)
    {
        if (request.generation != current.generation) error = -ESTALE;
        else if (!current.enabled) error = -EPERM;
        else { current.length = request.length; data = incoming; }
    }
    if (!error && (request.operation == XIME_DATA_CLEAR || request.operation == XIME_DATA_SET_ENABLED))
    {
        if (current.generation == ~0ULL) error = -EOVERFLOW;
        else ++current.generation;
        if (request.operation == XIME_DATA_CLEAR) { current.length = 0; data = NULL; }
        else current.enabled = (request.flags & XIME_DATA_ENABLED) != 0;
    }
    if (!error && request.operation >= XIME_DATA_SAVE && request.operation <= XIME_DATA_SET_ENABLED)
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
    request.flags = current.enabled ? XIME_DATA_ENABLED : 0;
    request.length = current.length;
    request.generation = current.generation;
    free(slots[0]);
    free(slots[1]);
    if (!error && (request.operation == XIME_DATA_CLEAR || request.operation == XIME_DATA_SET_ENABLED))
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
        request.operation > XIME_DATA_STATUS || (request.flags & ~(XIME_DATA_ENABLED | XIME_DATA_ASYNC)) ||
        request.capacity > XIME_USER_DATA_LIMIT || request.length > XIME_USER_DATA_LIMIT ||
        !memchr(request.id, 0, sizeof(request.id))) return -EINVAL;
    ImeUserIdentity identity = {};
    int error = ime_user_authorize(&request, &identity);
    if (error) return error;
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
