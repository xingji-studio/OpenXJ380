#ifndef _WINDOW_H_
#define _WINDOW_H_

#include <task/pcb.h>

typedef struct WINDOWLS *WINDOWLSP;
typedef void (*MsgPrcor)(uint64_t Type, uint64_t hData,
                         uint64_t lData); // 别问为什么放在这儿 不放这儿会爆炸

#include <graphics/sheet.h>
#include <graphics/components/rb_menu.h>

typedef struct WINDOWLS
{
    SHEET  *w_sheet;
    CHAR8     title[256];
    uint16_t  number;
    uint32_t  width;
    uint32_t  height;
    MsgPrcor  WinMPf;
    tcb_t     w_task;
    rb_menu_regt_p w_menu;
    WINDOWLSP next;
    uint64_t  type;
    bool      is_maximized;
    int       restore_bx;
    int       restore_by;
    uint32_t  restore_width;
    uint32_t  restore_height;
    bool      can_maximize;
    bool      can_resize;
} WINDOWLS;

typedef struct WIN_THEME
{
    SHEET_BUFFER *top;
    SHEET_BUFFER *bottom;
    SHEET_BUFFER *left;
    SHEET_BUFFER *right;
    SHEET_BUFFER *close;
    SHEET_BUFFER *max;
    SHEET_BUFFER *min;
    SHEET_BUFFER *close_light;
    SHEET_BUFFER *max_light;
    SHEET_BUFFER *min_light;
} WIN_THEME;

typedef struct XWM_INFO
{
    uint16_t  win_num;
    WINDOWLS *start;
    WIN_THEME theme;
} XWM_INFO;

void init_xwm(XWM_INFO *xwmi);
bool create_window(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS **windowls, CHAR8 *title, uint32_t width, uint32_t height);
bool create_window_fmoff(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS **windowls, uint32_t width, uint32_t height);
bool create_window_fscr(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS **windowls);
bool create_window_login(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS **windowls);
bool create_window_desktop(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS **windowls);
bool create_window_dock(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS **windowls);
void change_title(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls, CHAR8 *title);
void draw_text(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls, CHAR8 *title, int x, int y, int size,
               SHEET_BUFFER color);
void draw_textl(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls, CHAR8 *title, int x, int y, int size,
                SHEET_BUFFER color, uint32_t *i_width);
void draw_text_sw(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls, CHAR8 *title, int x, int y, int size,
                  SHEET_BUFFER color);
void delete_window(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls);
void delete_process_windows(XWM_INFO *xwmi, SHEET_INFO *sht, pcb_t process);
bool window_contains(XWM_INFO *xwmi, WINDOWLS *windowls);
WINDOWLS *sht_found_win(XWM_INFO *xwmi, SHEET_INFO *sht, SHEET *csheet);
WINDOWLS *sht_found_win_by_type(XWM_INFO *xwmi, SHEET_INFO *sht, SHEET *csheet, uint64_t type);
WINDOWLS *find_window_by_exe_path(XWM_INFO *xwmi, const char *exe_path);
WINDOWLS *thread_found_win(XWM_INFO *xwmi, tcb_t thread);
WINDOWLS *mpf_found_win(MsgPrcor mpf);
bool alt_tab_preview_begin(XWM_INFO *xwmi, SHEET_INFO *sht);
bool alt_tab_preview_next(XWM_INFO *xwmi, SHEET_INFO *sht);
bool alt_tab_preview_commit(XWM_INFO *xwmi, SHEET_INFO *sht);
bool minimize_window(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls);
bool restore_and_focus_window(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls);
bool focus_window_by_exe_path(XWM_INFO *xwmi, SHEET_INFO *sht, const char *exe_path);
bool close_window_by_exe_path(XWM_INFO *xwmi, SHEET_INFO *sht, const char *exe_path);
bool close_window_and_task(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls);
bool toggle_show_desktop(XWM_INFO *xwmi, SHEET_INFO *sht);
bool toggle_window_maximized(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls);
void set_window_maximize_support(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls, bool can_maximize);
bool resize_window_sheet_preserving_content(XWM_INFO *xwmi, SHEET_INFO *sht, WINDOWLS *windowls,
                                            int bx, int by, uint32_t width, uint32_t height);

void resize_theme_width(SHEET_BUFFER *dst, SHEET_BUFFER *src, uint32_t width, uint32_t swidth, uint32_t height);
void resize_theme_height(SHEET_BUFFER *dst, SHEET_BUFFER *src, uint32_t width, uint32_t height, uint32_t sheight);
void resize_theme_unheight(SHEET_BUFFER *dst, SHEET_BUFFER *src, uint32_t width, uint32_t height, uint32_t sheight);

#endif
