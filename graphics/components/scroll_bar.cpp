#include <global_color.h>
#include <graphics/components/scroll_bar.h>
#include <krlibc.h>
#include <proto.hpp>
#include <syscall/syscall.h>

static scroll_bar_regt_p first_scroll_bar;
static scroll_bar_regt_p active_scroll_bar;
static SHEET *active_scroll_bar_sheet;
static int active_scroll_bar_crlid;

static const SHEET_BUFFER SCROLL_BAR_TRACK_COLOR  = {0xe7, 0xee, 0xf6, 0xff};
static const SHEET_BUFFER SCROLL_BAR_BORDER_COLOR = {0x9e, 0xaf, 0xc5, 0xff};
static const SHEET_BUFFER SCROLL_BAR_THUMB_COLOR  = {0xb8, 0xc7, 0xd9, 0xff};
static const SHEET_BUFFER SCROLL_BAR_CLEAR_COLOR  = WHITE;

static int normalize_scroll_bar_length(int length)
{
    if (length < SCROLL_BAR_SIZE) return SCROLL_BAR_SIZE;
    return length;
}

static int normalize_scroll_bar_thumb_length(int length, int thumb_length)
{
    if (thumb_length < SCROLL_BAR_MIN_THUMB) thumb_length = SCROLL_BAR_MIN_THUMB;
    if (thumb_length > length) thumb_length = length;
    return thumb_length;
}

static int scroll_bar_length(scroll_bar_regt_p bar)
{
    if (bar == NULL) return 0;
    return bar->direction == ScrollBarVertical ? bar->y2 - bar->y1 + 1 : bar->x2 - bar->x1 + 1;
}

static int normalize_scroll_bar_position(scroll_bar_regt_p bar, int position)
{
    if (bar == NULL) return 0;

    int max_position = scroll_bar_length(bar) - bar->thumb_length;
    if (max_position < 0) max_position = 0;
    if (position < 0) position = 0;
    if (position > max_position) position = max_position;
    return position;
}

static void update_scroll_bar_thumb_rect(scroll_bar_regt_p bar)
{
    if (bar == NULL) return;

    bar->thumb_position = normalize_scroll_bar_position(bar, bar->thumb_position);
    if (bar->direction == ScrollBarVertical)
    {
        bar->thumb_x1 = bar->x1 + 2;
        bar->thumb_y1 = bar->y1 + bar->thumb_position;
        bar->thumb_x2 = bar->x2 - 2;
        bar->thumb_y2 = bar->thumb_y1 + bar->thumb_length - 1;
    }
    else
    {
        bar->thumb_x1 = bar->x1 + bar->thumb_position;
        bar->thumb_y1 = bar->y1 + 2;
        bar->thumb_x2 = bar->thumb_x1 + bar->thumb_length - 1;
        bar->thumb_y2 = bar->y2 - 2;
    }
}

static void draw_scroll_bar(scroll_bar_regt_p bar)
{
    if (bar == NULL || bar->obj_sheet == NULL) return;

    update_scroll_bar_thumb_rect(bar);

    draw_rect(sht_img, bar->obj_sheet, bar->x1, bar->y1, bar->x2, bar->y2, SCROLL_BAR_TRACK_COLOR);
    draw_line(sht_img, bar->obj_sheet, bar->x1, bar->y1, bar->x2, bar->y1, SCROLL_BAR_BORDER_COLOR);
    draw_line(sht_img, bar->obj_sheet, bar->x1, bar->y2, bar->x2, bar->y2, SCROLL_BAR_BORDER_COLOR);
    draw_line(sht_img, bar->obj_sheet, bar->x1, bar->y1, bar->x1, bar->y2, SCROLL_BAR_BORDER_COLOR);
    draw_line(sht_img, bar->obj_sheet, bar->x2, bar->y1, bar->x2, bar->y2, SCROLL_BAR_BORDER_COLOR);

    draw_rect(sht_img, bar->obj_sheet, bar->thumb_x1, bar->thumb_y1, bar->thumb_x2, bar->thumb_y2,
              SCROLL_BAR_THUMB_COLOR);
    draw_line(sht_img, bar->obj_sheet, bar->thumb_x1, bar->thumb_y1, bar->thumb_x2, bar->thumb_y1,
              SCROLL_BAR_BORDER_COLOR);
    draw_line(sht_img, bar->obj_sheet, bar->thumb_x1, bar->thumb_y2, bar->thumb_x2, bar->thumb_y2,
              SCROLL_BAR_BORDER_COLOR);
    draw_line(sht_img, bar->obj_sheet, bar->thumb_x1, bar->thumb_y1, bar->thumb_x1, bar->thumb_y2,
              SCROLL_BAR_BORDER_COLOR);
    draw_line(sht_img, bar->obj_sheet, bar->thumb_x2, bar->thumb_y1, bar->thumb_x2, bar->thumb_y2,
              SCROLL_BAR_BORDER_COLOR);
}

