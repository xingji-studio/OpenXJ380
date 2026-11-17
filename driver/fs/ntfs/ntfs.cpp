// SPDX-License-Identifier: Apache-2.0
// Original XJ380 VFS glue. No upstream kernel/transaction dependencies.
#include "native.h"
#include "write.h"
#include <block_access.h>
#include <device.h>
#include <errno.h>
#include <fs/ntfs/ntfs.h>
#include <input_method.h>
#include <krlibc.h>
#include <mutex.h>
#include <rtc.h>
#include <task/scheduler.h>
#include <user/permissions.h>

namespace {

struct Mount;
struct Node
{
    Mount        *mount;
    xjntfs::Inode inode;
    Node         *next;
    vfs_node_t    node;
    bool          deleted;
};

struct Mount
{
    xjntfs::Volume          volume;
    xjntfs::Workspace       work;
    xjntfs::WriteSession   *writer;
    xjntfs::WriteWorkspace *write_work;
    vfs_node_t              device;
    BlockExtent             extent;
    BlockAccessToken        lease;
    Node                   *handles;
    int                     fsid;
    bool                    io_failed, writable;
};

static mutex_t operation_lock;
static Mount  *mounts[16];
static int     filesystem_id, writable_id;
static bool    initialized, poisoned;
static tcb_t   operation_owner;
static size_t  operation_depth;

static bool failed()
{
    return __atomic_load_n(&poisoned, __ATOMIC_ACQUIRE);
}

static int failure(int error)
{
    if (error < 0) permissions_set_file_error(-error);
    return error;
}

static void poison_mounts()
{
    __atomic_store_n(&poisoned, true, __ATOMIC_RELEASE);
    for (Mount *mount : mounts)
        if (mount)
        {
            mount->io_failed = true;
            if (mount->lease) block_access_poison(mount->lease);
        }
}

// A workspace and the VFS cache are serialized with a sleepable mutex. Owner
// recovery does not prove an interrupted cache publication or DMA is complete:
// poison the driver and retain all affected objects until reboot.
class Guard {
    tcb_t task   = nullptr;
    bool  locked = false;

  public:
    Guard()
    {
        bool recovered = false;
        if (mutex_lock_recoverable(&operation_lock, &recovered))
        {
            __atomic_store_n(&poisoned, true, __ATOMIC_RELEASE);
            return;
        }
        locked = true;
        task   = get_current_task();
        if (recovered || (operation_depth && operation_owner != task))
        {
            operation_depth = 0;
            operation_owner = nullptr;
            poison_mounts();
        }
        if (failed()) poison_mounts();
        operation_owner = task;
        ++operation_depth;
        if (task) ++task->ime_storage_depth;
    }

