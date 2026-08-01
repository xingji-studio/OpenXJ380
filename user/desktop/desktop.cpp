#include <x3api.h>
#include <krlibc.h>
#include <xj380_i18n.h>

static const char *DEFAULT_DESKTOP_BACKGROUND = "/system/resources/image/background2.png";
static const char *FALLBACK_DESKTOP_BACKGROUND = "/system/resources/image/background3.png";

static HDLE   g_desktop_handle = 0;
static UINT64 g_screen_width    = 0;
static UINT64 g_screen_height   = 0;
static bool   g_needs_redraw    = false;
static bool   g_widgets_dirty   = false;
static bool   g_context_menu_open = false;
static int    g_desktop_language = XJ380_LANGUAGE_ZH_CN;

static DirNode g_entries[256];
static UINT32  g_entry_count = 0;

static const int ICON_CELL_W = 110;
static const int ICON_CELL_H = 110;
static const int ICON_SIZE   = 74;
static const int ICON_DRAG_THRESHOLD = 5;
static const int MAX_DESKTOP_ICONS = 258;

struct DesktopIconPosition
{
    char name[256];
    int  slot;
};

struct DesktopIconSettings
{
    UINT32 magic;
    UINT32 version;
    UINT32 count;
    DesktopIconPosition positions[MAX_DESKTOP_ICONS];
};

static const UINT32 DESKTOP_ICON_MAGIC = 0x49434f4e;
static const UINT32 DESKTOP_ICON_VERSION = 1;
static const char *SYSTEM_ICON_KEY = ":system:";
static const char *TRASH_ICON_KEY = ":trash:";

static DesktopIconSettings g_icon_settings;
static int  g_icon_slots[MAX_DESKTOP_ICONS];
static int  g_icon_count = 0;
static int  g_drag_icon = -1;
static int  g_drag_start_x = 0;
static int  g_drag_start_y = 0;
static int  g_drag_origin_slot = 0;
static int  g_drag_preview_slot = 0;
static bool g_icon_settings_loaded = false;
static bool g_dragging_icon = false;
static bool g_icon_drag_moved = false;

static const UINT32 WIDGET_GLASS       = 0x12324ae8;
static const UINT32 WIDGET_GLASS_SOFT  = 0x1d4c69dd;
static const UINT32 WIDGET_BLUE        = 0x0f6cbfff;
static const UINT32 WIDGET_BLUE_SOFT   = 0x9fd2ffff;
static const UINT32 WIDGET_WHITE       = 0xffffffff;
static const UINT32 WIDGET_TEXT_DIM    = 0xd7e9f7ff;
static const UINT32 WIDGET_TEXT_MUTED  = 0xa9c5ddff;
static const UINT32 WIDGET_SHADOW      = 0x00000055;

enum DesktopWidgetFlags
{
    DESKTOP_WIDGET_CLOCK  = 1 << 0,
    DESKTOP_WIDGET_SYSTEM = 1 << 1,
    DESKTOP_WIDGET_APPS   = 1 << 2,
    DESKTOP_WIDGET_NOTE   = 1 << 3,
};

enum DesktopWidgetCommand
{
    DESKTOP_CMD_TOGGLE_WIDGETS = 9101,
    DESKTOP_CMD_NEXT_WIDGET_LAYOUT,
    DESKTOP_CMD_REFRESH_WIDGETS,
};

struct DesktopWidgetSettings
{
    UINT32 magic;
    UINT32 version;
    UINT32 enabled_flags;
    UINT32 layout;
    char   note[160];
};

static const UINT32 DESKTOP_WIDGET_MAGIC = 0x574a5833; // WJX3
static const UINT32 DESKTOP_WIDGET_VERSION = 1;
static const UINT32 DESKTOP_WIDGET_ALL =
    DESKTOP_WIDGET_CLOCK | DESKTOP_WIDGET_SYSTEM | DESKTOP_WIDGET_APPS | DESKTOP_WIDGET_NOTE;

static DesktopWidgetSettings g_widget_settings;
static int g_widget_area_x1 = 0;
static int g_widget_area_y1 = 0;
static int g_widget_area_x2 = 0;
static int g_widget_area_y2 = 0;
static int g_last_widget_min = -1;
static int g_context_menu_x = 0;
static int g_context_menu_y = 0;

static void redraw_desktop();
static const char *desktop_icon_key(int index);
static void set_saved_icon_slot(const char *key, int slot);

typedef SettingsDataFileFormat DesktopSettings;

static void copy_cstr(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) return;
    memset(dst, 0, dst_size);
    if (src == NULL) return;
    strncpy(dst, src, dst_size - 1);
}

static char *desktop_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(g_desktop_language, zh_cn, en_us);
}

static void path_join(char *out, size_t out_size, const char *base, const char *name)
{
    if (out == NULL || out_size == 0) return;
    memset(out, 0, out_size);
    if (base == NULL) return;
    strncpy(out, base, out_size - 1);
    size_t used = strlen(out);
    if (used + 1 < out_size && used > 0 && out[used - 1] != '/')
    {
        out[used++] = '/';
        out[used] = '\0';
    }
    if (name != NULL && used + 1 < out_size)
    {
        size_t copy_len = strlen(name);
        if (copy_len > out_size - used - 1) copy_len = out_size - used - 1;
        memcpy(out + used, name, copy_len);
        out[used + copy_len] = '\0';
    }
}

