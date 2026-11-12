#include <user/permissions.h>
#include <user/auth.h>
#include <user/process_launch.h>
#include <user/file_access.h>
#include <user/special_permissions.h>
#include <cpu/lock.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <input_method.h>
#include <krlibc.h>
#include <stdio.h>

// The product implements the trusted notification UI. The standalone provider
// keeps its ordinary VFS semantics when that optional consumer is absent.
extern void permissions_settings_write_notice(const char *application, uint64_t pid) __attribute__((weak));

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
    bool special_ready, special_answered;
    int special_result;
    char special_path[XSP_APP_PATH_BYTES];
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
    memset(current_user->password, 0, sizeof(current_user->password));
    ++session_generation;
    spin_unlock(&permission_lock);
}

bool permissions_session_name(char *out, size_t capacity)
{
    if (!out || !capacity) return false;
    spin_lock(&permission_lock);
    out[0] = 0;
    if (user_session_is_ready() && current_user)
        strncpy(out, current_user->name, capacity - 1);
    out[capacity - 1] = 0;
    spin_unlock(&permission_lock);
    return out[0] != 0;
}

bool permissions_session_snapshot(UserInfo *out, uint64_t *generation)
{
    spin_lock(&permission_lock);
    *generation = session_generation;
    bool ready = user_session_is_ready() && current_user;
    if (ready) *out = *current_user;
    spin_unlock(&permission_lock);
    return ready;
}

static UserType session_type_locked()
{
    return user_session_is_ready() && current_user ? current_user->user_type : XUT_Admin;
}

static uint8_t session_mask_locked()
{
    return user_session_is_ready() && current_user ? mask_of(current_user) : user_type_permission_mask(XUT_Admin);
}

static void launch_user_locked(UserInfo *out, pcb_t parent, ProcessCredentialSource source)
{
    bool inherit = source == XPCS_INHERIT && parent && parent != kernel_group;
    const UserInfo *user = inherit ? &parent->permission_user :
        (user_session_is_ready() && current_user ? current_user : &root_user);
    *out = user ? *user : root_user;
    if (!inherit)
    {
        if (source == XPCS_VISITOR) out->user_type = XUT_Visitor;
        else if (!user_session_is_ready() || !user) out->user_type = XUT_Admin;
        if (out->user_type != XUT_Custom) out->user_prms = user_permissions_from_mask(mask_of(out));
    }
    memset(out->password, 0, sizeof(out->password));
}

void permissions_process_init(pcb_t process, pcb_t parent, ProcessCredentialSource source)
{
    spin_lock(&permission_lock);
    launch_user_locked(&process->permission_user, parent, source);
    process->permission_revision = 1;
    process->permission_file_dialog = false;
    if (source == XPCS_INHERIT && parent && parent != kernel_group)
        memcpy(process->permission_directory, parent->permission_directory, sizeof(process->permission_directory));
    else process->permission_directory[0] = 0;
    spin_unlock(&permission_lock);
}

bool permissions_launch_matches(pcb_t process, pcb_t parent, ProcessCredentialSource source)
{
    UserInfo expected;
    spin_lock(&permission_lock);
    launch_user_locked(&expected, parent, source);
    bool matches = process && expected.user_type == process->permission_user.user_type &&
        mask_of(&expected) == mask_of(&process->permission_user) &&
        !strcmp(expected.name, process->permission_user.name);
    spin_unlock(&permission_lock);
    return matches;
}

