#include <errno.h>
#include <cpu/lock.h>
#include <mutex.h>
#include <fs/vfs/vfs.h>
#include <global_color.h>
#include <graphics/GOP.hpp>
#include <graphics/sheet.h>
#include <graphics/window/window.h>
#include <mm/alloc/alloc.h>
#include <pipe.h>
#include <proto.hpp>
#include <syscall/syscall.h>
#include <syscall/pxapi.h>
#include <syscall/xapi_user.h>
#include <ttf.h>

extern bool have_full_screen_app;
extern bool desktop_done;
extern bool allow_to_flush;
extern SHEET_INFO *sht_img;
extern SHEET *top_ct_sheet;
extern XWM_INFO *xwmii;
extern mouse_dec ms_dec;
extern UserInfo *current_user;

static constexpr int TOAST_MAX_VISIBLE = 3;
static constexpr int TOAST_WIDTH = 336;
static constexpr int TOAST_HEIGHT_NO_ACTIONS = 86;
static constexpr int TOAST_HEIGHT_WITH_ACTIONS = 114;
static constexpr int TOAST_PADDING = 12;
static constexpr int TOAST_GAP = 10;
static constexpr int TOAST_TEXT_X = 56;
static constexpr int TOAST_ICON_X = 16;
static constexpr int TOAST_ICON_Y = 16;
static constexpr int TOAST_CLOSE_SIZE = 18;
static constexpr int TOAST_BUTTON_H = 22;
static constexpr int TOAST_BUTTON_GAP = 8;
static constexpr int TOAST_BUTTON_MIN_WIDTH = 62;
static constexpr int TOAST_BUTTON_MAX_WIDTH = 132;
static constexpr uint64_t TOAST_DEFAULT_LIFETIME_NS = 5ULL * 1000ULL * 1000ULL * 1000ULL;
static constexpr int TOAST_MAX_ACTIONS = 2;
static constexpr size_t TOAST_TITLE_MAX = 128;
static constexpr size_t TOAST_TEXT_MAX = 512;
static constexpr size_t TOAST_ICON_PATH_MAX = 256;
static constexpr size_t TOAST_ACTION_TEXT_MAX = 128;

typedef struct ToastAction
{
    char    *text;
    uint64_t id;
} ToastAction;

typedef struct ToastEntry
{
    bool      used;
    uint64_t  notification_id;
    uint64_t  owner_pid;
    uint64_t  key;
    char      title[128];
    char      text[512];
    uint64_t  builtin_icon;
    char      *icon_path;
    ToastAction actions[TOAST_MAX_ACTIONS];
    uint8_t   action_count;
    uint64_t  created_ns;
    uint64_t  expires_ns;
    SHEET    *sheet;
    int       bx;
    int       by;
    int       width;
    int       height;
} ToastEntry;

typedef struct ToastManager
{
    uint64_t  next_id;
    ToastEntry entries[TOAST_MAX_VISIBLE];
} ToastManager;

static ToastManager g_toast_mgr;
static mutex_t      g_toast_lock = {SPIN_INIT, MUTEX_UNLOCKED, NULL, NULL, 0, false};

static bool toast_visible_desktop_ready()
{
    if (!allow_to_flush || sht_img == NULL || top_ct_sheet == NULL || xwmii == NULL || desktop_done == false)
    {
        return false;
    }

    if (have_full_screen_app) return false;
    if (current_user == NULL) return false;

    WINDOWLS *desktop_win = find_window_by_exe_path(xwmii, "/apps/system/desktop.elf");
    WINDOWLS *dock_win    = find_window_by_exe_path(xwmii, "/apps/system/dock.elf");
    return desktop_win != NULL && dock_win != NULL;
}

static uint64_t toast_now()
{
    return nanoTime();
}

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static size_t toast_utf8_safe_len(const char *str, size_t max_len)
{
    if (str == NULL || max_len == 0) return 0;

    size_t len = 0;
    while (len < max_len && str[len] != '\0')
    {
        Rune rune;
        int  char_len = chartorune(&rune, str + len);
        if (char_len <= 0) break;
        if (len + (size_t)char_len > max_len) break;
        len += (size_t)char_len;
    }
    return len;
}

static size_t toast_strlen(const char *str, size_t max_len)
{
    if (str == NULL) return 0;
    size_t len = 0;
    while (len < max_len && str[len] != '\0') len++;
    return len;
}

