#include <graphics/sheet.h>
#include <graphics/components/rb_menu.h>
#include <proto.hpp>
#include <ttf.h>
#include <syscall/syscall.h>

rb_menu_regt_p first_rb_menu;

static void copy_menu_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) return;
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

void copy_rb_menu_items(RightMenuItem **dst, RightMenuItem_user *src, uint64_t cnt)
{
    *dst = (RightMenuItem *)malloc(sizeof(RightMenuItem) * cnt);
    RightMenuItem *dest = *dst;
    for (int i = 0; i < cnt; i++)
    {
        copy_menu_text(dest[i].text, sizeof(dest[i].text), src[i].text);
        dest[i].CRLid = src[i].CRLid;
    }
}

void register_right_rb_button_menu(WINDOWLSP window, RightMenuItem_user *items, uint64_t count)
{
    if (window == NULL || items == NULL || count == 0) return;
    if (window->w_menu != NULL) unregister_right_rb_button_menu_components(window);

    rb_menu_regt_p new_rb_menu = (rb_menu_regt_p)malloc(sizeof(rb_menu_regt));
    new_rb_menu->count = count;
    copy_rb_menu_items(&new_rb_menu->items, items, count);
    new_rb_menu->next = NULL;
    window->w_menu = new_rb_menu;
    new_rb_menu->x = 0;
    new_rb_menu->y = 0;
    new_rb_menu->can_r = false;

    rb_menu_regt_p front_p = first_rb_menu;
    if (front_p != NULL)
    {
        while (true)
        {
            if (front_p->next == NULL)
            {
                // 仅需在末尾插入即可
                front_p->next = new_rb_menu;
                new_rb_menu->prev = front_p;
                break;
            }
            front_p = front_p->next;
        }
    }
    else
    {
        first_rb_menu = new_rb_menu;
        new_rb_menu->prev = NULL;
    }
}

void unregister_right_rb_button_menu_components(WINDOWLSP win)
{
    if (win == NULL) return;

    rb_menu_regt_p front_p = first_rb_menu;
    while (true)
    {
        if (front_p == NULL)
        {
            break;
        }
        else if (front_p == win->w_menu)
        {
            if (front_p->prev == NULL) first_rb_menu = front_p->next;
            if (front_p->prev != NULL) front_p->prev->next = front_p->next;
            if (front_p->next != NULL) front_p->next->prev = front_p->prev;
            free(front_p->items);
            free(front_p);
            win->w_menu = NULL;
            break;
        }
        front_p = front_p->next;
    }
}

extern SHEET *dock_ct_sheet;
extern bool   user_dock_owns_dock_sheet;

void place_menu(rb_menu_regt_p regt)
{
    if (user_dock_owns_dock_sheet) return;

    draw_rect(sht_img, dock_ct_sheet, 
              regt->x, regt->y, 
              regt->x + 199, regt->y + 30 * regt->count - 1, 
              {0xed, 0xed, 0xed, 0xff});

    for (int i = 0; i < regt->count; i++)
    {
        print_box_ttf(sht_img, dock_ct_sheet, regt->items[i].text, {0,0,0,255}, 
                      regt->x + 8, regt->y + 30 * i + 4, 11);
    }

    refresh_part_sheet(sht_img, regt->x, regt->y, 
              regt->x + 200, regt->y + 30 * regt->count - 1);
}

void close_menu(WINDOWLSP win, int x, int y)
{
    if (win == NULL) return;
    if (user_dock_owns_dock_sheet)
    {
        rb_menu_regt_p front_p = first_rb_menu;
        while (front_p != NULL)
        {
            front_p->x = 0;
            front_p->y = 0;
            front_p->can_r = false;
            front_p = front_p->next;
        }
        return;
    }
    rb_menu_regt_p front_p = first_rb_menu;
    while (true)
    {
        if (front_p == NULL)
        {
            break;
        }
        else if (front_p->can_r)
        {
            int px = x - front_p->x;
            int py = y - front_p->y;

            if (px >= 0 && px <= 200)
            {
                if (py >= 0 && py <= 30 * front_p->count)
                {
                    do_message(MSG_CRL, front_p->items[py / 30].CRLid, NULL, win->WinMPf, win->w_task);
                }
            }

            draw_rect(sht_img, dock_ct_sheet, 
                      front_p->x, front_p->y, 
                      front_p->x + 199, front_p->y + 30 * front_p->count - 1, 
                      {0,0,0,0});
            refresh_part_sheet(sht_img, front_p->x, front_p->y, 
                      front_p->x + 200, front_p->y + 30 * front_p->count);
            WINDOWLSP dock_win = sht_found_win_by_type(xwmii, sht_img, dock_ct_sheet, XWIN_DOCK);
            if (dock_win != NULL) do_message(MSG_CRL, 0, 0, dock_win->WinMPf, dock_win->w_task);
            front_p->x = 0;
            front_p->y = 0;
            front_p->can_r = false;
            break;
        }
        front_p = front_p->next;
    }
}

void process_right_button_click_event(WINDOWLSP win, int x, int y)
{
    if (win == NULL || win->w_menu == NULL) return;

    rb_menu_regt_p front_p = first_rb_menu;
    while (true)
    {
        if (front_p == NULL)
        {
            break;
        }
        else if (front_p == win->w_menu)
        {
            front_p->x = x;
            front_p->y = y;
            place_menu(front_p);
            front_p->can_r = true;
            break;
        }
        front_p = front_p->next;
    }
}
