// SPDX-License-Identifier: Apache-2.0
// First-party VFS glue for native reads, journaled mutations and attributes.
#include "native.h"
#include "write.h"
#include "attributes.h"
#include "format.h"
#include <block_access.h>
#include <cpu/regio.h>
#include <device.h>
#include <fs/ext4/ext4.h>
#include <errno.h>
#include <input_method.h>
#include <installer/ext4_import.h>
#include <ioctl.h>
#include <krlibc.h>
#include <mutex.h>
#include <rtc.h>
#include <syscall/syscall.h>
#include <task/scheduler.h>
#include <user/permissions.h>

namespace {

struct Mount;
struct Node
{
    Mount *mount;
    xjext4::Inode inode;
    Node *next;
    vfs_node_t node;
    bool deleted;
    bool unlinked;
};

struct CompatibilityWriter
{
    xjext4::WriteSession *session;
    xjext4::WriteWorkspace *workspace;
    xjext4::WriteStorage storage;
};

struct Mount
{
    xjext4::Volume volume;
    vfs_node_t device;
    Node *handles;
    bool io_failed;
    bool writable;
    bool writer_open;
    int fsid;
    xjext4::WriteSession *writer;
    xjext4::WriteWorkspace *workspace;
    xjext4::WriteStorage storage;
    CompatibilityWriter *compatibility;
    BlockExtent extent;
    BlockAccessToken lease;
    Mount *quarantined_next;
    bool quarantined;
};

struct FormatContext
{
    vfs_node_t device;
    BlockExtent extent;
    BlockAccessToken lease;
    uint64_t io_offset, io_bytes;
    bool owns_lease;
    xjext4::FormatWorkspace *workspace;
    xjext4::FormatStorage storage;
    xjext4::FormatGeometry geometry;
    FormatContext *next;
    bool io_failed;
};

static mutex_t operation_lock;
static Mount *mounts[16];
static Mount *quarantined_mounts;
static FormatContext *format_contexts;
static int filesystem_id;
static int readonly_id;
static int refresh(Mount *mount, uint32_t first, uint32_t second = 0,
                   uint32_t third = 0, uint32_t fourth = 0);
static int mutation_result(Mount *mount, int error);

static bool inode_is_open(Mount *mount, uint32_t number, vfs_node_t current = NULL, unsigned caller_refs = 0)
{
    bool live = false;
    vfs_child_lock();
    for (Node *entry = mount->handles; entry; entry = entry->next)
    {
        if (entry->inode.number != number || !entry->node || entry->deleted) continue;
        unsigned owned = entry->unlinked ? 0 : 1; // Named vnodes own one directory-tree reference.
        if (entry->node == current) owned += caller_refs;
        if (entry->node->refcount > owned) { live = true; break; }
    }
    vfs_child_unlock();
    return live;
}

static const xjext4::Volume &view(Mount *mount)
{
    return mount->writable ? mount->writer->view() : mount->volume;
}

static uint64_t seconds_now() { return realtime_ns() / 1000000000ULL; }

// A dead owner can leave either a WS transaction or vnode publication midway.
// Recovery only restores mutual exclusion; that memory must never resume as a
// normal mount. Keep this latch until reboot, including unpublished mounts.
static bool operation_initialized;
static bool operation_poisoned;
static bool operations_finished;
static int finish_result;
static tcb_t operation_owner;
static size_t operation_depth;

static bool operation_has_failed()
{
    return __atomic_load_n(&operation_poisoned, __ATOMIC_ACQUIRE);
}

static void mark_io_failed(Mount *mount)
{
    mount->io_failed = true;
    if (mount->lease) block_access_poison(mount->lease);
}

static void poison_operations()
{
    __atomic_store_n(&operation_poisoned, true, __ATOMIC_RELEASE);
    for (Mount *mount : mounts) if (mount) mark_io_failed(mount);
    for (FormatContext *context = format_contexts; context; context = context->next)
    {
        context->io_failed = true;
        block_access_poison(context->lease);
    }
}

// The scoped storage privilege permits backend block I/O without granting
// applications raw device access. Check a caller's file permission before entry.
class Guard
{
    tcb_t task = NULL;
    bool locked = false;

public:
    Guard()
    {
        bool recovered = false;
        if (mutex_lock_recoverable(&operation_lock, &recovered))
        {
            // No protected mount traversal without ownership. Backend and
            // cleanup gates honor this latch immediately; the next successful
            // guard will publish io_failed on every registered mount.
            __atomic_store_n(&operation_poisoned, true, __ATOMIC_RELEASE);
            return;
        }
        locked = true;
        task = get_current_task();
        // Recursive guards of the same task are normal. A different owner
        // with an outstanding scope proves abandonment, independently of the
        // mutex recovery notification; never dereference the old task pointer.
        if (recovered || (operation_depth && operation_owner != task))
        {
            operation_depth = 0;
            operation_owner = NULL;
            poison_operations();
        }
        if (operation_has_failed()) poison_operations();
        operation_owner = task;
        ++operation_depth;
        if (task) ++task->ime_storage_depth;
    }

