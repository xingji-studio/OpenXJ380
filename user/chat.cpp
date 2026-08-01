#include "./xapi/include/x3api.h"
#include <xj380_i18n.h>

static const uint64_t CHAT_POLL_INTERVAL_MS = 500;
static const uint64_t CHAT_LOOP_SLEEP_MS    = 50;
static const int      CHAT_WIDTH            = 720;
static const int      CHAT_HEIGHT           = 480;
static const int      CHAT_MAX_LINES        = 256;
static const int      CHAT_LINE_LENGTH      = 96;
static const int      CHAT_HEADER_TEXT_MAX  = 52;
static const int      CHAT_INPUT_TEXT_MAX   = 68;
static const int      CHAT_SCROLL_STEP      = 3;
static const int      CHAT_LINES_TOP        = 60;
static const int      CHAT_LINE_HEIGHT      = 16;

typedef struct chat_line {
    char     text[CHAT_LINE_LENGTH];
    uint32_t color;
} chat_line_t;

static HDLE        g_window              = 0;
static int         g_listen_fd           = -1;
static int         g_sockfd              = -1;
static uint64_t    g_poll_elapsed_ms     = 0;
static bool        g_need_redraw         = true;
static bool        g_send_requested      = false;
static int         g_input_len           = 0;
static char        g_input[512]          = {0};
static char        g_status[128]         = "";
static chat_line_t g_lines[CHAT_MAX_LINES];
static int         g_line_count          = 0;
static int         g_scroll_offset       = 0;
static UINT64      g_width               = CHAT_WIDTH;
static UINT64      g_height              = CHAT_HEIGHT;
static int         g_language            = XJ380_LANGUAGE_ZH_CN;

static char *chat_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(g_language, zh_cn, en_us);
}

static int chat_visible_lines()
{
    int visible = ((int)g_height - CHAT_LINES_TOP - 88) / CHAT_LINE_HEIGHT;
    return visible < 1 ? 1 : visible;
}

