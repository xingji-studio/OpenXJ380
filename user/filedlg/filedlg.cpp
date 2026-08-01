#include <x3api.h>
#include <xj380_i18n.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const int DLG_WIDTH = 760;
static const int DLG_HEIGHT = 540;
static const int DLG_MAX_ITEMS = 256;
static const int DLG_ROW_H = 18;
static const int DLG_HEADER_H = 92;
static const int DLG_FOOTER_H = 84;
static const int DLG_PATH_MAX = 256;

enum
{
    DLG_BTN_UP = 2001,
    DLG_BTN_OK = 2002,
    DLG_BTN_CANCEL = 2003
};

typedef struct dialog_item
{
    char name[256];
    bool is_dir;
} dialog_item_t;

static HDLE g_handle = 0;
static UINT64 g_width = DLG_WIDTH;
static UINT64 g_height = DLG_HEIGHT;
static UINT64 g_mode = XAPI_FILE_DIALOG_OPEN;
static bool g_need_redraw = true;
static bool g_running = true;
static bool g_accept = false;
static bool g_title_is_default = false;
static int g_language = XJ380_LANGUAGE_ZH_CN;

static char g_title[64] = "";
static char g_current_path[DLG_PATH_MAX] = "/";
static char g_name_input[128] = {0};
static int g_name_len = 0;
static int g_scroll = 0;
static int g_selected = -1;
static char g_status[128] = "";

static dialog_item_t g_items[DLG_MAX_ITEMS];
static int g_item_count = 0;

static int dlg_min(int a, int b) { return a < b ? a : b; }
static int dlg_max(int a, int b) { return a > b ? a : b; }

static char *dlg_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(g_language, zh_cn, en_us);
}

static void set_status(const char *text)
{
    if (text == NULL) text = "";
    strncpy(g_status, text, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
}

static void set_status_tr(const char *zh_cn, const char *en_us)
{
    set_status(dlg_tr(zh_cn, en_us));
}

static void set_name_input(const char *text)
{
    if (text == NULL) text = "";
    strncpy(g_name_input, text, sizeof(g_name_input) - 1);
    g_name_input[sizeof(g_name_input) - 1] = '\0';
    g_name_len = (int)strlen(g_name_input);
}

static int list_top() { return DLG_HEADER_H; }
static int list_bottom() { return (int)g_height - DLG_FOOTER_H; }
static int visible_rows()
{
    int rows = (list_bottom() - list_top() - 8) / DLG_ROW_H;
    return rows < 1 ? 1 : rows;
}

static void clamp_scroll()
{
    int max_scroll = dlg_max(0, g_item_count - visible_rows());
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
}

static void join_path(char *out, int out_size, const char *base, const char *name)
{
    if (strcmp(base, "/") == 0) snprintf(out, (size_t)out_size, "/%s", name);
    else snprintf(out, (size_t)out_size, "%s/%s", base, name);
}

static void parent_path()
{
    if (strcmp(g_current_path, "/") == 0) return;
    int len = (int)strlen(g_current_path);
    while (len > 1 && g_current_path[len - 1] == '/') {
        g_current_path[--len] = '\0';
    }
    while (len > 1 && g_current_path[len - 1] != '/') {
        g_current_path[--len] = '\0';
    }
    if (len > 1) g_current_path[len - 1] = '\0';
}

static void sort_items()
{
    for (int i = 0; i < g_item_count; ++i) {
        for (int j = i + 1; j < g_item_count; ++j) {
            bool swap = false;
            if (g_items[i].is_dir != g_items[j].is_dir) swap = g_items[j].is_dir;
            else if (strcmp(g_items[i].name, g_items[j].name) > 0) swap = true;
            if (swap) {
                dialog_item_t tmp = g_items[i];
                g_items[i] = g_items[j];
                g_items[j] = tmp;
            }
        }
    }
}

static void reload_dir()
{
    DIR *dir = opendir(g_current_path);
    g_item_count = 0;
    g_selected = -1;
    g_scroll = 0;
    if (g_mode != XAPI_FILE_DIALOG_SAVE) set_name_input("");

    if (dir == NULL) {
        set_status_tr("无法打开目录", "Cannot open directory");
        g_need_redraw = true;
        return;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL && g_item_count < DLG_MAX_ITEMS) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        strncpy(g_items[g_item_count].name, ent->d_name, sizeof(g_items[g_item_count].name) - 1);
        g_items[g_item_count].name[sizeof(g_items[g_item_count].name) - 1] = '\0';
        g_items[g_item_count].is_dir = ent->d_type == DT_DIR;
        g_item_count++;
    }
    closedir(dir);
    sort_items();
    if (g_mode == XAPI_FILE_DIALOG_SAVE) {
        set_status_tr("选择位置后可输入文件名保存", "Choose a location, then enter a file name to save");
    } else {
        set_status_tr("双击目录进入，单击文件选择", "Double-click a folder to enter, click a file to select");
    }
    g_need_redraw = true;
}

