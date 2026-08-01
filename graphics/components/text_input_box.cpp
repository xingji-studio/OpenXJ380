#include <graphics/components/text_input_box.h>
#include <global_color.h>
#include <krlibc.h>
#include <proto.hpp>
#include <syscall/pxapi.h>
#include <ttf.h>

static text_input_box_regt_p first_text_input_box;
static text_input_box_regt_p active_text_input_box;
static uint64_t next_text_input_box_id = 1;

static text_input_box_regt_p find_text_input_box_by_id(uint64_t id)
{
    text_input_box_regt_p front_p = first_text_input_box;
    while (front_p != NULL)
    {
        if (front_p->id == id) return front_p;
        front_p = front_p->next;
    }
    return NULL;
}

static void redraw_text_input_box(text_input_box_regt_p box)
{
    if (box == NULL || box->obj_sheet == NULL) return;

    SHEET_BUFFER border = BGRAY;
    if (box == active_text_input_box) border = WIN_BLUE;
    draw_rect(sht_img, box->obj_sheet, box->x1, box->y1, box->x2, box->y2, WHITE);
    draw_line(sht_img, box->obj_sheet, box->x1, box->y1, box->x2, box->y1, border);
    draw_line(sht_img, box->obj_sheet, box->x1, box->y2, box->x2, box->y2, border);
    draw_line(sht_img, box->obj_sheet, box->x1, box->y1, box->x1, box->y2, border);
    draw_line(sht_img, box->obj_sheet, box->x2, box->y1, box->x2, box->y2, border);

    if (box->text[0] != '\0')
    {
        print_box_ttf(sht_img, box->obj_sheet, box->text, BLACK, box->x1 + 6, box->y1 + 2, 10);
    }

    if (box == active_text_input_box)
    {
        uint64_t text_width = calc_ttf_length(box->text, 10);
        int      cursor_x   = box->x1 + 6 + (int)text_width;
        if (cursor_x > box->x2 - 5) cursor_x = box->x2 - 5;
        draw_line(sht_img, box->obj_sheet, cursor_x, box->y1 + 4, cursor_x, box->y2 - 4, WIN_BLUE);
    }

    refresh_part_sheet(sht_img, getBX(sht_img, box->obj_sheet) + box->x1, getBY(sht_img, box->obj_sheet) + box->y1,
                       getBX(sht_img, box->obj_sheet) + box->x2 + 1,
                       getBY(sht_img, box->obj_sheet) + box->y2 + 1);
}

static uint32_t normalize_text_input_box_width(uint32_t width)
{
    if (width == 0) width = TEXT_INPUT_BOX_DEFAULT_WIDTH;
    if (width < TEXT_INPUT_BOX_MIN_WIDTH) width = TEXT_INPUT_BOX_MIN_WIDTH;
    return width;
}

static void set_text_input_box_text(text_input_box_regt_p box, const char *text)
{
    if (box == NULL) return;
    memset(box->text, 0, sizeof(box->text));
    box->length = 0;
    if (text == NULL) return;

    while (text[box->length] != '\0' && box->length < TEXT_INPUT_BOX_MAX)
    {
        box->text[box->length] = text[box->length];
        box->length++;
    }
    box->text[box->length] = '\0';
}

uint64_t register_text_input_box_components(SHEET *sheet, int x, int y)
{
    return register_text_input_box_components(sheet, x, y, TEXT_INPUT_BOX_DEFAULT_WIDTH);
}

uint64_t register_text_input_box_components(SHEET *sheet, int x, int y, uint32_t width)
{
    return register_text_input_box_components(sheet, x, y, width, NULL);
}

uint64_t register_text_input_box_components(SHEET *sheet, int x, int y, uint32_t width, const char *text)
{
    if (sheet == NULL) return 0;

    width = normalize_text_input_box_width(width);

    text_input_box_regt_p new_box = (text_input_box_regt_p)malloc(sizeof(text_input_box_regt));
    if (new_box == NULL) return 0;
    memset(new_box, 0, sizeof(text_input_box_regt));

    new_box->obj_sheet = sheet;
    new_box->x1        = x;
    new_box->y1        = y;
    new_box->x2        = x + (int)width;
    new_box->y2        = y + TEXT_INPUT_BOX_HEIGHT;
    new_box->id        = next_text_input_box_id++;
    if (next_text_input_box_id == 0) next_text_input_box_id = 1;
    set_text_input_box_text(new_box, text);

    text_input_box_regt_p front_p = first_text_input_box;
    if (front_p != NULL)
    {
        while (front_p->next != NULL) front_p = front_p->next;
        front_p->next = new_box;
        new_box->prev = front_p;
    }
    else
    {
        first_text_input_box = new_box;
    }

    text_input_box_regt_p old_active = active_text_input_box;
    active_text_input_box = new_box;
    if (old_active != NULL) redraw_text_input_box(old_active);
    redraw_text_input_box(new_box);
    return new_box->id;
}