static void refresh_scroll_bar(scroll_bar_regt_p bar)
{
    if (bar == NULL || bar->obj_sheet == NULL) return;

    refresh_part_sheet(sht_img, getBX(sht_img, bar->obj_sheet) + bar->x1, getBY(sht_img, bar->obj_sheet) + bar->y1,
                       getBX(sht_img, bar->obj_sheet) + bar->x2 + 1,
                       getBY(sht_img, bar->obj_sheet) + bar->y2 + 1);
}

static void clear_scroll_bar(scroll_bar_regt_p bar)
{
    if (bar == NULL || bar->obj_sheet == NULL) return;

    draw_rect(sht_img, bar->obj_sheet, bar->x1, bar->y1, bar->x2, bar->y2, SCROLL_BAR_CLEAR_COLOR);
    refresh_scroll_bar(bar);
}

static bool point_in_scroll_bar(scroll_bar_regt_p bar, int x, int y)
{
    return bar != NULL && x >= bar->x1 && x <= bar->x2 && y >= bar->y1 && y <= bar->y2;
}

static scroll_bar_regt_p find_scroll_bar(SHEET *sheet, int CRLid)
{
    scroll_bar_regt_p front_p = first_scroll_bar;
    while (front_p != NULL)
    {
        if (front_p->obj_sheet == sheet && front_p->CRLid == CRLid) return front_p;
        front_p = front_p->next;
    }
    return NULL;
}

static int scroll_bar_track_position(scroll_bar_regt_p bar, int x, int y)
{
    if (bar == NULL) return 0;

    int position = bar->direction == ScrollBarVertical ? y - bar->y1 : x - bar->x1;
    int length = scroll_bar_length(bar) - 1;
    if (position < 0) position = 0;
    if (position > length) position = length;
    return position;
}

static void send_scroll_bar_message(scroll_bar_regt_p bar, int x, int y)
{
    if (bar == NULL || bar->obj_sheet == NULL) return;

    WINDOWLSP current_window = sht_found_win(xwmii, sht_img, bar->obj_sheet);
    if (current_window == NULL) return;

    do_message(MSG_CRL, bar->CRLid, scroll_bar_track_position(bar, x, y), current_window->WinMPf,
               current_window->w_task);
}

void register_scroll_bar_components(SHEET *sheet, int x, int y, int length, int thumb_length, int CRLid,
                                    ScrollBarDirection direction)
{
    if (sheet == NULL) return;

    length       = normalize_scroll_bar_length(length);
    thumb_length = normalize_scroll_bar_thumb_length(length, thumb_length);

    scroll_bar_regt_p new_bar = (scroll_bar_regt_p)malloc(sizeof(scroll_bar_regt));
    if (new_bar == NULL) return;
    memset(new_bar, 0, sizeof(scroll_bar_regt));

    new_bar->obj_sheet  = sheet;
    new_bar->x1         = x;
    new_bar->y1         = y;
    new_bar->CRLid      = CRLid;
    new_bar->direction  = direction;
    new_bar->thumb_length = thumb_length;
    new_bar->thumb_position = 0;
    if (direction == ScrollBarVertical)
    {
        new_bar->x2 = x + SCROLL_BAR_SIZE - 1;
        new_bar->y2 = y + length - 1;
    }
    else
    {
        new_bar->x2 = x + length - 1;
        new_bar->y2 = y + SCROLL_BAR_SIZE - 1;
    }
    update_scroll_bar_thumb_rect(new_bar);

    scroll_bar_regt_p front_p = first_scroll_bar;
    if (front_p != NULL)
    {
        while (front_p->next != NULL) front_p = front_p->next;
        front_p->next = new_bar;
        new_bar->prev = front_p;
    }
    else
    {
        first_scroll_bar = new_bar;
    }

    draw_scroll_bar(new_bar);
    refresh_scroll_bar(new_bar);
}

