#include <global_color.h>
#include <graphics/GOP.hpp>
#include <graphics/components/button.h>
#include <graphics/components/scroll_bar.h>
#include <graphics/components/text_input_box.h>
#include <graphics/mouse.hpp>
#include <graphics/sheet.h>
#include <proto.hpp>
#include <ps2/keyboard.h>
#include <ps2/mouse.h>
#include <rtc.h>
#include <syscall/pxapi.h>
#include <syscall/syscall.h>
#include <ttf.h>
#include <efi/boot.h>
#include <dlinker.h>
#include <power.h>

mouse_dec ms_dec;
int       mouse_x = 0;
int       mouse_y = 0;
int       tby;
int       tbx;

extern char wbutton_close[18][33];
extern char wbutton_little[18][33];
bool        win_c_bt_isl = false;
bool        win_m_bt_isl = false;
#define WINDOW_ALPHA_VALUE 0xaa

SHEET        *mouse_ct_sheet_img;
extern SHEET *desktop_ct_sheet;
extern SHEET *dock_ct_sheet;
MouseType     type;

extern bool have_full_screen_app;
extern bool logo_menu_just_born;
extern bool logo_menu_is_open;
extern bool allow_to_flush;
extern bool desktop_done;

enum MouseCursorShape
{
    MouseCursorStandard = 0,
    MouseCursorHorizontal = 1,
    MouseCursorVertical = 2,
};

enum WindowResizeEdge
{
    WindowResizeLeft = 1,
    WindowResizeRight = 2,
    WindowResizeTop = 4,
    WindowResizeBottom = 8,
};

static const int WINDOW_RESIZE_HOTZONE = 10;
static const int WINDOW_SNAP_HOTZONE = 8;
static const int WINDOW_DRAG_THRESHOLD = 6;
static const int WINDOW_SNAP_PREVIEW_INSET = 8;
static const int REFRESH_RECT_MERGE_GAP = 18;
static const int REFRESH_RECT_MERGE_MAX_WASTE_PERCENT = 35;
static const uint64_t MOUSE_CURSOR_FRAME_INTERVAL_NS = 4166666ULL;
static const uint64_t WINDOW_MOVE_FRAME_INTERVAL_NS = 16666666ULL;
static const uint64_t WINDOW_RESIZE_FRAME_INTERVAL_NS = 16666666ULL;
static const uint32_t WINDOW_MIN_CONTENT_WIDTH = 80;
static const uint32_t WINDOW_MIN_CONTENT_HEIGHT = 60;

enum WindowSnapPreviewKind
{
    WindowSnapPreviewNone = 0,
    WindowSnapPreviewMaximize,
    WindowSnapPreviewLeft,
    WindowSnapPreviewRight,
};

static SHEET *g_snap_preview_sheet = NULL;
static WindowSnapPreviewKind g_snap_preview_kind = WindowSnapPreviewNone;
static int g_snap_preview_x = 0;
static int g_snap_preview_y = 0;
static int g_snap_preview_w = 0;
static int g_snap_preview_h = 0;

enum DockWindowMenuAction
{
    DockWindowMenuRestore = 1,
    DockWindowMenuMinimize,
    DockWindowMenuMaximize,
    DockWindowMenuClose,
};

static WINDOWLSP g_dock_window_menu_target = NULL;
static bool g_dock_window_menu_open = false;
static int g_dock_window_menu_x = 0;
static int g_dock_window_menu_y = 0;
static const int DOCK_WINDOW_MENU_WIDTH = 128;
static const int DOCK_WINDOW_MENU_ITEM_HEIGHT = 28;
static const int DOCK_WINDOW_MENU_ITEM_COUNT = 4;

static uint64_t g_last_mouse_cursor_flush_ns = 0;

static void clamp_drag_window_position(SHEET_INFO *sht, SHEET *sheet, int *x, int *y);
static void update_snap_preview(WINDOWLS *window);

static inline int rect_min_int(int a, int b)
{
    return a < b ? a : b;
}

static inline int rect_max_int(int a, int b)
{
    return a > b ? a : b;
}

static inline int abs_int(int value)
{
    return value < 0 ? -value : value;
}

static inline uint64_t rect_area_int(int x1, int y1, int x2, int y2)
{
    if (x2 <= x1 || y2 <= y1) return 0;
    return (uint64_t)(x2 - x1) * (uint64_t)(y2 - y1);
}

static inline bool rects_close_enough(int x1, int y1, int x2, int y2,
                                      int other_x1, int other_y1, int other_x2, int other_y2)
{
    return x1 <= other_x2 + REFRESH_RECT_MERGE_GAP && other_x1 <= x2 + REFRESH_RECT_MERGE_GAP &&
           y1 <= other_y2 + REFRESH_RECT_MERGE_GAP && other_y1 <= y2 + REFRESH_RECT_MERGE_GAP;
}

static bool should_merge_refresh_rects(int x1, int y1, int x2, int y2,
                                       int other_x1, int other_y1, int other_x2, int other_y2)
{
    if (!rects_close_enough(x1, y1, x2, y2, other_x1, other_y1, other_x2, other_y2)) return false;

    uint64_t source_area = rect_area_int(x1, y1, x2, y2) + rect_area_int(other_x1, other_y1, other_x2, other_y2);
    if (source_area == 0) return true;

    uint64_t merged_area = rect_area_int(rect_min_int(x1, other_x1), rect_min_int(y1, other_y1),
                                         rect_max_int(x2, other_x2), rect_max_int(y2, other_y2));
    if (merged_area <= source_area) return true;

    uint64_t waste = merged_area - source_area;
    return waste * 100 <= source_area * REFRESH_RECT_MERGE_MAX_WASTE_PERCENT;
}

static inline bool sheet_point_in_rect(const SHEET *sheet, int x, int y)
{
    if (sheet == NULL) return false;
    return sheet->bx <= x && x < sheet->bx + (int)sheet->width &&
           sheet->by <= y && y < sheet->by + (int)sheet->height;
}

static bool sheet_pixel_visible_at(const SHEET *sheet, int x, int y)
{
    if (!sheet_point_in_rect(sheet, x, y) || sheet->buffer == NULL ||
        (uint64_t)sheet->buffer < get_physical_memory_offset())
    {
        return false;
    }

    int lx = x - sheet->bx;
    int ly = y - sheet->by;
    SHEET_BUFFER *buffer = (SHEET_BUFFER *)sheet->buffer;
    return buffer[ly * (int)sheet->width + lx].a != 0;
}

static void refresh_union_rect(SHEET_INFO *sht, int x1, int y1, int x2, int y2,
                               int other_x1, int other_y1, int other_x2, int other_y2)
{
    if (sht == NULL) {
        return;
    }
    refresh_part_sheet(sht,
                       rect_min_int(x1, other_x1),
                       rect_min_int(y1, other_y1),
                       rect_max_int(x2, other_x2),
                       rect_max_int(y2, other_y2));
}

static void refresh_two_rects(SHEET_INFO *sht, int x1, int y1, int x2, int y2,
                              int other_x1, int other_y1, int other_x2, int other_y2)
{
    if (sht == NULL) {
        return;
    }
    if (should_merge_refresh_rects(x1, y1, x2, y2, other_x1, other_y1, other_x2, other_y2))
    {
        refresh_union_rect(sht, x1, y1, x2, y2, other_x1, other_y1, other_x2, other_y2);
        return;
    }
    refresh_part_sheet(sht, x1, y1, x2, y2);
    refresh_part_sheet(sht, other_x1, other_y1, other_x2, other_y2);
}

static void refresh_part_sheet_now(SHEET_INFO *sht, int x1, int y1, int x2, int y2)
{
    if (sht == NULL) {
        return;
    }
    refresh_part_sheet(sht, x1, y1, x2, y2);
    flush_sheet_damage_queue_now(sht);
}

static void refresh_union_rect_now(SHEET_INFO *sht, int x1, int y1, int x2, int y2,
                                   int other_x1, int other_y1, int other_x2, int other_y2)
{
    refresh_union_rect(sht, x1, y1, x2, y2, other_x1, other_y1, other_x2, other_y2);
    flush_sheet_damage_queue_now(sht);
}

static void refresh_two_rects_now(SHEET_INFO *sht, int x1, int y1, int x2, int y2,
                                  int other_x1, int other_y1, int other_x2, int other_y2)
{
    refresh_two_rects(sht, x1, y1, x2, y2, other_x1, other_y1, other_x2, other_y2);
    flush_sheet_damage_queue_now(sht);
}

static void refresh_part_sheet_deferred_or_now(SHEET_INFO *sht, int x1, int y1, int x2, int y2)
{
    if (desktop_done) {
        refresh_part_sheet(sht, x1, y1, x2, y2);
        return;
    }
    refresh_part_sheet_now(sht, x1, y1, x2, y2);
}

static void refresh_union_rect_deferred_or_now(SHEET_INFO *sht, int x1, int y1, int x2, int y2,
                                               int other_x1, int other_y1, int other_x2, int other_y2)
{
    if (desktop_done) {
        refresh_union_rect(sht, x1, y1, x2, y2, other_x1, other_y1, other_x2, other_y2);
        return;
    }
    refresh_union_rect_now(sht, x1, y1, x2, y2, other_x1, other_y1, other_x2, other_y2);
}

static void refresh_two_rects_deferred_or_now(SHEET_INFO *sht, int x1, int y1, int x2, int y2,
                                              int other_x1, int other_y1, int other_x2, int other_y2)
{
    if (desktop_done) {
        refresh_two_rects(sht, x1, y1, x2, y2, other_x1, other_y1, other_x2, other_y2);
        return;
    }
    refresh_two_rects_now(sht, x1, y1, x2, y2, other_x1, other_y1, other_x2, other_y2);
}

