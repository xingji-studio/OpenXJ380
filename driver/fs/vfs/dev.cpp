/**
 * Device Virtual File System
 * 设备虚拟文件系统
 */
#define ALL_IMPLEMENTATION
#include "fs/vfs/devfs.h"
#include "device.h"
#include "errno.h"
// #include "kprint.h"
#include "krlibc.h"
// #include "sprintf.h"
#include "fs/vfs/vfs.h"
#include <block_access.h>

extern device_t device_ctl[256];

int             devfs_id    = 0;
static int      devfs_inode = 2;
vfs_node_t      devfs_root  = NULL;
device_handle_t root_handle = NULL;

// Keep the published device_handle ABI as the first member. Cached handles
// retain the original registration identity even when a device ID is reused.
struct RegisteredDeviceHandle
{
    struct device_handle handle;
    uint64_t generation;
};

static uint64_t handle_generation(device_handle_t handle)
{
    return reinterpret_cast<RegisteredDeviceHandle *>(handle)->generation;
}

int block_access_resolve_node(vfs_node_t node, BlockExtent *extent)
{
    if (!node || node->fsid != devfs_id || !(node->type & file_block) || !node->handle) return -ENODEV;
    auto *handle = static_cast<device_handle_t>(node->handle);
    if (handle->is_dir || !handle->device || !handle_generation(handle)) return -ENODEV;
    uintptr_t first = reinterpret_cast<uintptr_t>(&device_ctl[0]);
    uintptr_t value = reinterpret_cast<uintptr_t>(handle->device);
    if (value < first || value - first >= 256 * sizeof(device_t) || (value - first) % sizeof(device_t))
        return -ENODEV;
    if (handle->device->type != DEVICE_BLOCK) return -ENODEV;
    return block_access_resolve(uint32_t((value - first) / sizeof(device_t)), handle_generation(handle), extent);
}

// Original registration-based node capture (Apache-2.0). The walk is bounded
// and performs only tree/handle inspection and a reference increment under the
// existing VFS child lock. No device name is accepted as identity.
namespace
{
constexpr unsigned devfs_capture_nodes = 4096;
constexpr unsigned devfs_capture_depth = 32;

struct DevfsCapture
{
    device_t *device;
    uint64_t generation;
    vfs_node_t match;
    unsigned visited;
    int error;
};

void devfs_capture_tree(vfs_node_t node, unsigned depth, DevfsCapture &capture)
{
    if (capture.error) return;
    if (!node || depth > devfs_capture_depth || ++capture.visited > devfs_capture_nodes)
    {
        capture.error = -E2BIG;
        return;
    }
    if (vfs_node_retiring_locked(node) || (node->type & (file_delete | file_unlinked))) return;
    if (node->fsid == devfs_id && node->handle && !(node->type & file_dir))
    {
        auto *handle = static_cast<device_handle_t>(node->handle);
        if (!handle->is_dir && handle->device == capture.device && handle_generation(handle) == capture.generation)
        {
            if (capture.match) { capture.error = -EEXIST; return; }
            capture.match = node;
        }
    }
    for (list_t entry = node->child; entry && !capture.error; entry = entry->next)
        devfs_capture_tree(static_cast<vfs_node_t>(entry->data), depth + 1, capture);
}

bool devfs_same_extent(const BlockExtent &a, const BlockExtent &b)
{
    return a.device == b.device && a.physical_device == b.physical_device && a.generation == b.generation &&
           a.physical_generation == b.physical_generation && a.start == b.start && a.bytes == b.bytes;
}
} // namespace