void permissions_process_exec(pcb_t process, const char *directory)
{
    spin_lock(&permission_lock);
    __atomic_store_n(&process->permission_file_dialog, false, __ATOMIC_RELEASE);
    if (directory)
    {
        strncpy(process->permission_directory, directory, sizeof(process->permission_directory) - 1);
        process->permission_directory[sizeof(process->permission_directory) - 1] = 0;
    }
    __atomic_add_fetch(&process->permission_revision, 1ULL, __ATOMIC_RELEASE); // Invalidate previous consent.
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
        if (r->prompt.special_id)
        {
            if (!user_session_is_ready() || !current_user || strcmp(current_user->name, r->prompt.user))
            { r->done = true; continue; }
            // Keep the head reserved while a prior answer is persisted. A
            // duplicate cannot display before its remembered decision exists.
            if (!r->special_ready || r->special_answered) return NULL;
            if (!r->prompt.token) r->prompt.token = next_token++;
            return r;
        }
        uint8_t current = mask_of(&r->process->permission_user);
        r->remaining &= ~current;
        r->remaining &= session_mask_locked();
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
        if (r->prompt.special_id)
        {
            r->special_answered = true;
            r->role_approved = approved;
            r->prompt.token = 0;
            spin_unlock(&permission_lock);
            return;
        }
        if (approved)
        {
            uint8_t granted = r->prompt.asking & session_mask_locked();
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
    special_permissions_store_exit(task->parent_group);
    user_auth_process_exit(task->parent_group);
    vfs_node_t dialog_reader = __atomic_exchange_n(&task->permission_dialog_reader, (vfs_node_t)NULL, __ATOMIC_ACQ_REL);
    if (dialog_reader) vfs_close(dialog_reader);
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

static int request_permissions(uint8_t target, UserType type, bool individual, const char *resource = NULL)
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
    if (resource) strncpy(r->prompt.resource, resource, sizeof(r->prompt.resource) - 1);

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
    UserType session_type = session_type_locked();
    if (!individual && process->permission_user.user_type != XUT_Custom &&
        session_type != XUT_Custom && type < session_type)
    {
        spin_unlock(&permission_lock);
        free(r);
        return missing ? missing : -EACCES;
    }
    r->remaining = missing & session_mask_locked();
    r->role_confirmation = !individual && process->permission_user.user_type != XUT_Custom &&
        type < process->permission_user.user_type && !missing;
    // Equal permission sets do not bypass the named user's grade ceiling.
    if (r->role_confirmation && session_type != XUT_Custom && type < session_type)
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

int permissions_request_special(uint64_t spid)
{
    if (spid < XSPT_ReadPicture || spid > XSPT_DeletePicture) return -EINVAL;
    tcb_t task = get_current_task();
    if (!task || !task->parent_group || task->task_level != TASK_APPLICATION_LEVEL) return -EPERM;
    pcb_t process = task->parent_group;
    PermissionRequest *r = (PermissionRequest *)calloc(1, sizeof(PermissionRequest));
    if (!r) return -ENOMEM;
    r->owner = task;
    r->process = process;
    r->special_result = -ESTALE;
    r->prompt.pid = process->pid;
    r->prompt.ppid = process->ppid;
    r->prompt.tid = task->tid;
    r->prompt.special_id = spid;
    spin_lock(&create_thread_lock);
    size_t length = process->exe_path ? strlen(process->exe_path) : 0;
    if (length && length < sizeof(r->special_path)) memcpy(r->special_path, process->exe_path, length + 1);
    strncpy(r->prompt.name, process->name, sizeof(r->prompt.name) - 1);
    r->revision = __atomic_load_n(&process->permission_revision, __ATOMIC_ACQUIRE);
    spin_unlock(&create_thread_lock);
    if (!length || length >= sizeof(r->special_path)) { free(r); return -ENAMETOOLONG; }
    strncpy(r->prompt.path, r->special_path, sizeof(r->prompt.path) - 1);
    spin_lock(&permission_lock);
    strncpy(r->prompt.user, process->permission_user.name, sizeof(r->prompt.user) - 1);
    r->prompt.current_type = process->permission_user.user_type;
    r->prompt.target_type = r->prompt.current_type;
    r->session = session_generation;
    spin_unlock(&permission_lock);
    int saved = special_permissions_get(r->prompt.user, r->special_path, spid);
    if (saved) { free(r); return saved < 0 ? saved : saved == 2 ? 0 : -EACCES; }
    snprintf(r->prompt.resource, sizeof(r->prompt.resource), "/users/%s/picture", r->prompt.user);
    spin_lock(&permission_lock);
    if (!backend_available || !valid_locked(r) || !user_session_is_ready() ||
        !current_user || strcmp(current_user->name, r->prompt.user))
    { spin_unlock(&permission_lock); free(r); return -EACCES; }
    if (tail) tail->next = r;
    else requests = r;
    tail = r;
    __atomic_add_fetch(&outstanding_requests, 1U, __ATOMIC_RELEASE);
    spin_unlock(&permission_lock);
    // Recheck after publication, without nesting cache_lock in permission_lock.
    // A concurrent leader either sees this waiter or has already saved a result.
    saved = special_permissions_cached(r->prompt.user, r->special_path, spid);
    spin_lock(&permission_lock);
    if (saved) { r->done = true; r->special_result = saved == 2 ? 0 : -EACCES; }
    r->special_ready = true;
    spin_unlock(&permission_lock);
    for (;;)
    {
        spin_lock(&permission_lock);
        first_locked();
        if (r->done)
        {
            int result = r->special_result;
            unlink_locked(r);
            spin_unlock(&permission_lock);
            free(r);
            return result;
        }
        bool answered = r->special_answered, approved = r->role_approved;
        spin_unlock(&permission_lock);
        if (answered)
        {
            int result = special_permissions_save(r->prompt.user, r->special_path, spid, approved);
            spin_lock(&permission_lock);
            for (PermissionRequest *waiter = requests; waiter; waiter = waiter->next)
            {
                if (waiter->prompt.special_id == spid && waiter->session == r->session &&
                    !strcmp(waiter->prompt.user, r->prompt.user) && !strcmp(waiter->special_path, r->special_path))
                {
                    waiter->special_result = valid_locked(waiter) ? result : -ESTALE;
                    waiter->done = true;
                    waiter->prompt.token = 0;
                }
            }
            spin_unlock(&permission_lock);
        }
        else scheduler_sleep_ns(10000000ULL);
    }
}

static bool prefix_fold(const char *path, const char *prefix)
{
    for (size_t i = 0; prefix[i]; ++i)
    {
        unsigned char a = path[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (!a || a != b) return false;
    }
    return true;
}

static bool equal_fold(const char *a, const char *b)
{
    return prefix_fold(a, b) && strlen(a) == strlen(b);
}

static bool under(const char *path, const char *directory)
{
    size_t n = strlen(directory);
    if (!n) return false;
    if (n == 1 && directory[0] == '/') return path[0] == '/';
    return prefix_fold(path, directory) && (path[n] == '/' || !path[n]);
}

static PermissionFolderPolicy folder_policy;

void permissions_set_folder_policy(PermissionFolderPolicy policy)
{
    __atomic_store_n(&folder_policy, policy, __ATOMIC_RELEASE);
}

int permissions_file_error()
{
    tcb_t task = get_current_task();
    return task ? task->file_access_error : 0;
}

void permissions_clear_file_error()
{
    tcb_t task = get_current_task();
    if (task) task->file_access_error = 0;
}

void permissions_set_file_error(int error)
{
    tcb_t task = get_current_task();
    if (task) task->file_access_error = error;
}

PermissionProfileScope::PermissionProfileScope(const char *username) : task(get_current_task()), previous(NULL), owner{}
{
    if (!task) return;
    if (username && username[0] && strlen(username) < sizeof(owner) &&
        !strchr(username, '/') && !strchr(username, '\\') && strcmp(username, ".") && strcmp(username, ".."))
        strcpy(owner, username);
    previous = task->permission_profile_owner;
    task->permission_profile_owner = owner;
}

PermissionProfileScope::~PermissionProfileScope()
{
    if (task) task->permission_profile_owner = previous;
}

PermissionDialogLaunchScope::PermissionDialogLaunchScope() : task(get_current_task())
{
    if (task) ++task->permission_dialog_launch_depth;
}

PermissionDialogLaunchScope::~PermissionDialogLaunchScope()
{
    if (task) --task->permission_dialog_launch_depth;
}

PermissionDialogReaderScope::PermissionDialogReaderScope(vfs_node_t reader) : task(get_current_task())
{
    __atomic_store_n(&task->permission_dialog_reader, reader, __ATOMIC_RELEASE);
}

PermissionDialogReaderScope::~PermissionDialogReaderScope()
{
    vfs_node_t reader = __atomic_exchange_n(&task->permission_dialog_reader, (vfs_node_t)NULL, __ATOMIC_ACQ_REL);
    if (reader) vfs_close(reader);
}

PermissionTerminalLaunchScope::PermissionTerminalLaunchScope(bool enabled) :
    task(get_current_task()), previous(task && task->permission_terminal_launch), active(task && enabled)
{
    if (active) task->permission_terminal_launch = true;
}

PermissionTerminalLaunchScope::~PermissionTerminalLaunchScope()
{
    finish();
}

void PermissionTerminalLaunchScope::finish()
{
    if (!active) return;
    task->permission_terminal_launch = previous;
    active = false;
}

bool permissions_dialog_metadata(vfs_node_t node, int operation)
{
    tcb_t task = get_current_task();
    return task && task->parent_group &&
        __atomic_load_n(&task->parent_group->permission_file_dialog, __ATOMIC_ACQUIRE) &&
        (operation == IME_FILE_LOOKUP || operation == IME_FILE_LIST || operation == IME_FILE_STAT ||
         (operation == IME_FILE_READ && node && (node->type & file_dir)));
}

int permissions_executable_directory(vfs_node_t executable, char *out, size_t capacity)
{
    if (!executable || !out || !capacity) return -EINVAL;
    char *path = vfs_get_fullpath(executable);
    if (!path) return -ENOMEM;
    char *slash = strrchr(path, '/');
    if (!slash) { free(path); return -EINVAL; }
    size_t length = slash == path ? 1 : (size_t)(slash - path);
    if (length >= capacity) { free(path); return -ENAMETOOLONG; }
    memcpy(out, path, length);
    out[length] = 0;
    free(path);
    return 0;
}

static bool shared_resource_path(const char *path)
{
    return prefix_fold(path, "/system/resources");
}

static bool public_read_path(const char *path)
{
    // Native interpreters and shared libraries are readable system runtime
    // files. Their writes still require the ordinary protected-system policy.
    return shared_resource_path(path) || under(path, "/system/lib") || under(path, "/etc");
}

static bool system_path(const char *path)
{
    // Shared resources do not inherit SV/SR/SW from their /system parent.
    return !shared_resource_path(path) && (under(path, "/system") || under(path, "/EFI"));
}

static bool account_store_path(const char *path, bool ancestors)
{
    return equal_fold(path, "/system/config/usereg.dat") ||
           equal_fold(path, "/system/config/usereg.dat.pending") ||
           equal_fold(path, "/system/config/usereg.dat.commit") ||
           (ancestors && (equal_fold(path, "/system/config") || equal_fold(path, "/system") || equal_fold(path, "/")));
}

static bool network_store_path(const char *path, bool ancestors)
{
    return under(path, "/config/network") ||
           (ancestors && (equal_fold(path, "/config") || equal_fold(path, "/")));
}

static bool path_access(const char *path, bool directory, int operation, uint8_t mask,
                        const char *user, const char *application, pcb_t process = NULL, uint8_t *required_out = NULL)
{
    if (operation == IME_FILE_EXPORT)
    {
        // Cross-category moves expose directory contents. SV only exposes
        // names; use the same data-read rule as the transfer guard below.
        operation = IME_FILE_READ;
        directory = false;
    }

    if (operation == IME_FILE_LOOKUP) return true; // Internal name lookup exposes no contents or listing.
    if (account_store_path(path, directory && (operation == IME_FILE_REMOVE || operation == IME_FILE_EXPORT))) return false;
    if (operation == IME_FILE_MOUNT) return false;
    int special = special_permissions_access(process, user, path, operation);
    if (special)
    {
        if (special < 0) permissions_set_file_error(-special);
        return special > 0;
    }
    bool write = operation == IME_FILE_WRITE || operation == IME_FILE_REMOVE || operation == IME_FILE_READ_WRITE;
    bool read = operation == IME_FILE_READ || operation == IME_FILE_READ_WRITE;
    bool metadata = operation == IME_FILE_STAT || operation == IME_FILE_LIST;
    if (!write && !read && !metadata) return false;
    // The native CS-checked service serializes its two snapshots. Ordinary
    // writes must not race its commit marker or create writable aliases, even
    // when the application has broad NV/CS file permissions.
    if (write && network_store_path(path, directory && operation == IME_FILE_REMOVE)) return false;

    // Every application can read shared assets, runtime libraries and /etc configuration, including
    // Custom with no file permissions. Resolved VFS operations and preflight use
    // the same rule; writes and deletion still use the ordinary policy.
    if (!write && public_read_path(path)) return true;

    if (under(path, "/users"))
    {
        const char *owner = path + strlen("/users/");
        if (strlen(path) > strlen("/users/"))
        {
            const char *slash = strchr(owner, '/');
            if (slash && equal_fold(slash, "/settings.json"))
            {
                size_t length = (size_t)(slash - owner);
                // Neither NV nor a broad executable directory may authorize another account's settings.
                if (strlen(user) != length || !prefix_fold(owner, user)) return false;
                // CS controls changes; reading the process owner's preferences
                // must not prompt for permission just to render an application.
                if (!write) return true;
                if (required_out) { *required_out |= XPERM_CS; return true; }
                return (mask & XPERM_CS) != 0;
            }
        }
    }
    uint8_t required;
    if (system_path(path))
    {
        required = metadata || (directory && read) ? (uint8_t)XPERM_SV :
            (uint8_t)((read ? XPERM_SR : 0) | (write ? XPERM_SW : 0));
        if (directory && write) required |= XPERM_SW;
    }
    else if (under(path, application))
        required = (write ? XPERM_W : 0) | ((read || metadata) ? XPERM_R : 0);
    else
    {
        PermissionFolderPolicy policy = __atomic_load_n(&folder_policy, __ATOMIC_ACQUIRE);
        PermissionFolderDecision decision = policy ? policy(process, path, operation) : XFD_DEFAULT;
        if (decision == XFD_ALLOW) return true;
        if (decision == XFD_DENY || decision == XFD_CONFIRM)
        {
            permissions_set_file_error(decision == XFD_CONFIRM ? EAGAIN : EACCES);
            return false;
        }
        required = XPERM_NV;
    }
    if (required_out) { *required_out |= required; return true; }
    return (mask & required) == required;
}

static char *canonical_target(const char *path)
{
    // Resolve the existing parent before authorizing creation/rename. A symlink
    // below an ordinary directory must not disguise an EFI/system destination.
    char *absolute = vfs_cwd_path_build((char *)path);
    if (!absolute) return NULL;
    char *slash = strrchr(absolute, '/');
    if (!slash || slash == absolute || !slash[1]) return absolute;
    char *leaf = strdup(slash + 1);
    if (!leaf) { free(absolute); return NULL; }
    *slash = 0;
    vfs_node_t parent = vfs_open(absolute);
    *slash = '/';
    if (!parent) { free(leaf); return absolute; }
    char *resolved = vfs_get_fullpath(parent);
    vfs_close(parent);
    if (!resolved) { free(leaf); free(absolute); return NULL; }
    size_t size = strlen(resolved) + strlen(leaf) + 2;
    char *joined = (char *)malloc(size);
    if (joined) snprintf(joined, size, "%s/%s", resolved, leaf);
    char *normalized = joined ? normalize_path(joined) : NULL;
    free(joined);
    free(resolved);
    free(leaf);
    free(absolute);
    return normalized;
}

static bool file_access(vfs_node_t node, const char *path, int operation, unsigned link_depth,
                         uint8_t *required_out = NULL)
{
    tcb_t task = get_current_task();
    if (!task || task->task_level != TASK_APPLICATION_LEVEL) return true;
    if (operation == IME_FILE_LOOKUP) return true;
    if (permissions_dialog_metadata(node, operation)) return true;
    if (operation == IME_FILE_MOUNT) { permissions_set_file_error(EACCES); return false; }
    // Filesystem callbacks already authorized the file operation. Their scoped
    // backend block I/O must not additionally require NV on /dev or /disk.
    // A direct application device access has no storage scope and stays denied
    // by the normal category policy and IME raw-device guard.
    if (node && (node->type & file_block) && task->ime_storage_depth) return true;
    // A hard link cannot turn a protected file into an ordinary writable file.
    // Unlinking removes only the directory entry, so it checks that entry alone.
    if (node && node->linkto && !(node->type & file_symlink) && operation != IME_FILE_REMOVE)
    {
        if (link_depth >= 40 || !file_access(node->linkto, NULL, operation, link_depth + 1, required_out))
        {
            permissions_set_file_error(EACCES);
            return false;
        }
    }
    if (task->permission_profile_owner)
    {
        // Authenticate first, then authorize only these fixed destinations.
        // Resolved nodes keep a settings symlink from reaching system/other-user data.
        char *resolved = node ? vfs_get_fullpath(node) : path ? canonical_target(path) : NULL;
        char base[96];
        snprintf(base, sizeof(base), "/users/%s", task->permission_profile_owner);
        const char *leaf = resolved && under(resolved, base) ? resolved + strlen(base) : NULL;
        bool allowed = resolved && task->permission_profile_owner[0] &&
            (equal_fold(resolved, "/users") || (leaf && (!leaf[0] || equal_fold(leaf, "/desktop") ||
                equal_fold(leaf, "/picture") ||
                equal_fold(leaf, "/settings.json") || equal_fold(leaf, "/settings.dat") ||
                equal_fold(leaf, "/runfile.dat"))) ||
             (equal_fold(resolved, "/system/config/language.dat") &&
                (operation == IME_FILE_READ || operation == IME_FILE_STAT)));
        free(resolved);
        if (!allowed) permissions_set_file_error(EACCES);
        return allowed;
    }
    if (task->ime_config_depth) return true;
    if (!path && node && node != rootdir &&
        (node->type & (file_pipe | file_socket | file_stream | file_ptmx | file_pts)))
    {
        // Anonymous IPC endpoints are detached from the
        // mounted namespace. Named FIFOs/devices still use their real category.
        vfs_node_t ancestor = node;
        while (ancestor->parent) ancestor = ancestor->parent;
        if (ancestor != rootdir) return true;
    }
    char username[64], application[4096];
    spin_lock(&permission_lock);
    pcb_t process = task->parent_group;
    uint8_t mask = mask_of(&process->permission_user);
    memcpy(username, process->permission_user.name, sizeof(username));
    memcpy(application, process->permission_directory, sizeof(application));
    spin_unlock(&permission_lock);
    username[sizeof(username) - 1] = 0;
    application[sizeof(application) - 1] = 0;
    char *resolved = node ? vfs_get_fullpath(node) : path ? canonical_target(path) : NULL;
    if (!resolved) { permissions_set_file_error(EACCES); return false; }
    // Fixed runtime image loading has read-only exceptions. The picker never
    // receives permission to read selected data.
    bool service_read = operation == IME_FILE_READ &&
        ((task->permission_dialog_launch_depth && equal_fold(resolved, XJ380_FILE_DIALOG_PATH)) ||
         (task->permission_terminal_launch && equal_fold(resolved, XJ380_TERMINAL_PATH)));
    bool allowed = service_read || path_access(resolved, node && (node->type & file_dir), operation, mask, username,
                                               application, process, required_out);
    free(resolved);
    if (!allowed && !permissions_file_error()) permissions_set_file_error(EACCES);
    return allowed;
}

bool permissions_file_access(vfs_node_t node, const char *path, int operation)
{
    return file_access(node, path, operation, 0);
}

bool permissions_file_requirements(vfs_node_t node, const char *path, int operation, uint8_t *required)
{
    return required && file_access(node, path, operation, 0, required);
}

PermissionFileChange permissions_file_change_begin(vfs_node_t node, const char *destination)
{
    PermissionFileChange change = {};
    if (!permissions_settings_write_notice || !node ||
        (node->type & (file_dir | file_block | file_pipe | file_socket | file_stream | file_ptmx | file_pts)))
        return change;
    tcb_t task = get_current_task();
    // Authenticated profile creation/migration is kernel maintenance, rather
    // than a setting edit by the application that happened to log the user in.
    if (!task || task->task_level != TASK_APPLICATION_LEVEL || !task->parent_group ||
        task->ime_config_depth || task->permission_profile_owner) return change;

    char owner[64], application[32];
    spin_lock(&permission_lock);
    pcb_t process = task->parent_group;
    memcpy(owner, process->permission_user.name, sizeof(owner));
    memcpy(application, process->name, sizeof(application));
    uint64_t pid = process->pid;
    spin_unlock(&permission_lock);
    owner[sizeof(owner) - 1] = application[sizeof(application) - 1] = 0;
    char expected[96];
    snprintf(expected, sizeof(expected), "/users/%s/settings.json", owner);

    int saved_error = permissions_file_error();
    bool changed = false;
    if (destination)
    {
        // FAT rename need not republish the cached source node's new name.
        // Resolve the destination before mutation and retain that identity.
        char *target = canonical_target(destination);
        char *source = vfs_get_fullpath(node);
        changed = target && source && equal_fold(target, expected) && !equal_fold(target, source);
        free(target);
        free(source);
    }
    else
    {
        for (unsigned depth = 0; node && depth < 40; ++depth, node = node->linkto)
        {
            char *path = vfs_get_fullpath(node);
            changed = path && !(node->type & file_dir) && equal_fold(path, expected);
            free(path);
            if (changed) break;
        }
    }
    if (changed)
    {
        change.pid = pid;
        memcpy(change.application, application, sizeof(change.application));
    }
    permissions_set_file_error(saved_error);
    return change;
}

void permissions_file_changed(const PermissionFileChange &change)
{
    if (!change.pid || !permissions_settings_write_notice) return;
    int saved_error = permissions_file_error();
    // Queue admission never changes a completed write's result or errno.
    permissions_settings_write_notice(change.application, change.pid);
    permissions_set_file_error(saved_error);
}

static bool query_operations(vfs_node_t node, const char *path, uint64_t access, uint8_t *required)
{
    if ((access & XFA_READ) &&
        !ime_vfs_requirements(node, path, access & XFA_EXPORT ? IME_FILE_EXPORT : IME_FILE_READ, required)) return false;
    if ((access & XFA_WRITE) && !ime_vfs_requirements(node, path, IME_FILE_WRITE, required)) return false;
    if ((access & XFA_LIST) && !ime_vfs_requirements(node, path, IME_FILE_LIST, required)) return false;
    if ((access & XFA_STAT) && !ime_vfs_requirements(node, path, IME_FILE_STAT, required)) return false;
    if ((access & XFA_REMOVE) && !ime_vfs_requirements(node, path, IME_FILE_REMOVE, required)) return false;
    return true;
}

int permissions_query_file_access(const char *path, uint64_t access, char *resolved, size_t capacity)
{
    if (!path || !path[0] || !(access & 63) || (access & ~511ULL) ||
        ((access & XFA_CREATE) && !(access & XFA_WRITE)) ||
        ((access & XFA_EXPORT) && !(access & XFA_READ))) return -EINVAL;
    permissions_clear_file_error();
    char *absolute = vfs_cwd_path_build((char *)path);
    if (!absolute) return -ENOMEM;
    vfs_node_t node = access & (XFA_NOFOLLOW | XFA_REMOVE) ? vfs_open_no_follow(absolute) : vfs_open(absolute);
    uint8_t required = 0;
    bool allowed = true;
    int error = permissions_file_error();
    char *canonical = node ? vfs_get_fullpath(node) : NULL;
    if (node)
    {
        if ((access & (XFA_CREATE | XFA_EXCLUSIVE)) == (XFA_CREATE | XFA_EXCLUSIVE))
        {
            allowed = false;
            error = EEXIST;
        }
        else allowed = canonical && query_operations(node, NULL, access, &required);
        vfs_close(node);
        if (!canonical) error = ENOMEM;
    }
    else if (!(access & XFA_CREATE) || (error && error != ENOENT))
    {
        allowed = false;
        if (!error) error = ENOENT;
    }
    else
    {
        error = 0;
        permissions_clear_file_error();
        // Discover the nearest existing ancestor first. Resolve its aliases
        // before classifying every missing component; the query creates nothing.
        char *ancestor = strdup(absolute);
        vfs_node_t parent = NULL;
        size_t prefix = 0;
        if (!ancestor) { allowed = false; error = ENOMEM; }
        while (ancestor && !parent)
        {
            char *slash = strrchr(ancestor, '/');
            if (!slash) break;
            if (slash == ancestor) ancestor[1] = 0;
            else *slash = 0;
            parent = vfs_open(ancestor);
            if (permissions_file_error() && permissions_file_error() != ENOENT) break;
            if (!parent) permissions_clear_file_error();
            if (ancestor[1] == 0) break;
        }
        if (!parent)
        {
            allowed = false;
            if (!error) error = permissions_file_error() ? permissions_file_error() : ENOENT;
        }
        else if (!(parent->type & file_dir)) { allowed = false; error = ENOTDIR; }
        else
        {
            char *base = vfs_get_fullpath(parent);
            prefix = strlen(ancestor);
            const char *suffix = absolute + prefix;
            size_t bytes = base ? strlen(base) + strlen(suffix) + 2 : 0;
            char *joined = base ? (char *)malloc(bytes) : NULL;
            if (joined)
            {
                snprintf(joined, bytes, "%s/%s", base, suffix);
                canonical = normalize_path(joined);
            }
            free(joined);
            if (!canonical) { allowed = false; error = ENOMEM; }
            // Creating a child needs its parent's capability category, but
            // does not replace the existing parent's protected IME anchor.
            // An album grant authorizes creating the album itself without
            // requiring ordinary NV on its existing home directory.
            const char *owner = get_current_task()->parent_group->permission_user.name;
            bool album = canonical && special_permissions_path_mask(canonical, IME_FILE_WRITE, owner);
            if (allowed && !album) allowed = permissions_file_requirements(parent, NULL, IME_FILE_WRITE, &required);
            // Include each directory that mkdir/mkfile would have to create.
            size_t base_length = base ? strlen(base) : 0;
            for (char *p = canonical ? canonical + base_length : NULL; allowed && p && *p; ++p)
            {
                if (*p != '/' || p == canonical || p == canonical + base_length) continue;
                char saved = *p;
                *p = 0;
                allowed = ime_vfs_requirements(NULL, canonical, IME_FILE_WRITE, &required);
                *p = saved;
            }
            if (allowed) allowed = query_operations(NULL, canonical, access, &required);
            free(base);
        }
        if (parent) vfs_close(parent);
        free(ancestor);
    }
    if (resolved && capacity)
    {
        const char *target = canonical ? canonical : absolute;
        size_t length = strlen(target);
        if (length >= capacity && capacity >= 8)
        {
            size_t head = (capacity - 4) / 2, tail_length = capacity - 4 - head;
            memcpy(resolved, target, head);
            memcpy(resolved + head, "...", 3);
            memcpy(resolved + head + 3, target + length - tail_length, tail_length);
        }
        else strncpy(resolved, target, capacity - 1);
        resolved[capacity - 1] = 0;
    }
    free(canonical);
    free(absolute);
    if (!allowed) return -(error ? error : permissions_file_error() ? permissions_file_error() : EACCES);
    return required & ~permissions_current_mask();
}

int permissions_request_file_access(const char *path, uint64_t access, uint8_t offered)
{
    char resolved[256];
    int missing = permissions_query_file_access(path, access, resolved, sizeof(resolved));
    if (missing == -EACCES)
    {
        tcb_t task = get_current_task();
        char user[64];
        spin_lock(&permission_lock);
        memcpy(user, task->parent_group->permission_user.name, sizeof(user));
        spin_unlock(&permission_lock);
        user[sizeof(user) - 1] = 0;
        // Query already resolved aliases and missing parents. Its shortened
        // display retains at least 126 leading bytes, enough for the complete
        // /users/<63-byte username>/picture boundary. Never reclassify a raw
        // pathname that could identify a different symlink destination.
        uint64_t special = 0;
        if (resolved[0])
        {
            if (access & (XFA_READ | XFA_LIST | XFA_STAT))
                special |= special_permissions_path_mask(resolved, IME_FILE_READ, user);
            if (access & (XFA_WRITE | XFA_CREATE))
                special |= special_permissions_path_mask(resolved, IME_FILE_WRITE, user);
            if (access & XFA_REMOVE) special |= special_permissions_path_mask(resolved, IME_FILE_REMOVE, user);
        }
        if (special && !(special & ~XSP_KNOWN_MASK))
        {
            for (uint64_t id = XSPT_ReadPicture; id <= XSPT_DeletePicture; ++id)
                if (special & (1ULL << id))
                {
                    int result = permissions_request_special(id);
                    if (result) return result;
                }
            missing = permissions_query_file_access(path, access, resolved, sizeof(resolved));
        }
    }
    if (missing <= 0) return missing;
    uint8_t asking = (uint8_t)missing & offered;
    if (asking)
    {
        int result = request_permissions(asking, XUT_Custom, true, resolved);
        if (result < 0) return result;
    }
    // A selected path may have been renamed or retargeted while the user was
    // deciding. Revalidate without widening the set offered by this operation.
    return permissions_query_file_access(path, access);
}

static int transfer_category(const char *path, const char *application)
{
    if (special_permissions_path_mask(path, IME_FILE_READ, NULL)) return 4;
    if (prefix_fold(path, "/users/"))
    {
        const char *slash = strchr(path + strlen("/users/"), '/');
        if (slash && equal_fold(slash, "/settings.json")) return 3;
    }
    // Public reads are a separate category: a persistent hard link from an
    // ordinary NV/application file must not make that file universally readable.
    if (public_read_path(path)) return 5;
    return system_path(path) ? 0 : under(path, application) ? 1 : 2;
}

static bool file_transfer(vfs_node_t node, const char *destination, bool hard_link)
{
    permissions_clear_file_error();
    tcb_t task = get_current_task();
    if (!task || task->task_level != TASK_APPLICATION_LEVEL || task->ime_config_depth) return true;
    if (!node || !destination) return false;
    char *source = vfs_get_fullpath(node);
    char *target = canonical_target(destination);
    if (!source || !target)
    {
        free(source);
        free(target);
        permissions_set_file_error(EACCES);
        return false;
    }
    // Account directory names are part of the CS boundary. Moving an ancestor
    // must not make another account's settings.json accessible through NV.
    bool account_root = equal_fold(source, "/users") ||
        (prefix_fold(source, "/users/") && !strchr(source + strlen("/users/"), '/'));
    bool allowed = !((node->type & file_dir) && account_root) &&
        !special_permissions_store_path(source, true) && !special_permissions_store_path(target, true) &&
        !network_store_path(source, true) && !network_store_path(target, true) &&
        !account_store_path(source, true) && !account_store_path(target, true);
    char application[4096], username[64];
    spin_lock(&permission_lock);
    pcb_t process = task->parent_group;
    uint8_t mask = mask_of(&process->permission_user);
    memcpy(application, process->permission_directory, sizeof(application));
    memcpy(username, process->permission_user.name, sizeof(username));
    spin_unlock(&permission_lock);
    application[sizeof(application) - 1] = 0;
    username[sizeof(username) - 1] = 0;
    if (allowed)
    {
        int source_access = special_permissions_access(process, username, source,
                                                       hard_link ? IME_FILE_READ : IME_FILE_REMOVE);
        int target_access = special_permissions_access(process, username, target, IME_FILE_WRITE);
        allowed = source_access >= 0 && target_access >= 0;
    }
    int from = transfer_category(source, application);
    int to = transfer_category(target, application);
    if (allowed && from != to)
    {
        // A persistent hard link would lose its source path on remount. Keep
        // links within a category until the filesystem can persist that policy.
        if (hard_link) allowed = false;
        // A move across categories exports the data, just like a copy. For a
        // system directory this needs SR; SV alone only reveals its listing.
        else allowed = path_access(source, false, IME_FILE_READ, mask, username, application, process);
    }
    free(source);
    free(target);
    if (!allowed && !permissions_file_error()) permissions_set_file_error(EACCES);
    return allowed;
}

bool permissions_file_move(vfs_node_t node, const char *destination)
{
    return file_transfer(node, destination, false);
}

bool permissions_file_link(vfs_node_t node, const char *destination)
{
    return file_transfer(node, destination, true);
}
