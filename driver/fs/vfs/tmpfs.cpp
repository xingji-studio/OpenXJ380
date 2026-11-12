#define ALL_IMPLEMENTATION
#include <cpu/lock.h>
#include <errno.h>
#include <fs/vfs/vfs.h>
#include <proto.hpp>

typedef struct tmpfs_entry {
    vfs_node_t          node;
    struct tmpfs_entry *parent;
    struct tmpfs_entry *next;
    char               *name;
    uint8_t            *data;
    size_t              size;
    size_t              capacity;
    bool                is_dir;
    uint64_t            inode;
} tmpfs_entry_t;

static int           tmpfs_id       = 0;
static uint64_t      tmpfs_next_ino = 2;
static spin_t        tmpfs_lock     = SPIN_INIT;
static tmpfs_entry_t *tmpfs_entries = NULL;

static tmpfs_entry_t *tmpfs_find_by_node(vfs_node_t node)
{
    for (tmpfs_entry_t *entry = tmpfs_entries; entry != NULL; entry = entry->next) {
        if (entry->node == node) return entry;
    }
    return NULL;
}

static tmpfs_entry_t *tmpfs_find_child(tmpfs_entry_t *parent, const char *name)
{
    if (parent == NULL || name == NULL) return NULL;
    for (tmpfs_entry_t *entry = tmpfs_entries; entry != NULL; entry = entry->next) {
        if (entry->parent == parent && entry->name != NULL && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static tmpfs_entry_t *tmpfs_alloc_entry(vfs_node_t node, tmpfs_entry_t *parent, const char *name, bool is_dir)
{
    tmpfs_entry_t *entry = (tmpfs_entry_t *)calloc(1, sizeof(tmpfs_entry_t));
    if (entry == NULL) return NULL;

    entry->node   = node;
    entry->parent = parent;
    entry->name   = strdup(name != NULL ? name : "");
    if (entry->name == NULL) {
        free(entry);
        return NULL;
    }
    entry->is_dir = is_dir;
    entry->inode  = tmpfs_next_ino++;
    entry->next   = tmpfs_entries;
    tmpfs_entries = entry;
    return entry;
}

static void tmpfs_unlink_entry(tmpfs_entry_t *target)
{
    tmpfs_entry_t **cursor = &tmpfs_entries;
    while (*cursor != NULL) {
        if (*cursor == target) {
            *cursor = target->next;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static bool tmpfs_has_child(tmpfs_entry_t *parent)
{
    for (tmpfs_entry_t *entry = tmpfs_entries; entry != NULL; entry = entry->next) {
        if (entry->parent == parent) return true;
    }
    return false;
}

static char *tmpfs_basename_dup(const char *path)
{
    if (path == NULL) return NULL;
    const char *slash = strrchr(path, '/');
    return strdup(slash != NULL ? slash + 1 : path);
}

static void tmpfs_apply_stat(tmpfs_entry_t *entry, vfs_node_t node)
{
    node->fsid  = tmpfs_id;
    node->type  = entry->is_dir ? file_dir : file_none;
    node->size  = entry->is_dir ? 0 : entry->size;
    node->inode = entry->inode;
    node->blksz = PAGE_SIZE;
    node->dev   = node->root != NULL ? node->root->dev : 0;
    node->mode  = node->mode == 0 ? (entry->is_dir ? 0777 : 0666) : node->mode;
}

static errno_t tmpfs_mount(const char *src, vfs_node_t node)
{
    if ((uint64_t)src != TMPFS_REGISTER_ID) return VFS_STATUS_FAILED;

    spin_lock(&tmpfs_lock);
    tmpfs_entry_t *entry = tmpfs_alloc_entry(node, NULL, node->name != NULL ? node->name : "", true);
    if (entry == NULL) {
        spin_unlock(&tmpfs_lock);
        return VFS_STATUS_FAILED;
    }
    node->handle = entry;
    node->fsid   = tmpfs_id;
    node->type   = file_dir;
    node->inode  = 1;
    node->blksz  = PAGE_SIZE;
    spin_unlock(&tmpfs_lock);
    return VFS_STATUS_SUCCESS;
}

static void tmpfs_open(void *parent, const char *name, vfs_node_t node)
{
    spin_lock(&tmpfs_lock);
    tmpfs_entry_t *entry = tmpfs_find_by_node(node);
    if (entry == NULL) {
        tmpfs_entry_t *parent_entry = (tmpfs_entry_t *)parent;
        entry = tmpfs_find_child(parent_entry, name);
        if (entry != NULL) entry->node = node;
    }
    if (entry != NULL) {
        node->handle = entry;
        tmpfs_apply_stat(entry, node);
    }
    spin_unlock(&tmpfs_lock);
}

static errno_t tmpfs_stat(void *file, vfs_node_t node)
{
    if (file == NULL || node == NULL) return VFS_STATUS_FAILED;
    spin_lock(&tmpfs_lock);
    tmpfs_entry_t *entry = (tmpfs_entry_t *)file;
    tmpfs_apply_stat(entry, node);
    spin_unlock(&tmpfs_lock);
    return EOK;
}

static errno_t tmpfs_mkdir(void *parent, const char *name, vfs_node_t node)
{
    spin_lock(&tmpfs_lock);
    tmpfs_entry_t *parent_entry = (tmpfs_entry_t *)parent;
    if (parent_entry == NULL || !parent_entry->is_dir || tmpfs_find_child(parent_entry, name) != NULL) {
        spin_unlock(&tmpfs_lock);
        return VFS_STATUS_FAILED;
    }
    tmpfs_entry_t *entry = tmpfs_alloc_entry(node, parent_entry, name, true);
    if (entry == NULL) {
        spin_unlock(&tmpfs_lock);
        return VFS_STATUS_FAILED;
    }
    node->handle = entry;
    tmpfs_apply_stat(entry, node);
    spin_unlock(&tmpfs_lock);
    return VFS_STATUS_SUCCESS;
}

static errno_t tmpfs_mkfile(void *parent, const char *name, vfs_node_t node)
{
    spin_lock(&tmpfs_lock);
    tmpfs_entry_t *parent_entry = (tmpfs_entry_t *)parent;
    if (parent_entry == NULL || !parent_entry->is_dir || tmpfs_find_child(parent_entry, name) != NULL) {
        spin_unlock(&tmpfs_lock);
        return VFS_STATUS_FAILED;
    }
    tmpfs_entry_t *entry = tmpfs_alloc_entry(node, parent_entry, name, false);
    if (entry == NULL) {
        spin_unlock(&tmpfs_lock);
        return VFS_STATUS_FAILED;
    }
    node->handle = entry;
    tmpfs_apply_stat(entry, node);
    spin_unlock(&tmpfs_lock);
    return VFS_STATUS_SUCCESS;
}

static size_t tmpfs_read(void *file, void *addr, size_t offset, size_t size)
{
    if (file == NULL || addr == NULL) return VFS_STATUS_FAILED;
    spin_lock(&tmpfs_lock);
    tmpfs_entry_t *entry = (tmpfs_entry_t *)file;
    if (entry->is_dir) {
        spin_unlock(&tmpfs_lock);
        return VFS_STATUS_FAILED;
    }
    if (offset >= entry->size) {
        spin_unlock(&tmpfs_lock);
        return 0;
    }
    size_t available = entry->size - offset;
    if (size > available) size = available;
    memcpy(addr, entry->data + offset, size);
    spin_unlock(&tmpfs_lock);
    return size;
}

static bool tmpfs_reserve(tmpfs_entry_t *entry, size_t want)
{
    if (want <= entry->capacity) return true;
    size_t new_capacity = entry->capacity == 0 ? 4096 : entry->capacity;
    while (new_capacity < want) {
        if (new_capacity > ((size_t)-1) / 2) return false;
        new_capacity *= 2;
    }
    // realloc failure leaves entry->data valid and unchanged; keep the old file contents.
    uint8_t *new_data = (uint8_t *)realloc(entry->data, new_capacity);
    if (new_data == NULL) return false;
    if (new_capacity > entry->capacity) {
        memset(new_data + entry->capacity, 0, new_capacity - entry->capacity);
    }
    entry->data     = new_data;
    entry->capacity = new_capacity;
    return true;
}

static size_t tmpfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    if (file == NULL || addr == NULL) return VFS_STATUS_FAILED;
    spin_lock(&tmpfs_lock);
    tmpfs_entry_t *entry = (tmpfs_entry_t *)file;
    if (entry->is_dir || offset > ((size_t)-1) - size || !tmpfs_reserve(entry, offset + size)) {
        spin_unlock(&tmpfs_lock);
        return VFS_STATUS_FAILED;
    }
    memcpy(entry->data + offset, addr, size);
    if (offset + size > entry->size) entry->size = offset + size;
    spin_unlock(&tmpfs_lock);
    return size;
}

static void tmpfs_resize(void *file, uint64_t size)
{
    spin_lock(&tmpfs_lock);
    tmpfs_entry_t *entry = (tmpfs_entry_t *)file;
    if (entry != NULL && !entry->is_dir && size <= (uint64_t)((size_t)-1) && tmpfs_reserve(entry, (size_t)size)) {
        if ((size_t)size > entry->size) {
            memset(entry->data + entry->size, 0, (size_t)size - entry->size);
        }
        entry->size = (size_t)size;
    }
    spin_unlock(&tmpfs_lock);
}

static errno_t tmpfs_delete(void *parent, vfs_node_t node)
{
    (void)parent;
    spin_lock(&tmpfs_lock);
    tmpfs_entry_t *entry = tmpfs_find_by_node(node);
    if (entry == NULL) {
        spin_unlock(&tmpfs_lock);
        return -ENOENT;
    }
    if (entry->is_dir && tmpfs_has_child(entry)) {
        spin_unlock(&tmpfs_lock);
        return -ENOTEMPTY;
    }
    tmpfs_unlink_entry(entry);
    free(entry->name);
    free(entry->data);
    free(entry);
    node->handle = NULL;
    spin_unlock(&tmpfs_lock);
    return VFS_STATUS_SUCCESS;
}

static errno_t tmpfs_rename(void *current, const char *nw)
{
    if (current == NULL || nw == NULL) return VFS_STATUS_FAILED;
    char *new_name = tmpfs_basename_dup(nw);
    if (new_name == NULL || new_name[0] == '\0') {
        free(new_name);
        return VFS_STATUS_FAILED;
    }

    spin_lock(&tmpfs_lock);
    tmpfs_entry_t *entry = (tmpfs_entry_t *)current;
    tmpfs_entry_t *conflict = tmpfs_find_child(entry->parent, new_name);
    if (conflict != NULL && conflict != entry) {
        spin_unlock(&tmpfs_lock);
        free(new_name);
        return -EEXIST;
    }
    free(entry->name);
    entry->name = new_name;
    if (entry->node != NULL) {
        free(entry->node->name);
        entry->node->name = strdup(new_name);
    }
    spin_unlock(&tmpfs_lock);
    return VFS_STATUS_SUCCESS;
}

static int tmpfs_dummy()
{
    return -ENOSYS;
}

static void tmpfs_close(void *current)
{
    (void)current;
}

static errno_t tmpfs_poll(void *file, size_t events)
{
    (void)file;
    (void)events;
    return 0;
}

static vfs_node_t tmpfs_dup(vfs_node_t node)
{
    return node;
}

static struct vfs_callback tmpfs_callbacks = {
    .mount    = tmpfs_mount,
    .unmount  = (vfs_unmount_t)tmpfs_close,
    .open     = tmpfs_open,
    .close    = tmpfs_close,
    .read     = tmpfs_read,
    .write    = tmpfs_write,
    .readlink = (vfs_readlink_t)tmpfs_dummy,
    .mkdir    = tmpfs_mkdir,
    .mkfile   = tmpfs_mkfile,
    .link     = (vfs_mk_t)tmpfs_dummy,
    .symlink  = (vfs_mk_t)tmpfs_dummy,
    .stat     = tmpfs_stat,
    .ioctl    = (vfs_ioctl_t)tmpfs_dummy,
    .dup      = tmpfs_dup,
    .poll     = tmpfs_poll,
    .map      = (vfs_mapfile_t)tmpfs_dummy,
    .resize   = tmpfs_resize,
    .del      = tmpfs_delete,
    .rename   = tmpfs_rename,
};

int tmpfs_setup(void)
{
    tmpfs_id = vfs_regist("tmpfs", &tmpfs_callbacks, TMPFS_REGISTER_ID, 0x01021994);
    if (tmpfs_id == VFS_STATUS_FAILED) return -EIO;

    vfs_mkdir("/tmp");
    vfs_node_t tmp = vfs_open("/tmp");
    if (tmp == NULL) return -ENOENT;
    if (vfs_mount((const char *)TMPFS_REGISTER_ID, tmp) == VFS_STATUS_FAILED) return -EIO;
    return 0;
}

// Original Apache-2.0 private bootstrap staging. This deliberately does not
// invoke the legacy callbacks above: their runtime publication/lifetime and
// allocator locking need a separate review before an installer root can run.
#include <fs/vfs/tmpfs.h>
#include <task/pcb.h>

namespace
{
constexpr uint64_t private_tree_magic = 0x58544d5050524956ULL;
constexpr uint64_t private_entry_limit = 262145;
constexpr unsigned private_depth_limit = 128;

struct PrivateEntry
{
    PrivateEntry *next;
    vfs_node_t node;
    uint8_t *data;
    uint64_t bytes;
    struct PrivateTree *tree;
    uint64_t inode;
    unsigned char lock;
    bool backend_pin, deleted;
};

struct PrivateTree
{
    uint64_t magic;
    const void *owner;
    uint64_t owner_tid;
    TmpfsPrivateLimits limits;
    PrivateEntry *entries;
    vfs_node_t root;
    uint64_t entry_count, data_bytes;
    bool sealed, quarantined, explicit_root, published;
    unsigned char lock;
    uint64_t next_inode;
};

int private_owner(TmpfsPrivateTree *tree, PrivateTree **output)
{
    if (!tree || !tree->state || !output) return -EINVAL;
    auto *state = static_cast<PrivateTree *>(tree->state);
    tcb_t task = get_current_task();
    if (state->magic != private_tree_magic || !task || state->owner != task || state->owner_tid != task->tid)
        return -EPERM;
    *output = state;
    return state->quarantined ? -EIO : 0;
}

int private_path(const char *path)
{
    if (!path || path[0] != '/') return -EINVAL;
    size_t bytes = 0;
    while (bytes < 512 && path[bytes]) ++bytes;
    if (!bytes || bytes == 512) return -ENAMETOOLONG;
    if (bytes == 1) return 0;
    if (path[bytes - 1] == '/') return -EINVAL;
    unsigned depth = 0;
    const char *start = path + 1;
    for (const char *at = start;; ++at)
    {
        if (*at == '\\') return -EINVAL;
        if (*at && *at != '/') continue;
        size_t size = at - start;
        if (!size || size > 255 || (size == 1 && start[0] == '.') ||
            (size == 2 && start[0] == '.' && start[1] == '.')) return -EINVAL;
        if (++depth > private_depth_limit) return -E2BIG;
        if (!*at) return 0;
        start = at + 1;
    }
}

vfs_node_t private_lookup(PrivateTree *state, const char *path)
{
    if (path[1] == '\0') return state->root;
    vfs_node_t node = state->root;
    const char *part = path + 1;
    for (;;)
    {
        const char *end = part;
        while (*end && *end != '/') ++end;
        size_t length = end - part;
        vfs_node_t found = NULL;
        for (list_t cell = node->child; cell; cell = cell->next)
        {
            auto *child = static_cast<vfs_node_t>(cell->data);
            if (child->name && strlen(child->name) == length && !memcmp(child->name, part, length))
            {
                found = child;
                break;
            }
        }
        if (!found) return NULL;
        if (!*end) return found;
        if (found->type != file_dir) return NULL;
        node = found;
        part = end + 1;
    }
}

PrivateEntry *private_entry(PrivateTree *state, vfs_node_t node)
{
    for (PrivateEntry *entry = state->entries; entry; entry = entry->next)
        if (entry->node == node) return entry;
    return NULL;
}

bool private_node_valid(PrivateTree *state, vfs_node_t node, vfs_node_t parent, unsigned depth, uint64_t *count)
{
    if (!node || depth > private_depth_limit || ++*count > state->entry_count || node->parent != parent ||
        node->root != state->root || node->refcount != 1 || node->is_mount || node->fsid || node->flags ||
        node->linkto || node->linkname || node->visited || (node->type != file_none && node->type != file_dir))
        return false;
    PrivateEntry *entry = private_entry(state, node);
    if (!entry || node->handle != entry || node->size != entry->bytes ||
        (node->type == file_dir && (entry->bytes || entry->data))) return false;
    list_t previous = NULL;
    for (list_t cell = node->child; cell; cell = cell->next)
    {
        if (node->type != file_dir || cell->prev != previous ||
            !private_node_valid(state, static_cast<vfs_node_t>(cell->data), node, depth + 1, count)) return false;
        previous = cell;
    }
    return true;
}

bool private_tree_valid(PrivateTree *state)
{
    if (!state->root || state->entry_count > state->limits.max_entries ||
        state->data_bytes > state->limits.max_data_bytes) return false;
    uint64_t count = 0, bytes = 0, entries = 0;
    if (!private_node_valid(state, state->root, NULL, 0, &count) || count != state->entry_count) return false;
    for (PrivateEntry *entry = state->entries; entry; entry = entry->next)
    {
        if (++entries > count || bytes > __UINT64_MAX__ - entry->bytes) return false;
        bytes += entry->bytes;
    }
    return entries == count && bytes == state->data_bytes;
}

void private_free(PrivateTree *state)
{
    // The complete tree was checked while exclusively owned and is unreachable.
    // List cells carry no additional ownership. No callback or spin lock is
    // involved while releasing either backend buffers or ordinary vnodes.
    PrivateEntry *entry = state->entries;
    while (entry)
    {
        PrivateEntry *next = entry->next;
        list_t cell = entry->node->child;
        while (cell)
        {
            list_t following = cell->next;
            free(cell);
            cell = following;
        }
        free(entry->data);
        free(entry->node->name);
        free(entry->node);
        free(entry);
        entry = next;
    }
    state->magic = 0;
    free(state);
}
} // namespace

int tmpfs_private_begin(TmpfsPrivateTree *tree, const TmpfsPrivateLimits &limits)
{
    tcb_t task = get_current_task();
    if (!tree || tree->state || !task || !limits.max_entries ||
        limits.max_entries > private_entry_limit || limits.max_data_bytes > __SIZE_MAX__) return -EINVAL;
    auto *state = static_cast<PrivateTree *>(calloc(1, sizeof(PrivateTree)));
    auto *entry = static_cast<PrivateEntry *>(calloc(1, sizeof(PrivateEntry)));
    vfs_node_t node = state && entry ? vfs_node_alloc(NULL, "/") : NULL;
    if (!node)
    {
        free(entry);
        free(state);
        return -ENOMEM;
    }
    state->magic = private_tree_magic;
    state->owner = task;
    state->owner_tid = task->tid;
    state->limits = limits;
    state->root = node;
    state->entries = entry;
    state->entry_count = 1;
    state->next_inode = 2;
    node->type = file_dir;
    node->mode = 0755;
    node->handle = entry;
    entry->node = node;
    entry->tree = state;
    entry->inode = 1;
    tree->state = state;
    return 0;
}

int tmpfs_private_add(TmpfsPrivateTree *tree, const char *path, bool directory, const void *data,
                      uint64_t bytes, uint16_t mode, uint32_t uid, uint32_t gid)
{
    PrivateTree *state = NULL;
    int error = private_owner(tree, &state);
    if (error) return error;
    if (state->sealed) return -EROFS;
    if ((error = private_path(path))) return error;
    if ((mode & ~07777U) || (directory && bytes) || (bytes && !data)) return -EINVAL;
    if (!path[1])
    {
        if (!directory || state->explicit_root) return -EEXIST;
        state->explicit_root = true;
        state->root->mode = mode;
        state->root->owner = uid;
        state->root->group = gid;
        return 0;
    }
    if (state->entry_count == state->limits.max_entries || bytes > state->limits.max_data_bytes - state->data_bytes)
        return -ENOSPC;
    if (private_lookup(state, path)) return -EEXIST;
    char parent_path[512];
    strcpy(parent_path, path);
    char *name = strrchr(parent_path, '/');
    *name++ = '\0';
    vfs_node_t parent = parent_path[0] ? private_lookup(state, parent_path) : state->root;
    if (!parent || parent->type != file_dir) return -ENOENT;
    auto *entry = static_cast<PrivateEntry *>(calloc(1, sizeof(PrivateEntry)));
    if (!entry) return -ENOMEM;
    if (bytes)
    {
        entry->data = static_cast<uint8_t *>(malloc(bytes));
        if (!entry->data) { free(entry); return -ENOMEM; }
        memcpy(entry->data, data, bytes);
    }
    vfs_node_t node = vfs_node_alloc(parent, name);
    if (!node)
    {
        free(entry->data);
        free(entry);
        return -ENOMEM;
    }
    node->type = directory ? file_dir : file_none;
    node->mode = mode;
    node->owner = uid;
    node->group = gid;
    node->size = bytes;
    node->handle = entry;
    entry->node = node;
    entry->bytes = bytes;
    entry->tree = state;
    entry->inode = state->next_inode++;
    entry->next = state->entries;
    state->entries = entry;
    ++state->entry_count;
    state->data_bytes += bytes;
    return 0;
}

int tmpfs_private_file(TmpfsPrivateTree *tree, const char *path, TmpfsPrivateFile *output)
{
    if (!output) return -EINVAL;
    *output = {};
    PrivateTree *state = NULL;
    int error = private_owner(tree, &state);
    if (error) return error;
    if (state->published) return -EBUSY;
    if ((error = private_path(path))) return error;
    vfs_node_t node = private_lookup(state, path);
    if (!node) return -ENOENT;
    PrivateEntry *entry = private_entry(state, node);
    if (!entry || node->handle != entry) { state->quarantined = true; return -EIO; }
    *output = {entry->data, entry->bytes, node->owner, node->group, node->mode, node->type == file_dir};
    return 0;
}

int tmpfs_private_seal(TmpfsPrivateTree *tree)
{
    PrivateTree *state = NULL;
    int error = private_owner(tree, &state);
    if (error) return error;
    if (state->sealed) return -EALREADY;
    if (!private_tree_valid(state)) { state->quarantined = true; return -EIO; }
    state->sealed = true;
    return 0;
}

int tmpfs_private_snapshot(TmpfsPrivateTree *tree, TmpfsPrivateSnapshot *output)
{
    if (!output) return -EINVAL;
    *output = {};
    PrivateTree *state = NULL;
    int error = private_owner(tree, &state);
    if (error && error != -EIO) return error;
    if (state->published) return -EBUSY;
    *output = {state->entry_count, state->data_bytes, state->sealed, state->quarantined};
    return 0;
}

int tmpfs_private_discard(TmpfsPrivateTree *tree)
{
    PrivateTree *state = NULL;
    int error = private_owner(tree, &state);
    if (error) return error;
    if (state->published) return -EBUSY;
    if (!private_tree_valid(state)) { state->quarantined = true; return -EBUSY; }
    tree->state = NULL;
    private_free(state);
    return 0;
}

// Original Apache-2.0 managed installer RAM backend. This deliberately does
// not invoke or change the legacy tmpfs callbacks above.
#include <ioctl.h>
#include <user/permissions.h>
namespace
{
constexpr uint64_t managed_memory_limit = 64ULL << 20;
constexpr uint64_t managed_entry_limit = 32768;
int managed_root_id;

class RamLock
{
    unsigned char *lock;
    uint64_t flags;
public:
    explicit RamLock(unsigned char *value) : lock(value)
    {
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
        while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    }
    ~RamLock()
    {
        __atomic_clear(lock, __ATOMIC_RELEASE);
        __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
    }
};

int ram_error(int error)
{
    permissions_set_file_error(error);
    return -error;
}

bool ram_budget(uint64_t entries, uint64_t bytes)
{
    return entries <= managed_entry_limit && bytes <= managed_memory_limit - 4096 &&
           entries <= (managed_memory_limit - 4096 - bytes) / 1024;
}

bool ram_entry_valid(PrivateEntry *entry)
{
    return entry && entry->tree && entry->tree->magic == private_tree_magic && entry->node &&
           !__atomic_load_n(&entry->tree->quarantined, __ATOMIC_ACQUIRE);
}

errno_t ram_mount(const char *, vfs_node_t) { return -EINVAL; }
void ram_unmount(void *) {}
int ram_unmount_checked(void *) { return ram_error(EBUSY); }
void ram_close(void *) {}

errno_t ram_stat(void *handle, vfs_node_t node)
{
    auto *entry = static_cast<PrivateEntry *>(handle);
    if (!ram_entry_valid(entry) || entry->node != node) return ram_error(EIO);
    RamLock lock(&entry->lock);
    // Type/fsid/inode are established once. Do not rewrite type here: generic
    // VFS may concurrently add unlink/delete state under its own child lock.
    node->size = entry->bytes;
    return 0;
}

void ram_open(void *parent_handle, const char *, vfs_node_t node)
{
    auto *parent = static_cast<PrivateEntry *>(parent_handle);
    if (!ram_entry_valid(parent)) { ram_error(EIO); return; }
    PrivateTree *state = parent->tree;
    RamLock tree_lock(&state->lock);
    for (PrivateEntry *entry = state->entries; entry; entry = entry->next)
        if (entry->node == node && !entry->deleted)
        {
            node->handle = entry;
            ram_stat(entry, node);
            return;
        }
    ram_error(ENOENT);
}

size_t ram_read(void *handle, void *destination, size_t offset, size_t bytes)
{
    auto *entry = static_cast<PrivateEntry *>(handle);
    if (!ram_entry_valid(entry) || !destination) { ram_error(EINVAL); return size_t(-1); }
    RamLock lock(&entry->lock);
    if (offset >= entry->bytes) return 0;
    uint64_t count = entry->bytes - offset;
    if (count > bytes) count = bytes;
    if (count) memcpy(destination, entry->data + offset, count);
    return count;
}

// Every reference to the old data pointer, including reads and copies during
// resize, stays under the entry lock. Allocations and displaced-buffer frees
// happen outside both locks. Tree-before-entry is the sole nested order.
int ram_change(PrivateEntry *entry, uint64_t length, const void *source, uint64_t offset, uint64_t written)
{
    if (!ram_entry_valid(entry)) return ram_error(EIO);
    if (entry->node->type & file_dir) return ram_error(EISDIR);
    if (length > managed_memory_limit || (written && !source) || offset > length || written > length - offset)
        return ram_error(EFBIG);
    for (;;)
    {
        uint8_t *fresh = length ? static_cast<uint8_t *>(malloc(length)) : NULL;
        if (length && !fresh) return ram_error(ENOMEM);
        uint8_t *retired = NULL;
        int error = 0;
        bool retry = false;
        {
            RamLock tree_lock(&entry->tree->lock);
            RamLock entry_lock(&entry->lock);
            PrivateTree *state = entry->tree;
            uint64_t current_length = written && entry->bytes > offset + written ? entry->bytes :
                                      written ? offset + written : length;
            if (state->quarantined || state->data_bytes < entry->bytes) error = EIO;
            else if (current_length != length) { length = current_length; retry = true; }
            else if (!ram_budget(state->entry_count, state->data_bytes - entry->bytes + length)) error = ENOSPC;
            else
            {
                uint64_t copied = entry->bytes < length ? entry->bytes : length;
                if (copied) memcpy(fresh, entry->data, copied);
                if (length > copied) memset(fresh + copied, 0, length - copied);
                if (written) memcpy(fresh + offset, source, written);
                retired = entry->data;
                state->data_bytes = state->data_bytes - entry->bytes + length;
                entry->data = fresh;
                entry->bytes = length;
                entry->node->size = length;
                fresh = NULL;
            }
        }
        free(fresh);
        free(retired);
        if (!retry) return error ? ram_error(error) : 0;
    }
}

size_t ram_write(void *handle, const void *source, size_t offset, size_t bytes)
{
    auto *entry = static_cast<PrivateEntry *>(handle);
    if (!ram_entry_valid(entry)) { ram_error(EIO); return size_t(-1); }
    if (!bytes) return 0;
    if (!source || offset > __UINT64_MAX__ - bytes) { ram_error(EFBIG); return size_t(-1); }
    uint64_t length;
    {
        RamLock lock(&entry->lock);
        length = entry->bytes > offset + bytes ? entry->bytes : offset + bytes;
    }
    // A concurrent grow must not be accidentally truncated by this write.
    return ram_change(entry, length, source, offset, bytes) ? size_t(-1) : bytes;
}

void ram_resize(void *handle, uint64_t length)
{
    ram_change(static_cast<PrivateEntry *>(handle), length, NULL, 0, 0);
}

errno_t ram_create(void *parent_handle, const char *, vfs_node_t node, bool directory)
{
    auto *parent = static_cast<PrivateEntry *>(parent_handle);
    if (!ram_entry_valid(parent) || !node || node->parent != parent->node) return ram_error(EIO);
    auto *entry = static_cast<PrivateEntry *>(calloc(1, sizeof(PrivateEntry)));
    if (!entry) return ram_error(ENOMEM);
    PrivateTree *state = parent->tree;
    int error = 0;
    {
        RamLock tree_lock(&state->lock);
        if (state->quarantined || parent->deleted) error = EIO;
        else if (!ram_budget(state->entry_count + 1, state->data_bytes)) error = ENOSPC;
        else if (state->next_inode == __UINT64_MAX__) error = EOVERFLOW;
        else
        {
            vfs_child_lock();
            if (node->refcount != 1 || !(node->type & file_delete) || vfs_node_retiring_locked(parent->node))
                error = EBUSY;
            else
            {
                entry->node = node;
                entry->tree = state;
                entry->inode = state->next_inode++;
                entry->backend_pin = true;
                entry->next = state->entries;
                state->entries = entry;
                ++state->entry_count;
                ++node->refcount;
                node->handle = entry;
                node->type = (directory ? file_dir : file_none) | file_delete;
                node->fsid = managed_root_id;
                node->inode = entry->inode;
                node->blksz = PAGE_SIZE;
                if (!node->mode) node->mode = directory ? 0777 : 0666;
            }
            vfs_child_unlock();
        }
    }
    if (error) free(entry);
    return error ? ram_error(error) : 0;
}

errno_t ram_mkdir(void *parent, const char *name, vfs_node_t node) { return ram_create(parent, name, node, true); }
errno_t ram_mkfile(void *parent, const char *name, vfs_node_t node) { return ram_create(parent, name, node, false); }
errno_t ram_unsupported_mk(void *, const char *, vfs_node_t) { return ram_error(EOPNOTSUPP); }
size_t ram_readlink(vfs_node_t, void *, size_t, size_t) { ram_error(EOPNOTSUPP); return size_t(-1); }
errno_t ram_ioctl(void *, size_t command, void *)
{
    if (command == IOBLKSYNC) return 0; // RAM data is already visible; no disk durability is implied.
    return ram_error(EOPNOTSUPP);
}
vfs_node_t ram_dup(vfs_node_t) { ram_error(EOPNOTSUPP); return NULL; }
errno_t ram_poll(void *, size_t events) { return events & 5; }
void *ram_map(void *, void *, size_t, size_t, size_t, size_t) { ram_error(EOPNOTSUPP); return NULL; }

errno_t ram_delete(void *parent_handle, vfs_node_t node)
{
    auto *parent = static_cast<PrivateEntry *>(parent_handle);
    auto *entry = node ? static_cast<PrivateEntry *>(node->handle) : NULL;
    if (!ram_entry_valid(parent) || !ram_entry_valid(entry) || parent->tree != entry->tree) return ram_error(EIO);
    RamLock tree_lock(&entry->tree->lock);
    vfs_child_lock();
    int error = node == entry->tree->root || node->is_mount || node->parent != parent->node ||
                !(node->type & file_delete) || entry->deleted || !entry->backend_pin ? EBUSY :
                (node->type & file_dir) && node->child ? ENOTEMPTY : node->refcount < 2 ? EIO : 0;
    if (!error)
    {
        entry->deleted = true;
        entry->backend_pin = false;
        --node->refcount;
    }
    vfs_child_unlock();
    return error ? ram_error(error) : 0;
}

int ram_finalize(void *handle, vfs_node_t node)
{
    auto *entry = static_cast<PrivateEntry *>(handle);
    if (!ram_entry_valid(entry) || entry->node != node) return ram_error(EIO);
    PrivateTree *state = entry->tree;
    uint8_t *data = NULL;
    {
        RamLock tree_lock(&state->lock);
        RamLock entry_lock(&entry->lock);
        if (!entry->deleted || entry->backend_pin || !state->entry_count || state->data_bytes < entry->bytes)
        {
            __atomic_store_n(&state->quarantined, true, __ATOMIC_RELEASE);
            return ram_error(EIO);
        }
        PrivateEntry **cursor = &state->entries;
        while (*cursor && *cursor != entry) cursor = &(*cursor)->next;
        if (!*cursor) { __atomic_store_n(&state->quarantined, true, __ATOMIC_RELEASE); return ram_error(EIO); }
        *cursor = entry->next;
        --state->entry_count;
        state->data_bytes -= entry->bytes;
        data = entry->data;
        entry->node = NULL;
    }
    free(data);
    free(entry);
    return 0;
}

errno_t ram_rename(void *handle, const char *destination)
{
    auto *entry = static_cast<PrivateEntry *>(handle);
    if (!ram_entry_valid(entry) || !destination || !destination[0]) return ram_error(EINVAL);
    size_t length = 0;
    while (length < 512 && destination[length]) ++length;
    if (length == 512) return ram_error(ENAMETOOLONG);
    char *path = strdup(destination);
    if (!path) return ram_error(ENOMEM);
    char *slash = strrchr(path, '/');
    char *name = slash ? slash + 1 : path;
    if (!name[0] || strlen(name) > 255 || !strcmp(name, ".") || !strcmp(name, "..") ||
        (slash && path[0] != '/')) { free(path); return ram_error(EINVAL); }
    char *copy = strdup(name);
    if (!copy) { free(path); return ram_error(ENOMEM); }
    vfs_node_t node = entry->node;
    vfs_node_t parent = NULL;
    if (slash)
    {
        if (slash == path) path[1] = '\0';
        else *slash = '\0';
        parent = vfs_open(path);
    }
    else
    {
        vfs_child_lock();
        parent = node->parent;
        if (parent && parent != rootdir && parent->refcount != __UINT32_MAX__) ++parent->refcount;
        else if (parent != rootdir) parent = NULL;
        vfs_child_unlock();
    }
    int error = !parent ? ENOENT : parent->fsid != managed_root_id || parent->root != node->root ? EXDEV :
                !(parent->type & file_dir) ? ENOTDIR : 0;
    if (!error && !vfs_check_child_creation(parent, copy)) error = vfs_last_error() ? vfs_last_error() : EACCES;
    char *retired_name = NULL;
    if (!error)
    {
        RamLock tree_lock(&entry->tree->lock);
        vfs_child_lock();
        vfs_node_t old_parent = node->parent;
        if (!old_parent || node == entry->tree->root || node->is_mount || entry->deleted ||
            (node->type & file_delete) || vfs_node_retiring_locked(node) ||
            vfs_node_retiring_locked(parent) || (parent->type & file_delete)) error = EBUSY;
        unsigned depth = 0;
        for (vfs_node_t ancestor = parent; ancestor && !error; ancestor = ancestor->parent)
            if (ancestor == node || ++depth > private_depth_limit) error = EINVAL;
        for (list_t cell = parent->child; cell && !error; cell = cell->next)
        {
            auto *other = static_cast<vfs_node_t>(cell->data);
            if (other != node && other->name && !strcmp(other->name, copy))
                error = other->type & file_delete ? EBUSY : EEXIST;
        }
        list_t moving = old_parent ? old_parent->child : NULL;
        while (moving && moving->data != node) moving = moving->next;
        if (!error && !moving) error = ENOENT;
        if (!error)
        {
            if (old_parent != parent)
            {
                if (moving->prev) moving->prev->next = moving->next;
                else old_parent->child = moving->next;
                if (moving->next) moving->next->prev = moving->prev;
                moving->prev = NULL;
                moving->next = parent->child;
                if (moving->next) moving->next->prev = moving;
                parent->child = moving;
                node->parent = parent;
            }
            retired_name = node->name;
            node->name = copy;
            copy = NULL;
        }
        vfs_child_unlock();
    }
    free(retired_name);
    free(copy);
    free(path);
    if (parent) vfs_close(parent);
    return error ? ram_error(error) : 0;
}

vfs_callback ram_callbacks = {
    .mount = ram_mount, .unmount = ram_unmount, .open = ram_open, .close = ram_close,
    .read = ram_read, .write = ram_write, .readlink = ram_readlink, .mkdir = ram_mkdir,
    .mkfile = ram_mkfile, .link = ram_unsupported_mk, .symlink = ram_unsupported_mk,
    .stat = ram_stat, .ioctl = ram_ioctl, .dup = ram_dup, .poll = ram_poll,
    .map = ram_map, .resize = ram_resize, .del = ram_delete, .rename = ram_rename
};
} // namespace

int tmpfs_private_publish(TmpfsPrivateTree *tree)
{
    PrivateTree *state = NULL;
    int error = private_owner(tree, &state);
    if (error) return error;
    if (!state->sealed || state->published || !private_tree_valid(state)) return -EBUSY;
    if (!managed_root_id)
    {
        error = vfs_regist_checked("instroot", &ram_callbacks, 0, private_tree_magic, &managed_root_id);
        if (error) return error;
        vfs_set_managed_lifetime(managed_root_id, true);
        vfs_set_unlinked_lifetime(managed_root_id, true);
        vfs_set_finalize_node(managed_root_id, ram_finalize);
        vfs_set_checked_unmount(managed_root_id, ram_unmount_checked);
        vfs_set_reject_overmount(managed_root_id, true);
    }
    vfs_node_t prepared = state->root;
    PrivateEntry *root_entry = private_entry(state, prepared);
    if (!root_entry) return -EIO;
    for (PrivateEntry *entry = state->entries; entry; entry = entry->next)
    {
        entry->node->fsid = managed_root_id;
        entry->node->inode = entry->inode;
        entry->node->root = rootdir;
        if (entry->node != prepared) { ++entry->node->refcount; entry->backend_pin = true; }
    }
    vfs_node_t *bindings[] = {&state->root, &root_entry->node};
    list_t covered = NULL;
    error = vfs_publish_system_tree(prepared, state->entry_count, bindings, 2, &covered);
    if (error)
    {
        for (PrivateEntry *entry = state->entries; entry; entry = entry->next)
        {
            entry->node->fsid = 0;
            entry->node->root = prepared;
            if (entry->backend_pin) { --entry->node->refcount; entry->backend_pin = false; }
        }
        return error;
    }
    state->published = true;
    // Covered placeholders were private and empty; no public reference can
    // exist. Dispose only after the atomic commit and outside all spin locks.
    while (covered)
    {
        list_t next = covered->next;
        auto *node = static_cast<vfs_node_t>(covered->data);
        auto *entry = static_cast<PrivateEntry *>(node->handle);
        {
            RamLock lock(&state->lock);
            PrivateEntry **cursor = &state->entries;
            while (*cursor != entry) cursor = &(*cursor)->next;
            *cursor = entry->next;
            --state->entry_count;
        }
        free(node->name);
        free(node);
        free(entry);
        free(covered);
        covered = next;
    }
    free(prepared->name);
    free(prepared);
    return 0;
}

int tmpfs_private_mountpoint(TmpfsPrivateTree *tree, const char *path)
{
    PrivateTree *state = NULL;
    int error = private_owner(tree, &state);
    if (error) return error;
    if (!state->published || !state->sealed || (error = private_path(path))) return error ? error : -EBUSY;
    // Boot owns this transition before starting runtime services. Ordinary
    // users cannot release a RAM namespace pin or bypass reject-overmount.
    // mkdir is idempotent for existing directories. A VFS cache miss can
    // return NULL without setting ENOENT, so do not infer existence from errno.
    if (vfs_mkdir(path) < 0) return -vfs_last_error();
    vfs_node_t node = vfs_open(path);
    if (!node) return -vfs_last_error();
    PrivateEntry *retired = NULL;
    {
        RamLock lock(&state->lock);
        vfs_child_lock();
        auto *entry = static_cast<PrivateEntry *>(node->handle);
        if (node == rootdir || node->is_mount || node->type != file_dir || node->child || node->linkto ||
            node->fsid != managed_root_id || node->root != rootdir || node->refcount != 3 ||
            vfs_node_retiring_locked(node) || !entry || entry->tree != state || entry->node != node ||
            entry->deleted || !entry->backend_pin || entry->bytes || entry->data) error = -EBUSY;
        else
        {
            PrivateEntry **cursor = &state->entries;
            while (*cursor && *cursor != entry) cursor = &(*cursor)->next;
            if (!*cursor || !state->entry_count) error = -EIO;
            else
            {
                *cursor = entry->next;
                --state->entry_count;
                --node->refcount; // Release the backend pin; cache + our open remain.
                node->handle = NULL;
                node->fsid = 0;
                node->root = node;
                node->inode = 0;
                node->visited = false;
                retired = entry;
            }
        }
        vfs_child_unlock();
    }
    free(retired);
    vfs_close(node);
    return error;
}