int devfs_retain_block(uint32_t device, uint64_t generation, DevfsBlockReference *output)
{
    if (!output) return -EINVAL;
    *output = {};
    if (device >= 256 || !generation) return -EINVAL;
    BlockAccessToken publication = 0;
    int result = block_access_management_begin(&publication);
    if (result) return result;
    DevfsBlockReference captured = {};
    BlockExtent before = {};
    result = block_access_resolve(device, generation, &before);
    if (!result)
    {
        device_t *hardware = &device_ctl[device];
        if (!hardware->flag || hardware->type != DEVICE_BLOCK || hardware->vdiskid != device ||
            !hardware->size || hardware->size != before.bytes || !hardware->sector_size ||
            hardware->sector_size > __UINT32_MAX__ || hardware->size % hardware->sector_size)
            result = -ESTALE;
        else
        {
            captured.extent = before;
            captured.bytes = hardware->size;
            captured.sector_size = hardware->sector_size;
            captured.writable = hardware->write && hardware->write != (void *)empty;
            DevfsCapture capture = {hardware, generation, nullptr, 0, 0};
            vfs_child_lock();
            if (!devfs_root || devfs_root->fsid != devfs_id || vfs_node_retiring_locked(devfs_root))
                result = -ENODEV;
            else
            {
                devfs_capture_tree(devfs_root, 0, capture);
                result = capture.error;
                if (!result && !capture.match) result = -ENODEV;
                if (!result && capture.match->refcount == __UINT32_MAX__) result = -EOVERFLOW;
                if (!result)
                {
                    ++capture.match->refcount;
                    captured.node = capture.match;
                }
            }
            vfs_child_unlock();
        }
    }
    if (!result)
    {
        BlockExtent current = {}, from_node = {};
        result = block_access_resolve(device, generation, &current);
        if (!result) result = block_access_resolve_node(captured.node, &from_node);
        if (!result && (!devfs_same_extent(before, current) || !devfs_same_extent(before, from_node) ||
                        captured.node->size != captured.bytes || captured.node->realsize != captured.sector_size))
            result = -ESTALE;
    }
    block_access_management_end(publication);
    if (result && captured.node)
    {
        int closed = vfs_close(captured.node);
        if (closed)
        {
            captured.quarantined = true;
            *output = captured;
            return closed < 0 ? closed : -EIO;
        }
        captured = {};
    }
    if (!result) *output = captured;
    return result;
}

static errno_t devfs_register0(const char *path, size_t id) {
    device_t *device = &device_ctl[id];
    if (device->flag < 1) return EOK;
    uint64_t generation = device->type == DEVICE_BLOCK ? block_access_generation(id) : 0;
    if (device->type == DEVICE_BLOCK && !generation) return -ENODEV;
    char     *buf    = NULL;
    if (path != NULL) {
        buf = (char*)malloc(strlen(path) + 6);
        sprintf(buf, "/dev/%s", path);
    } else {
        buf = (char*)malloc(5);
        strcpy(buf, "/dev");
    }

    vfs_node_t node = vfs_open(buf);
    if (node == NULL) {
        free(buf);
        return ENOENT;
    }

    device_handle_t parent = (device_handle_t)node->handle;
    if (parent == NULL) {
        vfs_close(node);
        free(buf);
        return ENODEV;
    }

    if (!parent->is_dir) {
        vfs_close(node);
        free(buf);
        return ENOTDIR;
    }

    auto *registered = static_cast<RegisteredDeviceHandle *>(calloc(1, sizeof(RegisteredDeviceHandle)));
    if (!registered) { vfs_close(node); free(buf); return -ENOMEM; }
    registered->generation = generation;
    device_handle_t handle = &registered->handle;
    // not_null_assets(handle, "device handle is null.");
    handle->is_dir = false;
    handle->device = device;
    handle->name   = strdup(device->drive_name);
    if (!handle->name) { free(registered); vfs_close(node); free(buf); return -ENOMEM; }
    vfs_node_t published = vfs_child_append(node, device->drive_name, handle);
    if (!published)
    {
        free(handle->name);
        free(registered);
        vfs_close(node);
        free(buf);
        return -ENOMEM;
    }
    vfs_child_lock();
    if (device->type == DEVICE_BLOCK)
    {
        // Block capture must work before a pathname lookup initializes metadata.
        // Other endpoints retain their existing lazy-open/stat behavior.
        published->fsid = devfs_id;
        published->type = file_block;
        published->size = device->size;
        published->realsize = device->sector_size;
        published->rdev = device->vdiskid;
    }
    llist_append(&parent->child, &handle->curr);
    vfs_child_unlock();
    vfs_close(node);
    free(buf);
    return EOK;
}