static void flush_mouse_cursor_frame_if_due(SHEET_INFO *sht)
{
    if (sht == NULL || !desktop_done) return;

    uint64_t now = nanoTime();
    if (g_last_mouse_cursor_flush_ns != 0 && now - g_last_mouse_cursor_flush_ns < MOUSE_CURSOR_FRAME_INTERVAL_NS)
    {
        return;
    }

    g_last_mouse_cursor_flush_ns = now;
    flush_sheet_damage_queue_now(sht);
}

static bool apply_pending_window_move(bool force)
{
    if (!ms_dec.win_move_pending || ms_dec.sht_now == NULL || sht_img == NULL) return false;

    uint64_t now = nanoTime();
    if (!force && ms_dec.last_win_move_ns != 0 &&
        now - ms_dec.last_win_move_ns < WINDOW_MOVE_FRAME_INTERVAL_NS)
    {
        return false;
    }

    int his_x = ms_dec.sht_now->bx;
    int his_y = ms_dec.sht_now->by;
    int next_x = ms_dec.pending_win_x;
    int next_y = ms_dec.pending_win_y;
    clamp_drag_window_position(sht_img, ms_dec.sht_now, &next_x, &next_y);

    ms_dec.win_move_pending = false;
    ms_dec.last_win_move_ns = now;
    if (next_x == his_x && next_y == his_y)
    {
        return false;
    }

    ms_dec.sht_now->bx = next_x;
    ms_dec.sht_now->by = next_y;
    refresh_two_rects_deferred_or_now(sht_img, ms_dec.sht_now->bx, ms_dec.sht_now->by,
                                      ms_dec.sht_now->bx + ms_dec.sht_now->width,
                                      ms_dec.sht_now->by + ms_dec.sht_now->height,
                                      his_x, his_y,
                                      his_x + ms_dec.sht_now->width,
                                      his_y + ms_dec.sht_now->height);
    update_snap_preview(sht_found_win(xwmii, sht_img, ms_dec.sht_now));
    return true;
}

static bool apply_pending_window_resize(bool force)
{
    if (!ms_dec.win_resize_pending || ms_dec.sht_now == NULL || sht_img == NULL) return false;

    uint64_t now = nanoTime();
    if (!force && ms_dec.last_win_resize_ns != 0 &&
        now - ms_dec.last_win_resize_ns < WINDOW_RESIZE_FRAME_INTERVAL_NS)
    {
        return false;
    }

    WINDOWLS *window = sht_found_win(xwmii, sht_img, ms_dec.sht_now);
    if (window == NULL || window->w_sheet == NULL)
    {
        ms_dec.win_resize_pending = false;
        return false;
    }

    int bx = ms_dec.pending_resize_bx;
    int by = ms_dec.pending_resize_by;
    uint32_t width = ms_dec.pending_resize_width;
    uint32_t height = ms_dec.pending_resize_height;

    ms_dec.win_resize_pending = false;
    ms_dec.last_win_resize_ns = now;
    if (width == window->width && height == window->height && bx == window->w_sheet->bx && by == window->w_sheet->by)
    {
        return false;
    }

    if (!resize_window_sheet_preserving_content(xwmii, sht_img, window, bx, by, width, height))
    {
        return false;
    }

    window->is_maximized = false;
    do_message(MSG_RESIZE, window->width, window->height, window->WinMPf, window->w_task);
    return true;
}

static void clamp_drag_window_position(SHEET_INFO *sht, SHEET *sheet, int *x, int *y)
{
    if (sht == NULL || sheet == NULL || x == NULL || y == NULL) {
        return;
    }

    const int visible_title_width = 96;
    const int visible_title_height = 24;

    int min_x = -(int)sheet->width + visible_title_width;
    int max_x = (int)sht->scrx - visible_title_width;
    int min_y = 0;
    int max_y = (int)sht->scry - visible_title_height;

    if (min_x > max_x) {
        min_x = max_x;
    }
    if (min_y > max_y) {
        min_y = max_y;
    }

    if (*x < min_x) *x = min_x;
    if (*x > max_x) *x = max_x;
    if (*y < min_y) *y = min_y;
    if (*y > max_y) *y = max_y;
}

static uint8_t get_window_resize_edges(WINDOWLS *window, int x, int y)
{
    if (window == NULL || window->w_sheet == NULL || window->type != XWIN_NORMAL || !window->can_resize ||
        window->is_maximized)
    {
        return 0;
    }

    SHEET *sheet = window->w_sheet;
    int left = sheet->bx;
    int top = sheet->by;
    int right = sheet->bx + (int)sheet->width;
    int bottom = sheet->by + (int)sheet->height;
    int content_top = sheet->by + 27;
    int content_bottom = sheet->by + (int)sheet->height - 20;

    if (x < left - WINDOW_RESIZE_HOTZONE || x > right + WINDOW_RESIZE_HOTZONE ||
        y < top - WINDOW_RESIZE_HOTZONE || y > bottom + WINDOW_RESIZE_HOTZONE)
    {
        return 0;
    }

    uint8_t edges = 0;
    if (x >= left - WINDOW_RESIZE_HOTZONE &&
        (x <= left + WINDOW_RESIZE_HOTZONE || (x <= left + 12 && y >= content_top && y <= content_bottom)))
        edges |= WindowResizeLeft;
    if (x <= right + WINDOW_RESIZE_HOTZONE &&
        (x >= right - WINDOW_RESIZE_HOTZONE || (x >= right - 12 && y >= content_top && y <= content_bottom)))
        edges |= WindowResizeRight;
    if (y >= top - WINDOW_RESIZE_HOTZONE && y <= top + WINDOW_RESIZE_HOTZONE) edges |= WindowResizeTop;
    if (y <= bottom + WINDOW_RESIZE_HOTZONE && y >= bottom - 20) edges |= WindowResizeBottom;

    return edges;
}

static bool window_can_snap(WINDOWLS *window)
{
    return window != NULL && window->type == XWIN_NORMAL && window->can_maximize && window->w_sheet != NULL;
}

static bool window_titlebar_hit(WINDOWLS *window, int x, int y)
{
    if (!window_can_snap(window)) return false;

    SHEET *sheet = window->w_sheet;
    int local_x = x - sheet->bx;
    int local_y = y - sheet->by;
    int button_area_left = (int)window->width - 82;
    if (button_area_left < 80) button_area_left = 80;

    return local_y >= 0 && local_y <= 24 && local_x >= 22 && local_x <= button_area_left;
}

static bool is_point_in_window(WINDOWLS *window, int x, int y)
{
    if (window == NULL || window->w_sheet == NULL) return false;

    int left = window->w_sheet->bx;
    int top = window->w_sheet->by;
    int right = left + (int)window->w_sheet->width;
    int bottom = top + (int)window->w_sheet->height;

    if (window->type == XWIN_NORMAL)
    {
        left += 12;
        top += 27;
        right -= 12;
        bottom -= 20;
    }

    return x >= left && x < right && y >= top && y < bottom;
}

static bool window_drag_moved_far_enough()
{
    return abs_int(mouse_x - ms_dec.move_start_mouse_x) >= WINDOW_DRAG_THRESHOLD ||
           abs_int(mouse_y - ms_dec.move_start_mouse_y) >= WINDOW_DRAG_THRESHOLD;
}

static void get_window_work_area(SHEET_INFO *sht, int *x, int *y, int *width, int *height)
{
    const int top_reserved = 24;
    const int bottom_reserved = 87;

    *x = 0;
    *y = top_reserved;
    *width = (int)sht->scrx;
    *height = (int)sht->scry - top_reserved - bottom_reserved;

    if (*width < 160) *width = (int)sht->scrx;
    if (*height < 120)
    {
        *y = 0;
        *height = (int)sht->scry;
    }
}

static void store_drag_restore_bounds(WINDOWLS *window)
{
    if (window == NULL) return;

    window->restore_bx = ms_dec.move_start_bx;
    window->restore_by = ms_dec.move_start_by;
    window->restore_width = ms_dec.move_start_width ? ms_dec.move_start_width : window->width;
    window->restore_height = ms_dec.move_start_height ? ms_dec.move_start_height : window->height;
}

static bool resize_window_to_work_rect(WINDOWLS *window, int x, int y, int width, int height, bool maximized)
{
    if (!window_can_snap(window) || width <= 24 || height <= 47) return false;

    uint32_t content_width = (uint32_t)(width - 24);
    uint32_t content_height = (uint32_t)(height - 47);
    if (!resize_window_sheet_preserving_content(xwmii, sht_img, window, x, y, content_width, content_height))
    {
        return false;
    }

    window->is_maximized = maximized;
    lift_sheet(sht_img, window->w_sheet);
    focus_window_dock(window);
    refresh_part_sheet_deferred_or_now(sht_img, window->w_sheet->bx, window->w_sheet->by,
                                       window->w_sheet->bx + window->w_sheet->width,
                                       window->w_sheet->by + window->w_sheet->height);
    do_message(MSG_RESIZE, window->width, window->height, window->WinMPf, window->w_task);
    return true;
}

static bool snap_window_to_top(WINDOWLS *window)
{
    if (!window_can_snap(window) || sht_img == NULL) return false;

    int work_x, work_y, work_w, work_h;
    get_window_work_area(sht_img, &work_x, &work_y, &work_w, &work_h);
    store_drag_restore_bounds(window);
    return resize_window_to_work_rect(window, work_x, work_y, work_w, work_h, true);
}

