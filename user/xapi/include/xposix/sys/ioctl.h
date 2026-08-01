#pragma once

#include "types.h"

#define TIOCSWINSZ 0x5414

struct winsize
{
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif
