// SPDX-License-Identifier: Apache-2.0
#include "native.h"
#include <block_access.h>
#include <device.h>
#include <efi/boot.h>
#include <errno.h>
#include <fs/iso9660/iso9660.h>
#include <input_method.h>
#include <krlibc.h>
#include <mutex.h>
#include <syscall/syscall.h>
#include <task/scheduler.h>
#include <user/permissions.h>

extern BOOT_CONFIG *EFI_BC;

namespace {
struct Mount;
struct Node
{
    Mount       *mount;
    Node        *next, *hash_next;
    vfs_node_t   node;
    xjiso::Inode inode;
};

struct Mount
{
    xjiso::Volume    volume;
    xjiso::Workspace work;
    vfs_node_t       device;
    BlockExtent      extent;
    BlockAccessToken lease;
    Node            *handles;
    bool             io_failed;
};

mutex_t operation_lock;
Mount  *mounts[16];
int     filesystem_id;
bool    initialized, poisoned;
tcb_t   operation_owner;
size_t  operation_depth;

int failure(int error)
{
    if (error < 0) permissions_set_file_error(-error);
    return error;
}

bool failed()
{
    return __atomic_load_n(&poisoned, __ATOMIC_ACQUIRE);
}

void poison_mounts()
{
    __atomic_store_n(&poisoned, true, __ATOMIC_RELEASE);
    for (Mount *mount : mounts)
        if (mount)
        {
            mount->io_failed = true;
            if (mount->lease) block_access_poison(mount->lease);
        }
}

// Serialize workspace use and cache preparation with a sleepable mutex. An
// abandoned operation may have stopped inside DMA or publication: retain its
// context and reject I/O until reboot, matching the native storage contract.
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

bool same_extent(const BlockExtent &a, const BlockExtent &b)
{
    return a.device == b.device && a.physical_device == b.physical_device && a.generation == b.generation &&
           a.physical_generation == b.physical_generation && a.start == b.start && a.bytes == b.bytes;
}

bool read_device(void *context, uint64_t offset, void *output, xjiso::Size length)
{
    auto *mount = static_cast<Mount *>(context);
    if (failed() || mount->io_failed || offset > mount->extent.bytes || length > mount->extent.bytes - offset)
        return false;
    BlockExtent current;
    if (!block_access_resolve_node(mount->device, &current) && same_extent(current, mount->extent) &&
        vfs_read(mount->device, output, offset, length) == length)
        return true;
    mount->io_failed = true;
    block_access_poison(mount->lease);
    return false;
}

Node *attach(Mount *mount, const xjiso::Inode &inode)
{
    auto *handle = static_cast<Node *>(calloc(1, sizeof(Node)));
    if (!handle) return nullptr;
    handle->mount  = mount;
    handle->inode  = inode;
    handle->next   = mount->handles;
    mount->handles = handle;
    return handle;
}

void release(Node *handle)
{
    Node **link = &handle->mount->handles;
    while (*link && *link != handle)
        link = &(*link)->next;
    if (*link) *link = handle->next;
    free(handle);
}

int destroy(Mount *mount)
{
    if (failed() || mount->io_failed) return failure(-EIO);
    if (mount->lease && block_access_release(mount->lease))
    {
        mount->io_failed = true;
        return failure(-EIO);
    }
    while (mount->handles)
        release(mount->handles);
    for (Mount *&slot : mounts)
        if (slot == mount) slot = nullptr;
    vfs_close(mount->device);
    free(mount);
    return 0;
}

void metadata(Node *handle, vfs_node_t node)
{
    const xjiso::Inode &inode = handle->inode;
    handle->node              = node;
    node->type                = inode.directory ? file_dir : file_none;
    node->inode               = inode.identifier;
    node->size                = inode.directory ? 0 : inode.size;
    node->realsize            = inode.size;
    node->blksz               = xjiso::Sector;
    node->owner = node->group = 0;
    node->mode = node->permissions = inode.directory ? 0555 : 0444;
    node->createtime = node->readtime = node->writetime = inode.modified;
    node->dev                                           = handle->mount->device->rdev;
    ime_vfs_resolved_name(node, node->name);
}

unsigned name_hash(const char *name)
{
    unsigned hash = 2166136261U;
    while (*name)
        hash = (hash ^ uint8_t(*name++)) * 16777619U;
    return hash % 1024;
}

int update(Node *handle, vfs_node_t node)
{
    Mount *mount = handle->mount;
    if (failed() || mount->io_failed) return failure(-EIO);
    metadata(handle, node);
    if (!handle->inode.directory) return 0;
    vfs_child_lock();
    bool cached = node->visited || node->child;
    if (cached) node->visited = true;
    vfs_child_unlock();
    if (cached) return 0;
    // Build an unreachable, complete directory. Allocation and disk reads
    // never occur under the VFS child spin lock.
    auto **buckets = static_cast<Node **>(calloc(1024, sizeof(Node *)));
    if (!buckets) return failure(-ENOMEM);
    struct vfs_node prepared = *node;
    prepared.child           = nullptr;
    xjiso::Cursor cursor;
    xjiso::Entry  entry;
    int           error = mount->volume.cursor(handle->inode, cursor);
    while (!error)
    {
        int result = mount->volume.next(cursor, entry, mount->work);
        if (result <= 0)
        {
            error = result;
            break;
        }
        unsigned hash      = name_hash(entry.name);
        Node    *duplicate = buckets[hash];
        while (duplicate && strcmp(duplicate->node->name, entry.name))
            duplicate = duplicate->hash_next;
        if (duplicate)
        {
            if (entry.inode.version == duplicate->inode.version)
            {
                error = -EIO;
                break;
            }
            if (entry.inode.version > duplicate->inode.version)
            {
                duplicate->inode = entry.inode;
                metadata(duplicate, duplicate->node);
            }
            continue;
        }
        Node      *child_handle = attach(mount, entry.inode);
        vfs_node_t child        = child_handle ? vfs_node_alloc(nullptr, entry.name) : nullptr;
        list_t     link         = child ? list_alloc(child) : nullptr;
        if (!link)
        {
            if (child)
            {
                free(child->name);
                free(child);
            }
            if (child_handle) release(child_handle);
            error = -ENOMEM;
            break;
        }
        child->parent = node;
        child->root   = prepared.root;
        child->fsid   = filesystem_id;
        child->handle = child_handle;
        metadata(child_handle, child);
        child_handle->hash_next = buckets[hash];
        buckets[hash]           = child_handle;
        link->next              = prepared.child;
        if (link->next) link->next->prev = link;
        prepared.child = link;
    }
    free(buckets);
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

int mount_volume(const char *source, vfs_node_t node)
{
    if (!source || is_virtual_fs(source) || !node || node == rootdir || node->is_mount || node->child) return -EINVAL;
    if (!strncmp(source, "iso9660:", 8)) source += 8;
    if (source[0] != '/') return -EINVAL;
    if (failed()) return failure(-EIO);
    // Resolve the source before taking the backend mutex: another filesystem
    // may participate in path lookup. Only authentic devfs block nodes qualify.
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
    unsigned slot = 0;
    while (slot < 16 && mounts[slot])
        ++slot;
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
    mount->device = device;
    mounts[slot]  = mount;
    int error     = block_access_resolve_node(device, &mount->extent);
    if (!error) error = block_access_claim(mount->extent, false, &mount->lease);
    if (!error) error = mount->volume.mount(read_device, mount, mount->extent.bytes, mount->work);
    Node *handle = error ? nullptr : attach(mount, mount->volume.root());
    if (!error && !handle) error = -ENOMEM;
    struct vfs_node previous = *node;
    struct vfs_node prepared = previous;
    prepared.child           = nullptr;
    prepared.handle          = handle;
    prepared.fsid            = filesystem_id;
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
    write_serial_fmt("ISO9660: read-only %s volume mounted from %s\n", mount->volume.joliet() ? "Joliet" : "ISO",
                     source);
    return 0;
}

int finalize_node(void *context, vfs_node_t)
{
    Guard guard;
    if (!context) return 0;
    auto *handle = static_cast<Node *>(context);
    if (failed() || handle->mount->io_failed) return failure(-EIO);
    release(handle);
    return 0;
}

void close_node(void *context)
{
    (void)finalize_node(context, nullptr);
}

void open_node(void *parent_context, const char *name, vfs_node_t node)
{
    Guard guard;
    auto *parent = static_cast<Node *>(parent_context);
    if (!parent || !node || failed() || parent->mount->io_failed)
    {
        failure(-EIO);
        return;
    }
    xjiso::Entry entry;
    int          error = parent->mount->volume.lookup(parent->inode, name, entry, parent->mount->work);
    if (!error && entry.inode.identifier != node->inode) error = -ESTALE;
    if (!error)
    {
        node->handle = attach(parent->mount, entry.inode);
        if (!node->handle) error = -ENOMEM;
    }
    if (error)
    {
        failure(error);
        return;
    }
    (void)update(static_cast<Node *>(node->handle), node);
}

int stat_node(void *context, vfs_node_t node)
{
    Guard guard;
    return context && node ? update(static_cast<Node *>(context), node) : failure(-EINVAL);
}

size_t read_node(void *context, void *output, size_t offset, size_t length)
{
    Guard guard;
    auto *handle = static_cast<Node *>(context);
    if (!handle || failed() || handle->mount->io_failed)
    {
        failure(-EIO);
        return size_t(-1);
    }
    size_t completed = 0;
    int    error = handle->mount->volume.read(handle->inode, offset, output, length, completed, handle->mount->work);
    if (error)
    {
        failure(error);
        return size_t(-1);
    }
    return completed;
}

int unmount_checked(void *context)
{
    Guard guard;
    return context ? destroy(static_cast<Node *>(context)->mount) : failure(-EINVAL);
}

void unmount_volume(void *context)
{
    (void)unmount_checked(context);
}

int lookup_name(vfs_node_t parent, const char *name, char *canonical, size_t capacity)
{
    Guard guard;
    auto *handle = parent ? static_cast<Node *>(parent->handle) : nullptr;
    if (!handle || failed() || handle->mount->io_failed) return failure(-EIO);
    int error = update(handle, parent);
    if (error) return error;
    // Parent/cache lifetime is pinned by the VFS traversal guard. Only string
    // inspection and copying occur inside the child lock.
    vfs_child_lock();
    error = -ENOENT;
    for (list_t item = parent->child; item; item = item->next)
    {
        auto *child = static_cast<vfs_node_t>(item->data);
        if (!handle->mount->volume.names_equal(child->name, name)) continue;
        size_t size = strlen(child->name) + 1;
        if (size > capacity)
            error = -ENAMETOOLONG;
        else
        {
            memcpy(canonical, child->name, size);
            error = 0;
        }
        break;
    }
    vfs_child_unlock();
    return failure(error);
}

size_t write_node(void *, const void *, size_t, size_t)
{
    failure(-EROFS);
    return size_t(-1);
}
int make_node(void *, const char *, vfs_node_t)
{
    return failure(-EROFS);
}
int delete_node(void *, vfs_node_t)
{
    return failure(-EROFS);
}
int rename_node(void *, const char *)
{
    return failure(-EROFS);
}
void resize_node(void *, uint64_t)
{
    failure(-EROFS);
}
int ioctl_node(void *, size_t, void *)
{
    return failure(-EOPNOTSUPP);
}

void *map_node(void *context, void *address, size_t offset, size_t length, size_t protection, size_t flags)
{
    return general_map(read_node, context, uint64_t(address), length, protection, flags, offset);
}
} // namespace

void iso9660fs_init()
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
    callbacks.mkdir = callbacks.mkfile = callbacks.link = callbacks.symlink = make_node;
    callbacks.del                                                           = delete_node;
    callbacks.rename                                                        = rename_node;
    callbacks.resize                                                        = resize_node;
    callbacks.ioctl                                                         = ioctl_node;
    callbacks.map                                                           = map_node;
    int error = vfs_regist_checked("iso9660", &callbacks, 0, 0x9660, &filesystem_id);
    if (error)
    {
        write_serial_fmt("ISO9660: registration failed %d\n", error);
        return;
    }
    vfs_set_readonly(filesystem_id, true);
    vfs_set_finalize_node(filesystem_id, finalize_node);
    vfs_set_checked_unmount(filesystem_id, unmount_checked);
    vfs_set_name_lookup(filesystem_id, lookup_name);
    __atomic_store_n(&initialized, true, __ATOMIC_RELEASE);
}