static void write_result_and_exit(const char *path)
{
    int pid = (int)getpid();
    char out_path[128];
    snprintf(out_path, sizeof(out_path), "/tmp/filedlg_%d.out", pid);
    int fd = open(out_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd >= 0) {
        if (path != NULL && path[0] != '\0') write(fd, path, strlen(path));
        close(fd);
    }
    g_accept = true;
    g_running = false;
}

static void accept_current()
{
    char target[DLG_PATH_MAX];
    memset(target, 0, sizeof(target));

    if (g_mode == XAPI_FILE_DIALOG_SAVE) {
        if (g_name_input[0] == '\0') {
            set_status_tr("请输入文件名", "Enter a file name");
            g_need_redraw = true;
            return;
        }
        join_path(target, sizeof(target), g_current_path, g_name_input);
        write_result_and_exit(target);
        return;
    }

    if (g_selected < 0 || g_selected >= g_item_count || g_items[g_selected].is_dir) {
        set_status_tr("请选择文件", "Select a file");
        g_need_redraw = true;
        return;
    }
    join_path(target, sizeof(target), g_current_path, g_items[g_selected].name);
    write_result_and_exit(target);
}

static void activate_item(int index)
{
    if (index < 0 || index >= g_item_count) return;
    if (g_items[index].is_dir) {
        char next_path[DLG_PATH_MAX];
        join_path(next_path, sizeof(next_path), g_current_path, g_items[index].name);
        strncpy(g_current_path, next_path, sizeof(g_current_path) - 1);
        g_current_path[sizeof(g_current_path) - 1] = '\0';
        reload_dir();
        return;
    }

    g_selected = index;
    set_name_input(g_items[index].name);
    if (g_mode == XAPI_FILE_DIALOG_OPEN) accept_current();
    else g_need_redraw = true;
}

