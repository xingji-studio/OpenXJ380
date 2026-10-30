#include <user/permissions.h>
#include <cpu/lock.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <input_method.h>
#include <krlibc.h>
#include <stdio.h>

// The lock protects session publication, process permissions and the entire FIFO.
// It never protects allocation, sleeping, rendering or filesystem operations.
extern spin_t create_thread_lock;
static spin_t permission_lock = SPIN_INIT;
static uint64_t session_generation = 1, next_token = 1;
static bool backend_available;
static uint32_t outstanding_requests;

struct PermissionRequest
{
    PermissionRequest *next;
    tcb_t owner;
    pcb_t process;
    PermissionPrompt prompt;
    uint64_t session, revision;
    uint8_t remaining;
    bool done;
    bool role_confirmation, role_approved;
};
static PermissionRequest *requests, *tail;

static uint8_t mask_of(const UserInfo *user)
{
    if (!user) return 0;
    return user->user_type == XUT_Custom ? user_permission_mask(user->user_prms) :
                                         user_type_permission_mask(user->user_type);
}

void permissions_session_publish(const UserInfo *info)
{
    spin_lock(&permission_lock);
    *current_user = *info;
    ++session_generation;
    spin_unlock(&permission_lock);
}

void permissions_process_init(pcb_t process, pcb_t parent)
{
    spin_lock(&permission_lock);
    const UserInfo *source = parent && parent != kernel_group ? &parent->permission_user : current_user;
    process->permission_user = source ? *source : root_user;
    // Passwords are account data and must never be copied into process credentials.
    memset(process->permission_user.password, 0, sizeof(process->permission_user.password));
    process->permission_revision = 1;
    spin_unlock(&permission_lock);
}

void permissions_process_exec(pcb_t process)
{
    spin_lock(&permission_lock);
    __atomic_add_fetch(&process->permission_revision, 1ULL, __ATOMIC_RELEASE); // Invalidate consent for the previous executable.
    spin_unlock(&permission_lock);
}

uint8_t permissions_current_mask()
{
    tcb_t task = get_current_task();
    spin_lock(&permission_lock);
    uint8_t mask = task && task->task_level == TASK_APPLICATION_LEVEL && task->parent_group ?
                       mask_of(&task->parent_group->permission_user) : (uint8_t)XPERM_ALL;
    spin_unlock(&permission_lock);
    return mask;
}

UserType permissions_current_type()
{
    tcb_t task = get_current_task();
    spin_lock(&permission_lock);
    UserType type = task && task->parent_group ? task->parent_group->permission_user.user_type : XUT_Root;
    spin_unlock(&permission_lock);
    return type;
}

bool permissions_have(uint8_t required)
{
    return (permissions_current_mask() & required) == required;
}

static bool valid_locked(PermissionRequest *r)
{
    return r->session == session_generation && r->revision == r->process->permission_revision &&
           r->process->status != DEATH && r->process->status != OUT && r->process->status != ZOMBIE &&
           r->owner->status != DEATH && r->owner->status != OUT;
}

static PermissionRequest *first_locked()
{
    for (PermissionRequest *r = requests; r; r = r->next)
    {
        if (r->done) continue;
        if (!backend_available || !valid_locked(r)) { r->done = true; continue; }
        uint8_t current = mask_of(&r->process->permission_user);
        r->remaining &= ~current;
        r->remaining &= mask_of(current_user);
        if (!r->remaining && !r->role_confirmation) { r->done = true; continue; }
        if (!r->prompt.token)
        {
            r->prompt.token = next_token++;
            r->prompt.current = current;
            r->prompt.current_type = r->process->permission_user.user_type;
            r->prompt.asking = r->remaining;
            if (r->prompt.individual)
            {
                for (uint8_t bit = XPERM_R; bit; bit >>= 1)
                    if (r->remaining & bit) { r->prompt.asking = bit; break; }
            }
        }
        return r;
    }
    return NULL;
}

bool permissions_modal_active()
{
    // Readable even from the serial overlay's drawing lock; never acquire another lock here.
    return __atomic_load_n(&outstanding_requests, __ATOMIC_ACQUIRE) != 0;
}

bool permissions_prompt(PermissionPrompt *out)
{
    spin_lock(&permission_lock);
    PermissionRequest *r = first_locked();
    if (r && out) *out = r->prompt;
    spin_unlock(&permission_lock);
    return r != NULL;
}

