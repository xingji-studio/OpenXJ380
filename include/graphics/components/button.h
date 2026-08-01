#pragma once

#include <stdint.h>
#include <graphics/sheet.h>
#include <graphics/window/window.h>

typedef struct button_regt *button_regt_p;

struct button_regt
{
    SHEET  *obj_sheet;
    int     x1;
    int     y1;
    int     x2;
    int     y2;
    int     CRLid;
    bool    is_switch;
    int     switch_status;
    button_regt_p prev;
    button_regt_p next;
};

void register_button_components(SHEET *sheet, int x1, int y1, int x2, int y2, int CRLid);
void unregister_button_components(SHEET *sheet, int CRLid);
void unregister_switch_components(SHEET *sheet, int CRLid);
void set_switch_components(SHEET *sheet, int CRLid, int status);
void process_click_event(SHEET *current_sht, int x, int y);
void put_button_theme(WINDOWLS *windowls, int x, int y, char *str, int CRLid, bool underline);
void put_switch_theme(WINDOWLS *windowls, int x, int y, int status, int CRLid);