    ~Guard()
    {
        if (!locked) return;
        if (task) --task->ime_storage_depth;
        if (operation_owner == task && operation_depth && !--operation_depth) operation_owner = NULL;
        mutex_unlock(&operation_lock);
    }
};

static int failure(int error)
{
    if (error < 0) permissions_set_file_error(-error);
    return error;
}

static bool read_device(void *context, uint64_t offset, void *buffer, xjext4::Size size)
{
    Mount *mount = (Mount *)context;
    if (operation_has_failed()) { mark_io_failed(mount); return false; }
    if (offset > mount->device->size || size > mount->device->size - offset) return false;
    if (mount->io_failed) return false;
    if (vfs_read(mount->device, buffer, offset, size) == size) return true;
    mark_io_failed(mount);
    return false;
}

static bool write_device(void *context, uint64_t offset, const void *buffer, xjext4::Size size)
{
    Mount *mount = (Mount *)context;
    if (operation_has_failed()) { mark_io_failed(mount); return false; }
    if (mount->io_failed || offset > mount->device->size || size > mount->device->size - offset) return false;
    if (vfs_write(mount->device, (void *)buffer, offset, size) == size) return true;
    mark_io_failed(mount);
    return false;
}

static bool flush_device(void *context)
{
    Mount *mount = (Mount *)context;
    if (operation_has_failed()) { mark_io_failed(mount); return false; }
    if (mount->io_failed) return false;
    if (vfs_ioctl(mount->device, IOBLKSYNC, NULL) == 0) return true;
    mark_io_failed(mount);
    return false;
}

static Node *attach(Mount *mount, const xjext4::Inode &inode)
{
    Node *handle = (Node *)malloc(sizeof(Node));
    if (!handle) return NULL;
    handle->mount = mount;
    handle->inode = inode;
    handle->next = mount->handles;
    handle->node = NULL;
    handle->deleted = false;
    handle->unlinked = false;
    mount->handles = handle;
    return handle;
}

static void release(Node *handle)
{
    Node **entry = &handle->mount->handles;
    while (*entry && *entry != handle) entry = &(*entry)->next;
    if (*entry) *entry = handle->next;
    free(handle);
}

static void destroy(Mount *mount)
{
    // An abandoned device request may still refer to the WS or its buffers.
    // Do not clean-close, free, or reuse any of these objects before reboot.
    if (!operation_has_failed() && !mount->io_failed && mount->writer_open)
    {
        if (mount->writer->close()) mark_io_failed(mount);
        else mount->writer_open = false;
    }
    if (operation_has_failed() || mount->io_failed)
    {
        mark_io_failed(mount);
        if (!mount->quarantined)
        {
            mount->quarantined = true;
            mount->quarantined_next = quarantined_mounts;
            quarantined_mounts = mount;
        }
        return;
    }
    // Legacy VFS keeps directory handles cached even while freeing a subtree.
    // Own every handle here so these allocations have one final cleanup path.
    while (mount->handles) release(mount->handles);
    vfs_close(mount->device);
    if (mount->lease) block_access_release(mount->lease);
    free(mount->storage.data);
    free(mount->writer);
    free(mount->workspace);
    free(mount);
}

static int metadata(Node *handle, vfs_node_t node)
{
    const xjext4::Inode &inode = handle->inode;
    if (handle->deleted) return failure(-ENOENT);
    if (operation_has_failed() || handle->mount->io_failed) return failure(-EIO);
    uint16_t type = inode.mode & 0170000;
    if (type != 0100000 && type != 0040000 && type != 0120000) return failure(-EOPNOTSUPP);
    handle->node = node;
    unsigned deleting = node->type & (file_delete | file_unlinked);
    node->type = (type == 0040000 ? file_dir : type == 0120000 ? file_symlink : file_none) | deleting;
    node->inode = inode.number;
    node->size = inode.size;
    node->realsize = inode.blocks_512 * 512;
    node->blksz = view(handle->mount).block_size();
    node->owner = inode.uid;
    node->group = inode.gid;
    node->permissions = node->mode = inode.mode & 07777;
    auto display_time = [](uint64_t seconds, uint32_t nanos) -> uint64_t
    {
        if (int64_t(seconds) < 0 || seconds > (uint64_t(-1) - nanos) / 1000000000ULL) return 0;
        return seconds * 1000000000ULL + nanos;
    };
    node->createtime = display_time(inode.ctime, inode.ctime_nsec);
    node->readtime = display_time(inode.atime, inode.atime_nsec);
    node->writetime = display_time(inode.mtime, inode.mtime_nsec);
    node->dev = handle->mount->device->rdev;
    ime_vfs_resolved_name(node, node->name);
    if (type == 0120000 && !node->linkname)
    {
        if (!inode.size || inode.size > 4095) return failure(-EIO);
        char *target = (char *)malloc(inode.size + 1);
        if (!target) return failure(-ENOMEM);
        xjext4::Size completed = 0;
        int error = view(handle->mount).read(inode, 0, target, inode.size, completed);
        if (error || completed != inode.size || memchr(target, 0, completed))
        {
            free(target);
            return failure(error ? error : -EIO);
        }
        target[completed] = 0;
        node->linkname = target;
    }
    return 0;
}

struct DirectoryContext
{
    Node *handle;
    vfs_node_t directory;
};

static int directory_entry(void *context, const char *name, uint32_t number, uint8_t)
{
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    DirectoryContext *directory = (DirectoryContext *)context;
    if (vfs_do_search(directory->directory, name)) return 0;
    xjext4::Inode inode;
    int error = view(directory->handle->mount).read_inode(number, inode);
    if (error) return error;
    // Devices, FIFOs, and sockets are not ordinary files in this adapter.
    uint16_t type = inode.mode & 0170000;
    if (type != 0100000 && type != 0040000 && type != 0120000) return 0;
    Node *handle = attach(directory->handle->mount, inode);
    if (!handle) return -ENOMEM;
    // Prepare each node fully before publishing it. A failed symlink read or
    // inode check must not leave a cached name that a retry mistakes for valid.
    vfs_node_t child = vfs_node_alloc(NULL, name);
    if (!child)
    {
        release(handle);
        return -ENOMEM;
    }
    child->parent = directory->directory;
    child->root = directory->directory->root;
    child->fsid = directory->handle->mount->fsid;
    child->handle = handle;
    error = metadata(handle, child);
    list_t entry = error ? NULL : list_alloc(child);
    if (error || !entry)
    {
        free(child->name);
        free(child->linkname);
        free(child);
        release(handle);
        return error ? error : -ENOMEM;
    }
    vfs_child_lock();
    entry->next = directory->directory->child;
    if (entry->next) entry->next->prev = entry;
    directory->directory->child = entry;
    vfs_child_unlock();
    return 0;
}

static int update(Node *handle, vfs_node_t node)
{
    if (operation_has_failed() || handle->mount->io_failed) return failure(-EIO);
    int error = handle->deleted ? -ENOENT : view(handle->mount).read_inode(handle->inode.number, handle->inode);
    if (!error) error = metadata(handle, node);
    if (!error && (node->type & file_dir) && !node->visited)
    {
        DirectoryContext context = {handle, node};
        error = view(handle->mount).list_directory(handle->inode, directory_entry, &context);
        if (!error) node->visited = true;
    }
    return failure(error);
}

struct RootValidation
{
    const XJ_ROOT_BINDING *binding;
    uint8_t buffer[4096];
};

static int validate_bound_root(void *context, const xjext4::Volume &volume)
{
    auto *check = static_cast<RootValidation *>(context);
    const XJ_ROOT_BINDING *binding = check->binding;
    if (memcmp(volume.uuid(), xj_root_binding_ext4_uuid(binding), 16)) return -EUCLEAN;
    xjext4::Inode root, system, kernel;
    int error = volume.read_inode(2, root);
    if (!error) error = volume.lookup(root, "system", system);
    if (!error && (system.mode & 0170000) != 0040000) error = -EUCLEAN;
    if (!error) error = volume.lookup(system, "kernel.krl", kernel);
    if (error) return error;
    if ((kernel.mode & 0170000) != 0100000 || kernel.size != xj_root_binding_kernel_bytes(binding) ||
        kernel.size > volume.blocks_count() * volume.block_size()) return -EUCLEAN;
    uint32_t checksum = 0xffffffffU;
    for (uint64_t offset = 0; offset < kernel.size;)
    {
        xjext4::Size bytes = kernel.size - offset < sizeof(check->buffer) ?
                            xjext4::Size(kernel.size - offset) : sizeof(check->buffer);
        xjext4::Size completed = 0;
        error = volume.read(kernel, offset, check->buffer, bytes, completed);
        if (error || completed != bytes) return error ? error : -EIO;
        checksum = xj_root_binding_crc32c_update(checksum, check->buffer, bytes);
        offset += bytes;
    }
    return (checksum ^ 0xffffffffU) == xj_root_binding_kernel_crc32c(binding) ? 0 : -EUCLEAN;
}

static vfs_node_t open_mount_device(const char *source)
{
    // Source lookup can traverse another filesystem. Do it without owning the
    // ext4 mutex, while preserving the authorized mount's scoped raw access.
    tcb_t task = get_current_task();
    if (task) ++task->ime_storage_depth;
    vfs_node_t device = vfs_open(source);
    if (task) --task->ime_storage_depth;
    return device;
}

static int mount_volume(const char *source, vfs_node_t node, bool writable,
                        const XJ_ROOT_BINDING *binding = nullptr)
{
    bool system_root = node == rootdir && binding && xj_root_binding_valid(binding) && writable;
    if (!source || is_virtual_fs(source) || !node || (node == rootdir && !system_root) || node->is_mount ||
        (node->child && !system_root))
        return -EINVAL;
    if (operation_has_failed() || __atomic_load_n(&operations_finished, __ATOMIC_ACQUIRE)) return failure(-EIO);
    vfs_node_t device = open_mount_device(source);
    if (!device || !(device->type & file_block))
    {
        if (device) vfs_close(device);
        return -ENODEV;
    }
    Guard guard;
    if (operation_has_failed() || operations_finished) { vfs_close(device); return failure(-EIO); }
    size_t slot = 16;
    for (size_t index = 0; index < 16; ++index)
    {
        if (mounts[index] && mounts[index]->device == device) { vfs_close(device); return -EBUSY; }
        if (!mounts[index] && slot == 16) slot = index;
    }
    if (slot == 16) { vfs_close(device); return -ENOSPC; }
    Mount *mount = (Mount *)calloc(1, sizeof(Mount));
    if (!mount) { vfs_close(device); return -ENOMEM; }
    mount->device = device;
    int claim_error = block_access_resolve_node(device, &mount->extent);
    if (!claim_error && writable) claim_error = block_access_require_flush(mount->extent);
    if (!claim_error) claim_error = block_access_claim(mount->extent, false, &mount->lease);
    if (claim_error) { vfs_close(device); free(mount); return failure(claim_error); }
    mount->writable = writable;
    mount->fsid = writable ? filesystem_id : readonly_id;
    int error;
    if (writable)
    {
        mount->writer = (xjext4::WriteSession *)calloc(1, sizeof(xjext4::WriteSession));
        mount->workspace = (xjext4::WriteWorkspace *)malloc(sizeof(xjext4::WriteWorkspace));
        if (!mount->writer || !mount->workspace) { destroy(mount); return -ENOMEM; }
        xjext4::WriteDevice io = {read_device, write_device, flush_device, mount, device->size};
        RootValidation validation = {binding, {}};
        error = xjext4::WriteSession::storage_size(io, mount->storage.bytes);
        if (!error)
        {
            mount->storage.data = malloc(mount->storage.bytes);
            if (!mount->storage.data) error = -ENOMEM;
        }
        if (!error) error = mount->writer->open(io, *mount->workspace,
                                     system_root ? validate_bound_root : nullptr, &validation, &mount->storage);
        mount->writer_open = error == 0;
    }
    else error = mount->volume.mount(read_device, mount, device->size);
    xjext4::Inode root;
    if (!error) error = view(mount).read_inode(2, root);
    if (error) { destroy(mount); return error; }
    Node *handle = attach(mount, root);
    if (!handle) { destroy(mount); return -ENOMEM; }
    struct vfs_node previous = *node;
    struct vfs_node prepared = previous;
    prepared.handle = handle;
    prepared.child = NULL;
    prepared.fsid = mount->fsid;
    prepared.root = node;
    prepared.visited = false;
    // Parse the entire root into a private tree. A failed read must never
    // publish handles that another lookup can retain while cleanup frees them.
    error = update(handle, &prepared);
    list_t covered = NULL;
    if (!error)
    {
        for (list_t entry = prepared.child; entry; entry = entry->next)
            ((vfs_node_t)entry->data)->parent = node;
        error = system_root ? vfs_publish_system_root(&prepared, &covered) :
                              vfs_publish_readonly_mount(node, &prepared);
    }
    if (error)
    {
        vfs_free_child(&prepared);
        destroy(mount);
        return error;
    }
    vfs_filesystem_t original = get_filesystem_node(&previous);
    if (previous.handle && original) original->callback->close(previous.handle);
    mounts[slot] = mount;
    handle->node = node;
    // The covered on-disk /dev is private and unopened. Its native handle
    // must leave the cache before freeing the vnode; /dev itself retains its
    // existing devfs identity and every block-device reference.
    while (covered)
    {
        list_t next = covered->next;
        auto *placeholder = static_cast<vfs_node_t>(covered->data);
        release(static_cast<Node *>(placeholder->handle));
        placeholder->handle = NULL;
        vfs_free(placeholder);
        free(covered);
        covered = next;
    }
    write_serial_fmt("EXT4: native Apache-2.0 %s %s mounted\n", writable ? "journaled writable" : "read-only",
                     system_root ? "system root" : "data volume");
    return 0;
}

static int mount_writable(const char *source, vfs_node_t node)
{
    if (!source || is_virtual_fs(source) || source[0] != '/') return -EINVAL;
    return mount_volume(source, node, true);
}

static int mount_readonly(const char *source, vfs_node_t node)
{
    if (!source || is_virtual_fs(source) || strncmp(source, "ext4ro:", 7)) return -EINVAL;
    return mount_volume(source + 7, node, false);
}

static int prepare_unmount(void *context)
{
    Guard guard;
    Mount *mount = ((Node *)context)->mount;
    if (operation_has_failed()) { mount->io_failed = true; return failure(-EIO); }
    if (!mount->writable) return 0;
    if (operation_has_failed() || mount->io_failed) return failure(-EIO);
    vfs_child_lock();
    bool busy = false;
    for (Node *entry = mount->handles; entry; entry = entry->next)
        if (entry->unlinked && entry->node && entry->node->refcount) { busy = true; break; }
    vfs_child_unlock();
    if (busy) return failure(-EBUSY);
    int error = mount->writer->close();
    if (error)
    {
        if (error != -EBUSY) mark_io_failed(mount);
    }
    else mount->writer_open = false;
    return failure(error);
}

static int unmount_checked(void *context)
{
    Guard guard;
    Node *handle = (Node *)context;
    Mount *mount = handle->mount;
    if (operation_has_failed() || mount->io_failed)
    {
        mark_io_failed(mount);
        return failure(-EIO);
    }
    if (mount->writer_open)
    {
        int error = mount->writer->close();
        if (error)
        {
            if (error != -EBUSY) mark_io_failed(mount);
            return failure(error);
        }
        mount->writer_open = false;
    }
    for (size_t i = 0; i < 16; ++i) if (mounts[i] == mount) mounts[i] = NULL;
    destroy(mount);
    return 0;
}

static void unmount_volume(void *context)
{
    unmount_checked(context);
}

static void open_node(void *, const char *, vfs_node_t node)
{
    Guard guard;
    if (node->handle) update((Node *)node->handle, node);
}

static int finalize_node(void *context, vfs_node_t)
{
    Guard guard;
    if (!context) return 0;
    Node *handle = (Node *)context;
    Mount *mount = handle->mount;
    if (operation_has_failed()) { mount->io_failed = true; return failure(-EIO); }
    uint32_t number = handle->inode.number;
    bool unlinked = handle->unlinked;
    release(handle);
    if (!unlinked || !mount->writable || !mount->writer_open || inode_is_open(mount, number)) return 0;
    int error = mount->io_failed ? -EIO : mount->writer->reclaim_orphan(number);
    if (!error) error = refresh(mount, number);
    return mutation_result(mount, error);
}

static void close_node(void *context) { (void)finalize_node(context, NULL); }

static int stat_node(void *context, vfs_node_t node)
{
    Guard guard;
    return context && node ? update((Node *)context, node) : -EINVAL;
}

static size_t read_node(void *context, void *buffer, size_t offset, size_t length)
{
    Guard guard;
    Node *handle = (Node *)context;
    if (!handle || !buffer) { failure(-EINVAL); return (size_t)-1; }
    if (operation_has_failed() || handle->mount->io_failed || handle->deleted)
    {
        failure(operation_has_failed() || handle->mount->io_failed ? -EIO : -ENOENT);
        return (size_t)-1;
    }
    xjext4::Size completed = 0;
    int error = view(handle->mount).read(handle->inode, offset, buffer, length, completed);
    if (error) { failure(error); return (size_t)-1; }
    return completed;
}

static size_t read_link(vfs_node_t node, void *buffer, size_t offset, size_t length)
{
    return read_node(node->handle, buffer, offset, length);
}

static int mutable_mount(Mount *mount)
{
    if (operation_has_failed()) { mark_io_failed(mount); return -EIO; }
    if (!mount->writable) return -EROFS;
    if (mount->io_failed || mount->writer->failed()) { mark_io_failed(mount); return -EIO; }
    return 0;
}

static int refresh(Mount *mount, uint32_t first, uint32_t second, uint32_t third, uint32_t fourth)
{
    if (!first) return -EINVAL;
    const uint32_t changed[] = {first, second, third, fourth};
    for (unsigned index = 0; index < 4; ++index)
    {
        uint32_t number = changed[index];
        bool duplicate = !number;
        for (unsigned previous = 0; previous < index; ++previous)
            if (changed[previous] == number) duplicate = true;
        if (duplicate) continue;
        bool cached = false;
        for (Node *entry = mount->handles; entry; entry = entry->next)
            if (!entry->deleted && entry->inode.number == number) { cached = true; break; }
        if (!cached) continue;

        // Read a changed inode once, then update every cached hard-link alias.
        // Avoid rereading unrelated inode tables/bitmaps on each small write.
        xjext4::Inode fresh;
        int error = view(mount).read_inode(number, fresh);
        for (Node *entry = mount->handles; entry; entry = entry->next)
        {
            if (entry->deleted || entry->inode.number != number) continue;
            if (error == -ENOENT && entry->unlinked) { entry->deleted = true; continue; }
            if (error) { mount->io_failed = true; return error; }
            entry->inode = fresh;
            if (entry->node)
            {
                int result = metadata(entry, entry->node);
                if (result) { mount->io_failed = true; return result; }
            }
        }
    }
    return 0;
}

static int mutation_result(Mount *mount, int error)
{
    if (operation_has_failed() || (mount->writer && mount->writer->failed())) mark_io_failed(mount);
    return failure(mount->io_failed ? -EIO : error);
}

static size_t write_node(void *context, const void *data, size_t offset, size_t length)
{
    Guard guard;
    Node *handle = (Node *)context;
    if (!handle || handle->deleted) { failure(-ENOENT); return (size_t)-1; }
    Mount *mount = handle->mount;
    int error = mutable_mount(mount);
    xjext4::Size completed = 0;
    if (!error) error = mount->writer->pwrite(handle->inode.number, offset, data, length, seconds_now(), completed);
    if (mount->writer && !mount->writer->failed() && (!error || completed))
    {
        int refresh_error = refresh(mount, handle->inode.number);
        if (!error) error = refresh_error;
    }
    error = mutation_result(mount, error);
    return completed ? completed : error ? (size_t)-1 : 0;
}

static int make_node(void *context, const char *name, vfs_node_t node, bool directory)
{
    Guard guard;
    Node *parent = (Node *)context;
    if (!parent || !node) return failure(-EINVAL);
    Mount *mount = parent->mount;
    int error = mutable_mount(mount);
    if (error) return failure(error);
    // Reserve the handle before committing the directory entry. VFS has also
    // reserved the unpublished cache node/list entry before calling us.
    Node *handle = (Node *)calloc(1, sizeof(Node));
    if (!handle) return failure(-ENOMEM);
    tcb_t task = get_current_task();
    pcb_t process = task ? task->parent_group : NULL;
    uint16_t kind = directory ? 0040000 : 0100000;
    bool supplied = (node->mode & 0170000) == kind;
    uint16_t mode = supplied ? node->mode : kind | ((directory ? 0777 : 0666) & ~(process ? process->umask : 0022));
    uint32_t uid = supplied ? node->owner : user_uid(task_effective_user());
    uint32_t gid = supplied ? node->group : user_gid(task_effective_user());
    error = mount->writer->create(parent->inode.number, name, mode, uid, gid, seconds_now(), handle->inode);
    if (!error)
    {
        handle->mount = mount;
        handle->next = mount->handles;
        mount->handles = handle;
        node->handle = handle;
        node->fsid = mount->fsid;
        node->root = parent->node->root;
        error = refresh(mount, handle->inode.number, parent->inode.number);
        if (!error) error = metadata(handle, node);
    }
    else free(handle);
    return mutation_result(mount, error);
}

static int create_file(void *parent, const char *name, vfs_node_t node) { return make_node(parent, name, node, false); }
static int create_directory(void *parent, const char *name, vfs_node_t node) { return make_node(parent, name, node, true); }

static int create_link_node(void *context, vfs_node_t source, vfs_node_t node)
{
    Guard guard;
    Node *parent = (Node *)context;
    Node *target = ext4fs_owns_node(source) ? (Node *)source->handle : NULL;
    Mount *mount = parent->mount;
    int error = target && target->mount == mount ? mutable_mount(mount) : -EXDEV;
    if (!error && target->deleted) error = -ENOENT;
    Node *handle = !error ? (Node *)calloc(1, sizeof(Node)) : NULL;
    if (!error && !handle) error = -ENOMEM;
    if (!error && (source->type & file_symlink) && !node->linkname)
    {
        node->linkname = strdup(source->linkname);
        if (!node->linkname) error = -ENOMEM;
    }
    if (!error) error = mount->writer->link(parent->inode.number, node->name, target->inode.number, seconds_now());
    if (!error)
    {
        handle->mount = mount;
        handle->inode = target->inode;
        handle->next = mount->handles;
        mount->handles = handle;
        node->handle = handle;
        node->fsid = mount->fsid;
        node->root = parent->node->root;
        error = refresh(mount, target->inode.number, parent->inode.number);
        if (!error) error = metadata(handle, node);
    }
    else free(handle);
    return mutation_result(mount, error);
}

static int create_link(void *, const char *, vfs_node_t) { return failure(-EOPNOTSUPP); }

static int create_symlink(void *context, const char *target, vfs_node_t node)
{
    Guard guard;
    Node *parent = (Node *)context;
    Mount *mount = parent->mount;
    int error = mutable_mount(mount);
    Node *handle = !error ? (Node *)calloc(1, sizeof(Node)) : NULL;
    if (!error && !handle) error = -ENOMEM;
    if (!error && !node->linkname)
    {
        node->linkname = strdup(target);
        if (!node->linkname) error = -ENOMEM;
    }
    if (!error) error = mount->writer->symlink(parent->inode.number, node->name, target,
                                               user_uid(task_effective_user()), user_gid(task_effective_user()),
                                               seconds_now(), handle->inode);
    if (!error)
    {
        handle->mount = mount;
        handle->next = mount->handles;
        mount->handles = handle;
        node->handle = handle;
        node->fsid = mount->fsid;
        node->root = parent->node->root;
        error = refresh(mount, handle->inode.number, parent->inode.number);
        if (!error) error = metadata(handle, node);
    }
    else free(handle);
    return mutation_result(mount, error);
}

static int delete_node(void *context, vfs_node_t node)
{
    Guard guard;
    if (!node || node->is_mount || node == node->root) return failure(-EBUSY);
    if (!node->parent || !ext4fs_owns_node(node->parent)) return failure(-EXDEV);
    Node *parent = (Node *)context;
    Node *handle = node ? (Node *)node->handle : NULL;
    if (!parent || !handle || parent->mount != handle->mount) return failure(-EINVAL);
    if (!node->parent || node->parent->handle != context || node->inode != handle->inode.number)
        return failure(-EBUSY);
    Mount *mount = handle->mount;
    int error = mutable_mount(mount);
    if (error) return failure(error);
    bool defer = inode_is_open(mount, handle->inode.number, node, 1);
    error = mount->writer->unlink(parent->inode.number, node->name, seconds_now(), defer);
    if (!error)
    {
        handle->unlinked = true;
        error = refresh(mount, handle->inode.number, parent->inode.number);
    }
    return mutation_result(mount, error);
}

static int rename_node(void *context, const char *destination)
{
    Node *handle = (Node *)context;
    if (!handle || !handle->node || !destination || destination[0] != '/') return failure(-EINVAL);
    if (handle->node->is_mount || handle->node == handle->node->root) return failure(-EBUSY);
    char *path = strdup(destination);
    if (!path) return failure(-ENOMEM);
    char *name = strrchr(path, '/');
    char *copy = name && name[1] ? strdup(name + 1) : NULL;
    if (!copy)
    {
        int error = name && !name[1] ? -EINVAL : -ENOMEM;
        free(path);
        return failure(error);
    }
    if (name == path) path[1] = 0;
    else *name = 0;
    vfs_node_t parent = vfs_open(path);
    free(path);
    if (!parent) { free(copy); return failure(-ENOENT); }
    if (!vfs_check_child_creation(parent, copy))
    {
        int error = vfs_last_error();
        vfs_close(parent); free(copy); return failure(-error);
    }
    Guard guard;
    Mount *mount = handle->mount;
    vfs_node_t node = handle->node;
    Node *new_parent = ext4fs_owns_node(parent) ? (Node *)parent->handle : NULL;
    int error = new_parent && new_parent->mount == mount ? mutable_mount(mount) : -EXDEV;
    vfs_node_t target = NULL;
    list_t moving = NULL, replaced = NULL;
    bool reserved_target = false, reserved_source = false, target_parent_pin = false;
    // Reserve the destination name against lookups and mutations. Existing
    // callers retained before this point are checked before overwriting.
    vfs_child_lock();
    if (!error)
    {
        if ((node->type & file_delete) || !node->parent) error = -EBUSY;
        else if (!ext4fs_owns_node(node->parent) || !node->parent->handle ||
                 ((Node *)node->parent->handle)->mount != mount) error = -EXDEV;
        for (list_t entry = parent->child; entry; entry = entry->next)
            if (!strcmp(((vfs_node_t)entry->data)->name, copy)) { target = (vfs_node_t)entry->data; replaced = entry; break; }
        if (!error && (target == node || (target && target->inode == node->inode))) error = 1;
        else if (!error && target && ((target->type & file_delete) || target->is_mount)) error = -EBUSY;
        if (node->parent)
            for (list_t entry = node->parent->child; entry; entry = entry->next)
                if (entry->data == node) { moving = entry; break; }
        if (!error && !moving) error = -ENOENT;
        if (!error && target && parent != rootdir)
        {
            if (parent->refcount == uint32_t(-1)) error = -EOVERFLOW;
            else { ++parent->refcount; target_parent_pin = true; }
        }
        if (!error && target) { target->type |= file_delete; reserved_target = true; }
        if (!error) { node->type |= file_delete; reserved_source = true; }
    }
    vfs_child_unlock();
    bool defer_target = !error && target && inode_is_open(mount, target->inode, target);
    uint32_t old_parent_number = !error ? ((Node *)node->parent->handle)->inode.number : 0;
    uint32_t target_number = target ? uint32_t(target->inode) : 0;
    if (!error)
        error = mount->writer->rename(((Node *)node->parent->handle)->inode.number, node->name,
                                      new_parent->inode.number, copy, seconds_now(), defer_target);
    char *old_name = NULL;
    if (!error)
    {
        vfs_child_lock();
        vfs_node_t old_parent = node->parent;
        if (moving->prev) moving->prev->next = moving->next;
        else old_parent->child = moving->next;
        if (moving->next) moving->next->prev = moving->prev;
        if (replaced)
        {
            if (replaced->prev) replaced->prev->next = replaced->next;
            else parent->child = replaced->next;
            if (replaced->next) replaced->next->prev = replaced->prev;
            target->type |= file_unlinked;
            ((Node *)target->handle)->unlinked = true;
            target_parent_pin = false; // The detached vnode now owns this parent pin.
        }
        moving->prev = NULL;
        moving->next = parent->child;
        if (moving->next) moving->next->prev = moving;
        parent->child = moving;
        node->parent = parent;
        old_name = node->name;
        node->name = copy;
        node->type &= ~file_delete;
        reserved_source = false;
        copy = NULL;
        vfs_child_unlock();
        if (target) { free(replaced); error = vfs_close(target); }
        free(old_name);
        if (!error) error = refresh(mount, handle->inode.number, old_parent_number,
                                    new_parent->inode.number, target_number);
    }
    else if (reserved_target)
    {
        vfs_child_lock(); target->type &= ~file_delete; vfs_child_unlock();
    }
    if (reserved_source)
    {
        vfs_child_lock(); node->type &= ~file_delete; vfs_child_unlock();
    }
    if (target_parent_pin) vfs_close(parent);
    vfs_close(parent);
    free(copy);
    return error == 1 ? 0 : mutation_result(mount, error);
}

static void resize_node(void *context, uint64_t size)
{
    Guard guard;
    Node *handle = (Node *)context;
    if (!handle) { failure(-EINVAL); return; }
    Mount *mount = handle->mount;
    int error = mutable_mount(mount);
    if (!error) error = mount->writer->truncate(handle->inode.number, size, seconds_now());
    if (!error) error = refresh(mount, handle->inode.number);
    mutation_result(mount, error);
}

static int ioctl_node(void *context, size_t request, void *)
{
    if (request != IOBLKSYNC) return failure(-EOPNOTSUPP);
    Guard guard;
    Mount *mount = ((Node *)context)->mount;
    if (!mount->writable) return 0;
    int error = mutable_mount(mount);
    if (!error) error = mount->writer->sync();
    return mutation_result(mount, error);
}

static bool privileged_attributes()
{
    tcb_t task = get_current_task();
    return !task || task->task_level != TASK_APPLICATION_LEVEL || user_uid(task_effective_user()) == 0;
}

static int attribute_access(vfs_node_t node, const char *name, bool writing)
{
    if (!name || !*name) return -EINVAL;
    // ACL handlers must validate their format and keep mode/mask semantics in
    // sync. Until those handlers exist, retain imported attributes for reads
    // but reject VFS mutation rather than promising unenforced permissions.
    if (writing && !strncmp(name, "system.", 7)) return -EOPNOTSUPP;
    if (writing && strncmp(name, "user.", 5) && strncmp(name, "trusted.", 8) &&
        strncmp(name, "security.", 9)) return -EOPNOTSUPP;
    bool privileged = privileged_attributes();
    if (!strncmp(name, "trusted.", 8) && !privileged) return -EPERM;
    if (writing && (strncmp(name, "user.", 5) != 0) && !privileged) return -EPERM;
    if (writing && !strncmp(name, "user.", 5) && (node->type & file_symlink)) return -EPERM;
    return 0;
}

static int get_attribute(vfs_node_t node, const char *name, void *value, size_t capacity, size_t *required)
{
    int error = attribute_access(node, name, false);
    if (error) return failure(error);
    Guard guard;
    Node *handle = (Node *)node->handle;
    if (!handle || handle->deleted) return failure(-ENOENT);
    if (operation_has_failed() || handle->mount->io_failed) return failure(-EIO);
    xjext4::Size size = 0;
    error = view(handle->mount).get_xattr(handle->inode.number, name, value, capacity, size);
    *required = size;
    return failure(error);
}

static int list_attributes(vfs_node_t node, char *names, size_t capacity, size_t *required)
{
    bool privileged = privileged_attributes();
    Guard guard;
    Node *handle = (Node *)node->handle;
    if (!handle || handle->deleted) return failure(-ENOENT);
    Mount *mount = handle->mount;
    if (operation_has_failed() || mount->io_failed) return failure(-EIO);
    xjext4::Size total = 0;
    if (privileged)
    {
        int error = view(mount).list_xattr(handle->inode.number, names, capacity, total);
        *required = total;
        return failure(error);
    }
    int error = view(mount).list_xattr(handle->inode.number, NULL, 0, total);
    if (error) return failure(error);
    char *all = total ? (char *)malloc(total) : NULL;
    if (total && !all) return failure(-ENOMEM);
    if (total) error = view(mount).list_xattr(handle->inode.number, all, total, total);
    size_t visible = 0;
    for (size_t position = 0; !error && position < total;)
    {
        char *end = (char *)memchr(all + position, 0, total - position);
        if (!end) { error = -EUCLEAN; break; }
        size_t bytes = size_t(end - all - position) + 1;
        if (strncmp(all + position, "trusted.", 8))
        {
            if (names && visible <= capacity && bytes <= capacity - visible)
                memcpy(names + visible, all + position, bytes);
            visible += bytes;
        }
        position += bytes;
    }
    free(all);
    *required = visible;
    if (!error && capacity && visible > capacity) error = -ERANGE;
    return failure(error);
}

static int set_attribute(vfs_node_t node, const char *name, const void *value, size_t length, unsigned flags)
{
    int error = attribute_access(node, name, true);
    if (error) return failure(error);
    Guard guard;
    Node *handle = (Node *)node->handle;
    if (!handle || handle->deleted) return failure(-ENOENT);
    Mount *mount = handle->mount;
    error = mutable_mount(mount);
    if (!error) error = mount->writer->set_xattr(handle->inode.number, name, value, length, flags, seconds_now());
    if (!error) error = refresh(mount, handle->inode.number);
    return mutation_result(mount, error);
}

static int remove_attribute(vfs_node_t node, const char *name)
{
    int error = attribute_access(node, name, true);
    if (error) return failure(error);
    Guard guard;
    Node *handle = (Node *)node->handle;
    if (!handle || handle->deleted) return failure(-ENOENT);
    Mount *mount = handle->mount;
    error = mutable_mount(mount);
    if (!error) error = mount->writer->remove_xattr(handle->inode.number, name, seconds_now());
    if (!error) error = refresh(mount, handle->inode.number);
    return mutation_result(mount, error);
}

static const vfs_xattr_ops attributes = {get_attribute, list_attributes, set_attribute, remove_attribute};
static int poll_node(void *, size_t) { return -EOPNOTSUPP; }
static vfs_node_t duplicate_node(vfs_node_t) { failure(-EOPNOTSUPP); return NULL; }
static void *map_node(void *context, void *address, size_t offset, size_t length, size_t prot, size_t flags)
{
    // general_map supplies private snapshots and has no dirty-page writeback.
    // Shared mappings also require coherence and later mprotect/writeback.
    if ((flags & 3U) != MAP_PRIVATE)
    {
        failure(-EOPNOTSUPP);
        return (void *)-EOPNOTSUPP;
    }
    return general_map(read_node, context, (uint64_t)address, length, prot, flags, offset);
}

static bool format_fail_io(FormatContext *context)
{
    context->io_failed = true;
    block_access_poison(context->lease);
    return false;
}

static int format_window_access(FormatContext *context)
{
    int error = block_access_validate_owned_exclusive(context->lease, context->extent,
                                                       context->io_offset, context->io_bytes);
    if (error) return error;
    // A live whole-device lease prevents device-table deletion/reuse while we
    // read its real geometry. Byte alignment alone cannot certify 512-byte
    // sectors: a larger logical sector could expand an RMW beyond this window.
    device_t *device = get_device(context->extent.physical_device);
    if (!device || device->type != DEVICE_BLOCK || device->vdiskid != context->extent.device ||
        device->size != context->extent.bytes) return -ENODEV;
    return device->sector_size == 512 ? 0 : -EOPNOTSUPP;
}

static bool format_ready(FormatContext *context)
{
    if (context->io_failed || operation_has_failed()) return format_fail_io(context);
    if (context->io_offset > context->extent.bytes ||
        context->io_bytes > context->extent.bytes - context->io_offset) return format_fail_io(context);
    if (!context->owns_lease)
    {
        // The window is a synchronous borrow, not a fake registered partition.
        // Match the real captured devfs identity independently of registry
        // authority, then authenticate the parent before every backend request.
        BlockExtent actual;
        if (block_access_resolve_node(context->device, &actual) ||
            actual.device != context->extent.device || actual.physical_device != context->extent.physical_device ||
            actual.generation != context->extent.generation ||
            actual.physical_generation != context->extent.physical_generation ||
            actual.start != context->extent.start || actual.bytes != context->extent.bytes ||
            context->device->size != actual.bytes ||
            format_window_access(context))
            return format_fail_io(context);
    }
    return true;
}

static bool format_read(void *opaque, uint64_t offset, void *buffer, xjext4::Size size)
{
    auto *context = static_cast<FormatContext *>(opaque);
    if (!format_ready(context)) return false;
    if (offset > context->io_bytes || size > context->io_bytes - offset) return format_fail_io(context);
    if (vfs_read(context->device, buffer, context->io_offset + offset, size) != size) return format_fail_io(context);
    return true;
}

static bool format_write(void *opaque, uint64_t offset, const void *buffer, xjext4::Size size)
{
    auto *context = static_cast<FormatContext *>(opaque);
    if (!format_ready(context)) return false;
    if (offset > context->io_bytes || size > context->io_bytes - offset) return format_fail_io(context);
    if (vfs_write(context->device, const_cast<void *>(buffer), context->io_offset + offset, size) != size)
        return format_fail_io(context);
    return true;
}

static bool format_flush(void *opaque)
{
    auto *context = static_cast<FormatContext *>(opaque);
    if (!format_ready(context)) return false;
    if (vfs_ioctl(context->device, IOBLKSYNC, NULL)) return format_fail_io(context);
    return true;
}

static struct vfs_callback callbacks = {
    mount_writable, unmount_volume, open_node, close_node, read_node, write_node, read_link,
    create_directory, create_file, create_link, create_symlink, stat_node, ioctl_node,
    duplicate_node, poll_node, map_node, resize_node, delete_node, rename_node,
};

} // namespace

