#include <x3api.h>
#include <xj380_i18n.h>

static HDLE   g_handle = 0;
static UINT64 g_width  = 0;
static UINT64 g_height = 0;
static int    g_language = XJ380_LANGUAGE_ZH_CN;

static const UINT32 BLUE       = 0x0f4c9aff;
static const UINT32 ACCENT     = 0x00a2e8ff;
static const UINT32 WHITE      = 0xffffffff;
static const UINT32 BLACK      = 0x000000ff;
static const UINT32 RED        = 0xff2d2dff;
static const UINT32 GREEN      = 0x02e16eff;
static const UINT32 LIGHT_LINE = 0xe6e6e6ff;

static char g_username[64];
static char g_password[64];
static char g_confirm[64];
static int  g_focus = 0;
static bool g_oobe = false;
static LoginUserInfo g_users[128];
static UINT64 g_user_count = 0;
static UINT64 g_selected_user = 0;
static char g_message[160];
static bool g_done = false;
static bool g_frame_drawn = false;
static bool g_oobe_submitting = false;

struct Rect
{
    int x;
    int y;
    int w;
    int h;
};

struct RegionCache
{
    Rect     rect;
    XCOLORA *pixels;
};

static RegionCache g_input_cache[3];
static RegionCache g_message_cache;
static RegionCache g_login_input_cache;
static RegionCache g_login_message_cache;

static const Rect OOBE_INPUT_RECTS[3] = {
    {224, 136, 321, 29},
    {224, 186, 321, 29},
    {224, 236, 321, 29},
};
static const Rect OOBE_MESSAGE_RECT = {64, 350, 620, 28};
static const Rect LOGIN_INPUT_RECT = {128, 168, 301, 29};
static const Rect LOGIN_MESSAGE_RECT = {64, 210, 520, 28};

UINT32 g_message_color = RED;

static char *login_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(g_language, zh_cn, en_us);
}

static void copy_cstr(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) return;
    memset(dst, 0, dst_size);
    if (src == NULL) return;
    strncpy(dst, src, dst_size - 1);
}

static bool username_char_ok(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
           ch == '-' || ch == ' ';
}

static bool username_ok(const char *username)
{
    if (username == NULL || username[0] == '\0') return false;
    if (username[0] == ' ') return false;
    size_t len = strlen(username);
    if (len == 0 || len >= sizeof(g_username) || username[len - 1] == ' ') return false;
    for (size_t i = 0; i < len; i++)
    {
        if (!username_char_ok(username[i])) return false;
    }
    return true;
}

static bool append_char(char *buf, size_t size, char ch)
{
    size_t len = strlen(buf);
    if (len + 1 >= size) return false;
    buf[len] = ch;
    buf[len + 1] = '\0';
    return true;
}

static void backspace_char(char *buf)
{
    size_t len = strlen(buf);
    if (len == 0) return;
    buf[len - 1] = '\0';
}

static void free_region_cache(RegionCache *cache)
{
    if (cache == NULL) return;
    if (cache->pixels != NULL)
    {
        free(cache->pixels);
        cache->pixels = NULL;
    }
    memset(&cache->rect, 0, sizeof(cache->rect));
}

static bool alloc_region_cache(RegionCache *cache, Rect rect)
{
    if (cache == NULL || rect.w <= 0 || rect.h <= 0) return false;
    if (cache->pixels != NULL && cache->rect.w == rect.w && cache->rect.h == rect.h) return true;

    free_region_cache(cache);
    cache->pixels = (XCOLORA *)malloc((size_t)rect.w * (size_t)rect.h * sizeof(XCOLORA));
    if (cache->pixels == NULL) return false;
    cache->rect = rect;
    return true;
}

static void capture_region(RegionCache *cache, Rect rect)
{
    if (!alloc_region_cache(cache, rect)) return;
    xapi_ReadBufferA(g_handle, rect.x, rect.y, rect.w, rect.h, (XCOLOR *)cache->pixels);
}

static void restore_region(RegionCache *cache)
{
    if (cache == NULL || cache->pixels == NULL) return;
    xapi_WriteBufferA(g_handle, cache->rect.x, cache->rect.y, cache->rect.w, cache->rect.h, cache->pixels);
}

static void draw_background()
{
    UINT32 image_width = 0;
    UINT32 image_height = 0;
    xapi_GetPicSize(&image_width, &image_height, (char *)"/system/resources/image/background3.png");
    if (image_width != 0 && image_height != 0)
    {
        xapi_DrawPicture(g_handle, 0, 0, (UINT32)g_width, (UINT32)g_height,
                         (char *)"/system/resources/image/background3.png");
        return;
    }

    xapi_DrawRect(g_handle, 0, 0, (UINT32)g_width - 1, (UINT32)g_height - 1, BLUE, true);
}