static errno_t devfs_mount(const char *handle, vfs_node_t node) {
    if ((uint64_t)handle != DEVFS_REGISTER_ID) return VFS_STATUS_FAILED;
    if (root_handle == NULL) {
        root_handle         =  (device_handle_t)calloc(1, sizeof(struct device_handle));
        root_handle->device = NULL;
        root_handle->is_dir = true;
        llist_init_head(&root_handle->child);
        llist_init_head(&root_handle->curr);
    }
    if (root_handle == NULL) return VFS_STATUS_FAILED;

    node->fsid   = devfs_id;
    node->handle = root_handle;
    node->dev    = 0;
    devfs_root   = node;

    for (size_t i = 0; i < 256; i++) {
        device_t *device = &device_ctl[i];
        // write_serial_fmt("mount dev name: %s\n", device->drive_name);
        devfs_register0(device->path, i);
    }

    return VFS_STATUS_SUCCESS;
}

static errno_t devfs_mkdir(void *handle, const char *name, vfs_node_t node) {
    if (handle == NULL) return VFS_STATUS_FAILED;
    device_handle_t dev_t =  (device_handle_t)handle;
    device_handle_t nw   =  (device_handle_t)calloc(1, sizeof(struct device_handle));
    nw->name             = strdup(name);
    nw->is_dir           = true;
    nw->device           = NULL;
    llist_init_head(&nw->child);
    llist_init_head(&nw->curr);

    llist_append(&dev_t->child, &nw->curr);
    node->handle = nw;
    node->fsid   = 0;
    return EOK;
}

static errno_t devfs_stat(void *handle, vfs_node_t node) {
    if (node->type == file_dir) return VFS_STATUS_SUCCESS;
    if (handle == NULL) return VFS_STATUS_FAILED;
    device_handle_t dev_t =  (device_handle_t)handle;

    node->handle = dev_t;
    node->type   = dev_t->device->type == DEVICE_STREAM ? file_stream :
                   (dev_t->device->type == DEVICE_FB ? file_fbdev : file_block);
    node->size =
        dev_t->device->type == DEVICE_STREAM ? (uint64_t)-1 : disk_size(dev_t->device->vdiskid);
    node->realsize = dev_t->device->sector_size;
    node->fsid     = devfs_id;
    node->rdev     = dev_t->device->vdiskid;
    return VFS_STATUS_SUCCESS;
}

static void devfs_open(void *parent, const char *name, vfs_node_t node) {
    if (node == devfs_root) {
        node->handle = root_handle;
        node->type   = file_dir;
        return;
    }
    if (node->type == file_dir) return;
    if (parent == NULL) return;

    device_handle_t dir_t =  (device_handle_t)parent;
    device_handle_t pos, nxt;

    device_handle_t dev_t = NULL;
    llist_for_each(pos, nxt, &dir_t->child, curr) {
        if (strcmp(pos->name, name) == 0) { dev_t = pos; }
    }
    if (dev_t == NULL) {
        node->handle = NULL;
        return;
    }

    node->handle = dev_t;
    node->type   = dev_t->device->type == DEVICE_STREAM ? file_stream :
                   (dev_t->device->type == DEVICE_FB ? file_fbdev : file_block);
    node->size =
        dev_t->device->type == DEVICE_STREAM ? (uint64_t)-1 : disk_size(dev_t->device->vdiskid);
    node->fsid     = devfs_id;
}

