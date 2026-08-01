#include <x3api.h>
#include <libsys.h>
#include <krlibc.h>
#include "cm_proto.h"
#include "../../kernel/build_settings.h"


int cindex = 1;

bool dont_refresh = false;
bool about_memory_show_mb = false;
static int g_ctrlmenu_language = XJ380_LANGUAGE_ZH_CN;

static XCOLORA *ctrlmenu_background_cache = NULL;
static UINT64   ctrlmenu_background_width = 0;
static UINT64   ctrlmenu_background_height = 0;

static const char *wdaystr_table_zh[7] = {
    "星期日",
    "星期一",
    "星期二",
    "星期三",
    "星期四",
    "星期五",
    "星期六",
};

static const char *wdaystr_table_en[7] = {
    "Sunday",
    "Monday",
    "Tuesday",
    "Wednesday",
    "Thursday",
    "Friday",
    "Saturday",
};

static void draw_background_region(UINT64 refresh_x1, UINT64 refresh_y1, UINT64 refresh_x2, UINT64 refresh_y2);

static char *cm_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(g_ctrlmenu_language, zh_cn, en_us);
}

void init_ctrlmenu_background_cache()
{
    if (win_width == 0 || win_height == 0) return;
    if (ctrlmenu_background_cache != NULL && ctrlmenu_background_width == win_width &&
        ctrlmenu_background_height == win_height)
        return;

    if (ctrlmenu_background_cache != NULL)
    {
        free(ctrlmenu_background_cache);
        ctrlmenu_background_cache = NULL;
    }

    size_t pixel_count = (size_t)win_width * (size_t)win_height;
    if (pixel_count == 0 || pixel_count > (size_t)-1 / sizeof(XCOLORA)) return;

    ctrlmenu_background_cache = (XCOLORA *)malloc(pixel_count * sizeof(XCOLORA));
    if (ctrlmenu_background_cache == NULL) return;

    if (!xapi_LoadPicture(ctrlmenu_background_cache, (UINT32)win_width, (UINT32)win_height,
                          (char *)"/system/resources/image/background3.png"))
    {
        free(ctrlmenu_background_cache);
        ctrlmenu_background_cache = NULL;
        ctrlmenu_background_width = 0;
        ctrlmenu_background_height = 0;
        return;
    }

    ctrlmenu_background_width = win_width;
    ctrlmenu_background_height = win_height;
}

static const UINT32 CTRLMENU_BLUE        = 0x00a2e8ff;
static const UINT32 CTRLMENU_PANEL       = 0x12324af2;
static const UINT32 CTRLMENU_PANEL_SOFT  = 0x1d4c69ee;
static const UINT32 CTRLMENU_CARD        = 0x173c58e8;
static const UINT32 CTRLMENU_CARD_ACTIVE = 0x00a2e8ff;
static const UINT32 CTRLMENU_WHITE       = 0xffffffff;
static const UINT32 CTRLMENU_TEXT_DIM    = 0xc8f7ffff;
static const UINT32 CTRLMENU_TEXT_MUTED  = 0xb8d8e8ff;

enum CtrlmenuAppCategory
{
    CM_APP_FAVORITE = 1 << 0,
    CM_APP_SYSTEM   = 1 << 1,
    CM_APP_CREATE   = 1 << 2,
    CM_APP_NETWORK  = 1 << 3,
    CM_APP_DEV      = 1 << 4,
    CM_APP_MEDIA    = 1 << 5,
};

struct CtrlmenuAppEntry
{
    char   *title_zh;
    char   *title_en;
    char   *desc_zh;
    char   *desc_en;
    char   *path;
    char   *icon;
    UINT32  categories;
    int     setting_page;
};

struct CtrlmenuAppCategoryEntry
{
    char  *title_zh;
    char  *title_en;
    UINT32 category;
};

