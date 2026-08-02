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
#include <device.h>
#include <task/poll.h>
#include <rng.h>

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
    if (file->handle != NULL) {
        callbackof(file, stat)(file->handle, file);
    } else {
        if (file->parent == NULL || file->parent->handle == NULL) return;
        callbackof(file, open)(file->parent->handle, file->name, file);
    }
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
    vfs_node_t child = (vfs_node_t)list_first(parent->child, data, streq(name, ((vfs_node_t)data)->name));
    vfs_child_unlock();
    return child;
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

errno_t vfs_mkdir(const char *name) {
    if (name[0] != '/') return VFS_STATUS_FAILED;
    char      *path     = strdup(name + 1);
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
            current       = vfs_node_alloc(father, buf);
            current->type = file_dir;
            callbackof(father, mkdir)(father->handle, buf, current);
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

errno_t vfs_link(const char *name, const char *target_name) {
    vfs_node_t current = rootdir;
    char      *path    = strdup(name + 1);

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
            new_current       = vfs_node_alloc(current, buf);
            new_current->type = file_dir;
            callbackof(current, mkdir)(current->handle, buf, new_current);
        }
        current = new_current;
        do_update(current);

        if (!(current->type & file_dir)) goto err;
    }

create:
    node = vfs_child_append(current, filename, NULL);
    node->type      = file_none;
    callbackof(current, link)(current->handle, target_name, node);
    node->linkto = vfs_open(target_name);

    free(path);

    return EOK;

err:
    free(path);
    return VFS_STATUS_FAILED;
}

errno_t vfs_symlink(const char *name, const char *target_name) {
    vfs_node_t current = rootdir;
    char      *path    = strdup(name + 1);

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
            new_current       = vfs_node_alloc(current, buf);
            new_current->type = file_dir;
            callbackof(current, mkdir)(current->handle, buf, new_current);
        }
        current = new_current;
        do_update(current);

        if (!(current->type & file_dir)) goto err;
    }

create:
    node = vfs_child_append(current, filename, NULL);
    node->type      = file_symlink;
    node->linkname  = strdup(target_name);
    node->size      = node->linkname != NULL ? strlen(node->linkname) : 0;
    callbackof(current, symlink)(current->handle, target_name, node);
    node->linkto = vfs_open(target_name);

    free(path);

    return EOK;

err:
    free(path);
    return -EIO;
}

errno_t vfs_mkfile(const char *name) {
    if (name[0] != '/') return VFS_STATUS_FAILED;

    // 分离路径和文件名
    char *fullpath  = strdup(name);
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

    if (parent == NULL || parent->type != file_dir) { return VFS_STATUS_FAILED; }

    // 创建文件
    vfs_node_t node = vfs_child_append(parent, filename, NULL);
    node->type      = file_none;
    errno_t status  = callbackof(parent, mkfile)(parent->handle, filename, node);
    free(fullpath);
    return status;
}

errno_t vfs_delete(vfs_node_t node) {
    if (node == rootdir) return VFS_STATUS_FAILED;
    if (node == NULL || node->parent == NULL) return VFS_STATUS_FAILED;
    if ((node->type & file_dir) && node->child != NULL) return -ENOTEMPTY;

    if (!((node->type & file_symlink) && node->linkname != NULL))
    {
        errno_t res = callbackof(node, del)(node->parent->handle, node);
        if (res < 0) return res;
    }

    vfs_child_lock();
    node->parent->child = list_delete(node->parent->child, node);
    vfs_child_unlock();

    node->parent = NULL;
    node->handle = NULL;
    vfs_free(node);
    return VFS_STATUS_SUCCESS;
}