static void capture_oobe_dynamic_regions()
{
    for (int i = 0; i < 3; i++) capture_region(&g_input_cache[i], OOBE_INPUT_RECTS[i]);
    capture_region(&g_message_cache, OOBE_MESSAGE_RECT);
}

static void capture_login_dynamic_regions()
{
    capture_region(&g_login_input_cache, LOGIN_INPUT_RECT);
    capture_region(&g_login_message_cache, LOGIN_MESSAGE_RECT);
}

static void draw_input_box(int x, int y, int width, const char *value, bool password, bool focused)
{
    const int height = 28;
    const int text_size = 12;
    const int text_y = y + (height - text_size) / 2 - 2;
    const int cursor_top = y + 5;
    const int cursor_bottom = y + height - 6;
    UINT32 border = focused ? ACCENT : LIGHT_LINE;
    xapi_DrawRect(g_handle, x, y, x + width, y + height, WHITE, true);
    xapi_DrawRect(g_handle, x, y, x + width, y + 1, border, true);
    xapi_DrawRect(g_handle, x, y + height - 2, x + width, y + height, border, true);
    xapi_DrawRect(g_handle, x, y, x + 1, y + height, border, true);
    xapi_DrawRect(g_handle, x + width - 1, y, x + width, y + height, border, true);

    char text[64];
    memset(text, 0, sizeof(text));
    if (password)
    {
        size_t len = strlen(value);
        if (len >= sizeof(text)) len = sizeof(text) - 1;
        for (size_t i = 0; i < len; i++) text[i] = '*';
    }
    else
    {
        copy_cstr(text, sizeof(text), value);
    }
    xapi_DrawText(g_handle, x + 8, text_y, text, text_size, BLACK);

    if (focused)
    {
        int cursor_x = x + 8 + (int)strlen(value) * 8;
        if (cursor_x > x + width - 12) cursor_x = x + width - 12;
        xapi_DrawRect(g_handle, cursor_x, cursor_top, cursor_x + 1, cursor_bottom, BLACK, true);
    }
}

static void draw_oobe()
{
    g_frame_drawn = true;
    draw_background();
    xapi_DrawText(g_handle, 64, 42, login_tr("欢迎使用 XJ380", "Welcome to XJ380"), 36, WHITE);
    xapi_DrawText(g_handle, 64, 98, login_tr("创建第一个管理员账户", "Create the first administrator account"), 16,
                  WHITE);
    xapi_DrawText(g_handle, 64, 142, login_tr("用户名：", "Username:"), 14, WHITE);
    xapi_DrawText(g_handle, 64, 192, login_tr("密码：", "Password:"), 14, WHITE);
    xapi_DrawText(g_handle, 64, 242, login_tr("确认密码：", "Confirm password:"), 14, WHITE);
    capture_oobe_dynamic_regions();

    draw_input_box(224, 136, 320, g_username, false, g_focus == 0);
    draw_input_box(224, 186, 320, g_password, true, g_focus == 1);
    draw_input_box(224, 236, 320, g_confirm, true, g_focus == 2);

    xapi_DrawRect(g_handle, 224, 294, 374, 328, ACCENT, true);
    xapi_DrawText(g_handle, 258, 302, login_tr("创建账户", "Create Account"), 13, WHITE);
    xapi_DrawText(g_handle, 64, 350,
                  g_message[0] != '\0' ? g_message :
                                          login_tr("按制表键切换输入框，按回车键创建账户。",
                                                   "Press Tab to move between fields; Enter creates the account."),
                  12,
                  g_message[0] != '\0' ? g_message_color : WHITE);
    xapi_RefreshWindow(g_handle);
}

static void draw_login()
{
    g_frame_drawn = true;
    draw_background();
    const char *name = g_user_count > 0 ? g_users[g_selected_user].name : "";
    xapi_DrawText(g_handle, 64, 44, login_tr("登录", "Sign in"), 40, WHITE);
    xapi_DrawText(g_handle, 64, 130, login_tr("用户：", "User:"), 16, WHITE);
    xapi_DrawText(g_handle, 128, 130, (char *)name, 16, WHITE);
    xapi_DrawText(g_handle, 64, 164, login_tr("密码：", "Password:"), 16, WHITE);
    capture_login_dynamic_regions();

    draw_input_box(128, 168, 300, g_password, true, true);
    xapi_DrawPicture(g_handle, 436, 168, 24, 24, (char *)"/system/icon/nextstep.png");
    if (g_message[0] != '\0') xapi_DrawText(g_handle, 64, 210, g_message, 12, RED);
    xapi_RefreshWindow(g_handle);
}

