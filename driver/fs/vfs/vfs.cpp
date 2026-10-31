#include <user/permissions.h>
#include <input_method.h>
/**
 * Virtual File System Interface
 * 虚拟文件系统接口
 * Create by min0911Y & zhouzhihao & copi143
 */
#define ALL_IMPLEMENTATION

#include "fs/vfs/vfs.h"
#include "errno.h"
// #include "iso9660.h"
#include "krlibc.h"
#include "fs/vfs/list.h"
#include "llist.h"
#include "pipe.h"
#include "rtc.h"
#include "task/pcb.h"
#include <ioctl.h>
#include <cpu/lock.h>
// #include "pipefs.h"
#include <syscall/syscall.h>
#include <syscall/fd_lifetime.h>
#include <device.h>
#include <task/poll.h>
#include <rng.h>

// Optional filesystem properties stay outside vfs_callback's module ABI.
static bool fs_readonly[256];
static bool fs_managed_lifetime[256];
static bool fs_unmount_quiescence[256];
static vfs_prepare_unmount_t fs_prepare_unmount[256];
static vfs_checked_unmount_t fs_checked_unmount[256];
static vfs_link_node_t fs_link_node[256];
static vfs_finalize_node_t fs_finalize_node[256];
static bool fs_unlinked_lifetime[256];
static bool fs_reject_overmount[256];
// Protected by vfs_child_list_lock; lookups cannot enter a retiring managed mount.
static vfs_node_t readonly_unmounting[256];
static unsigned readonly_traversals[256];
static unsigned path_traversals;
static unsigned creation_traversals;
// Short-lived subtree retirement records. The child lock protects both these
// records and the cache/tree references; no callback runs while it is held.
static vfs_node_t retiring_subtrees[64];
struct DetachedRetention
{
    vfs_node_t node;
    vfs_node_t mount;
};
static DetachedRetention detached_retentions[64];
static bool detached_retention_overflow;

bool vfs_node_retiring_locked(vfs_node_t node)
{
    for (unsigned depth = 0; node; node = node->parent, ++depth)
    {
        if (depth > 128) return true;
        if (node->fsid < 256 && readonly_unmounting[node->fsid] == node->root) return true;
        for (vfs_node_t retiring : retiring_subtrees) if (retiring == node) return true;
    }
    return false;
}

void vfs_set_readonly(int fsid, bool readonly)
{
    if (fsid > 0 && fsid < 256) __atomic_store_n(&fs_readonly[fsid], readonly, __ATOMIC_RELEASE);
}

bool vfs_is_readonly(vfs_node_t node)
{
    return node && node->fsid < 256 && __atomic_load_n(&fs_readonly[node->fsid], __ATOMIC_ACQUIRE);
}

void vfs_set_managed_lifetime(int fsid, bool enabled)
{
    if (fsid > 0 && fsid < 256) __atomic_store_n(&fs_managed_lifetime[fsid], enabled, __ATOMIC_RELEASE);
}

bool vfs_uses_managed_lifetime(vfs_node_t node)
{
    return node && node->fsid < 256 &&
           (__atomic_load_n(&fs_readonly[node->fsid], __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&fs_managed_lifetime[node->fsid], __ATOMIC_ACQUIRE));
}

void vfs_set_prepare_unmount(int fsid, vfs_prepare_unmount_t callback)
{
    if (fsid > 0 && fsid < 256) __atomic_store_n(&fs_prepare_unmount[fsid], callback, __ATOMIC_RELEASE);
}

void vfs_set_unmount_quiescence(int fsid, bool enabled)
{
    if (fsid > 0 && fsid < 256) __atomic_store_n(&fs_unmount_quiescence[fsid], enabled, __ATOMIC_RELEASE);
}

void vfs_set_checked_unmount(int fsid, vfs_checked_unmount_t callback)
{
    if (fsid > 0 && fsid < 256) __atomic_store_n(&fs_checked_unmount[fsid], callback, __ATOMIC_RELEASE);
}

void vfs_set_link_node(int fsid, vfs_link_node_t callback)
{
    if (fsid > 0 && fsid < 256) __atomic_store_n(&fs_link_node[fsid], callback, __ATOMIC_RELEASE);
}

void vfs_set_finalize_node(int fsid, vfs_finalize_node_t callback)
{
    if (fsid > 0 && fsid < 256) __atomic_store_n(&fs_finalize_node[fsid], callback, __ATOMIC_RELEASE);
}

void vfs_set_unlinked_lifetime(int fsid, bool enabled)
{
    if (fsid > 0 && fsid < 256) __atomic_store_n(&fs_unlinked_lifetime[fsid], enabled, __ATOMIC_RELEASE);
}

void vfs_set_reject_overmount(int fsid, bool enabled)
{
    if (fsid > 0 && fsid < 256) __atomic_store_n(&fs_reject_overmount[fsid], enabled, __ATOMIC_RELEASE);
}

bool vfs_uses_unlinked_lifetime(vfs_node_t node)
{
    return node && node->fsid < 256 &&
           __atomic_load_n(&fs_unlinked_lifetime[node->fsid], __ATOMIC_ACQUIRE);
}

bool vfs_check_access(vfs_node_t node, const char *path, int operation)
{
    permissions_clear_file_error();
    if (!ime_vfs_access(node, path, (ImeFileOperation)operation))
    {
        if (!permissions_file_error()) permissions_set_file_error(EACCES);
        return false;
    }
    if (vfs_is_readonly(node) &&
        (operation == IME_FILE_WRITE || operation == IME_FILE_READ_WRITE || operation == IME_FILE_REMOVE))
    {
        permissions_set_file_error(EROFS);
        return false;
    }
    return true;
}

int vfs_last_error()
{
    int error = permissions_file_error();
    return error ? error : EIO;
}

static bool check_child_creation(vfs_node_t parent, const char *name)
{
    if (!parent || !name || !*name) { permissions_set_file_error(EINVAL); return false; }
    if (vfs_is_readonly(parent) && !vfs_check_access(parent, NULL, IME_FILE_WRITE)) return false;
    char *base = vfs_get_fullpath(parent);
    if (!base) { permissions_set_file_error(ENOMEM); return false; }
    size_t capacity = strlen(base) + strlen(name) + 2;
    char *path = (char *)malloc(capacity);
    if (!path) { free(base); permissions_set_file_error(ENOMEM); return false; }
    snprintf(path, capacity, "%s/%s", base, name);
    char *canonical = normalize_path(path);
    bool allowed = canonical && vfs_check_access(NULL, canonical, IME_FILE_WRITE);
    if (!canonical) permissions_set_file_error(ENOMEM);
    free(canonical);
    free(path);
    free(base);
    return allowed;
}

bool vfs_check_child_creation(vfs_node_t parent, const char *name)
{
    return check_child_creation(parent, name);
}

vfs_node_t rootdir = NULL;

tty_t *defualt_tty = NULL;
int    tty_mode = KD_TEXT;
int    tty_kbmode = K_XLATE;
struct vt_mode current_vt_mode = {0};

static void empty_func() {}

struct vfs_callback   vfs_empty_callback;
struct vfs_filesystem vfs_empty_filesystem;

vfs_callback_t fs_callbacks[256] = {
    [0] = &vfs_empty_callback,
};

extern void p_xapi_output_kernel(const char *str);

struct llist_header fs_metadata_list;

static int fs_nextid = 1;
static spin_t vfs_child_list_lock = SPIN_INIT;

#define VFS_ALIAS_MAX 256

typedef struct
{
    char *alias_path;
    char *target_path;
} vfs_alias_entry_t;//就这样吧

static vfs_alias_entry_t vfs_aliases[VFS_ALIAS_MAX];
static spin_t vfs_alias_lock = SPIN_INIT;

#define callbackof(node, _name_) (fs_callbacks[(node)->fsid]->_name_)

void vfs_child_lock() {
    spin_lock(&vfs_child_list_lock);
}

void vfs_child_unlock() {
    spin_unlock(&vfs_child_list_lock);
}

// Legacy creation paths walk cached parents without retaining each node.
// Pin managed filesystem registries for the walk, so teardown cannot free a parent
// or make an existing retiring mount look like a missing mutable directory.
class ReadonlyTraversalGuard
{
    bool pinned[256] = {};
    bool valid = false;

public:
    ReadonlyTraversalGuard()
    {
        vfs_child_lock();
        for (vfs_node_t retiring : retiring_subtrees)
        {
            if (retiring) { vfs_child_unlock(); return; }
        }
        for (unsigned i = 1; i < 256; ++i)
        {
            if (readonly_unmounting[i])
            {
                vfs_child_unlock();
                return;
            }
        }
        for (unsigned i = 1; i < 256; ++i)
        {
            if (__atomic_load_n(&fs_readonly[i], __ATOMIC_ACQUIRE) ||
                __atomic_load_n(&fs_managed_lifetime[i], __ATOMIC_ACQUIRE) ||
                __atomic_load_n(&fs_unmount_quiescence[i], __ATOMIC_ACQUIRE))
            {
                pinned[i] = true;
                ++readonly_traversals[i];
            }
        }
        ++creation_traversals;
        valid = true;
        vfs_child_unlock();
    }

    ~ReadonlyTraversalGuard()
    {
        if (!valid) return;
        vfs_child_lock();
        --creation_traversals;
        for (unsigned i = 1; i < 256; ++i)
            if (pinned[i]) --readonly_traversals[i];
        vfs_child_unlock();
    }

    bool ready() const { return valid; }
};

class PathTraversalGuard
{
public:
    PathTraversalGuard()
    {
        vfs_child_lock();
        ++path_traversals;
        vfs_child_unlock();
    }

    ~PathTraversalGuard()
    {
        vfs_child_lock();
        --path_traversals;
        vfs_child_unlock();
    }
};

int vfs_publish_readonly_mount(vfs_node_t node, const struct vfs_node *prepared)
{
    if (!node || !prepared || !vfs_uses_managed_lifetime((vfs_node_t)prepared) ||
        !prepared->handle || prepared->type != file_dir) return -EINVAL;
    vfs_child_lock();
    bool busy = path_traversals != 0 || node->is_mount || node->child ||
                node->type != file_dir || node->refcount > 2;
    for (unsigned i = 1; i < 256 && !busy; ++i)
        busy = readonly_traversals[i] != 0;
    if (!busy)
    {
        // The adapter prepared an unreachable tree. Publication is one short
        // transaction; retain the public mountpoint's identity and references.
        vfs_node_t parent = node->parent;
        char *name = node->name;
        uint32_t references = node->refcount;
        *node = *prepared;
        node->parent = parent;
        node->name = name;
        node->refcount = references;
        node->root = node;
        node->is_mount = true;
    }
    vfs_child_unlock();
    if (busy) permissions_set_file_error(EBUSY);
    return busy ? -EBUSY : 0;
}

int vfs_publish_system_root(struct vfs_node *prepared, list_t *covered)
{
    if (!rootdir || !prepared || !covered || !prepared->handle || prepared->type != file_dir ||
        !vfs_uses_managed_lifetime(prepared)) return -EINVAL;
    *covered = NULL;
    vfs_child_lock();
    bool busy = path_traversals || rootdir->type != file_dir || rootdir->is_mount || rootdir->fsid ||
                rootdir->handle || rootdir->parent;
    for (unsigned i = 1; i < 256 && !busy; ++i) busy = readonly_traversals[i] != 0;
    for (list_t fresh = prepared->child; fresh && !busy; fresh = fresh->next)
    {
        auto *node = static_cast<vfs_node_t>(fresh->data);
        busy = !node || !node->name || node->refcount != 1 || node->is_mount || node->child;
    }
    for (list_t old = rootdir->child; old && !busy; old = old->next)
    {
        auto *mount = static_cast<vfs_node_t>(old->data);
        if (!mount || !mount->is_mount || mount->parent != rootdir) { busy = true; break; }
        for (list_t fresh = prepared->child; fresh; fresh = fresh->next)
        {
            auto *node = static_cast<vfs_node_t>(fresh->data);
            if (strcmp(node->name, mount->name)) continue;
            // Only a private, unopened directory placeholder may be covered.
            if (node->type != file_dir || node->is_mount || node->child || node->refcount != 1) busy = true;
            break;
        }
    }
    if (!busy)
    {
        list_t old = rootdir->child;
        while (old)
        {
            list_t next = old->next;
            auto *mount = static_cast<vfs_node_t>(old->data);
            for (list_t fresh = prepared->child; fresh; fresh = fresh->next)
            {
                auto *node = static_cast<vfs_node_t>(fresh->data);
                if (strcmp(node->name, mount->name)) continue;
                if (fresh->prev) fresh->prev->next = fresh->next;
                else prepared->child = fresh->next;
                if (fresh->next) fresh->next->prev = fresh->prev;
                node->parent = NULL;
                fresh->prev = NULL;
                fresh->next = *covered;
                if (*covered) (*covered)->prev = fresh;
                *covered = fresh;
                break;
            }
            old->prev = NULL;
            old->next = prepared->child;
            if (old->next) old->next->prev = old;
            prepared->child = old;
            old = next;
        }
        // Keep the root vnode and all preexisting mounted children alive:
        // cwd and the block device references must retain their identities.
        char *name = rootdir->name;
        uint32_t references = rootdir->refcount;
        *rootdir = *prepared;
        rootdir->name = name;
        rootdir->parent = NULL;
        rootdir->refcount = references;
        rootdir->root = rootdir;
        rootdir->is_mount = true;
        for (list_t entry = rootdir->child; entry; entry = entry->next)
            static_cast<vfs_node_t>(entry->data)->parent = rootdir;
        prepared->child = NULL;
    }
    vfs_child_unlock();
    if (busy) permissions_set_file_error(EBUSY);
    return busy ? -EBUSY : 0;
}