errno_t vfs_rename(vfs_node_t node, const char *nw) {
    if (node == NULL || nw == NULL) return VFS_STATUS_FAILED;
    if ((node->type & file_symlink) && node->linkname != NULL)
    {
        char *new_name = strrchr(nw, '/');
        new_name = new_name != NULL ? new_name + 1 : (char *)nw;
        if (*new_name == '\0') return VFS_STATUS_FAILED;
        char *copy = strdup(new_name);
        if (copy == NULL) return VFS_STATUS_FAILED;

        char *parent_path = strdup(nw);
        if (parent_path == NULL)
        {
            free(copy);
            return VFS_STATUS_FAILED;
        }
        char *last_slash = strrchr(parent_path, '/');
        if (last_slash == parent_path)
        {
            parent_path[1] = '\0';
        }
        else if (last_slash != NULL)
        {
            *last_slash = '\0';
        }
        else
        {
            strcpy(parent_path, ".");
        }

        vfs_node_t new_parent = streq(parent_path, ".") ? node->parent : vfs_open(parent_path);
        free(parent_path);
        if (new_parent == NULL || !(new_parent->type & file_dir))
        {
            if (new_parent != NULL && new_parent != node->parent) vfs_close(new_parent);
            free(copy);
            return VFS_STATUS_FAILED;
        }

        if (new_parent != node->parent)
        {
            vfs_child_lock();
            node->parent->child = list_delete(node->parent->child, node);
            new_parent->child = list_prepend(new_parent->child, node);
            node->parent = new_parent;
            node->fsid = new_parent->fsid;
            node->root = new_parent->root;
            node->dev = new_parent->dev;
            vfs_child_unlock();
        }

        free(node->name);
        node->name = copy;
        return VFS_STATUS_SUCCESS;
    }
    do_update(node);
    if (node->handle == NULL) return VFS_STATUS_FAILED;
    return callbackof(node, rename)(node->handle, nw);
}

errno_t vfs_resize(vfs_node_t node, uint64_t size) {
    if (node == NULL) return VFS_STATUS_FAILED;
    do_update(node);
    if (node->type == file_dir) return VFS_STATUS_FAILED;
    callbackof(node, resize)(node->handle, size);
    do_update(node);
    return VFS_STATUS_SUCCESS;
}

