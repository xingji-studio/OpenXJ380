#pragma once

char *normalize_path(const char *path);
#define VFS_STATUS_FAILED  (-1)
#define VFS_STATUS_SUCCESS 0

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP         0x4604
#define FBIOPUTCMAP         0x4605
#define FBIOPAN_DISPLAY     0x4606



#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_GETLK         5
#define F_SETLK         6
#define F_SETLKW        7
#define F_SETOWN        8
#define F_GETOWN        9
#define F_SETSIG        10
#define F_GETSIG        11
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1

#define SEEK_SET  0 /* Seek from beginning of file.  */
#define SEEK_CUR  1 /* Seek from current position.  */
#define SEEK_END  2 /* Seek from end of file.  */
#define SEEK_DATA 3
#define SEEK_HOLE 4

#define FIOCLEX 0x5451

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH 0x1000

#define UTIME_NOW  ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   3
#define O_CREAT     0100
#define O_EXCL      0200
#define O_NOCTTY    0400
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_DSYNC     010000
#define O_SYNC      04010000
#define O_RSYNC     04010000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW  0400000
#define O_CLOEXEC   02000000

#define O_ASYNC     020000
#define O_DIRECT    040000
#define O_LARGEFILE 0100000
#define O_NOATIME   01000000
#define O_PATH      010000000
#define O_TMPFILE   020200000
#define O_NDELAY    O_NONBLOCK

#include "proto.hpp"
#include "list.h"
#include "llist.h"

typedef struct vfs_node *vfs_node_t;

typedef errno_t (*vfs_mount_t)(const char *src, vfs_node_t node);
typedef void (*vfs_unmount_t)(void *root);
// Optional fallible durability step, before any mount nodes are destroyed.
typedef int (*vfs_prepare_unmount_t)(void *root);
typedef int (*vfs_checked_unmount_t)(void *handle);

typedef void (*vfs_open_t)(void *parent, const char *name, vfs_node_t node);
typedef void (*vfs_close_t)(void *current);
typedef void (*vfs_resize_t)(void *current, uint64_t size);

// 读写一个文件
typedef size_t (*vfs_write_t)(void *file, const void *addr, size_t offset, size_t size);
typedef size_t (*vfs_read_t)(void *file, void *addr, size_t offset, size_t size);
typedef size_t (*vfs_readlink_t)(vfs_node_t node, void *addr, size_t offset, size_t size);

typedef errno_t (*vfs_stat_t)(void *file, vfs_node_t node);

// 创建一个文件或文件夹
typedef errno_t (*vfs_mk_t)(void *parent, const char *name, vfs_node_t node);
typedef errno_t (*vfs_del_t)(void *parent, vfs_node_t node);
typedef errno_t (*vfs_rename_t)(void *current, const char *nw);

// VFS扩展接口 (CPOS特有, PLOS不支持)
typedef errno_t (*vfs_ioctl_t)(void *file, size_t req, void *arg);
typedef vfs_node_t (*vfs_dup_t)(vfs_node_t node);
typedef errno_t (*vfs_poll_t)(void *file, size_t events);
typedef void *(*vfs_mapfile_t)(void *file, void *addr, size_t offset, size_t size, size_t prot,
                               size_t flags);

enum {
    file_none     = 0x1UL,    // 未获取信息
    file_dir      = 0x2UL,    // 文件夹
    file_block    = 0x4UL,    // 块设备，如硬盘
    file_stream   = 0x8UL,    // 流式设备，如终端
    file_symlink  = 0x10UL,   // 符号链接
    file_fbdev    = 0x20UL,   // 帧缓冲设备
    file_keyboard = 0x40UL,   // 键盘设备
    file_mouse    = 0x80UL,   // 鼠标设备
    file_pipe     = 0x100UL,  // 管道设备
    file_socket   = 0x200UL,  // 套接字设备
    file_epoll    = 0x400UL,  // epoll 设备
    file_ptmx     = 0x800UL,  // ptmx 设备
    file_pts      = 0x1000UL, // pts 设备
    file_proxy    = 0x2000UL, // 代理节点
    file_delete   = 0x4000UL, // 删除标记 (仅在删除时使用)
    file_unlinked = 0x8000UL, // Detached managed name; parent reference remains owned until final close.
};