namespace
{
bool boot_tree_valid(vfs_node_t node, vfs_node_t parent, vfs_node_t prepared, uint64_t expected,
                     uint64_t *count, unsigned depth)
{
    if (!node || depth > 128 || ++*count > expected || node == rootdir || node->parent != parent ||
        node->root != rootdir || node->refcount != (node == prepared ? 1U : 2U) ||
        node->fsid != prepared->fsid || !node->handle || node->is_mount || node->flags ||
        node->linkto || node->linkname || (node->type != file_dir && node->type != file_none)) return false;
    if (!node->name || (node != prepared && (!node->name[0] || strchr(node->name, '/')))) return false;
    list_t previous = NULL;
    for (list_t cell = node->child; cell; cell = cell->next)
    {
        if (node->type != file_dir || cell->prev != previous) return false;
        auto *child = static_cast<vfs_node_t>(cell->data);
        if (!child || !child->name) return false;
        for (list_t earlier = node->child; earlier != cell; earlier = earlier->next)
        {
            auto *other = static_cast<vfs_node_t>(earlier->data);
            if (other == child || !other || !other->name || !strcmp(other->name, child->name)) return false;
        }
        if (!boot_tree_valid(child, node, prepared, expected, count, depth + 1)) return false;
        previous = cell;
    }
    return true;
}
} // namespace

int vfs_publish_system_tree(vfs_node_t prepared, uint64_t expected_nodes,
                            vfs_node_t **bindings, size_t binding_count, list_t *covered)
{
    if (covered) *covered = NULL;
    if (!rootdir || !prepared || !covered || !bindings || !binding_count || binding_count > 4 ||
        !expected_nodes || expected_nodes > 32768 || prepared->type != file_dir ||
        !vfs_uses_managed_lifetime(prepared)) return -EINVAL;
    for (size_t i = 0; i < binding_count; ++i)
        if (!bindings[i] || *bindings[i] != prepared) return -EINVAL;
    // The fresh tree is exclusively boot-owned and unreachable. Validate its
    // deep shape outside the global child lock; publication checks only shared
    // root/mount state and then makes an infallible pointer-only commit.
    uint64_t count = 0;
    if (!boot_tree_valid(prepared, NULL, prepared, expected_nodes, &count, 0) || count != expected_nodes)
        return -EINVAL;
    vfs_child_lock();
    bool busy = path_traversals || creation_traversals || rootdir->type != file_dir || rootdir->is_mount ||
                rootdir->fsid || rootdir->handle || rootdir->parent;
    for (unsigned i = 1; i < 256 && !busy; ++i) busy = readonly_traversals[i] != 0;
    for (list_t old = rootdir->child; old && !busy; old = old->next)
    {
        auto *mount = static_cast<vfs_node_t>(old->data);
        if (!mount || !mount->is_mount || !mount->name || mount->parent != rootdir)
        { busy = true; break; }
        for (list_t fresh = prepared->child; fresh; fresh = fresh->next)
        {
            auto *node = static_cast<vfs_node_t>(fresh->data);
            if (strcmp(node->name, mount->name)) continue;
            if (node->type != file_dir || node->is_mount || node->child || node->refcount != 2) busy = true;
            break;
        }
    }
    if (!busy)
    {
        list_t old = rootdir->child;
        while (old)
        {
            list_t next = old->next;
            auto *mount = static_cast<vfs_node_t>(old->data);
            for (list_t fresh = prepared->child; fresh; fresh = fresh->next)
            {
                auto *node = static_cast<vfs_node_t>(fresh->data);
                if (strcmp(node->name, mount->name)) continue;
                if (fresh->prev) fresh->prev->next = fresh->next;
                else prepared->child = fresh->next;
                if (fresh->next) fresh->next->prev = fresh->prev;
                node->parent = NULL;
                fresh->prev = NULL;
                fresh->next = *covered;
                if (*covered) (*covered)->prev = fresh;
                *covered = fresh;
                break;
            }
            old->prev = NULL;
            old->next = prepared->child;
            if (old->next) old->next->prev = old;
            prepared->child = old;
            old = next;
        }
        char *name = rootdir->name;
        uint32_t references = rootdir->refcount;
        *rootdir = *prepared;
        rootdir->name = name;
        rootdir->parent = NULL;
        rootdir->refcount = references;
        rootdir->root = rootdir;
        rootdir->is_mount = true;
        for (list_t cell = rootdir->child; cell; cell = cell->next)
            static_cast<vfs_node_t>(cell->data)->parent = rootdir;
        for (size_t i = 0; i < binding_count; ++i) *bindings[i] = rootdir;
        prepared->child = NULL;
    }
    vfs_child_unlock();
    if (busy) permissions_set_file_error(EBUSY);
    return busy ? -EBUSY : 0;
}

static inline char *pathtok(char **sp) {
    char *s = *sp;
    char *e = *sp;

    // 跳过所有连续的斜杠
    while (*e == '/') {
        e++;
    }

    // 如果已经到达字符串末尾，返回 NULL
    if (*e == '\0') {
        *sp = e; // 更新指针到字符串末尾
        return NULL;
    }

    s = e; // 设置令牌起始位置（第一个非斜杠字符）

    // 查找下一个斜杠或字符串结尾
    while (*e != '\0' && *e != '/') {
        e++;
    }

    // 保存下一个令牌的起始位置
    char *next = e;
    if (*e == '/') {
        next++; // 跳过斜杠指向下一个字符
    }

    // 终止当前令牌
    if (*e != '\0') { *e = '\0'; }

    *sp = next; // 更新指针到下一个令牌位置
    return s;   // 返回当前令牌
}

static inline void do_open(vfs_node_t file) {
    if (file == NULL) return;
    uint16_t retained_state = file->type & (file_delete | file_unlinked);
    if (file->handle != NULL) {
        callbackof(file, stat)(file->handle, file);
    } else {
        if (file->parent == NULL || file->parent->handle == NULL) return;
        callbackof(file, open)(file->parent->handle, file->name, file);
    }
    file->type |= retained_state;
}

static inline void do_update(vfs_node_t file) {
    if (file == NULL) return;
    if ((file->type & file_symlink) && file->linkname != NULL) return;
    if (file->type & file_none || file->handle == NULL || file->type & file_dir ||
        file->type & file_symlink || file->type & file_pipe)
        do_open(file);
}

vfs_filesystem_t get_filesystem(char *type) {
    vfs_filesystem_t filesystem = NULL;
    vfs_filesystem_t pos, n;
    llist_for_each(pos, n, &fs_metadata_list, node) {
        if (strcmp(pos->name, type) == 0) { filesystem = pos; }
    }
    return filesystem;
}

vfs_filesystem_t get_filesystem_node(vfs_node_t node) {
    vfs_filesystem_t filesystem = NULL;
    vfs_filesystem_t pos, n;
    llist_for_each(pos, n, &fs_metadata_list, node) {
        if (pos->fsid == node->fsid) { filesystem = pos; }
    }
    return filesystem;
}

vfs_node_t vfs_child_append(vfs_node_t parent, const char *name, void *handle) {
    vfs_node_t node = vfs_node_alloc(parent, name);
    if (node == NULL) return NULL;
    node->handle = handle;
    return node;
}

static vfs_node_t vfs_child_find(vfs_node_t parent, const char *name) {
    if (parent == NULL || name == NULL) return NULL;
    vfs_child_lock();
    vfs_node_t child = (vfs_node_t)list_first(parent->child, data,
                                               !(((vfs_node_t)data)->type & file_delete) &&
                                               streq(name, ((vfs_node_t)data)->name));
    if (child && vfs_node_retiring_locked(child)) child = NULL;
    vfs_child_unlock();
    return child;
}

static bool is_descriptor_endpoint(vfs_node_t node)
{
    return node && (node->type & (file_pipe | file_ptmx | file_pts));
}

static bool legacy_cache_domain(vfs_node_t node)
{
    // Legacy caches require a filesystem with reference-aware cache retirement.
    // Dynamic pseudo-filesystems still support text links, resolved afresh.
    return node && (node->fsid == 0 || vfs_uses_managed_lifetime(node) ||
                    (node->fsid < 256 &&
                     __atomic_load_n(&fs_unmount_quiescence[node->fsid], __ATOMIC_ACQUIRE)));
}

static vfs_node_t vfs_child_find_for_open(vfs_node_t parent, const char *name, bool final,
                                         bool reject_endpoints, bool &endpoint_rejected)
{
    if (parent == NULL || name == NULL) return NULL;

    vfs_child_lock();
    vfs_node_t child = (vfs_node_t)list_first(parent->child, data,
                                               !(((vfs_node_t)data)->type & file_delete) &&
                                               streq(name, ((vfs_node_t)data)->name));
    if (child && vfs_node_retiring_locked(child)) child = NULL;
    // A metadata lookup must not acquire a pipe/PTY descriptor or drop its
    // endpoint count later. Refuse before even taking a node reference.
    if (reject_endpoints && is_descriptor_endpoint(child))
    {
        endpoint_rejected = true;
        child = NULL;
    }
    // Every traversed component owns a reference, including legacy parents.
    // A directory refresh can otherwise retire an intermediate node while the
    // resolver is calling its backend. `final` remains part of the old API.
    (void)final;
    if (child && child->refcount == uint32_t(-1))
    {
        permissions_set_file_error(EOVERFLOW);
        child = NULL;
    }
    if (child) ++child->refcount;
    vfs_child_unlock();
    return child;
}

// Creation reserves a hidden child before invoking the filesystem. This pins
// its parent against empty-directory deletion and allocates the list entry
// before persistent mutation. No lookup can retain a half-created node.
static vfs_node_t reserve_creation(vfs_node_t parent, const char *name, uint16_t type,
                                  bool explicit_mode, uint16_t mode, uint32_t uid, uint32_t gid)
{
    vfs_node_t node = vfs_node_alloc(NULL, name);
    if (!node) { permissions_set_file_error(ENOMEM); return NULL; }
    node->parent = parent;
    node->root = parent->root;
    node->fsid = parent->fsid;
    node->dev = parent->dev;
    node->type = type | file_delete;
    node->mode = explicit_mode ? (uint16_t)((type == file_dir ? 0040000 : 0100000) | (mode & 07777)) : 0;
    node->permissions = explicit_mode ? mode & 07777 : 0;
    node->owner = uid;
    node->group = gid;
    list_t entry = list_alloc(node);
    int duplicate_error = 0;
    if (entry)
    {
        vfs_child_lock();
        for (list_t current = parent->child; current; current = current->next)
        {
            vfs_node_t existing = (vfs_node_t)current->data;
            if (existing->name && streq(existing->name, name))
            {
                duplicate_error = existing->type & file_delete ? EBUSY : EEXIST;
                break;
            }
        }
        if (!duplicate_error && vfs_node_retiring_locked(parent)) duplicate_error = EBUSY;
        if (!duplicate_error)
        {
            entry->next = parent->child;
            if (entry->next) entry->next->prev = entry;
            parent->child = entry;
        }
        vfs_child_unlock();
    }
    if (!entry || duplicate_error)
    {
        free(entry);
        node->parent = NULL;
        vfs_close(node);
        permissions_set_file_error(duplicate_error ? duplicate_error : ENOMEM);
        return NULL;
    }
    return node;
}

static int finish_creation(vfs_node_t node, int status)
{
    if (status == 0)
    {
        vfs_child_lock();
        node->type &= (uint16_t)~file_delete;
        vfs_child_unlock();
        return 0;
    }
    int error = permissions_file_error();
    if (!error) error = status < 0 ? -status : status;
    vfs_child_lock();
    vfs_node_t parent = node->parent;
    list_t entry = parent ? parent->child : NULL;
    while (entry && entry->data != node) entry = entry->next;
    if (entry)
    {
        if (entry->prev) entry->prev->next = entry->next;
        else parent->child = entry->next;
        if (entry->next) entry->next->prev = entry->prev;
    }
    node->parent = NULL;
    node->type |= file_delete;
    vfs_child_unlock();
    free(entry);
    vfs_close(node);
    permissions_set_file_error(error);
    return -error;
}

static int create_child(vfs_node_t parent, const char *name, uint16_t type, vfs_mk_t callback,
                        bool explicit_mode, uint16_t mode, uint32_t uid, uint32_t gid, vfs_node_t *created)
{
    vfs_node_t node = reserve_creation(parent, name, type, explicit_mode, mode, uid, gid);
    if (!node) return -vfs_last_error();
    permissions_clear_file_error();
    // The unmounted VFS root deliberately implements cache-only directories.
    int status = (void *)callback == (void *)empty_func ? 0 : callback(parent->handle, name, node);
    status = finish_creation(node, status);
    if (!status && created) *created = node;
    return status;
}

