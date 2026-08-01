#include <x3api.h>
#include <krlibc.h>
#include "fm_proto.h"
#include <xguiapi.h>
#include <xposix/stdlib.h>

char path[1024] = "/";

FILE_TYPEINFO_INDEX file_ti_index[2048];
DirNode file_dir_cache[256];

bool need_duopage = false;
int  file_count_base = 0;
int  file_count = 0;

static const int FM_CONTENT_TOP = 148;
static const int FM_ROW_HEIGHT = 24;
static const int FM_SIDEBAR_W = 132;

/*
static const char *xapi_demoSvgCheck =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 448 512\">"
    "<!--!Font Awesome Free v7.2.0 by @fontawesome - https://fontawesome.com License - "
    "https://fontawesome.com/license/free Copyright 2026 Fonticons, Inc.-->"
    "<path d=\"M434.8 70.1c14.3 10.4 17.5 30.4 7.1 44.7l-256 352c-5.5 7.6-14 12.3-23.4 13.1s-18.5-2.7-25.1-9.3l-128-128c-12.5-12.5-12.5-32.8 0-45.3s32.8-12.5 45.3 0l101.5 101.5 234-321.7c10.4-14.3 30.4-17.5 44.7-7.1z\"/>"
    "</svg>";*/

int fm_visible_rows()
{
    int rows = (fmr_height - FM_CONTENT_TOP - 8) / FM_ROW_HEIGHT;
    return rows < 1 ? 1 : rows;
}

int fm_sidebar_width()
{
    return fmr_width >= 520 ? FM_SIDEBAR_W : 0;
}

int fm_max_scroll_base()
{
    int max_base = file_count - fm_visible_rows();
    return max_base < 0 ? 0 : max_base;
}

void fm_clamp_scroll_base()
{
    int max_base = fm_max_scroll_base();
    if (file_count_base < 0) file_count_base = 0;
    if (file_count_base > max_base) file_count_base = max_base;
}

bool fm_scrollbar_rect(int *x1, int *y1, int *x2, int *y2)
{
    if (!need_duopage) return false;
    if (fmr_width < 80 || fmr_height <= 164) return false;
    if (x1 != NULL) *x1 = fmr_width - 22;
    if (y1 != NULL) *y1 = FM_CONTENT_TOP;
    if (x2 != NULL) *x2 = fmr_width - 7;
    if (y2 != NULL) *y2 = fmr_height - 10;
    return true;
}

bool fm_scrollbar_thumb_rect(int *x1, int *y1, int *x2, int *y2)
{
    int bar_x1, bar_y1, bar_x2, bar_y2;
    if (!fm_scrollbar_rect(&bar_x1, &bar_y1, &bar_x2, &bar_y2)) return false;

    int visible_rows = fm_visible_rows();
    if (file_count <= visible_rows) return false;

    int track_h = bar_y2 - bar_y1;
    if (track_h <= 0) return false;
    int thumb_h = (track_h * visible_rows) / file_count;
    if (thumb_h < 28) thumb_h = track_h < 28 ? track_h : 28;
    if (thumb_h > track_h) thumb_h = track_h;

    int max_base = fm_max_scroll_base();
    int travel = track_h - thumb_h;
    int thumb_y = bar_y1;
    if (max_base > 0 && travel > 0)
    {
        thumb_y += (file_count_base * travel + max_base / 2) / max_base;
    }

    if (x1 != NULL) *x1 = bar_x1 + 2;
    if (y1 != NULL) *y1 = thumb_y;
    if (x2 != NULL) *x2 = bar_x2 - 2;
    if (y2 != NULL) *y2 = thumb_y + thumb_h;
    return true;
}

static bool visible_index_is_valid(int visible_index)
{
    int visible_count = file_count - file_count_base;
    int visible_rows = fm_visible_rows();
    if (visible_count > visible_rows)
    {
        visible_count = visible_rows;
    }

    return visible_index >= 0 && visible_index < visible_count;
}

static int fm_lower_ascii(int ch)
{
    if (ch >= 'A' && ch <= 'Z')
    {
        return ch + ('a' - 'A');
    }
    return ch;
}

static int fm_compare_filename(const char *a, const char *b)
{
    const char *original_a = a;
    const char *original_b = b;

    while (*a && *b)
    {
        int ca = fm_lower_ascii((unsigned char)*a);
        int cb = fm_lower_ascii((unsigned char)*b);
        if (ca != cb)
        {
            return ca - cb;
        }
        a++;
        b++;
    }

    int result = fm_lower_ascii((unsigned char)*a) - fm_lower_ascii((unsigned char)*b);
    return result == 0 ? strcmp(original_a, original_b) : result;
}