int ext4fs_factory_ready()
{
    // Advisory preflight only: no guard, allocation, I/O, storage privilege or
    // reservation. Later acquisition must recheck the normal operation gates.
    if (!__atomic_load_n(&operation_initialized, __ATOMIC_ACQUIRE)) return -ENODEV;
    if (operation_has_failed() || __atomic_load_n(&operations_finished, __ATOMIC_ACQUIRE)) return -EIO;
    tcb_t task = get_current_task();
    if (task && mutex_get_owner(&operation_lock) == task) return -EBUSY;
    return 0;
}

int ext4fs_format_node(vfs_node_t device, const ext4_format_options *options, ext4_format_status *status)
{
    // These are trusted kernel pointers. Snapshot options before clearing a
    // potentially overlapping output structure; never accept user pointers.
    ext4_format_options snapshot = {};
    if (options) snapshot = *options;
    if (status) *status = {};
    if (!device || !options || !(device->type & file_block)) return failure(-EINVAL);
    if (!get_current_task() || !scheduler_is_enabled() || !(get_rflags() & (1ULL << 9)))
        return failure(-EWOULDBLOCK);
    if (!__atomic_load_n(&operation_initialized, __ATOMIC_ACQUIRE)) return failure(-ENODEV);
    if (!vfs_check_access(device, NULL, IME_FILE_MOUNT) || !vfs_check_access(device, NULL, IME_FILE_WRITE))
    {
        int error = vfs_last_error();
        return failure(error ? -error : -EACCES);
    }
    Guard guard;
    if (operation_has_failed() || operations_finished) return failure(-EIO);
    auto *context = static_cast<FormatContext *>(calloc(1, sizeof(FormatContext)));
    if (!context) return failure(-ENOMEM);
    context->workspace = static_cast<xjext4::FormatWorkspace *>(malloc(sizeof(xjext4::FormatWorkspace)));
    if (!context->workspace) { free(context); return failure(-ENOMEM); }
    int error = xjext4::format_storage_size(device->size, context->storage.bytes);
    if (!error && context->storage.bytes)
    {
        context->storage.data = malloc(context->storage.bytes);
        if (!context->storage.data) error = -ENOMEM;
    }
    if (!error) error = block_access_resolve_node(device, &context->extent);
    if (!error && (context->extent.bytes != device->size || !context->extent.bytes)) error = -EINVAL;
    if (!error) error = block_access_require_flush(context->extent);
    if (!error) error = block_access_claim(context->extent, true, &context->lease);
    if (error)
    {
        free(context->storage.data);
        free(context->workspace);
        free(context);
        return failure(error);
    }
    context->owns_lease = true;
    context->io_bytes = context->extent.bytes;
    context->device = device;
    vfs_child_lock();
    bool overflow = device->refcount == uint32_t(-1);
    if (!overflow) ++device->refcount;
    vfs_child_unlock();
    if (overflow)
    {
        block_access_release(context->lease);
        free(context->storage.data);
        free(context->workspace);
        free(context);
        return failure(-EOVERFLOW);
    }
    // Publish every lifetime pin before the first possible device request.
    // Guard abandonment leaves this list and the heap workspace quarantined.
    context->next = format_contexts;
    format_contexts = context;
    xjext4::FormatOptions native_options = {};
    memcpy(native_options.uuid, snapshot.uuid, sizeof(native_options.uuid));
    native_options.time_seconds = snapshot.time_seconds;
    native_options.journal_mib = snapshot.journal_mib;
    memcpy(native_options.label, snapshot.label, sizeof(native_options.label));
    xjext4::Ext4FormatDevice io = {context, format_read, format_write, format_flush, context->extent.bytes};
    error = xjext4::format(io, native_options, *context->workspace, context->geometry, &context->storage);
    if (operation_has_failed()) format_fail_io(context);
    if (status)
    {
        status->bytes = context->extent.bytes;
        status->blocks = context->geometry.blocks;
        status->groups = context->geometry.groups;
        status->inodes = context->geometry.inodes;
        status->free_blocks = context->geometry.free_blocks;
        status->metadata_durable = context->geometry.metadata_durable;
        status->publication_started = context->geometry.publication_started;
        status->primary_attempted = context->geometry.primary_attempted;
        status->io_failed = context->io_failed;
    }
    // An ambiguous failure can retain device DMA references. Do no cleanup I/O,
    // release, free or close on that lifetime, including a failed first flush.
    if (context->io_failed) return failure(-EIO);
    FormatContext **previous = &format_contexts;
    while (*previous && *previous != context) previous = &(*previous)->next;
    if (*previous) *previous = context->next;
    vfs_close(context->device);
    block_access_release(context->lease);
    free(context->storage.data);
    free(context->workspace);
    free(context);
    return failure(error);
}