static bool snap_window_to_half(WINDOWLS *window, bool left)
{
    if (!window_can_snap(window) || sht_img == NULL) return false;

    int work_x, work_y, work_w, work_h;
    get_window_work_area(sht_img, &work_x, &work_y, &work_w, &work_h);

    int left_width = work_w / 2;
    int right_width = work_w - left_width;
    int snap_x = left ? work_x : work_x + left_width;
    int snap_w = left ? left_width : right_width;
    if (snap_w < 160) return false;

    store_drag_restore_bounds(window);
    return resize_window_to_work_rect(window, snap_x, work_y, snap_w, work_h, false);
}

static bool snap_window_on_drag_release(WINDOWLS *window)
{
    if (!window_can_snap(window) || !ms_dec.win_move_dragged || sht_img == NULL) return false;

    if (mouse_y <= WINDOW_SNAP_HOTZONE) return snap_window_to_top(window);
    if (mouse_x <= WINDOW_SNAP_HOTZONE) return snap_window_to_half(window, true);
    if (mouse_x >= (int)sht_img->scrx - 1 - WINDOW_SNAP_HOTZONE) return snap_window_to_half(window, false);
    return false;
}

static WindowSnapPreviewKind snap_preview_kind_at_mouse(WINDOWLS *window)
{
    if (!window_can_snap(window) || !ms_dec.win_move_dragged || sht_img == NULL) return WindowSnapPreviewNone;

    if (mouse_y <= WINDOW_SNAP_HOTZONE) return WindowSnapPreviewMaximize;
    if (mouse_x <= WINDOW_SNAP_HOTZONE) return WindowSnapPreviewLeft;
    if (mouse_x >= (int)sht_img->scrx - 1 - WINDOW_SNAP_HOTZONE) return WindowSnapPreviewRight;
    return WindowSnapPreviewNone;
}

static bool get_snap_preview_rect(WindowSnapPreviewKind kind, int *x, int *y, int *width, int *height)
{
    if (sht_img == NULL || x == NULL || y == NULL || width == NULL || height == NULL ||
        kind == WindowSnapPreviewNone)
    {
        return false;
    }

    int work_x, work_y, work_w, work_h;
    get_window_work_area(sht_img, &work_x, &work_y, &work_w, &work_h);

    if (kind == WindowSnapPreviewMaximize)
    {
        *x = work_x;
        *y = work_y;
        *width = work_w;
        *height = work_h;
    }
    else
    {
        int left_width = work_w / 2;
        int right_width = work_w - left_width;
        *x = (kind == WindowSnapPreviewLeft) ? work_x : work_x + left_width;
        *y = work_y;
        *width = (kind == WindowSnapPreviewLeft) ? left_width : right_width;
        *height = work_h;
        if (*width < 160) return false;
    }

    if (*width > WINDOW_SNAP_PREVIEW_INSET * 2 && *height > WINDOW_SNAP_PREVIEW_INSET * 2)
    {
        *x += WINDOW_SNAP_PREVIEW_INSET;
        *y += WINDOW_SNAP_PREVIEW_INSET;
        *width -= WINDOW_SNAP_PREVIEW_INSET * 2;
        *height -= WINDOW_SNAP_PREVIEW_INSET * 2;
    }

    return *width > 0 && *height > 0;
}

static void draw_snap_preview_sheet(SHEET *sheet)
{
    if (sheet == NULL || sheet->buffer == NULL) return;

    memset(sheet->buffer, 0, (size_t)sheet->width * sheet->height * sizeof(SHEET_BUFFER));
    draw_rect(sht_img, sheet, 0, 0, (int)sheet->width - 1, (int)sheet->height - 1, {0x16, 0x7d, 0xd9, 0x58});

    const SHEET_BUFFER border = {0x48, 0xae, 0xff, 0xd8};
    for (int i = 0; i < 3; ++i)
    {
        draw_rect(sht_img, sheet, i, i, (int)sheet->width - 1 - i, i, border);
        draw_rect(sht_img, sheet, i, (int)sheet->height - 1 - i,
                  (int)sheet->width - 1 - i, (int)sheet->height - 1 - i, border);
        draw_rect(sht_img, sheet, i, i, i, (int)sheet->height - 1 - i, border);
        draw_rect(sht_img, sheet, (int)sheet->width - 1 - i, i,
                  (int)sheet->width - 1 - i, (int)sheet->height - 1 - i, border);
    }

    sheet->is_change = true;
}

static void hide_snap_preview()
{
    SHEET *sheet = g_snap_preview_sheet;
    g_snap_preview_sheet = NULL;
    g_snap_preview_kind = WindowSnapPreviewNone;
    g_snap_preview_x = 0;
    g_snap_preview_y = 0;
    g_snap_preview_w = 0;
    g_snap_preview_h = 0;

    if (sheet == NULL || sht_img == NULL || !sheet_contains(sht_img, sheet)) return;

    int x = sheet->bx;
    int y = sheet->by;
    int w = (int)sheet->width;
    int h = (int)sheet->height;
    delete_sheet(sht_img, sheet);
    refresh_part_sheet_deferred_or_now(sht_img, x, y, x + w, y + h);
}

static void update_snap_preview(WINDOWLS *window)
{
    WindowSnapPreviewKind kind = snap_preview_kind_at_mouse(window);
    if (kind == WindowSnapPreviewNone)
    {
        hide_snap_preview();
        return;
    }

    int x, y, w, h;
    if (!get_snap_preview_rect(kind, &x, &y, &w, &h))
    {
        hide_snap_preview();
        return;
    }

    if (g_snap_preview_sheet != NULL && g_snap_preview_kind == kind &&
        g_snap_preview_x == x && g_snap_preview_y == y &&
        g_snap_preview_w == w && g_snap_preview_h == h)
    {
        return;
    }

    hide_snap_preview();

    int16_t preview_layer = sht_img->sheet_num > 2 ? (int16_t)(sht_img->sheet_num - 1) : -1;
    if (!create_sheet(sht_img, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, FixedSheetType,
                      preview_layer, &g_snap_preview_sheet))
    {
        g_snap_preview_sheet = NULL;
        g_snap_preview_kind = WindowSnapPreviewNone;
        return;
    }

    g_snap_preview_kind = kind;
    g_snap_preview_x = x;
    g_snap_preview_y = y;
    g_snap_preview_w = w;
    g_snap_preview_h = h;
    draw_snap_preview_sheet(g_snap_preview_sheet);
    refresh_part_sheet_deferred_or_now(sht_img, x, y, x + w, y + h);
}

static WINDOWLS *find_resize_window_at(int x, int y, uint8_t *edges_out)
{
    if (edges_out != NULL) *edges_out = 0;
    if (xwmii == NULL || sht_img == NULL || sht_img->start == NULL || have_full_screen_app) return NULL;

    SHEET *front_p = sht_img->start;
    while (front_p->next != NULL) {
        front_p = front_p->next;
    }

    while (front_p != NULL)
    {
        if (front_p == mouse_ct_sheet_img)
        {
            front_p = front_p->front;
            continue;
        }

        WINDOWLS *window = sht_found_win(xwmii, sht_img, front_p);
        if (window != NULL)
        {
            if (window->type == XWIN_NORMAL)
            {
                uint8_t edges = get_window_resize_edges(window, x, y);
                if (edges != 0)
                {
                    if (edges_out != NULL) *edges_out = edges;
                    return window;
                }
            }

            if (sheet_pixel_visible_at(front_p, x, y))
            {
                return NULL;
            }
        }
        else if (front_p->type != FixedSheetType && sheet_pixel_visible_at(front_p, x, y))
        {
            return NULL;
        }

        front_p = front_p->front;
    }

    return NULL;
}

static void redraw_mouse_cursor(SHEET_INFO *sht, SHEET *csheet, uint8_t shape)
{
    if (sht == NULL || csheet == NULL || csheet->buffer == NULL) return;

    SHEET_BUFFER *buffer = (SHEET_BUFFER *)csheet->buffer;
    memset(buffer, 0, (size_t)csheet->width * csheet->height * sizeof(SHEET_BUFFER));

    int draw_w = 10;
    int draw_h = 21;
    if (shape == MouseCursorHorizontal)
    {
        draw_w = 25;
        draw_h = 13;
    }
    else if (shape == MouseCursorVertical)
    {
        draw_w = 13;
        draw_h = 25;
    }

    for (int y = 0; y < draw_h; y++)
    {
        for (int x = 0; x < draw_w; x++)
        {
            char pixel = '.';
            if (shape == MouseCursorHorizontal) pixel = mouse_horizontal[y][x];
            else if (shape == MouseCursorVertical) pixel = mouse_vertical[y][x];
            else pixel = mouse[y][x];

            if (pixel == '.') { draw_point(sht, csheet, x, y, {0, 0, 0, 0}); }
            else if (pixel == 'w') { draw_point(sht, csheet, x, y, WHITE); }
            else if (pixel == '@') { draw_point(sht, csheet, x, y, BLACK); }
        }
    }
}

static uint8_t cursor_shape_for_resize_edges(uint8_t edges)
{
    if (edges & (WindowResizeLeft | WindowResizeRight)) return MouseCursorHorizontal;
    if (edges & (WindowResizeTop | WindowResizeBottom)) return MouseCursorVertical;
    return MouseCursorStandard;
}

