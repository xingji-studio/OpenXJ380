#include "httpclient.h"
#include "https_client.h"
#include <xj380_i18n.h>

static const int      HTTPGET_WIDTH            = 760;
static const int      HTTPGET_HEIGHT           = 520;
static const int      HTTPGET_MAX_LINES        = 160;
static const int      HTTPGET_LINE_LENGTH      = 256;
static const int      HTTPGET_LOOP_SLEEP_MS    = 16;
static const int      HTTPGET_STARTUP_GUARD_TICKS = 32;
static const uint32_t HTTPGET_COLOR_BG         = 0xf2efe9ff;
static const uint32_t HTTPGET_COLOR_PANEL      = 0xfffbf5ff;
static const uint32_t HTTPGET_COLOR_HEADER     = 0x1d3557ff;
static const uint32_t HTTPGET_COLOR_STATUS     = 0xd8f3dcff;
static const uint32_t HTTPGET_COLOR_TEXT       = 0x222222ff;
static const uint32_t HTTPGET_COLOR_INFO       = 0x3d405bff;
static const uint32_t HTTPGET_COLOR_ERROR      = 0xb00020ff;
static const uint32_t HTTPGET_COLOR_RESPONSE   = 0x264653ff;

static void render_window();

typedef struct httpget_line {
    char     text[HTTPGET_LINE_LENGTH];
    uint32_t color;
} httpget_line_t;

typedef struct httpget_response_state {
    char line[HTTPGET_LINE_LENGTH];
    int  line_len;
} httpget_response_state_t;

static HDLE           g_window      = 0;
static bool           g_need_redraw = true;
static bool           g_send_req    = false;
static bool           g_ignore_first_submit = true;
static int            g_startup_submit_guard = HTTPGET_STARTUP_GUARD_TICKS;
static char           g_input[512]  = "GET https://example.com/";
static int            g_input_len   = 24;
static char           g_status[128] = "";
static const char    *g_status_zh   = "就绪";
static const char    *g_status_en   = "Ready";
static httpget_line_t g_lines[HTTPGET_MAX_LINES];
static int            g_line_count  = 0;
static UINT64         g_width       = HTTPGET_WIDTH;
static UINT64         g_height      = HTTPGET_HEIGHT;
static int            g_language    = XJ380_LANGUAGE_ZH_CN;

static char *httpget_tr(const char *zh_cn, const char *en_us)
{
    return xj380_tr_lang(g_language, zh_cn, en_us);
}

static int httpget_visible_lines()
{
    int visible = ((int)g_height - 120 - 84) / 16;
    return visible < 1 ? 1 : visible;
}