namespace {

// Keep the FormatContext as the first member: its existing quarantine list
// retains the complete factory-build lifetime on failure or guard abandonment.
struct BuildContext
{
    FormatContext format;
    xjinstaller::Ext4ImportWorkspace import;
    xjext4::WriteStorage storage;
    xjinstaller::Ext4ImportStatus import_status;
};

} // namespace

static int build_offline(vfs_node_t device, const BlockAccessWindow *window, const ext4_format_options *options,
                          const void *package, size_t package_bytes, ext4_build_status *status)
{
    ext4_format_options snapshot = {};
    if (options) snapshot = *options;
    // Do not clear a caller's output through the immutable package. All pointers
    // are trusted kernel spans; this API never validates user mappings.
    uintptr_t first = reinterpret_cast<uintptr_t>(package);
    uintptr_t output = reinterpret_cast<uintptr_t>(status);
    if (package_bytes > uintptr_t(-1) - first) return failure(-EINVAL);
    if (status && (output > uintptr_t(-1) - sizeof(*status) ||
                   (output < first + package_bytes && first < output + sizeof(*status))))
        return failure(-EINVAL);
    if (status) *status = {};
    if (!device || !options || !package || !(device->type & file_block)) return failure(-EINVAL);
    if (!get_current_task() || !scheduler_is_enabled() || !(get_rflags() & (1ULL << 9)))
        return failure(-EWOULDBLOCK);
    if (!__atomic_load_n(&operation_initialized, __ATOMIC_ACQUIRE)) return failure(-ENODEV);
    if (!vfs_check_access(device, NULL, IME_FILE_MOUNT) || !vfs_check_access(device, NULL, IME_FILE_WRITE))
    {
        int error = vfs_last_error();
        return failure(error ? -error : -EACCES);
    }
    xjpak2::Limits limits;
    xjpak2::HeaderInfo header;
    int error = xjpak2::inspect_header(static_cast<const uint8_t *>(package), package_bytes, limits, header);
    if (error) return failure(error);
    if (header.legacy_v1) return failure(-EOPNOTSUPP);

    Guard guard;
    if (operation_has_failed() || operations_finished) return failure(-EIO);
    auto *context = static_cast<BuildContext *>(calloc(1, sizeof(BuildContext)));
    if (!context) return failure(-ENOMEM);
    context->format.owns_lease = !window;
    // This closure is used only before I/O or after a completely closed writer.
    // A failed/abandoned request keeps the whole published BuildContext instead.
    auto release = [&]()
    {
        FormatContext **previous = &format_contexts;
        while (*previous && *previous != &context->format) previous = &(*previous)->next;
        if (*previous) *previous = context->format.next;
        if (context->format.device) vfs_close(context->format.device);
        if (context->format.owns_lease && context->format.lease) block_access_release(context->format.lease);
        free(context->storage.data);
        free(context->format.storage.data);
        free(context->import.read_volume);
        free(context->import.writer_workspace);
        free(context->import.writer);
        free(context->import.entries);
        free(context->import.records);
        free(context->format.workspace);
        free(context);
    };
    context->format.workspace = static_cast<xjext4::FormatWorkspace *>(malloc(sizeof(xjext4::FormatWorkspace)));
    context->import.capacity = header.record_count;
    context->import.records = static_cast<xjpak2::Record *>(malloc(header.record_count * sizeof(xjpak2::Record)));
    context->import.entries = static_cast<xjinstaller::Ext4ImportEntry *>(
        malloc(header.record_count * sizeof(xjinstaller::Ext4ImportEntry)));
    context->import.writer = static_cast<xjext4::WriteSession *>(calloc(1, sizeof(xjext4::WriteSession)));
    context->import.writer_workspace = static_cast<xjext4::WriteWorkspace *>(malloc(sizeof(xjext4::WriteWorkspace)));
    context->import.read_volume = static_cast<xjext4::Volume *>(calloc(1, sizeof(xjext4::Volume)));
    if (!context->format.workspace || !context->import.records || !context->import.entries ||
        !context->import.writer || !context->import.writer_workspace || !context->import.read_volume)
    {
        release();
        return failure(-ENOMEM);
    }
    xjinstaller::Ext4ImportRequirements requirements;
    error = xjinstaller::ext4_import_prepare(static_cast<const uint8_t *>(package), package_bytes, limits,
                                            context->import, requirements);
    if (!error) error = block_access_resolve_node(device, &context->format.extent);
    if (!error && (context->format.extent.bytes != device->size || !context->format.extent.bytes)) error = -EINVAL;
    if (!error) error = block_access_require_flush(context->format.extent);
    if (!error && window)
    {
        const BlockExtent &actual = context->format.extent;
        const BlockExtent &whole = window->whole;
        if (actual.device != whole.device || actual.physical_device != whole.physical_device ||
            actual.generation != whole.generation || actual.physical_generation != whole.physical_generation ||
            actual.start != whole.start || actual.bytes != whole.bytes) error = -ENODEV;
        if (!error)
        {
            context->format.lease = window->lease;
            context->format.io_offset = window->offset;
            context->format.io_bytes = window->bytes;
            error = format_window_access(&context->format);
        }
    }
    if (!error && !window)
    {
        error = block_access_claim(context->format.extent, true, &context->format.lease);
        context->format.io_bytes = context->format.extent.bytes;
    }
    xjext4::FormatOptions native_options = {};
    memcpy(native_options.uuid, snapshot.uuid, sizeof(native_options.uuid));
    native_options.time_seconds = snapshot.time_seconds;
    native_options.journal_mib = snapshot.journal_mib;
    memcpy(native_options.label, snapshot.label, sizeof(native_options.label));
    xjext4::Ext4FormatDevice io = {&context->format, format_read, format_write, format_flush,
                                  context->format.io_bytes};
    if (!error) error = xjext4::format_storage_size(io.bytes, context->format.storage.bytes);
    if (!error && context->format.storage.bytes)
    {
        context->format.storage.data = malloc(context->format.storage.bytes);
        if (!context->format.storage.data) error = -ENOMEM;
    }
    if (!error) error = xjext4::plan_format(io, native_options, *context->format.workspace,
                                          context->format.geometry, &context->format.storage);
    if (!error) error = xjext4::WriteSession::storage_size_for(context->format.geometry.blocks,
                          context->format.geometry.inodes, context->format.geometry.groups, context->storage.bytes);
    if (!error)
    {
        context->storage.data = malloc(context->storage.bytes);
        if (!context->storage.data) error = -ENOMEM;
    }
    if (!error) error = xjinstaller::ext4_import_check_budget(requirements, 4096,
                         context->format.geometry.free_blocks, context->format.geometry.inodes - 11);
    if (status)
    {
        status->required_blocks = requirements.blocks;
        status->required_inodes = requirements.new_inodes;
    }
    if (error)
    {
        release();
        return failure(error);
    }
    vfs_child_lock();
    bool overflow = device->refcount == uint32_t(-1);
    if (!overflow) ++device->refcount;
    vfs_child_unlock();
    if (overflow)
    {
        release();
        return failure(-EOVERFLOW);
    }
    context->format.device = device;
    // Every session, callback context, borrowed-package map and device pin is
    // published before the formatter's first flush or destructive write.
    context->format.next = format_contexts;
    format_contexts = &context->format;
    error = xjext4::format(io, native_options, *context->format.workspace, context->format.geometry,
                              &context->format.storage);
    if (!error)
    {
        if (status) status->import_started = true;
        xjext4::WriteDevice writer_io = {format_read, format_write, format_flush, &context->format,
                                        context->format.io_bytes};
        error = xjinstaller::ext4_import_apply_fresh(writer_io, snapshot.time_seconds, context->import,
                                                    context->import_status, &context->storage);
    }
    if (operation_has_failed()) format_fail_io(&context->format);
    if (status)
    {
        auto &geometry = context->format.geometry;
        status->format = {context->format.io_bytes, geometry.blocks, geometry.groups, geometry.inodes,
                          geometry.free_blocks, geometry.metadata_durable, geometry.publication_started,
                          geometry.primary_attempted, context->format.io_failed};
        status->imported_objects = context->import_status.inodes_created;
        status->written_bytes = context->import_status.data_bytes_committed;
        status->import_complete = context->import_status.complete;
        status->import_io_failed = context->import_status.io_failed;
    }
    // Even a semantic error after writer start retains an interrupted session.
    // Do not invent rollback/close after a partial package. The private I/O gate
    // and physical lease are poisoned; public status distinguishes real I/O
    // failure from a quarantined but otherwise readable partial installation.
    if (context->format.io_failed || context->import_status.io_failed ||
        (error && context->import_status.writer_started))
    {
        format_fail_io(&context->format);
        if (status) status->quarantined = true;
        return failure(error ? error : -EIO);
    }
    release();
    return failure(error);
}