typedef struct vfs_callback { // VFS回调函数
    vfs_mount_t    mount;     // 挂载文件系统
    vfs_unmount_t  unmount;   // 卸载文件系统 (虚拟文件系统不支持卸载)
    vfs_open_t     open;      // 打开一个文件句柄
    vfs_close_t    close;     // 关闭一个文件句柄
    vfs_read_t     read;      // 读取文件
    vfs_write_t    write;     // 写入文件
    vfs_readlink_t readlink;  // 读取软链接
    vfs_mk_t       mkdir;     // 创建文件夹
    vfs_mk_t       mkfile;    // 创建文件
    vfs_mk_t       link;      // 创建硬链接
    vfs_mk_t       symlink;   // 创建软链接
    vfs_stat_t     stat;      // 检查文件状态信息
    vfs_ioctl_t    ioctl;     // I/O 控制接口 (仅 devfs 等特殊文件系统实现)
    vfs_dup_t      dup;       // 复制文件节点
    vfs_poll_t     poll;      // 轮询文件状态 (仅 devfs 等特殊文件系统实现)
    vfs_mapfile_t  map;       // 映射文件到内存 (仅 devfs 等特殊文件系统实现)
    vfs_resize_t   resize;    // 调整文件大小
    vfs_del_t      del;         // 删除文件或文件夹
    vfs_rename_t   rename;      // 重命名文件或文件夹
} *vfs_callback_t;

typedef struct vfs_filesystem {
    vfs_callback_t      callback;
    char                name[10];
    int                 id;
    uint16_t            fsid;
    uint64_t            magic;
    struct llist_header node;
} *vfs_filesystem_t;

struct vfs_node {
    uint64_t ime_protection; // Kernel-only resolved object policy, never open flags.           // vfs节点
    vfs_node_t parent;      // 父目录
    vfs_node_t linkto;      // 符号链接指向的节点
    char      *name;        // 名称
    char      *linkname;    // 符号链接名称
    uint64_t   realsize;    // 项目真实占用的空间 (可选)
    uint64_t   size;        // 文件大小或若是文件夹则填0
    uint64_t   createtime;  // 创建时间
    uint64_t   readtime;    // 最后读取时间
    uint64_t   writetime;   // 最后写入时间
    uint64_t   inode;       // 节点编号
    uint64_t   blksz;       // 块大小
    uint32_t   owner;       // 所有者
    uint32_t   group;       // 所有组
    uint32_t   permissions; // 权限
    uint16_t   type;        // 类型
    uint32_t   refcount;    // 引用计数
    uint16_t   mode;        // 模式
    uint16_t   fsid;        // 文件系统挂载 id
    void      *handle;      // 操作文件的句柄
    uint64_t   flags;       // 文件标志
    list_t     child;       // 子节点
    vfs_node_t root;        // 根目录
    bool       visited;     // 是否与具体文件系统同步
    bool       is_mount;    // 是否是挂载点
    uint64_t   dev;         // 设备号
    uint64_t   rdev;        // 真实设备号
};

// Optional filesystem extensions stay outside the loadable-module callback ABI.
typedef int (*vfs_link_node_t)(void *parent, vfs_node_t source, vfs_node_t created);
void vfs_set_link_node(int fsid, vfs_link_node_t callback);
typedef int (*vfs_finalize_node_t)(void *handle, vfs_node_t node);
void vfs_set_finalize_node(int fsid, vfs_finalize_node_t callback);
// Bootstrap mount handoff only: caller exclusively owns this empty, unmounted
// directory through backend close AND replacement/publication. No spin/backend
// lock may be held. This is not a concurrent runtime mount reservation API.
// Existing backend errors propagate; never publish another fsid on failure.
int vfs_close_cached_backend(vfs_node_t node);
void vfs_set_unlinked_lifetime(int fsid, bool enabled);
bool vfs_uses_unlinked_lifetime(vfs_node_t node);
struct vfs_xattr_ops
{
    int (*get)(vfs_node_t, const char *, void *, size_t, size_t *);
    int (*list)(vfs_node_t, char *, size_t, size_t *);
    int (*set)(vfs_node_t, const char *, const void *, size_t, unsigned);
    int (*remove)(vfs_node_t, const char *);
};

void vfs_set_xattr_ops(int fsid, const vfs_xattr_ops *ops);
int vfs_getxattr(vfs_node_t node, const char *name, void *value, size_t capacity, size_t *required);
int vfs_listxattr(vfs_node_t node, char *names, size_t capacity, size_t *required);
int vfs_setxattr(vfs_node_t node, const char *name, const void *value, size_t length, unsigned flags);
int vfs_removexattr(vfs_node_t node, const char *name);

struct fd {
    void  *file;
    size_t offset;
    bool   readable;
    bool   writeable;
};

enum {
    DEVFS_REGISTER_ID = 0, // 设备文件系统注册ID
    MODFS_REGISTER_ID = 1, // 模块文件系统注册ID
    TMPFS_REGISTER_ID = 2, // 临时文件系统注册ID
    PIEFS_REGISTER_ID = 3, // 管道文件系统注册ID
    NETFS_REGISTER_ID = 4, // 网络文件系统注册ID
    CPFS_REGISTER_ID  = 5, // CPIO文件系统注册ID
    PROC_REGISTER_ID  = 6, // 进程信息文件系统注册ID
    DNS_REGISTER_ID   = 7, // DNS 文件系统注册ID
    NM_REGISTER_ID    = 8, // NetworkManager 运行态文件系统注册ID
};