static void render_dialog()
{
    xapi_DrawRect(g_handle, 0, 0, (UINT32)g_width - 1, (UINT32)g_height - 1, 0xf7f3e9ff, true);
    xapi_DrawRect(g_handle, 0, 0, (UINT32)g_width - 1, DLG_HEADER_H - 1, 0x3c6e71ff, true);
    xapi_DrawRect(g_handle, 0, DLG_HEADER_H, (UINT32)g_width - 1, (UINT32)g_height - DLG_FOOTER_H - 1, 0xfffefcff, true);
    xapi_DrawRect(g_handle, 0, (UINT32)g_height - DLG_FOOTER_H, (UINT32)g_width - 1, (UINT32)g_height - 1, 0xe8e1d3ff, true);

    xapi_DeleteButton(g_handle, DLG_BTN_UP);
    xapi_DeleteButton(g_handle, DLG_BTN_OK);
    xapi_DeleteButton(g_handle, DLG_BTN_CANCEL);
    xapi_Button(g_handle, DLG_BTN_UP, 12, 10, dlg_tr("上一级", "Up"));
    xapi_Button(g_handle,
                DLG_BTN_OK,
                (UINT64)g_width - 180,
                (UINT64)g_height - 44,
                g_mode == XAPI_FILE_DIALOG_SAVE ? dlg_tr("保存", "Save") : dlg_tr("打开", "Open"));
    xapi_ButtonEmp(g_handle, DLG_BTN_CANCEL, (UINT64)g_width - 92, (UINT64)g_height - 44, dlg_tr("取消", "Cancel"));

    xapi_DrawSWText(g_handle, 16, 46, g_title, 0xffffffff);
    xapi_DrawSWText(g_handle, 16, 68, g_current_path, 0xdcefefff);

    xapi_DrawRect(g_handle, 12, DLG_HEADER_H + 4, (UINT32)g_width - 13, (UINT32)g_height - DLG_FOOTER_H - 8, 0xd6d0c4ff, false);

    clamp_scroll();
    int rows = visible_rows();
    int start = g_scroll;
    int end = dlg_min(g_item_count, start + rows);
    for (int i = start; i < end; ++i) {
        int y = DLG_HEADER_H + 10 + (i - start) * DLG_ROW_H;
        if (i == g_selected) xapi_DrawRect(g_handle, 16, y - 1, (UINT32)g_width - 18, y + DLG_ROW_H - 2, 0xc7e0f4ff, true);
        char line[300];
        snprintf(line, sizeof(line), "%s %s", g_items[i].is_dir ? dlg_tr("[目录]", "[DIR]") : "      ", g_items[i].name);
        xapi_DrawSWText(g_handle, 20, y, line, g_items[i].is_dir ? 0x1d3557ff : 0x111111ff);
    }

    xapi_DrawSWText(g_handle,
                    16,
                    (UINT32)g_height - 72,
                    g_mode == XAPI_FILE_DIALOG_SAVE ? dlg_tr("文件名:", "File name:") : dlg_tr("已选:", "Selected:"),
                    0x3d405bff);
    xapi_DrawRect(g_handle, 84, (UINT32)g_height - 76, (UINT32)g_width - 204, (UINT32)g_height - 52, 0xffffffff, true);
    xapi_DrawRect(g_handle, 84, (UINT32)g_height - 76, (UINT32)g_width - 204, (UINT32)g_height - 52, 0xc8c1b5ff, false);
    xapi_DrawSWText(g_handle, 90, (UINT32)g_height - 72, g_name_input, 0x111111ff);
    xapi_DrawSWText(g_handle, 16, (UINT32)g_height - 28, g_status, 0x5b5345ff);

    xapi_RefreshWindow(g_handle);
    g_need_redraw = false;
}

static int item_index_from_mouse(int y)
{
    int row = (y - (DLG_HEADER_H + 10)) / DLG_ROW_H;
    if (row < 0) return -1;
    int index = g_scroll + row;
    if (index < 0 || index >= g_item_count) return -1;
    return index;
}

static void on_char(char ch)
{
    if (g_mode != XAPI_FILE_DIALOG_SAVE) return;
    if (ch < ' ' || g_name_len >= (int)sizeof(g_name_input) - 1) return;
    g_name_input[g_name_len++] = ch;
    g_name_input[g_name_len] = '\0';
    g_need_redraw = true;
}

static void on_special(char ch)
{
    if (ch == '\b') {
        if (g_mode == XAPI_FILE_DIALOG_SAVE && g_name_len > 0) {
            g_name_input[--g_name_len] = '\0';
            g_need_redraw = true;
        }
    } else if (ch == '\n') {
        accept_current();
    }
}

static void on_control(UINT64 id)
{
    if (id == DLG_BTN_UP) {
        parent_path();
        reload_dir();
    } else if (id == DLG_BTN_OK) {
        accept_current();
    } else if (id == DLG_BTN_CANCEL) {
        write_result_and_exit("");
    }
}