errno_t vfs_register_alias(const char *alias_path, const char *target_path)
{
    if (alias_path == NULL || target_path == NULL) return VFS_STATUS_FAILED;
    if (alias_path[0] != '/' || target_path[0] != '/') return VFS_STATUS_FAILED;

    spin_lock(&vfs_alias_lock);
    for (size_t i = 0; i < VFS_ALIAS_MAX; i++)
    {
        if (vfs_aliases[i].alias_path != NULL && streq(vfs_aliases[i].alias_path, alias_path))
        {
            char *new_target = strdup(target_path);
            if (new_target == NULL)
            {
                spin_unlock(&vfs_alias_lock);
                return VFS_STATUS_FAILED;
            }
            free(vfs_aliases[i].target_path);
            vfs_aliases[i].target_path = new_target;
            spin_unlock(&vfs_alias_lock);
            return EOK;
        }
    }

    for (size_t i = 0; i < VFS_ALIAS_MAX; i++)
    {
        if (vfs_aliases[i].alias_path == NULL)
        {
            vfs_aliases[i].alias_path = strdup(alias_path);
            vfs_aliases[i].target_path = strdup(target_path);
            if (vfs_aliases[i].alias_path == NULL || vfs_aliases[i].target_path == NULL)
            {
                free(vfs_aliases[i].alias_path);
                free(vfs_aliases[i].target_path);
                vfs_aliases[i].alias_path = NULL;
                vfs_aliases[i].target_path = NULL;
                spin_unlock(&vfs_alias_lock);
                return VFS_STATUS_FAILED;
            }
            spin_unlock(&vfs_alias_lock);
            return EOK;
        }
    }

    spin_unlock(&vfs_alias_lock);
    return VFS_STATUS_FAILED;
}

static char *vfs_alias_target_dup(const char *path)
{
    char *target = NULL;
    spin_lock(&vfs_alias_lock);
    for (size_t i = 0; i < VFS_ALIAS_MAX; i++)
    {
        if (vfs_aliases[i].alias_path != NULL && streq(vfs_aliases[i].alias_path, path))
        {
            target = strdup(vfs_aliases[i].target_path);
            break;
        }
    }
    spin_unlock(&vfs_alias_lock);
    return target;
}

static errno_t mkdir_with_metadata(const char *name, bool explicit_mode, uint16_t mode, uint32_t uid, uint32_t gid)
{
    permissions_clear_file_error();
    ReadonlyTraversalGuard traversal;
    if (!traversal.ready()) { permissions_set_file_error(EBUSY); return VFS_STATUS_FAILED; }
    if (!vfs_check_access(NULL, name, IME_FILE_WRITE)) return VFS_STATUS_FAILED;
    if (!name || name[0] != '/') { permissions_set_file_error(EINVAL); return VFS_STATUS_FAILED; }
    char      *path     = strdup(name + 1);
    if (!path) { permissions_set_file_error(ENOMEM); return VFS_STATUS_FAILED; }
    char      *save_ptr = path;
    vfs_node_t current  = rootdir;
    for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        const vfs_node_t father = current;
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (current->parent && current->type & file_dir) {
                current = current->parent;
                goto upd;
            } else {
                goto err;
            }
        }
        current = vfs_child_find(current, buf);

    upd:
        if (current == NULL) {
            if (!check_child_creation(father, buf)) goto err;
            if (create_child(father, buf, file_dir, callbackof(father, mkdir),
                             explicit_mode, mode, uid, gid, &current)) goto err;
            do_update(current);
        } else {
            do_update(current);
            if (!(current->type & file_dir)) goto err;
        }
    }

    free(path);
    return EOK;

err:
    free(path);
    return VFS_STATUS_FAILED;
}

errno_t vfs_mkdir(const char *name)
{
    return mkdir_with_metadata(name, false, 0, 0, 0);
}

errno_t vfs_mkdir_mode(const char *name, uint16_t mode, uint32_t uid, uint32_t gid)
{
    return mkdir_with_metadata(name, true, mode, uid, gid);
}

errno_t vfs_link(const char *name, const char *target_name) {
    permissions_clear_file_error();
    ReadonlyTraversalGuard traversal;
    if (!traversal.ready()) { permissions_set_file_error(EBUSY); return VFS_STATUS_FAILED; }
    if (!name || !target_name || name[0] != '/' || !name[1]) return VFS_STATUS_FAILED;
    if (!vfs_check_access(NULL, name, IME_FILE_WRITE)) return VFS_STATUS_FAILED;
    vfs_node_t source = vfs_open_no_follow(target_name);
    if (source && (source->type & file_symlink) && !vfs_uses_managed_lifetime(source))
    {
        vfs_close(source);
        source = vfs_open(target_name);
    }
    if (!source) return VFS_STATUS_FAILED;
    if ((source->type & file_dir) || !vfs_check_access(source, NULL, IME_FILE_READ) ||
        !permissions_file_link(source, name))
    {
        vfs_close(source);
        return VFS_STATUS_FAILED;
    }
    vfs_node_t current = rootdir;
    char      *path    = strdup(name + 1);
    if (!path) { vfs_close(source); permissions_set_file_error(ENOMEM); return -ENOMEM; }

    char *save_ptr = path;
    char *filename = path + strlen(path);

    while (*--filename != '/' && filename != path) {}
    if (filename != path) {
        *filename++ = '\0';
    } else {
        goto create;
    }

    if (strlen(path) == 0) {
        free(path);
        vfs_close(source);
        return -ENOENT;
    }
    vfs_node_t node ;
    for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (!current->parent || !(current->type & file_dir)) goto err;
            current = current->parent;
            continue;
        }
        vfs_node_t new_current = vfs_child_find(current, buf);
        if (new_current == NULL) {
            if (!check_child_creation(current, buf)) goto err;
            if (create_child(current, buf, file_dir, callbackof(current, mkdir),
                             false, 0, 0, 0, &new_current)) goto err;
        }
        current = new_current;
        do_update(current);

        if (!(current->type & file_dir)) goto err;
    }

create:
    if (!check_child_creation(current, filename)) goto err;
    node = vfs_uses_managed_lifetime(current) ?
               reserve_creation(current, filename, file_none, false, 0, 0, 0) :
               vfs_child_append(current, filename, NULL);
    if (!node) goto err;
    if (!vfs_uses_managed_lifetime(current)) node->type = file_none;
    {
        // Native backends receive the exact source already retained and
        // authorized above. Reopening its pathname could select a replacement
        // inode after a concurrent rename and bypass that authorization.
        vfs_link_node_t link_node = current->fsid < 256 ?
            __atomic_load_n(&fs_link_node[current->fsid], __ATOMIC_ACQUIRE) : NULL;
        int status = link_node ? link_node(current->handle, source, node) :
                                callbackof(current, link)(current->handle, target_name, node);
        if (vfs_uses_managed_lifetime(current) && finish_creation(node, status)) goto err;
    }
    if (vfs_uses_managed_lifetime(current)) vfs_close(source);
    else node->linkto = source;

    free(path);

    return EOK;

err:
    free(path);
    vfs_close(source);
    return VFS_STATUS_FAILED;
}

errno_t vfs_symlink(const char *name, const char *target_name) {
    permissions_clear_file_error();
    ReadonlyTraversalGuard traversal;
    if (!traversal.ready()) { permissions_set_file_error(EBUSY); return VFS_STATUS_FAILED; }
    if (!name || !target_name || name[0] != '/' || !name[1])
    { permissions_set_file_error(EINVAL); return VFS_STATUS_FAILED; }
    if (!vfs_check_access(NULL, name, IME_FILE_WRITE)) return VFS_STATUS_FAILED;
    vfs_node_t current = rootdir;
    char      *path    = strdup(name + 1);
    if (!path) { permissions_set_file_error(ENOMEM); return -ENOMEM; }

    char *save_ptr = path;
    char *filename = path + strlen(path);

    while (*--filename != '/' && filename != path) {}
    if (filename != path) {
        *filename++ = '\0';
    } else {
        goto create;
    }
    vfs_node_t node;

    if (strlen(path) == 0) {
        free(path);
        return -1;
    }
    for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (!current->parent || !(current->type & file_dir)) goto err;
            current = current->parent;
            continue;
        }
        vfs_node_t new_current = vfs_child_find(current, buf);
        if (new_current == NULL) {
            if (!check_child_creation(current, buf)) goto err;
            if (create_child(current, buf, file_dir, callbackof(current, mkdir),
                             false, 0, 0, 0, &new_current)) goto err;
        }
        current = new_current;
        do_update(current);

        if (!(current->type & file_dir)) goto err;
    }

create:
    if (!check_child_creation(current, filename)) goto err;
    // Legacy virtual links are hidden too, until text and optional cache
    // ownership are initialized. The reservation keeps a racing lookup from
    // observing a half-created link or donating the creator's reference.
    node = reserve_creation(current, filename, file_symlink, false, 0, 0, 0);
    if (!node) goto err;
    node->linkname  = strdup(target_name);
    node->size      = node->linkname != NULL ? strlen(node->linkname) : 0;
    {
        int status = node->linkname ? callbackof(current, symlink)(current->handle, target_name, node) : -ENOMEM;
        if (!node->linkname || vfs_uses_managed_lifetime(current))
        {
            if (finish_creation(node, status)) goto err;
        }
        else
        {
            vfs_node_t target = legacy_cache_domain(node) ? vfs_open(target_name) : NULL;
            bool keep = false;
            if (target)
            {
                vfs_child_lock();
                keep = legacy_cache_domain(target) && target != rootdir && !is_descriptor_endpoint(target) &&
                       !vfs_node_retiring_locked(target) && !(target->type & file_delete);
                if (keep) node->linkto = target; // The open reference becomes the cache's ownership.
                vfs_child_unlock();
                if (!keep) vfs_close(target);
            }
            // Legacy adapters can decline physical symlink storage while the
            // VFS still supplies their established virtual text-link behavior.
            permissions_clear_file_error();
            if (finish_creation(node, 0)) goto err;
        }
    }

    free(path);

    return EOK;

err:
    free(path);
    return -EIO;
}

static errno_t mkfile_with_metadata(const char *name, bool explicit_mode, uint16_t mode, uint32_t uid, uint32_t gid)
{
    permissions_clear_file_error();
    ReadonlyTraversalGuard traversal;
    if (!traversal.ready()) { permissions_set_file_error(EBUSY); return VFS_STATUS_FAILED; }
    if (!vfs_check_access(NULL, name, IME_FILE_WRITE)) return VFS_STATUS_FAILED;
    if (!name || name[0] != '/') { permissions_set_file_error(EINVAL); return VFS_STATUS_FAILED; }

    // 分离路径和文件名
    char *fullpath  = strdup(name);
    if (!fullpath) { permissions_set_file_error(ENOMEM); return VFS_STATUS_FAILED; }
    char *filename  = fullpath;
    char *lastslash = strrchr(fullpath, '/');

    if (lastslash == fullpath) {
        // 根目录下的文件
        filename   = fullpath + 1;
        *lastslash = '\0';
    } else if (lastslash) {
        *lastslash = '\0';
        filename   = lastslash + 1;
    }

    // 打开父目录
    vfs_node_t parent;
    if (lastslash == fullpath) {
        parent = rootdir;
    } else {
        parent = vfs_open(fullpath);
    }

    if (parent == NULL || parent->type != file_dir)
    {
        int error = permissions_file_error();
        if (!error) error = parent ? ENOTDIR : ENOENT;
        if (parent) vfs_close(parent);
        free(fullpath);
        permissions_set_file_error(error);
        return VFS_STATUS_FAILED;
    }

    if (!check_child_creation(parent, filename)) { vfs_close(parent); free(fullpath); return VFS_STATUS_FAILED; }
    // 创建文件
    errno_t status = create_child(parent, filename, file_none, callbackof(parent, mkfile),
                                 explicit_mode, mode, uid, gid, NULL);
    int error = permissions_file_error();
    vfs_close(parent);
    free(fullpath);
    if (error) permissions_set_file_error(error);
    return status;
}

errno_t vfs_mkfile(const char *name)
{
    return mkfile_with_metadata(name, false, 0, 0, 0);
}

errno_t vfs_mkfile_mode(const char *name, uint16_t mode, uint32_t uid, uint32_t gid)
{
    return mkfile_with_metadata(name, true, mode, uid, gid);
}

static int finalize_backend_node(vfs_node_t node)
{
    if (!node->handle || (node->type & file_proxy)) return 0;
    vfs_finalize_node_t callback = node->fsid < 256 ?
        __atomic_load_n(&fs_finalize_node[node->fsid], __ATOMIC_ACQUIRE) : NULL;
    int error = 0;
    if (callback) error = callback(node->handle, node);
    else callbackof(node, close)(node->handle);
    node->handle = NULL;
    return error;
}

int vfs_close_cached_backend(vfs_node_t node)
{
    if (!node) return -EINVAL;
    vfs_child_lock();
    bool busy = node->type != file_dir || node->is_mount || node->child || node->linkto ||
                node->refcount > 2 || vfs_node_retiring_locked(node);
    vfs_child_unlock();
    if (busy) { permissions_set_file_error(EBUSY); return -EBUSY; }
    // The bootstrap caller retains exclusivity until publishing the new fsid.
    // A native/FAT finalizer must run with its original type, fsid and handle.
    int error = finalize_backend_node(node);
    node->visited = false;
    if (error < 0) permissions_set_file_error(-error);
    return error;
}

