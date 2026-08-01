#pragma once

#include <cpu/lock.h>
#include <fs/vfs/vfs.h>
#include <id_alloc.h>
#include <llist.h>
#include <tty.h>

#define PTY_BUFF_SIZE 4096
#define MAX_PTY_DEVICE 256

typedef struct pty_handle {
    int                 id;
    vfs_node_t          master_node;
    vfs_node_t          slave_node;
    struct termios      term;
    struct winsize      win;
    struct vt_mode      vt_mode;
    spin_t              lock;
    int                 ctrl_pgid;
    int                 master_fds;
    int                 slave_fds;
    int                 tty_kbmode;
    size_t              ptr_master;
    size_t              ptr_slave;
    bool                locked;
    uint8_t            *master_buffer;
    uint8_t            *slave_buffer;
    struct llist_header list_node;
} pty_handle_t;

void pty_init();

