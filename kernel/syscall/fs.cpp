// #include "../../include/include.h"

// int sys_read(uint32_t fd, uint8_t *buf, size_t count)
// {
//     if (fd >= MAX_FD_NUM)
//     {
//         return -1;
//     }

//     if (fd == 0)
//     {
//         return 0;
//     }
//     if (fd == 1 || fd == 2)
//     {
//         return 0;
//     }
//     struct VFile *file = current_task->parent_group->fds[fd];
//     VNode *node = file->file;
//     if (node->ops->read == NULL && !file->readable)
//     {
//         return -1;
//     }
//     return node->ops->read(node, buf, file->offset, count);
// }

// int sys_write(uint32_t fd, uint8_t *buf, size_t count)
// {
//     if (fd >= MAX_FD_NUM)
//     {
//         return -1;
//     }

//     if (fd == 0)
//     {
//         return 0;
//     }
//     if (fd == 1 || fd == 2)
//     {
//         write_serial_string((const char *)buf);
//         return strlen((const char *)buf);
//     }
//     struct VFile *file = current_task->parent_group->fds[fd];
//     VNode *node = file->file;
//     if (node->ops->write == NULL && !file->writeable)
//     {
//         return -1;
//     }
//     return node->ops->write(node, buf, file->offset, count);
// }

// int sys_open(const char *pathname, int flags, int mode)
// {
//     VNode *node = path_walk(current_task->cwd_node, pathname);
//     if (node == NULL)
//     {
//         return -1;
//     }
//     int fd = current_task->fd_count++;
//     struct VFile *file = current_task->parent_group->fds[fd];

//     if (current_task->fd_count >= MAX_FD_NUM)
//         current_task->fd_count = 3;

//     file->file = node;
//     // TODO: 根据flags和mode设置readable和writeable
//     file->readable = 1;
//     file->writeable = 1;
//     return fd;
// }

// int sys_close(uint32_t fd)
// {
//     if (fd >= MAX_FD_NUM)
//     {
//         return -1;
//     }
//     current_task->parent_group->fds[fd] = NULL;
//     return 0;
// }

// int sys_fstat(uint32_t fd, struct stat *stat)
// {
//     if (fd == 0 || fd == 1 || fd == 2)
//     {
//         return 0;
//     }
//     if (fd >= MAX_FD_NUM)
//     {
//         return -1;
//     }
//     struct VFile *file = current_task->parent_group->fds[fd];
//     VNode *node = file->file;
//     if (node->ops->stat == NULL)
//     {
//         return -1;
//     }
//     xmemset(stat, 0, sizeof(struct stat));
//     return node->ops->stat(node, stat);
// }

// int sys_readv(uint32_t fd, struct iovec *iovec, size_t count)
// {
//     if ((uint64_t)iovec == 0)
//     {
//         return -EINVAL;
//     }

//     uint64_t len = 0;

//     for (uint64_t i = 0; i < count; i++)
//     {
//         size_t iov_len = iovec[i].len;
//         len += iov_len;
//     }

//     uint8_t *buf = (uint8_t *)malloc(len + 1);
//     if (!buf)
//     {
//         return -ENOMEM;
//     }
//     xmemset(buf, 0, len + 1);

//     int ret = sys_read(fd, buf, len);

//     uint8_t *ptr = buf;

//     for (uint64_t i = 0; i < count; i++)
//     {
//         uint8_t *iov_base = iovec[i].iov_base;
//         size_t iov_len = iovec[i].len;
//         if (iov_len == 0)
//         {
//             continue;
//         }
//         xmemcpy(iov_base, ptr, iov_len);
//         ptr += iov_len;
//     }

//     free(buf);
//     return ret;
// }

// int sys_writev(uint32_t fd, struct iovec *iovec, size_t count)
// {
//     if ((uint64_t)iovec == 0)
//     {
//         return -EINVAL;
//     }

//     uint64_t len = 0;

//     for (uint64_t i = 0; i < count; i++)
//     {
//         size_t iov_len = iovec[i].len;
//         len += iov_len;
//     }

//     uint8_t *buf = (uint8_t *)malloc(len + 1);
//     if (!buf)
//     {
//         return -ENOMEM;
//     }
//     xmemset(buf, 0, len + 1);
//     uint8_t *ptr = buf;

//     for (uint64_t i = 0; i < count; i++)
//     {
//         uint8_t *iov_base = iovec[i].iov_base;
//         size_t iov_len = iovec[i].len;
//         if (iov_len == 0)
//         {
//             continue;
//         }
//         xmemcpy(ptr, iov_base, iov_len);
//         ptr += iov_len;
//     }

//     int ret = sys_write(fd, buf, len);
//     free(buf);
//     return ret;
// }