static void update_mouse_cursor_shape(SHEET_INFO *sht, uint8_t shape)
{
    if (sht == NULL || mouse_ct_sheet_img == NULL || ms_dec.cursor_shape == shape) return;

    int old_x = mouse_ct_sheet_img->bx;
    int old_y = mouse_ct_sheet_img->by;
    int old_w = (int)mouse_ct_sheet_img->width;
    int old_h = (int)mouse_ct_sheet_img->height;
    ms_dec.cursor_shape = shape;
    redraw_mouse_cursor(sht, mouse_ct_sheet_img, shape);
    refresh_part_sheet_deferred_or_now(sht, old_x, old_y, old_x + old_w, old_y + old_h);
}

static void update_hover_resize_cursor()
{
    if (ms_dec.left || ms_dec.win_move_lock || ms_dec.win_resize_lock)
    {
        if (!ms_dec.win_resize_lock) update_mouse_cursor_shape(sht_img, MouseCursorStandard);
        return;
    }

    uint8_t edges = 0;
    (void)find_resize_window_at(mouse_x, mouse_y, &edges);
    update_mouse_cursor_shape(sht_img, cursor_shape_for_resize_edges(edges));
}

static bool send_desktop_mouse_message(uint64_t type, uint64_t x, uint64_t y)
{
    WINDOWLSP desktop_window = sht_found_win_by_type(xwmii, sht_img, desktop_ct_sheet, XWIN_DESKTOP);
    if (desktop_window == NULL) return false;

    do_message(type, x, y, desktop_window->WinMPf, desktop_window->w_task);
    return true;
}

static bool g_desktop_context_menu_open = false;
static bool g_desktop_left_pressed = false;
static bool g_desktop_left_dragged = false;
static int  g_desktop_press_x = 0;
static int  g_desktop_press_y = 0;

static bool desktop_mouse_area_at(int x, int y)
{
    if (have_full_screen_app || y < 24 || y >= (int)sht_img->scry - 87) return false;
    return found_sheetmb(sht_img, x, y) == NULL;
}

static bool open_desktop_context_menu(int x, int y)
{
    WINDOWLSP desktop_window = sht_found_win_by_type(xwmii, sht_img, desktop_ct_sheet, XWIN_DESKTOP);
    if (desktop_window == NULL) return false;

    do_message(MSG_RBUTTON, (uint64_t)x, (uint64_t)y, desktop_window->WinMPf, desktop_window->w_task);
    g_desktop_context_menu_open = true;
    return true;
}

static bool send_dock_mouse_message(uint64_t type, uint64_t x, uint64_t y)
{
    WINDOWLSP dock_window = sht_found_win_by_type(xwmii, sht_img, dock_ct_sheet, XWIN_DOCK);
    if (dock_window == NULL) return false;

    do_message(type, x, y, dock_window->WinMPf, dock_window->w_task);
    return true;
}

static void close_dock_window_menu()
{
    if (!g_dock_window_menu_open || dock_ct_sheet == NULL || sht_img == NULL) return;

    draw_rect(sht_img, dock_ct_sheet, g_dock_window_menu_x, g_dock_window_menu_y,
              g_dock_window_menu_x + DOCK_WINDOW_MENU_WIDTH - 1,
              g_dock_window_menu_y + DOCK_WINDOW_MENU_ITEM_HEIGHT * DOCK_WINDOW_MENU_ITEM_COUNT - 1,
              {0, 0, 0, 0});
    refresh_part_sheet_deferred_or_now(sht_img, g_dock_window_menu_x, g_dock_window_menu_y,
                                       g_dock_window_menu_x + DOCK_WINDOW_MENU_WIDTH,
                                       g_dock_window_menu_y +
                                           DOCK_WINDOW_MENU_ITEM_HEIGHT * DOCK_WINDOW_MENU_ITEM_COUNT);
    g_dock_window_menu_target = NULL;
    g_dock_window_menu_open = false;
}

static void draw_dock_window_menu(WINDOWLSP window, int x, int y)
{
    if (window == NULL || dock_ct_sheet == NULL || sht_img == NULL || have_full_screen_app) return;

    close_dock_window_menu();

    int menu_w = DOCK_WINDOW_MENU_WIDTH;
    int menu_h = DOCK_WINDOW_MENU_ITEM_HEIGHT * DOCK_WINDOW_MENU_ITEM_COUNT;
    if (x + menu_w > (int)sht_img->scrx) x = (int)sht_img->scrx - menu_w - 4;
    if (y + menu_h > (int)sht_img->scry) y = (int)sht_img->scry - menu_h - 4;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    g_dock_window_menu_target = window;
    g_dock_window_menu_open = true;
    g_dock_window_menu_x = x;
    g_dock_window_menu_y = y;

    draw_rect(sht_img, dock_ct_sheet, x, y, x + menu_w - 1, y + menu_h - 1, {0xed, 0xf4, 0xff, 0xf2});
    draw_rect(sht_img, dock_ct_sheet, x, y, x + menu_w - 1, y, {0x3a, 0x8d, 0xe8, 0xff});
    draw_rect(sht_img, dock_ct_sheet, x, y + menu_h - 1, x + menu_w - 1, y + menu_h - 1, {0x3a, 0x8d, 0xe8, 0xff});
    draw_rect(sht_img, dock_ct_sheet, x, y, x, y + menu_h - 1, {0x3a, 0x8d, 0xe8, 0xff});
    draw_rect(sht_img, dock_ct_sheet, x + menu_w - 1, y, x + menu_w - 1, y + menu_h - 1, {0x3a, 0x8d, 0xe8, 0xff});

    print_box_ttf(sht_img, dock_ct_sheet, (char *)"还原", BLACK, x + 12, y + 6, 10);
    print_box_ttf(sht_img, dock_ct_sheet, (char *)"最小化", BLACK, x + 12,
                  y + DOCK_WINDOW_MENU_ITEM_HEIGHT + 6, 10);
    print_box_ttf(sht_img, dock_ct_sheet, (char *)"最大化", BLACK, x + 12,
                  y + DOCK_WINDOW_MENU_ITEM_HEIGHT * 2 + 6, 10);
    print_box_ttf(sht_img, dock_ct_sheet, (char *)"关闭", BLACK, x + 12,
                  y + DOCK_WINDOW_MENU_ITEM_HEIGHT * 3 + 6, 10);

    refresh_part_sheet_deferred_or_now(sht_img, x, y, x + menu_w, y + menu_h);
}

static bool handle_dock_window_menu_click(int x, int y)
{
    if (!g_dock_window_menu_open) return false;

    bool inside = x >= g_dock_window_menu_x && x < g_dock_window_menu_x + DOCK_WINDOW_MENU_WIDTH &&
                  y >= g_dock_window_menu_y &&
                  y < g_dock_window_menu_y + DOCK_WINDOW_MENU_ITEM_HEIGHT * DOCK_WINDOW_MENU_ITEM_COUNT;
    WINDOWLSP target = g_dock_window_menu_target;
    int action = inside ? (y - g_dock_window_menu_y) / DOCK_WINDOW_MENU_ITEM_HEIGHT + 1 : 0;
    close_dock_window_menu();
    if (!inside || target == NULL || !window_contains(xwmii, target)) return true;

    if (action == DockWindowMenuRestore)
    {
        restore_and_focus_window(xwmii, sht_img, target);
    }
    else if (action == DockWindowMenuMinimize)
    {
        minimize_window(xwmii, sht_img, target);
    }
    else if (action == DockWindowMenuMaximize)
    {
        if (target->type == XWIN_NORMAL && target->can_maximize) toggle_window_maximized(xwmii, sht_img, target);
    }
    else if (action == DockWindowMenuClose)
    {
        close_window_and_task(xwmii, sht_img, target);
    }

    return true;
}

static bool begin_window_resize_drag(WINDOWLS *window, uint8_t edges)
{
    if (window == NULL || !sheet_contains(sht_img, window->w_sheet) || edges == 0) return false;

    hide_snap_preview();
    ms_dec.win_resize_lock = true;
    ms_dec.win_move_lock = false;
    ms_dec.sht_now = window->w_sheet;
    ms_dec.resize_edges = edges;
    ms_dec.resize_start_mouse_x = mouse_x;
    ms_dec.resize_start_mouse_y = mouse_y;
    ms_dec.resize_start_bx = window->w_sheet->bx;
    ms_dec.resize_start_by = window->w_sheet->by;
    ms_dec.resize_start_width = window->width;
    ms_dec.resize_start_height = window->height;
    ms_dec.win_resize_pending = false;
    ms_dec.last_win_resize_ns = 0;

    lift_sheet(sht_img, window->w_sheet);
    focus_window_dock(window);
    update_mouse_cursor_shape(sht_img, cursor_shape_for_resize_edges(edges));
    refresh_part_sheet_deferred_or_now(sht_img, window->w_sheet->bx, window->w_sheet->by,
                                       window->w_sheet->bx + window->w_sheet->width,
                                       window->w_sheet->by + window->w_sheet->height);
    return true;
}

static bool activate_window_at(int x, int y)
{
    if (sht_img == NULL || xwmii == NULL) return false;

    SHEET *hit_sheet = found_sheetmb(sht_img, (uint32_t)x, (uint32_t)y);
    if (hit_sheet == NULL) return false;

    SHEET *active_sheet = get_sheet(sht_img, hit_sheet);
    if (!sheet_contains(sht_img, active_sheet)) return false;

    WINDOWLSP window = sht_found_win(xwmii, sht_img, active_sheet);
    if (window == NULL) return false;

    ms_dec.sht_now = active_sheet;
    if (!have_full_screen_app)
    {
        lift_sheet(sht_img, active_sheet);
        if (!sheet_contains(sht_img, active_sheet)) return false;
    }

    focus_window_dock(window);
    refresh_part_sheet_deferred_or_now(sht_img, active_sheet->bx, active_sheet->by,
                                       active_sheet->bx + active_sheet->width,
                                       active_sheet->by + active_sheet->height);
    return true;
}

