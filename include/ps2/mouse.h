#pragma once

#include <stdint.h>

typedef struct mouse_dec {
    uint8_t buf[4];
    uint8_t phase;
    uint8_t buttons;
    int     x;
    int     y;
    int     scroll;
} mouse_dec;

void mouse_init();
bool mousedecode(uint8_t data);
int  get_mouse_x();
int  get_mouse_y();
int  get_mouse_scroll();
void set_mouse_position(int x, int y);

extern "C" void mouse_inject_report(int dx, int dy, uint8_t buttons, int wheel);