static CtrlmenuAppEntry g_ctrlmenu_apps[] = {
    {(char *)"文件管理器", (char *)"File Manager", (char *)"浏览磁盘、用户目录和系统文件",
     (char *)"Browse disks, user folders and system files", (char *)"/apps/system/fmanager.elf",
     (char *)"folder-open", CM_APP_FAVORITE | CM_APP_SYSTEM, 0},
    {(char *)"终端", (char *)"Terminal", (char *)"打开原生命令行 Shell", (char *)"Open the native command shell",
     (char *)"/apps/system/shell.elf", (char *)"file-lines",
     CM_APP_FAVORITE | CM_APP_SYSTEM | CM_APP_DEV, 0},
    {(char *)"任务管理器", (char *)"Task Manager", (char *)"查看进程、窗口和系统任务",
     (char *)"View processes, windows and system tasks", (char *)"/apps/system/taskmgr.elf", (char *)"gear",
     CM_APP_FAVORITE | CM_APP_SYSTEM, 0},
    {(char *)"控制面板", (char *)"Control Panel", (char *)"账户、外观、网络、时间与设备",
     (char *)"Accounts, appearance, network, time and devices", NULL, (char *)"gear",
     CM_APP_FAVORITE | CM_APP_SYSTEM, 1},
    {(char *)"关于 XJ380", (char *)"About XJ380", (char *)"查看系统版本和版权信息",
     (char *)"View system version and copyright", (char *)"/apps/system/xjver.elf", (char *)"user",
     CM_APP_FAVORITE | CM_APP_SYSTEM, 0},
    {(char *)"计算器", (char *)"Calculator", (char *)"基础计算工具", (char *)"Basic calculator",
     (char *)"/apps/builtin/calc.elf", (char *)"file",
     CM_APP_FAVORITE, 0},
    {(char *)"文本编辑器", (char *)"Text Editor", (char *)"编辑文本和配置文件",
     (char *)"Edit text and configuration files", (char *)"/apps/builtin/texter.elf", (char *)"markdown",
     CM_APP_FAVORITE | CM_APP_CREATE | CM_APP_DEV, 0},
    {(char *)"图片查看器", (char *)"Image Viewer", (char *)"查看图片文件", (char *)"View image files",
     (char *)"/apps/builtin/picturer.elf", (char *)"file-image",
     CM_APP_CREATE | CM_APP_MEDIA, 0},
    {(char *)"浏览器", (char *)"Browser", (char *)"浏览网页和 HTTP 内容",
     (char *)"Browse web pages and HTTP content", (char *)"/apps/builtin/browser.elf", (char *)"file",
     CM_APP_NETWORK, 0},
    {(char *)"Nut", (char *)"Nut", (char *)"脚本和自动化运行环境", (char *)"Script and automation runtime",
     (char *)"/apps/builtin/nut.elf", (char *)"file-lines",
     CM_APP_DEV, 0},
    {(char *)"兼容终端", (char *)"Compat Terminal", (char *)"Linux 兼容层终端",
     (char *)"Linux compatibility terminal", (char *)"/apps/busyterm.elf", (char *)"file-lines",
     CM_APP_DEV, 0},
};

static CtrlmenuAppCategoryEntry g_ctrlmenu_categories[] = {
    {(char *)"全部", (char *)"All", 0},
    {(char *)"常用", (char *)"Favorites", CM_APP_FAVORITE},
    {(char *)"系统", (char *)"System", CM_APP_SYSTEM},
    {(char *)"创作", (char *)"Create", CM_APP_CREATE},
    {(char *)"网络", (char *)"Network", CM_APP_NETWORK},
    {(char *)"开发", (char *)"Development", CM_APP_DEV},
    {(char *)"媒体", (char *)"Media", CM_APP_MEDIA},
};

static int g_app_showcase_category = 0;
static char   *g_last_launch_path = NULL;
static UINT64  g_last_launch_time = 0;

static int ctrlmenu_app_count()
{
    return (int)(sizeof(g_ctrlmenu_apps) / sizeof(g_ctrlmenu_apps[0]));
}

static int ctrlmenu_category_count()
{
    return (int)(sizeof(g_ctrlmenu_categories) / sizeof(g_ctrlmenu_categories[0]));
}

static char *ctrlmenu_app_title(const CtrlmenuAppEntry *app)
{
    return xj380_tr_lang(g_ctrlmenu_language, app->title_zh, app->title_en);
}

static char *ctrlmenu_app_desc(const CtrlmenuAppEntry *app)
{
    return xj380_tr_lang(g_ctrlmenu_language, app->desc_zh, app->desc_en);
}

static char *ctrlmenu_category_title(const CtrlmenuAppCategoryEntry *category)
{
    return xj380_tr_lang(g_ctrlmenu_language, category->title_zh, category->title_en);
}

