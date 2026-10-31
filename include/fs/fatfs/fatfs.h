#pragma once
#include "proto.hpp"
struct statfs;

void     fatfs_init();
// Task context, before quiescent FD cleanup and before a terminal TCB is freed.
void     fatfs_quiescent_exit();
errno_t  fatfs_statfs(vfs_node_t node, struct statfs *buf);
int      fatfs_format_node(vfs_node_t node);