static void toast_copy_trunc(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) return;
    dst[0] = '\0';
    if (src == NULL) return;
    size_t len = toast_utf8_safe_len(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void toast_free_copied_request(XApiNotificationRequest *req)
{
    if (req == NULL) return;
    free(req->title);
    free(req->text);
    free(req->icon_path);
    free(req->action0_text);
    free(req->action1_text);
    memset(req, 0, sizeof(*req));
}

static int toast_copy_optional_user_string(char **out, char *src, size_t max_len)
{
    if (out == NULL) return -EINVAL;
    *out = NULL;
    if (src == NULL) return 0;
    return xapi_copy_string_from_user(out, src, max_len);
}

static int toast_copy_user_request(XApiNotificationRequest *dst, const XApiNotificationRequest *user_req)
{
    if (dst == NULL || user_req == NULL) return -EINVAL;
    memset(dst, 0, sizeof(*dst));

    page_directory_t *pagedir = xapi_current_pagedir();
    if (pagedir == NULL) return -EFAULT;

    XApiNotificationRequest local;
    if (!copy_from_user_pagedir(pagedir, &local, user_req, sizeof(local))) return -EFAULT;

    dst->key          = local.key;
    dst->builtin_icon = local.builtin_icon;

    int ret = xapi_copy_string_from_user(&dst->title, local.title, TOAST_TITLE_MAX);
    if (ret < 0) goto fail;
    ret = toast_copy_optional_user_string(&dst->text, local.text, TOAST_TEXT_MAX);
    if (ret < 0) goto fail;
    ret = toast_copy_optional_user_string(&dst->icon_path, local.icon_path, TOAST_ICON_PATH_MAX);
    if (ret < 0) goto fail;
    ret = toast_copy_optional_user_string(&dst->action0_text, local.action0_text, TOAST_ACTION_TEXT_MAX);
    if (ret < 0) goto fail;
    ret = toast_copy_optional_user_string(&dst->action1_text, local.action1_text, TOAST_ACTION_TEXT_MAX);
    if (ret < 0) goto fail;

    dst->action0_id = local.action0_id;
    dst->action1_id = local.action1_id;
    return 0;

fail:
    toast_free_copied_request(dst);
    return ret;
}

static void toast_free_action(ToastAction *action)
{
    if (action == NULL) return;
    if (action->text != NULL)
    {
        free(action->text);
        action->text = NULL;
    }
    action->id = 0;
}

static void toast_free_entry(ToastEntry *entry)
{
    if (entry == NULL) return;
    int old_x = entry->bx;
    int old_y = entry->by;
    int old_w = entry->width;
    int old_h = entry->height;
    if (entry->sheet != NULL)
    {
        delete_sheet(sht_img, entry->sheet);
        entry->sheet = NULL;
        if (sht_img != NULL) refresh_part_sheet(sht_img, old_x, old_y, old_x + old_w, old_y + old_h);
    }
    if (entry->icon_path != NULL)
    {
        free(entry->icon_path);
        entry->icon_path = NULL;
    }
    for (uint8_t i = 0; i < entry->action_count; ++i)
    {
        toast_free_action(&entry->actions[i]);
    }
    memset(entry, 0, sizeof(*entry));
}

static void toast_clear_entry(ToastEntry *entry)
{
    if (entry == NULL) return;
    toast_free_entry(entry);
}

static bool toast_is_active_entry(const ToastEntry *entry)
{
    return entry != NULL && entry->used && entry->sheet != NULL && sheet_contains(sht_img, entry->sheet);
}

static bool toast_process_matches(const ToastEntry *entry, pcb_t process)
{
    if (entry == NULL || process == NULL) return false;
    return entry->owner_pid == process->pid;
}

static void toast_release_slot(int index)
{
    if (index < 0 || index >= TOAST_MAX_VISIBLE) return;
    toast_clear_entry(&g_toast_mgr.entries[index]);
}

static void toast_shift_remove(int index)
{
    if (index < 0 || index >= TOAST_MAX_VISIBLE) return;
    toast_release_slot(index);
    for (int i = index; i < TOAST_MAX_VISIBLE - 1; ++i)
    {
        if (!g_toast_mgr.entries[i + 1].used) break;
        g_toast_mgr.entries[i] = g_toast_mgr.entries[i + 1];
        memset(&g_toast_mgr.entries[i + 1], 0, sizeof(g_toast_mgr.entries[i + 1]));
    }
}

static void toast_render_builtin_icon(SHEET *sheet, int x, int y, uint64_t icon)
{
    if (sheet == NULL) return;

    SHEET_BUFFER accent = {0x28, 0x78, 0xf0, 0xff};
    SHEET_BUFFER success = {0x2d, 0xb5, 0x5d, 0xff};
    SHEET_BUFFER warning = {0xf2, 0xa0, 0x23, 0xff};
    SHEET_BUFFER error   = {0xe3, 0x4d, 0x5f, 0xff};
    SHEET_BUFFER neutral = {0x61, 0x6c, 0x7f, 0xff};

    SHEET_BUFFER fill = neutral;
    if (icon == XNOTIFY_ICON_INFO) fill = accent;
    else if (icon == XNOTIFY_ICON_SUCCESS) fill = success;
    else if (icon == XNOTIFY_ICON_WARNING) fill = warning;
    else if (icon == XNOTIFY_ICON_ERROR) fill = error;
    else if (icon == XNOTIFY_ICON_APP) fill = accent;

    draw_rect(sht_img, sheet, x, y, x + 23, y + 23, {0xec, 0xf2, 0xf8, 0xff});
    draw_rect(sht_img, sheet, x + 2, y + 2, x + 21, y + 21, fill);
    if (icon == XNOTIFY_ICON_INFO || icon == XNOTIFY_ICON_APP)
    {
        draw_rect(sht_img, sheet, x + 11, y + 6, x + 13, y + 16, {0xff, 0xff, 0xff, 0xff});
        draw_rect(sht_img, sheet, x + 11, y + 4, x + 13, y + 5, {0xff, 0xff, 0xff, 0xff});
    }
    else if (icon == XNOTIFY_ICON_SUCCESS)
    {
        draw_line(sht_img, sheet, x + 7, y + 12, x + 10, y + 15, {0xff, 0xff, 0xff, 0xff});
        draw_line(sht_img, sheet, x + 10, y + 15, x + 16, y + 8, {0xff, 0xff, 0xff, 0xff});
    }
    else if (icon == XNOTIFY_ICON_WARNING)
    {
        draw_line(sht_img, sheet, x + 12, y + 6, x + 12, y + 15, {0xff, 0xff, 0xff, 0xff});
        draw_rect(sht_img, sheet, x + 11, y + 17, x + 13, y + 18, {0xff, 0xff, 0xff, 0xff});
    }
    else if (icon == XNOTIFY_ICON_ERROR)
    {
        draw_line(sht_img, sheet, x + 7, y + 7, x + 16, y + 16, {0xff, 0xff, 0xff, 0xff});
        draw_line(sht_img, sheet, x + 16, y + 7, x + 7, y + 16, {0xff, 0xff, 0xff, 0xff});
    }
}

static void toast_draw_text_line(SHEET *sheet, int x, int y, int size, const char *text, SHEET_BUFFER color, int max_width)
{
    if (sheet == NULL || text == NULL) return;
    char buffer[512];
    toast_copy_trunc(buffer, sizeof(buffer), text);
    uint64_t width = calc_ttf_length(buffer, size);
    if ((int)width > max_width)
    {
        size_t len = toast_strlen(buffer, sizeof(buffer) - 1);
        while (len > 0)
        {
            len = toast_utf8_safe_len(buffer, len - 1);
            buffer[len] = '\0';
            width = calc_ttf_length(buffer, size);
            if ((int)width <= max_width) break;
        }
    }
    print_box_ttf(sht_img, sheet, buffer, color, x, y, size);
}

static size_t toast_fit_text(char *buffer, size_t buffer_size, int size, int max_width)
{
    if (buffer == NULL || buffer_size == 0) return 0;

    size_t len = toast_strlen(buffer, buffer_size - 1);
    uint64_t width = calc_ttf_length(buffer, size);
    while (len > 0 && (int)width > max_width)
    {
        len = toast_utf8_safe_len(buffer, len - 1);
        buffer[len] = '\0';
        width = calc_ttf_length(buffer, size);
    }
    return len;
}

static const char *toast_next_word_or_space(const char *text)
{
    if (text == NULL) return NULL;
    while (*text != '\0' && *text != ' ' && *text != '\n' && *text != '\t') text++;
    while (*text == ' ' || *text == '\n' || *text == '\t') text++;
    return text;
}

static void toast_draw_text_block(SHEET *sheet, int x, int y, int size, const char *text, SHEET_BUFFER color,
                                  int max_width)
{
    if (sheet == NULL || text == NULL || text[0] == '\0') return;

    char line0[256];
    toast_copy_trunc(line0, sizeof(line0), text);
    size_t line0_len = toast_fit_text(line0, sizeof(line0), size, max_width);
    print_box_ttf(sht_img, sheet, line0, color, x, y, size);

    const char *rest = toast_next_word_or_space(text + line0_len);
    if (rest == NULL || *rest == '\0') return;

    char line1[256];
    toast_copy_trunc(line1, sizeof(line1), rest);
    toast_fit_text(line1, sizeof(line1), size, max_width);
    print_box_ttf(sht_img, sheet, line1, color, x, y + 16, size);
}

static void toast_draw_border(SHEET *sheet, SHEET_BUFFER color)
{
    if (sheet == NULL) return;
    draw_rect(sht_img, sheet, 0, 0, sheet->width - 1, 0, color);
    draw_rect(sht_img, sheet, 0, sheet->height - 1, sheet->width - 1, sheet->height - 1, color);
    draw_rect(sht_img, sheet, 0, 0, 0, sheet->height - 1, color);
    draw_rect(sht_img, sheet, sheet->width - 1, 0, sheet->width - 1, sheet->height - 1, color);
}

static int toast_button_width(const char *label)
{
    int text_w = label != NULL ? (int)calc_ttf_length((char *)label, 10) : 0;
    int width = text_w + 18;
    if (width < TOAST_BUTTON_MIN_WIDTH) width = TOAST_BUTTON_MIN_WIDTH;
    if (width > TOAST_BUTTON_MAX_WIDTH) width = TOAST_BUTTON_MAX_WIDTH;
    return width;
}

static void toast_draw_button_label(SHEET *sheet, const char *label, int x, int y, int width)
{
    char buffer[128];
    toast_copy_trunc(buffer, sizeof(buffer), label != NULL ? label : "");
    toast_fit_text(buffer, sizeof(buffer), 10, width - 18);
    print_box_ttf(sht_img, sheet, buffer, {0x1b, 0x27, 0x39, 0xff}, x + 9, y + 5, 10);
}

static void toast_draw_entry(ToastEntry *entry)
{
    if (entry == NULL || entry->sheet == NULL) return;

    SHEET *sheet = entry->sheet;
    draw_rect(sht_img, sheet, 0, 0, entry->width - 1, entry->height - 1, {0xf7, 0xf9, 0xfc, 0xff});
    toast_draw_border(sheet, {0xcf, 0xd8, 0xe3, 0xff});
    draw_rect(sht_img, sheet, 0, 0, entry->width - 1, 2, {0x28, 0x78, 0xf0, 0xff});

    if (entry->icon_path != NULL && entry->icon_path[0] != '\0')
    {
        if (!LoadPicture((SHEET_BUFFER *)sheet->buffer + TOAST_ICON_Y * entry->width + TOAST_ICON_X, 24, 24,
                         entry->icon_path))
        {
            toast_render_builtin_icon(sheet, TOAST_ICON_X, TOAST_ICON_Y, entry->builtin_icon);
        }
    }
    else
    {
        toast_render_builtin_icon(sheet, TOAST_ICON_X, TOAST_ICON_Y, entry->builtin_icon);
    }

    draw_rect(sht_img, sheet, entry->width - TOAST_CLOSE_SIZE - 10, 10, entry->width - 10, 10 + TOAST_CLOSE_SIZE,
              {0xee, 0xf2, 0xf7, 0xff});
    draw_line(sht_img, sheet, entry->width - TOAST_CLOSE_SIZE - 6, 14, entry->width - 14, 22,
              {0x5c, 0x6b, 0x82, 0xff});
    draw_line(sht_img, sheet, entry->width - TOAST_CLOSE_SIZE - 6, 22, entry->width - 14, 14,
              {0x5c, 0x6b, 0x82, 0xff});

    toast_draw_text_line(sheet, TOAST_TEXT_X, 12, 14, entry->title, {0x17, 0x24, 0x35, 0xff}, entry->width - 78);
    toast_draw_text_block(sheet, TOAST_TEXT_X, 34, 11, entry->text, {0x48, 0x59, 0x70, 0xff}, entry->width - 76);

    int button_y = entry->height - TOAST_BUTTON_H - 12;
    int button_x = entry->width - 12;
    for (int i = (int)entry->action_count - 1; i >= 0; --i)
    {
        const char *label = entry->actions[i].text != NULL ? entry->actions[i].text : "";
        int button_w = toast_button_width(label);
        button_x -= button_w;
        draw_rect(sht_img, sheet, button_x, button_y, button_x + button_w, button_y + TOAST_BUTTON_H,
                  {0xe8, 0xee, 0xf5, 0xff});
        draw_rect(sht_img, sheet, button_x, button_y, button_x + button_w, button_y + 1,
                  {0xc7, 0xd2, 0xe1, 0xff});
        draw_rect(sht_img, sheet, button_x, button_y + TOAST_BUTTON_H - 1, button_x + button_w, button_y + TOAST_BUTTON_H - 1,
                  {0xc7, 0xd2, 0xe1, 0xff});
        toast_draw_button_label(sheet, label, button_x, button_y, button_w);
        button_x -= TOAST_BUTTON_GAP;
    }
}

static void toast_layout_entry(ToastEntry *entry, int slot)
{
    if (entry == NULL || entry->sheet == NULL || sht_img == NULL) return;

    int old_x = entry->bx;
    int old_y = entry->by;
    int screen_w = (int)sht_img->scrx;
    int screen_h = (int)sht_img->scry;
    int stack_bottom = screen_h - 87;
    int x = screen_w - TOAST_WIDTH - TOAST_PADDING;
    int y = stack_bottom - (entry->height + TOAST_GAP) * (slot + 1) + TOAST_GAP;
    if (y < 28) y = 28;

    entry->bx = x;
    entry->by = y;
    entry->sheet->bx = x;
    entry->sheet->by = y;
    if ((old_x != 0 || old_y != 0) && (old_x != x || old_y != y))
    {
        refresh_part_sheet(sht_img, old_x, old_y, old_x + entry->width, old_y + entry->height);
    }
}

static void toast_refresh_slot(int index)
{
    if (index < 0 || index >= TOAST_MAX_VISIBLE) return;
    ToastEntry *entry = &g_toast_mgr.entries[index];
    if (!entry->used || entry->sheet == NULL) return;
    toast_layout_entry(entry, index);
    toast_draw_entry(entry);
    refresh_part_sheet(sht_img, entry->bx, entry->by, entry->bx + entry->width, entry->by + entry->height);
}

static void toast_reflow()
{
    for (int i = 0; i < TOAST_MAX_VISIBLE; ++i)
    {
        if (!g_toast_mgr.entries[i].used) continue;
        toast_refresh_slot(i);
    }
}

static bool toast_create_sheet(ToastEntry *entry)
{
    if (entry == NULL || sht_img == NULL) return false;

    SHEET *sheet = NULL;
    if (!create_sheet(sht_img, 0, 0, entry->width, entry->height, TopWindowSheetType, 32764, &sheet))
    {
        return false;
    }
    entry->sheet = sheet;
    return true;
}

static bool toast_recreate_sheet_if_needed(ToastEntry *entry)
{
    if (entry == NULL || entry->sheet == NULL) return false;
    if (entry->sheet->width == (uint32_t)entry->width && entry->sheet->height == (uint32_t)entry->height) return true;

    int old_x = entry->bx;
    int old_y = entry->by;
    int old_w = (int)entry->sheet->width;
    int old_h = (int)entry->sheet->height;
    delete_sheet(sht_img, entry->sheet);
    entry->sheet = NULL;
    if (sht_img != NULL) refresh_part_sheet(sht_img, old_x, old_y, old_x + old_w, old_y + old_h);
    return toast_create_sheet(entry);
}

static void toast_init_entry(ToastEntry *entry)
{
    if (entry == NULL) return;
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->width = TOAST_WIDTH;
    entry->height = TOAST_HEIGHT_NO_ACTIONS;
    entry->created_ns = toast_now();
    entry->expires_ns = entry->created_ns + TOAST_DEFAULT_LIFETIME_NS;
}

static int toast_parse_icon(uint64_t icon)
{
    if (icon > XNOTIFY_ICON_APP) return XNOTIFY_ICON_NONE;
    return (int)icon;
}

static bool toast_copy_action(ToastAction *dst, char *text, uint64_t id)
{
    if (dst == NULL) return false;
    dst->text = NULL;
    dst->id = id;
    if (text == NULL || text[0] == '\0') return false;
    size_t len = toast_strlen(text, 127);
    dst->text = (char *)malloc(len + 1);
    if (dst->text == NULL) return false;
    memcpy(dst->text, text, len);
    dst->text[len] = '\0';
    return true;
}

static bool toast_validate_request(const XApiNotificationRequest *req)
{
    if (req == NULL || req->title == NULL || req->title[0] == '\0') return false;
    bool has_action0_text = req->action0_text != NULL && req->action0_text[0] != '\0';
    bool has_action1_text = req->action1_text != NULL && req->action1_text[0] != '\0';
    if (has_action0_text != (req->action0_id != 0)) return false;
    if (has_action1_text != (req->action1_id != 0)) return false;
    return true;
}

static bool toast_request_has_actions(const XApiNotificationRequest *req)
{
    return req != NULL && (((req->action0_text != NULL && req->action0_text[0] != '\0') && req->action0_id != 0) ||
                           ((req->action1_text != NULL && req->action1_text[0] != '\0') && req->action1_id != 0));
}

static void toast_update_from_request(ToastEntry *entry, const XApiNotificationRequest *req, uint64_t owner_pid)
{
    if (entry == NULL || req == NULL) return;

    toast_copy_trunc(entry->title, sizeof(entry->title), req->title);
    toast_copy_trunc(entry->text, sizeof(entry->text), req->text != NULL ? req->text : "");
    entry->builtin_icon = toast_parse_icon(req->builtin_icon);
    entry->owner_pid    = owner_pid;
    entry->key          = req->key;
    entry->created_ns    = toast_now();
    entry->expires_ns    = entry->created_ns + TOAST_DEFAULT_LIFETIME_NS;

    if (entry->icon_path != NULL)
    {
        free(entry->icon_path);
        entry->icon_path = NULL;
    }
    if (req->icon_path != NULL && req->icon_path[0] != '\0')
    {
        size_t len = toast_utf8_safe_len(req->icon_path, 255);
        entry->icon_path = (char *)malloc(len + 1);
        if (entry->icon_path != NULL)
        {
            memcpy(entry->icon_path, req->icon_path, len);
            entry->icon_path[len] = '\0';
        }
    }

    for (uint8_t i = 0; i < entry->action_count; ++i) toast_free_action(&entry->actions[i]);
    entry->action_count = 0;
    if (req->action0_text != NULL && req->action0_id != 0)
    {
        if (toast_copy_action(&entry->actions[entry->action_count], req->action0_text, req->action0_id))
        {
            entry->action_count++;
        }
    }
    if (req->action1_text != NULL && req->action1_id != 0 && entry->action_count < TOAST_MAX_ACTIONS)
    {
        if (toast_copy_action(&entry->actions[entry->action_count], req->action1_text, req->action1_id))
        {
            entry->action_count++;
        }
    }

    entry->height = entry->action_count > 0 ? TOAST_HEIGHT_WITH_ACTIONS : TOAST_HEIGHT_NO_ACTIONS;
}

static uint64_t toast_allocate_id()
{
    if (g_toast_mgr.next_id == 0) g_toast_mgr.next_id = 1;
    return g_toast_mgr.next_id++;
}

static int toast_count_used()
{
    int count = 0;
    for (int i = 0; i < TOAST_MAX_VISIBLE; ++i)
    {
        if (g_toast_mgr.entries[i].used) count++;
    }
    return count;
}

static int toast_find_key_slot(uint64_t owner_pid, uint64_t key)
{
    if (key == 0) return -1;
    for (int i = 0; i < TOAST_MAX_VISIBLE; ++i)
    {
        ToastEntry *entry = &g_toast_mgr.entries[i];
        if (entry->used && entry->owner_pid == owner_pid && entry->key == key) return i;
    }
    return -1;
}

static int toast_make_bottom_slot()
{
    if (toast_count_used() >= TOAST_MAX_VISIBLE)
    {
        toast_shift_remove(TOAST_MAX_VISIBLE - 1);
    }

    int count = toast_count_used();
    if (count < 0 || count >= TOAST_MAX_VISIBLE) return -1;
    for (int i = count; i > 0; --i)
    {
        g_toast_mgr.entries[i] = g_toast_mgr.entries[i - 1];
        memset(&g_toast_mgr.entries[i - 1], 0, sizeof(g_toast_mgr.entries[i - 1]));
    }
    return 0;
}

static int toast_find_id_slot(uint64_t notification_id)
{
    if (notification_id == 0) return -1;
    for (int i = 0; i < TOAST_MAX_VISIBLE; ++i)
    {
        if (g_toast_mgr.entries[i].used && g_toast_mgr.entries[i].notification_id == notification_id) return i;
    }
    return -1;
}

static void toast_clear_all_entries()
{
    for (int i = 0; i < TOAST_MAX_VISIBLE; ++i)
    {
        toast_release_slot(i);
    }
}

static uint64_t toast_show_request(const XApiNotificationRequest *req, uint64_t owner_pid)
{
    if (!toast_visible_desktop_ready())
    {
        return 0;
    }
    if (!toast_validate_request(req))
    {
        return (uint64_t)-EINVAL;
    }

    if (mutex_lock(&g_toast_lock) < 0) return (uint64_t)-EBUSY;
    if (!toast_visible_desktop_ready())
    {
        mutex_unlock(&g_toast_lock);
        return 0;
    }

    int slot = toast_find_key_slot(owner_pid, req->key);
    if (slot < 0) slot = toast_make_bottom_slot();
    if (slot < 0 || slot >= TOAST_MAX_VISIBLE)
    {
        mutex_unlock(&g_toast_lock);
        return (uint64_t)-ENOMEM;
    }

    ToastEntry *entry = &g_toast_mgr.entries[slot];
    if (!entry->used)
    {
        toast_init_entry(entry);
        if (!toast_create_sheet(entry))
        {
            toast_clear_entry(entry);
            mutex_unlock(&g_toast_lock);
            return (uint64_t)-ENOMEM;
        }
        entry->notification_id = toast_allocate_id();
    }
    else if (entry->key == req->key && req->key != 0)
    {
        // Preserve notification_id and slot for same-key updates.
    }
    else
    {
        toast_clear_entry(entry);
        toast_init_entry(entry);
        if (!toast_create_sheet(entry))
        {
            toast_clear_entry(entry);
            mutex_unlock(&g_toast_lock);
            return (uint64_t)-ENOMEM;
        }
        entry->notification_id = toast_allocate_id();
    }

    toast_update_from_request(entry, req, owner_pid);
    if (!toast_recreate_sheet_if_needed(entry))
    {
        toast_clear_entry(entry);
        mutex_unlock(&g_toast_lock);
        return (uint64_t)-ENOMEM;
    }
    entry->used = true;
    toast_layout_entry(entry, slot);
    toast_draw_entry(entry);
    refresh_part_sheet(sht_img, entry->bx, entry->by, entry->bx + entry->width, entry->by + entry->height);
    toast_reflow();
    uint64_t notification_id = entry->notification_id;
    mutex_unlock(&g_toast_lock);
    return notification_id;
}

static void toast_tick()
{
    uint64_t now = toast_now();
    for (int i = 0; i < TOAST_MAX_VISIBLE; ++i)
    {
        ToastEntry *entry = &g_toast_mgr.entries[i];
        if (!entry->used) continue;
        if (now < entry->expires_ns) continue;
        toast_shift_remove(i);
        --i;
    }
}

void toast_manager_flush()
{
    if (mutex_lock(&g_toast_lock) < 0) return;
    if (!toast_visible_desktop_ready())
    {
        toast_clear_all_entries();
        mutex_unlock(&g_toast_lock);
        return;
    }
    toast_tick();
    toast_reflow();
    mutex_unlock(&g_toast_lock);
}

void toast_manager_mark_process_exit(pcb_t process)
{
    if (process == NULL) return;
    if (mutex_lock(&g_toast_lock) < 0) return;
    for (int i = 0; i < TOAST_MAX_VISIBLE; ++i)
    {
        ToastEntry *entry = &g_toast_mgr.entries[i];
        if (!toast_process_matches(entry, process)) continue;
        toast_shift_remove(i);
        --i;
    }
    toast_reflow();
    mutex_unlock(&g_toast_lock);
}

static bool toast_hit_close(const ToastEntry *entry, int x, int y)
{
    if (entry == NULL || entry->sheet == NULL) return false;
    int left = entry->bx + entry->width - TOAST_CLOSE_SIZE - 10;
    int top = entry->by + 10;
    return x >= left && x <= left + TOAST_CLOSE_SIZE && y >= top && y <= top + TOAST_CLOSE_SIZE;
}

static int toast_hit_action(const ToastEntry *entry, int x, int y)
{
    if (entry == NULL || entry->sheet == NULL || entry->action_count == 0) return -1;
    int button_y = entry->by + entry->height - TOAST_BUTTON_H - 12;
    int button_x = entry->bx + entry->width - 12;
    for (int i = (int)entry->action_count - 1; i >= 0; --i)
    {
        const char *label = entry->actions[i].text != NULL ? entry->actions[i].text : "";
        int button_w = toast_button_width(label);
        button_x -= button_w;
        if (x >= button_x && x <= button_x + button_w && y >= button_y && y <= button_y + TOAST_BUTTON_H)
        {
            return i;
        }
        button_x -= TOAST_BUTTON_GAP;
    }
    return -1;
}

static void toast_dispatch_action(uint64_t owner_pid, uint64_t notification_id, uint64_t action_id)
{
    if (owner_pid == 0 || notification_id == 0 || action_id == 0) return;

    pcb_t owner = found_pcb((int)owner_pid);
    if (owner == NULL || !owner->notify_pcor_registered || owner->notify_pcor_pipe_write_fd < 0) return;
    if (!toast_visible_desktop_ready()) return;

    fd_file_handle *handle =
        (fd_file_handle *)queue_get(owner->file_open, (size_t)owner->notify_pcor_pipe_write_fd);
    if (handle == NULL || handle->node == NULL) return;

    MessageInfoFormat mif;
    memset(&mif, 0, sizeof(mif));
    mif.WinMpf   = (MsgPrcor)owner->notify_pcor_func;
    mif.msg_type = notification_id;
    mif.hData    = action_id;
    mif.lData    = 0;

    vfs_write(handle->node, &mif, 0, sizeof(mif));
}

bool toast_manager_handle_mouse_click(int x, int y)
{
    if (mutex_lock(&g_toast_lock) < 0) return false;
    if (!toast_visible_desktop_ready())
    {
        mutex_unlock(&g_toast_lock);
        return false;
    }

    for (int i = TOAST_MAX_VISIBLE - 1; i >= 0; --i)
    {
        ToastEntry *entry = &g_toast_mgr.entries[i];
        if (!toast_is_active_entry(entry)) continue;
        if (x < entry->bx || x > entry->bx + entry->width || y < entry->by || y > entry->by + entry->height)
        {
            continue;
        }

        if (toast_hit_close(entry, x, y))
        {
            toast_shift_remove(i);
            toast_reflow();
            mutex_unlock(&g_toast_lock);
            return true;
        }

        int action_index = toast_hit_action(entry, x, y);
        if (action_index >= 0)
        {
            uint64_t owner_pid       = entry->owner_pid;
            uint64_t notification_id = entry->notification_id;
            uint64_t action_id       = entry->actions[action_index].id;
            mutex_unlock(&g_toast_lock);

            toast_dispatch_action(owner_pid, notification_id, action_id);

            if (mutex_lock(&g_toast_lock) < 0) return true;
            int slot = toast_find_id_slot(notification_id);
            if (slot >= 0)
            {
                toast_shift_remove(slot);
                toast_reflow();
            }
            mutex_unlock(&g_toast_lock);
            return true;
        }

        mutex_unlock(&g_toast_lock);
        return true;
    }
    mutex_unlock(&g_toast_lock);
    return false;
}

uint64_t do_xapi_SendNotification(const XApiNotificationRequest *req);

uint64_t do_xapi_SendAppMessage(char *title, char *text)
{
    pcb_t current = get_current_task() != NULL ? get_current_task()->parent_group : NULL;
    if (current == NULL) return (uint64_t)-EINVAL;
    if (!toast_visible_desktop_ready()) return 0;

    XApiNotificationRequest req;
    memset(&req, 0, sizeof(req));
    int ret = xapi_copy_string_from_user(&req.title, title, TOAST_TITLE_MAX);
    if (ret < 0) return (uint64_t)ret;
    ret = toast_copy_optional_user_string(&req.text, text, TOAST_TEXT_MAX);
    if (ret < 0)
    {
        toast_free_copied_request(&req);
        return (uint64_t)ret;
    }
    req.builtin_icon = XNOTIFY_ICON_INFO;
    uint64_t result = toast_show_request(&req, current->pid);
    toast_free_copied_request(&req);
    return result;
}

uint64_t do_xapi_SendNotification(const XApiNotificationRequest *req)
{
    pcb_t current = get_current_task() != NULL ? get_current_task()->parent_group : NULL;
    if (current == NULL) return (uint64_t)-EINVAL;
    if (!toast_visible_desktop_ready()) return 0;

    XApiNotificationRequest kreq;
    int ret = toast_copy_user_request(&kreq, req);
    if (ret < 0) return (uint64_t)ret;

    if (toast_request_has_actions(&kreq) && !current->notify_pcor_registered)
    {
        toast_free_copied_request(&kreq);
        return (uint64_t)-EINVAL;
    }

    uint64_t result = toast_show_request(&kreq, current->pid);
    toast_free_copied_request(&kreq);
    return result;
}

uint64_t do_xapi_SetNotifyPrcor(uint64_t func)
{
    pcb_t current = get_current_task() != NULL ? get_current_task()->parent_group : NULL;
    if (current == NULL) return (uint64_t)-EINVAL;
    if (func == 0)
    {
        current->notify_pcor_func       = 0;
        current->notify_pcor_registered = false;
        return 0;
    }
    if (!init_notify_message(current, func)) return (uint64_t)-ENOMEM;
    current->notify_pcor_func = func;
    current->notify_pcor_registered = func != 0;
    return 0;
}
