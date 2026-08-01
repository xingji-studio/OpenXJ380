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

extern device_t device_ctl[26]; // vdisk.c

int             devfs_id    = 0;
static int      devfs_inode = 2;
vfs_node_t      devfs_root  = NULL;
device_handle_t root_handle = NULL;

static errno_t devfs_register0(const char *path, size_t id) {
    device_t *device = &device_ctl[id];
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

    device_handle_t handle = (device_handle_t)calloc(1, sizeof(struct device_handle));
    // not_null_assets(handle, "device handle is null.");
    handle->is_dir = false;
    handle->device = device;
    handle->name   = strdup(device->drive_name);
    node->rdev     = device->vdiskid;
    vfs_child_append(node, device->drive_name, handle);
    llist_append(&parent->child, &handle->curr);
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
    node->refcount = 1;
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
    if (handle->device->flag == 0) return VFS_STATUS_FAILED;
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
    if (handle->device->flag == 0) return VFS_STATUS_FAILED;
    if (handle->device->poll != (void *)empty) {
        return handle->device->poll(events);
    } else
        return VFS_STATUS_SUCCESS;
}

static void *devfs_map(void *file, void *addr, size_t offset, size_t size, size_t prot,
                       size_t flags) {
    device_handle_t handle =  (device_handle_t)file;
    if (handle->device->flag == 0) return NULL;
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
    node->parent->child = list_delete(node->parent->child, node);
    vfs_free(node);
    free(buf);
    return EOK;
}

errno_t devfs_register(const char *path, size_t id) {
    device_t *device = &device_ctl[id];
    char     *buf    = NULL;
    device->path     = path != NULL ? strdup(path) : NULL;
    if (devfs_root == NULL) { return EOK; }
    return devfs_register0(device->path, id);
}
