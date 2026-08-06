#pragma once
#ifndef _KEYBOARD_H_
#    define _KEYBOARD_H_

#    include <proto.hpp>

#define KB_BUF_SIZE 128

#define MOUSE_BBIT 0x01
#define MOUSE_ABIT 0x02

#define PORT_KB_DATA 0x60
#define PORT_KB_STATUS 0x64
#define PORT_KB_CMD 0x64

#define PS2_CMD_PORT  0x64
#define PS2_DATA_PORT 0x60

#define KB_STATUS_IBF 0x02
#define KB_STATUS_OBF 0x01
#define KB_INIT_MODE  0x47

#define KBCMD_WRITE_CMD 0x60
#define KBCMD_READ_CMD  0x20

#define KB_EN_MOUSE_INTFACE 0xa8
#define KB_SEND2MOUSE       0xd4
#define MOUSE_EN            0xf4

#define SCANCODE_ENTER   28
#define SCANCODE_BACK    14
#define SCANCODE_SHIFT_L 42
#define SCANCODE_SHIFT_R 0x36
#define SCANCODE_CAPS    58
#define SCANCODE_UP      0x48

#define KBSTATUS_IBF 0x02
#define KBSTATUS_OBF 0x01

#define CHARACTER_ENTER '\n'
#define CHARACTER_BACK  '\b'

enum special_key_code
{
    KEY_ESC = 128,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_ENTER,
    KEY_CAPS,
    KEY_SHIFT,
    KEY_CTRL,
    KEY_ALT,
    KEY_SPACE,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
    KEY_NUML,
    KEY_SCROLL,
    KEY_HOME,
    KEY_UP,
    KEY_PAGE_UP,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_END,
    KEY_DOWN,
    KEY_PAGE_DOWN,
    KEY_INSERT,
    KEY_DELETE
};
#    define wait_KB_write() while (inb(PORT_KB_STATUS) & KBSTATUS_IBF)

#    define wait_KB_read() while (inb(PORT_KB_STATUS) & KBSTATUS_OBF)

struct keyboard_buf
{
    uint8_t *p_head;
    uint8_t *p_tail;
    int      count;
    bool     ctrl;
    bool     shift;
    bool     alt;
    bool     win;
    bool     caps;
    uint8_t  buf[KB_BUF_SIZE];
};

void wait_ps2_write();
void wait_ps2_read();

uint8_t get_keyboard_input();
void keyboard_init();

#endif