void permissions_answer(uint64_t token, bool approved)
{
    spin_lock(&permission_lock);
    PermissionRequest *r = first_locked();
    if (r && r->prompt.token == token)
    {
        if (approved)
        {
            uint8_t granted = r->prompt.asking & mask_of(current_user);
            UserInfo *user = &r->process->permission_user;
            user->user_prms = user_permissions_from_mask(mask_of(user) | granted);
            user->user_type = XUT_Custom;
            r->role_approved = true;
            // A named elevation may retain extra rights from earlier grants.
            if (!r->prompt.individual &&
                user_permission_mask(user->user_prms) == user_type_permission_mask(r->prompt.target_type))
                user->user_type = r->prompt.target_type;
        }
        r->role_confirmation = false;
        r->remaining &= ~r->prompt.asking;
        r->prompt.token = 0;
        if (!r->remaining) r->done = true;
    }
    spin_unlock(&permission_lock);
}

void permissions_set_backend(bool available)
{
    spin_lock(&permission_lock);
    backend_available = available;
    if (!available)
        for (PermissionRequest *r = requests; r; r = r->next) r->done = true;
    spin_unlock(&permission_lock);
}

static void unlink_locked(PermissionRequest *r)
{
    PermissionRequest **link = &requests, *previous = NULL;
    while (*link && *link != r) { previous = *link; link = &(*link)->next; }
    if (!*link) return;
    *link = r->next;
    __atomic_sub_fetch(&outstanding_requests, 1U, __ATOMIC_RELEASE);
    if (tail == r) tail = previous;
}

void permissions_thread_exit(tcb_t task)
{
    PermissionRequest *garbage = NULL;
    spin_lock(&permission_lock);
    for (PermissionRequest *r = requests, *next; r; r = next)
    {
        next = r->next;
        if (r->owner != task) continue;
        unlink_locked(r);
        r->next = garbage;
        garbage = r;
    }
    spin_unlock(&permission_lock);
    while (garbage)
    {
        PermissionRequest *next = garbage->next;
        free(garbage);
        garbage = next;
    }
}

static int request_permissions(uint8_t target, UserType type, bool individual)
{
    tcb_t task = get_current_task();
    if (!task || !task->parent_group || task->task_level != TASK_APPLICATION_LEVEL) return -EPERM;
    pcb_t process = task->parent_group;
    PermissionRequest *r = (PermissionRequest *)calloc(1, sizeof(PermissionRequest));
    if (!r) return -ENOMEM;
    r->owner = task;
    r->process = process;
    r->prompt.pid = process->pid;
    r->prompt.ppid = process->ppid;
    r->prompt.tid = task->tid;
    // Exec publishes and replaces these strings under create_thread_lock. Copy
    // them before taking permission_lock, preserving the create -> permission order.
    spin_lock(&create_thread_lock);
    strncpy(r->prompt.name, process->name, sizeof(r->prompt.name) - 1);
    strncpy(r->prompt.path, process->exe_path ? process->exe_path : "(unknown)", sizeof(r->prompt.path) - 1);
    r->revision = __atomic_load_n(&process->permission_revision, __ATOMIC_ACQUIRE);
    spin_unlock(&create_thread_lock);
    r->prompt.target_type = type;
    r->prompt.individual = individual;

    spin_lock(&permission_lock);
    if (r->revision != process->permission_revision)
    {
        spin_unlock(&permission_lock);
        free(r);
        return -EAGAIN;
    }
    uint8_t current = mask_of(&process->permission_user);
    if (!individual && type == XUT_Custom)
    {
        process->permission_user.user_prms = user_permissions_from_mask(current);
        process->permission_user.user_type = XUT_Custom;
        __atomic_add_fetch(&process->permission_revision, 1ULL, __ATOMIC_RELEASE);
        spin_unlock(&permission_lock);
        free(r);
        return 0;
    }
    r->prompt.requested = target;
    // Only explicit Lift-to-lower/equal is a replacement. Additive requests never clamp existing rights.
    bool lower = !individual && type != XUT_Custom &&
        ((process->permission_user.user_type != XUT_Custom && type >= process->permission_user.user_type) ||
         (process->permission_user.user_type == XUT_Custom && !(target & ~current)));
    if (lower)
    {
        process->permission_user.user_type = type;
        process->permission_user.user_prms = user_permissions_from_mask(target);
        __atomic_add_fetch(&process->permission_revision, 1ULL, __ATOMIC_RELEASE);
        spin_unlock(&permission_lock);
        free(r);
        return 0;
    }
    uint8_t missing = target & ~current;
    if (!individual && process->permission_user.user_type != XUT_Custom && current_user &&
        current_user->user_type != XUT_Custom && type < current_user->user_type)
    {
        spin_unlock(&permission_lock);
        free(r);
        return missing ? missing : -EACCES;
    }
    r->remaining = missing & mask_of(current_user);
    r->role_confirmation = !individual && process->permission_user.user_type != XUT_Custom &&
        type < process->permission_user.user_type && !missing;
    // Equal permission sets do not bypass the named user's grade ceiling.
    if (r->role_confirmation && (!current_user ||
        (current_user->user_type != XUT_Custom && type < current_user->user_type)))
    {
        spin_unlock(&permission_lock);
        free(r);
        return -EACCES;
    }
    if ((!r->remaining && !r->role_confirmation) || !backend_available)
    {
        int failed = r->role_confirmation ? -EACCES : missing;
        spin_unlock(&permission_lock);
        free(r);
        return failed;
    }
    r->session = session_generation;
    strncpy(r->prompt.user, process->permission_user.name, sizeof(r->prompt.user) - 1);
    if (tail) tail->next = r;
    else requests = r;
    tail = r;
    __atomic_add_fetch(&outstanding_requests, 1U, __ATOMIC_RELEASE);
    spin_unlock(&permission_lock);

    for (;;)
    {
        spin_lock(&permission_lock);
        first_locked();
        if (r->done)
        {
            int failed = target & ~mask_of(&process->permission_user);
            if (!individual && !failed && !r->role_approved && process->permission_user.user_type != type)
                failed = -EACCES;
            unlink_locked(r);
            spin_unlock(&permission_lock);
            free(r);
            return failed;
        }
        spin_unlock(&permission_lock);
        scheduler_sleep_ns(10000000ULL);
    }
}

int permissions_lift(UserType type)
{
    if (type < XUT_Root || type > XUT_Custom) return -EINVAL;
    return request_permissions(user_type_permission_mask(type), type, false);
}

int permissions_request(uint8_t requested)
{
    if (requested & ~XPERM_ALL) return -EINVAL;
    return request_permissions(requested, XUT_Custom, true);
}

static bool under(const char *path, const char *directory)
{
    size_t n = strlen(directory);
    // FAT is case-insensitive; a differently cased spelling must not bypass policy.
    for (size_t i = 0; i < n; ++i)
    {
        unsigned char a = path[i], b = directory[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b || !a) return false;
    }
    return path[n] == '/' || path[n] == '\0';
}

static bool path_access(const char *path, bool directory, int operation, uint8_t mask, const char *user)
{
    bool write = operation != IME_FILE_READ;
    bool system = under(path, "/system") || under(path, "/config") || under(path, "/boot") || under(path, "/mod");
    // Shared UI assets are ordinary readable data. Their system-owned location
    // must not force every Admin desktop/app to acquire SR merely to draw itself.
    bool public_asset = under(path, "/system/resources") || under(path, "/system/icon") ||
        under(path, "/system/font") || under(path, "/system/theme") || under(path, "/system/foxland");
    uint8_t required = directory ? 0 : (write ? XPERM_W : XPERM_R);
    if (directory && write) required |= XPERM_W;
    if (system)
    {
        required |= XPERM_SV;
        if (write) required |= XPERM_SW;
        else if (!directory && !public_asset) required |= XPERM_SR;
        if (write && (under(path, "/config") || under(path, "/system/config"))) required |= XPERM_CS;
    }
    if (!(mask & XPERM_NV) && strcmp(path, "/") != 0)
    {
        // Directory traversal to one's own home remains possible without NV.
        char home[80];
        snprintf(home, sizeof(home), "/users/%s", user);
        if (!under(path, home) && !(directory && strcmp(path, "/users") == 0)) return false;
    }
    return (mask & required) == required;
}

bool permissions_file_access(vfs_node_t node, const char *path, int operation)
{
    tcb_t task = get_current_task();
    if (!task || task->task_level != TASK_APPLICATION_LEVEL || task->ime_config_depth) return true;
    uint8_t mask = permissions_current_mask();
    if (mask == XPERM_ALL) return true;
    if (operation == IME_FILE_MOUNT) return false;
    // Anonymous IPC descriptors have no filesystem location, but still require R/W.
    if (!path && (!node || !node->parent))
        return permissions_have(operation == IME_FILE_READ ? XPERM_R : XPERM_W);
    char username[64];
    spin_lock(&permission_lock);
    memcpy(username, task->parent_group->permission_user.name, sizeof(username));
    spin_unlock(&permission_lock);
    username[sizeof(username) - 1] = '\0';
    bool allowed = true;
    if (path)
    {
        char *joined = NULL;
        if (path[0] != '/' && task->cwd)
        {
            char *base = vfs_get_fullpath(task->cwd);
            if (!base) return false;
            size_t size = strlen(base) + strlen(path) + 2;
            joined = (char *)malloc(size);
            if (joined) snprintf(joined, size, "%s/%s", base, path);
            free(base);
            if (!joined) return false;
        }
        char *normalized = normalize_path(joined ? joined : path);
        free(joined);
        if (!normalized) return false;
        allowed = path_access(normalized, false, operation, mask, username);
        free(normalized);
    }
    if (allowed && node)
    {
        char *resolved = vfs_get_fullpath(node);
        if (!resolved) return false;
        allowed = path_access(resolved, (node->type & file_dir) != 0, operation, mask, username);
        free(resolved);
    }
    return allowed;
}