bool iso9660fs_owns_node(vfs_node_t node)
{
    return node && filesystem_id > 0 && node->fsid == filesystem_id;
}

int iso9660fs_statfs(vfs_node_t node, struct statfs *result)
{
    Guard guard;
    if (!iso9660fs_owns_node(node) || !result || !node->root || !node->root->handle) return failure(-EINVAL);
    auto *handle = static_cast<Node *>(node->root->handle);
    if (failed() || handle->mount->io_failed) return failure(-EIO);
    memset(result, 0, sizeof(*result));
    result->f_type  = 0x9660;
    result->f_bsize = result->f_frsize = xjiso::Sector;
    result->f_blocks                   = handle->mount->volume.volume_bytes() / xjiso::Sector;
    result->f_namelen                  = 255;
    result->f_flags                    = 1; // ST_RDONLY; immutable media has no writable free space.
    return 0;
}

void iso9660fs_quiescent_exit()
{
    if (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE)) return;
    Guard guard;
}

void iso9660fs_try_automount(size_t id)
{
    if (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE) || failed() ||
        (EFI_BC && (EFI_BC->boot_flags & BOOT_FLAG_INSTALLER)))
        return;
    device_t *hardware = get_device(id);
    if (!hardware || !hardware->flag || hardware->type != DEVICE_BLOCK || hardware->size < 18U * xjiso::Sector) return;
    char source[96], path[96];
    snprintf(source, sizeof(source), "/dev/%s", hardware->drive_name);
    snprintf(path, sizeof(path), "/mnt/%s", hardware->drive_name);
    vfs_node_t device = vfs_open(source);
    if (!device) return;
    BlockExtent      extent = {};
    BlockAccessToken lease  = 0;
    int              error  = block_access_resolve_node(device, &extent);
    // Slices are handled by ordinary partition discovery. The temporary probe
    // lease also prevents reading across an installer's exclusive target.
    if (!error && (extent.start || extent.device != extent.physical_device)) error = -ENODEV;
    if (!error) error = block_access_claim(extent, false, &lease);
    uint8_t descriptor[7] = {};
    if (!error && vfs_read(device, descriptor, 16U * xjiso::Sector, sizeof(descriptor)) != sizeof(descriptor))
        error = -EIO;
    bool recognized = !error && !memcmp(descriptor + 1, "CD001", 5) && descriptor[6] == 1;
    if (lease && block_access_release(lease)) recognized = false;
    vfs_close(device);
    if (!recognized) return;
    vfs_mkdir("/mnt");
    vfs_mkdir(path);
    vfs_node_t point = vfs_open(path);
    if (!point) return;
    if (!point->is_mount)
    {
        char explicit_source[112];
        snprintf(explicit_source, sizeof(explicit_source), "iso9660:%s", source);
        if (!vfs_mount(explicit_source, point)) write_serial_fmt("ISO9660: automount %s -> %s\n", source, path);
    }
    vfs_close(point);
}

void iso9660fs_automount()
{
    for (size_t id = 0; id < 256; ++id)
        iso9660fs_try_automount(id);
}
