#include <x3api.h>
#include <xj380_i18n.h>

static HDLE   g_handle = 0;
static UINT64 g_width  = 0;
static UINT64 g_height = 0;

static const UINT32 DOCK_GLASS = 0xffffff7d;
static const UINT32 BLACK     = 0x000000ff;
static const UINT32 CLEAR     = 0x00000000;

enum DockTopbarAction
{
    DOCK_TOPBAR_FILES,
    DOCK_TOPBAR_SETTINGS,
    DOCK_TOPBAR_TERMINAL,
    DOCK_TOPBAR_TASKS,
    DOCK_TOPBAR_ABOUT,
};

struct DockTopbarItem
{
    const char *label_zh;
    const char *label_en;
    DockTopbarAction action;
};

static const DockTopbarItem g_topbar_items[] = {
    {"文件", "Files", DOCK_TOPBAR_FILES},
    {"设置", "Settings", DOCK_TOPBAR_SETTINGS},
    {"终端", "Terminal", DOCK_TOPBAR_TERMINAL},
    {"进程", "Tasks", DOCK_TOPBAR_TASKS},
    {"关于", "About", DOCK_TOPBAR_ABOUT},
};

static void clear_dock_layer_buffer()
{
    if (g_width == 0 || g_height == 0) return;

    UINT64 pixel_count = g_width * g_height;
    XCOLORA *pixels = (XCOLORA *)calloc(pixel_count, sizeof(XCOLORA));
    if (pixels != NULL)
    {
        xapi_WriteBufferA(g_handle, 0, 0, (UINT32)g_width, (UINT32)g_height, pixels);
        free(pixels);
        return;
    }

    xapi_DrawRect(g_handle, 0, 0, (UINT32)g_width - 1, (UINT32)g_height - 1, CLEAR, true);
}

static void run_with_one_arg(const char *path, const char *arg)
{
    char *argv[2];
    argv[0] = (char *)arg;
    argv[1] = NULL;
    xapi_RunArgs((char *)path, argv);
}

static int topbar_x1()
{
    return (int)(g_width * 3 / 16);
}

static int topbar_x2()
{
    return (int)(g_width * 13 / 16);
}

static void draw_bottom_dock()
{
    int bar_w = (int)(g_width * 3 / 4);
    int bar_y = (int)g_height - 48;
    xapi_DrawRect(g_handle, 0, bar_y, bar_w - 1, (UINT32)g_height - 1, DOCK_GLASS, true);

    for (int y = 0; y <= 47; y++)
    {
        for (int x = 0; x <= y; x++)
        {
            xapi_DrawPoint(g_handle, (UINT32)(bar_w + x), (UINT32)(bar_y + y), DOCK_GLASS);
        }
    }

    xapi_DrawPicture(g_handle, 24, (UINT32)g_height - 24 - 63, 84, 64, (char *)"/system/xj380.png");
}

static void draw_topbar()
{
    int language = xj380_read_language();
    int x1 = topbar_x1();
    int x2 = topbar_x2();
    xapi_DrawRect(g_handle, x1, 0, x2 - 1, 23, DOCK_GLASS, true);
    xapi_DrawPicture(g_handle, x1 + 8, 4, 16, 16, (char *)"/system/xingji.png");

    int cursor = x1 + 32;
    for (int i = 0; i < (int)(sizeof(g_topbar_items) / sizeof(g_topbar_items[0])); i++)
    {
        char *label = xj380_tr_lang(language, g_topbar_items[i].label_zh, g_topbar_items[i].label_en);
        xapi_DrawText(g_handle, cursor, 1, label, 10, BLACK);
        int label_w = (int)xapi_CalcTextWidth(label, 10);
        if (label_w < 25) label_w = 25;
        cursor += label_w + 12;
    }
}

static void draw_time()
{
    TimeType tm;
    memset(&tm, 0, sizeof(tm));
    xapi_GetTimeX(&tm);

    int x2 = topbar_x2();
    char text[16];
    memset(text, 0, sizeof(text));
    snprintf(text, sizeof(text), "%02d:%02d", tm.tm_hour % 24, tm.tm_min);
    xapi_DrawRect(g_handle, x2 - 44, 0, x2 - 3, 23, DOCK_GLASS, true);
    xapi_DrawText(g_handle, x2 - 40, 1, text, 10, BLACK);
}

static void redraw_dock_after_layer_clear()
{
    draw_bottom_dock();
    draw_topbar();
    draw_time();
    xapi_RefreshWindow(g_handle);
}

