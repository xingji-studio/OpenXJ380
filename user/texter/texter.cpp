#include <x3api.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <xj380_i18n.h>
#include "texter.h"

static const int TXR_APPBAR_H = 58;
static const int TXR_COMMAND_H = 44;
static const int TXR_STATUS_H = 28;
static const int TXR_MARGIN_X = 18;
static const int TXR_MARGIN_Y = 14;
static const int TXR_EDITOR_PAD_X = 10;
static const int TXR_EDITOR_PAD_Y = 8;
static const int TXR_GUTTER_W = 54;
static const int TXR_SCROLLBAR_W = 10;
static const int TXR_LINE_H = 18;
static const int TXR_CHAR_W = 8;
static const int TXR_MAX_PATH = 256;
static const int TXR_GROW_STEP = 4096;
static const UINT64 TXR_KEY_HOME = 151;
static const UINT64 TXR_KEY_UP = 152;
static const UINT64 TXR_KEY_PAGE_UP = 153;
static const UINT64 TXR_KEY_LEFT = 154;
static const UINT64 TXR_KEY_RIGHT = 155;
static const UINT64 TXR_KEY_END = 156;
static const UINT64 TXR_KEY_DOWN = 157;
static const UINT64 TXR_KEY_PAGE_DOWN = 158;
static const UINT64 TXR_KEY_INSERT = 159;
static const UINT64 TXR_KEY_DELETE = 160;

static const UINT32 TXR_COLOR_APPBAR = 0x0f6cbfff;
static const UINT32 TXR_COLOR_APPBAR_DARK = 0x074f91ff;
static const UINT32 TXR_COLOR_BG = 0xf3f7fbff;
static const UINT32 TXR_COLOR_COMMAND = 0xffffffff;
static const UINT32 TXR_COLOR_EDITOR = 0xffffffff;
static const UINT32 TXR_COLOR_GUTTER = 0xeef6ffff;
static const UINT32 TXR_COLOR_BORDER = 0xcbd9e6ff;
static const UINT32 TXR_COLOR_TEXT = 0x152433ff;
static const UINT32 TXR_COLOR_MUTED = 0x637184ff;
static const UINT32 TXR_COLOR_BLUE = 0x0f6cbfff;
static const UINT32 TXR_COLOR_BLUE_LIGHT = 0xd7ebffff;
static const UINT32 TXR_COLOR_BUTTON_BORDER = 0x9fc7ebff;
static const UINT32 TXR_COLOR_BUTTON_TEXT = 0x084a83ff;
static const UINT32 TXR_COLOR_SELECTION = 0xb8d8f8ff;
static const UINT32 TXR_COLOR_STATUS = 0xeaf2fbff;
static const UINT32 TXR_COLOR_WHITE = 0xffffffff;

enum {
    BTN_NEW = 101,
    BTN_OPEN = 102,
    BTN_SAVE = 103,
    BTN_SAVE_AS = 104
};

struct TexterButton
{
    UINT64 id;
    const char *label_zh;
    const char *label_en;
    int x;
    int y;
    int w;
    int h;
};

static TexterButton g_buttons[] = {
    {BTN_NEW, "新建", "New", 20, 0, 66, 28},
    {BTN_OPEN, "打开", "Open", 94, 0, 66, 28},
    {BTN_SAVE, "保存", "Save", 168, 0, 66, 28},
    {BTN_SAVE_AS, "另存为", "Save As", 242, 0, 78, 28},
};

static HDLE g_handle = 0;
static int g_width = TXR_X;
static int g_height = TXR_Y;
static bool g_need_redraw = true;
static bool g_need_layout_redraw = true;
static bool g_need_text_redraw = true;
static bool g_need_status_redraw = true;
static bool g_running = true;
static int g_language = XJ380_LANGUAGE_ZH_CN;

static char *g_text = NULL;
static int g_length = 0;
static int g_capacity = 0;
static int g_cursor = 0;
static int g_select_anchor = -1;
static int g_scroll_y = 0;
static int g_scroll_x = 0;
static bool g_dirty = false;