static void redraw_oobe_dynamic()
{
    if (!g_frame_drawn)
    {
        draw_oobe();
        return;
    }

    for (int i = 0; i < 3; i++) restore_region(&g_input_cache[i]);
    restore_region(&g_message_cache);

    draw_input_box(224, 136, 320, g_username, false, g_focus == 0);
    draw_input_box(224, 186, 320, g_password, true, g_focus == 1);
    draw_input_box(224, 236, 320, g_confirm, true, g_focus == 2);
    xapi_DrawText(g_handle, 64, 350,
                  g_message[0] != '\0' ? g_message :
                                          login_tr("按制表键切换输入框，按回车键创建账户。",
                                                   "Press Tab to move between fields; Enter creates the account."),
                  12,
                  g_message[0] != '\0' ? g_message_color : WHITE);

    xapi_RefreshPartWindow(g_handle, 224, 136, 545, 165);
    xapi_RefreshPartWindow(g_handle, 224, 186, 545, 215);
    xapi_RefreshPartWindow(g_handle, 224, 236, 545, 265);
    xapi_RefreshPartWindow(g_handle, 64, 350, 684, 378);
}

static void redraw_login_dynamic()
{
    if (!g_frame_drawn)
    {
        draw_login();
        return;
    }

    restore_region(&g_login_input_cache);
    restore_region(&g_login_message_cache);
    draw_input_box(128, 168, 300, g_password, true, true);
    if (g_message[0] != '\0') xapi_DrawText(g_handle, 64, 210, g_message, 12, g_message_color);

    xapi_RefreshPartWindow(g_handle, 128, 168, 429, 197);
    xapi_RefreshPartWindow(g_handle, 64, 210, 584, 238);
}

static const char *error_message(INT64 ret, bool create)
{
    if (ret == -13) return login_tr("密码错误，请重试。", "Incorrect password. Try again.");
    if (ret == -17) return login_tr("系统中已经存在账户。", "An account already exists on this system.");
    if (ret == -22)
    {
        return create ? login_tr("用户名或密码不符合要求。", "The username or password is invalid.") :
                        login_tr("请输入用户名和密码。", "Enter a username and password.");
    }
    if (ret == -2) return login_tr("没有找到这个用户。", "This user was not found.");
    if (ret == -5) return login_tr("写入账户信息失败，请检查磁盘。", "Failed to write account data. Check the disk.");
    return create ? login_tr("创建账户失败。", "Failed to create the account.") :
                    login_tr("登录失败。", "Sign-in failed.");
}

static bool submit_oobe()
{
    if (g_done || g_oobe_submitting) return true;

    if (!username_ok(g_username))
    {
        copy_cstr(g_message, sizeof(g_message),
                  login_tr("用户名只能使用英文、数字、空格、下划线和短横线。",
                           "Usernames can only use letters, numbers, spaces, underscores, and hyphens."));
        return false;
    }
    if (g_password[0] == '\0')
    {
        copy_cstr(g_message, sizeof(g_message), login_tr("请输入密码。", "Enter a password."));
        return false;
    }
    if (strcmp(g_password, g_confirm) != 0)
    {
        copy_cstr(g_message, sizeof(g_message), login_tr("两次输入的密码不一致。", "The passwords do not match."));
        return false;
    }

    g_oobe_submitting = true;

    g_message_color = GREEN;
    copy_cstr(g_message, sizeof(g_message), login_tr("正在创建账户...", "Creating account..."));
    redraw_oobe_dynamic();

    INT64 ret = (INT64)xapi_UserCreateFirst(g_username, g_password);
    if (ret == 0)
    {
        g_done = true;
        return true;
    }
    g_oobe_submitting = false;
    g_message_color = RED;
    copy_cstr(g_message, sizeof(g_message), error_message(ret, true));
    return false;
}

static bool submit_login()
{
    if (g_user_count == 0)
    {
        copy_cstr(g_message, sizeof(g_message), login_tr("没有可登录的用户。", "No users are available to sign in."));
        return false;
    }
    INT64 ret = (INT64)xapi_UserLogin(g_users[g_selected_user].name, g_password);
    if (ret == 0) return true;
    copy_cstr(g_message, sizeof(g_message), error_message(ret, false));
    return false;
}