static bool ctrlmenu_point_in_rect(int x, int y, int x1, int y1, int x2, int y2)
{
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}

static void ctrlmenu_draw_card(int x1, int y1, int x2, int y2, UINT32 fill)
{
    xapi_DrawRect(handle, x1, y1, x2, y2, fill, true);
    xapi_DrawRect(handle, x1, y1, x2, y1 + 2, CTRLMENU_BLUE, true);
}

static void ctrlmenu_draw_app_icon(int x, int y, char *icon)
{
    xapi_DrawRect(handle, x, y, x + 39, y + 39, CTRLMENU_PANEL_SOFT, true);
    xapi_DrawFA(handle, x + 10, y + 9, 19, icon);
}

static void ctrlmenu_draw_app_card(const CtrlmenuAppEntry *app, int x1, int y1, int x2, int y2)
{
    ctrlmenu_draw_card(x1, y1, x2, y2, CTRLMENU_CARD);
    ctrlmenu_draw_app_icon(x1 + 14, y1 + 18, app->icon);
    xapi_DrawText(handle, x1 + 66, y1 + 14, ctrlmenu_app_title(app), 16, CTRLMENU_WHITE);
    xapi_DrawText(handle, x1 + 66, y1 + 46, ctrlmenu_app_desc(app), 11, CTRLMENU_TEXT_DIM);
}

static void ctrlmenu_draw_status_row(int x, int y, char *name, char *value)
{
    xapi_DrawText(handle, x, y, name, 11, CTRLMENU_TEXT_MUTED);
    xapi_DrawText(handle, x + 92, y, value, 11, CTRLMENU_WHITE);
}

static void ctrlmenu_format_memory(char *buffer, size_t size)
{
    if (buffer == NULL || size == 0) return;
    memset(buffer, 0, size);

    UINT64 memory_mib = xapi_GetMemorySize() / 1024;
    if (memory_mib >= 1024)
    {
        strcat(buffer, xcr_int2char(memory_mib / 1024));
        strcat(buffer, " GB");
    }
    else
    {
        strcat(buffer, xcr_int2char(memory_mib));
        strcat(buffer, " MB");
    }
}

static void ctrlmenu_format_resolution(char *buffer, size_t size)
{
    if (buffer == NULL || size == 0) return;
    memset(buffer, 0, size);
    strcat(buffer, xcr_int2char(win_width));
    strcat(buffer, " x ");
    strcat(buffer, xcr_int2char(win_height));
}