static char g_file_path[TXR_MAX_PATH] = {0};
static char g_status[160] = "就绪";

static int txr_min(int a, int b) { return a < b ? a : b; }
static int txr_max(int a, int b) { return a > b ? a : b; }

static char *txr_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(g_language, zh_cn, en_us);
}

static const char *txr_untitled()
{
    return txr_tr("未命名", "Untitled");
}

static const char *txr_untitled_document()
{
    return txr_tr("未命名文档", "Untitled Document");
}

static int txr_button_count()
{
    return (int)(sizeof(g_buttons) / sizeof(g_buttons[0]));
}

static void set_status(const char *text)
{
    if (text == NULL) text = "";
    strncpy(g_status, text, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    g_need_status_redraw = true;
    g_need_redraw = true;
}

static void update_title()
{
    g_language = xj380_read_language();
    if (g_handle == 0) return;
    char title[320];
    const char *path = g_file_path[0] ? g_file_path : txr_untitled();
    snprintf(title, sizeof(title), "%s%s - %s", path, g_dirty ? " *" : "", txr_tr("文本编辑器", "Text Editor"));
    xapi_SetWindowTitle(g_handle, title);
}

static bool ensure_capacity(int need)
{
    if (need <= g_capacity) return true;
    int new_cap = g_capacity > 0 ? g_capacity : TXR_GROW_STEP;
    while (new_cap < need) new_cap += TXR_GROW_STEP;
    char *new_buf = (char *)realloc(g_text, (size_t)new_cap);
    if (new_buf == NULL) {
        set_status(txr_tr("内存不足", "Not enough memory"));
        return false;
    }
    g_text = new_buf;
    g_capacity = new_cap;
    return true;
}

static void mark_dirty(bool dirty)
{
    bool dirty_changed = g_dirty != dirty;
    g_dirty = dirty;
    update_title();
    g_need_status_redraw = true;
    if (dirty_changed) g_need_layout_redraw = true;
    g_need_redraw = true;
}

static void clear_document()
{
    if (!ensure_capacity(1)) return;
    g_text[0] = '\0';
    g_length = 0;
    g_cursor = 0;
    g_select_anchor = -1;
    g_scroll_y = 0;
    g_scroll_x = 0;
    mark_dirty(false);
    g_need_text_redraw = true;
}

static bool save_to_path(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        set_status(txr_tr("保存路径为空", "Save path is empty"));
        return false;
    }

    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        set_status(txr_tr("无法打开文件保存", "Cannot open file for saving"));
        return false;
    }

    int wrote = 0;
    while (wrote < g_length) {
        int ret = write(fd, g_text + wrote, (size_t)(g_length - wrote));
        if (ret <= 0) {
            close(fd);
            set_status(txr_tr("写入失败", "Write failed"));
            return false;
        }
        wrote += ret;
    }
    close(fd);

    strncpy(g_file_path, path, sizeof(g_file_path) - 1);
    g_file_path[sizeof(g_file_path) - 1] = '\0';
    mark_dirty(false);
    set_status(txr_tr("已保存", "Saved"));
    return true;
}

static bool load_from_path(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        set_status(txr_tr("打开路径为空", "Open path is empty"));
        return false;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_status(txr_tr("无法打开文件", "Cannot open file"));
        return false;
    }

    struct stat st;
    int expected = 0;
    if (fstat(fd, &st) == 0 && st.st_size > 0) expected = (int)st.st_size;
    if (!ensure_capacity(expected + 1)) {
        close(fd);
        return false;
    }

    int total = 0;
    while (true) {
        if (total + 1024 >= g_capacity && !ensure_capacity(total + 1025)) {
            close(fd);
            return false;
        }
        int got = read(fd, g_text + total, (size_t)(g_capacity - total - 1));
        if (got < 0) {
            close(fd);
            set_status(txr_tr("读取失败", "Read failed"));
            return false;
        }
        if (got == 0) break;
        total += got;
    }
    close(fd);

    int normalized = 0;
    for (int i = 0; i < total; ++i) {
        if (g_text[i] == '\r') {
            if (i + 1 < total && g_text[i + 1] == '\n') continue;
            g_text[normalized++] = '\n';
            continue;
        }
        g_text[normalized++] = g_text[i];
    }

    g_text[normalized] = '\0';
    g_length = normalized;
    g_cursor = 0;
    g_select_anchor = -1;
    g_scroll_x = 0;
    g_scroll_y = 0;
    strncpy(g_file_path, path, sizeof(g_file_path) - 1);
    g_file_path[sizeof(g_file_path) - 1] = '\0';
    mark_dirty(false);
    set_status(txr_tr("已打开文件", "File opened"));
    g_need_text_redraw = true;
    return true;
}

