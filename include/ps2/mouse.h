#pragma once
#ifndef _MOUSE_H_
#    define _MOUSE_H_

#    include <graphics/sheet.h>
#    include <proto.hpp>

typedef struct
{
    uint8_t   buf[4], phase;

    int       x, y, btn;
    int       last_btn;
    int       last_mouse_x;
    int       last_mouse_y;
    uint8_t   pending_btn_down;
    uint8_t   pending_btn_up;
    char      roll;
    bool      left;
    bool      center;
    bool      right;

    bool      win_move_lock;
    bool      win_resize_lock;
    bool      need_flush;

    int       win_x_offset;
    int       win_y_offset;
    int       move_start_mouse_x;
    int       move_start_mouse_y;
    int       move_start_bx;
    int       move_start_by;
    uint32_t  move_start_width;
    uint32_t  move_start_height;
    bool      win_move_dragged;
    bool      win_move_pending;
    int       pending_win_x;
    int       pending_win_y;
    uint64_t  last_win_move_ns;
    int       resize_start_mouse_x;
    int       resize_start_mouse_y;
    int       resize_start_bx;
    int       resize_start_by;
    uint32_t  resize_start_width;
    uint32_t  resize_start_height;
    uint8_t   resize_edges;
    bool      win_resize_pending;
    int       pending_resize_bx;
    int       pending_resize_by;
    uint32_t  pending_resize_width;
    uint32_t  pending_resize_height;
    uint64_t  last_win_resize_ns;
    uint8_t   cursor_shape;

    bool      first_click;
    bool      double_click;
    uint64_t  click_time;
    bool      left_release_pending;

    SHEET    *sht_now;
    WINDOWLSP left_release_window;

    int       scroll;
} mouse_dec;

typedef enum mouse_type {
    Standard   = 0,
    OnlyScroll = 1,
    FiveButton = 2,
} MouseType;

#    define KB_SEND2MOUSE 0xd4
#    define MOUSE_EN      0xf4

#    define KB_EN_MOUSE_INTFACE 0xa8

#endif