static void refresh_topbar_only()
{
    int x1 = topbar_x1();
    int x2 = topbar_x2();
    xapi_DrawRect(g_handle, x1, 0, x2 - 1, 23, DOCK_GLASS, true);
    draw_topbar();
    draw_time();
    xapi_RefreshPartWindow(g_handle, x1, 0, x2, 24);
}

static void show_logo_menu()
{
    int language = xj380_read_language();
    int x1 = topbar_x1();
    xapi_DrawRect(g_handle, x1, 28, x1 + 99, 93, DOCK_GLASS, true);
    xapi_DrawText(g_handle, x1 + 8, 28, xj380_tr_lang(language, "重启", "Restart"), 10, BLACK);
    xapi_DrawText(g_handle, x1 + 8, 50, xj380_tr_lang(language, "关机", "Shut down"), 10, BLACK);
    xapi_DrawText(g_handle, x1 + 8, 72, xj380_tr_lang(language, "刷新", "Refresh"), 10, BLACK);
    xapi_RefreshPartWindow(g_handle, x1, 28, x1 + 100, 94);
}

static void run_topbar_action(DockTopbarAction action)
{
    if (action == DOCK_TOPBAR_FILES)
    {
        xapi_Run((char *)"/apps/system/fmanager.elf");
    }
    else if (action == DOCK_TOPBAR_SETTINGS)
    {
        run_with_one_arg("/apps/system/ctrlmenu.elf", "shortdock-open-settings");
    }
    else if (action == DOCK_TOPBAR_TERMINAL)
    {
        xapi_Run((char *)"/apps/system/shell.elf");
    }
    else if (action == DOCK_TOPBAR_TASKS)
    {
        xapi_Run((char *)"/apps/system/taskmgr.elf");
    }
    else if (action == DOCK_TOPBAR_ABOUT)
    {
        xapi_Run((char *)"/apps/system/xjver.elf");
    }
}

static void handle_topbar_click(int x, int y)
{
    (void)y;

    int language = xj380_read_language();
    int x1 = topbar_x1();
    if (x > x1 && x < x1 + 32)
    {
        show_logo_menu();
        return;
    }

    int cursor = x1 + 32;
    for (int i = 0; i < (int)(sizeof(g_topbar_items) / sizeof(g_topbar_items[0])); i++)
    {
        char *label = xj380_tr_lang(language, g_topbar_items[i].label_zh, g_topbar_items[i].label_en);
        int label_w = (int)xapi_CalcTextWidth(label, 10);
        if (label_w < 25) label_w = 25;
        int next = cursor + label_w + 12;
        if (x > cursor && x < next)
        {
            run_topbar_action(g_topbar_items[i].action);
            return;
        }
        cursor = next;
    }
}

static void handle_bottom_dock_click(int x, int y)
{
    if (x > 24 && x < 24 + 84 && y > (int)g_height - 24 - 63)
    {
        xapi_Run((char *)"/apps/system/ctrlmenu.elf");
    }
}

static void dock_MessagePrcor(UINT64 Type, UINT64 hData, UINT64 lData)
{
    if (Type == MSG_LBUTTON)
    {
        int x = (int)hData;
        int y = (int)lData;
        if (y < 24) handle_topbar_click(x, y);
        else handle_bottom_dock_click(x, y);
    }
    else if (Type == MSG_CRL)
    {
        if (hData == 1)
        {
            clear_dock_layer_buffer();
            redraw_dock_after_layer_clear();
        }
        else refresh_topbar_only();
    }
}

static int dock_main_impl(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    XWINDOW window;
    memset(&window, 0, sizeof(window));
    window.title = xj380_tr("程序坞", "Dock");
    window.sets = XWIN_DOCK;
    xapi_CreateWindow(&g_handle, &window);
    if (g_handle == 0) return 1;

    xapi_GetWindowSize(g_handle, &g_width, &g_height);
    SetMsgPrcor(g_handle, dock_MessagePrcor);
    clear_dock_layer_buffer();
    redraw_dock_after_layer_clear();

    int last_min = -1;
    while (1)
    {
        TimeType tm;
        memset(&tm, 0, sizeof(tm));
        xapi_GetTimeX(&tm);
        if (tm.tm_min != last_min)
        {
            last_min = tm.tm_min;
            draw_time();
            xapi_RefreshPartWindow(g_handle, topbar_x2() - 44, 0, topbar_x2(), 24);
        }
        xapi_Sleep(500);
    }

    return 0;
}

extern "C" int dock_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int dock_main_cpp(int argc, char *argv[], char *envp[])
{
    return dock_main_impl(argc, argv, envp);
}