int ext4fs_build_node(vfs_node_t device, const ext4_format_options *options,
                      const void *package, size_t package_bytes, ext4_build_status *status)
{
    return build_offline(device, NULL, options, package, package_bytes, status);
}

int ext4fs_build_window(const BlockAccessWindow *window, const ext4_format_options *options,
                        const void *package, size_t package_bytes, ext4_build_status *status)
{
    // Snapshot the trusted descriptor before a possibly overlapping status is
    // cleared. A borrowed descriptor never acquires/relinquishes the parent.
    BlockAccessWindow snapshot = {};
    if (window) snapshot = *window;
    return build_offline(window ? snapshot.node : NULL, window ? &snapshot : NULL, options,
                          package, package_bytes, status);
}

int ext4fs_mount_bound_root(const char *device, const XJ_ROOT_BINDING *binding)
{
    if (!device || !binding || !xj_root_binding_valid(binding)) return failure(-EINVAL);
    return mount_volume(device, rootdir, true, binding);
}

int ext4fs_finish_all()
{
    if (!__atomic_load_n(&operation_initialized, __ATOMIC_ACQUIRE)) return 0;
    Guard guard;
    if (operation_has_failed()) return failure(-EIO);
    if (operations_finished) return finish_result;
    __atomic_store_n(&operations_finished, true, __ATOMIC_RELEASE);
    finish_result = operation_has_failed() ? -EIO : 0;
    if (quarantined_mounts) finish_result = -EIO;
    for (FormatContext *context = format_contexts; context; context = context->next)
        if (context->io_failed) finish_result = -EIO;
    for (Mount *mount : mounts)
    {
        if (!mount) continue;
        if (mount->writable && mount->writer_open)
        {
            int error = operation_has_failed() || mount->io_failed ? -EIO : mount->writer->shutdown();
            if (error && !finish_result) finish_result = error;
            if (!error) mount->writer_open = false;
        }
        // Every callback takes Guard, so no operation can observe the closed
        // writer before this latch is published. Retain the live vnode/device
        // tree for the remaining shutdown code, while rejecting further I/O.
        mount->io_failed = true;
    }
    return failure(finish_result);
}