static void refresh_status_translation()
{
    strncpy(g_status, httpget_tr(g_status_zh, g_status_en), sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    g_need_redraw                  = true;
}

static void set_status_text(const char *zh_cn, const char *en_us)
{
    g_status_zh = zh_cn == NULL ? "" : zh_cn;
    g_status_en = en_us == NULL ? "" : en_us;
    refresh_status_translation();
}

static void append_line(const char *text, uint32_t color)
{
    if (text == NULL) {
        return;
    }

    if (g_line_count == HTTPGET_MAX_LINES) {
        for (int i = 1; i < HTTPGET_MAX_LINES; ++i) {
            g_lines[i - 1] = g_lines[i];
        }
        g_line_count--;
    }

    strncpy(g_lines[g_line_count].text, text, HTTPGET_LINE_LENGTH - 1);
    g_lines[g_line_count].text[HTTPGET_LINE_LENGTH - 1] = '\0';
    g_lines[g_line_count].color                         = color;
    g_line_count++;
    g_need_redraw = true;
}

static void append_response_line(httpget_response_state_t *state)
{
    state->line[state->line_len] = '\0';
    append_line(state->line_len == 0 ? "" : state->line, HTTPGET_COLOR_RESPONSE);
    state->line_len = 0;
}

static int httpget_response_callback(const char *data, int len, void *user_data)
{
    httpget_response_state_t *state = (httpget_response_state_t *)user_data;
    for (int i = 0; i < len; ++i) {
        char ch = data[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n' || state->line_len >= HTTPGET_LINE_LENGTH - 1) {
            append_response_line(state);
            if (ch == '\n') {
                continue;
            }
        }
        state->line[state->line_len++] = ch;
    }

    render_window();
    return 0;
}

static bool httpget_parse_url_target(char *target,
                                     bool *out_use_tls,
                                     char **out_host,
                                     uint16_t *out_port,
                                     char **out_path)
{
    if (target == NULL || out_use_tls == NULL || out_host == NULL || out_port == NULL || out_path == NULL) {
        return false;
    }

    *out_use_tls = strncmp(target, "https://", 8) == 0;
    if (!(strncmp(target, "http://", 7) == 0 || *out_use_tls)) {
        return false;
    }

    char *host = strstr(target, "://");
    host = host ? host + 3 : target;
    uint16_t port = *out_use_tls ? 443 : 80;

    char *path = strchr(host, '/');
    if (path != NULL) {
        *path = '\0';
        ++path;
        --path;
    } else {
        path = (char *)"/";
    }

    if (host[0] == '[') {
        char *host_end = strchr(host, ']');
        if (host_end == NULL) {
            return false;
        }
        *host_end = '\0';
        *out_host = host + 1;
        if (host_end[1] != '\0') {
            if (host_end[1] != ':') {
                return false;
            }
            long parsed_port = strtol(host_end + 2, NULL, 10);
            if (parsed_port <= 0 || parsed_port > 65535) {
                return false;
            }
            port = (uint16_t)parsed_port;
        }
    } else {
        char *port_sep = strrchr(host, ':');
        if (port_sep != NULL && strchr(host, ':') == port_sep) {
            *port_sep = '\0';
            long parsed_port = strtol(port_sep + 1, NULL, 10);
            if (parsed_port <= 0 || parsed_port > 65535) {
                return false;
            }
            port = (uint16_t)parsed_port;
        }
        *out_host = host;
    }

    *out_port = port;
    *out_path = path;
    return *out_host != NULL && (*out_host)[0] != '\0';
}

static void render_window()
{
    xapi_GetWindowSize(g_window, &g_width, &g_height);
    xapi_DrawRect(g_window, 0, 0, (UINT32)g_width - 1, (UINT32)g_height - 1, HTTPGET_COLOR_BG, true);
    xapi_DrawRect(g_window, 0, 0, (UINT32)g_width - 1, 39, HTTPGET_COLOR_HEADER, true);
    xapi_DrawRect(g_window, 12, 52, (UINT32)g_width - 13, (UINT32)g_height - 84, HTTPGET_COLOR_PANEL, true);
    xapi_DrawRect(g_window, 12, (UINT32)g_height - 64, (UINT32)g_width - 13, (UINT32)g_height - 16, 0xffffffff, true);

    xapi_DrawSWText(g_window, 16, 12, httpget_tr("HTTP 客户端", "HTTP Client"), 0xffffffff);
    xapi_DrawSWText(g_window, 170, 12, g_status, HTTPGET_COLOR_STATUS);
    xapi_DrawSWText(g_window, 18, 56,
                    httpget_tr("语法: METHOD URL [BODY] 或 METHOD HOST PORT PATH [BODY]",
                               "Syntax: METHOD URL [BODY] or METHOD HOST PORT PATH [BODY]"),
                    HTTPGET_COLOR_INFO);
    xapi_DrawSWText(g_window, 18, 72,
                    httpget_tr("方法: GET POST PUT PATCH DELETE HEAD OPTIONS TRACE CONNECT",
                               "Methods: GET POST PUT PATCH DELETE HEAD OPTIONS TRACE CONNECT"),
                    HTTPGET_COLOR_INFO);
    xapi_DrawSWText(g_window, 18, 88,
                    httpget_tr("示例: GET https://example.com/  或  POST 10.0.2.2 80 /api hi",
                               "Example: GET https://example.com/  or  POST 10.0.2.2 80 /api hi"),
                    HTTPGET_COLOR_INFO);
    xapi_DrawSWText(g_window, 18, (UINT32)g_height - 56, httpget_tr("请求:", "Request:"), HTTPGET_COLOR_INFO);
    xapi_DrawSWText(g_window, 90, (UINT32)g_height - 56, g_input, HTTPGET_COLOR_TEXT);

    int visible_lines = httpget_visible_lines();
    int start = g_line_count > visible_lines ? g_line_count - visible_lines : 0;
    for (int i = start; i < g_line_count; ++i) {
        xapi_DrawSWText(g_window, 20, 120 + (i - start) * 16, g_lines[i].text, g_lines[i].color);
    }

    xapi_RefreshWindow(g_window);
    g_need_redraw = false;
}

static void run_http_request()
{
    g_send_req = false;
    xapi_OutputSerial((char *)"httpget：开始执行请求\n");

    char request_line[sizeof(g_input)];
    strncpy(request_line, g_input, sizeof(request_line) - 1);
    request_line[sizeof(request_line) - 1] = '\0';

    char *method = strtok(request_line, " ");
    char *target = strtok(NULL, " ");
    char *port_s = strtok(NULL, " ");
    char *path   = strtok(NULL, " ");
    char *body   = strtok(NULL, "");

    if (method == NULL || target == NULL) {
        append_line(httpget_tr("语法无效。请使用：METHOD URL 或 METHOD HOST PORT PATH [BODY]",
                               "Invalid syntax. Use: METHOD URL or METHOD HOST PORT PATH [BODY]"),
                    HTTPGET_COLOR_ERROR);
        set_status_text("解析失败", "Parse failed");
        return;
    }

    if (!xhttp_validate_method(method)) {
        append_line(httpget_tr("HTTP 方法无效。", "Invalid HTTP method."), HTTPGET_COLOR_ERROR);
        set_status_text("方法无效", "Invalid method");
        return;
    }

    bool      use_tls        = false;
    uint16_t  port           = 0;
    bool      allocated_path = false;
    char     *host           = NULL;
    char      target_copy[sizeof(g_input)];

    if (strncmp(target, "http://", 7) == 0 || strncmp(target, "https://", 8) == 0) {
        strncpy(target_copy, target, sizeof(target_copy) - 1);
        target_copy[sizeof(target_copy) - 1] = '\0';

        if (!httpget_parse_url_target(target_copy, &use_tls, &host, &port, &path)) {
            append_line(httpget_tr("URL 主机或端口无效。", "Invalid URL host or port."), HTTPGET_COLOR_ERROR);
            set_status_text("URL 无效", "Invalid URL");
            return;
        }
    } else {
        host = target;
        if (port_s == NULL || path == NULL) {
            append_line(httpget_tr("旧语法缺少 HOST PORT PATH。", "Legacy syntax requires HOST PORT PATH."),
                        HTTPGET_COLOR_ERROR);
            set_status_text("解析失败", "Parse failed");
            return;
        }

        long parsed_port = strtol(port_s, NULL, 10);
        if (parsed_port <= 0 || parsed_port > 65535) {
            append_line(httpget_tr("端口无效。", "Invalid port."), HTTPGET_COLOR_ERROR);
            set_status_text("端口无效", "Invalid port");
            return;
        }
        port = (uint16_t)parsed_port;
    }

    if (host == NULL || host[0] == '\0') {
        append_line(httpget_tr("主机无效。", "Invalid host."), HTTPGET_COLOR_ERROR);
        set_status_text("主机无效", "Invalid host");
        return;
    }

    if (path == NULL || path[0] == '\0') {
        path = (char *)"/";
    } else if (path[0] != '/') {
        char normalized_path[256];
        snprintf(normalized_path, sizeof(normalized_path), "/%s", path);
        size_t normalized_len = strlen(normalized_path) + 1;
        char *owned_path = (char *)malloc(normalized_len);
        if (owned_path == NULL) {
            append_line(httpget_tr("整理路径时内存不足。", "Not enough memory while normalizing the path."),
                        HTTPGET_COLOR_ERROR);
            set_status_text("内存不足", "Out of memory");
            return;
        }
        memcpy(owned_path, normalized_path, normalized_len);
        path = owned_path;
        allocated_path = true;
    }

    xhttp_request_t request;
    request.method        = method;
    request.host          = host;
    request.port          = port;
    request.path          = path;
    request.body          = body;
    request.content_type  = "text/plain";
    request.extra_headers = "";

    httpget_response_state_t response_state;
    memset(&response_state, 0, sizeof(response_state));

    append_line(use_tls ? httpget_tr("HTTPS 请求已排队。", "HTTPS request queued.")
                        : httpget_tr("HTTP 请求已排队。", "HTTP request queued."),
                HTTPGET_COLOR_INFO);
    append_line(httpget_tr("---------------- 响应 ----------------", "---------------- Response ----------------"),
                HTTPGET_COLOR_INFO);
    if (use_tls)
        set_status_text("正在 TLS 请求", "Requesting with TLS");
    else
        set_status_text("正在请求", "Requesting");
    render_window();
    xapi_OutputSerial((char *)"httpget：分发请求\n");

    int status = use_tls
                     ? xtls_request(&request, httpget_response_callback, &response_state)
                     : xhttp_request(&request, httpget_response_callback, &response_state);
    if (allocated_path) {
        free(path);
    }
    {
        char logbuf[96];
        snprintf(logbuf, sizeof(logbuf), "httpget：请求状态=%d\n", status);
        xapi_OutputSerial(logbuf);
    }
    if (response_state.line_len > 0) {
        append_response_line(&response_state);
    }

    if (status == XHTTP_OK) {
        append_line(httpget_tr("-------------- 响应结束 --------------", "-------------- Response End --------------"),
                    HTTPGET_COLOR_INFO);
        set_status_text("完成", "Done");
        return;
    }

    switch (status) {
    case XHTTP_ERR_SOCKET:
        append_line(httpget_tr("socket() 失败", "socket() failed"), HTTPGET_COLOR_ERROR);
        set_status_text("套接字失败", "Socket failed");
        break;
    case XHTTP_ERR_CONNECT:
        append_line(httpget_tr("connect() 失败", "connect() failed"), HTTPGET_COLOR_ERROR);
        set_status_text("连接失败", "Connect failed");
        break;
    case XHTTP_ERR_REQUEST_BUILD:
        append_line(httpget_tr("请求过大。", "Request is too large."), HTTPGET_COLOR_ERROR);
        set_status_text("请求过大", "Request too large");
        break;
    case XHTTP_ERR_SEND:
        append_line(httpget_tr("send() 失败", "send() failed"), HTTPGET_COLOR_ERROR);
        set_status_text("发送失败", "Send failed");
        break;
    case XHTTP_ERR_RECV:
        append_line(httpget_tr("read() 失败", "read() failed"), HTTPGET_COLOR_ERROR);
        set_status_text("接收失败", "Receive failed");
        break;
    case XHTTP_ERR_TIMEOUT:
        append_line(httpget_tr("请求超时", "Request timed out"), HTTPGET_COLOR_ERROR);
        set_status_text("超时", "Timed out");
        break;
    case XHTTP_ERR_TLS_INIT:
        append_line(httpget_tr("TLS 初始化失败", "TLS initialization failed"), HTTPGET_COLOR_ERROR);
        set_status_text("TLS 初始化失败", "TLS init failed");
        break;
    case XHTTP_ERR_TLS_SEED:
        append_line(httpget_tr("TLS 随机种子失败", "TLS random seed failed"), HTTPGET_COLOR_ERROR);
        set_status_text("TLS 种子失败", "TLS seed failed");
        break;
    case XHTTP_ERR_TLS_CONFIG:
        append_line(httpget_tr("TLS 配置失败", "TLS configuration failed"), HTTPGET_COLOR_ERROR);
        set_status_text("TLS 配置失败", "TLS config failed");
        break;
    case XHTTP_ERR_TLS_HANDSHAKE:
        append_line(httpget_tr("TLS 握手失败", "TLS handshake failed"), HTTPGET_COLOR_ERROR);
        set_status_text("TLS 握手失败", "TLS handshake failed");
        break;
    case XHTTP_ERR_TLS_SEND:
        append_line(httpget_tr("TLS 写入失败", "TLS write failed"), HTTPGET_COLOR_ERROR);
        set_status_text("TLS 发送失败", "TLS send failed");
        break;
    case XHTTP_ERR_TLS_RECV:
        append_line(httpget_tr("TLS 读取失败", "TLS read failed"), HTTPGET_COLOR_ERROR);
        set_status_text("TLS 接收失败", "TLS receive failed");
        break;
    case XHTTP_ERR_TLS_CA_LOAD:
        append_line(httpget_tr("找不到 TLS CA 证书包。请安装 /etc/ssl/certs/ca-certificates.crt。",
                               "TLS CA bundle was not found. Install /etc/ssl/certs/ca-certificates.crt."),
                    HTTPGET_COLOR_ERROR);
        set_status_text("缺少 TLS CA", "Missing TLS CA");
        break;
    case XHTTP_ERR_TLS_CA_PARSE:
        append_line(httpget_tr("TLS CA 证书包解析失败。", "TLS CA bundle parsing failed."),
                    HTTPGET_COLOR_ERROR);
        set_status_text("TLS CA 无效", "Invalid TLS CA");
        break;
    case XHTTP_ERR_TLS_VERIFY:
        append_line(httpget_tr("TLS 证书验证失败。", "TLS certificate verification failed."), HTTPGET_COLOR_ERROR);
        set_status_text("TLS 验证失败", "TLS verify failed");
        break;
    default:
        append_line(httpget_tr("HTTP 请求失败。", "HTTP request failed."), HTTPGET_COLOR_ERROR);
        set_status_text("请求失败", "Request failed");
        break;
    }

    append_line(httpget_tr("-------------- 响应结束 --------------", "-------------- Response End --------------"),
                HTTPGET_COLOR_INFO);
}

static void httpget_message_proc(UINT64 type, UINT64 hData, UINT64 lData)
{
    (void)hData;

    switch (type) {
    case MSG_CHAR:
        if ((char)lData >= ' ' && g_input_len < (int)sizeof(g_input) - 1) {
            g_ignore_first_submit = false;
            g_input[g_input_len++] = (char)lData;
            g_input[g_input_len]   = '\0';
            g_need_redraw          = true;
        }
        break;
    case MSG_SPCHAR:
        if ((char)lData == '\b') {
            if (g_input_len > 0) {
                g_ignore_first_submit = false;
                g_input[--g_input_len] = '\0';
                g_need_redraw          = true;
            }
        } else if ((char)lData == '\n') {
            if (g_startup_submit_guard > 0) {
                xapi_OutputSerial((char *)"httpget：忽略启动期提交保护\n");
                break;
            }
            if (g_ignore_first_submit) {
                g_ignore_first_submit = false;
                xapi_OutputSerial((char *)"httpget：忽略启动期提交\n");
                break;
            }
            g_send_req = true;
        }
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

static int httpget_main_impl(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    xapi_OutputSerial((char *)"httpget：进入主函数\n");
    g_language = xj380_read_language();
    set_status_text("就绪", "Ready");

    XWINDOW window;
    window.title  = httpget_tr("HTTP 客户端", "HTTP Client");
    window.width  = HTTPGET_WIDTH;
    window.height = HTTPGET_HEIGHT;
    window.sets   = XWIN_NORMAL | XWIN_SUPPORT_RESIZEABLE;

    xapi_CreateWindow(&g_window, &window);
    xapi_OutputSerial((char *)"httpget：窗口已创建\n");
    xapi_SetIcon(g_window, "/system/icon/terminal.png");
    xapi_OutputSerial((char *)"httpget：图标已设置\n");
    SetMsgPrcor(g_window, httpget_message_proc);
    xapi_OutputSerial((char *)"httpget：消息处理器已设置\n");

    append_line(httpget_tr("已就绪，可以发送 HTTP/HTTPS 请求。", "Ready. You can send HTTP/HTTPS requests."),
                HTTPGET_COLOR_INFO);
    render_window();
    xapi_OutputSerial((char *)"httpget：初始渲染完成\n");

    while (true) {
        if (g_send_req) {
            run_http_request();
        }
        if (g_need_redraw) {
            render_window();
        }
        if (g_startup_submit_guard > 0) {
            g_startup_submit_guard--;
        }
        xapi_Sleep(HTTPGET_LOOP_SLEEP_MS);
    }

    return 0;
}

extern "C" int httpget_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int httpget_main_cpp(int argc, char *argv[], char *envp[])
{
    return httpget_main_impl(argc, argv, envp);
}