extern struct vfs_callback vfs_empty_callback;
extern vfs_node_t          rootdir;
extern struct llist_header fs_metadata_list;

void vfs_child_lock();
void vfs_child_unlock();

/**
 * 创建目录节点
 * @param name 绝对路径
 * @return 非0代表创建失败
 */
errno_t vfs_mkdir(const char *name);
errno_t vfs_mkdir_mode(const char *name, uint16_t mode, uint32_t uid, uint32_t gid);

/**
 * 创建文件节点
 * @param name 绝对路径
 * @return 非0代表创建失败
 */
errno_t vfs_mkfile(const char *name);
errno_t vfs_mkfile_mode(const char *name, uint16_t mode, uint32_t uid, uint32_t gid);

/**
 * 注册文件系统回调指针
 * - 注意: 所有的回调函数都必须实现不得为NULL, 否则会注册失败.
 * (如果不需要某个回调函数, 用一个空实现替代即可)
 * @param name 文件系统名
 * @param callback 回调指针
 * @param register_id 文件系统挂载id (非虚拟文件系统填0)
 * @param magic 文件系统属性类型
 * @return 文件系统id
 */
int vfs_regist(const char *name, vfs_callback_t callback, int register_id, uint64_t magic);
// Fallible boot registration; leaves the registry unchanged on failure.
int vfs_regist_checked(const char *name, vfs_callback_t callback, int register_id, uint64_t magic, int *fsid);
// Filesystem properties are registered separately to preserve the callback ABI.
void vfs_set_readonly(int fsid, bool readonly);
bool vfs_is_readonly(vfs_node_t node);
void vfs_set_managed_lifetime(int fsid, bool enabled);
// Opt-in namespace ownership: generic mounts cannot overwrite this backend.
void vfs_set_reject_overmount(int fsid, bool enabled);
bool vfs_uses_managed_lifetime(vfs_node_t node);
void vfs_set_prepare_unmount(int fsid, vfs_prepare_unmount_t callback);
// Opt into lookup/creation retirement and busy-subtree checks without changing
// a legacy filesystem's file lifetime or virtual symlink behavior.
void vfs_set_unmount_quiescence(int fsid, bool enabled);
// On error the adapter must retain its context/device lifetime. This extension
// lets VFS report final teardown errors without changing vfs_callback's ABI.
void vfs_set_checked_unmount(int fsid, vfs_checked_unmount_t callback);
// Historical name retained for ABI compatibility; managed RW mounts may use it.
int vfs_publish_readonly_mount(vfs_node_t node, const struct vfs_node *prepared);
// Boot-only publication retaining the original root and mounted /dev tree.
// Caller releases covered private directory placeholders after unlocking.
int vfs_publish_system_root(struct vfs_node *prepared, list_t *covered);
// Boot-only deep RAM tree publication. The caller exclusively owns the fresh
// tree (root ref=1, each nonroot ref=2: cache plus backend pin). Descendant
// root pointers already name the stable rootdir. Bindings point to backend
// root-owner fields initially equal to prepared; commit changes them together
// with rootdir. No callback/allocator runs in the publication lock.
int vfs_publish_system_tree(struct vfs_node *prepared, uint64_t expected_nodes,
                            vfs_node_t **bindings, size_t binding_count, list_t *covered);

/**
 * 创建 link 文件
 * @param name 被链接文件
 * @param target_name 创建的link文件名
 * @return 是否成功创建
 */
errno_t vfs_link(const char *name, const char *target_name);

/**
 * 创建一个符号链接文件
 * @param name 被链接文件
 * @param target_name 创建的链接文件
 * @return 是否成功创建
 */
errno_t vfs_symlink(const char *name, const char *target_name);

/**
 * 向父节点添加一个子节点
 * @param parent 父节点
 * @param name 子节点名称
 * @param handle 节点句柄
 * @return 子节点
 */
vfs_node_t vfs_child_append(vfs_node_t parent, const char *name, void *handle);
vfs_node_t vfs_node_alloc(vfs_node_t parent, const char *name);

/**
 * 关闭一个已打开的 vfs 节点
 * @param node
 * @return 非0 代表错误返回
 */
errno_t vfs_close(vfs_node_t node);

/**
 * 释放一个 vfs 节点
 * @param vfs 节点
 */