void ext4fs_quiescent_exit()
{
    // Exit hooks run in task context before FD/VFS cleanup and before freeing
    // terminal TCBs. The mutex checks scheduler references before recovery.
    if (!__atomic_load_n(&operation_initialized, __ATOMIC_ACQUIRE)) return;
    Guard guard;
}

void ext4fs_init()
{
    mutex_create(&operation_lock, true);
    __atomic_store_n(&operation_initialized, true, __ATOMIC_RELEASE);
    filesystem_id = vfs_regist("ext4", &callbacks, 0, 0xef53);
    if (filesystem_id > 0)
    {
        vfs_set_managed_lifetime(filesystem_id, true);
        vfs_set_prepare_unmount(filesystem_id, prepare_unmount);
        vfs_set_checked_unmount(filesystem_id, unmount_checked);
        vfs_set_xattr_ops(filesystem_id, &attributes);
        vfs_set_link_node(filesystem_id, create_link_node);
        vfs_set_unlinked_lifetime(filesystem_id, true);
        vfs_set_finalize_node(filesystem_id, finalize_node);
    }
    static vfs_callback ro_callbacks;
    ro_callbacks = callbacks;
    ro_callbacks.mount = mount_readonly;
    readonly_id = vfs_regist("ext4ro", &ro_callbacks, 0, 0xef53);
    if (readonly_id > 0)
    {
        vfs_set_readonly(readonly_id, true);
        vfs_set_checked_unmount(readonly_id, unmount_checked);
        vfs_set_xattr_ops(readonly_id, &attributes);
        vfs_set_finalize_node(readonly_id, finalize_node);
    }
}