static void set_default_desktop_settings(DesktopSettings *settings)
{
    if (settings == NULL) return;
    memset(settings, 0, sizeof(DesktopSettings));
    strcpy(settings->BackgroundFilePath, DEFAULT_DESKTOP_BACKGROUND);
    settings->ClockHourOffset = 8;
    settings->Language = XJ380_LANGUAGE_ZH_CN;
}

static void load_desktop_settings(DesktopSettings *settings)
{
    if (settings == NULL) return;
    set_default_desktop_settings(settings);

    UserInfo user;
    memset(&user, 0, sizeof(user));
    xapi_GetCurrentUser(&user);
    if (user.name[0] == '\0') return;

    char path[256];
    memset(path, 0, sizeof(path));
    snprintf(path, sizeof(path), "/users/%s/settings.dat", user.name);

    XFILE *file = xapi_OpenFile(path);
    if (file == NULL || file->buffer == NULL)
    {
        if (file != NULL) xapi_CloseFile(file);
        return;
    }

    DesktopSettings loaded;
    memset(&loaded, 0, sizeof(loaded));
    size_t copy_size = file->length < sizeof(loaded) ? (size_t)file->length : sizeof(loaded);
    if (copy_size > 0) memcpy(&loaded, file->buffer, copy_size);
    xapi_CloseFile(file);

    loaded.BackgroundFilePath[sizeof(loaded.BackgroundFilePath) - 1] = '\0';
    if (loaded.BackgroundFilePath[0] != '\0')
    {
        copy_cstr(settings->BackgroundFilePath, sizeof(settings->BackgroundFilePath), loaded.BackgroundFilePath);
    }
    settings->ClockHourOffset = loaded.ClockHourOffset;
    settings->Language = copy_size >= sizeof(loaded) ? xj380_normalize_language(loaded.Language) : XJ380_LANGUAGE_ZH_CN;
}

static void current_user_base_path(char *path, size_t path_size)
{
    UserInfo user;
    memset(&user, 0, sizeof(user));
    xapi_GetCurrentUser(&user);
    if (user.name[0] == '\0')
    {
        copy_cstr(path, path_size, "/users/Root");
        return;
    }

    memset(path, 0, path_size);
    snprintf(path, path_size, "/users/%s", user.name);
}

static void current_widget_settings_path(char *path, size_t path_size)
{
    char base[256];
    current_user_base_path(base, sizeof(base));
    path_join(path, path_size, base, "desktop_widgets.dat");
}

static void current_icon_settings_path(char *path, size_t path_size)
{
    char base[256];
    current_user_base_path(base, sizeof(base));
    path_join(path, path_size, base, "desktop_icons.dat");
}

static void load_icon_settings()
{
    if (g_icon_settings_loaded) return;
    g_icon_settings_loaded = true;
    memset(&g_icon_settings, 0, sizeof(g_icon_settings));
    g_icon_settings.magic = DESKTOP_ICON_MAGIC;
    g_icon_settings.version = DESKTOP_ICON_VERSION;

    char path[256];
    current_icon_settings_path(path, sizeof(path));
    XFILE *file = xapi_OpenFile(path);
    if (file == NULL || file->buffer == NULL)
    {
        if (file != NULL) xapi_CloseFile(file);
        return;
    }

    const size_t header_size = sizeof(g_icon_settings.magic) + sizeof(g_icon_settings.version) +
                               sizeof(g_icon_settings.count);
    if (file->length >= header_size)
    {
        memcpy(&g_icon_settings, file->buffer, header_size);
        size_t settings_size = header_size + (size_t)g_icon_settings.count * sizeof(DesktopIconPosition);
        if (g_icon_settings.magic != DESKTOP_ICON_MAGIC || g_icon_settings.version != DESKTOP_ICON_VERSION ||
            g_icon_settings.count > MAX_DESKTOP_ICONS || file->length < settings_size)
        {
            memset(&g_icon_settings, 0, sizeof(g_icon_settings));
            g_icon_settings.magic = DESKTOP_ICON_MAGIC;
            g_icon_settings.version = DESKTOP_ICON_VERSION;
        }
        else if (g_icon_settings.count > 0)
        {
            memcpy(g_icon_settings.positions, (char *)file->buffer + header_size,
                   (size_t)g_icon_settings.count * sizeof(DesktopIconPosition));
            for (UINT32 i = 0; i < g_icon_settings.count; i++)
                g_icon_settings.positions[i].name[sizeof(g_icon_settings.positions[i].name) - 1] = '\0';
        }
    }
    xapi_CloseFile(file);
}

static void save_icon_layout()
{
    memset(&g_icon_settings, 0, sizeof(g_icon_settings));
    g_icon_settings.magic = DESKTOP_ICON_MAGIC;
    g_icon_settings.version = DESKTOP_ICON_VERSION;
    for (int icon = 0; icon < g_icon_count; icon++)
    {
        set_saved_icon_slot(desktop_icon_key(icon), g_icon_slots[icon]);
    }

    const size_t header_size = sizeof(g_icon_settings.magic) + sizeof(g_icon_settings.version) +
                               sizeof(g_icon_settings.count);
    size_t settings_size = header_size + (size_t)g_icon_settings.count * sizeof(DesktopIconPosition);
    char path[256];
    current_icon_settings_path(path, sizeof(path));
    xapi_DeleteFile(path);
    xapi_WriteFile(path, (char *)&g_icon_settings, settings_size, 0);
}