static bool run_file_dialog(UINT64 mode, const char *title, char *path, int path_size)
{
    char result[TXR_MAX_PATH];
    memset(result, 0, sizeof(result));
    UINT64 ret = xapi_FileDialog(mode, (char *)title, path != NULL ? path : (char *)"", result, sizeof(result));
    if ((INT64)ret < 0) {
        set_status(txr_tr("文件对话框调用失败", "File dialog failed"));
        return false;
    }
    if (result[0] == '\0') {
        set_status(txr_tr("已取消", "Canceled"));
        return false;
    }

    strncpy(path, result, (size_t)path_size - 1);
    path[path_size - 1] = '\0';
    return true;
}

static bool do_save(bool save_as)
{
    char path[TXR_MAX_PATH];
    memset(path, 0, sizeof(path));
    if (!save_as && g_file_path[0] != '\0') {
        strncpy(path, g_file_path, sizeof(path) - 1);
    } else {
        strncpy(path, g_file_path[0] ? g_file_path : "/users", sizeof(path) - 1);
        if (!run_file_dialog(XAPI_FILE_DIALOG_SAVE, txr_tr("另存为", "Save As"), path, sizeof(path))) return false;
    }
    return save_to_path(path);
}

static bool do_open()
{
    char path[TXR_MAX_PATH];
    strncpy(path, g_file_path[0] ? g_file_path : "/users", sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    if (!run_file_dialog(XAPI_FILE_DIALOG_OPEN, txr_tr("打开文件", "Open File"), path, sizeof(path))) return false;
    return load_from_path(path);
}

static void do_new()
{
    g_file_path[0] = '\0';
    clear_document();
    set_status(txr_tr("新建文件", "New file"));
}

static int visible_rows()
{
    int h = g_height - TXR_APPBAR_H - TXR_COMMAND_H - TXR_STATUS_H - TXR_MARGIN_Y * 2 - TXR_EDITOR_PAD_Y * 2;
    int rows = h / TXR_LINE_H;
    return rows < 1 ? 1 : rows;
}

static int visible_cols()
{
    int w = g_width - TXR_MARGIN_X * 2 - TXR_GUTTER_W - TXR_SCROLLBAR_W - TXR_EDITOR_PAD_X * 2;
    int cols = w / TXR_CHAR_W;
    return cols < 1 ? 1 : cols;
}

static void normalize_selection(int *start, int *end)
{
    if (g_select_anchor < 0 || g_select_anchor == g_cursor) {
        *start = -1;
        *end = -1;
        return;
    }
    *start = txr_min(g_select_anchor, g_cursor);
    *end = txr_max(g_select_anchor, g_cursor);
}

static void delete_selection()
{
    int sel_start;
    int sel_end;
    normalize_selection(&sel_start, &sel_end);
    if (sel_start < 0) return;

    memmove(g_text + sel_start, g_text + sel_end, (size_t)(g_length - sel_end + 1));
    g_length -= sel_end - sel_start;
    g_cursor = sel_start;
    g_select_anchor = -1;
    mark_dirty(true);
}

static void insert_bytes(const char *text, int len)
{
    if (text == NULL || len <= 0) return;
    delete_selection();
    if (!ensure_capacity(g_length + len + 1)) return;
    memmove(g_text + g_cursor + len, g_text + g_cursor, (size_t)(g_length - g_cursor + 1));
    memcpy(g_text + g_cursor, text, (size_t)len);
    g_cursor += len;
    g_length += len;
    mark_dirty(true);
}

static void delete_backward()
{
    int sel_start;
    int sel_end;
    normalize_selection(&sel_start, &sel_end);
    if (sel_start >= 0) {
        delete_selection();
        return;
    }
    if (g_cursor <= 0) return;
    memmove(g_text + g_cursor - 1, g_text + g_cursor, (size_t)(g_length - g_cursor + 1));
    g_cursor--;
    g_length--;
    mark_dirty(true);
}

static void cursor_line_col(int pos, int *line, int *col)
{
    int l = 0;
    int c = 0;
    if (pos < 0) pos = 0;
    if (pos > g_length) pos = g_length;
    for (int i = 0; i < pos; ++i) {
        if (g_text[i] == '\n') {
            l++;
            c = 0;
        } else {
            c++;
        }
    }
    *line = l;
    *col = c;
}

static int position_from_line_col(int target_line, int target_col)
{
    int line = 0;
    int col = 0;
    for (int i = 0; i < g_length; ++i) {
        if (line == target_line && col == target_col) return i;
        if (g_text[i] == '\n') {
            if (line == target_line) return i;
            line++;
            col = 0;
        } else {
            col++;
        }
    }
    return g_length;
}

static void ensure_cursor_visible()
{
    int line;
    int col;
    cursor_line_col(g_cursor, &line, &col);
    int rows = visible_rows();
    int cols = visible_cols();

    if (line < g_scroll_y) g_scroll_y = line;
    if (line >= g_scroll_y + rows) g_scroll_y = line - rows + 1;
    if (col < g_scroll_x) g_scroll_x = col;
    if (col >= g_scroll_x + cols) g_scroll_x = col - cols + 1;
    if (g_scroll_y < 0) g_scroll_y = 0;
    if (g_scroll_x < 0) g_scroll_x = 0;
}

static void move_cursor_to(int pos, bool keep_selection)
{
    if (pos < 0) pos = 0;
    if (pos > g_length) pos = g_length;
    if (!keep_selection) g_select_anchor = -1;
    else if (g_select_anchor < 0) g_select_anchor = g_cursor;
    g_cursor = pos;
    ensure_cursor_visible();
}

static int command_top() { return TXR_APPBAR_H; }
static int editor_frame_top() { return TXR_APPBAR_H + TXR_COMMAND_H + TXR_MARGIN_Y; }
static int editor_frame_bottom() { return g_height - TXR_STATUS_H - TXR_MARGIN_Y; }
static int editor_frame_left() { return TXR_MARGIN_X; }
static int editor_frame_right() { return g_width - TXR_MARGIN_X - 1; }
static int text_area_top() { return editor_frame_top() + TXR_EDITOR_PAD_Y; }
static int text_area_bottom() { return editor_frame_bottom() - TXR_EDITOR_PAD_Y; }
static int text_area_left() { return editor_frame_left() + TXR_GUTTER_W + TXR_EDITOR_PAD_X; }
static int text_area_right() { return editor_frame_right() - TXR_SCROLLBAR_W - TXR_EDITOR_PAD_X; }
static int gutter_left() { return editor_frame_left(); }
static int gutter_right() { return editor_frame_left() + TXR_GUTTER_W - 1; }
static int scrollbar_left() { return editor_frame_right() - TXR_SCROLLBAR_W + 1; }
static int scrollbar_right() { return editor_frame_right(); }

static void request_text_redraw()
{
    g_need_text_redraw = true;
    g_need_status_redraw = true;
    g_need_redraw = true;
}

static void draw_command_button(const TexterButton &button)
{
    int y = command_top() + button.y + 8;
    xapi_DrawRect(g_handle, button.x, y, button.x + button.w, y + button.h, TXR_COLOR_BLUE_LIGHT, true);
    xapi_DrawLine(g_handle, button.x, y, button.x + button.w, y, TXR_COLOR_BUTTON_BORDER);
    xapi_DrawLine(g_handle, button.x, y + button.h, button.x + button.w, y + button.h, TXR_COLOR_BUTTON_BORDER);
    xapi_DrawLine(g_handle, button.x, y, button.x, y + button.h, TXR_COLOR_BUTTON_BORDER);
    xapi_DrawLine(g_handle, button.x + button.w, y, button.x + button.w, y + button.h, TXR_COLOR_BUTTON_BORDER);
    xapi_DrawSWText(g_handle, button.x + 15, y + 7,
                    xj380_tr_lang(g_language, button.label_zh, button.label_en), TXR_COLOR_BUTTON_TEXT);
}

static UINT64 hit_command_button(int x, int y)
{
    for (int i = 0; i < txr_button_count(); ++i) {
        const TexterButton &button = g_buttons[i];
        int by = command_top() + button.y + 8;
        if (x >= button.x && x <= button.x + button.w && y >= by && y <= by + button.h) return button.id;
    }
    return 0;
}

static void render_layout()
{
    g_language = xj380_read_language();
    xapi_DrawRect(g_handle, 0, 0, g_width - 1, g_height - 1, TXR_COLOR_BG, true);
    xapi_DrawRect(g_handle, 0, 0, g_width - 1, TXR_APPBAR_H - 1, TXR_COLOR_APPBAR, true);
    xapi_DrawRect(g_handle, 0, TXR_APPBAR_H - 3, g_width - 1, TXR_APPBAR_H - 1, TXR_COLOR_APPBAR_DARK, true);
    xapi_DrawRect(g_handle, 0, command_top(), g_width - 1, command_top() + TXR_COMMAND_H - 1, TXR_COLOR_COMMAND, true);
    xapi_DrawLine(g_handle, 0, command_top() + TXR_COMMAND_H - 1, g_width - 1, command_top() + TXR_COMMAND_H - 1,
                  TXR_COLOR_BORDER);
    xapi_DrawRect(g_handle, editor_frame_left(), editor_frame_top(), editor_frame_right(), editor_frame_bottom(),
                  TXR_COLOR_EDITOR, true);
    xapi_DrawRect(g_handle, editor_frame_left(), editor_frame_top(), gutter_right(), editor_frame_bottom(), TXR_COLOR_GUTTER,
                  true);
    xapi_DrawLine(g_handle, gutter_right(), editor_frame_top(), gutter_right(), editor_frame_bottom(), TXR_COLOR_BORDER);
    xapi_DrawRect(g_handle, scrollbar_left(), editor_frame_top(), scrollbar_right(), editor_frame_bottom(), 0xe7eef6ff,
                  true);
    xapi_DrawRect(g_handle, 0, g_height - TXR_STATUS_H, g_width - 1, g_height - 1, TXR_COLOR_STATUS, true);
    xapi_DrawLine(g_handle, 0, g_height - TXR_STATUS_H, g_width - 1, g_height - TXR_STATUS_H, TXR_COLOR_BORDER);

    xapi_DrawSWText(g_handle, 20, 12, txr_tr("文本编辑器", "Text Editor"), TXR_COLOR_WHITE);
    xapi_DrawSWText(g_handle, 20, 34, g_file_path[0] ? g_file_path : (char *)txr_untitled_document(), 0xd8ecffff);

    for (int i = 0; i < txr_button_count(); ++i) {
        draw_command_button(g_buttons[i]);
    }
    xapi_DrawSWText(g_handle, 350, command_top() + 13, txr_tr("UTF-8 纯文本", "UTF-8 Plain Text"), TXR_COLOR_MUTED);

    g_need_layout_redraw = false;
    g_need_text_redraw = true;
    g_need_status_redraw = true;
}

static void render_text_area()
{
    xapi_DrawRect(g_handle, editor_frame_left(), editor_frame_top(), editor_frame_right(), editor_frame_bottom(),
                  TXR_COLOR_EDITOR, true);
    xapi_DrawRect(g_handle, gutter_left(), editor_frame_top(), gutter_right(), editor_frame_bottom(), TXR_COLOR_GUTTER,
                  true);
    xapi_DrawLine(g_handle, gutter_right(), editor_frame_top(), gutter_right(), editor_frame_bottom(), TXR_COLOR_BORDER);
    xapi_DrawRect(g_handle, scrollbar_left(), editor_frame_top(), scrollbar_right(), editor_frame_bottom(), 0xe7eef6ff,
                  true);

    int sel_start;
    int sel_end;
    normalize_selection(&sel_start, &sel_end);

    int line_no = 0;
    int col = 0;
    bool cursor_drawn = false;
    int rows = visible_rows();
    int cols = visible_cols();
    int line_number = g_scroll_y + 1;

    for (int row = 0; row < rows; ++row) {
        int y = text_area_top() + row * TXR_LINE_H;
        if (y + TXR_LINE_H > text_area_bottom()) break;
        char line_label[16];
        snprintf(line_label, sizeof(line_label), "%d", line_number + row);
        xapi_DrawSWText(g_handle, gutter_left() + 10, y, line_label, TXR_COLOR_MUTED);
    }

    for (int i = 0; i <= g_length; ++i) {
        bool end = i == g_length;
        char ch = end ? '\0' : g_text[i];

        if (line_no >= g_scroll_y && line_no < g_scroll_y + rows) {
            int visible_col = col - g_scroll_x;
            if (!end && ch != '\n' && visible_col >= 0 && visible_col < cols) {
                int x = text_area_left() + visible_col * TXR_CHAR_W;
                int y = text_area_top() + (line_no - g_scroll_y) * TXR_LINE_H;
                if (sel_start >= 0 && i >= sel_start && i < sel_end) {
                    xapi_DrawRect(g_handle, x - 1, y, x + TXR_CHAR_W, y + TXR_LINE_H - 1, TXR_COLOR_SELECTION, true);
                }
                char buf[2] = {ch, '\0'};
                xapi_DrawSWText(g_handle, x, y, buf, TXR_COLOR_TEXT);
            }
            if (i == g_cursor && !cursor_drawn) {
                int cursor_x = text_area_left() + (col - g_scroll_x) * TXR_CHAR_W;
                int cursor_y = text_area_top() + (line_no - g_scroll_y) * TXR_LINE_H;
                if (cursor_x < text_area_left()) cursor_x = text_area_left();
                if (cursor_x > text_area_right()) cursor_x = text_area_right();
                xapi_DrawLine(g_handle, cursor_x, cursor_y, cursor_x, cursor_y + TXR_LINE_H - 2, TXR_COLOR_BLUE);
                cursor_drawn = true;
            }
        }

        if (end) break;
        if (ch == '\n') {
            line_no++;
            col = 0;
        } else {
            col++;
        }
    }

    if (!cursor_drawn) {
        ensure_cursor_visible();
    }

    int total_lines = 1;
    for (int i = 0; i < g_length; ++i) {
        if (g_text[i] == '\n') total_lines++;
    }
    if (total_lines > rows) {
        int track_top = editor_frame_top() + 8;
        int track_bottom = editor_frame_bottom() - 8;
        int track_h = txr_max(1, track_bottom - track_top);
        int thumb_h = txr_max(24, track_h * rows / total_lines);
        int max_scroll = txr_max(1, total_lines - rows);
        int thumb_y = track_top + (track_h - thumb_h) * g_scroll_y / max_scroll;
        xapi_DrawRect(g_handle, scrollbar_left() + 2, thumb_y, scrollbar_right() - 2, thumb_y + thumb_h, TXR_COLOR_BLUE,
                      true);
    }

    g_need_text_redraw = false;
    xapi_RefreshPartWindow(g_handle, editor_frame_left(), editor_frame_top(), editor_frame_right(), editor_frame_bottom());
}

static void render_status_bar()
{
    g_language = xj380_read_language();
    xapi_DrawRect(g_handle, 0, g_height - TXR_STATUS_H, g_width - 1, g_height - 1, TXR_COLOR_STATUS, true);
    xapi_DrawLine(g_handle, 0, g_height - TXR_STATUS_H, g_width - 1, g_height - TXR_STATUS_H, TXR_COLOR_BORDER);
    char info[256];
    int cursor_line;
    int cursor_col;
    cursor_line_col(g_cursor, &cursor_line, &cursor_col);
    snprintf(info, sizeof(info), txr_tr("%s | 行 %d，列 %d | %d 字节 | %s%s",
                                       "%s | Line %d, Col %d | %d bytes | %s%s"),
             g_file_path[0] ? g_file_path : txr_untitled(),
             cursor_line + 1,
             cursor_col + 1,
             g_length,
             g_dirty ? txr_tr("未保存 | ", "Unsaved | ") : "",
             g_status);
    xapi_DrawSWText(g_handle, 14, g_height - 22, info, TXR_COLOR_MUTED);
    g_need_status_redraw = false;
    xapi_RefreshPartWindow(g_handle, 0, g_height - TXR_STATUS_H, g_width - 1, g_height - 1);
}

static void render_editor()
{
    bool full_refresh = g_need_layout_redraw;
    if (g_need_layout_redraw) render_layout();
    if (g_need_text_redraw) render_text_area();
    if (g_need_status_redraw) render_status_bar();
    if (full_refresh) xapi_RefreshWindow(g_handle);
    g_need_redraw = false;
}

static int position_from_mouse(int x, int y)
{
    int row = (y - text_area_top()) / TXR_LINE_H;
    int col = (x - text_area_left()) / TXR_CHAR_W;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    return position_from_line_col(g_scroll_y + row, g_scroll_x + col);
}

static void handle_char(char ch)
{
    if (ch == '\r' || ch < ' ') return;
    char buf[2] = {ch, '\0'};
    insert_bytes(buf, 1);
    ensure_cursor_visible();
    request_text_redraw();
}

static void move_cursor_left()
{
    int sel_start;
    int sel_end;
    normalize_selection(&sel_start, &sel_end);
    if (sel_start >= 0) {
        move_cursor_to(sel_start, false);
        return;
    }
    if (g_cursor > 0) move_cursor_to(g_cursor - 1, false);
}

static void move_cursor_right()
{
    int sel_start;
    int sel_end;
    normalize_selection(&sel_start, &sel_end);
    if (sel_start >= 0) {
        move_cursor_to(sel_end, false);
        return;
    }
    if (g_cursor < g_length) move_cursor_to(g_cursor + 1, false);
}

static void move_cursor_up()
{
    int line;
    int col;
    cursor_line_col(g_cursor, &line, &col);
    if (line > 0) move_cursor_to(position_from_line_col(line - 1, col), false);
}

static void move_cursor_down()
{
    int line;
    int col;
    cursor_line_col(g_cursor, &line, &col);
    move_cursor_to(position_from_line_col(line + 1, col), false);
}

static void move_cursor_home()
{
    int line;
    int col;
    cursor_line_col(g_cursor, &line, &col);
    (void)col;
    move_cursor_to(position_from_line_col(line, 0), false);
}

static void move_cursor_end()
{
    int pos = g_cursor;
    while (pos < g_length && g_text[pos] != '\n') pos++;
    move_cursor_to(pos, false);
}

static void delete_forward()
{
    int sel_start;
    int sel_end;
    normalize_selection(&sel_start, &sel_end);
    if (sel_start >= 0) {
        delete_selection();
        return;
    }
    if (g_cursor >= g_length) return;
    memmove(g_text + g_cursor, g_text + g_cursor + 1, (size_t)(g_length - g_cursor));
    g_length--;
    mark_dirty(true);
}

static void handle_special(UINT64 ch)
{
    if (ch == '\r') {
        return;
    } else if (ch == '\b') {
        delete_backward();
        ensure_cursor_visible();
        request_text_redraw();
    } else if (ch == '\n') {
        insert_bytes("\n", 1);
        ensure_cursor_visible();
        request_text_redraw();
    } else if (ch == TXR_KEY_LEFT) {
        move_cursor_left();
        request_text_redraw();
    } else if (ch == TXR_KEY_RIGHT) {
        move_cursor_right();
        request_text_redraw();
    } else if (ch == TXR_KEY_UP) {
        move_cursor_up();
        request_text_redraw();
    } else if (ch == TXR_KEY_DOWN) {
        move_cursor_down();
        request_text_redraw();
    } else if (ch == TXR_KEY_HOME) {
        move_cursor_home();
        request_text_redraw();
    } else if (ch == TXR_KEY_END) {
        move_cursor_end();
        request_text_redraw();
    } else if (ch == TXR_KEY_DELETE) {
        delete_forward();
        ensure_cursor_visible();
        request_text_redraw();
    } else if (ch == TXR_KEY_PAGE_UP) {
        g_scroll_y -= visible_rows();
        if (g_scroll_y < 0) g_scroll_y = 0;
        request_text_redraw();
    } else if (ch == TXR_KEY_PAGE_DOWN) {
        g_scroll_y += visible_rows();
        ensure_cursor_visible();
        request_text_redraw();
    }
}

static void handle_control(UINT64 id)
{
    switch (id) {
    case BTN_NEW:
        do_new();
        g_need_layout_redraw = true;
        g_need_redraw = true;
        break;
    case BTN_OPEN:
        if (do_open()) request_text_redraw();
        break;
    case BTN_SAVE:
        if (do_save(false)) request_text_redraw();
        break;
    case BTN_SAVE_AS:
        if (do_save(true)) request_text_redraw();
        break;
    default:
        break;
    }
}

static void texter_message_proc(UINT64 type, UINT64 hData, UINT64 lData)
{
    switch (type) {
    case MSG_CHAR:
        handle_char((char)lData);
        break;
    case MSG_SPCHAR:
        handle_special(lData);
        break;
    case MSG_LBUTTON:
    {
        UINT64 button_id = hit_command_button((int)hData, (int)lData);
        if (button_id != 0) {
            handle_control(button_id);
        } else if ((int)hData >= text_area_left() && (int)hData <= text_area_right() && (int)lData >= text_area_top() &&
                   (int)lData < text_area_bottom()) {
            g_select_anchor = -1;
            move_cursor_to(position_from_mouse((int)hData, (int)lData), false);
            request_text_redraw();
        }
        break;
    }
    case MSG_CRL:
        handle_control(hData);
        break;
    case MSG_ROLLER:
        g_scroll_y -= (int)hData * 3;
        if (g_scroll_y < 0) g_scroll_y = 0;
        request_text_redraw();
        break;
    case MSG_RESIZE:
        g_width = (int)hData;
        g_height = (int)lData;
        ensure_cursor_visible();
        g_need_layout_redraw = true;
        g_need_redraw = true;
        break;
    default:
        break;
    }
}

static int texter_main_impl(int argc, char *argv[], char *envp[])
{
    (void)envp;

    g_language = xj380_read_language();
    set_status(txr_tr("就绪", "Ready"));
    if (!ensure_capacity(TXR_GROW_STEP)) return -1;
    clear_document();

    if (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') {
        load_from_path(argv[1]);
    } else if (argc == 1 && argv[0] != NULL && argv[0][0] != '\0') {
        load_from_path(argv[0]);
    }

    XWINDOW window;
    g_language = xj380_read_language();
    window.title = txr_tr("文本编辑器", "Text Editor");
    window.width = TXR_X;
    window.height = TXR_Y;
    window.sets = XWIN_NORMAL | XWIN_SUPPORT_RESIZEABLE;
    xapi_CreateWindow(&g_handle, &window);
    if (g_handle == 0) return -1;
    xapi_SetIcon(g_handle, "/system/icon/texter.png");
    SetMsgPrcor(g_handle, texter_message_proc);
    update_title();
    g_need_redraw = true;

    while (g_running) {
        if (g_need_redraw) render_editor();
        xapi_Sleep(16);
    }

    return 0;
}

extern "C" int texter_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int texter_main_cpp(int argc, char *argv[], char *envp[])
{
    return texter_main_impl(argc, argv, envp);
}
