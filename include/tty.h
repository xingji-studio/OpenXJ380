#pragma once

#include <stdint.h>

#define NCCS 20

#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IUCLC   0001000
#define IXON    0002000
#define IXANY   0004000
#define IXOFF   0010000
#define IMAXBEL 0020000
#define IUTF8   0040000

#define OPOST 0000001

#define CSIZE  0000060
#define CS5    0000000
#define CS6    0000020
#define CS7    0000040
#define CS8    0000060
#define CSTOPB 0000100
#define CREAD  0000200
#define PARENB 0000400
#define PARODD 0001000
#define HUPCL  0002000
#define CLOCAL 0004000

#define ISIG   0000001
#define ICANON 0000002
#define ECHO   0000010
#define ECHOE  0000020
#define ECHOK  0000040
#define ECHONL 0000100
#define NOFLSH 0000200
#define TOSTOP 0000400
#define IEXTEN 0100000

#define KDGETMODE   0x4B3B
#define KDSETMODE   0x4B3A
#define KD_TEXT     0x00
#define KD_GRAPHICS 0x01

#define KDGKBMODE   0x4B44
#define KDSKBMODE   0x4B45
#define K_RAW       0x00
#define K_XLATE     0x01
#define K_MEDIUMRAW 0x02
#define K_UNICODE   0x03

#define VT_OPENQRY 0x5600
#define VT_GETMODE 0x5601
#define VT_SETMODE 0x5602

#define VT_GETSTATE 0x5603
#define VT_SENDSIG  0x5604

#define VT_ACTIVATE   0x5606
#define VT_WAITACTIVE 0x5607

#define VT_AUTO    0x00
#define VT_PROCESS 0x01

#define B38400 0x1000

struct vt_state
{
    uint16_t v_active;
    uint16_t v_state;
};

struct vt_mode
{
    char  mode;
    char  waitv;
    short relsig;
    short acqsig;
    short frsig;
};

typedef struct termios
{
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
} termios_t;

typedef struct tty_virtual_device
{
    void (*print)(struct tty_virtual_device *res, const char *string);
    void (*putchar)(struct tty_virtual_device *res, int c);
    void (*flush)(struct tty_virtual_device *res);

    uint64_t volatile *video_ram;
    uint64_t           width;
    uint64_t           height;
    termios_t          termios;
    bool               is_sigterm;
} tty_t;

struct winsize
{
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

void   init_tty();
tty_t *alloc_default_tty();
void   free_tty(tty_t *tty);
void   build_tty_device();
tty_t *get_default_tty();
