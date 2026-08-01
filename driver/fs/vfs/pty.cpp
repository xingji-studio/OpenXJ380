#define ALL_IMPLEMENTATION

#include <errno.h>
#include <fs/vfs/vfs.h>
#include <id_alloc.h>
#include <ioctl.h>
#include <krlibc.h>
#include <proto.hpp>
#include <pty.h>
#include <task/pcb.h>
#include <task/poll.h>
#include <task/scheduler.h>

static int                 ptmx_fsid = 0;
static int                 pts_fsid  = 0;
static struct llist_header ptmx_list_head;
static spin_t              pty_global_lock = SPIN_INIT;
static id_allocator_t     *pty_allocator   = NULL;
static vfs_node_t          pts_root        = NULL;

static int pty_dummy()
{
    return -ENOSYS;
}

static int pty_str_to_int(const char *str, int *result)
{
    if (str == NULL || *str == '\0' || result == NULL) return -EINVAL;

    int value = 0;
    for (; *str != '\0'; str++)
    {
        if (*str < '0' || *str > '9') return -EINVAL;
        value = value * 10 + (*str - '0');
    }

    *result = value;
    return 0;
}

static int pty_id_alloc()
{
    spin_lock(&pty_global_lock);
    int ret = id_alloc(pty_allocator);
    spin_unlock(&pty_global_lock);
    return ret;
}

static void pty_id_free(int id)
{
    spin_lock(&pty_global_lock);
    id_free(pty_allocator, (uint32_t)id);
    spin_unlock(&pty_global_lock);
}

static void pty_termios_default(struct termios *term)
{
    memset(term, 0, sizeof(*term));
    term->c_iflag = ICRNL | IXON | BRKINT | ISTRIP | INPCK;
    term->c_oflag = OPOST | ONLCR;
    term->c_cflag = B38400 | CS8 | CREAD | HUPCL;
    term->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;

    term->c_cc[VINTR]  = 3;
    term->c_cc[VQUIT]  = 28;
    term->c_cc[VERASE] = 127;
    term->c_cc[VKILL]  = 21;
    term->c_cc[VEOF]   = 4;
    term->c_cc[VTIME]  = 0;
    term->c_cc[VMIN]   = 1;
    term->c_cc[VSTART] = 17;
    term->c_cc[VSTOP]  = 19;
    term->c_cc[VSUSP]  = 26;
}

static pty_handle_t *pty_find_locked(int id)
{
    pty_handle_t *pos;
    pty_handle_t *n;

    llist_for_each(pos, n, &ptmx_list_head, list_node)
    {
        if (pos->id == id) return pos;
    }
    return NULL;
}

static void pty_pair_destroy_locked(pty_handle_t *pair)
{
    if (pair->slave_node != NULL && pair->slave_node->parent != NULL)
    {
        bool slave_is_open = pair->slave_node->handle != NULL;
        vfs_child_lock();
        pair->slave_node->parent->child = list_delete(pair->slave_node->parent->child, pair->slave_node);
        vfs_child_unlock();
        pair->slave_node->handle = NULL;
        pair->slave_node->parent = NULL;
        if (!slave_is_open)
        {
            free(pair->slave_node->name);
            free(pair->slave_node);
        }
    }

    llist_delete(&pair->list_node);
    id_free(pty_allocator, (uint32_t)pair->id);
    free(pair->master_buffer);
    free(pair->slave_buffer);
    free(pair);
}

static size_t ptmx_data_available(pty_handle_t *pair)
{
    return pair->ptr_master;
}

static size_t pts_data_available(pty_handle_t *pair)
{
    if (!(pair->term.c_lflag & ICANON)) return pair->ptr_slave;

    for (size_t i = 0; i < pair->ptr_slave; i++)
    {
        if (pair->slave_buffer[i] == '\n' || pair->slave_buffer[i] == pair->term.c_cc[VEOF] ||
            pair->slave_buffer[i] == pair->term.c_cc[VEOL] ||
            pair->slave_buffer[i] == pair->term.c_cc[VEOL2])
            return i + 1;
    }
    return 0;
}

static void pts_ctrl_assign(pty_handle_t *pair)
{
    tcb_t task = get_current_task();
    pair->ctrl_pgid = task != NULL && task->parent_group != NULL ? (int)task->parent_group->pid : 0;
}

static void ptmx_open(void *parent, const char *name, vfs_node_t node)
{
    UNUSED(parent, name);

    pty_handle_t *pair = (pty_handle_t *)calloc(1, sizeof(pty_handle_t));
    if (pair == NULL) return;

    llist_init_head(&pair->list_node);
    pty_termios_default(&pair->term);
    pair->win.ws_row    = 24;
    pair->win.ws_col    = 80;
    pair->tty_kbmode    = K_XLATE;
    pair->master_buffer = (uint8_t *)calloc(1, PTY_BUFF_SIZE);
    pair->slave_buffer  = (uint8_t *)calloc(1, PTY_BUFF_SIZE);
    pair->id            = pty_id_alloc();
    pair->master_fds    = 1;
    pair->lock          = SPIN_INIT;

    if (pair->id < 0 || pair->master_buffer == NULL || pair->slave_buffer == NULL || pts_root == NULL)
    {
        if (pair->id >= 0) pty_id_free(pair->id);
        free(pair->master_buffer);
        free(pair->slave_buffer);
        free(pair);
        return;
    }

    char slave_name[16];
    snprintf(slave_name, sizeof(slave_name), "%d", pair->id);

    vfs_node_t old_slave = vfs_do_search(pts_root, slave_name);
    if (old_slave != NULL)
    {
        vfs_child_lock();
        pts_root->child = list_delete(pts_root->child, old_slave);
        vfs_child_unlock();
        old_slave->parent = NULL;
        old_slave->handle = NULL;
    }

    vfs_node_t slave = vfs_node_alloc(pts_root, slave_name);
    if (slave == NULL)
    {
        pty_id_free(pair->id);
        free(pair->master_buffer);
        free(pair->slave_buffer);
        free(pair);
        return;
    }

    slave->type        = file_pts;
    slave->fsid        = pts_fsid;
    slave->handle      = NULL;
    slave->mode        = 0666;
    slave->size        = (uint64_t)-1;
    pair->slave_node   = slave;
    pair->master_node  = node;

    vfs_node_t dev_node = node->parent;
    if (dev_node != NULL)
    {
        vfs_child_lock();
        dev_node->child = list_delete(dev_node->child, node);
        vfs_child_unlock();
        node->parent = NULL;

        vfs_node_t new_ptmx = vfs_node_alloc(dev_node, "ptmx");
        if (new_ptmx != NULL)
        {
            new_ptmx->type   = file_ptmx;
            new_ptmx->fsid   = ptmx_fsid;
            new_ptmx->handle = NULL;
            new_ptmx->mode   = 0666;
            new_ptmx->size   = (uint64_t)-1;
        }
    }

    node->handle       = pair;
    node->type         = file_ptmx;
    node->fsid         = ptmx_fsid;
    node->refcount     = 1;
    node->size         = (uint64_t)-1;
    node->mode         = 0666;

    spin_lock(&pty_global_lock);
    llist_append(&ptmx_list_head, &pair->list_node);
    spin_unlock(&pty_global_lock);
}

static void ptmx_close(void *handle)
{
    pty_handle_t *pair = (pty_handle_t *)handle;
    if (pair == NULL) return;

    spin_lock(&pair->lock);
    if (pair->master_fds > 0) pair->master_fds--;
    bool destroy = pair->master_fds == 0 && pair->slave_fds == 0;
    spin_unlock(&pair->lock);

    if (destroy)
    {
        spin_lock(&pty_global_lock);
        pty_pair_destroy_locked(pair);
        spin_unlock(&pty_global_lock);
    }
}

static size_t ptmx_read(void *file, void *addr, size_t offset, size_t size)
{
    UNUSED(offset);
    pty_handle_t *pair = (pty_handle_t *)file;
    if (pair == NULL || addr == NULL) return VFS_STATUS_FAILED;

    while (true)
    {
        spin_lock(&pair->lock);
        if (ptmx_data_available(pair) > 0)
        {
            spin_unlock(&pair->lock);
            break;
        }
        if (pair->slave_fds == 0)
        {
            spin_unlock(&pair->lock);
            return 0;
        }
        spin_unlock(&pair->lock);
        scheduler_yield();
    }

    spin_lock(&pair->lock);
    size_t to_copy = MIN(size, ptmx_data_available(pair));
    memcpy(addr, pair->master_buffer, to_copy);
    memmove(pair->master_buffer, pair->master_buffer + to_copy, pair->ptr_master - to_copy);
    pair->ptr_master -= to_copy;
    spin_unlock(&pair->lock);
    return to_copy;
}