static void unlink_text_input_box(text_input_box_regt_p box)
{
    if (box == NULL) return;
    if (box == active_text_input_box) active_text_input_box = NULL;
    if (box->prev != NULL)
        box->prev->next = box->next;
    else
        first_text_input_box = box->next;
    if (box->next != NULL) box->next->prev = box->prev;
}

void unregister_text_input_box_components(SHEET *sheet)
{
    text_input_box_regt_p front_p = first_text_input_box;
    while (front_p != NULL)
    {
        text_input_box_regt_p next = front_p->next;
        if (front_p->obj_sheet == sheet)
        {
            unlink_text_input_box(front_p);
            free(front_p);
        }
        front_p = next;
    }
}

bool unregister_text_input_box_component(uint64_t id)
{
    text_input_box_regt_p box = find_text_input_box_by_id(id);
    if (box == NULL) return false;
    unlink_text_input_box(box);
    free(box);
    return true;
}

bool get_text_input_box_text(uint64_t id, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) return false;

    text_input_box_regt_p box = find_text_input_box_by_id(id);
    if (box == NULL) return false;

    size_t copy_len = box->length;
    if (copy_len >= buffer_size) copy_len = buffer_size - 1;
    memcpy(buffer, box->text, copy_len);
    buffer[copy_len] = '\0';
    return true;
}

void process_text_input_box_click_event(SHEET *current_sht, int x, int y)
{
    text_input_box_regt_p front_p = first_text_input_box;
    text_input_box_regt_p old_active = active_text_input_box;
    active_text_input_box = NULL;

    while (front_p != NULL)
    {
        if (front_p->obj_sheet == current_sht && x >= front_p->x1 && x <= front_p->x2 &&
            y >= front_p->y1 && y <= front_p->y2)
        {
            active_text_input_box = front_p;
            break;
        }
        front_p = front_p->next;
    }

    if (old_active != active_text_input_box)
    {
        redraw_text_input_box(old_active);
        redraw_text_input_box(active_text_input_box);
    }
}

bool process_text_input_box_key_event(SHEET *current_sht, uint64_t msg_type, uint8_t value)
{
    if (msg_type != MSG_KEYDOWN || active_text_input_box == NULL || active_text_input_box->obj_sheet != current_sht)
    {
        return false;
    }

    bool changed = false;
    if (value == '\b' || value == KEY_BACKSPACE)
    {
        if (active_text_input_box->length > 0)
        {
            active_text_input_box->length--;
            active_text_input_box->text[active_text_input_box->length] = '\0';
            changed = true;
        }
    }
    else if (value >= ' ' && value <= '~' && active_text_input_box->length < TEXT_INPUT_BOX_MAX)
    {
        active_text_input_box->text[active_text_input_box->length] = (char)value;
        active_text_input_box->length++;
        active_text_input_box->text[active_text_input_box->length] = '\0';
        changed = true;
    }

    if (changed) redraw_text_input_box(active_text_input_box);
    return changed;
}

void put_text_input_box_theme(WINDOWLS *windowls, int x, int y)
{
    put_text_input_box_theme(windowls, x, y, TEXT_INPUT_BOX_DEFAULT_WIDTH);
}

void put_text_input_box_theme(WINDOWLS *windowls, int x, int y, uint32_t width)
{
    if (windowls == NULL) return;
    SHEET *sheet = found_sheet_byid(sht_img, windowls->w_sheet);
    if (sheet == NULL) return;

    width = normalize_text_input_box_width(width);

    draw_rect(sht_img, sheet, x, y, x + (int)width, y + TEXT_INPUT_BOX_HEIGHT, WHITE);
    draw_line(sht_img, sheet, x, y, x + (int)width, y, BGRAY);
    draw_line(sht_img, sheet, x, y + TEXT_INPUT_BOX_HEIGHT, x + (int)width,
              y + TEXT_INPUT_BOX_HEIGHT, BGRAY);
    draw_line(sht_img, sheet, x, y, x, y + TEXT_INPUT_BOX_HEIGHT, BGRAY);
    draw_line(sht_img, sheet, x + (int)width, y, x + (int)width,
              y + TEXT_INPUT_BOX_HEIGHT, BGRAY);
}
