#pragma once

#include "types.h"

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

#ifdef __cplusplus
extern "C" {
#endif

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#ifdef __cplusplus
}
#endif
