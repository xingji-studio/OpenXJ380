#pragma once

#include <proto.hpp>
#define PIPE_BUFF 8192

typedef struct task_block_list {
    struct task_block_list *next;
    tcb_t                   thread;
} task_block_list_t;

typedef struct pipe_info {
    uint32_t ptr;
    char    *buf;
    int      assigned;

    int write_fds;
    int read_fds;

    spin_t lock;

    task_block_list_t blocking_read;
    task_block_list_t blocking_write;

    struct pipe_specific *read_spec;
    struct pipe_specific *write_spec;
} pipe_info_t;

typedef struct pipe_specific pipe_specific_t;
struct pipe_specific {
    bool         write;
    pipe_info_t *info;
    vfs_node_t   node;
};

void pipefs_setup();
