#pragma once

#define POLLIN  0x0001 // 有数据可读
#define POLLPRI 0x0002 // 有紧急数据可读（如 socket 的带外数据）
#define POLLOUT 0x0004 // 写操作不会阻塞（可写）

#define POLLERR  0x0008 // 错误（不需要设置，由内核返回）
#define POLLHUP  0x0010 // 挂起（对端关闭）
#define POLLNVAL 0x0020 // fd 无效（文件描述符非法）

#define EPOLLIN        0x001
#define EPOLLPRI       0x002
#define EPOLLOUT       0x004
#define EPOLLRDNORM    0x040
#define EPOLLNVAL      0x020
#define EPOLLRDBAND    0x080
#define EPOLLWRNORM    0x100
#define EPOLLWRBAND    0x200
#define EPOLLMSG       0x400
#define EPOLLERR       0x008
#define EPOLLHUP       0x010
#define EPOLLRDHUP     0x2000
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLWAKEUP    (1U << 29)
#define EPOLLONESHOT   (1U << 30)
#define EPOLLET        (1U << 31)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_CLOEXEC 02000000

#include "krlibc.h"
#include "lock_queue.h"

struct pollfd {
    int   fd;
    short events;
    short revents;
};

typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
} __attribute__((packed));

typedef struct epoll_watch {
    int                fd;
    struct epoll_event event;
} epoll_watch_t;

typedef struct epoll_file {
    lock_queue *watches;
} epoll_file_t;

struct pollfd *select_add(struct pollfd **comp, size_t *compIndex, size_t *complength, int fd,
                          int events);
bool           select_bitmap(const uint8_t *map, int index);
void           select_bitmap_set(uint8_t *map, int index);
uint32_t       poll_to_epoll_comp(uint32_t poll_events);
uint32_t       epoll_to_poll_comp(uint32_t epoll_events);