static void process_left_button_down(int x, int y)
{
    if (!ms_dec.left)
    {
        WINDOWLS *launcher_window = find_window_by_exe_path(xwmii, "/apps/system/launcher.elf");
        if (launcher_window != NULL && launcher_window->w_sheet != NULL &&
            !is_point_in_window(launcher_window, x, y))
        {
            close_window_and_task(xwmii, sht_img, launcher_window);
            ms_dec.sht_now = NULL;
        }
        activate_window_at(x, y);
    }
    ms_dec.left = true;
}

static void process_window_resize_drag()
{
    hide_snap_preview();
    WINDOWLS *window = sht_found_win(xwmii, sht_img, ms_dec.sht_now);
    if (window == NULL || window->w_sheet == NULL)
    {
        ms_dec.win_resize_lock = false;
        ms_dec.win_resize_pending = false;
        ms_dec.resize_edges = 0;
        update_mouse_cursor_shape(sht_img, MouseCursorStandard);
        return;
    }

    int dx = mouse_x - ms_dec.resize_start_mouse_x;
    int dy = mouse_y - ms_dec.resize_start_mouse_y;
    int bx = ms_dec.resize_start_bx;
    int by = ms_dec.resize_start_by;
    int width = (int)ms_dec.resize_start_width;
    int height = (int)ms_dec.resize_start_height;

    if (ms_dec.resize_edges & WindowResizeLeft)
    {
        width -= dx;
        bx += dx;
    }
    if (ms_dec.resize_edges & WindowResizeRight) width += dx;
    if (ms_dec.resize_edges & WindowResizeTop)
    {
        height -= dy;
        by += dy;
    }
    if (ms_dec.resize_edges & WindowResizeBottom) height += dy;

    if (width < (int)WINDOW_MIN_CONTENT_WIDTH)
    {
        if (ms_dec.resize_edges & WindowResizeLeft) bx -= (int)WINDOW_MIN_CONTENT_WIDTH - width;
        width = WINDOW_MIN_CONTENT_WIDTH;
    }
    if (height < (int)WINDOW_MIN_CONTENT_HEIGHT)
    {
        if (ms_dec.resize_edges & WindowResizeTop) by -= (int)WINDOW_MIN_CONTENT_HEIGHT - height;
        height = WINDOW_MIN_CONTENT_HEIGHT;
    }

    if (width == (int)window->width && height == (int)window->height && bx == window->w_sheet->bx &&
        by == window->w_sheet->by)
    {
        return;
    }

    ms_dec.pending_resize_bx = bx;
    ms_dec.pending_resize_by = by;
    ms_dec.pending_resize_width = (uint32_t)width;
    ms_dec.pending_resize_height = (uint32_t)height;
    ms_dec.win_resize_pending = true;
    apply_pending_window_resize(false);
}

extern EFI_SYSTEM_TABLE *EFI_ST;
extern BOOT_CONFIG *EFI_BC;