static int finalize_deleted_node(vfs_node_t node)
{
    // A detached managed name owns its former parent's reference. Keeping the
    // parent chain alive preserves FD permission/path metadata after unlink,
    // including when its now-empty directory is subsequently removed.
    vfs_node_t parent = NULL;
    if (node->type & file_unlinked)
    {
        vfs_child_lock();
        parent = node->parent;
        node->parent = NULL;
        vfs_child_unlock();
    }
    vfs_child_lock();
    vfs_node_t cached = node->linkto;
    node->linkto = NULL;
    vfs_child_unlock();
    int error = cached ? vfs_close(cached) : 0;
    if (!error) error = finalize_backend_node(node);
    free(node->name);
    free(node->linkname);
    free(node);
    if (parent && parent != rootdir)
    {
        int parent_error = vfs_close(parent);
        if (!error) error = parent_error;
    }
    if (error < 0) permissions_set_file_error(-error);
    return error;
}

static errno_t delete_impl(vfs_node_t node, bool *consumed)
{
    if (consumed) *consumed = false;
    permissions_clear_file_error();
    if (!vfs_check_access(node, NULL, IME_FILE_REMOVE)) return VFS_STATUS_FAILED;
    if (node == NULL || node == rootdir)
    {
        permissions_set_file_error(node ? EBUSY : EINVAL);
        return VFS_STATUS_FAILED;
    }
    if (node->is_mount) { permissions_set_file_error(EBUSY); return VFS_STATUS_FAILED; }

    vfs_child_lock();
    vfs_node_t parent = node->parent;
    if (parent == NULL || vfs_node_retiring_locked(node) || (node->type & file_delete) || node->is_mount ||
        ((node->type & file_dir) && node->child != NULL))
    {
        int error = !parent ? ENOENT : vfs_node_retiring_locked(node) ||
                    (node->type & file_delete) || node->is_mount ? EBUSY : ENOTEMPTY;
        vfs_child_unlock();
        permissions_set_file_error(error);
        return VFS_STATUS_FAILED;
    }
    if (parent != rootdir && parent->refcount == uint32_t(-1))
    {
        vfs_child_unlock();
        permissions_set_file_error(EOVERFLOW);
        return VFS_STATUS_FAILED;
    }
    node->type |= file_delete;
    if (parent != rootdir) ++parent->refcount;
    vfs_child_unlock();

    if (node->handle == NULL) do_update(node);

    errno_t res = VFS_STATUS_SUCCESS;
    if (vfs_uses_managed_lifetime(node) || !((node->type & file_symlink) && node->linkname != NULL))
    {
        if (node->handle == NULL)
        {
            res = VFS_STATUS_FAILED;
        }
        else
        {
            res = callbackof(node, del)(parent->handle, node);
        }
    }

    if (res < 0)
    {
        int error = permissions_file_error();
        vfs_child_lock();
        node->type &= (uint16_t)~file_delete;
        vfs_child_unlock();
        if (consumed) *consumed = true;
        vfs_close(node);
        if (parent != rootdir) vfs_close(parent);
        if (error) permissions_set_file_error(error);
        return res;
    }

    vfs_child_lock();
    list_t retired = parent->child;
    while (retired && retired->data != node) retired = retired->next;
    if (retired)
    {
        if (retired->prev) retired->prev->next = retired->next;
        else parent->child = retired->next;
        if (retired->next) retired->next->prev = retired->prev;
    }
    bool keep_parent = vfs_uses_unlinked_lifetime(node);
    if (keep_parent) node->type |= file_unlinked;
    else node->parent = NULL;
    // Drop the directory-tree reference and the reference held by this
    // delete caller. Any other open references keep the detached node alive.
    if (node->refcount > 0) node->refcount--;
    if (node->refcount > 0) node->refcount--;
    if (consumed) *consumed = true;
    bool release = node->refcount == 0;
    vfs_child_unlock();
    free(retired);
    int error = release ? finalize_deleted_node(node) : 0;
    // For managed unlinked names the temporary parent pin becomes the
    // detached vnode's pin and is released by finalize_deleted_node().
    if (!keep_parent && parent != rootdir) vfs_close(parent);
    return error < 0 ? error : VFS_STATUS_SUCCESS;
}

errno_t vfs_delete(vfs_node_t node)
{
    return delete_impl(node, NULL);
}

errno_t vfs_delete_owned(vfs_node_t node)
{
    bool consumed = false;
    int result = delete_impl(node, &consumed);
    if (!consumed && node)
    {
        int error = permissions_file_error();
        vfs_close(node);
        if (error) permissions_set_file_error(error);
    }
    return result;
}

// A linked child does not permanently own its parent. Capture the pointer and
// its temporary ownership together, before a concurrent rename can detach it.
struct VfsNodeReference
{
    vfs_node_t node = NULL;
    ~VfsNodeReference() { if (node) vfs_close(node); }
};

static int retain_parent(vfs_node_t child, VfsNodeReference &reference)
{
    vfs_child_lock();
    vfs_node_t parent = child->parent;
    int error = vfs_node_retiring_locked(child) ? -EBUSY : !parent ? -ENOENT : 0;
    if (!error && (parent->type & (file_delete | file_unlinked))) error = -EBUSY;
    if (!error && parent != rootdir && parent->refcount == uint32_t(-1)) error = -EOVERFLOW;
    if (!error)
    {
        if (parent != rootdir) ++parent->refcount;
        reference.node = parent;
    }
    vfs_child_unlock();
    return error;
}

errno_t vfs_rename(vfs_node_t node, const char *nw) {
    permissions_clear_file_error();
    ReadonlyTraversalGuard traversal;
    if (!traversal.ready()) { permissions_set_file_error(EBUSY); return VFS_STATUS_FAILED; }
    if (!vfs_check_access(node, NULL, IME_FILE_REMOVE) ||
        !vfs_check_access(NULL, nw, IME_FILE_WRITE) ||
        !permissions_file_move(node, nw)) return VFS_STATUS_FAILED;
    if (node == NULL || nw == NULL) return VFS_STATUS_FAILED;
    if ((node->type & file_symlink) && node->linkname != NULL && !vfs_uses_managed_lifetime(node))
    {
        const char *new_name = strrchr(nw, '/');
        new_name = new_name ? new_name + 1 : nw;
        if (!*new_name || streq(new_name, ".") || streq(new_name, ".."))
        { permissions_set_file_error(EINVAL); return -EINVAL; }
        char *copy = strdup(new_name);
        char *parent_path = strdup(nw);
        if (!copy || !parent_path)
        {
            free(copy);
            free(parent_path);
            permissions_set_file_error(ENOMEM);
            return -ENOMEM;
        }
        char *last_slash = strrchr(parent_path, '/');
        if (last_slash == parent_path) parent_path[1] = '\0';
        else if (last_slash) *last_slash = '\0';

        int result = [&]() -> int
        {
            VfsNodeReference original_parent, destination_parent;
            int error = retain_parent(node, original_parent);
            if (error) return error;
            // A basename-only destination retains the existing parent. Every
            // actual vfs_open owns a separate pin even if both pointers match.
            if (last_slash) destination_parent.node = vfs_open(parent_path);
            vfs_node_t parent = last_slash ? destination_parent.node : original_parent.node;
            if (!parent) return -vfs_last_error();
            if (!(parent->type & file_dir)) return -ENOTDIR;
            if (!vfs_check_child_creation(parent, copy)) return -vfs_last_error();

            unsigned slot = 64;
            list_t moving = NULL;
            vfs_node_t cached = NULL;
            auto validate_locked = [&]() -> int
            {
                if (node->parent != original_parent.node || node->is_mount ||
                    (node->type & (file_delete | file_unlinked)) ||
                    (parent->type & (file_delete | file_unlinked)) ||
                    vfs_node_retiring_locked(parent)) return -EBUSY;
                if (node->root != parent->root || node->fsid != parent->fsid) return -EXDEV;
                moving = original_parent.node->child;
                while (moving && moving->data != node) moving = moving->next;
                if (!moving) return -ENOENT;
                for (list_t entry = parent->child; entry; entry = entry->next)
                {
                    vfs_node_t existing = (vfs_node_t)entry->data;
                    if (existing == node || !existing->name || !streq(existing->name, copy)) continue;
                    return existing->type & file_delete ? -EBUSY : -EEXIST;
                }
                return 0;
            };
            vfs_child_lock();
            error = vfs_node_retiring_locked(node) ? -EBUSY : validate_locked();
            bool unchanged = !error && parent == original_parent.node && streq(node->name, copy);
            if (!error && !unchanged)
            {
                for (unsigned i = 0; i < 64; ++i) if (!retiring_subtrees[i]) { slot = i; break; }
                if (slot == 64) error = -EBUSY;
                else
                {
                    retiring_subtrees[slot] = node;
                    cached = node->linkto;
                    node->linkto = NULL;
                }
            }
            vfs_child_unlock();
            if (error || unchanged) return error;

            // This may be an ext4 orphan's last reference and journal. Keep the
            // original namespace and all node/parent storage until it succeeds.
            error = cached ? vfs_close(cached) : 0;
            char *old_name = NULL;
            vfs_child_lock();
            if (!error) error = validate_locked();
            if (!error)
            {
                if (parent != original_parent.node)
                {
                    // Reuse the original cell: all allocation and disposal stay
                    // outside the child lock, and no duplicate name is exposed.
                    if (moving->prev) moving->prev->next = moving->next;
                    else original_parent.node->child = moving->next;
                    if (moving->next) moving->next->prev = moving->prev;
                    moving->prev = NULL;
                    moving->next = parent->child;
                    if (moving->next) moving->next->prev = moving;
                    parent->child = moving;
                    node->parent = parent;
                }
                old_name = node->name;
                node->name = copy;
                copy = NULL;
            }
            retiring_subtrees[slot] = NULL;
            vfs_child_unlock();
            free(old_name);
            return error;
        }();
        free(copy);
        free(parent_path);
        if (result < 0) permissions_set_file_error(-result);
        return result;
    }
    do_update(node);
    if (node->handle == NULL) return VFS_STATUS_FAILED;
    return callbackof(node, rename)(node->handle, nw);
}

errno_t vfs_resize(vfs_node_t node, uint64_t size) {
    permissions_clear_file_error();
    if (!vfs_check_access(node, NULL, IME_FILE_WRITE)) return VFS_STATUS_FAILED;
    if (node == NULL) return VFS_STATUS_FAILED;
    do_update(node);
    if (node->type == file_dir) return VFS_STATUS_FAILED;
    callbackof(node, resize)(node->handle, size);
    int error = permissions_file_error();
    do_update(node);
    if (!error) error = permissions_file_error();
    if (error) { permissions_set_file_error(error); return -error; }
    return VFS_STATUS_SUCCESS;
}

errno_t vfs_resize_fast(vfs_node_t node, uint64_t size) {
    permissions_clear_file_error();
    if (!vfs_check_access(node, NULL, IME_FILE_WRITE)) return VFS_STATUS_FAILED;
    if (node == NULL) return VFS_STATUS_FAILED;
    if (node->type == file_dir) return VFS_STATUS_FAILED;
    callbackof(node, resize)(node->handle, size);
    int error = permissions_file_error();
    if (error) return -error;
    return VFS_STATUS_SUCCESS;
}

int vfs_regist(const char *name, vfs_callback_t callback, int register_id, uint64_t magic) {
    if (callback == NULL) return VFS_STATUS_FAILED;
    for (size_t i = 0; i < sizeof(struct vfs_callback) / sizeof(void *); i++) {
        if (((void **)callback)[i] == NULL) return VFS_STATUS_FAILED;
    }
    int id           = fs_nextid++;
    fs_callbacks[id] = callback;

    vfs_filesystem_t filesystem = (vfs_filesystem_t)malloc(sizeof(struct vfs_filesystem));
    filesystem->callback        = callback;
    filesystem->id              = register_id;
    filesystem->fsid            = id;
    filesystem->magic           = magic;
    strcpy(filesystem->name, name);
    llist_init_head(&filesystem->node);
    llist_append(&fs_metadata_list, &filesystem->node);
    return id;
}

int vfs_regist_checked(const char *name, vfs_callback_t callback, int register_id, uint64_t magic, int *output)
{
    if (output) *output = 0;
    if (!output || !name || !name[0] || !callback) return -EINVAL;
    size_t name_length = 0;
    while (name_length < 10 && name[name_length]) ++name_length;
    if (name_length == 10) return -EINVAL;
    for (size_t i = 0; i < sizeof(struct vfs_callback) / sizeof(void *); ++i)
        if (!reinterpret_cast<void **>(callback)[i]) return -EINVAL;
    auto *filesystem = static_cast<vfs_filesystem_t>(calloc(1, sizeof(struct vfs_filesystem)));
    if (!filesystem) return -ENOMEM;
    filesystem->callback = callback;
    filesystem->id = register_id;
    filesystem->magic = magic;
    strcpy(filesystem->name, name);
    llist_init_head(&filesystem->node);
    vfs_child_lock();
    int id = fs_nextid < 256 ? fs_nextid++ : 0;
    if (id)
    {
        filesystem->fsid = id;
        fs_callbacks[id] = callback;
        llist_append(&fs_metadata_list, &filesystem->node);
    }
    vfs_child_unlock();
    if (!id) { free(filesystem); return -ENOSPC; }
    *output = id;
    return 0;
}

vfs_node_t vfs_do_search(vfs_node_t dir, const char *name) {
    if (dir == NULL || name == NULL) return NULL;
    vfs_child_lock();
    vfs_node_t child = (vfs_node_t)list_first(dir->child, data,
                                               !(((vfs_node_t)data)->type & file_delete) &&
                                               streq(name, ((vfs_node_t)data)->name));
    vfs_child_unlock();
    return child;
}

static constexpr unsigned VFS_MAX_LINK_DEPTH = 40;
static constexpr size_t VFS_LINK_PATH_MAX = 4096;