static void set_default_widget_settings(DesktopWidgetSettings *settings)
{
    if (settings == NULL) return;
    memset(settings, 0, sizeof(DesktopWidgetSettings));
    settings->magic = DESKTOP_WIDGET_MAGIC;
    settings->version = DESKTOP_WIDGET_VERSION;
    settings->enabled_flags = 0;
    settings->layout = 0;
    int language = xj380_read_language();
    strcpy(settings->note,
           xj380_tr_lang(language, "欢迎使用 XJ380\n右键桌面可切换小组件布局。",
                         "Welcome to XJ380\nRight-click the desktop to change widget layout."));
}

static void save_widget_settings()
{
    char path[256];
    current_widget_settings_path(path, sizeof(path));
    xapi_CreateFile(path);
    xapi_WriteFile(path, (char *)&g_widget_settings, sizeof(g_widget_settings), 0);
}

static void load_widget_settings()
{
    set_default_widget_settings(&g_widget_settings);

    char path[256];
    current_widget_settings_path(path, sizeof(path));
    XFILE *file = xapi_OpenFile(path);
    if (file == NULL || file->buffer == NULL)
    {
        if (file != NULL) xapi_CloseFile(file);
        save_widget_settings();
        return;
    }

    DesktopWidgetSettings loaded;
    memset(&loaded, 0, sizeof(loaded));
    size_t copy_size = file->length < sizeof(loaded) ? (size_t)file->length : sizeof(loaded);
    if (copy_size > 0) memcpy(&loaded, file->buffer, copy_size);
    xapi_CloseFile(file);

    if (copy_size >= 16 && loaded.magic == DESKTOP_WIDGET_MAGIC)
    {
        loaded.note[sizeof(loaded.note) - 1] = '\0';
        g_widget_settings = loaded;
        g_widget_settings.enabled_flags &= DESKTOP_WIDGET_ALL;
        if (g_widget_settings.layout > 2) {
            g_widget_settings.layout = 0;
        }
    }
}

static bool draw_background_image(const char *path)
{
    if (path == NULL || path[0] == '\0') return false;

    UINT32 image_width = 0;
    UINT32 image_height = 0;
    xapi_GetPicSize(&image_width, &image_height, (char *)path);
    if (image_width == 0 || image_height == 0) return false;

    xapi_DrawPicture(g_desktop_handle, 0, 0, (UINT32)g_screen_width, (UINT32)g_screen_height, (char *)path);
    return true;
}

static void draw_background(const char *preferred_path)
{
    if (draw_background_image(preferred_path)) return;
    if (preferred_path == NULL || strcmp(preferred_path, DEFAULT_DESKTOP_BACKGROUND) != 0)
    {
        if (draw_background_image(DEFAULT_DESKTOP_BACKGROUND)) return;
    }
    if (draw_background_image(FALLBACK_DESKTOP_BACKGROUND)) return;

    xapi_DrawRect(g_desktop_handle, 0, 0, (UINT32)g_screen_width - 1, (UINT32)g_screen_height - 1, 0x0f4c9aff, true);
}

static int desktop_rows()
{
    int rows = ((int)g_screen_height - 88) / ICON_CELL_H;
    return rows > 0 ? rows : 1;
}

static int desktop_columns()
{
    int columns = ((int)g_screen_width - 20) / ICON_CELL_W;
    return columns > 0 ? columns : 1;
}

static int desktop_slot_count()
{
    return desktop_rows() * desktop_columns();
}

static int icon_x(int index)
{
    return 20 + ICON_CELL_W * (index / desktop_rows());
}

static int icon_y(int index)
{
    int rows = desktop_rows();
    return 30 + (index - (index / rows) * rows) * ICON_CELL_H;
}

static int icon_image_x(int index)
{
    return icon_x(index) + (ICON_CELL_W - ICON_SIZE) / 2;
}

static void draw_centered_icon_text(const char *text, int index)
{
    int text_width = (int)xapi_CalcTextWidth((char *)text, 11);
    int x = icon_x(index) + ICON_CELL_W / 2 - text_width / 2;
    int y = icon_y(index) + ICON_SIZE + 1;
    xapi_DrawText(g_desktop_handle, x, y + 1, (char *)text, 11, 0x000000ff);
    xapi_DrawText(g_desktop_handle, x, y, (char *)text, 11, 0xffffffff);
}

static void draw_desktop_icon(const char *label, const char *icon_path, int index)
{
    int x = icon_image_x(index);
    int y = icon_y(index);
    xapi_DrawPicture(g_desktop_handle, x, y, ICON_SIZE, ICON_SIZE, (char *)icon_path);
    draw_centered_icon_text(label, index);
}

static void draw_drag_selection_box()
{
    if (!g_dragging_icon || !g_icon_drag_moved) return;

    int x1 = icon_x(g_drag_preview_slot);
    int y1 = icon_y(g_drag_preview_slot);
    int x2 = x1 + ICON_CELL_W - 1;
    int y2 = y1 + ICON_CELL_H - 1;
    xapi_DrawRect(g_desktop_handle, x1, y1, x2, y2, 0x3a8de833, true);
    xapi_DrawRect(g_desktop_handle, x1, y1, x2, y2, WIDGET_BLUE_SOFT, false);
}

static const char *desktop_icon_key(int index)
{
    if (index == 0) return SYSTEM_ICON_KEY;
    if (index == 1) return TRASH_ICON_KEY;
    if (index >= 2 && index - 2 < (int)g_entry_count) return g_entries[index - 2].filename;
    return NULL;
}

