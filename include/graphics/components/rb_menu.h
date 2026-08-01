#pragma once

#include <stdint.h>

typedef struct 
{
	uint64_t 	CRLid;
	char	    text[64];
} RightMenuItem;

typedef struct 
{
	uint64_t 	CRLid;
	char	   *text;
} RightMenuItem_user;

typedef struct rb_menu_regt *rb_menu_regt_p;

struct rb_menu_regt
{
    RightMenuItem  *items;
    uint64_t        count;
    int             x;
    int             y;
    bool            can_r;
    rb_menu_regt_p  prev;
    rb_menu_regt_p  next;
};

void register_right_rb_button_menu(WINDOWLSP window, RightMenuItem_user *items, uint64_t count);
void unregister_right_rb_button_menu_components(WINDOWLSP win);
void close_menu(WINDOWLSP win, int x, int y);
void process_right_button_click_event(WINDOWLSP win, int x, int y);
