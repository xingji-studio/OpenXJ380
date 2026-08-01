/*
 *
 *      XJ380 Window Manager
 *      Copyright(C) XINGJI Interactive Software 2017-2026 All rights reserved.
 *
 */
#include "./wbuttons.h"
#include <global_color.h>
#include <graphics/window/window.h>
#include <graphics/components/text_input_box.h>
#include <proto.hpp>
#include <ps2/mouse.h>
#include <ttf.h>
#include <syscall/pxapi.h>

bool WindowIsCreating = false;
extern mouse_dec ms_dec;

int window_create_offest_xy = 32;
static volatile int window_create_lock = 0;

static bool is_valid_gui_ptr(const void *ptr)
{
    return ptr != NULL && (uint64_t)ptr >= get_physical_memory_offset();
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static void blend_pixel_with_white(SHEET_BUFFER *pixel, uint8_t alpha)
{
    pixel->r = (uint8_t)(pixel->r + (((int)0xff - pixel->r) * alpha) / 255);
    pixel->g = (uint8_t)(pixel->g + (((int)0xff - pixel->g) * alpha) / 255);
    pixel->b = (uint8_t)(pixel->b + (((int)0xff - pixel->b) * alpha) / 255);
    pixel->a = (uint8_t)(pixel->a + (((int)0xff - pixel->a) * alpha) / 255);
}

static void draw_title_fog_background(SHEET *sheet, CHAR8 *title, uint32_t frame_width)
{
    if (sheet == NULL || sheet->buffer == NULL || title == NULL || title[0] == '\0') return;
    if (!is_valid_gui_ptr(sheet->buffer)) return;

    const int title_x = 12;
    const int title_y = 8;
    const int title_size = 11;
    const int pad_x = 8;
    const int pad_y = 3;
    const int fog_h = 19;
    const int feather = 10;

    uint32_t title_width = (uint32_t)calc_ttf_length(title, title_size);
    if (title_width < 12) title_width = 12;

    int left = title_x - pad_x;
    int top = title_y - pad_y;
    int right = title_x + (int)title_width + pad_x;
    int bottom = top + fog_h;
    int button_left = (int)frame_width - 78;
    int max_right = (button_left > title_x) ? button_left : ((int)sheet->width - 1);

    left = clamp_int(left, 0, (int)sheet->width - 1);
    right = clamp_int(right, left, max_right);
    top = clamp_int(top, 0, (int)sheet->height - 1);
    bottom = clamp_int(bottom, top, (int)sheet->height - 1);

    SHEET_BUFFER *buffer = (SHEET_BUFFER *)sheet->buffer;
    for (int y = top; y <= bottom; y++)
    {
        int dy = y - top;
        int v_edge = dy < bottom - y ? dy : bottom - y;
        for (int x = left; x <= right; x++)
        {
            int dx = x - left;
            int h_edge = dx < right - x ? dx : right - x;
            int edge = h_edge < v_edge ? h_edge : v_edge;
            int edge_alpha = clamp_int(edge * 255 / feather, 0, 255);
            int t2 = edge_alpha * edge_alpha / 255;
            int t3 = t2 * edge_alpha / 255;
            int t4 = t3 * edge_alpha / 255;
            int t5 = t4 * edge_alpha / 255;
            edge_alpha = clamp_int(6 * t5 - 15 * t4 + 10 * t3, 0, 255);

            int center = (right - left) / 2;
            int center_dist = dx > center ? dx - center : center - dx;
            int glow = 44 - (center_dist * 44) / (center + 1);
            int alpha = 96 + glow;
            alpha = alpha * edge_alpha / 255;

            blend_pixel_with_white(&buffer[(uint32_t)y * sheet->width + (uint32_t)x], (uint8_t)alpha);
        }
    }
}

static void draw_window_title(SHEET_INFO *sht, SHEET *sheet, CHAR8 *title, uint32_t frame_width)
{
    draw_title_fog_background(sheet, title, frame_width);
    print_box_ttf(sht, sheet, title, BLACK, 12, 4, 11);
}

static void redraw_window_frame(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls)
{
    if (!is_valid_gui_ptr(xwmi) || !is_valid_gui_ptr(windowls) || windowls->w_sheet == NULL) return;

    uint32_t width = windowls->width;
    uint32_t height = windowls->height;
    SHEET *sheet = windowls->w_sheet;

    SHEET_BUFFER *top = (SHEET_BUFFER *)malloc((width + 24) * 27 * sizeof(SHEET_BUFFER));
    SHEET_BUFFER *lft = (SHEET_BUFFER *)malloc(height * 12 * sizeof(SHEET_BUFFER));
    if (top == NULL || lft == NULL)
    {
        if (top != NULL) free(top);
        if (lft != NULL) free(lft);
        return;
    }

    resize_theme_width(top, xwmi->theme.top, width + 24, 336, 27);
    copy_buffer_by_id(sht, sheet, top, 0, 0, width + 24, 27);

    resize_theme_width(top, xwmi->theme.bottom, width + 24, 181, 20);
    copy_buffer_by_id(sht, sheet, top, 0, height + 27, width + 24, 20);

    resize_theme_height(lft, xwmi->theme.left, 12, height, 600);
    copy_buffer_by_id(sht, sheet, lft, 0, 27, 12, height);

    resize_theme_height(lft, xwmi->theme.right, 12, height, 600);
    copy_buffer_by_id(sht, sheet, lft, width + 12, 27, 12, height);

    copy_buffer_by_id_without_alpha(sht, sheet, xwmi->theme.close, width - 21, 5, 41, 18);
    copy_buffer_by_id_without_alpha(sht, sheet, xwmi->theme.min, width - 48, 5, 26, 18);
    if (windowls->can_maximize)
    {
        copy_buffer_by_id_without_alpha(sht, windowls->w_sheet, xwmi->theme.max, width - 48, 5, 26, 18);
        copy_buffer_by_id_without_alpha(sht, windowls->w_sheet, xwmi->theme.min, width - 74, 5, 26, 18);
    }
    else
    {
        copy_buffer_by_id_without_alpha(sht, windowls->w_sheet, xwmi->theme.min, width - 48, 5, 26, 18);
    }

    draw_window_title(sht, sheet, windowls->title, width);

    free(top);
    free(lft);
}

bool resize_window_sheet_preserving_content(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls,
                                            int bx, int by, uint32_t width, uint32_t height)
{
    if (!is_valid_gui_ptr(xwmi) || !is_valid_gui_ptr(windowls) || windowls->type != XWIN_NORMAL ||
        windowls->w_sheet == NULL)
    {
        return false;
    }

    uint32_t new_sheet_width = width + 24;
    uint32_t new_sheet_height = height + 47;
    SHEET_BUFFER *new_buffer = (SHEET_BUFFER *)calloc((size_t)new_sheet_width * new_sheet_height,
                                                       sizeof(SHEET_BUFFER));
    if (new_buffer == NULL) return false;

    SHEET_BUFFER white = {0xff, 0xff, 0xff, 0xff};
    for (uint32_t y = 27; y < height + 27; y++)
    {
        SHEET_BUFFER *row = &new_buffer[y * new_sheet_width + 12];
        for (uint32_t x = 0; x < width; x++)
        {
            row[x] = white;
        }
    }

    lock_sheet_manager();

    if (!is_valid_gui_ptr(windowls) || windowls->type != XWIN_NORMAL || windowls->w_sheet == NULL)
    {
        unlock_sheet_manager();
        free(new_buffer);
        return false;
    }

    SHEET *sheet = windowls->w_sheet;
    int old_bx = sheet->bx;
    int old_by = sheet->by;
    uint32_t old_sheet_width = sheet->width;
    uint32_t old_sheet_height = sheet->height;
    uint32_t old_content_width = windowls->width;
    uint32_t old_content_height = windowls->height;
    void *old_buffer = sheet->buffer;

    if (is_valid_gui_ptr(old_buffer))
    {
        SHEET_BUFFER *old_sheet_buffer = (SHEET_BUFFER *)old_buffer;
        uint32_t copy_width = min_u32(old_content_width, width);
        uint32_t copy_height = min_u32(old_content_height, height);
        for (uint32_t y = 0; y < copy_height; y++)
        {
            SHEET_BUFFER *dst = &new_buffer[(y + 27) * new_sheet_width + 12];
            SHEET_BUFFER *src = &old_sheet_buffer[(y + 27) * old_sheet_width + 12];
            memcpy(dst, src, (size_t)copy_width * sizeof(SHEET_BUFFER));
        }
    }

    sheet->buffer = new_buffer;
    sheet->bx = bx;
    sheet->by = by;
    sheet->width = new_sheet_width;
    sheet->height = new_sheet_height;
    windowls->width = width;
    windowls->height = height;

    redraw_window_frame(xwmi, sht, windowls);

    unlock_sheet_manager();

    if (is_valid_gui_ptr(old_buffer)) free(old_buffer);
    refresh_part_sheet(sht, old_bx, old_by, old_bx + (int)old_sheet_width, old_by + (int)old_sheet_height);
    refresh_part_sheet(sht, bx, by, bx + (int)new_sheet_width, by + (int)new_sheet_height);
    return true;
}

void init_xwm(XWM_INFO *xwmi)
{
    write_serial_string("Initializing XWM...\n");
    WINDOWLS *tmp_window = (WINDOWLS *)(malloc(sizeof(WINDOWLS))); // 开辟内存空间
    memset(tmp_window, 0, sizeof(WINDOWLS));
    tmp_window->number = 30000;
    xwmi->start   = tmp_window;
    xwmi->win_num = 0;
    write_serial_string("Reading Theme...\n");
    LoadPictureOgM(&xwmi->theme.top,        "/system/theme/skyglass/top.png");
    LoadPictureOgM(&xwmi->theme.bottom,     "/system/theme/skyglass/bottom.png");
    LoadPictureOgM(&xwmi->theme.left,       "/system/theme/skyglass/left.png");
    LoadPictureOgM(&xwmi->theme.right,      "/system/theme/skyglass/right.png");
    LoadPictureOgM(&xwmi->theme.close,      "/system/theme/skyglass/close.png");
    LoadPictureOgM(&xwmi->theme.min,        "/system/theme/skyglass/min.png");
    LoadPictureOgM(&xwmi->theme.max,        "/system/theme/skyglass/max.png");
    LoadPictureOgM(&xwmi->theme.close_light,"/system/theme/skyglass/close_light.png");
    LoadPictureOgM(&xwmi->theme.max_light,  "/system/theme/skyglass/max_light.png");
    LoadPictureOgM(&xwmi->theme.min_light,  "/system/theme/skyglass/min_light.png");
    write_serial_string("XWM Initializing Success.\n");
}

extern SHEET   *dock_ct_sheet;
extern SHEET   *desktop_ct_sheet;
extern SHEET   *middle_ct_sheet;
extern bool have_full_screen_app;
extern bool user_dock_owns_dock_sheet;
extern TDB_t first_dock_block;

static void show_desktop_remove_window(WINDOWLS *window);

static void append_window_node(XWM_INFO *xwmi, WINDOWLS *new_window)
{
    WINDOWLS *front_p = xwmi->start;
    if (front_p == NULL)
    {
        xwmi->start = new_window;
        return;
    }

    while (true)
    {
        if (front_p->next == NULL)
        {
            front_p->next = new_window;
            return;
        }
        front_p = front_p->next;
    }
}

static void publish_window_node(XWM_INFO *xwmi, WINDOWLS *new_window, WINDOWLS **windowls)
{
    append_window_node(xwmi, new_window);
    if (windowls != NULL) *windowls = new_window;
    xwmi->win_num++;
}

static void clear_window_creation_state(WINDOWLS **windowls)
{
    if (windowls != NULL) *windowls = NULL;
    WindowIsCreating = false;
    __sync_lock_release(&window_create_lock);
}

static void begin_window_creation()
{
    while (__sync_lock_test_and_set(&window_create_lock, 1) != 0)
    {
        scheduler_yield();
    }
    WindowIsCreating = true;
}

static WINDOWLS *alloc_window_node(WINDOWLS **windowls)
{
    if (windowls != NULL) *windowls = NULL;
    WINDOWLS *new_window = (WINDOWLS *)(malloc(sizeof(WINDOWLS)));
    if (new_window == NULL)
    {
        write_serial_string("Create Window Failed. Reason: Cannot allocate window node.\n");
        return NULL;
    }

    memset(new_window, 0, sizeof(WINDOWLS));
    return new_window;
}

static void init_overlay_window_common(WINDOWLS *new_window, uint64_t type, SHEET *sheet)
{
    new_window->WinMPf = NULL;
    new_window->type   = type;
    new_window->width  = sht_img->fbc->horizontal_resolution;
    new_window->height = sht_img->fbc->vertical_resolution;
    new_window->w_task = get_current_task();
    get_current_task()->window_count++;
    new_window->w_sheet = sheet;
}

static void clear_overlay_sheet(SHEET *sheet)
{
    if (sheet == NULL || !is_valid_gui_ptr(sheet->buffer)) return;
    memset(sheet->buffer, 0, (size_t)sheet->width * sheet->height * sizeof(SHEET_BUFFER));
    sheet->is_change = true;
}

bool create_window_fscr(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS **windowls)
{
    begin_window_creation();
    have_full_screen_app = true;

    WINDOWLS *new_window = alloc_window_node(windowls);
    if (new_window == NULL)
    {
        have_full_screen_app = false;
        clear_window_creation_state(windowls);
        return false;
    }

    new_window->WinMPf = NULL;
    new_window->type   = XWIN_FULL_SCR;
    get_current_task()->hasfscr = true;
    get_current_task()->winnum++;

    new_window->width  = sht_img->fbc->horizontal_resolution;
    new_window->height = sht_img->fbc->vertical_resolution;

    new_window->w_task = get_current_task();
    get_current_task()->window_count++;

    int16_t full_screen_sheet_index = sht->sheet_num > 1 ? (int16_t)(sht->sheet_num - 1) : -1;
    if (!create_sheet(sht, 0, 0, sht_img->fbc->horizontal_resolution, sht_img->fbc->vertical_resolution,
                      TopWindowSheetType, full_screen_sheet_index, &new_window->w_sheet))
    {
        write_serial_string("Create Window Failed. Reason: Cannot create full screen sheet.\n");
        get_current_task()->hasfscr = false;
        if (new_window->w_task != NULL && new_window->w_task->winnum > 0) new_window->w_task->winnum--;
        if (new_window->w_task != NULL && new_window->w_task->window_count > 0) new_window->w_task->window_count--;
        have_full_screen_app = false;
        free(new_window);
        clear_window_creation_state(windowls);
        return false;
    }
    publish_window_node(xwmi, new_window, windowls);
    change_sheet_type(sht, new_window->w_sheet, TopWindowSheetType);
    draw_rect(sht, new_window->w_sheet, 0, 0, sht_img->fbc->horizontal_resolution - 1,
              sht_img->fbc->vertical_resolution - 1, {0xff, 0xff, 0xff, 0xff});
    ms_dec.sht_now = new_window->w_sheet;

    refresh_sheet(sht);
    clear_window_creation_state(NULL);
    return true;
}

bool create_window_desktop(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS **windowls)
{
    begin_window_creation();

    WINDOWLS *new_window = alloc_window_node(windowls);
    if (new_window == NULL)
    {
        clear_window_creation_state(windowls);
        return false;
    }

    init_overlay_window_common(new_window, XWIN_DESKTOP, desktop_ct_sheet);
    publish_window_node(xwmi, new_window, windowls);
    if (new_window->w_sheet != NULL)
    {
        change_sheet_type(sht, new_window->w_sheet, FixedSheetType);
        refresh_sheet(sht);
    }

    clear_window_creation_state(NULL);
    return true;
}

bool create_window_dock(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS **windowls)
{
    begin_window_creation();

    WINDOWLS *new_window = alloc_window_node(windowls);
    if (new_window == NULL)
    {
        clear_window_creation_state(windowls);
        return false;
    }

    init_overlay_window_common(new_window, XWIN_DOCK, dock_ct_sheet);
    publish_window_node(xwmi, new_window, windowls);
    // user_dock_owns_dock_sheet = true;
    if (new_window->w_sheet != NULL)
    {
        clear_overlay_sheet(new_window->w_sheet);
        change_sheet_type(sht, new_window->w_sheet, FixedSheetType);
        refresh_sheet(sht);
    }

    clear_window_creation_state(NULL);
    return true;
}

bool create_window_login(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS **windowls)
{
    begin_window_creation();
    have_full_screen_app = true;

    WINDOWLS *new_window = alloc_window_node(windowls);
    if (new_window == NULL)
    {
        have_full_screen_app = false;
        clear_window_creation_state(windowls);
        return false;
    }

    init_overlay_window_common(new_window, XWIN_LOGIN, middle_ct_sheet);
    get_current_task()->hasfscr = true;
    publish_window_node(xwmi, new_window, windowls);
    if (new_window->w_sheet != NULL)
    {
        change_sheet_type(sht, new_window->w_sheet, TopWindowSheetType);
        draw_rect(sht, new_window->w_sheet, 0, 0, sht_img->fbc->horizontal_resolution - 1,
                  sht_img->fbc->vertical_resolution - 1, {0, 0, 0, 0});
        ms_dec.sht_now = new_window->w_sheet;
    }

    clear_window_creation_state(NULL);
    return true;
}


bool create_window_fmoff(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS **windowls, uint32_t width, uint32_t height)
{
    begin_window_creation();

    WINDOWLS *new_window = alloc_window_node(windowls);
    if (new_window == NULL)
    {
        clear_window_creation_state(windowls);
        return false;
    }

    new_window->WinMPf = NULL;
    new_window->type   = XWIN_FRAME_OFF;

    new_window->width  = width;
    new_window->height = height;

    new_window->w_task = get_current_task();
    get_current_task()->window_count++;

    uint32_t bx = 0;
    uint32_t by = 0;
    if (sht != NULL)
    {
        if (sht->scrx > width) bx = (sht->scrx - width) / 2;
        if (sht->scry > height) by = (sht->scry - height) / 2;
    }

    if (!create_sheet(sht, bx, by, width, height, NoEdgeWindowSheetType, -1, &new_window->w_sheet))
    {
        write_serial_string("Create Window Failed. Reason: Cannot create frame-off sheet.\n");
        if (new_window->w_task != NULL && new_window->w_task->window_count > 0) new_window->w_task->window_count--;
        free(new_window);
        clear_window_creation_state(windowls);
        return false;
    }
    publish_window_node(xwmi, new_window, windowls);
    draw_rect(sht, new_window->w_sheet, 0, 0, width - 1, height - 1, {0xff, 0xff, 0xff, 0xff});
    int refresh_bx = getBX(sht, new_window->w_sheet);
    int refresh_by = getBY(sht, new_window->w_sheet);

    ms_dec.sht_now = new_window->w_sheet;

    refresh_part_sheet(sht, refresh_bx, refresh_by, refresh_bx + width, refresh_by + height);
    clear_window_creation_state(NULL);
    return true;
}


bool create_window(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS **windowls, CHAR8 *title, uint32_t width, uint32_t height)
{
    begin_window_creation();

    WINDOWLS *new_window = alloc_window_node(windowls);
    if (new_window == NULL)
    {
        clear_window_creation_state(windowls);
        return false;
    }

    new_window->WinMPf = NULL;
    new_window->type   = XWIN_NORMAL;

    new_window->width  = width;
    new_window->height = height;

    strncpy(new_window->title, title, sizeof(new_window->title) - 1);
    new_window->title[sizeof(new_window->title) - 1] = '\0';

    new_window->w_task = get_current_task();
    get_current_task()->window_count++;

    if (!create_sheet(sht, window_create_offest_xy, window_create_offest_xy,
                      width + 24, height + 47, MovableSheetType, -1, &new_window->w_sheet))
    {
        write_serial_string("Create Window Failed. Reason: Cannot create window sheet.\n");
        if (new_window->w_task != NULL && new_window->w_task->window_count > 0) new_window->w_task->window_count--;
        free(new_window);
        clear_window_creation_state(windowls);
        return false;
    }
    draw_rect(sht, new_window->w_sheet, 12, 27, width + 12, height + 26, {0xff, 0xff, 0xff, 0xff});
    int refresh_bx = getBX(sht, new_window->w_sheet);
    int refresh_by = getBY(sht, new_window->w_sheet);

    window_create_offest_xy %= sht_img->fbc->vertical_resolution / 2;
    window_create_offest_xy += 32;

    SHEET_BUFFER *top = (SHEET_BUFFER *)malloc((width + 24) * 27 * 4);
    SHEET_BUFFER *lft = (SHEET_BUFFER *)malloc(height * 12 * 4);
    if (top == NULL || lft == NULL)
    {
        write_serial_string("Create Window Failed. Reason: Cannot allocate window frame buffer.\n");
        free(top);
        free(lft);
        if (new_window->w_task != NULL && new_window->w_task->window_count > 0) new_window->w_task->window_count--;
        SHEET *failed_sheet = new_window->w_sheet;
        free(new_window);
        clear_window_creation_state(windowls);
        delete_sheet(sht, failed_sheet);
        return false;
    }

    publish_window_node(xwmi, new_window, windowls);

    // 标题栏
    resize_theme_width(top, xwmi->theme.top, width + 24, 336, 27);
    copy_buffer_by_id(sht, new_window->w_sheet, top, 0, 0, width + 24, 27);
    // 底部
    resize_theme_width(top, xwmi->theme.bottom, width + 24, 181, 20);
    copy_buffer_by_id(sht, new_window->w_sheet, top, 0, height + 27, width + 24, 20);
    // 左侧
    resize_theme_height(lft, xwmi->theme.left, 12, height, 600);
    copy_buffer_by_id(sht, new_window->w_sheet, lft, 0, 27, 12, height);
    // 右侧
    resize_theme_height(lft, xwmi->theme.right, 12, height, 600);
    copy_buffer_by_id(sht, new_window->w_sheet, lft, width + 12, 27, 12, height);
    // 按钮
    copy_buffer_by_id_without_alpha(sht, new_window->w_sheet, xwmi->theme.close, width - 21, 5, 41, 18);
    if (new_window->can_maximize)
    {
        copy_buffer_by_id_without_alpha(sht, new_window->w_sheet, xwmi->theme.max, width - 48, 5, 26, 18);
        copy_buffer_by_id_without_alpha(sht, new_window->w_sheet, xwmi->theme.min, width - 74, 5, 26, 18);
    }
    else
    {
        copy_buffer_by_id_without_alpha(sht, new_window->w_sheet, xwmi->theme.min, width - 48, 5, 26, 18);
    }

    // print_box_ttf(sht, new_window->w_sheet, title, {0xd7, 0xd7, 0xd7, 0xff}, 11, 3, 12);
    draw_window_title(sht, new_window->w_sheet, title, width);

    free(top);
    free(lft);

    refresh_part_sheet(sht, refresh_bx, refresh_by, refresh_bx + width + 24, refresh_by + height + 47);

    register_task_dock(new_window);

    clear_window_creation_state(NULL);
    return true;
}

void change_title(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls, CHAR8 *title)
{
    if (windowls == NULL || windowls->w_sheet == NULL) return;

    strncpy(windowls->title, title, sizeof(windowls->title) - 1);
    windowls->title[sizeof(windowls->title) - 1] = '\0';
    redraw_window_frame(xwmi, sht, windowls);
    refresh_part_sheet(sht, getBX(sht, windowls->w_sheet), getBY(sht, windowls->w_sheet),
                       getBX(sht, windowls->w_sheet) + getXsize(sht, windowls->w_sheet) + 10,
                       getBY(sht, windowls->w_sheet) + 22);
}

bool toggle_window_maximized(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls)
{
    if (!is_valid_gui_ptr(xwmi) || sht == NULL || !is_valid_gui_ptr(windowls) || windowls->type != XWIN_NORMAL ||
        windowls->w_sheet == NULL || !windowls->can_maximize)
    {
        return false;
    }

    if (!windowls->is_maximized)
    {
        windowls->restore_bx = windowls->w_sheet->bx;
        windowls->restore_by = windowls->w_sheet->by;
        windowls->restore_width = windowls->width;
        windowls->restore_height = windowls->height;

        const int top_reserved = 24;
        const int bottom_reserved = 87;
        int work_x = 0;
        int work_y = top_reserved;
        int work_w = (int)sht->scrx;
        int work_h = (int)sht->scry - top_reserved - bottom_reserved;

        if (work_w < 160) work_w = (int)sht->scrx;
        if (work_h < 120) {
            work_y = 0;
            work_h = (int)sht->scry;
        }

        uint32_t max_width = work_w > 24 ? (uint32_t)(work_w - 24) : 1;
        uint32_t max_height = work_h > 47 ? (uint32_t)(work_h - 47) : 1;

        if (!resize_window_sheet_preserving_content(xwmi, sht, windowls, work_x, work_y, max_width, max_height))
        {
            return false;
        }
        windowls->is_maximized = true;
        redraw_window_frame(xwmi, sht, windowls);
        refresh_part_sheet(sht, windowls->w_sheet->bx, windowls->w_sheet->by,
                           windowls->w_sheet->bx + (int)windowls->w_sheet->width,
                           windowls->w_sheet->by + (int)windowls->w_sheet->height);
        do_message(MSG_RESIZE, windowls->width, windowls->height, windowls->WinMPf, windowls->w_task);
        return true;
    }

    int restore_bx = windowls->restore_bx;
    int restore_by = windowls->restore_by;
    uint32_t restore_width = windowls->restore_width ? windowls->restore_width : windowls->width;
    uint32_t restore_height = windowls->restore_height ? windowls->restore_height : windowls->height;

    if (!resize_window_sheet_preserving_content(xwmi, sht, windowls, restore_bx, restore_by, restore_width,
                                                restore_height))
    {
        return false;
    }
    windowls->is_maximized = false;
    redraw_window_frame(xwmi, sht, windowls);
    refresh_part_sheet(sht, windowls->w_sheet->bx, windowls->w_sheet->by,
                       windowls->w_sheet->bx + (int)windowls->w_sheet->width,
                       windowls->w_sheet->by + (int)windowls->w_sheet->height);
    do_message(MSG_RESIZE, windowls->width, windowls->height, windowls->WinMPf, windowls->w_task);
    return true;
}

void set_window_maximize_support(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls, bool can_maximize)
{
    if (!is_valid_gui_ptr(xwmi) || sht == NULL || !is_valid_gui_ptr(windowls) || windowls->type != XWIN_NORMAL ||
        windowls->w_sheet == NULL)
    {
        return;
    }

    windowls->can_maximize = can_maximize;
    windowls->can_resize = can_maximize;
    redraw_window_frame(xwmi, sht, windowls);
    refresh_part_sheet(sht, windowls->w_sheet->bx, windowls->w_sheet->by,
                       windowls->w_sheet->bx + (int)windowls->w_sheet->width,
                       windowls->w_sheet->by + 27);
}

void draw_text(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls, CHAR8 *title, int x, int y, int size,
               SHEET_BUFFER color)
{
    print_box_ttf(sht, windowls->w_sheet, title, color, x, y, size);
}

void draw_textl(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls, CHAR8 *title, int x, int y, int size,
               SHEET_BUFFER color, uint32_t *i_width)
{
    print_box_ttfl(sht, windowls->w_sheet, title, color, x, y, size, i_width);
}

void draw_text_sw(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls, CHAR8 *title, int x, int y, int size,
               SHEET_BUFFER color)
{
    print_box_ttf_c(sht, windowls->w_sheet, title, color, x, y, size);
}


void delete_window(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls)
{
    if (!is_valid_gui_ptr(xwmi) || !is_valid_gui_ptr(windowls)) return;
    begin_window_creation();
    if (!window_contains(xwmi, windowls))
    {
        clear_window_creation_state(NULL);
        return;
    }
    if (!is_valid_gui_ptr(windowls->w_sheet) && windowls->type != XWIN_FULL_SCR && windowls->type != XWIN_DESKTOP &&
        windowls->type != XWIN_DOCK && windowls->type != XWIN_LOGIN)
    {
        clear_window_creation_state(NULL);
        return;
    }

    if (ms_dec.sht_now == windowls->w_sheet)
    {
        ms_dec.sht_now       = NULL;
        ms_dec.win_move_lock = false;
        ms_dec.win_resize_lock = false;
        ms_dec.win_resize_pending = false;
        ms_dec.resize_edges = 0;
    }
    if (ms_dec.left_release_window == windowls)
    {
        ms_dec.left_release_window  = NULL;
        ms_dec.left_release_pending = false;
    }
    show_desktop_remove_window(windowls);

    bool need_refresh = false;

    if (windowls->type == XWIN_FULL_SCR || windowls->type == XWIN_LOGIN)
    {
        have_full_screen_app = false;
        SHEET *fs_sheet = found_sheet_byid(sht_img, windowls->w_sheet);
        if (is_valid_gui_ptr(fs_sheet) && is_valid_gui_ptr(fs_sheet->buffer))
        {
            fs_sheet->type = FixedSheetType;
            memset(fs_sheet->buffer, 0, sizeof(SHEET_BUFFER) * fs_sheet->width * fs_sheet->height);
            need_refresh = true;
        }
    }
    else if (windowls->type == XWIN_DOCK)
    {
        user_dock_owns_dock_sheet = false;
    }

    int       bx = 0, by = 0, ex = 0, ey = 0;

    unregister_text_input_box_components(windowls->w_sheet);
    unregister_right_rb_button_menu_components(windowls);
    if (windowls->type != XWIN_DESKTOP && windowls->type != XWIN_DOCK && windowls->type != XWIN_LOGIN)
        unregister_task_dock(windowls);

    if (is_valid_gui_ptr(windowls->w_sheet))
    {
        bx = windowls->w_sheet->bx;
        by = windowls->w_sheet->by;
        ex = bx + windowls->w_sheet->width;
        ey = by + windowls->w_sheet->height;
        need_refresh = true;
    }

    WINDOWLS *front_p  = xwmi->start;
    WINDOWLS *front_p2 = NULL;
    while (front_p != NULL)
    {
        if (front_p == windowls)
        {
            if (front_p2 == NULL) xwmi->start = front_p->next;
            else front_p2->next = front_p->next;

            if (windowls->w_task != NULL && windowls->w_task->window_count > 0) windowls->w_task->window_count--;
            if (windowls->type == XWIN_FULL_SCR && windowls->w_task != NULL && windowls->w_task->winnum > 0)
                windowls->w_task->winnum--;
            if (xwmi->win_num > 0) xwmi->win_num--;

            if (windowls->type != XWIN_DESKTOP && windowls->type != XWIN_DOCK && windowls->type != XWIN_LOGIN)
            {
                delete_sheet(sht, windowls->w_sheet);
            }

            memset(windowls, 0, sizeof(WINDOWLS));
            free((void *)(windowls));
            break;
        }
        front_p2 = front_p;
        front_p  = front_p->next;
    }

    clear_window_creation_state(NULL);

    if (front_p == NULL)
    {
        write_serial_string("Window Delete Faild. (Cannot found window)\n");
        return;
    }

    if (need_refresh) refresh_part_sheet(sht_img, bx, by, ex, ey);
}

void delete_process_windows(XWM_INFO *xwmi, SHEET_INFO *sht, pcb_t process)
{
    if (!is_valid_gui_ptr(xwmi) || sht == NULL || process == NULL) return;

    while (true)
    {
        WINDOWLS *target = NULL;
        WINDOWLS *front_p = xwmi->start;
        while (front_p != NULL)
        {
            if (front_p->w_task != NULL && front_p->w_task->parent_group == process)
            {
                target = front_p;
                break;
            }
            front_p = front_p->next;
        }

        if (target == NULL) break;
        delete_window(xwmi, sht, target);
    }
}

bool window_contains(XWM_INFO *xwmi, WINDOWLS *windowls)
{
    if (!is_valid_gui_ptr(xwmi) || !is_valid_gui_ptr(windowls)) return false;

    WINDOWLS *front_p = xwmi->start;
    while (front_p != NULL)
    {
        if (front_p == windowls) return true;
        front_p = front_p->next;
    }
    return false;
}

WINDOWLS *sht_found_win(XWM_INFO *xwmi, SHEET_INFO *sht, SHEET *csheet)
{
    if (!is_valid_gui_ptr(xwmi) || csheet == NULL) { return NULL; }

    WINDOWLS *front_p = xwmi->start;
    WINDOWLS *matched = NULL;
    while (front_p != NULL)
    {
        if (front_p->w_sheet == csheet) matched = front_p;
        front_p = front_p->next;
    }
    return matched;
}

WINDOWLS *sht_found_win_by_type(XWM_INFO *xwmi, SHEET_INFO *sht, SHEET *csheet, uint64_t type)
{
    if (!is_valid_gui_ptr(xwmi) || csheet == NULL) { return NULL; }

    WINDOWLS *front_p = xwmi->start;
    WINDOWLS *matched = NULL;
    while (front_p != NULL)
    {
        if (front_p->w_sheet == csheet && front_p->type == type) matched = front_p;
        front_p = front_p->next;
    }
    return matched;
}

WINDOWLS *find_window_by_exe_path(XWM_INFO *xwmi, const char *exe_path)
{
    if (!is_valid_gui_ptr(xwmi) || exe_path == NULL || exe_path[0] == '\0') return NULL;

    WINDOWLS *front_p = xwmi->start;
    WINDOWLS *matched = NULL;
    while (front_p != NULL)
    {
        pcb_t process = front_p->w_task != NULL ? front_p->w_task->parent_group : NULL;
        if (process != NULL && process->exe_path != NULL && strcmp(process->exe_path, exe_path) == 0)
        {
            matched = front_p;
        }
        front_p = front_p->next;
    }
    return matched;
}

WINDOWLS *thread_found_win(XWM_INFO *xwmi, tcb_t thread)
{
    if (!is_valid_gui_ptr(xwmi) || thread == NULL) { return NULL; }
    WINDOWLS *front_p = xwmi->start;
    if (front_p == NULL) { return NULL; }
    while (true)
    {
        if (front_p->w_task == thread) { return front_p; }
        else if (front_p->next == NULL) { return NULL; }
        front_p = front_p->next;
    }
}

WINDOWLS *mpf_found_win(MsgPrcor mpf)
{
    if (mpf == NULL) { return NULL; }

    WINDOWLS *front_p = xwmii->start;
    if (front_p == NULL) { return NULL; }
    while (true)
    {
        if (front_p->WinMPf == mpf) { return front_p; }
        else if (front_p->next == NULL) { return NULL; }
        front_p = front_p->next;
    }
}

static bool window_can_alt_tab(WINDOWLS *window)
{
    return window != NULL && window->type == XWIN_NORMAL && window->w_sheet != NULL;
}

enum
{
    ALT_TAB_MAX_WINDOWS = 16,
    ALT_TAB_ITEM_WIDTH = 116,
    ALT_TAB_ITEM_HEIGHT = 86,
    ALT_TAB_GAP = 12,
    ALT_TAB_MARGIN = 18,
    ALT_TAB_HEADER_HEIGHT = 34,
};

struct AltTabState
{
    bool active;
    SHEET *sheet;
    WINDOWLS *items[ALT_TAB_MAX_WINDOWS];
    int count;
    int selected;
};

static AltTabState g_alt_tab;

enum
{
    SHOW_DESKTOP_MAX_WINDOWS = 64,
};

struct ShowDesktopState
{
    bool active;
    WINDOWLS *items[SHOW_DESKTOP_MAX_WINDOWS];
    int bx[SHOW_DESKTOP_MAX_WINDOWS];
    int by[SHOW_DESKTOP_MAX_WINDOWS];
    int count;
};

static ShowDesktopState g_show_desktop;

static void alt_tab_reset_items()
{
    memset(g_alt_tab.items, 0, sizeof(g_alt_tab.items));
    g_alt_tab.count = 0;
    g_alt_tab.selected = 0;
}

static int alt_tab_collect_windows(XWM_INFO *xwmi)
{
    alt_tab_reset_items();
    if (!is_valid_gui_ptr(xwmi)) return 0;

    WINDOWLS *front_p = xwmi->start;
    while (front_p != NULL && g_alt_tab.count < ALT_TAB_MAX_WINDOWS)
    {
        if (window_can_alt_tab(front_p))
        {
            g_alt_tab.items[g_alt_tab.count++] = front_p;
        }
        front_p = front_p->next;
    }
    return g_alt_tab.count;
}

static int alt_tab_index_of(WINDOWLS *window)
{
    for (int i = 0; i < g_alt_tab.count; ++i)
    {
        if (g_alt_tab.items[i] == window) return i;
    }
    return -1;
}

static void alt_tab_select_next_from_current(XWM_INFO *xwmi, SHEET_INFO *sht)
{
    WINDOWLS *current = sht_found_win(xwmi, sht, ms_dec.sht_now);
    int current_index = alt_tab_index_of(current);
    if (g_alt_tab.count <= 0)
    {
        g_alt_tab.selected = 0;
        return;
    }

    g_alt_tab.selected = (current_index >= 0) ? (current_index + 1) % g_alt_tab.count : 0;
}

static void alt_tab_draw_border(SHEET_INFO *sht, SHEET *sheet, int x1, int y1, int x2, int y2, SHEET_BUFFER color)
{
    draw_rect(sht, sheet, x1, y1, x2, y1 + 1, color);
    draw_rect(sht, sheet, x1, y2 - 1, x2, y2, color);
    draw_rect(sht, sheet, x1, y1, x1 + 1, y2, color);
    draw_rect(sht, sheet, x2 - 1, y1, x2, y2, color);
}

static void alt_tab_draw_preview(SHEET_INFO *sht)
{
    if (sht == NULL || g_alt_tab.sheet == NULL) return;

    SHEET *sheet = g_alt_tab.sheet;
    draw_rect(sht, sheet, 0, 0, (int)sheet->width - 1, (int)sheet->height - 1, {0x10, 0x1a, 0x2b, 0xd8});
    print_box_ttf(sht, sheet, (char *)"切换窗口", {0xff, 0xff, 0xff, 0xff}, ALT_TAB_MARGIN, 12, 12);
    char counter[32];
    snprintf(counter, sizeof(counter), "%d / %d", g_alt_tab.selected + 1, g_alt_tab.count);
    print_box_ttf(sht, sheet, counter, {0xd8, 0xe5, 0xff, 0xff}, (unsigned)sheet->width - 72, 12, 10);

    int max_visible = ((int)sheet->width - ALT_TAB_MARGIN * 2 + ALT_TAB_GAP) / (ALT_TAB_ITEM_WIDTH + ALT_TAB_GAP);
    if (max_visible < 1) max_visible = 1;
    if (max_visible > g_alt_tab.count) max_visible = g_alt_tab.count;
    int start_index = g_alt_tab.selected - max_visible / 2;
    if (start_index < 0) start_index = 0;
    if (start_index + max_visible > g_alt_tab.count) start_index = g_alt_tab.count - max_visible;

    int x = ALT_TAB_MARGIN;
    int y = ALT_TAB_HEADER_HEIGHT + 8;
    for (int slot = 0; slot < max_visible; ++slot)
    {
        int i = start_index + slot;
        WINDOWLS *win = g_alt_tab.items[i];
        bool selected = i == g_alt_tab.selected;
        SHEET_BUFFER panel = selected ? SHEET_BUFFER{0xf6, 0xfa, 0xff, 0xff} : SHEET_BUFFER{0xe8, 0xef, 0xf8, 0xee};
        SHEET_BUFFER border = selected ? SHEET_BUFFER{0x13, 0x6d, 0xff, 0xff} : SHEET_BUFFER{0x9a, 0xa8, 0xb8, 0xff};

        int item_x = x + slot * (ALT_TAB_ITEM_WIDTH + ALT_TAB_GAP);
        draw_rect(sht, sheet, item_x, y, item_x + ALT_TAB_ITEM_WIDTH, y + ALT_TAB_ITEM_HEIGHT, panel);
        alt_tab_draw_border(sht, sheet, item_x, y, item_x + ALT_TAB_ITEM_WIDTH, y + ALT_TAB_ITEM_HEIGHT, border);
        draw_rect(sht, sheet, item_x + 12, y + 12, item_x + ALT_TAB_ITEM_WIDTH - 12, y + 48,
                  selected ? SHEET_BUFFER{0xc7, 0xdd, 0xff, 0xff} : SHEET_BUFFER{0xcf, 0xd8, 0xe3, 0xff});
        alt_tab_draw_border(sht, sheet, item_x + 12, y + 12, item_x + ALT_TAB_ITEM_WIDTH - 12, y + 48,
                            {0x7c, 0x8a, 0x99, 0xff});

        char *title = (win != NULL && win->title[0] != '\0') ? win->title : (char *)"窗口";
        print_box_ttf(sht, sheet, title, {0x11, 0x18, 0x27, 0xff}, item_x + 10, y + 58, 9);
    }

    refresh_part_sheet(sht, sheet->bx, sheet->by, sheet->bx + (int)sheet->width, sheet->by + (int)sheet->height);
}

static void alt_tab_close_preview(SHEET_INFO *sht)
{
    SHEET *sheet = g_alt_tab.sheet;
    if (sheet != NULL && sheet_contains(sht, sheet))
    {
        int x = sheet->bx;
        int y = sheet->by;
        int w = (int)sheet->width;
        int h = (int)sheet->height;
        delete_sheet(sht, sheet);
        refresh_part_sheet(sht, x, y, x + w, y + h);
    }

    g_alt_tab.active = false;
    g_alt_tab.sheet = NULL;
    alt_tab_reset_items();
}

static bool alt_tab_create_preview_sheet(SHEET_INFO *sht)
{
    if (sht == NULL || g_alt_tab.count <= 0) return false;

    int visible_count = g_alt_tab.count;
    int width = ALT_TAB_MARGIN * 2 + visible_count * ALT_TAB_ITEM_WIDTH + (visible_count - 1) * ALT_TAB_GAP;
    if (width > (int)sht->scrx - 48) width = (int)sht->scrx - 48;
    if (width < 240) width = 240;
    int height = ALT_TAB_HEADER_HEIGHT + ALT_TAB_ITEM_HEIGHT + ALT_TAB_MARGIN + 8;
    int x = ((int)sht->scrx - width) / 2;
    int y = ((int)sht->scry - height) / 2;

    int16_t preview_layer = sht->sheet_num > 2 ? (int16_t)(sht->sheet_num - 1) : -1;
    if (!create_sheet(sht, x, y, (uint32_t)width, (uint32_t)height, TopWindowSheetType, preview_layer,
                      &g_alt_tab.sheet))
    {
        g_alt_tab.sheet = NULL;
        return false;
    }
    return true;
}

static WINDOWLS *alt_tab_selected_window()
{
    if (g_alt_tab.selected < 0 || g_alt_tab.selected >= g_alt_tab.count) return NULL;
    return g_alt_tab.items[g_alt_tab.selected];
}

static void restore_minimized_window(WINDOWLS *window);

static void show_desktop_remove_window(WINDOWLS *window)
{
    if (!g_show_desktop.active || window == NULL) return;

    for (int i = 0; i < g_show_desktop.count; ++i)
    {
        if (g_show_desktop.items[i] != window) continue;

        int last = g_show_desktop.count - 1;
        g_show_desktop.items[i] = g_show_desktop.items[last];
        g_show_desktop.bx[i] = g_show_desktop.bx[last];
        g_show_desktop.by[i] = g_show_desktop.by[last];
        g_show_desktop.items[last] = NULL;
        g_show_desktop.count--;
        if (g_show_desktop.count <= 0) g_show_desktop.active = false;
        return;
    }
}

static bool show_desktop_restore_window(WINDOWLS *window)
{
    if (!g_show_desktop.active || window == NULL || window->w_sheet == NULL) return false;

    for (int i = 0; i < g_show_desktop.count; ++i)
    {
        if (g_show_desktop.items[i] != window) continue;

        window->w_sheet->bx = g_show_desktop.bx[i];
        window->w_sheet->by = g_show_desktop.by[i];
        show_desktop_remove_window(window);
        return true;
    }

    return false;
}

static bool is_window_offscreen_minimized(SHEET_INFO *sht, WINDOWLS *window)
{
    return sht != NULL && window != NULL && window->w_sheet != NULL &&
           window->w_sheet->bx > (int)sht->scrx;
}

static void mark_dock_window_restored(WINDOWLS *window)
{
    if (window == NULL || user_dock_owns_dock_sheet) return;

    TDB_t front_p = first_dock_block;
    while (front_p != NULL)
    {
        if (front_p->mcount && front_p->windowls == window)
        {
            front_p->min_mode = false;
            front_p->in_focus = false;
            return;
        }
        front_p = front_p->next;
    }
}

static bool switch_to_window(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *target)
{
    if (!is_valid_gui_ptr(xwmi) || sht == NULL || target == NULL || target->w_sheet == NULL ||
        (target->type != XWIN_NORMAL && target->type != XWIN_FRAME_OFF) ||
        !sheet_contains(sht, target->w_sheet))
    {
        return false;
    }

    int old_x = target->w_sheet->bx;
    int old_y = target->w_sheet->by;
    int old_w = (int)target->w_sheet->width;
    int old_h = (int)target->w_sheet->height;

    restore_minimized_window(target);
    lift_sheet(sht, target->w_sheet);
    ms_dec.sht_now = target->w_sheet;
    ms_dec.win_move_lock = false;
    ms_dec.win_resize_lock = false;
    ms_dec.win_resize_pending = false;
    ms_dec.resize_edges = 0;
    focus_window_dock(target);

    refresh_part_sheet(sht, old_x, old_y, old_x + old_w, old_y + old_h);
    refresh_part_sheet(sht, target->w_sheet->bx, target->w_sheet->by,
                       target->w_sheet->bx + (int)target->w_sheet->width,
                       target->w_sheet->by + (int)target->w_sheet->height);
    return true;
}

static void restore_minimized_window(WINDOWLS *window)
{
    if (window == NULL || window->w_sheet == NULL || user_dock_owns_dock_sheet) return;

    TDB_t front_p = first_dock_block;
    while (front_p != NULL)
    {
        if (front_p->mcount && front_p->windowls == window && front_p->min_mode)
        {
            window->w_sheet->bx = front_p->bmx;
            window->w_sheet->by = front_p->bmy;
            front_p->min_mode = false;
            return;
        }
        front_p = front_p->next;
    }
}

bool minimize_window(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls)
{
    if (!is_valid_gui_ptr(xwmi) || sht == NULL || windowls == NULL || windowls->type != XWIN_NORMAL ||
        windowls->w_sheet == NULL || !sheet_contains(sht, windowls->w_sheet))
    {
        return false;
    }

    if (is_window_offscreen_minimized(sht, windowls)) return true;

    save_window_xy(windowls);
    int bx = windowls->w_sheet->bx;
    int by = windowls->w_sheet->by;
    int w = (int)windowls->w_sheet->width;
    int h = (int)windowls->w_sheet->height;
    windowls->w_sheet->bx = sht->scrx + 10;
    if (ms_dec.sht_now == windowls->w_sheet) ms_dec.sht_now = NULL;
    show_desktop_remove_window(windowls);
    refresh_part_sheet(sht, bx, by, bx + w, by + h);
    focus_window_dock(NULL);
    return true;
}

bool restore_and_focus_window(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls)
{
    if (!is_valid_gui_ptr(xwmi) || sht == NULL || windowls == NULL || windowls->type != XWIN_NORMAL ||
        windowls->w_sheet == NULL || !sheet_contains(sht, windowls->w_sheet))
    {
        return false;
    }

    show_desktop_restore_window(windowls);
    return switch_to_window(xwmi, sht, windowls);
}

bool focus_window_by_exe_path(XWM_INFO *xwmi, SHEET_INFO *sht, const char *exe_path)
{
    WINDOWLS *window = find_window_by_exe_path(xwmi, exe_path);
    if (window == NULL || window->w_sheet == NULL || !sheet_contains(sht, window->w_sheet)) return false;

    if (window->type == XWIN_NORMAL) show_desktop_restore_window(window);
    return switch_to_window(xwmi, sht, window);
}

bool close_window_and_task(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls)
{
    if (!is_valid_gui_ptr(xwmi) || sht == NULL || windowls == NULL || !window_contains(xwmi, windowls)) return false;

    show_desktop_remove_window(windowls);
    tcb_t current_win_task = windowls->w_task;
    delete_window(xwmi, sht, windowls);
    if (current_win_task != NULL && current_win_task->window_count == 0) { kill_thread(current_win_task); }
    return true;
}

bool close_window_by_exe_path(XWM_INFO *xwmi, SHEET_INFO *sht, const char *exe_path)
{
    WINDOWLS *window = find_window_by_exe_path(xwmi, exe_path);
    if (window == NULL) return false;
    return close_window_and_task(xwmi, sht, window);
}

bool toggle_show_desktop(XWM_INFO *xwmi, SHEET_INFO *sht)
{
    if (!is_valid_gui_ptr(xwmi) || sht == NULL || have_full_screen_app) return false;

    if (g_show_desktop.active)
    {
        for (int i = 0; i < g_show_desktop.count; ++i)
        {
            WINDOWLS *window = g_show_desktop.items[i];
            if (window == NULL || window->w_sheet == NULL || !window_contains(xwmi, window) ||
                !sheet_contains(sht, window->w_sheet))
            {
                continue;
            }

            int old_x = window->w_sheet->bx;
            int old_y = window->w_sheet->by;
            int old_w = (int)window->w_sheet->width;
            int old_h = (int)window->w_sheet->height;
            window->w_sheet->bx = g_show_desktop.bx[i];
            window->w_sheet->by = g_show_desktop.by[i];
            mark_dock_window_restored(window);
            refresh_part_sheet(sht, old_x, old_y, old_x + old_w, old_y + old_h);
            refresh_part_sheet(sht, window->w_sheet->bx, window->w_sheet->by,
                               window->w_sheet->bx + (int)window->w_sheet->width,
                               window->w_sheet->by + (int)window->w_sheet->height);
        }

        memset(&g_show_desktop, 0, sizeof(g_show_desktop));
        focus_window_dock(NULL);
        return true;
    }

    memset(&g_show_desktop, 0, sizeof(g_show_desktop));

    WINDOWLS *front_p = xwmi->start;
    while (front_p != NULL && g_show_desktop.count < SHOW_DESKTOP_MAX_WINDOWS)
    {
        WINDOWLS *window = front_p;
        front_p = front_p->next;

        if (window->type != XWIN_NORMAL || window->w_sheet == NULL || !sheet_contains(sht, window->w_sheet) ||
            is_window_offscreen_minimized(sht, window))
        {
            continue;
        }

        int index = g_show_desktop.count++;
        g_show_desktop.items[index] = window;
        g_show_desktop.bx[index] = window->w_sheet->bx;
        g_show_desktop.by[index] = window->w_sheet->by;

        int bx = window->w_sheet->bx;
        int by = window->w_sheet->by;
        int w = (int)window->w_sheet->width;
        int h = (int)window->w_sheet->height;
        window->w_sheet->bx = sht->scrx + 10;
        refresh_part_sheet(sht, bx, by, bx + w, by + h);
    }

    g_show_desktop.active = g_show_desktop.count > 0;
    ms_dec.sht_now = NULL;
    ms_dec.win_move_lock = false;
    ms_dec.win_resize_lock = false;
    ms_dec.win_resize_pending = false;
    ms_dec.resize_edges = 0;
    focus_window_dock(NULL);
    return g_show_desktop.active;
}

bool alt_tab_preview_begin(XWM_INFO *xwmi, SHEET_INFO *sht)
{
    if (!is_valid_gui_ptr(xwmi) || sht == NULL || have_full_screen_app) return false;
    if (g_alt_tab.active) return alt_tab_preview_next(xwmi, sht);
    if (alt_tab_collect_windows(xwmi) <= 0) return false;

    alt_tab_select_next_from_current(xwmi, sht);
    if (!alt_tab_create_preview_sheet(sht))
    {
        alt_tab_reset_items();
        return false;
    }

    g_alt_tab.active = true;
    alt_tab_draw_preview(sht);
    return true;
}

bool alt_tab_preview_next(XWM_INFO *xwmi, SHEET_INFO *sht)
{
    if (!is_valid_gui_ptr(xwmi) || sht == NULL || have_full_screen_app) return false;
    if (!g_alt_tab.active) return alt_tab_preview_begin(xwmi, sht);
    if (g_alt_tab.count <= 0) return false;

    g_alt_tab.selected = (g_alt_tab.selected + 1) % g_alt_tab.count;
    alt_tab_draw_preview(sht);
    return true;
}

bool alt_tab_preview_commit(XWM_INFO *xwmi, SHEET_INFO *sht)
{
    if (!g_alt_tab.active)
    {
        return false;
    }

    WINDOWLS *target = alt_tab_selected_window();
    alt_tab_close_preview(sht);
    if (target == NULL || have_full_screen_app) return false;
    return switch_to_window(xwmi, sht, target);
}
