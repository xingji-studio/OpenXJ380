#include <x3api.h>
#include <libsys.h>
#include <krlibc.h>
#include "fm_proto.h"

#define FM_DOUBLE_CLICK_NS 1000000000ULL
#define FM_CLOCK_MONOTONIC 1

typedef struct {
    UINT64 tv_sec;
    UINT64 tv_nsec;
} fm_timespec;

static UINT64 get_click_time_ns()
{
    fm_timespec ts;
    memset(&ts, 0, sizeof(ts));
    enter_syscall(SYS_CLOCK_GETTIME, FM_CLOCK_MONOTONIC, (UINT64)&ts, 0, 0, 0, 0);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

bool first_click = false;
bool double_click = false;
int double_click_file_index = -1;
UINT64 clicktime_recorder = 0;
int last_click_file_index = -1;

int click_x = 0;
int click_y = 0;

int choosing_index = -1;
int choosing_history = -1;

volatile bool register_lock = false;
bool yicixing_lock = false;

bool need_paint = false;

static const char *fm_sidebar_paths[] = {"/", "/users", "/apps", "/system", "/dev"};
static const int fm_sidebar_path_count = sizeof(fm_sidebar_paths) / sizeof(fm_sidebar_paths[0]);

static void set_scroll_base(int base)
{
    int old_base = file_count_base;
    file_count_base = base;
    fm_clamp_scroll_base();
    if (file_count_base != old_base)
    {
        choosing_index = -1;
        need_paint = true;
    }
}

static bool get_clicked_file_index(int x, int y, int *file_index, int *visible_index)
{
    int left = fm_sidebar_width() + 4;
    int right = need_duopage ? fmr_width - 22 : fmr_width - 1;
    if (x < left || x > right || y < 148)
    {
        return false;
    }

    int current_visible_index = (y - 148) / 24;
    int visible_count = file_count - file_count_base;
    int visible_rows = fm_visible_rows();
    if (visible_count > visible_rows)
    {
        visible_count = visible_rows;
    }

    if (visible_count <= 0 || current_visible_index < 0 || current_visible_index >= visible_count)
    {
        return false;
    }

    *visible_index = current_visible_index;
    *file_index = file_count_base + current_visible_index;
    return true;
}

bool fm_handle_sidebar_click(int x, int y)
{
    int sidebar_w = fm_sidebar_width();
    if (sidebar_w <= 0 || x < 0 || x > sidebar_w || y < 148) return false;
    int index = (y - 148) / 34;
    if (index < 0 || index >= fm_sidebar_path_count) return false;

    strcpy(current_path, fm_sidebar_paths[index]);
    file_count_base = 0;
    choosing_index = -1;
    first_click = false;
    double_click = false;
    need_paint = true;
    return true;
}

static bool point_in_rect(int x, int y, int x1, int y1, int x2, int y2)
{
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}

void register_scrollbar_click(int x, int y)
{
    if (!need_duopage) return;

    int bar_x1, bar_y1, bar_x2, bar_y2;
    if (!fm_scrollbar_rect(&bar_x1, &bar_y1, &bar_x2, &bar_y2)) return;
    if (!point_in_rect(x, y, bar_x1, bar_y1, bar_x2, bar_y2)) return;

    int track_h = bar_y2 - bar_y1;
    int max_base = fm_max_scroll_base();
    if (track_h <= 0 || max_base <= 0) return;

    int target = ((y - bar_y1) * max_base + track_h / 2) / track_h;
    set_scroll_base(target);
}

void register_scrollbar_position(int position)
{
    if (!need_duopage) return;

    int bar_x1, bar_y1, bar_x2, bar_y2;
    if (!fm_scrollbar_rect(&bar_x1, &bar_y1, &bar_x2, &bar_y2)) return;

    int track_h = bar_y2 - bar_y1;
    int max_base = fm_max_scroll_base();
    if (track_h <= 0 || max_base <= 0) return;
    if (position < 0) position = 0;
    if (position > track_h) position = track_h;

    int target = (position * max_base + track_h / 2) / track_h;
    set_scroll_base(target);
}

void register_scroll_wheel(int delta)
{
    if (!need_duopage || delta == 0) return;
    set_scroll_base(file_count_base - delta * 3);
}

void fm_select_item_at(int x, int y)
{
    int current_file_index = -1;
    int current_visible_index = -1;
    if (!get_clicked_file_index(x, y, &current_file_index, &current_visible_index))
    {
        return;
    }

    choosing_history = choosing_index;
    choosing_index = current_visible_index;
    first_click = false;
    double_click = false;
    double_click_file_index = -1;
    last_click_file_index = current_file_index;
    if (choosing_history != choosing_index)
    {
        refresh_choose_change(choosing_history, choosing_index);
    }
}

void reg_lock()
{
    while (register_lock);
    register_lock = true;
    need_paint = false;
}

void reg_unlock()
{
    register_lock = false;
}

void register_click(int x, int y)
{
    if (register_lock) return;

    if (yicixing_lock)
    {
        yicixing_lock = false;
        return;
    }
    
    click_x = x;
    click_y = y;

    if (fm_handle_sidebar_click(x, y))
    {
        return;
    }

    int  current_file_index = -1;
    int  current_visible_index = -1;
    bool click_on_file = get_clicked_file_index(x, y, &current_file_index, &current_visible_index);
    UINT64 current_time = get_click_time_ns();

    if (click_on_file && first_click &&
        current_time - clicktime_recorder < FM_DOUBLE_CLICK_NS &&
        current_file_index == last_click_file_index)
    {
        double_click = true;
        double_click_file_index = current_file_index;
        first_click = false;
    }
    else
    {
        double_click = false;
        double_click_file_index = -1;
        first_click = click_on_file;
        clicktime_recorder = click_on_file ? current_time : 0;
    }

    last_click_file_index = click_on_file ? current_file_index : -1;

    if (x >= 5 && x <= 23 && y >= 4 && y <= 22)
    {
        revert_path(current_path);
        need_paint = true;
    }
    else if (x >= 29 && x <= 47 && y >= 4 && y <= 22)
    {
        if (path_p != path_r)
        {
            if (path_p == 0) { path_p = 19; }
            else { path_p--; }
            strcpy(current_path, path_his[path_p]);

            need_paint = true;
        }
    }
    
    register_scrollbar_click(x, y);

    if (click_on_file)
    {
        if (double_click)
        {
            choosing_history = choosing_index;
            choosing_index = current_visible_index;
            return;
        }

        choosing_history = choosing_index;
        choosing_index = current_visible_index;
        if (choosing_history != choosing_index)
        {
            refresh_choose_change(choosing_history, choosing_index);
        }
    }
}