struct LinkTargetStorage
{
    char *data = (char *)malloc(VFS_LINK_PATH_MAX + 1);
    ~LinkTargetStorage() { free(data); }
};

static bool link_expansion_fits(const char *target, const char *remaining)
{
    size_t target_length = strlen(target);
    size_t rest = remaining ? strlen(remaining) : 0;
    size_t slash = rest && target_length && target[target_length - 1] != '/' ? 1 : 0;
    if (target_length >= VFS_LINK_PATH_MAX || rest > VFS_LINK_PATH_MAX - 1 - target_length ||
        slash > VFS_LINK_PATH_MAX - 1 - target_length - rest)
    {
        permissions_set_file_error(ENAMETOOLONG);
        return false;
    }
    return true;
}

static size_t vfs_readlink_internal(vfs_node_t node, char *buf, size_t bufsize);

static vfs_node_t vfs_open_impl(const char *str, bool follow_final_symlink, bool allow_alias, unsigned link_depth,
                              bool reject_endpoints) {
    if (unlikely(str == NULL)) return NULL;
    if (unlikely(str[0] != '/')) return NULL;
    if (unlikely(link_depth > VFS_MAX_LINK_DEPTH)) { permissions_set_file_error(ELOOP); return NULL; }
    size_t path_length = 0;
    while (path_length < VFS_LINK_PATH_MAX && str[path_length]) ++path_length;
    if (path_length == VFS_LINK_PATH_MAX) { permissions_set_file_error(ENAMETOOLONG); return NULL; }
    PathTraversalGuard traversal;
    if (str[1] == '\0') return rootdir; // 根目录

    char *path = strdup(str + 1);
    if (unlikely(path == NULL)) { permissions_set_file_error(ENOMEM); return NULL; }

    char      *save_ptr = path;
    vfs_node_t current  = rootdir;
    vfs_node_t acquired_current = NULL;
    bool endpoint_rejected = false;

    for (char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) {
            // 当前目录，不需要操作
            continue;
        } else if (streq(buf, "..")) {
            // A concurrent rename can replace this parent and make the old
            // directory reclaimable. Capture and pin it in the same child-lock
            // section; retaining the child alone does not retain its old parent.
            vfs_child_lock();
            vfs_node_t parent = current->parent;
            bool retain = parent && parent != rootdir;
            bool retiring = retain && vfs_node_retiring_locked(parent);
            bool overflow = retain && parent->refcount == uint32_t(-1);
            if (retain && !retiring && !overflow) ++parent->refcount;
            vfs_child_unlock();
            if (retiring || overflow)
            {
                permissions_set_file_error(overflow ? EOVERFLOW : EBUSY);
                goto err;
            }
            if (parent)
            {
                if (acquired_current) vfs_close(acquired_current);
                acquired_current = retain ? parent : NULL;
                current = parent;
            }
            continue;
        }

        bool is_final_component = save_ptr == NULL || *save_ptr == '\0';
        vfs_node_t previous_reference = acquired_current;
        current = vfs_child_find_for_open(current, buf, is_final_component, reject_endpoints, endpoint_rejected);
        acquired_current = current;
        if (previous_reference) vfs_close(previous_reference);
        if (current == NULL) { goto err; }
        do_update(current);
        if (is_final_component && !follow_final_symlink) break;
        if (current->type & file_symlink) {
            if (!current->parent) { goto err; }

            bool dynamic_symlink = current->name != NULL && streq(current->name, "self") &&
                                   current->parent != NULL && current->parent->parent == NULL &&
                                   current->handle != NULL && current->linkto == NULL;
            // Backend-computed links (proc/self, proc/fd, etc.) are resolved on
            // every lookup. Text links alone may retain a stable target cache.
            bool uncached_link = dynamic_symlink || vfs_uses_managed_lifetime(current) || !current->linkname ||
                                 !legacy_cache_domain(current);
            vfs_node_t target = NULL;
            bool from_cache = false;
            if (!uncached_link)
            {
                vfs_child_lock();
                current->type |= file_proxy;
                vfs_node_t cached = current->linkto;
                if (cached && is_descriptor_endpoint(cached))
                {
                    // A historic endpoint cache is never borrowed as a fresh
                    // descriptor. Metadata lookup leaves its ownership alone;
                    // normal lookup goes through the ordinary open path.
                    endpoint_rejected = reject_endpoints;
                }
                else if (cached && !vfs_node_retiring_locked(cached) && !(cached->type & file_delete) &&
                         cached->refcount != uint32_t(-1))
                {
                    ++cached->refcount;
                    target = cached;
                    from_cache = true;
                }
                else if (cached)
                {
                    int error = cached->refcount == uint32_t(-1) ? EOVERFLOW : EBUSY;
                    vfs_child_unlock();
                    permissions_set_file_error(error);
                    goto err;
                }
                vfs_child_unlock();
                if (endpoint_rejected) goto err;
            }
            if (target == NULL)
            {
                LinkTargetStorage storage;
                if (!storage.data) { permissions_set_file_error(ENOMEM); goto err; }
                char *target_path = storage.data;
                VfsNodeReference relative_parent;
                size_t len = vfs_readlink_internal(current, target_path, VFS_LINK_PATH_MAX);
                if (len == (size_t)VFS_STATUS_FAILED || len == 0) goto err;
                if (len >= VFS_LINK_PATH_MAX) { permissions_set_file_error(ENAMETOOLONG); goto err; }
                target_path[len] = '\0';
                if (target_path[0] == '/')
                {
                    if (!link_expansion_fits(target_path, save_ptr)) goto err;
                    target = vfs_open_impl(target_path, true, true, link_depth + 1, reject_endpoints);
                }
                else
                {
                    int parent_error = retain_parent(current, relative_parent);
                    if (parent_error) { permissions_set_file_error(-parent_error); goto err; }
                    char *parent_path = vfs_get_fullpath(relative_parent.node);
                    if (parent_path == NULL) { permissions_set_file_error(ENOMEM); goto err; }
                    size_t parent_len = strlen(parent_path);
                    size_t target_len = strlen(target_path);
                    if (!link_expansion_fits(parent_path, target_path)) { free(parent_path); goto err; }
                    char *joined_target = (char *)malloc(parent_len + target_len + 2);
                    if (joined_target == NULL)
                    {
                        free(parent_path);
                        permissions_set_file_error(ENOMEM);
                        goto err;
                    }
                    sprintf(joined_target, "%s%s%s", parent_path,
                            parent_path[parent_len - 1] == '/' ? "" : "/", target_path);
                    char *full_target = normalize_path(joined_target);
                    free(joined_target);
                    free(parent_path);
                    if (full_target == NULL) { permissions_set_file_error(ENOMEM); goto err; }
                    if (!link_expansion_fits(full_target, save_ptr)) { free(full_target); goto err; }
                    target = vfs_open_impl(full_target, true, true, link_depth + 1, reject_endpoints);
                    free(full_target);
                }
                // A retained parent can itself move while its absolute path is
                // resolved. Without a namespace-generation proof, retain no
                // relative-path result beyond this caller's snapshot.
                if (!uncached_link && !relative_parent.node && target && target != rootdir &&
                    !is_descriptor_endpoint(target))
                {
                    // The recursive resolver's reference belongs to this
                    // caller. A winning NULL publication obtains a separate
                    // cache reference; racing losers keep their own result.
                    vfs_child_lock();
                    if (!current->linkto && legacy_cache_domain(target) &&
                        !vfs_node_retiring_locked(current) && !vfs_node_retiring_locked(target) &&
                        !(target->type & file_delete) && target->refcount != uint32_t(-1))
                    {
                        ++target->refcount;
                        current->linkto = target;
                    }
                    vfs_child_unlock();
                }
            }
            if (!target)
            {
                if (reject_endpoints && permissions_file_error() == EOPNOTSUPP) endpoint_rejected = true;
                goto err;
            }
            if (from_cache && !is_final_component)
            {
                char *full_target = vfs_get_fullpath(target);
                bool fits = full_target && link_expansion_fits(full_target, save_ptr);
                if (!full_target) permissions_set_file_error(ENOMEM);
                free(full_target);
                if (!fits) { vfs_close(target); goto err; }
            }
            if (acquired_current) vfs_close(acquired_current);
            acquired_current = target;

            current = target;
            continue;
        }
    }

    free(path);
    return current;
err:
    free(path);
    if (acquired_current != NULL) vfs_close(acquired_current);
    if (endpoint_rejected)
    {
        permissions_set_file_error(EOPNOTSUPP);
        return NULL;
    }
    if (permissions_file_error() == ENAMETOOLONG || permissions_file_error() == ELOOP) return NULL;
    if (allow_alias) {
        char *alias_target = vfs_alias_target_dup(str);
        if (alias_target != NULL) {
            write_serial_fmt("[busybox-debug] vfs_alias_hit %s -> %s\n", str, alias_target);
            vfs_node_t alias_node = vfs_open_impl(alias_target, follow_final_symlink, false, link_depth + 1,
                                                reject_endpoints);
            free(alias_target);
            return alias_node;
        }
    }
    return NULL;
}

vfs_node_t vfs_open(const char *str) {
    permissions_clear_file_error();
    vfs_node_t node = vfs_open_impl(str, true, true, 0, false);
    if (node && !vfs_check_access(node, NULL, IME_FILE_LOOKUP)) { vfs_close(node); return NULL; }
    return node;
}

vfs_node_t vfs_open_no_follow(const char *str) {
    permissions_clear_file_error();
    vfs_node_t node = vfs_open_impl(str, false, true, 0, false);
    if (node && !vfs_check_access(node, NULL, IME_FILE_LOOKUP)) { vfs_close(node); return NULL; }
    return node;
}

vfs_node_t vfs_open_metadata(const char *str, bool follow_final_symlink)
{
    permissions_clear_file_error();
    vfs_node_t node = vfs_open_impl(str, follow_final_symlink, true, 0, true);
    if (node && !vfs_check_access(node, NULL, IME_FILE_LOOKUP)) { vfs_close(node); return NULL; }
    return node;
}

void vfs_update(vfs_node_t node) {
    do_update(node);
}

void vfs_deinit() {
    // 目前并不支持
}

fd_file_handle *fd_dup(fd_file_handle *src) {
    fd_file_handle *nw = (fd_file_handle *)malloc(sizeof(fd_file_handle));
    // not_null_assets(new, "fd_dup out of memory.");
    if (!nw) { permissions_set_file_error(ENOMEM); return NULL; }
    nw->node       = src->node;
    nw->offset     = src->offset;
    nw->flags      = src->flags;
    nw->fd         = src->fd;
    vfs_node_t node = nw->node;
    if ((node->type & (file_ptmx | file_pts)) && fs_callbacks[node->fsid]->dup != (void *)empty_func) {
        fs_callbacks[node->fsid]->dup(node);
        node->refcount++;
        return nw;
    }
    if ((node->type & file_pipe) && node->handle != NULL) {
        pipe_specific_t *spec = (pipe_specific_t *)node->handle;
        pipe_info_t     *pipe = spec != NULL ? spec->info : NULL;
        if (pipe != NULL) {
            spin_lock(&pipe->lock);
            if (spec->write) {
                pipe->write_fds++;
            } else {
                pipe->read_fds++;
            }
            spin_unlock(&pipe->lock);
        }
    }
    if (!(node->type & file_pipe))
    {
        // Match managed lookups, close, and the busy-unmount snapshot. Mixing
        // an unlocked increment with locked decrements can lose a live FD ref.
        vfs_child_lock();
        if (node->refcount == uint32_t(-1))
        {
            vfs_child_unlock();
            free(nw);
            permissions_set_file_error(EOVERFLOW);
            return NULL;
        }
        ++node->refcount;
        vfs_child_unlock();
    }
    else node->refcount++;
    return nw;
}

vfs_node_t get_rootdir() {
    return rootdir;
}

void set_rootdir(vfs_node_t node) {
    rootdir         = node;
    rootdir->parent = NULL;
}

void *vfs_map(vfs_node_t node, uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
              uint64_t offset) {
    permissions_clear_file_error();
    if (unlikely(node == NULL)) return NULL;
    if (unlikely(node->type == file_dir)) return NULL;
    if (!vfs_check_access(node, NULL, (prot & 2) ? ((prot & 1) ? IME_FILE_READ_WRITE : IME_FILE_WRITE) : IME_FILE_READ)) return NULL;
    return callbackof(node, map)(node->handle, (void *)addr, offset, len, prot, flags);
}

vfs_node_t vfs_node_alloc(vfs_node_t parent, const char *name) {
    vfs_node_t node = (vfs_node_t)malloc(sizeof(struct vfs_node));
    // not_null_assets(node, "vfs alloc null");
    if (unlikely(node == NULL)) return NULL;
    memset(node, 0, sizeof(struct vfs_node));
    node->parent   = parent;
    node->name     = name ? strdup(name) : NULL;
    if (name && !node->name)
    {
        free(node);
        return NULL;
    }
    node->type     = file_none;
    node->fsid     = parent ? parent->fsid : 0;
    node->root     = parent ? parent->root : node;
    node->dev      = parent ? parent->dev : 0;
    node->refcount = 1;
    node->blksz    = PAGE_SIZE;
    node->mode     = 0777;
    node->linkto   = NULL;
    if (parent)
    {
        list_t entry = list_alloc(node);
        if (!entry)
        {
            free(node->name);
            free(node);
            return NULL;
        }
        vfs_child_lock();
        if (vfs_node_retiring_locked(parent))
        {
            vfs_child_unlock();
            free(entry);
            free(node->name);
            free(node);
            permissions_set_file_error(EBUSY);
            return NULL;
        }
        entry->next = parent->child;
        if (parent->child) parent->child->prev = entry;
        parent->child = entry;
        vfs_child_unlock();
    }
    return node;
}

