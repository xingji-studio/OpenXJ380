#include <libsys.h>
#include <x3api.h>
#include <xj380_i18n.h>

static const int      NETCFG_WIDTH             = 760;
static const int      NETCFG_HEIGHT            = 540;
static const int      NETCFG_LOOP_SLEEP_MS     = 16;
static const int      NETCFG_STARTUP_GUARD     = 32;
static const int      NETCFG_MAX_STATUS_LINES  = 96;
static const int      NETCFG_MAX_LOG_LINES     = 48;
static const int      NETCFG_LINE_LENGTH       = 96;
static const int      NETCFG_HEADER_TEXT_MAX   = 52;
static const int      NETCFG_INPUT_TEXT_MAX    = 72;
static const int      NETCFG_SCROLL_STEP       = 3;
static const int      NETCFG_STATUS_TOP        = 138;
static const int      NETCFG_LINE_HEIGHT       = 16;
static const int      NETCFG_STATUS_PANEL_TOP  = 132;
static const uint32_t NETCFG_COLOR_BG          = 0xf4f1ebff;
static const uint32_t NETCFG_COLOR_PANEL       = 0xfffcf6ff;
static const uint32_t NETCFG_COLOR_HEADER      = 0x264653ff;
static const uint32_t NETCFG_COLOR_TEXT        = 0x1f2933ff;
static const uint32_t NETCFG_COLOR_MUTED       = 0x51606aff;
static const uint32_t NETCFG_COLOR_STATUS      = 0xd8f3dcff;
static const uint32_t NETCFG_COLOR_ERROR       = 0xb00020ff;
static const uint32_t NETCFG_COLOR_LOG         = 0x2a9d8fff;

typedef struct netcfg_line {
    char     text[NETCFG_LINE_LENGTH];
    uint32_t color;
} netcfg_line_t;

static HDLE          g_window               = 0;
static bool          g_need_redraw          = true;
static int           g_startup_submit_guard = NETCFG_STARTUP_GUARD;
static char          g_input[256]           = "auto";
static int           g_input_len            = 4;
static char          g_status[128]          = "就绪";
static netcfg_line_t g_status_lines[NETCFG_MAX_STATUS_LINES];
static int           g_status_line_count    = 0;
static netcfg_line_t g_log_lines[NETCFG_MAX_LOG_LINES];
static int           g_log_line_count       = 0;
static int           g_status_scroll_top    = 0;
static int           g_log_scroll_top       = 0;
static int           g_last_mouse_y         = 0;
static UINT64        g_width                = NETCFG_WIDTH;
static UINT64        g_height               = NETCFG_HEIGHT;
static int           g_language             = XJ380_LANGUAGE_ZH_CN;

static char *netcfg_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(g_language, zh_cn, en_us);
}

static int netcfg_log_panel_top(void)
{
    int top = (int)g_height - 152;
    return top < NETCFG_STATUS_PANEL_TOP + 96 ? NETCFG_STATUS_PANEL_TOP + 96 : top;
}

static int netcfg_status_panel_bottom(void)
{
    return netcfg_log_panel_top() - 40;
}

static int netcfg_log_panel_bottom(void)
{
    return (int)g_height - 72;
}

static int netcfg_status_visible(void)
{
    int visible = (netcfg_status_panel_bottom() - NETCFG_STATUS_TOP - 8) / NETCFG_LINE_HEIGHT;
    return visible < 1 ? 1 : visible;
}

static int netcfg_log_top(void)
{
    return netcfg_log_panel_top() + 6;
}

static int netcfg_log_visible(void)
{
    int visible = (netcfg_log_panel_bottom() - netcfg_log_top() - 4) / NETCFG_LINE_HEIGHT;
    return visible < 1 ? 1 : visible;
}

static void netcfg_log_serial(const char *text)
{
    if (text == NULL) {
        return;
    }
    xapi_OutputSerial((char *)text);
}