spin_t RD_lk =SPIN_INIT;
size_t devfs_read(void *file, void *addr, size_t offset, size_t size) {
    spin_lock(&RD_lk);
    device_handle_t handle =  (device_handle_t)file;
    if (handle->is_dir) 
    {
        spin_unlock(&RD_lk);
        return VFS_STATUS_FAILED;
    }
    device_t *deivce = handle->device;
    if (deivce->flag == 0)
    {
        spin_unlock(&RD_lk);
        return VFS_STATUS_FAILED;
    }
    device_t device = *deivce;
    spin_unlock(&RD_lk);

    if ((device.type == DEVICE_BLOCK) != (handle_generation(handle) != 0)) return VFS_STATUS_FAILED;
    if (device.type == DEVICE_BLOCK && offset > device.size) return VFS_STATUS_SUCCESS;
    size_t transfer = device.type == DEVICE_BLOCK ? MIN(size, device.size - offset) : size;
    BlockAccessIo access(device.vdiskid, device.type == DEVICE_BLOCK ? handle_generation(handle) : 0,
                          offset, transfer, device.type == DEVICE_BLOCK);
    if (!access.ready()) return VFS_STATUS_FAILED;

    if (device.type == DEVICE_STREAM) {
        size_t bounce_size = size == 0 ? 1 : size;
        uint8_t *bounce = (uint8_t *)malloc(bounce_size);
        if (bounce == NULL) {
            return VFS_STATUS_FAILED;
        }
        size_t got = device.read(device.vdiskid, bounce, size, offset);
        if (got != (size_t)VFS_STATUS_FAILED) {
            memcpy(addr, bounce, got);
        }
        free(bounce);
        return got;
    }
    if (offset > device.size)
    {
        return VFS_STATUS_SUCCESS;
    }
    if (size > device.size - offset) {
        size = device.size - offset;
    }
    return blk_device_read(device.vdiskid, addr, offset, size);
}
spin_t WR_lk =SPIN_INIT;
size_t devfs_write(void *file, const void *addr, size_t offset, size_t size) {
    spin_lock(&WR_lk);
    device_handle_t handle =  (device_handle_t)file;
    if (handle->is_dir)
    {
        spin_unlock(&WR_lk);
        return VFS_STATUS_FAILED;
    }
    device_t *deivce = handle->device;
    if (deivce->flag == 0)
    {
        spin_unlock(&WR_lk);
        return VFS_STATUS_FAILED;
    }
    device_t device = *deivce;
    spin_unlock(&WR_lk);

    if ((device.type == DEVICE_BLOCK) != (handle_generation(handle) != 0)) return VFS_STATUS_FAILED;
    if (device.type == DEVICE_BLOCK && offset > device.size) return VFS_STATUS_SUCCESS;
    size_t transfer = device.type == DEVICE_BLOCK ? MIN(size, device.size - offset) : size;
    BlockAccessIo access(device.vdiskid, device.type == DEVICE_BLOCK ? handle_generation(handle) : 0,
                          offset, transfer, device.type == DEVICE_BLOCK);
    if (!access.ready()) return VFS_STATUS_FAILED;

    if (device.type == DEVICE_STREAM) {
        return device.write(device.vdiskid, (uint8_t *)addr, size, offset);
    }
    if (offset > device.size)
    {
        return VFS_STATUS_SUCCESS;
    }
    if (size > device.size - offset) {
        size = device.size - offset;
    }
    return blk_device_write(device, addr, offset, size);
}

static errno_t devfs_ioctl(void *file, size_t req, void *arg) {
    device_handle_t handle =  (device_handle_t)file;
    if (!handle || handle->is_dir || !handle->device || handle->device->flag == 0 || !handle->device->ioctl)
        return VFS_STATUS_FAILED;
    device_t device = *handle->device;
    if ((device.type == DEVICE_BLOCK) != (handle_generation(handle) != 0)) return -ENODEV;
    BlockAccessIo access(device.vdiskid, device.type == DEVICE_BLOCK ? handle_generation(handle) : 0,
                          0, device.size, device.type == DEVICE_BLOCK);
    if (!access.ready()) return access.error();
    return handle->device->ioctl(handle->device, req, arg);
}

static vfs_node_t devfs_dup(vfs_node_t node) {
    vfs_node_t new_node = vfs_node_alloc(node->parent, node->name);
    if (new_node == NULL) return NULL;
    new_node->type        = node->type;
    new_node->handle      = node->handle;
    new_node->size        = node->size;
    new_node->child       = node->child;
    new_node->flags       = node->flags;
    new_node->permissions = node->permissions;
    new_node->realsize    = node->realsize;
    return new_node;
}