static int fm_compare_dirnode(const void *a, const void *b)
{
    const DirNode *left = (const DirNode *)a;
    const DirNode *right = (const DirNode *)b;

    if (left->filetype != right->filetype)
    {
        return left->filetype ? -1 : 1;
    }

    return fm_compare_filename(left->filename, right->filename);
}

static void fm_sort_file_cache()
{
    if (file_count > 1)
    {
        qsort(file_dir_cache, file_count, sizeof(file_dir_cache[0]), fm_compare_dirnode);
    }
}

bool fm_get_selected_file(DirNode *node)
{
    if (node == NULL || choosing_index < 0)
    {
        return false;
    }

    int file_index = choosing_index + file_count_base;
    if (file_index < 0 || file_index >= file_count)
    {
        return false;
    }

    *node = file_dir_cache[file_index];
    return true;
}

static int fm_list_left()
{
    return fm_sidebar_width() + 4;
}

static int fm_name_x()
{
    return fm_list_left() + 24;
}

static int fm_type_x()
{
    return fm_list_left() + 236;
}

static int fm_size_x()
{
    return fm_list_left() + 396;
}

static int fm_list_right()
{
    int right = need_duopage ? fmr_width - 22 : fmr_width - 1;
    if (right < fm_list_left() + 120) right = fm_list_left() + 120;
    return right;
}

int fm_item_name_x()
{
    return fm_name_x();
}

int fm_item_name_y(int visible_index)
{
    return FM_CONTENT_TOP + FM_ROW_HEIGHT * visible_index - 4;
}

int fm_item_name_width()
{
    int width = fm_type_x() - fm_name_x() - 8;
    return width < 80 ? 80 : width;
}

void refresh_fm_part(int x1, int y1, int x2, int y2)
{
    xapi_RefreshPartWindow(handle, x1 + 12, y1 + 27, x2 + 12, y2 + 27);
}

static bool path_is_prefix(const char *path, const char *prefix)
{
    size_t len = strlen(prefix);
    return strcmp(path, prefix) == 0 || (strncmp(path, prefix, len) == 0 && path[len] == '/');
}

static void paint_sidebar_item(int index, const char *label, const char *target)
{
    int x = 8;
    int y = FM_CONTENT_TOP + index * 34;
    int w = fm_sidebar_width() - 16;
    bool selected = strcmp(target, "/") == 0 ? strcmp(current_path, "/") == 0 : path_is_prefix(current_path, target);
    xapi_DrawRect(handle, x, y, x + w, y + 28, selected ? 0xe5f3ffff : 0xf8f8f8ff, true);
    xapi_DrawRect(handle, x, y, x + w, y + 28, selected ? 0x9cc7f4ff : 0xe4e4e4ff, false);
    xapi_DrawText(handle, x + 10, y + 7, (char *)label, 10, selected ? 0x005a9eff : 0x333333ff);
}

static void paint_sidebar()
{
    int width = fm_sidebar_width();
    if (width <= 0) return;
    xapi_DrawRect(handle, 0, 120, width, fmr_height - 1, 0xf8f8f8ff, true);
    xapi_DrawLine(handle, width, 120, width, fmr_height - 1, 0xd6d6d6ff);
    xapi_DrawText(handle, 12, 124, fm_tr("常用位置", "Locations"), 10, 0x333333ff);
    paint_sidebar_item(0, fm_tr("根目录", "Root"), "/");
    paint_sidebar_item(1, fm_tr("用户", "Users"), "/users");
    paint_sidebar_item(2, fm_tr("应用", "Apps"), "/apps");
    paint_sidebar_item(3, fm_tr("系统", "System"), "/system");
    paint_sidebar_item(4, fm_tr("磁盘/分区", "Disks"), "/dev");
}