errno_t vfs_resize_fast(vfs_node_t node, uint64_t size) {
    if (node == NULL) return VFS_STATUS_FAILED;
    if (node->type == file_dir) return VFS_STATUS_FAILED;
    callbackof(node, resize)(node->handle, size);
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

vfs_node_t vfs_do_search(vfs_node_t dir, const char *name) {
    if (dir == NULL || name == NULL) return NULL;
    vfs_child_lock();
    vfs_node_t child = (vfs_node_t)list_first(dir->child, data, streq(name, ((vfs_node_t)data)->name));
    vfs_child_unlock();
    return child;
}

static constexpr unsigned VFS_MAX_LINK_DEPTH = 40;

static vfs_node_t vfs_open_impl(const char *str, bool follow_final_symlink, bool allow_alias, unsigned link_depth) {
    if (unlikely(str == NULL)) return NULL;
    if (unlikely(str[0] != '/')) return NULL;
    if (unlikely(link_depth > VFS_MAX_LINK_DEPTH)) return NULL;
    if (str[1] == '\0') return rootdir; // 根目录

    char *path = strdup(str + 1);
    if (unlikely(path == NULL)) return NULL;

    char      *save_ptr = path;
    vfs_node_t current  = rootdir;

    for (char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) {
            // 当前目录，不需要操作
            continue;
        } else if (streq(buf, "..")) {
            // 父目录
            if (current->parent) { current = current->parent; }
            continue;
        }

        current = vfs_child_find(current, buf);
        if (current == NULL) { goto err; }

        bool final_component_had_handle = current->handle != NULL;
        do_update(current);
        bool is_final_component = save_ptr == NULL || *save_ptr == '\0';
        if (is_final_component && !final_component_had_handle && current->handle != NULL && current->refcount == 1)
            current->refcount = 0;
        if (is_final_component && !follow_final_symlink) break;
        if (current->type & file_symlink) {
            if (!current->parent) { goto err; }

            bool dynamic_symlink = current->name != NULL && streq(current->name, "self") &&
                                   current->parent != NULL && current->parent->parent == NULL &&
                                   current->handle != NULL && current->linkto == NULL;
            if (!dynamic_symlink) current->type = file_symlink | file_proxy;

            vfs_node_t target = dynamic_symlink ? NULL : current->linkto;
            if (target == NULL)
            {
                char target_path[256];
                size_t len = vfs_readlink(current, target_path, sizeof(target_path) - 1);
                if (len == (size_t)VFS_STATUS_FAILED || len == 0 || len >= sizeof(target_path)) goto err;
                target_path[len] = '\0';
                if (target_path[0] == '/')
                {
                    target = vfs_open_impl(target_path, true, true, link_depth + 1);
                }
                else
                {
                    char *parent_path = vfs_get_fullpath(current->parent);
                    if (parent_path == NULL) goto err;
                    size_t parent_len = strlen(parent_path);
                    size_t target_len = strlen(target_path);
                    char *joined_target = (char *)malloc(parent_len + target_len + 2);
                    if (joined_target == NULL)
                    {
                        free(parent_path);
                        goto err;
                    }
                    sprintf(joined_target, "%s%s%s", parent_path,
                            parent_path[parent_len - 1] == '/' ? "" : "/", target_path);
                    char *full_target = normalize_path(joined_target);
                    free(joined_target);
                    free(parent_path);
                    if (full_target == NULL) goto err;
                    target = vfs_open_impl(full_target, true, true, link_depth + 1);
                    free(full_target);
                }
                if (!dynamic_symlink) current->linkto = target;
            }
            if (!target) goto err;

            current = target;
            continue;
        }
    }

    free(path);
    if (current != NULL) current->refcount++;
    return current;
err:
    free(path);
    if (allow_alias) {
        char *alias_target = vfs_alias_target_dup(str);
        if (alias_target != NULL) {
            write_serial_fmt("[busybox-debug] vfs_alias_hit %s -> %s\n", str, alias_target);
            vfs_node_t alias_node = vfs_open_impl(alias_target, follow_final_symlink, false, link_depth + 1);
            free(alias_target);
            return alias_node;
        }
    }
    return NULL;
}

vfs_node_t vfs_open(const char *str) {
    return vfs_open_impl(str, true, true, 0);
}

vfs_node_t vfs_open_no_follow(const char *str) {
    return vfs_open_impl(str, false, true, 0);
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
    node->refcount++;
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
    if (unlikely(node == NULL)) return NULL;
    if (unlikely(node->type == file_dir)) return NULL;
    return callbackof(node, map)(node->handle, (void *)addr, offset, len, prot, flags);
}

vfs_node_t vfs_node_alloc(vfs_node_t parent, const char *name) {
    vfs_node_t node = (vfs_node_t)malloc(sizeof(struct vfs_node));
    // not_null_assets(node, "vfs alloc null");
    if (unlikely(node == NULL)) return NULL;
    memset(node, 0, sizeof(struct vfs_node));
    node->parent   = parent;
    node->name     = name ? strdup(name) : NULL;
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
        vfs_child_lock();
        parent->child = list_prepend(parent->child, node);
        vfs_child_unlock();
    }
    return node;
}