void process_mouse_button()
{
    if (sht_img == NULL) return;

    if (!allow_to_flush) return;
    tbx = get_mouse_x();
    tby = get_mouse_y();
    uint8_t pending_down = __atomic_exchange_n(&ms_dec.pending_btn_down, 0, __ATOMIC_ACQ_REL);
    uint8_t pending_up = __atomic_exchange_n(&ms_dec.pending_btn_up, 0, __ATOMIC_ACQ_REL);
    bool button_changed = pending_down != 0 || pending_up != 0 || ms_dec.btn != ms_dec.last_btn;
    bool mouse_moved = tbx != ms_dec.last_mouse_x || tby != ms_dec.last_mouse_y;
    bool left_down_after_up = (pending_down & 0x01) && (pending_up & 0x01) && (ms_dec.btn & 0x01);
    bool right_down_after_up = (pending_down & 0x02) && (pending_up & 0x02) && (ms_dec.btn & 0x02);
    bool center_down_after_up = (pending_down & 0x04) && (pending_up & 0x04) && (ms_dec.btn & 0x04);

    if ((pending_down & 0x01) && !left_down_after_up)
    {
        // 左键按下
        process_left_button_down(tbx, tby);

        g_desktop_left_pressed = desktop_mouse_area_at(tbx, tby);
        g_desktop_left_dragged = false;
        if (g_desktop_left_pressed)
        {
            g_desktop_press_x = tbx;
            g_desktop_press_y = tby;
            send_desktop_mouse_message(MSG_LBUTTONDOWN, tbx, tby);
        }

        WINDOWLSP current_window = sht_found_win(xwmii, sht_img, ms_dec.sht_now);
        if (current_window != NULL && current_window->w_sheet != NULL)
        {
            process_scroll_bar_mouse_down_event(current_window->w_sheet, tbx - current_window->w_sheet->bx,
                                                tby - current_window->w_sheet->by);
        }
    }
    if ((pending_up & 0x01) && ms_dec.left)
    {
        if (g_desktop_left_pressed)
        {
            send_desktop_mouse_message(MSG_LBUTTONUP, tbx, tby);
            g_desktop_left_pressed = false;
        }
        bool was_resizing = ms_dec.win_resize_lock;
        bool was_moving = ms_dec.win_move_lock;
        bool window_drag_suppressed = was_moving && ms_dec.win_move_dragged;
        bool window_release_consumed = window_drag_suppressed;
        if (g_desktop_context_menu_open)
        {
            send_desktop_mouse_message(MSG_LBUTTON, tbx, tby);
            g_desktop_context_menu_open = false;
            window_release_consumed = true;
        }
        if (was_moving)
        {
            apply_pending_window_move(true);
        }
        if (was_resizing)
        {
            process_window_resize_drag();
            apply_pending_window_resize(true);
        }
        tm time;
        time_read(&time);
        uint64_t click_time = mktime(&time);
        if (ms_dec.first_click && click_time - ms_dec.click_time < 2)
        {
            ms_dec.first_click  = false;
            ms_dec.double_click = true;
        }
        else
        {
            ms_dec.first_click = true;
            ms_dec.click_time  = click_time;
        }
        if (g_desktop_left_dragged)
        {
            ms_dec.first_click = false;
            ms_dec.double_click = false;
            g_desktop_left_dragged = false;
        }

        WINDOWLSP current_window = sht_found_win(xwmii, sht_img, ms_dec.sht_now);
        if (!window_release_consumed && !was_resizing && current_window != NULL)
        {
            if (!window_drag_suppressed && ms_dec.double_click && window_titlebar_hit(current_window, tbx, tby))
            {
                ms_dec.sht_now = current_window->w_sheet;
                toggle_window_maximized(xwmii, sht_img, current_window);
                ms_dec.double_click = false;
                window_release_consumed = true;
            }
            else if (!window_drag_suppressed)
            {
            if (tbx < current_window->w_sheet->bx + 12 ||
                tby < current_window->w_sheet->by + 27 ||
                tbx > current_window->w_sheet->bx +
                          current_window->w_sheet->width - 12 ||
                tby > current_window->w_sheet->by +
                          current_window->w_sheet->height - 20)
            { /* do nothing */
            }
            else
            {
                if (current_window->type == XWIN_NORMAL)
                {
                    do_message(MSG_LBUTTON, tbx - current_window->w_sheet->bx - 12,
                               tby - current_window->w_sheet->by - 27, current_window->WinMPf,
                               current_window->w_task);
                }
                else
                {
                    do_message(MSG_LBUTTON, tbx - current_window->w_sheet->bx,
                               tby - current_window->w_sheet->by, current_window->WinMPf,
                               current_window->w_task);
                }

                process_click_event(current_window->w_sheet, tbx - current_window->w_sheet->bx,
                                                             tby - current_window->w_sheet->by);
                process_scroll_bar_click_event(current_window->w_sheet, tbx - current_window->w_sheet->bx,
                                               tby - current_window->w_sheet->by);
                process_text_input_box_click_event(current_window->w_sheet, tbx - current_window->w_sheet->bx,
                                                   tby - current_window->w_sheet->by);
            }
            if (current_window->type == XWIN_NORMAL &&
                tbx > current_window->w_sheet->bx + current_window->w_sheet->width - 12 - 21 &&
                tbx < current_window->w_sheet->bx + current_window->w_sheet->width + 18 &&
                tby > current_window->w_sheet->by + 5 &&
                tby < current_window->w_sheet->by + 18 + 5)
            {
                // 关闭窗口
                close_window_and_task(xwmii, sht_img, current_window);
                current_window = NULL;
            }
            else if (current_window->type == XWIN_NORMAL &&
                     tbx > current_window->w_sheet->bx + (int)current_window->width - 12 - 48 &&
                     tbx < current_window->w_sheet->bx + (int)current_window->width - 12 - 21 &&
                     tby > current_window->w_sheet->by + 5 &&
                     tby < current_window->w_sheet->by + 18 + 5)
            {
                if (current_window->can_maximize)
                {
                    // 最大化 / 还原
                    ms_dec.sht_now = current_window->w_sheet;
                    toggle_window_maximized(xwmii, sht_img, current_window);
                }
                else
                {
                    // 最小化
                    minimize_window(xwmii, sht_img, current_window);
                }
            }
            else if (current_window->type == XWIN_NORMAL && current_window->can_maximize &&
                     current_window->width >= 80 &&
                     tbx > current_window->w_sheet->bx + (int)current_window->width - 75 &&
                     tbx < current_window->w_sheet->bx + (int)current_window->width - 48 &&
                     tby > current_window->w_sheet->by + 5 &&
                     tby < current_window->w_sheet->by + 18 + 5)
            {
                // 最小化
                minimize_window(xwmii, sht_img, current_window);
            }
            }
        }
        if (window_drag_suppressed && current_window != NULL)
        {
            hide_snap_preview();
            snap_window_on_drag_release(current_window);
            ms_dec.double_click = false;
        }
        else
        {
            hide_snap_preview();
        }
        ms_dec.sht_now              = (window_drag_suppressed && current_window != NULL) ?
                                      current_window->w_sheet : found_sheetmb(sht_img, mouse_x, mouse_y);
        ms_dec.left_release_window  = (was_resizing || window_release_consumed) ? NULL : current_window;
        ms_dec.left_release_pending = !was_resizing && !window_release_consumed;

        ms_dec.left          = false;
        process_scroll_bar_mouse_up_event();
        ms_dec.win_move_lock = false;
        ms_dec.win_resize_lock = false;
        ms_dec.win_move_dragged = false;
        ms_dec.win_move_pending = false;
        ms_dec.win_resize_pending = false;
        ms_dec.resize_edges = 0;
        update_hover_resize_cursor();
    }
    if (left_down_after_up) { process_left_button_down(tbx, tby); }

    if ((pending_down & 0x02) && !right_down_after_up) { ms_dec.right = true; }
    if ((pending_up & 0x02) && ms_dec.right)
    {
        bool handled_right = false;
        if (!have_full_screen_app && tby > sht_img->scry - 24 - 63 && tbx > 132)
        {
            TDB_t tdb = find_dock_icon((tbx - 132) / 80);
            if (tdb != NULL && tdb->windowls != NULL && window_contains(xwmii, tdb->windowls))
            {
                draw_dock_window_menu(tdb->windowls, tbx,
                                      tby - DOCK_WINDOW_MENU_ITEM_HEIGHT * DOCK_WINDOW_MENU_ITEM_COUNT);
                handled_right = true;
            }
        }

        WINDOWLSP current_window = sht_found_win(xwmii, sht_img, ms_dec.sht_now);
        if (!handled_right && current_window != NULL)
        {
            g_desktop_context_menu_open = false;
            if (tbx < current_window->w_sheet->bx + 12 ||
                tby < current_window->w_sheet->by + 27 ||
                tbx > current_window->w_sheet->bx +
                          current_window->w_sheet->width - 12 ||
                tby > current_window->w_sheet->by +
                          current_window->w_sheet->height - 20)
            { /* do nothing */
            }
            else
            {
                if (current_window->type == XWIN_NORMAL)
                {
                    do_message(MSG_RBUTTON, tbx - current_window->w_sheet->bx - 12,
                               tby - current_window->w_sheet->by - 27, current_window->WinMPf,
                               current_window->w_task);
                }
                else
                {
                    do_message(MSG_RBUTTON, tbx - current_window->w_sheet->bx,
                               tby - current_window->w_sheet->by, current_window->WinMPf,
                               current_window->w_task);
                }
                close_menu(current_window, tbx, tby);
                delete_logo_menu();
                process_right_button_click_event(current_window, tbx, tby);
            }
        }
        else if (!handled_right)
        {
            close_dock_window_menu();
            if (!have_full_screen_app) handled_right = open_desktop_context_menu(tbx, tby);
        }

        ms_dec.right = false;
    }
    if (right_down_after_up) { ms_dec.right = true; }

    if ((pending_down & 0x04) && !center_down_after_up) { ms_dec.center = true; }
    if ((pending_up & 0x04) && ms_dec.center)
    {
        WINDOWLSP current_window = sht_found_win(xwmii, sht_img, ms_dec.sht_now);
        if (current_window != NULL)
        {
            if (tbx < current_window->w_sheet->bx + 12 ||
                tby < current_window->w_sheet->by + 27 ||
                tbx > current_window->w_sheet->bx +
                          current_window->w_sheet->width - 12 ||
                tby > current_window->w_sheet->by +
                          current_window->w_sheet->height - 20)
            { /* do nothing */
            }
            else
            {
                if (current_window->type == XWIN_NORMAL)
                {
                    do_message(MSG_MBUTTON, tbx - current_window->w_sheet->bx - 12,
                               tby - current_window->w_sheet->by - 27, current_window->WinMPf,
                               current_window->w_task);
                }
                else
                {
                    do_message(MSG_MBUTTON, tbx - current_window->w_sheet->bx,
                               tby - current_window->w_sheet->by, current_window->WinMPf,
                               current_window->w_task);
                }
            }
        }

        ms_dec.center = false;
    }
    if (center_down_after_up) { ms_dec.center = true; }

    if (mouse_moved && mouse_ct_sheet_img != NULL)
    {
        int prev_tbx = getBX(sht_img, mouse_ct_sheet_img);
        int prev_tby = getBY(sht_img, mouse_ct_sheet_img);
        int mouse_w = (int)getXsize(sht_img, mouse_ct_sheet_img);
        int mouse_h = (int)getYsize(sht_img, mouse_ct_sheet_img);
        move_mouse(sht_img, mouse_ct_sheet_img, tbx, tby);
        refresh_two_rects_deferred_or_now(sht_img, prev_tbx, prev_tby, prev_tbx + mouse_w, prev_tby + mouse_h,
                                          getBX(sht_img, mouse_ct_sheet_img), getBY(sht_img, mouse_ct_sheet_img),
                                          getBX(sht_img, mouse_ct_sheet_img) + mouse_w,
                                          getBY(sht_img, mouse_ct_sheet_img) + mouse_h);
        update_hover_resize_cursor();
        flush_mouse_cursor_frame_if_due(sht_img);
    }

    if (mouse_moved && ms_dec.left)
    {
        if (g_desktop_left_pressed)
        {
            if (abs_int(tbx - g_desktop_press_x) >= 5 || abs_int(tby - g_desktop_press_y) >= 5)
                g_desktop_left_dragged = true;
            send_desktop_mouse_message(MSG_MOVE, tbx, tby);
        }
        process_scroll_bar_drag_event(tbx, tby);
    }

    WINDOWLSP current_window = sht_found_win(xwmii, sht_img, ms_dec.sht_now);
    if (mouse_moved && current_window != NULL && !ms_dec.win_move_lock && !ms_dec.win_resize_lock)
    {
        if (tbx < current_window->w_sheet->bx + 12 ||
            tby < current_window->w_sheet->by + 27 ||
            tbx > current_window->w_sheet->bx + current_window->w_sheet->width -
                      12 ||
            tby > current_window->w_sheet->by +
                      current_window->w_sheet->height - 20)
        { /* do nothing */
        }
        else
        {
            if (current_window->type == XWIN_NORMAL)
            {
                do_message(MSG_MOVE, tbx - current_window->w_sheet->bx - 12,
                           tby - current_window->w_sheet->by - 27, current_window->WinMPf,
                           current_window->w_task);
            }
            else
            {
                do_message(MSG_MOVE, tbx - current_window->w_sheet->bx,
                           tby - current_window->w_sheet->by, current_window->WinMPf,
                           current_window->w_task);
            }
        }
    }

    // 处理滚轮事件
    int scroll = get_mouse_scroll();
    if (scroll != 0 && current_window != NULL)
    {
        // 发送滚轮消息到当前窗口
        // MSG_ROLLER 消息格式：参数1 = 滚轮方向（正=向上，负=向下）
        do_message(MSG_ROLLER, scroll, 0, current_window->WinMPf, current_window->w_task);
    }

    ms_dec.last_btn = ms_dec.btn;
    ms_dec.last_mouse_x = tbx;
    ms_dec.last_mouse_y = tby;
}

extern "C" void c_mouse_handler(void *regs_ptr, uint64_t error_code)
{
    (void)regs_ptr;
    (void)error_code;
    send_eoi();
    uint8_t data = inb(PS2_DATA_PORT);
    if (mousedecode(data))
    {
        ms_dec.need_flush = true;
    }
}