void paint_fm_back()
{
    xapi_DeleteScrollBar(handle, FM_CMD_SCROLLBAR);

    xapi_DrawRect(handle, 0, 0 , fmr_width - 1, 25, 0xeaeaeaff, true);
    xapi_DrawRect(handle, 52, 4, fmr_width - 5, 22, 0xffffffff, true);
    xapi_DrawPNG(handle, 0, 26, fmr_width - 1, 94, "/system/resources/image/fm_bg.png");
    // xapi_DrawPNG(handle, 5, 4, 18, 18, "/system/icon/bleft.png");
    xapi_DrawFA(handle, 7, 6, 8, "chevron-left");
    xapi_DrawFA(handle, 31, 6, 8, "chevron-right");
    //xapi_DrawPNG(handle, 29, 4, 18, 18, "/system/icon/bright.png");
    xapi_DrawText(handle, 56, 2, current_path, 11, 0x000000ff);
    // xapi_DrawSvg(handle, FMR_X - 40, 30, 28, (char *)xapi_demoSvgCheck);
    if (need_duopage)
    {
        int bar_x1, bar_y1, bar_x2, bar_y2;
        int thumb_x1, thumb_y1, thumb_x2, thumb_y2;
        if (fm_scrollbar_rect(&bar_x1, &bar_y1, &bar_x2, &bar_y2) &&
            fm_scrollbar_thumb_rect(&thumb_x1, &thumb_y1, &thumb_x2, &thumb_y2))
        {
            xapi_PutVerticalScrollBar(handle, bar_x1, bar_y1, bar_y2 - bar_y1 + 1, thumb_y2 - thumb_y1 + 1,
                                      FM_CMD_SCROLLBAR);
            xapi_SetScrollBarPosition(handle, FM_CMD_SCROLLBAR, thumb_y1 - bar_y1);
        }
    }
}

void paint_item(int index, char *name, bool is_folder, int size, bool is_choosing)
{
    int left = fm_list_left();
    int y = FM_CONTENT_TOP + FM_ROW_HEIGHT * index;
    int right = fm_list_right();
    xapi_DrawRect(handle, left, y - 4, right, y + FM_ROW_HEIGHT - 4, 0xffffffff, true);
    if (is_choosing) xapi_DrawRect(handle, left, y - 4, right, y + FM_ROW_HEIGHT - 4, 0xe5f3ffff, true);

    xapi_DrawText(handle, fm_name_x(), y - 3, name, 10, 0x000000ff);
    if (is_folder)
    {
        xapi_DrawFA(handle, left + 4, y + 2, 16, "folder-open");
        // xapi_DrawPNG(handle, 8, 148 + 24 * index, 16, 16, "/system/icon/folder.png");
        xapi_DrawText(handle, fm_type_x(), y - 3, fm_tr("文件夹", "Folder"), 10, 0x6d6d6dff);
    }
    else
    {
        char file_type[32];
        memset(file_type, 0, sizeof(char) * 32);
        if (get_file_type(name, file_type))
        {
            if (strcmp("elf", file_type) == 0 || strcmp("epf", file_type) == 0)
            {
                xapi_DrawText(handle, fm_type_x(), y - 3, fm_tr("可执行文件", "Executable"), 10, 0x6d6d6dff);
                xapi_DrawFA(handle, left + 6, y + 2, 12, "gear");
            }
            else if (strcmp("txt", file_type) == 0)
            {
                xapi_DrawText(handle, fm_type_x(), y - 3, fm_tr("文本文件", "Text File"), 10, 0x6d6d6dff);
                xapi_DrawFA(handle, left + 6, y + 2, 12, "file-lines");
            }
            else if (strcmp("png", file_type) == 0)
            {
                xapi_DrawText(handle, fm_type_x(), y - 3, fm_tr("PNG 图片文件", "PNG Image"), 10, 0x6d6d6dff);
                xapi_DrawFA(handle, left + 6, y + 2, 12, "file-image");
            }
            else if (strcmp("jpg", file_type) == 0)
            {
                xapi_DrawText(handle, fm_type_x(), y - 3, fm_tr("JPEG 图片文件", "JPEG Image"), 10, 0x6d6d6dff);
                xapi_DrawFA(handle, left + 6, y + 2, 12, "file-image");
            }
            else if (strcmp("bmp", file_type) == 0)
            {
                xapi_DrawText(handle, fm_type_x(), y - 3, fm_tr("位图文件", "Bitmap Image"), 10, 0x6d6d6dff);
                xapi_DrawFA(handle, left + 6, y + 2, 12, "file-image");
            }
            else if (strcmp("svg", file_type) == 0)
            {
                xapi_DrawText(handle, fm_type_x(), y - 3, fm_tr("矢量图文件", "Vector Image"), 10, 0x6d6d6dff);
                xapi_DrawFA(handle, left + 6, y + 2, 12, "file-image");
            }
            else if (strcmp("md", file_type) == 0)
            {
                xapi_DrawText(handle, fm_type_x(), y - 3, "Markdown", 10, 0x6d6d6dff);
                xapi_DrawFA(handle, left + 4, y, 16, "markdown");
            }
            else
            {
                for (int i = 0; file_ti_index[i].file_type[0] != '\0'; i++)
                {
                    if (strcmp(file_type, file_ti_index[i].file_type) == 0)
                    {
                        // xapi_DrawPNG(handle, 8, 148 + 24 * index, 16, 16, "/system/icon/folder.png");
                        xapi_DrawText(handle, fm_type_x(), y - 3, file_ti_index[i].file_type_name, 10, 0x6d6d6dff);
                        goto type_is_found;
                    }
                }
                strcat(file_type, xj380_read_language() == XJ380_LANGUAGE_EN_US ? " file" : " 文件");
                xapi_DrawText(handle, fm_type_x(), y - 3, file_type, 10, 0x6d6d6dff);
                xapi_DrawFA(handle, left + 6, y + 2, 12, "file");
            }
        }
        else 
        {
            xapi_DrawText(handle, fm_type_x(), y - 3, fm_tr("未知类型文件", "Unknown File"), 10, 0x6d6d6dff);
            xapi_DrawFA(handle, left + 6, y + 2, 12, "file");
            // xapi_DrawPNG(handle, 8, 148 + 24 * index, 16, 16, "/system/icon/unknowfile.png");
        }
type_is_found:
        char file_size_str[32];
        memset(file_size_str, 0, sizeof(char) * 32);
        strcpy(file_size_str, xcr_int2char((size + 1023) / 1024));
        strcat(file_size_str, " KB");
        xapi_DrawText(handle, fm_size_x(), y - 3, file_size_str, 10, 0x6d6d6dff);
    }
}