static const char *netcfg_resolve_dns_alias(const char *value)
{
    if (value == NULL) {
        return NULL;
    }
    if (strcmp(value, "ali") == 0 || strcmp(value, "alidns") == 0 || strcmp(value, "aliyun") == 0) {
        return "223.5.5.5";
    }
    if (strcmp(value, "ali2") == 0 || strcmp(value, "alidns2") == 0 || strcmp(value, "aliyun2") == 0) {
        return "223.6.6.6";
    }
    if (strcmp(value, "auto") == 0 || strcmp(value, "dhcp") == 0) {
        return value;
    }
    return value;
}

static void netcfg_format_head_text(const char *text, char *buffer, int buffer_size, int max_chars)
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

static void netcfg_format_tail_text(const char *text, char *buffer, int buffer_size, int max_chars)
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

static void netcfg_clamp_status_scroll(void)
{
    int max_scroll = MAX(0, g_status_line_count - netcfg_status_visible());
    g_status_scroll_top = MAX(0, MIN(g_status_scroll_top, max_scroll));
}

static void netcfg_clamp_log_scroll(void)
{
    int max_scroll = MAX(0, g_log_line_count - netcfg_log_visible());
    g_log_scroll_top = MAX(0, MIN(g_log_scroll_top, max_scroll));
}

static void netcfg_append_visual_line(netcfg_line_t *lines, int *count, int max_count, const char *text, uint32_t color)
{
    if (lines == NULL || count == NULL || text == NULL || text[0] == '\0' || max_count <= 0) {
        return;
    }

    if (*count == max_count) {
        for (int i = 1; i < max_count; ++i) {
            lines[i - 1] = lines[i];
        }
        (*count)--;
    }

    strncpy(lines[*count].text, text, NETCFG_LINE_LENGTH - 1);
    lines[*count].text[NETCFG_LINE_LENGTH - 1] = '\0';
    lines[*count].color                        = color;
    (*count)++;
}

static void netcfg_append_wrapped_line(netcfg_line_t *lines, int *count, int max_count, const char *text, uint32_t color)
{
    if (lines == NULL || count == NULL || text == NULL) {
        return;
    }

    int len = (int)strlen(text);
    int pos = 0;
    while (pos < len || (len == 0 && pos == 0)) {
        char line[NETCFG_LINE_LENGTH];
        int  chunk_len = 0;

        while (pos < len && (text[pos] == '\n' || text[pos] == '\r')) {
            pos++;
        }
        while (pos + chunk_len < len &&
               text[pos + chunk_len] != '\n' &&
               text[pos + chunk_len] != '\r' &&
               chunk_len < NETCFG_LINE_LENGTH - 1) {
            line[chunk_len] = text[pos + chunk_len];
            chunk_len++;
        }

        if (chunk_len == 0 && pos >= len) {
            break;
        }

        line[chunk_len] = '\0';
        netcfg_append_visual_line(lines, count, max_count, line, color);
        pos += chunk_len;
    }
}