static int saved_icon_slot(const char *key, int fallback)
{
    if (key == NULL) return fallback;
    for (UINT32 i = 0; i < g_icon_settings.count; i++)
    {
        if (strcmp(g_icon_settings.positions[i].name, key) == 0) return g_icon_settings.positions[i].slot;
    }
    return fallback;
}

static void set_saved_icon_slot(const char *key, int slot)
{
    if (key == NULL) return;
    for (UINT32 i = 0; i < g_icon_settings.count; i++)
    {
        if (strcmp(g_icon_settings.positions[i].name, key) == 0)
        {
            g_icon_settings.positions[i].slot = slot;
            return;
        }
    }
    if (g_icon_settings.count >= MAX_DESKTOP_ICONS) return;

    DesktopIconPosition *position = &g_icon_settings.positions[g_icon_settings.count++];
    memset(position, 0, sizeof(*position));
    copy_cstr(position->name, sizeof(position->name), key);
    position->slot = slot;
}

static bool icon_slot_used_before(int icon, int slot)
{
    for (int i = 0; i < icon; i++)
    {
        if (g_icon_slots[i] == slot) return true;
    }
    return false;
}

static void arrange_icon_slots()
{
    int slot_count = desktop_slot_count();
    for (int icon = 0; icon < g_icon_count; icon++)
    {
        int slot = saved_icon_slot(desktop_icon_key(icon), icon);
        if (slot < 0 || slot >= slot_count || icon_slot_used_before(icon, slot))
        {
            slot = 0;
            while (slot < slot_count && icon_slot_used_before(icon, slot)) slot++;
            if (slot >= slot_count) slot = icon % slot_count;
        }
        g_icon_slots[icon] = slot;
    }
}

static int icon_at(int x, int y)
{
    for (int icon = g_icon_count - 1; icon >= 0; icon--)
    {
        int slot = g_icon_slots[icon];
        int bx = icon_x(slot);
        int by = icon_y(slot);
        if (x >= bx && x < bx + ICON_CELL_W && y >= by && y < by + ICON_CELL_H) return icon;
    }
    return -1;
}

static int nearest_icon_slot(int x, int y)
{
    int column = (x - 20) / ICON_CELL_W;
    int row = (y - 30) / ICON_CELL_H;
    if (column < 0) column = 0;
    if (row < 0) row = 0;
    if (column >= desktop_columns()) column = desktop_columns() - 1;
    if (row >= desktop_rows()) row = desktop_rows() - 1;
    return column * desktop_rows() + row;
}

static bool desktop_widgets_visible()
{
    return g_widget_settings.enabled_flags != 0;
}

static bool desktop_widget_enabled(UINT32 flag)
{
    return (g_widget_settings.enabled_flags & flag) != 0;
}

static int desktop_widget_panel_width()
{
    int width = (int)g_screen_width / 4;
    if (width < 260) width = 260;
    if (width > 360) width = 360;
    return width;
}

static int desktop_widget_left()
{
    int panel_w = desktop_widget_panel_width();
    if (g_widget_settings.layout == 1) return 22;
    if (g_widget_settings.layout == 2) return ((int)g_screen_width - panel_w) / 2;
    return (int)g_screen_width - panel_w - 24;
}

static void draw_widget_card(int x, int y, int w, int h, const char *title, const char *icon)
{
    xapi_DrawRect(g_desktop_handle, x + 4, y + 5, x + w + 4, y + h + 5, WIDGET_SHADOW, true);
    xapi_DrawRect(g_desktop_handle, x, y, x + w, y + h, WIDGET_GLASS, true);
    xapi_DrawRect(g_desktop_handle, x, y, x + w, y + 3, WIDGET_BLUE, true);
    xapi_DrawRect(g_desktop_handle, x + 14, y + 16, x + 43, y + 45, WIDGET_GLASS_SOFT, true);
    if (icon != NULL) xapi_DrawFA(g_desktop_handle, x + 22, y + 22, 15, (char *)icon);
    if (title != NULL) xapi_DrawText(g_desktop_handle, x + 54, y + 14, (char *)title, 15, WIDGET_WHITE);
}

static void draw_widget_status_text(int x, int y, const char *name, const char *value)
{
    xapi_DrawText(g_desktop_handle, x, y, (char *)name, 11, WIDGET_TEXT_MUTED);
    xapi_DrawText(g_desktop_handle, x + 82, y, (char *)value, 11, WIDGET_WHITE);
}

static void format_memory_text(char *buffer, size_t size)
{
    if (buffer == NULL || size == 0) return;
    memset(buffer, 0, size);
    UINT64 memory_mib = xapi_GetMemorySize() / 1024;
    if (memory_mib >= 1024)
    {
        snprintf(buffer, size, "%llu GB", memory_mib / 1024);
    }
    else
    {
        snprintf(buffer, size, "%llu MB", memory_mib);
    }
}

