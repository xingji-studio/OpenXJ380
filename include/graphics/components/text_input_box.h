#pragma once

#include <graphics/sheet.h>
#include <graphics/window/window.h>
#include <stdint.h>

#define TEXT_INPUT_BOX_DEFAULT_WIDTH 180
#define TEXT_INPUT_BOX_MIN_WIDTH     24
#define TEXT_INPUT_BOX_HEIGHT        24
#define TEXT_INPUT_BOX_MAX           255

typedef struct text_input_box_regt *text_input_box_regt_p;

struct text_input_box_regt
{
    SHEET  *obj_sheet;
    int     x1;
    int     y1;
    int     x2;
    int     y2;
    uint64_t id;
    char    text[TEXT_INPUT_BOX_MAX + 1];
    size_t  length;
    text_input_box_regt_p prev;
    text_input_box_regt_p next;
};

uint64_t register_text_input_box_components(SHEET *sheet, int x, int y);
uint64_t register_text_input_box_components(SHEET *sheet, int x, int y, uint32_t width);
uint64_t register_text_input_box_components(SHEET *sheet, int x, int y, uint32_t width, const char *text);
void unregister_text_input_box_components(SHEET *sheet);
bool unregister_text_input_box_component(uint64_t id);
bool get_text_input_box_text(uint64_t id, char *buffer, size_t buffer_size);
void process_text_input_box_click_event(SHEET *current_sht, int x, int y);
bool process_text_input_box_key_event(SHEET *current_sht, uint64_t msg_type, uint8_t value);
void put_text_input_box_theme(WINDOWLS *windowls, int x, int y);
void put_text_input_box_theme(WINDOWLS *windowls, int x, int y, uint32_t width);
