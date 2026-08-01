#include <x3api.h>
#include <krlibc.h>
#include "cm_proto.h"
#include "../../kernel/build_settings.h"

static const char *CM_SETTINGS_PATH = "/system/resources/ctrlmenu.settings";
static const int   CM_MAX_PAGES = 16;
static const int   CM_MAX_ITEMS = 256;
static const int   CM_MAX_ACTIVE_CONTROLS = 192;
static const int   CM_CONTENT_TOP = 132;
static const int   CM_SCROLLBAR_ID = 52000;
static const int   CM_ITEM_BUTTON_BASE = 53000;
static const int   CM_ITEM_SWITCH_BASE = 54000;
static const int   CM_APP_BUTTON_BASE = 55000;

enum CmSettingItemType
{
    CM_ITEM_H1,
    CM_ITEM_H2,
    CM_ITEM_H3,
    CM_ITEM_TEXT,
    CM_ITEM_VALUE,
    CM_ITEM_BUTTON,
    CM_ITEM_INPUT,
    CM_ITEM_SWITCH,
    CM_ITEM_DIVIDER,
    CM_ITEM_SPACE,
    CM_ITEM_DYNAMIC,
};

struct CmSettingItem
{
    CmSettingItemType type;
    char             *fields[5];
    int               screen_y;
    int               height;
    UINT64            input_id;
    bool              input_initialized;
    char              input_buffer[256];
};

struct CmSettingPage
{
    char *id;
    char *title;
    int   first_item;
    int   item_count;
};

struct CmActiveControl
{
    UINT64 id;
    int    type;
};

static char            *g_config_data = NULL;
static CmSettingPage    g_pages[CM_MAX_PAGES];
static CmSettingItem    g_items[CM_MAX_ITEMS];
static CmActiveControl  g_active_controls[CM_MAX_ACTIVE_CONTROLS];
static int              g_page_count = 0;
static int              g_item_count = 0;
static int              g_active_control_count = 0;
static int              g_scroll_y = 0;
static int              g_content_height = 0;
static bool             g_wheel_reverse = false;

