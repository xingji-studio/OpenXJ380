#pragma once

#include "time.h"
#include "types.h"

#define FD_SETSIZE 1024

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / 8 / sizeof(unsigned long)];
} fd_set;

#define FD_ZERO(set) do { for (size_t _i = 0; _i < sizeof(fd_set) / sizeof(unsigned long); _i++) (set)->fds_bits[_i] = 0; } while (0)
#define FD_SET(fd, set) ((set)->fds_bits[(fd) / (8 * sizeof(unsigned long))] |= (1UL << ((fd) % (8 * sizeof(unsigned long)))))
#define FD_CLR(fd, set) ((set)->fds_bits[(fd) / (8 * sizeof(unsigned long))] &= ~(1UL << ((fd) % (8 * sizeof(unsigned long)))))
#define FD_ISSET(fd, set) (((set)->fds_bits[(fd) / (8 * sizeof(unsigned long))] & (1UL << ((fd) % (8 * sizeof(unsigned long))))) != 0)

#ifdef __cplusplus
extern "C" {
#endif

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
            const struct timespec *timeout, const void *sigmask);

#ifdef __cplusplus
}
#endif