void vfs_free(vfs_node_t vfs);
// Caller holds vfs_child_lock; this bounded query never allocates or performs I/O.
bool vfs_node_retiring_locked(vfs_node_t node);
// Retire a still-linked cached child outside all spin/backend locks. `parent`
// must remain retained. On success consume its tree ownership and exactly
// held_references caller-owned pins; on failure preserve both tree and pins.
// Cache eviction and checked backend close precede any object destruction.
int vfs_prune_child(vfs_node_t parent, vfs_node_t child, unsigned held_references = 0,
                    bool require_unvisited = false);
// Caller serializes a private retired queue and owns an already detached tree.
// EBUSY preserves the tree and mount lifetime; success consumes/frees it. Never
// call under a spin/backend lock or mark these nodes file_delete/unlinked.
int vfs_try_free_detached(vfs_node_t node);

/**
 * 更新 vfs 节点的状态
 * @param node 节点
 */
void vfs_update(vfs_node_t node);

/**
 * 打开一个节点
 * @param str 路径(绝对路径)
 * @return 为NULL代表打开失败
 */
vfs_node_t vfs_open(const char *str);
vfs_node_t vfs_open_no_follow(const char *str);
// Retained metadata lookup. Rejects pipe/PTY endpoints with EOPNOTSUPP before
// opening or retaining them; release ordinary returned nodes with vfs_close.
vfs_node_t vfs_open_metadata(const char *str, bool follow_final_symlink);

errno_t vfs_register_alias(const char *alias_path, const char *target_path);

/**
 * 向一个节点发送 I/O 控制命令
 * @param device 节点设备
 * @param options 操作码
 * @param arg 参数
 * @return 非0代表操作失败
 */
errno_t vfs_ioctl(vfs_node_t device, size_t options, void *arg);

/**
 * 读取一个符号链接文件
 * @param node 文件节点
 * @param buf 缓冲区
 * @param bufsize
 * @return
 */
size_t vfs_readlink(vfs_node_t node, char *buf, size_t bufsize);

/**
 * 根据类型获取文件系统句柄
 * @param type 类型
 * @return 句柄(NULL为找不到)
 */
vfs_filesystem_t get_filesystem(char *type);

/**
 * 获取指定文件节点的文件系统描述符
 * @param node 文件节点
 * @return 文件系统描述符
 */
vfs_filesystem_t get_filesystem_node(vfs_node_t node);

bool is_virtual_fs(const char *src);

vfs_node_t vfs_do_search(vfs_node_t dir, const char *name);
void       vfs_free_child(vfs_node_t vfs);
errno_t    vfs_delete(vfs_node_t node);
// Consumes the caller's retained node reference on every success/failure path.
errno_t    vfs_delete_owned(vfs_node_t node);
errno_t    vfs_rename(vfs_node_t node, const char *nw);
errno_t    vfs_resize(vfs_node_t node, uint64_t size);
errno_t    vfs_resize_fast(vfs_node_t node, uint64_t size);
errno_t    vfs_poll(vfs_node_t node, size_t event);
void      *vfs_map(vfs_node_t node, uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
                   uint64_t offset);
size_t     vfs_read(vfs_node_t file, void *addr, size_t offset, size_t size);  // 读取节点数据
size_t     vfs_write(vfs_node_t file, void *addr, size_t offset, size_t size); // 写入节点
size_t     vfs_write_fast(vfs_node_t file, void *addr, size_t offset, size_t size);
void *general_map(vfs_read_t read_callback, void *file, uint64_t addr, uint64_t len, uint64_t prot,
                  uint64_t flags, uint64_t offset); // 文件映射

/**
 * 挂载一个文件系统到指定节点
  * 挂载后，src 代表设备的路径，node 代表挂载点
  * 挂载点必须是一个目录
  *
 * @param src 设备路径
 * @param node 挂载点
 * @return 非0代表挂载失败
 */
errno_t vfs_mount(const char *src, vfs_node_t node);

/**
 * 卸载一个挂载点
 * - 注意: 一些内核常驻挂载点不会被此函数卸载, 如 devfs/tmpfs 等
 * @param path 挂载点路径
 * @return 非0代表写在失败
 */
errno_t vfs_unmount(const char *path);

/**
 * 获取根目录节点
 * @return 根目录节点
 */
vfs_node_t get_rootdir();
void       set_rootdir(vfs_node_t node);

char *vfs_get_fullpath(vfs_node_t node);
char *at_resolve_pathname(int dirfd, char *pathname);
char *vfs_cwd_path_build(char *src); // 构建当前工作目录的路径
bool  vfs_init();

// Authorization is separate from internal name lookup.
bool vfs_check_access(vfs_node_t node, const char *path, int operation);
// Checks the canonical destination child, without mutating its existing parent.
bool vfs_check_child_creation(vfs_node_t parent, const char *name);
int vfs_last_error();