errno_t vfs_close(vfs_node_t node) {
    if (unlikely(node == NULL)) return VFS_STATUS_FAILED;
    if (node == rootdir) return VFS_STATUS_SUCCESS;

    // Match lookup publication and the read-only busy scan's reference lock.
    vfs_child_lock();
    if (node->refcount > 0) node->refcount--;
    uint32_t remaining = node->refcount;
    uint16_t type = node->type;
    bool endpoint = (type & (file_pipe | file_ptmx | file_pts)) != 0;
    vfs_child_unlock();

    // Capture the result while owning the reference lock. Another closer may
    // release the last detached reference as soon as we unlock, so a non-final
    // ordinary close must not inspect this vnode again.
    if (!endpoint && remaining > 0) return VFS_STATUS_SUCCESS;

    if (node->type & (file_ptmx | file_pts)) {
        if (node->handle != NULL) callbackof(node, close)(node->handle);
        if (node->refcount > 0) return VFS_STATUS_SUCCESS;
        node->handle = NULL;
        if (node->parent == NULL) {
            free(node->name);
            free(node);
        }
        return VFS_STATUS_SUCCESS;
    }

    if (node->type & file_pipe) {
        if (node->handle != NULL) {
            callbackof(node, close)(node->handle);
        }

        if (node->refcount <= 0) {
            if (node->parent != NULL) {
                vfs_child_lock();
                node->parent->child = list_delete(node->parent->child, node);
                vfs_child_unlock();
            }
            node->handle = NULL;
            free(node->name);
            free(node);
        }
        return VFS_STATUS_SUCCESS;
    }

    if (node->type & file_epoll) {
        if (node->refcount > 0) return VFS_STATUS_SUCCESS;
        epoll_file_t *ep = (epoll_file_t *)node->handle;
        if (ep != NULL) {
            if (ep->watches != NULL) {
                while (true) {
                    epoll_watch_t *watch = (epoll_watch_t *)queue_dequeue(ep->watches);
                    if (watch == NULL) break;
                    free(watch);
                }
                queue_destroy(ep->watches);
            }
            free(ep);
            node->handle = NULL;
        }
        if (node->parent == NULL) {
            free(node->name);
            free(node);
        }
        return VFS_STATUS_SUCCESS;
    }

    if (unlikely(node->handle == NULL))
    {
        if ((type & file_delete) && remaining == 0) return finalize_deleted_node(node);
        return VFS_STATUS_SUCCESS;
    }

    if (node->type & file_socket)
    {
        if (node->refcount > 0) return VFS_STATUS_SUCCESS;
        if (node->type & file_delete)
        {
            callbackof(node, del)(node->parent != NULL ? node->parent->handle : NULL, node);
            node->handle = NULL;
            if (node->parent != NULL)
            {
                vfs_child_lock();
                node->parent->child = list_delete(node->parent->child, node);
                node->parent = NULL;
                vfs_child_unlock();
            }
            free(node->name);
            free(node->linkname);
            free(node);
        }
        return VFS_STATUS_SUCCESS;
    }

    if (node->type & file_proxy){
         if ((type & file_delete) && remaining == 0) return finalize_deleted_node(node);
         return VFS_STATUS_SUCCESS;
    }
    if ((node->type & file_dir) && !(node->type & file_delete))
    {
        return VFS_STATUS_SUCCESS;
    } 
    if (!(node->type & file_delete) && node->refcount > 0) {
        return VFS_STATUS_SUCCESS;
    }
    if (node->type & file_delete)
    {
        if (node->refcount > 0) return VFS_STATUS_SUCCESS;
        if (!(node->type & file_unlinked) && node->parent != NULL)
        {
            vfs_child_lock();
            list_t retired = node->parent->child;
            while (retired && retired->data != node) retired = retired->next;
            if (retired)
            {
                if (retired->prev) retired->prev->next = retired->next;
                else node->parent->child = retired->next;
                if (retired->next) retired->next->prev = retired->prev;
            }
            node->parent = NULL;
            vfs_child_unlock();
            free(retired);
        }
        return finalize_deleted_node(node);
    }
    else
    {
        int error = finalize_backend_node(node);
        if (error < 0) permissions_set_file_error(-error);
        return error;
    }
    return VFS_STATUS_SUCCESS;
}

bool is_virtual_fs(const char *src) {
    return ((uint64_t)src) == DEVFS_REGISTER_ID || ((uint64_t)src) == MODFS_REGISTER_ID ||
           ((uint64_t)src) == TMPFS_REGISTER_ID || ((uint64_t)src) == PIEFS_REGISTER_ID ||
           ((uint64_t)src) == NETFS_REGISTER_ID || ((uint64_t)src) == CPFS_REGISTER_ID ||
           ((uint64_t)src) == PROC_REGISTER_ID || ((uint64_t)src) == DNS_REGISTER_ID ||
           ((uint64_t)src) == NM_REGISTER_ID;
}

// Checked cache retirement. Callers of the legacy void disposers own a private
// tree with only directory references and no outgoing cache references. Those
// disposers never release a cross-volume cache under an unknown caller lock.
static bool private_subtree_quiet(vfs_node_t node, unsigned depth)
{
    if (!node || depth > 128 || node->refcount > 1 || node->linkto ||
        (node->type & (file_delete | file_unlinked | file_pipe | file_ptmx | file_pts))) return false;
    for (list_t entry = node->child; entry; entry = entry->next)
        if (!private_subtree_quiet((vfs_node_t)entry->data, depth + 1)) return false;
    return true;
}

static bool child_member_locked(vfs_node_t parent, vfs_node_t node)
{
    for (list_t entry = parent ? parent->child : NULL; entry; entry = entry->next)
        if (entry->data == node) return true;
    return false;
}

static int detached_retention_slot(vfs_node_t node)
{
    for (unsigned i = 0; i < 64; ++i) if (detached_retentions[i].node == node) return i;
    return -1;
}

// Compatibility callers may already have detached a busy tree. Retaining its
// heap alone is insufficient: its mount context must also outlive all handles.
// This bounded registration performs no allocation or callback under the lock.
static void retain_detached_tree(vfs_node_t node)
{
    if (detached_retention_slot(node) >= 0 || child_member_locked(node->parent, node)) return;
    vfs_node_t mount = node->root;
    if (!mount || mount == node || !mount->is_mount) return;
    if (mount->refcount != uint32_t(-1))
    {
        for (unsigned i = 0; i < 64; ++i)
            if (!detached_retentions[i].node)
            {
                ++mount->refcount;
                detached_retentions[i] = {node, mount};
                return;
            }
    }
    // Unknown detached ownership is never an excuse to publish a clean
    // unmount. Exhaustion retains storage and refuses unmount until reboot.
    __atomic_store_n(&detached_retention_overflow, true, __ATOMIC_RELEASE);
}

void vfs_free(vfs_node_t vfs) {
    if (vfs == NULL) return;
    vfs_child_lock();
    // A compatibility-retained tree needs the explicit fallible collector;
    // the void API must not hide a backend-close error or consume its root pin.
    if (detached_retention_slot(vfs) >= 0 || child_member_locked(vfs->parent, vfs) ||
        !private_subtree_quiet(vfs, 0))
    {
        retain_detached_tree(vfs);
        vfs_child_unlock();
        permissions_set_file_error(EBUSY);
        return;
    }
    list_t child = vfs->child;
    vfs->child   = NULL;
    vfs_child_unlock();
    list_free_with(child, [](void *node)
    {
        vfs_free(static_cast<vfs_node_t>(node));
    });
    vfs_close(vfs);
    free(vfs->name);
    free(vfs->linkname);
    free(vfs);
}

void vfs_free_child(vfs_node_t vfs) {
    if (vfs == NULL) return;
    vfs_child_lock();
    for (list_t entry = vfs->child; entry; entry = entry->next)
        if (!private_subtree_quiet((vfs_node_t)entry->data, 0))
        {
            vfs_child_unlock();
            permissions_set_file_error(EBUSY);
            return;
        }
    list_t child = vfs->child;
    vfs->child   = NULL;
    vfs_child_unlock();
    list_free_with(child, [](void *node)
    {
        vfs_free(static_cast<vfs_node_t>(node));
    });
}

static bool retirement_structure(vfs_node_t node, bool root, unsigned depth)
{
    if (!node || depth > 128 || (!root && node->is_mount) ||
        (node->type & (file_delete | file_unlinked | file_pipe | file_ptmx | file_pts))) return false;
    // Non-text legacy aliases can contain authoritative backend references;
    // they are not an evictable text-resolution cache.
    if (node->linkto && (!(node->type & file_symlink) || !node->linkname)) return false;
    for (list_t entry = node->child; entry; entry = entry->next)
        if (!retirement_structure((vfs_node_t)entry->data, false, depth + 1)) return false;
    return true;
}

static vfs_node_t take_cached_link(vfs_node_t node)
{
    if (node->linkto)
    {
        vfs_node_t target = node->linkto;
        node->linkto = NULL;
        return target;
    }
    for (list_t entry = node->child; entry; entry = entry->next)
    {
        vfs_node_t target = take_cached_link((vfs_node_t)entry->data);
        if (target) return target;
    }
    return NULL;
}

// The caller owns a retirement marker or an unpublished private tree. Keep the
// complete source tree alive until all cache references have been detached and
// closed; an outside target can be an ext4 orphan whose final close journals.
static int release_cached_links(vfs_node_t root)
{
    for (;;)
    {
        vfs_child_lock();
        vfs_node_t target = take_cached_link(root);
        vfs_child_unlock();
        if (!target) return 0;
        int error = vfs_close(target);
        if (error < 0) return error;
    }
}

// Close only same-tree backend handles, retaining every vnode/list cell. A
// later failure can then report the complete failed mounted context instead of
// leaving a partially freed tree. The final prepare flush covers these closes.
static int close_cached_tree(vfs_node_t root, bool include_root)
{
    for (list_t entry = root->child; entry; entry = entry->next)
    {
        int error = close_cached_tree((vfs_node_t)entry->data, true);
        if (error < 0) return error;
    }
    if (!include_root) return 0;
    int error = finalize_backend_node(root);
    root->visited = false;
    return error;
}

static bool readonly_subtree_busy(vfs_node_t node, bool root, unsigned depth);

int vfs_try_free_detached(vfs_node_t node)
{
    if (!node) return -EINVAL;
    vfs_child_lock();
    retain_detached_tree(node);
    bool busy = node->is_mount || child_member_locked(node->parent, node) ||
                vfs_node_retiring_locked(node) || !private_subtree_quiet(node, 0);
    if (busy) retain_detached_tree(node);
    vfs_child_unlock();
    if (busy) { permissions_set_file_error(EBUSY); return -EBUSY; }
    // The caller serializes its private retired queue and owns the whole
    // detached tree. No new pathname lookup can acquire one of these nodes.
    int error = close_cached_tree(node, true);
    if (error) return error;
    vfs_child_lock();
    int retained = detached_retention_slot(node);
    vfs_node_t mount = retained >= 0 ? detached_retentions[retained].mount : NULL;
    if (retained >= 0) detached_retentions[retained] = {};
    vfs_child_unlock();
    vfs_free(node);
    // Keep the mounted root alive through all backend and storage cleanup.
    // Its tree baseline remains live, so this decrement cannot run a callback.
    if (mount)
    {
        vfs_child_lock();
        if (mount->refcount > 1) --mount->refcount;
        vfs_child_unlock();
    }
    return 0;
}

int vfs_prune_child(vfs_node_t parent, vfs_node_t child, unsigned held_references, bool require_unvisited)
{
    if (!parent || !child || held_references > uint32_t(-1) - 2U) return -EINVAL;
    unsigned slot = 64;
    vfs_child_lock();
    list_t member = parent->child;
    while (member && member->data != child) member = member->next;
    // Test membership before dereferencing the caller's expected child pointer.
    bool busy = !member || creation_traversals || vfs_node_retiring_locked(parent) ||
                vfs_node_retiring_locked(child) || child->is_mount || child->refcount == uint32_t(-1) ||
                (require_unvisited && child->visited) || !retirement_structure(child, true, 0);
    if (!busy)
    {
        for (unsigned i = 0; i < 64; ++i) if (!retiring_subtrees[i]) { slot = i; break; }
        busy = slot == 64;
    }
    if (!busy)
    {
        ++child->refcount; // Retire operation pin also protects against private ancestor disposal.
        retiring_subtrees[slot] = child;
    }
    vfs_child_unlock();
    if (busy) { permissions_set_file_error(EBUSY); return -EBUSY; }

    int error = release_cached_links(child);
    if (!error)
    {
        vfs_child_lock();
        // The root's tree reference and explicit caller references are owned
        // by this disposal request; every descendant must have only its tree
        // reference after outgoing cache eviction.
        bool external = child->refcount > 2U + held_references;
        for (list_t entry = child->child; !external && entry; entry = entry->next)
            external = readonly_subtree_busy((vfs_node_t)entry->data, false, 0);
        vfs_child_unlock();
        if (external) error = -EBUSY;
    }
    if (!error) error = close_cached_tree(child, true);
    vfs_child_lock();
    if (!error)
    {
        if (member->prev) member->prev->next = member->next;
        else parent->child = member->next;
        if (member->next) member->next->prev = member->prev;
        child->parent = NULL;
        child->refcount = 1; // Consume caller-owned references on success only.
    }
    else --child->refcount;
    retiring_subtrees[slot] = NULL;
    vfs_child_unlock();
    if (error) { permissions_set_file_error(-error); return error; }
    free(member);
    vfs_free(child);
    return 0;
}

