#pragma once

#include <graphics/sheet.h>
#include <graphics/window/window.h>
#include <stdint.h>

#define SCROLL_BAR_SIZE      16
#define SCROLL_BAR_MIN_THUMB 8

typedef enum
{
    ScrollBarVertical,
    ScrollBarHorizontal
} ScrollBarDirection;

typedef struct scroll_bar_regt *scroll_bar_regt_p;

struct scroll_bar_regt
{
    SHEET             *obj_sheet;
    int                x1;
    int                y1;
    int                x2;
    int                y2;
    int                thumb_x1;
    int                thumb_y1;
    int                thumb_x2;
    int                thumb_y2;
    int                thumb_length;
    int                thumb_position;
    int                CRLid;
    ScrollBarDirection direction;
    scroll_bar_regt_p  prev;
    scroll_bar_regt_p  next;
};

void register_scroll_bar_components(SHEET *sheet, int x, int y, int length, int thumb_length, int CRLid,
                                    ScrollBarDirection direction);
void unregister_scroll_bar_components(SHEET *sheet, int CRLid);
void set_scroll_bar_position_components(SHEET *sheet, int CRLid, int position);
void process_scroll_bar_mouse_down_event(SHEET *current_sht, int x, int y);
void process_scroll_bar_drag_event(int x, int y);
void process_scroll_bar_mouse_up_event();
void process_scroll_bar_click_event(SHEET *current_sht, int x, int y);
void put_scroll_bar_theme(WINDOWLS *windowls, int x, int y, int length, int thumb_length, int CRLid,
                          ScrollBarDirection direction);