static void draw_clock_widget(int x, int y, int w)
{
    TimeType tm;
    memset(&tm, 0, sizeof(tm));
    xapi_GetTimeX(&tm);

    draw_widget_card(x, y, w, 136, desktop_tr("时间", "Time"), "user");

    char time_text[24];
    char date_text[64];
    memset(time_text, 0, sizeof(time_text));
    memset(date_text, 0, sizeof(date_text));
    snprintf(time_text, sizeof(time_text), "%02d:%02d", tm.tm_hour % 24, tm.tm_min);
    if (g_desktop_language == XJ380_LANGUAGE_EN_US)
    {
        snprintf(date_text, sizeof(date_text), "%04d-%02d-%02d", tm.tm_year, tm.tm_mon, tm.tm_mday);
    }
    else
    {
        snprintf(date_text, sizeof(date_text), "%d 年 %d 月 %d 日", tm.tm_year, tm.tm_mon, tm.tm_mday);
    }

    xapi_DrawText(g_desktop_handle, x + 18, y + 54, time_text, 30, WIDGET_WHITE);
    xapi_DrawText(g_desktop_handle, x + 20, y + 110, date_text, 11, WIDGET_TEXT_DIM);
}

static void draw_system_widget(int x, int y, int w)
{
    draw_widget_card(x, y, w, 132, desktop_tr("系统状态", "System Status"), "gear");

    char memory_text[64];
    char resolution_text[64];
    char version_text[64];
    memset(memory_text, 0, sizeof(memory_text));
    memset(resolution_text, 0, sizeof(resolution_text));
    memset(version_text, 0, sizeof(version_text));

    format_memory_text(memory_text, sizeof(memory_text));
    snprintf(resolution_text, sizeof(resolution_text), "%llux%llu", g_screen_width, g_screen_height);
    xapi_GetSystemVersion(version_text);

    draw_widget_status_text(x + 20, y + 58, desktop_tr("系统", "System"), version_text);
    draw_widget_status_text(x + 20, y + 82, desktop_tr("内存", "Memory"), memory_text);
    draw_widget_status_text(x + 20, y + 106, desktop_tr("分辨率", "Resolution"), resolution_text);
}

struct WidgetAppShortcut
{
    const char *title_zh;
    const char *title_en;
    const char *path;
    const char *icon;
};

static const WidgetAppShortcut g_widget_apps[] = {
    {"文件", "Files", "/apps/system/fmanager.elf", "folder-open"},
    {"终端", "Terminal", "/apps/system/shell.elf", "file-lines"},
    {"设置", "Settings", "/apps/system/ctrlmenu.elf", "gear"},
    {"文本", "Text", "/apps/builtin/texter.elf", "markdown"},
};

static int widget_app_count()
{
    return (int)(sizeof(g_widget_apps) / sizeof(g_widget_apps[0]));
}

static void widget_app_rect(int widget_x, int widget_y, int index, int *x1, int *y1, int *x2, int *y2)
{
    int col = index % 2;
    int row = index / 2;
    int cell_w = 108;
    int cell_h = 54;
    *x1 = widget_x + 18 + col * (cell_w + 12);
    *y1 = widget_y + 56 + row * (cell_h + 10);
    *x2 = *x1 + cell_w;
    *y2 = *y1 + cell_h;
}

static void draw_apps_widget(int x, int y, int w)
{
    draw_widget_card(x, y, w, 176, desktop_tr("快捷启动", "Quick Launch"), "folder-open");

    for (int i = 0; i < widget_app_count(); i++)
    {
        int x1, y1, x2, y2;
        widget_app_rect(x, y, i, &x1, &y1, &x2, &y2);
        xapi_DrawRect(g_desktop_handle, x1, y1, x2, y2, WIDGET_GLASS_SOFT, true);
        xapi_DrawRect(g_desktop_handle, x1, y1, x1 + 3, y2, WIDGET_BLUE_SOFT, true);
        xapi_DrawFA(g_desktop_handle, x1 + 14, y1 + 15, 16, (char *)g_widget_apps[i].icon);
        xapi_DrawText(g_desktop_handle, x1 + 42, y1 + 16,
                      xj380_tr_lang(g_desktop_language, g_widget_apps[i].title_zh, g_widget_apps[i].title_en), 12,
                      WIDGET_WHITE);
    }
}

static void draw_note_line(const char *line, int x, int y)
{
    if (line == NULL || line[0] == '\0') return;
    xapi_DrawText(g_desktop_handle, x, y, (char *)line, 11, WIDGET_TEXT_DIM);
}

static void draw_note_widget(int x, int y, int w)
{
    draw_widget_card(x, y, w, 132, desktop_tr("便签", "Notes"), "file-lines");

    char line[96];
    int line_y = y + 56;
    const char *cursor = g_widget_settings.note;
    for (int row = 0; row < 3 && cursor != NULL && *cursor != '\0'; row++)
    {
        int len = 0;
        while (cursor[len] != '\0' && cursor[len] != '\n' && len + 1 < (int)sizeof(line))
        {
            line[len] = cursor[len];
            len++;
        }
        line[len] = '\0';
        draw_note_line(line, x + 20, line_y);
        line_y += 22;
        if (cursor[len] == '\n') cursor += len + 1;
        else cursor += len;
    }
}