    ~Guard()
    {
        if (!locked) return;
        if (task) --task->ime_storage_depth;
        if (operation_owner == task && operation_depth && !--operation_depth) operation_owner = nullptr;
        mutex_unlock(&operation_lock);
    }
};

static bool read_device(void *context, uint64_t offset, void *output, xjntfs::Size length)
{
    auto *mount = static_cast<Mount *>(context);
    if (failed() || mount->io_failed || offset > mount->extent.bytes || length > mount->extent.bytes - offset)
        return false;
    BlockExtent current;
    int         error = block_access_resolve_node(mount->device, &current);
    bool        same  = !error && current.device == mount->extent.device &&
                current.physical_device == mount->extent.physical_device &&
                current.generation == mount->extent.generation &&
                current.physical_generation == mount->extent.physical_generation &&
                current.start == mount->extent.start && current.bytes == mount->extent.bytes;
    if (same && vfs_read(mount->device, output, offset, length) == length) return true;
    mount->io_failed = true;
    block_access_poison(mount->lease);
    return false;
}

static bool device_current(Mount *mount)
{
    BlockExtent current;
    int         error = block_access_resolve_node(mount->device, &current);
    return !error && current.device == mount->extent.device && current.generation == mount->extent.generation &&
           current.physical_device == mount->extent.physical_device &&
           current.physical_generation == mount->extent.physical_generation && current.start == mount->extent.start &&
           current.bytes == mount->extent.bytes;
}

static bool write_device(void *context, uint64_t offset, const void *input, xjntfs::Size length)
{
    auto *mount = static_cast<Mount *>(context);
    if (!mount->writable || failed() || mount->io_failed || !device_current(mount) || offset > mount->extent.bytes ||
        length > mount->extent.bytes - offset ||
        vfs_write(mount->device, const_cast<void *>(input), offset, length) != length)
    {
        mount->io_failed = true;
        block_access_poison(mount->lease);
        return false;
    }
    return true;
}

static bool flush_device(void *context)
{
    auto *mount = static_cast<Mount *>(context);
    if (failed() || mount->io_failed || !device_current(mount) || block_access_require_flush(mount->extent) ||
        vfs_ioctl(mount->device, BlockSyncRequest, nullptr))
    {
        mount->io_failed = true;
        block_access_poison(mount->lease);
        return false;
    }
    return true;
}

static Node *attach(Mount *mount, const xjntfs::Inode &inode)
{
    auto *handle = static_cast<Node *>(calloc(1, sizeof(Node)));
    if (!handle) return nullptr;
    handle->mount  = mount;
    handle->inode  = inode;
    handle->next   = mount->handles;
    mount->handles = handle;
    return handle;
}

static void release(Node *handle)
{
    Node **entry = &handle->mount->handles;
    while (*entry && *entry != handle)
        entry = &(*entry)->next;
    if (*entry) *entry = handle->next;
    free(handle);
}

static int destroy(Mount *mount)
{
    if (failed() || mount->io_failed) return failure(-EIO);
    // Never free objects on an uncertain lease/lifetime failure.
    if (mount->lease && block_access_release(mount->lease))
    {
        mount->io_failed = true;
        return failure(-EIO);
    }
    mount->lease = 0;
    while (mount->handles)
        release(mount->handles);
    free(mount->writer);
    free(mount->write_work);
    for (Mount *&slot : mounts)
        if (slot == mount) slot = nullptr;
    vfs_close(mount->device);
    free(mount);
    return 0;
}

static uint64_t unix_nanos(uint64_t ntfs_time)
{
    constexpr uint64_t Epoch = 116444736000000000ULL;
    if (ntfs_time < Epoch || ntfs_time - Epoch > __UINT64_MAX__ / 100) return 0;
    return (ntfs_time - Epoch) * 100;
}

static void metadata(Node *handle, vfs_node_t node)
{
    handle->node               = node;
    const xjntfs::Inode &inode = handle->inode;
    node->type                 = inode.directory ? file_dir : file_none;
    node->inode                = inode.reference;
    node->size                 = inode.directory ? 0 : inode.size;
    node->realsize             = inode.allocated;
    node->blksz                = handle->mount->volume.cluster_size();
    node->owner = node->group = 0;
    // A documented local data-volume policy, not an implementation of NT ACLs.
    node->permissions = node->mode = inode.directory ? 0555 : 0444;
    if (handle->mount->writable && !inode.unsupported)
    {
        if (handle->mount->writer && (inode.directory || !(inode.attributes & (1U | 4U))))
            node->permissions = node->mode = inode.directory ? 0755 : 0644;
        else if (inode.nonresident && !inode.directory && !(inode.attributes & (1U | 4U)))
            node->permissions = node->mode = 0644;
    }
    node->createtime = unix_nanos(inode.created);
    node->writetime  = unix_nanos(inode.modified);
    node->readtime   = unix_nanos(inode.accessed);
    node->dev        = handle->mount->device->rdev;
    ime_vfs_resolved_name(node, node->name);
}

struct DirectoryContext
{
    Mount     *mount;
    vfs_node_t prepared, parent;
};

static int directory_entry(void *context, const xjntfs::DirectoryEntry &entry)
{
    auto *directory = static_cast<DirectoryContext *>(context);
    // Reserved NTFS metadata files are not ordinary user data or applications.
    if ((entry.reference & 0x0000ffffffffffffULL) < 16) return 0;
    if (vfs_do_search(directory->prepared, entry.name)) return -EIO;
    xjntfs::Inode inode = {};
    inode.reference     = entry.reference;
    inode.directory     = entry.directory;
    inode.size          = entry.size;
    inode.attributes    = entry.attributes;
    Node *handle        = attach(directory->mount, inode);
    if (!handle) return -ENOMEM;
    vfs_node_t child = vfs_node_alloc(nullptr, entry.name);
    list_t     link  = child ? list_alloc(child) : nullptr;
    if (!child || !link)
    {
        if (child)
        {
            free(child->name);
            free(child);
        }
        release(handle);
        return -ENOMEM;
    }
    child->parent = directory->parent;
    child->root   = directory->prepared->root;
    child->fsid   = directory->mount->fsid;
    child->handle = handle;
    metadata(handle, child);
    // The tree is private; neither VFS nor another reader can retain it yet.
    link->next = directory->prepared->child;
    if (link->next) link->next->prev = link;
    directory->prepared->child = link;
    return 0;
}

static int update(Node *handle, vfs_node_t node)
{
    Mount *mount = handle->mount;
    if (handle->deleted) return failure(-ENOENT);
    if (failed() || mount->io_failed) return failure(-EIO);
    int error = mount->volume.read_inode(handle->inode.reference, handle->inode, mount->work);
    if (error) return failure(error);
    metadata(handle, node);
    if (!handle->inode.directory) return 0;
    // A busy unmount clears visited while keeping the existing cached tree.
    // This immutable backend publishes only complete child lists. Preserve
    // their vnode identities and external references rather than replacing
    // the tree on the next lookup (which would orphan open cached nodes).
    vfs_child_lock();
    bool cached = node->visited || node->child;
    if (cached) node->visited = true;
    vfs_child_unlock();
    if (cached) return 0;
    struct vfs_node prepared = *node;
    prepared.child           = nullptr;
    DirectoryContext context = {mount, &prepared, node};
    error                    = mount->volume.list_directory(handle->inode, directory_entry, &context, mount->work);
    if (error)
    {
        vfs_free_child(&prepared);
        return failure(error);
    }
    vfs_child_lock();
    node->child   = prepared.child;
    node->visited = true;
    vfs_child_unlock();
    return 0;
}

static int mount_common(const char *source, vfs_node_t node, bool writable)
{
    if (!source || is_virtual_fs(source) || !node || node == rootdir || node->is_mount || node->child) return -EINVAL;
    if (writable && strncmp(source, "ntfsrw:", 7)) return -EINVAL;
    if (!strncmp(source, writable ? "ntfsrw:" : "ntfsro:", 7)) source += 7;
    if (source[0] != '/') return -EINVAL;
    if (failed()) return failure(-EIO);
    // Resolve before the driver mutex: path lookup may call another backend.
    tcb_t task = get_current_task();
    if (task) ++task->ime_storage_depth;
    vfs_node_t device = vfs_open(source);
    if (task) --task->ime_storage_depth;
    if (!device || !(device->type & file_block))
    {
        if (device) vfs_close(device);
        return -ENODEV;
    }
    Guard guard;
    if (failed())
    {
        vfs_close(device);
        return failure(-EIO);
    }
    unsigned slot = 16;
    for (unsigned i = 0; i < 16; ++i)
        if (!mounts[i])
        {
            slot = i;
            break;
        }
    if (slot == 16)
    {
        vfs_close(device);
        return failure(-ENOSPC);
    }
    auto *mount = static_cast<Mount *>(calloc(1, sizeof(Mount)));
    if (!mount)
    {
        vfs_close(device);
        return failure(-ENOMEM);
    }
    mount->device   = device;
    mount->writable = writable;
    mount->fsid     = writable ? writable_id : filesystem_id;
    mounts[slot]    = mount; // Track even an unpublished mount across owner death.
    int error       = block_access_resolve_node(device, &mount->extent);
    if (!error && writable) error = block_access_require_flush(mount->extent);
    if (!error) error = block_access_claim(mount->extent, false, &mount->lease);
    if (!error && writable)
    {
        auto    *physical       = get_device(mount->extent.physical_device);
        unsigned logical_sector = physical && physical->sector_size <= 4096 ? unsigned(physical->sector_size) : 0;
        if (!logical_sector || logical_sector > 4096 || mount->extent.start % logical_sector ||
            mount->extent.bytes % logical_sector)
            error = -EINVAL;
        mount->writer     = static_cast<xjntfs::WriteSession *>(calloc(1, sizeof(xjntfs::WriteSession)));
        mount->write_work = static_cast<xjntfs::WriteWorkspace *>(calloc(1, sizeof(xjntfs::WriteWorkspace)));
        if (!mount->writer || !mount->write_work)
            error = -ENOMEM;
        else if (!error)
            error = mount->writer->open(
                {read_device, write_device, flush_device, mount, mount->extent.bytes, logical_sector}, mount->volume,
                mount->work, *mount->write_work);
        if (error == -EOPNOTSUPP && mount->writer && !mount->writer->failed() && !mount->io_failed)
        {
            // Preserve the separately audited initialized-data-only profile on
            // geometries/log versions outside the metadata writer's bounds.
            free(mount->writer);
            free(mount->write_work);
            mount->writer     = nullptr;
            mount->write_work = nullptr;
            error             = mount->volume.mount(read_device, mount, mount->extent.bytes, mount->work);
            if (!error) error = mount->volume.enable_overwrite(write_device, flush_device, mount->work);
        }
    }
    else if (!error)
        error = mount->volume.mount(read_device, mount, mount->extent.bytes, mount->work);
    xjntfs::Inode root;
    if (!error) error = mount->volume.root(root, mount->work);
    Node *handle = error ? nullptr : attach(mount, root);
    if (!error && !handle) error = -ENOMEM;
    struct vfs_node previous = *node;
    struct vfs_node prepared = previous;
    prepared.child           = nullptr;
    prepared.handle          = handle;
    prepared.fsid            = mount->fsid;
    prepared.root            = node;
    prepared.visited         = false;
    if (!error) error = update(handle, &prepared);
    if (!error)
    {
        for (list_t item = prepared.child; item; item = item->next)
            static_cast<vfs_node_t>(item->data)->parent = node;
        error = vfs_publish_readonly_mount(node, &prepared);
    }
    if (error)
    {
        vfs_free_child(&prepared);
        (void)destroy(mount);
        return failure(error);
    }
    vfs_filesystem_t old = get_filesystem_node(&previous);
    if (previous.handle && old) old->callback->close(previous.handle);
    handle->node = node;
    write_serial_string(writable ? "NTFS: P3 experimental writable volume mounted\n"
                                 : "NTFS: P3 Apache-2.0 read-only data volume mounted\n");
    return 0;
}

static int mount_volume(const char *source, vfs_node_t node)
{
    return mount_common(source, node, false);
}

static int mount_writable(const char *source, vfs_node_t node)
{
    return mount_common(source, node, true);
}

static int prepare_unmount(void *context)
{
    Guard guard;
    if (!context || failed()) return failure(-EIO);
    auto *mount = static_cast<Node *>(context)->mount;
    if (mount->io_failed) return failure(-EIO);
    return failure(mount->writer ? mount->writer->sync() : mount->volume.sync());
}

static int unmount_checked(void *context)
{
    Guard guard;
    if (!context) return failure(-EINVAL);
    return destroy(static_cast<Node *>(context)->mount);
}

static void unmount_volume(void *context)
{
    (void)unmount_checked(context);
}

static int finalize_node(void *context, vfs_node_t)
{
    Guard guard;
    if (!context) return 0;
    auto *handle = static_cast<Node *>(context);
    if (failed() || handle->mount->io_failed) return failure(-EIO);
    release(handle);
    return 0;
}

static void close_node(void *context)
{
    (void)finalize_node(context, nullptr);
}

static void open_node(void *, const char *, vfs_node_t node)
{
    Guard guard;
    if (failed())
    {
        failure(-EIO);
        return;
    }
    if (!node->handle)
    {
        auto *root = node->root ? static_cast<Node *>(node->root->handle) : nullptr;
        if (!root || root->mount->io_failed)
        {
            failure(-EIO);
            return;
        }
        xjntfs::Inode inode = {};
        inode.reference     = node->inode;
        node->handle        = attach(root->mount, inode);
        if (!node->handle)
        {
            failure(-ENOMEM);
            return;
        }
    }
    (void)update(static_cast<Node *>(node->handle), node);
}

static int stat_node(void *context, vfs_node_t node)
{
    Guard guard;
    return context && node ? update(static_cast<Node *>(context), node) : failure(-EINVAL);
}

static size_t read_node(void *context, void *output, size_t offset, size_t length)
{
    Guard guard;
    auto *handle = static_cast<Node *>(context);
    if (!handle)
    {
        failure(-EINVAL);
        return size_t(-1);
    }
    if (failed() || handle->mount->io_failed)
    {
        failure(-EIO);
        return size_t(-1);
    }
    xjntfs::Size completed = 0;
    int error = handle->mount->volume.read(handle->inode, offset, output, length, completed, handle->mount->work);
    if (error)
    {
        failure(error);
        return size_t(-1);
    }
    return completed;
}

static int lookup_name(vfs_node_t directory, const char *name, char *canonical, size_t capacity)
{
    Guard guard;
    if (failed() || !directory || !directory->handle) return failure(-EIO);
    auto *handle = static_cast<Node *>(directory->handle);
    if (handle->mount->io_failed) return failure(-EIO);
    if (vfs_do_search(directory, name))
    {
        size_t length = strlen(name);
        if (length >= capacity) return failure(-ENAMETOOLONG);
        memcpy(canonical, name, length + 1);
        return 0;
    }
    xjntfs::DirectoryEntry entry;
    int                    error = handle->mount->volume.lookup_entry(handle->inode, name, entry, handle->mount->work);
    if (error) return failure(error);
    size_t length = strlen(entry.name);
    if (length >= capacity) return failure(-ENAMETOOLONG);
    memcpy(canonical, entry.name, length + 1);
    return 0;
}

static uint64_t ntfs_time()
{
    return 116444736000000000ULL + realtime_ns() / 100;
}

static int mutation_result(Mount *mount, int error)
{
    if (failed() || (mount->writer && mount->writer->failed()))
    {
        mount->io_failed = true;
        block_access_poison(mount->lease);
    }
    return failure(mount->io_failed ? -EIO : error);
}

static int mutable_mount(Mount *mount, bool metadata_write = true)
{
    if (failed() || mount->io_failed) return -EIO;
    if (!mount->writable) return -EROFS;
    if (mount->writer && mount->writer->failed()) return mutation_result(mount, -EIO);
    return metadata_write && !mount->writer ? -EOPNOTSUPP : 0;
}

static int refresh(Mount *mount, uint64_t first, uint64_t second = 0, uint64_t third = 0)
{
    for (Node *entry = mount->handles; entry; entry = entry->next)
    {
        if (entry->deleted ||
            (entry->inode.reference != first && entry->inode.reference != second && entry->inode.reference != third))
            continue;
        int error = mount->volume.read_inode(entry->inode.reference, entry->inode, mount->work);
        if (error) return mutation_result(mount, error);
        if (entry->node) metadata(entry, entry->node);
    }
    return 0;
}

static size_t write_node(void *context, const void *input, size_t offset, size_t length)
{
    Guard guard;
    auto *handle = static_cast<Node *>(context);
    if (!handle)
    {
        failure(-EINVAL);
        return size_t(-1);
    }
    Mount       *mount     = handle->mount;
    int          error     = handle->deleted ? -ENOENT : mutable_mount(mount, false);
    xjntfs::Size completed = 0;
    if (!error)
    {
        if (mount->writer)
            error = mount->writer->pwrite(handle->inode, offset, input, length, ntfs_time(), completed);
        else
            error = mount->volume.overwrite(handle->inode, offset, input, length, completed, mount->work);
    }
    if (!error && mount->writer) error = refresh(mount, handle->inode.reference);
    error = mutation_result(mount, error);
    return error ? size_t(-1) : completed;
}

static int make_node(void *context, const char *name, vfs_node_t node, bool directory)
{
    Guard guard;
    auto *parent = static_cast<Node *>(context);
    if (!parent || !node) return failure(-EINVAL);
    Mount *mount = parent->mount;
    int    error = mutable_mount(mount);
    if (error) return failure(error);
    // Allocate the handle before committing the on-disk name. VFS reserves the
    // unpublished vnode and list cell before entering this callback.
    auto *handle = static_cast<Node *>(calloc(1, sizeof(Node)));
    if (!handle) return failure(-ENOMEM);
    error = mount->writer->create(parent->inode, name, directory, ntfs_time(), handle->inode);
    if (!error)
    {
        handle->mount  = mount;
        handle->next   = mount->handles;
        mount->handles = handle;
        node->handle   = handle;
        node->root     = parent->node->root;
        node->fsid     = mount->fsid;
        metadata(handle, node);
        error = refresh(mount, parent->inode.reference);
    }
    else
        free(handle);
    return mutation_result(mount, error);
}

static int make_file(void *parent, const char *name, vfs_node_t node)
{
    return make_node(parent, name, node, false);
}

static int make_directory(void *parent, const char *name, vfs_node_t node)
{
    return make_node(parent, name, node, true);
}

static int unsupported_mutation(void *context, const char *, vfs_node_t)
{
    auto *handle = static_cast<Node *>(context);
    return failure(handle && handle->mount->writable ? -EOPNOTSUPP : -EROFS);
}

static int delete_node(void *context, vfs_node_t node)
{
    Guard guard;
    auto *parent = static_cast<Node *>(context);
    auto *handle = node ? static_cast<Node *>(node->handle) : nullptr;
    if (!parent || !handle || parent->mount != handle->mount || !node->parent || node->parent->handle != parent ||
        node->is_mount || node == node->root)
        return failure(-EBUSY);
    Mount *mount = handle->mount;
    int    error = mutable_mount(mount);
    // No deferred orphan implementation: reject open aliases before unlink.
    // Tree ownership and the VFS delete caller account for two references.
    vfs_child_lock();
    for (Node *entry = mount->handles; !error && entry; entry = entry->next)
        if (!entry->deleted && entry->inode.reference == handle->inode.reference && entry->node &&
            entry->node->refcount > unsigned(1 + (entry->node == node)))
            error = -EBUSY;
    vfs_child_unlock();
    if (!error) error = mount->writer->unlink(parent->inode, node->name);
    if (!error)
    {
        handle->deleted = true;
        error           = refresh(mount, parent->inode.reference);
    }
    return mutation_result(mount, error);
}

static int rename_node(void *context, const char *destination)
{
    auto *handle = static_cast<Node *>(context);
    if (!handle || !handle->node || !destination || destination[0] != '/') return failure(-EINVAL);
    char *path = strdup(destination);
    if (!path) return failure(-ENOMEM);
    char *slash = strrchr(path, '/');
    char *name  = slash && slash[1] ? strdup(slash + 1) : nullptr;
    if (!name)
    {
        int error = slash && !slash[1] ? -EINVAL : -ENOMEM;
        free(path);
        return failure(error);
    }
    if (slash == path)
        path[1] = 0;
    else
        *slash = 0;
    vfs_node_t parent = vfs_open(path);
    free(path);
    if (!parent)
    {
        free(name);
        return failure(-ENOENT);
    }
    if (!vfs_check_child_creation(parent, name))
    {
        int error = vfs_last_error();
        vfs_close(parent);
        free(name);
        return failure(-error);
    }
    Guard      guard;
    Mount     *mount      = handle->mount;
    vfs_node_t node       = handle->node;
    auto      *target     = ntfsfs_owns_node(parent) ? static_cast<Node *>(parent->handle) : nullptr;
    int        error      = target && target->mount == mount ? mutable_mount(mount) : -EXDEV;
    list_t     moving     = nullptr;
    vfs_node_t old_parent = nullptr;
    bool       reserved   = false;
    vfs_child_lock();
    if (!error)
    {
        if (!node->parent || node->is_mount || node == node->root || (node->type & file_delete))
            error = -EBUSY;
        else
        {
            old_parent = node->parent;
            for (list_t entry = old_parent->child; entry; entry = entry->next)
                if (entry->data == node)
                {
                    moving = entry;
                    break;
                }
            if (!moving) error = -ENOENT;
        }
        // Namespace replacement is refused by the bounded writer. Reserve the
        // source while I/O runs; the sleepable operation mutex serializes NTFS.
        for (list_t entry = parent->child; !error && entry; entry = entry->next)
        {
            auto *child = static_cast<vfs_node_t>(entry->data);
            if (child != node && !strcmp(child->name, name)) error = -EEXIST;
        }
        if (!error)
        {
            node->type |= file_delete;
            reserved    = true;
        }
    }
    vfs_child_unlock();
    if (!error)
        error = mount->writer->rename(static_cast<Node *>(old_parent->handle)->inode, node->name, target->inode, name);
    char *old_name = nullptr;
    if (!error)
    {
        vfs_child_lock();
        if (moving->prev)
            moving->prev->next = moving->next;
        else
            old_parent->child = moving->next;
        if (moving->next) moving->next->prev = moving->prev;
        moving->prev = nullptr;
        moving->next = parent->child;
        if (moving->next) moving->next->prev = moving;
        parent->child  = moving;
        node->parent   = parent;
        old_name       = node->name;
        node->name     = name;
        name           = nullptr;
        node->type    &= ~file_delete;
        reserved       = false;
        vfs_child_unlock();
        error = refresh(mount, handle->inode.reference, target->inode.reference,
                        static_cast<Node *>(old_parent->handle)->inode.reference);
    }
    if (reserved)
    {
        vfs_child_lock();
        node->type &= ~file_delete;
        vfs_child_unlock();
    }
    free(old_name);
    free(name);
    vfs_close(parent);
    return mutation_result(mount, error);
}

static void resize_node(void *context, uint64_t size)
{
    Guard guard;
    auto *handle = static_cast<Node *>(context);
    if (!handle)
    {
        failure(-EINVAL);
        return;
    }
    Mount *mount = handle->mount;
    int    error = handle->deleted ? -ENOENT : mutable_mount(mount);
    if (!error) error = mount->writer->resize(handle->inode, size, ntfs_time());
    if (!error) error = refresh(mount, handle->inode.reference);
    (void)mutation_result(mount, error);
}

static int ioctl_node(void *context, size_t request, void *)
{
    Guard guard;
    auto *handle = static_cast<Node *>(context);
    if (handle && handle->mount->writable && request == BlockSyncRequest)
    {
        Mount *mount = handle->mount;
        return mutation_result(mount, mount->writer ? mount->writer->sync() : mount->volume.sync());
    }
    return failure(-EOPNOTSUPP);
}

static void *map_node(void *, void *, size_t, size_t, size_t, size_t)
{
    failure(-EOPNOTSUPP);
    return reinterpret_cast<void *>(-EOPNOTSUPP);
}

} // namespace