static void dialog_message(UINT64 type, UINT64 hData, UINT64 lData)
{
    switch (type) {
    case MSG_CHAR:
        on_char((char)lData);
        break;
    case MSG_SPCHAR:
        on_special((char)lData);
        break;
    case MSG_CRL:
        on_control(hData);
        break;
    case MSG_LBUTTON:
        if ((int)lData >= DLG_HEADER_H + 8 && (int)lData < list_bottom() - 8) {
            int index = item_index_from_mouse((int)lData);
            if (index >= 0) {
                if (g_selected == index) {
                    activate_item(index);
                    break;
                }
                g_selected = index;
                set_name_input(g_items[index].name);
                g_need_redraw = true;
            }
        }
        break;
    case MSG_ROLLER:
        g_scroll -= (int)hData * 3;
        clamp_scroll();
        g_need_redraw = true;
        break;
    case MSG_RESIZE:
        g_width = hData;
        g_height = lData;
        g_need_redraw = true;
        break;
    default:
        break;
    }
}

static int filedlg_main_impl(int argc, char *argv[], char *envp[])
{
    (void)envp;

    g_language = xj380_read_language();

    if (argc > 1 && argv[1] != NULL) g_mode = (UINT64)atoi(argv[1]);
    if (argc > 2 && argv[2] != NULL && argv[2][0] != '\0') {
        strncpy(g_title, argv[2], sizeof(g_title) - 1);
        g_title[sizeof(g_title) - 1] = '\0';
        g_title_is_default = false;
    } else {
        strncpy(g_title,
                g_mode == XAPI_FILE_DIALOG_SAVE ? dlg_tr("保存文件", "Save File") : dlg_tr("选择文件", "Select File"),
                sizeof(g_title) - 1);
        g_title[sizeof(g_title) - 1] = '\0';
        g_title_is_default = true;
    }
    if (argc > 3 && argv[3] != NULL && argv[3][0] != '\0') {
        strncpy(g_current_path, argv[3], sizeof(g_current_path) - 1);
        g_current_path[sizeof(g_current_path) - 1] = '\0';
    }

    struct stat st;
    if (stat(g_current_path, &st) == 0 && !S_ISDIR(st.st_mode)) {
        const char *last_slash = strrchr(g_current_path, '/');
        if (last_slash != NULL) {
            set_name_input(last_slash + 1);
            if (last_slash == g_current_path) strcpy(g_current_path, "/");
            else {
                int len = (int)(last_slash - g_current_path);
                g_current_path[len] = '\0';
            }
        }
    } else if (g_mode == XAPI_FILE_DIALOG_SAVE) {
        const char *last_slash = strrchr(g_current_path, '/');
        if (last_slash != NULL && last_slash[1] != '\0') {
            set_name_input(last_slash + 1);
            if (last_slash == g_current_path) strcpy(g_current_path, "/");
            else {
                int len = (int)(last_slash - g_current_path);
                g_current_path[len] = '\0';
            }
        }
    }

    XWINDOW window;
    window.title = g_title;
    window.width = DLG_WIDTH;
    window.height = DLG_HEIGHT;
    window.sets = XWIN_NORMAL | XWIN_SUPPORT_RESIZEABLE;
    xapi_CreateWindow(&g_handle, &window);
    xapi_SetIcon(g_handle, "/system/icon/folder.png");
    SetMsgPrcor(g_handle, dialog_message);

    reload_dir();
    while (g_running) {
        if (g_need_redraw) render_dialog();
        xapi_Sleep(16);
    }

    return g_accept ? 0 : 1;
}

extern "C" int filedlg_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int filedlg_main_cpp(int argc, char *argv[], char *envp[])
{
    return filedlg_main_impl(argc, argv, envp);
}
