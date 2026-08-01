#pragma once
#include "proto.hpp"
struct statfs;

void     fatfs_init();
errno_t  fatfs_statfs(vfs_node_t node, struct statfs *buf);
int      fatfs_format_node(vfs_node_t node);