static errno_t devfs_poll(void *file, size_t events) {
    device_handle_t handle =  (device_handle_t)file;
    if (!handle || handle->is_dir || !handle->device || handle->device->flag == 0) return VFS_STATUS_FAILED;
    device_t device = *handle->device;
    if ((device.type == DEVICE_BLOCK) != (handle_generation(handle) != 0)) return -ENODEV;
    BlockAccessIo access(device.vdiskid, device.type == DEVICE_BLOCK ? handle_generation(handle) : 0,
                          0, device.size, device.type == DEVICE_BLOCK);
    if (!access.ready()) return access.error();
    if (handle->device->poll && handle->device->poll != (void *)empty) {
        return handle->device->poll(events);
    } else
        return VFS_STATUS_SUCCESS;
}

static void *devfs_map(void *file, void *addr, size_t offset, size_t size, size_t prot,
                       size_t flags) {
    device_handle_t handle =  (device_handle_t)file;
    if (!handle || handle->is_dir || !handle->device || handle->device->flag == 0) return NULL;
    // A transient ticket cannot pin an mmap until munmap. Block mappings are
    // unsupported until the VMA layer can own a complete extent lease.
    if (handle_generation(handle) || handle->device->type == DEVICE_BLOCK) return NULL;
    if (handle->device->map) { return handle->device->map(handle->device->vdiskid, addr, size); }
    return NULL;
}

static int dummy() {
    return -ENOSYS;
}

static struct vfs_callback devfs_callbacks = {
    .mount    = devfs_mount,
    .unmount  = (vfs_unmount_t)empty,
    .mkdir    = devfs_mkdir,
    .close    = (vfs_close_t)empty,
    .stat     = devfs_stat,
    .open     = devfs_open,
    .read     = devfs_read,
    .write    = devfs_write,
    .readlink = (vfs_readlink_t)dummy,
    .mkfile   = (vfs_mk_t)empty,
    .link     = (vfs_mk_t)dummy,
    .symlink  = (vfs_mk_t)dummy,
    .ioctl    = devfs_ioctl,
    .dup      = devfs_dup,
    .resize   = (vfs_resize_t)dummy,
    .del   = (vfs_del_t)empty,
    .rename   = (vfs_rename_t)empty,
    .poll     = devfs_poll,
    .map      = devfs_map,
};

void devfs_setup() {
    devfs_id = vfs_regist("devfs", &devfs_callbacks, DEVFS_REGISTER_ID, 0x454d444d);
    vfs_mkdir("/dev");
    vfs_node_t dev = vfs_open("/dev");
    if (dev == NULL) {
        write_serial_fmt("'dev' handle is null.");
        return;
    }
    if (vfs_mount((char*)DEVFS_REGISTER_ID, dev) == VFS_STATUS_FAILED) {
        write_serial_fmt("Cannot mount device file system.");
        return;
    }
    vfs_mkdir("/dev/ptx");
}

errno_t devfs_delete(const char *path) {
    char *buf = NULL;
    if (path != NULL) {
        buf = (char*)malloc(strlen(path) + 6);
        sprintf(buf, "/dev/%s", path);
    } else {
        buf = (char*)malloc(5);
        strcpy(buf, "/dev");
    }

    vfs_node_t node = vfs_open(buf);
    if (node == NULL) {
        free(buf);
        return -ENOENT;
    }
    // The operation owns this lookup reference; the checked request consumes
    // it only on success. Descendant or cache references keep the name linked.
    int result = vfs_prune_child(node->parent, node, 1, false);
    if (result) vfs_close(node);
    free(buf);
    return result;
}

errno_t devfs_register(const char *path, size_t id) {
    device_t *device = &device_ctl[id];
    char     *buf    = NULL;
    device->path     = path != NULL ? strdup(path) : NULL;
    if (devfs_root == NULL) { return EOK; }
    return devfs_register0(device->path, id);
}