errno_t vfs_mount(const char *src, vfs_node_t node) {
    if (!vfs_check_access(node, NULL, IME_FILE_MOUNT)) return VFS_STATUS_FAILED;
    if (node == NULL) return VFS_STATUS_FAILED;
    if (node->type != file_dir) return VFS_STATUS_FAILED;
    if (node->is_mount) return VFS_STATUS_FAILED;
    // Backends with permanent namespace ownership require an explicit boot
    // handoff. A generic overmount must not strand their handle or owner pin.
    if (node->fsid < 256 && __atomic_load_n(&fs_reject_overmount[node->fsid], __ATOMIC_ACQUIRE))
    {
        permissions_set_file_error(EBUSY);
        return VFS_STATUS_FAILED;
    }
    int retained_error = 0;
    auto probe_rejection = [](int error)
    {
        return error == EINVAL || error == ENODEV || error == ENOENT || error == ENOSYS || error == EOPNOTSUPP;
    };
    for (int i = 1; i < fs_nextid; i++) {
        permissions_clear_file_error();
        int result = fs_callbacks[i]->mount(src, node);
        if (result == 0) {
            vfs_child_lock();
            node->fsid     = i;
            node->root     = node;
            node->is_mount = true;
            vfs_child_unlock();
            return VFS_STATUS_SUCCESS;
        }
        // Cleanup and later filesystem probes can clear errno. Retain the
        // concrete callback result, particularly a publication EBUSY, rather
        // than turning a retryable mount into a fabricated device EIO.
        int error = result >= -4095 && result < -1 ? -result : permissions_file_error();
        if (error && (!retained_error || (probe_rejection(retained_error) && !probe_rejection(error))))
            retained_error = error;
    }
    permissions_set_file_error(retained_error ? retained_error : EINVAL);
    return VFS_STATUS_FAILED;
}

size_t vfs_read(vfs_node_t file, void *addr, size_t offset, size_t size) {
    permissions_clear_file_error();
    if (!vfs_check_access(file, NULL, IME_FILE_READ)) return VFS_STATUS_FAILED;
    if (file == NULL || addr == NULL) return VFS_STATUS_FAILED;
    do_update(file);
    if (file->type == file_dir) return VFS_STATUS_FAILED;
    return callbackof(file, read)(file->handle, addr, offset, size);
}

size_t vfs_write(vfs_node_t file, void *addr, size_t offset, size_t size) {
    permissions_clear_file_error();
    if (!vfs_check_access(file, NULL, IME_FILE_WRITE)) return VFS_STATUS_FAILED;
    if (file == NULL || addr == NULL) return VFS_STATUS_FAILED;
    do_update(file);
    if (file->type == file_dir) return VFS_STATUS_FAILED;
    size_t ret = callbackof(file, write)(file->handle, addr, offset, size);
    int error = permissions_file_error();
    if (ret == (size_t)-1 && !error) error = EIO;
    do_update(file);
    if (error) permissions_set_file_error(error);
    return ret;
}

size_t vfs_write_fast(vfs_node_t file, void *addr, size_t offset, size_t size) {
    permissions_clear_file_error();
    if (!vfs_check_access(file, NULL, IME_FILE_WRITE)) return VFS_STATUS_FAILED;
    if (file == NULL || addr == NULL) return VFS_STATUS_FAILED;
    if (file->type == file_dir) return VFS_STATUS_FAILED;
    return callbackof(file, write)(file->handle, addr, offset, size);
}

errno_t vfs_ioctl(vfs_node_t device, size_t options, void *arg) {
    permissions_clear_file_error();
    if (!vfs_check_access(device, NULL, IME_FILE_WRITE)) return VFS_STATUS_FAILED;
    if (device == NULL) return VFS_STATUS_FAILED;
    do_update(device);
    if (device->type == file_dir) return VFS_STATUS_FAILED;
    return callbackof(device, ioctl)(device->handle, options, arg);
}

errno_t vfs_poll(vfs_node_t node, size_t event) {
    do_update(node);
    if (node->type & file_dir) return -1;
    return callbackof(node, poll)(node->handle, event);
}

// Caller holds the child lock. Never invoke callbacks or allocate while scanning.
static bool readonly_subtree_busy(vfs_node_t node, bool root, unsigned depth)
{
    if (depth > 128 || node->refcount > (root ? 2U : 1U)) return true;
    if (!root && node->is_mount) return true;
    for (list_t child = node->child; child; child = child->next)
    {
        if (readonly_subtree_busy((vfs_node_t)child->data, false, depth + 1)) return true;
    }
    return false;
}

errno_t vfs_unmount(const char *path) {
    if (!vfs_check_access(NULL, NULL, IME_FILE_MOUNT)) return VFS_STATUS_FAILED;
    if (__atomic_load_n(&detached_retention_overflow, __ATOMIC_ACQUIRE))
    { permissions_set_file_error(EBUSY); return VFS_STATUS_FAILED; }
    vfs_node_t node = vfs_open(path);
    if (node == NULL) return VFS_STATUS_FAILED;
    if (node->type != file_dir || node->fsid == 0)
    {
        vfs_close(node);
        return VFS_STATUS_FAILED;
    }
    if (node->parent) {
        vfs_node_t cur = node;
        node           = node->parent;
        if (cur->root == cur) {
            uint16_t retiring_fsid = cur->fsid;
            bool managed = vfs_uses_managed_lifetime(cur);
            bool legacy_guard = retiring_fsid < 256 && !managed &&
                                __atomic_load_n(&fs_unmount_quiescence[retiring_fsid], __ATOMIC_ACQUIRE);
            bool guarded = managed || legacy_guard;
            if (guarded)
            {
                vfs_child_lock();
                bool busy = readonly_unmounting[retiring_fsid] != NULL || readonly_traversals[retiring_fsid] != 0 ||
                            creation_traversals || (legacy_guard && path_traversals) ||
                            !retirement_structure(cur, true, 0);
                if (!busy) readonly_unmounting[retiring_fsid] = cur;
                vfs_child_unlock();
                if (busy)
                {
                    vfs_close(cur);
                    permissions_set_file_error(EBUSY);
                    return VFS_STATUS_FAILED;
                }
            }
            int cache_error = guarded ? release_cached_links(cur) : 0;
            if (!cache_error && guarded)
            {
                vfs_child_lock();
                bool busy = readonly_subtree_busy(cur, true, 0);
                vfs_child_unlock();
                if (busy) cache_error = -EBUSY;
            }
            // Close cached child backend handles before the durability gate.
            // The complete vnode tree remains available if either phase fails,
            // and FAT's retained prepare level is never leaked on a close error.
            if (!cache_error && guarded) cache_error = close_cached_tree(cur, false);
            if (cache_error)
            {
                vfs_child_lock();
                cur->visited = false;
                if (guarded) readonly_unmounting[retiring_fsid] = NULL;
                vfs_child_unlock();
                vfs_close(cur);
                permissions_set_file_error(-cache_error);
                return cache_error == -EBUSY ? VFS_STATUS_FAILED : cache_error;
            }
            vfs_prepare_unmount_t prepare = __atomic_load_n(&fs_prepare_unmount[retiring_fsid], __ATOMIC_ACQUIRE);
            int prepare_error = prepare ? prepare(cur->handle) : 0;
            if (prepare_error)
            {
                if (guarded)
                {
                    vfs_child_lock();
                    readonly_unmounting[retiring_fsid] = NULL;
                    vfs_child_unlock();
                }
                vfs_close(cur);
                permissions_set_file_error(prepare_error < 0 ? -prepare_error : prepare_error);
                return prepare_error < 0 ? prepare_error : -prepare_error;
            }
            vfs_free_child(cur);
            vfs_checked_unmount_t checked = __atomic_load_n(&fs_checked_unmount[retiring_fsid], __ATOMIC_ACQUIRE);
            int unmount_error = 0;
            if (checked) unmount_error = checked(cur->handle);
            else callbackof(cur, unmount)(cur->handle);
            if (unmount_error)
            {
                // The adapter retains its failed context and device lease.
                // Cached children may be gone, but this is still a mounted,
                // failed volume: never publish success or recycle its handle.
                vfs_child_lock();
                cur->visited = false;
                if (guarded) readonly_unmounting[retiring_fsid] = NULL;
                vfs_child_unlock();
                vfs_close(cur);
                permissions_set_file_error(unmount_error < 0 ? -unmount_error : unmount_error);
                return unmount_error < 0 ? unmount_error : -unmount_error;
            }
            vfs_child_lock();
            cur->fsid     = node->fsid; // 交给上级
            cur->root     = node->root;
            cur->handle   = NULL;
            cur->child    = NULL;
            cur->is_mount = false;
            if (guarded) readonly_unmounting[retiring_fsid] = NULL;
            vfs_child_unlock();
            if (cur->fsid) do_update(cur);
            vfs_close(cur);
            return VFS_STATUS_SUCCESS;
        }
        vfs_close(cur);
        return VFS_STATUS_FAILED;
    }
    vfs_close(node);
    return VFS_STATUS_FAILED;
}

static size_t vfs_readlink_internal(vfs_node_t node, char *buf, size_t bufsize) {
    if (node != NULL && node->linkname != NULL) {
        size_t link_size = strlen(node->linkname);
        size_t copy_size = MIN(link_size, bufsize);
        memcpy(buf, node->linkname, copy_size);
        return copy_size;
    }
    size_t ret = callbackof(node, readlink)(node, buf, 0, bufsize);
    return ret;
}

size_t vfs_readlink(vfs_node_t node, char *buf, size_t bufsize)
{
    permissions_clear_file_error();
    if (!vfs_check_access(node, NULL, IME_FILE_READ)) return VFS_STATUS_FAILED;
    return vfs_readlink_internal(node, buf, bufsize);
}

void *general_map(vfs_read_t read_callback, void *file, uint64_t addr, uint64_t len, uint64_t prot,
                  uint64_t flags, uint64_t offset) {
    UNUSED(flags);
    pcb_t current_task        = get_current_task()->parent_group;
    // current_task->mmap_start += (len + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));

    uint64_t pt_flags = PTE_USER | PTE_WRITEABLE | PTE_PRESENT;

    if (prot & PROT_READ) pt_flags |= PTE_PRESENT;
    if (prot & PROT_WRITE) pt_flags |= PTE_WRITEABLE;
    if (!(prot & PROT_EXEC)) pt_flags |= PTE_NO_EXECUTE;

    page_map_range_to_random(get_current_directory(), addr & (~(PAGE_SIZE - 1)),
                             (len + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1)), pt_flags);

    ssize_t ret = read_callback(file, (void *)addr, offset, len);
    if (ret < 0) return (void *)-ENOMEM;

    return (void *)addr;
}

// Retained as an exported legacy symbol. Path construction now snapshots the
// parent/name chain under the child lock and allocates only after unlocking.
spin_t get_path_lock = SPIN_INIT;

static int measure_fullpath_locked(vfs_node_t node, size_t &length)
{
    length = 0;
    vfs_node_t slow = node, fast = node;
    for (vfs_node_t current = node; current && current != rootdir; current = current->parent)
    {
        if (!current->name) return -EINVAL;
        size_t bytes = strlen(current->name);
        if (length == size_t(-1) || bytes > size_t(-1) - length - 1) return -EOVERFLOW;
        length += bytes + 1;
        slow = slow ? slow->parent : NULL;
        fast = fast && fast->parent ? fast->parent->parent : NULL;
        if (slow && fast && slow == fast && slow != rootdir) return -ELOOP;
    }
    if (!length) length = 1;
    return length == size_t(-1) ? -EOVERFLOW : 0;
}

// The caller retains node and owns the returned absolute path. Detached managed
// nodes retain their parents, so this remains valid after unlink or replacement.
char *vfs_get_fullpath(vfs_node_t node)
{
    if (!node) return NULL;
    for (unsigned retry = 0; retry < 8; ++retry)
    {
        size_t capacity = 0;
        vfs_child_lock();
        int error = measure_fullpath_locked(node, capacity);
        vfs_child_unlock();
        if (error) { permissions_set_file_error(-error); return NULL; }
        char *path = (char *)malloc(capacity + 1);
        if (!path) { permissions_set_file_error(ENOMEM); return NULL; }

        size_t length = 0;
        vfs_child_lock();
        error = measure_fullpath_locked(node, length);
        if (!error && length <= capacity)
        {
            char *cursor = path + length;
            *cursor = 0;
            if (node == rootdir) *--cursor = '/';
            else
                for (vfs_node_t current = node; current && current != rootdir; current = current->parent)
                {
                    size_t bytes = strlen(current->name);
                    cursor -= bytes;
                    memcpy(cursor, current->name, bytes);
                    *--cursor = '/';
                }
        }
        vfs_child_unlock();
        if (!error && length <= capacity) return path;
        free(path);
        if (error) { permissions_set_file_error(-error); return NULL; }
    }
    permissions_set_file_error(EAGAIN);
    return NULL;
}