static void ctrlmenu_format_datetime(char *buffer, size_t size)
{
    if (buffer == NULL || size == 0) return;
    memset(buffer, 0, size);

    TimeType tm;
    xapi_GetTimeX(&tm);
    sprintf(buffer, "%d-%02u-%02u %02u:%02u", tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

static void ctrlmenu_launch_app(const CtrlmenuAppEntry *app)
{
    if (app == NULL) return;

    UINT64 now = xapi_GetTimeNano();
    char *launch_key = app->path != NULL ? app->path : app->title_zh;
    if (g_last_launch_path == launch_key && now >= g_last_launch_time &&
        now - g_last_launch_time < 300000000ULL)
    {
        return;
    }
    g_last_launch_path = launch_key;
    g_last_launch_time = now;

    if (app->setting_page > 0)
    {
        cindex = 3;
        setting_cindex = app->setting_page;
        draw_background();
        return;
    }

    if (app->path != NULL) xapi_Run(app->path);
}

static void ctrlmenu_home_app_rect(int index, int *x1, int *y1, int *x2, int *y2)
{
    int left   = 24;
    int right  = (int)win_width - 24;
    int top    = (win_height < 700) ? 300 : 312;
    int gap    = (win_height < 700) ? 10 : 16;
    int height = (win_height < 700) ? 72 : 82;
    int cols   = (win_width >= 1120) ? 3 : 2;
    int width  = (right - left - gap * (cols - 1)) / cols;

    *x1 = left + (index % cols) * (width + gap);
    *y1 = top + (index / cols) * (height + gap);
    *x2 = *x1 + width;
    *y2 = *y1 + height;
}

static bool ctrlmenu_app_visible_in_showcase(const CtrlmenuAppEntry *app)
{
    UINT32 category = g_ctrlmenu_categories[g_app_showcase_category].category;
    return category == 0 || (app->categories & category) != 0;
}

static void ctrlmenu_showcase_app_rect(int visible_index, int *x1, int *y1, int *x2, int *y2)
{
    int left   = 224;
    int right  = (int)win_width - 24;
    int top    = 202;
    int gap    = (win_height < 700) ? 8 : 14;
    int height = (win_height < 700) ? 62 : 86;
    int cols   = (right - left >= 900) ? 3 : 2;
    int width  = (right - left - gap * (cols - 1)) / cols;

    *x1 = left + (visible_index % cols) * (width + gap);
    *y1 = top + (visible_index / cols) * (height + gap);
    *x2 = *x1 + width;
    *y2 = *y1 + height;
}

void draw_background()
{
    draw_background_region(0, 0, win_width, win_height);
}

void draw_background_body()
{
    draw_background_region(0, 121, win_width, win_height);
}

void draw_background_time()
{
    draw_background_region(900, 18, win_width - 8, 68);
}

static void draw_background_region(UINT64 refresh_x1, UINT64 refresh_y1, UINT64 refresh_x2, UINT64 refresh_y2)
{
    if (dont_refresh) return;
    g_ctrlmenu_language = read_settings_language();

    xapi_DrawRect(handle, 0, 0, win_width - 1, win_height - 1, 0x4d4d4dea, true);
    if (ctrlmenu_background_cache != NULL && ctrlmenu_background_width == win_width &&
        ctrlmenu_background_height == win_height)
    {
        xapi_WriteBufferA(handle, 0, 0, (UINT32)win_width, (UINT32)win_height, ctrlmenu_background_cache);
    }
    else
    {
        xapi_DrawPicture(handle, 0, 0, win_width, win_height, "/system/resources/image/background3.png");
    }
    xapi_DrawText(handle, 30, 18, cm_tr("控制中心", "Control Center"), 40, 0xffffffff);
    xapi_DrawRect(handle, 0, 116, win_width - 1, 120, 0x00a2e8ff, true);

    if (cindex == 1) 
    {
        xapi_DrawRect(handle, 280, 60, 386, 120, 0x00a2e8ff, true);
        draw_mainpage();
    }
    else if (cindex == 2) 
    {
        xapi_DrawRect(handle, 386, 60, 544, 120, 0x00a2e8ff, true);
        draw_app_showcase();
    }
    else if (cindex == 3)
    {
        xapi_DrawRect(handle, 544, 60, 670, 120, 0x00a2e8ff, true);
        draw_setting();
    }

    xapi_DrawText(handle, 312, 64, cm_tr("主页", "Home"), 16, 0xffffffff);
    xapi_DrawText(handle, 412, 64, cm_tr("应用陈列柜", "Apps"), 16, 0xffffffff);
    xapi_DrawText(handle, 576, 64, cm_tr("控制台", "Panel"), 16, 0xffffffff);

    char time_str_buffer[64];
    memset(time_str_buffer, 0, 64);

    TimeType tm;
    xapi_GetTimeX(&tm);

    int wday_index = tm.tm_wday > 0 ? tm.tm_wday - 1 : 0;
    if (wday_index < 0 || wday_index > 6) wday_index = 0;
    if (g_ctrlmenu_language == XJ380_LANGUAGE_EN_US)
    {
        sprintf(time_str_buffer, "%04d-%02u-%02u %s %02u:%02u", tm.tm_year, tm.tm_mon, tm.tm_mday,
                wdaystr_table_en[wday_index], tm.tm_hour, tm.tm_min);
    }
    else
    {
        sprintf(time_str_buffer, "%d 年 %d 月 %d 日 %s %02u:%02u", tm.tm_year, tm.tm_mon, tm.tm_mday,
                wdaystr_table_zh[wday_index], tm.tm_hour, tm.tm_min);
    }
    xapi_DrawText(handle, 940, 44, time_str_buffer, 16, 0xffffffff);

    xapi_RefreshPartWindow(handle, refresh_x1, refresh_y1, refresh_x2, refresh_y2);
}

void draw_mainpage()
{
    UserInfo user_info;
    memset(&user_info, 0, sizeof(user_info));
    xapi_GetCurrentUser(&user_info);

    char system_version[64];
    char memory_size[64];
    char resolution[64];
    char datetime[64];
    memset(system_version, 0, sizeof(system_version));
    xapi_GetSystemVersion(system_version);
    ctrlmenu_format_memory(memory_size, sizeof(memory_size));
    ctrlmenu_format_resolution(resolution, sizeof(resolution));
    ctrlmenu_format_datetime(datetime, sizeof(datetime));

    int user_x1 = 24;
    int user_y1 = 140;
    int user_x2 = 344;
    int user_y2 = 260;
    ctrlmenu_draw_card(user_x1, user_y1, user_x2, user_y2, CTRLMENU_PANEL);
    ctrlmenu_draw_app_icon(user_x1 + 18, user_y1 + 32, (char *)"user");
    xapi_DrawText(handle, user_x1 + 76, user_y1 + 24, cm_tr("当前账户", "Current Account"), 11, CTRLMENU_TEXT_DIM);
    xapi_DrawText(handle, user_x1 + 76, user_y1 + 54, user_info.name, 18, CTRLMENU_WHITE);

    int system_x1 = 368;
    int system_y1 = 140;
    int system_x2 = (int)win_width - 24;
    int system_y2 = 260;
    ctrlmenu_draw_card(system_x1, system_y1, system_x2, system_y2, CTRLMENU_PANEL);
    xapi_DrawText(handle, system_x1 + 18, system_y1 + 16, cm_tr("XJ380 操作系统", "XJ380 Operating System"),
                  18, CTRLMENU_WHITE);
    xapi_DrawText(handle, system_x1 + 18, system_y1 + 48, OS_VERSION, 11, CTRLMENU_TEXT_DIM);
    ctrlmenu_draw_status_row(system_x1 + 18, system_y1 + 80, cm_tr("系统版本", "System Version"), system_version);
    ctrlmenu_draw_status_row(system_x1 + 310, system_y1 + 80, cm_tr("内核版本", "Kernel Version"), (char *)KN_VERSION);

    xapi_DrawText(handle, 24, 282, cm_tr("常用入口", "Quick Access"), 16, CTRLMENU_WHITE);
    int home_apps[] = {0, 1, 2, 3, 4, 5};
    for (int i = 0; i < (int)(sizeof(home_apps) / sizeof(home_apps[0])); i++)
    {
        int x1, y1, x2, y2;
        ctrlmenu_home_app_rect(i, &x1, &y1, &x2, &y2);
        if (y2 < (int)win_height - 44) ctrlmenu_draw_app_card(&g_ctrlmenu_apps[home_apps[i]], x1, y1, x2, y2);
    }

    int info_y = (win_width >= 1120) ? 508 : 606;
    int info_x1 = 24;
    int info_x2 = (int)win_width - 24;
    if (info_y + 86 < (int)win_height - 50)
    {
        ctrlmenu_draw_card(info_x1, info_y, info_x2, info_y + 86, CTRLMENU_PANEL);
        xapi_DrawText(handle, info_x1 + 16, info_y + 14, cm_tr("系统状态", "System Status"), 16, CTRLMENU_WHITE);
        ctrlmenu_draw_status_row(info_x1 + 16, info_y + 48, cm_tr("运行内存", "Memory"), memory_size);
        ctrlmenu_draw_status_row(info_x1 + 260, info_y + 48, cm_tr("分辨率", "Resolution"), resolution);
        ctrlmenu_draw_status_row(info_x1 + 504, info_y + 48, cm_tr("当前时间", "Current Time"), datetime);
    }

    xapi_DrawRect(handle, 8, win_height - 38, win_width - 8, win_height - 8, 0x00a2e8ff, true);
    xapi_DrawText(handle, 16, win_height - 38, cm_tr("→ 返回桌面", "-> Back to Desktop"), 16, 0xffffffff);
}

void draw_app_showcase()
{
    xapi_DrawText(handle, 24, 136, cm_tr("应用陈列柜", "App Showcase"), 24, CTRLMENU_WHITE);
    xapi_DrawText(handle, 24, 176, cm_tr("选择分类或直接启动应用", "Choose a category or launch an app directly"),
                  11, CTRLMENU_TEXT_DIM);

    int category_x1 = 24;
    int category_x2 = 188;
    int category_y  = 220;
    for (int i = 0; i < ctrlmenu_category_count(); i++)
    {
        int y1 = category_y + i * 42;
        int y2 = y1 + 30;
        UINT32 color = (i == g_app_showcase_category) ? CTRLMENU_CARD_ACTIVE : CTRLMENU_CARD;
        xapi_DrawRect(handle, category_x1, y1, category_x2, y2, color, true);
        xapi_DrawText(handle, category_x1 + 14, y1 + 5, ctrlmenu_category_title(&g_ctrlmenu_categories[i]), 11,
                      CTRLMENU_WHITE);
    }

    int panel_x1 = 208;
    int panel_y1 = 136;
    int panel_x2 = (int)win_width - 24;
    int panel_y2 = (int)win_height - 24;
    ctrlmenu_draw_card(panel_x1, panel_y1, panel_x2, panel_y2, CTRLMENU_PANEL);
    xapi_DrawText(handle, panel_x1 + 16, panel_y1 + 14,
                  ctrlmenu_category_title(&g_ctrlmenu_categories[g_app_showcase_category]), 18, CTRLMENU_WHITE);
    xapi_DrawText(handle, panel_x1 + 16, panel_y1 + 48,
                  cm_tr("点击卡片启动应用；控制面板会在当前控制中心内打开。",
                        "Click a card to launch an app; Control Panel opens here."),
                  11, CTRLMENU_TEXT_DIM);

    int visible_index = 0;
    for (int i = 0; i < ctrlmenu_app_count(); i++)
    {
        if (!ctrlmenu_app_visible_in_showcase(&g_ctrlmenu_apps[i])) continue;

        int x1, y1, x2, y2;
        ctrlmenu_showcase_app_rect(visible_index, &x1, &y1, &x2, &y2);
        if (y2 < (int)win_height - 30)
        {
            ctrlmenu_draw_app_card(&g_ctrlmenu_apps[i], x1, y1, x2, y2);
        }
        visible_index++;
    }
}

bool ctrlmenu_handle_mainpage_click(int x, int y)
{
    if (x > 8 && y > (int)win_height - 38 && x < (int)win_width - 8 && y < (int)win_height - 8)
    {
        exit_cm = true;
        xapi_CloseWindow(handle);
        return true;
    }

    int home_apps[] = {0, 1, 2, 3, 4, 5};
    for (int i = 0; i < (int)(sizeof(home_apps) / sizeof(home_apps[0])); i++)
    {
        int x1, y1, x2, y2;
        ctrlmenu_home_app_rect(i, &x1, &y1, &x2, &y2);
        if (y2 < (int)win_height - 44 && ctrlmenu_point_in_rect(x, y, x1, y1, x2, y2))
        {
            ctrlmenu_launch_app(&g_ctrlmenu_apps[home_apps[i]]);
            return true;
        }
    }

    return false;
}

bool ctrlmenu_handle_app_showcase_click(int x, int y)
{
    int category_x1 = 24;
    int category_x2 = 188;
    int category_y  = 220;
    for (int i = 0; i < ctrlmenu_category_count(); i++)
    {
        int y1 = category_y + i * 42;
        int y2 = y1 + 30;
        if (ctrlmenu_point_in_rect(x, y, category_x1, y1, category_x2, y2))
        {
            g_app_showcase_category = i;
            draw_background_body();
            return true;
        }
    }

    int visible_index = 0;
    for (int i = 0; i < ctrlmenu_app_count(); i++)
    {
        if (!ctrlmenu_app_visible_in_showcase(&g_ctrlmenu_apps[i])) continue;

        int x1, y1, x2, y2;
        ctrlmenu_showcase_app_rect(visible_index, &x1, &y1, &x2, &y2);
        if (y2 < (int)win_height - 30 && ctrlmenu_point_in_rect(x, y, x1, y1, x2, y2))
        {
            ctrlmenu_launch_app(&g_ctrlmenu_apps[i]);
            return true;
        }
        visible_index++;
    }

    return false;
}

int setting_cindex = 1;

enum CtrlmenuInputTarget
{
    CM_INPUT_NONE,
    CM_INPUT_DEFAULT_APP,
    CM_INPUT_BACKGROUND,
};

static char                charbox_buffer[256];
static UINT64              charbox_input_id       = 0;
static CtrlmenuInputTarget charbox_target         = CM_INPUT_NONE;
static int                 charbox_default_app_id = -1;

static bool save_default_app_path(int id, const char *path)
{
    if (id < 0 || id >= 1024 || path == NULL) return false;

    UserInfo user;
    xapi_GetCurrentUser(&user);
    char runfile_path[256];
    snprintf(runfile_path, sizeof(runfile_path), "/users/%s/runfile.dat", user.name);
    XFILE *file = xapi_OpenFile(runfile_path);
    if (file == NULL || file->buffer == NULL || file->length < sizeof(RunfileSettings_Format))
    {
        if (file != NULL) xapi_CloseFile(file);
        return false;
    }

    RunfileSettings_Format *settings = (RunfileSettings_Format *)file->buffer;
    strncpy(settings->items[id].runpath, path, sizeof(settings->items[id].runpath) - 1);
    settings->items[id].runpath[sizeof(settings->items[id].runpath) - 1] = '\0';
    xapi_CloseFile(file);
    return true;
}

static bool create_input_box(const char *initial_value, CtrlmenuInputTarget target,
                             int default_app_id)
{
    if (charbox_input_id != 0 || target == CM_INPUT_NONE) return false;

    memset(charbox_buffer, 0, sizeof(charbox_buffer));
    if (initial_value != NULL)
    {
        strncpy(charbox_buffer, initial_value, sizeof(charbox_buffer) - 1);
    }

    dont_refresh = true;
    xapi_DrawRect(handle, win_width / 2 - 300, win_height / 2 - 50, win_width / 2 + 300,
                  win_height / 2 + 50, 0x007accff, true);
    xapi_DrawText(handle, win_width / 2 - 300 + 8, win_height / 2 - 50 + 4,
                  cm_tr("输入新的值：", "Enter a new value:"), 11, 0xffffffff);
    xapi_Button(handle, 114514, win_width / 2 + 300 - 58, win_height / 2 + 50 - 32,
                cm_tr("取消", "Cancel"));
    xapi_Button(handle, 114515, win_width / 2 + 300 - 116, win_height / 2 + 50 - 32,
                cm_tr("确定", "OK"));
    charbox_input_id =
        xapi_PutTextInputBox(handle, win_width / 2 - 300 + 8, win_height / 2 - 12, 584, charbox_buffer);
    if (charbox_input_id == 0)
    {
        xapi_DeleteButton(handle, 114514);
        xapi_DeleteButton(handle, 114515);
        dont_refresh = false;
        draw_background_body();
        return false;
    }

    charbox_target = target;
    charbox_default_app_id = default_app_id;
    xapi_RefreshPartWindow(handle, win_width / 2 - 300, win_height / 2 - 50, win_width / 2 + 300,
                           win_height / 2 + 50);
    return true;
}

void delete_input_box(bool save)
{
    if (charbox_input_id == 0) return;

    if (save) xapi_GetTextInputBox(charbox_input_id, charbox_buffer);
    xapi_DeleteTextInputBox(charbox_input_id);
    xapi_DeleteButton(handle, 114514);
    xapi_DeleteButton(handle, 114515);
    charbox_input_id = 0;

    CtrlmenuInputTarget target         = charbox_target;
    int                 default_app_id = charbox_default_app_id;
    charbox_target                     = CM_INPUT_NONE;
    charbox_default_app_id = -1;

    if (save)
    {
        if (target == CM_INPUT_DEFAULT_APP) save_default_app_path(default_app_id, charbox_buffer);
        else if (target == CM_INPUT_BACKGROUND) set_background_file_path(charbox_buffer);
    }

    dont_refresh = false;
    draw_background_body();
}

static void make_settings_path(char *path, size_t path_size)
{
    if (path == NULL || path_size == 0) return;

    UserInfo usif;
    xapi_GetCurrentUser(&usif);

    memset(path, 0, path_size);
    snprintf(path, path_size, "/users/%s/settings.dat", usif.name);
}

static void default_settings_data(SettingsDataFileFormat *settings)
{
    if (settings == NULL) return;
    memset(settings, 0, sizeof(SettingsDataFileFormat));
    strcpy(settings->BackgroundFilePath, "/system/resources/image/background2.png");
    settings->ClockHourOffset = 8;
    settings->Language = XJ380_LANGUAGE_ZH_CN;
}

static bool load_settings_data(SettingsDataFileFormat *settings)
{
    if (settings == NULL) return false;
    default_settings_data(settings);

    char path[256];
    make_settings_path(path, sizeof(path));
    XFILE *f = xapi_OpenFile(path);
    if (!f || f->buffer == NULL)
    {
        if (f) xapi_CloseFile(f);
        return false;
    }

    SettingsDataFileFormat loaded;
    default_settings_data(&loaded);
    size_t copy_size = f->length < sizeof(loaded) ? (size_t)f->length : sizeof(loaded);
    if (copy_size > 0) memcpy(&loaded, f->buffer, copy_size);
    xapi_CloseFile(f);

    loaded.BackgroundFilePath[sizeof(loaded.BackgroundFilePath) - 1] = '\0';
    if (loaded.BackgroundFilePath[0] != '\0')
    {
        strcpy(settings->BackgroundFilePath, loaded.BackgroundFilePath);
    }
    settings->ClockHourOffset = loaded.ClockHourOffset;
    settings->Language = copy_size >= sizeof(SettingsDataFileFormat) ?
                         xj380_normalize_language(loaded.Language) : XJ380_LANGUAGE_ZH_CN;
    return true;
}

static void save_settings_data(const SettingsDataFileFormat *settings)
{
    if (settings == NULL) return;

    char path[256];
    make_settings_path(path, sizeof(path));
    xapi_CreateFile(path);
    xapi_WriteFile(path, (char *)settings, sizeof(SettingsDataFileFormat), 0);
}

void change_setting_apps(int id)
{
    if (id < 0 || id >= 1024 || charbox_input_id != 0) return;

    char current_path[256];
    memset(current_path, 0, sizeof(current_path));
    UserInfo user;
    xapi_GetCurrentUser(&user);
    char runfile_path[256];
    snprintf(runfile_path, sizeof(runfile_path), "/users/%s/runfile.dat", user.name);
    XFILE *file = xapi_OpenFile(runfile_path);
    if (file != NULL && file->buffer != NULL && file->length >= sizeof(RunfileSettings_Format))
    {
        RunfileSettings_Format *settings = (RunfileSettings_Format *)file->buffer;
        strncpy(current_path, settings->items[id].runpath, sizeof(current_path) - 1);
    }
    if (file != NULL) xapi_CloseFile(file);

    create_input_box(current_path, CM_INPUT_DEFAULT_APP, id);
}

void change_setting_background()
{
    if (charbox_input_id != 0) return;

    char *background = get_background_file_path();
    create_input_box(background, CM_INPUT_BACKGROUND, -1);
}

void change_settings_background_path(const char *path)
{
    if (path == NULL) return;

    SettingsDataFileFormat settings;
    load_settings_data(&settings);
    memset(settings.BackgroundFilePath, 0, sizeof(settings.BackgroundFilePath));
    strncpy(settings.BackgroundFilePath, path, sizeof(settings.BackgroundFilePath) - 1);
    settings.BackgroundFilePath[sizeof(settings.BackgroundFilePath) - 1] = '\0';
    save_settings_data(&settings);
}

int read_settings_time_offset()
{
    SettingsDataFileFormat settings;
    load_settings_data(&settings);
    return settings.ClockHourOffset;
}

void change_clock_hour_offset(int value)
{
    SettingsDataFileFormat settings;
    load_settings_data(&settings);
    settings.ClockHourOffset += value;
    save_settings_data(&settings);

    xapi_FlushTime();
    draw_background_body();
}

int read_settings_language()
{
    SettingsDataFileFormat settings;
    load_settings_data(&settings);
    return settings.Language;
}

void change_settings_language(int language)
{
    SettingsDataFileFormat settings;
    load_settings_data(&settings);
    settings.Language = xj380_normalize_language(language);
    save_settings_data(&settings);
    g_ctrlmenu_language = settings.Language;
    draw_background();
}

char background_file_path[256];

void set_background_file_path(const char *path)
{
    if (path == NULL) return;
    change_settings_background_path(path);
    draw_background_body();
}

char *get_background_file_path()
{
    SettingsDataFileFormat settings;
    load_settings_data(&settings);
    strcpy(background_file_path, settings.BackgroundFilePath);
    return background_file_path;
}