static void netcfg_set_status(const char *text)
{
    strncpy(g_status, text, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    g_need_redraw                  = true;
}

static void netcfg_set_status_tr(const char *zh_cn, const char *en_us)
{
    netcfg_set_status(netcfg_tr(zh_cn, en_us));
}

static void netcfg_append_log(const char *text, uint32_t color)
{
    if (text == NULL) {
        return;
    }

    int old_count = g_log_line_count;
    bool keep_bottom = (g_log_scroll_top + netcfg_log_visible()) >= old_count;

    netcfg_append_wrapped_line(g_log_lines, &g_log_line_count, NETCFG_MAX_LOG_LINES, text, color);
    if (keep_bottom) {
        g_log_scroll_top = MAX(0, g_log_line_count - netcfg_log_visible());
    }
    netcfg_clamp_log_scroll();
    g_need_redraw = true;
}

static int netcfg_open_file(const char *path)
{
    if (path == NULL) {
        return -1;
    }
    return (int)enter_syscall(SYS_OPEN, (uint64_t)path, 0, 0, 0, 0, 0);
}

static int netcfg_read_text_file(const char *path, char *buffer, size_t size)
{
    if (path == NULL || buffer == NULL || size == 0) {
        return -1;
    }

    int fd = netcfg_open_file(path);
    if (fd < 0) {
        return -1;
    }

    int got = read(fd, buffer, size - 1);
    close(fd);
    if (got < 0) {
        return -1;
    }

    buffer[got] = '\0';
    return got;
}

static int netcfg_write_text_file(const char *path, const char *text)
{
    if (path == NULL || text == NULL) {
        return -1;
    }

    int fd = netcfg_open_file(path);
    if (fd < 0) {
        return -1;
    }

    int expected = (int)strlen(text);
    int wrote    = write(fd, (char *)text, (uint64_t)expected);
    close(fd);
    return wrote == expected ? 0 : -1;
}

static bool netcfg_text_contains(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    return strstr(text, needle) != NULL;
}

static int netcfg_apply_dns_value(const char *value)
{
    static char dns_path[] = "/run/dns/server";

    if (value == NULL || value[0] == '\0') {
        return -1;
    }

    const char *resolved = netcfg_resolve_dns_alias(value);
    char        dns_text[64];
    snprintf(dns_text, sizeof(dns_text), "%s\n", resolved);

    {
        char logbuf[96];
        snprintf(logbuf, sizeof(logbuf), "netcfg: apply dns value=%s resolved=%s\n", value, resolved);
        netcfg_log_serial(logbuf);
    }

    if (netcfg_write_text_file(dns_path, dns_text) < 0) {
        netcfg_log_serial((char *)"netcfg: dns write failed\n");
        return -1;
    }

    char verify[128];
    memset(verify, 0, sizeof(verify));
    if (netcfg_read_text_file(dns_path, verify, sizeof(verify)) <= 0) {
        netcfg_log_serial((char *)"netcfg: dns verify read failed\n");
        return -1;
    }

    if ((strcmp(resolved, "auto") == 0 || strcmp(resolved, "dhcp") == 0)) {
        return 0;
    }
    if (!netcfg_text_contains(verify, resolved)) {
        netcfg_log_serial((char *)"netcfg: dns verify mismatch\n");
        return -1;
    }
    return 0;
}

static void netcfg_refresh_status(void)
{
    static char status_path[] = "/run/NetworkManager/status";
    char        buffer[1024];
    memset(buffer, 0, sizeof(buffer));

    g_status_line_count = 0;
    int got = netcfg_read_text_file(status_path, buffer, sizeof(buffer));
    if (got <= 0) {
        netcfg_append_visual_line(g_status_lines, &g_status_line_count, NETCFG_MAX_STATUS_LINES,
                                  netcfg_tr("读取 /run/NetworkManager/status 失败",
                                            "Failed to read /run/NetworkManager/status"),
                                  NETCFG_COLOR_ERROR);
        g_status_scroll_top = 0;
        netcfg_set_status_tr("读取状态失败", "Status read failed");
        return;
    }

    char *cursor = buffer;
    while (*cursor != '\0' && g_status_line_count < NETCFG_MAX_STATUS_LINES) {
        char line[NETCFG_LINE_LENGTH * 2];
        size_t len = 0;
        while (cursor[len] != '\0' && cursor[len] != '\n' && len + 1 < sizeof(line)) {
            line[len] = cursor[len];
            len++;
        }
        line[len] = '\0';

        uint32_t color = line[0] == '[' ? 0x457b9dff : NETCFG_COLOR_TEXT;
        netcfg_append_wrapped_line(g_status_lines, &g_status_line_count, NETCFG_MAX_STATUS_LINES, line, color);

        if (cursor[len] == '\n') {
            cursor += len + 1;
        } else {
            cursor += len;
        }
    }

    g_status_scroll_top = 0;
    netcfg_clamp_status_scroll();
    netcfg_set_status_tr("状态已刷新", "Status refreshed");
}

static void netcfg_render_window(void)
{
    g_language = xj380_read_language();
    char header_status[NETCFG_LINE_LENGTH];
    char input_view[NETCFG_LINE_LENGTH];
    xapi_GetWindowSize(g_window, &g_width, &g_height);
    netcfg_format_head_text(g_status, header_status, sizeof(header_status), NETCFG_HEADER_TEXT_MAX);
    netcfg_format_tail_text(g_input, input_view, sizeof(input_view), NETCFG_INPUT_TEXT_MAX);

    int status_bottom = netcfg_status_panel_bottom();
    int log_top = netcfg_log_panel_top();
    int log_bottom = netcfg_log_panel_bottom();

    xapi_DrawRect(g_window, 0, 0, (UINT32)g_width - 1, (UINT32)g_height - 1, NETCFG_COLOR_BG, true);
    xapi_DrawRect(g_window, 0, 0, (UINT32)g_width - 1, 39, NETCFG_COLOR_HEADER, true);
    xapi_DrawRect(g_window, 12, 52, (UINT32)g_width - 13, 104, 0xf2ebe2ff, true);
    xapi_DrawRect(g_window, 12, NETCFG_STATUS_PANEL_TOP, (UINT32)g_width - 13, status_bottom, NETCFG_COLOR_PANEL, true);
    xapi_DrawRect(g_window, 12, log_top, (UINT32)g_width - 13, log_bottom, 0xf8f9faff, true);
    xapi_DrawRect(g_window, 12, (UINT32)g_height - 64, (UINT32)g_width - 13, (UINT32)g_height - 16, 0xffffffff, true);
    xapi_DrawRect(g_window, 12, NETCFG_STATUS_PANEL_TOP, (UINT32)g_width - 13, status_bottom, 0xd7cec2ff, false);
    xapi_DrawRect(g_window, 12, log_top, (UINT32)g_width - 13, log_bottom, 0xd3d8deff, false);
    xapi_DrawRect(g_window, 12, (UINT32)g_height - 64, (UINT32)g_width - 13, (UINT32)g_height - 16, 0xd9d9d9ff, false);

    xapi_DrawSWText(g_window, 16, 12, netcfg_tr("网络配置", "Network Config"), 0xffffffff);
    xapi_DrawSWText(g_window, 164, 12, header_status, NETCFG_COLOR_STATUS);
    xapi_DrawSWText(g_window, (UINT32)g_width - 204, 12, netcfg_tr("滚轮：滚动面板", "Wheel: scroll panels"), 0xcfe8e6ff);

    xapi_DrawSWText(g_window, 18, 56, netcfg_tr("命令:", "Command:"), NETCFG_COLOR_MUTED);
    xapi_DrawSWText(g_window, 118, 56, (char *)"auto", NETCFG_COLOR_TEXT);
    xapi_DrawSWText(g_window, 18, 72, (char *)"manual <ip> <mask> <gateway> [dns]", NETCFG_COLOR_TEXT);
    xapi_DrawSWText(g_window, 18, 88, (char *)"dns <ip|auto>    refresh", NETCFG_COLOR_TEXT);
    xapi_DrawSWText(g_window, 470, 56, netcfg_tr("状态文件", "Status File"), NETCFG_COLOR_MUTED);
    xapi_DrawSWText(g_window, 470, 72, (char *)"/run/NetworkManager/status", NETCFG_COLOR_TEXT);
    xapi_DrawSWText(g_window, 18, 116, netcfg_tr("实时网络状态", "Live Network Status"), NETCFG_COLOR_MUTED);
    xapi_DrawSWText(g_window, 18, log_top - 16, netcfg_tr("最近操作", "Recent Activity"), NETCFG_COLOR_MUTED);

    netcfg_clamp_status_scroll();
    netcfg_clamp_log_scroll();

    int status_end = MIN(g_status_line_count, g_status_scroll_top + netcfg_status_visible());
    for (int i = g_status_scroll_top; i < status_end; ++i) {
        xapi_DrawSWText(g_window, 20, NETCFG_STATUS_TOP + (i - g_status_scroll_top) * NETCFG_LINE_HEIGHT,
                        g_status_lines[i].text, g_status_lines[i].color);
    }

    int log_end = MIN(g_log_line_count, g_log_scroll_top + netcfg_log_visible());
    for (int i = g_log_scroll_top; i < log_end; ++i) {
        xapi_DrawSWText(g_window, 20, netcfg_log_top() + (i - g_log_scroll_top) * NETCFG_LINE_HEIGHT,
                        g_log_lines[i].text, g_log_lines[i].color);
    }

    xapi_DrawSWText(g_window, 18, (UINT32)g_height - 56, netcfg_tr("输入:", "Input:"), NETCFG_COLOR_MUTED);
    xapi_DrawSWText(g_window, 74, (UINT32)g_height - 56, input_view, NETCFG_COLOR_TEXT);
    xapi_RefreshWindow(g_window);
    g_need_redraw = false;
}

static void netcfg_apply_command(void)
{
    static char ipv4_path[] = "/run/NetworkManager/ipv4";

    char command[sizeof(g_input)];
    strncpy(command, g_input, sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0';

    char *verb = strtok(command, " ");
    if (verb == NULL) {
        netcfg_set_status_tr("命令为空", "Command is empty");
        netcfg_append_log(netcfg_tr("命令为空。", "Command is empty."), NETCFG_COLOR_ERROR);
        return;
    }

    if (strcmp(verb, "refresh") == 0) {
        netcfg_refresh_status();
        netcfg_append_log(netcfg_tr("已刷新网络状态。", "Network status refreshed."), NETCFG_COLOR_LOG);
        return;
    }

    if (strcmp(verb, "auto") == 0) {
        if (netcfg_write_text_file(ipv4_path, "method=auto\n") < 0) {
            netcfg_set_status_tr("应用失败", "Apply failed");
            netcfg_append_log(netcfg_tr("启用 DHCP 失败。", "Failed to enable DHCP."), NETCFG_COLOR_ERROR);
            return;
        }
        netcfg_refresh_status();
        netcfg_set_status_tr("DHCP 已启用", "DHCP enabled");
        netcfg_append_log(netcfg_tr("IPv4 已切换为 DHCP。", "IPv4 switched to DHCP."), NETCFG_COLOR_LOG);
        return;
    }

    if (strcmp(verb, "dns") == 0) {
        char *dns = strtok(NULL, " ");
        if (dns == NULL) {
            netcfg_set_status_tr("DNS 命令无效", "Invalid DNS command");
            netcfg_append_log("用法: dns <ip|auto|ali|ali2>", NETCFG_COLOR_ERROR);
            return;
        }
        if (netcfg_apply_dns_value(dns) < 0) {
            netcfg_set_status_tr("DNS 应用失败", "DNS apply failed");
            netcfg_append_log(netcfg_tr("更新 DNS 服务器失败。可尝试：dns ali",
                                        "Failed to update DNS server. Try: dns ali"),
                              NETCFG_COLOR_ERROR);
            return;
        }
        netcfg_refresh_status();
        netcfg_set_status_tr("DNS 已更新", "DNS updated");
        netcfg_append_log(netcfg_tr("DNS 服务器已更新。", "DNS server updated."), NETCFG_COLOR_LOG);
        return;
    }

    if (strcmp(verb, "manual") == 0) {
        char *ip   = strtok(NULL, " ");
        char *mask = strtok(NULL, " ");
        char *gw   = strtok(NULL, " ");
        char *dns  = strtok(NULL, " ");
        if (ip == NULL || mask == NULL || gw == NULL) {
            netcfg_set_status_tr("手动配置命令无效", "Invalid manual command");
            netcfg_append_log("用法: manual <ip> <mask> <gateway> [dns]", NETCFG_COLOR_ERROR);
            return;
        }

        char ipv4_text[192];
        snprintf(ipv4_text, sizeof(ipv4_text),
                 "method=manual\n"
                 "address=%s\n"
                 "netmask=%s\n"
                 "gateway=%s\n",
                 ip, mask, gw);
        if (netcfg_write_text_file(ipv4_path, ipv4_text) < 0) {
            netcfg_set_status_tr("手动配置失败", "Manual config failed");
            netcfg_append_log(netcfg_tr("应用手动 IPv4 配置失败。", "Failed to apply manual IPv4 config."),
                              NETCFG_COLOR_ERROR);
            return;
        }

        if (dns != NULL) {
            if (netcfg_apply_dns_value(dns) < 0) {
                netcfg_set_status_tr("DNS 应用失败", "DNS apply failed");
                netcfg_append_log(netcfg_tr("IPv4 已应用，但 DNS 更新失败。", "IPv4 was applied, but DNS update failed."),
                                  NETCFG_COLOR_ERROR);
                netcfg_refresh_status();
                return;
            }
        }

        netcfg_refresh_status();
        netcfg_set_status_tr("手动 IPv4 已应用", "Manual IPv4 applied");
        netcfg_append_log(netcfg_tr("手动 IPv4 配置已应用。", "Manual IPv4 config applied."), NETCFG_COLOR_LOG);
        return;
    }

    netcfg_set_status_tr("未知命令", "Unknown command");
    netcfg_append_log(netcfg_tr("未知命令。", "Unknown command."), NETCFG_COLOR_ERROR);
}

static void netcfg_message_proc(UINT64 type, UINT64 hData, UINT64 lData)
{
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
            if (g_startup_submit_guard > 0) {
                break;
            }
            netcfg_apply_command();
        }
        break;
    case MSG_MOVE:
        g_last_mouse_y = (int)lData;
        break;
    case MSG_ROLLER:
        if (g_last_mouse_y >= netcfg_log_panel_top() && g_last_mouse_y <= netcfg_log_panel_bottom()) {
            g_log_scroll_top -= (int)hData * NETCFG_SCROLL_STEP;
            netcfg_clamp_log_scroll();
        } else {
            g_status_scroll_top -= (int)hData * NETCFG_SCROLL_STEP;
            netcfg_clamp_status_scroll();
        }
        g_need_redraw = true;
        break;
    case MSG_RESIZE:
        g_width = hData;
        g_height = lData;
        netcfg_clamp_status_scroll();
        netcfg_clamp_log_scroll();
        g_need_redraw = true;
        break;
    default:
        break;
    }
}