static void draw_desktop_widgets()
{
    if (!desktop_widgets_visible())
    {
        g_widget_area_x1 = 0;
        g_widget_area_y1 = 0;
        g_widget_area_x2 = 0;
        g_widget_area_y2 = 0;
        return;
    }

    int x = desktop_widget_left();
    int y = 42;
    int w = desktop_widget_panel_width();
    int start_y = y;

    if (desktop_widget_enabled(DESKTOP_WIDGET_CLOCK))
    {
        draw_clock_widget(x, y, w);
        y += 150;
    }
    if (desktop_widget_enabled(DESKTOP_WIDGET_SYSTEM))
    {
        draw_system_widget(x, y, w);
        y += 146;
    }
    if (desktop_widget_enabled(DESKTOP_WIDGET_APPS))
    {
        draw_apps_widget(x, y, w);
        y += 190;
    }
    if (desktop_widget_enabled(DESKTOP_WIDGET_NOTE))
    {
        draw_note_widget(x, y, w);
        y += 146;
    }

    g_widget_area_x1 = x;
    g_widget_area_y1 = start_y;
    g_widget_area_x2 = x + w + 8;
    g_widget_area_y2 = y;
}

static const int DESKTOP_MENU_W = 188;
static const int DESKTOP_MENU_ITEM_H = 30;
static const int DESKTOP_MENU_COUNT = 3;

static void clamp_context_menu_position()
{
    if (g_context_menu_x + DESKTOP_MENU_W > (int)g_screen_width) {
        g_context_menu_x = (int)g_screen_width - DESKTOP_MENU_W - 2;
    }
    if (g_context_menu_y + DESKTOP_MENU_ITEM_H * DESKTOP_MENU_COUNT > (int)g_screen_height) {
        g_context_menu_y = (int)g_screen_height - DESKTOP_MENU_ITEM_H * DESKTOP_MENU_COUNT - 2;
    }
    if (g_context_menu_x < 0) g_context_menu_x = 0;
    if (g_context_menu_y < 24) g_context_menu_y = 24;
}

static void draw_context_menu()
{
    if (!g_context_menu_open) return;

    clamp_context_menu_position();
    int x = g_context_menu_x;
    int y = g_context_menu_y;
    int h = DESKTOP_MENU_ITEM_H * DESKTOP_MENU_COUNT;

    xapi_DrawRect(g_desktop_handle, x + 4, y + 4, x + DESKTOP_MENU_W + 4, y + h + 4, WIDGET_SHADOW, true);
    xapi_DrawRect(g_desktop_handle, x, y, x + DESKTOP_MENU_W, y + h, 0xf6fbffff, true);
    xapi_DrawRect(g_desktop_handle, x, y, x + DESKTOP_MENU_W, y + 2, WIDGET_BLUE, true);

    const char *labels[DESKTOP_MENU_COUNT] = {
        desktop_tr("显示/隐藏小组件", "Show/Hide Widgets"),
        desktop_tr("切换小组件位置", "Move Widgets"),
        desktop_tr("刷新桌面", "Refresh Desktop"),
    };
    for (int i = 0; i < DESKTOP_MENU_COUNT; i++)
    {
        int item_y = y + i * DESKTOP_MENU_ITEM_H;
        xapi_DrawText(g_desktop_handle, x + 12, item_y + 6, (char *)labels[i], 11, 0x16283aff);
        if (i + 1 < DESKTOP_MENU_COUNT) {
            xapi_DrawRect(g_desktop_handle, x + 8, item_y + DESKTOP_MENU_ITEM_H - 1,
                          x + DESKTOP_MENU_W - 8, item_y + DESKTOP_MENU_ITEM_H - 1,
                          0xd5e3efff, true);
        }
    }
}

static void current_desktop_path(char *path, size_t path_size)
{
    UserInfo user;
    memset(&user, 0, sizeof(user));
    xapi_GetCurrentUser(&user);
    if (user.name[0] == '\0')
    {
        copy_cstr(path, path_size, "/users/Root/desktop");
        return;
    }

    memset(path, 0, path_size);
    snprintf(path, path_size, "/users/%s/desktop", user.name);
}

static void draw_desktop_icons()
{
    char desktop_path[256];
    current_desktop_path(desktop_path, sizeof(desktop_path));
    xapi_Mkdir(desktop_path);

    memset(g_entries, 0, sizeof(g_entries));
    g_entry_count = 0;
    xapi_SearchFile(desktop_path, &g_entry_count, g_entries);
    if (g_entry_count == 404)
    {
        g_entry_count = 0;
    }
    if (g_entry_count > 255) g_entry_count = 255;

    g_icon_count = (int)g_entry_count + 2;
    arrange_icon_slots();

    if (!(g_dragging_icon && g_icon_drag_moved && g_drag_icon == 0))
        draw_desktop_icon(desktop_tr("系统", "System"), "/system/icon/d_system.png", g_icon_slots[0]);
    if (!(g_dragging_icon && g_icon_drag_moved && g_drag_icon == 1))
        draw_desktop_icon(desktop_tr("垃圾桶", "Trash"), "/system/icon/bin.png", g_icon_slots[1]);

    for (UINT32 i = 0; i < g_entry_count; i++)
    {
        int index = (int)i + 2;
        if (g_dragging_icon && g_icon_drag_moved && g_drag_icon == index) continue;
        const char *icon_path = g_entries[i].filetype == 1 ? "/system/icon/folder.png" : "/system/icon/texter.png";
        draw_desktop_icon(g_entries[i].filename, icon_path, g_icon_slots[index]);
    }

    if (g_dragging_icon && g_icon_drag_moved && g_drag_icon >= 0 && g_drag_icon < g_icon_count)
    {
        draw_drag_selection_box();
        const char *label = g_drag_icon == 0 ? desktop_tr("系统", "System") :
                            g_drag_icon == 1 ? desktop_tr("垃圾桶", "Trash") :
                            g_entries[g_drag_icon - 2].filename;
        const char *icon_path = g_drag_icon == 0 ? "/system/icon/d_system.png" :
                                g_drag_icon == 1 ? "/system/icon/bin.png" :
                                g_entries[g_drag_icon - 2].filetype == 1 ? "/system/icon/folder.png" :
                                                                        "/system/icon/texter.png";
        draw_desktop_icon(label, icon_path, g_drag_preview_slot);
    }
}

