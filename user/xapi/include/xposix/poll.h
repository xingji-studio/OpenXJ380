#pragma once

#include "sys/types.h"

#define POLLIN  0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004

#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

#ifdef __cplusplus
extern "C" {
#endif

struct pollfd {
    int   fd;
    short events;
    short revents;
};

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#ifdef __cplusplus
}
#endif
