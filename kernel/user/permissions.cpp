#include <user/permissions.h>
#include <user/process_launch.h>
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

static bool system_path(const char *path)
{
    // The requested resources prefix is ordinary data, even though it lives
    // below /system. No SV/SR/SW requirements are inherited from that parent.
    return !prefix_fold(path, "/system/resources") && (under(path, "/system") || under(path, "/EFI"));
}

static bool path_access(const char *path, bool directory, int operation, uint8_t mask,
                        const char *user, const char *application, pcb_t process = NULL)
{
    if (operation == IME_FILE_LOOKUP) return true; // Internal name lookup exposes no contents or listing.
    if (operation == IME_FILE_MOUNT) return false;
    bool write = operation == IME_FILE_WRITE || operation == IME_FILE_REMOVE || operation == IME_FILE_READ_WRITE;
    bool read = operation == IME_FILE_READ || operation == IME_FILE_READ_WRITE;
    bool metadata = operation == IME_FILE_STAT || operation == IME_FILE_LIST;
    if (!write && !read && !metadata) return false;

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
                return strlen(user) == length && prefix_fold(owner, user) && (mask & XPERM_CS);
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

static bool file_access(vfs_node_t node, const char *path, int operation, unsigned link_depth)
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
        if (link_depth >= 40 || !file_access(node->linkto, NULL, operation, link_depth + 1))
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
    // Fixed runtime image loading and picker icons have read-only exceptions.
    // The picker never receives permission to read selected data.
    bool service_read = operation == IME_FILE_READ &&
        ((task->permission_dialog_launch_depth && equal_fold(resolved, XJ380_FILE_DIALOG_PATH)) ||
         (task->permission_terminal_launch && equal_fold(resolved, XJ380_TERMINAL_PATH)) ||
         (__atomic_load_n(&process->permission_file_dialog, __ATOMIC_ACQUIRE) &&
          under(resolved, "/system/resources")));
    bool allowed = service_read || path_access(resolved, node && (node->type & file_dir), operation, mask, username,
                                               application, process);
    free(resolved);
    if (!allowed && !permissions_file_error()) permissions_set_file_error(EACCES);
    return allowed;
}

bool permissions_file_access(vfs_node_t node, const char *path, int operation)
{
    return file_access(node, path, operation, 0);
}

static int transfer_category(const char *path, const char *application)
{
    if (prefix_fold(path, "/users/"))
    {
        const char *slash = strchr(path + strlen("/users/"), '/');
        if (slash && equal_fold(slash, "/settings.json")) return 3;
    }
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
    bool allowed = !((node->type & file_dir) && account_root);
    char application[4096], username[64];
    spin_lock(&permission_lock);
    pcb_t process = task->parent_group;
    uint8_t mask = mask_of(&process->permission_user);
    memcpy(application, process->permission_directory, sizeof(application));
    memcpy(username, process->permission_user.name, sizeof(username));
    spin_unlock(&permission_lock);
    application[sizeof(application) - 1] = 0;
    username[sizeof(username) - 1] = 0;
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
