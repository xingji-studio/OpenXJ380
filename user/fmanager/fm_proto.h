#pragma once

#include <xj380_i18n.h>

// 文件管理器窗口尺寸
#define FMR_X 800
#define FMR_Y 450

struct FILE_TYPEINFO_INDEX
{
    char file_type[32];
    char icon_path[64];
    char file_type_name[64];
};

extern HDLE handle;
extern int fmr_width;
extern int fmr_height;
extern char current_path[1024];
extern FILE_TYPEINFO_INDEX file_ti_index[2048];

extern bool double_click;
extern int double_click_file_index;

extern int click_x;
extern int click_y;

extern char path_his[20][1024];
extern int path_p;
extern int path_r;

extern bool need_duopage;
extern int file_count_base;
extern int file_count;
extern DirNode file_dir_cache[256];

extern int choosing_index;

extern bool need_paint;

extern volatile bool register_lock;
extern bool yicixing_lock;

enum FmCommandId
{
    FM_CMD_NEW_FOLDER = 1,
    FM_CMD_RENAME = 2,
    FM_CMD_DELETE = 3,
    FM_CMD_COPY = 4,
    FM_CMD_CUT = 5,
    FM_CMD_PASTE = 6,
    FM_CMD_PROPERTIES = 7,
    FM_CMD_SCROLLBAR = 1000,
};

void paint_fm_back();
void paint_dir(char *path);
void refresh_fm_part(int x1, int y1, int x2, int y2);
void refresh_choose_change(int old_visible_index, int new_visible_index);
bool get_file_type(char *name, char *type);
void register_click(int x, int y);
bool fm_handle_sidebar_click(int x, int y);
void cat_path(char *folder_name);
void revert_path(char *path);
void record_current_path();
void process_crl(int CRLid, UINT64 data);
void fm_finish_inline_rename();
void fm_show_error(const char *title, const char *detail);
void fm_show_info(const char *title, const char *detail);
void fm_show_properties();
void fm_copy_selected(bool cut);
void fm_paste_clipboard();
bool fm_selected_path(char *out, int out_size, DirNode *out_node);
void fm_select_item_at(int x, int y);
char *fm_tr(const char *zh_cn, const char *en_us);

void reg_lock();
void reg_unlock();

int fm_visible_rows();
int fm_sidebar_width();
int fm_item_name_x();
int fm_item_name_y(int visible_index);
int fm_item_name_width();
int fm_max_scroll_base();
void fm_clamp_scroll_base();
bool fm_scrollbar_rect(int *x1, int *y1, int *x2, int *y2);
bool fm_scrollbar_thumb_rect(int *x1, int *y1, int *x2, int *y2);
bool fm_get_selected_file(DirNode *node);
void register_scrollbar_click(int x, int y);
void register_scrollbar_position(int position);
void register_scroll_wheel(int delta);