void process_mouse_info()
{
    ms_dec.need_flush = false;
    bool consumed_left_click = false;

    process_mouse_button();

    if (ms_dec.left_release_pending)
    {
        int  click_x  = get_mouse_x();
        int  click_y  = get_mouse_y();
        bool handled  = false;
        int  menu_x1  = sht_img->scrx * 0.1875;
        tbx = click_x;
        tby = click_y;

        WINDOWLSP current_window = sht_found_win(xwmii, sht_img, ms_dec.sht_now);
        bool full_screen_click_already_sent = have_full_screen_app && current_window != NULL &&
                                              ms_dec.left_release_window == current_window;
        if (toast_manager_handle_mouse_click(click_x, click_y))
        {
            handled = true;
        }
        else if (handle_dock_window_menu_click(click_x, click_y))
        {
            handled = true;
        }
        else if (have_full_screen_app && current_window != NULL && !full_screen_click_already_sent)
        {
            do_message(MSG_LBUTTON, click_x, click_y, current_window->WinMPf, current_window->w_task);
            handled = true;
        }
        else if (full_screen_click_already_sent)
        {
            handled = true;
        }

        if (!have_full_screen_app)
        {
            // 底栏和快捷栏点击优先，不依赖 ms_dec.sht_now，避免被错误命中窗口吞掉事件。
            if (click_x > 24 && click_y > sht_img->scry - 24 - 63 && click_x < 24 + 84)
            {
                handled = send_dock_mouse_message(MSG_LBUTTON, click_x, click_y);
                if (!handled) create_user_process_from_file((char *)"/apps/system/ctrlmenu.elf", NULL, NULL);
                handled = true;
            }
            else if (click_x > 132 && click_y > sht_img->scry - 24 - 63)
            {
                TDB_t tdb = find_dock_icon((click_x - 132) / 80);
                if (tdb != NULL && tdb->windowls != NULL && sheet_contains(sht_img, tdb->windowls->w_sheet))
                {
                    handled = restore_and_focus_window(xwmii, sht_img, tdb->windowls);
                }
            }
            else if (click_y < 24)
            {
                if (click_x > menu_x1 && click_x < menu_x1 + 32)
                {
                    draw_logo_menu();
                    handled = true;
                }
                else
                {
                    handled = send_dock_mouse_message(MSG_LBUTTON, click_x, click_y);
                }
            }
        }

        if (!handled && logo_menu_is_open && click_x > menu_x1 && click_x < menu_x1 + 100)
        {
            if (click_y > 28 && click_y < 50)
            {
                // 重启
                do_xapi_PowerAction(XPOWER_REBOOT);
                handled = true;
            }
            else if (click_y > 50 && click_y < 72)
            {
                // 关机
                do_xapi_PowerAction(XPOWER_SHUTDOWN);
                handled = true;
            }
            else if (click_y > 72 && click_y < 94)
            {
                // 刷新
                send_desktop_mouse_message(MSG_CRL, 0, 0);
                handled = true;
            }
        }

        if (handled)
        {
            consumed_left_click = true;
            ms_dec.double_click = false;
        }
        close_menu(ms_dec.left_release_window, tbx, tby);
        delete_logo_menu();
        ms_dec.left_release_window  = NULL;
        ms_dec.left_release_pending = false;
    }

    if (!consumed_left_click && ms_dec.left)
    {
        if (ms_dec.win_resize_lock)
        {
            process_window_resize_drag();
        }
        else if (!ms_dec.win_move_lock)
        {
            uint8_t resize_edges = 0;
            WINDOWLS *resize_window = find_resize_window_at(mouse_x, mouse_y, &resize_edges);
            if (resize_window != NULL && begin_window_resize_drag(resize_window, resize_edges))
            {
                return;
            }

            SHEET *movable_sheet = found_sheet_movable(sht_img, mouse_x, mouse_y);
            if (movable_sheet == NULL || have_full_screen_app)
            {
                // 点在了标题栏外
                ms_dec.win_move_lock = false;
                ms_dec.win_move_pending = false;
                hide_snap_preview();
            }
            else
            {
                // 点在了窗口上
                ms_dec.win_move_lock = true;
                ms_dec.sht_now       = get_sheet(sht_img, movable_sheet);
                WINDOWLSP moving_window = sht_found_win(xwmii, sht_img, ms_dec.sht_now);
                if (!sheet_contains(sht_img, ms_dec.sht_now))
                {
                    ms_dec.win_move_lock = false;
                    ms_dec.sht_now       = NULL;
                    ms_dec.win_move_pending = false;
                    hide_snap_preview();
                }
                else if (moving_window != NULL && moving_window->is_maximized)
                {
                    ms_dec.win_move_lock = false;
                    ms_dec.win_move_pending = false;
                    hide_snap_preview();
                }
                else
                {
                    ms_dec.win_x_offset = mouse_x - ms_dec.sht_now->bx;
                    ms_dec.win_y_offset = mouse_y - ms_dec.sht_now->by;
                    ms_dec.move_start_mouse_x = mouse_x;
                    ms_dec.move_start_mouse_y = mouse_y;
                    ms_dec.move_start_bx = ms_dec.sht_now->bx;
                    ms_dec.move_start_by = ms_dec.sht_now->by;
                    ms_dec.move_start_width = moving_window != NULL ? moving_window->width : 0;
                    ms_dec.move_start_height = moving_window != NULL ? moving_window->height : 0;
                    ms_dec.win_move_dragged = false;
                    ms_dec.win_move_pending = false;
                    ms_dec.last_win_move_ns = 0;
                }
            }
            SHEET *hover_sheet = found_sheetmb(sht_img, mouse_x, mouse_y);
            if (hover_sheet == NULL)
            {
                // 点在了桌面或快捷栏上
                // do nothing
                if (ms_dec.double_click)
                {
                    ms_dec.double_click = false;
                    send_desktop_mouse_message(MSG_LBUTTON, tbx, tby);
                }
            }
            else
            {
                SHEET *active_sheet = get_sheet(sht_img, hover_sheet);
                if (!sheet_contains(sht_img, active_sheet)) return;
                focus_window_dock(sht_found_win(xwmii, sht_img, active_sheet));
            }
        }
        else
        {
            if (ms_dec.sht_now == NULL)
            {
                ms_dec.win_move_lock = false;
                ms_dec.win_move_pending = false;
                hide_snap_preview();
                ms_dec.need_flush = false;
                return;
            }
            // 移动窗口
            int next_x = mouse_x - ms_dec.win_x_offset;
            int next_y = mouse_y - ms_dec.win_y_offset;
            if (window_drag_moved_far_enough()) ms_dec.win_move_dragged = true;
            clamp_drag_window_position(sht_img, ms_dec.sht_now, &next_x, &next_y);
            ms_dec.pending_win_x = next_x;
            ms_dec.pending_win_y = next_y;
            ms_dec.win_move_pending = true;
            apply_pending_window_move(false);
        }
    }
}

bool mousedecode(uint8_t data)
{
    if (ms_dec.phase == 0)
    {
        if (data == 0xfa) { ms_dec.phase = 1; }
        return false;
    }
    if (ms_dec.phase == 1)
    {
        if ((data & 0xc8) == 0x08)
        {
            ms_dec.buf[0] = data;
            ms_dec.phase  = 2;
        }
        return 0;
    }
    if (ms_dec.phase == 2)
    {
        ms_dec.buf[1] = data;
        ms_dec.phase  = 3;
        return 0;
    }
    if (ms_dec.phase == 3)
    {
        ms_dec.buf[2] = data;
        ms_dec.phase  = 1;
        int old_btn   = ms_dec.btn;
        ms_dec.btn    = ms_dec.buf[0] & 0x07;
        int changed   = old_btn ^ ms_dec.btn;
        __atomic_fetch_or(&ms_dec.pending_btn_down, (uint8_t)(changed & ms_dec.btn), __ATOMIC_RELEASE);
        __atomic_fetch_or(&ms_dec.pending_btn_up, (uint8_t)(changed & old_btn), __ATOMIC_RELEASE);
        ms_dec.x      = ms_dec.buf[1];
        ms_dec.y      = ms_dec.buf[2];

        if ((ms_dec.buf[0] & 0x10) != 0) ms_dec.x |= 0xffffff00;
        if ((ms_dec.buf[0] & 0x20) != 0) ms_dec.y |= 0xffffff00;
        ms_dec.y = -ms_dec.y;
        if (type == OnlyScroll || type == FiveButton)
        {
            ms_dec.phase = 4;
            return 0;
        }
        else
        {
            mouse_x += ms_dec.x;
            mouse_y += ms_dec.y;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_x > get_hor() - 1) mouse_x = get_hor() - 1;
            if (mouse_y > get_ver() - 1) mouse_y = get_ver() - 1;
            return true;
        }
    }
    if (ms_dec.phase == 4)
    {
        ms_dec.buf[3]   = data;
        ms_dec.phase    = 1;
        int wheel_delta = ms_dec.buf[3];
        if (wheel_delta >= 255) wheel_delta = -1;

        ms_dec.scroll = -wheel_delta;
        // 更新鼠标位置
        mouse_x += ms_dec.x;
        mouse_y += ms_dec.y;
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x > get_hor() - 1) mouse_x = get_hor() - 1;
        if (mouse_y > get_ver() - 1) mouse_y = get_ver() - 1;
        return true;
    }
    return false;
}

int get_mouse_x()
{
    return mouse_x;
}

int get_mouse_y()
{
    return mouse_y;
}

void set_mouse_position(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > (int)get_hor() - 1) x = (int)get_hor() - 1;
    if (y > (int)get_ver() - 1) y = (int)get_ver() - 1;

    int old_x = mouse_x;
    int old_y = mouse_y;
    mouse_x = x;
    mouse_y = y;

    ms_dec.win_move_lock = false;
    ms_dec.win_resize_lock = false;
    ms_dec.sht_now = NULL;
    ms_dec.win_move_pending = false;
    ms_dec.win_resize_pending = false;
    ms_dec.last_win_move_ns = 0;
    ms_dec.last_win_resize_ns = 0;
    ms_dec.resize_edges = 0;
    ms_dec.cursor_shape = MouseCursorStandard;
    ms_dec.last_btn = ms_dec.btn;
    ms_dec.last_mouse_x = mouse_x;
    ms_dec.last_mouse_y = mouse_y;
    hide_snap_preview();

    if (sht_img == NULL || mouse_ct_sheet_img == NULL) {
        return;
    }

    int mouse_w = (int)getXsize(sht_img, mouse_ct_sheet_img);
    int mouse_h = (int)getYsize(sht_img, mouse_ct_sheet_img);
    move_mouse(sht_img, mouse_ct_sheet_img, mouse_x, mouse_y);
    refresh_two_rects_deferred_or_now(sht_img, old_x, old_y, old_x + mouse_w, old_y + mouse_h,
                                      getBX(sht_img, mouse_ct_sheet_img), getBY(sht_img, mouse_ct_sheet_img),
                                      getBX(sht_img, mouse_ct_sheet_img) + mouse_w,
                                      getBY(sht_img, mouse_ct_sheet_img) + mouse_h);
    flush_mouse_cursor_frame_if_due(sht_img);
}

