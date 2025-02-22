#include "include.h"
//键盘按键表
PUBLIC u32 keymap[NR_SCAN_CODES * MAP_COLS] = {
    0,            0,           0,
    ESC,          ESC,         0,
    '1',          '!',         0,
    '2',          '@',         0,
    '3',          '#',         0,
    '4',          '$',         0,
    '5',          '%',         0,
    '6',          '^',         0,
    '7',          '&',         0,
    '8',          '*',         0,
    '9',          '(',         0,
    '0',          ')',         0,
    '-',          '_',         0,
    '=',          '+',         0,
    BACKSPACE,    BACKSPACE,   0,
    TAB,          TAB,         0,
    'q',          'Q',         0,
    'w',          'W',         0,
    'e',          'E',         0,
    'r',          'R',         0,
    't',          'T',         0,
    'y',          'Y',         0,
    'u',          'U',         0,
    'i',          'I',         0,
    'o',          'O',         0,
    'p',          'P',         0,
    '[',          '{',         0,
    ']',          '}',         0,
    ENTER,        ENTER,       PAD_ENTER,
    CTRL_L,       CTRL_L,      CTRL_R,
    'a',          'A',         0,
    's',          'S',         0,
    'd',          'D',         0,
    'f',          'F',         0,
    'g',          'G',         0,
    'h',          'H',         0,
    'j',          'J',         0,
    'k',          'K',         0,
    'l',          'L',         0,
    ';',          ':',         0,
    '\'',         '"',         0,
    '`',          '~',         0,
    SHIFT_L,      SHIFT_L,     0,
    '\\',         '|',         0,
    'z',          'Z',         0,
    'x',          'X',         0,
    'c',          'C',         0,
    'v',          'V',         0,
    'b',          'B',         0,
    'n',          'N',         0,
    'm',          'M',         0,
    ',',          '<',         0,
    '.',          '>',         0,
    '/',          '?',         PAD_SLASH,
    SHIFT_R,      SHIFT_R,     0,
    '*',          '*',         0,
    ALT_L,        ALT_L,       ALT_R,
    ' ',          ' ',         0,
    CAPS_LOCK,    CAPS_LOCK,   0,
    F1,           F1,          0,
    F2,           F2,          0,
    F3,           F3,          0,
    F4,           F4,          0,
    F5,           F5,          0,
    F6,           F6,          0,
    F7,           F7,          0,
    F8,           F8,          0,
    F9,           F9,          0,
    F10,          F10,         0,
    NUM_LOCK,     NUM_LOCK,    0,
    SCROLL_LOCK,  SCROLL_LOCK, 0,
    PAD_HOME,     '7',         HOME,
    PAD_UP,       '8',         UP,
    PAD_PAGEUP,   '9',         PAGEUP,
    PAD_MINUS,    '-',         0,
    PAD_LEFT,     '4',         LEFT,
    PAD_MID,      '5',         0,
    PAD_RIGHT,    '6',         RIGHT,
    PAD_PLUS,     '+',         0,
    PAD_END,      '1',         END,
    PAD_DOWN,     '2',         DOWN,
    PAD_PAGEDOWN, '3',         PAGEDOWN,
    PAD_INS,      '0',         INSERT,
    PAD_DOT,      '.',         DELETE,
    0,            0,           0,
    0,            0,           0,
    0,            0,           0,
    F11,          F11,         0,
    F12,          F12,         0,
    0,            0,           0,
    0,            0,           0,
    0,            0,           GUI_L,
    0,            0,           GUI_R,
    0,            0,           APPS,
    0,            0,           0
};