static size_t ptmx_write(void *file, const void *addr, size_t offset, size_t size)
{
    UNUSED(offset);
    pty_handle_t *pair = (pty_handle_t *)file;
    if (pair == NULL || addr == NULL) return VFS_STATUS_FAILED;
    if (size == 0) return 0;

    while (true)
    {
        spin_lock(&pair->lock);
        if (pair->ptr_slave < PTY_BUFF_SIZE)
        {
            spin_unlock(&pair->lock);
            break;
        }
        spin_unlock(&pair->lock);
        scheduler_yield();
    }

    spin_lock(&pair->lock);
    size_t writable = MIN(size, PTY_BUFF_SIZE - pair->ptr_slave);
    memcpy(pair->slave_buffer + pair->ptr_slave, addr, writable);
    if (pair->term.c_iflag & ICRNL)
    {
        for (size_t i = 0; i < writable; i++)
        {
            if (pair->slave_buffer[pair->ptr_slave + i] == '\r')
                pair->slave_buffer[pair->ptr_slave + i] = '\n';
        }
    }
    pair->ptr_slave += writable;
    spin_unlock(&pair->lock);
    return writable;
}

static errno_t ptmx_ioctl(void *handle, size_t request, void *arg)
{
    pty_handle_t *pair = (pty_handle_t *)handle;
    if (pair == NULL) return -ENOTTY;

    uint32_t req = (uint32_t)request;
    errno_t ret = EOK;
    spin_lock(&pair->lock);
    switch (req)
    {
    case TIOCSPTLCK:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        pair->locked = (*(int *)arg) != 0;
        break;
    case TIOCGPTN:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        *(int *)arg = pair->id;
        break;
    case TIOCGPTLCK:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        *(int *)arg = pair->locked ? 1 : 0;
        break;
    case TIOCGWINSZ:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        memcpy(arg, &pair->win, sizeof(pair->win));
        break;
    case TIOCSWINSZ:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        memcpy(&pair->win, arg, sizeof(pair->win));
        break;
    default:
        ret = -ENOTTY;
        break;
    }
    spin_unlock(&pair->lock);
    return ret;
}

static errno_t ptmx_poll(void *file, size_t events)
{
    pty_handle_t *pair = (pty_handle_t *)file;
    if (pair == NULL) return VFS_STATUS_FAILED;

    int revents = 0;
    spin_lock(&pair->lock);
    if (ptmx_data_available(pair) > 0 && (events & EPOLLIN)) revents |= EPOLLIN;
    if (pair->ptr_slave < PTY_BUFF_SIZE && (events & EPOLLOUT)) revents |= EPOLLOUT;
    if (pair->slave_fds == 0) revents |= EPOLLHUP;
    spin_unlock(&pair->lock);
    return revents;
}

static vfs_node_t ptmx_dup(vfs_node_t node)
{
    pty_handle_t *pair = (pty_handle_t *)node->handle;
    if (pair != NULL)
    {
        spin_lock(&pair->lock);
        pair->master_fds++;
        spin_unlock(&pair->lock);
    }
    return node;
}

static void pts_open(void *parent, const char *name, vfs_node_t node)
{
    UNUSED(parent);

    int id;
    if (pty_str_to_int(name, &id) < 0) return;

    spin_lock(&pty_global_lock);
    pty_handle_t *pair = pty_find_locked(id);
    if (pair == NULL || pair->locked)
    {
        spin_unlock(&pty_global_lock);
        return;
    }

    spin_lock(&pair->lock);
    pair->slave_fds++;
    node->handle = pair;
    node->type   = file_pts;
    node->fsid   = pts_fsid;
    node->size   = (uint64_t)-1;
    node->mode   = 0666;
    if (pair->slave_node == NULL) pair->slave_node = node;
    spin_unlock(&pair->lock);
    spin_unlock(&pty_global_lock);
}

static void pts_close(void *handle)
{
    pty_handle_t *pair = (pty_handle_t *)handle;
    if (pair == NULL) return;

    spin_lock(&pair->lock);
    if (pair->slave_fds > 0) pair->slave_fds--;
    bool destroy = pair->master_fds == 0 && pair->slave_fds == 0;
    spin_unlock(&pair->lock);

    if (destroy)
    {
        spin_lock(&pty_global_lock);
        pty_pair_destroy_locked(pair);
        spin_unlock(&pty_global_lock);
    }
}

static size_t pts_read(void *file, void *addr, size_t offset, size_t size)
{
    UNUSED(offset);
    pty_handle_t *pair = (pty_handle_t *)file;
    if (pair == NULL || addr == NULL) return VFS_STATUS_FAILED;

    while (true)
    {
        spin_lock(&pair->lock);
        if (pts_data_available(pair) > 0)
        {
            spin_unlock(&pair->lock);
            break;
        }
        if (pair->master_fds == 0)
        {
            spin_unlock(&pair->lock);
            return 0;
        }
        spin_unlock(&pair->lock);
        scheduler_yield();
    }

    spin_lock(&pair->lock);
    size_t to_copy = MIN(size, pts_data_available(pair));
    memcpy(addr, pair->slave_buffer, to_copy);
    memmove(pair->slave_buffer, pair->slave_buffer + to_copy, pair->ptr_slave - to_copy);
    pair->ptr_slave -= to_copy;
    spin_unlock(&pair->lock);
    return to_copy;
}

static size_t pts_write_inner(pty_handle_t *pair, const uint8_t *in, size_t size)
{
    size_t written = 0;
    bool translate = (pair->term.c_oflag & OPOST) && (pair->term.c_oflag & ONLCR);

    for (size_t i = 0; i < size; i++)
    {
        uint8_t ch = in[i];
        if (translate && ch == '\n')
        {
            if (pair->ptr_master + 2 > PTY_BUFF_SIZE) break;
            pair->master_buffer[pair->ptr_master++] = '\r';
            pair->master_buffer[pair->ptr_master++] = '\n';
        }
        else
        {
            if (pair->ptr_master + 1 > PTY_BUFF_SIZE) break;
            pair->master_buffer[pair->ptr_master++] = ch;
        }
        written++;
    }

    return written;
}

static size_t pts_write(void *file, const void *addr, size_t offset, size_t size)
{
    UNUSED(offset);
    pty_handle_t *pair = (pty_handle_t *)file;
    if (pair == NULL || addr == NULL) return VFS_STATUS_FAILED;
    if (size == 0) return 0;
    bool translate = (pair->term.c_oflag & OPOST) && (pair->term.c_oflag & ONLCR);
    size_t min_space = translate ? 2 : 1;

    while (true)
    {
        spin_lock(&pair->lock);
        if (pair->master_fds == 0)
        {
            spin_unlock(&pair->lock);
            return (size_t)-EIO;
        }
        if (PTY_BUFF_SIZE - pair->ptr_master >= min_space)
        {
            spin_unlock(&pair->lock);
            break;
        }
        spin_unlock(&pair->lock);
        scheduler_yield();
    }

    spin_lock(&pair->lock);
    size_t written = pts_write_inner(pair, (const uint8_t *)addr, size);
    spin_unlock(&pair->lock);
    return written;
}

static errno_t pts_ioctl(void *handle, size_t request, void *arg)
{
    pty_handle_t *pair = (pty_handle_t *)handle;
    if (pair == NULL) return -ENOTTY;

    uint32_t req = (uint32_t)request;
    errno_t ret = EOK;
    spin_lock(&pair->lock);
    switch (req)
    {
    case TIOCGWINSZ:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        memcpy(arg, &pair->win, sizeof(pair->win));
        break;
    case TIOCSWINSZ:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        memcpy(&pair->win, arg, sizeof(pair->win));
        break;
    case TIOCSCTTY:
        pts_ctrl_assign(pair);
        break;
    case TCGETS:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        memcpy(arg, &pair->term, sizeof(pair->term));
        break;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        memcpy(&pair->term, arg, sizeof(pair->term));
        break;
    case TIOCGPGRP:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        *(int *)arg = pair->ctrl_pgid != 0 ? pair->ctrl_pgid : get_current_task()->parent_group->pid;
        break;
    case TIOCSPGRP:
        if (arg != NULL) pair->ctrl_pgid = *(int *)arg;
        break;
    case KDGKBMODE:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        *(int *)arg = pair->tty_kbmode;
        break;
    case KDSKBMODE:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        pair->tty_kbmode = *(int *)arg;
        break;
    case VT_SETMODE:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        memcpy(&pair->vt_mode, arg, sizeof(pair->vt_mode));
        break;
    case VT_GETMODE:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        memcpy(arg, &pair->vt_mode, sizeof(pair->vt_mode));
        break;
    case VT_ACTIVATE:
    case VT_WAITACTIVE:
    case TIOCNOTTY:
        break;
    case VT_GETSTATE:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        ((struct vt_state *)arg)->v_active = 2;
        ((struct vt_state *)arg)->v_state  = 0;
        break;
    case VT_OPENQRY:
        if (arg == NULL)
        {
            ret = -EFAULT;
            break;
        }
        *(int *)arg = 2;
        break;
    default:
        ret = -ENOTTY;
        break;
    }
    spin_unlock(&pair->lock);
    return ret;
}