void center_mouse_cursor()
{
    set_mouse_position((int)(get_hor() - 16) / 2, (int)(get_ver() - 28 - 29) / 2);
}

int get_mouse_scroll()
{
    int scroll = ms_dec.scroll;
    ms_dec.scroll = 0;  // 读取后重置
    return scroll;
}

extern "C" void mouse_inject_report(int dx, int dy, uint8_t buttons, int wheel)
{
    int old_btn = ms_dec.btn;
    ms_dec.btn = buttons & 0x07;
    int changed = old_btn ^ ms_dec.btn;
    __atomic_fetch_or(&ms_dec.pending_btn_down, (uint8_t)(changed & ms_dec.btn), __ATOMIC_RELEASE);
    __atomic_fetch_or(&ms_dec.pending_btn_up, (uint8_t)(changed & old_btn), __ATOMIC_RELEASE);
    ms_dec.x   = dx;
    ms_dec.y   = dy;
    ms_dec.scroll += wheel;

    mouse_x += dx;
    mouse_y += dy;

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x > get_hor() - 1) mouse_x = get_hor() - 1;
    if (mouse_y > get_ver() - 1) mouse_y = get_ver() - 1;

    ms_dec.need_flush = true;
}

EXPORT_SYMBOL(mouse_inject_report);

static bool send_command(uint8_t value)
{
    wait_ps2_write();
    outb(PS2_CMD_PORT, KB_SEND2MOUSE);
    wait_ps2_write();
    outb(PS2_DATA_PORT, value);
    return inb(PS2_DATA_PORT) != 0xfa;
}

void mouse_wait(uint8_t a_type)
{
    uint64_t time_ns = nanoTime() + 1ULL * 1000000000ULL;
    if (!a_type)
    {
        while (nanoTime() < time_ns)
        {
            if (inb(PORT_KB_STATUS) & MOUSE_BBIT) break;
        }
    }
    else
    {
        while (nanoTime() < time_ns)
        {
            if (!((inb(PORT_KB_STATUS) & MOUSE_ABIT))) break;
        }
    }
}

void mouse_write(uint8_t write)
{
    mouse_wait(1);
    outb(PORT_KB_CMD, KB_SEND2MOUSE);
    mouse_wait(1);
    outb(PORT_KB_DATA, write);
}

uint8_t mouse_read()
{
    mouse_wait(0);
    char t = inb(PORT_KB_DATA);
    return t;
}

static MouseType get_mouse_type()
{
    send_command(0xf3);
    send_command(200);

    send_command(0xf3);
    send_command(100);

    send_command(0xf3);
    send_command(80);

    send_command(0xf2);
    wait_ps2_read();
    uint8_t type0 = inb(PS2_DATA_PORT);
    return type0 == 0x3 ? OnlyScroll : (type0 == 0x4 ? FiveButton : Standard);
}

// 修正后的 mouse_init()
void mouse_init()
{

    // 2. 启用辅助PS/2端口（鼠标）
    wait_KB_write();
    outb(PORT_KB_CMD, 0xA8);

    // 3. 清空输出缓冲区
    inb(PORT_KB_DATA);

    // 4. 设置控制器命令字节
    wait_KB_write();
    outb(PORT_KB_CMD, 0x60); // 写命令字节命令
    wait_KB_write();

    // 启用IRQ1（键盘）和IRQ12（鼠标），保留其他位
    uint8_t cmd_byte = 0x47; // 0110 0111
    // 位7: 保留
    // 位6: 翻译键盘扫描码
    // 位5: 禁用鼠标
    // 位4: 禁用键盘
    // 位3: 保留
    // 位2: 系统标志
    // 位1: 启用鼠标IRQ
    // 位0: 启用键盘IRQ
    outb(PORT_KB_DATA, cmd_byte);

    delay_ms(20);

    // 5. 重置鼠标
    mouse_write(0xFF); // 重置命令
    delay_ms(100);

    // 读取ACK和自检结果
    uint8_t ack = mouse_read();
    if (ack != 0xFA)
    {
        printk("Mouse reset ACK failed: 0x%x\n", ack);
        return;
    }

    delay_ms(500);
    uint8_t self_test = mouse_read();
    if (self_test != 0xAA)
    {
        printk("Mouse self-test failed: 0x%x\n", self_test);
        return;
    }

    uint8_t mouse_id = mouse_read();
    if (mouse_id != 0x00) { printk("Mouse ID: 0x%x\n", mouse_id); }

    // 6. 设置默认参数
    mouse_write(0xF6); // 设置默认参数
    delay_ms(10);
    ack = mouse_read();
    if (ack != 0xFA)
    {
        printk("Set defaults ACK failed: 0x%x\n", ack);
        return;
    }

    // 7. 设置采样率（重要！实体机需要）
    mouse_write(0xF3); // 设置采样率
    delay_ms(10);
    ack = mouse_read();
    if (ack != 0xFA) return;

    mouse_write(200); // 200 samples/s keeps cursor motion smoother.
    delay_ms(10);
    ack = mouse_read();
    if (ack != 0xFA) return;

    // 8. 设置分辨率
    mouse_write(0xE8); // 设置分辨率
    delay_ms(10);
    ack = mouse_read();
    if (ack != 0xFA) return;

    mouse_write(0x03); // 8 counts/mm（中等分辨率）
    delay_ms(10);
    ack = mouse_read();
    if (ack != 0xFA) return;

    // 9. 设置缩放（1:1）
    mouse_write(0xE6); // 设置缩放1:1
    delay_ms(10);
    ack = mouse_read();
    if (ack != 0xFA) return;

    // 10. 启用数据报告
    mouse_write(0xF4);
    delay_ms(10);
    ack = mouse_read();
    if (ack != 0xFA) return;

    // 11. 识别鼠标类型（支持滚轮检测）
    mouse_write(0xF2); // 获取设备ID
    delay_ms(10);
    ack = mouse_read();
    if (ack != 0xFA) return;

    uint8_t device_id = mouse_read();
    if (device_id == 0x00)
    {
        type = Standard;
        printk("Standard PS/2 mouse detected\n");
    }
    else if (device_id == 0x03)
    {
        type = OnlyScroll;
        printk("Scroll wheel mouse detected\n");
    }
    else if (device_id == 0x04)
    {
        type = FiveButton;
        printk("5-button mouse detected\n");
    }
    else
    {
        type = Standard;
        printk("Unknown mouse ID 0x%x, assuming standard\n", device_id);
    }

    // 12. 重新启用数据报告（某些鼠标需要）
    mouse_write(0xF4);
    delay_ms(10);
    ack = mouse_read();
    if (ack != 0xFA) return;

    // 13. 清空可能的数据包
    while (inb(PORT_KB_STATUS) & 0x01)
    {
        inb(PORT_KB_DATA);
    }

    // 14. 初始化状态变量
    mouse_x              = (get_hor() - 16) / 2;
    mouse_y              = (get_ver() - 28 - 29) / 2;
    // 所有初始化命令的 ACK 都已经在 mouse_init() 内同步读走了，
    // 这里应当直接进入首字节等待状态，否则运行期数据包会被一直丢掉。
    ms_dec.phase         = 1;
    ms_dec.win_move_lock = false;
    ms_dec.win_resize_lock = false;
    ms_dec.sht_now       = NULL;
    ms_dec.need_flush    = false;
    ms_dec.last_btn      = ms_dec.btn;
    ms_dec.last_mouse_x  = mouse_x;
    ms_dec.last_mouse_y  = mouse_y;
    ms_dec.win_move_pending = false;
    ms_dec.win_resize_pending = false;
    ms_dec.pending_win_x    = mouse_x;
    ms_dec.pending_win_y    = mouse_y;
    ms_dec.last_win_move_ns = 0;
    ms_dec.last_win_resize_ns = 0;
    ms_dec.pending_btn_down = 0;
    ms_dec.pending_btn_up   = 0;
    ms_dec.first_click   = false;
    ms_dec.double_click  = false;
    ms_dec.left_release_pending = false;
    ms_dec.left          = false;
    ms_dec.left_release_window  = NULL;
    ms_dec.right         = false;
    ms_dec.center        = false;
    ms_dec.scroll        = 0;
    ms_dec.resize_edges  = 0;
    ms_dec.cursor_shape  = MouseCursorStandard;

    printk("Mouse initialized successfully, type: %d\n", type);
}

// void mouse_init()
// {
//     wait_KB_write();
//     outb(PORT_KB_CMD, KB_EN_MOUSE_INTFACE);

//     delay_ms(100);

//     wait_KB_write();
//     outb(PORT_KB_CMD, KB_SEND2MOUSE);
//     wait_KB_write();
//     outb(PORT_KB_DATA, MOUSE_EN);

//     delay_ms(100);

//     wait_KB_write();
//     outb(PORT_KB_CMD, KBCMD_WRITE_CMD);
//     wait_KB_write();
//     outb(PORT_KB_DATA, KB_INIT_MODE);

//     mouse_x      = (get_hor() - 16) / 2;
//     mouse_y      = (get_ver() - 28 - 29) / 2;
//     ms_dec.phase = 0;

//     ms_dec.win_move_lock = false;
//     ms_dec.sht_now       = NULL;
// }

void draw_mouse(SHEET_INFO *sht, SHEET *csheet)
{
    ms_dec.cursor_shape = MouseCursorStandard;
    redraw_mouse_cursor(sht, csheet, MouseCursorStandard);
}

void move_mouse(SHEET_INFO *sht, SHEET *csheet, int px, int py)
{
    setBX(sht, csheet, px);
    setBY(sht, csheet, py);
}