static int netcfg_main_impl(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    XWINDOW window;
    g_language = xj380_read_language();
    netcfg_set_status_tr("就绪", "Ready");
    window.title  = netcfg_tr("网络配置", "Network Config");
    window.width  = NETCFG_WIDTH;
    window.height = NETCFG_HEIGHT;
    window.sets   = XWIN_NORMAL | XWIN_SUPPORT_RESIZEABLE;

    xapi_CreateWindow(&g_window, &window);
    xapi_SetIcon(g_window, "/system/icon/terminal.png");
    SetMsgPrcor(g_window, netcfg_message_proc);
    netcfg_log_serial((char *)"netcfg: window created\n");

    netcfg_append_log(netcfg_tr("就绪。按 Enter 应用当前命令。", "Ready. Press Enter to apply the current command."),
                      NETCFG_COLOR_LOG);
    netcfg_append_log(netcfg_tr("DNS 快捷命令：dns ali / dns ali2", "DNS shortcuts: dns ali / dns ali2"),
                      NETCFG_COLOR_LOG);
    netcfg_refresh_status();
    netcfg_render_window();

    while (true) {
        if (g_need_redraw) {
            netcfg_render_window();
        }
        if (g_startup_submit_guard > 0) {
            g_startup_submit_guard--;
        }
        xapi_Sleep(NETCFG_LOOP_SLEEP_MS);
    }

    return 0;
}

extern "C" int netcfg_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int netcfg_main_cpp(int argc, char *argv[], char *envp[])
{
    return netcfg_main_impl(argc, argv, envp);
}
