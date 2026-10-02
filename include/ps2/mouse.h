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
extern "C" void set_mouse_wheel_reverse(bool enabled);
extern "C" int  mouse_transform_wheel_delta(int wheel);
extern "C" void mouse_set_settings(uint64_t pointer_speed_percent, uint64_t double_click_ms);
extern "C" uint64_t mouse_get_pointer_speed_percent();
extern "C" uint64_t mouse_get_double_click_speed_ms();