static errno_t pts_poll(void *file, size_t events)
{
    pty_handle_t *pair = (pty_handle_t *)file;
    if (pair == NULL) return VFS_STATUS_FAILED;

    int revents = 0;
    spin_lock(&pair->lock);
    if ((pair->master_fds == 0 || pts_data_available(pair) > 0) && (events & EPOLLIN)) revents |= EPOLLIN;
    if (pair->master_fds == 0) revents |= EPOLLHUP;
    if (pair->ptr_master < PTY_BUFF_SIZE && (events & EPOLLOUT)) revents |= EPOLLOUT;
    spin_unlock(&pair->lock);
    return revents;
}

static vfs_node_t pts_dup(vfs_node_t node)
{
    pty_handle_t *pair = (pty_handle_t *)node->handle;
    if (pair != NULL)
    {
        spin_lock(&pair->lock);
        pair->slave_fds++;
        spin_unlock(&pair->lock);
    }
    return node;
}

static errno_t pty_stat(void *handle, vfs_node_t node)
{
    UNUSED(handle);
    node->size = (uint64_t)-1;
    return EOK;
}

static struct vfs_callback ptmx_callbacks = {
    .mount    = (vfs_mount_t)pty_dummy,
    .unmount  = (vfs_unmount_t)pty_dummy,
    .open     = ptmx_open,
    .close    = ptmx_close,
    .read     = ptmx_read,
    .write    = ptmx_write,
    .readlink = (vfs_readlink_t)pty_dummy,
    .mkdir    = (vfs_mk_t)pty_dummy,
    .mkfile   = (vfs_mk_t)pty_dummy,
    .link     = (vfs_mk_t)pty_dummy,
    .symlink  = (vfs_mk_t)pty_dummy,
    .stat     = pty_stat,
    .ioctl    = ptmx_ioctl,
    .dup      = ptmx_dup,
    .poll     = ptmx_poll,
    .map      = (vfs_mapfile_t)pty_dummy,
    .resize   = (vfs_resize_t)pty_dummy,
    .del      = (vfs_del_t)pty_dummy,
    .rename   = (vfs_rename_t)pty_dummy,
};

static struct vfs_callback pts_callbacks = {
    .mount    = (vfs_mount_t)pty_dummy,
    .unmount  = (vfs_unmount_t)pty_dummy,
    .open     = pts_open,
    .close    = pts_close,
    .read     = pts_read,
    .write    = pts_write,
    .readlink = (vfs_readlink_t)pty_dummy,
    .mkdir    = (vfs_mk_t)pty_dummy,
    .mkfile   = (vfs_mk_t)pty_dummy,
    .link     = (vfs_mk_t)pty_dummy,
    .symlink  = (vfs_mk_t)pty_dummy,
    .stat     = pty_stat,
    .ioctl    = pts_ioctl,
    .dup      = pts_dup,
    .poll     = pts_poll,
    .map      = (vfs_mapfile_t)pty_dummy,
    .resize   = (vfs_resize_t)pty_dummy,
    .del      = (vfs_del_t)pty_dummy,
    .rename   = (vfs_rename_t)pty_dummy,
};

void pty_init()
{
    if (pty_allocator != NULL) return;

    pty_allocator = id_allocator_create(MAX_PTY_DEVICE);
    llist_init_head(&ptmx_list_head);

    ptmx_fsid = vfs_regist("ptyfs_master", &ptmx_callbacks, 0, 0x50544d58);
    pts_fsid  = vfs_regist("ptyfs_slave", &pts_callbacks, 0, 0x50545346);
    if (pty_allocator == NULL || ptmx_fsid == VFS_STATUS_FAILED || pts_fsid == VFS_STATUS_FAILED)
    {
        write_serial_fmt("pty: init failed\n");
        return;
    }

    vfs_node_t dev = vfs_open("/dev");
    if (dev == NULL)
    {
        write_serial_fmt("pty: /dev not ready\n");
        return;
    }

    vfs_node_t ptmx = vfs_do_search(dev, "ptmx");
    if (ptmx == NULL) ptmx = vfs_node_alloc(dev, "ptmx");
    if (ptmx != NULL)
    {
        ptmx->type   = file_ptmx;
        ptmx->fsid   = ptmx_fsid;
        ptmx->handle = NULL;
        ptmx->mode   = 0666;
        ptmx->size   = (uint64_t)-1;
    }

    vfs_mkdir("/dev/pts");
    pts_root = vfs_open("/dev/pts");
    if (pts_root != NULL)
    {
        pts_root->type = file_dir;
    }

    vfs_close(dev);
    write_serial_fmt("pty: initialized\n");
}