bool ext4fs_owns_node(vfs_node_t node)
{
    return node && ((filesystem_id > 0 && node->fsid == filesystem_id) ||
                    (readonly_id > 0 && node->fsid == readonly_id));
}

int ext4fs_stat(vfs_node_t node, struct stat *result)
{
    if (!ext4fs_owns_node(node) || !node->handle || !result) return -EINVAL;
    Guard guard;
    if (operation_has_failed() || ((Node *)node->handle)->mount->io_failed) return failure(-EIO);
    const xjext4::Inode &inode = ((Node *)node->handle)->inode;
    memset(result, 0, sizeof(*result));
    result->st_dev = node->dev;
    result->st_ino = inode.number;
    result->st_mode = inode.mode;
    result->st_nlink = inode.links;
    result->st_uid = inode.uid;
    result->st_gid = inode.gid;
    result->st_size = inode.size;
    result->st_blocks = inode.blocks_512;
    result->st_blksize = node->blksz;
    result->st_atim.tv_sec = inode.atime;
    result->st_atim.tv_nsec = inode.atime_nsec;
    result->st_mtim.tv_sec = inode.mtime;
    result->st_mtim.tv_nsec = inode.mtime_nsec;
    result->st_ctim.tv_sec = inode.ctime;
    result->st_ctim.tv_nsec = inode.ctime_nsec;
    return 0;
}