void ntfsfs_init()
{
    mutex_create(&operation_lock, true);
    static struct vfs_callback callbacks;
    callbacks         = vfs_empty_callback;
    callbacks.mount   = mount_volume;
    callbacks.unmount = unmount_volume;
    callbacks.open    = open_node;
    callbacks.close   = close_node;
    callbacks.stat    = stat_node;
    callbacks.read    = read_node;
    callbacks.write   = write_node;
    callbacks.mkfile  = make_file;
    callbacks.mkdir   = make_directory;
    callbacks.link = callbacks.symlink = unsupported_mutation;
    callbacks.del                      = delete_node;
    callbacks.rename                   = rename_node;
    callbacks.resize                   = resize_node;
    callbacks.ioctl                    = ioctl_node;
    callbacks.map                      = map_node;
    int error                          = vfs_regist_checked("ntfsro", &callbacks, 0, 0x5346544e, &filesystem_id);
    if (error)
    {
        write_serial_fmt("NTFS: registration failed %d\n", error);
        return;
    }
    vfs_set_readonly(filesystem_id, true);
    vfs_set_finalize_node(filesystem_id, finalize_node);
    vfs_set_checked_unmount(filesystem_id, unmount_checked);
    vfs_set_name_lookup(filesystem_id, lookup_name);
    static struct vfs_callback write_callbacks;
    write_callbacks       = callbacks;
    write_callbacks.mount = mount_writable;
    error                 = vfs_regist_checked("ntfsrw", &write_callbacks, 0, 0x5346544e, &writable_id);
    if (!error)
    {
        vfs_set_managed_lifetime(writable_id, true);
        vfs_set_prepare_unmount(writable_id, prepare_unmount);
        vfs_set_finalize_node(writable_id, finalize_node);
        vfs_set_checked_unmount(writable_id, unmount_checked);
        vfs_set_name_lookup(writable_id, lookup_name);
    }
    else
        write_serial_fmt("NTFS: experimental writer registration failed %d\n", error);
    __atomic_store_n(&initialized, true, __ATOMIC_RELEASE);
}

bool ntfsfs_owns_node(vfs_node_t node)
{
    return node &&
           ((filesystem_id > 0 && node->fsid == filesystem_id) || (writable_id > 0 && node->fsid == writable_id));
}

void ntfsfs_quiescent_exit()
{
    if (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE)) return;
    Guard guard;
}