static void refresh_widget_area()
{
    g_widgets_dirty = false;
    if (!desktop_widgets_visible())
    {
        redraw_desktop();
        return;
    }

    int old_x1 = g_widget_area_x1;
    int old_y1 = g_widget_area_y1;
    int old_x2 = g_widget_area_x2;
    int old_y2 = g_widget_area_y2;

    DesktopSettings settings;
    load_desktop_settings(&settings);
    g_desktop_language = settings.Language;
    draw_background(settings.BackgroundFilePath);
    draw_desktop_icons();
    draw_desktop_widgets();

    if (old_x1 <= 0 || old_y1 <= 0 || old_x2 <= old_x1 || old_y2 <= old_y1)
    {
        xapi_RefreshWindow(g_desktop_handle);
        return;
    }

    int x1 = old_x1 < g_widget_area_x1 ? old_x1 : g_widget_area_x1;
    int y1 = old_y1 < g_widget_area_y1 ? old_y1 : g_widget_area_y1;
    int x2 = old_x2 > g_widget_area_x2 ? old_x2 : g_widget_area_x2;
    int y2 = old_y2 > g_widget_area_y2 ? old_y2 : g_widget_area_y2;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)g_screen_width) x2 = (int)g_screen_width;
    if (y2 > (int)g_screen_height) y2 = (int)g_screen_height;
    if (x1 < x2 && y1 < y2) xapi_RefreshPartWindow(g_desktop_handle, x1, y1, x2, y2);
}

static void redraw_desktop()
{
    DesktopSettings settings;
    load_desktop_settings(&settings);
    g_desktop_language = settings.Language;
    draw_background(settings.BackgroundFilePath);
    draw_desktop_icons();
    draw_desktop_widgets();
    draw_context_menu();
    xapi_RefreshWindow(g_desktop_handle);
}

static void run_with_one_arg(const char *path, const char *arg)
{
    char *argv[2];
    argv[0] = (char *)arg;
    argv[1] = NULL;
    xapi_RunArgs((char *)path, argv);
}

static void open_desktop_file(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot != NULL && (strcmp(dot, ".elf") == 0 || strcmp(dot, ".epf") == 0))
    {
        xapi_Run((char *)path);
        return;
    }

    if (dot != NULL)
    {
        if (strcmp(dot, ".png") == 0 || strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0 ||
            strcmp(dot, ".bmp") == 0)
        {
            run_with_one_arg("/apps/builtin/picturer.elf", path);
            return;
        }
        if (strcmp(dot, ".txt") == 0 || strcmp(dot, ".inf") == 0 || strcmp(dot, ".xtb") == 0)
        {
            run_with_one_arg("/apps/builtin/texter.elf", path);
            return;
        }
    }

    run_with_one_arg("/apps/builtin/texter.elf", path);
}

static void open_desktop_icon_at(int x, int y)
{
    char desktop_path[256];
    current_desktop_path(desktop_path, sizeof(desktop_path));

    int icon = icon_at(x, y);
    if (icon < 0) return;
    if (icon < 2)
    {
        if (icon == 0) xapi_Run("/apps/system/fmanager.elf");
        return;
    }

    UINT32 entry = (UINT32)(icon - 2);
    char run_path[512];
    path_join(run_path, sizeof(run_path), desktop_path, g_entries[entry].filename);
    if (g_entries[entry].filetype == 1) run_with_one_arg("/apps/system/fmanager.elf", run_path);
    else open_desktop_file(run_path);
}

static void begin_icon_drag(int x, int y)
{
    if (g_context_menu_open || y < 24) return;
    int icon = icon_at(x, y);
    if (icon < 0) return;

    g_drag_icon = icon;
    g_drag_start_x = x;
    g_drag_start_y = y;
    g_drag_origin_slot = g_icon_slots[icon];
    g_drag_preview_slot = g_drag_origin_slot;
    g_dragging_icon = true;
    g_icon_drag_moved = false;
}

static void move_icon_drag(int x, int y)
{
    if (!g_dragging_icon) return;
    int dx = x - g_drag_start_x;
    int dy = y - g_drag_start_y;
    if (!g_icon_drag_moved && dx > -ICON_DRAG_THRESHOLD && dx < ICON_DRAG_THRESHOLD &&
        dy > -ICON_DRAG_THRESHOLD && dy < ICON_DRAG_THRESHOLD)
        return;

    g_icon_drag_moved = true;
    int slot = nearest_icon_slot(x, y);
    if (slot != g_drag_preview_slot)
    {
        g_drag_preview_slot = slot;
        g_needs_redraw = true;
    }
}

static void end_icon_drag(int x, int y)
{
    if (!g_dragging_icon) return;
    move_icon_drag(x, y);
    if (g_icon_drag_moved)
    {
        int target_slot = g_drag_preview_slot;
        int target_icon = -1;
        for (int icon = 0; icon < g_icon_count; icon++)
        {
            if (icon != g_drag_icon && g_icon_slots[icon] == target_slot)
            {
                target_icon = icon;
                break;
            }
        }

        g_icon_slots[g_drag_icon] = target_slot;
        if (target_icon >= 0) g_icon_slots[target_icon] = g_drag_origin_slot;
        save_icon_layout();
    }

    g_dragging_icon = false;
    g_icon_drag_moved = false;
    g_drag_icon = -1;
    g_needs_redraw = true;
}