static void chat_format_head_text(const char *text, char *buffer, int buffer_size, int max_chars)
{
    if (buffer == NULL || buffer_size <= 0) {
        return;
    }
    buffer[0] = '\0';
    if (text == NULL) {
        return;
    }

    int len = (int)strlen(text);
    if (len <= max_chars || max_chars < 4) {
        strncpy(buffer, text, (size_t)buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        return;
    }

    int copy_len = MIN(max_chars - 3, buffer_size - 1);
    memcpy(buffer, text, (uint64_t)copy_len);
    memcpy(buffer + copy_len, "...", 3);
    buffer[copy_len + 3] = '\0';
}

static void chat_format_tail_text(const char *text, char *buffer, int buffer_size, int max_chars)
{
    if (buffer == NULL || buffer_size <= 0) {
        return;
    }
    buffer[0] = '\0';
    if (text == NULL) {
        return;
    }

    int len = (int)strlen(text);
    if (len <= max_chars || max_chars < 4) {
        strncpy(buffer, text, (size_t)buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        return;
    }

    int copy_len = MIN(max_chars - 3, buffer_size - 1);
    memcpy(buffer, "...", 3);
    memcpy(buffer + 3, text + len - copy_len, (uint64_t)copy_len);
    buffer[copy_len + 3] = '\0';
}

static void chat_clamp_scroll(void)
{
    int max_scroll = MAX(0, g_line_count - chat_visible_lines());
    if (g_scroll_offset < 0) {
        g_scroll_offset = 0;
    } else if (g_scroll_offset > max_scroll) {
        g_scroll_offset = max_scroll;
    }
}

static void set_status(const char *text)
{
    strncpy(g_status, text, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    g_need_redraw                  = true;
}

static void set_status_tr(const char *zh_cn, const char *en_us)
{
    set_status(chat_tr(zh_cn, en_us));
}

static void append_line(const char *text, uint32_t color)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    if (g_line_count == CHAT_MAX_LINES) {
        for (int i = 1; i < CHAT_MAX_LINES; ++i) {
            g_lines[i - 1] = g_lines[i];
        }
        g_line_count--;
    }

    strncpy(g_lines[g_line_count].text, text, CHAT_LINE_LENGTH - 1);
    g_lines[g_line_count].text[CHAT_LINE_LENGTH - 1] = '\0';
    g_lines[g_line_count].color                      = color;
    g_line_count++;
    chat_clamp_scroll();
    g_need_redraw = true;
}

static void append_wrapped_text(const char *prefix, const char *data, int len, uint32_t color)
{
    char line[CHAT_LINE_LENGTH];
    int  prefix_len = prefix != NULL ? (int)strlen(prefix) : 0;
    int  data_pos   = 0;
    bool first_line = true;

    if (len < 0) {
        len = 0;
    }
    if (prefix_len >= CHAT_LINE_LENGTH - 2) {
        prefix_len = CHAT_LINE_LENGTH - 2;
    }

    while (data_pos < len || first_line) {
        int  line_pos      = 0;
        int  text_capacity = CHAT_LINE_LENGTH - prefix_len - 1;
        char prefix_buf[CHAT_LINE_LENGTH];
        memset(prefix_buf, ' ', sizeof(prefix_buf));
        prefix_buf[prefix_len] = '\0';
        if (first_line && prefix != NULL) {
            memcpy(prefix_buf, prefix, (uint64_t)prefix_len);
        }

        if (prefix_len > 0) {
            memcpy(line, prefix_buf, (uint64_t)prefix_len);
            line_pos = prefix_len;
        }

        while (data_pos < len && (data[data_pos] == '\n' || data[data_pos] == '\r')) {
            data_pos++;
        }

        int chunk_len = 0;
        while (data_pos + chunk_len < len &&
               data[data_pos + chunk_len] != '\n' &&
               data[data_pos + chunk_len] != '\r' &&
               chunk_len < text_capacity) {
            line[line_pos + chunk_len] = data[data_pos + chunk_len];
            chunk_len++;
        }

        if (chunk_len == 0 && data_pos >= len && !first_line) {
            break;
        }

        line[line_pos + chunk_len] = '\0';
        append_line(line, color);

        data_pos += chunk_len;
        while (data_pos < len && (data[data_pos] == '\n' || data[data_pos] == '\r')) {
            data_pos++;
            if (data_pos < len) {
                break;
            }
        }
        first_line = false;
    }
}

static int socket_send_all(int sockfd, const char *data, int len)
{
    int sent_total = 0;
    while (sent_total < len) {
        int sent = write(sockfd, (char *)data + sent_total, (uint64_t)(len - sent_total));
        if (sent <= 0) {
            return -1;
        }
        sent_total += sent;
    }
    return 0;
}

static void render_chat_window()
{
    char header_status[CHAT_LINE_LENGTH];
    char input_view[CHAT_LINE_LENGTH];
    xapi_GetWindowSize(g_window, &g_width, &g_height);
    chat_format_head_text(g_status, header_status, sizeof(header_status), CHAT_HEADER_TEXT_MAX);
    chat_format_tail_text(g_input, input_view, sizeof(input_view), CHAT_INPUT_TEXT_MAX);

    xapi_DrawRect(g_window, 0, 0, (UINT32)g_width - 1, (UINT32)g_height - 1, 0xf4efe6ff, true);
    xapi_DrawRect(g_window, 0, 0, (UINT32)g_width - 1, 39, 0x264653ff, true);
    xapi_DrawRect(g_window, 0, (UINT32)g_height - 72, (UINT32)g_width - 1, (UINT32)g_height - 1, 0xe9dcc9ff, true);
    xapi_DrawRect(g_window, 12, 52, (UINT32)g_width - 13, (UINT32)g_height - 84, 0xfffbf5ff, true);
    xapi_DrawRect(g_window, 12, (UINT32)g_height - 60, (UINT32)g_width - 13, (UINT32)g_height - 16, 0xffffffff, true);
    xapi_DrawRect(g_window, 12, 52, (UINT32)g_width - 13, (UINT32)g_height - 84, 0xded6caff, false);
    xapi_DrawRect(g_window, 12, (UINT32)g_height - 60, (UINT32)g_width - 13, (UINT32)g_height - 16, 0xd0c4b4ff, false);

    xapi_DrawSWText(g_window, 16, 12, chat_tr("XJ380 聊天", "XJ380 Chat"), 0xffffffff);
    xapi_DrawSWText(g_window, 150, 12, header_status, 0xd8f3dcff);
    xapi_DrawSWText(g_window, (UINT32)g_width - 184, 12, chat_tr("滚轮：滚动历史", "Wheel: history"), 0xcfe8e6ff);
    xapi_DrawSWText(g_window, 16, (UINT32)g_height - 54, chat_tr("输入:", "Input:"), 0x3d405bff);
    xapi_DrawSWText(g_window, 72, (UINT32)g_height - 54, input_view, 0x111111ff);

    chat_clamp_scroll();
    int visible_lines = chat_visible_lines();
    int max_start = MAX(0, g_line_count - visible_lines);
    int start     = MAX(0, max_start - g_scroll_offset);
    int end       = MIN(g_line_count, start + visible_lines);
    for (int i = start; i < end; ++i) {
        xapi_DrawSWText(g_window, 20, CHAT_LINES_TOP + (i - start) * CHAT_LINE_HEIGHT, g_lines[i].text, g_lines[i].color);
    }

    xapi_RefreshWindow(g_window);
    g_need_redraw = false;
}

static bool setup_listener()
{
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        append_line(chat_tr("聊天：socket() 失败", "Chat: socket() failed"), 0xb00020ff);
        set_status_tr("套接字失败", "Socket failed");
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(2323);
    addr.sin_addr.s_addr = 0;

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        append_line(chat_tr("聊天：绑定 0.0.0.0:2323 失败", "Chat: bind 0.0.0.0:2323 failed"), 0xb00020ff);
        set_status_tr("绑定失败", "Bind failed");
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }

    if (listen(g_listen_fd, 1) < 0) {
        append_line(chat_tr("聊天：listen() 失败", "Chat: listen() failed"), 0xb00020ff);
        set_status_tr("监听失败", "Listen failed");
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }

    append_line(chat_tr("正在监听 0.0.0.0:2323", "Listening on 0.0.0.0:2323"), 0x1d3557ff);
    append_line(chat_tr("宿主机可连接 127.0.0.1:2323", "Host can connect to 127.0.0.1:2323"), 0x1d3557ff);
    set_status_tr("等待宿主机连接", "Waiting for host");
    return true;
}

static void poll_listener_once()
{
    if (g_listen_fd < 0 || g_sockfd >= 0) {
        return;
    }

    struct pollfd fd;
    fd.fd      = g_listen_fd;
    fd.events  = POLLIN;
    fd.revents = 0;

    int ready = poll(&fd, 1, 0);
    if (ready <= 0) {
        return;
    }
    if (!(fd.revents & POLLIN)) {
        return;
    }

    struct sockaddr_in peer_addr;
    socklen_t          peer_len = sizeof(peer_addr);
    g_sockfd                    = accept(g_listen_fd, (struct sockaddr *)&peer_addr, &peer_len);
    if (g_sockfd < 0) {
        append_line(chat_tr("聊天：accept() 失败", "Chat: accept() failed"), 0xb00020ff);
        set_status_tr("接受连接失败", "Accept failed");
        return;
    }

    close(g_listen_fd);
    g_listen_fd = -1;
    append_line(chat_tr("宿主机已连接。", "Host connected."), 0x2a9d8fff);
    set_status_tr("已连接", "Connected");
}

static void poll_socket_once()
{
    if (g_sockfd < 0) {
        return;
    }

    struct pollfd fd;
    fd.fd      = g_sockfd;
    fd.events  = POLLIN;
    fd.revents = 0;

    int ready = poll(&fd, 1, 0);
    if (ready <= 0) {
        return;
    }

    if (fd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
        append_line(chat_tr("对端已断开连接。", "Peer disconnected."), 0xb00020ff);
        set_status_tr("已断开", "Disconnected");
        close(g_sockfd);
        g_sockfd = -1;
        return;
    }

    if (!(fd.revents & POLLIN)) {
        return;
    }

    char buffer[512];
    int  got = read(g_sockfd, buffer, sizeof(buffer));
    if (got <= 0) {
        append_line(chat_tr("对端已断开连接。", "Peer disconnected."), 0xb00020ff);
        set_status_tr("已断开", "Disconnected");
        close(g_sockfd);
        g_sockfd = -1;
        return;
    }

    append_wrapped_text(chat_tr("[对方] ", "[Peer] "), buffer, got, 0x264653ff);
}

static void send_input_line()
{
    if (!g_send_requested) {
        return;
    }

    g_send_requested = false;
    if (g_input_len == 0) {
        g_need_redraw = true;
        return;
    }

    if (g_sockfd < 0) {
        append_line(chat_tr("还没有宿主机连接。", "No host connection yet."), 0xb00020ff);
        g_input[0]   = '\0';
        g_input_len  = 0;
        g_need_redraw = true;
        return;
    }

    char output[sizeof(g_input) + 2];
    memcpy(output, g_input, (uint64_t)g_input_len);
    int len = g_input_len;
    output[len++] = '\n';
    output[len]   = '\0';

    if (socket_send_all(g_sockfd, output, len) < 0) {
        append_line(chat_tr("聊天：发送失败", "Chat: send failed"), 0xb00020ff);
        set_status_tr("发送失败", "Send failed");
        close(g_sockfd);
        g_sockfd = -1;
    } else {
        append_wrapped_text(chat_tr("[我] ", "[Me] "), g_input, g_input_len, 0xe76f51ff);
    }

    g_input[0]    = '\0';
    g_input_len   = 0;
    g_need_redraw = true;
}

static void chat_message_proc(UINT64 type, UINT64 hData, UINT64 lData)
{
    (void)hData;

    switch (type) {
    case MSG_CHAR:
        if ((char)lData >= ' ' && g_input_len < (int)sizeof(g_input) - 1) {
            g_input[g_input_len++] = (char)lData;
            g_input[g_input_len]   = '\0';
            g_need_redraw          = true;
        }
        break;
    case MSG_SPCHAR:
        if ((char)lData == '\b') {
            if (g_input_len > 0) {
                g_input[--g_input_len] = '\0';
                g_need_redraw          = true;
            }
        } else if ((char)lData == '\n') {
            g_send_requested = true;
        }
        break;
    case MSG_ROLLER:
        g_scroll_offset += (int)hData * CHAT_SCROLL_STEP;
        chat_clamp_scroll();
        g_need_redraw = true;
        break;
    case MSG_RESIZE:
        g_width = hData;
        g_height = lData;
        chat_clamp_scroll();
        g_need_redraw = true;
        break;
    default:
        break;
    }
}

static int chat_main_impl(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    g_language = xj380_read_language();
    set_status_tr("正在启动聊天...", "Starting chat...");

    XWINDOW window;
    window.title  = chat_tr("聊天", "Chat");
    window.width  = CHAT_WIDTH;
    window.height = CHAT_HEIGHT;
    window.sets   = XWIN_NORMAL | XWIN_SUPPORT_RESIZEABLE;

    xapi_CreateWindow(&g_window, &window);
    xapi_SetIcon(g_window, "/system/icon/terminal.png");
    SetMsgPrcor(g_window, chat_message_proc);

    append_line(chat_tr("输入消息后按 Enter 发送。", "Type a message and press Enter to send."), 0x3d405bff);
    append_line(chat_tr("每 0.5 秒轮询一次套接字。", "Polling the socket every 0.5 seconds."), 0x3d405bff);
    setup_listener();
    render_chat_window();

    while (true) {
        send_input_line();

        g_poll_elapsed_ms += CHAT_LOOP_SLEEP_MS;
        if (g_poll_elapsed_ms >= CHAT_POLL_INTERVAL_MS) {
            g_poll_elapsed_ms = 0;
            poll_listener_once();
            poll_socket_once();
        }

        if (g_need_redraw) {
            render_chat_window();
        }

        xapi_Sleep(CHAT_LOOP_SLEEP_MS);
    }

    return 0;
}

extern "C" int chat_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int chat_main_cpp(int argc, char *argv[], char *envp[])
{
    return chat_main_impl(argc, argv, envp);
}