int ext4fs_statfs(vfs_node_t node, struct statfs *result)
{
    if (!ext4fs_owns_node(node) || !node->handle || !result) return -EINVAL;
    Guard guard;
    if (operation_has_failed() || ((Node *)node->handle)->mount->io_failed) return failure(-EIO);
    Mount *mount = ((Node *)node->handle)->mount;
    const xjext4::Volume &volume = view(mount);
    memset(result, 0, sizeof(*result));
    result->f_type = 0xef53;
    result->f_bsize = result->f_frsize = volume.block_size();
    result->f_blocks = volume.blocks_count();
    result->f_bfree = volume.free_blocks();
    result->f_bavail = mount->writable ? volume.free_blocks() : 0;
    result->f_files = volume.inodes_count();
    result->f_ffree = volume.free_inodes();
    result->f_namelen = 255;
    result->f_flags = mount->writable ? 0 : 1; // ST_RDONLY
    return 0;
}

int ext4fs_pwrite_existing(vfs_node_t node, uint64_t offset, const void *bytes, size_t length,
                          uint64_t seconds, size_t *completed)
{
    if (completed) *completed = 0;
    if (!completed || !ext4fs_owns_node(node) || !node->handle || (!bytes && length)) return -EINVAL;
    if (!ime_vfs_access(node, NULL, IME_FILE_WRITE)) return failure(-EACCES);
    Guard guard;
    Node *handle = (Node *)node->handle;
    Mount *mount = handle->mount;
    if (operation_has_failed() || mount->io_failed) return failure(-EIO);
    if (offset > handle->inode.size || length > handle->inode.size - offset) return failure(-EOPNOTSUPP);
    if (mount->writable)
    {
        xjext4::Size done = 0;
        int error = mount->writer->pwrite(handle->inode.number, offset, bytes, length, seconds, done);
        if (!error) error = refresh(mount, handle->inode.number);
        *completed = done;
        return mutation_result(mount, error);
    }
    auto *context = static_cast<CompatibilityWriter *>(calloc(1, sizeof(CompatibilityWriter)));
    if (!context) return failure(-ENOMEM);
    context->session = static_cast<xjext4::WriteSession *>(calloc(1, sizeof(xjext4::WriteSession)));
    context->workspace = static_cast<xjext4::WriteWorkspace *>(malloc(sizeof(xjext4::WriteWorkspace)));
    if (!context->session || !context->workspace)
    {
        free(context->session); free(context->workspace); free(context);
        return failure(-ENOMEM);
    }
    // Guard abandonment retains the entire temporary writer through its mount.
    mount->compatibility = context;
    auto *session = context->session;
    auto *workspace = context->workspace;
    xjext4::WriteDevice io = {read_device, write_device, flush_device, mount, mount->device->size};
    int error = xjext4::WriteSession::storage_size(io, context->storage.bytes);
    if (!error)
    {
        context->storage.data = malloc(context->storage.bytes);
        if (!context->storage.data) error = -ENOMEM;
    }
    if (!error) error = session->open(io, *workspace, nullptr, nullptr, &context->storage);
    bool opened = error == 0;
    xjext4::Size done = 0;
    if (!error) error = session->pwrite(handle->inode.number, offset, bytes, length, seconds, done);
    if (opened && !session->failed())
    {
        int close_error = session->close();
        if (!error) error = close_error;
    }
    if (session->failed()) mount->io_failed = true;
    if (mount->io_failed && error) error = -EIO;
    if (!error)
    {
        xjext4::Inode fresh;
        error = view(mount).read_inode(handle->inode.number, fresh);
        if (!error)
        {
            // The mount mutex serializes all reads and keeps cached aliases
            // coherent after a journalled overwrite of a hard-linked inode.
            for (Node *entry = mount->handles; entry; entry = entry->next)
                if (entry->inode.number == fresh.number) entry->inode = fresh;
            *completed = done;
        }
        else mount->io_failed = true;
    }
    if (mount->io_failed || operation_has_failed()) return failure(error ? error : -EIO);
    mount->compatibility = NULL;
    free(context->storage.data);
    free(workspace);
    free(session);
    free(context);
    return failure(error);
}

int ext4fs_sync(vfs_node_t node)
{
    if (!ext4fs_owns_node(node) || !node->handle) return -EINVAL;
    return ioctl_node(node->handle, IOBLKSYNC, NULL);
}

enum AttributePolicy { kWriteAttributes, kOwnerAttributes, kPrivilegedAttributes };

static int change_attributes(vfs_node_t node, const xjext4::AttributeUpdate &update, AttributePolicy policy)
{
    if (!ext4fs_owns_node(node) || !node->handle) return -EINVAL;
    if (!vfs_check_access(node, NULL, IME_FILE_WRITE)) return -vfs_last_error();
    Guard guard;
    Node *handle = (Node *)node->handle;
    Mount *mount = handle->mount;
    // Ownership is shared mutable inode state. Recheck it under the same lock
    // as chown and the transaction, after ordinary caller access was checked.
    bool privileged = privileged_attributes();
    if (!privileged && (policy == kPrivilegedAttributes ||
                        (policy == kOwnerAttributes && node->owner != user_uid(task_effective_user()))))
        return failure(-EPERM);
    int error = handle->deleted ? -ENOENT : mutable_mount(mount);
    if (!error) error = mount->writer->set_attributes(handle->inode.number, update, seconds_now());
    if (!error) error = refresh(mount, handle->inode.number);
    return mutation_result(mount, error);
}

int ext4fs_chmod(vfs_node_t node, uint16_t mode)
{
    if (!node) return -EINVAL;
    xjext4::AttributeUpdate update = {};
    update.mask = xjext4::kAttributeMode;
    update.mode = mode;
    return change_attributes(node, update, kOwnerAttributes);
}

int ext4fs_chown(vfs_node_t node, uint32_t uid, uint32_t gid)
{
    if (!node) return -EINVAL;
    xjext4::AttributeUpdate update = {};
    if (uid != uint32_t(-1)) { update.mask |= xjext4::kAttributeUid; update.uid = uid; }
    if (gid != uint32_t(-1)) { update.mask |= xjext4::kAttributeGid; update.gid = gid; }
    return change_attributes(node, update, kPrivilegedAttributes);
}

int ext4fs_utimens(vfs_node_t node, const struct timespec *times)
{
    if (!node) return -EINVAL;
    uint64_t now = realtime_ns();
    xjext4::AttributeUpdate update = {};
    for (unsigned i = 0; i < 2; ++i)
    {
        if (times && times[i].tv_nsec == UTIME_OMIT) continue;
        bool current = !times || times[i].tv_nsec == UTIME_NOW;
        if (!current && (times[i].tv_nsec < 0 || times[i].tv_nsec >= 1000000000L))
            return failure(-EINVAL);
        int64_t seconds = current ? int64_t(now / 1000000000ULL) : int64_t(times[i].tv_sec);
        uint32_t nanos = current ? uint32_t(now % 1000000000ULL) : uint32_t(times[i].tv_nsec);
        if (!i)
        {
            update.mask |= xjext4::kAttributeAtime;
            update.atime_seconds = seconds;
            update.atime_nanoseconds = nanos;
        }
        else
        {
            update.mask |= xjext4::kAttributeMtime;
            update.mtime_seconds = seconds;
            update.mtime_nanoseconds = nanos;
        }
    }
    return change_attributes(node, update, times ? kOwnerAttributes : kWriteAttributes);
}

int ext4fs_sync_all()
{
    Guard guard;
    int first_error = 0;
    for (Mount *mount : mounts)
    {
        if (!mount || !mount->writable) continue;
        int error = mutable_mount(mount);
        if (!error) error = mount->writer->sync();
        error = mutation_result(mount, error);
        if (error && !first_error) first_error = error;
    }
    return failure(first_error);
}