static void refresh_visible_item(int visible_index)
{
    if (!visible_index_is_valid(visible_index))
    {
        return;
    }

    int file_index = file_count_base + visible_index;
    paint_item(visible_index,
        file_dir_cache[file_index].filename,
        file_dir_cache[file_index].filetype,
        file_dir_cache[file_index].length,
        (choosing_index == visible_index));
    refresh_fm_part(fm_list_left(), FM_CONTENT_TOP + FM_ROW_HEIGHT * visible_index - 4,
                    fmr_width - 1, FM_CONTENT_TOP + FM_ROW_HEIGHT * visible_index + 20);
}

void refresh_choose_change(int old_visible_index, int new_visible_index)
{
    if (register_lock)
    {
        return;
    }

    reg_lock();
    refresh_visible_item(old_visible_index);
    if (new_visible_index != old_visible_index)
    {
        refresh_visible_item(new_visible_index);
    }
    reg_unlock();
}

void paint_dir(char *path)
{
    if (register_lock) return;
    reg_lock();

    uint32_t count = 0;
    const int visible_rows = fm_visible_rows();
    memset(file_dir_cache, 0, sizeof(DirNode) * 256);
    xapi_DrawRect(handle, 0, 0, fmr_width - 1, fmr_height - 1, 0xffffffff, true);
    xapi_SearchFile(path, &count, file_dir_cache);
    file_count = (count == 404) ? 0 : (int)count;
    if (file_count > 255) file_count = 255;
    fm_sort_file_cache();

    if (file_count > visible_rows)
    {
        need_duopage = true;
        if (file_count_base + visible_rows > file_count)
        {
            file_count_base = file_count - visible_rows;
            fm_clamp_scroll_base();
        }
    }
    else 
    {
        need_duopage = false;
        file_count_base = 0;
    }

    paint_fm_back();
    paint_sidebar();
    
    xapi_DrawLine(handle, 0, 141, fmr_width - 1, 141, 0xc4c4c4ff);
    xapi_DrawText(handle, fm_list_left() + 12, 120, fm_tr("文件名称", "Name"), 10, 0x000000ff);
    xapi_DrawText(handle, fm_type_x(), 120, fm_tr("类型", "Type"), 10, 0x000000ff);
    xapi_DrawText(handle, fm_size_x(), 120, fm_tr("大小", "Size"), 10, 0x000000ff);
    UINT64 tmp_fmr_width = 0;
    UINT64 tmp_fmr_height = 0;
    xapi_GetWindowSize(handle, &tmp_fmr_width, &tmp_fmr_height);
    if (count == 0 || count == 404) 
    {
        // 不知道为什么居中会看起来偏右，那就往左偏一点吧（视觉错误这一块）
        xapi_DrawText(handle, tmp_fmr_width / 2 - 32, 154, fm_tr("空文件夹", "Empty Folder"), 10, 0x6d6d6dff);
    }

    for (int i = file_count_base; i < ((file_count > visible_rows + file_count_base) ? visible_rows + file_count_base : file_count); i++)
    {
        paint_item(i - file_count_base, 
            file_dir_cache[i].filename, 
            file_dir_cache[i].filetype, 
            file_dir_cache[i].length, 
            (choosing_index == i - file_count_base));
    }
    refresh_fm_part(0, 0, fmr_width - 1, fmr_height - 1);

    reg_unlock();
}