void unregister_scroll_bar_components(SHEET *sheet, int CRLid)
{
    scroll_bar_regt_p front_p = first_scroll_bar;
    while (front_p != NULL)
    {
        scroll_bar_regt_p next = front_p->next;
        if (front_p->obj_sheet == sheet && front_p->CRLid == CRLid)
        {
            if (active_scroll_bar == front_p) active_scroll_bar = NULL;
            clear_scroll_bar(front_p);
            if (front_p->prev != NULL)
                front_p->prev->next = front_p->next;
            else
                first_scroll_bar = front_p->next;
            if (front_p->next != NULL) front_p->next->prev = front_p->prev;
            free(front_p);
        }
        front_p = next;
    }
}

void set_scroll_bar_position_components(SHEET *sheet, int CRLid, int position)
{
    scroll_bar_regt_p front_p = first_scroll_bar;
    while (front_p != NULL)
    {
        if (front_p->obj_sheet == sheet && front_p->CRLid == CRLid)
        {
            front_p->thumb_position = position;
            draw_scroll_bar(front_p);
            refresh_scroll_bar(front_p);
        }
        front_p = front_p->next;
    }
}

static void drag_scroll_bar_to(scroll_bar_regt_p bar, int x, int y)
{
    if (bar == NULL) return;

    int position = scroll_bar_track_position(bar, x, y) - bar->thumb_length / 2;
    bar->thumb_position = normalize_scroll_bar_position(bar, position);
    draw_scroll_bar(bar);
    refresh_scroll_bar(bar);
}

void process_scroll_bar_mouse_down_event(SHEET *current_sht, int x, int y)
{
    active_scroll_bar = NULL;
    active_scroll_bar_sheet = NULL;
    active_scroll_bar_crlid = 0;

    scroll_bar_regt_p front_p = first_scroll_bar;
    while (front_p != NULL)
    {
        if (front_p->obj_sheet == current_sht && point_in_scroll_bar(front_p, x, y))
        {
            active_scroll_bar = front_p;
            active_scroll_bar_sheet = current_sht;
            active_scroll_bar_crlid = front_p->CRLid;
            drag_scroll_bar_to(active_scroll_bar, x, y);
            send_scroll_bar_message(active_scroll_bar, x, y);
            break;
        }
        front_p = front_p->next;
    }
}

void process_scroll_bar_drag_event(int x, int y)
{
    if (active_scroll_bar == NULL && active_scroll_bar_sheet != NULL)
    {
        active_scroll_bar = find_scroll_bar(active_scroll_bar_sheet, active_scroll_bar_crlid);
    }
    if (active_scroll_bar == NULL || active_scroll_bar->obj_sheet == NULL) return;

    int local_x = x - (int)getBX(sht_img, active_scroll_bar->obj_sheet);
    int local_y = y - (int)getBY(sht_img, active_scroll_bar->obj_sheet);
    drag_scroll_bar_to(active_scroll_bar, local_x, local_y);
    send_scroll_bar_message(active_scroll_bar, local_x, local_y);
}

void process_scroll_bar_mouse_up_event()
{
    active_scroll_bar = NULL;
    active_scroll_bar_sheet = NULL;
    active_scroll_bar_crlid = 0;
}

void process_scroll_bar_click_event(SHEET *current_sht, int x, int y)
{
    scroll_bar_regt_p front_p = first_scroll_bar;
    while (front_p != NULL)
    {
        if (front_p->obj_sheet == current_sht && x >= front_p->x1 && x <= front_p->x2 &&
            y >= front_p->y1 && y <= front_p->y2)
        {
            WINDOWLSP current_window = sht_found_win(xwmii, sht_img, current_sht);
            if (current_window != NULL)
            {
                do_message(MSG_CRL, front_p->CRLid, scroll_bar_track_position(front_p, x, y), current_window->WinMPf,
                           current_window->w_task);
            }
        }
        front_p = front_p->next;
    }
}

void put_scroll_bar_theme(WINDOWLS *windowls, int x, int y, int length, int thumb_length, int CRLid,
                          ScrollBarDirection direction)
{
    if (windowls == NULL) return;

    SHEET *sheet = found_sheet_byid(sht_img, windowls->w_sheet);
    register_scroll_bar_components(sheet, x, y, length, thumb_length, CRLid, direction);
}