static void handle_oobe_key(UINT64 ch, bool *done)
{
    if (ch == XKEY_TAB)
    {
        g_focus = (g_focus + 1) % 3;
        g_message[0] = '\0';
    }
    else if (ch == '\n' || ch == XKEY_ENTER)
    {
        *done = submit_oobe();
    }
    else if (ch == '\b' || ch == XKEY_BACKSPACE)
    {
        if (g_focus == 0) backspace_char(g_username);
        else if (g_focus == 1) backspace_char(g_password);
        else backspace_char(g_confirm);
        g_message[0] = '\0';
    }
    else if (ch >= 32 && ch < 127)
    {
        if (g_focus == 0)
        {
            if (username_char_ok((char)ch)) append_char(g_username, sizeof(g_username), (char)ch);
            else
            {
                copy_cstr(g_message, sizeof(g_message),
                          login_tr("用户名只能使用英文、数字、空格、下划线和短横线。",
                                   "Usernames can only use letters, numbers, spaces, underscores, and hyphens."));
            }
        }
        else if (g_focus == 1) append_char(g_password, sizeof(g_password), (char)ch);
        else append_char(g_confirm, sizeof(g_confirm), (char)ch);
        if (g_message[0] != '\0' && g_focus != 0) g_message[0] = '\0';
    }
    redraw_oobe_dynamic();
}

static void handle_login_key(UINT64 ch, bool *done)
{
    if (ch == '\n' || ch == XKEY_ENTER)
    {
        *done = submit_login();
    }
    else if (ch == '\b' || ch == XKEY_BACKSPACE)
    {
        backspace_char(g_password);
        g_message[0] = '\0';
    }
    else if (ch >= 32 && ch < 127)
    {
        append_char(g_password, sizeof(g_password), (char)ch);
        g_message[0] = '\0';
    }
    redraw_login_dynamic();
}

static void login_MessagePrcor(UINT64 Type, UINT64 hData, UINT64 lData)
{
    if (Type == MSG_CHAR || Type == MSG_SPCHAR)
    {
        if (g_oobe) handle_oobe_key(lData, &g_done);
        else handle_login_key(lData, &g_done);
        return;
    }

    if (Type == MSG_LBUTTON)
    {
        int x = (int)hData;
        int y = (int)lData;
        if (g_oobe)
        {
            if (x >= 224 && x <= 544 && y >= 136 && y <= 164)
            {
                g_focus = 0;
                redraw_oobe_dynamic();
                return;
            }
            else if (x >= 224 && x <= 544 && y >= 186 && y <= 214)
            {
                g_focus = 1;
                redraw_oobe_dynamic();
                return;
            }
            else if (x >= 224 && x <= 544 && y >= 236 && y <= 264)
            {
                g_focus = 2;
                redraw_oobe_dynamic();
                return;
            }
            else if (x >= 224 && x <= 374 && y >= 294 && y <= 328) g_done = submit_oobe();
            if (!g_done) redraw_oobe_dynamic();
            return;
        }

        for (UINT64 i = 0; i < g_user_count; i++)
        {
            int row_y = 130 + (int)i * 50;
            if (x > 64 && y > row_y && y < row_y + 50)
            {
                g_selected_user = i;
                memset(g_password, 0, sizeof(g_password));
                g_message[0] = '\0';
                draw_login();
                return;
            }
        }
        if (x > 436 && y > 168 && x < 460 && y < 192)
        {
            g_done = submit_login();
            if (!g_done) draw_login();
        }
    }
}

static int login_main_impl(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    g_language = xj380_read_language();

    XWINDOW window;
    memset(&window, 0, sizeof(window));
    window.title = login_tr("登录", "Sign in");
    window.sets = XWIN_LOGIN;
    xapi_CreateWindow(&g_handle, &window);
    if (g_handle == 0) return 1;

    xapi_GetWindowSize(g_handle, &g_width, &g_height);
    SetMsgPrcor(g_handle, login_MessagePrcor);

    g_oobe = xapi_UserOobeRequired() != 0;
    if (g_oobe)
    {
        draw_oobe();
    }
    else
    {
        g_user_count = xapi_UserList(g_users, 128);
        if ((INT64)g_user_count < 0) g_user_count = 0;
        draw_login();
    }

    while (!g_done)
    {
        xapi_Sleep(30);
    }

    xapi_CloseWindow(g_handle);
    return 0;
}

extern "C" int login_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int login_main_cpp(int argc, char *argv[], char *envp[])
{
    return login_main_impl(argc, argv, envp);
}