char *at_resolve_pathname(int dirfd, char *pathname) {
    if (pathname[0] == '/') { // by absolute pathname
        return normalize_path(pathname);
    } else if (pathname[0] != '/') {
        if (dirfd == AT_FDCWD) { // relative to cwd
            return vfs_cwd_path_build(pathname);
        } else { // relative to dirfd, resolve accordingly
            FdOperation operation;
            int error = operation.acquire(dirfd);
            if (error) { permissions_set_file_error(-error); return NULL; }
            fd_file_handle *handle = operation.get();
            if (!handle || !handle->node) { permissions_set_file_error(EBADF); return NULL; }
            vfs_node_t node = handle->node;
            if (node->type & (file_delete | file_unlinked)) { permissions_set_file_error(ENOENT); return NULL; }
            if (node->type != file_dir) { permissions_set_file_error(ENOTDIR); return NULL; }

            char *dirname = vfs_get_fullpath(node);
            if (dirname == NULL) return NULL;
            char *joined = pathacat(dirname, pathname);
            free(dirname);
            if (joined == NULL) return NULL;
            char *normalized = normalize_path(joined);
            free(joined);
            return normalized;
        }
    }

    return NULL;
}
char *normalize_path(const char *path)
{
    if (!path) return NULL;
    size_t capacity = strlen(path) + 2;
    char *result = (char *)malloc(capacity);
    if (!result) return NULL;
    size_t used = 1;
    result[0] = '/';
    for (const char *cursor = path; *cursor;)
    {
        while (*cursor == '/' || *cursor == '\\') ++cursor;
        const char *start = cursor;
        while (*cursor && *cursor != '/' && *cursor != '\\') ++cursor;
        size_t length = (size_t)(cursor - start);
        if (!length || (length == 1 && start[0] == '.')) continue;
        if (length == 2 && start[0] == '.' && start[1] == '.')
        {
            while (used > 1 && result[used - 1] != '/') --used;
            if (used > 1) --used;
            continue;
        }
        if (used > 1) result[used++] = '/';
        memcpy(result + used, start, length);
        used += length;
    }
    result[used] = 0;
    return result;
}
char *vfs_cwd_path_build(char *src) {
    char *s = src;
    char *path;
    char *bpath = NULL;
    if (s[0] == '/') {
        path = strdup(s);
    } else {
        bpath = vfs_get_fullpath(get_current_task()->cwd);
        path  = pathacat(bpath, s);
    }
    char *normalized_path = normalize_path(path);
    free(path);
    free(bpath);
    return normalized_path;
}

bool vfs_init() {
    for (size_t i = 0; i < sizeof(struct vfs_callback) / sizeof(void *); i++) {
        ((void **)&vfs_empty_callback)[i] = (void *)empty_func;
    }
    llist_init_head(&fs_metadata_list);
    rootdir       = vfs_node_alloc(NULL, "/");
    rootdir->type = file_dir;
    write_serial_fmt("Virtual File System initialize.\n");
    return true;
}

static tty_t *current_tty()
{
    tcb_t task = get_current_task();
    if (task == NULL || task->parent_group == NULL || task->parent_group->tty == NULL) return get_default_tty();
    return task->parent_group->tty;
}

static xtttp_dtt *current_xtttp_terminal()
{
    tcb_t task = get_current_task();
    pcb_t process = task != NULL ? task->parent_group : NULL;
    while (process != NULL && process != kernel_group)
    {
        if (process->xtttp_stc != NULL && process->xtttp_stc->is_shell) return process->xtttp_stc;
        process = process->parent_task;
    }
    return NULL;
}

static void tty_kernel_flush(tty_t *tty)
{
    (void)tty;
}

static void tty_kernel_print(tty_t *tty, const char *msg)
{
    (void)tty;
    write_serial_fmt("%s", msg);
    p_xapi_output_kernel(msg);
}

static void tty_kernel_putc(tty_t *tty, int c)
{
    (void)tty;
    char buf[2] = {(char)c, '\0'};
    write_serial_fmt("%s", buf);
    p_xapi_output_kernel(buf);
}

static void tty_termios_default(termios_t *termios)
{
    memset(termios, 0, sizeof(*termios));
    termios->c_lflag = ECHO | ICANON | IEXTEN | ISIG;
    termios->c_iflag = BRKINT | ICRNL | INPCK | ISTRIP | IXON;
    termios->c_oflag = OPOST;
    termios->c_cflag = CS8 | CREAD | CLOCAL;
    termios->c_line = 0;
    termios->c_cc[VINTR] = 3;
    termios->c_cc[VQUIT] = 28;
    termios->c_cc[VERASE] = 0x7F;
    termios->c_cc[VKILL] = 21;
    termios->c_cc[VEOF] = 4;
    termios->c_cc[VTIME] = 0;
    termios->c_cc[VMIN] = 1;
    termios->c_cc[VSTART] = 17;
    termios->c_cc[VSTOP] = 19;
    termios->c_cc[VSUSP] = 26;
    termios->c_cc[VREPRINT] = 18;
    termios->c_cc[VDISCARD] = 15;
    termios->c_cc[VWERASE] = 23;
    termios->c_cc[VLNEXT] = 22;
}

tty_t *alloc_default_tty()
{
    tty_t *tty = (tty_t *)malloc(sizeof(tty_t));
    if (tty == NULL) return NULL;
    memset(tty, 0, sizeof(*tty));
    tty->width = 720;
    tty->height = 400;
    tty->print = tty_kernel_print;
    tty->putchar = tty_kernel_putc;
    tty->flush = tty_kernel_flush;
    tty_termios_default(&tty->termios);
    tty->is_sigterm = false;
    return tty;
}

void free_tty(tty_t *tty)
{
    if (tty == NULL || tty == defualt_tty) return;
    free(tty);
}

tty_t *get_default_tty()
{
    return defualt_tty;
}

static size_t stdin_read(int drive, uint8_t *buffer, size_t number, size_t lba) {
    bool is_sti = are_interrupts_enabled();
    open_interrupt;

    xtttp_dtt *terminal = current_xtttp_terminal();
    if (terminal != NULL)
    {
        terminal->wait_for_input = true;
        while (!terminal->input_lock)
        {
            scheduler_yield();
        }
    }

    size_t i = 0;
    for (; i < number; i++) {
        char c;
        if (terminal != NULL)
        {
            c = terminal->input[0];
            if (c == '\0') break;
            size_t remaining = strlen(terminal->input);
            if (remaining > 0)
            {
                memmove(terminal->input, terminal->input + 1, remaining);
            }
        }
        else
        {
            c = (char)get_keyboard_input();
        }
        if (c == '\0') { break; }
        tty_t *tty = current_tty();
        uint32_t lflag = tty != NULL ? tty->termios.c_lflag : 0;
        if (c == 0x9) { c = '\t'; }
        if (terminal == NULL && (c == 0x7f || c == '\b') && (lflag & ICANON)) {
            if (tty != NULL && (lflag & ECHO)) write_serial_fmt("\b \b");
            if (i > 0) {
                i--;
                buffer[i] = '\0';
            }
            continue;
        }
        if (tty != NULL && (lflag & ECHO)) write_serial_fmt("%c", c);
        buffer[i] = c;
        if (c == '\n') {
            i++;
            break;
        }
    }

    if (terminal != NULL)
    {
        if (terminal->input[0] == '\0')
        {
            terminal->input_lock = false;
            terminal->wait_for_input = false;
        }
        else
        {
            terminal->input_lock = true;
            terminal->wait_for_input = false;
        }
    }

    if (!is_sti) close_interrupt;

    pcb_t process = get_current_task() ? get_current_task()->parent_group : NULL;
    if (process != NULL && process->linux_abi) {
        // write_serial_fmt("[] stdin_read requested=%llu ret=%llu\n",
        //                  (unsigned long long)number,
        //                  (unsigned long long)i);
    }

    return i;
}

static size_t stdout_write(int drive, uint8_t *buffer, size_t number, size_t lba) {
    tty_t *tty = current_tty();
    if (tty != NULL && tty->print != NULL)
    {
        char *text = (char *)malloc(number + 1);
        if (text == NULL) return 0;
        memcpy(text, buffer, number);
        text[number] = '\0';
        tty->print(tty, text);
        free(text);
    }
    else
    {
        for (size_t i = 0; i < number; i++) write_serial_fmt("%c", buffer[i]);
    }
    return number;
}

static errno_t tty_ioctl(device_t *device, size_t req, void *arg)
{
    (void)device;
    tty_t *tty = current_tty();
    if (tty == NULL) return -ENOTTY;

    switch (req)
    {
    case TIOCGWINSZ:
    {
        struct winsize *ws = (struct winsize *)arg;
        if (ws != NULL)
        {
            ws->ws_row = 25;
            ws->ws_col = 80;
            ws->ws_xpixel = (unsigned short)tty->width;
            ws->ws_ypixel = (unsigned short)tty->height;
        }
        break;
    }
    case TCGETS:
    {
        struct termios *termios = (struct termios *)arg;
        if (termios == NULL) return -EFAULT;
        memcpy(termios, &tty->termios, sizeof(struct termios));
        break;
    }
    case TCSETS:
    case TCSETSF:
    case TCSETSW:
    {
        const struct termios *termios = (const struct termios *)arg;
        if (termios == NULL) return -EFAULT;
        memcpy(&tty->termios, termios, sizeof(struct termios));
        break;
    }
    case TIOCGPGRP:
        if (arg == NULL) return -EFAULT;
        *(int *)arg = get_current_task()->parent_group->pid;
        tty->is_sigterm = true;
        break;
    case TIOCSPGRP:
    case TIOCSCTTY:
        break;
    case KDGETMODE:
        if (arg == NULL) return -EFAULT;
        *(int *)arg = tty_mode;
        break;
    case KDSETMODE:
        if (arg == NULL) return -EFAULT;
        tty_mode = *(int *)arg;
        break;
    case KDGKBMODE:
        if (arg == NULL) return -EFAULT;
        *(int *)arg = tty_kbmode;
        break;
    case KDSKBMODE:
        if (arg == NULL) return -EFAULT;
        tty_kbmode = *(int *)arg;
        break;
    case VT_SETMODE:
        if (arg == NULL) return -EFAULT;
        memcpy(&current_vt_mode, arg, sizeof(current_vt_mode));
        break;
    case VT_GETMODE:
        if (arg == NULL) return -EFAULT;
        memcpy(arg, &current_vt_mode, sizeof(current_vt_mode));
        break;
    case VT_OPENQRY:
        if (arg == NULL) return -EFAULT;
        *(int *)arg = 1;
        break;
    default:
        return -ENOTTY;
    }
    return EOK;
}

static errno_t tty_poll(size_t events)
{
    int revents = 0;
    if (events & POLLIN)
    {
        xtttp_dtt *terminal = current_xtttp_terminal();
        if (terminal != NULL)
        {
            if (terminal->input[0] != '\0') revents |= POLLIN;
        }
        else
        {
            revents |= POLLIN;
        }
    }
    if (events & POLLOUT) revents |= POLLOUT;
    return revents;
}

static size_t null_read(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    UNUSED(drive);
    UNUSED(buffer);
    UNUSED(lba);
    return 0;
}

static size_t null_write(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    UNUSED(drive);
    UNUSED(buffer);
    UNUSED(lba);
    return number;
}

static size_t urandom_read(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    UNUSED(drive);
    UNUSED(lba);
    if (buffer == NULL) return 0;

    get_random_bytes(buffer, number);
    return number;
}

static size_t urandom_write(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    UNUSED(drive);
    UNUSED(buffer);
    UNUSED(lba);
    return number;
}

void build_tty_device()
{
    device_t stdio;
    stdio.type = DEVICE_STREAM;
    strcpy(stdio.drive_name, "stdio");
    stdio.flag        = 1;
    stdio.sector_size = 1;
    stdio.size        = 1;
    stdio.read        = stdin_read;
    stdio.write       = stdout_write;
    stdio.ioctl       = tty_ioctl;
    stdio.poll        = tty_poll;
    stdio.map         = (mapf)empty;
    regist_device(NULL, stdio);

    device_t ttydev = stdio;
    strcpy(ttydev.drive_name, "tty");
    regist_device(NULL, ttydev);

    device_t null_dev = {};
    null_dev.type = DEVICE_STREAM;
    strcpy(null_dev.drive_name, "null");
    null_dev.flag        = 1;
    null_dev.sector_size = 1;
    null_dev.size        = 0;
    null_dev.read        = null_read;
    null_dev.write       = null_write;
    null_dev.ioctl       = tty_ioctl;
    null_dev.poll        = tty_poll;
    null_dev.map         = (mapf)empty;
    regist_device(NULL, null_dev);

    device_t urandom_dev = {};
    urandom_dev.type = DEVICE_STREAM;
    strcpy(urandom_dev.drive_name, "urandom");
    urandom_dev.flag        = 1;
    urandom_dev.sector_size = 1;
    urandom_dev.size        = (size_t)-1;
    urandom_dev.read        = urandom_read;
    urandom_dev.write       = urandom_write;
    urandom_dev.ioctl       = tty_ioctl;
    urandom_dev.poll        = tty_poll;
    urandom_dev.map         = (mapf)empty;
    regist_device(NULL, urandom_dev);
}

void init_tty()
{
    defualt_tty = alloc_default_tty();
    build_tty_device();
}

void stdio_init()
{
    init_tty();
}