errno_t vfs_close(vfs_node_t node) {
    if (unlikely(node == NULL)) return VFS_STATUS_FAILED;
    if (node == rootdir) return VFS_STATUS_SUCCESS;

    if (node->refcount > 0) node->refcount--;

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

    if (unlikely(node->handle == NULL)) return VFS_STATUS_SUCCESS;

    if ((node->type & file_socket) && node->refcount > 0) {
        return VFS_STATUS_SUCCESS;
    }

    if (node->type & file_proxy){
         return VFS_STATUS_SUCCESS;
    }
    if ((node->type & file_dir) && !(node->type & file_delete))
    {
        return VFS_STATUS_SUCCESS;
    } 
    if (!(node->type & file_delete) && node->refcount > 0) {
        return VFS_STATUS_SUCCESS;
    }
    if (node->type & file_delete && node->refcount <= 0) {
        errno_t res = callbackof(node, del)(node->parent->handle, node);
        if (res < 0) return res;
        vfs_child_lock();
        node->parent->child = list_delete(node->parent->child, node);
        vfs_child_unlock();
        node->handle = NULL;
        vfs_free(node);
    } else {
        callbackof(node, close)(node->handle);
        node->handle = NULL;
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

void vfs_free(vfs_node_t vfs) {
    if (vfs == NULL) return;
    vfs_child_lock();
    list_t child = vfs->child;
    vfs->child   = NULL;
    vfs_child_unlock();
    list_free_with(child, (void (*)(void *))vfs_free);
    vfs_close(vfs);
    free(vfs->name);
    free(vfs->linkname);
    free(vfs);
}

void vfs_free_child(vfs_node_t vfs) {
    if (vfs == NULL) return;
    vfs_child_lock();
    list_t child = vfs->child;
    vfs->child   = NULL;
    vfs_child_unlock();
    list_free_with(child, (void (*)(void *))vfs_free);
}

errno_t vfs_mount(const char *src, vfs_node_t node) {
    if (node == NULL) return VFS_STATUS_FAILED;
    if (node->type != file_dir) return VFS_STATUS_FAILED;
    for (int i = 1; i < fs_nextid; i++) {
        if (fs_callbacks[i]->mount(src, node) == 0) {
            node->fsid     = i;
            node->root     = node;
            node->is_mount = true;
            return VFS_STATUS_SUCCESS;
        }
    }
    return VFS_STATUS_FAILED;
}

size_t vfs_read(vfs_node_t file, void *addr, size_t offset, size_t size) {
    if (file == NULL || addr == NULL) return VFS_STATUS_FAILED;
    do_update(file);
    if (file->type == file_dir) return VFS_STATUS_FAILED;
    return callbackof(file, read)(file->handle, addr, offset, size);
}

size_t vfs_write(vfs_node_t file, void *addr, size_t offset, size_t size) {
    if (file == NULL || addr == NULL) return VFS_STATUS_FAILED;
    do_update(file);
    if (file->type == file_dir) return VFS_STATUS_FAILED;
    size_t ret = callbackof(file, write)(file->handle, addr, offset, size);
    do_update(file);
    return ret;
}

size_t vfs_write_fast(vfs_node_t file, void *addr, size_t offset, size_t size) {
    if (file == NULL || addr == NULL) return VFS_STATUS_FAILED;
    if (file->type == file_dir) return VFS_STATUS_FAILED;
    return callbackof(file, write)(file->handle, addr, offset, size);
}

errno_t vfs_ioctl(vfs_node_t device, size_t options, void *arg) {
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

errno_t vfs_unmount(const char *path) {
    vfs_node_t node = vfs_open(path);
    if (node == NULL) return VFS_STATUS_FAILED;
    if (node->type != file_dir) return VFS_STATUS_FAILED;
    if (node->fsid == 0) return VFS_STATUS_FAILED;
    if (node->parent) {
        vfs_node_t cur = node;
        node           = node->parent;
        if (cur->root == cur) {
            vfs_free_child(cur);
            callbackof(cur, unmount)(cur->handle);
            cur->fsid     = node->fsid; // 交给上级
            cur->root     = node->root;
            cur->handle   = NULL;
            cur->child    = NULL;
            cur->is_mount = false;
            if (cur->fsid) do_update(cur);
            return VFS_STATUS_SUCCESS;
        }
    }
    return VFS_STATUS_FAILED;
}

size_t vfs_readlink(vfs_node_t node, char *buf, size_t bufsize) {
    if (node != NULL && node->linkname != NULL) {
        size_t link_size = strlen(node->linkname);
        size_t copy_size = MIN(link_size, bufsize);
        memcpy(buf, node->linkname, copy_size);
        return copy_size;
    }
    size_t ret = callbackof(node, readlink)(node, buf, 0, bufsize);
    return ret;
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

spin_t get_path_lock = SPIN_INIT;

// The caller owns the returned absolute path.
char *vfs_get_fullpath(vfs_node_t node)
{
    if (node == NULL) return NULL;

    spin_lock(&get_path_lock);

    size_t capacity = 32;
    size_t count    = 0;
    vfs_node_t *nodes = (vfs_node_t *)malloc(sizeof(vfs_node_t) * capacity);
    if (nodes == NULL)
    {
        spin_unlock(&get_path_lock);
        return NULL;
    }

    for (vfs_node_t current = node; current != NULL; current = current->parent)
    {
        if (count == capacity)
        {
            if (capacity > (size_t)-1 / 2 / sizeof(vfs_node_t))
            {
                free(nodes);
                spin_unlock(&get_path_lock);
                return NULL;
            }

            capacity *= 2;
            vfs_node_t *expanded = (vfs_node_t *)realloc(nodes, sizeof(vfs_node_t) * capacity);
            if (expanded == NULL)
            {
                free(nodes);
                spin_unlock(&get_path_lock);
                return NULL;
            }
            nodes = expanded;
        }
        nodes[count++] = current;
    }

    size_t path_length = 1;
    for (size_t index = count; index > 0; index--)
    {
        vfs_node_t current = nodes[index - 1];
        if (current == rootdir) continue;
        if (current->name == NULL)
        {
            free(nodes);
            spin_unlock(&get_path_lock);
            return NULL;
        }

        size_t name_length = strlen(current->name);
        size_t separator   = index > 1 ? 1 : 0;
        if (name_length > (size_t)-1 - path_length - separator - 1)
        {
            free(nodes);
            spin_unlock(&get_path_lock);
            return NULL;
        }
        path_length += name_length + separator;
    }

    char *path = (char *)malloc(path_length + 1);
    if (path == NULL)
    {
        free(nodes);
        spin_unlock(&get_path_lock);
        return NULL;
    }

    char *cursor = path;
    *cursor++ = '/';
    for (size_t index = count; index > 0; index--)
    {
        vfs_node_t current = nodes[index - 1];
        if (current == rootdir) continue;

        size_t name_length = strlen(current->name);
        memcpy(cursor, current->name, name_length);
        cursor += name_length;
        if (index > 1) *cursor++ = '/';
    }
    *cursor = '\0';

    free(nodes);
    spin_unlock(&get_path_lock);
    return path;
}

char *at_resolve_pathname(int dirfd, char *pathname) {
    if (pathname[0] == '/') { // by absolute pathname
        return normalize_path(pathname);
    } else if (pathname[0] != '/') {
        if (dirfd == AT_FDCWD) { // relative to cwd
            return vfs_cwd_path_build(pathname);
        } else { // relative to dirfd, resolve accordingly
            fd_file_handle *handle = (fd_file_handle *)queue_get(get_current_task()->parent_group->file_open, dirfd);
            if (!handle || !handle->node) return NULL;
            vfs_node_t node = handle->node;
            if (node->type != file_dir) return NULL;

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

    size_t len    = strlen(path);
    char  *result = (char *)malloc(len + 1);
    if (!result) return NULL;

    char *dup = strdup(path);
    if (!dup)
    {
        free(result);
        return NULL;
    }

    strcpy(result, "/");
    if (strcmp(path, "/") == 0)
    {
        free(dup);
        return result;
    }

    char *start = dup;
    if (*start == '/') start++;

    char *token = strtok(start, "/");
    while (token)
    {
        if (strcmp(token, ".") == 0) {}
        else if (strcmp(token, "..") == 0)
        {
            char *last_slash = strrchr(result, '/');
            if (last_slash != result)
                *last_slash = '\0';
            else
                result[1] = '\0';
        }
        else
        {
            if (result[strlen(result) - 1] != '/') strcat(result, "/");
            strcat(result, token);
        }

        token = strtok(NULL, "/");
    }

    free(dup);
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