static char *cm_trim(char *text)
{
    while (*text == ' ' || *text == '\t') text++;
    size_t length = strlen(text);
    while (length > 0 &&
           (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r'))
    {
        text[--length] = '\0';
    }
    return text;
}

static char *cm_text(char *text)
{
    if (text == NULL) return (char *)"";

    char *separator = strchr(text, '^');
    if (separator == NULL) return text;

    static char buffers[4][256];
    static int  buffer_index = 0;
    char       *buffer = buffers[buffer_index++ % 4];
    memset(buffer, 0, 256);

    if (read_settings_language() == XJ380_LANGUAGE_EN_US)
    {
        strncpy(buffer, separator + 1, 255);
    }
    else
    {
        size_t length = (size_t)(separator - text);
        if (length > 255) length = 255;
        memcpy(buffer, text, length);
        buffer[length] = '\0';
    }

    return buffer;
}

static int cm_split_fields(char *line, char **fields, int capacity)
{
    int count = 0;
    while (count < capacity)
    {
        char *separator = strchr(line, '|');
        if (separator != NULL) *separator = '\0';
        fields[count++] = cm_trim(line);
        if (separator == NULL) break;
        line = separator + 1;
    }
    return count;
}

static bool cm_item_type(const char *name, CmSettingItemType *type)
{
    if (strcmp(name, "h1") == 0) *type = CM_ITEM_H1;
    else if (strcmp(name, "h2") == 0) *type = CM_ITEM_H2;
    else if (strcmp(name, "h3") == 0) *type = CM_ITEM_H3;
    else if (strcmp(name, "text") == 0) *type = CM_ITEM_TEXT;
    else if (strcmp(name, "value") == 0) *type = CM_ITEM_VALUE;
    else if (strcmp(name, "button") == 0) *type = CM_ITEM_BUTTON;
    else if (strcmp(name, "input") == 0) *type = CM_ITEM_INPUT;
    else if (strcmp(name, "switch") == 0) *type = CM_ITEM_SWITCH;
    else if (strcmp(name, "divider") == 0) *type = CM_ITEM_DIVIDER;
    else if (strcmp(name, "space") == 0) *type = CM_ITEM_SPACE;
    else if (strcmp(name, "dynamic") == 0) *type = CM_ITEM_DYNAMIC;
    else return false;
    return true;
}

static int cm_required_fields(CmSettingItemType type)
{
    if (type == CM_ITEM_DIVIDER) return 0;
    if (type == CM_ITEM_VALUE) return 2;
    if (type == CM_ITEM_BUTTON || type == CM_ITEM_INPUT || type == CM_ITEM_SWITCH) return 4;
    return 1;
}

static void cm_parse_config()
{
    CmSettingPage *page = NULL;
    char          *line = g_config_data;
    while (line != NULL && *line != '\0')
    {
        char *next = strchr(line, '\n');
        if (next != NULL) *next++ = '\0';

        char *trimmed = cm_trim(line);
        if (*trimmed != '\0' && *trimmed != '#')
        {
            char *fields[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
            int   field_count = cm_split_fields(trimmed, fields, 6);
            if (field_count >= 3 && strcmp(fields[0], "page") == 0 && g_page_count < CM_MAX_PAGES)
            {
                page = &g_pages[g_page_count++];
                page->id = fields[1];
                page->title = fields[2];
                page->first_item = g_item_count;
                page->item_count = 0;
            }
            else if (page != NULL && g_item_count < CM_MAX_ITEMS)
            {
                CmSettingItemType type;
                if (cm_item_type(fields[0], &type) && field_count - 1 >= cm_required_fields(type))
                {
                    CmSettingItem *item = &g_items[g_item_count++];
                    memset(item, 0, sizeof(*item));
                    item->type = type;
                    for (int i = 1; i < field_count && i <= 5; i++) item->fields[i - 1] = fields[i];
                    page->item_count++;
                }
            }
        }
        line = next;
    }
}

void ctrlmenu_settings_init()
{
    if (g_config_data != NULL) return;

    XFILE *file = xapi_OpenFile((char *)CM_SETTINGS_PATH);
    if (file == NULL || file->length == 0 || file->length > 65535)
    {
        if (file != NULL) xapi_CloseFile(file);
        return;
    }

    g_config_data = (char *)malloc((size_t)file->length + 1);
    if (g_config_data == NULL)
    {
        xapi_CloseFile(file);
        return;
    }
    memcpy(g_config_data, file->buffer, (size_t)file->length);
    g_config_data[file->length] = '\0';
    xapi_CloseFile(file);
    cm_parse_config();
}

static int cm_content_left()
{
    return (int)(win_width / 10 * 3) + 30;
}

static int cm_content_right()
{
    return (int)win_width - 34;
}

static int cm_content_bottom()
{
    return (int)win_height - 12;
}

static int cm_visible_height()
{
    int height = cm_content_bottom() - CM_CONTENT_TOP;
    return height > 1 ? height : 1;
}

static int cm_max_scroll()
{
    int maximum = g_content_height - cm_visible_height();
    return maximum > 0 ? maximum : 0;
}

static void cm_clamp_scroll()
{
    int maximum = cm_max_scroll();
    if (g_scroll_y < 0) g_scroll_y = 0;
    if (g_scroll_y > maximum) g_scroll_y = maximum;
}

static bool cm_fully_visible(int y, int height)
{
    return y >= CM_CONTENT_TOP && y + height <= cm_content_bottom();
}

static void cm_remember_control(UINT64 id, int type)
{
    if (g_active_control_count >= CM_MAX_ACTIVE_CONTROLS) return;
    g_active_controls[g_active_control_count].id = id;
    g_active_controls[g_active_control_count].type = type;
    g_active_control_count++;
}

static void cm_clear_controls()
{
    for (int i = 0; i < g_item_count; i++)
    {
        if (g_items[i].input_id != 0) xapi_GetTextInputBox(g_items[i].input_id, g_items[i].input_buffer);
    }
    for (int i = 0; i < g_active_control_count; i++)
    {
        if (g_active_controls[i].type == 1) xapi_DeleteSwitch(handle, g_active_controls[i].id);
        else if (g_active_controls[i].type == 2) xapi_DeleteTextInputBox(g_active_controls[i].id);
        else xapi_DeleteButton(handle, g_active_controls[i].id);
    }
    for (int i = 0; i < g_item_count; i++) g_items[i].input_id = 0;
    g_active_control_count = 0;
    xapi_DeleteScrollBar(handle, CM_SCROLLBAR_ID);
}

void ctrlmenu_settings_hide_controls()
{
    cm_clear_controls();
}

static char *cm_resolve_value(char *value, char *buffer, size_t size)
{
    if (value == NULL) return (char *)"";
    if (value[0] != '@') return value;

    memset(buffer, 0, size);
    if (strcmp(value, "@os_version") == 0) return (char *)OS_VERSION;
    if (strcmp(value, "@kernel_version") == 0) return (char *)KN_VERSION;
    if (strcmp(value, "@system_version") == 0)
    {
        xapi_GetSystemVersion(buffer);
    }
    else if (strcmp(value, "@cpu") == 0)
    {
        xapi_GetCpuModel(buffer);
    }
    else if (strcmp(value, "@memory") == 0)
    {
        uint64_t memory_kib = xapi_GetMemorySize();
        uint64_t display_size = about_memory_show_mb ? memory_kib / 1024 : memory_kib / 1024 / 1024;
        snprintf(buffer, size, "%llu %s", (unsigned long long)display_size,
                 about_memory_show_mb ? "MB" : "GB");
    }
    else if (strcmp(value, "@resolution") == 0)
    {
        snprintf(buffer, size, "%llux%llu", (unsigned long long)win_width, (unsigned long long)win_height);
    }
    else if (strcmp(value, "@background") == 0)
    {
        char *path = get_background_file_path();
        if (path != NULL) strncpy(buffer, path, size - 1);
    }
    else if (strcmp(value, "@current_time") == 0)
    {
        TimeType time;
        xapi_GetTimeX(&time);
        if (read_settings_language() == XJ380_LANGUAGE_EN_US)
        {
            snprintf(buffer, size, "%04d-%02u-%02u %02u:%02u", time.tm_year, time.tm_mon, time.tm_mday,
                     time.tm_hour, time.tm_min);
        }
        else
        {
            snprintf(buffer, size, "%d 年 %d 月 %d 日 %02u:%02u", time.tm_year, time.tm_mon, time.tm_mday,
                     time.tm_hour, time.tm_min);
        }
    }
    else if (strcmp(value, "@timezone") == 0)
    {
        snprintf(buffer, size, "UTC%+d", read_settings_time_offset());
    }
    else if (strcmp(value, "@memory_mb") == 0)
    {
        strcpy(buffer, about_memory_show_mb ? "1" : "0");
    }
    else if (strcmp(value, "@wheel_reverse") == 0)
    {
        strcpy(buffer, g_wheel_reverse ? "1" : "0");
    }
    else if (strcmp(value, "@language") == 0)
    {
        strcpy(buffer, read_settings_language() == XJ380_LANGUAGE_EN_US ? "English" : "中文");
    }
    return buffer;
}

static bool cm_value_is_true(char *value)
{
    char buffer[256];
    char *resolved = cm_resolve_value(value, buffer, sizeof(buffer));
    return strcmp(resolved, "1") == 0 || strcmp(resolved, "true") == 0 || strcmp(resolved, "on") == 0;
}

static int cm_default_app_count()
{
    UserInfo user;
    xapi_GetCurrentUser(&user);
    char path[256];
    snprintf(path, sizeof(path), "/users/%s/runfile.dat", user.name);
    XFILE *file = xapi_OpenFile(path);
    if (file == NULL) return 0;

    RunfileSettings_Format *settings = (RunfileSettings_Format *)file->buffer;
    int count = 0;
    while (count < 128 && settings->items[count].exname[0] != '\0') count++;
    xapi_CloseFile(file);
    return count;
}

static int cm_network_line_count()
{
    char buffer[1024];
    int  fd = (int)enter_syscall(SYS_OPEN, (uint64_t)"/run/NetworkManager/status", 0, 0, 0, 0, 0);
    if (fd < 0) return 1;
    int got = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (got <= 0) return 1;
    int count = 1;
    for (int i = 0; i < got; i++)
    {
        if (buffer[i] == '\n') count++;
    }
    return count;
}

static int cm_dynamic_height(CmSettingItem *item)
{
    if (item->fields[0] == NULL) return 36;
    if (strcmp(item->fields[0], "default_apps") == 0) return cm_default_app_count() * 54 + 8;
    if (strcmp(item->fields[0], "network_status") == 0) return cm_network_line_count() * 26 + 8;
    return 36;
}

static int cm_item_height(CmSettingItem *item)
{
    switch (item->type)
    {
    case CM_ITEM_H1: return 54;
    case CM_ITEM_H2: return 46;
    case CM_ITEM_H3: return 36;
    case CM_ITEM_TEXT: return 42;
    case CM_ITEM_VALUE: return 48;
    case CM_ITEM_BUTTON: return 56;
    case CM_ITEM_INPUT: return 62;
    case CM_ITEM_SWITCH: return 62;
    case CM_ITEM_DIVIDER: return 22;
    case CM_ITEM_SPACE:
    {
        int height = item->fields[0] == NULL ? 16 : atoi(item->fields[0]);
        return height < 0 ? 0 : height;
    }
    case CM_ITEM_DYNAMIC: return cm_dynamic_height(item);
    }
    return 0;
}

static void cm_draw_card(int y, int height)
{
    xapi_DrawRect(handle, cm_content_left(), y, cm_content_right(), y + height - 6, 0x173c58e8, true);
}

static void cm_draw_default_apps(int y)
{
    UserInfo user;
    xapi_GetCurrentUser(&user);
    char path[256];
    snprintf(path, sizeof(path), "/users/%s/runfile.dat", user.name);
    XFILE *file = xapi_OpenFile(path);
    if (file == NULL)
    {
        if (cm_fully_visible(y, 30))
            xapi_DrawText(handle, cm_content_left(), y,
                          cm_text((char *)"默认应用配置不可用。^Default app configuration is unavailable."), 11,
                          0xffffffff);
        return;
    }

    RunfileSettings_Format *settings = (RunfileSettings_Format *)file->buffer;
    for (int i = 0; i < 128 && settings->items[i].exname[0] != '\0'; i++)
    {
        int row_y = y + i * 54;
        if (!cm_fully_visible(row_y, 50)) continue;
        cm_draw_card(row_y, 54);
        char extension[16] = ".";
        memcpy(extension + 1, settings->items[i].exname, sizeof(settings->items[i].exname));
        extension[sizeof(settings->items[i].exname) + 1] = '\0';
        xapi_DrawText(handle, cm_content_left() + 14, row_y + 7, extension, 11, 0xffffffff);
        xapi_DrawText(handle, cm_content_left() + 112, row_y + 7, settings->items[i].describe, 11, 0xc8f7ffff);
        xapi_DrawText(handle, cm_content_left() + 14, row_y + 28, settings->items[i].runpath, 9, 0xb8d8e8ff);
        UINT64 id = CM_APP_BUTTON_BASE + i;
        xapi_Button(handle, id, cm_content_right() - 82, row_y + 11, cm_text((char *)"更改^Change"));
        cm_remember_control(id, 0);
    }
    xapi_CloseFile(file);
}

static void cm_draw_network_status(int y)
{
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    int fd = (int)enter_syscall(SYS_OPEN, (uint64_t)"/run/NetworkManager/status", 0, 0, 0, 0, 0);
    if (fd < 0)
    {
        if (cm_fully_visible(y, 30))
            xapi_DrawText(handle, cm_content_left(), y,
                          cm_text((char *)"网络运行态不可用。^Network runtime status is unavailable."), 11,
                          0xffffffff);
        return;
    }
    int got = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (got <= 0)
    {
        if (cm_fully_visible(y, 30))
            xapi_DrawText(handle, cm_content_left(), y, cm_text((char *)"状态文件为空。^Status file is empty."), 11,
                          0xffffffff);
        return;
    }

    char *line = buffer;
    int   line_index = 0;
    while (line != NULL && *line != '\0')
    {
        char *next = strchr(line, '\n');
        if (next != NULL) *next++ = '\0';
        char *text = cm_trim(line);
        int   row_y = y + line_index * 26;
        if (*text != '\0' && cm_fully_visible(row_y, 24))
        {
            UINT32 color = text[0] == '[' ? 0x00a2e8ff : 0xffffffff;
            xapi_DrawText(handle, cm_content_left() + 10, row_y, text, 11, color);
        }
        line_index++;
        line = next;
    }
}

static void cm_draw_dynamic(CmSettingItem *item, int y)
{
    if (item->fields[0] == NULL) return;
    if (strcmp(item->fields[0], "default_apps") == 0) cm_draw_default_apps(y);
    else if (strcmp(item->fields[0], "network_status") == 0) cm_draw_network_status(y);
}

static void cm_draw_item(CmSettingItem *item, int item_index, int y, int height)
{
    char value_buffer[512];
    char *value = cm_resolve_value(item->fields[1], value_buffer, sizeof(value_buffer));
    int   left = cm_content_left();
    int   right = cm_content_right();

    if (item->type == CM_ITEM_DYNAMIC)
    {
        cm_draw_dynamic(item, y);
        return;
    }
    if (!cm_fully_visible(y, height)) return;

    switch (item->type)
    {
    case CM_ITEM_H1:
        xapi_DrawText(handle, left, y + 4, cm_text(item->fields[0]), 22, 0xffffffff);
        break;
    case CM_ITEM_H2:
        xapi_DrawText(handle, left, y + 10, cm_text(item->fields[0]), 16, 0xffffffff);
        break;
    case CM_ITEM_H3:
        xapi_DrawText(handle, left, y + 8, cm_text(item->fields[0]), 12, 0x00a2e8ff);
        break;
    case CM_ITEM_TEXT:
        xapi_DrawText(handle, left, y + 8, cm_text(item->fields[0]), 11, 0xb8d8e8ff);
        break;
    case CM_ITEM_VALUE:
        cm_draw_card(y, height);
        xapi_DrawText(handle, left + 14, y + 12, cm_text(item->fields[0]), 11, 0xffffffff);
        xapi_DrawText(handle, left + 220, y + 12, cm_text(value), 11, 0xc8f7ffff);
        break;
    case CM_ITEM_BUTTON:
    {
        cm_draw_card(y, height);
        xapi_DrawText(handle, left + 14, y + 9, cm_text(item->fields[0]), 11, 0xffffffff);
        xapi_DrawText(handle, left + 220, y + 9, cm_text(value), 11, 0xc8f7ffff);
        UINT64 id = CM_ITEM_BUTTON_BASE + item_index;
        xapi_Button(handle, id, right - 96, y + 14, cm_text(item->fields[2]));
        cm_remember_control(id, 0);
        break;
    }
    case CM_ITEM_INPUT:
    {
        cm_draw_card(y, height);
        xapi_DrawText(handle, left + 14, y + 17, cm_text(item->fields[0]), 11, 0xffffffff);
        int input_left = left + 180;
        int input_right = right - 112;
        if (!item->input_initialized)
        {
            strncpy(item->input_buffer, value, sizeof(item->input_buffer) - 1);
            item->input_buffer[sizeof(item->input_buffer) - 1] = '\0';
            item->input_initialized = true;
        }
        item->input_id = xapi_PutTextInputBox(handle, input_left, y + 13, input_right - input_left,
                                               item->input_buffer);
        if (item->input_id != 0) cm_remember_control(item->input_id, 2);
        UINT64 id = CM_ITEM_BUTTON_BASE + item_index;
        xapi_Button(handle, id, right - 96, y + 14, cm_text(item->fields[2]));
        cm_remember_control(id, 0);
        break;
    }
    case CM_ITEM_SWITCH:
    {
        cm_draw_card(y, height);
        xapi_DrawText(handle, left + 14, y + 8, cm_text(item->fields[0]), 11, 0xffffffff);
        xapi_DrawText(handle, left + 14, y + 30, cm_text(item->fields[1]), 9, 0xb8d8e8ff);
        UINT64 id = CM_ITEM_SWITCH_BASE + item_index;
        xapi_PutSwitch(handle, right - 82, y + 16, cm_value_is_true(item->fields[2]) ? 1 : 0, id);
        cm_remember_control(id, 1);
        break;
    }
    case CM_ITEM_DIVIDER:
        xapi_DrawRect(handle, left, y + 10, right, y + 11, 0x4e7d94ff, true);
        break;
    case CM_ITEM_SPACE:
    case CM_ITEM_DYNAMIC:
        break;
    }
}

static void cm_layout_page(CmSettingPage *page, bool draw)
{
    int virtual_y = CM_CONTENT_TOP;
    for (int offset = 0; offset < page->item_count; offset++)
    {
        int item_index = page->first_item + offset;
        CmSettingItem *item = &g_items[item_index];
        int height = cm_item_height(item);
        int screen_y = virtual_y - g_scroll_y;
        item->screen_y = screen_y;
        item->height = height;
        if (draw) cm_draw_item(item, item_index, screen_y, height);
        virtual_y += height;
    }
    g_content_height = virtual_y - CM_CONTENT_TOP;
}

static void cm_draw_sidebar()
{
    int sidebar_right = (int)(win_width / 10 * 3);
    xapi_DrawRect(handle, sidebar_right + 8, 128, sidebar_right + 12, win_height - 8, 0x00a2e8ff, true);
    for (int i = 0; i < g_page_count; i++)
    {
        int y = 132 + i * 44;
        UINT32 color = i + 1 == setting_cindex ? 0x00a2e8ff : 0x173c58e8;
        xapi_DrawRect(handle, 8, y, sidebar_right, y + 34, color, true);
        xapi_DrawText(handle, 18, y + 4, cm_text(g_pages[i].title), 14, 0xffffffff);
        if (i + 1 == setting_cindex)
        {
            xapi_DrawRect(handle, sidebar_right, y, sidebar_right + 8, y + 34, 0x00a2e8ff, true);
        }
    }
}

static void cm_draw_scrollbar()
{
    int maximum = cm_max_scroll();
    if (maximum <= 0) return;
    int track_height = cm_visible_height();
    int thumb_height = track_height * track_height / g_content_height;
    if (thumb_height < 32) thumb_height = 32;
    if (thumb_height > track_height) thumb_height = track_height;
    int travel = track_height - thumb_height;
    int position = maximum > 0 ? g_scroll_y * travel / maximum : 0;
    xapi_PutVerticalScrollBar(handle, win_width - 24, CM_CONTENT_TOP, track_height, thumb_height, CM_SCROLLBAR_ID);
    xapi_SetScrollBarPosition(handle, CM_SCROLLBAR_ID, position);
}

void draw_setting()
{
    ctrlmenu_settings_init();
    cm_clear_controls();
    cm_draw_sidebar();
    if (g_page_count == 0)
    {
        xapi_DrawText(handle, cm_content_left(), CM_CONTENT_TOP,
                      cm_text((char *)"无法读取控制中心配置文件。^Unable to read Control Center configuration."),
                      16, 0xffffffff);
        return;
    }
    if (setting_cindex < 1 || setting_cindex > g_page_count) setting_cindex = 1;

    CmSettingPage *page = &g_pages[setting_cindex - 1];
    cm_layout_page(page, false);
    cm_clamp_scroll();
    cm_layout_page(page, true);
    cm_draw_scrollbar();
}

static void cm_run_action(char *action)
{
    if (action == NULL) return;
    if (strcmp(action, "toggle_memory_unit") == 0) about_memory_show_mb = !about_memory_show_mb;
    else if (strcmp(action, "edit_background") == 0)
    {
        change_setting_background();
        return;
    }
    else if (strcmp(action, "timezone_plus") == 0) change_clock_hour_offset(+1);
    else if (strcmp(action, "timezone_minus") == 0) change_clock_hour_offset(23);
    else if (strcmp(action, "language_zh") == 0) change_settings_language(XJ380_LANGUAGE_ZH_CN);
    else if (strcmp(action, "language_en") == 0) change_settings_language(XJ380_LANGUAGE_EN_US);
    else if (strcmp(action, "toggle_wheel_reverse") == 0) g_wheel_reverse = !g_wheel_reverse;
    draw_background_body();
}

bool ctrlmenu_settings_handle_control(UINT64 id, UINT64 data)
{
    if (id == CM_SCROLLBAR_ID)
    {
        int track_height = cm_visible_height();
        int thumb_height = g_content_height > 0 ? track_height * track_height / g_content_height : track_height;
        if (thumb_height < 32) thumb_height = 32;
        if (thumb_height > track_height) thumb_height = track_height;
        int travel = track_height - thumb_height;
        int position = (int)data;
        if (position < 0) position = 0;
        if (position > travel) position = travel;
        g_scroll_y = travel > 0 ? position * cm_max_scroll() / travel : 0;
        draw_background_body();
        return true;
    }
    if (id >= CM_APP_BUTTON_BASE && id < CM_APP_BUTTON_BASE + 128)
    {
        change_setting_apps((int)(id - CM_APP_BUTTON_BASE));
        return true;
    }

    int item_index = -1;
    if (id >= CM_ITEM_BUTTON_BASE && id < CM_ITEM_BUTTON_BASE + CM_MAX_ITEMS)
        item_index = (int)(id - CM_ITEM_BUTTON_BASE);
    else if (id >= CM_ITEM_SWITCH_BASE && id < CM_ITEM_SWITCH_BASE + CM_MAX_ITEMS)
        item_index = (int)(id - CM_ITEM_SWITCH_BASE);
    if (item_index < 0 || item_index >= g_item_count) return false;

    CmSettingItem *item = &g_items[item_index];
    if (item->type == CM_ITEM_INPUT && item->input_id != 0)
    {
        xapi_GetTextInputBox(item->input_id, item->input_buffer);
        if (item->fields[3] != NULL && strcmp(item->fields[3], "edit_background") == 0)
        {
            set_background_file_path(item->input_buffer);
            return true;
        }
    }
    cm_run_action(item->fields[3]);
    return true;
}

bool ctrlmenu_settings_handle_click(int x, int y)
{
    int sidebar_right = (int)(win_width / 10 * 3);
    if (x <= sidebar_right)
    {
        for (int i = 0; i < g_page_count; i++)
        {
            int item_y = 132 + i * 44;
            if (y >= item_y && y <= item_y + 34)
            {
                if (setting_cindex != i + 1)
                {
                    setting_cindex = i + 1;
                    g_scroll_y = 0;
                }
                draw_background_body();
                return true;
            }
        }
        return false;
    }

    return false;
}

void ctrlmenu_settings_scroll(int delta)
{
    if (delta == 0 || cindex != 3) return;
    int direction = g_wheel_reverse ? -1 : 1;
    int old_scroll = g_scroll_y;
    g_scroll_y -= delta * 54 * direction;
    cm_clamp_scroll();
    if (old_scroll != g_scroll_y) draw_background_body();
}