static bool open_widget_app_at(int x, int y)
{
    if (!desktop_widget_enabled(DESKTOP_WIDGET_APPS)) return false;

    int widget_x = desktop_widget_left();
    int widget_y = 42;
    int widget_w = desktop_widget_panel_width();
    (void)widget_w;
    if (desktop_widget_enabled(DESKTOP_WIDGET_CLOCK)) widget_y += 150;
    if (desktop_widget_enabled(DESKTOP_WIDGET_SYSTEM)) widget_y += 146;

    for (int i = 0; i < widget_app_count(); i++)
    {
        int x1, y1, x2, y2;
        widget_app_rect(widget_x, widget_y, i, &x1, &y1, &x2, &y2);
        if (x >= x1 && x <= x2 && y >= y1 && y <= y2)
        {
            xapi_Run((char *)g_widget_apps[i].path);
            return true;
        }
    }
    return false;
}

static void handle_widget_command(UINT64 command)
{
    if (command == DESKTOP_CMD_TOGGLE_WIDGETS)
    {
        if (desktop_widgets_visible())
        {
            g_widget_settings.enabled_flags = 0;
        }
        else
        {
            g_widget_settings.enabled_flags = DESKTOP_WIDGET_ALL;
        }
        save_widget_settings();
        g_needs_redraw = true;
    }
    else if (command == DESKTOP_CMD_NEXT_WIDGET_LAYOUT)
    {
        g_widget_settings.layout = (g_widget_settings.layout + 1) % 3;
        save_widget_settings();
        g_needs_redraw = true;
    }
    else if (command == DESKTOP_CMD_REFRESH_WIDGETS)
    {
        g_needs_redraw = true;
    }
}

static bool handle_context_menu_click(int x, int y)
{
    if (!g_context_menu_open) return false;

    int menu_x = g_context_menu_x;
    int menu_y = g_context_menu_y;
    int menu_h = DESKTOP_MENU_ITEM_H * DESKTOP_MENU_COUNT;
    g_context_menu_open = false;

    if (x >= menu_x && x <= menu_x + DESKTOP_MENU_W && y >= menu_y && y <= menu_y + menu_h)
    {
        int item = (y - menu_y) / DESKTOP_MENU_ITEM_H;
        if (item == 0) handle_widget_command(DESKTOP_CMD_TOGGLE_WIDGETS);
        else if (item == 1) handle_widget_command(DESKTOP_CMD_NEXT_WIDGET_LAYOUT);
        else if (item == 2) handle_widget_command(DESKTOP_CMD_REFRESH_WIDGETS);
        g_needs_redraw = true;
        return true;
    }

    g_needs_redraw = true;
    return true;
}

static void desktop_MessagePrcor(UINT64 Type, UINT64 hData, UINT64 lData)
{
    if (Type == MSG_LBUTTON)
    {
        int x = (int)hData;
        int y = (int)lData;
        if (handle_context_menu_click(x, y)) return;
        if (y >= 24 && !open_widget_app_at(x, y)) open_desktop_icon_at(x, y);
    }
    else if (Type == MSG_RBUTTON)
    {
        g_context_menu_x = (int)hData;
        g_context_menu_y = (int)lData;
        g_context_menu_open = true;
        g_needs_redraw = true;
    }
    else if (Type == MSG_CRL)
    {
        handle_widget_command(hData);
    }
    else if (Type == MSG_LBUTTONDOWN)
    {
        begin_icon_drag((int)hData, (int)lData);
    }
    else if (Type == MSG_MOVE)
    {
        move_icon_drag((int)hData, (int)lData);
    }
    else if (Type == MSG_LBUTTONUP)
    {
        end_icon_drag((int)hData, (int)lData);
    }
}

static int desktop_main_impl(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    XWINDOW window;
    memset(&window, 0, sizeof(window));
    window.title = xj380_tr("桌面", "Desktop");
    window.sets = XWIN_DESKTOP;
    xapi_CreateWindow(&g_desktop_handle, &window);
    if (g_desktop_handle == 0) return 1;

    xapi_GetWindowSize(g_desktop_handle, &g_screen_width, &g_screen_height);
    SetMsgPrcor(g_desktop_handle, desktop_MessagePrcor);
    load_widget_settings();
    load_icon_settings();
    redraw_desktop();

    while (1)
    {
        if (g_needs_redraw)
        {
            g_needs_redraw = false;
            redraw_desktop();
        }
        TimeType tm;
        memset(&tm, 0, sizeof(tm));
        xapi_GetTimeX(&tm);
        if (desktop_widgets_visible() && tm.tm_min != g_last_widget_min)
        {
            g_last_widget_min = tm.tm_min;
            g_widgets_dirty = true;
        }
        if (g_widgets_dirty)
        {
            refresh_widget_area();
        }
        xapi_Sleep(g_dragging_icon ? 16 : 200);
    }

    return 0;
}

extern "C" int desktop_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int desktop_main_cpp(int argc, char *argv[], char *envp[])
{
    return desktop_main_impl(argc, argv, envp);
}
